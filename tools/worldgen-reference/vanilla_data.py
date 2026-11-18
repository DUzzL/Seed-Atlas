#!/usr/bin/env python3
"""Vanilla data pack and data generator reports from an official server jar.

Two things, both taken from Mojang's own server jar (verified by the sha1 of
Mojang's version manifest) and written to directories OUTSIDE the repository:

  --data DIR     extract the built-in data pack: every data/minecraft/**.json
                 of the server jar (worldgen/, dimension_type/, tags/, ...) to
                 DIR/data/minecraft/..., plus version.json and a MANIFEST.txt.
  --reports DIR  run the official data generator
                 (java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar
                 --reports --output DIR/generated) and keep its output. The file
                 DIR/generated/reports/biome_parameters/minecraft/overworld.json
                 is the vanilla overworld biome parameter list in game order
                 (1.19+; 1.18.x has no such report).

Since 1.18 the server jar is a bundler: the real jar is nested at
META-INF/versions/<path of META-INF/versions.list>. Older jars hold the data
directly.

Usage:
  vanilla_data.py --mc 1.21.1 --work <scratch dir> --data <dir> --reports <dir>
  options: --jar FILE (official jar, else <work>/jars or download), --java,
           --only PREFIX (repeatable, e.g. worldgen/ dimension_type/ tags/worldgen/;
           default: everything under data/minecraft/)

Nothing here may be written into the repository: the files are Mojang's.
"""

import argparse
import io
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # keep the repo directory free of __pycache__
sys.path.insert(0, os.path.join(HERE, "..", "portal-reference"))

import portal_reference as pr  # noqa: E402

DATA_PREFIX = "data/minecraft/"


def refuse_repo(path):
    """The extracted files are Mojang's: never write them into the repository.

    Compares directories with os.path.samefile() (the same file system
    object), not path strings, which differ in case on case-insensitive file
    systems (APFS) or through links: the nearest existing directory of @path
    and every directory above it must not be the repository.
    """
    repo = os.path.join(HERE, "..", "..")
    cur = os.path.abspath(path)
    while not os.path.exists(cur):
        parent = os.path.dirname(cur)
        if parent == cur:
            return
        cur = parent
    while True:
        if os.path.samefile(cur, repo):
            raise SystemExit("refusing to write vanilla data into the repository: %s" % path)
        parent = os.path.dirname(cur)
        if parent == cur:
            return
        cur = parent


def inner_jar(jar):
    """Returns (ZipFile of the jar that holds the game, nested path or None)."""
    outer = zipfile.ZipFile(jar)
    names = set(outer.namelist())
    if "META-INF/versions.list" not in names:
        return outer, None
    # lines: <sha256>\t<version id>\t<path below META-INF/versions/>
    line = outer.read("META-INF/versions.list").decode().strip().splitlines()[0]
    nested = "META-INF/versions/" + line.split("\t")[2]
    data = outer.read(nested)
    outer.close()
    return zipfile.ZipFile(io.BytesIO(data)), nested


def extract_data(jar, out, only=()):
    """Extract data/minecraft/**.json; returns (number of files, nested jar path)."""
    z, nested = inner_jar(jar)
    count = 0
    with z:
        wanted = []
        for name in sorted(z.namelist()):
            if not name.startswith(DATA_PREFIX) or not name.endswith(".json"):
                continue
            rel = name[len(DATA_PREFIX):]
            if only and not any(rel.startswith(p) for p in only):
                continue
            parts = name.split("/")
            if ".." in parts or name.startswith("/") or "\\" in name:
                raise SystemExit("suspicious path in jar: %r" % name)
            wanted.append(name)
        for name in wanted:
            dst = os.path.join(out, *name.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(z.read(name))
            count += 1
        for extra in ("version.json",):
            if extra in z.namelist():
                with open(os.path.join(out, extra), "wb") as f:
                    f.write(z.read(extra))
    return count, nested


def run_reports(jar, java, out, work, mc):
    """Run vanilla's data generator; returns the path of the overworld parameter report (or None)."""
    # The bundler unpacks its libraries into the working directory: use a scratch one.
    run_dir = os.path.join(work, "datagen-%s" % mc)
    os.makedirs(run_dir, exist_ok=True)
    gen = os.path.join(out, "generated")
    if os.path.isdir(gen):
        shutil.rmtree(gen)
    z, nested = inner_jar(jar)
    z.close()
    if nested:
        cmd = [java, "-DbundlerMainClass=net.minecraft.data.Main", "-jar", os.path.abspath(jar)]
    else:
        cmd = [java, "-cp", os.path.abspath(jar), "net.minecraft.data.Main"]
    cmd += ["--reports", "--output", os.path.abspath(gen)]
    with open(os.path.join(out, "datagen.log"), "w") as logf:
        logf.write(" ".join(cmd) + "\n")
        logf.flush()
        r = subprocess.run(cmd, cwd=run_dir, stdout=logf, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise SystemExit("data generator failed (exit %d), see %s"
                         % (r.returncode, os.path.join(out, "datagen.log")))
    rep = os.path.join(gen, "reports", "biome_parameters", "minecraft", "overworld.json")
    return rep if os.path.exists(rep) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mc", required=True)
    ap.add_argument("--work", required=True, help="scratch dir for the jar and the data generator run")
    ap.add_argument("--jar", help="official server jar (verified by sha1)")
    ap.add_argument("--java", default="java")
    ap.add_argument("--data", help="output directory of the extracted data pack")
    ap.add_argument("--reports", help="output directory of the data generator reports")
    ap.add_argument("--only", action="append", default=[],
                    help="extract only paths below data/minecraft/ with this prefix (repeatable)")
    args = ap.parse_args()
    if not args.data and not args.reports:
        raise SystemExit("nothing to do: give --data and/or --reports")

    work = os.path.abspath(args.work)
    for d in (work, args.data, args.reports):
        if d:
            refuse_repo(d)
            os.makedirs(d, exist_ok=True)
    jar = pr.obtain_jar(args.mc, work, args.jar)
    jsha = pr.sha1_of(jar)
    pr.log("mc %s, server jar %s, sha1 %s" % (args.mc, jar, jsha))

    if args.data:
        n, nested = extract_data(jar, args.data, args.only)
        with open(os.path.join(args.data, "MANIFEST.txt"), "w") as f:
            f.write("vanilla Minecraft %s data pack, extracted by tools/worldgen-reference/vanilla_data.py\n"
                    "server jar sha1 %s\nnested jar %s\nfiles %d (data/minecraft/**.json%s)\n"
                    % (args.mc, jsha, nested or "-", n,
                       ", only: " + " ".join(args.only) if args.only else ""))
        pr.log("extracted %d files to %s" % (n, os.path.join(args.data, "data", "minecraft")))
    if args.reports:
        rep = run_reports(jar, args.java, args.reports, work, args.mc)
        pr.log("reports in %s" % os.path.join(args.reports, "generated", "reports"))
        pr.log("overworld biome parameters: %s" % (rep or "not produced by this version"))


if __name__ == "__main__":
    main()
