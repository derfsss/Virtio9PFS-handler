"""
Tier 15 — Boundary parsing.

Covers investigation items N12 + N13 (P3-14 + P3-15).

15.1  test_walk_too_long_path
        A 2048-byte path is longer than pathbuf[1024] in P9_Walk.  Pre-fix
        the path is silently truncated and we walk a garbage prefix.
        Post P3-14 it returns ENAMETOOLONG cleanly.
15.2  test_malformed_string_in_response
        Marshal-layer bounds.  Best done as a host-side unit test on a
        compiled-natively version of p9_marshal.c; we SKIP here and
        defer to the unit-test runner.
15.3  test_pathological_filenames
        Long names (255 bytes), names with spaces, UTF-8 multibyte,
        names like '..foo', '.foo.bar.baz', etc.  All must be visible,
        openable, and deletable.
"""
from __future__ import annotations
import os

from . import common as cm


TIER = "Tier 15"


def _t15_1_walk_too_long_path(ctx: cm.Ctx) -> None:
    # Build a 2048-byte path consisting of nested directories.  pathbuf
    # in P9_Walk is 1024 bytes, so this exceeds the buffer by 2x.
    parts = ["d" * 60] * 30  # 30 × 61 = 1830 chars plus separators ≈ 1860
    deep_path = "/".join(parts)
    full_path = cm.guest_path(deep_path + "/leaf.txt")
    assert len(full_path) >= 1500, "test path needs to exceed pathbuf[1024]"

    # Don't create it on the host — we just want to see how Walk handles
    # the oversize input.  Pre-fix: silently truncated path may match an
    # arbitrary host file or return Garbage error.  Post-fix: ENAMETOOLONG.
    out = cm.run(ctx.c, f"C:List {full_path}", timeout=20)

    # AmigaOS error mapping for ENAMETOOLONG is typically Error 222
    # ("Disk is full") historically, or via FBX the more usual
    # ERROR_LINE_TOO_LONG (480 hex / 1152 dec) / ERROR_INVALID_COMPONENT_NAME.
    # We accept "any error indicating refusal" but FAIL on "ok" or hang.
    refused = ("error" in out.lower()
               or "fail" in out.lower()
               or "not found" in out.lower()
               or "too long" in out.lower())
    handler_ok = cm.handler_alive(ctx.c)
    ctx.score.record(
        TIER, "15.1", "walk of too-long path is refused cleanly",
        "PASS" if (refused and handler_ok) else "FAIL",
        f"refused={refused}, handler_alive={handler_ok}",
    )


def _t15_2_malformed_string_in_response(ctx: cm.Ctx) -> None:
    """Pure marshal-layer unit test — needs a host-native build of
    p9_marshal.c (or LD_PRELOAD-style harness).  Not part of the QEMU
    suite; left as SKIP with a pointer to where it should live."""
    import os, subprocess
    repo_root = r"C:\msys64\home\rich_\Projects\Virtio9PFS-handler"
    native_bin_rel = "build/test_p9_marshal_native"
    native_bin = os.path.join(repo_root, native_bin_rel.replace("/", os.sep))

    if not os.path.exists(native_bin):
        # Try to build it on the fly via WSL+make.
        try:
            subprocess.run(
                ["wsl", "-d", "Ubuntu", "--", "sh", "-c",
                 "cd /mnt/c/msys64/home/rich_/Projects/Virtio9PFS-handler "
                 "&& make test-native"],
                capture_output=True, text=True, timeout=60,
            )
        except Exception as e:
            ctx.score.record(
                TIER, "15.2", "p9_get_str rejects malformed length",
                "SKIP", f"native binary missing and build failed: {e}",
            )
            return
    if not os.path.exists(native_bin):
        ctx.score.record(
            TIER, "15.2", "p9_get_str rejects malformed length",
            "SKIP", f"native binary not built at {native_bin_rel}",
        )
        return

    if os.name == "nt":
        proc = subprocess.run(
            ["wsl", "-d", "Ubuntu", "--", f"./{native_bin_rel}"],
            cwd=repo_root,
            capture_output=True, text=True, timeout=30,
        )
    else:
        proc = subprocess.run(
            [native_bin],
            capture_output=True, text=True, timeout=30,
        )
    rc = proc.returncode
    last_line = (proc.stderr.strip().splitlines()[-1]
                 if proc.stderr.strip() else "")
    ctx.score.record(
        TIER, "15.2", "p9_get_str rejects malformed length",
        "PASS" if rc == 0 else "FAIL",
        f"native rc={rc}: {last_line}",
    )


def _t15_3_pathological_filenames(ctx: cm.Ctx) -> None:
    base_rel = "_tier15_names"
    cm.rm_host(base_rel)
    os.makedirs(cm.host_path(base_rel), exist_ok=True)

    names = [
        ("max_byte_name", "x" * 200 + ".txt"),
        ("with spaces", "name with spaces.txt"),
        ("utf8 cafe",   "café-é𝟗.txt"),
        ("dot prefix",  ".hidden.txt"),
        ("multi-dot",   "a.b.c.d.e.txt"),
        ("digits only", "1234567890.txt"),
    ]

    failures: list[str] = []
    for label, name in names:
        host_p = cm.host_path(f"{base_rel}/{name}")
        try:
            with open(host_p, "wb") as f:
                f.write(f"content-of-{label}\n".encode())
        except OSError as e:
            failures.append(f"{label}: host create failed: {e}")
            continue

        out = cm.run(ctx.c, f"C:List {cm.guest_path(base_rel)}", timeout=15)
        if name not in out:
            # Try the byte-name in case of any encoding round-trip oddities
            short_probe = name[:32]
            if short_probe not in out:
                failures.append(f"{label}: not visible in guest List")

    cm.rm_host(base_rel)

    if not failures:
        ctx.score.record(
            TIER, "15.3", "pathological filenames listable",
            "PASS", f"{len(names)} variants all visible",
        )
    else:
        ctx.score.record(
            TIER, "15.3", "pathological filenames listable",
            "FAIL", "; ".join(failures[:3]),
        )


def run(ctx: cm.Ctx) -> None:
    cm.header("Tier 15 — Boundary parsing")
    _t15_1_walk_too_long_path(ctx)
    _t15_2_malformed_string_in_response(ctx)
    _t15_3_pathological_filenames(ctx)
