# Stable Minecraft version support

The viewer exposes every stable Java Edition release from 1.21.1 through 26.2.
Snapshots, pre-releases and release candidates are intentionally excluded.

| Releases | Seed-generation impact handled by the viewer |
| --- | --- |
| 1.21.1 | Existing 1.21 generation and Trial Chambers |
| 1.21.2–1.21.3 | Updated world-spawn algorithm used from 1.21.2 onward |
| 1.21.4 | Pale Garden biome |
| 1.21.5 | Expanded Pale Garden climate points and Woodland Mansion viability in Pale Gardens |
| 1.21.6–1.21.8 | No biome or structure-placement change; separate selectable compatibility versions |
| 1.21.9–1.21.10 | No biome or structure-placement change; Mojang's terrain-router refactor does not change the climate biome table |
| 1.21.11 | No biome or structure-placement change; mob-spawn and gameplay additions are outside this seed viewer's scope |
| 26.1–26.1.2 | No biome or structure-placement change; separate selectable compatibility versions |
| 26.2 | Stable Sulfur Caves climate parameters and biome metadata |

The 26.2 implementation uses the final release's unobfuscated
`OverworldBiomeBuilder` values, not the earlier community snapshot estimate:

- temperature: `[-1.0, 1.0]`
- humidity: `[-1.0, 1.0]`
- continentalness: `[-0.19, 0.55]`
- erosion: `[0.45, 1.0]`
- depth: `[0.2, 0.9]`
- weirdness: `[-1.1, -0.85]`

## Structure placement verification

The official Java `structure_set` data is semantically unchanged from 1.21.1
through 26.2. The regression suite checks the spacing, separation and salt for
every random-spread structure-set type exposed by the viewer, including the
special legacy frequency reducers for buried treasure and mineshafts. It also
checks structure starts observed in unmodified Vanilla server worlds for
1.21.5 and 26.2.

As in Vanilla and [Chunkbase's Seed Map](https://www.chunkbase.com/apps/seed-map),
a seed-derived generation attempt is not an unconditional guarantee that every
piece survives terrain generation. Desert Pyramids, Jungle Temples and some
Woodland Mansions can still be rejected by unsuitable terrain; this existing
limitation is documented in the main README.

Primary release references:

- [Minecraft Java Edition 1.21.4](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-4)
- [Minecraft Java Edition 1.21.5](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-5)
- [Minecraft Java Edition 1.21.6](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-6)
- [Minecraft Java Edition 1.21.7](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-7)
- [Minecraft Java Edition 1.21.8](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-8)
- [Minecraft Java Edition 1.21.9](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-9)
- [Minecraft Java Edition 1.21.10](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-10)
- [Minecraft Java Edition 1.21.11](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-11)
- [Minecraft Java Edition 26.1](https://www.minecraft.net/en-us/article/minecraft-java-edition-26-1)
- [Minecraft Java Edition 26.2](https://www.minecraft.net/en-us/article/minecraft-java-edition-26-2)
