#!/usr/bin/env python3
"""The worldgen test pack in the data format of Minecraft 26.3 (testpack26/).

26.3 renamed most density function types and fields, replaced the noise
format, changed the noise settings and the biome format, and evaluates
everything in single precision. This script derives testpack26/ from the
hand-written test pack testpack/ (see README.md, "Test pack"):

  * the same biome list (dimension file), biome ids, tags and colours;
  * the density functions translated to the 26.3 names and fields
    (argument -> input, argument1/2 -> left/right, y_clamped_gradient ->
    gradient, flat_cache -> cache of a slice at y 0, the other caches ->
    cache, shifted_noise -> noise with shifts, weird_scaled_sampler -> abs of
    an interval_select like vanilla 26.3 writes it, ...);
  * the noises in the 26.3 format (base_octave, octave_count,
    amplitude_modifiers, base_amplitude chosen like the game's own
    conversion, so that the values stay close to the old ones);
  * terms that use the types and options new in 26.3 (sub, div, pow, log,
    sqrt, sign, negate, lerp, floor/round/ceil/truncate, slice,
    distance_to_point, gradient tilings, interval_select, a noise with
    normalize false, one with "legacy", one with a scale that is not a power
    of two) added to the vegetation and erosion functions;
  * pack.mcmeta with min_format / max_format and overlays in the new notation
    (one that applies to 26.3 and one that must not: its minimum is 121.1).

Nothing in here is copied from the game's data (check_no_vanilla.py checks
the result); vanilla resources are only named by id (the material rule
minecraft:bedrock_floor here, vanilla's density functions through testpack/).
RARITY holds what the two mappers of 1.21's weird_scaled_sampler do, in the
notation of 26.3.

Usage:
  make_testpack26.py            rewrite testpack26/
  make_testpack26.py --check    compare with testpack26/ (exit 1 on a difference)
"""

import argparse
import json
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "testpack")
DST = os.path.join(HERE, "testpack26")

# cell size of the test pack's noise settings (size_horizontal 2, size_vertical 1)
CELL_XZ, CELL_Y = 8, 4


# ------------------------------------------------------------------ noises

def parity_base_amplitude(first_octave, amps):
    """NormalNoise.computeParityBaseAmplitude() of 26.3: the base amplitude
    that gives the octaves the amplitudes of the old NormalNoise."""
    n = len(amps)
    amp = 1.0 * math.pow(0.5, -(n - 1)) / (math.pow(0.5, -n) - 1.0)
    octs = []
    for a in amps:
        if a != 0.0:
            octs.append(amp * a)
        amp *= 0.5
    s = math.fsum(abs(o) for o in octs)
    dev = math.sqrt(sum((0.2702247831245211 * abs(o)) ** 2 for o in octs))
    if dev == 0.0:
        return 1.0
    nf = (s * 0.3333333333333333) / (dev * math.sqrt(2.0))
    idx = [i for i, a in enumerate(amps) if a != 0.0]
    pdev = 0.1 * (1.0 + 1.0 / float(idx[-1] - idx[0] + 1))
    return (1.0 * 0.5 * 0.3333333333333333 / pdev) / nf


def convert_noise(old, normalize=None):
    amps = old["amplitudes"]
    if any(a < 0 for a in amps):
        raise SystemExit("a noise with a negative amplitude cannot be converted")
    out = {
        "base_octave": old["firstOctave"],
        "octave_count": len(amps),
        "amplitude_modifiers": amps,
        "base_amplitude": parity_base_amplitude(old["firstOctave"], amps),
    }
    if normalize is not None:
        out["normalize"] = normalize
    return out


# -------------------------------------------------------- density functions

RARITY = {
    "type_1": ([-0.5, 0.0, 0.5], [0.75, 1.0, 1.5, 2.0]),
    "type_2": ([-0.75, -0.5, 0.5, 0.75], [0.5, 0.75, 1.0, 2.0, 3.0]),
}


def convert_df(v):
    """A 1.21 density function (reference, number or object) in 26.3 form."""
    if not isinstance(v, dict):
        return v
    t = v["type"].replace("minecraft:", "")
    c = convert_df
    if t in ("add", "mul", "min", "max"):
        return {"type": "minecraft:" + t, "left": c(v["argument1"]), "right": c(v["argument2"])}
    if t in ("abs", "square", "cube", "half_negative", "quarter_negative", "squeeze"):
        return {"type": "minecraft:" + t, "input": c(v["argument"])}
    if t == "invert":
        return {"type": "minecraft:reciprocal", "input": c(v["argument"])}
    if t == "constant":
        return {"type": "minecraft:constant", "value": v["argument"]}
    if t == "y_clamped_gradient":
        return {"type": "minecraft:gradient", "axis": "y", "from_coordinate": v["from_y"],
                "to_coordinate": v["to_y"], "from_value": v["from_value"], "to_value": v["to_value"]}
    if t == "flat_cache":
        # the old flat cache sampled its argument at y 0
        return {"type": "minecraft:cache",
                "input": {"type": "minecraft:slice", "axis": "y", "coordinate": 0, "input": c(v["argument"])}}
    if t in ("cache_2d", "cache_once", "cache_all_in_cell"):
        return {"type": "minecraft:cache", "input": c(v["argument"])}
    if t == "interpolated":
        return {"type": "minecraft:interpolated", "input": c(v["argument"]),
                "cell_size_xz": CELL_XZ, "cell_size_y": CELL_Y}
    if t == "blend_density":
        return {"type": "minecraft:blend_density", "input": c(v["argument"])}
    if t == "noise":
        return {"type": "minecraft:noise", "noise": v["noise"], "xz_scale": v["xz_scale"], "y_scale": v["y_scale"]}
    if t == "shifted_noise":
        out = {"type": "minecraft:noise", "noise": v["noise"], "xz_scale": v["xz_scale"], "y_scale": v["y_scale"]}
        for k in ("shift_x", "shift_y", "shift_z"):
            out[k] = c(v[k])
        return out
    if t in ("shift_a", "shift_b", "shift"):
        return {"type": "minecraft:" + t, "noise": v["argument"]}
    if t == "weird_scaled_sampler":
        th, es = RARITY[v["rarity_value_mapper"]]
        fs = [{"type": "minecraft:mul", "left": e,
               "right": {"type": "minecraft:noise", "noise": v["noise"], "xz_scale": 1.0 / e, "y_scale": 1.0 / e}}
              for e in es]
        return {"type": "minecraft:abs",
                "input": {"type": "minecraft:interval_select", "input": c(v["input"]),
                          "thresholds": th, "functions": fs}}
    if t == "clamp":
        return {"type": "minecraft:clamp", "input": c(v["input"]), "min": v["min"], "max": v["max"]}
    if t == "range_choice":
        return {"type": "minecraft:range_choice", "input": c(v["input"]),
                "min_inclusive": v["min_inclusive"], "max_exclusive": v["max_exclusive"],
                "when_in_range": c(v["when_in_range"]), "when_out_of_range": c(v["when_out_of_range"])}
    if t == "spline":
        return {"type": "minecraft:spline", "spline": convert_spline(v["spline"])}
    if t in ("blend_alpha", "blend_offset", "beardifier", "old_blended_noise"):
        return dict(v)
    if t == "end_islands":
        return {"type": "minecraft:end_outer_islands"}
    raise SystemExit("no translation for the density function type " + t)


def convert_spline(s):
    if not isinstance(s, dict):
        return s
    return {"coordinate": convert_df(s["coordinate"]),
            "points": [{"location": p["location"], "derivative": p["derivative"],
                        "value": convert_spline(p["value"])} for p in s["points"]]}


def noise(n, xz, y, **shifts):
    out = {"type": "minecraft:noise", "noise": n, "xz_scale": xz, "y_scale": y}
    out.update(shifts)
    return out


def op(t, **kw):
    out = {"type": "minecraft:" + t}
    out.update(kw)
    return out


# Terms with the types and options that are new in 26.3. Each stays within
# about +-1 and is added with a small weight, so the climate keeps the shape
# the test pack's list was designed for.
NEW_FUNCTIONS = {
    # sub, div (general and by a constant), pow, sqrt, log, sign, negate, lerp
    "new26/arith": op("lerp",
        alpha=op("clamp", input=op("add", left=0.5, right=noise("testpack:legacy", 0.37, 0.0)), min=0.0, max=1.0),
        first=op("sub",
            left=op("pow", base=op("abs", input=noise("testpack:plain", 0.25, 0.0)), exponent=1.7),
            right=op("div", left=op("log", input=op("add", left=2.0, right=op("abs", input=noise("testpack:plain", 0.11, 0.0)))),
                     right=3.0)),
        second=op("mul",
            left=op("sign", input=noise("testpack:raw", 0.5, 0.0)),
            right=op("negate", input=op("sqrt", input=op("abs", input=op("div",
                left=noise("testpack:raw", 0.31, 0.0),
                right=op("add", left=1.5, right=noise("testpack:legacy", 0.2, 0.0)))))))),
    # rounding, gradients with tiling, distance, slice, interval_select
    "new26/shape": op("add",
        left=op("mul", left=0.4, right=op("floor", input=op("mul", left=3.0, right=noise("testpack:plain", 0.13, 0.0)))),
        right=op("add",
            left=op("round", input=noise("testpack:raw", 0.07, 0.0), multiple=0.25),
            right=op("add",
                left=op("mul", left=op("ceil", input=noise("testpack:plain", 0.09, 0.0)),
                        right=op("truncate", input=op("mul", left=2.5, right=noise("testpack:legacy", 0.05, 0.0)))),
                right=op("add",
                    left=op("gradient", axis="x", tiling="repeat", from_coordinate=-3000, to_coordinate=4000,
                            from_value=-0.5, to_value=0.5),
                    right=op("add",
                        left=op("gradient", axis="z", tiling="mirrored_repeat", from_coordinate=5000,
                                to_coordinate=-2500, from_value=0.4, to_value=-0.4),
                        right=op("add",
                            left=op("div", left=op("distance_to_point", point=[1200, 0, -900], metric="manhattan"),
                                    right=-20000.0),
                            right=op("add",
                                left=op("slice", axis="x", coordinate=777, input=noise("testpack:plain", 0.21, 0.0)),
                                right=op("interval_select", input=noise("testpack:raw", 0.17, 0.0),
                                         thresholds=[-0.4, 0.1, 0.1, 0.6],
                                         functions=[-0.3, 0.2, 0.9, noise("testpack:plain", 0.6, 0.0), 0.5])))))))),
}


# ------------------------------------------------------------- the pack

def build():
    files = {}

    def put(path, obj):
        files[path] = json.dumps(obj, indent=2, sort_keys=True) + "\n"

    def load(path):
        with open(os.path.join(SRC, path)) as f:
            return json.load(f)

    put("pack.mcmeta", {
        "pack": {
            "description": "Seed Atlas worldgen test pack in the format of 26.3",
            "min_format": [121, 0],
            "max_format": 121,
        },
        "overlays": {"entries": [
            {"directory": "overlay_26a", "min_format": [121, 0], "max_format": [121, 0]},
            {"directory": "overlay_26b", "min_format": [121, 1], "max_format": 121},
        ]},
    })

    # resources of the base pack
    for root, _, names in os.walk(os.path.join(SRC, "data")):
        for name in sorted(names):
            full = os.path.join(root, name)
            rel = os.path.relpath(full, SRC).replace(os.sep, "/")
            obj = load(rel)
            if "/worldgen/density_function/" in rel:
                obj = convert_df(obj)
            elif "/worldgen/noise/" in rel:
                obj = convert_noise(obj)
            elif "/worldgen/noise_settings/" in rel:
                obj = convert_settings(obj)
            elif "/worldgen/biome/" in rel and "/tags/" not in rel:
                obj = convert_biome(obj)
            put(rel, obj)

    # noises with the new options
    put("data/testpack/worldgen/noise/plain.json",
        {"base_octave": -7, "octave_count": 3, "amplitude_modifiers": [1.0, 0.0, 0.7]})
    put("data/testpack/worldgen/noise/raw.json",
        {"base_octave": -6, "octave_count": 2, "normalize": False, "base_amplitude": 0.8})
    put("data/testpack/worldgen/noise/legacy.json",
        {"base_octave": -8, "octave_count": 4, "amplitude_modifiers": [1.0, 0.5, 0.0, 0.25],
         "normalize": "legacy", "base_amplitude": 1.3})
    for name, fn in NEW_FUNCTIONS.items():
        put("data/testpack/worldgen/density_function/%s.json" % name, fn)

    # the climate functions with the new terms
    veg = "data/testpack/worldgen/density_function/climate/vegetation.json"
    put(veg, op("add", left=json.loads(files[veg]),
                right=op("mul", left=0.12, right="testpack:new26/arith")))
    ero = "data/testpack/worldgen/density_function/climate/erosion.json"
    put(ero, op("add", left=json.loads(files[ero]),
                right=op("mul", left=0.08, right="testpack:new26/shape")))

    # overlays: overlay_26a applies to 26.3 (121.0), overlay_26b does not
    put("overlay_26a/data/testpack/worldgen/density_function/new26/bias.json", 0.05)
    put("overlay_26b/data/testpack/worldgen/density_function/new26/bias.json", 0.9)
    put("data/testpack/worldgen/density_function/new26/bias.json", -0.6)
    tmp = "data/testpack/worldgen/density_function/climate/temperature.json"
    put(tmp, op("add", left=json.loads(files[tmp]), right="testpack:new26/bias"))
    return files


def convert_settings(old):
    r = old["noise_router"]
    router = {k: convert_df(r[k]) for k in ("temperature", "vegetation", "continents", "erosion", "depth", "ridges",
                                             "final_density")}
    router["chunk_surface_level"] = 64.0
    return {
        "sea_level": old["sea_level"],
        "disable_mob_generation": old["disable_mob_generation"],
        "legacy_random_source": old["legacy_random_source"],
        "default_block": old["default_block"]["Name"],
        "default_fluid": old["default_fluid"]["Name"],
        "noise": {"min_y": old["noise"]["min_y"], "height": old["noise"]["height"]},
        "noise_router": router,
        "material_rule": "minecraft:bedrock_floor",
        "spawn_target": [{"testpack:climate/continents": [0.05, 1.0], "testpack:climate/erosion": [-0.8, 1.0]}],
    }


def convert_biome(old):
    return {
        "has_precipitation": old["has_precipitation"],
        "temperature": old["temperature"],
        "downfall": old["downfall"],
        "effects": {"water_color": "#%06x" % old["effects"]["water_color"]},
        "carvers": [],
        "features": [],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    files = build()
    if args.check:
        bad = 0
        for path, text in files.items():
            p = os.path.join(DST, path)
            if not os.path.isfile(p) or open(p).read() != text:
                print("differs: " + path)
                bad += 1
        for root, _, names in os.walk(DST):
            for name in names:
                rel = os.path.relpath(os.path.join(root, name), DST).replace(os.sep, "/")
                if rel not in files:
                    print("extra: " + rel)
                    bad += 1
        sys.exit(1 if bad else 0)
    if os.path.isdir(DST):
        shutil.rmtree(DST)
    for path, text in files.items():
        p = os.path.join(DST, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text)
    print("wrote %d files to %s" % (len(files), DST))


if __name__ == "__main__":
    main()
