#!/usr/bin/env bash
# Headless tests of a built Seed Atlas executable, run by tests.yml:
#   bash .github/workflows/app-tests.sh PATH/TO/seed-atlas [WORK_DIR]
#
# 1. Known seeds: example filters from rc/examples over a fixed seed list. The
#    list holds seeds that matched in an earlier build plus 300 pseudo-random
#    seeds that match none of the filters; the results must be exactly the
#    known seeds.
# 2. Session round trip: a result file is a session; loading it again must
#    reproduce the same header and the same seeds, including the world
#    generation lines (#EstTerrain, #StructSalt).
# 3. A session with a broken line and a closed stdin must end, not hang.
# 4. World type: sessions from before Amplified existed ("#Large:" only) keep
#    their header, "#Amplified: 1" survives a round trip and is dropped for a
#    version without Amplified, and an Amplified search uses Amplified terrain.
# 4a. The MCP server (seed-atlas --mcp): protocol, every tool against known
#    values, searches, rights; datapack worlds with SEED_ATLAS_TEST_VANILLA.
# 4b. The download of the game data asks before anything is requested from
#    Mojang (GUI, off screen; the question is answered with Cancel). Only with
#    SEED_ATLAS_TEST_VANILLA (see 5) or SEED_ATLAS_TEST_GUI=1.
#
# 5. Datapack worlds (GUI, off screen): only with SEED_ATLAS_TEST_VANILLA set
#    to the vanilla data of Minecraft 1.21.1 (the game's own 1.21.1.jar, the
#    server jar, or a folder with data/minecraft/worldgen). That data is
#    Mojang's and not part of the repository, so CI skips this test. It loads
#    the project's test pack, compares the map tiles with the datapack
#    generator at every zoom level, and checks names, highlight, switching
#    packs off and on, removal (back to the built-in map, pixel for pixel), a
#    missing pack, an unsupported version and the settings across two starts.
#    It also runs Seed Finder searches with datapack conditions and the biome
#    analysis (counts and areas) and compares them with a brute force of the
#    generator, and reads a session with datapack conditions without the packs.
#    With the structure test pack it compares the structures on the map, the
#    structure analysis and structure conditions of the search with the
#    generator's structure finder, the height and terrain layers with its
#    heights, the Nether and the End, and checks the pack check report.
#
# The tests use their own settings and session folder, never the user's.
set -euo pipefail

app="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="${2:-$(mktemp -d)}"
mkdir -p "$work/settings"
cd "$work"
export SEED_ATLAS_TEST_SETTINGS_DIR="$work/settings"
export SEED_ATLAS_TEST_SETTINGS_ID="${SEED_ATLAS_TEST_SETTINGS_ID:-seed-atlas-ci}"
export QT_QPA_PLATFORM=offscreen
python="$(command -v python3 || command -v python)"
failures=0

fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

# run_app LIMIT_SECONDS LOG ARGS...: runs the app with stdin closed, kills it
# after the limit; returns its exit code (124 on timeout)
run_app() {
  local limit="$1" log="$2"
  shift 2
  "$app" "$@" < /dev/null > "$log" 2>&1 &
  local pid=$! waited=0
  while kill -0 "$pid" 2> /dev/null; do
    if (( waited >= limit * 10 )); then
      kill -9 "$pid" 2> /dev/null || true
      wait "$pid" 2> /dev/null || true
      return 124
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  local rc=0
  wait "$pid" || rc=$?
  return "$rc"
}

# seeds of a result file or seed list, sorted
seeds_of() {
  grep -v '^#' "$1" | tr -d '\r' | sed '/^$/d' | LC_ALL=C sort -u
}

"$python" - <<'PY' > random.txt
x = 0x5EED
for _ in range(300):
    x = (x * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
    print(x - (1 << 64) if x >= 1 << 63 else x)
PY

# Seeds found with these filters for 26.3 (pinned below, so that a new
# MC_NEWEST does not change the test), one filter set per line: the example
# in rc/examples, then its known seeds.
cat > known.txt <<'KNOWN'
portal_village_or_treasure 1026541 1331591 1478592 1614654 1754453 1795438 1974938 2003490 2136267 2225433 2333307 2636625 3173674 3188508 3205521 33969 3608843 3927567 3961387 4017579 4035358 4110202 412876 4174306 4378839 4666871 4725725 48667 5463446 5634812 5725599 5731038 6248404 6365658 6418606 6490976 6512904 741194 827782 859165 
mushroom_icespike 35492 40743 50578 78063 84404 86992 88937 93410 94149 
two_zombie_villages 228856 407271 73338 
sinkhole 10435443 10725630 1961842 891990 
biome_diversity_1_18 131296 203511 
large_birch_forest_1_18 1735176 440361 52096 
huge_jungle_1_18 1207081 2563667
KNOWN

echo "== known seeds"
while read -r name seeds; do
  printf '%s\n' $seeds > "known.$name"
  cat "known.$name" random.txt > "list.$name"
  {
    echo "#Version:  4.3.-1"
    echo "#MC:       26.3"
    echo "#Search:   2"
    echo "#List64:   list.$name"
    echo "#ResStop:  0"
    grep -E '^#(Cond|Mode48|HutQual|MonArea):' "$root/rc/examples/$name.txt" | tr -d '\r'
  } > "$name.session"
  rc=0
  run_app 300 "$name.log" --nogui "--session=$name.session" "--out=$name.out" || rc=$?
  if [[ "$rc" != 0 ]]; then
    fail "$name: exit code $rc"
    tail -c 2000 "$name.log" >&2 || true
    continue
  fi
  if ! diff <(seeds_of "known.$name") <(seeds_of "$name.out") > "$name.diff"; then
    fail "$name: results differ from the known seeds (< expected, > found)"
    cat "$name.diff" >&2
  else
    echo "ok: $name ($(seeds_of "known.$name" | wc -l | tr -d ' ') seeds)"
  fi
done < known.txt

echo "== session round trip"
{
  cat sinkhole.session
  echo "#EstTerrain: 0"
  echo "#StructSalt: village 12345"
} > roundtrip.session
rc=0
run_app 300 roundtrip1.log --nogui --session=roundtrip.session --out=roundtrip1.out || rc=$?
[[ "$rc" == 0 ]] || fail "round trip, first run: exit code $rc"
rc=0
run_app 300 roundtrip2.log --nogui --session=roundtrip1.out --out=roundtrip2.out || rc=$?
[[ "$rc" == 0 ]] || fail "round trip, second run: exit code $rc"
header() {
  grep '^#' "$1" | tr -d '\r' | grep -Ev '^#(Time|Progress|Threads):'
}
if ! diff <(header roundtrip1.out) <(header roundtrip2.out); then
  fail "round trip: the session header changed"
fi
for line in "#EstTerrain: 0" "#StructSalt: village 12345"; do
  grep -qxF "$line" <(header roundtrip2.out) || fail "round trip: \"$line\" was lost"
done
if ! diff <(seeds_of roundtrip1.out) <(seeds_of roundtrip2.out); then
  fail "round trip: the seeds changed"
fi
grep -v '^#' roundtrip2.out | tr -d '\r' | sed '/^$/d' | LC_ALL=C sort | uniq -d > roundtrip.dups
[[ ! -s roundtrip.dups ]] || fail "round trip: duplicate seeds in the result file"
[[ "$failures" != 0 ]] || echo "ok: round trip"

echo "== broken session with closed stdin"
{ cat sinkhole.session; echo "not-a-seed"; } > broken.session
rc=0
run_app 30 broken.log --nogui --session=broken.session --out=broken.out || rc=$?
if [[ "$rc" == 124 ]]; then
  fail "broken session: still running after 30 s (waiting for input?)"
fi
size="$(wc -c < broken.log | tr -d ' ')"
if (( size > 100000 )); then
  fail "broken session: $size bytes of output"
fi
[[ "$rc" == 124 || "$size" -gt 100000 ]] || echo "ok: broken session ended (exit code $rc)"

echo "== world type"
before="$failures"
# wt_session MC WORLD_TYPE_LINES...: a session over the seeds of wt.list
printf '%s\n' 1 2 3 > wt.list
wt_session() {
  local mc="$1"
  shift
  echo "#Version:  4.3.-1"
  echo "#MC:       $mc"
  printf '%s\n' "$@"
  echo "#Search:   2"
  echo "#List64:   wt.list"
  echo "#ResStop:  0"
  grep '^#Cond:' mushroom_icespike.session
}
# wt_run NAME: runs NAME.session, the result file is NAME.out
wt_run() {
  local rc=0
  run_app 120 "$1.log" --nogui "--session=$1.session" "--out=$1.out" || rc=$?
  [[ "$rc" == 0 ]] || fail "world type, $1: exit code $rc"
  [[ -f "$1.out" ]] || : > "$1.out"
}
has_line() {
  grep -qxF "$2" <(tr -d '\r' < "$1")
}
# as written by every version before Amplified existed
wt_session 26.3 "#Large:    1" > wt-large.session
wt_run wt-large
has_line wt-large.out "#Large:    1" || fail "world type: \"#Large: 1\" was lost"
! grep -q '^#Amplified' wt-large.out || fail "world type: a Large Biomes session became Amplified"
wt_session 26.3 > wt-default.session
wt_run wt-default
has_line wt-default.out "#Large:    0" || fail "world type: a default session lost \"#Large: 0\""
! grep -q '^#Amplified' wt-default.out || fail "world type: a default session got an \"#Amplified\" line"
wt_session 26.3 "#Large:    0" "#Amplified: 1" > wt-amplified.session
wt_run wt-amplified
cp wt-amplified.out wt-amplified2.session
wt_run wt-amplified2
for out in wt-amplified.out wt-amplified2.out; do
  has_line "$out" "#Amplified: 1" || fail "world type: \"#Amplified: 1\" was lost in $out"
  has_line "$out" "#Large:    0" || fail "world type: Amplified with Large Biomes in $out"
done
# there is no Amplified before 1.18 (the lines can come in any order)
wt_session 1.17 "#Large:    0" "#Amplified: 1" > wt-old.session
wt_run wt-old
! grep -q '^#Amplified' wt-old.out || fail "world type: Amplified was kept for 1.17"
has_line wt-old.out "#MC:       1.17" || fail "world type: the version of the 1.17 session changed"
# The world type has to reach the search. The height condition of the
# sinkhole example, as "surface at the origin at y 200 or above": no default
# world of the list has that, Amplified terrain does.
height="$(grep '^#Cond: 3e' "$root/rc/examples/sinkhole.txt" | head -n 1 | tr -d '\r' \
  | sed -e 's/0900000007000000/0100000000000000/' -e 's/c4ffffffffffff7f/00000080c8000000/')"
if [[ "$height" != *0100000000000000*00000080c8000000* ]]; then
  fail "world type: the height condition of the sinkhole example changed"
else
  seq 0 2999 > wt-height.list
  for type in default amplified; do
    {
      echo "#Version:  4.3.-1"
      echo "#MC:       26.3"
      if [[ "$type" == amplified ]]; then echo "#Amplified: 1"; fi
      echo "#Search:   2"
      echo "#List64:   wt-height.list"
      echo "#ResStop:  0"
      echo "$height"
    } > "wt-height-$type.session"
    wt_run "wt-height-$type"
  done
  [[ -z "$(seeds_of wt-height-default.out)" ]] \
    || fail "world type: a default world with its surface at y 200 or above at the origin"
  [[ -n "$(seeds_of wt-height-amplified.out)" ]] \
    || fail "world type: no Amplified world with its surface at y 200 or above at the origin"
fi
[[ "$failures" != "$before" ]] || echo "ok: world type"

echo "== MCP server"
# The server (seed-atlas --mcp) as an MCP client sees it: see mcp-test.py. It
# runs with settings of its own, which must stay unwritten.
before="$failures"
mcp_id="$SEED_ATLAS_TEST_SETTINGS_ID-mcp"
mkdir -p "$work/mcp-settings"
if ! SEED_ATLAS_TEST_SETTINGS_ID="$mcp_id" SEED_ATLAS_TEST_SETTINGS_DIR="$work/mcp-settings" \
    "$python" "$root/.github/workflows/mcp-test.py" "$app" "$root" "$work"; then
  fail "MCP server (logs: $work/mcp-*.log)"
fi
if [[ -n "$(ls -A "$work/mcp-settings")" || -e "$HOME/Library/Preferences/com.$mcp_id.$mcp_id.plist" \
    || -e "$HOME/.config/$mcp_id" ]]; then
  fail "MCP server: it wrote settings"
fi
[[ "$failures" != "$before" ]] || echo "ok: MCP server"

echo "== game data download: the user is asked before Mojang is contacted"
# A GUI test (off screen), like the datapack test below: it runs where that one
# runs, or with SEED_ATLAS_TEST_GUI=1. The question is answered with Cancel;
# the manifest address points at a port that does not answer, should anything
# be requested anyway.
if [[ -z "${SEED_ATLAS_TEST_VANILLA:-}" && -z "${SEED_ATLAS_TEST_GUI:-}" ]]; then
  echo "skipped: game data download question (GUI test; set SEED_ATLAS_TEST_GUI=1)"
else
  rc=0
  SEED_ATLAS_UI_SNAPSHOT="$work/consent.png" SEED_ATLAS_UI_TEST_VANILLA_CONSENT=1 \
    SEED_ATLAS_TEST_VANILLA_MANIFEST="http://127.0.0.1:9/version_manifest_v2.json" \
    run_app 120 consent.log || rc=$?
  if [[ "$rc" != 0 ]] || ! grep -q "vanilla consent: asked first 1, saved 1, idle after cancel 1" consent.log; then
    fail "game data download: no question before the first request (exit code $rc)"
    tail -c 2000 consent.log >&2 || true
  else
    echo "ok: game data download asks first"
  fi
fi

echo "== datapack worlds"
before="$failures"
if [[ -z "${SEED_ATLAS_TEST_VANILLA:-}" ]]; then
  echo "skipped: datapack worlds (set SEED_ATLAS_TEST_VANILLA to the vanilla data of" \
    "Minecraft 1.21.1 to run this test; the data is not part of the repository)"
elif [[ ! -e "$SEED_ATLAS_TEST_VANILLA" ]]; then
  fail "datapack worlds: SEED_ATLAS_TEST_VANILLA does not exist: $SEED_ATLAS_TEST_VANILLA"
else
  # The packs of the test, made from the project's test pack: the pack itself,
  # one with two biomes of the same name, one without a dimension file (the
  # world type decides) and one that cannot be loaded; and the test pack with
  # structures (structures, heights, Nether and End, the pack check).
  "$python" - "$root/tools/worldgen-reference/testpack" "$work/dp" \
      "$root/tools/worldgen-reference/testpack-structures" <<'PY'
import os, shutil, sys
src, out = sys.argv[1], sys.argv[2]
shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)
shutil.copytree(sys.argv[3], os.path.join(out, "testpack-structures"))
def copy(name):
    dst = os.path.join(out, name)
    shutil.copytree(src, dst)
    return dst
copy("testpack")
d = copy("collide")
for folder, _, files in os.walk(d):
    for name in files:
        path = os.path.join(folder, name)
        if name.endswith((".json", ".mcmeta")):
            with open(path, encoding="utf-8") as f:
                text = f.read()
            if "testpack:amber_plains" in text:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text.replace("testpack:amber_plains", "testpack:plains"))
        if name == "amber_plains.json":
            os.rename(path, os.path.join(folder, "plains.json"))
d = copy("nodim")
shutil.rmtree(os.path.join(d, "data", "minecraft", "dimension"))
d = copy("broken")
with open(os.path.join(d, "data", "testpack", "worldgen", "density_function", "depth",
        "offset.json"), "w", encoding="utf-8") as f:
    f.write('{\n  "type": "testpack:swirl",\n  "argument": 0.5\n}\n')
PY
  rc=0
  SEED_ATLAS_UI_SNAPSHOT="$work/datapack1.png" SEED_ATLAS_UI_TEST_DATAPACK=1 \
    SEED_ATLAS_TEST_DATAPACK_DIR="$work/dp" SEED_ATLAS_TEST_DATAPACK_REPORT="$work/datapack-report.txt" \
    run_app 900 datapack1.log || rc=$?
  if [[ "$rc" != 0 ]]; then
    fail "datapack worlds: exit code $rc"
    grep 'datapack test: FAIL' datapack1.log >&2 || tail -c 2000 datapack1.log >&2 || true
  else
    # a second start has to bring the packs of the first one back
    rc=0
    SEED_ATLAS_UI_SNAPSHOT="$work/datapack2.png" SEED_ATLAS_UI_TEST_DATAPACK=restore \
      SEED_ATLAS_TEST_DATAPACK_DIR="$work/dp" run_app 300 datapack2.log || rc=$?
    if [[ "$rc" != 0 ]]; then
      fail "datapack worlds, second start: exit code $rc"
      grep 'datapack test: FAIL' datapack2.log >&2 || tail -c 2000 datapack2.log >&2 || true
    fi
  fi
  [[ "$failures" != "$before" ]] || echo "ok: datapack worlds"
fi

if [[ "$failures" != 0 ]]; then
  echo "$failures failure(s); files in $work" >&2
  exit 1
fi
echo "all app tests passed"
