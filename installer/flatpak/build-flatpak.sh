#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../.." && pwd)"
build_dir="$project_root/build-flatpak"
repo_dir="$project_root/build-flatpak-repo"
dist_dir="$project_root/dist"
manifest="$script_dir/org.seedatlas.SeedAtlas.yml"
# version (argument or VERSION.txt), build_id, build_time
source "$script_dir/../release-info.sh" "$project_root" "${1:-}"

command -v flatpak >/dev/null || { echo "flatpak is required" >&2; exit 1; }
command -v flatpak-builder >/dev/null || { echo "flatpak-builder is required" >&2; exit 1; }
architecture="$(flatpak --default-arch)"

flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
mkdir -p "$dist_dir"
build_manifest="$(mktemp "$script_dir/seed-atlas-build.XXXXXX.yml")"
trap 'rm -f "$build_manifest"' EXIT
sed -e 's/${SEED_ATLAS_BUILD_ID}/'"$build_id"'/g' \
  -e 's/${SEED_ATLAS_BUILD_TIME}/'"$build_time"'/g' "$manifest" > "$build_manifest"
flatpak-builder --user --force-clean --default-branch=stable \
  --repo="$repo_dir" "$build_dir" "$build_manifest"
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo "$repo_dir" \
  "$dist_dir/Seed-Atlas-$version-Linux-$architecture.flatpak" \
  org.seedatlas.SeedAtlas stable

write_update_json "$dist_dir/update.json"

echo "Created: $dist_dir/Seed-Atlas-$version-Linux-$architecture.flatpak"
echo "Created: $dist_dir/update.json"
