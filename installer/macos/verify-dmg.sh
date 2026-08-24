#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 DMG_PATH" >&2
  exit 2
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "DMG verification requires macOS" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dmg_path="$1"
if [[ ! -f "$dmg_path" ]]; then
  echo "Missing DMG: $dmg_path" >&2
  exit 2
fi
dmg_path="$(cd "$(dirname "$dmg_path")" && pwd)/$(basename "$dmg_path")"

hdiutil verify "$dmg_path" >/dev/null
codesign --verify --strict --verbose=2 "$dmg_path"

device=""
mount_path=""
cleanup() {
  if [[ -n "$mount_path" && -d "$mount_path" ]]; then
    volume_name="$(basename "$mount_path")"
    osascript \
      -e 'on run argv' \
      -e 'tell application "Finder" to if exists disk (item 1 of argv) then close container window of disk (item 1 of argv)' \
      -e 'end run' \
      "$volume_name" >/dev/null 2>&1 || true
  fi
  if [[ -n "$device" ]]; then
    hdiutil detach -force "$device" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

attach_output="$(hdiutil attach \
  -readonly -nobrowse -noautoopen -owners off "$dmg_path")"
device="$(printf '%s\n' "$attach_output" | awk '/^\/dev\// {value=$1} END {print value}')"
if [[ -z "$device" ]]; then
  echo "Could not determine the mounted DMG device" >&2
  exit 1
fi
mount_path="$(diskutil info -plist "$device" | \
  plutil -extract MountPoint raw -o - -)"
if [[ -z "$mount_path" || ! -d "$mount_path" ]]; then
  echo "Could not determine the mounted DMG path" >&2
  exit 1
fi

visible_entries="$(find "$mount_path" -mindepth 1 -maxdepth 1 \
  ! -name '.*' -exec basename {} \; | LC_ALL=C sort)"
expected_entries=$'Applications\nSeed Atlas.app'
if [[ "$visible_entries" != "$expected_entries" ]]; then
  echo "Unexpected visible DMG contents:" >&2
  printf '%s\n' "$visible_entries" >&2
  exit 1
fi
if [[ ! -L "$mount_path/Applications" || \
      "$(readlink "$mount_path/Applications")" != "/Applications" ]]; then
  echo "Applications must be a symlink to /Applications" >&2
  exit 1
fi
if [[ ! -s "$mount_path/.DS_Store" ]]; then
  echo "Missing Finder layout metadata" >&2
  exit 1
fi
if ! cmp -s "$script_dir/dmg-background.png" \
    "$mount_path/.background/dmg-background.png"; then
  echo "DMG background does not match the canonical asset" >&2
  exit 1
fi

background_width="$(sips -g pixelWidth \
  "$mount_path/.background/dmg-background.png" | \
  awk '/pixelWidth:/ {print $2}')"
background_height="$(sips -g pixelHeight \
  "$mount_path/.background/dmg-background.png" | \
  awk '/pixelHeight:/ {print $2}')"
if [[ "$background_width" != "660" || "$background_height" != "400" ]]; then
  echo "Unexpected DMG background dimensions" >&2
  exit 1
fi

app_path="$mount_path/Seed Atlas.app"
codesign --verify --deep --strict --verbose=2 "$app_path"
binary="$app_path/Contents/MacOS/seed-atlas"
if [[ ! -x "$binary" ]]; then
  echo "Missing Seed Atlas executable" >&2
  exit 1
fi
if [[ "${SEED_ATLAS_REQUIRE_UNIVERSAL:-0}" == "1" ]]; then
  architectures=" $(lipo -archs "$binary") "
  if [[ "$architectures" != *" arm64 "* || \
        "$architectures" != *" x86_64 "* ]]; then
    echo "Expected a universal arm64/x86_64 application" >&2
    exit 1
  fi
fi

volume_name="$(basename "$mount_path")"
finder_ready=0
for _ in {1..40}; do
  if [[ "$(osascript \
      -e 'on run argv' \
      -e 'tell application "Finder" to return exists disk (item 1 of argv)' \
      -e 'end run' \
      "$volume_name")" == "true" ]]; then
    finder_ready=1
    break
  fi
  sleep 0.25
done
if [[ "$finder_ready" != "1" ]]; then
  echo "Finder did not discover the mounted DMG" >&2
  exit 1
fi

layout="$(osascript \
  -e 'on run argv' \
  -e 'set volumeName to item 1 of argv' \
  -e 'tell application "Finder"' \
  -e 'tell disk volumeName' \
  -e 'open' \
  -e 'delay 1' \
  -e 'set w to container window' \
  -e 'set o to icon view options of w' \
  -e 'set b to bounds of w' \
  -e 'set a to position of item "Seed Atlas.app"' \
  -e 'set p to position of item "Applications"' \
  -e 'set resultText to ((item 1 of b) as text) & "," & ((item 2 of b) as text) & "," & ((item 3 of b) as text) & "," & ((item 4 of b) as text)' \
  -e 'set resultText to resultText & "|" & (icon size of o as text)' \
  -e 'set resultText to resultText & "|" & ((item 1 of a) as text) & "," & ((item 2 of a) as text)' \
  -e 'set resultText to resultText & "|" & ((item 1 of p) as text) & "," & ((item 2 of p) as text)' \
  -e 'close w' \
  -e 'return resultText' \
  -e 'end tell' \
  -e 'end tell' \
  -e 'end run' \
  "$volume_name")"
if [[ "$layout" != "120,120,780,520|128|170,205|490,205" ]]; then
  echo "Unexpected Finder layout: $layout" >&2
  exit 1
fi

hdiutil detach "$device" >/dev/null
device=""
mount_path=""

echo "Verified custom DMG layout: $dmg_path"
