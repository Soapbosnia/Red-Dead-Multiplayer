"""syncdiff.py <copy 1 log> <copy 2 log>: how well each player's puppet in the other game copy follows them.

Both copies run with [debug] sync_probe=1 on one PC (one clock): every 100 ms tick each logs its local player ("SP L")
and its puppets ("SP P"). For each direction this pairs the player's real state at tick t with their puppet's state
at t + lag (the lag that fits best) and reports position / heading / aim error and, per state flag, how often the two
disagree.

    python tools/syncdiff.py build/bin/rdmp.log build/bin/rdmp_2.log
"""
import math
import re
import sys

FLAGS = ["on ground", "jumping", "crouching", "cover", "ragdoll", "reacting", "shooting", "reloading", "drawn",
         "holstered", "ledge", "dead", "gait", "posture"]
LINE = re.compile(r"SP ([LP]) (\d+) pos (\S+) (\S+) (\S+) h (\S+) hp (\S+) inhand (-?\d+) aim (\S+) (\S+) (\S+) f (\S+)")


def load(path):
    local, puppet = {}, {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if not m:
            continue
        who, tick = m.group(1), int(m.group(2))
        pos = tuple(float(m.group(i)) for i in (3, 4, 5))
        aim = tuple(float(m.group(i)) for i in (9, 10, 11))
        flags = [int(x) for x in m.group(12).strip(",").split(",")]
        rec = {"pos": pos, "h": float(m.group(6)), "hp": float(m.group(7)), "inhand": int(m.group(8)), "aim": aim,
               "flags": flags}
        (local if who == "L" else puppet)[tick] = rec
    return local, puppet


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def hdiff(a, b):
    d = (a - b) % 360.0
    return d - 360.0 if d > 180.0 else d


def compare(name, real, puppet):
    if not real or not puppet:
        print(f"== {name}: no data")
        return
    best = None
    for lag in range(0, 8):  # ticks of 100 ms
        pairs = [(real[t], puppet[t + lag]) for t in real if t + lag in puppet]
        if len(pairs) < 10:
            continue
        err = sorted(dist(a["pos"], b["pos"]) for a, b in pairs)
        med = err[len(err) // 2]
        if best is None or med < best[0]:
            best = (med, lag, pairs)
    if not best:
        print(f"== {name}: too few matching ticks")
        return
    med, lag, pairs = best
    err = sorted(dist(a["pos"], b["pos"]) for a, b in pairs)
    herr = sorted(abs(hdiff(a["h"], b["h"])) for a, b in pairs)
    dy = sorted(b["pos"][1] - a["pos"][1] for a, b in pairs)
    aerr = sorted(dist(a["aim"], b["aim"]) for a, b in pairs if any(a["aim"]) and any(b["aim"]))
    p95 = lambda xs: xs[min(len(xs) - 1, int(len(xs) * 0.95))] if xs else float("nan")
    print(f"== {name}: {len(pairs)} ticks, best lag {lag * 100} ms")
    print(f"   position  median {err[len(err) // 2]:.2f} m, p95 {p95(err):.2f} m, max {err[-1]:.2f} m")
    print(f"   height    puppet - real: median {dy[len(dy) // 2]:+.2f} m, range {dy[len(dy) // 20]:+.2f} .. {dy[-len(dy) // 20 - 1]:+.2f} m")
    print(f"   heading   median {herr[len(herr) // 2]:.1f} deg, p95 {p95(herr):.1f} deg")
    if aerr:
        print(f"   aim point median {aerr[len(aerr) // 2]:.2f} m, p95 {p95(aerr):.2f} m")
    inhand = sum(1 for a, b in pairs if a["inhand"] != b["inhand"])
    print(f"   weapon in hand differs {100.0 * inhand / len(pairs):.0f}% of the time")
    for i, f in enumerate(FLAGS):
        vals = [(a["flags"][i], b["flags"][i]) for a, b in pairs if len(a["flags"]) > i and len(b["flags"]) > i]
        if not vals or all(x == -9 for x, _ in vals):
            continue
        diff = sum(1 for x, y in vals if x != y)
        real_on = sum(1 for x, _ in vals if x not in (0, -9))
        pup_on = sum(1 for _, y in vals if y not in (0, -9))
        mark = "  <--" if diff * 10 > len(vals) else ""
        print(f"   {f:10s} differs {100.0 * diff / len(vals):3.0f}%  (real on {real_on}, puppet on {pup_on} of {len(vals)}){mark}")


NLINE = re.compile(r"SP N (\d+) pos (\S+) (\S+) (\S+)")


def load_net(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = NLINE.search(line)
        if m:
            out[int(m.group(1))] = tuple(float(m.group(i)) for i in (2, 3, 4))
    return out


def lag_of(real, other, name):
    best = None
    for lag in range(0, 8):
        errs = sorted(dist(real[t]["pos"], other[t + lag]) for t in real if t + lag in other)
        if len(errs) >= 10 and (best is None or errs[len(errs) // 2] < best[0]):
            best = (errs[len(errs) // 2], lag)
    if best:
        print(f"   {name}: best lag {best[1] * 100} ms (median {best[0]:.2f} m)")


def main():
    l1, p1 = load(sys.argv[1])
    l2, p2 = load(sys.argv[2])
    compare("copy 1's player -> its puppet in copy 2", l1, p2)
    n2 = load_net(sys.argv[2])
    if n2:
        lag_of(l1, n2, "network (copy 1's player -> newest state received by copy 2)")
        lag_of(l1, {t: v["pos"] for t, v in p2.items()}, "puppet (copy 1's player -> its puppet)")
    compare("copy 2's player -> its puppet in copy 1", l2, p1)


if __name__ == "__main__":
    main()
