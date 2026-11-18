#!/usr/bin/env python3
"""Sanity checks and summary tables of the worldgen reference data.

Reads data/<level_type>_<mc>_<seed>.txt and prints, per Minecraft version:
header checks, the ocean check (ocean columns have world_surface 62), land
height percentiles for Default and Amplified, and the share of biome cells
that differ between Default and Amplified at every sampled y.

Usage: stats.py [--data DIR] [--pairs N]
Exit status 1 if a header or an ocean check fails or a file has no H or B lines.
"""

import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SEA_LEVEL = 63
NOISE = {"normal": "minecraft:overworld", "amplified": "minecraft:amplified"}
NAME_RE = re.compile(r"^(normal|amplified)_(.+)_(-?\d+)\.txt$")


def version_key(mc):
    return [int(v) for v in re.findall(r"\d+", mc)]


def load(path):
    """Returns (header dict, {(x, z): (ws, of)}, {(x, y, z): biome}, {(x, z): area})."""
    hdr, h, b, area_of = {"area": []}, {}, {}, {}
    area = None
    with open(path) as f:
        for line in f:
            p = line.split()
            if p[0] == "H":
                h[(int(p[1]), int(p[2]))] = (int(p[3]), int(p[4]))
                area_of[(int(p[1]), int(p[2]))] = area
            elif p[0] == "B":
                b[(int(p[1]), int(p[2]), int(p[3]))] = p[4]
            elif p[0] == "#" and len(p) == 3 and p[1] == "area":
                area = p[2]
            elif p[0] == "#" and len(p) == 7 and p[1] == "area":
                hdr["area"].append(tuple(p[2:]))
            elif p[0] == "#" and len(p) >= 3 and p[1] in ("mc", "level_type", "noise_settings", "seed",
                                                         "min_y", "ticks"):
                hdr[p[1]] = " ".join(p[2:])
    return hdr, h, b, area_of


def pct(sorted_vals, p):
    """Nearest-rank percentile: the value at index floor(p/100 * n)."""
    if not sorted_vals:
        return None
    return sorted_vals[min(len(sorted_vals) - 1, int(p / 100.0 * len(sorted_vals)))]


def row(cells, widths):
    return "| " + " | ".join(str(c).ljust(w) for c, w in zip(cells, widths)) + " |"


def table(head, rows):
    widths = [max(len(str(r[i])) for r in [head] + rows) for i in range(len(head))]
    print(row(head, widths))
    print("|" + "|".join("-" * (w + 2) for w in widths) + "|")
    for r in rows:
        print(row(r, widths))
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    ap.add_argument("--pairs", type=int, default=6, help="biome changes listed per y (0 = none)")
    args = ap.parse_args()

    files = {}
    for path in sorted(glob.glob(os.path.join(args.data, "*.txt"))):
        m = NAME_RE.match(os.path.basename(path))
        if m:
            files[(m.group(2), int(m.group(3)), m.group(1))] = path
    if not files:
        raise SystemExit("no data files in %s" % args.data)
    bad = 0
    for mc in sorted({k[0] for k in files}, key=version_key):
        seeds = sorted({k[1] for k in files if k[0] == mc})
        print("## %s (%d seeds: %s)\n" % (mc, len(seeds), ", ".join(str(s) for s in seeds)))
        data = {}
        for seed in seeds:
            for lt in ("normal", "amplified"):
                path = files.get((mc, seed, lt))
                if not path:
                    print("missing: %s_%s_%d.txt" % (lt, mc, seed))
                    bad += 1
                    continue
                hdr, h, b, area_of = load(path)
                if not h or not b:
                    # a file without data passes every other check
                    print("EMPTY: %s has %d H and %d B lines" % (os.path.basename(path), len(h), len(b)))
                    bad += 1
                    continue
                data[(seed, lt)] = (hdr, h, b, area_of)
                want = {"mc": mc, "level_type": lt, "noise_settings": NOISE[lt], "seed": str(seed),
                        "min_y": "-64 height 384 sea_level 63"}
                for k, v in want.items():
                    if hdr.get(k) != v:
                        print("HEADER MISMATCH %s: %s is %r, expected %r"
                              % (os.path.basename(path), k, hdr.get(k), v))
                        bad += 1
        ticks = sorted({d[0].get("ticks") for d in data.values()})
        print("Headers: %d files, level_type / noise_settings / mc / seed agree with the file names;"
              " ticks: %s.\n" % (len(data), ", ".join(ticks)))
        for seed in seeds:
            if (seed, "normal") in data and (seed, "amplified") in data:
                n, a = data[(seed, "normal")], data[(seed, "amplified")]
                if n[0]["area"] != a[0]["area"] or set(n[1]) != set(a[1]) or set(n[2]) != set(a[2]):
                    print("AREA MISMATCH: seed %d normal and amplified cover different columns" % seed)
                    bad += 1

        # ---- ocean check
        print("Ocean check. \"Ocean column\": biome at y 64 is an ocean biome and ocean_floor < 62"
              " (sea bed under water). The last two columns are the remaining ocean-biome columns"
              " (islands, icebergs, ice sheets) and how many of them lie below the water level.\n")
        rows = []
        for lt in ("normal", "amplified"):
            tot = ok = shallow = shallow_low = 0
            other = {}
            for seed in seeds:
                if (seed, lt) not in data:
                    continue
                _, h, b, _ = data[(seed, lt)]
                for (x, y, z), biome in b.items():
                    if y != 64 or not biome.endswith("ocean"):
                        continue
                    ws, of = h[(x, z)]
                    if of >= SEA_LEVEL - 1:
                        shallow += 1
                        shallow_low += ws < SEA_LEVEL - 1
                        continue
                    tot += 1
                    if ws == SEA_LEVEL - 1:
                        ok += 1
                    else:
                        other[ws] = other.get(ws, 0) + 1
            rows.append((lt, tot, ok, tot - ok,
                         ", ".join("%d: %d" % kv for kv in sorted(other.items())) or "-", shallow,
                         shallow_low))
            if lt == "normal" and tot != ok:
                # ice or a structure above the water is legitimate, a surface below the water level is not
                if any(ws < SEA_LEVEL - 1 for ws in other):
                    bad += 1
        table(("level type", "ocean columns", "world_surface 62", "other", "other values (ws: count)",
               "ocean biome with ocean_floor >= 62", "of those world_surface < 62"), rows)

        # ---- land heights
        print("Land heights: ocean_floor of the columns with ocean_floor >= 63"
              " (nearest-rank percentiles), all seeds and areas together.\n")
        rows = []
        for lt in ("normal", "amplified"):
            allc = land = 0
            vals = []
            for seed in seeds:
                if (seed, lt) in data:
                    hs = data[(seed, lt)][1].values()
                    allc += len(hs)
                    vals += [of for _, of in hs if of >= SEA_LEVEL]
            vals.sort()
            rows.append((lt, allc, len(vals), "%.1f %%" % (100.0 * len(vals) / max(1, allc)),
                         pct(vals, 50), pct(vals, 90), pct(vals, 99), vals[-1] if vals else None))
        table(("level type", "columns", "land columns", "land share", "p50", "p90", "p99", "max"), rows)
        rows = []
        for seed in seeds:
            cells = [seed]
            for lt in ("normal", "amplified"):
                vals = sorted(of for _, of in data[(seed, lt)][1].values() if of >= SEA_LEVEL) \
                    if (seed, lt) in data else []
                cells += [len(vals), "%s / %s / %s" % (pct(vals, 50), pct(vals, 90), pct(vals, 99))]
            rows.append(tuple(cells))
        table(("seed", "normal land", "normal p50 / p90 / p99", "amplified land",
               "amplified p50 / p90 / p99"), rows)
        # columns that are land in both worlds: how much higher is Amplified?
        d = []
        for seed in seeds:
            if (seed, "normal") in data and (seed, "amplified") in data:
                hn, ha = data[(seed, "normal")][1], data[(seed, "amplified")][1]
                d += [ha[k][1] - hn[k][1] for k in hn if hn[k][1] >= SEA_LEVEL and ha[k][1] >= SEA_LEVEL]
        d.sort()
        if d:
            print("Columns that are land in both world types: %d; amplified minus normal ocean_floor:"
                  " p10 %d, p50 %d, p90 %d, p99 %d, min %d, max %d; amplified lower in %.1f %%.\n"
                  % (len(d), pct(d, 10), pct(d, 50), pct(d, 90), pct(d, 99), d[0], d[-1],
                     100.0 * sum(1 for v in d if v < 0) / len(d)))

        # ---- biome differences
        print("Biome cells (B lines) that differ between normal and amplified, same seed and position.\n")
        ys = sorted({k[1] for (seed, lt), v in data.items() for k in v[2]})
        per_y = {y: [0, 0] for y in ys}
        per_seed = {seed: {y: [0, 0] for y in ys} for seed in seeds}
        pairs = {y: {} for y in ys}
        for seed in seeds:
            if (seed, "normal") not in data or (seed, "amplified") not in data:
                continue
            bn, ba = data[(seed, "normal")][2], data[(seed, "amplified")][2]
            for k, v in bn.items():
                y = k[1]
                per_y[y][0] += 1
                per_seed[seed][y][0] += 1
                if ba[k] != v:
                    per_y[y][1] += 1
                    per_seed[seed][y][1] += 1
                    pairs[y][(v, ba[k])] = pairs[y].get((v, ba[k]), 0) + 1
        rows = []
        for y in ys:
            n, diff = per_y[y]
            cells = [y, n, diff, "%.2f %%" % (100.0 * diff / max(1, n))]
            cells += ["%.2f %%" % (100.0 * per_seed[s][y][1] / max(1, per_seed[s][y][0])) for s in seeds]
            rows.append(tuple(cells))
        n = sum(v[0] for v in per_y.values())
        diff = sum(v[1] for v in per_y.values())
        rows.append(("all", n, diff, "%.2f %%" % (100.0 * diff / max(1, n))) + ("",) * len(seeds))
        table(("y", "cells", "differ", "share") + tuple("seed %d" % s for s in seeds), rows)
        if args.pairs:
            print("Most frequent changes per y (normal -> amplified: cells):\n")
            for y in ys:
                top = sorted(pairs[y].items(), key=lambda kv: (-kv[1], kv[0]))[:args.pairs]
                if top:
                    print("* y %d: %s" % (y, "; ".join("%s -> %s: %d" % (a, b, c) for (a, b), c in top)))
            print()
    if bad:
        print("FAILED: %d problem(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
