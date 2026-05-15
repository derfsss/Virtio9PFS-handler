# Virtio9PFS — Test Suite (v0.9.0)

A strictly sequential, in-QEMU regression suite for the Virtio9PFS
handler. Tiers execute in declaration order. Each test inside a tier
records `PASS` / `FAIL` / `SKIP`. The final summary prints per-tier
counts and a single overall score. There is **no parallelism**, **no
flake-retry**, and **no early-exit on failure** — every tier runs, so a
single fix can be evaluated against the full suite in one shot.

This suite is the unified replacement for the earlier
`stress_suite.py` (now removed): the surviving non-duplicate stress
tiers were absorbed as Tiers 0–5 alongside the v0.9.0
robustness tiers (6–14).

## Layout

```
tools/qemu-regression/robustness/
├── __init__.py
├── README.md              # you are here
├── common.py              # shared paths, helpers, Score model
├── injection.py           # QMP pause/resume, host CPU/mem pressure
├── iterate.py             # build-test driver (boots QEMU, uploads
│                          #   handler, hot-mounts, runs runner)
├── runner.py              # main entry point (sequential)
├── tier00_sanity.py       # SHARED: mounted + host canary visible
├── tier01_transport.py    # serial-log mode-decision marker
├── tier02_file_io.py      # SHA round-trips up to 1.5 MB
├── tier03_test9p.py       # native on-guest test_9p binary
├── tier04_features.py     # FUSE-callback feature coverage
├── tier05_regressions.py  # named historical regressions (v0.7.1 fsync)
├── tier06_walk.py         # Walk validation (deep + partial)
├── tier07_bounds.py       # boundary parsing (paths, marshal)
├── tier08_concurrency.py  # parallel Copy + host-write-mid-read
├── tier09_resync.py       # transport resync after timeout
├── tier10_tflush.py       # Tflush isolation under timeout stress
├── tier11_dma.py          # DMA stability under cache/alloc pressure
├── tier12_reset.py        # V9P_Reset + FID orphan lifecycle
├── tier13_timeout.py      # wall-clock transaction timeout
└── tier14_soak.py         # opt-in long-haul soak
```

Tier 0 is the sanity gate: if SHARED: isn't mounted or the host canary
isn't visible, the runner aborts with exit code 2 before doing any
mutating work.

## QEMU configuration

The runner needs two host-side endpoints into the VM:

1. **SerialShell TCP** (default `localhost:4321`). Launch QEMU with
   `-serial tcp::4321,server,wait` (or hostfwd from a serial-over-TCP
   forwarder).
2. **QMP** (default `tcp:127.0.0.1:14322`, used by fault-injection tiers
   9.2, 10.1, 12.3, 13.x). Launch QEMU with
   `-qmp tcp:127.0.0.1:14322,server,nowait` (or `-qmp unix:/path/sock,...`).

If QMP is missing, the fault-injection tests are reported `SKIP`, not
`FAIL` — the runner still produces a usable score.

The host directory bound by `-virtfs` must be exposed under mount tag
`SHARED`. The `iterate.py` driver manages the QEMU lifecycle
(pidfile-scoped) and uploads the freshly built handler binary directly
to `L:` over SerialShell, then hot-mounts `SHARED:` without rebooting.

## Running

From the repo root:

```sh
# Full suite via the iterate driver (boots QEMU + uploads + runs)
python tools/qemu-regression/robustness/iterate.py

# Specific tiers (faster iteration during fix dev)
python tools/qemu-regression/robustness/iterate.py --tiers 3

# Add the long-haul soak (Tier 14 with --soak == 24 h)
python tools/qemu-regression/robustness/iterate.py --soak

# Or, against an already-running QEMU, run the runner directly:
python -m tools.qemu_regression.robustness.runner \
    --port 4321 \
    --qmp tcp:127.0.0.1:14322 \
    --serial-log .runs/serial.log \
    --tiers 9,10
```

Exit codes: `0` all pass · `1` one or more fails · `2` setup failure
(SerialShell connect, SHARED: not mounted).

## How the score is computed

Each test ends in exactly one of `PASS`, `FAIL`, or `SKIP`.

- **Pass rate** = `pass / (pass + fail)`. Skipped tests are **not**
  counted in the denominator (they represent unavailable injection,
  not failed code).
- Final line: `TOTAL: P/R passed (X%)  + S skipped  [T cases]` where
  `R = P + F` is the running count and `T` includes skips.
- A standalone block lists every failed case with its test ID,
  human-readable name, and `detail` string.

## Skip vs. Fail policy

Tests `SKIP` (not `FAIL`) when the prerequisite genuinely isn't
available in the current environment, not when the code under test is
broken. Current SKIPs:

| Tier · test | Reason |
|-------------|--------|
| 3.1 — native `test_9p` | Skipped if `build/test_9p` doesn't exist (run `make test` to build it). When it runs, Tier 4.9 (open-handle ftruncate) flips from SKIP to PASS-by-association. |
| 4.10 — `v9p_readlink` (symlink follow) | On Windows hosts `os.symlink` requires Admin/Developer Mode and the QEMU 9P backend on NTFS doesn't expose junctions as POSIX symlinks. Runs on Linux hosts. |

If a fault-injection tier (9.2, 10.1, 12.3, 13.x) has no QMP endpoint
available, its impacted tests also SKIP cleanly.

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

To add a whole new tier, drop a `tierNN_topic.py` file alongside the
existing ones and append `("NN", "tierNN_topic")` to `NEW_TIERS` in
`runner.py`.

## Iteration loop

```sh
# Build (Docker via WSL on Windows, or directly on Linux)
docker run --rm -v $(pwd):/src -w /src \
    walkero/amigagccondocker:os4-gcc11 make clean debug

# Iterate.py handles boot + upload + hot-mount + run automatically:
python tools/qemu-regression/robustness/iterate.py --tiers 9,10

# Full-suite gate:
python tools/qemu-regression/robustness/iterate.py
```
