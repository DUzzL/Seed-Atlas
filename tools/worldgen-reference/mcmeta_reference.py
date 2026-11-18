#!/usr/bin/env python3
"""Which data packs and overlays a real vanilla server loads, by pack.mcmeta.

Writes small probe packs (made by this script, one per case below), puts all
of them into the world of a vanilla dedicated server, and asks the running
server which of them it loaded and which of their overlays it applied:

* every probe pack has a function <case>/base in its own files, and a
  function <case>/ov_<directory> in each of its overlay directories (in both
  folder names, "function" from 1.21 and "functions" before);
* the tool runs each of these functions; a function that runs belongs to a
  loaded pack (base) or to an applied overlay.

Output: data/mcmeta_<mc>.txt (see "pack.mcmeta" in README.md):

    # Seed Atlas pack.mcmeta reference data, vanilla Minecraft <mc> dedicated server, sha1 <jar sha1>
    # mc <mc>
    # pack_format <major>.<minor>
    C <case> <overlay directories, comma separated, or -> <pack.mcmeta, one line>
    R <case> loaded|rejected
    O <case> <directory> applied|absent
    W <case> <layer>     (ov_order: whose file of a path the pack and two overlays share is used)

Usage:
    python3 -B tools/worldgen-reference/mcmeta_reference.py --mc 1.21.1 --jar <server jar> \\
        --work <scratch dir outside the repository> --accept-eula [--port 25971]

Nothing of the game is written to the repository: the packs are made here,
the output holds what the server did with them.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "portal-reference"))

from mcserver import Server, write_properties, require_eula  # noqa: E402

# A pack section every version since 1.20 accepts: pack_format and
# supported_formats for the versions before 1.21.9 (format 82), min_format and
# max_format for the later ones (both notations agree, as 1.21.9 requires).
HEAD = {"description": "probe", "pack_format": 15, "supported_formats": [15, 81],
        "min_format": 15, "max_format": 200}


def ov(dirs):
    return {"pack": HEAD, "overlays": {"entries": dirs}}


def e(directory, **kw):
    d = dict(kw)
    d["directory"] = directory
    return d


# (case, pack.mcmeta: a dict written as JSON or the exact text, overlay directories)
CASES = [
    # the pack section
    ("ok", {"pack": HEAD}, []),
    ("no_pack_section", {"other": {"description": "probe"}}, []),
    ("no_description", {"pack": {k: v for k, v in HEAD.items() if k != "description"}}, []),
    ("description_number", {"pack": dict(HEAD, description=5)}, []),
    ("no_pack_format", {"pack": {"description": "probe"}}, []),
    ("string_pack_format", {"pack": {"description": "probe", "pack_format": "48"}}, []),
    ("fraction_pack_format", {"pack": {"description": "probe", "pack_format": 48.7}}, []),
    ("new_notation_only", {"pack": {"description": "probe", "min_format": 82, "max_format": 200}}, []),
    ("min_format_only", {"pack": {"description": "probe", "min_format": 82},
                         "overlays": {"entries": [e("a", min_format=82, max_format=200)]}}, ["a"]),
    ("invalid_json", '{"pack":{"description":"probe","pack_format":15}', []),
    ("pack_section_array", {"pack": [15]}, []),
    ("comment", '{/* c */"pack":{"description":"probe","pack_format":15,"supported_formats":[15,81],'
                '"min_format":15,"max_format":200}}', []),
    ("trailing_comma", '{"pack":{"description":"probe","pack_format":15,"supported_formats":[15,81],'
                       '"min_format":15,"max_format":200,}}', []),
    ("description_empty_object", {"pack": dict(HEAD, description={})}, []),
    ("description_empty_list", {"pack": dict(HEAD, description=[])}, []),
    ("description_bool", {"pack": dict(HEAD, description=True)}, []),
    ("description_empty_string", {"pack": dict(HEAD, description="")}, []),
    ("supported_reversed", {"pack": {"description": "probe", "pack_format": 15, "supported_formats": [81, 15]}}, []),
    ("supported_list3", {"pack": {"description": "probe", "pack_format": 15, "supported_formats": [15, 30, 81]}}, []),
    ("supported_string", {"pack": {"description": "probe", "pack_format": 15, "supported_formats": "15"}}, []),
    ("pack_format_negative", {"pack": {"description": "probe", "pack_format": -3}}, []),
    # the same file in two overlays and the pack: the last applied overlay wins
    ("ov_order", ov([e("a", min_format=18, max_format=200, formats=[18, 81]),
                     e("b", min_format=18, max_format=200, formats=[18, 81])]), ["a", "b"]),
    # the "features" section (feature flags the pack needs)
    ("features_vanilla", {"pack": HEAD, "features": {"enabled": ["minecraft:vanilla"]}}, []),
    ("features_not_a_list", {"pack": HEAD, "features": {"enabled": 5}}, []),
    ("features_unknown_flag", {"pack": HEAD, "features": {"enabled": ["probe:no_such_flag"]}}, []),
    ("features_not_object", {"pack": HEAD, "features": []}, []),
    # overlays in the notation before 1.21.9
    ("ov_old", ov([e("a", formats=[18, 200]), e("b", formats=[18, 20]), e("c", formats=48),
                   e("d", formats={"min_inclusive": 41, "max_inclusive": 48})]), ["a", "b", "c", "d"]),
    ("ov_old_bounded", ov([e("a", formats=[18, 81]), e("b", formats=[41, 48])]), ["a", "b"]),
    ("ov_old_fraction", ov([e("a", formats=[15.6, 15.6]), e("b", formats=[18.6, 18.6]),
                            e("c", formats=[41.6, 41.6]), e("d", formats=[48.6, 48.6]),
                            e("e", formats=[71.6, 71.6]), e("f", formats=[81.6, 81.6])]),
     ["a", "b", "c", "d", "e", "f"]),
    ("ov_list3", ov([e("a", formats=[18, 30, 48]), e("b", formats=[18, 81])]), ["a", "b"]),
    ("ov_reversed", ov([e("a", formats=[48, 18]), e("b", formats=[18, 81])]), ["a", "b"]),
    ("ov_empty_entry_old", ov([e("a"), e("b", formats=[18, 81])]), ["a", "b"]),
    # overlays in the notation of 1.21.9
    ("ov_new", ov([e("a", min_format=82, max_format=200), e("b", min_format=[101, 2], max_format=200),
                   e("c", min_format=82, max_format=[101, 0]), e("d", min_format=[101, 1], max_format=[101, 1]),
                   e("e", min_format=88, max_format=88), e("f", min_format=94, max_format=94),
                   e("g", min_format=107, max_format=107), e("h", min_format=121, max_format=121)]),
     ["a", "b", "c", "d", "e", "f", "g", "h"]),
    ("ov_new_fraction", ov([e("a", min_format=88.6, max_format=88.6), e("b", min_format=94.6, max_format=94.6),
                            e("c", min_format=101.6, max_format=101.6), e("d", min_format=107.6, max_format=107.6),
                            e("e", min_format=121.6, max_format=121.6),
                            e("f", min_format=[101, 1.6], max_format=[101, 1.6]),
                            e("g", min_format=[94, 1.6], max_format=[94, 1.6])]),
     ["a", "b", "c", "d", "e", "f", "g"]),
    ("ov_new_lists", ov([e("a", min_format=[82], max_format=[200]),
                         e("b", min_format=[101, 1, 5], max_format=200)]), ["a", "b"]),
    ("ov_new_dir_chars", ov([e("x.y-z_W", min_format=82, max_format=200)]), ["x.y-z_W"]),
    ("ov_bridge", ov([e("a", min_format=18, max_format=200, formats=[18, 81])]), ["a"]),
    ("ov_bridge_max", ov([e("a", min_format=18, max_format=60, formats=[18, 60])]), ["a"]),
    ("ov_bridge_bad_min", ov([e("a", min_format=20, max_format=200, formats=[18, 81])]), ["a"]),
    ("ov_bridge_bad_max", ov([e("a", min_format=18, max_format=200, formats=[18, 70])]), ["a"]),
    ("ov_mixed", ov([e("a", formats=[18, 81]), e("b", min_format=82, max_format=200)]), ["a", "b"]),
    ("ov_deprecated", ov([e("a", min_format=82, max_format=200, formats=[82, 200])]), ["a"]),
    ("ov_min_only", ov([e("a", min_format=82)]), ["a"]),
    ("ov_empty_entry_new", ov([e("a"), e("b", min_format=82, max_format=200)]), ["a", "b"]),
    ("ov_new_reversed", ov([e("a", min_format=200, max_format=82)]), ["a"]),
    ("ov_negative", ov([e("a", min_format=-1, max_format=200)]), ["a"]),
    ("ov_bad_dir", ov([e("a b", min_format=18, max_format=200, formats=[18, 81]),
                       e("c", min_format=18, max_format=200, formats=[18, 81])]), ["a b", "c"]),
    ("ov_entry_number", {"pack": HEAD, "overlays": {"entries": [5, e("a", min_format=18, max_format=200,
                                                                   formats=[18, 81])]}}, ["a"]),
    ("ov_no_directory", ov([{"min_format": 18, "max_format": 200, "formats": [18, 81]},
                            e("a", min_format=18, max_format=200, formats=[18, 81])]), ["a"]),
    ("ov_not_object", {"pack": HEAD, "overlays": []}, []),
    ("ov_no_entries", {"pack": HEAD, "overlays": {}}, []),
]


def mcmeta_text(meta):
    return meta if isinstance(meta, str) else json.dumps(meta, separators=(",", ":"))


def zip_bytes(files):
    """files: {path: bytes}; stored, sorted, fixed dates (the same zip everywhere)"""
    import io
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for path in sorted(files):
            info = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            z.writestr(info, files[path])
    return buf.getvalue()


def fname(d):
    """the function of an overlay directory (resource paths are lower case)"""
    return "ov_" + re.sub(r"[^a-z0-9_.-]", "_", d.lower())


def probe_files(case, meta, dirs):
    files = {"pack.mcmeta": mcmeta_text(meta).encode()}

    def fn(prefix, name):
        for folder in ("function", "functions"):
            files["%sdata/probe/%s/%s/%s.mcfunction" % (prefix, folder, case, name)] = \
                ("say PROBE %s %s\n" % (case, name)).encode()
    fn("", "base")
    for d in dirs:
        fn(d + "/", fname(d))
    if case == "ov_order":
        for prefix, layer in [("", "root")] + [(d + "/", d) for d in dirs]:
            for folder in ("function", "functions"):
                files["%sdata/probe/%s/%s/shared.mcfunction" % (prefix, folder, case)] = \
                    ("say PROBE %s shared:%s\n" % (case, layer)).encode()
    return files


def data_format(jar):
    with zipfile.ZipFile(jar) as z:
        pv = json.loads(z.read("version.json"))["pack_version"]
    if isinstance(pv, int):
        return (pv, 0)
    if "data" in pv:
        return (pv["data"], 0)
    return (pv["data_major"], pv.get("data_minor", 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mc", required=True)
    ap.add_argument("--jar", required=True, help="the official server jar of the version")
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", default=os.path.join(HERE, "data"))
    ap.add_argument("--port", type=int, default=25971)
    ap.add_argument("--java", default="java")
    ap.add_argument("--heap", default="2G")
    ap.add_argument("--accept-eula", action="store_true")
    args = ap.parse_args()
    require_eula(args.accept_eula)

    sdir = os.path.join(os.path.abspath(args.work), "mcmeta-%s" % args.mc)
    if os.path.isdir(sdir):
        shutil.rmtree(sdir)
    os.makedirs(os.path.join(sdir, "world", "datapacks"))
    shutil.copy(args.jar, os.path.join(sdir, "server.jar"))
    with open(args.jar, "rb") as f:
        sha1 = hashlib.sha1(f.read()).hexdigest()
    fmt = data_format(args.jar)
    for case, meta, dirs in CASES:
        with open(os.path.join(sdir, "world", "datapacks", case + ".zip"), "wb") as f:
            f.write(zip_bytes(probe_files(case, meta, dirs)))
    write_properties(sdir, 1, "world", args.port, "minecraft:flat")

    seen = set()
    cmd = [args.java, "-Xmx" + args.heap, "-jar", "server.jar", "--nogui"]
    with open(os.path.join(sdir, "server-out.log"), "w") as logf:
        with Server(sdir, cmd, log=logf, start_timeout=600) as srv:
            for case, meta, dirs in CASES:
                srv.send("function probe:%s/base" % case)
                for d in dirs:
                    srv.send("function probe:%s/%s" % (case, fname(d)))
                if case == "ov_order":
                    srv.send("function probe:%s/shared" % case)
            srv.send("say PROBE-END")
            end = time.time() + 120
            while time.time() < end:
                m = srv.wait_for(re.compile(r"PROBE(-END| (\S+) (\S+))"), timeout=120)
                if m.group(1) == "-END":
                    break
                seen.add((m.group(2), m.group(3)))
            else:
                raise RuntimeError("no end marker")

    lines = ["# Seed Atlas pack.mcmeta reference data, vanilla Minecraft %s dedicated server, sha1 %s" % (args.mc, sha1),
             "# generated by tools/worldgen-reference/mcmeta_reference.py, format: see README.md",
             "# mc %s" % args.mc,
             "# pack_format %d.%d" % fmt]
    for case, meta, dirs in CASES:
        lines.append("C %s %s %s" % (case, ",".join(d.replace(" ", "%20") for d in dirs) or "-", mcmeta_text(meta)))
        loaded = (case, "base") in seen
        lines.append("R %s %s" % (case, "loaded" if loaded else "rejected"))
        for d in dirs:
            lines.append("O %s %s %s" % (case, d.replace(" ", "%20"),
                                         "applied" if (case, fname(d)) in seen else "absent"))
        for c, what in sorted(seen):
            if c == case and what.startswith("shared:"):
                lines.append("W %s %s" % (case, what[7:]))
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, "mcmeta_%s.txt" % args.mc)
    with open(out + ".tmp", "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(out + ".tmp", out)
    print("wrote %s: %d of %d packs loaded" % (out, sum(1 for c in CASES if (c[0], "base") in seen), len(CASES)))


if __name__ == "__main__":
    main()
