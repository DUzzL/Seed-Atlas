#include "terrainmap.h"

#include <algorithm>
#include <cmath>

// The terrain look follows the Seed Atlas mod for Xaero's World Map: each
// biome's vanilla grass, foliage or water tint times the average brightness
// of the block texture, lit from the north-west. The surface is inferred from
// the biome and the approximate height; trees, structures and surface rules
// are not predicted.

namespace {

enum Ground
{
    G_GRASS, G_WATER, G_FROZEN_WATER, G_SAND, G_STONE, G_GRAVEL,
    G_TERRACOTTA, G_MYCELIUM, G_MUD, G_FIXED,
};

struct BiomeLook
{
    float temp = 0.8f;      // vanilla base temperature
    float downfall = 0.4f;  // vanilla downfall
    int ground = G_GRASS;
    int water = 0x3F76E4;   // vanilla water tint
    int grass = -1;         // grass tint override
    int foliage = -1;       // foliage tint override
    bool darkGrass = false; // dark forest grass modifier
    int fixed = 0;          // color for G_FIXED
    float canopy = 0;       // share of the ground covered by tree crowns
    float leafTexture = 0;  // average brightness of the leaf texture
    int leaf = -1;          // fixed leaf color, otherwise the foliage tint
    bool deep = false;      // deep ocean variant
};

const float GRASS_TEXTURE = 0.62f;
const float WATER_TEXTURE = 0.70f;
const int SNOW = 0xE8EFF0;
const int ICE = 0xA0B8E0;
const int SAND = 0xD8CF9B;
const int STONE = 0x858585;
const int GRAVEL = 0x7F7C7B;
const int TERRACOTTA = 0xB06B48;
const int MYCELIUM = 0x7A6D72;
const int MUD = 0x514B40;
const int END_STONE = 0xD9D99A;
const int END_VOID = 0x1A1A2E;
const int CROWN_CELL = 5;
const uint64_t CROWN_SALT = 0x5EEDA71A5ULL;
const float SEA_LEVEL = 62.5f;

// Grass and foliage colormap corners: hot-dry, hot-wet and cold.
const int GRASS_CORNERS[3] = { 0xBFB755, 0x47CD33, 0x80B497 };
const int FOLIAGE_CORNERS[3] = { 0xAEA42A, 0x1ABF00, 0x60A17B };
const int FOLIAGE_BIRCH = 0x80A755;
const int FOLIAGE_EVERGREEN = 0x619961;

struct Looks
{
    BiomeLook b[256];

    BiomeLook& at(int id) { return b[id & 0xff]; }

    void climate(int id, float temp, float downfall)
    {
        at(id).temp = temp;
        at(id).downfall = downfall;
    }
    void ground(int id, int ground) { at(id).ground = ground; }
    void sea(int id, int water, bool deep, bool frozen = false)
    {
        at(id).temp = frozen ? 0.0f : 0.5f;
        at(id).downfall = 0.5f;
        at(id).ground = frozen ? G_FROZEN_WATER : G_WATER;
        at(id).water = water;
        at(id).deep = deep;
    }
    void trees(int id, float canopy, float texture, int leaf = -1)
    {
        at(id).canopy = canopy;
        at(id).leafTexture = texture;
        at(id).leaf = leaf;
    }
    void fixed(int id, int color)
    {
        at(id).ground = G_FIXED;
        at(id).fixed = color;
    }

    Looks()
    {
        sea(ocean, 0x3F76E4, false);
        sea(deep_ocean, 0x3F76E4, true);
        sea(warm_ocean, 0x43D5EE, false);
        sea(deep_warm_ocean, 0x43D5EE, true);
        sea(lukewarm_ocean, 0x45ADF2, false);
        sea(deep_lukewarm_ocean, 0x45ADF2, true);
        sea(cold_ocean, 0x3D57D6, false);
        sea(deep_cold_ocean, 0x3D57D6, true);
        sea(frozen_ocean, 0x3938C9, false, true);
        sea(deep_frozen_ocean, 0x3938C9, true, true);
        sea(river, 0x3F76E4, false);
        sea(frozen_river, 0x3938C9, false, true);

        for (int id : { plains, sunflower_plains })
            climate(id, 0.8f, 0.4f);
        for (int id : { desert, desert_hills, desert_lakes })
        {
            climate(id, 2.0f, 0.0f);
            ground(id, G_SAND);
        }
        for (int id : { mountains, mountain_edge, wooded_mountains,
                gravelly_mountains, modified_gravelly_mountains, stone_shore })
            climate(id, 0.2f, 0.3f);
        ground(gravelly_mountains, G_GRAVEL);
        ground(modified_gravelly_mountains, G_GRAVEL);
        ground(stone_shore, G_STONE);
        for (int id : { forest, wooded_hills, flower_forest, dark_forest,
                dark_forest_hills, dappled_forest })
            climate(id, 0.7f, 0.8f);
        for (int id : { taiga, taiga_hills, taiga_mountains,
                giant_spruce_taiga, giant_spruce_taiga_hills })
            climate(id, 0.25f, 0.8f);
        for (int id : { giant_tree_taiga, giant_tree_taiga_hills })
            climate(id, 0.3f, 0.8f);
        for (int id : { snowy_taiga, snowy_taiga_hills, snowy_taiga_mountains })
            climate(id, -0.5f, 0.4f);
        for (int id : { snowy_tundra, snowy_mountains, ice_spikes })
            climate(id, 0.0f, 0.5f);
        climate(snowy_beach, 0.05f, 0.3f);
        for (int id : { swamp, swamp_hills, mangrove_swamp })
        {
            climate(id, 0.8f, 0.9f);
            at(id).grass = at(id).foliage = 0x6A7039;
            at(id).water = 0x617B64;
        }
        at(mangrove_swamp).water = 0x3A7A6A;
        ground(mangrove_swamp, G_MUD);
        for (int id : { mushroom_fields, mushroom_field_shore })
        {
            climate(id, 0.9f, 1.0f);
            ground(id, G_MYCELIUM);
        }
        climate(beach, 0.8f, 0.4f);
        ground(beach, G_SAND);
        for (int id : { jungle, jungle_hills, modified_jungle,
                bamboo_jungle, bamboo_jungle_hills })
            climate(id, 0.95f, 0.9f);
        for (int id : { jungle_edge, modified_jungle_edge })
            climate(id, 0.95f, 0.8f);
        for (int id : { birch_forest, birch_forest_hills, tall_birch_forest, tall_birch_hills })
            climate(id, 0.6f, 0.6f);
        for (int id : { savanna, savanna_plateau, shattered_savanna, shattered_savanna_plateau })
            climate(id, 2.0f, 0.0f);
        for (int id : { badlands, wooded_badlands_plateau, badlands_plateau, eroded_badlands,
                modified_wooded_badlands_plateau, modified_badlands_plateau })
        {
            climate(id, 2.0f, 0.0f);
            ground(id, G_TERRACOTTA);
            at(id).grass = 0x90814D;
            at(id).foliage = 0x9E814D;
        }
        climate(meadow, 0.5f, 0.8f);
        at(meadow).water = 0x0E4ECF;
        climate(grove, -0.2f, 0.8f);
        climate(snowy_slopes, -0.3f, 0.9f);
        climate(jagged_peaks, -0.7f, 0.9f);
        climate(frozen_peaks, -0.7f, 0.9f);
        climate(stony_peaks, 1.0f, 0.3f);
        ground(stony_peaks, G_STONE);
        climate(cherry_grove, 0.5f, 0.8f);
        at(cherry_grove).grass = at(cherry_grove).foliage = 0xB6DB61;
        at(cherry_grove).water = 0x5DB7EF;
        climate(pale_garden, 0.7f, 0.8f);
        at(pale_garden).grass = 0x778272;
        at(pale_garden).foliage = 0x878D76;
        at(pale_garden).water = 0x76889D;
        climate(lush_caves, 0.5f, 0.5f);
        for (int id : { dark_forest, dark_forest_hills })
            at(id).darkGrass = true;

        // Leaf textures: average brightness of the vanilla leaf textures.
        for (int id : { forest, wooded_hills })
            trees(id, 0.55f, 0.60f);
        trees(flower_forest, 0.35f, 0.60f);
        trees(dappled_forest, 0.45f, 0.60f);
        trees(wooded_mountains, 0.45f, 0.55f);
        for (int id : { dark_forest, dark_forest_hills })
            trees(id, 0.90f, 0.50f);
        for (int id : { birch_forest, birch_forest_hills })
            trees(id, 0.55f, 0.62f, FOLIAGE_BIRCH);
        for (int id : { tall_birch_forest, tall_birch_hills })
            trees(id, 0.60f, 0.62f, FOLIAGE_BIRCH);
        for (int id : { taiga, taiga_hills, taiga_mountains })
            trees(id, 0.55f, 0.42f, FOLIAGE_EVERGREEN);
        for (int id : { giant_tree_taiga, giant_tree_taiga_hills })
            trees(id, 0.70f, 0.42f, FOLIAGE_EVERGREEN);
        for (int id : { giant_spruce_taiga, giant_spruce_taiga_hills })
            trees(id, 0.75f, 0.42f, FOLIAGE_EVERGREEN);
        for (int id : { jungle, jungle_hills, modified_jungle })
            trees(id, 0.85f, 0.62f);
        for (int id : { bamboo_jungle, bamboo_jungle_hills })
            trees(id, 0.75f, 0.62f);
        for (int id : { jungle_edge, modified_jungle_edge })
            trees(id, 0.30f, 0.62f);
        trees(savanna, 0.12f, 0.62f);
        trees(savanna_plateau, 0.15f, 0.62f);
        for (int id : { shattered_savanna, shattered_savanna_plateau })
            trees(id, 0.08f, 0.62f);
        for (int id : { wooded_badlands_plateau, modified_wooded_badlands_plateau })
            trees(id, 0.25f, 0.60f);
        for (int id : { swamp, swamp_hills })
            trees(id, 0.15f, 0.60f);
        trees(mangrove_swamp, 0.75f, 0.55f, 0x8DB127);
        // Cherry and pale oak leaves are not tinted: these are their texture averages.
        trees(cherry_grove, 0.50f, 0.95f, 0xE5ADC2);
        trees(pale_garden, 0.85f, 1.05f, 0x757A73);
        // Alpha and Beta biomes.
        trees(seasonal_forest, 0.55f, 0.60f);
        trees(rainforest, 0.75f, 0.62f);
        trees(shrubland, 0.20f, 0.60f);

        // Nether surfaces: netherrack, soul sand, nylium and basalt.
        fixed(nether_wastes, 0x723232);
        fixed(soul_sand_valley, 0x54402F);
        fixed(crimson_forest, 0x8B1F1F);
        fixed(warped_forest, 0x2B7265);
        fixed(basalt_deltas, 0x4C4C51);
        for (int id : { nether_wastes, soul_sand_valley, crimson_forest,
                warped_forest, basalt_deltas })
            climate(id, 2.0f, 0.0f);
    }
};

static const BiomeLook& look(int id)
{
    static const Looks looks;
    return looks.b[id & 0xff];
}

static uint64_t hash3(int64_t x, int64_t z, uint64_t seed)
{
    uint64_t h = seed ^ ((uint64_t)x * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)z * 0xC2B2AE3D27D4EB4FULL);
    h ^= h >> 32; h *= 0xD6E8FEB86659FD93ULL; h ^= h >> 32;
    return h & 0x7fffffffffffffffULL;
}

static int clampc(float v)
{
    return v <= 0 ? 0 : v >= 255 ? 255 : (int) lroundf(v);
}

static int scaleColor(int rgb, float f)
{
    return (clampc(((rgb >> 16) & 0xff) * f) << 16)
         | (clampc(((rgb >>  8) & 0xff) * f) <<  8)
         |  clampc(((rgb >>  0) & 0xff) * f);
}

static int mixColor(int a, int b, float t)
{
    int ret = 0;
    for (int s = 16; s >= 0; s -= 8)
    {
        float ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
        ret |= clampc(ca + (cb - ca) * t) << s;
    }
    return ret;
}

// Barycentric form of the vanilla colormap lookup.
static int colormap(const int corners[3], float temp, float downfall)
{
    float t = std::clamp(temp, 0.0f, 1.0f);
    float d = std::clamp(downfall, 0.0f, 1.0f) * t;
    float w[3] = { t - d, d, 1.0f - t };
    int ret = 0;
    for (int s = 16; s >= 0; s -= 8)
    {
        float c = 0;
        for (int k = 0; k < 3; k++)
            c += w[k] * ((corners[k] >> s) & 0xff);
        ret |= clampc(c) << s;
    }
    return ret;
}

static void shadeTo(unsigned char *col, int rgb, int percent)
{
    float f = (100 + percent) / 100.0f;
    col[0] = clampc(((rgb >> 16) & 0xff) * f);
    col[1] = clampc(((rgb >>  8) & 0xff) * f);
    col[2] = clampc(((rgb >>  0) & 0xff) * f);
}

static int grassColor(const BiomeLook& b)
{
    int c = b.grass >= 0 ? b.grass : colormap(GRASS_CORNERS, b.temp, b.downfall);
    if (b.darkGrass)
        c = ((c & 0xFEFEFE) + 0x28340A) >> 1;
    return c;
}

// Tree crowns of radius 1.8..2.7 on a jittered 5-block grid, lit from the
// north-west. Pixels of 8+ blocks blend ground and leaves by coverage.
static int crowns(int ground, int leaf, float density, int64_t x, int64_t z, int scale)
{
    if (scale >= 8)
        return mixColor(ground, leaf, density);
    int64_t cx = x >= 0 ? x / CROWN_CELL : -((-x + CROWN_CELL - 1) / CROWN_CELL);
    int64_t cz = z >= 0 ? z / CROWN_CELL : -((-z + CROWN_CELL - 1) / CROWN_CELL);
    // A crown covers about 0.6 of its cell, which gives the requested coverage.
    uint64_t threshold = (uint64_t) lroundf(std::min(1.0f, density / 0.6f) * 1000.0f);
    for (int dz = -1; dz <= 1; dz++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            uint64_t h = hash3(cx + dx, cz + dz, CROWN_SALT);
            if (h % 1000 >= threshold)
                continue;
            // subtract in integers: a float has no fractional block left
            // beyond about 8 million blocks (both +0.5 centring terms cancel)
            int64_t centrex = (cx + dx) * CROWN_CELL + (int64_t)((h >> 12) % CROWN_CELL);
            int64_t centrez = (cz + dz) * CROWN_CELL + (int64_t)((h >> 20) % CROWN_CELL);
            float radius = 1.8f + ((h >> 28) % 10) / 10.0f;
            float ox = (float)(x - centrex);
            float oz = (float)(z - centrez);
            if (ox * ox + oz * oz > radius * radius)
                continue;
            float light = -(ox + oz) / radius * 9.0f;
            return scaleColor(leaf, 1.0f + light / 100.0f);
        }
    }
    return ground;
}

static int surfaceColor(const BiomeLook& b, int64_t x, int64_t z, float y, int scale)
{
    switch (b.ground)
    {
    case G_WATER:
        return scaleColor(b.water, WATER_TEXTURE);
    case G_FROZEN_WATER:
        return mixColor(scaleColor(b.water, WATER_TEXTURE), ICE, 0.6f);
    case G_FIXED:
        return b.fixed;
    }
    // Vanilla cools the air by 0.05 per 40 blocks above y=80.
    float temp = b.temp;
    if (y > 80)
        temp -= (y - 80) * 0.05f / 40.0f;
    if (temp < 0.15f)
        return SNOW; // snow layers also cover the trees
    int ground;
    switch (b.ground)
    {
    case G_SAND:        ground = SAND; break;
    case G_STONE:       ground = STONE; break;
    case G_GRAVEL:      ground = GRAVEL; break;
    case G_TERRACOTTA:  ground = TERRACOTTA; break;
    case G_MYCELIUM:    ground = MYCELIUM; break;
    case G_MUD:         ground = MUD; break;
    default:            ground = scaleColor(grassColor(b), GRASS_TEXTURE); break;
    }
    if (b.canopy <= 0)
        return ground;
    int tint = b.leaf;
    if (tint < 0)
        tint = b.foliage >= 0 ? b.foliage : colormap(FOLIAGE_CORNERS, b.temp, b.downfall);
    return crowns(ground, scaleColor(tint, b.leafTexture), b.canopy, x, z, scale);
}

// Share of solid ground that an End biome carries when no heights are known.
// The main island sits inside the central the_end biome, which is otherwise void.
static float endLand(int id, int64_t bx, int64_t bz)
{
    switch (id)
    {
    case end_highlands:     return 0.90f;
    case end_midlands:      return 0.65f;
    case end_barrens:       return 0.30f;
    case small_end_islands: return 0.12f;
    case the_end:           return bx * bx + bz * bz < 100 * 100 ? 1.0f : 0.0f;
    default:                return 0.0f;
    }
}

static void applyEndStyle(unsigned char *rgb, const int *biomes, Range r,
        const std::vector<float>& height)
{
    int w = r.sx, h = r.sz, hw = w + 2;
    bool heights = height.size() == (size_t) hw * (h + 2);
    std::vector<float> land;
    if (!heights)
    {
        land.resize(w * h);
        for (int j = 0; j < h; j++)
        {
            for (int i = 0; i < w; i++)
            {
                int64_t bx = (int64_t)(r.x + i) * r.scale + r.scale / 2;
                int64_t bz = (int64_t)(r.z + j) * r.scale + r.scale / 2;
                land[j*w + i] = endLand(biomes[j*w + i], bx, bz);
            }
        }
    }
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            int id = biomes[j*w + i];
            if (id < 0)
                continue;
            unsigned char *col = rgb + 3*(j*w + i);
            int grain = (int)(hash3(r.x + i, r.z + j, 0x5EED5) % 7) - 3;
            if (heights)
            {
                float y = height[(j+1)*hw + i+1];
                if (!(y > 0))
                {
                    shadeTo(col, END_VOID, 0);
                    continue;
                }
                // Void has no surface: do not turn the drop into a bright or dark rim.
                float nw = height[j*hw + i];
                float se = height[(j+2)*hw + i+2];
                if (!(nw > 0)) nw = y;
                if (!(se > 0)) se = y;
                float slope = (se - nw) / (2.0f * r.scale);
                int delta = (int) lroundf(std::clamp(slope * 120.0f, -40.0f, 40.0f));
                shadeTo(col, END_STONE, delta + grain);
                continue;
            }
            // Without heights, the biomes alternate at the scale of single
            // islands, so blend stone and void by the land share around.
            float sum = 0;
            int cnt = 0;
            for (int dj = -1; dj <= 1; dj++)
            {
                for (int di = -1; di <= 1; di++)
                {
                    int ii = i + di, jj = j + dj;
                    if (ii < 0 || jj < 0 || ii >= w || jj >= h)
                        continue;
                    sum += land[jj*w + ii];
                    cnt++;
                }
            }
            float share = sum / cnt;
            shadeTo(col, mixColor(END_VOID, END_STONE, share), share > 0 ? grain - 6 : 0);
        }
    }
}

} // namespace

static float cubic_hermite(float p[4], float u)
{
    float a = p[1];
    float b = 0.5 * (-p[0] + p[2]);
    float c = p[0] - 2.5*p[1] + 2*p[2] - 0.5*p[3];
    float d = 0.5 * (-p[0] + 3*p[1] - 3*p[2] + p[3]);
    return a + b*u + c*u*u + d*u*u*u;
}

// floor(v / 4) for negative coordinates as well
static int floorDiv4(int64_t v)
{
    return (int) (v >= 0 ? v / 4 : -((-v + 3) / 4));
}

bool sampleHeightGrid(std::vector<float>& height, Range r,
        const Generator *g, const SurfaceNoise *sn, int stepbits, bool bicubic,
        const std::atomic_bool *abort)
{
    int bd = bicubic ? 1 : 0; // sampling border
    int ps = stepbits; // bits in step size
    int st = (1 << ps) - 1; // step mask
    // the sampling spans [x-1, x+w+1), reduced to the stepped grid
    int x = r.x, z = r.z, w = r.sx, h = r.sz;
    int px = ((x-1) >> ps);
    int pz = ((z-1) >> ps);
    int pw = ((x+w+2+st) >> ps) - px + 1;
    int ph = ((z+h+2+st) >> ps) - pz + 1;
    if (bd)
    {   // add border for cubic sampling
        px -= bd; pz -= bd;
        pw += 2*bd; ph += 2*bd;
    }
    std::vector<float> buf(pw * ph);
    if (ps == 0 && r.scale <= 8 && g->dim == DIM_END)
    {
        mapEndSurfaceHeight(&buf[0], &g->en, sn, px, pz, pw, ph, r.scale, 0);
        mapEndIslandHeight(&buf[0], &g->en, g->seed, px, pz, pw, ph, r.scale);
    }
    else if (ps == 0 && r.scale == 4)
    {
        mapApproxHeight(&buf[0], 0, g, sn, px, pz, pw, ph);
    }
    else
    {
        for (int j = 0; j < ph; j++)
        {
            for (int i = 0; i < pw; i++)
            {
                if (abort && *abort) return false;
                int samplex = floorDiv4((int64_t)((px + i) << ps) * r.scale);
                int samplez = floorDiv4((int64_t)((pz + j) << ps) * r.scale);
                float y = 0;
                mapApproxHeight(&y, 0, g, sn, samplex, samplez, 1, 1);
                buf[j*pw+i] = y;
            }
        }
    }

    // interpolate height
    height.assign((w+2) * (h+2), 0);
    for (int j = 0; j < h+2; j++)
    {
        for (int i = 0; i < w+2; i++)
        {
            if (abort && *abort) return false;
            int pi = ((x + i - 1) >> ps) - px - bd;
            int pj = ((z + j - 1) >> ps) - pz - bd;
            float di = ((x + i - 1) & st) / (float)(st + 1);
            float dj = ((z + j - 1) & st) / (float)(st + 1);
            float v = 0;
            if (bicubic)
            {
                float p[] = {
                    cubic_hermite(&buf.at((pj+0)*pw + pi), di),
                    cubic_hermite(&buf.at((pj+1)*pw + pi), di),
                    cubic_hermite(&buf.at((pj+2)*pw + pi), di),
                    cubic_hermite(&buf.at((pj+3)*pw + pi), di),
                };
                v = cubic_hermite(p, dj);
            }
            else // bilinear
            {
                float v00 = buf.at((pj+0)*pw + pi+0);
                float v01 = buf.at((pj+0)*pw + pi+1);
                float v10 = buf.at((pj+1)*pw + pi+0);
                float v11 = buf.at((pj+1)*pw + pi+1);
                v = lerp2(di, dj, v00, v01, v10, v11);
            }
            height[j*(w+2)+i] = v;
        }
    }
    return true;
}

void applyTerrainStyle(unsigned char *rgb, const int *biomes, Range r, int mc,
        int dim, uint64_t seed, const std::vector<float>& height)
{
    (void) mc;
    if (dim == DIM_END)
    {
        applyEndStyle(rgb, biomes, r, height);
        return;
    }
    int w = r.sx, h = r.sz, hw = w + 2;
    bool heights = height.size() == (size_t) hw * (h + 2);
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            int id = biomes[j*w + i];
            if (id < 0 || id >= 256)
                continue;
            const BiomeLook& b = look(id);
            int64_t bx = (int64_t)(r.x + i) * r.scale;
            int64_t bz = (int64_t)(r.z + j) * r.scale;
            float y = heights ? height[(j+1)*hw + i+1] : 64;
            bool water = b.ground == G_WATER || b.ground == G_FROZEN_WATER;
            int color = surfaceColor(b, bx, bz, y, r.scale);
            int delta = -8;
            if (dim == DIM_NETHER)
            {
                delta += (int)(hash3(r.x + i, r.z + j, seed ^ 0x4E7B) % 5) - 2;
            }
            else if (water)
            {   // darker where the floor is further below sea level
                if (heights)
                    delta -= (int) lroundf(std::min(24.0f, std::max(0.0f, SEA_LEVEL - y) * 0.65f));
                if (b.deep)
                    delta -= 6;
            }
            else
            {
                if (heights)
                {   // light from the north-west
                    float slope = (height[(j+2)*hw + i+2] - height[j*hw + i]) / (2.0f * r.scale);
                    if (std::isfinite(slope))
                        delta += (int) lroundf(std::clamp(slope * 65.0f, -28.0f, 28.0f));
                }
                delta += (int)(hash3(r.x + i, r.z + j, seed) % 5) - 2;
            }
            // biome-edge outline: lighter on the north-west, darker on the south-east
            if (i > 0 && j > 0 && biomes[(j-1)*w + i-1] != id)
                delta += 10;
            if (i+1 < w && j+1 < h && biomes[(j+1)*w + i+1] != id)
                delta -= 10;
            shadeTo(rgb + 3*(j*w + i), color, delta);
        }
    }
}
