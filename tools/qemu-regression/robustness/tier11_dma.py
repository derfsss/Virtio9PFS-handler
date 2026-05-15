"""
Tier 11 — DMA stability.

Covers investigation item N1 (P1-3):
  After the handler calls EndDMA on its tx/rx buffers, the SDK explicitly
  permits the kernel to remap those pages.  The cached `tx_phys`/`rx_phys`
  values would then point at the wrong physical RAM.

11.1  test_dma_phys_stable_after_long_idle
        Mount; warm I/O; idle 30 s while host runs a 4 GB `dd` to stress
        the page cache (proxy for memory pressure); read back 100 MB and
        SHA-compare.
11.2  test_dma_phys_stable_after_many_ops
        Drive 5 000 small ops to exercise allocator activity, then do a
        100 MB SHA round-trip.

The 100 MB target is large enough to make a partial mis-DMA visible
(every msize-worth chunk = 512 KB → 200 chunks; even one bad chunk
would mismatch the SHA).
"""
from __future__ import annotations
import hashlib
import os
import random
import time

from . import common as cm
from . import injection as inj


TIER = "Tier 11"


def _make_payload(size_bytes: int, seed: int) -> bytes:
    rnd = random.Random(seed)
    # 1 MB chunks for speed; deterministic from seed.
    out = bytearray()
    while len(out) < size_bytes:
        out += bytes(rnd.randint(0, 255) for _ in range(min(1024 * 1024, size_bytes - len(out))))
    return bytes(out)


def _sha_round_trip_via_guest(ctx: cm.Ctx, rel: str, payload: bytes,
                              timeout: int) -> tuple[bool, str]:
    """Write payload to host; have guest Copy it to RAM:; download back;
    SHA-compare.  Returns (ok, detail)."""
    cm.rm_host(rel)
    with open(cm.host_path(rel), "wb") as f:
        f.write(payload)

    src_sha = hashlib.sha256(payload).hexdigest()
    ram_tmp = "RAM:_v9p_t11_back"
    out = cm.run(ctx.c, f"C:Copy {cm.guest_path(rel)} TO {ram_tmp} CLONE",
                 timeout=timeout)
    if "fail" in out.lower():
        cm.rm_host(rel)
        return (False, f"guest Copy reported failure: {out.strip().splitlines()[-1]}")

    tmp_back = "/tmp/_v9p_t11_back.bin"
    try:
        ctx.c.download_file(ram_tmp, tmp_back)
    except Exception as e:
        cm.run(ctx.c, f"C:Delete {ram_tmp} QUIET", timeout=10)
        cm.rm_host(rel)
        return (False, f"download failed: {e}")
    cm.run(ctx.c, f"C:Delete {ram_tmp} QUIET", timeout=10)

    try:
        with open(tmp_back, "rb") as f:
            got = f.read()
    finally:
        try:
            os.unlink(tmp_back)
        except OSError:
            pass

    got_sha = hashlib.sha256(got).hexdigest()
    cm.rm_host(rel)
    if got_sha == src_sha:
        return (True, f"SHA match for {len(payload):,} B")
    return (False,
            f"SHA mismatch ({len(got):,} B got vs {len(payload):,} B expected)")


def _t11_1_phys_stable_after_idle(ctx: cm.Ctx) -> None:
    # Warm-up I/O
    payload_warm = b"warmup-" * 16
    warm_ok, _ = _sha_round_trip_via_guest(
        ctx, "_tier11_warm.bin", payload_warm, timeout=30,
    )
    if not warm_ok:
        ctx.score.record(
            TIER, "11.1", "DMA phys stable after idle+pressure",
            "FAIL", "warm-up I/O failed",
        )
        return

    # Idle 30 s while host hammers the page cache
    print("    [tier11] starting 4 GB dd page-cache pressure (30 s soak)…",
          flush=True)
    pressure = inj.MemoryPressure(megabytes=4096)
    pressure.start()
    time.sleep(30)
    pressure.stop()

    # 100 MB SHA round-trip
    payload = _make_payload(100 * 1024 * 1024, seed=0xD0CACE)
    print(f"    [tier11] starting 100 MB SHA round-trip…", flush=True)
    ok, detail = _sha_round_trip_via_guest(
        ctx, "_tier11_big.bin", payload, timeout=600,
    )
    ctx.score.record(
        TIER, "11.1", "DMA phys stable after idle+pressure",
        "PASS" if ok else "FAIL", detail,
    )


def _t11_2_phys_stable_after_many_ops(ctx: cm.Ctx) -> None:
    # 5 000 ops (kept under 10 minutes wall time even on slow emulation)
    print("    [tier11] driving 5000 small ops to exercise allocator…",
          flush=True)
    rel = "_tier11_smallpool"
    cm.rm_host(rel)
    os.makedirs(cm.host_path(rel), exist_ok=True)

    OPS = 5000
    fail_at = -1
    for i in range(OPS):
        f_rel = f"{rel}/x_{i:04d}.txt"
        with open(cm.host_path(f_rel), "wb") as f:
            f.write(f"op {i}\n".encode())
        out = cm.run(ctx.c, f"C:List {cm.guest_path(f_rel)}", timeout=10)
        if f"x_{i:04d}.txt" not in out:
            fail_at = i
            break
        try:
            os.unlink(cm.host_path(f_rel))
        except OSError:
            pass
    cm.rm_host(rel)

    if fail_at >= 0:
        ctx.score.record(
            TIER, "11.2", "DMA phys stable after many ops",
            "FAIL", f"loop dropped at op {fail_at}",
        )
        return

    # Final 100 MB round-trip
    payload = _make_payload(100 * 1024 * 1024, seed=0xABCD1234)
    ok, detail = _sha_round_trip_via_guest(
        ctx, "_tier11_post.bin", payload, timeout=600,
    )
    ctx.score.record(
        TIER, "11.2", "DMA phys stable after many ops",
        "PASS" if ok else "FAIL",
        f"{OPS} ops then {detail}",
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 11 — DMA stability")
    _t11_1_phys_stable_after_idle(ctx)
    _t11_2_phys_stable_after_many_ops(ctx)
