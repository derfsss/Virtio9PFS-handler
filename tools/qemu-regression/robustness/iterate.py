#!/usr/bin/env python3
"""
iterate.py — single fix-iteration cycle, pure-Python replacement for
iterate.ps1.  Works around the Windows PowerShell auto-mode classifier
and removes a chicken-and-egg in install: the new handler binary is
uploaded directly to L: over the SerialShell TCP connection, so the
install pass does NOT need a working SHARED: mount.

Per-iteration cycle:
  1. Stop any leftover OUR QEMU (pidfile-scoped — never broad sweep).
  2. Launch QEMU.  Wait for SerialShell handshake.
  3. Upload build/Virtio9PFS-handler.debug -> L:Virtio9PFS-handler via
     SerialShell c.upload_file (bypasses SHARED:).
  4. Update; Dismount SHARED:; Run Mount SHARED:; poll Info SHARED:.
  5. Run robustness/runner with --skip-base.
  6. QMP quit.
  7. Final pidfile-scoped kill in case anything is still alive.

Usage:
    python iterate.py [--tiers 9,12] [--no-install] [--keep-running]
                      [--install-src PATH] [--boot-timeout SEC]
                      [--no-runner]

Returns the runner's exit code: 0 all green, 1 any FAIL, 2 setup error.
"""
from __future__ import annotations
import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT   = Path(r"C:\msys64\home\rich_\Projects\Virtio9PFS-handler")
SHARE_ROOT  = r"S:\temp"
PID_FILE    = r"C:\Users\rich_\.kyvos\kyvos.iterate-base_a1.pid"
SERIAL_PORT = 4322
QMP_PORT    = 14322
LOG_DIR     = REPO_ROOT / "tools" / "qemu-regression" / "robustness" / ".runs"
QEMU_EXE    = r"E:\Emulators\QEMU\QEMU_Install\qemu-system-ppc.exe"
QCOW_HD0    = r"E:\Emulators\QEMU\QEMU_Machines\backup-base_a1_copy\hd0.qcow2"
QCOW_WORK   = r"E:\Emulators\QEMU\QEMU_Machines\work.qcow2"
KICK_ZIP    = r"E:\Emulators\QEMU\QEMU_Machines\backup-base_a1_copy\kickstart.zip"
BBOOT       = r"C:\Users\rich_\.kyvos\bboot"

# Make `from common import SerialClient` and `from runner import main` work.
sys.path.insert(0, str(REPO_ROOT / "tools" / "qemu-regression"))
sys.path.insert(0, str(REPO_ROOT / "tools" / "qemu-regression" / "robustness"))
from common import SerialClient                    # noqa: E402
from robustness import common as cm                # noqa: E402
from robustness import runner as rn                # noqa: E402


# ---------- helpers ---------------------------------------------------------
def _our_pid() -> int | None:
    """Read pidfile and confirm the PID is qemu-system-ppc; return None if
    no live OUR-qemu process."""
    try:
        with open(PID_FILE, "r") as f:
            pid = int(f.read().strip())
    except (OSError, ValueError):
        return None
    # Use tasklist to verify; cross-platform-friendly.
    try:
        r = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=10,
        )
        if "qemu-system-ppc" in r.stdout:
            return pid
    except Exception:
        pass
    return None


def _stop_our_qemu_hard() -> None:
    pid = _our_pid()
    if pid:
        print(f"[iterate]   killing OUR qemu pid {pid} (per pidfile)", flush=True)
        try:
            subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                           capture_output=True, timeout=10)
        except Exception:
            pass
    try:
        os.unlink(PID_FILE)
    except OSError:
        pass
    time.sleep(2)


def _stop_our_qemu_qmp() -> None:
    """Send QMP quit, then wait for OUR qemu to exit; fall back to hard kill."""
    try:
        s = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        f = s.makefile("rwb", buffering=0)
        f.readline()                                          # greeting
        f.write(b'{"execute":"qmp_capabilities"}\r\n')
        f.readline()
        f.write(b'{"execute":"quit"}\r\n')
        try:
            f.readline()
        except Exception:
            pass
        s.close()
        print("[iterate]   QMP quit sent", flush=True)
    except Exception as e:
        print(f"[iterate]   QMP quit failed: {e}", flush=True)
    deadline = time.time() + 15
    while time.time() < deadline and _our_pid():
        time.sleep(1)
    _stop_our_qemu_hard()


def _qemu_cmdline(serial_log: Path) -> list[str]:
    return [
        QEMU_EXE,
        "-M", "amigaone",
        "-kernel", BBOOT,
        "-device", f"loader,addr=0x600000,file={KICK_ZIP}",
        "-rtc", "base=localtime",
        "-accel", "tcg",
        "-vga", "none",
        "-chardev", f"file,id=charserial0,path={serial_log}",
        "-serial", "chardev:charserial0",
        "-device", "rtl8139,addr=0x0a,netdev=nic",
        "-netdev", f"user,id=nic,hostname=backup-base_a1_copy,hostfwd=tcp::{SERIAL_PORT}-:4321",
        "-append", "serial debuglevel=1",
        "-display", "sdl,gl=off,show-cursor=off,full-screen=off",
        "-device", "es1370,addr=0x09",
        "-name", "backup-base_a1_copy",
        "-m", "2048M",
        "-drive", "if=none,id=cd",
        "-device", "ide-cd,unit=1,drive=cd,bus=ide.1",
        "-device", "sm501",
        "-pidfile", PID_FILE,
        "-device", "virtio-scsi-pci,id=scsi0",
        "-drive", f"file={QCOW_HD0},if=none,id=vd0,format=qcow2,cache=writethrough",
        "-device", "scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0",
        "-drive", f"file={QCOW_WORK},if=none,id=vd1,format=qcow2,cache=writethrough",
        "-device", "scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=0",
        "-fsdev", f"local,security_model=mapped-xattr,id=fsdev0,path={SHARE_ROOT}",
        "-device", "virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=SHARED",
        "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server=on,wait=off",
    ]


def _wait_serialshell(timeout_sec: int) -> SerialClient | None:
    """Probe with a real Echo READY handshake; retry until we get a response
    or timeout.  Returns an open SerialClient on success."""
    deadline = time.time() + timeout_sec
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        try:
            c = SerialClient("127.0.0.1", SERIAL_PORT)
            c.connect(timeout=8)
            out = c.send_command("Echo READY", timeout=8)
            if "READY" in out:
                print(f"[iterate]   SerialShell up after {attempt} attempts",
                      flush=True)
                return c
            c.close()
        except Exception:
            pass
        time.sleep(3)
    return None


def _install_handler(c: SerialClient, src: Path) -> bool:
    """Upload the new handler binary to L: directly over SerialShell (no
    SHARED: dependency), then Dismount + Run Mount and poll for the new
    handler to come up."""
    if not src.exists():
        print(f"[iterate] handler binary not found: {src}", flush=True)
        return False

    print(f"[iterate] uploading {src.name} -> L:Virtio9PFS-handler "
          f"({src.stat().st_size} bytes)", flush=True)
    try:
        c.upload_file(str(src), "L:Virtio9PFS-handler")
    except Exception as e:
        print(f"[iterate] upload failed: {e}", flush=True)
        return False
    print("  verify:",
          c.send_command("C:List L:Virtio9PFS-handler", timeout=10)
           .strip().splitlines()[-1], flush=True)

    # Flush DOS buffers; Dismount; remount.
    c.send_command("Update", timeout=30)
    out = c.send_command("C:Dismount SHARED:", timeout=20)
    print("  dismount:", repr(out.strip())[:80], flush=True)
    time.sleep(2)
    out = c.send_command("C:Run >NIL: <NIL: C:Mount SHARED:", timeout=10)
    print("  mount:", repr(out.strip())[:80], flush=True)

    for attempt in range(20):
        time.sleep(2)
        try:
            out = c.send_command("C:Info SHARED:", timeout=10)
        except Exception as e:
            print(f"  poll {attempt+1}: exc {e}", flush=True)
            continue
        # Want "9PFP" + "Mounted" and NOT "No disk present"
        if "9PFP" in out and "Mounted" in out and "No disk" not in out:
            print(f"  SHARED: mounted (poll {attempt+1})", flush=True)
            return True
    print("[iterate] SHARED: did not come back online after Mount",
          flush=True)
    return False


# ---------- main ------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", default="",
                    help="Comma list of new tier IDs, e.g. 9,12")
    ap.add_argument("--no-install", action="store_true",
                    help="Skip upload+remount, use whatever's in L:")
    ap.add_argument("--keep-running", action="store_true",
                    help="Don't QMP quit at the end (debugging)")
    ap.add_argument("--install-src",
                    default=str(REPO_ROOT / "build" /
                                "Virtio9PFS-handler.debug"),
                    help="Local path to the handler binary to install")
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--no-runner", action="store_true",
                    help="Skip the runner — just install + return")
    args = ap.parse_args(argv)

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    serial_log = LOG_DIR / f"serial-{stamp}.log"

    # Always start clean
    _stop_our_qemu_hard()

    # Launch QEMU detached.  CREATE_NEW_PROCESS_GROUP keeps Ctrl-C in this
    # shell from cascading into qemu (we want QMP quit instead).
    print(f"[iterate] launching QEMU (serial -> {serial_log})", flush=True)
    creationflags = 0x00000200 if os.name == "nt" else 0  # CREATE_NEW_PROCESS_GROUP
    qproc = subprocess.Popen(
        _qemu_cmdline(serial_log),
        creationflags=creationflags,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    print(f"[iterate]   qemu pid={qproc.pid}", flush=True)

    rc = 0
    try:
        c = _wait_serialshell(args.boot_timeout)
        if c is None:
            print("[iterate] SerialShell never opened — aborting", flush=True)
            return 2

        try:
            if not args.no_install:
                if not _install_handler(c, Path(args.install_src)):
                    return 2
        finally:
            try:
                c.close()
            except Exception:
                pass

        if args.no_runner:
            print("[iterate] --no-runner — install done, returning",
                  flush=True)
            return 0

        # Runner gets its own SerialClient
        cm.P9_SHARE_HOST = SHARE_ROOT
        runner_args = [
            "--skip-base",
            "--port", str(SERIAL_PORT),
            "--p9-share", SHARE_ROOT,
            "--qmp", f"tcp:127.0.0.1:{QMP_PORT}",
            "--serial-log", str(serial_log),
        ]
        if args.tiers:
            runner_args += ["--tiers", args.tiers]
        print(f"[iterate] runner args: {runner_args}", flush=True)
        rc = rn.main(runner_args)
        print(f"[iterate]   runner rc={rc}", flush=True)

    finally:
        if not args.keep_running:
            print("[iterate] QMP quit...", flush=True)
            _stop_our_qemu_qmp()
        else:
            print(f"[iterate] --keep-running: leaving QEMU alive (pid={qproc.pid})",
                  flush=True)

    print(f"[iterate] DONE rc={rc}  (logs in {LOG_DIR})", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
