#!/usr/bin/env bash
set -euo pipefail

version="${1:-4.2.dev0}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../.." && pwd)"
build_dir="$project_root/build-flatpak"
repo_dir="$project_root/build-flatpak-repo"
dist_dir="$project_root/dist"
manifest="$script_dir/org.seedatlas.SeedAtlas.yml"

command -v flatpak >/dev/null || { echo "flatpak is required" >&2; exit 1; }
command -v flatpak-builder >/dev/null || { echo "flatpak-builder is required" >&2; exit 1; }
architecture="$(flatpak --default-arch)"

flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.kde.Platform//6.8 org.kde.Sdk//6.8
mkdir -p "$dist_dir"
flatpak-builder --user --force-clean --default-branch=stable \
  --repo="$repo_dir" "$build_dir" "$manifest"
flatpak build-bundle "$repo_dir" \
  "$dist_dir/Seed-Atlas-$version-Linux-$architecture.flatpak" \
  org.seedatlas.SeedAtlas stable

echo "Created: $dist_dir/Seed-Atlas-$version-Linux-$architecture.flatpak"
