# Virtio9PFS-handler Roadmap

Created: 07 Mar 2026
Based on: forum feedback (amigans.net topic 10056), GitHub issue #1

## v0.4.0 — Permission Support + Windows Docs

### Context

Forum user kas1e got `-virtfs` working on patched Windows QEMU (10.2.1),
discovered `mapped-xattr` stores Unix permissions in NTFS Alternate Data
Streams, and filed GitHub issue #1: `protect` command fails with "packet
request type unknown" because the handler has no `chmod` callback.

### Tasks

#### 1. Update README with Windows QEMU setup instructions
**Status: DONE**

- Replaced hard "not available on Windows" warning with link to new section
- Updated Requirements to mention patched Windows
- Added "Windows QEMU Setup" section with:
  - Pre-built binaries: https://github.com/arixmkii/qcw/tags
  - Patch files: https://github.com/arixmkii/qcw/tree/main/patches/qemu
  - Working `-fsdev` + `-device` command line syntax
  - Notes on `non-transitional` vs legacy, `mapped-xattr` vs `none`
- Both README.md and README.txt updated

#### 2. Add `chmod` FUSE callback (protection bit support)
**Status: DONE**

- Add `v9p_chmod(const char *path, mode_t mode)` to `src/fuse_ops.c`
- Per GitHub issue #1: read current mode via `P9_Getattr` first to preserve
  file type bits (S_IFREG/S_IFDIR in upper bits), then merge caller's
  permission bits (lower 12 bits), then `P9_Setattr` with `P9_SETATTR_MODE`
- Register in `V9P_FillOperations()` as `ops->chmod`
- All infrastructure exists (`P9_Setattr`, `P9_SETATTR_MODE`, `P9_Getattr`)
  — just needs a ~25 line FUSE wrapper

#### 3. Add `chown` FUSE callback
**Status: DONE**

- Add `v9p_chown(const char *path, uid_t uid, gid_t gid)` to `src/fuse_ops.c`
- Uses `P9_SETATTR_UID | P9_SETATTR_GID` via existing `P9_Setattr`
- Register as `ops->chown`

#### 4. Update test suite
**Status: DONE**

- Add chmod/protect test to `test/test_9p.c`: create file, set protection
  bits, read back, verify
- Test with `security_model=mapped-xattr` (expect success) and
  `security_model=none` (expect EPERM or no-op)

#### 5. Version bump + release
**Status: DONE**

- Bump `include/version.h` to v0.4.0
- Update version history in both READMEs
- Build, deploy, test — 12/12 tests passed
- Added `ftruncate` FUSE callback (FBX uses ftruncate for open-file truncation)
- Added `make dist` target producing `Virtio9PFS_0.4.0-beta.lha`
- Close GitHub issue #1

---

## v0.5.0 — Pegasos2 Validation + Error Recovery

### 1. Pegasos2 hardware validation (modern VirtIO)
- Test modern VirtIO 1.0 transport on QEMU Pegasos2 (`-M pegasos2`)
- Validate `stwbrx`/`lwbrx` MMIO path end-to-end
- Confirm `virtio-9p-pci-non-transitional` device works

### 2. SAM460 testing
- Test on QEMU SAM460 if available
- Document any platform-specific quirks

### 3. Error recovery / VirtIO reset
- Add timeout detection to `V9P_Transact()` polling loop
- On timeout: VirtIO reset + re-init sequence
- Graceful error propagation to FBX (return -EIO, not hang)

### 4. Robust mount failure handling
- Better diagnostics when no VirtIO 9P device found
- Log PCI scan results even on failure
- Validate `filesysbox.library` version at startup

---

## v0.6.0 — Multiple Volumes + Mount Tag Matching

### 1. Multiple mount tag support
- Match DOSDriver `Control` field to specific VirtIO 9P device's `mount_tag`
- Allow multiple `-virtfs` shares mounted as separate volumes (e.g. `SHARED:`,
  `CODE:`, `DATA:`)
- Currently discovers first VirtIO 9P device; needs per-device tag comparison

### 2. DOSDriver template improvements
- Example DOSDrivers for common multi-volume setups
- Document `Control` field syntax for tag matching

---

## v0.7.0 — Symlinks + Advanced Filesystem

### 1. Symlink support
- 9P2000.L supports `Tsymlink` / `Treadlink`
- Investigate whether FBX FUSE exposes `readlink` / `symlink` callbacks
- If FBX supports it: implement `v9p_symlink` + `v9p_readlink`
- If not: document limitation

### 2. Hard link support
- 9P2000.L `Tlink` message
- FBX FUSE `link` callback (if available)

### 3. File locking
- 9P2000.L `Tlock` / `Tgetlock` messages
- FBX FUSE `flock` callback (if available)
- Needed for concurrent access from multiple AmigaOS processes

---

## v0.8.0 — Performance

### 1. Write-back caching
- Currently all writes are synchronous to host
- Batch small writes in a local buffer, flush on `release` / `fsync`
- Configurable via DOSDriver `Control` field (e.g. `writeback=1`)

### 2. Read-ahead caching
- Pre-fetch next chunk during sequential reads
- Track access pattern per open fid

### 3. Directory entry caching
- Cache `readdir` results for recently-visited directories
- Invalidate on `create` / `unlink` / `rename`
- Reduces round-trips for Workbench browsing (icon scanning)

---

## v1.0.0 — Stable Release

### 1. Full platform validation
- Tested on AmigaOne (legacy), Pegasos2 (modern), SAM460
- All FUSE callbacks exercised by test suite

### 2. Production hardening
- `config_generation` retry pattern for modern VirtIO config reads
- Comprehensive error handling for all 9P error codes
- Stress testing with large directory trees and concurrent file access

### 3. Documentation
- User guide with screenshots
- Aminet / OS4Depot release package with `.readme`
- Forum announcement post
