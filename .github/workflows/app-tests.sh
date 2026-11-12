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
#
# The tests use their own settings and session folder, never the user's.
set -euo pipefail

app="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="${2:-$(mktemp -d)}"
mkdir -p "$work/settings"
cd "$work"
export SEED_ATLAS_TEST_SETTINGS_DIR="$work/settings"
export SEED_ATLAS_TEST_SETTINGS_ID="seed-atlas-ci"
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

if [[ "$failures" != 0 ]]; then
  echo "$failures failure(s); files in $work" >&2
  exit 1
fi
echo "all app tests passed"
