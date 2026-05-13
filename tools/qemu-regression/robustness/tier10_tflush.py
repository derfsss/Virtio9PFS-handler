"""
Tier 10 — Tflush isolation.

Covers investigation item B2 (P0-2):
  Today the Tflush message is built into the *same* tx_buf that holds
  the original (still in-flight) T-message, racing the device's read.

10.1  test_tflush_does_not_corrupt_pending_tx
        Inject 50 forced timeouts during a stress and assert all data
        round-trips remain SHA-correct.
10.2  test_tflush_buffer_marker
        Once P0-2 ships a dedicated flush buffer, a startup DPRINTF
        (debug build) reports the flush-buf physical address.  Verify
        it appears.  Until then this is a SKIP-on-release-build,
        FAIL-on-debug-pre-fix scaffold.
"""
from __future__ import annotations
import hashlib
import os
import random
import time

from . import common as cm
from . import injection as inj


TIER = "Tier 10"


def _t10_1_tflush_no_tx_corruption(ctx: cm.Ctx) -> None:
    if not inj.qmp_available(ctx.qmp_path):
        ctx.score.record(
            TIER, "10.1", "Tflush isolation under timeout stress",
            "SKIP", "QMP socket not available",
        )
        return

    rnd = random.Random(0xBADCAFE)
    N_OPS = 200
    INJECT_EVERY = 4  # ≈ 50 timeout injections across 200 ops
    file_count = 16

    # Pre-create a pool of files with known SHA on the host
    expected_sha: dict[str, str] = {}
    for i in range(file_count):
        rel = f"_tier10_pool_{i:02d}.bin"
        cm.rm_host(rel)
        payload = bytes(rnd.randint(0, 255) for _ in range(64 * 1024))
        with open(cm.host_path(rel), "wb") as f:
            f.write(payload)
        expected_sha[rel] = hashlib.sha256(payload).hexdigest()

    qmp = inj.pause_resume(ctx.qmp_path)

    bad = 0
    bad_files: list[str] = []

    try:
        for i in range(N_OPS):
            # Periodically inject a stall.  Short pauses (~600 ms) trigger
            # the iter-count timeout pre-P2-7 and the wallclock timeout
            # post-P2-7 alike (after we tune the wallclock value down for
            # this test, or accept this as a stress with no timeouts).
            if (i % INJECT_EVERY) == 0:
                qmp.stop()
                time.sleep(0.6)
                qmp.cont()
                time.sleep(0.05)

            rel = f"_tier10_pool_{rnd.randrange(file_count):02d}.bin"
            ram_tmp = "RAM:_v9p_t10_back"
            cm.run(ctx.c, f"C:Copy {cm.guest_path(rel)} TO {ram_tmp} CLONE",
                   timeout=20)
            tmp_back = "/tmp/_v9p_t10_back.bin"
            try:
                ctx.c.download_file(ram_tmp, tmp_back)
            except Exception:
                bad += 1
                bad_files.append(rel)
                continue
            cm.run(ctx.c, f"C:Delete {ram_tmp} QUIET", timeout=10)

            try:
                with open(tmp_back, "rb") as f:
                    h = hashlib.sha256(f.read()).hexdigest()
            finally:
                try:
                    os.unlink(tmp_back)
                except OSError:
                    pass
            if h != expected_sha[rel]:
                bad += 1
                bad_files.append(rel)
    finally:
        qmp.close()
        for rel in expected_sha:
            cm.rm_host(rel)

    ok = (bad == 0)
    detail = f"{N_OPS - bad}/{N_OPS} round-trips matched"
    if not ok:
        detail += f" (bad: {bad_files[:3]}{'…' if len(bad_files) > 3 else ''})"
    ctx.score.record(
        TIER, "10.1", "Tflush isolation under timeout stress",
        "PASS" if ok else "FAIL", detail,
    )


def _t10_2_tflush_buffer_marker(ctx: cm.Ctx) -> None:
    """After P0-2 lands, debug build prints a marker on startup naming
    the dedicated flush buffer's phys address.  This is also useful as
    a regression: if someone reverts to tx_buf reuse, the marker is
    gone."""
    log = inj.scrape_serial_log(ctx.serial_log)
    if not log:
        ctx.score.record(
            TIER, "10.2", "dedicated Tflush buffer marker present",
            "SKIP", "no serial log available",
        )
        return
    # Looking for any of the candidate marker prefixes the fix may emit.
    candidates = [
        "[virtio9p] flush_buf phys=",
        "[virtio9p] tflush_buf=",
        "[virtio9p] dedicated tflush",
        "flush_phys=0x",       # actual marker from main.c P0-2 commit
    ]
    found = next((c for c in candidates if c in log), None)
    if found:
        ctx.score.record(
            TIER, "10.2", "dedicated Tflush buffer marker present",
            "PASS", f"saw '{found}…'",
        )
    else:
        # Pre-fix this is expected to fail — that's the point of a
        # red-baseline test.  But it's noise on release builds (no
        # DPRINTF).  Mark FAIL and let the runner show the rationale.
        ctx.score.record(
            TIER, "10.2", "dedicated Tflush buffer marker present",
            "FAIL",
            "no flush-buf marker in serial log "
            "(P0-2 not implemented yet, or release build)",
        )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 10 — Tflush isolation")
    _t10_1_tflush_no_tx_corruption(ctx)
    _t10_2_tflush_buffer_marker(ctx)
