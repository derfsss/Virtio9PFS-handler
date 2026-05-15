"""
Tier 5 -- Regression guards for specific historical bugs.

Slimmed from the legacy stress_suite.py Tier 5 down to the one test
that targets a specific, named historical regression.  The rest of
the legacy Tier 5 (long filename, rename, symlink, hardlink) is
covered by Tier 7.3 and Tier 3.3/3.10/3.11 and is not duplicated
here.

5.1  fsync_null_fi_guard -- v0.7.1 fixed a NULL-fi crash in v9p_fsync
                            when FBX flushed an unopened path.  We
                            drive a Copy then Delete cycle which
                            historically tickled the bug, then probe
                            handler liveness with C:Info.
"""
from __future__ import annotations
import os
import random

from . import common as cm


TIER = "Tier 5"


def _t5_1_fsync_null_fi_guard(ctx: cm.Ctx) -> None:
    rnd = random.Random(0xFACE)
    data = bytes(rnd.randint(0, 255) for _ in range(8 * 1024))
    rel = "tier5_fsync.bin"
    copy_rel = "tier5_fsync_copy.bin"
    cm.rm_host(rel); cm.rm_host(copy_rel)
    with open(cm.host_path(rel), "wb") as f:
        f.write(data)

    cm.run(ctx.c, f"C:Copy {cm.guest_path(rel)} TO {cm.guest_path(copy_rel)} "
                  f"CLONE", timeout=20)
    cm.run(ctx.c, f"C:Delete {cm.guest_path(rel)} QUIET", timeout=10)

    dst = cm.host_path(copy_rel)
    copy_ok = os.path.exists(dst) and open(dst, "rb").read() == data
    alive = cm.handler_alive(ctx.c)

    cm.rm_host(copy_rel)
    ok = copy_ok and alive
    ctx.score.record(
        TIER, "5.1",
        "fsync NULL-fi crash guard (Copy then Delete; handler still alive)",
        "PASS" if ok else "FAIL",
        f"dst={'present' if copy_ok else 'missing'}, alive={alive}",
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 5 -- Regression guards")
    _t5_1_fsync_null_fi_guard(ctx)
