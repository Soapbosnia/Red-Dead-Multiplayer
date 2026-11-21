import re
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zstandard as zstd
import zlib

from rpf6 import RPF6


def joaat(s: str) -> int:
    h = 0
    for ch in s.lower():
        h = (h + ord(ch)) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


r = RPF6(os.path.join(os.environ.get("RDR_DIR", "."), "game", "flash.rpf"))
by_hash = {e.name_hash: e for e in r.entries}

# pull every name-ish value out of the manifest entry
manifest = None
for e in r.entries:
    if e.is_dir:
        continue
    d = r.read(e)
    if d[:4] == b"\x28\xb5\x2f\xfd":
        try:
            out = zstd.ZstdDecompressor().decompress(d, max_output_size=64 * 1024 * 1024)
        except Exception:
            continue
        if b"<sagHudData>" in out or b"<Name>" in out:
            manifest = out.decode("utf-8", "replace")
            break

names = set()
if manifest:
    for m in re.finditer(r"<(?:Name|FileName|PathAndFilename)>(?:value=\"([^\"]+)\"|([^<]+))<", manifest):
        names.add((m.group(1) or m.group(2)).strip())

print("manifest names:", len(names))
hits = 0
for n in sorted(names):
    for cand in (n, n + ".gfx", n.lower(), n.lower() + ".gfx"):
        e = by_hash.get(joaat(cand))
        if e is not None:
            print(f"  {n!r:34} -> entry {e.index:3d} hash=0x{e.name_hash:08X} size={e.size} dir={e.is_dir}")
            hits += 1
            break
print("matched", hits)
