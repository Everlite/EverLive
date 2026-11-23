#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reject binary-patching helpers and compiled artifacts in the source tree.

Run without arguments inside a git checkout to inspect the tracked file list,
or pass ``--tree DIR`` to inspect an extracted release archive::

    python3 tools/check_tree_hygiene.py
    python3 tools/check_tree_hygiene.py --tree /tmp/everlive-archive

The check fails when a file is a backup/patch leftover, a compiled binary or
library, an executable image, or a script that carries the x86-64 stack-canary
patterns used by the removed patching helpers. Tracked assets such as PNG icons
and TTF fonts are intentionally allowed.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import subprocess
import sys
from pathlib import Path

SELF_PATH = "tools/check_tree_hygiene.py"

# Backup/patch leftovers and compiled artifacts. Build outputs are covered by
# .gitignore; this catches anything that was force-added or slipped in.
FORBIDDEN_SUFFIXES = (
    ".bak",
    ".orig",
    ".rej",
    ".o",
    ".a",
    ".so",
    ".dylib",
    ".ko",
    ".dll",
    ".exe",
)
FORBIDDEN_GLOBS = ("*.so.*", "*.bak.*", "*.orig.*", "*.rej.*")

# Executable image magic. A "patched library" renamed without an extension is
# caught here even though its path looks harmless.
ELF_MAGIC = b"\x7fELF"
MACHO_MAGICS = (
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
)

# Text sources that could hide a renamed patcher/scan tool. The markers are
# detection signatures, not instructions for reproducing the patch.
TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".go",
    ".h",
    ".js",
    ".mk",
    ".pl",
    ".py",
    ".rb",
    ".rs",
    ".sh",
    ".bash",
    ".zsh",
    ".ts",
}
TEXT_BASENAMES = {"Makefile", "Kbuild"}
CANARY_MARKERS = ("gs:0x28", "\\x65\\x48\\x8b\\x04\\x25")
MAX_SCAN_BYTES = 2 * 1024 * 1024


def _git_root() -> Path:
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"error: not inside a git checkout: {exc}") from exc
    return Path(out.stdout.strip())


def _tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    return [item.decode("utf-8", "surrogateescape") for item in out.stdout.split(b"\0") if item]


def _tree_files(root: Path) -> list[str]:
    files: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d != ".git")
        for name in sorted(filenames):
            full = Path(dirpath) / name
            files.append(full.relative_to(root).as_posix())
    return files


def _forbidden_path_reason(rel_path: str) -> str | None:
    lower = rel_path.lower()
    name = os.path.basename(lower)
    for suffix in FORBIDDEN_SUFFIXES:
        if lower.endswith(suffix):
            return f"forbidden file type ({suffix})"
    for pattern in FORBIDDEN_GLOBS:
        if fnmatch.fnmatch(name, pattern):
            return f"forbidden file type ({pattern})"
    return None


def _binary_magic_reason(head: bytes) -> str | None:
    if head.startswith(ELF_MAGIC):
        return "tracked ELF binary/library"
    for magic in MACHO_MAGICS:
        if head.startswith(magic):
            return "tracked Mach-O binary/library"
    if len(head) >= 0x40 and head[:2] == b"MZ":
        offset = int.from_bytes(head[0x3C:0x40], "little")
        if head[offset : offset + 4] == b"PE\0\0":
            return "tracked PE executable"
    return None


def _canary_reason(full_path: Path, rel_path: str) -> str | None:
    if rel_path == SELF_PATH:
        return None
    suffix = full_path.suffix.lower()
    if suffix not in TEXT_SUFFIXES and full_path.name not in TEXT_BASENAMES:
        return None
    try:
        if full_path.stat().st_size > MAX_SCAN_BYTES:
            return None
        text = full_path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    for marker in CANARY_MARKERS:
        if marker in text:
            return f"contains binary-patching marker {marker!r}"
    return None


def _check(root: Path, files: list[str]) -> list[tuple[str, str]]:
    violations: list[tuple[str, str]] = []
    for rel_path in files:
        reason = _forbidden_path_reason(rel_path)
        if reason is not None:
            violations.append((rel_path, reason))
            continue

        full_path = root / rel_path
        if full_path.is_symlink():
            continue
        try:
            with full_path.open("rb") as handle:
                head = handle.read(4096)
        except OSError as exc:
            violations.append((rel_path, f"unreadable: {exc}"))
            continue

        reason = _binary_magic_reason(head)
        if reason is not None:
            violations.append((rel_path, reason))
            continue

        reason = _canary_reason(full_path, rel_path)
        if reason is not None:
            violations.append((rel_path, reason))
    return violations


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--tree",
        metavar="DIR",
        help="scan an extracted source tree instead of the git tracked file list",
    )
    args = parser.parse_args(argv)

    if args.tree:
        root = Path(args.tree).resolve()
        if not root.is_dir():
            parser.error(f"--tree is not a directory: {root}")
        files = _tree_files(root)
        scope = f"tree {root}"
    else:
        root = _git_root()
        files = _tracked_files(root)
        scope = "git tracked files"

    violations = _check(root, files)
    if violations:
        print(f"Release hygiene check failed ({len(violations)} violation(s)):", file=sys.stderr)
        for rel_path, reason in violations:
            print(f"  {rel_path}: {reason}", file=sys.stderr)
        return 1

    print(f"Release hygiene OK: {len(files)} file(s) checked from {scope}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
