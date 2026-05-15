# Virtio9PFS-handler Roadmap

Last updated: 15 May 2026

This is a forward-looking roadmap. For the per-release history of what
has actually shipped, see [CHANGELOG.md](../CHANGELOG.md).

---

## Shipped milestones

- **v0.4.0** (Mar 2026) — Permission/ownership support; 12 integration tests
- **v0.5.0 / v0.6.0-beta** (Mar 2026) — Symlinks, hard links, fsync,
  Tflush request cancellation, append/truncate-on-open
- **v0.7.0-beta** (Mar 2026) — Restart-system shutdown crash fix
- **v0.7.1-beta** (Apr 2026) — Null-fi fsync crash fix
- **v0.8.0-beta** (Apr 2026) — Modern VirtIO PCI handshake for transitional
  devices (fixes Pegasos2 mounting); 29-check stress regression suite
- **v0.9.0-beta** (May 2026) — Full robustness pass: tag matching, FID
  orphan tracking, V9P_Reset, PPC time-base wallclock timeouts, lwsync
  barriers, bounds-checked marshal; 31-case robustness suite covering
  ~96% of v9p_* FUSE callbacks

---

## v1.0.0 — Stable Release (planned)

The criteria for declaring a 1.0:

### 1. Coverage to 100%

- Wire `test_9p` (in-guest binary) into the robustness harness so the
  one remaining FUSE callback (`ftruncate` on an open dos.library file
  handle) gets active coverage.
- Drop the Windows-host symlink SKIP by adding a Linux-host CI run.

### 2. Multi-volume support

- Match DOSDriver `Control` field to a specific VirtIO 9P device's
  `mount_tag` so multiple `-virtfs` shares can mount as separate volumes
  (e.g. `SHARED:`, `CODE:`, `DATA:`). Today the handler discovers the
  first device only.
- Provide example DOSDrivers for common multi-volume setups.

### 3. File locking

- Implement 9P2000.L `Tlock` / `Tgetlock`.
- Hook the FBX FUSE `flock` callback if present.
- Required for safe concurrent access from multiple AmigaOS processes.

### 4. Long-haul soak validation

- 24-hour Tier-16 soak (`--soak`) on each of AmigaOne, Pegasos2, and
  SAM460ex with a randomised workload.
- Zero ghost-FIDs, zero transport resets, zero handler crashes.

### 5. Distribution

- OS4Depot / Aminet release with `.readme`.
- Forum announcement.
- `config_generation` retry pattern for modern VirtIO config reads.

---

## Possible future work

These are speculative — not committed for any specific release.

- **Caching** — write-back (batch small writes, flush on release/fsync),
  read-ahead during sequential reads, and dirent caching for Workbench
  icon scans. Would require a `Control` field knob (`writeback=1`) since
  it changes durability semantics.
- **Wider QEMU machine support** — Sam440ep, X1000 dummy machine, etc.
  if/when they gain VirtIO 9P transports.
- **xattr passthrough** — expose `security_model=mapped-xattr` xattrs as
  AmigaOS file comments / metadata.
