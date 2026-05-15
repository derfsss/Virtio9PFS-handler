"""
Shared helpers for the v0.9.0 robustness test tiers.

Mirrors the conventions of stress_suite.py so the two suites cooperate:
- Same SerialClient (port 4321 by default).
- Same host-side ground-truth dir (/tmp/p9share).
- Same SHARED: guest mount.

Results are accumulated in a single Score object so the final summary
covers every tier the runner executed.
"""
from __future__ import annotations
import hashlib
import os
import sys
import time
from dataclasses import dataclass, field
from typing import Callable

# Resolve the qemu-runner SerialClient module across host styles
# (WSL → /mnt/..., Windows native → drive-letter path).  Both refer
# to the same checkout; whichever exists first wins.
_QEMU_RUNNER_CANDIDATES = [
    "/mnt/w/Code/amiga/antigravity/projects/tools/qemu-runner",
    r"W:\Code\amiga\antigravity\projects\tools\qemu-runner",
    os.environ.get("V9P_QEMU_RUNNER", ""),
]
for _p in _QEMU_RUNNER_CANDIDATES:
    if _p and os.path.isdir(_p):
        sys.path.insert(0, _p)
        break

from serial_client import SerialClient  # noqa: E402

# ----- Paths / constants matching stress_suite.py ------------------------
DEFAULT_PORT      = 4321
DEFAULT_SERIAL    = os.environ.get("V9P_SERIAL_LOG",
                                   "/tmp/test_peg2/serial_full_2.log")
DEFAULT_QMP       = os.environ.get("V9P_QMP",
                                   "/tmp/test_peg2/qmp.sock")
P9_SHARE_HOST     = os.environ.get("V9P_SHARE_HOST", "/tmp/p9share")
GUEST_VOL         = "SHARED:"
MSIZE_BYTES       = 512 * 1024


# ----- Result model -------------------------------------------------------
@dataclass
class TestResult:
    tier: str           # e.g. "Tier 9"
    test_id: str        # e.g. "9.1"
    name: str
    status: str         # "PASS" | "FAIL" | "SKIP"
    detail: str = ""


@dataclass
class Score:
    results: list[TestResult] = field(default_factory=list)

    def record(self, tier: str, test_id: str, name: str,
               status: str, detail: str = "") -> None:
        self.results.append(TestResult(tier, test_id, name, status, detail))
        tag = status.ljust(4)
        suffix = f"  ({detail})" if detail else ""
        print(f"  [{tag}] {test_id} {name}{suffix}", flush=True)

    def passed(self) -> int:
        return sum(1 for r in self.results if r.status == "PASS")

    def failed(self) -> int:
        return sum(1 for r in self.results if r.status == "FAIL")

    def skipped(self) -> int:
        return sum(1 for r in self.results if r.status == "SKIP")

    def total(self) -> int:
        return len(self.results)

    def running(self) -> int:
        """Running = PASS + FAIL; SKIP does not count toward the rate."""
        return self.passed() + self.failed()

    def per_tier(self) -> dict[str, tuple[int, int, int]]:
        """Returns {tier: (passed, failed, skipped)}."""
        out: dict[str, list[int]] = {}
        for r in self.results:
            slot = out.setdefault(r.tier, [0, 0, 0])
            slot[0 if r.status == "PASS" else 1 if r.status == "FAIL" else 2] += 1
        return {k: tuple(v) for k, v in out.items()}  # type: ignore[misc]


# ----- Context ------------------------------------------------------------
@dataclass
class Ctx:
    c: SerialClient
    qmp_path: str
    serial_log: str
    score: Score
    run_soak: bool = False
    """Set by runner CLI flag; tier 16 only executes when True."""


# ----- Shell + path helpers (mirror stress_suite.py) ---------------------
def run(c: SerialClient, cmd: str, timeout: int = 30) -> str:
    return c.send_command(cmd, timeout=timeout)


def guest_path(rel: str) -> str:
    return f"{GUEST_VOL}{rel}"


def host_path(rel: str) -> str:
    return os.path.join(P9_SHARE_HOST, rel)


def hash_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def size_of(out: str) -> int:
    for line in (out or "").splitlines():
        line = line.strip()
        if "bytes" in line:
            toks = line.replace(",", "").split()
            for i, t in enumerate(toks):
                if t == "bytes" and i > 0 and toks[i - 1].isdigit():
                    return int(toks[i - 1])
    return -1


def free_mem(c: SerialClient) -> int:
    out = run(c, "avail", timeout=10)
    for line in out.splitlines():
        if line.strip().startswith("Free:"):
            try:
                return int(line.split()[1].replace(",", ""))
            except Exception:
                return -1
    return -1


def rm_host(rel: str) -> None:
    p = host_path(rel)
    try:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p, topdown=False):
                for f in files:
                    os.unlink(os.path.join(root, f))
                for d in dirs:
                    os.rmdir(os.path.join(root, d))
            os.rmdir(p)
        elif os.path.exists(p) or os.path.islink(p):
            os.unlink(p)
    except OSError:
        pass


def rm_guest(c: SerialClient, rel: str) -> None:
    run(c, f"C:Delete {guest_path(rel)} ALL QUIET", timeout=30)


def header(title: str) -> None:
    print(f"\n============ {title} ============", flush=True)


# ----- Liveness probe -----------------------------------------------------
def handler_alive(c: SerialClient, timeout: int = 10) -> bool:
    """Quick liveness check after a fault-injection step.  We use `info
    SHARED:` because it goes through a handler packet but doesn't open or
    walk anything heavy."""
    try:
        out = run(c, "info SHARED:", timeout=timeout)
    except Exception:
        return False
    return "SHARED:" in (out or "")
