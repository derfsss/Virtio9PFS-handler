#include "virtio/virtio_init.h"
#include "virtio/virtio_pci_modern.h"
#include "virtio/virtio_pci.h"
#include "virtio/virtqueue.h"
#include "virtio9p_handler.h"

/* Forward declaration */
static BOOL V9P_InitVirtIO_Modern(struct V9PHandler *handler);

BOOL V9P_InitVirtIO(struct V9PHandler *handler)
{
    struct PCIDevice *pciDev = handler->pciDevice;

    if (!pciDev) {
        DPRINTF("InitVirtIO: No PCI device handle.\n");
        return FALSE;
    }

    if (handler->modern_mode)
        return V9P_InitVirtIO_Modern(handler);

    /* --- Legacy path --- */
    if (!handler->bar0) {
        DPRINTF("InitVirtIO: BAR0 not available.\n");
        return FALSE;
    }

    uint32 iobase = handler->iobase;

    DPRINTF("InitVirtIO: Legacy init at I/O 0x%08lX\n", iobase);

    /* Reset */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS, 0x00);
    pciDev->InByte(iobase + VIRTIO_PCI_ISR);

    /* ACKNOWLEDGE */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* DRIVER */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS,
                    VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation */
    uint32 host_features = pciDev->InLong(iobase + VIRTIO_PCI_HOST_FEATURES);
    DPRINTF("InitVirtIO: Host features: 0x%08lX\n", host_features);

    /* Accept MOUNT_TAG (bit 0) + EVENT_IDX (bit 29) */
    uint32 guest_features = host_features & (VIRTIO_9P_F_MOUNT_TAG | (1UL << VIRTIO_F_EVENT_IDX));
    BOOL use_event_idx = (guest_features & (1UL << VIRTIO_F_EVENT_IDX)) != 0;

    DPRINTF("InitVirtIO: Guest features: 0x%08lX\n", guest_features);
    pciDev->OutLong(iobase + VIRTIO_PCI_GUEST_FEATURES, guest_features);

    /* VirtQueue Setup — single queue (index 0) */
    pciDev->OutWord(iobase + VIRTIO_PCI_QUEUE_SEL, 0);
    uint16 q_max = pciDev->InWord(iobase + VIRTIO_PCI_QUEUE_NUM);

    if (q_max == 0) {
        DPRINTF("InitVirtIO: Queue 0 unavailable.\n");
        return FALSE;
    }

    DPRINTF("InitVirtIO: Queue 0 max size: %u\n", q_max);

    struct virtqueue *vq = VirtQueue_Allocate(IExec, 0, q_max);
    if (!vq) {
        DPRINTF("InitVirtIO: Failed to allocate Queue 0.\n");
        return FALSE;
    }

    /* DMA-map the vring */
    uint32 vring_entries = IExec->StartDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM);
    if (vring_entries == 0) {
        DPRINTF("InitVirtIO: StartDMA failed for queue 0\n");
        VirtQueue_Free(IExec, vq);
        return FALSE;
    }

    struct DMAEntry *vring_dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, vring_entries, TAG_DONE);
    if (!vring_dma) {
        IExec->EndDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
        VirtQueue_Free(IExec, vq);
        return FALSE;
    }

    IExec->GetDMAList(vq->desc, vq->mem_size, DMA_ReadFromRAM, vring_dma);
    uint32 phys_addr = (uint32)vring_dma[0].PhysicalAddress;
    IExec->FreeSysObject(ASOT_DMAENTRY, vring_dma);

    vq->dma_phys = phys_addr;
    vq->dma_entries = vring_entries;

    uint32 pfn = phys_addr / 4096;
    pciDev->OutLong(iobase + VIRTIO_PCI_QUEUE_PFN, pfn);

    vq->modern = FALSE;
    vq->use_event_idx = use_event_idx;
    vq->last_kick_avail_idx = 0xFFFF;
    vq->use_indirect = FALSE;

    handler->vq = vq;

    DPRINTF("InitVirtIO: Queue 0 configured, phys=0x%08lX PFN=0x%08lX\n",
            phys_addr, pfn);

    /* DRIVER_OK */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS,
                    VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    uint8 final_status = pciDev->InByte(iobase + VIRTIO_PCI_STATUS);
    DPRINTF("InitVirtIO: Complete. Status=0x%02X\n", final_status);

    return TRUE;
}

static BOOL V9P_InitVirtIO_Modern(struct V9PHandler *handler)
{
    struct PCIDevice *pciDev = handler->pciDevice;
    uint32 base = handler->common_cfg_base;

    DPRINTF("InitVirtIO_Modern: common_cfg=0x%08lX\n", base);

    /* Enable PCI Memory Space + Bus Master */
    {
        uint16 pci_cmd = pciDev->ReadConfigWord(PCI_COMMAND);
        if (!(pci_cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))) {
            pciDev->WriteConfigWord(PCI_COMMAND, pci_cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        }
        uint32 caps = pciDev->GetCapabilities();
        if (!(caps & PCI_CAP_BUSMASTER)) {
            pciDev->SetCapabilities(PCI_CAP_BUSMASTER | PCI_CAP_SETCLR);
        }
    }

    /* Reset */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, 0x00);
    {
        uint32 tries = 0;
        uint8 rst;
        do {
            rst = mmio_r8(pciDev, base + VIRTIO_PCI_COMMON_STATUS);
            tries++;
        } while (rst != 0 && tries < 1000);
    }

    /* ACKNOWLEDGE */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* DRIVER */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation (two 32-bit words) */
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECT, 0);
    uint32 dev_feat_lo = mmio_r32(pciDev, base + VIRTIO_PCI_COMMON_DF);

    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECT, 1);
    uint32 dev_feat_hi = mmio_r32(pciDev, base + VIRTIO_PCI_COMMON_DF);

    DPRINTF("InitVirtIO_Modern: Device features hi=0x%08lX lo=0x%08lX\n",
            dev_feat_hi, dev_feat_lo);

    /* Accept MOUNT_TAG (bit 0) + EVENT_IDX (bit 29) + VERSION_1 (bit 0 hi) */
    uint32 drv_feat_lo = dev_feat_lo & (VIRTIO_9P_F_MOUNT_TAG | (1UL << VIRTIO_F_EVENT_IDX));
    uint32 drv_feat_hi = dev_feat_hi & 1UL; /* VIRTIO_F_VERSION_1 */

    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECTG, 0);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFG, drv_feat_lo);

    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECTG, 1);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFG, drv_feat_hi);

    BOOL use_event_idx = (drv_feat_lo & (1UL << VIRTIO_F_EVENT_IDX)) != 0;

    /* FEATURES_OK */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    uint8 status_check = mmio_r8(pciDev, base + VIRTIO_PCI_COMMON_STATUS);
    if (!(status_check & VIRTIO_STATUS_FEATURES_OK)) {
        DPRINTF("InitVirtIO_Modern: FEATURES_OK rejected! Status=0x%02X\n",
                (uint32)status_check);
        mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return FALSE;
    }

    /* VirtQueue Setup — single queue (index 0) */
    mmio_w16(pciDev, base + VIRTIO_PCI_COMMON_Q_SELECT, 0);
    uint16 q_max = mmio_r16(pciDev, base + VIRTIO_PCI_COMMON_Q_SIZE);

    if (q_max == 0) {
        DPRINTF("InitVirtIO_Modern: Queue 0 unavailable.\n");
        return FALSE;
    }

    DPRINTF("InitVirtIO_Modern: Queue 0 max size: %u\n", (uint32)q_max);

    struct virtqueue *vq = VirtQueue_Allocate(IExec, 0, q_max);
    if (!vq) {
        DPRINTF("InitVirtIO_Modern: Failed to allocate Queue 0.\n");
        return FALSE;
    }

    /* DMA-map the vring */
    uint32 vring_entries = IExec->StartDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM);
    if (vring_entries == 0) {
        VirtQueue_Free(IExec, vq);
        return FALSE;
    }

    struct DMAEntry *vring_dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, vring_entries, TAG_DONE);
    if (!vring_dma) {
        IExec->EndDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
        VirtQueue_Free(IExec, vq);
        return FALSE;
    }

    IExec->GetDMAList(vq->desc, vq->mem_size, DMA_ReadFromRAM, vring_dma);
    uint32 desc_phys = (uint32)vring_dma[0].PhysicalAddress;
    IExec->FreeSysObject(ASOT_DMAENTRY, vring_dma);

    vq->dma_phys    = desc_phys;
    vq->dma_entries = vring_entries;

    uint32 desc_size   = sizeof(struct vring_desc) * q_max;
    uint32 avail_size  = sizeof(uint16) * (3 + q_max);
    uint32 used_offset = (desc_size + avail_size + 4095U) & ~4095U;

    vq->avail_phys = desc_phys + desc_size;
    vq->used_phys  = desc_phys + used_offset;

    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_DESCLO,  desc_phys);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_DESCHI,  0);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_AVAILLO, vq->avail_phys);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_AVAILHI, 0);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_USEDLO,  vq->used_phys);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_USEDHI,  0);

    mmio_w16(pciDev, base + VIRTIO_PCI_COMMON_Q_ENABLE, 1);

    uint16 q_noff = mmio_r16(pciDev, base + VIRTIO_PCI_COMMON_Q_NOFF);
    uint32 notify_mult = handler->notify_off_mult;
    vq->notify_addr = handler->notify_cfg_base +
                      (notify_mult ? (uint32)q_noff * notify_mult : 0);

    vq->modern = TRUE;
    vq->use_event_idx = use_event_idx;
    vq->last_kick_avail_idx = 0xFFFF;
    vq->use_indirect = FALSE;

    handler->vq = vq;

    DPRINTF("InitVirtIO_Modern: Q0 desc=0x%08lX avail=0x%08lX used=0x%08lX notify=0x%08lX\n",
            desc_phys, vq->avail_phys, vq->used_phys, vq->notify_addr);

    /* DRIVER_OK */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    uint8 final_status = mmio_r8(pciDev, base + VIRTIO_PCI_COMMON_STATUS);
    DPRINTF("InitVirtIO_Modern: Complete. Status=0x%02X\n", (uint32)final_status);

    return TRUE;
}

void V9P_ReadMountTag(struct V9PHandler *handler)
{
    struct PCIDevice *pciDev = handler->pciDevice;
    uint16 tag_len;
    uint16 i;

    if (handler->modern_mode) {
        /* Modern: read from DEVICE_CFG region with config_generation retry */
        uint8 gen1, gen2;
        do {
            gen1 = mmio_r8(pciDev, handler->common_cfg_base + VIRTIO_PCI_COMMON_CFGGEN);
            tag_len = mmio_r16(pciDev, handler->device_cfg_base + 0);
            if (tag_len > 32) tag_len = 32;
            for (i = 0; i < tag_len; i++) {
                handler->mount_tag[i] = (char)mmio_r8(pciDev, handler->device_cfg_base + 2 + i);
            }
            gen2 = mmio_r8(pciDev, handler->common_cfg_base + VIRTIO_PCI_COMMON_CFGGEN);
        } while (gen1 != gen2);
    } else {
        /* Legacy: read from I/O BAR at config offset 0x14 */
        uint32 iobase = handler->iobase;
        tag_len = pciDev->InWord(iobase + VIRTIO_PCI_DEVICE_CONFIG_OFFSET);
        if (tag_len > 32) tag_len = 32;
        for (i = 0; i < tag_len; i++) {
            handler->mount_tag[i] = (char)pciDev->InByte(
                iobase + VIRTIO_PCI_DEVICE_CONFIG_OFFSET + 2 + i);
        }
    }

    handler->mount_tag[tag_len] = '\0';

    /* The VirtIO config tag field is fixed at 32 bytes.  QEMU reports
     * tag_len = field size (32), not the actual string length.  Bytes
     * past the real tag are uninitialized garbage.  Truncate at the
     * first null byte or non-printable character. */
    for (i = 0; i < tag_len; i++) {
        uint8 ch = (uint8)handler->mount_tag[i];
        if (ch == 0 || ch < 0x20 || ch > 0x7E) {
            handler->mount_tag[i] = '\0';
            break;
        }
    }

    DPRINTF("ReadMountTag: raw_len=%u tag=\"%s\"\n",
            (uint32)tag_len, handler->mount_tag);
}

void V9P_CleanupVirtIO(struct V9PHandler *handler)
{
    struct PCIDevice *pciDev = handler->pciDevice;

    if (pciDev) {
        if (handler->modern_mode && handler->common_cfg_base) {
            mmio_w8(pciDev, handler->common_cfg_base + VIRTIO_PCI_COMMON_STATUS, 0x00);
        } else if (handler->bar0) {
            pciDev->OutByte(handler->iobase + VIRTIO_PCI_STATUS, 0x00);
        }
        DPRINTF("CleanupVirtIO: Hardware reset issued.\n");
    }

    if (handler->vq) {
        VirtQueue_Free(IExec, handler->vq);
        handler->vq = NULL;
    }
}
