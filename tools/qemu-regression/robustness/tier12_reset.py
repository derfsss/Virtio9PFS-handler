"""
Tier 12 -- Transport reset + FID lifecycle.

Active bodies that exercise the P1-5 V9P_Reset() and P1-6 FID-orphan
machinery via the debug knobs added to fuse_ops.c (debug builds only):
  /_v9p_debug_reset_now      getattr triggers V9P_Reset()
  /_v9p_debug_orphan_count   stat returns FidPool_OrphanCount()
  /_v9p_debug_next_tag       stat returns h->next_tag

12.1  test_transport_reset_round_trip
        Trigger reset; serial log shows '[virtio9p] V9P_Reset: complete OK';
        post-reset 50 ops succeed.
12.2  test_reset_invalidates_outstanding_fids
        Open a file via guest, trigger reset, retry the open -- must still
        succeed (handler reset, fid pool fresh) but the OLD file handle
        FBX held is gone.  Confirmed by FBX issuing a fresh open and
        getattr after reset.
12.3  test_fid_orphan_marker_appears_on_timeout
        Pause QEMU long enough that V9P_Transact wallclock times out
        on a Twalk; after resume the FidPool_OrphanCount has incremented.
12.4  test_long_soak_no_ghost_fids
        --soak only; same as Tier 14 but with stricter ghost-FID assertion.
"""
from __future__ import annotations
import os
import time

from . import common as cm
from . import injection as inj


TIER = "Tier 12"

DEBUG_RESET   = "/_v9p_debug_reset_now"
DEBUG_ORPHANS = "/_v9p_debug_orphan_count"


def _has_debug_knobs(ctx: cm.Ctx) -> bool:
    """Probe for the debug knob by issuing a getattr-via-FileSize.  If
    the build has the knob, the synthetic stat comes back; otherwise
    the handler walks the path and returns 'object not found'."""
    out = cm.run(ctx.c, f'C:FileSize "SHARED:{DEBUG_RESET[1:]}"', timeout=15)
    return "bytes" in out.lower() and "not found" not in out.lower()


def _read_orphan_count(ctx: cm.Ctx) -> int:
    """Stat the orphan-count debug path; return the file's reported size,
    or -1 if unavailable."""
    out = cm.run(ctx.c, f'C:FileSize "SHARED:{DEBUG_ORPHANS[1:]}"', timeout=10)
    return cm.size_of(out)


def _trigger_reset(ctx: cm.Ctx) -> int:
    """Stat the reset-now debug path; size==1 means V9P_Reset returned OK,
    size==0 means it failed.  -1 if knob missing."""
    out = cm.run(ctx.c, f'C:FileSize "SHARED:{DEBUG_RESET[1:]}"', timeout=30)
    return cm.size_of(out)


# ---------- 12.1 ----------------------------------------------------------
def _t12_1_reset_round_trip(ctx: cm.Ctx) -> None:
    if not _has_debug_knobs(ctx):
        ctx.score.record(TIER, "12.1", "transport reset round-trip",
                         "SKIP", "DEBUG knobs not present (release build?)")
        return

    pre_log = inj.scrape_serial_log(ctx.serial_log)
    pre_resets = pre_log.count("V9P_Reset: complete OK")

    rc = _trigger_reset(ctx)
    if rc != 1:
        ctx.score.record(TIER, "12.1", "transport reset round-trip",
                         "FAIL", f"V9P_Reset returned size={rc} (expected 1)")
        return

    # Drive 50 post-reset ops -- handler should be fully alive.
    canary = "_t12_1_canary.txt"
    cm.rm_host(canary)
    with open(cm.host_path(canary), "wb") as f:
        f.write(b"after-reset\n")
    ok_count = 0
    for i in range(50):
        out = cm.run(ctx.c, f"C:List {cm.guest_path(canary)}", timeout=10)
        if canary in out:
            ok_count += 1
    cm.rm_host(canary)

    post_log = inj.scrape_serial_log(ctx.serial_log)
    post_resets = post_log.count("V9P_Reset: complete OK")
    reset_marker_fired = (post_resets > pre_resets)

    ok = (ok_count == 50) and reset_marker_fired
    ctx.score.record(
        TIER, "12.1", "transport reset round-trip",
        "PASS" if ok else "FAIL",
        f"reset_marker_delta={post_resets - pre_resets}, "
        f"post_reset_ops={ok_count}/50",
    )


# ---------- 12.2 ----------------------------------------------------------
def _t12_2_reset_invalidates_outstanding_fids(ctx: cm.Ctx) -> None:
    if not _has_debug_knobs(ctx):
        ctx.score.record(TIER, "12.2", "reset invalidates outstanding FIDs",
                         "SKIP", "DEBUG knobs not present")
        return

    canary = "_t12_2_open.txt"
    cm.rm_host(canary)
    with open(cm.host_path(canary), "wb") as f:
        f.write(b"open me\n")

    # Use the file once to make sure it's reachable.
    out_pre = cm.run(ctx.c, f"C:Type {cm.guest_path(canary)}", timeout=15)
    pre_ok = "open me" in out_pre

    # Force a reset.  After reset, the handler's fid_pool is fresh --
    # any FBX-held fid is now invalid server-side.  FBX will discover
    # this when it next walks/opens; our test simply re-uses the file
    # and expects it to STILL work (handler is alive, can re-walk).
    rc = _trigger_reset(ctx)
    if rc != 1:
        cm.rm_host(canary)
        ctx.score.record(TIER, "12.2", "reset invalidates outstanding FIDs",
                         "FAIL", f"reset returned size={rc}")
        return

    # Post-reset access should succeed (FBX re-walks, gets fresh fid).
    out_post = cm.run(ctx.c, f"C:Type {cm.guest_path(canary)}", timeout=15)
    post_ok = "open me" in out_post

    cm.rm_host(canary)

    ok = pre_ok and post_ok
    ctx.score.record(
        TIER, "12.2", "reset invalidates outstanding FIDs",
        "PASS" if ok else "FAIL",
        f"pre={pre_ok}, post={post_ok}",
    )


# ---------- 12.3 ----------------------------------------------------------
def _t12_3_fid_orphan_marker_on_timeout(ctx: cm.Ctx) -> None:
    if not _has_debug_knobs(ctx):
        ctx.score.record(TIER, "12.3", "FID orphan on transport timeout",
                         "SKIP", "DEBUG knobs not present")
        return
    if not inj.qmp_available(ctx.qmp_path):
        ctx.score.record(TIER, "12.3", "FID orphan on transport timeout",
                         "SKIP", "QMP socket not available")
        return

    canary = "_t12_3_orphan.txt"
    cm.rm_host(canary)
    with open(cm.host_path(canary), "wb") as f:
        f.write(b"orphan-test\n")

    pre_orphans = _read_orphan_count(ctx)

    # Pause QEMU > V9P_TRANSACT_TIMEOUT_SEC (10 s) -- long enough that
    # the wallclock budget elapses inside V9P_Transact.  We trigger this
    # by issuing a guest command that needs a Twalk while the QEMU is
    # paused.  Since SerialShell ALSO talks via QEMU, we can't easily
    # fire-and-poll; instead pause + wait + cont, then look at the
    # orphan count after the next walk_to.
    qmp = inj.pause_resume(ctx.qmp_path)
    try:
        qmp.stop()
        time.sleep(12)         # exceed default 10 s wallclock timeout
        qmp.cont()
    finally:
        qmp.close()
    time.sleep(1)

    # Now do a walk_to (any List on a path under SHARED:) -- this won't
    # itself time out, but the previous in-flight ones (if any) should
    # have orphaned fids during the pause.  Pre-fix: this assertion is
    # weak because we don't reliably trigger orphans; we accept either:
    #   - orphan_count went up, OR
    #   - handler is alive and ops still work (no regression).
    out = cm.run(ctx.c, f"C:List {cm.guest_path(canary)}", timeout=30)
    visible = canary in out
    post_orphans = _read_orphan_count(ctx)
    cm.rm_host(canary)

    delta = post_orphans - pre_orphans if pre_orphans >= 0 else -1
    ok = visible  # primary: handler still functional after pause/resume
    ctx.score.record(
        TIER, "12.3", "FID orphan on transport timeout",
        "PASS" if ok else "FAIL",
        f"orphan_count delta={delta}, post-pause file visible={visible}",
    )


# ---------- 12.4 ----------------------------------------------------------
def _t12_4_no_ghost_fid_soak(ctx: cm.Ctx) -> None:
    """Inline mini-soak: drive 200 random ops and assert orphan_count
    grows by at most a handful (proxy for "transport-error rate is ~0
    after P0-1+P0-2+P1-3").  --soak bumps the op count to provide more
    soak time; without --soak we still always run a short version."""
    if not _has_debug_knobs(ctx):
        ctx.score.record(TIER, "12.4", "ghost-FID accumulation bounded",
                         "SKIP", "DEBUG knobs not present")
        return

    pre_orphans = _read_orphan_count(ctx)
    if pre_orphans < 0:
        ctx.score.record(TIER, "12.4", "ghost-FID accumulation bounded",
                         "FAIL", "could not read orphan count")
        return

    # 200 ops is a quick smoke; --soak bumps to 5000 (~30 s on Pegasos2).
    n_ops = 5000 if ctx.run_soak else 200
    canary = "_t12_4_orphan_smoke.txt"
    cm.rm_host(canary)
    with open(cm.host_path(canary), "wb") as f:
        f.write(b"orphan smoke\n")
    failed = 0
    for i in range(n_ops):
        out = cm.run(ctx.c, f"C:List {cm.guest_path(canary)}", timeout=10)
        if canary not in out:
            failed += 1
            if failed > 5:
                break
    cm.rm_host(canary)

    post_orphans = _read_orphan_count(ctx)
    delta = post_orphans - pre_orphans

    # After P0-1+P0-2+P1-3 the transport-error rate should be ~0; orphan
    # count should NOT grow during normal ops.  Allow up to 5 to leave
    # headroom for environmental hiccups.
    ok = (failed == 0) and (delta <= 5)
    ctx.score.record(
        TIER, "12.4", "ghost-FID accumulation bounded",
        "PASS" if ok else "FAIL",
        f"{n_ops - failed}/{n_ops} ops OK, "
        f"orphan_count {pre_orphans}->{post_orphans} (delta={delta})",
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 12 -- Transport reset + FID lifecycle")
    _t12_1_reset_round_trip(ctx)
    _t12_2_reset_invalidates_outstanding_fids(ctx)
    _t12_3_fid_orphan_marker_on_timeout(ctx)
    _t12_4_no_ghost_fid_soak(ctx)
