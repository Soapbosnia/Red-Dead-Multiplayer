# Remote-player sync report from two game copies on one PC ([debug] net_trace=1, RDMP.exe --instance 2).
# Both copies stamp their traces with the same QPC clock, so a remote puppet in one trace can be compared with the
# real player in the other at the same instant.
#
#   python tools/sync_report.py [build/bin] [--from SECONDS] [--to SECONDS]
#
# For each copy's remotes: the matching player's truth is the L rows of the other trace (the one remote id per copy).
# Prints horizontal / vertical error of the puppet vs truth at the same instant, error of the network target, how far
# frames the soft pull moved the puppet while the remote moved (feet slide then), teleports.
import csv
import math
import os
import statistics
import sys


def load(path):
    local, remote = [], {}
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row:
                continue
            try:
                if row[0] == "L":
                    local.append((float(row[1]), float(row[2]), float(row[3]), float(row[4]), float(row[5]), float(row[6])))
                elif row[0] == "R" and len(row) >= 15:
                    rid = int(row[2])
                    remote.setdefault(rid, []).append({
                        "t": float(row[1]), "tx": float(row[3]), "ty": float(row[4]), "tz": float(row[5]),
                        "px": float(row[7]), "py": float(row[8]), "pz": float(row[9]), "ph": float(row[10]),
                        "moving": row[12] == "1", "tp": row[13] == "1", "pulled": row[14] == "1",
                        "extra": row[18:] if len(row) > 18 else [],
                    })
            except ValueError:
                continue
    return local, remote


def truth_at(local, t, cache={}):
    # binary search the L rows (sorted by time), linear interpolation
    lo, hi = 0, len(local) - 1
    if t <= local[0][0] or t >= local[-1][0]:
        return None
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if local[mid][0] <= t:
            lo = mid
        else:
            hi = mid
    a, b = local[lo], local[hi]
    f = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0.0
    return tuple(a[i] + (b[i] - a[i]) * f for i in range(1, 6))


def pct(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p))]


def report(name, rows, truth, t0, t_from, t_to):
    herr_m, herr_s, verr, terr, glide, tps = [], [], [], [], [], 0
    along, side = [], []
    worst = []
    prev = None
    for r in rows:
        rel = (r["t"] - t0) / 1000.0
        if rel < t_from or rel > t_to:
            continue
        tr = truth_at(truth, r["t"])
        if tr is None:
            continue
        x, y, z, h, speed = tr
        he = math.hypot(r["px"] - x, r["pz"] - z)
        ve = r["py"] - y
        te = math.hypot(r["tx"] - x, r["tz"] - z)
        (herr_m if speed > 0.3 else herr_s).append(he)
        if speed > 0.3:
            # split along the truth's motion (+ = puppet behind) and sideways
            t2 = truth_at(truth, r["t"] - 50.0)
            if t2 is not None:
                mx, mz = x - t2[0], z - t2[2]
                ml = math.hypot(mx, mz)
                if ml > 1e-3:
                    dx, dz = x - r["px"], z - r["pz"]
                    along.append((dx * mx + dz * mz) / ml)
                    side.append(abs((dx * mz - dz * mx) / ml))
        verr.append(abs(ve))
        terr.append(te)
        if r["tp"]:
            tps += 1
        if speed > 0.3:
            glide.append(1.0 if r["pulled"] else 0.0)  # moving frames where the pull, not the animation, moved it
        worst.append((he, abs(ve), rel, speed, r["moving"]))
        prev = r
    if not worst:
        print(f"{name}: no overlapping samples")
        return
    print(f"== {name}: {len(worst)} frames")
    if herr_m:
        print(f"  moving   horiz err  mean {statistics.mean(herr_m):.3f}  p50 {pct(herr_m, .5):.3f}  p95 {pct(herr_m, .95):.3f}  max {max(herr_m):.3f} m")
    if along:
        print(f"  moving   along (+ behind) mean {statistics.mean(along):+.3f}  sideways mean {statistics.mean(side):.3f}  p95 {pct(side, .95):.3f} m")
    if herr_s:
        print(f"  standing horiz err  mean {statistics.mean(herr_s):.3f}  p50 {pct(herr_s, .5):.3f}  p95 {pct(herr_s, .95):.3f}  max {max(herr_s):.3f} m")
    # the same, against the truth shifted by the best-fitting delay (the puppet shows the remote that much late)
    best = None
    for d in range(0, 201, 5):
        errs = []
        for r in rows[::3]:
            rel = (r["t"] - t0) / 1000.0
            if rel < t_from or rel > t_to:
                continue
            tr = truth_at(truth, r["t"] - d)
            if tr is None or tr[4] <= 0.3:
                continue
            errs.append(math.hypot(r["px"] - tr[0], r["pz"] - tr[2]))
        if errs and (best is None or statistics.mean(errs) < best[1]):
            best = (d, statistics.mean(errs), pct(errs, .95))
    if best:
        print(f"  moving, {best[0]} ms late: horiz err mean {best[1]:.3f}  p95 {best[2]:.3f} m")
    print(f"  vertical err        mean {statistics.mean(verr):.3f}  p95 {pct(verr, .95):.3f}  max {max(verr):.3f} m")
    print(f"  network target err  mean {statistics.mean(terr):.3f}  p95 {pct(terr, .95):.3f} m")
    if glide:
        print(f"  moving frames pulled (feet slide) {statistics.mean(glide) * 100:.1f}%")
    print(f"  teleports {tps}")
    worst.sort(reverse=True)
    print("  worst: " + ", ".join(f"{w[0]:.2f}m/dy{w[1]:.2f}@{w[2]:.1f}s(v{w[3]:.1f})" for w in worst[:6]))


def main():
    args = sys.argv[1:]
    folder = args[0] if args and not args[0].startswith("--") else os.path.join(os.path.dirname(__file__), "..", "build", "bin")
    t_from = float(args[args.index("--from") + 1]) if "--from" in args else 0.0
    t_to = float(args[args.index("--to") + 1]) if "--to" in args else 1e9
    a = load(os.path.join(folder, "rdmp_trace.csv"))
    b = load(os.path.join(folder, "rdmp_trace_2.csv"))
    t0 = min(x[0][0][0] for x in (a, b) if x[0])
    for name, (_, remotes), (truth, _) in (("copy 1 sees copy 2", a, b), ("copy 2 sees copy 1", b, a)):
        if not truth:
            continue
        for rid, rows in remotes.items():
            report(f"{name} (remote {rid})", rows, truth, t0, t_from, t_to)


if __name__ == "__main__":
    main()
