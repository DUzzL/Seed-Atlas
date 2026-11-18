#!/usr/bin/env python3
"""Generated parts of the worldgen test pack (testpack/).

Hand-written and NOT touched by this script: pack.mcmeta, the noise settings,
the noises, the density functions and the overlay_* directories. Nothing in
the pack is copied from the game's data: vanilla resources are only named by
id (check_no_vanilla.py checks that). Generated,
deterministically (fixed random seed, no input files):

  data/minecraft/dimension/overworld.json       the inline biome parameter list
  data/testpack/worldgen/biome/*.json           one file per testpack biome
  data/minecraft/tags/worldgen/biome/...        two biome tag files
  data/c/worldgen/biome_colors.json             map colours of the testpack biomes

Usage:
  make_testpack.py                 rewrite the generated files in testpack/
  make_testpack.py --check         generate in memory and compare with testpack/ (and
                                   testpack-structures/ if present; exit 1 on a difference)
  make_testpack.py --structures    write testpack-structures/: a copy of testpack/ with structures
                                   (see "structures" below)
  make_testpack.py --list          print the entries with their role
  make_testpack.py --probe PARAM --out DIR
        write a copy of the pack to DIR whose biome list splits PARAM into 160
        bands of 0.025 ('testpack:probe_000' ... ), every other parameter
        unconstrained: the biomes of a generated world are then a measurement
        of that climate input. Used to find the distributions in MARGINALS.

How the list is built and which entries are special: see README.md
("Test pack"). Vanilla computes with quantised values, q(x) = (long)((float)x * 10000f);
q() below reproduces that.
"""

import argparse
import gzip
import io
import json
import os
import random
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, "testpack")
SEED = 20261115

# Order of the parameters in an entry. 'depth' is last here; vanilla's own
# order (temperature, humidity, continentalness, erosion, depth, weirdness,
# offset) only matters to whoever rebuilds vanilla's search tree.
PARAMS = ("temperature", "humidity", "continentalness", "erosion", "weirdness", "depth")
T, H, C, E, W, D = range(6)
LO, HI = -2.0, 2.0  # vanilla accepts parameter bounds in [-2, 2]


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def q(x):
    """Vanilla's quantisation of a parameter bound or a climate value."""
    return int(f32(f32(x) * 10000.0))


# ------------------------------------------------------------- distributions
#
# Cumulative distribution of every climate input of the pack, measured with
# --probe worlds on a vanilla 1.21.1 server (seeds 12345 and -42, the areas of
# worlds.txt, all ten y levels of the reference data): (value, share of cells
# below value). They only steer where the boxes are split, so that most
# entries are reachable; they do not have to be exact.
MARGINALS = {
    # temperature: the part between the plateaus only
    "temperature": [(-0.975, 0), (-0.9125, 0.05), (-0.8375, 0.1), (-0.7875, 0.15), (-0.7125, 0.2),
                    (-0.6625, 0.25), (-0.6125, 0.3), (-0.5375, 0.35), (-0.4875, 0.4), (-0.4125, 0.45),
                    (-0.3625, 0.5), (-0.2875, 0.55), (-0.1875, 0.6), (-0.1125, 0.65), (-0.0375, 0.7),
                    (0.0625, 0.75), (0.1375, 0.8), (0.2375, 0.85), (0.3125, 0.9), (0.4125, 0.95), (0.55, 1)],
    "humidity": [(-1.425, 0), (-0.8625, 0.05), (-0.7125, 0.1), (-0.5875, 0.15), (-0.4875, 0.2),
                 (-0.4125, 0.25), (-0.3125, 0.3), (-0.2375, 0.35), (-0.1625, 0.4), (-0.0625, 0.45),
                 (0.0125, 0.5), (0.0875, 0.55), (0.1625, 0.6), (0.2375, 0.65), (0.3375, 0.7),
                 (0.4375, 0.75), (0.5375, 0.8), (0.6625, 0.85), (0.7875, 0.9), (0.9875, 0.95), (2, 1)],
    "continentalness": [(-1.5, 0), (-1.0625, 0.05), (-0.8625, 0.1), (-0.7375, 0.15), (-0.6375, 0.2),
                        (-0.5375, 0.25), (-0.4625, 0.3), (-0.4125, 0.35), (-0.3625, 0.4), (-0.2875, 0.45),
                        (-0.2375, 0.5), (-0.1875, 0.55), (-0.1375, 0.6), (-0.0625, 0.65), (-0.0125, 0.7),
                        (0.0625, 0.75), (0.1375, 0.8), (0.2125, 0.85), (0.3125, 0.9), (0.4375, 0.95),
                        (0.85, 1)],
    "erosion": [(-1.1, 0), (-0.7375, 0.05), (-0.6625, 0.1), (-0.6125, 0.15), (-0.5625, 0.2),
                (-0.5125, 0.25), (-0.4625, 0.3), (-0.4125, 0.35), (-0.3375, 0.4), (-0.2625, 0.45),
                (-0.1875, 0.5), (-0.1125, 0.55), (0.0375, 0.6), (0.2375, 0.65), (0.3375, 0.7),
                (0.3875, 0.75), (0.4125, 0.8), (0.4875, 0.85), (0.5625, 0.9), (0.6875, 0.95), (1.7, 1)],
    "weirdness": [(-1.5, 0), (-0.8125, 0.05), (-0.5875, 0.1), (-0.4125, 0.15), (-0.3125, 0.2),
                  (-0.2625, 0.25), (-0.2125, 0.3), (-0.1625, 0.35), (-0.1375, 0.4), (-0.0875, 0.45),
                  (-0.0375, 0.5), (0.0125, 0.55), (0.0625, 0.6), (0.1125, 0.65), (0.1625, 0.7),
                  (0.2375, 0.75), (0.3125, 0.8), (0.4125, 0.85), (0.5125, 0.9), (0.7125, 0.95),
                  (1.55, 1)],
    "depth": [(-1.85, 0), (-1.6875, 0.05), (-1.4125, 0.1), (-1.2875, 0.15), (-1.0875, 0.2),
              (-0.8375, 0.25), (-0.6625, 0.3), (-0.4875, 0.35), (-0.3125, 0.4), (-0.1625, 0.45),
              (-0.0375, 0.5), (0.1125, 0.55), (0.2375, 0.6), (0.3625, 0.65), (0.4375, 0.7),
              (0.5375, 0.75), (0.6625, 0.8), (0.7875, 0.85), (0.9125, 0.9), (1.1125, 0.95), (1.775, 1)],
}
# Share of cells on the two plateaus of the temperature (its clamp bounds).
T_COLD_SHARE, T_HOT_SHARE = 0.15, 0.18

# The temperature is clamped to [-0.99995, 0.57]. Both bounds are plateaus that
# cover whole regions, and both quantise in a way that is easy to get wrong:
#   q(-0.99995) = -9999  (truncation towards zero; floor or rounding give -10000)
#   q(0.57)     =  5700  (float arithmetic; (long)(0.57 * 10000.0) in double is 5699)
# The list has a one-unit gap exactly there, see build():
#   -1.0 | -0.9999     quantise to -10000 | -9999 with every method
#   0.56996 | 0.57006  quantise to 5699 | 5700 by truncation; rounded to
#                      nearest they are 5700 | 5701
# A wrong quantisation of the climate value puts a whole plateau strictly into
# the boxes on the other side of the gap, and so do list bounds that are
# rounded instead of truncated (upper plateau).
T_MIN, T_MAX = -0.99995, 0.57
COLD_HI, MAIN_LO = -1.0, -0.9999     # q: -10000 | -9999
MAIN_HI, HOT_LO = 0.56996, 0.57006   # q:   5699 |  5700
# Depth band of the equidistant pair (equi_p / equi_q), see build(): both
# boxes start 300 units above the hot plateau and differ only in their upper
# temperature bound, which no temperature can reach.
BAND_LO, BAND_HI = -0.15, 0.1
EQUI_T_LO, EQUI_P_HI, EQUI_Q_HI = 0.6, 2.0, 1.0

# Bounds whose quantisation is not obvious, used as split positions wherever
# they fall into the populated range of a parameter.
FRAGILE = (0.33333334, 0.12345678, -0.99995, 0.57, -0.45678912, 0.07, 0.83, -0.29, -0.58,
           0.14, 0.56, -0.6666667, 0.2857143, -0.07, 0.41, -0.14, 0.69, -0.83, 0.99995, 1.13)

N_BASE = 252       # boxes of the partition (before the duplicates and the overlapping boxes)
N_OVERLAP = 36
N_TIES = 6
BASE_OFFSETS = (0.0,) * 9 + (0.03125, 0.05, 0.05) + (0.1,) * 4 + (0.2,) * 3 + (0.3,)
SCALAR_OFFSETS = (0.0, 0.0, 0.03125)
OVERLAP_OFFSETS = (0.02, 0.07, 0.15)   # none of them is a base offset
# The main body is cut into a strip along each plateau and the part between them.
STRIP_COLD, STRIP_HOT = -0.83, 0.41


def cdf(param, x):
    pts = MARGINALS[param]
    if x <= pts[0][0]:
        return 0.0
    for (x0, c0), (x1, c1) in zip(pts, pts[1:]):
        if x <= x1:
            return c0 + (c1 - c0) * (x - x0) / (x1 - x0) if x1 > x0 else c1
    return 1.0


def quantile(param, p):
    pts = MARGINALS[param]
    for (x0, c0), (x1, c1) in zip(pts, pts[1:]):
        if p <= c1:
            return x0 + (x1 - x0) * (p - c0) / (c1 - c0) if c1 > c0 else x0
    return pts[-1][0]


def cuts_of(d):
    """Candidate split positions of a parameter: 5 % quantiles (two decimals) and the fragile bounds."""
    param = PARAMS[d]
    lo, hi = quantile(param, 0.02), quantile(param, 0.98)
    out = {round(quantile(param, k / 20.0), 2) for k in range(1, 20)}
    out |= {v for v in FRAGILE if lo < v < hi}
    if d == T:
        # nothing may sit within the gaps or beyond the plateaus
        out = {v for v in out if MAIN_LO + 0.02 < v < MAIN_HI - 0.02}
    return sorted(out)


# -------------------------------------------------------------------- biomes

GREEN = ("amber_plains", "azure_marsh", "cobalt_steppe", "crimson_heath", "emerald_basin",
         "jade_thicket", "violet_moor")
BARE = ("basalt_flats", "ivory_tundra", "ochre_dunes", "pearl_coast", "quartz_ridge",
        "ruby_canyon", "slate_highland")
POOL_MAIN = tuple("testpack:" + n for n in GREEN + BARE) + tuple("minecraft:" + n for n in (
    "plains", "forest", "birch_forest", "dark_forest", "taiga", "savanna", "desert", "swamp",
    "jungle", "meadow", "badlands", "beach", "river", "ocean", "deep_ocean", "warm_ocean",
    "lukewarm_ocean", "cold_ocean", "windswept_hills", "cherry_grove", "mangrove_swamp",
    "sunflower_plains", "stony_shore", "mushroom_fields", "sparse_jungle", "old_growth_pine_taiga",
    "savanna_plateau", "flower_forest", "wooded_badlands", "stony_peaks"))
# temperature on its upper plateau (quantised 5700)
POOL_HOT = ("testpack:hot_ember", "testpack:hot_scorch", "testpack:hot_cinder",
            "minecraft:eroded_badlands", "minecraft:windswept_savanna", "minecraft:bamboo_jungle")
# depth >= 0.5 / depth <= -0.8 everywhere in the box
POOL_DEEP = ("testpack:umber_grotto", "testpack:obsidian_depths", "minecraft:dripstone_caves",
             "minecraft:lush_caves", "minecraft:deep_dark")
POOL_SKY = ("testpack:sky_mist", "testpack:sky_gale", "minecraft:frozen_peaks", "minecraft:jagged_peaks",
            "minecraft:snowy_slopes", "minecraft:grove", "minecraft:ice_spikes", "minecraft:snowy_plains",
            "minecraft:snowy_taiga", "minecraft:frozen_ocean", "minecraft:snowy_beach")
# humidity >= 1.13 or continentalness <= -1.06 everywhere in the box (bounds beyond +-1)
POOL_OUTER = ("testpack:outer_rim", "testpack:outer_reach")
POOL_PATCH = ("testpack:patch_a", "testpack:patch_b", "testpack:patch_c", "testpack:patch_d")
UNREACHABLE = "testpack:unreachable_cold"
EQUI_P, EQUI_Q = "testpack:equi_p", "testpack:equi_q"
DEEP_FROM, SKY_UNTIL = 0.5, -0.8
# Slabs beyond +-1 that are cut off first: humidity above 1.13, continentalness below -1.06.
OUTER_H, OUTER_C = 1.13, -1.06


def testpack_biomes():
    """Names (without namespace) of all biomes the pack defines, in a fixed order."""
    names = list(GREEN + BARE)
    for pool in (POOL_HOT, POOL_DEEP, POOL_SKY, POOL_OUTER, POOL_PATCH):
        names += [b.split(":")[1] for b in pool if b.startswith("testpack:")]
    names += [UNREACHABLE.split(":")[1], EQUI_P.split(":")[1], EQUI_Q.split(":")[1]]
    for i in range(1, N_TIES + 1):
        names += ["tie%d_a" % i, "tie%d_b" % i]
    return names


def colour_of(name):
    """A stable, well spread colour per biome name."""
    rnd = random.Random("colour:" + name)
    return {"r": rnd.randrange(40, 236), "g": rnd.randrange(40, 236), "b": rnd.randrange(40, 236)}


# Decoration of the 'green' testpack biomes: a few vanilla features, named by
# id, in steps 6 (underground ores) and 9 (vegetal decoration). Their order
# within a step agrees with every vanilla biome (the game rejects biome sets
# whose feature orders contradict each other). The other testpack biomes are
# bare.
GREEN_FEATURES = [[], [], [], [], [], [],
                  ["minecraft:ore_coal_upper", "minecraft:ore_iron_small", "minecraft:disk_clay"],
                  [], [],
                  ["minecraft:trees_plains", "minecraft:patch_sugar_cane"]]


def biome_json(name):
    c = colour_of(name)
    rgb = (c["r"] << 16) | (c["g"] << 8) | c["b"]
    green = name in GREEN
    return {
        "has_precipitation": green,
        "temperature": 0.75 if green else 1.2,
        "downfall": 0.45 if green else 0.0,
        "effects": {
            "sky_color": 0x88b4f0,
            "fog_color": 0xd0e0f8,
            "water_color": rgb,
            "water_fog_color": 0x0b2a48,
            "grass_color": rgb,
            "foliage_color": rgb ^ 0x202020,
        },
        "spawners": {},
        "spawn_costs": {},
        "carvers": {"air": ["minecraft:cave"]} if green else {},
        "features": GREEN_FEATURES if green else [],
    }


# ------------------------------------------------------------------- entries

class Entry:
    def __init__(self, box, biome=None, offset=0.0, role="base"):
        self.box = [tuple(b) for b in box]   # per parameter (min, max)
        self.biome = biome
        self.offset = offset
        self.role = role
        self.frozen = False                  # never split further

    def copy(self):
        e = Entry(self.box, self.biome, self.offset, self.role)
        e.frozen = self.frozen
        return e

    def qbox(self):
        return [(q(a), q(b)) for a, b in self.box]

    def mass(self):
        m = 1.0
        for d, (a, b) in enumerate(self.box):
            m *= share(d, a, b)
        return m

    def json(self):
        par = {}
        for d, (a, b) in enumerate(self.box):
            par[PARAMS[d]] = num(a) if a == b else "[%s, %s]" % (num(a), num(b))
        par["offset"] = num(self.offset)
        return '{"biome": "%s", "parameters": {%s}}' % (
            self.biome, ", ".join('"%s": %s' % kv for kv in par.items()))


def num(x):
    """Shortest decimal that reads back as the same number."""
    s = repr(float(x))
    return s


def share(d, a, b):
    """Estimated share of cells whose parameter d lies in [a, b]."""
    if d == T:
        # two plateaus (point masses) and the continuous part between them
        s = 0.0
        if q(a) <= q(T_MIN) <= q(b):
            s += T_COLD_SHARE
        if q(a) <= q(T_MAX) <= q(b):
            s += T_HOT_SHARE
        lo, hi = max(a, T_MIN), min(b, T_MAX)
        if hi > lo:
            s += (1.0 - T_COLD_SHARE - T_HOT_SHARE) * (cdf("temperature", hi) - cdf("temperature", lo))
        return max(s, 1e-9)
    return max(cdf(PARAMS[d], b) - cdf(PARAMS[d], a), 1e-9)


def split(e, d, left_hi, right_lo=None):
    """Two copies of e, cut in parameter d (with a gap if right_lo is given)."""
    a, b = e.box[d]
    l, r = e.copy(), e.copy()
    l.box[d] = (a, left_hi)
    r.box[d] = (left_hi if right_lo is None else right_lo, b)
    return l, r


def build():
    rnd = random.Random(SEED)
    cuts = [cuts_of(d) for d in range(6)]

    # ---- fixed top of the partition -------------------------------------
    # temperature: cold slab | main body | hot slab, with one-unit gaps at the plateaus
    root = Entry([(LO, HI)] * 6)
    cold, rest = split(root, T, COLD_HI, MAIN_LO)
    main, hot = split(rest, T, MAIN_HI, HOT_LO)
    # No correct implementation ever returns the cold slab: the temperature
    # cannot quantise below -9999.
    cold.biome, cold.role, cold.frozen = UNREACHABLE, "unreachable", True
    # humidity and continentalness: cut the populated tails beyond +-1 off the main body
    main, outer_h = split(main, H, OUTER_H)
    outer_c, main = split(main, C, OUTER_C)
    # temperature again: a strip along each plateau. Only the boxes of the part
    # between the strips are free to have offsets and single values.
    cold_strip, x = split(main, T, STRIP_COLD)
    middle, hot_strip = split(x, T, STRIP_HOT)
    # depth: cut at the band of the equidistant pair
    leaves = [cold, outer_h, outer_c]
    for part in (cold_strip, middle, hot_strip, hot):
        below, x = split(part, D, BAND_LO)
        band, above = split(x, D, BAND_HI)
        leaves += [below, band, above]
        if part is hot:
            # The hot plateau within the depth band is left without a box of
            # its own: the nearest boxes there are the pair made below.
            leaves.remove(band)
            hole = band

    # ---- k-d partition ---------------------------------------------------
    while len(leaves) < N_BASE:
        order = sorted((e for e in leaves if not e.frozen), key=lambda e: -e.mass())
        done = False
        for e in order:
            options = []
            for d in range(6):
                a, b = e.box[d]
                tot = share(d, a, b)
                good = [c for c in cuts[d] if a < c < b
                        and min(share(d, a, c), share(d, c, b)) >= 0.28 * tot]
                if good:
                    options.append((d, good, tot))
            if not options:
                continue
            # prefer the parameters in which the box still spans much of the distribution
            d, good, _ = rnd.choices(options, weights=[o[2] ** 2 for o in options])[0]
            fragile = [c for c in good if c in FRAGILE]
            c = rnd.choice(fragile) if fragile and rnd.random() < 0.6 else rnd.choice(good)
            l, r = split(e, d, c)
            i = leaves.index(e)
            leaves[i:i + 1] = [l, r]
            done = True
            break
        if not done:
            raise SystemExit("cannot split further at %d boxes" % len(leaves))

    # ---- offsets -----------------------------------------------------------
    for e in leaves:
        if e.frozen:
            continue
        lo_t, hi_t = e.box[T]
        at_plateau = hi_t == MAIN_HI or lo_t == MAIN_LO or lo_t == HOT_LO
        if (hi_t == MAIN_HI and lo_t >= STRIP_HOT and BAND_LO <= e.box[D][0] and e.box[D][1] <= BAND_HI
                and e.box[H][1] <= OUTER_H and e.box[C][0] >= OUTER_C):
            # the hot strip within the band lies next to the hole of the pair
            # and must lose against the pair on the plateau (offset 0.05 = 500
            # units, the pair is 300 units away)
            e.offset = 0.05
        elif at_plateau:
            # the quantisation of the plateau values decides between these boxes
            e.offset = 0.0
        else:
            e.offset = rnd.choice(BASE_OFFSETS)

    # ---- a scalar instead of a range in one parameter of some boxes ---------
    # Not next to a plateau: a box that is a point in some parameter is not the
    # nearest one for most of its former range, and its neighbour across the
    # one-unit gap would take over.
    used = {v for e in leaves for a, b in e.qbox() for v in (a, b)}
    free = [e for e in leaves if not e.frozen
            and e.box[T][1] != MAIN_HI and e.box[T][0] not in (MAIN_LO, HOT_LO)]
    for e in rnd.sample(free, N_BASE // 7):
        d = rnd.randrange(6)
        a, b = e.box[d]
        # a point inside, near the middle of the populated part, that is no
        # bound of any other box (equal bounds would make two boxes
        # equidistant in a whole region)
        ca, cb = cdf(PARAMS[d], a), cdf(PARAMS[d], b)
        v = round(quantile(PARAMS[d], ca + (cb - ca) * rnd.uniform(0.35, 0.65)), 4)
        v = min(max(v, round(a + 0.001, 4)), round(b - 0.001, 4))
        while q(v) in used:
            v = round(v + 0.0001, 4)
        used.add(q(v))
        e.box[d] = (v, v)
        e.role = "scalar"
        e.offset = rnd.choice(SCALAR_OFFSETS)

    # ---- biomes -------------------------------------------------------------
    def pool_of(e):
        if e.box[T][0] == HOT_LO:
            return POOL_HOT
        if e.box[H][0] >= OUTER_H or e.box[C][1] <= OUTER_C:
            return POOL_OUTER
        if e.box[D][0] >= DEEP_FROM:
            return POOL_DEEP
        if e.box[D][1] <= SKY_UNTIL:
            return POOL_SKY
        return POOL_MAIN

    def touching(x, y):
        return all(a0 <= b1 and b0 <= a1 for (a0, a1), (b0, b1) in zip(x.qbox(), y.qbox()))

    for i, e in enumerate(leaves):
        if e.biome:
            continue
        near = {o.biome for o in leaves[:i] if o.biome and touching(e, o)}
        pool = pool_of(e)
        free = [b for b in pool if b not in near] or list(pool)
        e.biome = rnd.choice(free)

    # ---- overlapping boxes ----------------------------------------------------
    overlaps = []
    tries = 0
    while len(overlaps) < N_OVERLAP:
        tries += 1
        if tries > 20000:
            raise SystemExit("cannot place the overlapping boxes")
        box = []
        point = rnd.randrange(12)   # at most one parameter is a single value (in half of the boxes)
        for d in range(6):
            param = PARAMS[d]
            c = quantile(param, rnd.uniform(0.1, 0.9))
            if d == point:
                v = round(c, 4)
                box.append((v, v))
            elif rnd.random() < 0.45:
                box.append((round(c - rnd.uniform(0.5, 1.5), 2), round(c + rnd.uniform(0.5, 1.5), 2)))
            else:
                w = rnd.uniform(0.1, 0.4)
                box.append((round(c - w, 3), round(c + w, 3)))
        box = [(max(LO, a), min(HI, b)) for a, b in box]
        # stay out of the plateaus: the boxes there are tests of their own
        if box[T][1] > 0.5 or box[T][0] < -0.95:
            continue
        e = Entry(box, POOL_PATCH[len(overlaps) % len(POOL_PATCH)], rnd.choice(OVERLAP_OFFSETS), "overlap")
        if any(region_tie(e, o) for o in leaves + overlaps):
            continue
        overlaps.append(e)

    # ---- identical boxes with different biomes ---------------------------------
    cand = sorted((e for e in leaves if not e.frozen and e.role == "base" and e.offset == 0.0
                   and e.box[T][0] != HOT_LO), key=lambda e: -e.mass())
    picked = cand[:N_TIES // 2] + rnd.sample(cand[N_TIES:len(cand) // 2], N_TIES - N_TIES // 2)
    twins = []
    for i, e in enumerate(picked, 1):
        e.biome, e.role = "testpack:tie%d_a" % i, "tie first"
        t = e.copy()
        t.biome, t.role = "testpack:tie%d_b" % i, "tie second"
        twins.append((e, t))

    # ---- two different boxes that are equally far from every reachable point ------
    # Both lie 300 units above the hot plateau, within the depth band, and
    # differ only in their upper temperature bound.
    pe, qe = hole.copy(), hole.copy()
    pe.box[T], qe.box[T] = (EQUI_T_LO, EQUI_P_HI), (EQUI_T_LO, EQUI_Q_HI)
    pe.biome, qe.biome = EQUI_P, EQUI_Q
    for e in (pe, qe):
        e.offset, e.role, e.frozen = 0.0, "equi", True

    # ---- list order -------------------------------------------------------------
    entries = leaves + overlaps + [pe]
    rnd.shuffle(entries)
    for i, (e, t) in enumerate(twins):
        if i % 2 == 0:
            entries.insert(entries.index(e) + 1, t)   # directly after its twin
        else:
            entries.append(t)                          # far behind its twin
    entries.insert(rnd.randrange(entries.index(pe) + 1, len(entries) + 1), qe)   # equi_p is listed first
    check(entries, twins + [(pe, qe)])
    return entries


def region_tie(x, y):
    """True if two entries are equidistant from every point of some region.

    That is the case when the offsets quantise equally and in every parameter
    the ranges either overlap in more than a point or share a bound (the
    points beyond that bound are equally far from both).
    """
    if q(x.offset) != q(y.offset):
        return False
    for (a0, a1), (b0, b1) in zip(x.qbox(), y.qbox()):
        if not (max(a0, b0) < min(a1, b1) or a0 == b0 or a1 == b1):
            return False
    return True


def check(entries, twins):
    """No region-sized tie except the intended ones; all values legal."""
    allowed = {frozenset((id(a), id(b))) for a, b in twins}
    for i, x in enumerate(entries):
        for a, b in x.box:
            if not (LO <= a <= b <= HI):
                raise SystemExit("bad range %r" % (x.box,))
        if not 0.0 <= x.offset <= 1.0:
            raise SystemExit("bad offset %r" % x.offset)
        for y in entries[i + 1:]:
            if region_tie(x, y) and frozenset((id(x), id(y))) not in allowed:
                raise SystemExit("unintended region tie:\n  %s\n  %s" % (x.json(), y.json()))
    names = {"testpack:" + n for n in testpack_biomes()}
    for e in entries:
        if e.biome.startswith("testpack:") and e.biome not in names:
            raise SystemExit("biome %s has no file" % e.biome)


# -------------------------------------------------------------------- output

def dimension_json(entries):
    lines = ",\n".join("        " + e.json() for e in entries)
    return ('{\n  "type": "minecraft:overworld",\n  "generator": {\n    "type": "minecraft:noise",\n'
            '    "settings": "testpack:overworld",\n    "biome_source": {\n'
            '      "type": "minecraft:multi_noise",\n      "biomes": [\n%s\n      ]\n    }\n  }\n}\n' % lines)


def pretty(obj):
    return json.dumps(obj, indent=2) + "\n"


def generated_files(entries, biome_names):
    """{path relative to the pack: text}."""
    out = {"data/minecraft/dimension/overworld.json": dimension_json(entries)}
    for n in biome_names:
        out["data/testpack/worldgen/biome/%s.json" % n] = pretty(biome_json(n))
    # extends a vanilla tag
    out["data/minecraft/tags/worldgen/biome/is_overworld.json"] = pretty(
        {"replace": False, "values": ["testpack:" + n for n in biome_names]})
    # replaces a vanilla tag: plains villages only in these two biomes
    out["data/minecraft/tags/worldgen/biome/has_structure/village_plains.json"] = pretty(
        {"replace": True, "values": ["testpack:amber_plains", "minecraft:meadow"]})
    out["data/c/worldgen/biome_colors.json"] = pretty(
        {"testpack:" + n: colour_of(n) for n in biome_names})
    return out


GENERATED_DIRS = ("data/minecraft/dimension", "data/testpack/worldgen/biome",
                  "data/minecraft/tags", "data/c")


def write_files(root, files):
    for d in GENERATED_DIRS:
        p = os.path.join(root, *d.split("/"))
        if os.path.isdir(p):
            shutil.rmtree(p)
    for rel, text in files.items():
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)


# ---------------------------------------------------------------- structures
#
# testpack-structures/: the test pack plus structures of its own, for the
# structure start reference data (worldgen_reference.py --structures). Every
# placement rule of the game has a set here: random_spread with both spread
# types and all four frequency reduction methods, an exclusion zone, a weighted
# set (with an empty start element, so the game falls back to the next
# structure), concentric rings with preferred biomes; jigsaw structures with
# every height provider type, a projected start, a named start jigsaw that two
# jigsaw blocks share (the game's shuffle decides), list and legacy elements;
# and two structure types of the game (fortress, swamp hut) in biomes of the
# pack. The templates are written here (NBT, gzip with a fixed time stamp).

STRUCT_PACK = os.path.join(HERE, "testpack-structures")
DATA_VERSION = 3839     # 1.20.6; 1.21.1 upgrades the templates when it loads them


def _nbt(t, v):
    if t == 1:
        return struct.pack(">b", v)
    if t == 3:
        return struct.pack(">i", v)
    if t == 8:
        b = v.encode("utf-8")
        return struct.pack(">H", len(b)) + b
    if t == 9:
        et, items = v
        return struct.pack(">bi", et if items else 0, len(items)) + b"".join(_nbt(et, x) for x in items)
    if t == 10:
        return b"".join(struct.pack(">b", ct) + _nbt(8, k) + _nbt(ct, cv) for k, (ct, cv) in v.items()) + b"\0"
    raise ValueError(t)


def nbt_gzip(root):
    """A gzip compressed NBT file with an unnamed root compound; reproducible."""
    raw = b"\x0a" + _nbt(8, "") + _nbt(10, root)
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0, filename="") as g:
        g.write(raw)
    return buf.getvalue()


def template(size, jigsaws=()):
    """A floor of stone with jigsaw blocks: jigsaws = ((x, y, z, name, selection_priority), ...)."""
    sx, sy, sz = size
    palette = [{"Name": (8, "minecraft:stone")},
               {"Name": (8, "minecraft:jigsaw"), "Properties": (10, {"orientation": (8, "up_north")})}]
    at = {(j[0], j[1], j[2]): j for j in jigsaws}
    blocks = []
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                j = at.get((x, y, z))
                if j:
                    nbt = {"id": (8, "minecraft:jigsaw"), "name": (8, j[3]), "target": (8, "minecraft:empty"),
                           "pool": (8, "minecraft:empty"), "joint": (8, "rollable"),
                           "final_state": (8, "minecraft:stone"), "selection_priority": (3, j[4]),
                           "placement_priority": (3, 0)}
                    blocks.append({"pos": (9, (3, [x, y, z])), "state": (3, 1), "nbt": (10, nbt)})
                elif y == 0:
                    blocks.append({"pos": (9, (3, [x, y, z])), "state": (3, 0)})
    return nbt_gzip({"DataVersion": (3, DATA_VERSION), "size": (9, (3, list(size))),
                     "palette": (9, (10, palette)), "blocks": (9, (10, blocks)), "entities": (9, (10, []))})


STRUCT_TEMPLATES = {
    "tower/a": template((5, 9, 7)),
    "tower/b": template((9, 6, 4)),
    # two jigsaws share the start name; one with a higher selection priority does not
    "camp/a": template((11, 4, 7), ((2, 1, 5, "testpack:anchor", 0), (8, 2, 1, "testpack:anchor", 0),
                                    (5, 1, 3, "testpack:other", 5))),
    "camp/b1": template((3, 3, 3)),
    "camp/b2": template((6, 2, 9)),
    "pillar/a": template((2, 12, 3)),
    "pillar/b": template((1, 1, 1)),
    "ring/marker": template((3, 5, 3)),
}


def single(loc, legacy=False):
    return {"element_type": "minecraft:legacy_single_pool_element" if legacy else "minecraft:single_pool_element",
            "location": "testpack:" + loc, "projection": "rigid", "processors": "minecraft:empty"}


STRUCT_POOLS = {
    "tower/start": [(3, single("tower/a")), (1, single("tower/b"))],
    "camp_a/start": [(1, single("camp/a"))],
    "camp_b/start": [(2, {"element_type": "minecraft:list_pool_element", "projection": "rigid",
                          "elements": [single("camp/b1"), single("camp/b2")]}),
                     (1, {"element_type": "minecraft:empty_pool_element"})],
    "pillar/start": [(1, single("pillar/a")), (2, single("pillar/b")), (1, single("pillar/a", True))],
    "ring/start": [(1, single("ring/marker"))],
}


def jigsaw(pool, height, biomes, **extra):
    o = {"type": "minecraft:jigsaw", "biomes": biomes, "step": "surface_structures", "spawn_overrides": {},
         "start_pool": "testpack:" + pool, "size": 1, "start_height": height, "max_distance_from_center": 32,
         "use_expansion_hack": False}
    o.update(extra)
    return o


STRUCTURES = {
    "tower": jigsaw("tower/start", {"type": "minecraft:uniform", "min_inclusive": {"absolute": 20},
                                    "max_inclusive": {"absolute": 60}}, "#testpack:tower"),
    "camp_a": jigsaw("camp_a/start", {"absolute": 0},
                     ["testpack:outer_reach", "testpack:patch_b", "testpack:ruby_canyon", "minecraft:savanna"],
                     project_start_to_heightmap="WORLD_SURFACE_WG", start_jigsaw_name="testpack:anchor"),
    "camp_b": jigsaw("camp_b/start", {"type": "minecraft:biased_to_bottom", "min_inclusive": {"above_bottom": 10},
                                      "max_inclusive": {"absolute": 40}, "inner": 4},
                     ["testpack:violet_moor", "testpack:hot_scorch", "minecraft:sunflower_plains"]),
    "pillar": jigsaw("pillar/start", {"type": "minecraft:trapezoid", "min_inclusive": {"absolute": -20},
                                      "max_inclusive": {"absolute": 50}, "plateau": 10}, "#testpack:pillar"),
    "ring_marker": jigsaw("ring/start", {"type": "minecraft:very_biased_to_bottom",
                                         "min_inclusive": {"absolute": -30}, "max_inclusive": {"absolute": 30},
                                         "inner": 3}, "#minecraft:is_overworld"),
    "keep": {"type": "minecraft:fortress", "biomes": ["testpack:pearl_coast", "testpack:outer_rim", "minecraft:river"],
             "step": "underground_decoration", "spawn_overrides": {}},
    "hut": {"type": "minecraft:swamp_hut", "biomes": ["testpack:outer_reach", "testpack:equi_q", "minecraft:swamp"],
            "step": "surface_structures", "spawn_overrides": {}},
}


def spread(salt, spacing, separation, structures, **extra):
    pl = {"type": "minecraft:random_spread", "salt": salt, "spacing": spacing, "separation": separation}
    pl.update(extra)
    return {"placement": pl, "structures": [{"structure": "testpack:" + n, "weight": w} for n, w in structures]}


STRUCTURE_SETS = {
    "towers": spread(1001, 10, 3, [("tower", 1)], frequency=0.7),
    "camps": spread(1002, 14, 5, [("camp_a", 2), ("camp_b", 1), ("keep", 1)], spread_type="triangular",
                    frequency=0.5, frequency_reduction_method="legacy_type_1",
                    exclusion_zone={"other_set": "testpack:towers", "chunk_count": 2}),
    "pillars": spread(1003, 6, 2, [("pillar", 1)], frequency=0.3, frequency_reduction_method="legacy_type_2"),
    "huts": spread(1004, 5, 1, [("hut", 1)], frequency=0.25, frequency_reduction_method="legacy_type_3"),
    "rings": {"placement": {"type": "minecraft:concentric_rings", "salt": 0, "distance": 6, "spread": 4,
                            "count": 10, "preferred_biomes": "#testpack:ring_preferred"},
              "structures": [{"structure": "testpack:ring_marker", "weight": 1}]},
}

STRUCT_TAGS = {
    "tower": ["testpack:pearl_coast", "testpack:violet_moor", "testpack:outer_rim", "testpack:cobalt_steppe",
              "minecraft:river"],
    # a tag that includes another one
    "pillar": ["testpack:patch_b", "#testpack:tower"],
    "ring_preferred": ["testpack:violet_moor", "testpack:pearl_coast"],
}


def structure_files():
    """{path relative to the pack: bytes} of the structure part."""
    out = {}
    for name, data in STRUCT_TEMPLATES.items():
        # "structure" since 1.21, "structures" before
        out["data/testpack/structure/%s.nbt" % name] = data
        out["data/testpack/structures/%s.nbt" % name] = data
    for name, elements in STRUCT_POOLS.items():
        out["data/testpack/worldgen/template_pool/%s.json" % name] = pretty(
            {"fallback": "minecraft:empty", "elements": [{"weight": w, "element": e} for w, e in elements]}).encode()
    for name, o in STRUCTURES.items():
        out["data/testpack/worldgen/structure/%s.json" % name] = pretty(o).encode()
    for name, o in STRUCTURE_SETS.items():
        out["data/testpack/worldgen/structure_set/%s.json" % name] = pretty(o).encode()
    for name, values in STRUCT_TAGS.items():
        out["data/testpack/tags/worldgen/biome/%s.json" % name] = pretty({"values": values}).encode()
    return out


def struct_pack_files(base_files):
    """All files of testpack-structures/ that this script writes: the test pack's plus the structures."""
    out = {}
    for root, _, names in os.walk(PACK):
        for n in names:
            if n.startswith("."):
                continue
            rel = os.path.relpath(os.path.join(root, n), PACK).replace(os.sep, "/")
            with open(os.path.join(root, n), "rb") as f:
                out[rel] = f.read()
    for rel, text in base_files.items():
        out[rel] = text.encode()
    out.update(structure_files())
    return out


def probe_files(param):
    """A list of 160 bands of one parameter; the band index is the biome name."""
    d = PARAMS.index(param)
    entries, names = [], []
    for i in range(160):
        box = [(LO, HI)] * 6
        box[d] = (round(LO + i * 0.025, 3), round(LO + (i + 1) * 0.025, 3))
        names.append("probe_%03d" % i)
        entries.append(Entry(box, "testpack:" + names[-1]))
    files = {"data/minecraft/dimension/overworld.json": dimension_json(entries)}
    for n in names:
        files["data/testpack/worldgen/biome/%s.json" % n] = pretty(biome_json(n))
    return files


# ------------------------------------------------------------ legacy pack
#
# testpack-legacy/: a second, small pack (written by --legacy) that replaces
# the Nether and the End by generators of its own with legacy_random_source
# (the game's LegacyRandomSource). Vanilla's own Nether and End only use the
# noises the game replaces by name (minecraft:temperature, minecraft:vegetation,
# minecraft:offset) and the blended noise; this pack also seeds noises of its
# own the generic way (String.hashCode() of the id), uses one of them in the
# Nether's biome list and in the End's erosion (so the biomes measure it), and
# has a flat_cache term outside of "interpolated" in final_density (sampled at
# quart positions). Nothing in it is copied from the game's data.

LEGACY = os.path.join(HERE, "testpack-legacy")


def legacy_files():
    def noise(name, xz, y):
        return {"type": "minecraft:noise", "noise": "testpack_legacy:" + name, "xz_scale": xz, "y_scale": y}

    def add(a, b):
        return {"type": "minecraft:add", "argument1": a, "argument2": b}

    def mul(a, b):
        return {"type": "minecraft:mul", "argument1": a, "argument2": b}

    def grad(y0, y1, v0, v1):
        return {"type": "minecraft:y_clamped_gradient", "from_y": y0, "to_y": y1, "from_value": v0, "to_value": v1}

    def shifted(name, xz):
        return {"type": "minecraft:shifted_noise", "noise": name, "xz_scale": xz, "y_scale": 0.0,
                "shift_x": "minecraft:shift_x", "shift_y": 0.0, "shift_z": "minecraft:shift_z"}

    blended = {"type": "minecraft:old_blended_noise", "xz_scale": 0.3, "y_scale": 0.45,
               "xz_factor": 90.0, "y_factor": 70.0, "smear_scale_multiplier": 6.0}
    zero_router = {k: 0.0 for k in (
        "barrier", "fluid_level_floodedness", "fluid_level_spread", "lava", "temperature", "vegetation",
        "continents", "erosion", "depth", "ridges", "initial_density_without_jaggedness",
        "final_density", "vein_toggle", "vein_ridged", "vein_gap")}

    def settings(router, noise_cfg, block, fluid, sea):
        return {"sea_level": sea, "disable_mob_generation": True, "aquifers_enabled": False,
                "ore_veins_enabled": False, "legacy_random_source": True,
                "default_block": {"Name": block}, "default_fluid": {"Name": fluid},
                "noise": noise_cfg, "noise_router": router, "spawn_target": [],
                "surface_rule": {"type": "minecraft:sequence", "sequence": []}}

    # Nether: a cavern between a floor and a roof, shaped by an own 3D noise
    # and the blended noise (interpolated), plus plateaus of a flat_cache noise
    # and a fine grain noise per block (not interpolated)
    shape = add(add(grad(20, 60, 1.2, -0.9), grad(92, 124, 0.0, 2.4)),
                add(mul(0.7, noise("hills", 1.0, 0.5)), mul(0.5, blended)))
    nether_router = dict(zero_router)
    nether_router.update({
        "temperature": shifted("minecraft:temperature", 0.3),
        "vegetation": shifted("minecraft:vegetation", 0.2),
        "continents": noise("hills", 0.25, 0.0),
        "final_density": add(
            {"type": "minecraft:squeeze", "argument": mul(0.64, {"type": "minecraft:interpolated", "argument": shape})},
            add({"type": "minecraft:flat_cache", "argument": mul(0.25, noise("plateau", 1.0, 0.0))},
                mul(0.05, noise("grain", 2.0, 2.0)))),
    })
    nether_settings = settings(nether_router, {"min_y": 0, "height": 128, "size_horizontal": 1, "size_vertical": 1},
                               "minecraft:netherrack", "minecraft:lava", 32)
    nether_biomes = []
    for biome, t, h, c, off in (
            ("minecraft:nether_wastes", 0.05, 0.0, -0.05, 0.0),
            ("minecraft:soul_sand_valley", 0.0, -0.5, 0.3, 0.0),
            ("minecraft:crimson_forest", 0.4, 0.0, -0.3, 0.0),
            ("minecraft:warped_forest", 0.0, 0.5, 0.4, 0.1),
            ("minecraft:basalt_deltas", -0.5, 0.0, -0.4, 0.05),
            ("minecraft:crimson_forest", -0.2, 0.3, 0.6, 0.0)):
        nether_biomes.append({"biome": biome, "parameters": {
            "temperature": t, "humidity": h, "continentalness": c, "erosion": 0.0, "weirdness": 0.0,
            "depth": 0.0, "offset": off}})
    nether_dim = {"type": "minecraft:the_nether", "generator": {
        "type": "minecraft:noise", "settings": "testpack_legacy:nether",
        "biome_source": {"type": "minecraft:multi_noise", "biomes": nether_biomes}}}

    # End: the vanilla islands, roughened by the own noise; the biomes follow
    # the erosion, which the own noise shifts
    end_router = dict(zero_router)
    end_router.update({
        "erosion": add({"type": "minecraft:cache_2d", "argument": {"type": "minecraft:end_islands"}},
                       mul(0.15, noise("hills", 0.5, 0.0))),
        "final_density": {"type": "minecraft:squeeze", "argument": mul(0.64, {
            "type": "minecraft:interpolated", "argument": add(
                add({"type": "minecraft:end_islands"}, grad(40, 90, 0.25, -0.5)),
                add(mul(0.2, noise("hills", 1.0, 1.0)), mul(0.3, blended)))})},
    })
    end_settings = settings(end_router, {"min_y": 0, "height": 128, "size_horizontal": 2, "size_vertical": 2},
                            "minecraft:end_stone", "minecraft:air", 0)
    end_dim = {"type": "minecraft:the_end", "generator": {
        "type": "minecraft:noise", "settings": "testpack_legacy:end", "biome_source": {"type": "minecraft:the_end"}}}

    noises = {
        # an octave of amplitude 0 in the middle: it is not created, the
        # octaves around it keep their names
        "hills": {"firstOctave": -6, "amplitudes": [1.0, 0.5, 0.0, 0.25]},
        "plateau": {"firstOctave": -4, "amplitudes": [1.0, 1.0]},
        "grain": {"firstOctave": -2, "amplitudes": [1.0]},
    }
    files = {
        "pack.mcmeta": pretty({"pack": {"pack_format": 48, "supported_formats": [41, 48],
                                        "description": "Seed Atlas worldgen test pack: legacy random source "
                                                       "(Nether and End)"}}),
        "data/minecraft/dimension/the_nether.json": pretty(nether_dim),
        "data/minecraft/dimension/the_end.json": pretty(end_dim),
        "data/testpack_legacy/worldgen/noise_settings/nether.json": pretty(nether_settings),
        "data/testpack_legacy/worldgen/noise_settings/end.json": pretty(end_settings),
    }
    for name, n in noises.items():
        files["data/testpack_legacy/worldgen/noise/%s.json" % name] = pretty(n)
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--probe", choices=PARAMS)
    ap.add_argument("--out", help="with --probe: directory of the probe pack")
    ap.add_argument("--structures", action="store_true",
                    help="(re)write testpack-structures/ (the test pack with structures) from testpack/")
    ap.add_argument("--legacy", action="store_true", help="write testpack-legacy/ (Nether and End)")
    args = ap.parse_args()

    if args.legacy:
        if os.path.isdir(LEGACY):
            shutil.rmtree(LEGACY)
        write_files(LEGACY, legacy_files())
        print("testpack-legacy written")
        return 0

    if args.probe:
        if not args.out:
            raise SystemExit("--probe needs --out")
        # only a folder an earlier probe made is replaced
        marker = os.path.join(args.out, ".seed-atlas-testpack")
        if os.path.exists(args.out) and not os.path.isfile(marker):
            raise SystemExit("%s exists and was not made by this tool (no %s): not replaced"
                             % (args.out, os.path.basename(marker)))
        if os.path.isdir(args.out):
            shutil.rmtree(args.out)
        shutil.copytree(PACK, args.out, ignore=shutil.ignore_patterns(".*", "__pycache__"))
        write_files(args.out, probe_files(args.probe))
        # hidden: not part of the pack (pack_files() and the zips leave it out)
        with open(marker, "w") as f:
            f.write("made by tools/worldgen-reference/make_testpack.py --probe\n")
        print("probe pack for %s written to %s" % (args.probe, args.out))
        return 0

    entries = build()
    files = generated_files(entries, testpack_biomes())
    if args.list:
        for i, e in enumerate(entries):
            print("%3d %-12s %s" % (i, e.role, e.json()))
    roles = {}
    for e in entries:
        roles[e.role] = roles.get(e.role, 0) + 1
    print("%d entries (%s), %d different biomes (%d testpack), %d biome files"
          % (len(entries), ", ".join("%s %d" % kv for kv in sorted(roles.items())),
             len({e.biome for e in entries}),
             len({e.biome for e in entries if e.biome.startswith("testpack:")}),
             len(testpack_biomes())), file=sys.stderr)
    if args.structures:
        if os.path.isdir(STRUCT_PACK):
            shutil.rmtree(STRUCT_PACK)
        for rel, data in sorted(struct_pack_files(files).items()):
            path = os.path.join(STRUCT_PACK, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        print("wrote %s" % STRUCT_PACK, file=sys.stderr)
        return 0
    if args.check:
        bad = 0
        for rel, text in files.items():
            path = os.path.join(PACK, *rel.split("/"))
            if not os.path.exists(path) or open(path).read() != text:
                print("differs: %s" % rel)
                bad += 1
        if os.path.isdir(STRUCT_PACK):
            want = struct_pack_files(files)
            have = set()
            for root, _, names in os.walk(STRUCT_PACK):
                for n in names:
                    have.add(os.path.relpath(os.path.join(root, n), STRUCT_PACK).replace(os.sep, "/"))
            for rel in sorted(have | set(want)):
                path = os.path.join(STRUCT_PACK, *rel.split("/"))
                if rel not in want or not os.path.exists(path) or open(path, "rb").read() != want[rel]:
                    print("differs: testpack-structures/%s" % rel)
                    bad += 1
        return 1 if bad else 0
    if not args.list:
        write_files(PACK, files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
