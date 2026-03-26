#include "pci/pci_modern_detect.h"
#include "virtio/virtio_pci_modern.h"
#include "virtio9p_handler.h"

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
            cap = pciDev->GetNextCapability(cap);
            continue;
        }

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

    DPRINTF("pci_modern_detect: VirtIO mode: %s\n",
            handler->modern_mode ? "MODERN" : "LEGACY");

    return handler->modern_mode;
}
