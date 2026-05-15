"""
Tier 16 — Long-haul soak.

Default behaviour: SKIP (24 hours is too long for an interactive run).
Enable with `--soak` on the runner CLI.  When enabled, runs a random
workload for `--soak-seconds` (default 86400) with 0.05 % timeout
injection.  Catches:

  * gradual FID/server-state leak (N3)
  * slow memory drift in fid_pool or handler allocations
  * latent race conditions in walk_to/clunk under high churn
"""
from __future__ import annotations
import hashlib
import os
import random
import time

from . import common as cm
from . import injection as inj


TIER = "Tier 16"

WEIGHTS = {
    "read":   40,
    "write":  30,
    "list":   15,
    "create": 7,
    "delete": 3,
    "rename": 3,
    "chmod":  2,
}


def _random_op(rnd: random.Random) -> str:
    pool: list[str] = []
    for op, w in WEIGHTS.items():
        pool.extend([op] * w)
    return rnd.choice(pool)


def _t16_random_workload(ctx: cm.Ctx, total_seconds: int,
                        inject_pct: float) -> None:
    rnd = random.Random(0x501C)
    root_rel = "_tier16_soak"
    cm.rm_host(root_rel)
    os.makedirs(cm.host_path(root_rel), exist_ok=True)

    files: list[str] = []
    for i in range(200):
        rel = f"{root_rel}/seed_{i:04d}.bin"
        with open(cm.host_path(rel), "wb") as f:
            f.write(bytes(rnd.randint(0, 255) for _ in range(1024)))
        files.append(rel)

    qmp = (inj.pause_resume(ctx.qmp_path)
           if inj.qmp_available(ctx.qmp_path) else None)

    start = time.time()
    deadline = start + total_seconds
    ops = 0
    errors = 0
    last_progress = start
    try:
        while time.time() < deadline:
            op = _random_op(rnd)
            try:
                if op == "list":
                    out = cm.run(ctx.c, f"C:List {cm.guest_path(root_rel)}",
                                 timeout=15)
                    if "seed_" not in out:
                        errors += 1
                elif op == "read":
                    rel = rnd.choice(files)
                    out = cm.run(ctx.c, f"C:Type {cm.guest_path(rel)}",
                                 timeout=15)
                    if "fail" in out.lower():
                        errors += 1
                elif op == "write":
                    rel = f"{root_rel}/wr_{ops:06d}.bin"
                    with open(cm.host_path(rel), "wb") as f:
                        f.write(bytes(rnd.randint(0, 255) for _ in range(256)))
                    files.append(rel)
                elif op == "create":
                    rel = f"{root_rel}/cr_{ops:06d}.txt"
                    cm.run(ctx.c, f"C:Echo > {cm.guest_path(rel)}", timeout=10)
                    files.append(rel)
                elif op == "delete":
                    if files:
                        rel = files.pop(rnd.randrange(len(files)))
                        cm.run(ctx.c, f"C:Delete {cm.guest_path(rel)} QUIET",
                               timeout=10)
                elif op == "rename":
                    if files:
                        rel = rnd.choice(files)
                        new = rel + ".r"
                        cm.run(ctx.c, f"C:Rename {cm.guest_path(rel)} TO "
                                      f"{cm.guest_path(new)}", timeout=10)
                        files = [new if x == rel else x for x in files]
                elif op == "chmod":
                    if files:
                        rel = rnd.choice(files)
                        flags = "+w" if (ops & 1) else "-w"
                        cm.run(ctx.c,
                               f"C:Protect {cm.guest_path(rel)} {flags}",
                               timeout=10)
            except Exception:
                errors += 1
            ops += 1

            # Random fault injection
            if qmp is not None and rnd.random() < inject_pct:
                try:
                    qmp.stop()
                    time.sleep(0.7)
                    qmp.cont()
                except Exception:
                    pass

            # Progress dot every 5 minutes of wall-time
            if time.time() - last_progress > 300:
                elapsed = int(time.time() - start)
                print(f"    [tier16] {elapsed}s elapsed, {ops} ops, "
                      f"{errors} errors", flush=True)
                last_progress = time.time()
    finally:
        if qmp is not None:
            try:
                qmp.close()
            except Exception:
                pass
        cm.rm_host(root_rel)

    # Final liveness check + handler responsiveness probe
    alive = cm.handler_alive(ctx.c)

    ok = alive and errors == 0
    detail = (f"{ops} ops, {errors} errors, handler "
              f"{'alive' if alive else 'unresponsive'}")
    ctx.score.record(
        TIER, "16.1", f"random workload soak ({total_seconds} s)",
        "PASS" if ok else "FAIL", detail,
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 16 — Long-haul soak")
    if not ctx.run_soak:
        ctx.score.record(
            TIER, "16.1", "random workload soak",
            "SKIP", "--soak not passed",
        )
        return
    seconds = int(os.environ.get("V9P_SOAK_SECONDS", "86400"))
    inject_pct = float(os.environ.get("V9P_SOAK_INJECT_PCT", "0.0005"))
    _t16_random_workload(ctx, seconds, inject_pct)
