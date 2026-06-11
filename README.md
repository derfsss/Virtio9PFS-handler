# Virtio9PFS-handler

A FileSysBox-based filesystem handler for AmigaOS 4.1 Final Edition that mounts QEMU host-shared folders as DOS volumes via VirtIO 9P (9P2000.L).

**Status:** Beta (v0.10.0) — tested on QEMU AmigaOne (legacy VirtIO), Pegasos2 (modern VirtIO), and SAM460ex.

> ⚠️ **Beta — actively under development.** Expect bugs and rough
> edges; do not rely on it for anything important. Use at your own
> risk.

---

## Overview

When running AmigaOS under QEMU with a `-virtfs` shared folder, this
handler presents the host directory as a native AmigaOS volume (e.g.
`SHARED:`). You can browse, copy, create, rename, and delete files on
the host filesystem directly from Workbench or the Shell.

This is a **handler**, not a device driver: there is no `.device`
file, no `BeginIO`/`AbortIO`, and no block layer. The 9P protocol is
file-level, so the handler IS the filesystem. FileSysBox handles all
DOS packet translation; the handler implements FUSE callbacks.

**Important:** Official QEMU for Windows (x64) does not include
`-virtfs` support, but it can be patched — see
[Windows QEMU Setup](#windows-qemu-setup) below.

---

## Features

- **9P2000.L protocol** — full Linux client dialect for maximum QEMU compatibility
- **Dual VirtIO transport** — supports both legacy (device 0x1009) and modern
  VirtIO 1.0 (device 0x1049) PCI devices
- **Legacy mode** — I/O BAR access, native big-endian vring fields; tested on
  AmigaOne (Articia S)
- **Modern mode** — MMIO BAR access via `stwbrx`/`lwbrx` inline asm,
  little-endian vring fields; tested on Pegasos2 (MV64361) and SAM460ex
- **FileSysBox FUSE interface** — implements 25 FUSE callbacks; all DOS packet
  handling is done by `filesysbox.library` v54+
- **Full filesystem operations** — directory listing, file read/write, create,
  delete, rename, mkdir, rmdir, truncate, chmod, chown, statfs, utimens,
  fsync, symlink, readlink, hard link
- **Large file support** — reads and writes larger than msize are automatically
  split into multiple 9P transactions
- **512 KB message size** — negotiates 512 KB msize with QEMU for maximum
  throughput; a 1 MB file transfer needs only 2 round-trips instead of 16
- **DMA-safe buffers** — `MEMF_SHARED` allocations with cached physical
  addresses and PPC `dcbst`/`dcbf` cache management for zero-copy DMA
- **Wall-clock transaction timeout** — every 9P transaction has a 10-second
  PPC time-base budget; stalled transactions are cancelled with a Tflush
  and the FID is marked orphaned
- **Graceful no-device boot** — if QEMU is started without a 9P device, the
  handler declines the mount silently and boot continues normally
- **Automatic installer** — AmigaOS Shell script copies handler and DOSDriver

---

## Requirements

- AmigaOS 4.1 Final Edition (or later)
- `filesysbox.library` v54 or newer (included with OS 4.1 FE)
- QEMU 10.0+ with VirtIO 9P support (Linux, WSL2, macOS, or patched Windows —
  see [Windows QEMU Setup](#windows-qemu-setup))
- A supported QEMU PPC machine (tested on AmigaOne, Pegasos2, and SAM460ex)

---

## QEMU Setup

Add the `-virtfs` option to your QEMU command line to share a host folder:

```sh
qemu-system-ppc -M amigaone \
    -virtfs local,path=/path/to/shared/folder,mount_tag=SHARED,security_model=none,id=share0 \
    ...
```

The `mount_tag` must match the DOSDriver name (e.g. `SHARED` for
`DEVS:DOSDrivers/SHARED`). QEMU automatically creates the appropriate
VirtIO 9P PCI device for the machine type.

### Windows QEMU Setup

Official Windows QEMU builds do not include VirtIO 9P (`-virtfs`)
support. You can patch and rebuild QEMU yourself to enable it.
Community-maintained patches are available at:

- **Pre-built Windows QEMU binaries with 9P patches:**
  https://github.com/arixmkii/qcw/tags
- **Patch files (to apply to upstream QEMU source):**
  https://github.com/arixmkii/qcw/tree/main/patches/qemu

On Windows, the `-virtfs` shorthand may not work. Use the explicit
`-fsdev` + `-device` syntax instead:

```
-fsdev local,security_model=mapped-xattr,id=fsdev0,path=D:\SHARED
-device virtio-9p-pci-non-transitional,id=fs0,fsdev=fsdev0,mount_tag=SHARED
```

**Notes:**
- `virtio-9p-pci-non-transitional` (modern VirtIO 1.0) is required for
  Pegasos2. For AmigaOne, use `virtio-9p-pci` (legacy) instead.
- `security_model=mapped-xattr` stores Unix permission metadata in NTFS
  Alternate Data Streams, allowing `protect` (chmod) to work on the
  shared volume. `security_model=none` passes through host permissions
  directly.

---

## Installation

### Quick install

Extract the distribution archive and double-click the **install.py**
icon in the extracted drawer — the AmigaOS **Installation Utility**
wizard copies the handler to `L:` and the DOSDriver to
`DEVS:DOSDrivers/`, then offers a reboot to activate the `SHARED:`
volume.  From a shell:

```
cd RAM:Virtio9PFS
"SYS:Utilities/Installation Utility" PACKAGE=install.py
```

(the drawer must be the current directory).

### Manual install

1. Copy `Virtio9PFS-handler` to `L:` on your AmigaOS system
2. Copy `SHARED` to `DEVS:DOSDrivers/SHARED`
3. Reboot to activate (or mount manually)

### DOSDriver configuration

The included `DOSDriver/SHARED` file contains:

```
Handler   = L:Virtio9PFS-handler
StackSize = 65536
Priority  = 5
GlobVec   = -1
Activate  = 1
Control   = "auto"
```

- `Activate = 1` — auto-mount on boot
- `Control = "auto"` — auto-detect the first VirtIO 9P PCI device
- The volume name matches the QEMU `mount_tag` parameter

If the machine is booted **without** a VirtIO 9P device (e.g. QEMU
started without `-virtfs`), the handler declines the mount silently:
no requester appears, boot continues normally, and the volume simply
does not exist. The DOSDriver can stay installed permanently
regardless of whether the device is present.

---

## Building from Source

Cross-compile from a Linux host (or WSL2) using the AmigaOS GCC 11
Docker image:

```sh
docker run --rm -v /path/to/repo:/src -w /src \
    walkero/amigagccondocker:os4-gcc11 make clean
docker run --rm -v /path/to/repo:/src -w /src \
    walkero/amigagccondocker:os4-gcc11 make all
```

The handler binary is output to `build/Virtio9PFS-handler`.

### Debug build

Compile with `-DDEBUG` to enable serial debug output (QEMU serial
console or Sashimi on real hardware). All debug messages are prefixed
with `[virtio9p]`:

```sh
docker run --rm -v /path/to/repo:/src -w /src \
    walkero/amigagccondocker:os4-gcc11 make CFLAGS="-O2 -Wall -I./include -fno-tree-loop-distribute-patterns -DDEBUG"
```

---

## Project Structure

```
include/
├── version.h               Version string macros
├── debug.h                 Debug output macros (DPRINTF)
├── string_utils.h          Inline string/memory helpers (no newlib)
├── virtio9p_handler.h      Main handler state struct (V9PHandler)
├── p9_protocol.h           9P message types, constants, wire format
├── p9_client.h             9P client session API
├── fid_pool.h              FID number allocator
├── pci/                    PCI discovery + capability walking
└── virtio/                 VirtIO registers, virtqueue, init, IRQ

src/
├── main.c                  Handler entry (_start), FileSysBox setup
├── fuse_ops.c              FUSE callbacks (25 operations)
├── p9_client.c             9P session + V9P_Transact
├── p9_marshal.c            9P wire format marshal/unmarshal
├── fid_pool.c              FID allocator implementation
├── pci/                    Modern/legacy PCI scan
└── virtio/                 Dual-mode VirtIO init, virtqueue, ISR

DOSDriver/SHARED            Example DOSDriver mount entry
install.py                  AmigaOS Installation Utility wizard
```

### Architecture notes

Endianness at each layer:

| Layer | Endianness | Swap? |
|-------|-----------|-------|
| 9P wire protocol | Always LE | Byte-by-byte extraction (natural conversion on BE PPC) |
| Vring fields (legacy) | Native BE | No swap (`vr16`/`vr32`/`vr64` = no-op) |
| Vring fields (modern) | LE | Swap via `vr16`/`vr32`/`vr64` (`__builtin_bswap`) |
| VirtIO config (legacy) | Host-native | PCI I/O functions handle it |
| VirtIO config (modern) | LE | `stwbrx`/`lwbrx` handle LE conversion |

Completion handling is polled (fast used-ring polling with
`VRING_AVAIL_F_NO_INTERRUPT`) to avoid signal-based `Wait()` conflicts
with the FileSysBox event loop.

---

## Documentation

- [CHANGELOG.md](CHANGELOG.md) — full release history.

**Current: 0.10.0-beta** — graceful exit when no VirtIO 9P device is
present: the handler declines the mount without raising a blocking
boot requester and removes its device node so DOS does not relaunch it
on every volume reference. Inherits the v0.9.x fixes: ftruncate
return-value convention, tag-matched 9P transport, dedicated Tflush
buffer, held-open DMA mappings, V9P_Reset() recovery, FID orphan
tracking, PPC time-base wallclock timeouts, lwsync barriers for
cacheable RAM.

---

## Development

This project was created with help from [ClaudeAI](https://claude.ai)
(Anthropic) — writing the C code, designing the architecture, and
debugging hardware-level issues against the AmigaOS 4.1 SDK — with a
human developer directing, reviewing, and testing the result.

## License

Copyright © 2026. All rights reserved.
