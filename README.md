# Seed Atlas

Seed Atlas provides efficient, flexible seed-finding utilities and a map
viewer for Minecraft biome and structure generation.

The tool is designed for high performance and supports Minecraft Java Edition
stable releases up to 26.2. Snapshots, pre-releases and release candidates are
not included.

See [stable version support](VERSION_SUPPORT.md) for the release-by-release
generation changes covered by the viewer.


## Download

Windows releases are available as a normal installer and as a portable build.
Every binary release includes the exact corresponding source code.

This repository contains a modified GPLv3 version of the upstream project.
See [LEGAL_NOTICE.md](LEGAL_NOTICE.md) for attribution and modification
information.


## Build from source

Build instructions can be found in the [buildguide](buildguide.md).


## Basic feature overview

The tool features a map viewer that outlines the biomes of the Overworld,
Nether and End dimensions, with a wide zoom range and with toggles for each
supported structure type. The active game version and seed can be changed
on the fly while a matching seeds list stores a working buffer of seeds for
examination.

The integrated seed finder is highly customizable, utilizing a hierarchical
condition system that allows the user to look for features that are relative to
one another. Conditions can be based on a varity of criteria, including
structure placement, world spawn point and requirements for the biomes of an
area. The search supports Quad-Hut and Quad-Monument seed generators, which can
quickly look for seeds that include extremely rare structure constellations.
For more complex searches, the tool provides logic gates in the form of helper
conditions and can integrate Lua scripts to create custom filters that can be
edited right inside the tool.

It is also possible to find Locations in a fixed seed. In this mode, the
conditions are checked against a list of trial positions instead of the
world origin. Each location that passes the conditions is then collected
with additional information on where each individual condition was triggered.

An analysis of the biomes and structures can be performed in their respective
tabs. This provides information on the amount of biomes and structures that
are available in an area, as well as their size and positions.


## Languages

The active language can be selected under `Edit preferences`, which currently includes translations for:

- English
- German
- Chinese

Chinese translation credits are retained for
[SunnySlopes](https://github.com/SunnySlopes).


## Known issues

Desert Pyramids, Jungle Temples and, to a lesser extent, Woodland Mansions can
fail to generate in 1.18+ due to unsuitable terrain. Seed Atlas makes an
attempt to estimate the terrain based on the biomes and climate noise. However,
expect some inaccurate results.

The World Spawn point for pre-1.18 versions can sometimes be off because it
depends on the presence of a grass block, which Seed Atlas cannot test for.


## Legal information

The main code is under the GPLv3, see [LICENSE](LICENSE), while other
components are released under their respective author licenses:

- The bundled Seed Atlas generation engine, licensed under MIT; see the third-party notices for attribution.
- Cross platform [Qt](https://www.qt.io/licensing) GUI toolkit, available under (L)GPLv3.
- Dark Qt theme derived from [QDarkStyleSheet](https://github.com/ColinDuquesnoy/QDarkStyleSheet), licensed under MIT.
- Biome colors and icons are inspired by [Amidst](https://github.com/toolbox4minecraft/amidst), licensed under GPLv3.
- [Lua](https://www.lua.org/license.html) is distributed under the terms of the MIT license.

NOT AN OFFICIAL MINECRAFT PRODUCT.
NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.
