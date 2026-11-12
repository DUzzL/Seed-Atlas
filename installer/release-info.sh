# Release information shared by the packaging scripts (sourced, not run).
#
#   source installer/release-info.sh "$project_root" "${1:-}"
#
# sets
#   version     the first argument, or else the content of VERSION.txt
#   build_id    $SEED_ATLAS_BUILD_ID, or else the checked-out commit
#   build_time  $SEED_ATLAS_BUILD_TIME, or else the commit time of build_id
#               (seconds since 1970; the update check orders builds by it)
# and defines
#   write_update_json FILE    writes the update manifest of this build
#   copy_license_files DIR    copies the license texts and notices to DIR

release_root="$1"
version="${2:-}"
if [[ -z "$version" ]]; then
  version="$(tr -d '[:space:]' < "$release_root/VERSION.txt")"
fi
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.(dev)?[0-9]+$ ]]; then
  echo "Invalid Seed Atlas version: $version" >&2
  exit 2
fi
build_id="${SEED_ATLAS_BUILD_ID:-$(git -C "$release_root" rev-parse --verify HEAD)}"
if [[ ! "$build_id" =~ ^[0-9a-fA-F]{40}$ ]]; then
  echo "Could not determine the Git commit used as the Seed Atlas build ID" >&2
  exit 2
fi
build_id="$(printf '%s' "$build_id" | tr 'A-F' 'a-f')"
build_time="${SEED_ATLAS_BUILD_TIME:-$(git -C "$release_root" show -s --format=%ct "$build_id")}"
if [[ ! "$build_time" =~ ^[0-9]+$ ]]; then
  echo "Could not determine the commit time used as the Seed Atlas build time" >&2
  exit 2
fi

# "buildId" is all that builds before 4.3 read; newer builds also use
# "buildTime" to offer only newer releases.
write_update_json() {
  printf '{\n  "buildId": "%s",\n  "buildTime": %s,\n  "version": "%s"\n}\n' \
    "$build_id" "$build_time" "$version" > "$1"
}

copy_license_files() {
  mkdir -p "$1"
  cp "$release_root/LICENSE" "$1/LICENSE.txt"
  cp "$release_root/installer/licenses/LGPL-3.0.txt" "$1/LGPL-3.0.txt"
  cp "$release_root/THIRD_PARTY_NOTICES.md" "$1/THIRD_PARTY_NOTICES.md"
  cp "$release_root/LEGAL_NOTICE.md" "$1/LEGAL_NOTICE.md"
  cp "$release_root/SOURCE_CODE.md" "$1/SOURCE_CODE.md"
  cp "$release_root/seedatlas-engine/LICENSE" "$1/cubiomes-LICENSE.txt"
}
