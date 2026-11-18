# Structure icon sources

Icons follow DUzzL/seed-atlas-xaero, StructureIcons.java (main, imported 2026-10-31).
Vanilla textures are extracted from the official Minecraft Java 26.3 client.
Villager, warden, chest and cracked mossy stone bricks use the identical crop,
flip and composition rules from that implementation. Six custom PNGs are copied
byte/pixel faithfully from the Xaero repository. Minecraft textures belong to
Mojang/Microsoft; these assets are not covered by the application code license.

The inactive look of a layer is computed from the icon (`inactiveLayerImage()` in
`src/widgets.cpp`); there are no separate files for it.

# Condition and layer glyphs

The icons of the dimensions (Overworld, Nether, End), the grid and terrain
layers and the non-structure condition types (biome layers, biome patches,
climate, temperature, surface height, quad structures, logic groups, Lua,
spiral) are not image files. They are monochrome glyphs drawn in code
(`src/glyphs.cpp`, original artwork of this project) and tinted from the
theme when painted. They replace the former bitmaps `overworld`, `nether`,
`the_end`, `grid` (with their `_d` variants), `map`, `tempcat`, `height`,
`quad`, `reference` and `helper`. `SEED_ATLAS_UI_GLYPH_SHEET=<file.png>`
writes a contact sheet of them.
