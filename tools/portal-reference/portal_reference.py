#!/usr/bin/env python3
"""Generate end-portal-frame / stronghold-piece reference data from vanilla.

For every requested (seed, stronghold) this starts a local vanilla dedicated
server (127.0.0.1 only, no rcon/query), force-loads the stronghold's start
chunk, reads the stored structure start from the region files, force-loads all
chunks covering the portal room, saves, and reads the end_portal_frame block
states (with their 'eye' property) that vanilla actually generated.

Stronghold positions and their placement index come from vanilla itself:
  * 26.1+ (unobfuscated jars): RingDump.java prints the server's own
    concentric-ring position list in order -> sh_index is vanilla's index.
  * older (obfuscated) jars: an approximate position from the seedatlas engine
    (helper 'shpos', see build_shpos.sh) is handed to vanilla's
    '/locate structure minecraft:stronghold'; the located stronghold is used
    and sh_index is written as -1.

Usage:
  portal_reference.py --mc 26.3 --seeds seeds_26.3.txt --work <scratch dir>
See README.md.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # keep the repo directory free of __pycache__

from mcserver import Server, write_properties, remove_world, level_type_property, require_eula  # noqa: E402
import mcworld  # noqa: E402

MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
FRAME = "minecraft:end_portal_frame"
PORTAL_ROOM = "minecraft:shpr"
STRONGHOLD = "minecraft:stronghold"


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---------------------------------------------------------------- jar handling

def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def official_server_info(mc):
    with urllib.request.urlopen(MANIFEST) as r:
        man = json.load(r)
    for v in man["versions"]:
        if v["id"] == mc:
            with urllib.request.urlopen(v["url"]) as r:
                return json.load(r)["downloads"]["server"]
    raise SystemExit("version %s not in Mojang's manifest" % mc)


def obtain_jar(mc, work, explicit=None):
    """Return a path to the official server jar, verified by sha1."""
    info = official_server_info(mc)
    cands = [explicit] if explicit else []
    cands.append(os.path.expanduser("~/.gradle/caches/fabric-loom/%s/minecraft-server.jar" % mc))
    cands.append(os.path.join(work, "jars", "server-%s.jar" % mc))
    for c in cands:
        if c and os.path.exists(c) and sha1_of(c) == info["sha1"]:
            return c
    dst = os.path.join(work, "jars", "server-%s.jar" % mc)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    log("downloading", info["url"])
    urllib.request.urlretrieve(info["url"], dst)
    if sha1_of(dst) != info["sha1"]:
        raise SystemExit("sha1 mismatch for downloaded %s" % dst)
    return dst


def prepare_server_dir(mc, work, jar, java, heap="6G", name=None):
    """Set up <work>/<name> (default 'server-<mc>') and return (dir, command line, unobfuscated)."""
    d = os.path.join(work, name or "server-%s" % mc)
    os.makedirs(d, exist_ok=True)
    sj = os.path.join(d, "server.jar")
    if not os.path.exists(sj) or sha1_of(sj) != sha1_of(jar):
        shutil.copyfile(jar, sj)
    with zipfile.ZipFile(sj) as z:
        names = set(z.namelist())
        bundled = "META-INF/classpath-joined" in names
        if bundled:
            cp = z.read("META-INF/classpath-joined").decode().strip().split(";")
            vjar = [c for c in cp if c.startswith("versions/")]
    if bundled and not all(os.path.exists(os.path.join(d, c)) for c in cp):
        # Let the bundler extract its libraries once (exits after writing settings).
        subprocess.run([java, "-jar", "server.jar", "--initSettings", "--nogui"], cwd=d,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    unobf = False
    if bundled and vjar:
        with zipfile.ZipFile(os.path.join(d, vjar[0])) as z:
            unobf = "net/minecraft/world/level/ChunkPos.class" in z.namelist()
    if unobf:
        out = os.path.join(work, "ringdump-%s" % mc)
        os.makedirs(out, exist_ok=True)
        full_cp = ":".join(os.path.join(d, c) for c in cp)
        subprocess.run(["javac", "-d", out, "-cp", full_cp, os.path.join(HERE, "RingDump.java")],
                       check=True)
        cmd = [java, "-Xmx" + heap, "--add-opens", "java.base/java.lang=ALL-UNNAMED",
               "-cp", out + ":" + full_cp, "RingDump", "--nogui"]
    else:
        cmd = [java, "-Xmx" + heap, "-jar", "server.jar", "--nogui"]
    return d, cmd, unobf


# ------------------------------------------------------------------- the run

PASS_RE = re.compile(r"Test (passed|failed)")
LOADED_RE = re.compile(r"Test (passed|failed)|(not loaded)")
LOCATE_RE = re.compile(r"nearest (?:minecraft:)?stronghold is at \[(-?\d+), [^,]+, (-?\d+)\]")
FORCE_RE = re.compile(r"(Marked chunk|marked for force loading|already marked|No chunks were marked|Chunk at)")


def wait_loaded(srv, chunks, timeout=900):
    end = time.time() + timeout
    pending = list(chunks)
    while pending:
        if time.time() > end:
            raise TimeoutError("chunks not loaded: %s" % pending[:5])
        still = []
        for cx, cz in pending:
            # 'execute if loaded' only exists since 1.19.4; a block test works in all
            # versions and fails with "not loaded" until the chunk is loaded.
            m = srv.command("execute unless block %d 0 %d minecraft:structure_void" % (cx * 16, cz * 16),
                            LOADED_RE, 120)
            if m.group(1) != "passed":
                still.append((cx, cz))
        pending = still
        if pending:
            time.sleep(2)


def forceload(srv, chunks):
    for cx, cz in chunks:
        srv.send("forceload add %d %d" % (cx * 16, cz * 16))
    wait_loaded(srv, chunks)


def chunks_of_box(x0, z0, x1, z1):
    return [(cx, cz) for cx in range(x0 >> 4, (x1 >> 4) + 1) for cz in range(z0 >> 4, (z1 >> 4) + 1)]


def region_dir(server_dir, level):
    for p in (os.path.join(server_dir, level, "dimensions", "minecraft", "overworld", "region"),
              os.path.join(server_dir, level, "region")):
        if os.path.isdir(p):
            return p
    raise RuntimeError("no overworld region dir in %s" % level)


def layout(st):
    return [(p["id"], *p["BB"], p.get("O", -1), p["GD"]) for p in st["Children"]] + \
        [("start", st["ChunkX"], st["ChunkZ"])]


def read_frames(world, bb, defaults, seed):
    """All end portal frames inside the portal room box, sorted by x, z."""
    frames = []
    for cx, cz in chunks_of_box(bb[0], bb[2], bb[3], bb[5]):
        ch = world.chunk(cx, cz)
        status = mcworld.chunk_status(ch)
        if status not in ("minecraft:full", "full"):  # 1.18.2 stores it without namespace
            raise RuntimeError("seed %d chunk %d,%d status %s" % (seed, cx, cz, status))
        for x, y, z, props in mcworld.find_blocks(ch, FRAME, defaults):
            if bb[0] <= x <= bb[3] and bb[1] <= y <= bb[4] and bb[2] <= z <= bb[5]:
                frames.append((x, y, z, 1 if props.get("eye") == "true" else 0, props.get("facing")))
    if len(frames) != 12:
        raise RuntimeError("seed %d: found %d frames in portal room %s" % (seed, len(frames), bb))
    facings = sorted(f[4] or "?" for f in frames)
    if facings != sorted(["north", "south", "east", "west"] * 3):
        raise RuntimeError("seed %d: unexpected frame facings %s" % (seed, facings))
    frames.sort(key=lambda f: (f[0], f[2], f[1]))
    return frames


def suffix_of(level_type):
    """File and world name suffix; empty for the Default world type."""
    return "" if level_type == "normal" else "_" + level_type


def check_level_type(server_dir, level, level_type):
    """The saved world must really use the requested world type.

    An unknown 'level-type' value makes the server fall back to Default
    (1.18.2 without any message), so read vanilla's own level data.
    """
    got = mcworld.overworld_noise_settings(os.path.join(server_dir, level))
    want = mcworld.NOISE_SETTINGS[level_type]
    if got != want:
        raise SystemExit("world %s: overworld noise settings are %r, expected %r (level type %s)"
                         % (level, got, want, level_type))
    return got


def run_seed(mc, server_dir, cmd, unobf, seed, wanted, shpos, port, logf, level_type="normal",
             start_timeout=900):
    """wanted: list of stronghold indices. Returns list of result dicts."""
    level = "pr_%s_%d%s" % (mc.replace(".", "_"), seed & 0xFFFFFFFFFFFFFFFF, suffix_of(level_type))
    remove_world(server_dir, level)
    write_properties(server_dir, seed, level, port, level_type_property(mc, level_type))
    results = []
    with Server(server_dir, cmd, log=logf, start_timeout=start_timeout) as srv:
        targets = []  # (sh_index, approx_chunk)
        if unobf:
            srv.ringdump_done.wait(300)
            ring = srv.ringdump
            if not ring:
                raise RuntimeError("RingDump failed, see server log")
            ring = [r for r in ring if r[0] == "minecraft:strongholds"]
            for i in wanted:
                _, idx, cx, cz = ring[i]
                assert int(idx) == i
                targets.append((i, (int(cx), int(cz)), "vanilla ring list"))
        else:
            out = subprocess.run([shpos, mc, str(seed), str(max(wanted) + 1)],
                                 check=True, capture_output=True, text=True).stdout.split("\n")
            for i in wanted:
                f = out[i].split()
                bx, bz = int(f[2]), int(f[3])
                old = [int(v) for v in re.findall(r"\d+", mc)[:2]] < [1, 19]
                what = "stronghold" if old else "structure minecraft:stronghold"
                m = srv.command("execute positioned %d 0 %d run locate %s" % (bx, bz, what),
                                LOCATE_RE, 600)
                lx, lz = int(m.group(1)), int(m.group(2))
                log("  engine idx %d approx %d,%d -> vanilla locate %d,%d" % (i, bx, bz, lx, lz))
                if (lx >> 4, lz >> 4) in [t[1] for t in targets]:
                    # two hints resolved to the same stronghold; keep it once
                    log("  engine idx %d: same stronghold as an earlier hint, skipped" % i)
                    continue
                targets.append((-1, (lx >> 4, lz >> 4), "engine idx %d + vanilla locate" % i))

        defaults = dict(srv.default_states)
        if unobf and FRAME not in defaults:
            raise RuntimeError("RingDump did not report the end_portal_frame default state")
        starts = [t[1] for t in targets]
        forceload(srv, starts)
        srv.save_flush()
        check_level_type(server_dir, level, level_type)
        world = mcworld.World(region_dir(server_dir, level))
        rooms = []
        for (idx, (cx, cz), how) in targets:
            st = mcworld.structure_starts(world.chunk(cx, cz)).get(STRONGHOLD)
            if st is None:
                raise RuntimeError("seed %d: no stronghold start stored in chunk %d,%d" % (seed, cx, cz))
            pr = [p for p in st["Children"] if p["id"] == PORTAL_ROOM]
            if len(pr) != 1:
                raise RuntimeError("seed %d: %d portal rooms" % (seed, len(pr)))
            bb = pr[0]["BB"]
            rooms.append((idx, st, bb, how))
        need = sorted({c for (_, _, bb, _) in rooms for c in chunks_of_box(bb[0], bb[2], bb[3], bb[5])})
        forceload(srv, need)
        srv.save_flush()
        # Read the saved state while the server still runs and cross-check every
        # frame's eye state with vanilla's own block predicate.
        world = mcworld.World(region_dir(server_dir, level))
        live = [read_frames(world, bb, defaults, seed) for (_, _, bb, _) in rooms]
        for frames in live:
            for x, y, z, eye, facing in frames:
                for e in ("true", "false"):
                    m = srv.command("execute if block %d %d %d minecraft:end_portal_frame[eye=%s,facing=%s]"
                                    % (x, y, z, e, facing), PASS_RE, 120)
                    if (m.group(1) == "passed") != ((e == "true") == bool(eye)):
                        raise RuntimeError("seed %d: in-game check disagrees at %d %d %d" % (seed, x, y, z))
    # server stopped: read the final state from disk again
    world = mcworld.World(region_dir(server_dir, level))
    for (idx, st, bb, how), frames_live in zip(rooms, live):
        frames = read_frames(world, bb, defaults, seed)
        if frames != frames_live:
            raise RuntimeError("seed %d: frames changed after server stop" % seed)
        st2 = mcworld.structure_starts(world.chunk(st["ChunkX"], st["ChunkZ"]))[STRONGHOLD]
        # 'references' and per-piece flags such as 'Chest'/'Mob' legitimately change
        # while chunks get decorated; the layout itself must not.
        if layout(st2) != layout(st):
            raise RuntimeError("seed %d: stored structure start changed between saves" % seed)
        pieces = layout(st2)[:-1]
        results.append({
            "mc": mc, "seed": seed, "sh_index": idx, "how": how, "wanted": len(wanted),
            "start": (st["ChunkX"], st["ChunkZ"]), "portal_bb": bb,
            "frames": frames, "pieces": pieces,
        })
    remove_world(server_dir, level)
    return results


# ------------------------------------------------------------------- output

def span_of(frames):
    return len({(x >> 4, z >> 4) for x, _, z, _, _ in frames})


def write_outputs(mc, results, outdir, jar_sha1, unobf, level_type="normal"):
    os.makedirs(outdir, exist_ok=True)
    sfx = suffix_of(level_type)
    results = sorted(results, key=lambda r: (r["seed"], r["sh_index"], r["start"]))
    src = ("sh_index = vanilla ConcentricRingsStructurePlacement order (RingDump)" if unobf else
           "sh_index = -1 (obfuscated jar; position located via vanilla /locate)")
    hdr = ["# Seed Atlas end portal reference data, generated by vanilla Minecraft %s" % mc,
           "# dedicated server, sha1 %s, via tools/portal-reference/portal_reference.py" % jar_sha1,
           "# " + src]
    if level_type != "normal":
        hdr.insert(2, "# world type %s: server.properties level-type=%s, overworld noise settings %s"
                   % (level_type, level_type_property(mc, level_type), mcworld.NOISE_SETTINGS[level_type]))
        if not unobf:
            hdr.append("# the search hints (\"engine idx\") are the engine's positions for the Default"
                       " world type")
    with open(os.path.join(outdir, "portals_%s%s.txt" % (mc, sfx)), "w") as f:
        f.write("\n".join(hdr) + "\n")
        f.write("# <mc> <seed> <sh_index> <start_cx> <start_cz> then 12x <x> <y> <z> <eye>"
                " (sorted by x, then z)\n")
        for r in results:
            if r["how"] != "vanilla ring list":
                f.write("# %s\n" % r["how"])
            parts = [mc, str(r["seed"]), str(r["sh_index"]), str(r["start"][0]), str(r["start"][1])]
            for x, y, z, e, _ in r["frames"]:
                parts += [str(x), str(y), str(z), str(e)]
            f.write(" ".join(parts) + "\n")
    with open(os.path.join(outdir, "pieces_%s%s.txt" % (mc, sfx)), "w") as f:
        f.write("\n".join(hdr) + "\n")
        f.write("# S <mc> <seed> <start_cx> <start_cz> <n>, then n lines in stored order:\n"
                "# <id> <x0> <y0> <z0> <x1> <y1> <z1> <orientation> <gen_depth>\n")
        for r in results:
            f.write("S %s %d %d %d %d\n" % (mc, r["seed"], r["start"][0], r["start"][1], len(r["pieces"])))
            for p in r["pieces"]:
                f.write(" ".join(str(v) for v in p) + "\n")


def summary(results):
    spans, eyes = {}, {}
    for r in results:
        s = span_of(r["frames"])
        spans[s] = spans.get(s, 0) + 1
        e = sum(f[3] for f in r["frames"])
        eyes[e] = eyes.get(e, 0) + 1
    return {"strongholds": len(results), "seeds": len({r["seed"] for r in results}),
            "chunk_span": dict(sorted(spans.items())), "eye_counts": dict(sorted(eyes.items()))}


def parse_seeds(path):
    """Lines: '<seed> [indices]' with indices like '0,1,2' or '0-8'. Default 0,1,2."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            p = line.split()
            idx = []
            spec = p[1] if len(p) > 1 else "0-2"
            for part in spec.split(","):
                if "-" in part[1:]:
                    a, b = part.split("-")
                    idx += list(range(int(a), int(b) + 1))
                else:
                    idx.append(int(part))
            out.append((int(p[0]), idx))
    return out


def main():
    # turn SIGTERM into SystemExit so the server context manager stops the server
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(143))
    ap = argparse.ArgumentParser()
    ap.add_argument("--mc", required=True)
    ap.add_argument("--seeds", required=True)
    ap.add_argument("--work", required=True, help="scratch dir for jars, server and worlds")
    ap.add_argument("--jar", help="official server jar (verified by sha1)")
    ap.add_argument("--java", default="java")
    ap.add_argument("--shpos", help="engine stronghold helper, needed for obfuscated jars")
    ap.add_argument("--out", default=os.path.join(HERE, "data"))
    ap.add_argument("--port", type=int, default=25771)
    ap.add_argument("--accept-eula", action="store_true",
                    help="you accept Mojang's EULA (https://aka.ms/MinecraftEULA) for the temporary server;"
                    " required (or MINECRAFT_ACCEPT_EULA=1)")
    ap.add_argument("--bg-threads", type=int, help="limit worldgen worker threads (1.18.2 races)")
    ap.add_argument("--level-type", default="normal", choices=("normal", "amplified", "large_biomes"),
                    help="world type; other than normal, the output files get a _<type> suffix")
    ap.add_argument("--heap", default="6G", help="Java heap of the server (-Xmx)")
    ap.add_argument("--start-timeout", type=int, default=900,
                    help="seconds to wait for a server start (1.18.2 sometimes hangs; it is retried)")
    args = ap.parse_args()
    require_eula(args.accept_eula)

    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    jar = obtain_jar(args.mc, work, args.jar)
    jsha = sha1_of(jar)
    sdir, cmd, unobf = prepare_server_dir(args.mc, work, jar, args.java, args.heap)
    if args.bg_threads:
        # vanilla reads max.bg.threads for its worldgen worker pool
        cmd.insert(1, "-Dmax.bg.threads=%d" % args.bg_threads)
    if not unobf and not args.shpos:
        raise SystemExit("obfuscated jar: --shpos is required")
    cache = os.path.join(work, "results-%s%s.json" % (args.mc, suffix_of(args.level_type)))
    done = {}
    if os.path.exists(cache):
        with open(cache) as f:
            for r in json.load(f):
                r["start"] = tuple(r["start"])
                r["frames"] = [tuple(x) for x in r["frames"]]
                r["pieces"] = [tuple(x) for x in r["pieces"]]
                done.setdefault(r["seed"], []).append(r)
    logf = open(os.path.join(work, "server-%s.log" % args.mc), "a")
    for seed, idx in parse_seeds(args.seeds):
        if seed in done and done[seed][0].get("wanted", len(done[seed])) == len(idx):
            continue
        log("mc %s seed %d strongholds %s" % (args.mc, seed, idx))
        for attempt in range(3):
            try:
                res = run_seed(args.mc, sdir, cmd, unobf, seed, idx, args.shpos, args.port, logf,
                               args.level_type, args.start_timeout)
                break
            except (RuntimeError, TimeoutError) as e:
                # e.g. 1.18.2's sporadic "Missing Palette entry" worker crash
                log("  attempt %d failed: %s" % (attempt + 1, e))
                if attempt == 2:
                    raise
        for r in res:
            log("  sh %d start %s span %d eyes %d" % (r["sh_index"], r["start"], span_of(r["frames"]),
                                                      sum(f[3] for f in r["frames"])))
        done[seed] = res
        with open(cache, "w") as f:
            json.dump([r for rs in done.values() for r in rs], f)
    allres = [r for rs in done.values() for r in rs]
    write_outputs(args.mc, allres, args.out, jsha, unobf, args.level_type)
    log("summary", json.dumps(summary(allres)))


if __name__ == "__main__":
    main()
