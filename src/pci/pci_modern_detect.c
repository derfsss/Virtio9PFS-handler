#include "pci/pci_modern_detect.h"
#include "virtio/virtio_pci_modern.h"
#include "virtio/virtio_pci.h"
#include "virtio9p_handler.h"

/*
 * V9P_DetectModern: walk the PCI capability list for VirtIO vendor-specific
 * capabilities (type 0x09).  Each such cap describes one configuration region
 * (COMMON, NOTIFY, ISR, DEVICE) located within a specific BAR at a given offset.
 *
 * If a COMMON_CFG cap is found, handler->modern_mode is set TRUE and all
 * cfg_base fields are populated.  Then the function *probes* MMIO by writing
 * VIRTIO_STATUS_ACKNOWLEDGE and reading it back: if the bridge forwards the
 * write, modern mode is kept; otherwise modern_mode is cleared so the caller
 * uses the legacy I/O init path.
 *
 * Transitional devices (0x1009) expose modern caps alongside their legacy
 * I/O interface — on AmigaOne the bridge swallows MMIO writes, so we must
 * fall back to legacy.  On Pegasos2/SAM460ex modern MMIO works.
 */
BOOL V9P_DetectModern(struct V9PHandler *handler)
{
    struct PCIDevice *pciDev = handler->pciDevice;

    handler->modern_mode     = FALSE;
    handler->common_cfg_base = 0;
    handler->notify_cfg_base = 0;
    handler->notify_off_mult = 0;
    handler->isr_cfg_base    = 0;
    handler->device_cfg_base = 0;

    if (!pciDev) {
        DPRINTF("pci_modern_detect: No PCI device handle.\n");
        return FALSE;
    }

    struct PCICapability *cap = pciDev->GetFirstCapability();
    int guard = 0;

    while (cap && guard < 32) {
        guard++;

        if (cap->Type != PCI_CAPABILITYID_VENDOR) {
            cap = pciDev->GetNextCapability(cap);
            continue;
        }

        uint8 cfg_type = pciDev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_CFG_TYPE);
        uint8 bar_num  = pciDev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_BAR);
        uint32 offset  = pciDev->ReadConfigLong(cap->CapOffset + VIRTIO_CAP_OFF_OFFSET);

        struct PCIResourceRange *bar = pciDev->GetResourceRange(bar_num);
        if (!bar) {
            DPRINTF("pci_modern_detect: cfg_type=%u: BAR%u not available.\n",
                    (uint32)cfg_type, (uint32)bar_num);
            cap = pciDev->GetNextCapability(cap);
            continue;
        }

        /*
         * Use BaseAddress (CPU-visible mapped address), NOT Physical (PCI bus
         * address).  On Pegasos2 (MV64361) these differ: Physical is the PCI-
         * side address while BaseAddress is where the CPU can actually reach
         * the MMIO region.  Using Physical causes all MMIO reads to return 0.
         */
        uint32 addr = bar->BaseAddress + offset;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            handler->common_cfg_base = addr;
            handler->modern_mode = TRUE;
            DPRINTF("pci_modern_detect: COMMON_CFG BAR%u+0x%lX -> 0x%08lX\n",
                    (uint32)bar_num, offset, addr);
            break;

        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            handler->notify_cfg_base = addr;
            handler->notify_off_mult = pciDev->ReadConfigLong(
                cap->CapOffset + VIRTIO_CAP_OFF_NOTIFY_MULT);
            DPRINTF("pci_modern_detect: NOTIFY_CFG BAR%u+0x%lX -> 0x%08lX mult=%lu\n",
                    (uint32)bar_num, offset, addr, handler->notify_off_mult);
            break;

        case VIRTIO_PCI_CAP_ISR_CFG:
            handler->isr_cfg_base = addr;
            DPRINTF("pci_modern_detect: ISR_CFG BAR%u+0x%lX -> 0x%08lX\n",
                    (uint32)bar_num, offset, addr);
            break;

        case VIRTIO_PCI_CAP_DEVICE_CFG:
            handler->device_cfg_base = addr;
            DPRINTF("pci_modern_detect: DEVICE_CFG BAR%u+0x%lX -> 0x%08lX\n",
                    (uint32)bar_num, offset, addr);
            break;

        default:
            break;
        }

        cap = pciDev->GetNextCapability(cap);
    }

    /*
     * MMIO probe: verify modern VirtIO MMIO actually works on this bridge
     * before committing to modern mode.  Transitional devices (0x1009)
     * advertise the modern capabilities on every QEMU machine, but MMIO
     * only succeeds if the bridge forwards CPU memory cycles to device BARs.
     *
     *   Pegasos2 (MV64361 transparent bridge): probe passes → modern mode
     *   AmigaOne (Articia S floating buffer):  probe fails  → legacy fallback
     *
     * Probe sequence:
     *   1. Enable PCI Memory Space + Bus Master (required on MV64361)
     *   2. Reset device (STATUS = 0)
     *   3. Write STATUS = ACKNOWLEDGE (0x01)
     *   4. Read STATUS back — match means MMIO is live
     *   5. Reset again to leave clean state for InitVirtIO
     */
    if (handler->modern_mode && handler->common_cfg_base) {
        uint16 pci_cmd = pciDev->ReadConfigWord(PCI_COMMAND);
        if (!(pci_cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))) {
            pciDev->WriteConfigWord(PCI_COMMAND,
                                    pci_cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
            DPRINTF("pci_modern_detect: Enabled PCI Memory+BusMaster for probe (was 0x%04lX)\n",
                    (uint32)pci_cmd);
        }

        uint32 base = handler->common_cfg_base;

        mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, 0x00);
        mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
        uint8 probe = mmio_r8(pciDev, base + VIRTIO_PCI_COMMON_STATUS);
        mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, 0x00);

        if (probe == VIRTIO_STATUS_ACKNOWLEDGE) {
            DPRINTF("pci_modern_detect: MMIO probe OK (status=0x%02X) — modern mode confirmed.\n",
                    (uint32)probe);
        } else {
            DPRINTF("pci_modern_detect: MMIO probe FAILED (status=0x%02X, expected 0x01) — "
                    "falling back to legacy.\n",
                    (uint32)probe);
            handler->modern_mode = FALSE;
        }
    }

    DPRINTF("pci_modern_detect: VirtIO mode: %s\n",
            handler->modern_mode ? "MODERN" : "LEGACY");

    return handler->modern_mode;
}
