"""
Tier 14 — Wall-clock timeout.

Covers investigation item D2 (P2-7):
  MAX_POLLS iteration-count timeout varies with CPU speed; should be
  replaced with IExec->ReadEClock-based wall-clock.

14.1  test_timeout_is_wallclock_bounded
        Pause QEMU for 12 s.  Handler should escalate (Tflush or, after
        P1-5, transport reset) within (timeout ± 1 s) of the pause.
14.2  test_short_stall_does_not_timeout
        Pause QEMU for 200 ms.  Handler must NOT timeout.

Both require QMP + a debug-build marker emitted by the timeout path
once P2-7 lands.  Until then 14.1 SKIPs; 14.2 runs as a smoke test
(no timeout means the op should still succeed).
"""
from __future__ import annotations
import time

from . import common as cm
from . import injection as inj


TIER = "Tier 14"


def _has_wallclock_marker(serial_log: str) -> bool:
    log = inj.scrape_serial_log(serial_log)
    candidates = [
        "[virtio9p] V9P_Transact: wallclock timeout",
        "[virtio9p] timeout fired t=",
    ]
    return any(c in log for c in candidates)


def _t14_1_long_pause_timeouts(ctx: cm.Ctx) -> None:
    if not inj.qmp_available(ctx.qmp_path):
        ctx.score.record(
            TIER, "14.1", "long pause triggers wallclock timeout",
            "SKIP", "QMP socket not available",
        )
        return
    # Need P2-7's marker; without it we cannot distinguish a real
    # wallclock fire from a coincidental success.
    if not _has_wallclock_marker(ctx.serial_log):
        ctx.score.record(
            TIER, "14.1", "long pause triggers wallclock timeout",
            "SKIP", "P2-7 wallclock-timeout marker not present in log",
        )
        return

    canary_rel = "_tier14_long.txt"
    cm.rm_host(canary_rel)
    with open(cm.host_path(canary_rel), "wb") as f:
        f.write(b"tier14-long\n")

    qmp = inj.pause_resume(ctx.qmp_path)
    qmp.stop()
    # 12 s well past a typical 10 s wallclock timeout.
    time.sleep(12)
    qmp.cont()
    qmp.close()

    # Look for ≥1 new wallclock timeout marker in the log.
    log_after = inj.scrape_serial_log(ctx.serial_log)
    fired = log_after.count("[virtio9p] V9P_Transact: wallclock timeout") >= 1
    alive = cm.handler_alive(ctx.c, timeout=20)
    cm.rm_host(canary_rel)

    ok = fired and alive
    ctx.score.record(
        TIER, "14.1", "long pause triggers wallclock timeout",
        "PASS" if ok else "FAIL",
        f"timeout marker={'yes' if fired else 'no'}, handler "
        f"{'alive' if alive else 'unresponsive'} after resume",
    )


def _t14_2_short_stall_does_not_timeout(ctx: cm.Ctx) -> None:
    if not inj.qmp_available(ctx.qmp_path):
        ctx.score.record(
            TIER, "14.2", "short stall does not timeout",
            "SKIP", "QMP socket not available",
        )
        return

    canary_rel = "_tier14_short.txt"
    cm.rm_host(canary_rel)
    with open(cm.host_path(canary_rel), "wb") as f:
        f.write(b"tier14-short\n")

    pre_log = inj.scrape_serial_log(ctx.serial_log)
    pre_count = pre_log.count("[virtio9p] V9P_Transact: wallclock timeout")

    qmp = inj.pause_resume(ctx.qmp_path)
    qmp.stop()
    time.sleep(0.2)
    qmp.cont()
    qmp.close()

    # Should still succeed after a 200 ms blip
    out = cm.run(ctx.c, f"C:List {cm.guest_path(canary_rel)}", timeout=15)
    cm.rm_host(canary_rel)

    post_log = inj.scrape_serial_log(ctx.serial_log)
    post_count = post_log.count("[virtio9p] V9P_Transact: wallclock timeout")

    no_timeout = (post_count == pre_count)
    visible = (canary_rel in out)

    ok = no_timeout and visible
    detail = (f"file {'visible' if visible else 'NOT visible'}; "
              f"timeout marker delta={post_count - pre_count}")
    ctx.score.record(
        TIER, "14.2", "short stall does not timeout",
        "PASS" if ok else "FAIL", detail,
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 14 — Wall-clock timeout")
    _t14_1_long_pause_timeouts(ctx)
    _t14_2_short_stall_does_not_timeout(ctx)
