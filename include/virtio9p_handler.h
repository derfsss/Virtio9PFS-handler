#ifndef VIRTIO9P_HANDLER_H
#define VIRTIO9P_HANDLER_H

#include <exec/exec.h>
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <expansion/pci.h>
#include <interfaces/expansion.h>

/* Forward declarations */
struct virtqueue;
struct FidPool;

/* VirtIO 9P PCI Device IDs */
#define V9P_PCI_DEVICE_ID_MODERN  0x1049  /* 0x1040 + VIRTIO_ID_9P(9) */
#define V9P_PCI_DEVICE_ID_LEGACY  0x1009  /* Transitional 9P */

/* VirtIO 9P feature bit */
#define VIRTIO_9P_F_MOUNT_TAG     (1UL << 0)

/* 9P uses a single virtqueue (index 0) */
#define V9P_NUM_QUEUES  1

/*
 * V9PHandler — Top-level handler state.
 *
 * Contains all infrastructure for VirtIO PCI transport (legacy + modern),
 * interrupt handling, 9P session state, and DMA buffers.
 */
struct V9PHandler
{
    /* PCI */
    struct PCIIFace         *IPCI;
    struct PCIDevice        *pciDevice;
    struct PCIResourceRange *bar0;        /* Legacy: I/O port BAR */
    struct PCIResourceRange *bar4;        /* Modern: MMIO BAR (NULL if legacy) */
    uint32                   iobase;      /* Legacy: bar0->Physical */

    /* VirtIO mode */
    BOOL                     modern_mode; /* TRUE = modern (0x1049), FALSE = legacy (0x1009) */

    /* Modern config region addresses (physical, valid only if modern_mode) */
    uint32                   common_cfg_base;
    uint32                   notify_cfg_base;
    uint32                   notify_off_mult;
    uint32                   isr_cfg_base;
    uint32                   device_cfg_base;

    /* VirtIO transport */
    struct virtqueue        *vq;          /* Single VQ, index 0 */
    struct SignalSemaphore   vq_lock;     /* Serialize AddBuf+Kick */

    /* ISR */
    struct Interrupt         irq_handler;
    uint32                   irq_number;
    struct Task             *handler_task;
    uint32                   irq_signal;  /* Signal mask for ISR→handler task */
    BOOL                     irq_installed;

    /* 9P session */
    uint32                   msize;       /* Negotiated max message size */
    uint32                   root_fid;    /* Root directory fid (from Tattach) */
    struct FidPool          *fid_pool;    /* FID allocator */
    uint16                   next_tag;    /* Monotonic tag counter */

    /* Buffers (MEMF_SHARED for DMA) */
    uint8                   *tx_buf;      /* T-message build buffer (msize bytes) */
    uint8                   *rx_buf;      /* R-message receive buffer (msize bytes) */
    uint32                   tx_phys;     /* Cached physical address of tx_buf */
    uint32                   rx_phys;     /* Cached physical address of rx_buf */

    /* Config */
    char                     mount_tag[33]; /* From VirtIO config space, null-terminated */
};

/* Debug output — see debug.h for DPRINTF macro and prefix */
#include "debug.h"

#endif /* VIRTIO9P_HANDLER_H */
