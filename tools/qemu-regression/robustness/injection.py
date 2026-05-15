"""
Fault-injection helpers — used by Tier 9/10/14 to deterministically
provoke transport-level conditions (timeouts, queue stalls) that are
otherwise too rare to test reliably.

Two mechanisms:

  1. QEMU monitor over QMP socket  (best — pauses the virtual machine
     including the VirtIO device; the handler sees exactly the same
     stall pattern as a real device freeze).
  2. Host CPU pressure              (fallback when no QMP socket is
     wired up — slows QEMU enough to provoke ~500 ms+ stalls).

The QMP path requires that QEMU was launched with
    -qmp unix:/tmp/test_peg2/qmp.sock,server,nowait
or similar.  See robustness/README.md for the QEMU launch line.
"""
from __future__ import annotations
import json
import os
import socket
import subprocess
import time
from contextlib import contextmanager
from typing import Iterator


# ----- QMP (QEMU Machine Protocol) ---------------------------------------
class QMPError(RuntimeError):
    pass


def _parse_qmp_endpoint(spec: str) -> tuple[str, str | tuple[str, int]]:
    """Parse a QMP endpoint spec.

    Returns ("unix", path) or ("tcp", (host, port)).
    Accepted forms:
      "/some/path"                 -> unix socket at that path
      "unix:/some/path"            -> same
      "tcp:HOST:PORT"              -> TCP socket
      "127.0.0.1:14322"            -> TCP socket (auto-detect form)
    """
    if spec.startswith("tcp:"):
        rest = spec[4:]
        host, _, port = rest.rpartition(":")
        return ("tcp", (host or "127.0.0.1", int(port)))
    if spec.startswith("unix:"):
        return ("unix", spec[5:])
    if ":" in spec and spec.rsplit(":", 1)[1].isdigit():
        host, _, port = spec.rpartition(":")
        return ("tcp", (host, int(port)))
    return ("unix", spec)


class QMPClient:
    """Minimal QMP client -- supports both UNIX-socket and TCP endpoints
    (`unix:/path`, raw `/path`, `tcp:host:port`, or `host:port`)."""

    def __init__(self, path: str):
        self.path = path
        self.kind, self.target = _parse_qmp_endpoint(path)
        self.sock: socket.socket | None = None
        self.buf = b""

    def connect(self, timeout: float = 5.0) -> None:
        if self.kind == "unix":
            if not os.path.exists(self.target):
                raise QMPError(f"QMP UNIX socket not found: {self.target}")
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect(self.target)
        else:  # tcp
            try:
                s = socket.create_connection(self.target, timeout=timeout)
            except OSError as e:
                raise QMPError(
                    f"QMP TCP connect to {self.target} failed: {e}") from e
        self.sock = s
        greeting = self._recv_msg()
        if "QMP" not in greeting:
            raise QMPError(f"QMP greeting unexpected: {greeting!r}")
        self._send({"execute": "qmp_capabilities"})
        self._recv_msg()  # ack

    def _send(self, obj: dict) -> None:
        assert self.sock is not None
        self.sock.sendall((json.dumps(obj) + "\r\n").encode())

    def _recv_msg(self) -> dict:
        assert self.sock is not None
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise QMPError("QMP socket closed")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        try:
            return json.loads(line)
        except json.JSONDecodeError as e:
            raise QMPError(f"QMP non-JSON line: {line!r}") from e

    def command(self, cmd: str, **args) -> dict:
        msg = {"execute": cmd}
        if args:
            msg["arguments"] = args
        self._send(msg)
        # Skip async events until we see a return / error
        while True:
            r = self._recv_msg()
            if "return" in r or "error" in r:
                if "error" in r:
                    raise QMPError(f"QMP error for {cmd}: {r['error']}")
                return r["return"]

    def stop(self) -> None:
        self.command("stop")

    def cont(self) -> None:
        self.command("cont")

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None


@contextmanager
def vm_paused(qmp_path: str, duration_s: float) -> Iterator[None]:
    """Pause the VM for `duration_s` seconds (wall clock on the host), then
    resume.  Yields once paused so the caller can do nothing (the pause
    itself is the fault).  Useful only for symmetric stalls — for tests
    that need to act on the host *while* the guest is paused, use
    pause_resume() directly."""
    qmp = QMPClient(qmp_path)
    qmp.connect()
    try:
        qmp.stop()
        yield
        time.sleep(duration_s)
    finally:
        try:
            qmp.cont()
        finally:
            qmp.close()


def pause_resume(qmp_path: str) -> QMPClient:
    """Returns an open QMP client.  Caller is responsible for calling
    .stop(), doing whatever host-side work, then .cont() and .close().
    Useful for race tests (e.g. modify host while guest is paused).
    """
    q = QMPClient(qmp_path)
    q.connect()
    return q


def qmp_available(qmp_path: str) -> bool:
    """True if the configured QMP endpoint looks reachable.

    For UNIX sockets we check file existence + R/W access.
    For TCP we attempt a quick connect (best-effort, 1 s) -- if QEMU
    is up with `-qmp tcp:host:port,server=on,wait=off`, this succeeds.
    """
    kind, target = _parse_qmp_endpoint(qmp_path)
    if kind == "unix":
        return os.path.exists(target) and os.access(target, os.R_OK | os.W_OK)
    # tcp: try a short-timeout probe (and immediately close so we don't
    # consume the QMP greeting -- a fresh connect will get its own).
    try:
        s = socket.create_connection(target, timeout=1.0)
        s.close()
        return True
    except Exception:
        return False


# ----- Host CPU pressure (fallback) --------------------------------------
class CPUPressure:
    """Spawn N stress workers on the host to slow QEMU.  Falls back to
    Python loops if `stress-ng` is not available."""

    def __init__(self, workers: int = 4, duration_s: int = 30):
        self.workers = workers
        self.duration_s = duration_s
        self.procs: list[subprocess.Popen] = []

    def start(self) -> None:
        # Prefer stress-ng if installed (more deterministic load).
        which_sng = subprocess.run(
            ["which", "stress-ng"], capture_output=True, text=True
        )
        if which_sng.returncode == 0:
            self.procs.append(
                subprocess.Popen(
                    ["stress-ng", "--cpu", str(self.workers),
                     "--timeout", f"{self.duration_s}s"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
            )
            return
        # Fallback: spawn tight Python loops.
        for _ in range(self.workers):
            self.procs.append(
                subprocess.Popen(
                    ["python3", "-c",
                     f"import time; t=time.time()+{self.duration_s}\n"
                     f"x=0\nwhile time.time()<t: x=(x+1)%1000003\n"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
            )

    def stop(self) -> None:
        for p in self.procs:
            try:
                p.terminate()
            except Exception:
                pass
        for p in self.procs:
            try:
                p.wait(timeout=2)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
        self.procs.clear()


# ----- Host memory pressure (Tier 11) ------------------------------------
class MemoryPressure:
    """Push large data through the host page cache as a proxy for memory
    pressure (so we can test DMA-phys stability under churn).  Uses the
    OS-native primitive: `dd` on Linux, `fsutil file createnew` on
    Windows.  Either way we then read the file back to actually warm
    the page cache.  Auto-cleans the temp file."""

    def __init__(self, megabytes: int = 4096, path: str | None = None):
        self.megabytes = megabytes
        if path is None:
            tmp = os.environ.get("TEMP") or os.environ.get("TMP") or "/tmp"
            path = os.path.join(tmp, "_v9p_dd_pressure.bin")
        self.path = path
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        if os.name == "nt":
            # fsutil is the closest Windows equivalent — sparse-allocates
            # then we touch the file to force allocation.
            size_bytes = self.megabytes * 1024 * 1024
            try:
                self.proc = subprocess.Popen(
                    ["fsutil", "file", "createnew", self.path, str(size_bytes)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
            except FileNotFoundError:
                # No fsutil either — fall back to a Python writer
                self.proc = subprocess.Popen(
                    ["python", "-c",
                     f"open(r'{self.path}','wb').write(b'\\x00' * {size_bytes})"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
        else:
            self.proc = subprocess.Popen(
                ["dd", "if=/dev/zero", f"of={self.path}",
                 "bs=1M", f"count={self.megabytes}", "conv=fdatasync"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )

    def wait(self, timeout: int = 600) -> None:
        if self.proc is None:
            return
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
            except Exception:
                pass
        try:
            os.unlink(self.path)
        except OSError:
            pass


# ----- Serial-log scraping ------------------------------------------------
def scrape_serial_log(serial_log: str) -> str:
    """Return the serial log as text, stripped of nulls.  Empty string on
    failure."""
    if not os.path.exists(serial_log):
        return ""
    try:
        with open(serial_log, "rb") as f:
            raw = f.read()
        return raw.replace(b"\x00", b"").decode(errors="replace")
    except OSError:
        return ""


def log_contains(serial_log: str, needle: str) -> bool:
    return needle in scrape_serial_log(serial_log)


def log_count(serial_log: str, needle: str) -> int:
    return scrape_serial_log(serial_log).count(needle)
