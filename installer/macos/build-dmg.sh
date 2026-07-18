#!/usr/bin/env bash
set -euo pipefail

version="${1:-4.2.dev0}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../.." && pwd)"
build_dir="$project_root/build-macos"
dist_dir="$project_root/dist"
qt_prefix="${QT_PREFIX:-$(brew --prefix qt)}"

rm -rf "$build_dir"
mkdir -p "$build_dir" "$dist_dir"
cd "$build_dir"

qmake_args=(CONFIG+=release)
if [[ "${SEED_ATLAS_UNIVERSAL:-0}" == "1" ]]; then
  qmake_args+=(QMAKE_APPLE_DEVICE_ARCHS="x86_64 arm64")
fi
"$qt_prefix/bin/qmake" "${qmake_args[@]}" "$project_root/seed-atlas.pro"
make -j"$(sysctl -n hw.logicalcpu)"
"$qt_prefix/bin/macdeployqt" seed-atlas.app -dmg
mv -f seed-atlas.dmg "$dist_dir/Seed-Atlas-$version-macOS.dmg"

echo "Created: $dist_dir/Seed-Atlas-$version-macOS.dmg"
