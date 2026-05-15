#!/usr/bin/env python3
"""
robustness/runner.py — sequential runner for the full v0.9.0 test suite.

Default mode: existing stress_suite.py tiers (0..8) followed by the new
robustness tiers (9..15).  Sub-tier execution order is strictly
sequential — no threads, no async — so a failure in tier N is visible
before tier N+1 starts mutating state.

Final summary prints per-tier P/F/S counts and a single overall score
line.  Exit code is 0 iff all *running* tests passed (skips do not
fail the run).

Usage
-----
    # Default: assumes QEMU launched with `-serial tcp::4321,server,wait`
    # (or compat) and SHARED: mounted at /tmp/p9share on the host.
    python3 -m tools.qemu_regression.robustness.runner

    # All options:
    python3 -m tools.qemu_regression.robustness.runner \
        --port 4321 \
        --qmp /tmp/test_peg2/qmp.sock \
        --serial-log /tmp/test_peg2/serial_full_2.log \
        --skip-base       # skip existing Tier 0..8 (faster iteration)
        --skip-robust     # skip new Tier 9..15
        --soak            # run Tier 16 long-haul soak
        --tiers 9,11,15   # only listed new tiers (overrides --skip-*)

Exit codes
----------
  0  all running tests passed
  1  one or more tests failed
  2  setup error (handler not mounted, can't connect)
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
    from robustness import injection as inj # type: ignore[no-redef]
else:
    from . import common as cm
    from . import injection as inj


NEW_TIERS = [
    ("9",  "tier09_resync"),
    ("10", "tier10_tflush"),
    ("11", "tier11_dma"),
    ("12", "tier12_walk"),
    ("13", "tier13_reset"),
    ("14", "tier14_timeout"),
    ("15", "tier15_bounds"),
    ("16", "tier16_soak"),
    ("17", "tier17_features"),
]


# ----- run_base_tiers: call into existing stress_suite.py -----------------
def run_base_tiers(c, serial_log: str, score: cm.Score) -> bool:
    """Run Tier 0..8 from the existing stress_suite.py, then transcribe
    its RESULTS into our Score object so the final summary covers them
    too.  Returns False if Tier 0 sanity failed (suite aborts)."""
    # Import the existing suite as a module
    suite_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sys.path.insert(0, suite_dir)
    import stress_suite  # type: ignore

    cm.header("Tier 0..8 — baseline feature suite (stress_suite.py)")

    # Reset its RESULTS so we don't double-count if rerun
    stress_suite.RESULTS = []

    if not stress_suite.tier0_sanity(c):
        for name, ok, detail in stress_suite.RESULTS:
            score.record("Tier 0", "0.?",
                         name, "PASS" if ok else "FAIL", detail)
        return False

    stress_suite.tier1_integrity(c)
    stress_suite.tier2_directory_ops(c)
    stress_suite.tier3_metadata(c)
    stress_suite.tier4_concurrency(c)
    stress_suite.tier5_regression_guards(c)
    stress_suite.tier6_soak(c, max_cycles=500, max_seconds=60)
    stress_suite.tier7_transport_confirm(serial_log)
    stress_suite.tier8_native_test_9p(c)

    # Transcribe — assign each result to a synthetic tier label
    for name, ok, detail in stress_suite.RESULTS:
        first = name.split(" ", 1)[0]
        tier = f"Tier {first.split('.')[0]}" if "." in first else "Tier ?"
        score.record(tier, first, name,
                     "PASS" if ok else "FAIL", detail)
    return True


# ----- run_new_tiers -----------------------------------------------------
def run_new_tiers(ctx: cm.Ctx,
                  selected: set[str] | None) -> None:
    for tier_id, mod_name in NEW_TIERS:
        if selected is not None and tier_id not in selected:
            continue
        mod = importlib.import_module(
            f"robustness.{mod_name}"
            if __package__ in (None, "") else
            f".{mod_name}", package=__package__)
        mod.run(ctx)


# ----- summary -----------------------------------------------------------
def print_summary(score: cm.Score) -> None:
    cm.header("FINAL SCORE")

    per_tier = score.per_tier()
    # Preserve declaration order: base tiers (0..8) then new (9..16)
    ordering = [f"Tier {n}" for n in
                ["0", "1", "2", "3", "4", "5", "6", "7", "8",
                 "9", "10", "11", "12", "13", "14", "15", "16"]]
    seen: set[str] = set()
    for label in ordering:
        if label not in per_tier:
            continue
        seen.add(label)
        p, f, s = per_tier[label]
        line = f"  {label:<10} : {p} pass, {f} fail, {s} skip"
        print(line)
    # Catch any tier label we did not anticipate
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
                        help="QEMU QMP UNIX socket path "
                             "(default %(default)s)")
    parser.add_argument("--serial-log", default=cm.DEFAULT_SERIAL,
                        help="Path to QEMU serial log file")
    parser.add_argument("--p9-share", default=cm.P9_SHARE_HOST,
                        help="Host directory backing the SHARED: volume "
                             "(default %(default)s).  Also reads "
                             "V9P_SHARE_HOST env var.")
    parser.add_argument("--skip-base", action="store_true",
                        help="Skip Tier 0..8 baseline suite")
    parser.add_argument("--skip-robust", action="store_true",
                        help="Skip Tier 9..15 robustness suite")
    parser.add_argument("--soak", action="store_true",
                        help="Enable Tier 16 long-haul soak")
    parser.add_argument("--tiers", default="",
                        help="Comma list of NEW tier IDs to run "
                             "(e.g. 9,11,15).  Overrides --skip-robust.")
    args = parser.parse_args(argv)

    selected: set[str] | None = None
    if args.tiers:
        selected = {t.strip() for t in args.tiers.split(",") if t.strip()}
        args.skip_robust = False  # explicit --tiers overrides

    # Honor --p9-share before any tier helper resolves a path.
    cm.P9_SHARE_HOST = args.p9_share

    # Connect SerialShell
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
    try:
        if not args.skip_base:
            if not run_base_tiers(c, args.serial_log, score):
                print("\n  [ABORT] baseline sanity failed — SHARED: not mounted",
                      flush=True)
                print_summary(score)
                return 2

        if not args.skip_robust:
            run_new_tiers(ctx, selected)
    finally:
        try:
            c.close()
        except Exception:
            pass

    print_summary(score)
    elapsed = int(time.time() - start)
    print(f"\n  Wall time: {elapsed} s")
    return 0 if score.failed() == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
