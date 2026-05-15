"""
Tier 13 — Transport reset + FID lifecycle.

Covers L4 + N3 (P1-5 + P1-6).  All four sub-tests SKIP by default
because they require:
  * a controlled way to force the device into a failed state, AND
  * a debug-build marker ("[virtio9p] V9P_Reset") emitted by the new
    reset code path.

Once P1-5 ships, the agent flips these to active by setting a
'tier13_active' env var and providing the debug knob — typically a
private DOS command or a debug-only escape sequence that the handler
recognises and uses to fail-injection the next transaction.
"""
from __future__ import annotations
import os

from . import common as cm
from . import injection as inj


TIER = "Tier 13"


def _has_reset_support(ctx: cm.Ctx) -> bool:
    if os.environ.get("V9P_TIER13_ACTIVE") != "1":
        return False
    log = inj.scrape_serial_log(ctx.serial_log)
    return "[virtio9p] V9P_Reset" in log


def _t13_1_reset_round_trip(ctx: cm.Ctx) -> None:
    if not _has_reset_support(ctx):
        ctx.score.record(
            TIER, "13.1", "transport reset round-trip",
            "SKIP", "P1-5 not yet active (set V9P_TIER13_ACTIVE=1 once shipped)",
        )
        return
    # Active body intentionally left empty — to be authored in the
    # same commit as P1-5.  It will:
    #   1. trigger reset via debug knob
    #   2. confirm '[virtio9p] V9P_Reset OK' appears in serial
    #   3. drive 1000 post-reset ops and assert all succeed
    ctx.score.record(
        TIER, "13.1", "transport reset round-trip",
        "SKIP", "test body authored alongside P1-5",
    )


def _t13_2_reset_invalidates_fids(ctx: cm.Ctx) -> None:
    if not _has_reset_support(ctx):
        ctx.score.record(
            TIER, "13.2", "reset invalidates outstanding FIDs",
            "SKIP", "P1-5 not yet active",
        )
        return
    ctx.score.record(
        TIER, "13.2", "reset invalidates outstanding FIDs",
        "SKIP", "test body authored alongside P1-5",
    )


def _t13_3_fid_orphan_not_reused(ctx: cm.Ctx) -> None:
    if not _has_reset_support(ctx):
        ctx.score.record(
            TIER, "13.3", "FID orphan never reused before reclaim",
            "SKIP", "P1-6 not yet active",
        )
        return
    ctx.score.record(
        TIER, "13.3", "FID orphan never reused before reclaim",
        "SKIP", "test body authored alongside P1-6",
    )


def _t13_4_no_ghost_fid_soak(ctx: cm.Ctx) -> None:
    """12-h ghost-FID soak; only runs when both P1-6 is active AND the
    runner was invoked with --soak."""
    if not _has_reset_support(ctx) or not ctx.run_soak:
        ctx.score.record(
            TIER, "13.4", "12-h ghost-FID soak",
            "SKIP",
            "requires P1-6 active and --soak",
        )
        return
    ctx.score.record(
        TIER, "13.4", "12-h ghost-FID soak",
        "SKIP", "test body authored alongside P1-6",
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 13 — Transport reset + FID lifecycle")
    _t13_1_reset_round_trip(ctx)
    _t13_2_reset_invalidates_fids(ctx)
    _t13_3_fid_orphan_not_reused(ctx)
    _t13_4_no_ghost_fid_soak(ctx)
