# Seed Atlas: Build- und Update-Anleitung

Diese Anleitung beschreibt den manuellen Release-Ablauf: Windows-Installer,
macOS-DMG und Linux-Flatpak bauen, auf GitHub austauschen und mit `update.json`
die Update-Meldung in bereits installierten Apps auslösen. Eine fortlaufende
öffentliche Versionsnummer ist dafür nicht erforderlich.

Die Beispiele entsprechen dem Stand vom 11. November 2026. Sie bauen ohne
Publisher-Signaturen und ohne Notarisierung. Alle Build-Befehle werden im
Projektverzeichnis ausgeführt, sofern nicht anders angegeben.

## 1. Der Ablauf bei jedem Update

1. Änderungen fertigstellen und testen. Einen identischen Quellstand für alle
   drei Betriebssysteme bereitstellen.
2. Genau eine neue interne Build-ID festlegen und für alle drei Builds verwenden.
3. Die drei Installer mit den unten beschriebenen Skripten bauen. Jedes Skript
   erzeugt auch eine passende `dist/update.json`.
4. Prüfen, dass die drei erzeugten Manifestdateien dieselbe `buildId` und
   dieselbe `buildTime` enthalten.
   Installer auf den Zielsystemen testen, einschließlich des Update-Checks.
5. Die bisherigen vier Release-Dateien als zusammengehöriges Backup aufbewahren.
6. Auf der GitHub-Release-Seite zuerst die drei Installer ersetzen und ihre
   Downloads prüfen. Erst danach die zugehörige `update.json` ersetzen.
7. Die öffentliche Manifest-URL abrufen: Eine alte App muss ein Update anbieten,
   eine frisch installierte neue App darf keines anbieten.

Die zentrale Regel lautet: **Eine Veröffentlichung = ein Quellstand, eine
gemeinsame Build-ID und eine dazu passende `update.json`.** Niemals nur die
JSON-Datei auf eine neue ID ändern und die alten Installer stehen lassen.

## 2. Versionsnummer, Minecraft-Version und Build-ID

Diese drei Angaben erfüllen unterschiedliche Aufgaben:

| Angabe | Beispiel | Bedeutung für den Updater |
| --- | --- | --- |
| Seed-Atlas-App-/Paketversion | `4.3.dev0` | Steht nur in `VERSION.txt`. Wird im Update-Dialog angezeigt, aber nicht verglichen. |
| Unterstützte Minecraft-Version | `26.3` | Auswahl für die Weltgenerierung, keine Update-Kennung. |
| Interne Build-ID | 40 hexadezimale Zeichen | Wird in die App eingebaut und mit GitHub verglichen. |
| Build-Zeit | Commit-Zeit in Sekunden seit 1970 | Ordnet die Builds: Ein Update wird nur angeboten, wenn der veröffentlichte Build neuer ist. |

Die App-Version hat genau eine Quelle: die Datei `VERSION.txt` im
Projektverzeichnis (z. B. `4.3.0` oder `4.3.dev0`). `seed-atlas.pro`, die drei
Packaging-Skripte und die Release-Workflows lesen sie von dort. Beim Wechsel der
Version außerdem in `etc/org.seedatlas.SeedAtlas.metainfo.xml` einen neuen
obersten `<release>`-Eintrag anlegen; der CI-Job `packaging-metadata` prüft, dass
beide übereinstimmen.

Du kannst die Installer weiterhin unter gleichbleibenden Namen hochladen und
die Paketversion beibehalten. Auch ein Release-Tag muss für den Vergleich nicht
hochgezählt werden. Die interne Build-ID muss sich bei einem neuen App-Stand
aber ändern. Der optionale Versionsparameter der Packaging-Skripte ersetzt diese
ID nicht; ohne ihn verwenden die Skripte `VERSION.txt`.

Die Build-Zeit ermitteln die Skripte aus der Commit-Zeit der Build-ID
(`git show -s --format=%ct`). Bei Möglichkeit B (eigene Kennung) zusätzlich
`SEED_ATLAS_BUILD_TIME` setzen, z. B. `date +%s` beim Festlegen der Kennung,
und auf allen drei Rechnern denselben Wert verwenden.

### Möglichkeit A: Git-Commit als Build-ID

Standardmäßig verwenden alle Packaging-Skripte den vollständigen Git-Commit:

```sh
git status --short
git rev-parse --verify HEAD
```

Vor dem Build die gewünschten Änderungen committen und auf allen Build-Rechnern
exakt diesen Commit verwenden. Dafür sind weder ein neuer öffentlicher
Versionsname noch ein neuer Release-Tag nötig. Ohne explizite Überschreibung
übernehmen die Skripte die ID automatisch.

Wichtig: `git rev-parse HEAD` berücksichtigt keine uncommitteten Änderungen.
Zwei unterschiedliche Arbeitsstände auf demselben Commit bekommen ansonsten
dieselbe ID; das Update würde nicht erkannt. Auch zusätzliche ungetrackte
Quelldateien werden durch diese ID-Ermittlung nicht berücksichtigt.

Eine früher gesetzte Überschreibung hat Vorrang vor Git. Zum bewussten Wechsel
zur automatischen Commit-ID im jeweiligen Terminal entfernen:

```sh
unset SEED_ATLAS_BUILD_ID
```

```powershell
Remove-Item Env:SEED_ATLAS_BUILD_ID -ErrorAction SilentlyContinue
```

### Möglichkeit B: Eigene Kennung ohne neuen Commit

Wenn du einen uncommitteten Quellstand oder ein Quellarchiv ohne `.git` baust,
vergib einmal eine eigene ID. Zulässig sind genau 40 Zeichen aus `0–9` und
`a–f`; verwende durchgängig Kleinbuchstaben. Zum Beispiel einmalig mit Python:

```sh
python3 -c "import secrets; print(secrets.token_hex(20))"
```

Die Ausgabe zusammen mit dem Quellstand notieren. Diese Kennung ist eine
Release-Kennung, nicht zwingend ein Hash des Inhalts. Nicht auf jedem Rechner
neu generieren! Auf allen drei Rechnern dieselbe Ausgabe einsetzen:

```sh
# macOS / Linux: Platzhalter durch die einmal gewählte Kennung ersetzen.
export SEED_ATLAS_BUILD_ID="<GEMEINSAME-40-STELLIGE-BUILD-ID>"
```

```powershell
# Windows PowerShell: exakt dieselbe Kennung verwenden.
$env:SEED_ATLAS_BUILD_ID = "<GEMEINSAME-40-STELLIGE-BUILD-ID>"
```

Die Variable gilt nur für das jeweilige Terminal und dessen Kindprozesse.
Sie muss deshalb auch in einer Linux-VM oder einem neu geöffneten Terminal
gesetzt werden. Nach weiteren App-Änderungen für die nächste Veröffentlichung
eine neue Kennung vergeben und wieder alle drei Pakete damit bauen.

## 3. Quellstand und Build-Voraussetzungen

Seed Atlas ist eine Qt-Widgets-Anwendung mit eingebundener C-Engine. Benötigt
werden GCC/MinGW oder Clang mit GNU-Erweiterungen; die Engine benötigt unter
anderem die Compileroption `-fwrapv`. Die Projektdatei setzt diese beim App-Build.
Windows und macOS verwenden in diesen Beispielen Qt 6.8.3, Flatpak das KDE/Qt-
Runtime-/SDK-Paar 6.10 aus dem Manifest.

Verwende einen vollständigen Checkout oder das mitgelieferte Quellarchiv:

```sh
unzip Seed-Atlas-<version>-Source.zip
cd Seed-Atlas-<version>-Source
```

Beim Quellarchiv fehlt die Git-Historie; daher vor dem Packaging eine explizite
`SEED_ATLAS_BUILD_ID` und `SEED_ATLAS_BUILD_TIME` nach Abschnitt 2 setzen.

Für jedes Betriebssystem eine eigene, frische Quellkopie verwenden. Insbesondere
keinen bereits kompilierten Quellordner zwischen Windows, macOS und Linux teilen:
Die Engine legt ihre `.o`-Dateien und ihr Archiv direkt in `seedatlas-engine/`
ab. Ein separates GUI-Buildverzeichnis allein isoliert diese Dateien nicht.
Bei geänderten Headern, Compileroptionen oder Architekturen können alte
Engine-Objekte sonst wiederverwendet werden.

Die Packaging-Skripte schreiben nach `dist/` und können dort gleichnamige
Ergebnisse ersetzen. Vorherige Releases separat sichern. Niemals mehrere
Packaging-Läufe gleichzeitig in derselben Quellkopie starten; auch
`dist/update.json` würde dabei überschrieben.

### Regressionstests vor dem Packaging

Die Engine-Tests benötigen kein Qt. In einer separaten Test-Quellkopie auf
macOS oder Linux ausführen:

```sh
make -C seedatlas-engine clean
make -C seedatlas-engine test-versions CFLAGS="-O3 -DSTRUCT_CONFIG_OVERRIDE=1"
./seedatlas-engine/test-versions
make -C seedatlas-engine clean
```

Der Build-Befehl allein führt die Tests nicht aus; dafür ist die dritte Zeile
erforderlich. Nur bei erfolgreichem Testlauf weiterveröffentlichen. Das letzte
`clean` entfernt erzeugte Engine-Builddateien, keine Quelldateien, damit der
anschließende App-Build seine eigenen Compiler-/Architekturflags verwenden kann.
Die Engine-Makefile setzt `-fwrapv` selbst; die App-Builds erhalten dort immer
`-O3`. Auf Windows die Tests mit dem passenden MinGW-Compiler und
`mingw32-make` bauen und die erzeugte `test-versions.exe` ausführen.

Die App selbst prüft das Skript `.github/workflows/app-tests.sh` (wie in der
CI): bekannte Seeds für mehrere Beispielfilter, Sitzungs-Roundtrip und eine
fehlerhafte Sitzung bei geschlossener Standardeingabe. Aufruf mit dem gebauten
Executable, z. B. auf macOS:

```sh
bash .github/workflows/app-tests.sh "$PWD/build/seed-atlas.app/Contents/MacOS/seed-atlas"
```

Zusätzlich die fertig installierte GUI auf den Zielbetriebssystemen starten:
Versionsauswahl, Karte, Struktur-Icons, gespeicherte Sitzung und Update-Dialog
prüfen. Ein erfolgreicher Compilerlauf oder ein Test unter Wine ersetzt keinen
vollständigen Test auf Windows.

Den leeren Erststart (kein Seed, keine Versionsauswahl, keine Bedingungen,
ausgeblendeter Such-Tab) prüft die App auch automatisiert. Der Test läuft nur
per Umgebungsvariablen, beendet sich selbst und liefert Exit-Code 0 nur bei
bestandenem Check:

```sh
first_run_dir="$(mktemp -d /tmp/seed-atlas-first-run.XXXXXX)"
mkdir -p "$first_run_dir/settings"
SEED_ATLAS_TEST_SETTINGS_DIR="$first_run_dir/settings" \
SEED_ATLAS_UPDATE_URL="file://$first_run_dir/missing-update.json" \
SEED_ATLAS_UI_TEST_EMPTY_FIRST_RUN=1 \
SEED_ATLAS_UI_SNAPSHOT="$first_run_dir/firstrun.png" \
  "dist/seed-atlas.app/Contents/MacOS/seed-atlas"
echo "Exit-Code: $?"
```

Mit `SEED_ATLAS_TEST_SETTINGS_DIR` liegen auch automatische Sitzung, deren
Sicherung (`session.save.bak`) und die Lua-Skripte in diesem Verzeichnis; die
Sitzung der normalen Installation bleibt unberührt. Vorher die Testeinstellungen
entfernen, damit wirklich ein Erststart simuliert wird (macOS:
`defaults delete com.seed-atlas-uitest.seed-atlas-uitest`). Der
gleiche Aufruf eignet sich auch auf Windows und Linux mit dem jeweils
installierten Executable.

## 4. Windows: unsigned `.exe`

Benötigt werden Windows x64, Git, Qt 6.8.3 mit MinGW 13.1 x64 einschließlich
Qt SVG sowie Inno Setup 6. Die folgenden Pfade entsprechen den Standardwerten
des Skripts; bei abweichender Installation anpassen.

In PowerShell, nach Festlegung der Build-ID:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File installer/windows/build-installer.ps1 `
  -QtBin "C:\Qt\6.8.3\mingw_64\bin" `
  -MingwBin "C:\Qt\Tools\mingw1310_64\bin" `
  -InnoCompiler "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
```

Das Skript liest die Version aus `VERSION.txt` (ein `-Version` überschreibt
sie), kompiliert die App, bündelt Qt-/MinGW-Laufzeitdateien mit `windeployqt`,
legt die Lizenztexte in `Licenses` ab, erstellt das Quellarchiv (inklusive
`seedatlas-sax/` und `tools/`) und baut den Inno-Setup-Installer.
Eine Authenticode-Signatur wird nicht erzeugt.

Ergebnisse:

```text
dist/Seed-Atlas-<version>-Windows-x64-Setup.exe
dist/Seed-Atlas-<version>-Windows-x64-Portable/
dist/Seed-Atlas-<version>-Source.zip
dist/update.json
```

Für den regulären Download die Setup-EXE verwenden. Beim normalen Installer
bleiben App-ID und Installationsziel stabil; ein neuer Installer aktualisiert
eine bestehende Installation an Ort und Stelle.

## 5. macOS: universelle unsigned `.dmg`

Auf macOS mit installierten Xcode Command Line Tools und Python 3 bauen. Für
Intel und Apple Silicon wird das universelle Qt-SDK benötigt; eine beliebige
Homebrew-Qt-Installation ist dafür kein Ersatz. Die DMG-Erstellung konfiguriert
Finder über AppleScript und benötigt eine passende angemeldete Desktop-Sitzung.

Einmalige Werkzeuginstallation im Projektverzeichnis:

```sh
xcode-select --install
python3 -m venv build-tools/aqt-venv
build-tools/aqt-venv/bin/python -m pip install aqtinstall==3.3.0
build-tools/aqt-venv/bin/python -m aqt install-qt mac desktop 6.8.3 clang_64 \
  --archives qtbase qtsvg qttools --outputdir "$PWD/build-tools/Qt"
```

Wichtig: Der Quellordner darf keine Leerzeichen im Pfad enthalten. Das Qt-SDK
aus `build-tools/Qt` scheitert sonst beim Linken (Makefile-Einträge mit
gequoteten SDK-Pfaden werden nicht aufgelöst). Bei Bedarf vor dem Build eine
Kopie unter einem Pfad ohne Leerzeichen anlegen.

Falls die Command Line Tools schon installiert sind, deren Installationsschritt
überspringen. Danach, mit der gemeinsamen Build-ID aus Abschnitt 2:

```sh
export QT_PREFIX="$PWD/build-tools/Qt/6.8.3/macos"
export SEED_ATLAS_UNIVERSAL=1
export SEED_ATLAS_UNSIGNED=1
unset SEED_ATLAS_BUILD_ONLY SEED_ATLAS_PACKAGE_ONLY
bash installer/macos/build-dmg.sh

SEED_ATLAS_UNSIGNED=1 SEED_ATLAS_REQUIRE_UNIVERSAL=1 \
  bash installer/macos/verify-dmg.sh \
  dist/Seed-Atlas-<version>-macOS-UNSIGNED.dmg
```

Ergebnisse:

```text
dist/Seed-Atlas-<version>-macOS-UNSIGNED.dmg
dist/update.json
```

`SEED_ATLAS_UNSIGNED=1` unterbindet Developer-ID-Signierung, Notarisierung und
eine DMG-Signatur auch dann, wenn Signier-Zugangsdaten im Terminal gesetzt sind.
Die App im DMG behält technisch notwendige Ad-hoc-Code-Signaturen für Apple
Silicon. Das ist keine Publisher-Signatur und keine Apple-Freigabe. Die Datei
nicht als signiert oder notarisiert bewerben.

Das Prüfskript kontrolliert unter anderem beide Architekturen, die Integrität
des App-Bundles, die Lizenztexte in `Contents/Resources/Licenses` und das
Finder-Layout mit Hintergrund und Applications-Link. Zum Installationstest
`Seed Atlas.app` aus dem DMG nach Applications ziehen und die installierte
Kopie starten.

`SEED_ATLAS_BUILD_ONLY=1` kompiliert nur und erzeugt noch kein vollständiges
Release-Paket mit Manifest. `SEED_ATLAS_PACKAGE_ONLY=1` verpackt ein vorhandenes
App-Bundle, ohne neu zu kompilieren. Dabei darf die Build-ID nicht nachträglich
geändert werden: Sonst kann ein neues Manifest zu einer alten App entstehen.
Für normale Veröffentlichungen den oben gezeigten vollständigen Lauf verwenden.

## 6. Linux: unsigned `.flatpak`

Auf Linux oder in einer Linux-VM bauen, für die bisherige Download-Ausgabe auf
einem x86_64-System. Auf Debian/Ubuntu die Werkzeuge installieren:

```sh
sudo apt update
sudo apt install flatpak flatpak-builder
```

Den identischen Quellstand in ein eigenes Verzeichnis innerhalb des Linux-
Dateisystems kopieren und dort dieselbe Build-ID setzen. Anschließend:

```sh
bash installer/flatpak/build-flatpak.sh
flatpak install --user ./dist/Seed-Atlas-<version>-Linux-x86_64.flatpak
flatpak run org.seedatlas.SeedAtlas
```

Das Skript richtet Flathub für den aktuellen Benutzer ein und installiert
`org.kde.Platform//6.10` sowie `org.kde.Sdk//6.10`. Dafür werden Internetzugang
und ausreichend Platz für SDK, Runtime und Build benötigt. Es erzeugt ohne
GPG-Signatur:

```text
dist/Seed-Atlas-<version>-Linux-x86_64.flatpak
dist/update.json
```

Auf ARM64 lautet die Ausgabe stattdessen `...-Linux-aarch64.flatpak`; das wäre
ein eigener Architektur-Download, kein Ersatz für ein x86_64-Paket. Das Bundle
enthält einen Verweis auf Flathub zum Nachladen der Runtime, nicht die gesamte
Runtime selbst.

Eine neuere Bundle-Datei mit `flatpak install --user` ersetzt die installierte
Version derselben App-ID.

### Hinweis zu GitHub Actions

Der Workflow [Tests](.github/workflows/tests.yml) läuft bei jedem Push und Pull
Request: Engine-Tests auf Linux, macOS und Windows, Mod-ABI-Tests, ein
App-Build auf allen drei Systemen mit `app-tests.sh` und ein Linux-Build mit
AddressSanitizer/UBSan. [Unsigned release](.github/workflows/unsigned-release.yml)
baut alle drei Pakete (manuell oder bei Push auf einen `release-*`-Branch).
[Native packages](.github/workflows/native-packages.yml) baut Linux und macOS,
nicht Windows; sein macOS-Job verlangt Signier- und Notarisierungs-Secrets. Für den hier beschriebenen Ablauf die lokalen Packaging-Skripte verwenden;
Details zur optionalen signierten Variante stehen in
[installer/PACKAGING.md](installer/PACKAGING.md).

## 7. Wie `update.json` genau funktioniert

### Dateiformat und Erzeugung

Die Packaging-Skripte schreiben ein JSON-Objekt dieser Form:

```json
{
  "buildId": "ff6308982ac37052c97f9fb19349f85a992f2361",
  "buildTime": 1762851234,
  "version": "4.3.0"
}
```

- `buildId` (Pflicht): genau 40 hexadezimale Zeichen. Builds vor 4.3 lesen nur
  dieses Feld und vergleichen es auf Gleichheit; die zusätzlichen Felder stören
  sie nicht.
- `buildTime` (ab 4.3): Commit-Zeit des Builds in Sekunden seit 1970. Damit
  erkennt die App, ob der veröffentlichte Build neuer ist als sie selbst.
- `version`: wird im Update-Dialog angezeigt, aber nicht verglichen.

Normalerweise nicht von Hand erstellen. Jedes der drei Packaging-Skripte
verwendet dieselbe ID und Build-Zeit beim Kompilieren und für seine
`dist/update.json`. Deshalb genügt für GitHub eine der drei Dateien, aber nur,
wenn alle drei übereinstimmen. Die Datei entsteht erst am Ende eines
erfolgreichen Packaging-Laufs.

Das Manifest enthält keine Installer-URLs, Prüfsummen oder Signaturen. Die App
lädt nichts herunter und führt nichts aus; sie öffnet nur die Release-Seite.

### Wo die App nachschaut

Die fest eingebauten Adressen sind:

- Manifest: [update.json des Latest Release](https://github.com/DUzzL/Seed-Atlas/releases/latest/download/update.json)
- Ziel des Update-Buttons: [Latest Release von Seed Atlas](https://github.com/DUzzL/Seed-Atlas/releases/latest)

`update.json` muss als eigenständiges Release-Asset hochgeladen werden. Eine
Datei im Git-Repository, in einem ZIP, neben der lokal installierten App oder
nur in einem Actions-Artefakt reicht nicht aus. GitHubs `latest/download/NAME`
adressiert das Asset mit diesem Namen im neuesten Release.
Siehe [GitHub: Linking to releases](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases).

Die App greift ohne GitHub-Anmeldung zu. Release und Downloads müssen deshalb
öffentlich erreichbar sein. Bei einem späteren Repository-Umzug müssen die
Adressen im Updater berücksichtigt werden; ein Feld in `update.json` kann den
fest eingebauten Link des Update-Buttons nicht ändern.

### Der Vergleich

Bei aktivierter Einstellung prüft die GUI einmal nach jedem Start asynchron
das Manifest. Manuell: `Menu → Help → Check for updates` (das Menü-Symbol der
Werkzeugleiste enthält die früheren Menüs).

| Eingebauter Build | Manifest auf GitHub | Ergebnis |
| --- | --- | --- |
| ID A | ID A | Kein Popup („up to date“ bei manueller Prüfung). |
| ID A, Zeit t | ID B, Zeit > t | Update-Popup („newer build“). |
| ID A, Zeit t | ID B, Zeit ≤ t | Kein Popup; manuell: „newer than the published release“. |
| ID A | ID B ohne `buildTime` (Manifest vor 4.3) | Update-Popup („different build“), da keine Reihenfolge bekannt ist. |
| Beliebig | Fehlende/ungültige Datei oder Netzwerkfehler | Beim Start still; manuell eine Fehlermeldung. |
| `development` | Beliebig | Kein Update-Check für diesen Entwicklungsbuild. |

Die IDs werden ohne Beachtung der Groß-/Kleinschreibung verglichen; die Skripte
schreiben sie klein. Ein Build ohne eingebaute Build-Zeit (älteres Packaging)
kann wie ein Manifest ohne `buildTime` nur auf Ungleichheit prüfen.

### Popup, Installation und Daten

- `Download`: Öffnet die GitHub-Release-Seite im Browser. Die installierte App
  bleibt unverändert und geöffnet. Installiert wird der neue Build vom Nutzer:
  Der Windows-Installer aktualisiert die bestehende Installation (gleiche
  Inno-`AppId`), auf macOS ersetzt das Ziehen aus dem DMG die alte App, ein
  neues Flatpak-Bundle ersetzt die installierte Version.
- `Not now`: Beim nächsten Start wird erneut gefragt.
- `Do not check for updates at start`: Deaktiviert zukünftige Startchecks
  insgesamt. Wieder aktivieren: Einstellungen, `General → Miscellaneous →
  Check for Seed Atlas updates at startup`.

Frühere Ausgaben (vor 4.3) entfernten sich bei `Update` selbst. Das macht die
App nicht mehr; es gibt keinen Selbstentfernungs- oder Dry-Run-Modus mehr.

Einstellungen und Sitzungsdaten liegen außerhalb der Programminstallation und
bleiben bei einer Neuinstallation erhalten. Für zukünftige Installer die
bestehenden Kennungen beibehalten: die `seed-atlas`-Settings-Kennung, die
Windows-Inno-`AppId` und die Flatpak-ID `org.seedatlas.SeedAtlas`.

## 8. Die vier Dateien manuell auf GitHub austauschen

Für die bisher verwendeten Download-Namen kann der fertige Upload-Satz so aussehen:

```text
Seed-Atlas-Windows.exe
Seed-Atlas-MacOS.dmg
Seed-Atlas-Linux.flatpak
update.json
```

Die automatisch erzeugten Installer darfst du dafür kopieren und umbenennen.
Das ändert ihre eingebaute Build-ID nicht. Nur `update.json` muss wegen der festen
Download-URL genau diesen Dateinamen behalten. Keine unterschiedlichen
plattformbezogenen Manifestdateien hochladen.

Vorgehen im Browser:

1. `DUzzL/Seed-Atlas → Releases` öffnen und das für Nutzer bestimmte Latest
   Release bearbeiten. Prüfen, dass `/releases/latest` wirklich dort landet.
2. Die bisherigen Installer und ihre passende JSON-Datei lokal sichern.
3. Die drei alten Installer-Assets durch die drei fertig getesteten neuen
   Dateien ersetzen. Änderungen speichern und alle Downloads prüfen.
4. Erst jetzt das alte `update.json`-Asset durch die neue, zu diesen Installern
   gehörende Datei ersetzen und die Änderungen speichern.
5. Den direkten Manifest-Link prüfen und anschließend den Check mit einer alten
   und einer neuen App durchführen.

GitHub beschreibt das Bearbeiten von Releases und Assets unter
[Managing releases](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository?tool=webui).
Das öffentliche Ziel muss ein veröffentlichtes reguläres Release sein, kein
Draft und kein als Pre-release markiertes Release: Diese können nicht als
Latest gesetzt werden. Das ist unabhängig davon, ob eine unterstützte
Minecraft-Version ursprünglich ein Pre-Release war.
Siehe [GitHub: Release-API, `make_latest`](https://docs.github.com/en/rest/releases/releases#create-a-release).

Bei aktivierter Release-Immutability lassen sich Assets eines veröffentlichten
Releases nicht mehr austauschen. Dann ist ein neues Release mit komplettem
Dateisatz nötig; das Überschreiben desselben Releases ist damit nicht vereinbar.
Siehe [GitHub: Immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases).

Warum JSON zuletzt? Sobald die neue ID online steht, bietet jede ältere
Installation ein Update an. Dann müssen bereits alle neuen Installer verfügbar
sein. Der Austausch in einem bestehenden Release ist trotzdem nicht atomar:
Eine schon während des Austauschs heruntergeladene neue App kann vorübergehend
noch die alte Manifest-ID sehen. Das Zeitfenster klein halten. Ein neu
veröffentlichtes Release mit vorab vollständig hochgeladenen Assets vermeidet
den schrittweisen Austausch des bisherigen Dateisatzes.

Die öffentliche Datei auf macOS/Linux ohne Anmeldung prüfen:

```sh
curl --fail --location --header "Cache-Control: no-cache" \
  https://github.com/DUzzL/Seed-Atlas/releases/latest/download/update.json
```

In Windows PowerShell explizit `curl.exe` verwenden, damit nicht ein anders
arbeitender PowerShell-Alias ausgeführt wird. Die ausgegebenen Werte `buildId`
und `buildTime` müssen mit der lokalen `dist/update.json` und allen drei Builds
übereinstimmen.

Die drei lokal erzeugten JSON-Dateien vor dem Zusammenführen getrennt aufbewahren
und ihren Inhalt vergleichen. Zur zusätzlichen Syntaxprüfung einer Datei:

```sh
python3 -m json.tool dist/update.json
```

Die Syntaxprüfung allein garantiert noch keine gültige 40-stellige ID und keine
Übereinstimmung mit dem App-Binary. `seed-atlas --version` zeigt die App-Version,
nicht diese Build-ID. Deshalb die gewählte Kennung im Build-Protokoll festhalten
und die tatsächlich installierten Pakete gegen das Manifest testen.

Auch den passenden Quellstand archivieren. Wenn ein alter Release-Tag bestehen
bleibt, bildet GitHubs automatisch erzeugtes „Source code“-Archiv nicht plötzlich
den neuen lokalen Arbeitsstand ab. Das Windows-Packaging erzeugt dafür zusätzlich
ein eigenes `Seed-Atlas-<version>-Source.zip`; es gehört zu jedem Release.

Bei einem Rollback immer Installer und ihre ursprüngliche JSON-Datei gemeinsam
zurücksetzen. Nutzer des zurückgezogenen (neueren) Builds bekommen dann kein
Update angeboten, weil der veröffentlichte Build älter ist; sie müssen den
älteren Installer bei Bedarf selbst installieren.

## 9. Den Update-Dialog testen

Der Updater besitzt folgende Test-Umgebungsvariablen. Diese nicht mit den
Build-Variablen aus Abschnitt 2 verwechseln:

| Variable | Wirkung beim Start der App |
| --- | --- |
| `SEED_ATLAS_UPDATE_URL` | Verwendet ein anderes Manifest, auch eine lokale `file://`-URL. Der Download-Button öffnet weiterhin die echte GitHub-Release-Seite. |
| `SEED_ATLAS_UPDATE_BUILD_ID` | Überschreibt nur zum Test die lokale Vergleichs-ID. |
| `SEED_ATLAS_UPDATE_BUILD_TIME` | Überschreibt nur zum Test die lokale Build-Zeit (Sekunden seit 1970). |
| `SEED_ATLAS_TEST_SETTINGS_DIR` | Testmodus: Einstellungen unter der Kennung `seed-atlas-uitest`; automatische Sitzung, deren Sicherung und Lua-Skripte in diesem Verzeichnis. Die normale Installation bleibt unverändert. |
| `SEED_ATLAS_TEST_SETTINGS_ID` | Verwendet genau diese Settings-Kennung. Ohne `SEED_ATLAS_TEST_SETTINGS_DIR` liegen Sitzung und Lua-Skripte in einem eigenen Ordner neben dem normalen Konfigurationsordner, benannt nach der Kennung. |

Wichtig: Ohne `SEED_ATLAS_TEST_SETTINGS_DIR` bzw. `SEED_ATLAS_TEST_SETTINGS_ID`
schreibt der Test in die echten App-Einstellungen.

Beispiel auf macOS mit bereits installierter App und lokalem Release-Manifest:

```sh
update_test_dir="$(mktemp -d /tmp/seed-atlas-update-test.XXXXXX)"
cp dist/update.json "$update_test_dir/update.json"

SEED_ATLAS_UPDATE_BUILD_ID=0000000000000000000000000000000000000000 \
SEED_ATLAS_UPDATE_BUILD_TIME=1 \
SEED_ATLAS_UPDATE_URL="file://$update_test_dir/update.json" \
SEED_ATLAS_TEST_SETTINGS_DIR="$update_test_dir/settings" \
  "/Applications/Seed Atlas.app/Contents/MacOS/seed-atlas"
```

Prüffälle:

1. Andere ID, ältere lokale Zeit (wie oben): Popup „newer build“. `Not now`
   lässt die App offen; `Download` öffnet GitHub, die App bleibt installiert.
2. Andere ID, `SEED_ATLAS_UPDATE_BUILD_TIME` größer als `buildTime` des
   Manifests: kein Start-Popup; `Check for updates` meldet „newer than the
   published release“.
3. `Do not check for updates at start` aktivieren und denselben Aufruf mit
   demselben Testordner wiederholen: kein Start-Popup.
4. Die Override-Zeilen weglassen: Das neue Paket darf mit seinem eigenen
   Manifest kein Update-Popup anzeigen.

Bei Flatpak müssen Testvariablen mit
`flatpak run --env=NAME=WERT ... org.seedatlas.SeedAtlas` explizit in die Sandbox
übergeben werden; lokale Manifest- und Einstellungspfade müssen darin
zugänglich sein.

## 10. Häufige Fehler

| Problem | Prüfen / beheben |
| --- | --- |
| Kein Popup trotz neuer App-Dateien | Wurde wirklich eine neue Build-ID verwendet? Uncommittete Änderungen verändern `HEAD` nicht. |
| Kein Popup, obwohl ein neuer Build veröffentlicht ist | `buildTime` im Manifest prüfen: Sie muss größer sein als die Build-Zeit der installierten App. Manuell über `Menu → Help → Check for updates` prüfen. |
| Gar keine Update-Prüfung | „Do not ask again“ bzw. Startcheck-Einstellung prüfen. Ausgaben ohne Updater oder mit ID `development` zunächst manuell ersetzen. |
| Manifest nicht erreichbar / kein Popup trotz verschiedener IDs | Öffentliche URL mit `curl` prüfen: Internetzugang, Latest-Ziel, Assetname und JSON-Format kontrollieren. Beim Start werden Netzwerk-/Manifestfehler absichtlich nicht angezeigt. |
| Nach jedem Neuinstallieren wieder ein Update | Manifest-ID und alle drei eingebauten IDs prüfen; keine alte EXE/DMG/Flatpak oder JSON aus einem fehlgeschlagenen Build hochladen. Test-Overrides entfernen. |
| Nur eine Plattform meldet ständig ein Update | Wahrscheinlich unterschiedliche Build-IDs oder ein vergessenes altes Plattformpaket. Alle drei Builds mit derselben ID veröffentlichen. |
| Lokaler neuer Build meldet ein Update auf den alten Download | Nur solange das Manifest auf GitHub noch kein `buildTime` enthält (vor 4.3 erzeugt). Ein direkter `qmake`-Build ohne ID ist ein `development`-Build und prüft nicht auf Updates. |
| Beim Umstieg auf universal/anderen Compiler Linker- oder Architekturfehler | Frische Quellkopie verwenden bzw. generierte Engine-Objekte vor dem neuen Build bereinigen. |

Die maßgeblichen Implementierungen sind [src/updater.cpp](src/updater.cpp),
[seed-atlas.pro](seed-atlas.pro) und die Packaging-Skripte für
[Windows](installer/windows/build-installer.ps1),
[macOS](installer/macos/build-dmg.sh) und
[Flatpak](installer/flatpak/build-flatpak.sh). Wenn deren Verhalten geändert wird,
diese Anleitung entsprechend mitpflegen.
