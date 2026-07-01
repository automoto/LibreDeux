#!/usr/bin/env python3
"""Generate recompiled C++ for Army of Two, then apply source workarounds.

Runs ReXGlue codegen against config/aot_manifest.toml (producing
generated/default/), then applies the AoT-specific patches via
apply_workarounds.py. Run this once after extracting your game and building the
SDK, before configuring with CMake.

Usage: python scripts/codegen.py
"""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    rexglue = os.path.join(REPO, "tools", "rexglue-sdk", "out", "install",
                           "win-amd64", "bin", "rexglue.exe")
    manifest = os.path.join(REPO, "config", "aot_manifest.toml")

    if not os.path.isfile(rexglue):
        sys.exit(f"ReXGlue not found: {rexglue}\n"
                 "Build the SDK first (see README, step 3).")
    if not os.path.isfile(os.path.join(REPO, "game", "default.xex")):
        sys.exit("game/default.xex missing. Extract your game first: "
                 "python scripts/extract_iso.py")

    print("Running ReXGlue codegen...")
    if subprocess.run([rexglue, "codegen", manifest], cwd=REPO).returncode != 0:
        sys.exit("ReXGlue codegen failed.")

    print("Applying Army of Two workarounds...")
    subprocess.run([sys.executable, os.path.join(REPO, "scripts", "apply_workarounds.py")],
                   check=True)

    print("Codegen complete. Now configure and build with CMake (see README).")


if __name__ == "__main__":
    main()
