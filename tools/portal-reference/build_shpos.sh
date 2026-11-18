#!/bin/sh
# Build the engine stronghold-position helper out of tree (only needed for
# obfuscated, pre-26.1 server jars). Usage: build_shpos.sh <output binary>
set -e
here=$(cd "$(dirname "$0")" && pwd)
eng="$here/../../seedatlas-engine"
out=${1:?usage: build_shpos.sh <output binary>}
cc -fwrapv -ffp-contract=off -O3 -I"$eng" -o "$out" "$here/shpos.c" \
    "$eng/finders.c" "$eng/generator.c" "$eng/layers.c" "$eng/biomenoise.c" \
    "$eng/biomes.c" "$eng/noise.c" "$eng/util.c" "$eng/quadbase.c" -lm
