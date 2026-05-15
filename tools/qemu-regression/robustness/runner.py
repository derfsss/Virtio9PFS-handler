#!/usr/bin/env python3
"""
robustness/runner.py -- sequential runner for the Virtio9PFS test suite.

Runs Tiers 0..14 in declaration order against a live QEMU AmigaOS guest
that has SHARED: mounted via virtio-9p.  Tier 0 is the sanity gate -- if
it fails, the runner aborts with exit code 2.

Sub-tier execution order is strictly sequential (no threads, no async)
so a failure in tier N is visible before tier N+1 starts mutating state.
The final summary prints per-tier P/F/S counts and a single overall
score line; exit code is 0 iff every running test passed (skips do not
fail the run).

Usage
-----
    # Default: assumes QEMU launched with `-serial tcp::4321,server,wait`
    # and SHARED: mounted at /tmp/p9share on the host.
    python -m tools.qemu_regression.robustness.runner

    # Common options:
    python -m tools.qemu_regression.robustness.runner \\
        --port 4321 \\
        --qmp tcp:127.0.0.1:14322 \\
        --serial-log .runs/serial.log \\
        --tiers 0,1,3        # subset
        --soak               # opt-in 24 h tier 14 soak

Exit codes
----------
  0  all running tests passed
  1  one or more tests failed
  2  setup error (Tier 0 sanity, can't connect)
"""
from __future__ import annotations
import argparse
import importlib
import os
import sys
import time

# Make `from . import common` work when invoked as script
if __package__ in (None, ""):
    sys.path.insert(0,
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from robustness import common as cm     # type: ignore[no-redef]
else:
    from . import common as cm


# Declaration order == execution order.  Tier 0 is the sanity gate;
# Tiers 1..5 are the fast smoke / feature / regression coverage absorbed
# from the legacy stress_suite.py; Tiers 6..14 are the v0.9.0 robustness
# pass arranged smoke-to-soak.
TIERS = [
    ("0",  "tier00_sanity",       True),   # is_gate=True -> abort on fail
    ("1",  "tier01_transport",    False),
    ("2",  "tier02_file_io",      False),
    ("3",  "tier03_test9p",       False),
    ("4",  "tier04_features",     False),
    ("5",  "tier05_regressions",  False),
    ("6",  "tier06_walk",         False),
    ("7",  "tier07_bounds",       False),
    ("8",  "tier08_concurrency",  False),
    ("9",  "tier09_resync",       False),
    ("10", "tier10_tflush",       False),
    ("11", "tier11_dma",          False),
    ("12", "tier12_reset",        False),
    ("13", "tier13_timeout",      False),
    ("14", "tier14_soak",         False),
]


def _import_tier(mod_name: str):
    if __package__ in (None, ""):
        return importlib.import_module(f"robustness.{mod_name}")
    return importlib.import_module(f".{mod_name}", package=__package__)


def run_tiers(ctx: cm.Ctx, selected: set[str] | None) -> int:
    """Run all (or selected) tiers; return 0 on success, 2 if Tier 0
    sanity gate failed when scheduled."""
    for tier_id, mod_name, is_gate in TIERS:
        if selected is not None and tier_id not in selected:
            continue
        mod = _import_tier(mod_name)
        rc = mod.run(ctx)
        if is_gate and rc is False:
            print("\n  [ABORT] Tier 0 sanity failed -- SHARED: not mounted",
                  flush=True)
            return 2
    return 0


# ----- summary -----------------------------------------------------------
def print_summary(score: cm.Score) -> None:
    cm.header("FINAL SCORE")

    per_tier = score.per_tier()
    ordering = [f"Tier {tid}" for tid, _, _ in TIERS]
    seen: set[str] = set()
    for label in ordering:
        if label not in per_tier:
            continue
        seen.add(label)
        p, f, s = per_tier[label]
        print(f"  {label:<10} : {p} pass, {f} fail, {s} skip")
    for label, (p, f, s) in per_tier.items():
        if label not in seen:
            print(f"  {label:<10} : {p} pass, {f} fail, {s} skip")

    total   = score.total()
    passed  = score.passed()
    failed  = score.failed()
    skipped = score.skipped()
    running = passed + failed

    pct = (100.0 * passed / running) if running else 0.0
    print()
    print(f"  TOTAL: {passed}/{running} passed ({pct:.1f}%)  "
          f"+ {skipped} skipped  [{total} cases]")
    if failed:
        print(f"  FAILED: {failed}")
        for r in score.results:
            if r.status == "FAIL":
                print(f"    - {r.test_id} {r.name}  ({r.detail})")


# ----- main --------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=cm.DEFAULT_PORT,
                        help="SerialShell TCP port (default %(default)s)")
    parser.add_argument("--qmp", default=cm.DEFAULT_QMP,
                        help="QEMU QMP endpoint, tcp:host:port or unix:/path "
                             "(default %(default)s)")
    parser.add_argument("--serial-log", default=cm.DEFAULT_SERIAL,
                        help="Path to QEMU serial log file")
    parser.add_argument("--p9-share", default=cm.P9_SHARE_HOST,
                        help="Host directory backing the SHARED: volume "
                             "(default %(default)s).  Also reads "
                             "V9P_SHARE_HOST env var.")
    parser.add_argument("--soak", action="store_true",
                        help="Enable Tier 14 long-haul soak")
    parser.add_argument("--tiers", default="",
                        help="Comma list of tier IDs to run (e.g. 0,1,3). "
                             "Default: all tiers.  Tier 0 is always run "
                             "as the sanity gate when scheduled.")
    # Legacy aliases (kept so existing iterate.py invocations don't break):
    parser.add_argument("--skip-base", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument("--skip-robust", action="store_true",
                        help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    selected: set[str] | None = None
    if args.tiers:
        selected = {t.strip() for t in args.tiers.split(",") if t.strip()}
    elif args.skip_base or args.skip_robust:
        # Honour legacy flags: --skip-base => only 6..14 (robustness);
        # --skip-robust => only 0..5 (base smoke + features).
        base_ids   = {"0", "1", "2", "3", "4", "5"}
        robust_ids = {"6", "7", "8", "9", "10", "11", "12", "13", "14"}
        selected = robust_ids if args.skip_base else base_ids

    cm.P9_SHARE_HOST = args.p9_share

    c = cm.SerialClient("localhost", args.port)
    try:
        c.connect()
    except Exception as e:
        print(f"[FATAL] cannot connect to SerialShell on port {args.port}: {e}",
              file=sys.stderr)
        return 2

    score = cm.Score()
    ctx = cm.Ctx(c=c, qmp_path=args.qmp,
                 serial_log=args.serial_log, score=score,
                 run_soak=args.soak)

    start = time.time()
    rc_setup = 0
    try:
        rc_setup = run_tiers(ctx, selected)
    finally:
        try:
            c.close()
        except Exception:
            pass

    print_summary(score)
    elapsed = int(time.time() - start)
    print(f"\n  Wall time: {elapsed} s")
    if rc_setup == 2:
        return 2
    return 0 if score.failed() == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
