#include "virtio/virtqueue.h"
#include "virtio/virtio_pci.h"
#include "virtio/virtio_pci_modern.h"
#include "virtio9p_handler.h"
#include <exec/exectags.h>
#include <exec/memory.h>
#include <expansion/pci.h>
#include <interfaces/exec.h>
#include <interfaces/expansion.h>

/*
 * VirtQueue Layout (VirtIO Spec Section 2.7.2)
 *
 * Queue Align = 4096:
 *   Descriptor Table:  offset 0
 *   Available Ring:    immediately after descriptors
 *   (padding to next 4096-byte boundary)
 *   Used Ring:         aligned to 4096
 *
 * Endianness:
 *   Legacy (vq->modern == FALSE): Native guest endian (BE on PPC) — no swap.
 *   Modern (vq->modern == TRUE):  Little-endian — swap via vr16/vr32/vr64.
 */

#define VIRTIO_PCI_VRING_ALIGN 4096

struct virtqueue *VirtQueue_Allocate(struct ExecIFace *IExec, uint32 queue_index, uint32 queue_size)
{
    uint32 desc_size = sizeof(struct vring_desc) * queue_size;
    uint32 avail_size = sizeof(uint16) * (3 + queue_size);
    uint32 avail_end = desc_size + avail_size;

    uint32 used_offset = (avail_end + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);
    uint32 used_size = sizeof(uint16) * 3 + sizeof(struct vring_used_elem) * queue_size;
    uint32 total_mem = used_offset + used_size;

    uint32 alloc_size = total_mem + VIRTIO_PCI_VRING_ALIGN;
    void *raw = IExec->AllocVecTags(alloc_size, AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED, TAG_DONE);

    if (!raw)
        return NULL;

    uint32 raw_addr = (uint32)raw;
    uint32 aligned_addr = (raw_addr + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);
    uint8 *base = (uint8 *)aligned_addr;

    struct virtqueue *vq =
        IExec->AllocVecTags(sizeof(struct virtqueue), AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED, TAG_DONE);

    if (!vq) {
        IExec->FreeVec(raw);
        return NULL;
    }

    void **cookies =
        IExec->AllocVecTags(sizeof(void *) * queue_size, AVT_ClearWithValue, 0, AVT_Type, MEMF_PRIVATE, TAG_DONE);

    if (!cookies) {
        IExec->FreeVec(raw);
        IExec->FreeVec(vq);
        return NULL;
    }

    void **indirect_tables =
        IExec->AllocVecTags(sizeof(void *) * queue_size, AVT_ClearWithValue, 0, AVT_Type, MEMF_PRIVATE, TAG_DONE);

    if (!indirect_tables) {
        IExec->FreeVec(cookies);
        IExec->FreeVec(raw);
        IExec->FreeVec(vq);
        return NULL;
    }

    vq->index = queue_index;
    vq->num = queue_size;
    vq->mem_block = raw;
    vq->mem_size = total_mem;
    vq->cookies = cookies;
    vq->indirect_tables = indirect_tables;

    vq->desc = (struct vring_desc *)base;
    vq->avail = (struct vring_avail *)(base + desc_size);
    vq->used = (struct vring_used *)(base + used_offset);

    vq->free_head = 0;
    vq->num_free = (uint16)queue_size;
    vq->last_used_idx = 0;
    vq->modern = FALSE;
    vq->notify_addr = 0;
    vq->avail_phys = 0;
    vq->used_phys  = 0;

    uint32 j;
    for (j = 0; j < queue_size - 1; j++) {
        vq->desc[j].next = (uint16)(j + 1);
    }
    vq->desc[queue_size - 1].next = 0;

    vq->avail->flags = 0;

    return vq;
}

void VirtQueue_Free(struct ExecIFace *IExec, struct virtqueue *vq)
{
    if (!vq)
        return;
    if (vq->dma_entries > 0 && vq->desc) {
        IExec->EndDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
        vq->dma_entries = 0;
    }
    if (vq->indirect_tables)
        IExec->FreeVec(vq->indirect_tables);
    if (vq->cookies)
        IExec->FreeVec(vq->cookies);
    if (vq->mem_block)
        IExec->FreeVec(vq->mem_block);
    IExec->FreeVec(vq);
}

int32 VirtQueue_AddBuf(struct ExecIFace *IExec, struct virtqueue *vq,
                       struct vring_sg *sg, uint32 out_num, uint32 in_num,
                       void *cookie)
{
    uint32 total = out_num + in_num;

    if (vq->num_free < total)
        return -1;

    uint16 head = vq->free_head;
    uint16 idx = head;
    uint32 n;

    for (n = 0; n < total; n++) {
        uint16 desc_flags = 0;

        if (n >= out_num)
            desc_flags |= VRING_DESC_F_WRITE;

        if (n < total - 1)
            desc_flags |= VRING_DESC_F_NEXT;

        uint16 next_idx = vq->desc[idx].next;
        vq->desc[idx].addr  = vr64(vq->modern, (uint64)sg[n].addr);
        vq->desc[idx].len   = vr32(vq->modern, sg[n].len);
        vq->desc[idx].flags = vr16(vq->modern, desc_flags);
        if (n < total - 1) {
            vq->desc[idx].next = vr16(vq->modern, next_idx);
            idx = next_idx;
        } else {
            vq->free_head = next_idx;
        }
    }

    vq->num_free -= (uint16)total;

    vq->cookies[head] = cookie;
    vq->indirect_tables[head] = NULL;

    uint16 avail_idx = vr16(vq->modern, vq->avail->idx);
    vq->avail->ring[avail_idx % vq->num] = vr16(vq->modern, head);

    __asm__ volatile("eieio" ::: "memory");

    vq->avail->idx = vr16(vq->modern, (uint16)(avail_idx + 1));

    return 0;
}

void VirtQueue_Kick(struct ExecIFace *IExec, struct virtqueue *vq,
                    struct PCIDevice *pciDev, uint32 iobase)
{
    (void)IExec;
    __asm__ volatile("sync" ::: "memory");

    if (vq->modern) {
        mmio_w16(pciDev, vq->notify_addr, (uint16)vq->index);
    } else {
        pciDev->OutWord(iobase + VIRTIO_PCI_QUEUE_NOTIFY, (uint16)vq->index);
    }
}

void *VirtQueue_GetBuf(struct ExecIFace *IExec, struct virtqueue *vq, uint32 *len_out)
{
    __asm__ volatile("lwsync" ::: "memory");

    if (vq->last_used_idx == vr16(vq->modern, vq->used->idx))
        return NULL;

    uint16 used_slot = vq->last_used_idx % vq->num;
    uint32 desc_id = vr32(vq->modern, vq->used->ring[used_slot].id);
    uint32 written = vr32(vq->modern, vq->used->ring[used_slot].len);

    /* Validate descriptor index from device to prevent out-of-bounds access */
    if (desc_id >= vq->num)
        return NULL;

    if (len_out)
        *len_out = written;

    void *cookie = vq->cookies[desc_id];
    vq->cookies[desc_id] = NULL;

    void *itbl_virt = vq->indirect_tables[desc_id];
    vq->indirect_tables[desc_id] = NULL;

    uint16 idx = (uint16)desc_id;
    uint32 freed = 0;

    if (itbl_virt) {
        uint32 itbl_size = vr32(vq->modern, vq->desc[idx].len);
        IExec->EndDMA(itbl_virt, itbl_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(itbl_virt);

        vq->desc[idx].addr  = 0;
        vq->desc[idx].len   = 0;
        vq->desc[idx].flags = 0;
        vq->desc[idx].next  = vq->free_head;
        vq->free_head = idx;
        freed = 1;
    } else {
        while (1) {
            uint16 next     = vr16(vq->modern, vq->desc[idx].next);
            uint16 has_next = vr16(vq->modern, vq->desc[idx].flags) & VRING_DESC_F_NEXT;

            vq->desc[idx].addr  = 0;
            vq->desc[idx].len   = 0;
            vq->desc[idx].flags = 0;
            vq->desc[idx].next  = vq->free_head;
            vq->free_head = idx;
            freed++;

            if (!has_next)
                break;
            idx = next;
        }
    }

    vq->num_free += (uint16)freed;

    vq->last_used_idx++;

    if (vq->use_event_idx) {
        vq->avail->ring[vq->num] = vr16(vq->modern, vq->last_used_idx);
        __asm__ volatile("eieio" ::: "memory");
    }

    return cookie;
}
