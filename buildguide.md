# Seed Atlas: Build- und Update-Anleitung

Diese Anleitung beschreibt den manuellen Release-Ablauf: Windows-Installer,
macOS-DMG und Linux-Flatpak bauen, auf GitHub austauschen und mit `update.json`
die Update-Meldung in bereits installierten Apps auslösen. Eine fortlaufende
öffentliche Versionsnummer ist dafür nicht erforderlich.

Die Beispiele entsprechen dem Stand vom 10. September 2026. Sie bauen ohne
Publisher-Signaturen und ohne Notarisierung. Alle Build-Befehle werden im
Projektverzeichnis ausgeführt, sofern nicht anders angegeben.

## 1. Der Ablauf bei jedem Update

1. Änderungen fertigstellen und testen. Einen identischen Quellstand für alle
   drei Betriebssysteme bereitstellen.
2. Genau eine neue interne Build-ID festlegen und für alle drei Builds verwenden.
3. Die drei Installer mit den unten beschriebenen Skripten bauen. Jedes Skript
   erzeugt auch eine passende `dist/update.json`.
4. Prüfen, dass die drei erzeugten Manifestdateien dieselbe `buildId` enthalten.
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
| Seed-Atlas-App-/Paketversion | `4.2.dev0` | Nicht für den Update-Vergleich verwendet. |
| Unterstützte Minecraft-Version | `26.3` | Auswahl für die Weltgenerierung, keine Update-Kennung. |
| Interne Build-ID | 40 hexadezimale Zeichen | Wird in die App eingebaut und mit GitHub verglichen. |

Du kannst die Installer weiterhin unter gleichbleibenden Namen hochladen und
die Paketversion beibehalten. Auch ein Release-Tag muss für den Vergleich nicht
hochgezählt werden. Die interne Build-ID muss sich bei einem neuen App-Stand
aber ändern. Der Versionsparameter der Packaging-Skripte ersetzt diese ID nicht
und ändert nicht automatisch die im Quellcode hinterlegte App-Versionsanzeige.

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
unzip Seed-Atlas-4.2.dev0-Source.zip
cd Seed-Atlas-4.2.dev0-Source
```

Beim Quellarchiv fehlt die Git-Historie; daher vor dem Packaging eine explizite
`SEED_ATLAS_BUILD_ID` nach Abschnitt 2 setzen.

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
make -C seedatlas-engine test-versions CFLAGS="-O2 -fwrapv -DSTRUCT_CONFIG_OVERRIDE=1"
./seedatlas-engine/test-versions
make -C seedatlas-engine clean
```

Der Build-Befehl allein führt die Tests nicht aus; dafür ist die dritte Zeile
erforderlich. Nur bei erfolgreichem Testlauf weiterveröffentlichen. Das letzte
`clean` entfernt erzeugte Engine-Builddateien, keine Quelldateien, damit der
anschließende App-Build seine eigenen Compiler-/Architekturflags verwenden kann.
Auf Windows die Tests mit dem passenden MinGW-Compiler und `mingw32-make`
bauen und die erzeugte `test-versions.exe` ausführen.

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
  "dist/seed-atlas.app/Contents/MacOS/seed-atlas" \
  --session="$first_run_dir/session.save"
echo "Exit-Code: $?"
```

Vorher die Testeinstellungen entfernen, damit wirklich ein Erststart simuliert
wird (macOS: `defaults delete com.seed-atlas-uitest.seed-atlas-uitest`). Der
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
  -Version 4.2.dev0 `
  -QtBin "C:\Qt\6.8.3\mingw_64\bin" `
  -MingwBin "C:\Qt\Tools\mingw1310_64\bin" `
  -InnoCompiler "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
```

Das Skript kompiliert die App, bündelt Qt-/MinGW-Laufzeitdateien mit
`windeployqt`, erstellt das Quellarchiv und baut den Inno-Setup-Installer.
Eine Authenticode-Signatur wird nicht erzeugt.

Ergebnisse:

```text
dist/Seed-Atlas-4.2.dev0-Windows-x64-Setup.exe
dist/Seed-Atlas-4.2.dev0-Windows-x64-Portable/
dist/Seed-Atlas-4.2.dev0-Source.zip
dist/update.json
```

Für den regulären Download die Setup-EXE verwenden. Die portable Ausgabe hat
keinen Uninstaller und kann deshalb nicht denselben Selbstentfernungsablauf
ausführen. Beim normalen Installer bleiben App-ID und Installationsziel stabil.

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
bash installer/macos/build-dmg.sh 4.2.dev0

SEED_ATLAS_UNSIGNED=1 SEED_ATLAS_REQUIRE_UNIVERSAL=1 \
  bash installer/macos/verify-dmg.sh \
  dist/Seed-Atlas-4.2.dev0-macOS-UNSIGNED.dmg
```

Ergebnisse:

```text
dist/Seed-Atlas-4.2.dev0-macOS-UNSIGNED.dmg
dist/update.json
```

`SEED_ATLAS_UNSIGNED=1` unterbindet Developer-ID-Signierung, Notarisierung und
eine DMG-Signatur auch dann, wenn Signier-Zugangsdaten im Terminal gesetzt sind.
Die App im DMG behält technisch notwendige Ad-hoc-Code-Signaturen für Apple
Silicon. Das ist keine Publisher-Signatur und keine Apple-Freigabe. Die Datei
nicht als signiert oder notarisiert bewerben.

Das Prüfskript kontrolliert unter anderem beide Architekturen, die Integrität
des App-Bundles und das Finder-Layout mit Hintergrund und Applications-Link.
Zum Installationstest `Seed Atlas.app` aus dem DMG nach Applications ziehen
und die installierte Kopie starten. Nicht direkt aus dem schreibgeschützten
DMG auf Selbstentfernung testen.

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
bash installer/flatpak/build-flatpak.sh 4.2.dev0
flatpak install --user ./dist/Seed-Atlas-4.2.dev0-Linux-x86_64.flatpak
flatpak run org.seedatlas.SeedAtlas
```

Das Skript richtet Flathub für den aktuellen Benutzer ein und installiert
`org.kde.Platform//6.10` sowie `org.kde.Sdk//6.10`. Dafür werden Internetzugang
und ausreichend Platz für SDK, Runtime und Build benötigt. Es erzeugt ohne
GPG-Signatur:

```text
dist/Seed-Atlas-4.2.dev0-Linux-x86_64.flatpak
dist/update.json
```

Auf ARM64 lautet die Ausgabe stattdessen `...-Linux-aarch64.flatpak`; das wäre
ein eigener Architektur-Download, kein Ersatz für ein x86_64-Paket. Das Bundle
enthält einen Verweis auf Flathub zum Nachladen der Runtime, nicht die gesamte
Runtime selbst.

Für den derzeitigen Update-Ablauf mit `--user` installieren. Die automatische
Entfernung ruft ebenfalls `flatpak uninstall --user` auf und ist nicht für eine
systemweite Installation ausgelegt.

### Hinweis zu GitHub Actions

Der vorhandene Workflow [Native packages](.github/workflows/native-packages.yml)
baut derzeit Linux und macOS, nicht Windows. Sein macOS-Job verlangt Signier-
und Notarisierungs-Secrets. Er ist daher nicht ohne Anpassung ein unsigned
Drei-Plattform-Builder. Er führt derzeit auch nicht die obige Engine-Testfolge
aus. Für den hier beschriebenen Ablauf die lokalen Packaging-Skripte verwenden;
Details zur optionalen signierten Variante stehen in
[installer/PACKAGING.md](installer/PACKAGING.md).

## 7. Wie `update.json` genau funktioniert

### Dateiformat und Erzeugung

Die Datei ist ein normales JSON-Objekt mit genau einer benötigten Eigenschaft:

```json
{
  "buildId": "ff6308982ac37052c97f9fb19349f85a992f2361"
}
```

Das ist die Kennung des lokal erstellten 26.3-Installer-Satzes, kein Platzhalter
für alle zukünftigen Updates. Bei einer neuen Veröffentlichung muss hier deren
eigene Kennung stehen.

`buildId` ist eine Zeichenkette mit genau 40 hexadezimalen Zeichen. Schreibweise
des Feldnamens beachten; kein `buildID`, keine Versionsnummer wie `26.3`, keine
Kommentare und kein zusätzliches Komma nach dem letzten Feld. Die Skripte
schreiben gültiges UTF-8-JSON automatisch. Normalerweise nicht von Hand erstellen.

Jedes der drei Packaging-Skripte verwendet dieselbe ermittelte ID sowohl beim
Kompilieren als auch für seine `dist/update.json`. Deshalb genügt für GitHub
eine der drei Dateien, aber nur, wenn alle drei `buildId`-Werte übereinstimmen.
Die Datei entsteht erst am Ende eines erfolgreichen Packaging-Laufs. Nach einem
abgebrochenen Lauf kann in `dist/` noch eine Datei vom letzten Build liegen.

Das Manifest enthält keine Installer-URLs, Betriebssystemliste, Release-Notizen,
Datumsangabe, Versionsreihenfolge oder Download-Prüfsummen. Solche zusätzlichen
Felder werden vom aktuellen Updater nicht ausgewertet. Die Build-ID ist auch
keine kryptografische Überprüfung des heruntergeladenen Installers.

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

### Der Vergleich ist ausschließlich „gleich oder verschieden“

Bei aktivierter Einstellung prüft die GUI einmal nach jedem Start asynchron
das Manifest. Ein laufendes Programm fragt nicht regelmäßig erneut ab.

Aktuelle UI-Einschränkung: Im Code existiert zusätzlich `Help → Check for
updates`, aber das Hauptfenster leert und versteckt die Menüleiste. Diese Aktion
ist daher in der derzeitigen Oberfläche nicht zugänglich. Zum erneuten Prüfen
Startchecks aktivieren und die App neu starten. Ein sichtbarer manueller
Update-Button wäre eine separate Codeänderung; diese Anleitung setzt ihn nicht
voraus.

| Eingebaute ID | ID auf GitHub | Ergebnis |
| --- | --- | --- |
| A | A | Kein Start-Popup. |
| A | B | Update-Popup. |
| B | A | Ebenfalls Update-Popup, auch wenn B tatsächlich neuer ist. |
| Beliebige ID | Fehlende/ungültige Datei oder Netzwerkfehler | Beim Start still, kein Popup. |
| `development` | Beliebige ID | Kein Update-Check für diesen Entwicklungsbuild. |

A und B stehen hier für unterschiedliche gültige 40-stellige Kennungen.
Die App prüft weder Git-Historie noch Zeitstempel oder semantische Versionen.
Auch Groß-/Kleinschreibung kann beim Zeichenkettenvergleich einen Unterschied
auslösen; deshalb überall exakt dieselbe kleingeschriebene ID verwenden.

Die intern vorhandene manuelle Prüffunktion würde bei gleicher ID „up to date“
und bei Netzwerk-/Manifestproblemen einen Fehler anzeigen. Der derzeit sichtbare
Startcheck zeigt diese Rückmeldungen nicht. Kein Popup allein beweist daher
noch nicht, dass GitHub erfolgreich erreicht wurde; zusätzlich die URL und die
Prüffälle aus Abschnitt 9 kontrollieren.

Wenn nur ein Plattformpaket eine andere ID bekommt, können dessen Nutzer nach
jeder Neuinstallation erneut ein Update angeboten bekommen. Auch bei einer
Änderung nur für ein Betriebssystem deshalb für diesen einfachen gemeinsamen
Update-Kanal alle drei Pakete mit der neuen ID veröffentlichen.

### Popup, Installation und Daten

- `Not now`: App bleibt installiert und geöffnet. Beim nächsten Start wird
  erneut gefragt, solange eine andere ID online steht und Startchecks aktiv sind.
- `Do not ask again`: Deaktiviert zukünftige Startchecks insgesamt, nicht nur
  dieses eine Update. Gilt unabhängig davon, welcher Button angeklickt wird.
- Wieder aktivieren: Einstellungen, `General → Miscellaneous → Check for Seed
  Atlas updates at startup`. Danach neu starten, damit wieder geprüft wird.
- `Update`: Öffnet die GitHub-Seite im Browser, stößt danach die Entfernung
  der installierten Kopie an und beendet die App über den normalen Schließpfad.
  Anschließend lädt der Nutzer den neuen Installer herunter und installiert ihn.

Dies ist kein automatischer Download und kein In-place-Update. Die App wartet
nicht, bis der neue Installer heruntergeladen oder seine Installation gelungen
ist. Bereits das erfolgreiche Übergeben des Links an den Browser führt zur
Selbstentfernung. Schlägt das Öffnen des Browsers fehl, bleibt die App bestehen.

Die Entfernung unterscheidet sich nach Plattform:

| Plattform | Aktuelles Verhalten |
| --- | --- |
| Windows-Setup | Startet den mitinstallierten Inno-Uninstaller leise. Eine portable Kopie ohne `unins???.exe` wird nicht gelöscht; es erscheint ein Hinweis. |
| macOS | Verschiebt das laufende `.app`-Bundle in den Papierkorb. Fehlende Rechte oder ein Start aus dem DMG können das verhindern. |
| Linux-Flatpak | Startet über den Host `flatpak uninstall --user --noninteractive org.seedatlas.SeedAtlas`, ohne `--delete-data`. |
| Linux ohne Flatpak | Versucht, nur die ausführbare Datei in den Papierkorb zu verschieben. |

Ein sofort erkannter Entfernungsfehler wird angezeigt und die App bleibt offen.
Bei extern gestarteten Uninstallern wird allerdings nur der Prozessstart geprüft,
nicht dessen späteres Ergebnis. Im Fehlerfall kann man die alte App manuell
ersetzen; Benutzerdaten dafür nicht löschen.

Einstellungen und regulär gespeicherte Sitzungsdaten liegen außerhalb der
Programminstallation. Die vorgesehenen Entfernungswege löschen diese nicht.
Auch die Einstellung „Do not ask again“ bleibt damit nach einer Neuinstallation
erhalten. Sitzungswiederherstellung und automatische Speicherung richten sich
weiterhin nach den App-Einstellungen; wichtige ungespeicherte Arbeit vorher
selbst speichern und bei Bedarf sichern.

Für zukünftige Installer die bestehenden Kennungen und Datenpfade beibehalten:
insbesondere die `seed-atlas`-Settings-Kennung, die Windows-Inno-`AppId` und die
Flatpak-ID `org.seedatlas.SeedAtlas`. Keine Datenbereinigung zum Update hinzufügen,
kein `flatpak uninstall --delete-data` und kein `--reset-all` verwenden.

Sehr alte Seed-Atlas-Ausgaben ohne eingebauten Updater erhalten durch das bloße
Hochladen der JSON-Datei noch keinen Update-Dialog. Diese Nutzer müssen zunächst
einmal manuell eine Ausgabe mit Updater installieren.

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

Warum JSON zuletzt? Sobald die neue ID online steht, bietet jede abweichende
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
arbeitender PowerShell-Alias ausgeführt wird. Der ausgegebene `buildId`-Wert muss
mit der lokalen `dist/update.json` und allen drei Builds übereinstimmen.

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
ein eigenes `Seed-Atlas-4.2.dev0-Source.zip`.

Bei einem Rollback immer Installer und ihre ursprüngliche JSON-Datei gemeinsam
zurücksetzen. Weil nur auf Ungleichheit geprüft wird, bekommen Nutzer des
zurückgezogenen Builds dann ebenfalls einen Wechsel angeboten.

## 9. Den Update-Dialog testen, ohne die App zu entfernen

Der Updater besitzt folgende Test-Umgebungsvariablen. Diese nicht mit der
Build-Variable aus Abschnitt 2 verwechseln:

| Variable | Wirkung beim Start der App |
| --- | --- |
| `SEED_ATLAS_UPDATE_URL` | Verwendet ein anderes Manifest, auch eine lokale `file://`-URL. Der Update-Button öffnet weiterhin die echte GitHub-Release-Seite. |
| `SEED_ATLAS_UPDATE_BUILD_ID` | Überschreibt nur zum Test die lokale Vergleichs-ID. Ändert weder das Binary noch `update.json`. |
| `SEED_ATLAS_UPDATE_DRY_RUN` | Verhindert Selbstentfernung und anschließendes Beenden durch den Updater. Der Browser wird trotzdem geöffnet. |
| `SEED_ATLAS_TEST_SETTINGS_DIR` | Schaltet den Testmodus ein: Die App legt ihre Einstellungen unter der eigenen Kennung `seed-atlas-uitest` ab und lässt die normalen Einstellungen unverändert. Das Verzeichnis wird zusätzlich für das Ini-Format registriert. |
| `SEED_ATLAS_TEST_SETTINGS_ID` | Verwendet genau diese Kennung statt `seed-atlas` bzw. statt der aus `SEED_ATLAS_TEST_SETTINGS_DIR` abgeleiteten Testkennung. Für Erststarttests vorher die Testeinstellungen entfernen, z. B. auf macOS mit `defaults delete com.seed-atlas-uitest.seed-atlas-uitest`. |

Bei `SEED_ATLAS_UPDATE_DRY_RUN` zählt bereits das Vorhandensein der Variable.
Auch der Wert `0` aktiviert diesen Schutz. Zum Abschalten die Variable vollständig
entfernen. Die Checkbox wird im Dry Run weiterhin gespeichert.
Wichtig: Ohne `SEED_ATLAS_TEST_SETTINGS_DIR` bzw. `SEED_ATLAS_TEST_SETTINGS_ID`
schreibt der Test in die echten App-Einstellungen. Ein Klick auf
`Do not ask again` würde dort den Startcheck dauerhaft abschalten.

Beispiel auf macOS mit bereits installierter App und lokalem Release-Manifest:

```sh
update_test_dir="$(mktemp -d /tmp/seed-atlas-update-test.XXXXXX)"
cp dist/update.json "$update_test_dir/update.json"

SEED_ATLAS_UPDATE_DRY_RUN=1 \
SEED_ATLAS_UPDATE_BUILD_ID=0000000000000000000000000000000000000000 \
SEED_ATLAS_UPDATE_URL="file://$update_test_dir/update.json" \
SEED_ATLAS_TEST_SETTINGS_DIR="$update_test_dir/settings" \
  "/Applications/Seed Atlas.app/Contents/MacOS/seed-atlas" \
  --session="$update_test_dir/session.save"
```

Die hier simulierte Null-ID muss sich von der Manifest-ID unterscheiden. Eine
schon laufende App vor dem Test regulär schließen. Der direkte Executable-Aufruf
stellt sicher, dass dieser neue Prozess die Variablen erhält. Der eigene
`--session`-Pfad hält die Testsitzung getrennt von der normalen Sitzung.

Prüffälle:

1. Unterschiedliche IDs: Popup erscheint. `Not now` lässt die App offen.
2. `Update` im Dry Run: GitHub öffnet sich, die App wird nicht entfernt und
   bleibt geöffnet.
3. `Do not ask again` aktivieren und denselben Test mit demselben Testordner
   wiederholen: kein Start-Popup. Dafür nur den App-Aufruf wiederholen, nicht
   erneut mit `mktemp` einen frischen Einstellungsordner anlegen.
4. Startchecks in den Einstellungen wieder aktivieren. Als simulierte lokale ID
   exakt die Manifest-ID verwenden: kein Start-Popup. Zur Gegenprobe wieder eine
   andere ID einsetzen: Das Popup muss wieder erscheinen.
5. Für die abschließende Kontrolle der wirklich eingebauten ID die Zeile
   `SEED_ATLAS_UPDATE_BUILD_ID=...` weglassen und eine eventuell früher exportierte
   Variable dieses Namens entfernen. Das neue Paket darf mit seinem eigenen
   Manifest kein Update-Popup anzeigen. Den Dry-Run-Schutz dabei beibehalten und
   durch den vorherigen Test mit abweichender ID sicherstellen, dass die
   Manifestdatei erreichbar ist und Startchecks eingeschaltet sind.

Die Variablen in diesem Shell-Beispiel gelten nur für den jeweiligen App-Aufruf.
Wer sie stattdessen per `export` oder `$env:...` setzt, muss sie danach wieder
entfernen. Für den abschließenden öffentlichen Check auch
`SEED_ATLAS_UPDATE_URL` entfernen, damit tatsächlich GitHub geprüft wird.

Auf Windows gelten dieselben Variablen in PowerShell, auf Linux bei einem
nativen App-Aufruf ebenfalls. Bei Flatpak müssen Testvariablen mit
`flatpak run --env=NAME=WERT ... org.seedatlas.SeedAtlas` explizit in die Sandbox
übergeben werden; lokale Manifest-, Einstellungs- und Sitzungspfade müssen darin
zugänglich sein. Eine echte Deinstallation nur an einer separaten Testinstallation
mit zuvor gesicherten Testdaten prüfen, nicht an der Arbeitsinstallation.

## 10. Häufige Fehler

| Problem | Prüfen / beheben |
| --- | --- |
| Kein Popup trotz neuer App-Dateien | Wurde wirklich eine neue Build-ID verwendet? Uncommittete Änderungen verändern `HEAD` nicht. |
| Kein sichtbarer manueller Update-Button | Die aktuelle Menüleiste ist ausgeblendet. Startchecks aktivieren und neu starten; siehe Abschnitt 7. |
| Gar keine Update-Prüfung | „Do not ask again“ bzw. Startcheck-Einstellung prüfen. Ausgaben ohne Updater oder mit ID `development` zunächst manuell ersetzen. |
| Manifest nicht erreichbar / kein Popup trotz verschiedener IDs | Öffentliche URL mit `curl` prüfen: Internetzugang, Latest-Ziel, Assetname und JSON-Format kontrollieren. Beim Start werden Netzwerk-/Manifestfehler absichtlich nicht angezeigt. |
| Nach jedem Neuinstallieren wieder ein Update | Manifest-ID und alle drei eingebauten IDs prüfen; keine alte EXE/DMG/Flatpak oder JSON aus einem fehlgeschlagenen Build hochladen. Test-Overrides entfernen. |
| Nur eine Plattform meldet ständig ein Update | Wahrscheinlich unterschiedliche Build-IDs oder ein vergessenes altes Plattformpaket. Alle drei Builds mit derselben ID veröffentlichen. |
| Lokaler neuer Build meldet ein Update auf den alten Download | Erwartbar, solange GitHub eine andere ID hat. Der Updater kennt keine Reihenfolge. |
| `Update` entfernt die App beim Test nicht | Dry-Run-Variable noch gesetzt, portable Windows-Kopie, schreibgeschütztes DMG oder systemweites Flatpak prüfen. |
| Beim Umstieg auf universal/anderen Compiler Linker- oder Architekturfehler | Frische Quellkopie verwenden bzw. generierte Engine-Objekte vor dem neuen Build bereinigen. |

Die maßgeblichen Implementierungen sind [src/updater.cpp](src/updater.cpp),
[seed-atlas.pro](seed-atlas.pro) und die Packaging-Skripte für
[Windows](installer/windows/build-installer.ps1),
[macOS](installer/macos/build-dmg.sh) und
[Flatpak](installer/flatpak/build-flatpak.sh). Wenn deren Verhalten geändert wird,
diese Anleitung entsprechend mitpflegen.
