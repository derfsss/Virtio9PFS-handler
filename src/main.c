/*
 * Virtio9PFS-handler -- Main entry point
 *
 * FileSysBox-based handler that mounts QEMU host-shared folders via
 * VirtIO 9P (9P2000.L protocol) as AmigaOS DOS volumes.
 *
 * Supports both VirtIO legacy (0x1009) and modern (0x1049) transport.
 *
 * Uses _start() instead of main() because the newlib CRT consumes the
 * DOS handler startup message from pr_MsgPort (treating it as WBStartup).
 * With -nostartfiles we intercept the message before any CRT code runs.
 */

#include "version.h"

static const char *version __attribute__((used)) =
    "$VER: " HANDLER_VERSION_STRING;

#define __NOLIBBASE__
#define __NOGLOBALIFACE__
#include <proto/exec.h>
#include <proto/dos.h>

#include "virtio9p_handler.h"
#include "pci/pci_discovery.h"
#include "virtio/virtio_init.h"
#include "virtio/virtio_irq.h"
#include "p9_client.h"
#include "p9_protocol.h"
#include "fid_pool.h"

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/startup.h>
#include <exec/exec.h>
#include <exec/exectags.h>
#include <exec/memory.h>
#include <libraries/filesysbox.h>
#include <proto/expansion.h>
#include <proto/filesysbox.h>
#include "string_utils.h"

/* Global library bases and interfaces -- manually managed.
 * IExec must be set up before anything else; expansion.library
 * is opened here instead of relying on -lauto.
 */
struct ExecIFace *IExec = NULL;

struct Library *ExpansionBase = NULL;

struct Library *FileSysBoxBase = NULL;
struct FileSysBoxIFace *IFileSysBox = NULL;

/* Global handler state (accessed by FUSE callbacks via extern) */
struct V9PHandler *g_handler = NULL;

/* Forward declaration */
void V9P_FillOperations(struct fuse_operations *ops);

/* Gracefully decline the mount on a permanent failure (no VirtIO 9P device,
 * missing system library/interface).  `reason` goes into the serial log;
 * messages here use DebugPrintF, not DPRINTF, deliberately -- they are the
 * one trace of the decline and must appear in release builds too.
 *
 * Two failure modes have to be avoided here:
 *
 * Boot requester: replying the ACTION_STARTUP packet with DOSFALSE makes
 * the boot-time Mount/mounter raise a blocking "Could not mount device"
 * requester that halts the Workbench boot until dismissed.  We therefore
 * reply DOSTRUE; the node removal below then makes the volume vanish so
 * nothing ever tries to use the (never-initialised) handler.
 *
 * Relaunch storm: a handler that exits without establishing dn_Port -- our
 * case, since we bail before FbxSetupFS -- is relaunched by DOS "for every
 * new device access thereafter" (ACTION_STARTUP autodoc).  With the failure
 * permanent, every reference to the volume would respawn this handler,
 * have it re-scan PCI, fail, and exit again.  Removing our DeviceNode
 * (dp_Arg3 of the startup packet) makes subsequent references fail
 * instantly instead.
 *
 * ORDER MATTERS.  The mount caller holds the DosList semaphore while it
 * waits for the ACTION_STARTUP reply, so the reply must come FIRST.  The
 * removal afterwards uses NonBlockingModifyDosEntry(NBM_REMDOSENTRY) --
 * the V51.29 function the AddDosEntry/RemDosEntry autodocs direct handlers
 * to, precisely because it can never block the caller on the DosList lock
 * (all locking, including the contended case, is handled internally).  If
 * the removal fails the node stays mountable with dn_Port unset, so the
 * next access relaunches the handler, which retries this removal -- the
 * fallback converges instead of hanging.
 *
 * The removed node is intentionally NOT freed: dn_SegList is the very
 * seglist this code is executing from, and the mount caller may still hold
 * references.  A one-time boot allocation is the safe price.
 *
 * dos.library is opened locally rather than held for the handler lifetime
 * because this is the only place that needs it.  Version 53 (4.1) is
 * comfortably past the V51.29 NonBlockingModifyDosEntry baseline.
 */
static void V9P_DeclineMount(struct Message *startupMsg, const char *reason)
{
    struct DosPacket *pkt = startupMsg ?
        (struct DosPacket *)startupMsg->mn_Node.ln_Name : NULL;
    struct DeviceNode *dn = pkt ?
        (struct DeviceNode *)BADDR(pkt->dp_Arg3) : NULL;

    /* Reply first; releases the caller's DosList lock (see above).  Running
     * code after the reply is safe -- the early-exit cleanup paths in
     * _start() already do.  Without FBX (open failed), reply by hand:
     * the packet result fields are NOT initialised by DOS, so both must
     * be set before ReplyMsg. */
    if (IFileSysBox) {
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSTRUE, 0);
    } else if (startupMsg) {
        if (pkt) {
            pkt->dp_Res1 = DOSTRUE;
            pkt->dp_Res2 = 0;
        }
        IExec->ReplyMsg(startupMsg);
    }

    if (!dn)
        return;

    struct Library *dosbase = IExec->OpenLibrary("dos.library", 53);
    if (!dosbase)
        return;

    struct DOSIFace *idos = (struct DOSIFace *)IExec->GetInterface(
        dosbase, "main", 1, NULL);
    if (idos) {
        /* DeviceNode and DosList share their leading layout, so the
         * cast is the standard idiom for DosList entry calls. */
        int32 removed = idos->NonBlockingModifyDosEntry(
            (struct DosList *)dn, NBM_REMDOSENTRY, NULL, NULL);
        IExec->DebugPrintF(removed ?
            "[virtio9p] %s -- mount declined, device node removed.\n" :
            "[virtio9p] %s -- mount declined, device node removal failed.\n",
            reason);
        IExec->DropInterface((struct Interface *)idos);
    }
    IExec->CloseLibrary(dosbase);
}

int32 _start(STRPTR argstring __attribute__((unused)),
             int32 arglen __attribute__((unused)),
             struct ExecBase *sysbase)
{
    struct V9PHandler handler;
    struct FbxFS *fs = NULL;
    int32 ret = RETURN_OK;

    /* Set up IExec -- must be first, everything else depends on it */
    IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->Obtain();

    IExec->DebugPrintF("[virtio9p] === " VERSION_LOG_STRING " ===\n");

    /* Guard against being run from a CLI shell.  Handlers receive their
     * startup message via pr_MsgPort (pr_CLI == 0).  If pr_CLI is set,
     * we were launched from a Shell -- print a diagnostic and bail. */
    {
        struct Process *earlyProc = (struct Process *)IExec->FindTask(NULL);
        if (earlyProc->pr_CLI != 0) {
            IExec->DebugPrintF("[virtio9p] " HANDLER_NAME " cannot be executed "
                               "from a shell -- install to L: with a DOSDriver "
                               "in DEVS:DOSDrivers/ and reboot.\n");
            IExec->Release();
            return RETURN_FAIL;
        }
    }

    memset(&handler, 0, sizeof(handler));
    g_handler = &handler;

    /* Initial msize before Version negotiation */
    handler.msize = P9_MSIZE;
    handler.root_fid = 0;
    handler.next_tag = 1;

    /* 1. Get startup message from DOS.
     *
     * With -nostartfiles, the CRT does not run -- the handler startup
     * message is still sitting on pr_MsgPort.  We are a handler process
     * (pr_CLI == 0), so WaitPort + GetMsg to receive it.
     */
    struct Process *proc = (struct Process *)IExec->FindTask(NULL);

    DPRINTF("main: Waiting for startup message (pr_CLI=%p)...\n",
            (void *)proc->pr_CLI);

    IExec->WaitPort(&proc->pr_MsgPort);
    struct Message *startupMsg = IExec->GetMsg(&proc->pr_MsgPort);

    DPRINTF("main: Startup message: %p\n", startupMsg);

    if (!startupMsg) {
        DPRINTF("main: No startup message received.\n");
        ret = RETURN_FAIL;
        goto cleanup_exec;
    }

    /* 2. Open filesysbox.library.
     *
     * A missing filesysbox.library is permanent for this boot, like a
     * missing 9P device -- decline (V9P_DeclineMount replies the packet
     * by hand when IFileSysBox is unavailable) rather than fail, so no
     * blocking boot requester and no relaunch storm. */
    FileSysBoxBase = IExec->OpenLibrary("filesysbox.library", 54);
    if (!FileSysBoxBase) {
        V9P_DeclineMount(startupMsg, "No filesysbox.library v54");
        goto cleanup_exec;
    }
    IFileSysBox = (struct FileSysBoxIFace *)IExec->GetInterface(
        FileSysBoxBase, "main", 1, NULL);
    if (!IFileSysBox) {
        V9P_DeclineMount(startupMsg, "No FileSysBox interface");
        goto cleanup_fbx;
    }

    /* 3. Open expansion.library (for PCI interface).  Like a missing 9P
     * device, a missing system library/interface is permanent for this
     * boot -- decline rather than fail, or every later reference to the
     * volume relaunches the handler just to fail again. */
    ExpansionBase = IExec->OpenLibrary("expansion.library", 50);
    if (!ExpansionBase) {
        V9P_DeclineMount(startupMsg, "No expansion.library");
        goto cleanup_fbx;
    }

    /* 4. Get PCI interface from expansion.library.  The "main" interface
     * is not used by this handler -- we go straight to the "pci" one. */
    handler.IPCI = (struct PCIIFace *)IExec->GetInterface(
        ExpansionBase, "pci", 1, NULL);

    if (!handler.IPCI) {
        V9P_DeclineMount(startupMsg, "No PCI interface");
        goto cleanup_expansion;
    }

    /* 5. PCI discovery -- try modern 0x1049, fallback to legacy 0x1009.
     *
     * No device is an expected, permanent condition (the QEMU machine was
     * started without -device virtio-9p-pci).  Decline the mount quietly:
     * remove the device node so DOS won't relaunch us on every reference,
     * log a single serial line, and exit cleanly without a requester. */
    if (!V9P_DiscoverDevice(&handler)) {
        V9P_DeclineMount(startupMsg, "No 9P device");
        goto cleanup_pci;
    }

    DPRINTF("main: Device found (mode=%s)\n",
            handler.modern_mode ? "MODERN" : "LEGACY");

    /* 6. Initialize VirtIO transport (legacy or modern).  vq_lock was
     * removed -- FBX runs a single-threaded event loop, so no
     * AddBuf/Kick concurrency to serialize.
     *
     * From here on, every init failure DECLINES the mount instead of
     * replying DOSFALSE.  These conditions are just as permanent for
     * this boot as a missing device, and a DOSFALSE reply at boot time
     * raises the blocking "Could not mount device" requester AND leaves
     * the DeviceNode relaunchable (see V9P_DeclineMount above). */
    if (!V9P_InitVirtIO(&handler)) {
        V9P_DeclineMount(startupMsg, "VirtIO transport init failed");
        goto cleanup_pci;
    }

    /* 7. Read mount tag from config space */
    V9P_ReadMountTag(&handler);

    if (handler.mount_tag[0] == '\0') {
        strncpy(handler.mount_tag, "9P", sizeof(handler.mount_tag) - 1);
        handler.mount_tag[sizeof(handler.mount_tag) - 1] = '\0';
        DPRINTF("main: No mount tag found, using default '%s'\n",
                handler.mount_tag);
    }

    /* 8. Install ISR.
     *
     * The ISR exists only to read the VirtIO ISR register and de-assert
     * the device INT line (otherwise QEMU would retrigger it continually).
     * V9P_Transact polls the used ring -- no task signalling involved --
     * so there is no signal bit to allocate.
     */
    if (!V9P_InstallInterrupt(&handler)) {
        V9P_DeclineMount(startupMsg, "Failed to install interrupt handler");
        goto cleanup_virtio;
    }

    /* 9. Allocate DMA-capable tx/rx buffers.
     *
     * AVT_Contiguous guarantees physically contiguous pages -- required
     * because the device is programmed with a single physical address
     * per buffer.  Without it, AllocVecTags may return virtually-
     * contiguous but physically fragmented memory (StartDMA autodoc:
     * "adjacent virtual addresses might not be adjacent in physical
     * memory"), which used to make init fail intermittently depending
     * on boot-time memory layout.  AVT_Lock defaults to TRUE for
     * MEMF_SHARED, pinning the pages.
     *
     * Sized P9_MSIZE (not handler.msize) deliberately: P9_Version may
     * shrink handler.msize later, and the EndDMA calls at cleanup_bufs
     * must be issued with the exact same size StartDMA was given. */
    handler.tx_buf = (uint8 *)IExec->AllocVecTags(
        P9_MSIZE, AVT_Type, MEMF_SHARED, AVT_Contiguous, TRUE,
        AVT_ClearWithValue, 0, TAG_DONE);
    handler.rx_buf = (uint8 *)IExec->AllocVecTags(
        P9_MSIZE, AVT_Type, MEMF_SHARED, AVT_Contiguous, TRUE,
        AVT_ClearWithValue, 0, TAG_DONE);
    /* Dedicated 16-byte buffer for Tflush.  Held throughout
     * handler lifetime so the timeout path never overwrites a possibly-
     * still-being-read T-message in tx_buf. */
    handler.flush_buf = (uint8 *)IExec->AllocVecTags(
        16, AVT_Type, MEMF_SHARED, AVT_Contiguous, TRUE,
        AVT_ClearWithValue, 0, TAG_DONE);

    if (!handler.tx_buf || !handler.rx_buf || !handler.flush_buf) {
        V9P_DeclineMount(startupMsg, "Failed to allocate DMA buffers");
        goto cleanup_irq;
    }

    /* 9b. Resolve physical addresses for DMA -- and KEEP the StartDMA
     * regions live for the buffer lifetime.
     *
     * Per the SDK autodoc for StartDMA, "the mapping will not
     * change as long as EndDMA is not called".  We therefore defer the
     * matching EndDMA to cleanup_bufs -- this guarantees that the
     * cached tx_phys/rx_phys/flush_phys remain valid even under host
     * memory pressure or page compaction.
     *
     * V9P_Transact uses PPC dcbst/dcbf inline asm for per-transaction
     * cache management instead of calling CachePostDMA every time
     * (saves ~10 kernel calls per transact). */
    {
        uint32 n;
        struct DMAEntry *de;

        n = IExec->StartDMA(handler.tx_buf, P9_MSIZE, DMA_ReadFromRAM);
        if (n == 0) {
            V9P_DeclineMount(startupMsg, "StartDMA(tx) failed");
            goto cleanup_bufs;
        }
        if (n > 1) {
            /* Cannot happen with AVT_Contiguous; kept as a guard. */
            DPRINTF("main: tx_buf physically fragmented (%lu entries) despite AVT_Contiguous.\n", n);
            IExec->EndDMA(handler.tx_buf, P9_MSIZE, DMA_ReadFromRAM | DMAF_NoModify);
            V9P_DeclineMount(startupMsg, "tx_buf physically fragmented");
            goto cleanup_bufs;
        }
        de = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
        if (!de) {
            IExec->EndDMA(handler.tx_buf, P9_MSIZE, DMA_ReadFromRAM | DMAF_NoModify);
            V9P_DeclineMount(startupMsg, "No memory for tx DMAEntry list");
            goto cleanup_bufs;
        }
        IExec->GetDMAList(handler.tx_buf, P9_MSIZE, DMA_ReadFromRAM, de);
        handler.tx_phys = (uint32)de[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, de);
        handler.tx_dma_active = TRUE;          /* defer EndDMA */

        n = IExec->StartDMA(handler.rx_buf, P9_MSIZE, 0);
        if (n == 0) {
            V9P_DeclineMount(startupMsg, "StartDMA(rx) failed");
            goto cleanup_bufs;
        }
        if (n > 1) {
            /* Cannot happen with AVT_Contiguous; kept as a guard. */
            DPRINTF("main: rx_buf physically fragmented (%lu entries) despite AVT_Contiguous.\n", n);
            IExec->EndDMA(handler.rx_buf, P9_MSIZE, DMAF_NoModify);
            V9P_DeclineMount(startupMsg, "rx_buf physically fragmented");
            goto cleanup_bufs;
        }
        de = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
        if (!de) {
            IExec->EndDMA(handler.rx_buf, P9_MSIZE, DMAF_NoModify);
            V9P_DeclineMount(startupMsg, "No memory for rx DMAEntry list");
            goto cleanup_bufs;
        }
        IExec->GetDMAList(handler.rx_buf, P9_MSIZE, 0, de);
        handler.rx_phys = (uint32)de[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, de);
        handler.rx_dma_active = TRUE;          /* defer EndDMA */

        /* Also resolve flush_buf phys */
        n = IExec->StartDMA(handler.flush_buf, 16, DMA_ReadFromRAM);
        if (n == 0) {
            V9P_DeclineMount(startupMsg, "StartDMA(flush) failed");
            goto cleanup_bufs;
        }
        de = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
        if (!de) {
            IExec->EndDMA(handler.flush_buf, 16, DMA_ReadFromRAM | DMAF_NoModify);
            V9P_DeclineMount(startupMsg, "No memory for flush DMAEntry list");
            goto cleanup_bufs;
        }
        IExec->GetDMAList(handler.flush_buf, 16, DMA_ReadFromRAM, de);
        handler.flush_phys = (uint32)de[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, de);
        handler.flush_dma_active = TRUE;       /* defer EndDMA */

        DPRINTF("main: DMA cached tx_phys=0x%08lX rx_phys=0x%08lX flush_phys=0x%08lX (held open)\n",
                handler.tx_phys, handler.rx_phys, handler.flush_phys);
    }

    /* 10. 9P Version negotiation */
    int32 err = P9_Version(&handler);
    if (err) {
        DPRINTF("main: P9_Version failed: %ld\n", err);
        V9P_DeclineMount(startupMsg, "9P version handshake failed");
        goto cleanup_bufs;
    }

    DPRINTF("main: 9P version negotiated, msize=%lu\n", handler.msize);

    /* 11. 9P Attach (fid 0 = root) */
    err = P9_Attach(&handler, handler.root_fid);
    if (err) {
        DPRINTF("main: P9_Attach failed: %ld\n", err);
        V9P_DeclineMount(startupMsg, "9P attach failed");
        goto cleanup_bufs;
    }

    DPRINTF("main: Attached to root (fid=%lu)\n", handler.root_fid);

    /* 12. Create FID pool */
    handler.fid_pool = FidPool_Create();
    if (!handler.fid_pool) {
        DPRINTF("main: Failed to create FID pool.\n");
        P9_Clunk(&handler, handler.root_fid);
        V9P_DeclineMount(startupMsg, "Failed to create FID pool");
        goto cleanup_bufs;
    }

    /* 13. Set up FileSysBox */
    struct fuse_operations ops;
    V9P_FillOperations(&ops);

    struct TagItem fbxTags[] = {
        { FBXT_DOSTYPE, 0x39504650 },  /* "9PFP" */
        { FBXT_FSFLAGS, FBXF_ENABLE_UTF8_NAMES | FBXF_ENABLE_32BIT_UIDS },
        { TAG_DONE, 0 }
    };

    fs = IFileSysBox->FbxSetupFS(startupMsg, fbxTags, &ops, sizeof(ops), &handler);

    if (fs) {
        DPRINTF("main: FbxSetupFS success, entering event loop.\n");
        IFileSysBox->FbxEventLoop(fs);
        DPRINTF("main: Event loop exited.\n");

        /* Remove ISR from the system chain BEFORE FbxCleanupFS.
         *
         * FbxCleanupFS replies to DOS, signaling "handler is done".
         * During Restart System, the OS may kill this process immediately
         * after that reply -- before our cleanup code at cleanup_irq runs.
         * The struct Interrupt is on our stack, so if the process is killed,
         * the ISR chain has a dangling pointer into freed memory.
         *
         * V9P_Transact uses polling (not the ISR signal), so any FUSE
         * callbacks FBX invokes during cleanup still work without the ISR.
         * V9P_RemoveInterrupt is idempotent -- the call at cleanup_irq
         * becomes a no-op. */
        V9P_RemoveInterrupt(&handler);

        IFileSysBox->FbxCleanupFS(fs);
    } else {
        DPRINTF("main: FbxSetupFS failed!\n");
        ret = RETURN_FAIL;
    }

    /* 14. Cleanup */
    P9_Clunk(&handler, handler.root_fid);

    FidPool_Destroy(handler.fid_pool);

cleanup_bufs:
    /* Matching EndDMA for each successful StartDMA we left live.
     * Sizes are P9_MSIZE, NOT handler.msize -- P9_Version may have
     * renegotiated msize down, and EndDMA "*must* be the same value
     * that was passed to StartDMA" (exec autodoc).
     * DMAF_NoModify is only claimed for regions the device never
     * wrote: tx/flush (device reads them).  rx was device-written all
     * session, so it gets the honest flags (none). */
    if (handler.flush_dma_active)
        IExec->EndDMA(handler.flush_buf, 16, DMA_ReadFromRAM | DMAF_NoModify);
    if (handler.rx_dma_active)
        IExec->EndDMA(handler.rx_buf, P9_MSIZE, 0);
    if (handler.tx_dma_active)
        IExec->EndDMA(handler.tx_buf, P9_MSIZE, DMA_ReadFromRAM | DMAF_NoModify);
    if (handler.flush_buf)
        IExec->FreeVec(handler.flush_buf);
    if (handler.rx_buf)
        IExec->FreeVec(handler.rx_buf);
    if (handler.tx_buf)
        IExec->FreeVec(handler.tx_buf);

cleanup_irq:
    V9P_RemoveInterrupt(&handler);

cleanup_virtio:
    V9P_CleanupVirtIO(&handler);

cleanup_pci:
    /* Release PCI resources */
    if (handler.pciDevice) {
        if (handler.bar4)
            handler.pciDevice->FreeResourceRange(handler.bar4);
        if (handler.bar0)
            handler.pciDevice->FreeResourceRange(handler.bar0);
        handler.IPCI->FreeDevice(handler.pciDevice);
    }
    if (handler.IPCI)
        IExec->DropInterface((struct Interface *)handler.IPCI);

cleanup_expansion:
    if (ExpansionBase)
        IExec->CloseLibrary(ExpansionBase);

cleanup_fbx:
    if (IFileSysBox)
        IExec->DropInterface((struct Interface *)IFileSysBox);
    if (FileSysBoxBase)
        IExec->CloseLibrary(FileSysBoxBase);

cleanup_exec:
    /* Clear the global before the struct on our stack goes out of scope;
     * no callback should fire after this point, but don't leave a dangling
     * pointer to dead stack memory just in case. */
    g_handler = NULL;
    DPRINTF("main: Shutdown complete.\n");
    IExec->Release();
    return ret;
}
