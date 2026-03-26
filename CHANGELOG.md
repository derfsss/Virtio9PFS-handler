# Changelog

All notable changes to Virtio9PFS-handler are documented here.

## 0.6.0-beta (26 Mar 2026)

### New features
- **File sync** — flushing files now properly syncs data to the host disk,
  ensuring changes are written through to the host filesystem
- **Symbolic links** — you can now create and follow symbolic links on the
  shared volume using `MakeLink` with `LINK_SOFT`
- **Hard links** — creating hard links is now supported via `MakeLink` with
  `LINK_HARD`
- **Append and truncate on open** — opening files with append or truncate
  mode now works correctly (previously these flags were ignored)
- **Request cancellation** — if a filesystem request stalls, the handler
  now automatically cancels it and recovers instead of leaking resources

### Bug fixes
- **Fixed crash on restart/shutdown** — the handler could crash the system
  during restart or shutdown due to the interrupt handler accessing freed
  memory; the interrupt is now properly quiesced before teardown
  (reported on AmigaOne and Pegasos II)
- **Fixed potential crash from malformed host data** — receiving an
  unexpectedly long string from the host could read past the buffer
- **Fixed potential crash from invalid device responses** — the handler
  now validates VirtIO descriptor indices to prevent out-of-bounds access
- **Fixed deep directory paths being silently truncated** — paths with
  more than 16 components now return a proper error instead of being cut off
- **Fixed incorrect free space reporting on large volumes** — disk usage
  statistics now correctly handle volumes larger than 4 GB
- **Fixed modern VirtIO init failure** — device initialization now fails
  cleanly if the device doesn't respond to a reset, instead of hanging
- **Fixed startup failure with fragmented memory** — the handler now
  detects and reports when it can't allocate a physically contiguous DMA
  buffer, instead of silently malfunctioning

### Test suite
- 25 integration tests (up from 12), covering all supported filesystem
  operations including symlinks, hard links, fsync, and edge cases

## 0.4.0-beta (07 Mar 2026)

- **Permission support** — `Protect` command now works on shared volumes,
  letting you set file permissions from Workbench or Shell
  (thanks to kas1e for reporting —
  [GitHub issue #1](https://github.com/derfsss/Virtio9PFS-handler/issues/1))
- **Ownership support** — changing file owner and group is now supported
- **Truncate open files** — `ChangeFileSize` now works on files that are
  already open
- **Windows QEMU setup guide** — added instructions for getting folder
  sharing working on Windows using community QEMU patches
  (thanks to kas1e for testing)
- 12 integration tests passing

## 0.3.0-beta (02 Mar 2026)

- **8x faster file transfers** — message size increased from 64 KB to 512 KB,
  dramatically reducing the number of round-trips for large files
- **Faster I/O** — DMA addresses are now cached at startup, eliminating
  overhead on every read/write operation
- **Faster directory listings** — directory entries are read directly from
  the receive buffer without unnecessary copying

## 0.2.0-beta (02 Mar 2026)

- **First working release** — SHARED: volume appears in Workbench and is
  fully browsable
- Tested on QEMU AmigaOne (legacy VirtIO)
- Includes AmigaOS Shell installer script

## 0.1.0 (02 Mar 2026)

- Initial implementation with dual VirtIO transport (legacy + modern),
  full 9P2000.L filesystem protocol, and 18 FUSE callbacks
