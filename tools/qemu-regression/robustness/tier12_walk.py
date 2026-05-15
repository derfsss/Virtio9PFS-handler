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
    """Walk a path whose intermediate components exist but tail is
    missing.  QEMU's 9P backend returns Rwalk with nwqid < nwname
    (partial walk).  P1-4's nwqid==nwname check should turn that
    into -ENOENT and emit a 'P9_Walk: partial walk' marker.

    No QMP needed -- this is the natural failure mode of any walk to
    a missing leaf, exercised millions of times a day by DOpus etc."""
    root_rel = "_tier12_partial"
    cm.rm_host(root_rel)
    os.makedirs(cm.host_path(f"{root_rel}/a/b"), exist_ok=True)
    # NOTE: /a/b/missing intentionally NOT created.

    pre_log = inj.scrape_serial_log(ctx.serial_log)
    pre_marker = pre_log.count("P9_Walk: partial walk")

    # Walk /tier12_partial/a/b/missing -- /a + /a/b succeed, /missing fails.
    out = cm.run(
        ctx.c,
        f"C:List {cm.guest_path(root_rel)}/a/b/missing",
        timeout=15,
    )

    saw_not_found = ("not found" in out.lower()
                     or "no such" in out.lower()
                     or "doesn't exist" in out.lower())
    alive = cm.handler_alive(ctx.c)

    post_log = inj.scrape_serial_log(ctx.serial_log)
    post_marker = post_log.count("P9_Walk: partial walk")
    marker_fired = (post_marker > pre_marker)

    cm.rm_host(root_rel)

    # PASS if the handler returned a not-found error and stayed alive.
    # The marker delta is informational -- a server that returns full
    # Rlerror (instead of partial Rwalk) for missing-tail still makes
    # the test pass (and the marker delta is then 0).
    ok = saw_not_found and alive
    ctx.score.record(
        TIER, "12.1", "partial walk returns ENOENT",
        "PASS" if ok else "FAIL",
        f"not_found={saw_not_found}, alive={alive}, "
        f"partial_marker_delta={post_marker - pre_marker}",
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
