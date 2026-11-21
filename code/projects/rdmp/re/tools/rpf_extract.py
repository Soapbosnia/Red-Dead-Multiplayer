#!/usr/bin/env python3
"""Extract RPF6 entries that are plain zstd frames.

RPF6 payloads on the PC port come in two kinds:
  - zstd frames (magic 28 B5 2F FD) - extracted here
  - XCompress/LZX streams (no container magic) - not supported yet

Usage:
    python rpf_extract.py <archive.rpf> <outdir> [--all]
"""

from __future__ import annotations

import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zstandard as zstd

from rpf6 import RPF6


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    r = RPF6(path)
    rows = []
    ok = 0
    for e in r.entries:
        if e.is_dir or e.size == 0:
            continue
        data = r.read(e)
        status = "raw"
        out = data
        if data[:4] == b"\x28\xb5\x2f\xfd":
            try:
                out = zstd.ZstdDecompressor().decompress(data, max_output_size=256 * 1024 * 1024)
                status = "zstd"
                ok += 1
            except Exception as exc:
                status = f"zstd-error:{exc}"
        rows.append((e.index, f"{e.name_hash:08X}", e.size, len(out), status))
        if status in ("zstd", "raw"):
            with open(os.path.join(outdir, f"{e.index:04d}_{e.name_hash:08X}.bin"), "wb") as fh:
                fh.write(out)

    with open(os.path.join(outdir, "manifest.csv"), "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["index", "name_hash", "size_in_archive", "size_out", "status"])
        w.writerows(rows)
    print(f"{os.path.basename(path)}: {len(rows)} files, {ok} zstd extracted -> {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
