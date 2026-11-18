#!/usr/bin/env python3
"""Mutated copies of the worldgen test pack (testpack/), and their comparison.

Every variant changes one thing of the pack. A world generated with a variant
is compared with the same world generated with the unmodified pack: the share
of B lines that differ shows whether that part of the pack has consequences
in the dumped areas. Each variant carries an expectation:

  differ   the part must matter (if it does not, the pack does not test it)
  same     vanilla must ignore the change (a statement about vanilla)
  observe  no expectation, the result is the finding

Usage:
  testpack_variants.py list
  testpack_variants.py make <out root> <name>...     writes <out root>/<name>/ (a pack folder)
  testpack_variants.py compare <base file> <file>... B lines that differ from the base file
  testpack_variants.py stats [<data file>...]        what the data files (default: data/testpack_*.txt)
                                                     contain: share of testpack biomes per y, biomes of
                                                     the list that never appear, the tie pairs

Generating the worlds is done with worldgen_reference.py --datapack, see
README.md ("Evidence that every part matters").
"""
import json
import math
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, "testpack")

DF = "data/testpack/worldgen/density_function/"


def load(root, rel):
    return json.load(open(os.path.join(root, rel)))


def save(root, rel, obj):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(obj, open(path, "w"), indent=2)


def walk(o, fn):
    """Rebuild o bottom-up, passing every dict through fn."""
    if isinstance(o, dict):
        return fn({k: walk(v, fn) for k, v in o.items()})
    if isinstance(o, list):
        return [walk(v, fn) for v in o]
    return o


def edit(root, rel, fn, must_change=True):
    old = load(root, rel)
    new = walk(old, fn)
    if must_change and new == old:
        raise SystemExit("mutation did not change %s" % rel)
    save(root, rel, new)


def unwrap(type_name, key="argument"):
    """Replace every function of this type by its argument."""
    return lambda o: o[key] if o.get("type") == "minecraft:" + type_name else o


def const(type_name, value):
    return lambda o: value if o.get("type") == "minecraft:" + type_name else o


def mcmeta(root, fn):
    m = load(root, "pack.mcmeta")
    fn(m)
    save(root, "pack.mcmeta", m)


def spline_edit(fn):
    def f(o):
        if "location" in o and "derivative" in o:
            return fn(o)
        return o
    return f


def zero_derivatives(o):
    o = dict(o)
    o["derivative"] = 0.0
    return o


def flatten_nested(o):
    """Replace nested splines by a constant."""
    o = dict(o)
    if isinstance(o["value"], dict):
        o["value"] = 0.1
    return o


def set_key(type_name, key, value):
    def f(o):
        if o.get("type") == "minecraft:" + type_name:
            o = dict(o)
            o[key] = value
        return o
    return f


V = {}


def variant(name, expect):
    def deco(fn):
        V[name] = (expect, fn)
        return fn
    return deco


# ---- overlays
variant("no_overlays", "differ")(lambda r: mcmeta(r, lambda m: m.pop("overlays")))
variant("overlays_swapped", "differ")(lambda r: mcmeta(r, lambda m: m["overlays"]["entries"].reverse()))
variant("overlay_a_only", "differ")(lambda r: mcmeta(r, lambda m: m["overlays"]["entries"].pop(2)))
variant("overlay_b_only", "differ")(lambda r: mcmeta(r, lambda m: m["overlays"]["entries"].pop(1)))
variant("overlay_old_removed", "same")(lambda r: mcmeta(r, lambda m: m["overlays"]["entries"].pop(0)))


@variant("overlay_old_applies", "differ")
def _(r):
    mcmeta(r, lambda m: m["overlays"]["entries"][0].update(formats=[41, 48]))


# Both applicable overlays replace depth/terrace.json: only the copy of the one listed last counts.
variant("overlay_a_terrace_removed", "same")(lambda r: os.remove(os.path.join(r, "overlay_a", DF + "depth/terrace.json")))
variant("overlay_b_terrace_removed", "differ")(lambda r: os.remove(os.path.join(r, "overlay_b", DF + "depth/terrace.json")))

# ---- markers
@variant("flat_cache_at_y0", "same")
def _(r):
    # continents/local with its argument rewritten to what it is at y = 0
    save(r, DF + "continents/local.json", {"type": "minecraft:flat_cache", "argument": {
        "type": "minecraft:add",
        "argument1": {"type": "minecraft:mul", "argument1": 1.0, "argument2": {
            "type": "minecraft:noise", "noise": "testpack:single", "xz_scale": 0.75, "y_scale": 0.0}},
        "argument2": {"type": "minecraft:mul", "argument1": 0.45, "argument2": -1.0 + (64.0 / 384.0) * 2.0}}})


variant("flat_cache_removed", "differ")(lambda r: edit(r, DF + "continents/local.json", unwrap("flat_cache")))
variant("cache_2d_removed", "same")(lambda r: [edit(r, DF + f, unwrap("cache_2d")) for f in (
    "temperature/lapse.json", "depth/offset.json", "shift/x.json", "shift/z.json")])
variant("cache_once_removed", "same")(lambda r: [edit(r, DF + f, unwrap("cache_once")) for f in (
    "temperature/lapse.json", "depth/offset.json")])
variant("interpolated_removed", "same")(lambda r: edit(r, DF + "vegetation/volume.json", unwrap("interpolated")))
variant("cache_all_in_cell_removed", "same")(lambda r: edit(r, DF + "vegetation/tubes.json", unwrap("cache_all_in_cell")))
variant("blend_alpha_is_1", "same")(lambda r: [edit(r, DF + f, const("blend_alpha", 1.0)) for f in (
    "temperature/lapse.json", "depth/offset.json")])
variant("blend_alpha_is_0", "differ")(lambda r: [edit(r, DF + f, const("blend_alpha", 0.0)) for f in (
    "temperature/lapse.json", "depth/offset.json")])
# (only temperature/lapse uses blend_offset)
variant("blend_offset_is_0", "same")(lambda r: edit(r, DF + "temperature/lapse.json", const("blend_offset", 0.0)))
variant("blend_offset_is_01", "differ")(lambda r: edit(r, DF + "temperature/lapse.json", const("blend_offset", 0.1)))

# ---- one-argument functions replaced by their argument
variant("abs_removed", "differ")(lambda r: edit(r, DF + "climate/ridges.json", unwrap("abs")))
variant("square_removed", "differ")(lambda r: edit(r, DF + "erosion/floor.json", unwrap("square")))
variant("cube_removed", "differ")(lambda r: edit(r, DF + "erosion/base.json", unwrap("cube")))
variant("half_negative_removed", "differ")(lambda r: edit(r, DF + "climate/vegetation.json", unwrap("half_negative")))
# (blob is quarter_negative(<reference>): its argument alone would be a file that is only a reference)
variant("quarter_negative_removed", "differ")(lambda r: edit(r, DF + "erosion/blob.json", lambda o: {
    "type": "minecraft:add", "argument1": 0.0, "argument2": o["argument"]} if o.get("type") == "minecraft:quarter_negative" else o))
variant("squeeze_removed", "differ")(lambda r: edit(r, DF + "climate/vegetation.json", unwrap("squeeze")))
variant("clamp_removed", "differ")(lambda r: edit(r, DF + "climate/temperature.json", unwrap("clamp", "input")))
# ---- two-argument functions
variant("min_removed", "differ")(lambda r: edit(r, DF + "climate/erosion.json", unwrap("min", "argument1")))
variant("max_removed", "differ")(lambda r: edit(r, DF + "climate/erosion.json", unwrap("max", "argument1")))
# (a file that is only a reference to another function does not load: the out-of-range branch is wrapped in an add)
variant("range_choice_erosion_removed", "differ")(lambda r: edit(r, DF + "erosion/choice.json", lambda o: {
    "type": "minecraft:add", "argument1": 0.0, "argument2": o["when_out_of_range"]} if o.get("type") == "minecraft:range_choice" else o))
variant("range_choice_y_removed", "differ")(lambda r: edit(r, DF + "temperature/lapse.json", unwrap("range_choice", "when_out_of_range")))
# upper bound of the y range made inclusive for the sampled level 128
variant("range_choice_y_max_inclusive", "differ")(lambda r: edit(r, DF + "temperature/lapse.json", set_key("range_choice", "max_exclusive", 129.0)))
variant("range_choice_y_min_exclusive", "differ")(lambda r: edit(r, DF + "temperature/lapse.json", set_key("range_choice", "min_inclusive", 65.0)))


@variant("depth_gradient_unclamped", "differ")
def _(r):
    # the same line without the clamps at y -32 and 256
    def f(o):
        if o.get("type") == "minecraft:y_clamped_gradient":
            slope = (o["to_value"] - o["from_value"]) / (o["to_y"] - o["from_y"])
            o = dict(o, from_y=-4064, to_y=4062,
                     from_value=o["from_value"] + slope * (-4064 - o["from_y"]),
                     to_value=o["from_value"] + slope * (4062 - o["from_y"]))
        return o
    edit(r, DF + "climate/depth.json", f)


# ---- splines
variant("spline_derivatives_zero", "differ")(lambda r: edit(r, DF + "ridges/spline.json", spline_edit(zero_derivatives)))
variant("spline_not_nested", "differ")(lambda r: edit(r, DF + "ridges/spline.json", spline_edit(flatten_nested)))
variant("terrace_derivatives_zero", "differ")(lambda r: edit(r, "overlay_b/" + DF + "depth/terrace.json", spline_edit(zero_derivatives)))

# ---- references to vanilla's own density functions
def subst(o, ref, value):
    if isinstance(o, dict):
        return {k: subst(v, ref, value) for k, v in o.items()}
    if isinstance(o, list):
        return [subst(v, ref, value) for v in o]
    return value if o == ref else o


def unref(rel, ref):
    def f(r):
        old = load(r, DF + rel)
        new = subst(old, ref, 0.0)
        if new == old:
            raise SystemExit("no reference to %s in %s" % (ref, rel))
        save(r, DF + rel, new)
    return f


variant("vanilla_continents_removed", "differ")(unref("climate/continents.json", "minecraft:overworld/continents"))
variant("vanilla_erosion_removed", "differ")(unref("erosion/base.json", "minecraft:overworld/erosion"))
variant("vanilla_offset_removed", "differ")(unref("depth/offset.json", "minecraft:overworld/offset"))

# ---- noises
variant("temperature_override_removed", "differ")(lambda r: os.remove(os.path.join(r, "data/minecraft/worldgen/noise/temperature.json")))
# the same octaves without the leading zero amplitude: only the scale of the values differs
variant("wide_without_leading_zero", "differ")(lambda r: save(r, "data/testpack/worldgen/noise/wide.json",
                                                             {"firstOctave": -8, "amplitudes": [1.0, 1.0, 2.0, 0.0, 1.0]}))
variant("volume_without_leading_zeros", "differ")(lambda r: save(r, "data/testpack/worldgen/noise/volume.json",
                                                                {"firstOctave": -6, "amplitudes": [1.0, 1.0]}))
variant("fold_without_trailing_zeros", "differ")(lambda r: save(r, "data/testpack/worldgen/noise/fold.json",
                                                               {"firstOctave": -7, "amplitudes": [1.0, 0.5]}))
variant("shift_without_trailing_zero", "differ")(lambda r: save(r, "data/testpack/worldgen/noise/shift.json",
                                                               {"firstOctave": -6, "amplitudes": [1.0, 1.0, 1.0]}))
variant("temperature_shift_removed", "differ")(lambda r: edit(r, DF + "climate/temperature.json", lambda o: dict(o, shift_x=0.0, shift_z=0.0) if o.get("type") == "minecraft:shifted_noise" else o))
variant("warp_shift_x_removed", "differ")(lambda r: edit(r, DF + "ridges/warped.json", lambda o: dict(o, shift_x=0.0) if "shift_x" in o else o))
variant("warp_shift_y_removed", "differ")(lambda r: edit(r, DF + "ridges/warped.json", lambda o: dict(o, shift_y=0.0) if "shift_y" in o else o))
variant("warp_shift_z_removed", "differ")(lambda r: edit(r, DF + "ridges/warped.json", lambda o: dict(o, shift_z=0.0) if "shift_z" in o else o))
variant("shift_a_b_swapped", "differ")(lambda r: [edit(r, DF + "shift/x.json", lambda o: dict(o, type="minecraft:shift_b") if o.get("type") == "minecraft:shift_a" else o),
                                                  edit(r, DF + "shift/z.json", lambda o: dict(o, type="minecraft:shift_a") if o.get("type") == "minecraft:shift_b" else o)])
variant("wss_type_1_as_2", "differ")(lambda r: edit(r, DF + "vegetation/tubes.json", set_key("weird_scaled_sampler", "rarity_value_mapper", "type_2")))
variant("wss_type_2_as_1", "differ")(lambda r: edit(r, DF + "erosion/ceiling.json", set_key("weird_scaled_sampler", "rarity_value_mapper", "type_1")))
# erosion/blob uses vanilla's blended noise by reference (minecraft:overworld/base_3d_noise)
variant("old_blended_noise_zero", "differ")(unref("erosion/blob.json", "minecraft:overworld/base_3d_noise"))
# the reference replaced by a blended noise of the pack's own (other scales and factors, smear 6)
variant("old_blended_noise_own", "differ")(lambda r: save(r, DF + "erosion/blob.json", {
    "type": "minecraft:quarter_negative", "argument": {
        "type": "minecraft:old_blended_noise", "xz_scale": 0.3, "y_scale": 0.2, "xz_factor": 96.0,
        "y_factor": 120.0, "smear_scale_multiplier": 6.0}}))
# ---- things that must not matter for the biomes
variant("tags_removed", "same")(lambda r: shutil.rmtree(os.path.join(r, "data/minecraft/tags")))
variant("colours_removed", "same")(lambda r: shutil.rmtree(os.path.join(r, "data/c")))

# ---- overlay that only applies to pack format 41 (1.20.5 / 1.20.6)
variant("overlay_41_removed", "same")(lambda r: mcmeta(r, lambda m: m["overlays"]["entries"].pop(3)))
variant("overlay_41_applies", "differ")(lambda r: mcmeta(r, lambda m: m["overlays"]["entries"][3].update(formats=[41, 48])))
# listed first instead of last: on 1.20.6 overlay_b then wins the file both replace
variant("overlay_41_first", "same")(lambda r: mcmeta(r, lambda m: m["overlays"]["entries"].insert(0, m["overlays"]["entries"].pop(3))))

# ---- temperature plateaus and the quantisation
variant("clamp_min_is_-1", "differ")(lambda r: edit(r, DF + "climate/temperature.json", set_key("clamp", "min", -1.0)))
variant("clamp_max_is_0.5699", "differ")(lambda r: edit(r, DF + "climate/temperature.json", set_key("clamp", "max", 0.5699)))
# flat_cache instead of cache_2d around a function of y
variant("lapse_cache_2d_as_flat_cache", "differ")(lambda r: edit(r, DF + "temperature/lapse.json",
    lambda o: dict(o, type="minecraft:flat_cache") if o.get("type") == "minecraft:cache_2d" else o))

# ---- the biome list
DIM = "data/minecraft/dimension/overworld.json"
PARAMS = ("temperature", "humidity", "continentalness", "erosion", "weirdness", "depth")


def dim_edit(r, fn, must_change=True):
    d = load(r, DIM)
    old = d["generator"]["biome_source"]["biomes"]
    new = fn(json.loads(json.dumps(old)))
    if must_change and new == old:
        raise SystemExit("list mutation changed nothing")
    d["generator"]["biome_source"]["biomes"] = new
    save(r, DIM, d)


def each_bound(biomes, fn, params=PARAMS):
    for b in biomes:
        for p in params:
            v = b["parameters"][p]
            b["parameters"][p] = [fn(x) for x in v] if isinstance(v, list) else fn(v)
    return biomes


def setall(biomes, key, value):
    for b in biomes:
        b["parameters"][key] = value
    return biomes


def widen(biomes):
    for b in biomes:
        for p in PARAMS:
            v = b["parameters"][p]
            if not isinstance(v, list):
                b["parameters"][p] = [round(v - 0.03, 4), round(v + 0.03, 4)]
    return biomes


def swap_names(biomes, pairs):
    m = {}
    for a, b in pairs:
        m[a], m[b] = b, a
    for b in biomes:
        b["biome"] = m.get(b["biome"], b["biome"])
    return biomes


def swap_positions(biomes, a, b):
    i = [k for k, e in enumerate(biomes) if e["biome"] == a]
    j = [k for k, e in enumerate(biomes) if e["biome"] == b]
    assert len(i) == 1 and len(j) == 1
    biomes[i[0]], biomes[j[0]] = biomes[j[0]], biomes[i[0]]
    return biomes



def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def requant(mode):
    """A bound moved to where a wrong quantisation (floor / round half away) would put it."""
    def f(x):
        p = f32(f32(x) * 10000.0)
        good = int(p)
        bad = math.floor(p) if mode == "floor" else int(math.copysign(math.floor(abs(p) + 0.5), p))
        return x if bad == good else bad / 10000.0
    return f


variant("list_offsets_zero", "differ")(lambda r: dim_edit(r, lambda b: setall(b, "offset", 0.0)))
variant("list_overlaps_removed", "differ")(lambda r: dim_edit(r, lambda b: [e for e in b if not e["biome"].startswith("testpack:patch_")]))
variant("list_scalars_widened", "differ")(lambda r: dim_edit(r, widen))
variant("list_bounds_clipped", "differ")(lambda r: dim_edit(r, lambda b: each_bound(b, lambda x: max(-1.0, min(1.0, x)))))
variant("list_depth_opened", "differ")(lambda r: dim_edit(r, lambda b: setall(b, "depth", [-2.0, 2.0])))
variant("list_ties_swapped", "observe")(lambda r: dim_edit(r, lambda b: swap_names(b, [("testpack:tie%d_a" % i, "testpack:tie%d_b" % i) for i in range(1, 7)])))
variant("list_equi_swapped", "observe")(lambda r: dim_edit(r, lambda b: swap_positions(b, "testpack:equi_p", "testpack:equi_q")))
variant("list_reversed", "observe")(lambda r: dim_edit(r, lambda b: b[::-1]))


def all_twins(r):
    """Every ordinary entry gets an identical twin directly behind it, biome testpack:second_twin."""
    special = ("testpack:tie", "testpack:equi_", "testpack:unreachable_")
    def f(biomes):
        out = []
        for e in biomes:
            out.append(e)
            if not e["biome"].startswith(special):
                out.append({"biome": "testpack:second_twin", "parameters": json.loads(json.dumps(e["parameters"]))})
        return out
    dim_edit(r, f)
    src = os.path.join(r, "data/testpack/worldgen/biome")
    shutil.copyfile(os.path.join(src, "unreachable_cold.json"), os.path.join(src, "second_twin.json"))


# testpack:second_twin appears wherever vanilla prefers the second of two identical entries
variant("list_all_twins", "observe")(all_twins)
variant("list_bounds_floor", "observe")(lambda r: dim_edit(r, lambda b: each_bound(b, requant("floor"))))
variant("list_bounds_round", "observe")(lambda r: dim_edit(r, lambda b: each_bound(b, requant("round"), PARAMS + ("offset",))))


def load_b(path):
    b = {}
    with open(path) as f:
        for line in f:
            if line[0] == "B":
                _, x, y, z, n = line.split()
                b[(int(x), int(y), int(z))] = n
    return b


def compare(base_path, paths):
    base = load_b(base_path)
    for p in paths:
        o = load_b(p)
        if set(o) != set(base):
            raise SystemExit("%s does not cover the same cells as %s" % (p, base_path))
        diff = [k for k in base if base[k] != o[k]]
        per_y = {}
        for k in diff:
            per_y[k[1]] = per_y.get(k[1], 0) + 1
        name = os.path.basename(p)
        for prefix in ("v_",):
            if name.startswith(prefix):
                name = name[len(prefix):]
        name = name.rsplit("_", 2)[0]   # drop _<mc>_<seed>.txt
        print("%-32s %-8s %5d of %d (%5.1f %%)  per y: %s" % (
            name, V.get(name, ("",))[0], len(diff), len(base), 100.0 * len(diff) / len(base),
            " ".join("%d:%d" % kv for kv in sorted(per_y.items()))))


def stats(paths):
    import glob
    import make_testpack as mt
    paths = paths or sorted(glob.glob(os.path.join(HERE, "data", "testpack_*.txt")))
    entries = mt.build()
    count, per_y, sha = {}, {}, set()
    for p in paths:
        with open(p) as f:
            for line in f:
                if line.startswith("# datapack"):
                    sha.add(line.split()[4])
                if line[0] != "B":
                    continue
                _, x, y, z, n = line.split()
                n = n if ":" in n else "minecraft:" + n
                count[n] = count.get(n, 0) + 1
                c = per_y.setdefault(int(y), [0, 0, set()])
                c[0] += 1
                c[1] += n.startswith("testpack:")
                c[2].add(n)
    total = sum(count.values())
    print("%d files, %d B lines, pack sha1 %s" % (len(paths), total, ", ".join(sorted(sha))))
    # the same reproducible zip the data tool makes of testpack/
    import tempfile
    import worldgen_reference as wr
    with tempfile.TemporaryDirectory() as tmp:
        wr.zip_pack(PACK, os.path.join(tmp, "pack.zip"))
        now = wr.pr.sha1_of(os.path.join(tmp, "pack.zip"))
    print("testpack/ as it is now: sha1 %s (%s)" % (now, "the pack of the files" if sha == {now} else "NOT the pack of the files"))
    listed = {e.biome for e in entries}
    print("biomes: %d in the list, %d appear, never: %s" % (
        len(listed), len(listed & set(count)), ", ".join(sorted(b for b in listed if b not in count)) or "-"))
    if set(count) - listed:
        print("NOT IN THE LIST: %s" % ", ".join(sorted(set(count) - listed)))
    print("testpack biomes: %.1f %% of all cells" % (
        100.0 * sum(v for k, v in count.items() if k.startswith("testpack:")) / total))
    for y in sorted(per_y):
        n, t, names = per_y[y]
        print("  y %4d: %5.1f %% testpack, %d different biomes" % (y, 100.0 * t / n, len(names)))
    index = {}
    for i, e in enumerate(entries):
        index.setdefault(e.biome, []).append(i)
    print("pairs (list index: cells):")
    pairs = [("testpack:tie%d_a" % i, "testpack:tie%d_b" % i) for i in range(1, mt.N_TIES + 1)]
    for a, b in pairs + [(mt.EQUI_P, mt.EQUI_Q)]:
        print("  %-18s %3d: %5d   %-18s %3d: %5d" % (
            a.split(":")[1], index[a][0], count.get(a, 0), b.split(":")[1], index[b][0], count.get(b, 0)))
    roles = {}
    for e in entries:
        if e.role in ("overlap", "unreachable"):
            roles.setdefault(e.role, set()).add(e.biome)
    for role, names in sorted(roles.items()):
        print("%s boxes (%s): %d cells" % (role, ", ".join(sorted(n.split(":")[1] for n in names)),
                                          sum(count.get(n, 0) for n in names)))


# A folder that a run of this tool made and a later run may replace
MARKER = ".seed-atlas-testpack"


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("list", "make", "compare", "stats"):
        raise SystemExit(__doc__)
    if sys.argv[1] == "list":
        for k, (e, _) in V.items():
            print(k, e)
    elif sys.argv[1] == "compare":
        compare(sys.argv[2], sys.argv[3:])
    elif sys.argv[1] == "stats":
        stats(sys.argv[2:])
    else:
        if len(sys.argv) < 4:
            raise SystemExit(__doc__)
        out = sys.argv[2]
        names = sys.argv[3:]
        # every name first: nothing is deleted for a typo
        unknown = [n for n in names if n not in V]
        if unknown:
            raise SystemExit("unknown variant(s): %s (see: testpack_variants.py list)" % ", ".join(unknown))
        for name in names:
            root = os.path.join(out, name)
            if os.path.exists(root) and not os.path.isfile(os.path.join(root, MARKER)):
                raise SystemExit("%s exists and was not made by this tool (no %s): not replaced" % (root, MARKER))
        for name in names:
            root = os.path.join(out, name)
            if os.path.isdir(root):
                shutil.rmtree(root)
            shutil.copytree(PACK, root, ignore=shutil.ignore_patterns(".*", "__pycache__"))
            V[name][1](root)
            # hidden: not part of the pack (pack_files() and the zips leave it out)
            with open(os.path.join(root, MARKER), "w") as f:
                f.write("made by tools/worldgen-reference/testpack_variants.py\n")


if __name__ == "__main__":
    sys.dont_write_bytecode = True  # keep the repo directory free of __pycache__
    sys.path.insert(0, HERE)
    main()
