#include "virtio/virtio_irq.h"
#include "virtio/virtio_pci_modern.h"
#include "virtio/virtio_pci.h"
#include "virtio9p_handler.h"

static uint32 V9P_InterruptHandler(struct ExceptionContext *ctx, struct ExecBase *SysBase, APTR is_Data)
{
    struct V9PHandler *handler = (struct V9PHandler *)is_Data;

    (void)ctx;

    /* Guard against is_Data being NULL/invalid during handler teardown.
     * During shutdown, the handler process may be killed before
     * RemIntServer completes — is_Data is nulled first as a signal. */
    if (!handler)
        return 0;

    struct PCIDevice *pciDev = handler->pciDevice;
    if (!pciDev)
        return 0;

    uint8 isr;
    if (handler->modern_mode) {
        isr = mmio_r8(pciDev, handler->isr_cfg_base);
    } else {
        isr = pciDev->InByte(handler->iobase + VIRTIO_PCI_ISR);
    }

    if (isr == 0)
        return 0;

    if (isr & 1) {
        /* Signal the handler task */
        struct ExecIFace *IExec = (struct ExecIFace *)((struct ExecBase *)SysBase)->MainInterface;
        if (handler->handler_task) {
            IExec->Signal(handler->handler_task, handler->irq_signal);
        }
    }

    return 1;
}

BOOL V9P_InstallInterrupt(struct V9PHandler *handler)
{
    handler->irq_number = handler->pciDevice->MapInterrupt();

    DPRINTF("IRQ: MapInterrupt returned vector %lu\n", handler->irq_number);

    if (handler->irq_number == 0) {
        DPRINTF("IRQ: MapInterrupt failed\n");
        return FALSE;
    }

    handler->irq_handler.is_Node.ln_Type = NT_INTERRUPT;
    handler->irq_handler.is_Node.ln_Pri = 0;
    handler->irq_handler.is_Node.ln_Name = "Virtio9PFS";
    handler->irq_handler.is_Data = (APTR)handler;
    handler->irq_handler.is_Code = (VOID (*)())V9P_InterruptHandler;

    BOOL ok = IExec->AddIntServer(handler->irq_number, &handler->irq_handler);

    if (!ok) {
        DPRINTF("IRQ: AddIntServer failed for vector %lu\n", handler->irq_number);
        return FALSE;
    }

    handler->irq_installed = TRUE;
    DPRINTF("IRQ: Interrupt handler installed on vector %lu\n", handler->irq_number);

    return TRUE;
}

void V9P_RemoveInterrupt(struct V9PHandler *handler)
{
    if (!handler->irq_installed)
        return;

    /* Quiesce the device first — stop it from generating new interrupts.
     * The ISR is still registered so any final pending interrupt is handled
     * safely before we remove it from the chain. */
    if (handler->pciDevice) {
        if (handler->modern_mode) {
            if (handler->isr_cfg_base)
                (void)mmio_r8(handler->pciDevice, handler->isr_cfg_base);
            if (handler->common_cfg_base)
                mmio_w8(handler->pciDevice,
                        handler->common_cfg_base + VIRTIO_PCI_COMMON_STATUS,
                        0x00);
        } else if (handler->bar0) {
            (void)handler->pciDevice->InByte(
                handler->iobase + VIRTIO_PCI_ISR);
            handler->pciDevice->OutByte(
                handler->iobase + VIRTIO_PCI_STATUS, 0x00);
        }
    }

    /* Null is_Data inside a Disable/Enable section so the ISR
     * returns immediately if it fires between here and RemIntServer. */
    IExec->Disable();
    handler->irq_handler.is_Data = NULL;
    IExec->Enable();

    IExec->RemIntServer(handler->irq_number, &handler->irq_handler);
    handler->irq_installed = FALSE;

    DPRINTF("IRQ: Interrupt handler removed\n");
}
