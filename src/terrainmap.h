#ifndef TERRAINMAP_H
#define TERRAINMAP_H

#include "seedatlas-engine/finders.h"

#include <atomic>
#include <vector>

// Samples approximate surface heights for the pixels of a map tile.
// The output is a (r.sx+2) x (r.sz+2) grid with a one-pixel border, so that
// slopes can be evaluated at the edges of the tile. Coarse step bits sample
// every (1 << stepbits) pixels and interpolate in between.
bool sampleHeightGrid(std::vector<float>& height, Range r,
        const Generator *g, const SurfaceNoise *sn, int stepbits, bool bicubic,
        const std::atomic_bool *abort);

// Recolors a biome tile in a Minecraft-like terrain style: the surface
// material of each biome (tinted grass, sand, snow, stone, water, ...) with
// hill shading from the approximate surface heights. The height grid has the
// layout of sampleHeightGrid() and may be empty, which leaves the tile flat.
// Biomes that are unknown (negative ids) keep their color.
void applyTerrainStyle(unsigned char *rgb, const int *biomes, Range r, int mc,
        int dim, uint64_t seed, const std::vector<float>& height);

#endif // TERRAINMAP_H
