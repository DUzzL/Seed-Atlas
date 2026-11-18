#!/usr/bin/env python3
"""Checks the app's data pack generator against a real vanilla server for one
data pack, and prints aggregate numbers only.

For a pack whose content must not be shown or shared (its licence forbids
it, or it is private): the pack stays on this machine, and the output names
nothing of it. The tool

1. picks areas per dimension where the app's generator expects the pack's
   biomes (dptest areas),
2. lets a vanilla dedicated server generate those areas, and the structure
   starts of a grid of chunks, with the pack installed
   (worldgen_reference.py; the server binds to 127.0.0.1 only),
3. compares the server's biomes, heights and structure starts with the
   generator and measures map tiles (dptest packcheck).

It prints counts, shares and times: differing biome cells per dimension and
y, the share of exact heights, structure starts found or missing, times per
map tile, and the names of the types the generator does not know. It never
prints the pack's file name, resource ids, coordinates or values; everything
the server logged and the reference files stay in the work directory.

Usage (from the repository root, after building dptest, see
src/datapack/tests/dptest.cpp):

    python3 -B tools/worldgen-reference/check_pack.py --mc 1.21.1 --pack <zip, jar or folder> \\
        --dptest <build dir>/dptest --work <scratch dir outside the repo> --accept-eula

See README.md ("Checking a pack you cannot share").
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import traceback
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(HERE, "..", "portal-reference"))

import portal_reference as pr  # noqa: E402
from mcserver import require_eula  # noqa: E402

PACK_NAME = "checkpack"     # the pack's name in the worlds and in the file names
VANILLA_DIMS = (("minecraft:overworld", "overworld"), ("minecraft:the_nether", "nether"),
                ("minecraft:the_end", "end"))
FAR_AREA = "far2:-1258,742,16,16"   # the far area of worlds.txt (about -20000, 12000)


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def pack_dimensions(zip_path):
    """Ids of the dimensions the pack defines with files of its own (its own
    namespaces; the vanilla ones are checked anyway)."""
    out = set()
    with zipfile.ZipFile(zip_path) as z:
        for n in z.namelist():
            m = re.match(r"^(?:[^/]+/)?data/([a-z0-9_.-]+)/dimension/([a-z0-9_./-]+)\.json$", n)
            if m:
                out.add("%s:%s" % m.groups())
    vanilla = {d for d, _ in VANILLA_DIMS}
    return sorted(out - vanilla)


def run_logged(cmd, log_path, cwd=None):
    """Runs a command with all its output going to a log file; returns its exit status."""
    with open(log_path, "a") as f:
        f.write("\n$ %s\n" % " ".join(cmd))
        f.flush()
        return subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=cwd)


def main():
    ap = argparse.ArgumentParser(description="Compare a data pack world of the app with a vanilla server; "
                                 "prints aggregate numbers only.")
    ap.add_argument("--mc", required=True, help="Minecraft version of the server (and of the generator)")
    ap.add_argument("--pack", required=True, help="the data pack: zip, jar or folder")
    ap.add_argument("--dptest", required=True, help="the dptest binary (src/datapack/tests)")
    ap.add_argument("--work", required=True, help="scratch directory for the server, the worlds, logs and"
                    " reference files (outside the repository)")
    ap.add_argument("--jar", help="official server jar of the version (verified by sha1; else downloaded)")
    ap.add_argument("--vanilla", help="vanilla data for the generator (default: the server jar)")
    ap.add_argument("--seed", type=int, action="append", help="world seeds (default 12345 and -42)")
    ap.add_argument("--dimension", action="append",
                    help="overworld, nether, end, or the id of a dimension of the pack (default: all of them)")
    ap.add_argument("--areas", type=int, default=4, help="areas of 16 x 16 chunks picked per dimension and seed"
                    " in addition to the origin and a far area (default 4)")
    ap.add_argument("--no-structures", action="store_true", help="skip the structure starts")
    ap.add_argument("--no-terrain-only", action="store_true", help="skip the worlds without features (one seed"
                    " per dimension, 1.19 and later; their heights are the terrain alone)")
    ap.add_argument("--structure-seeds", type=int, default=1, help="how many of the seeds also get structure"
                    " starts (default 1)")
    ap.add_argument("--structure-grid", type=int, default=15, help="chunks per side of the structure grid,"
                    " 24 chunks apart (default 15: 360 x 360 chunks, like structures.txt)")
    ap.add_argument("--heap", default="8G", help="Java heap of the server (default 8G; packs with very many"
                    " structures need more, or a smaller --structure-grid)")
    ap.add_argument("--port", type=int, default=25995)
    ap.add_argument("--java", default="java")
    ap.add_argument("--accept-eula", action="store_true",
                    help="you accept Mojang's EULA (https://aka.ms/MinecraftEULA) for the temporary server")
    args = ap.parse_args()
    require_eula(args.accept_eula)

    work = os.path.abspath(args.work)
    repo = os.path.abspath(os.path.join(HERE, "..", ".."))
    if os.path.commonpath([work, repo]) == repo:
        raise SystemExit("--work must be outside the repository (the reference files hold the pack's ids)")
    if not os.path.exists(args.pack):
        raise SystemExit("--pack: no such file or folder")
    dptest = os.path.abspath(args.dptest)
    if not os.access(dptest, os.X_OK):
        raise SystemExit("--dptest: not an executable")
    seeds = args.seed or [12345, -42]
    for sub in ("pack", "logs", "ref", "worlds"):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    logs = os.path.join(work, "logs")

    # the pack under a neutral name
    pack = os.path.join(work, "pack", PACK_NAME + ".zip")
    if os.path.isdir(args.pack):
        if not os.path.isfile(os.path.join(args.pack, "pack.mcmeta")):
            raise SystemExit("--pack: the folder has no pack.mcmeta, so a vanilla server would not load it")
        sys.path.insert(0, HERE)
        import worldgen_reference as wr
        wr.zip_pack(os.path.abspath(args.pack), pack)
    else:
        shutil.copyfile(args.pack, pack)
    if not zipfile.is_zipfile(pack):
        raise SystemExit("--pack: not a zip or jar file, nor a folder")
    with zipfile.ZipFile(pack) as z:
        if "pack.mcmeta" not in z.namelist():
            raise SystemExit("--pack: it has no pack.mcmeta, so a vanilla server would not load it (a mod's jar"
                             " needs its mod loader); nothing to compare with")
    jar = pr.obtain_jar(args.mc, work, args.jar)
    vanilla = os.path.abspath(args.vanilla) if args.vanilla else jar

    # the dimensions: labels only in the output
    own = pack_dimensions(pack)
    dims = [(d, label) for d, label in VANILLA_DIMS] + \
        [(d, "dimension %d of the pack" % (i + 1)) for i, d in enumerate(own)]
    if args.dimension:
        names = {label: d for d, label in VANILLA_DIMS}
        wanted = {names.get(v, v) for v in args.dimension}
        dims = [(d, label) for d, label in dims if d in wanted]
        if len(dims) != len(wanted):
            raise SystemExit("--dimension: not a dimension of the world")
    say("version %s, %d seeds, %d dimensions (%d of them the pack's own)"
        % (args.mc, len(seeds), len(dims), sum(1 for d, _ in dims if d in own)))

    tool = os.path.join(HERE, "worldgen_reference.py")
    common = ["--mc", args.mc, "--jar", jar, "--datapack", pack, "--datapack-name", PACK_NAME,
              "--work", os.path.join(work, "server"), "--heap", args.heap, "--port", str(args.port),
              "--java", args.java, "--accept-eula", "--allow-warnings", "--force"]

    def server_run(name, extra, label, hint=""):
        """One run of worldgen_reference.py into ref/<name>; the files it wrote, or None."""
        out = os.path.join(work, "ref", name)
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        log = os.path.join(logs, "server-%s.log" % name)
        t0 = time.time()
        rc = run_logged([sys.executable, "-B", tool] + common + extra + ["--out", out], log)
        files = sorted(os.path.join(out, n) for n in os.listdir(out) if n.endswith(".txt"))
        if rc != 0 or not files:
            say("%s: the server run failed (see %s)%s" % (label, log, hint))
            return None
        say("%s: done in %.0f s" % (label, time.time() - t0))
        return files
    refs = []
    for k, (dim, label) in enumerate(dims):
        # areas where the generator expects the pack's biomes
        lines = []
        for seed in seeds:
            out = os.path.join(logs, "areas-%d-%d.txt" % (k, seed))
            with open(out, "w") as f:
                rc = subprocess.call([dptest, "areas", vanilla, args.mc, str(seed), "--pack", pack, "--dimension",
                                      dim, "--count", str(args.areas), "--quiet"], stdout=f, stderr=subprocess.STDOUT)
            text = open(out).read()
            line = [ln for ln in text.splitlines() if ln.startswith("%d origin:" % seed)]
            if rc != 0 or not line:
                say("%s: the generator does not support it, skipped" % label)
                break
            m = re.search(r"(\d+) biomes of packs in the dimension's list, (\d+) of them found", text)
            if m and seed == seeds[0]:
                say("%s: %s biomes of the pack in its list, %s of them found near the origin"
                    % (label, m.group(1), m.group(2)))
            lines.append(line[0] + " " + FAR_AREA)
        if len(lines) != len(seeds):
            continue
        worlds = os.path.join(work, "worlds", "heights-%d.txt" % k)
        with open(worlds, "w") as f:
            f.write("\n".join(lines) + "\n")
        dimarg = [] if dim == "minecraft:overworld" else ["--dimension", dim]
        say("%s: the server generates %d worlds ..." % (label, len(seeds)))
        files = server_run("heights-%d" % k, dimarg + ["--worlds", worlds], label)
        if not files:
            continue
        refs += files
        if not args.no_terrain_only and [int(v) for v in re.findall(r"\d+", args.mc)] >= [1, 19]:
            # the same areas without features, carvers and structures: the
            # heights are then the terrain alone
            say("%s: the server generates the terrain alone ..." % label)
            refs += server_run("terrain-%d" % k, dimarg + ["--worlds", worlds, "--seed", str(seeds[0]),
                                                            "--terrain-only"], label) or []

        if args.no_structures:
            continue
        grid = args.structure_grid
        sworlds = os.path.join(work, "worlds", "structures-%d.txt" % k)
        with open(sworlds, "w") as f:
            for seed in seeds[:args.structure_seeds]:
                f.write("%d main:%d,%d,%d,%d,24\n" % (seed, -12 * grid, -12 * grid, grid, grid))
        say("%s: the server places structures ..." % label)
        files = server_run("structures-%d" % k, ["--structures"] + dimarg + ["--worlds", sworlds], label,
                           "; if the server ran out of memory, a larger --heap or a smaller --structure-grid helps")
        refs += files or []

    # warnings of the server while it loaded the pack (counts only)
    warned = 0
    for n in os.listdir(logs):
        if n.startswith("server-"):
            with open(os.path.join(logs, n), errors="replace") as f:
                for ln in f:
                    m = re.search(r"the server logged (\d+) warnings/errors while loading the pack", ln)
                    if m:
                        warned = max(warned, int(m.group(1)))
    if warned:
        say("the server logged %d warnings or errors while loading the pack (the server logs in %s)"
            % (warned, os.path.join(work, "server")))
    if refs:
        say("comparing %d reference files with the generator" % len(refs))
    else:
        say("no reference data: only the generator's view of the pack follows")
    print(flush=True)
    rc = subprocess.call([dptest, "packcheck", vanilla, args.mc, "--pack", pack, "--timing"] + refs)
    sys.exit(0 if rc == 0 and refs else 1)


def guarded():
    """main(), but an unexpected error prints no more than its type: its text
    could name the pack's files. The traceback goes to the work directory."""
    try:
        main()
    except (SystemExit, KeyboardInterrupt):
        raise
    except Exception as e:
        where = None
        try:
            work = os.path.abspath(sys.argv[sys.argv.index("--work") + 1])
            os.makedirs(os.path.join(work, "logs"), exist_ok=True)
            where = os.path.join(work, "logs", "check_pack-error.log")
            with open(where, "w") as f:
                traceback.print_exc(file=f)
        except (ValueError, IndexError, OSError):
            pass
        raise SystemExit("check_pack.py failed (%s)%s" % (type(e).__name__,
                         ", the details are in " + where if where else ""))


if __name__ == "__main__":
    guarded()
