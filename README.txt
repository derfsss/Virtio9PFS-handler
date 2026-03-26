Virtio9PFS-handler
==================

A FileSysBox-based handler for AmigaOS 4.1 FE that mounts QEMU host-shared
folders as DOS volumes via the VirtIO 9P (9P2000.L) protocol.

Status: Beta (v0.5.0) -- tested on QEMU AmigaOne (legacy VirtIO) only.
Pegasos2 (modern VirtIO) is implemented but not yet validated. Use at your
own risk.

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


0.5.0-beta (26 Mar 2026)
-------------------------

  New 9P operations:
  - fsync -- FFlush on open file handles now syncs data to host disk via
    Tfsync (9P type 50); wired to FBX fsync callback
  - symlink / readlink -- MakeLink(LINK_SOFT) creates symbolic links on
    the host via Tsymlink (type 16); symlink targets are resolved via
    Treadlink (type 22)
  - hard link -- MakeLink(LINK_HARD) creates hard links via Tlink (type 70)
  - flush (request cancellation) -- Tflush (type 108) is now sent
    automatically when V9P_Transact times out, draining stalled descriptors
    from the VirtIO queue instead of leaking them

  Bug fixes:
  - Buffer over-read in p9_get_str -- malformed wire-length fields could
    read past rx_buf; now clamped to P9_MSIZE
  - Descriptor index validation -- VirtQueue_GetBuf now rejects
    device-written desc_id >= queue_size, preventing out-of-bounds access
  - Path depth overflow -- P9_Walk returns ENAMETOOLONG for paths deeper
    than 16 components instead of silently truncating
  - Open flags too restrictive -- O_APPEND and O_TRUNC are now passed
    through to Lopen/Lcreate (previously masked to access-mode only)
  - statvfs 64-bit truncation -- block/file counts use fsblkcnt_t/
    fsfilcnt_t instead of unsigned long
  - Modern VirtIO reset timeout -- init now fails cleanly if device status
    doesn't reach 0 within the retry limit
  - DMA contiguity check -- startup fails with clear error if StartDMA
    returns a physically fragmented buffer

  Comment and code quality:
  - Fixed misleading __builtin_bswap comment in p9_protocol.h that
    contradicted the byte-extraction implementation
  - Added doc comments to all fid_pool.c functions, walk_to(),
    p9stat_to_fbxstat(), and P9_Readdir lifetime semantics in header
  - Zero compiler warnings in release, DEBUG, and test builds -- DPRINTF
    macro now uses dead-code pattern to suppress unused-variable warnings

  Test suite:
  - 25 tests (up from 12): added unlink, rmdir, chown, utimens,
    ftruncate, fsync, symlink, hard link, readdir multi-entry, deep path,
    read-only open, delete non-existent, cross-directory rename
  - All path buffers widened to 512 bytes; volume name length validated

  Protocol coverage:
  - 21 of 24 useful 9P2000.L operations now implemented (was 16)
  - 25 FUSE callbacks wired (was 21)


0.4.0-beta (07 Mar 2026)
-------------------------

  - chmod support -- SetProtection (protect) now works on shared volumes;
    reads current mode via P9_Getattr to preserve file type bits, then
    merges permission bits via P9_Setattr(P9_SETATTR_MODE)
    (raised by kas1e -- GitHub issue #1:
    https://github.com/derfsss/Virtio9PFS-handler/issues/1)
  - chown support -- uid/gid changes via P9_Setattr
  - Windows QEMU documentation -- added setup instructions with links to
    community patches (arixmkii/qcw), working -fsdev command line syntax,
    and mapped-xattr notes (raised by kas1e)
  - ftruncate support -- ChangeFileSize on open file handles now works
  - 21 FUSE callbacks -- up from 18 (added chmod, chown, ftruncate)
  - Test suite expanded -- new chmod test (Test 12), 12/12 tests passing


0.3.0-beta (02 Mar 2026)
-------------------------

  - 512 KB message size -- increased P9_MSIZE from 64 KB to 512 KB; each
    P9_Read/Write transfers up to ~512 KB per round-trip (8x fewer
    transactions)
  - Cached DMA -- physical addresses resolved once at startup;
    per-transaction cache coherency via PPC dcbst (flush) and dcbf
    (invalidate) instead of 10 kernel calls per transaction
  - Zero-copy readdir -- P9_Readdir returns pointer into rx_buf,
    eliminating a memcpy per directory batch
  - Word-aligned memcpy/memset -- 4-byte bulk transfers in string_utils.h
  - Semaphore removed -- FBX is single-threaded; lock overhead eliminated
  - Version string in debug output -- startup banner shows handler name
    and version number


0.2.0-beta (02 Mar 2026)
-------------------------

  - First working release -- Workbench loads, SHARED: volume is browsable
  - Tested on QEMU AmigaOne (legacy VirtIO) only
  - Switched to _start() entry point with -nostartfiles -nodefaultlibs -lgcc
  - Added string_utils.h -- inline string/memory helpers replacing C stdlib
  - Fixed mount tag garbage and Wait() deadlock with FBX event loop
  - Manual library management: IExec from sysbase, explicit OpenLibrary
  - Added install.sh AmigaOS Shell installer script


0.1.1 (02 Mar 2026)
--------------------

  - Separated version strings into version.h
  - Added debug.h with auto-prefixed [virtio9p] DPRINTF macro
  - Added integration test suite (test/test_9p.c)
  - Added README.md and CLAUDE.md


0.1.0 (02 Mar 2026)
--------------------

  - Initial implementation
  - Dual-mode VirtIO transport (legacy I/O + modern MMIO)
  - Full 9P2000.L client
  - 18 FUSE callbacks via FileSysBox
  - DMA-safe transport with interrupt-driven completion
  - EVENT_IDX interrupt coalescing


License
-------

Copyright 2026. All rights reserved.
