#!/usr/bin/env python3
"""Materialize the ReXGlue SDK's libmspack symlinks on Windows.

Some ReXGlue thirdparty files (libmspack/cabextract/mspack/*) are git symlinks.
On Windows without symlink support they are checked out as small text files
containing the link target, which breaks the SDK build. This replaces each such
stub with a real copy of its target. Safe to re-run (idempotent).

Usage: python scripts/repair_sdk_symlinks.py [path/to/rexglue-sdk]
"""

import os
import shutil
import sys

REL_DIR = os.path.join("thirdparty", "libmspack", "cabextract", "mspack")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sdk = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo_root, "tools", "rexglue-sdk")
    link_dir = os.path.join(sdk, REL_DIR)

    if not os.path.isdir(link_dir):
        print(f"skip: {link_dir} not present")
        return

    repaired = 0
    for name in os.listdir(link_dir):
        path = os.path.join(link_dir, name)
        if not os.path.isfile(path) or os.path.getsize(path) > 256:
            continue
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read().strip()
        if not text.startswith("../../"):
            continue
        target = os.path.normpath(os.path.join(os.path.dirname(path), text.replace("/", os.sep)))
        if not os.path.isfile(target):
            raise SystemExit(f"symlink target not found for {path}: {target}")
        shutil.copyfile(target, path)
        repaired += 1

    print(f"materialized {repaired} symlink(s) under {REL_DIR}")


if __name__ == "__main__":
    main()
