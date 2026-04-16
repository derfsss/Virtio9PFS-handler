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

    /* Try transitional VirtIO 9P first (0x1009), then modern-only (0x1049).
     * Transitional devices work on ALL QEMU machines: the driver auto-detects
     * whether to use legacy I/O or modern MMIO based on hardware capability.
     * Modern-only 0x1049 is kept as fallback for setups using
     * -device virtio-9p-pci-non-transitional. */
    device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4,
                                   FDT_DeviceID, V9P_PCI_DEVICE_ID_LEGACY,
                                   TAG_DONE);

    if (!device) {
        device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4,
                                       FDT_DeviceID, V9P_PCI_DEVICE_ID_MODERN,
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

    /* AmigaOne firmware-chain workaround for 64-bit BAR high DWORD.
     *
     * On AmigaOne with QEMU 10.2.2, the VirtIO modern MMIO BAR (BAR4) is
     * a 64-bit prefetchable memory BAR.  BBoot does not write to the high
     * DWORD, and AmigaOS's later PCI enumerator performs a classic sizing
     * probe (write 0xffffffff, read size, write address back) but fails to
     * write 0 back to the high DWORD.  Result: BAR5 (config offset 0x24)
     * sits at 0xffffffff, placing BAR4 at 0xffffffff84204000 — outside
     * Articia's decoded PCI memory window, so MMIO reads return 0xff and
     * writes are dropped.
     *
     * Fix it at the source: read BAR5; if it's 0xffffffff, write 0 back.
     * This must happen BEFORE GetResourceRange(4) so the AmigaOS PCI
     * library reads a sane high DWORD when computing the BAR's CPU-visible
     * address.  Pegasos2 / SAM460ex are unaffected (VOF programs BAR5=0).
     */
    uint32 bar5 = device->ReadConfigLong(0x24);
    if (bar5 == 0xFFFFFFFFUL) {
        DPRINTF("PCI_Discovery: BAR5 high DWORD is 0xffffffff (AmigaOne PCI probe bug), zeroing.\n");
        device->WriteConfigLong(0x24, 0);
        uint32 bar5_after = device->ReadConfigLong(0x24);
        DPRINTF("PCI_Discovery: BAR5 after fix: 0x%08lX\n", (unsigned long)bar5_after);
    }

    handler->pciDevice = device;
    handler->bar0 = device->GetResourceRange(0);
    handler->bar4 = device->GetResourceRange(4);

    if (handler->bar0) {
        const char *bar0_type = (handler->bar0->Flags & PCI_RANGE_IO) ? "I/O" : "MEM";
        DPRINTF("PCI_Discovery: BAR0 (%s) Physical=0x%08lX Size=%lu\n",
                bar0_type, handler->bar0->Physical, handler->bar0->Size);
        /* Cache legacy iobase up-front — used if modern probe fails. */
        handler->iobase = (uint32)handler->bar0->Physical;
    }

    if (handler->bar4) {
        DPRINTF("PCI_Discovery: BAR4 (MMIO) Physical=0x%08lX Size=%lu\n",
                handler->bar4->Physical, handler->bar4->Size);
    }

    /*
     * Attempt modern VirtIO detection for ALL device types.
     *
     * Transitional devices (0x1009) expose vendor-specific PCI capabilities
     * for modern mode alongside their legacy I/O interface.  V9P_DetectModern
     * walks the capability chain and probes MMIO to verify it actually works
     * on this platform's PCI bridge:
     *
     *   Pegasos2 (MV64361 transparent bridge): MMIO probe passes → modern mode
     *   AmigaOne (Articia S floating buffer):  MMIO probe fails  → legacy I/O
     *
     * Non-transitional devices (0x1049) also go through the probe; if MMIO
     * works the driver uses modern mode, otherwise init will fail gracefully.
     */
    V9P_DetectModern(handler);

    return TRUE;
}
