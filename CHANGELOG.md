# Changelog

All notable changes to Virtio9PFS-handler are documented here.

## 0.9.0-beta (15 May 2026)

### Robustness (the headline)

A 15-item robustness plan was executed end-to-end against the v0.8.0
codebase, addressing transport correctness, FID lifecycle, DMA
stability, time/timeout discipline, boundary parsing, and PCI
discovery edge cases.  Highlights:

- **Tag matching in V9P_Transact** — every reply is now matched
  against the request tag before being accepted; the previous
  first-reply-wins logic could attribute another in-flight reply to
  the wrong call after a timeout (P0-1).
- **Dedicated Tflush buffer** — Tflush bodies no longer share a
  buffer with the transaction they're cancelling; pre-fix a
  late-arriving reply could corrupt the outgoing flush message
  (P0-2).
- **Held-open StartDMA** — the DMA mapping for vring buffers is now
  established once at handler start and held for the handler's
  lifetime, eliminating per-op StartDMA/EndDMA churn that occasionally
  remapped to a fresh phys address mid-transaction (P1-3).
- **V9P_Reset() transport reset** — clean teardown + re-init of the
  virtqueues is now possible without restarting the handler; used
  internally as a recovery hatch and exposed for test (P1-5).
- **FID-pool orphan tracking** — FIDs that go missing because of a
  transport timeout/reset are now marked orphaned and counted, so
  ghost-FID accumulation is observable rather than silent (P1-6).
- **PPC time-base wallclock timeout** — V9P_Transact now uses mftbu/
  mftb to enforce a 10-second wallclock budget on every transaction
  (was a busy-spin loop counter sensitive to CPU speed) (P2-7).
- **Legacy reset poll** — the legacy VirtIO reset path now waits for
  the device to acknowledge the reset by polling STATUS=0, matching
  the modern path (P2-8).
- **lwsync for cacheable RAM** — virtqueue producer/consumer barriers
  switched from eieio to lwsync, the correct PowerPC ordering
  primitive for cacheable memory (P2-9).
- **iobase mapping caveat WARN** — pci_discovery now warns when an
  iobase BAR maps suspiciously low, signalling the AmigaOne Articia
  S firmware bug (P2-10).
- **VRING_AVAIL_F_NO_INTERRUPT + EVENT_IDX dropped from feature
  mask** — the host no longer sends spurious notifications during
  active polling, and feature negotiation no longer claims
  EVENT_IDX support we don't actually implement (P3-11, P3-12).
- **Path-length pre-check in P9_Walk** — paths longer than the
  internal pathbuf return ENAMETOOLONG cleanly instead of being
  silently truncated and walked against a garbage prefix (P3-14).
- **Bounds-checked p9_get_str** — the marshal helper now takes a
  buffer length and refuses to read past it; covered by a host-
  native unit test (P3-15).

### Test suite

- **Robustness test harness** — new pure-Python harness at
  `tools/qemu-regression/robustness/` driving the QEMU AmigaOne
  machine via SerialShell + QMP fault injection.  Covers the 15
  robustness items above plus general feature coverage.
- **17 tiers, 31 cases, 29/29 PASS + 2 SKIP** in ~12.5 minutes wall
  time.  Coverage of v9p_* FUSE callbacks is ~96% (24/25); the one
  remaining SKIP needs an in-guest binary to call SetFileSize on an
  open dos.library handle.
- **Header dependency tracking** — Makefile now uses -MMD -MP so
  editing a header forces the right TUs to recompile, eliminating
  the stale-object-with-mismatched-struct-layout class of bug.

## 0.8.0-beta (17 Apr 2026)

### New features
- **Modern VirtIO PCI handshake for transitional devices** — ported the
  proven 3-step detection pattern from the VirtualSCSIDevice project:
  1. Zero BAR5 high DWORD if 0xFFFFFFFF (AmigaOne firmware bug workaround)
  2. Always call V9P_DetectModern() regardless of device ID (was gated to
     0x1049 only); walks PCI capability chain, then probes MMIO by writing
     STATUS=ACKNOWLEDGE and reading back; enables PCI_COMMAND_MEMORY +
     PCI_COMMAND_MASTER before probe
  3. If MMIO probe fails (Articia S bridge on AmigaOne), fall back to
     legacy I/O

  This fixed `SHARED:` not mounting on Pegasos2 — the handler was stuck
  in legacy mode where the MV64361 bridge requires MMIO.

### Test suite
- **New comprehensive regression suite** — `tools/qemu-regression/stress_suite.py`
  with 29 checks across 8 tiers: sanity, file I/O integrity (up to 1.5 MB
  SHA round-trips), directory ops, metadata, concurrency, regression guards,
  soak, transport confirmation, and native test_9p
- Validated on all 3 QEMU machines: AmigaOne (27/29), Pegasos2 (28/29),
  SAM460ex (27/29)
- 2 expected failures: tier 7.1 (release build has no DPRINTF) and
  tier 8.1 (chown + ftruncate unsupported under security_model=none)

## 0.7.1-beta (16 Apr 2026)

### Bug fixes
- **Fixed crash when flushing files without an open handle** — filesysbox
  can invoke the fsync callback with a NULL `fuse_file_info` during a
  general flush (e.g. after `CMD_UPDATE` on an unopened path), and the
  handler was dereferencing it unconditionally to read `fi->fh`. The
  resulting page fault on address 0x14 killed the `SHARED` handler
  process and any filesystem activity on the volume. The fsync callback
  now treats a NULL `fi` as a no-op and returns success.

## 0.7.0-beta (27 Mar 2026)

### Bug fixes
- **Fixed shutdown freeze on Restart System** — the interrupt handler was
  removed from the system chain *after* FbxCleanupFS replied to DOS; during
  Restart System the OS could kill the handler process before RemIntServer ran,
  leaving the stack-allocated ISR node dangling in the interrupt chain. The ISR
  is now detached before FbxCleanupFS.
  (reported on AmigaOne and Pegasos II)

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
  ([GitHub issue #1](https://github.com/derfsss/Virtio9PFS-handler/issues/1))
- **Ownership support** — changing file owner and group is now supported
- **Truncate open files** — `ChangeFileSize` now works on files that are
  already open
- **Windows QEMU setup guide** — added instructions for getting folder
  sharing working on Windows using community QEMU patches
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
