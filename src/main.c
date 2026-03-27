/*
 * Virtio9PFS-handler — Main entry point
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
#include <exec/memory.h>
#include <libraries/filesysbox.h>
#include <proto/expansion.h>
#include <proto/filesysbox.h>
#include "string_utils.h"

/* Global library bases and interfaces — manually managed.
 * IExec must be set up before anything else; expansion.library
 * is opened here instead of relying on -lauto.
 */
struct ExecIFace *IExec = NULL;

struct Library *ExpansionBase = NULL;
struct ExpansionIFace *IExpansion = NULL;

struct Library *FileSysBoxBase = NULL;
struct FileSysBoxIFace *IFileSysBox = NULL;

/* Global handler state (accessed by FUSE callbacks via extern) */
struct V9PHandler *g_handler = NULL;

/* Forward declaration */
void V9P_FillOperations(struct fuse_operations *ops);

int32 _start(STRPTR argstring __attribute__((unused)),
             int32 arglen __attribute__((unused)),
             struct ExecBase *sysbase)
{
    struct V9PHandler handler;
    struct FbxFS *fs = NULL;
    int32 ret = RETURN_OK;

    /* Set up IExec — must be first, everything else depends on it */
    IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->Obtain();

    IExec->DebugPrintF("[virtio9p] === " VERSION_LOG_STRING " ===\n");

    memset(&handler, 0, sizeof(handler));
    g_handler = &handler;

    /* Initial msize before Version negotiation */
    handler.msize = P9_MSIZE;
    handler.root_fid = 0;
    handler.next_tag = 1;

    /* 1. Get startup message from DOS.
     *
     * With -nostartfiles, the CRT does not run — the handler startup
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

    /* 2. Open filesysbox.library */
    FileSysBoxBase = IExec->OpenLibrary("filesysbox.library", 54);
    if (!FileSysBoxBase) {
        DPRINTF("main: Failed to open filesysbox.library v54.\n");
        /* Cannot call FbxReturnMountMsg — no FBX interface yet.
         * ReplyMsg manually so DOS doesn't hang. */
        IExec->ReplyMsg(startupMsg);
        ret = RETURN_FAIL;
        goto cleanup_exec;
    }
    IFileSysBox = (struct FileSysBoxIFace *)IExec->GetInterface(
        FileSysBoxBase, "main", 1, NULL);
    if (!IFileSysBox) {
        DPRINTF("main: Failed to get FileSysBox interface.\n");
        IExec->ReplyMsg(startupMsg);
        ret = RETURN_FAIL;
        goto cleanup_fbx;
    }

    /* 3. Open expansion.library (for PCI interface) */
    ExpansionBase = IExec->OpenLibrary("expansion.library", 50);
    if (!ExpansionBase) {
        DPRINTF("main: Failed to open expansion.library.\n");
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_DISK);
        ret = RETURN_FAIL;
        goto cleanup_fbx;
    }
    IExpansion = (struct ExpansionIFace *)IExec->GetInterface(
        ExpansionBase, "main", 1, NULL);

    /* 4. Get PCI interface from expansion.library */
    handler.IPCI = (struct PCIIFace *)IExec->GetInterface(
        ExpansionBase, "pci", 1, NULL);

    if (!handler.IPCI) {
        DPRINTF("main: Failed to get PCI interface.\n");
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_DISK);
        ret = RETURN_FAIL;
        goto cleanup_expansion;
    }

    /* 5. PCI discovery — try modern 0x1049, fallback to legacy 0x1009 */
    if (!V9P_DiscoverDevice(&handler)) {
        DPRINTF("main: No VirtIO 9P device found.\n");
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_DISK);
        goto cleanup_pci;
    }

    DPRINTF("main: Device found (mode=%s)\n",
            handler.modern_mode ? "MODERN" : "LEGACY");

    /* 6. Initialize VirtIO transport (legacy or modern) */
    IExec->InitSemaphore(&handler.vq_lock);

    if (!V9P_InitVirtIO(&handler)) {
        DPRINTF("main: VirtIO init failed.\n");
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_DISK);
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

    /* 8. Install ISR
     *
     * AllocSignal(-1) returns the highest free bit.  Bits 28-31 are
     * the SIGBREAKF_CTRL_C/D/E/F break signals.  FBX's FbxEventLoop
     * waits on break signals — if our IRQ signal lands on one of those
     * bits, FBX consumes it and V9P_Transact blocks forever.
     *
     * Workaround: pre-allocate bits 28-31 to skip them, allocate our
     * real signal (gets bit ≤27), then free the dummies.
     */
    int8 dummy_sigs[4];
    int i;
    for (i = 0; i < 4; i++)
        dummy_sigs[i] = IExec->AllocSignal(-1);

    int8 sig_bit = IExec->AllocSignal(-1);

    for (i = 0; i < 4; i++) {
        if (dummy_sigs[i] >= 0)
            IExec->FreeSignal(dummy_sigs[i]);
    }

    if (sig_bit < 0) {
        DPRINTF("main: Failed to allocate signal bit.\n");
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_FREE_STORE);
        goto cleanup_virtio;
    }

    DPRINTF("main: IRQ signal bit=%ld mask=0x%08lX\n",
            (int32)sig_bit, (1UL << sig_bit));

    handler.irq_signal = (1UL << sig_bit);
    handler.handler_task = IExec->FindTask(NULL);

    if (!V9P_InstallInterrupt(&handler)) {
        DPRINTF("main: Failed to install interrupt handler.\n");
        IExec->FreeSignal(sig_bit);
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_DISK);
        goto cleanup_virtio;
    }

    /* 9. Allocate DMA-capable tx/rx buffers */
    handler.tx_buf = (uint8 *)IExec->AllocVecTags(
        handler.msize, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
    handler.rx_buf = (uint8 *)IExec->AllocVecTags(
        handler.msize, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);

    if (!handler.tx_buf || !handler.rx_buf) {
        DPRINTF("main: Failed to allocate DMA buffers.\n");
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_FREE_STORE);
        goto cleanup_irq;
    }

    /* 9b. Resolve physical addresses for DMA (one-time, cached).
     *
     * StartDMA here just resolves virtual→physical; we EndDMA immediately
     * with DMAF_NoModify (no cache side-effects).  V9P_Transact uses
     * dcbst/dcbi inline asm for per-transaction cache management instead
     * of calling StartDMA/EndDMA every time (saves ~10 kernel calls). */
    {
        uint32 n;
        struct DMAEntry *de;

        n = IExec->StartDMA(handler.tx_buf, handler.msize, DMA_ReadFromRAM);
        if (n == 0) {
            DPRINTF("main: StartDMA(tx) failed.\n");
            IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_FREE_STORE);
            goto cleanup_bufs;
        }
        if (n > 1) {
            DPRINTF("main: tx_buf physically fragmented (%lu entries) — need contiguous.\n", n);
            IExec->EndDMA(handler.tx_buf, handler.msize, DMA_ReadFromRAM | DMAF_NoModify);
            IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_FREE_STORE);
            goto cleanup_bufs;
        }
        de = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
        IExec->GetDMAList(handler.tx_buf, handler.msize, DMA_ReadFromRAM, de);
        handler.tx_phys = (uint32)de[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, de);
        IExec->EndDMA(handler.tx_buf, handler.msize, DMA_ReadFromRAM | DMAF_NoModify);

        n = IExec->StartDMA(handler.rx_buf, handler.msize, 0);
        if (n == 0) {
            DPRINTF("main: StartDMA(rx) failed.\n");
            IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_FREE_STORE);
            goto cleanup_bufs;
        }
        if (n > 1) {
            DPRINTF("main: rx_buf physically fragmented (%lu entries) — need contiguous.\n", n);
            IExec->EndDMA(handler.rx_buf, handler.msize, DMAF_NoModify);
            IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_FREE_STORE);
            goto cleanup_bufs;
        }
        de = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
        IExec->GetDMAList(handler.rx_buf, handler.msize, 0, de);
        handler.rx_phys = (uint32)de[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, de);
        IExec->EndDMA(handler.rx_buf, handler.msize, DMAF_NoModify);

        DPRINTF("main: DMA cached tx_phys=0x%08lX rx_phys=0x%08lX\n",
                handler.tx_phys, handler.rx_phys);
    }

    /* 10. 9P Version negotiation */
    int32 err = P9_Version(&handler);
    if (err) {
        DPRINTF("main: P9_Version failed: %ld\n", err);
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_DISK);
        goto cleanup_bufs;
    }

    DPRINTF("main: 9P version negotiated, msize=%lu\n", handler.msize);

    /* 11. 9P Attach (fid 0 = root) */
    err = P9_Attach(&handler, handler.root_fid);
    if (err) {
        DPRINTF("main: P9_Attach failed: %ld\n", err);
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_DISK);
        goto cleanup_bufs;
    }

    DPRINTF("main: Attached to root (fid=%lu)\n", handler.root_fid);

    /* 12. Create FID pool */
    handler.fid_pool = FidPool_Create();
    if (!handler.fid_pool) {
        DPRINTF("main: Failed to create FID pool.\n");
        P9_Clunk(&handler, handler.root_fid);
        IFileSysBox->FbxReturnMountMsg(startupMsg, DOSFALSE, ERROR_NO_FREE_STORE);
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
         * after that reply — before our cleanup code at cleanup_irq runs.
         * The struct Interrupt is on our stack, so if the process is killed,
         * the ISR chain has a dangling pointer into freed memory.
         *
         * V9P_Transact uses polling (not the ISR signal), so any FUSE
         * callbacks FBX invokes during cleanup still work without the ISR.
         * V9P_RemoveInterrupt is idempotent — the call at cleanup_irq
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
    if (handler.rx_buf)
        IExec->FreeVec(handler.rx_buf);
    if (handler.tx_buf)
        IExec->FreeVec(handler.tx_buf);

cleanup_irq:
    V9P_RemoveInterrupt(&handler);
    IExec->FreeSignal(sig_bit);

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
    if (IExpansion)
        IExec->DropInterface((struct Interface *)IExpansion);
    if (ExpansionBase)
        IExec->CloseLibrary(ExpansionBase);

cleanup_fbx:
    if (IFileSysBox)
        IExec->DropInterface((struct Interface *)IFileSysBox);
    if (FileSysBoxBase)
        IExec->CloseLibrary(FileSysBoxBase);

cleanup_exec:
    DPRINTF("main: Shutdown complete.\n");
    IExec->Release();
    return ret;
}
