"""
Tier 12 — Walk validation.

Covers investigation item N2 (P1-4):
  P9_Walk currently does not validate nwqid == nwname in the Rwalk
  response, so partial walks are silently accepted as success.

12.1  test_partial_walk_returns_enoent
        Race the host removing an intermediate component with a guest
        walk.  SKIP by default — reliably reproducing the race needs
        controlled fault injection that doesn't exist pre-fix.  Once
        P1-4 lands the handler will simply reject partial Rwalks; the
        race can then be approximated with QMP pause + host rmdir +
        resume.
12.2  test_walk_deep_path
        A 6-level path walks successfully end-to-end (regression
        guard against an over-eager P1-4 implementation rejecting
        legitimate deep walks).
"""
from __future__ import annotations
import os
import time

from . import common as cm
from . import injection as inj


TIER = "Tier 12"


def _t12_1_partial_walk_enoent(ctx: cm.Ctx) -> None:
    """SKIP unless QMP injection is wired AND a debug-build marker
    signalling partial-walk detection is in place (post P1-4)."""
    if not inj.qmp_available(ctx.qmp_path):
        ctx.score.record(
            TIER, "12.1", "partial walk returns ENOENT",
            "SKIP", "QMP socket not available",
        )
        return

    log_text = inj.scrape_serial_log(ctx.serial_log)
    has_marker = ("[virtio9p] partial Rwalk" in log_text
                  or "[virtio9p] walk_partial" in log_text
                  or "P1-4" in log_text)
    if not has_marker:
        ctx.score.record(
            TIER, "12.1", "partial walk returns ENOENT",
            "SKIP", "P1-4 fix not detectable in serial log yet",
        )
        return

    # When P1-4 lands and the marker appears, the agent can flip this
    # into an active test by:
    #   1. host: create /tmp/p9share/_t12/a/b/c/leaf.txt
    #   2. guest: start `Type SHARED:_t12/a/b/c/leaf.txt`
    #   3. pause QEMU mid-walk; host removes _t12/a/b
    #   4. resume; expect ENOENT-flavoured error in CON: output
    #   5. confirm no orphan FID in fid_pool DPRINTF
    ctx.score.record(
        TIER, "12.1", "partial walk returns ENOENT",
        "SKIP", "active body to be enabled once P1-4 marker confirmed",
    )


def _t12_2_walk_deep_path(ctx: cm.Ctx) -> None:
    """6-component walk + simple file read to prove deep traversal
    still works."""
    root_rel = "_tier12_deep"
    deep_rel = f"{root_rel}/a/b/c/d/e/leaf.txt"
    cm.rm_host(root_rel)
    os.makedirs(cm.host_path(f"{root_rel}/a/b/c/d/e"), exist_ok=True)
    with open(cm.host_path(deep_rel), "wb") as f:
        f.write(b"deep-leaf-content\n")

    out = cm.run(ctx.c, f"C:Type {cm.guest_path(deep_rel)}", timeout=20)
    ok = "deep-leaf-content" in out
    cm.rm_host(root_rel)
    ctx.score.record(
        TIER, "12.2", "deep walk (6 components) succeeds",
        "PASS" if ok else "FAIL",
        out.strip().splitlines()[-1] if out else "no output",
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 12 — Walk validation")
    _t12_1_partial_walk_enoent(ctx)
    _t12_2_walk_deep_path(ctx)
