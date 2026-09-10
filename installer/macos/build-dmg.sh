#!/usr/bin/env bash
set -euo pipefail

version="${1:-4.2.dev0}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../.." && pwd)"
build_dir="$project_root/build-macos"
dist_dir="$project_root/dist"
build_id="${SEED_ATLAS_BUILD_ID:-$(git -C "$project_root" rev-parse --verify HEAD)}"
if [[ ! "$build_id" =~ ^[0-9a-fA-F]{40}$ ]]; then
  echo "Could not determine the Git commit used as the Seed Atlas build ID" >&2
  exit 2
fi
if [[ -n "${QT_PREFIX:-}" ]]; then
  qt_prefix="$QT_PREFIX"
else
  qt_prefix="$(brew --prefix qt 2>/dev/null || true)"
  if [[ ! -x "$qt_prefix/bin/qmake" ]]; then
    qt_prefix="$(brew --prefix qtbase)"
  fi
fi
sign_identity="${MACOS_SIGN_IDENTITY:-}"
notary_profile="${MACOS_NOTARY_PROFILE:-}"
apple_id="${APPLE_ID:-}"
apple_password="${APPLE_APP_SPECIFIC_PASSWORD:-}"
apple_team_id="${APPLE_TEAM_ID:-}"
build_only="${SEED_ATLAS_BUILD_ONLY:-0}"
package_only="${SEED_ATLAS_PACKAGE_ONLY:-0}"
unsigned="${SEED_ATLAS_UNSIGNED:-0}"
if [[ "$unsigned" == "1" ]]; then
  sign_identity=""
  notary_profile=""
  apple_id=""
  apple_password=""
  apple_team_id=""
fi

if [[ "$build_only" == "1" && "$package_only" == "1" ]]; then
  echo "SEED_ATLAS_BUILD_ONLY and SEED_ATLAS_PACKAGE_ONLY are mutually exclusive" >&2
  exit 2
fi

notary_credentials=0
if [[ -n "$notary_profile" ]]; then
  notary_credentials=1
elif [[ -n "$apple_id" || -n "$apple_password" || -n "$apple_team_id" ]]; then
  if [[ -z "$apple_id" || -z "$apple_password" || -z "$apple_team_id" ]]; then
    echo "APPLE_ID, APPLE_APP_SPECIFIC_PASSWORD, and APPLE_TEAM_ID must be set together" >&2
    exit 2
  fi
  notary_credentials=1
fi
if [[ "$notary_credentials" == "1" && -z "$sign_identity" ]]; then
  echo "MACOS_SIGN_IDENTITY is required for notarized distribution" >&2
  exit 2
fi
if [[ -n "$sign_identity" && "$notary_credentials" != "1" ]]; then
  echo "Notarization credentials are required with MACOS_SIGN_IDENTITY" >&2
  exit 2
fi

if [[ "$package_only" == "1" ]]; then
  if [[ ! -d "$build_dir/seed-atlas.app" ]]; then
    echo "Missing prebuilt app: $build_dir/seed-atlas.app" >&2
    exit 2
  fi
  mkdir -p "$dist_dir"
else
  rm -rf "$build_dir"
  mkdir -p "$build_dir" "$dist_dir"
  cd "$build_dir"

  qmake_args=(CONFIG+=release "SEED_ATLAS_BUILD_ID=$build_id")
  if [[ "${SEED_ATLAS_UNIVERSAL:-0}" == "1" ]]; then
    qmake_args+=(QMAKE_APPLE_DEVICE_ARCHS="x86_64 arm64")
  fi
  "$qt_prefix/bin/qmake" "${qmake_args[@]}" "$project_root/seed-atlas.pro"
  make -j"$(sysctl -n hw.logicalcpu)"

  if [[ "$build_only" == "1" ]]; then
    echo "Created unsigned app for later packaging: $build_dir/seed-atlas.app"
    exit 0
  fi
fi

# Package outside the source tree so iCloud/Finder cannot attach metadata while
# macdeployqt is signing the bundle.
package_dir="$(mktemp -d "${TMPDIR:-/tmp}/seed-atlas-package.XXXXXX")"
cleanup_package_dir() {
  rm -rf "$package_dir"
}
trap cleanup_package_dir EXIT
ditto --norsrc --noextattr --noqtn --noacl \
  "$build_dir/seed-atlas.app" "$package_dir/seed-atlas.app"
cd "$package_dir"
xattr -cr seed-atlas.app
export COPYFILE_DISABLE=1

deploy_args=(seed-atlas.app)
qt_libs="$("$qt_prefix/bin/qmake" -query QT_INSTALL_LIBS)"
if [[ -n "$qt_libs" ]]; then
  deploy_args+=("-libpath=$qt_libs")
fi
qtsvg_prefix=""
if command -v brew >/dev/null 2>&1; then
  qtsvg_prefix="$(brew --prefix qtsvg 2>/dev/null || true)"
  if [[ -n "$qtsvg_prefix" && "$qtsvg_prefix/lib" != "$qt_libs" ]]; then
    deploy_args+=("-libpath=$qtsvg_prefix/lib")
  fi
fi

qtsvg_framework=""
for candidate in \
  "$qt_prefix/lib/QtSvg.framework" \
  "$qt_libs/QtSvg.framework" \
  "${qtsvg_prefix:+$qtsvg_prefix/lib/QtSvg.framework}"
do
  if [[ -n "$candidate" && -d "$candidate" ]]; then
    qtsvg_framework="$candidate"
    break
  fi
done
if [[ -z "$qtsvg_framework" ]]; then
  echo "Missing QtSvg.framework; install the qtsvg Qt module" >&2
  exit 2
fi
mkdir -p "$package_dir/lib"
ditto --norsrc --noextattr --noqtn --noacl \
  "$qtsvg_framework" "$package_dir/lib/QtSvg.framework"

if [[ -n "$sign_identity" ]]; then
  deploy_args+=("-sign-for-notarization=$sign_identity")
else
  # Keep local builds internally consistent even without a paid Developer ID.
  # Ad-hoc signing does not satisfy Gatekeeper for public downloads.
  deploy_args+=("-codesign=-")
fi
deploy_log="$package_dir/macdeployqt.log"
"$qt_prefix/bin/macdeployqt" "${deploy_args[@]}" 2>&1 | tee "$deploy_log"
if grep -q '^ERROR:' "$deploy_log"; then
  echo "macdeployqt reported an error" >&2
  exit 1
fi
codesign --verify --deep --strict --verbose=2 seed-atlas.app
bash "$script_dir/create-dmg.sh" \
  "$package_dir/seed-atlas.app" "$package_dir/seed-atlas.dmg" "Seed Atlas"

if [[ "$notary_credentials" == "1" ]]; then
  codesign --force --timestamp --sign "$sign_identity" seed-atlas.dmg
  codesign --verify --strict --verbose=2 seed-atlas.dmg
  if [[ -n "$notary_profile" ]]; then
    xcrun notarytool submit seed-atlas.dmg \
      --keychain-profile "$notary_profile" --wait
  else
    xcrun notarytool submit seed-atlas.dmg \
      --apple-id "$apple_id" \
      --password "$apple_password" \
      --team-id "$apple_team_id" \
      --wait
  fi
  xcrun stapler staple seed-atlas.dmg
  xcrun stapler validate seed-atlas.dmg
  spctl --assess --type open --context context:primary-signature \
    --verbose=2 seed-atlas.dmg
elif [[ "$unsigned" != "1" ]]; then
  codesign --force --sign - seed-atlas.dmg
  codesign --verify --strict --verbose=2 seed-atlas.dmg
  echo "Created an ad-hoc-signed, non-notarized local DMG." >&2
  echo "Do not publish it as a Gatekeeper-clean release." >&2
fi

SEED_ATLAS_REQUIRE_UNIVERSAL="${SEED_ATLAS_UNIVERSAL:-0}" \
  SEED_ATLAS_UNSIGNED="$unsigned" \
  bash "$script_dir/verify-dmg.sh" "$package_dir/seed-atlas.dmg"

output_suffix=""
if [[ "$unsigned" == "1" ]]; then
  output_suffix="-UNSIGNED"
elif [[ "$notary_credentials" != "1" ]]; then
  output_suffix="-UNNOTARIZED"
fi
output_path="$dist_dir/Seed-Atlas-$version-macOS$output_suffix.dmg"
mv -f seed-atlas.dmg "$output_path"

printf '{\n  "buildId": "%s"\n}\n' "$build_id" > "$dist_dir/update.json"

echo "Created: $output_path"
echo "Created: $dist_dir/update.json"
