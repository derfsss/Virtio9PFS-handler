# Virtio9PFS — Robustness Test Suite (v0.9.0)

Implementation of the test plan in `.claude/plans/test_plan.md`.

This is a **strictly sequential** test runner. Tiers execute in order
0 → 16. Each test inside a tier records `PASS` / `FAIL` / `SKIP`. The
final summary prints per-tier counts and a single overall score. There
is **no parallelism**, **no flake-retry**, and **no early-exit on
failure** — every tier runs, so a single fix can be evaluated against
the full suite in one shot.

## Layout

```
tools/qemu-regression/
├── stress_suite.py            # existing — feature coverage (Tier 0..8)
└── robustness/                # this folder — v0.9.0 robustness tiers
    ├── __init__.py
    ├── README.md              # you are here
    ├── common.py              # shared paths, helpers, Score model
    ├── injection.py           # QMP pause/resume, host CPU/mem pressure
    ├── runner.py              # main entry point (sequential)
    ├── tier09_resync.py       # B1 / N14
    ├── tier10_tflush.py       # B2
    ├── tier11_dma.py          # N1
    ├── tier12_walk.py         # N2
    ├── tier13_reset.py        # L4 / N3 (mostly SKIP pre-P1-5)
    ├── tier14_timeout.py      # D2
    ├── tier15_bounds.py       # N12 / N13
    └── tier16_soak.py         # 24 h opt-in
```

## QEMU configuration

The runner needs two host-side endpoints into the VM:

1. **SerialShell TCP** (existing) — used by `stress_suite.py` too.
   Default `localhost:4321`. Launch QEMU with e.g.
   `-serial tcp::4321,server,wait`.

2. **QMP socket** (new for fault-injection tiers 9.2, 10.1, 14.x).
   Default `/tmp/test_peg2/qmp.sock`. Launch QEMU with
   `-qmp unix:/tmp/test_peg2/qmp.sock,server,nowait`.

If QMP is missing the fault-injection tests are reported `SKIP`, not
`FAIL` — the runner still produces a usable score.

The `/tmp/p9share` host directory must be bound by `-virtfs` to mount
tag `SHARED`, exactly as for `stress_suite.py`.

## Running

From the repo root, with the WSL Ubuntu shell in this folder:

```sh
# Full suite (Tier 0..15, ~10-25 min on Pegasos2 QEMU)
python3 -m tools.qemu_regression.robustness.runner

# New tiers only (faster iteration during fix dev)
python3 -m tools.qemu_regression.robustness.runner --skip-base

# Specific tiers (e.g. while iterating on P0-1 fix)
python3 -m tools.qemu_regression.robustness.runner --tiers 9,10

# Add the 24-h soak (Tier 16)
python3 -m tools.qemu_regression.robustness.runner --soak

# Custom ports / paths
python3 -m tools.qemu_regression.robustness.runner \
    --port 4321 \
    --qmp /tmp/test_peg2/qmp.sock \
    --serial-log /tmp/test_peg2/serial_full_2.log
```

Exit codes: `0` all pass · `1` one or more fails · `2` setup failure
(SerialShell connect, SHARED: not mounted).

## How the score is computed

Each test ends in exactly one of `PASS`, `FAIL`, or `SKIP`.

- **Pass rate** = `pass / (pass + fail)`. Skipped tests are **not**
  counted in the denominator (they represent unavailable injection,
  not failed code).
- Final line: `TOTAL: P/R passed (X%)  + S skipped  [T cases]` where
  `R = P + F` is the "running" count and `T` includes skips.
- A standalone block lists every failed case with its test ID,
  human-readable name, and `detail` string.

## Skip vs. Fail policy

The plan deliberately ships several tests as `SKIP` today because they
need either (a) fault-injection knobs the handler does not yet have,
or (b) debug-build log markers that arrive with the corresponding fix
commit.

The pattern is:

```python
log = inj.scrape_serial_log(ctx.serial_log)
if "[virtio9p] V9P_Reset" not in log:
    ctx.score.record(TIER, "13.1", "...", "SKIP",
                     "P1-5 marker absent")
    return
# active body here once the marker shows up
```

When the agent lands the corresponding fix it **must**:

1. Add the expected marker to the handler (DPRINTF or similar).
2. Author the active body of the SKIPped test in the same commit.
3. Confirm the runner now reports `PASS` on that test.

The list of pending SKIPs to flip on later, indexed by which fix
unlocks them:

| Fix    | Tier · test                                 |
|--------|---------------------------------------------|
| P0-2   | 10.2 — flush-buf marker                     |
| P1-4   | 12.1 — partial-walk active body             |
| P1-5   | 13.1, 13.2 — reset round-trip + fid invalidate |
| P1-6   | 13.3, 13.4 — orphan + 12-h soak             |
| P2-7   | 14.1 — wallclock timeout marker             |
| P3-15  | 15.2 — host-native marshal unit test        |

## Adding a new test

```python
def _t9_4_my_new_test(ctx: cm.Ctx) -> None:
    # ... do the thing ...
    ctx.score.record(
        "Tier 9", "9.4", "human-readable name",
        "PASS" if condition else "FAIL",
        "free-form detail string",
    )

def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 9 — Transport resync")
    _t9_1_tag_match_basic(ctx)
    # ...
    _t9_4_my_new_test(ctx)
```

The runner picks the tier module up automatically — no registration step.

## Iteration loop (matching the robustness plan)

After each fix in `.claude/plans/robustness_plan.md`:

```sh
# Build (WSL → docker)
docker run --rm -v $(pwd):/src -w /src \
    walkero/amigagccondocker:os4-gcc11 make clean debug

# Boot Pegasos2 with the new debug build, mount SHARED:
# Then:
python3 -m tools.qemu_regression.robustness.runner --skip-base \
    --tiers 9,10            # tiers relevant to the fix you just made

# Tier-gate run (all three machines, full suite):
python3 -m tools.qemu_regression.robustness.runner
```
