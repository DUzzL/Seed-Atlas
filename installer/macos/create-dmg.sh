#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 APP_PATH OUTPUT_DMG [VOLUME_NAME]" >&2
  exit 2
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "DMG creation requires macOS" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app_path="$1"
output_dmg="$2"
volume_name="${3:-Seed Atlas}"
background="$script_dir/dmg-background.png"
layout_script="$script_dir/dmg-layout.applescript"

if [[ ! -d "$app_path/Contents" ]]; then
  echo "Invalid application bundle: $app_path" >&2
  exit 2
fi
if [[ ! -f "$background" ]]; then
  echo "Missing DMG background: $background" >&2
  exit 2
fi
if [[ ! -f "$layout_script" ]]; then
  echo "Missing Finder layout script: $layout_script" >&2
  exit 2
fi
if [[ -e "$output_dmg" ]]; then
  echo "Refusing to overwrite existing output: $output_dmg" >&2
  exit 2
fi
if [[ ! -d "$(dirname "$output_dmg")" ]]; then
  echo "Output directory does not exist: $(dirname "$output_dmg")" >&2
  exit 2
fi

for tool in hdiutil osascript SetFile sips ditto diskutil plutil; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required macOS tool: $tool" >&2
    exit 2
  fi
done

app_path="$(cd "$(dirname "$app_path")" && pwd)/$(basename "$app_path")"
output_dmg="$(cd "$(dirname "$output_dmg")" && pwd)/$(basename "$output_dmg")"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/seed-atlas-dmg.XXXXXX")"
dmg_root="$work_dir/root"
rw_dmg="$work_dir/Seed-Atlas-readwrite.dmg"
device=""

cleanup() {
  if [[ -n "$device" ]]; then
    hdiutil detach -force "$device" >/dev/null 2>&1 || true
  fi
  if [[ -d "$work_dir" && "$(basename "$work_dir")" == seed-atlas-dmg.* ]]; then
    rm -rf -- "$work_dir"
  fi
}
trap cleanup EXIT INT TERM

mkdir -p "$dmg_root/.background"
ditto --norsrc --noextattr --noqtn --noacl \
  "$app_path" "$dmg_root/Seed Atlas.app"
ln -s /Applications "$dmg_root/Applications"
ditto --norsrc --noextattr --noqtn --noacl \
  "$background" "$dmg_root/.background/dmg-background.png"
touch "$dmg_root/.metadata_never_index"
chflags hidden "$dmg_root/.background"

background_width="$(sips -g pixelWidth "$background" | awk '/pixelWidth:/ {print $2}')"
background_height="$(sips -g pixelHeight "$background" | awk '/pixelHeight:/ {print $2}')"
if [[ "$background_width" != "660" || "$background_height" != "400" ]]; then
  echo "DMG background must be exactly 660x400 pixels" >&2
  exit 1
fi

hdiutil create \
  -srcfolder "$dmg_root" \
  -volname "$volume_name" \
  -fs HFS+ \
  -format UDRW \
  -ov "$rw_dmg" >/dev/null

attach_output="$(hdiutil attach \
  -readwrite -nobrowse -noautoopen -owners off "$rw_dmg")"
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
finder_volume_name="$(basename "$mount_path")"

SetFile -a V "$mount_path/.background"

finder_ready=0
for _ in {1..40}; do
  if [[ "$(osascript \
      -e 'on run argv' \
      -e 'tell application "Finder" to return exists disk (item 1 of argv)' \
      -e 'end run' \
      "$finder_volume_name")" == "true" ]]; then
    finder_ready=1
    break
  fi
  sleep 0.25
done
if [[ "$finder_ready" != "1" ]]; then
  echo "Finder did not discover the mounted DMG" >&2
  exit 1
fi

osascript "$layout_script" "$finder_volume_name"

last_size="-1"
stable_count=0
for _ in {1..30}; do
  if [[ -s "$mount_path/.DS_Store" ]]; then
    current_size="$(stat -f %z "$mount_path/.DS_Store")"
    if [[ "$current_size" == "$last_size" ]]; then
      stable_count=$((stable_count + 1))
    else
      stable_count=0
      last_size="$current_size"
    fi
    if [[ "$stable_count" -ge 2 ]]; then
      break
    fi
  fi
  sleep 0.5
done
if [[ ! -s "$mount_path/.DS_Store" || "$stable_count" -lt 2 ]]; then
  echo "Finder did not finish writing the DMG layout" >&2
  exit 1
fi

sync
detached=0
for _ in {1..5}; do
  if hdiutil detach "$device" >/dev/null 2>&1; then
    detached=1
    device=""
    break
  fi
  sleep 1
done
if [[ "$detached" != "1" ]]; then
  echo "Could not detach the writable DMG" >&2
  exit 1
fi

hdiutil convert "$rw_dmg" \
  -format UDZO \
  -imagekey zlib-level=9 \
  -o "$output_dmg" >/dev/null
hdiutil verify "$output_dmg" >/dev/null

echo "Created custom DMG: $output_dmg"
