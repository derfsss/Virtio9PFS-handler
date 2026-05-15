"""
Tier 0 -- Sanity gate.

If either of these two fails, the rest of the suite cannot produce
meaningful results -- the runner aborts with exit code 2.

0.1  test_shared_mounted     -- C:Info SHARED: reports the volume mounted
0.2  test_host_canary_visible -- a host file shows up via guest C:List
"""
from __future__ import annotations
import os

from . import common as cm


TIER = "Tier 0"


def _t0_1_shared_mounted(ctx: cm.Ctx) -> bool:
    out = cm.run(ctx.c, "info SHARED:", timeout=15)
    mounted = "SHARED:" in out and ("9PFP" in out or "Mounted" in out)
    last = out.strip().splitlines()[-1] if out.strip() else ""
    ctx.score.record(
        TIER, "0.1", "SHARED: mounted (info reports it)",
        "PASS" if mounted else "FAIL", last,
    )
    return mounted


def _t0_2_host_canary(ctx: cm.Ctx) -> bool:
    rel = "_tier0_canary.txt"
    data = b"tier0-canary-v1\n"
    cm.rm_host(rel)
    with open(cm.host_path(rel), "wb") as f:
        f.write(data)
    ls = cm.run(ctx.c, f"C:List {cm.guest_path(rel)}", timeout=15)
    ok = rel in ls and str(len(data)) in ls.replace(",", "")
    last = ls.strip().splitlines()[-1] if ls.strip() else ""
    cm.rm_host(rel)
    ctx.score.record(
        TIER, "0.2", "host canary visible from guest",
        "PASS" if ok else "FAIL", last,
    )
    return ok


def run(ctx: cm.Ctx) -> bool:
    """Returns False if sanity failed (runner should abort)."""
    cm.header("Tier 0 -- Sanity gate")
    if not _t0_1_shared_mounted(ctx):
        return False
    return _t0_2_host_canary(ctx)
