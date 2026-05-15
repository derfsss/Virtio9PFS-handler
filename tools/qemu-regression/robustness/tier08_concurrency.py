"""
Tier 8 -- Concurrency.

The handler runs in a single FBX event loop, but FBX itself can issue
overlapping requests when multiple AmigaOS shells are doing I/O at
once.  These tests don't exercise true OS-level threading; they
exercise the handler's resilience to back-to-back asynchronous
work and to host-side mutation mid-transfer.

8.1  parallel_copy   -- 3 guest C:Run Copy jobs from a single source;
                        all three target files must end byte-equal.
8.2  host_write_mid_read -- guest starts a Copy, host overwrites the
                        source mid-flight; guest sees coherent content
                        (either old or new payload, never a torn mix).
"""
from __future__ import annotations
import os
import random
import time

from . import common as cm


TIER = "Tier 8"


def _t8_1_parallel_copy(ctx: cm.Ctx) -> None:
    N = 3
    seed_rel = "tier8_seed.bin"
    dests = [f"tier8_parallel_{i}.bin" for i in range(N)]
    cm.rm_host(seed_rel)
    for d in dests:
        cm.rm_host(d)

    rnd = random.Random(0xC0FFEE)
    seed = bytes(rnd.randint(0, 255) for _ in range(200 * 1024))
    with open(cm.host_path(seed_rel), "wb") as f:
        f.write(seed)

    for d in dests:
        cm.run(ctx.c, f"C:Run C:Copy {cm.guest_path(seed_rel)} TO "
                      f"{cm.guest_path(d)} CLONE", timeout=10)

    deadline = time.time() + 60
    finished = [False] * N
    while time.time() < deadline and not all(finished):
        for i, d in enumerate(dests):
            if finished[i]:
                continue
            p = cm.host_path(d)
            if os.path.exists(p) and os.path.getsize(p) == len(seed):
                finished[i] = True
        if not all(finished):
            time.sleep(1)

    hashes_match = True
    for d in dests:
        p = cm.host_path(d)
        if not os.path.exists(p) or open(p, "rb").read() != seed:
            hashes_match = False
            break

    cm.rm_host(seed_rel)
    for d in dests:
        cm.rm_host(d)

    ok = all(finished) and hashes_match
    ctx.score.record(
        TIER, "8.1",
        f"{N} parallel guest Copy jobs land correct bytes on host",
        "PASS" if ok else "FAIL",
        f"finished={sum(finished)}/{N} hashes={'OK' if hashes_match else 'MISMATCH'}",
    )


def _t8_2_host_write_mid_read(ctx: cm.Ctx) -> None:
    pay_rel = "tier8_rw.bin"
    dst_rel = "tier8_rw_out.bin"
    cm.rm_host(pay_rel); cm.rm_host(dst_rel)

    payload1 = b"payload1-" * (32 * 1024)    # ~256 KB
    payload2 = b"payload2-" * (32 * 1024)
    with open(cm.host_path(pay_rel), "wb") as f:
        f.write(payload1)
    cm.run(ctx.c, f"C:Run C:Copy {cm.guest_path(pay_rel)} TO "
                  f"{cm.guest_path(dst_rel)} CLONE", timeout=10)
    with open(cm.host_path(pay_rel), "wb") as f:
        f.write(payload2)

    ok = False
    for _ in range(30):
        p = cm.host_path(dst_rel)
        if os.path.exists(p) and os.path.getsize(p) in (len(payload1), len(payload2)):
            content = open(p, "rb").read()
            ok = (content == payload1 or content == payload2)
            break
        time.sleep(0.5)

    cm.rm_host(pay_rel); cm.rm_host(dst_rel)
    ctx.score.record(
        TIER, "8.2",
        "host-write-mid-guest-read: guest sees coherent (old OR new) content",
        "PASS" if ok else "FAIL",
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 8 -- Concurrency")
    _t8_1_parallel_copy(ctx)
    _t8_2_host_write_mid_read(ctx)
