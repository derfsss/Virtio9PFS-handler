#include "virtio/virtio_irq.h"
#include "virtio/virtio_pci_modern.h"
#include "virtio/virtio_pci.h"
#include "virtio9p_handler.h"

static uint32 V9P_InterruptHandler(struct ExceptionContext *ctx, struct ExecBase *SysBase, APTR is_Data)
{
    struct V9PHandler *handler = (struct V9PHandler *)is_Data;
    struct PCIDevice *pciDev = handler->pciDevice;

    (void)ctx;

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
    if (handler->irq_installed) {
        IExec->RemIntServer(handler->irq_number, &handler->irq_handler);
        handler->irq_installed = FALSE;
        DPRINTF("IRQ: Interrupt handler removed\n");
    }
}
