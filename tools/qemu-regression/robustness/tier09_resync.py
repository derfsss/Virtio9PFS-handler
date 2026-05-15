"""
Tier 9 — Transport resync.

Covers investigation items B1 / N14 (P0-1):
  Response-stream desync after a Tflush timeout.  A stale response from
  the device gets consumed by the next caller, surfacing as a fake
  "Error 1" / "-1" to userland.

9.1  test_tag_match_basic              — 1000 getattr ops, all succeed.
9.2  test_resync_after_simulated_timeout — pause QEMU 15s, resume,
                                            then 100 ops; require all OK.
9.3  test_bulk_delete_no_error1        — 500-file dir delete, no "Error 1"
                                          or "-1" in CON: output.
"""
from __future__ import annotations
import os
import time

from . import common as cm
from . import injection as inj


TIER = "Tier 9"


def _t9_1_tag_match_basic(ctx: cm.Ctx) -> None:
    """1000 mass getattrs.  Pre-fix the bug only triggers after a
    timeout; this test is a baseline-stability check: it must always
    pass on a healthy mount.  Score it as a smoke-test."""
    canary_rel = "_tier9_basic.txt"
    cm.rm_host(canary_rel)
    with open(cm.host_path(canary_rel), "wb") as f:
        f.write(b"tier9-canary\n")

    ok_count = 0
    fail_at = -1
    for i in range(1000):
        out = cm.run(ctx.c, f"C:List {cm.guest_path(canary_rel)}", timeout=10)
        if canary_rel in out:
            ok_count += 1
        else:
            fail_at = i
            break

    cm.rm_host(canary_rel)
    detail = f"{ok_count}/1000 ops OK"
    if fail_at >= 0:
        detail += f" (failed at iter {fail_at})"
    ctx.score.record(
        TIER, "9.1", "tag match basic — 1000-iter getattr loop",
        "PASS" if ok_count == 1000 else "FAIL",
        detail,
    )


def _t9_2_resync_after_simulated_timeout(ctx: cm.Ctx) -> None:
    """Pause QEMU long enough that the handler's poll loop times out
    (default ~500 ms today, will be ReadEClock-bounded post P2-7).
    Resume, then drive 100 ops.  Pre-P0-1 fix this is the test that
    reproduces 'Error 1' — post-fix it passes."""

    if not inj.qmp_available(ctx.qmp_path):
        ctx.score.record(
            TIER, "9.2", "resync after simulated timeout",
            "SKIP", "QMP socket not available — see robustness/README.md",
        )
        return

    canary_rel = "_tier9_resync.txt"
    cm.rm_host(canary_rel)
    with open(cm.host_path(canary_rel), "wb") as f:
        f.write(b"tier9-resync\n")

    # Liveness baseline
    if not cm.handler_alive(ctx.c):
        cm.rm_host(canary_rel)
        ctx.score.record(
            TIER, "9.2", "resync after simulated timeout",
            "FAIL", "handler not alive before injection",
        )
        return

    # Pause the VM for 15 s (well over both legacy iter-count and any
    # reasonable wallclock timeout).  Note: simply pausing is enough —
    # the handler is on the VM, so it freezes too.  What we want is to
    # let the device stay paused while the host-side QEMU-9P backend
    # keeps responding once we cont.  In practice QEMU's pause halts
    # the whole vCPU + device; the handler sees no work happen but
    # comes back alive on cont.
    try:
        qmp = inj.pause_resume(ctx.qmp_path)
        qmp.stop()
        time.sleep(15)
        qmp.cont()
        qmp.close()
    except inj.QMPError as e:
        cm.rm_host(canary_rel)
        ctx.score.record(
            TIER, "9.2", "resync after simulated timeout",
            "FAIL", f"QMP error: {e}",
        )
        return

    # Give the handler a beat to drain any flush activity
    time.sleep(0.5)

    # Probe — pre-fix this often returns -EIO once and then recovers
    # if we keep going, OR (worse) returns garbage for an op.
    if not cm.handler_alive(ctx.c, timeout=20):
        cm.rm_host(canary_rel)
        ctx.score.record(
            TIER, "9.2", "resync after simulated timeout",
            "FAIL", "handler unresponsive after VM resume",
        )
        return

    # 100 ops post-resume
    ok_count = 0
    fail_at = -1
    for i in range(100):
        out = cm.run(ctx.c, f"C:List {cm.guest_path(canary_rel)}", timeout=15)
        if canary_rel in out:
            ok_count += 1
        else:
            fail_at = i
            break

    cm.rm_host(canary_rel)
    detail = f"{ok_count}/100 post-resume ops OK"
    if fail_at >= 0:
        detail += f" (failed at iter {fail_at})"
    ctx.score.record(
        TIER, "9.2", "resync after simulated timeout",
        "PASS" if ok_count == 100 else "FAIL",
        detail,
    )


def _t9_3_bulk_delete_no_error1(ctx: cm.Ctx) -> None:
    """The reported DOpus4 bug, distilled: create a directory of 500
    small files and delete it in one shell command.  Watch for
    'Error 1' or '-1' in the output."""
    dir_rel = "_tier9_bulk"
    cm.rm_host(dir_rel)
    os.makedirs(cm.host_path(dir_rel), exist_ok=True)
    for i in range(500):
        with open(cm.host_path(f"{dir_rel}/f_{i:03d}.txt"), "wb") as f:
            f.write(f"bulk-delete iter {i}\n".encode())

    # Drive the delete from the guest in a single command and capture output
    out = cm.run(
        ctx.c, f"C:Delete {cm.guest_path(dir_rel)} ALL", timeout=120,
    )

    saw_error1 = ("Error 1" in out) or (" -1" in out) or ("error -1" in out.lower())
    gone = not os.path.exists(cm.host_path(dir_rel))

    detail = f"output {len(out)} chars; "
    detail += "tree gone, " if gone else "tree REMAINS, "
    detail += "Error 1 in output" if saw_error1 else "no Error 1 in output"

    ctx.score.record(
        TIER, "9.3", "bulk delete no Error 1 (reported bug repro)",
        "PASS" if (gone and not saw_error1) else "FAIL",
        detail,
    )

    # Cleanup any leftover
    if not gone:
        cm.rm_host(dir_rel)


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 9 — Transport resync")
    _t9_1_tag_match_basic(ctx)
    _t9_2_resync_after_simulated_timeout(ctx)
    _t9_3_bulk_delete_no_error1(ctx)
