# Seed Atlas packaging

All commands below package the current source tree as version `4.2.dev0`.
The generated files are written to `dist/`.

## macOS DMG

The build script can create a universal Intel + Apple Silicon application when
used with Qt's universal macOS SDK.

Install Apple's command-line tools and the exact Qt version:

```bash
xcode-select --install
python3 -m pip install --user aqtinstall
python3 -m aqt install-qt mac desktop 6.8.3 clang_64 --outputdir "$HOME/Qt"
```

From the project root, build the DMG:

```bash
export QT_PREFIX="$HOME/Qt/6.8.3/macos"
export SEED_ATLAS_UNIVERSAL=1
bash installer/macos/build-dmg.sh 4.2.dev0
```

Output:

```text
dist/Seed-Atlas-4.2.dev0-macOS.dmg
```

The DMG is unsigned. Public distribution without Gatekeeper warnings requires
an Apple Developer ID certificate and notarization. Those credentials are not
required for local testing; use Finder's **Open** context-menu action for an
unsigned first launch.

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

The script installs the KDE/Qt 6.8 runtime for the current user and creates one
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
