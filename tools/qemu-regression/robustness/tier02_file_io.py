"""
Tier 2 -- File I/O integrity (size tiers + read path).

Drives the SerialClient.upload_file path (guest->host write) at four
size tiers spanning the 64 KB I/O chunk boundary and the 512 KB msize
boundary, plus a host->guest->host read-path round-trip.

The 100-iteration small-file loop from the legacy stress_suite is not
ported -- Tier 12.4 (200-op orphan-bound smoke) provides equivalent
churn-and-survive coverage.

2.1  tiny (20 B)              -- byte-exact short payload
2.2  90 KB                    -- just over the 64 KB I/O chunk
2.3  500 KB                   -- below msize (512 KB)
2.4  1.5 MB                   -- forces multi-msize 9P round-trips
2.5  1 MB host->guest->host   -- exercises the read path
"""
from __future__ import annotations
import hashlib
import os
import random
import tempfile

from . import common as cm


TIER = "Tier 2"


def _upload_and_verify(ctx: cm.Ctx, src_host: str, rel: str) -> tuple[bool, str]:
    """Push src_host -> guest L: -> SHARED:rel via SerialClient, verify
    on host by SHA."""
    cm.rm_host(rel)
    expected = cm.hash_file(src_host)
    expected_size = os.path.getsize(src_host)
    try:
        ctx.c.upload_file(src_host, f"L:{rel}")
    except Exception as e:
        return False, f"upload raised: {e}"
    cm.run(ctx.c, f"C:Copy L:{rel} TO {cm.guest_path(rel)} CLONE QUIET",
           timeout=120)
    cm.run(ctx.c, f"C:Delete L:{rel} QUIET", timeout=10)
    host_p = cm.host_path(rel)
    if not os.path.exists(host_p):
        return False, "host file missing after upload+copy"
    actual = cm.hash_file(host_p)
    actual_size = os.path.getsize(host_p)
    ok = (actual == expected) and (actual_size == expected_size)
    return ok, f"size={actual_size}/{expected_size} sha=" + (
        "match" if actual == expected else f"MISMATCH ({actual[:8]} vs {expected[:8]})")


def _host_write_then_guest_read(
    ctx: cm.Ctx, rel: str, data: bytes
) -> tuple[bool, str]:
    """Host writes the file, guest copies it back to L:, then we SHA the
    L: copy (download via SerialClient)."""
    cm.rm_host(rel)
    with open(cm.host_path(rel), "wb") as f:
        f.write(data)
    cm.run(ctx.c, f"C:Copy {cm.guest_path(rel)} TO L:{rel}_back CLONE QUIET",
           timeout=120)
    # Read back via guest C:Type into a buffer would be slow; use FileSize
    # as a sanity gate, then download for SHA.
    sz = cm.size_of(cm.run(ctx.c, f"C:FileSize L:{rel}_back", timeout=15))
    if sz != len(data):
        cm.run(ctx.c, f"C:Delete L:{rel}_back QUIET", timeout=10)
        return False, f"L: copy size {sz} != {len(data)}"
    # Download L: copy for SHA verify
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tmp = tf.name
    try:
        ctx.c.download_file(f"L:{rel}_back", tmp)
        with open(tmp, "rb") as f:
            actual = hashlib.sha256(f.read()).hexdigest()
    except Exception as e:
        cm.run(ctx.c, f"C:Delete L:{rel}_back QUIET", timeout=10)
        return False, f"download raised: {e}"
    finally:
        try: os.unlink(tmp)
        except OSError: pass
    cm.run(ctx.c, f"C:Delete L:{rel}_back QUIET", timeout=10)
    expected = hashlib.sha256(data).hexdigest()
    ok = (actual == expected)
    return ok, "sha match" if ok else f"sha MISMATCH ({actual[:8]} vs {expected[:8]})"


def _src_with(payload: bytes) -> str:
    fd, path = tempfile.mkstemp(prefix="_v9p_t2_", suffix=".bin")
    with os.fdopen(fd, "wb") as f:
        f.write(payload)
    return path


def _t2_1_tiny(ctx: cm.Ctx) -> None:
    src = _src_with(b"byte-exact 9p marker\n")
    try:
        ok, d = _upload_and_verify(ctx, src, "tier2_tiny.bin")
        ctx.score.record(TIER, "2.1", "tiny-file guest->host SHA round-trip",
                         "PASS" if ok else "FAIL", d)
    finally:
        os.unlink(src)
        cm.rm_host("tier2_tiny.bin")


def _t2_2_mid(ctx: cm.Ctx) -> None:
    rnd = random.Random(0xBEEF)
    src = _src_with(bytes(rnd.randint(0, 255) for _ in range(90 * 1024)))
    try:
        ok, d = _upload_and_verify(ctx, src, "tier2_mid.bin")
        ctx.score.record(TIER, "2.2", "90 KB guest->host SHA round-trip",
                         "PASS" if ok else "FAIL", d)
    finally:
        os.unlink(src)
        cm.rm_host("tier2_mid.bin")


def _t2_3_big(ctx: cm.Ctx) -> None:
    rnd = random.Random(0xBEEF + 1)
    src = _src_with(bytes(rnd.randint(0, 255) for _ in range(500 * 1024)))
    try:
        ok, d = _upload_and_verify(ctx, src, "tier2_big.bin")
        ctx.score.record(TIER, "2.3", "500 KB guest->host SHA round-trip",
                         "PASS" if ok else "FAIL", d)
    finally:
        os.unlink(src)
        cm.rm_host("tier2_big.bin")


def _t2_4_huge(ctx: cm.Ctx) -> None:
    rnd = random.Random(0xBEEF + 2)
    src = _src_with(bytes(rnd.randint(0, 255) for _ in range(1536 * 1024)))
    try:
        ok, d = _upload_and_verify(ctx, src, "tier2_huge.bin")
        ctx.score.record(
            TIER, "2.4", "1.5 MB guest->host SHA round-trip (multi-msize)",
            "PASS" if ok else "FAIL", d)
    finally:
        os.unlink(src)
        cm.rm_host("tier2_huge.bin")


def _t2_5_read_path(ctx: cm.Ctx) -> None:
    rnd = random.Random(0xBEEF + 3)
    payload = bytes(rnd.randint(0, 255) for _ in range(1024 * 1024))
    ok, d = _host_write_then_guest_read(ctx, "tier2_h2g.bin", payload)
    cm.rm_host("tier2_h2g.bin")
    ctx.score.record(
        TIER, "2.5", "1 MB host->guest->host SHA round-trip (read path)",
        "PASS" if ok else "FAIL", d)


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 2 -- File I/O integrity")
    _t2_1_tiny(ctx)
    _t2_2_mid(ctx)
    _t2_3_big(ctx)
    _t2_4_huge(ctx)
    _t2_5_read_path(ctx)
