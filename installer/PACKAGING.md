# Seed Atlas packaging

All commands below package the current source tree as version `4.2.dev0`.
The generated files are written to `dist/`.

For the complete unsigned three-platform build procedure and detailed manual
`update.json` publishing, testing, and troubleshooting instructions, see the
[build and update guide](../buildguide.md).

## macOS DMG

The build script can create a universal Intel + Apple Silicon application when
used with Qt's universal macOS SDK.

Install Apple's command-line tools and the exact Qt version:

```bash
xcode-select --install
python3 -m pip install --user aqtinstall
python3 -m aqt install-qt mac desktop 6.8.3 clang_64 \
  --outputdir "$HOME/Qt"
```

From the project root, build the DMG:

```bash
export QT_PREFIX="$HOME/Qt/6.8.3/macos"
export SEED_ATLAS_UNIVERSAL=1
bash installer/macos/build-dmg.sh 4.2.dev0
```

Output:

```text
dist/Seed-Atlas-4.2.dev0-macOS-UNNOTARIZED.dmg
```

To explicitly build without a Developer ID certificate, notarization, or a DMG
signature, set `SEED_ATLAS_UNSIGNED=1`. The result ends in `-UNSIGNED.dmg`.
The application inside retains only the ad-hoc code signatures needed to run
on Apple Silicon; these do not identify a publisher. Use the same variable when
running `verify-dmg.sh` on that image.

The image opens as a fixed, minimal Finder window with `Seed Atlas.app` on the
left and an `Applications` link on the right. The arrow between them indicates
the standard drag-to-install action. The build fails if that layout, the link,
or the background is missing. You can verify an existing image separately:

```sh
SEED_ATLAS_REQUIRE_UNIVERSAL=1 \
  bash installer/macos/verify-dmg.sh \
  dist/Seed-Atlas-4.2.dev0-macOS-UNNOTARIZED.dmg
```

Without signing credentials the script creates an ad-hoc-signed local DMG and
clearly marks it as unsuitable for public distribution. To create a public DMG
that Gatekeeper accepts, set a **Developer ID Application** identity plus
notarization credentials:

```sh
export MACOS_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
export APPLE_ID="developer@example.com"
export APPLE_APP_SPECIFIC_PASSWORD="xxxx-xxxx-xxxx-xxxx"
export APPLE_TEAM_ID="TEAMID"
bash installer/macos/build-dmg.sh 4.2.dev0
```

A successful notarized build uses the public release name
`dist/Seed-Atlas-4.2.dev0-macOS.dmg`.

As an alternative to the three Apple variables, store notary credentials with
`xcrun notarytool store-credentials` and set `MACOS_NOTARY_PROFILE` to that
profile name. The script signs the app with hardened runtime and a secure
timestamp, submits the DMG to Apple, staples the ticket, and verifies the final
artifact.

The GitHub workflow targets a `macos-release` environment and deliberately
refuses to publish a macOS artifact until these environment secrets exist:

- `MACOS_CERTIFICATE_P12`: base64-encoded Developer ID Application `.p12`
- `MACOS_CERTIFICATE_PASSWORD`: password used when exporting the `.p12`
- `APPLE_ID`: Apple developer account email
- `APPLE_APP_SPECIFIC_PASSWORD`: app-specific Apple ID password
- `APPLE_TEAM_ID`: ten-character Apple Developer team ID

Before the first run, create that environment, restrict it to the release
branch/tags, require a reviewer, enable **Prevent self-review**, and store the
credentials only there. This prevents a manually dispatched workflow from an
arbitrary ref from accessing the Developer ID credentials.

For a one-off trusted local test of an unsigned older build, Finder's
**Open** context-menu action can approve it. Do not present that workaround as
a signed release.

## Linux Flatpak

On Ubuntu or Debian, install the required tools:

```bash
sudo apt update
sudo apt install flatpak flatpak-builder
```

Then run from the project root:

```bash
bash installer/flatpak/build-flatpak.sh 4.2.dev0
```

The script installs the supported KDE/Qt 6.10 runtime for the current user and creates one
of these, depending on the Linux machine's architecture:

```text
dist/Seed-Atlas-4.2.dev0-Linux-x86_64.flatpak
dist/Seed-Atlas-4.2.dev0-Linux-aarch64.flatpak
```

Install and test the bundle locally with:

```bash
flatpak install --user ./dist/Seed-Atlas-4.2.dev0-Linux-*.flatpak
flatpak run org.seedatlas.SeedAtlas
```

## Build Linux and macOS without local machines

The workflow `.github/workflows/native-packages.yml` builds both packages on
native GitHub-hosted systems. After this folder is pushed to a GitHub
repository, open **Actions**, select **Native packages**, and choose
**Run workflow**. The finished run contains the Flatpak and DMG as downloadable
artifacts.

## Windows

Run in PowerShell from the project root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File installer/windows/build-installer.ps1
```

This produces the portable directory, corresponding source archive, and Inno
Setup installer in `dist/`.

## Publishing an update manually

Each packaging script embeds the current Git commit as Seed Atlas' internal
build ID and writes the matching `dist/update.json`. Commit the release source
before building so all three platforms receive the same build ID.
For a source snapshot without a commit, all three scripts also accept
`SEED_ATLAS_BUILD_ID` as a 40-character hexadecimal release identifier (not
necessarily a content hash). Pass the exact same lowercase ID to every platform
and use a new ID whenever the packaged application changes.

Replace the three installers in the existing GitHub release as usual. Upload
`update.json` last, after every installer is available. Installed copies then
compare their embedded build ID with:

```text
https://github.com/DUzzL/Seed-Atlas/releases/latest/download/update.json
```

When the IDs differ, Seed Atlas offers to open the latest GitHub release page.
Choosing **Update** opens that page, removes the installed application while
preserving user settings, and exits. Choosing **Not now** shows the prompt again
on the next start. **Do not ask again** disables startup checks and can be
reversed in Seed Atlas' general settings.
