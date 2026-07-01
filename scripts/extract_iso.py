#!/usr/bin/env python3
"""Extract Army of Two game files from your own Xbox 360 disc image.

Xbox 360 discs use XDVDFS (the Xbox DVD File System). This script locates the
game partition inside a decrypted retail ISO and extracts its files, producing
the ``game/`` directory this project expects (with ``game/default.xex`` at the
root). It uses only the Python standard library.

Usage:
    python scripts/extract_iso.py [path/to/game.iso] [output_dir]

If no ISO path is given, the first ``*.iso`` found under ``iso/`` (searched
recursively) is used. The default output directory is ``game/``.

You must supply your own legally obtained disc image. This project ships no game
data. If the ISO is still encrypted (no XDVDFS partition is found), decrypt/redump
it first, or use extract-xiso: https://github.com/XboxDev/extract-xiso
"""

import os
import struct
import sys

MAGIC = b'MICROSOFT*XBOX*MEDIA'
SECTOR_SIZE = 2048

# Known XDVDFS partition base offsets for the common disc formats.
KNOWN_OFFSETS = [
    0x0FD90000,  # XGD2 standard
    0x0FDA0000,  # XGD2 variant
    0x02080000,  # XGD1
    0x00000000,  # start of file (already-extracted / XBLA image)
]


def find_partition(f):
    """Return the XDVDFS partition base offset, or None if not found."""
    for off in KNOWN_OFFSETS:
        f.seek(off)
        if f.read(20) != MAGIC:
            continue
        f.seek(off + 20)
        root_sector = struct.unpack('<I', f.read(4))[0]
        root_size = struct.unpack('<I', f.read(4))[0]
        adjusted_base = off - 0x10000
        if adjusted_base >= 0 and root_sector > 0 and root_size > 0:
            test_off = adjusted_base + root_sector * SECTOR_SIZE
            fsize = f.seek(0, 2)
            if test_off + root_size <= fsize:
                f.seek(test_off)
                if any(b != 0 for b in f.read(min(64, root_size))):
                    return adjusted_base
        return off

    print("Scanning for XDVDFS partition...")
    f.seek(0)
    chunk_size = 1024 * 1024
    pos = 0
    while pos < 0x20000000:
        f.seek(pos)
        data = f.read(chunk_size)
        idx = data.find(MAGIC)
        if idx >= 0:
            magic_off = pos + idx
            adjusted = magic_off - 0x10000
            if adjusted >= 0:
                f.seek(magic_off + 20)
                rs = struct.unpack('<I', f.read(4))[0]
                rsz = struct.unpack('<I', f.read(4))[0]
                test_off = adjusted + rs * SECTOR_SIZE
                fsize = f.seek(0, 2)
                if test_off + rsz <= fsize:
                    f.seek(test_off)
                    if any(b != 0 for b in f.read(min(64, rsz))):
                        return adjusted
            return magic_off
        pos += chunk_size - 20
    return None


def parse_dir(f, sector, size, base_offset):
    """Parse an XDVDFS directory (a sector-packed binary tree of entries)."""
    entries = []
    f.seek(base_offset + sector * SECTOR_SIZE)
    data = f.read(size)

    stack = [0]
    visited = set()
    while stack:
        offset = stack.pop()
        if offset in visited or offset + 14 > len(data):
            continue
        visited.add(offset)

        left = struct.unpack_from('<H', data, offset)[0]
        right = struct.unpack_from('<H', data, offset + 2)[0]
        start = struct.unpack_from('<I', data, offset + 4)[0]
        file_size = struct.unpack_from('<I', data, offset + 8)[0]
        attrs = data[offset + 12]
        name_len = data[offset + 13]

        if name_len == 0 or offset + 14 + name_len > len(data):
            continue
        name = data[offset + 14:offset + 14 + name_len]
        if not all(32 <= b < 127 for b in name):
            print("WARNING: non-ASCII directory name; the image may be encrypted.")
            return []

        entries.append({
            'name': name.decode('ascii'),
            'sector': start,
            'size': file_size,
            'is_dir': bool(attrs & 0x10),
        })
        if left and left * 4 < len(data):
            stack.append(left * 4)
        if right and right * 4 < len(data):
            stack.append(right * 4)
    return entries


def extract_tree(f, sector, size, base_offset, out_dir, prefix=""):
    """Recursively extract files from an XDVDFS directory."""
    count = 0
    for e in parse_dir(f, sector, size, base_offset):
        rel = os.path.join(prefix, e['name'])
        out_path = os.path.join(out_dir, rel)
        if e['is_dir']:
            os.makedirs(out_path, exist_ok=True)
            count += extract_tree(f, e['sector'], e['size'], base_offset, out_dir, rel)
        else:
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            f.seek(base_offset + e['sector'] * SECTOR_SIZE)
            remaining = e['size']
            with open(out_path, 'wb') as out_f:
                while remaining > 0:
                    chunk = f.read(min(remaining, 4 * 1024 * 1024))
                    if not chunk:
                        break
                    out_f.write(chunk)
                    remaining -= len(chunk)
            count += 1
    return count


def autodetect_iso(repo_root):
    iso_dir = os.path.join(repo_root, "iso")
    if os.path.isdir(iso_dir):
        for root, _dirs, files in os.walk(iso_dir):
            for name in sorted(files):
                if name.lower().endswith(".iso"):
                    return os.path.join(root, name)
    return None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    iso_path = sys.argv[1] if len(sys.argv) > 1 else autodetect_iso(repo_root)
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(repo_root, "game")

    if not iso_path:
        print(__doc__)
        print("ERROR: no ISO given and none found under iso/. Drop your disc image in iso/.")
        sys.exit(1)
    if not os.path.isfile(iso_path):
        print(f"ERROR: ISO not found: {iso_path}")
        sys.exit(1)

    print(f"ISO:    {iso_path}")
    print(f"Output: {out_dir}")

    with open(iso_path, 'rb') as f:
        base = find_partition(f)
        if base is None:
            print("\nERROR: no XDVDFS partition found. The image may be encrypted.")
            print("Decrypt/redump it, or use extract-xiso:")
            print("  https://github.com/XboxDev/extract-xiso")
            sys.exit(1)

        print(f"XDVDFS partition at 0x{base:08X}")
        f.seek(base + 0x10000)
        if f.read(20) == MAGIC:
            f.seek(base + 0x10000 + 20)
        else:
            f.seek(base + 20)
        root_sector = struct.unpack('<I', f.read(4))[0]
        root_size = struct.unpack('<I', f.read(4))[0]

        os.makedirs(out_dir, exist_ok=True)
        print("Extracting (this can take a few minutes)...")
        count = extract_tree(f, root_sector, root_size, base, out_dir)

    if count == 0:
        print("\nERROR: no files extracted; the image may be encrypted.")
        sys.exit(1)

    xex = os.path.join(out_dir, "default.xex")
    if not os.path.isfile(xex):
        print(f"\nWARNING: extracted {count} files but {xex} is missing.")
        print("This may not be an Army of Two disc image.")
        sys.exit(1)

    print(f"\nExtracted {count} files to {out_dir}/")
    print(f"Found {xex} ({os.path.getsize(xex):,} bytes) - ready to build.")


if __name__ == "__main__":
    main()
