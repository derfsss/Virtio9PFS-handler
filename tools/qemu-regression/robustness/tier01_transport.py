"""
Tier 1 -- Transport-mode confirmation via serial log.

Parses the QEMU serial log (which the handler emits debug markers
into) and confirms the transport-detection path the handler ended up
on:

  - On Pegasos2 / SAM460ex (transparent-bridge machines) the handler
    should report MODERN mode after the MMIO probe succeeds.
  - On AmigaOne (Articia S floating-buffer bug) the handler should
    fall back to LEGACY I/O mode after the modern probe fails.

Either outcome is a PASS as long as the probe + mode-decision lines
are present.  Absent markers indicate the handler shipped without
the v0.8.0 modern-detect code paths -- a regression.

1.1  modern_or_legacy_marker -- one of MODERN/legacy-fallback printed
1.2  bar5_workaround_log     -- the BAR5-high-DWORD workaround printed
                                on AmigaOne (informational on others)
"""
from __future__ import annotations
import os

from . import common as cm


TIER = "Tier 1"


def _read_log(path: str) -> str:
    if not os.path.exists(path):
        return ""
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        return ""
    return raw.replace(b"\x00", b"").decode(errors="replace")


def _t1_1_mode_marker(ctx: cm.Ctx) -> None:
    text = _read_log(ctx.serial_log)
    if not text:
        ctx.score.record(
            TIER, "1.1", "modern probe + mode-decision marker present",
            "SKIP", f"serial log not readable: {ctx.serial_log}",
        )
        return
    modern = ("[virtio9p] pci_modern_detect: MMIO probe OK" in text
              or "[virtio9p] pci_modern_detect: VirtIO mode: MODERN" in text)
    legacy_fallback = ("falling back to legacy" in text
                       and "[virtio9p] pci_modern_detect" in text)
    ok = modern or legacy_fallback
    detail = ("MODERN" if modern
              else "LEGACY-fallback" if legacy_fallback
              else "no probe logged")
    ctx.score.record(
        TIER, "1.1", "modern probe + mode-decision marker present",
        "PASS" if ok else "FAIL", detail,
    )


def _t1_2_bar5_marker(ctx: cm.Ctx) -> None:
    text = _read_log(ctx.serial_log)
    if not text:
        ctx.score.record(
            TIER, "1.2", "BAR5 high-DWORD workaround marker (informational)",
            "SKIP", "serial log not readable",
        )
        return
    bar5 = "[virtio9p] PCI_Discovery: BAR5 high DWORD" in text
    # 1.2 is informational -- absence on Pegasos2/SAM460 is normal.
    # We always PASS as long as the log is parseable; the detail
    # tells you which machine you're on.
    ctx.score.record(
        TIER, "1.2", "BAR5 high-DWORD workaround marker (informational)",
        "PASS",
        "BAR5 fix applied (AmigaOne)" if bar5
        else "BAR5 was sane (Pegasos2/SAM460)",
    )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 1 -- Transport-mode confirmation")
    _t1_1_mode_marker(ctx)
    _t1_2_bar5_marker(ctx)
