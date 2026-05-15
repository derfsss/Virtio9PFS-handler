"""
Tier 3 -- Native on-guest test_9p binary.

The repo ships a guest-native test_9p (built by `make test`) that
opens dos.library FileHandles directly and exercises every dos
operation the handler implements -- including the open-handle
SetFileSize path that the previously-SKIPPed Tier 4.9 couldn't
reach from the shell.

This tier uploads the native binary, runs it against SHARED:, and
checks that no per-test FAIL line appears in its output.

Resolves the Tier 4.9 SKIP: ftruncate (open-handle SetFileSize) is
covered here, end-to-end, against the live handler.

3.1  test_9p_runs_and_passes
"""
from __future__ import annotations
import os

from . import common as cm


TIER = "Tier 3"


def _resolve_test_9p_path() -> str:
    """Locate build/test_9p relative to this file: this file lives at
    tools/qemu-regression/robustness/, so the project root is three
    levels up."""
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.normpath(os.path.join(here, "..", "..", ".."))
    return os.path.join(repo, "build", "test_9p")


def _t3_1_run_test_9p(ctx: cm.Ctx) -> None:
    bin_path = _resolve_test_9p_path()
    if not os.path.exists(bin_path):
        ctx.score.record(
            TIER, "3.1", "native test_9p end-to-end on SHARED:",
            "SKIP",
            f"binary not built at {bin_path} -- run `make test`",
        )
        return

    try:
        ctx.c.upload_file(bin_path, "RAM:test_9p")
    except Exception as e:
        ctx.score.record(
            TIER, "3.1", "native test_9p end-to-end on SHARED:",
            "FAIL", f"upload raised: {e}",
        )
        return

    cm.run(ctx.c, "C:Protect RAM:test_9p +rwed", timeout=10)
    out = cm.run(ctx.c, f"RAM:test_9p {cm.GUEST_VOL}", timeout=300)

    tail = out.strip().splitlines()[-1] if out.strip() else ""
    fail_lines = [line.strip() for line in out.splitlines()
                  if "FAIL" in line and "FAILED" not in line]
    has_fail = bool(fail_lines)
    ok = not has_fail and bool(tail)
    if has_fail:
        # Surface the first FAIL line so the cause is visible without
        # opening the serial log.
        detail = f"{tail}; first FAIL: {fail_lines[0]}"
    else:
        detail = tail or "no output"
    ctx.score.record(
        TIER, "3.1", "native test_9p end-to-end on SHARED:",
        "PASS" if ok else "FAIL", detail,
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 3 -- Native on-guest test_9p binary")
    _t3_1_run_test_9p(ctx)
