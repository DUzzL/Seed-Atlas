#!/usr/bin/env python3
"""Check that the test packs contain nothing copied from vanilla's worldgen data.

Usage:
  check_no_vanilla.py --vanilla DIR [--vanilla DIR ...] [--min-leaves N] [PACK ...]

DIR is a folder with Mojang's data pack of one version (it holds
data/minecraft/...; a misode/mcmeta "<version>-data" clone or the output of
vanilla_data.py --data). That data stays outside the repository. PACK
defaults to every test pack folder next to this script (testpack*).

Every JSON value of every pack file (.json and .mcmeta) is compared with every
JSON value of every vanilla file: objects and lists at any depth, whole files
included. Before the comparison both sides are normalised: object keys sorted,
numbers as floats, the namespace "minecraft:" removed from every string (also
after a leading "#" of a tag). A value of the pack that also occurs in vanilla
counts as a copy if it holds at least N scalars (numbers, strings, booleans;
default 4). Smaller values are what any pack writes in the same way: a block
state, "block X", a reference, a list of two ids, a marker around a marker
(cache_once(blend_alpha)), [-1.0, 1.0]. A list of numbers only (amplitudes of
a noise, thresholds, a point) counts from 2 * N numbers on: [1.0, 1.0, 1.0, 0.0]
is no structure that could be copied. Only the outermost copied value of a
file is reported. Exit status 1 if anything is reported.
"""

import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def norm(o):
    if isinstance(o, dict):
        return {k: norm(v) for k, v in o.items()}
    if isinstance(o, list):
        return [norm(v) for v in o]
    if isinstance(o, str):
        tag = o.startswith("#")
        s = o[1:] if tag else o
        if s.startswith("minecraft:"):
            s = s[len("minecraft:"):]
        return ("#" if tag else "") + s
    if isinstance(o, bool) or o is None:
        return o
    return float(o)


def leaves(o):
    if isinstance(o, dict):
        return sum(leaves(v) for v in o.values())
    if isinstance(o, list):
        return sum(leaves(v) for v in o)
    return 1


def counts(v, min_leaves):
    """Whether a value is big enough to count as a copy."""
    n = leaves(v)
    if isinstance(v, list) and all(isinstance(x, float) for x in v):
        return n >= 2 * min_leaves
    return n >= min_leaves


def key(o):
    return json.dumps(o, sort_keys=True, separators=(",", ":"))


def values(o, path=""):
    """(path, value) of o and of every object and list inside it, outermost first."""
    yield path, o
    if isinstance(o, dict):
        for k, v in o.items():
            if isinstance(v, (dict, list)):
                yield from values(v, path + "/" + k)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            if isinstance(v, (dict, list)):
                yield from values(v, "%s[%d]" % (path, i))


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def index_vanilla(root, min_leaves, out):
    files = 0
    for folder, _, names in os.walk(os.path.join(root, "data")):
        for n in names:
            if not n.endswith(".json"):
                continue
            p = os.path.join(folder, n)
            try:
                o = norm(load(p))
            except ValueError:
                continue
            files += 1
            rel = os.path.relpath(p, root)
            for sp, v in values(o):
                if isinstance(v, (dict, list)) and counts(v, min_leaves):
                    out.setdefault(key(v), "%s:%s" % (rel, sp or "/"))
    return files


def check_pack(pack, index, min_leaves):
    hits = []
    for folder, dirs, names in os.walk(pack):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for n in sorted(names):
            if not n.endswith((".json", ".mcmeta")):
                continue
            p = os.path.join(folder, n)
            o = norm(load(p))
            covered = []
            for sp, v in values(o):
                if any(sp == c or sp.startswith(c + "/") or sp.startswith(c + "[") for c in covered):
                    continue
                if not isinstance(v, (dict, list)) or not counts(v, min_leaves):
                    continue
                where = index.get(key(v))
                if where:
                    covered.append(sp)
                    hits.append("%s:%s (%d scalars) == vanilla %s"
                                % (os.path.relpath(p, os.path.dirname(pack)), sp or "/", leaves(v), where))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vanilla", action="append", required=True, help="vanilla data folder (holds data/minecraft)")
    ap.add_argument("--min-leaves", type=int, default=4)
    ap.add_argument("packs", nargs="*")
    args = ap.parse_args()
    packs = args.packs or sorted(p for p in glob.glob(os.path.join(HERE, "testpack*")) if os.path.isdir(p))
    index = {}
    for v in args.vanilla:
        if not os.path.isdir(os.path.join(v, "data", "minecraft")):
            raise SystemExit("%s: no data/minecraft folder" % v)
        n = index_vanilla(v, args.min_leaves, index)
        print("%s: %d files" % (v, n))
    print("vanilla values with at least %d scalars: %d" % (args.min_leaves, len(index)))
    bad = 0
    for pack in packs:
        hits = check_pack(pack, index, args.min_leaves)
        for h in hits:
            print("  " + h)
        print("%s: %d copied values" % (os.path.basename(os.path.normpath(pack)), len(hits)))
        bad += len(hits)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
