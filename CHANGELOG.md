# Changelog

All notable changes to Virtio9PFS-handler are documented here.

## 0.5.0-beta (26 Mar 2026)

### New 9P operations
- **fsync** — `FFlush` on open file handles now syncs data to host disk via
  `Tfsync` (9P type 50); wired to FBX `fsync` callback
- **symlink / readlink** — `MakeLink(LINK_SOFT)` creates symbolic links on
  the host via `Tsymlink` (type 16); symlink targets are resolved via
  `Treadlink` (type 22)
- **hard link** — `MakeLink(LINK_HARD)` creates hard links via `Tlink`
  (type 70)
- **flush (request cancellation)** — `Tflush` (type 108) is now sent
  automatically when `V9P_Transact` times out, draining stalled descriptors
  from the VirtIO queue instead of leaking them

### Bug fixes
- **Buffer over-read in `p9_get_str`** — malformed wire-length fields could
  read past `rx_buf`; now clamped to `P9_MSIZE`
- **Descriptor index validation** — `VirtQueue_GetBuf` now rejects
  device-written `desc_id >= queue_size`, preventing out-of-bounds access
- **Path depth overflow** — `P9_Walk` returns `ENAMETOOLONG` for paths deeper
  than 16 components instead of silently truncating
- **Open flags too restrictive** — `O_APPEND` and `O_TRUNC` are now passed
  through to `Lopen`/`Lcreate` (previously masked to access-mode only)
- **`statvfs` 64-bit truncation** — block/file counts use `fsblkcnt_t`/
  `fsfilcnt_t` instead of `unsigned long`
- **Modern VirtIO reset timeout** — init now fails cleanly if device status
  doesn't reach 0 within the retry limit
- **DMA contiguity check** — startup fails with clear error if `StartDMA`
  returns a physically fragmented buffer

### Comment and code quality
- Fixed misleading `__builtin_bswap` comment in `p9_protocol.h` that
  contradicted the byte-extraction implementation
- Added doc comments to all `fid_pool.c` functions, `walk_to()`,
  `p9stat_to_fbxstat()`, and `P9_Readdir` lifetime semantics in header
- Zero compiler warnings in release, DEBUG, and test builds — `DPRINTF`
  macro now uses dead-code pattern to suppress unused-variable warnings

### Test suite
- **25 tests** (up from 12): added unlink, rmdir, chown, utimens,
  ftruncate, fsync, symlink, hard link, readdir multi-entry, deep path,
  read-only open, delete non-existent, cross-directory rename
- All path buffers widened to 512 bytes; volume name length validated

### Protocol coverage
- **21 of 24 useful 9P2000.L operations** now implemented (was 16)
- **25 FUSE callbacks** wired (was 21)

## 0.4.0-beta (07 Mar 2026)

- **chmod support** — `SetProtection` (protect) now works on shared volumes;
  reads current mode via `P9_Getattr` to preserve file type bits, then merges
  permission bits via `P9_Setattr(P9_SETATTR_MODE)` (raised by kas1e —
  [GitHub issue #1](https://github.com/derfsss/Virtio9PFS-handler/issues/1))
- **chown support** — `uid`/`gid` changes via `P9_Setattr(P9_SETATTR_UID|GID)`
- **Windows QEMU documentation** — added setup instructions with links to
  community patches ([arixmkii/qcw](https://github.com/arixmkii/qcw)),
  working `-fsdev` command line syntax, and `mapped-xattr` notes
  (raised by kas1e)
- **ftruncate support** — `ChangeFileSize` on open file handles now works
  (FBX maps `ACTION_SET_FILE_SIZE` to `ftruncate`, not `truncate`)
- **21 FUSE callbacks** — up from 18 (added `chmod`, `chown`, `ftruncate`)
- **Test suite expanded** — new `chmod` test (Test 12), 12/12 tests passing

## 0.3.0-beta (02 Mar 2026)

- **512 KB message size** — increased P9_MSIZE from 64 KB to 512 KB; each
  P9_Read/Write transfers up to ~512 KB per round-trip (8x fewer transactions)
- **Cached DMA** — physical addresses resolved once at startup; per-transaction
  cache coherency via PPC `dcbst` (flush) and `dcbf` (invalidate) instead of
  10 kernel calls per transaction (`StartDMA`/`GetDMAList`/`EndDMA`)
- **Zero-copy readdir** — `P9_Readdir` returns pointer into rx_buf, eliminating
  a memcpy per directory batch
- **Word-aligned memcpy/memset** — 4-byte bulk transfers in `string_utils.h`
- **Semaphore removed** — FBX is single-threaded; lock overhead eliminated
- **Version string in debug output** — startup banner shows handler name and
  version number

## 0.2.0-beta (02 Mar 2026)

- **First working release** — Workbench loads, SHARED: volume is browsable
- Tested on QEMU AmigaOne (legacy VirtIO) only
- Switched to `_start()` entry point with `-nostartfiles -nodefaultlibs -lgcc`
  to prevent newlib CRT from consuming the DOS handler startup message
- Added `string_utils.h` — inline string/memory helpers replacing C stdlib
  (memset, memcpy, memmove, strlen, strncpy, strcmp, strchr, strrchr)
- Fixed mount tag garbage: VirtIO config reports tag_len=32 (field size) but
  actual tag is shorter; now truncates at first null/non-printable byte
- Replaced Wait()-based VirtIO completion with polled used-ring check — fixes
  deadlock where FBX's event loop consumed the ISR signal bit
- Manual library management: IExec from sysbase, explicit OpenLibrary for
  expansion.library and filesysbox.library
- Added `install.sh` AmigaOS Shell installer script
- Added `-D__AMIGAOS4__ -U__USE_INLINE__` to CFLAGS (matching RapidFS pattern)

## 0.1.1 (02 Mar 2026)

- Separated version strings into `version.h`
- Added `debug.h` with auto-prefixed `[virtio9p]` DPRINTF macro
- Added integration test suite (`test/test_9p.c`)
- Added README.md and CLAUDE.md

## 0.1.0 (02 Mar 2026)

- Initial implementation
- Dual-mode VirtIO transport (legacy I/O + modern MMIO)
- Full 9P2000.L client (Version, Attach, Walk, Clunk, Lopen, Lcreate, Read,
  Write, Getattr, Setattr, Statfs, Readdir, Mkdir, Unlinkat, Renameat)
- 18 FUSE callbacks via FileSysBox
- DMA-safe transport with interrupt-driven completion
- EVENT_IDX interrupt coalescing
