#include "pci/pci_discovery.h"
#include "pci/pci_modern_detect.h"
#include "virtio9p_handler.h"

BOOL V9P_DiscoverDevice(struct V9PHandler *handler)
{
    struct PCIIFace *IPCI = handler->IPCI;
    struct PCIDevice *device = NULL;

    if (!IPCI) {
        DPRINTF("PCI_Discovery: IPCI interface not available.\n");
        return FALSE;
    }

    DPRINTF("PCI_Discovery: Scanning for VirtIO 9P device...\n");

    /* Try modern VirtIO 1.0 first (0x1049), then legacy transitional (0x1009) */
    device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4,
                                   FDT_DeviceID, V9P_PCI_DEVICE_ID_MODERN,
                                   TAG_DONE);

    if (!device) {
        device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4,
                                       FDT_DeviceID, V9P_PCI_DEVICE_ID_LEGACY,
                                       TAG_DONE);
    }

    if (!device) {
        DPRINTF("PCI_Discovery: No VirtIO 9P device found.\n");
        return FALSE;
    }

    uint16 vendor = device->ReadConfigWord(PCI_VENDOR_ID);
    uint16 devid = device->ReadConfigWord(PCI_DEVICE_ID);

    uint8 bus, dev, fn;
    device->GetAddress(&bus, &dev, &fn);

    DPRINTF("PCI_Discovery: Found device (%04x:%04x) at %02x:%02x.%u\n",
            vendor, devid, (unsigned int)bus, (unsigned int)dev, (unsigned int)fn);

    handler->pciDevice = device;
    handler->bar0 = device->GetResourceRange(0);
    handler->bar4 = device->GetResourceRange(4);

    if (handler->bar0) {
        const char *bar0_type = (handler->bar0->Flags & PCI_RANGE_IO) ? "I/O" : "MEM";
        DPRINTF("PCI_Discovery: BAR0 (%s) Physical=0x%08lX Size=%lu\n",
                bar0_type, handler->bar0->Physical, handler->bar0->Size);
    }

    if (handler->bar4) {
        DPRINTF("PCI_Discovery: BAR4 (MMIO) Physical=0x%08lX Size=%lu\n",
                handler->bar4->Physical, handler->bar4->Size);
    }

    if (devid == V9P_PCI_DEVICE_ID_MODERN) {
        V9P_DetectModern(handler);
    } else {
        handler->modern_mode = FALSE;
        if (handler->bar0) {
            handler->iobase = (uint32)handler->bar0->Physical;
        }
        DPRINTF("PCI_Discovery: Legacy device, iobase=0x%08lX\n", handler->iobase);
    }

    return TRUE;
}
