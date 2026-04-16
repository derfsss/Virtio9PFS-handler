Virtio9PFS-handler
==================

A FileSysBox-based handler for AmigaOS 4.1 FE that mounts QEMU host-shared
folders as DOS volumes via the VirtIO 9P (9P2000.L) protocol.

Status: Beta (v0.7.0) -- tested on QEMU AmigaOne (legacy VirtIO) and
Pegasos2 (modern VirtIO). Use at your own risk.

Important: Official QEMU for Windows (x64) does not include -virtfs
support. However, it can be patched -- see "Windows QEMU Setup" below.


What It Does
------------

When running AmigaOS under QEMU with a -virtfs shared folder, this handler
presents the host directory as a native AmigaOS volume (e.g. SHARED:). You
can browse, copy, create, rename, and delete files on the host filesystem
directly from Workbench or the Shell.


Features
--------

- 9P2000.L protocol -- full Linux client dialect for maximum QEMU
  compatibility
- Dual VirtIO transport -- supports both legacy (device 0x1009) and modern
  VirtIO 1.0 (device 0x1049) PCI devices
- Legacy mode -- I/O BAR access, native big-endian vring fields; tested on
  AmigaOne (Articia S)
- Modern mode -- MMIO BAR access via stwbrx/lwbrx inline asm, little-endian
  vring fields; implemented for Pegasos2 (MV64361), not yet tested
- FileSysBox FUSE interface -- implements 25 FUSE callbacks; all DOS packet
  handling is done by filesysbox.library v54+
- Full filesystem operations -- directory listing, file read/write, create,
  delete, rename, mkdir, rmdir, truncate, chmod, chown, statfs, utimens,
  fsync, symlink, readlink, hard link
- Large file support -- reads and writes larger than msize are automatically
  split into multiple 9P transactions
- 512 KB message size -- negotiates 512 KB msize with QEMU for maximum
  throughput; a 1 MB file transfer needs only 2 round-trips instead of 16
- DMA-safe buffers -- uses MEMF_SHARED allocations with cached physical
  addresses and PPC dcbst/dcbf cache management for zero-copy DMA
- Polled completion -- fast used-ring polling for VirtIO completions; avoids
  signal-based Wait() conflicts with FileSysBox event loop
- EVENT_IDX support -- interrupt coalescing via used_event to reduce ISR
  overhead under load
- No newlib dependency -- uses _start() entry point with -nostartfiles;
  inline string/memory helpers in string_utils.h
- Automatic installer -- AmigaOS Shell script copies handler and DOSDriver


Requirements
------------

- AmigaOS 4.1 Final Edition (or later)
- filesysbox.library v54 or newer (included with OS 4.1 FE)
- QEMU 10.0+ with VirtIO 9P support (Linux, WSL2, macOS, or patched
  Windows -- see "Windows QEMU Setup" below)
- QEMU emulating a PPC AmigaOS machine (tested on AmigaOne only; Pegasos2
  and SAM460 are not yet tested)


Installation
------------

Quick Install (from AmigaOS Shell):

  Extract the distribution archive, cd into the extracted directory, and run:

    cd RAM:Virtio9PFS
    execute install.sh

  This copies the handler to L: and the DOSDriver to DEVS:DOSDrivers/.
  Reboot to activate the SHARED: volume.

Manual Install:

  1. Copy Virtio9PFS-handler to L:
  2. Copy SHARED to DEVS:DOSDrivers/SHARED
  3. Reboot to activate (or mount manually)


QEMU Configuration
------------------

Add the -virtfs option to your QEMU command line to share a host folder:

    qemu-system-ppc -M amigaone \
        -virtfs local,path=/path/to/shared/folder,mount_tag=SHARED,security_model=none,id=share0 \
        ...

The mount_tag must match the DOSDriver name (e.g. SHARED for
DEVS:DOSDrivers/SHARED). QEMU automatically creates the appropriate VirtIO
9P PCI device for the machine type.


Windows QEMU Setup
------------------

Official Windows QEMU builds do not include VirtIO 9P (-virtfs) support.
You can patch and rebuild QEMU yourself to enable it. Community-maintained
patches are available at:

  Pre-built Windows QEMU binaries with 9P patches:
    https://github.com/arixmkii/qcw/tags

  Patch files (to apply to upstream QEMU source):
    https://github.com/arixmkii/qcw/tree/main/patches/qemu

On Windows, the -virtfs shorthand may not work. Use the explicit -fsdev +
-device syntax instead:

    -fsdev local,security_model=mapped-xattr,id=fsdev0,path=D:\SHARED
    -device virtio-9p-pci-non-transitional,id=fs0,fsdev=fsdev0,mount_tag=SHARED

Notes:
  - virtio-9p-pci-non-transitional (modern VirtIO 1.0) is required for
    Pegasos2. For AmigaOne, use virtio-9p-pci (legacy) instead.
  - security_model=mapped-xattr stores Unix permission metadata in NTFS
    Alternate Data Streams, allowing protect (chmod) to work on the shared
    volume. security_model=none passes through host permissions directly.


DOSDriver Configuration
-----------------------

The included SHARED DOSDriver file contains:

    Handler   = L:Virtio9PFS-handler
    Stacksize = 65536
    Priority  = 5
    GlobVec   = -1
    Activate  = 1
    Control   = "auto"

- Activate = 1 -- auto-mount on boot
- Control = "auto" -- auto-detect the first VirtIO 9P PCI device
- The volume name matches the QEMU mount_tag parameter


Debug Output
------------

Compile with -DDEBUG to enable serial debug output (QEMU serial console or
Sashimi on real hardware). All debug messages are prefixed with [virtio9p].


Architecture
------------

This is a handler, not a device driver. There is no .device file, no
BeginIO/AbortIO, and no block layer. The 9P protocol is file-level, so
the handler IS the filesystem. FileSysBox handles all DOS packet
translation; we only implement FUSE callbacks.

Endianness:

  9P wire protocol       Always LE    Byte-by-byte extraction (natural on BE PPC)
  Vring fields (legacy)  Native BE    No swap (vr16/vr32/vr64 = no-op)
  Vring fields (modern)  LE           Swap via vr16/vr32/vr64
  VirtIO config (legacy) Host-native  PCI I/O functions handle it
  VirtIO config (modern) LE           stwbrx/lwbrx handle LE conversion


How This Project Was Created
----------------------------

This handler was developed with the assistance of Claude Code (Anthropic's
AI coding agent, model Claude Opus 4). The entire implementation -- VirtIO
transport, 9P protocol, FUSE callbacks, and build system -- was designed and
written collaboratively between a human developer and the AI agent over
multiple sessions.

The AI agent was provided with:
- The AmigaOS 4.1 SDK headers
- A VirtIO device driver skeleton project as reference
- The VirtIO specification and 9P2000.L protocol documentation
- FileSysBox FUSE header definitions and example code

The human developer provided architectural direction, reviewed the
implementation plan, and tested on QEMU-emulated AmigaOne.


Version History
===============


0.7.1-beta (16 Apr 2026)
-------------------------

  Bug fixes:
  - Fixed crash when flushing files without an open handle -- filesysbox
    can call the fsync callback with a NULL file-info pointer during a
    general flush (e.g. after CMD_UPDATE on an unopened path); the handler
    was dereferencing it unconditionally to read the 9P fid. The resulting
    page fault on address 0x14 killed the SHARED handler process. The
    fsync callback now returns success when no open file handle is
    attached.


0.7.0-beta (27 Mar 2026)
-------------------------

  Bug fixes:
  - Fixed shutdown freeze on Restart System -- the ISR was removed after
    FbxCleanupFS replied to DOS; the OS could kill the process before
    RemIntServer ran, leaving a dangling ISR chain pointer. The ISR is
    now detached before FbxCleanupFS.
    (reported on AmigaOne and Pegasos II)


0.6.0-beta (26 Mar 2026)
-------------------------

  New features:
  - File sync -- flushing files now properly syncs data to the host disk
  - Symbolic links -- create and follow symlinks on the shared volume
  - Hard links -- creating hard links is now supported
  - Append and truncate on open -- these flags now work correctly
  - Request cancellation -- stalled requests are automatically cancelled

  Bug fixes:
  - Fixed crash on restart/shutdown (reported on AmigaOne and Pegasos II)
  - Fixed potential crash from malformed host data
  - Fixed potential crash from invalid device responses
  - Fixed deep directory paths being silently truncated
  - Fixed incorrect free space reporting on large volumes
  - Fixed modern VirtIO init failure on unresponsive devices
  - Fixed startup failure with fragmented memory

  Test suite:
  - 25 integration tests covering all supported filesystem operations


0.4.0-beta (07 Mar 2026)
-------------------------

  - Permission support -- Protect command now works on shared volumes
  - Ownership support -- changing file owner and group is now supported
  - Truncate open files -- ChangeFileSize now works on open files
  - Windows QEMU setup guide added
  - 12 integration tests passing


0.3.0-beta (02 Mar 2026)
-------------------------

  - 8x faster file transfers -- message size increased to 512 KB
  - Faster I/O -- DMA addresses cached at startup
  - Faster directory listings -- zero-copy directory reads


0.2.0-beta (02 Mar 2026)
-------------------------

  - First working release -- SHARED: volume appears in Workbench
  - Tested on QEMU AmigaOne (legacy VirtIO)
  - Includes AmigaOS Shell installer script


0.1.0 (02 Mar 2026)
--------------------

  - Initial implementation with dual VirtIO transport and full 9P2000.L
    filesystem protocol


License
-------

Copyright 2026. All rights reserved.
