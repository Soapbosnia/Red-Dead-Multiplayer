import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zstandard as zstd

from rpf6 import RPF6

game = os.path.join(os.environ.get("RDR_DIR", "."), "game")
rows = []
total_files = 0
total_zstd = 0
for path in sorted(glob.glob(os.path.join(game, "*.rpf"))):
    try:
        r = RPF6(path)
    except Exception as exc:
        rows.append((os.path.basename(path), "ERR", str(exc)[:40]))
        continue
    files = [e for e in r.entries if not e.is_dir and e.size > 0]
    z = 0
    for e in files:
        head = r.read(e)[:4]
        if head == b"\x28\xb5\x2f\xfd":
            z += 1
    total_files += len(files)
    total_zstd += z
    rows.append((os.path.basename(path), len(files), z))

print(f"{'archive':28} {'files':>6} {'zstd':>6}")
for name, n, z in rows:
    print(f"{name:28} {str(n):>6} {str(z):>6}")
print(f"{'TOTAL':28} {total_files:>6} {total_zstd:>6}")
print(f"zstd share: {100.0*total_zstd/max(total_files,1):.1f}%")
