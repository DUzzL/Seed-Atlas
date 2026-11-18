#!/usr/bin/env python3
"""Compare the stronghold reference of the Default world type with another world type.

Reads data/portals_<mc>.txt and data/portals_<mc>_<level_type>.txt and reports,
for the strongholds both files have, how many start chunks moved and by how
far, and whether frames and eyes agree where the start chunk is the same.

Pairing:
* sh_index >= 0 (26.x, vanilla's own ring order): by (seed, sh_index).
* sh_index -1 (obfuscated jars): by (seed, search hint), the hint being the
  "# engine idx N + vanilla locate" comment above the line. Both lines are then
  the stronghold nearest to the same point (the engine's position for the
  Default world type). That is the same ring index only as long as a
  stronghold stays close to that point; in 1.18.2 it does not (see README).

Usage: compare_level_types.py [--data DIR] [--level-type amplified] [--list]
"""

import argparse
import glob
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HINT_RE = re.compile(r"^# engine idx (\d+) \+ vanilla locate")


def load(path):
    """key -> (start chunk, frames); key = (seed, sh_index) or (seed, 'hint N')."""
    out, starts, hint = {}, {}, None
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                m = HINT_RE.match(line)
                hint = int(m.group(1)) if m else None
                continue
            p = line.split()
            if not p:
                continue
            # P <seed> <index> <chunk x> <chunk z> and 12 frames of 4 numbers
            if len(p) != 5 + 48:
                raise SystemExit("%s: a line with %d fields, expected %d: %s" % (path, len(p), 5 + 48, line.strip()))
            seed, idx = int(p[1]), int(p[2])
            if idx < 0 and hint is None:
                raise SystemExit("%s: line without index and without hint" % path)
            key = (seed, idx if idx >= 0 else "hint %d" % hint)
            if key in out:
                raise SystemExit("%s: %s twice" % (path, key))
            v = [int(x) for x in p[5:]]
            frames = [tuple(v[i:i + 4]) for i in range(0, 48, 4)]
            out[key] = ((int(p[3]), int(p[4])), frames)
            starts.setdefault(seed, set()).add((int(p[3]), int(p[4])))
            hint = None
    return out, starts


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(p / 100.0 * len(sorted_vals)))]


def version_key(mc):
    return [int(v) for v in re.findall(r"\d+", mc)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    ap.add_argument("--level-type", default="amplified")
    ap.add_argument("--list", action="store_true", help="list every moved stronghold")
    args = ap.parse_args()
    sfx = "_%s.txt" % args.level_type
    paths = sorted(glob.glob(os.path.join(args.data, "portals_*" + sfx)),
                   key=lambda p: version_key(os.path.basename(p)))
    if not paths:
        raise SystemExit("no portals_<mc>%s in %s" % (sfx, args.data))
    paired = 0
    for path in paths:
        mc = os.path.basename(path)[len("portals_"):-len(sfx)]
        base = os.path.join(args.data, "portals_%s.txt" % mc)
        if not os.path.exists(base):
            print("%s: no Default file" % mc)
            continue
        a, a_starts = load(base)
        b, b_starts = load(path)
        common = sorted((k for k in b if k in a), key=str)
        paired += len(common)
        by_index = all(isinstance(k[1], int) for k in common)
        moved, same = [], []
        for k in common:
            (ac, af), (bc, bf) = a[k], b[k]
            (moved if ac != bc else same).append(k)
        frames_same = sum(1 for k in same if [f[:3] for f in a[k][1]] == [f[:3] for f in b[k][1]])
        eyes_same = sum(1 for k in same if a[k][1] == b[k][1])
        eye_frames = sum(1 for k in same for fa, fb in zip(a[k][1], b[k][1]) if fa[:3] == fb[:3])
        eye_diff = sum(1 for k in same for fa, fb in zip(a[k][1], b[k][1])
                       if fa[:3] == fb[:3] and fa[3] != fb[3])
        print("## %s: normal vs %s, paired by %s" % (
            mc, args.level_type, "(seed, sh_index)" if by_index else "(seed, search hint)"))
        print("strongholds: normal %d, %s %d, paired %d (%d seeds)"
              % (len(a), args.level_type, len(b), len(common), len({k[0] for k in common})))
        print("start chunk: same %d, moved %d (%.1f %%)"
              % (len(same), len(moved), 100.0 * len(moved) / max(1, len(common))))
        if moved:
            cheb = sorted(max(abs(a[k][0][0] - b[k][0][0]), abs(a[k][0][1] - b[k][0][1])) for k in moved)
            eucl = sorted(16 * math.hypot(a[k][0][0] - b[k][0][0], a[k][0][1] - b[k][0][1]) for k in moved)
            print("moved by (chunks, larger of |dx|, |dz|): min %d, median %d, max %d;"
                  " (blocks, straight line): min %.0f, median %.0f, max %.0f"
                  % (cheb[0], pct(cheb, 50), cheb[-1], eucl[0], pct(eucl, 50), eucl[-1]))
            hist = {}
            for c in cheb:
                lab = "1-3" if c <= 3 else "4-7" if c <= 7 else "8-14" if c <= 14 else "15-31" if c <= 31 else "32+"
                hist[lab] = hist.get(lab, 0) + 1
            print("moved by, histogram (chunks): " + ", ".join(
                "%s: %d" % (lab, hist[lab]) for lab in ("1-3", "4-7", "8-14", "15-31", "32+") if lab in hist))
        print("same start chunk (%d): frame positions identical in %d, all 12 eye states identical in %d;"
              " eye states differing: %d of %d frames"
              % (len(same), frames_same, eyes_same, eye_diff, eye_frames))
        if not by_index:
            # independent of any pairing: positions as sets, per seed
            seeds = sorted(set(a_starts) & set(b_starts))
            inter = sum(len(a_starts[s] & b_starts[s]) for s in seeds)
            print("without pairing, seeds in both files (%d): %d of the %d %s start chunks are also a"
                  " normal start chunk"
                  % (len(seeds), inter, sum(len(b_starts[s]) for s in seeds), args.level_type))
        if args.list:
            for k in moved:
                print("  seed %d %s: normal %d,%d -> %s %d,%d"
                      % (k[0], ("idx %d" % k[1]) if by_index else k[1], a[k][0][0], a[k][0][1],
                         args.level_type, b[k][0][0], b[k][0][1]))
        print()
    if paired == 0:
        print("FAILED: no stronghold of the %s files pairs with a Default one" % args.level_type)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
