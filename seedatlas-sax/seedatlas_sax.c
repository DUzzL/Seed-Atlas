#include "seedatlas_sax.h"

#include "biomes.h"
#include "finders.h"
#include "generator.h"
#include "noise.h"
#include "util.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SAX_CONTEXT_MAGIC UINT32_C(0x53415831)
#define SAX_MAX_SCAN_CELLS INT64_C(4194304)
/* Oldest selectable version. Terrain heights, the Nether floor and ore veins
   follow the 1.18+ noise generation and are unsupported before SAX_TERRAIN_MC. */
#define SAX_MIN_MC MC_1_1
#define SAX_TERRAIN_MC MC_1_18

struct sax_context {
    uint32_t magic;
    uint32_t generator_flags;
    int mc; /* engine MC_* version the context generates */
    uint64_t seed;
    Generator biome_generators[3];
    SurfaceNoise end_height_noise; /* read-only after sax_create */
    SurfaceNoise nether_noise; /* vanilla nether old_blended_noise */
    uint32_t biome_argb[256];
};

typedef struct sax_ore_noise {
    PerlinNoise oct[8];
    DoublePerlinNoise veininess;
    DoublePerlinNoise vein_a;
    DoublePerlinNoise vein_b;
    DoublePerlinNoise gap;
    Xoroshiro positional;
} sax_ore_noise;

static int context_valid(const sax_context *context)
{
    return context && context->magic == SAX_CONTEXT_MAGIC;
}

static int dimension_valid(int32_t dimension)
{
    return dimension == SAX_DIM_OVERWORLD || dimension == SAX_DIM_NETHER ||
           dimension == SAX_DIM_END;
}

static int32_t floor_div(int32_t value, int32_t divisor)
{
    int32_t quotient = value / divisor;
    int32_t remainder = value % divisor;
    return quotient - (remainder < 0);
}

static int32_t copy_string(const char *text, char *out, int32_t capacity)
{
    size_t length;
    int32_t required;
    if (!text)
        return 0;
    length = strlen(text);
    if (length >= INT32_MAX)
        return SAX_ERR_GENERATION;
    required = (int32_t) length + 1;
    if (out && capacity > 0) {
        size_t copied = length;
        if (copied >= (size_t) capacity)
            copied = (size_t) capacity - 1;
        memcpy(out, text, copied);
        out[copied] = '\0';
    }
    return required;
}

static int setup_dimension_generator(const sax_context *context,
                                     int32_t dimension, Generator *generator)
{
    if (!context_valid(context) || !generator || !dimension_valid(dimension))
        return SAX_ERR_ARGUMENT;
    setupGenerator(generator, context->mc, context->generator_flags);
    applySeed(generator, dimension, context->seed);
    return SAX_OK;
}

static const Generator *context_biome_generator(const sax_context *context,
                                                 int32_t dimension)
{
    if (!context_valid(context) || !dimension_valid(dimension))
        return NULL;
    if (dimension == SAX_DIM_OVERWORLD)
        return &context->biome_generators[0];
    if (dimension == SAX_DIM_NETHER)
        return &context->biome_generators[1];
    return &context->biome_generators[2];
}

/* Scale the generator produces directly for sample_step: layered Overworld
   generation (up to 1.17) only has the 1:4, 1:16, 1:64 and 1:256 layers. */
static int32_t layered_scale(const Generator *generator, int32_t sample_step)
{
    int32_t scale = 256;
    if (generator->dim != DIM_OVERWORLD || generator->mc >= MC_1_18 || sample_step < 4)
        return sample_step;
    while (scale > sample_step)
        scale /= 4;
    return scale;
}

static int sample_step_valid(int32_t sample_step)
{
    return sample_step >= 1 && sample_step <= 1024 &&
           (sample_step & (sample_step - 1)) == 0;
}

static uint32_t color_from_table(unsigned char colors[256][3], int mc, int id)
{
    if (id < 0 || id >= 256 || !biomeExists(mc, id))
        return 0;
    return UINT32_C(0xff000000) | ((uint32_t) colors[id][0] << 16) |
           ((uint32_t) colors[id][1] << 8) | (uint32_t) colors[id][2];
}

static uint32_t context_biome_color(const sax_context *context, int id)
{
    if (!context_valid(context) || id < 0 || id >= 256)
        return 0;
    return context->biome_argb[id];
}

static int append_result(int32_t *out, int32_t capacity, int32_t *count,
                         int32_t type, int32_t x, int32_t y, int32_t z,
                         int32_t flags, int32_t detail)
{
    int32_t *row;
    if (*count >= capacity)
        return 0;
    row = out + (size_t) *count * SAX_RESULT_STRIDE;
    row[0] = type;
    row[1] = x;
    row[2] = y;
    row[3] = z;
    row[4] = flags;
    row[5] = detail;
    ++*count;
    return 1;
}

static int box_valid(int32_t min_x, int32_t min_z,
                     int32_t max_x, int32_t max_z)
{
    return min_x <= max_x && min_z <= max_z;
}

static int in_box(Pos position, int32_t min_x, int32_t min_z,
                  int32_t max_x, int32_t max_z)
{
    return position.x >= min_x && position.x <= max_x &&
           position.z >= min_z && position.z <= max_z;
}

static int range_too_large(int32_t x0, int32_t z0, int32_t x1, int32_t z1)
{
    int64_t width = (int64_t) x1 - x0 + 1;
    int64_t height = (int64_t) z1 - z0 + 1;
    return width <= 0 || height <= 0 || width > SAX_MAX_SCAN_CELLS ||
           height > SAX_MAX_SCAN_CELLS || width * height > SAX_MAX_SCAN_CELLS;
}

SAX_API int32_t sax_abi_version(void)
{
    return SAX_ABI_VERSION;
}

SAX_API uint32_t sax_capabilities(void)
{
    return SAX_CAP_BIOMES | SAX_CAP_STRUCTURES | SAX_CAP_STRONGHOLDS |
           SAX_CAP_SPAWN | SAX_CAP_SLIME | SAX_CAP_ORE_VEINS |
           SAX_CAP_END_SHIPS | SAX_CAP_BIOME_SAMPLING |
           SAX_CAP_BIOME_AREAS | SAX_CAP_HEIGHTS |
           SAX_CAP_NETHER_FLOOR | SAX_CAP_NETHER_CARVING |
           SAX_CAP_MC_VERSIONS;
}

SAX_API int32_t sax_engine_version(char *out, int32_t capacity)
{
    return copy_string("seedatlas-engine MC 1.1-26.3 / ABI 6", out, capacity);
}

SAX_API int32_t sax_mc_version_supported(const char *mc_version)
{
    int mc = mc_version ? str2mc(mc_version) : MC_UNDEF;
    return mc >= SAX_MIN_MC && mc <= MC_NEWEST;
}

SAX_API sax_context *sax_create(int64_t seed, uint32_t world_flags)
{
    return sax_create_versioned(seed, world_flags, NULL);
}

SAX_API sax_context *sax_create_versioned(int64_t seed, uint32_t world_flags,
                                          const char *mc_version)
{
    sax_context *context;
    unsigned char colors[256][3];
    int mc = MC_NEWEST;
    int i;
    if (world_flags & ~SAX_WORLD_LARGE_BIOMES)
        return NULL;
    if (mc_version) {
        if (!sax_mc_version_supported(mc_version))
            return NULL;
        mc = str2mc(mc_version);
    }
    context = (sax_context *) calloc(1, sizeof(*context));
    if (!context)
        return NULL;
    context->magic = SAX_CONTEXT_MAGIC;
    context->mc = mc;
    context->seed = (uint64_t) seed;
    context->generator_flags =
        (world_flags & SAX_WORLD_LARGE_BIOMES) ? LARGE_BIOMES : 0;
    setupGenerator(&context->biome_generators[0], mc,
                   context->generator_flags);
    applySeed(&context->biome_generators[0], SAX_DIM_OVERWORLD,
              context->seed);
    setupGenerator(&context->biome_generators[1], mc,
                   context->generator_flags);
    applySeed(&context->biome_generators[1], SAX_DIM_NETHER,
              context->seed);
    setupGenerator(&context->biome_generators[2], mc,
                   context->generator_flags);
    applySeed(&context->biome_generators[2], SAX_DIM_END,
              context->seed);
    initSurfaceNoise(&context->end_height_noise, DIM_END, context->seed);
    /* 1.18+ RandomState: legacy terrain noise uses the world seed directly. */
    initSurfaceNoise(&context->nether_noise, DIM_END, context->seed);
    /* noise_settings/nether: xz 1 / y 3 per cell, factors 80 and 60. */
    context->nether_noise.xzScale = 1.0;
    context->nether_noise.yScale = 3.0;
    context->nether_noise.xzFactor = 80;
    context->nether_noise.yFactor = 60;
    initBiomeColors(colors);
    for (i = 0; i < 256; ++i)
        context->biome_argb[i] = color_from_table(colors, mc, i);
    return context;
}

SAX_API void sax_destroy(sax_context *context)
{
    if (!context)
        return;
    context->magic = 0;
    free(context);
}

SAX_API int32_t sax_biome_at(const sax_context *context, int32_t dimension,
                            int32_t x, int32_t y, int32_t z,
                            int32_t *out_biome_id)
{
    const Generator *generator;
    int result;
    if (!out_biome_id)
        return SAX_ERR_ARGUMENT;
    generator = context_biome_generator(context, dimension);
    if (!generator)
        return SAX_ERR_ARGUMENT;
    result = getBiomeAt(generator, 1, x, y, z);
    if (result < 0)
        return SAX_ERR_GENERATION;
    *out_biome_id = result;
    return SAX_OK;
}

SAX_API int32_t sax_biome_chunk(const sax_context *context, int32_t dimension,
                               int32_t chunk_x, int32_t chunk_z, int32_t y,
                               int32_t out_ids[SAX_CHUNK_SAMPLE_COUNT],
                               uint32_t out_argb[SAX_CHUNK_SAMPLE_COUNT])
{
    const Generator *generator;
    Range range;
    int *cache;
    int result;
    int i;
    int64_t block_x = (int64_t) chunk_x * 16;
    int64_t block_z = (int64_t) chunk_z * 16;

    if ((!out_ids && !out_argb) || block_x < INT32_MIN || block_x > INT32_MAX ||
        block_z < INT32_MIN || block_z > INT32_MAX)
        return SAX_ERR_ARGUMENT;
    generator = context_biome_generator(context, dimension);
    if (!generator)
        return SAX_ERR_ARGUMENT;
    range.scale = 1;
    range.x = (int32_t) block_x;
    range.z = (int32_t) block_z;
    range.sx = 16;
    range.sz = 16;
    range.y = y;
    range.sy = 1;
    cache = allocCache(generator, range);
    if (!cache)
        return SAX_ERR_ALLOCATION;
    result = genBiomes(generator, cache, range);
    if (result != 0) {
        free(cache);
        return SAX_ERR_GENERATION;
    }
    if (out_ids)
        memcpy(out_ids, cache, sizeof(int32_t) * SAX_CHUNK_SAMPLE_COUNT);
    if (out_argb) {
        for (i = 0; i < SAX_CHUNK_SAMPLE_COUNT; ++i)
            out_argb[i] = context_biome_color(context, cache[i]);
    }
    free(cache);
    return SAX_OK;
}

SAX_API int32_t sax_biome_chunk_sampled(
    const sax_context *context, int32_t dimension,
    int32_t chunk_x, int32_t chunk_z, int32_t y, int32_t sample_step,
    int32_t out_ids[SAX_CHUNK_SAMPLE_COUNT],
    uint32_t out_argb[SAX_CHUNK_SAMPLE_COUNT])
{
    const Generator *generator;
    Range sample_range;
    int *cache;
    int64_t block_x = (int64_t) chunk_x * 16;
    int64_t block_z = (int64_t) chunk_z * 16;
    int sample_x, sample_z;

    if (sample_step == 1)
        return sax_biome_chunk(context, dimension, chunk_x, chunk_z, y,
                               out_ids, out_argb);
    if ((!out_ids && !out_argb) ||
        (sample_step != 2 && sample_step != 4 &&
         sample_step != 8 && sample_step != 16) ||
        block_x < INT32_MIN || block_x > INT32_MAX ||
        block_z < INT32_MIN || block_z > INT32_MAX)
        return SAX_ERR_ARGUMENT;

    generator = context_biome_generator(context, dimension);
    if (!generator)
        return SAX_ERR_ARGUMENT;
    sample_range.scale = 1;
    sample_range.x = (int32_t) block_x;
    sample_range.z = (int32_t) block_z;
    sample_range.sx = 1;
    sample_range.sz = 1;
    sample_range.y = y;
    sample_range.sy = 1;
    cache = allocCache(generator, sample_range);
    if (!cache)
        return SAX_ERR_ALLOCATION;
    for (sample_z = 0; sample_z < 16; sample_z += sample_step) {
        for (sample_x = 0; sample_x < 16; sample_x += sample_step) {
            int id;
            int fill_x, fill_z;
            uint32_t color;
            sample_range.x = (int32_t) block_x + sample_x + sample_step / 2;
            sample_range.z = (int32_t) block_z + sample_z + sample_step / 2;
            if (genBiomes(generator, cache, sample_range) != 0) {
                free(cache);
                return SAX_ERR_GENERATION;
            }
            id = cache[0];
            color = out_argb ? context_biome_color(context, id) : 0;
            for (fill_z = sample_z;
                 fill_z < sample_z + sample_step; ++fill_z) {
                for (fill_x = sample_x;
                     fill_x < sample_x + sample_step; ++fill_x) {
                    int index = fill_z * 16 + fill_x;
                    if (out_ids)
                        out_ids[index] = id;
                    if (out_argb)
                        out_argb[index] = color;
                }
            }
        }
    }
    free(cache);
    return SAX_OK;
}

SAX_API int32_t sax_biome_area_sampled(
    const sax_context *context, int32_t dimension,
    int32_t origin_block_x, int32_t origin_block_z, int32_t y,
    int32_t sample_step, int32_t sample_width, int32_t sample_height,
    int32_t *out_ids, uint32_t *out_argb, int32_t output_capacity)
{
    const Generator *generator;
    Range range;
    int *cache;
    int64_t sample_count;
    int64_t last_x, last_z;
    int64_t covered_last_x, covered_last_z;
    int result;
    int x, z;

    if ((!out_ids && !out_argb) || !sample_step_valid(sample_step) ||
        sample_width <= 0 || sample_height <= 0 || output_capacity < 0)
        return SAX_ERR_ARGUMENT;

    sample_count = (int64_t) sample_width * sample_height;
    if (sample_count > SAX_MAX_BIOME_AREA_SAMPLES)
        return SAX_ERR_RANGE_TOO_LARGE;
    if (output_capacity < sample_count)
        return SAX_ERR_ARGUMENT;

    last_x = (int64_t) origin_block_x +
             (int64_t) (sample_width - 1) * sample_step;
    last_z = (int64_t) origin_block_z +
             (int64_t) (sample_height - 1) * sample_step;
    covered_last_x = (int64_t) origin_block_x +
                     (int64_t) sample_width * sample_step - 1;
    covered_last_z = (int64_t) origin_block_z +
                     (int64_t) sample_height * sample_step - 1;
    if (last_x < INT32_MIN || last_x > INT32_MAX ||
        last_z < INT32_MIN || last_z > INT32_MAX ||
        covered_last_x < INT32_MIN || covered_last_x > INT32_MAX ||
        covered_last_z < INT32_MIN || covered_last_z > INT32_MAX)
        return SAX_ERR_RANGE_TOO_LARGE;
    /* Block-scale cubiomes generation expands the source two blocks west and
       north before shifting to quart coordinates. Avoid signed overflow. */
    if (sample_step <= 2 &&
        (origin_block_x < INT32_MIN + 2 ||
         origin_block_z < INT32_MIN + 2))
        return SAX_ERR_RANGE_TOO_LARGE;
    if (sample_step >= 4 &&
        (origin_block_x % sample_step != 0 ||
         origin_block_z % sample_step != 0))
        return SAX_ERR_ARGUMENT;

    generator = context_biome_generator(context, dimension);
    if (!generator)
        return SAX_ERR_ARGUMENT;

    if (sample_step == 1) {
        range.scale = 1;
        range.x = origin_block_x;
        range.z = origin_block_z;
        range.sx = sample_width;
        range.sz = sample_height;
        range.y = y;
        range.sy = 1;
        cache = allocCache(generator, range);
        if (!cache)
            return SAX_ERR_ALLOCATION;
        result = genBiomes(generator, cache, range);
        if (result != 0) {
            free(cache);
            return SAX_ERR_GENERATION;
        }
        if (out_ids)
            memcpy(out_ids, cache, (size_t) sample_count * sizeof(*out_ids));
    }
    else if (sample_step == 2) {
        int64_t dense_width = (int64_t) sample_width * 2;
        int64_t dense_height = (int64_t) sample_height * 2;
        int64_t dense_count = dense_width * dense_height;
        if (dense_count > SAX_MAX_BIOME_AREA_SAMPLES)
            return SAX_ERR_RANGE_TOO_LARGE;
        range.scale = 1;
        range.x = origin_block_x;
        range.z = origin_block_z;
        range.sx = (int32_t) dense_width;
        range.sz = (int32_t) dense_height;
        range.y = y;
        range.sy = 1;
        cache = allocCache(generator, range);
        if (!cache)
            return SAX_ERR_ALLOCATION;
        result = genBiomes(generator, cache, range);
        if (result != 0) {
            free(cache);
            return SAX_ERR_GENERATION;
        }
        if (out_ids) {
            for (z = 0; z < sample_height; ++z) {
                for (x = 0; x < sample_width; ++x) {
                    out_ids[(int64_t) z * sample_width + x] =
                        cache[(int64_t) (z * 2 + 1) * range.sx + x * 2 + 1];
                }
            }
        }
    }
    else if (layered_scale(generator, sample_step) != sample_step) {
        /* Sample a supported finer layer and keep the centre of every cell. */
        int32_t base = layered_scale(generator, sample_step);
        int32_t factor = sample_step / base;
        int64_t dense_count = sample_count * factor * factor;
        if (dense_count > INT64_C(4) * SAX_MAX_BIOME_AREA_SAMPLES)
            return SAX_ERR_RANGE_TOO_LARGE;
        range.scale = base;
        range.x = origin_block_x / base;
        range.z = origin_block_z / base;
        range.sx = sample_width * factor;
        range.sz = sample_height * factor;
        range.y = floor_div(y, 4);
        range.sy = 1;
        cache = allocCache(generator, range);
        if (!cache)
            return SAX_ERR_ALLOCATION;
        result = genBiomes(generator, cache, range);
        if (result != 0) {
            free(cache);
            return SAX_ERR_GENERATION;
        }
        for (z = 0; z < sample_height; ++z)
            for (x = 0; x < sample_width; ++x)
                cache[(int64_t) z * sample_width + x] =
                    cache[((int64_t) z * factor + factor / 2) * range.sx + (int64_t) x * factor + factor / 2];
        if (out_ids)
            memcpy(out_ids, cache, (size_t) sample_count * sizeof(*out_ids));
    }
    else {
        range.scale = sample_step;
        range.x = origin_block_x / sample_step;
        range.z = origin_block_z / sample_step;
        range.sx = sample_width;
        range.sz = sample_height;
        range.y = floor_div(y, 4);
        range.sy = 1;
        cache = allocCache(generator, range);
        if (!cache)
            return SAX_ERR_ALLOCATION;
        result = genBiomes(generator, cache, range);
        if (result != 0) {
            free(cache);
            return SAX_ERR_GENERATION;
        }
        if (out_ids)
            memcpy(out_ids, cache, (size_t) sample_count * sizeof(*out_ids));
    }

    if (out_argb) {
        if (out_ids) {
            for (int64_t i = 0; i < sample_count; ++i)
                out_argb[i] = context_biome_color(context, out_ids[i]);
        }
        else if (sample_step == 2) {
            for (z = 0; z < sample_height; ++z) {
                for (x = 0; x < sample_width; ++x) {
                    int id = cache[(int64_t) (z * 2 + 1) * range.sx +
                                   x * 2 + 1];
                    out_argb[(int64_t) z * sample_width + x] =
                        context_biome_color(context, id);
                }
            }
        }
        else {
            for (int64_t i = 0; i < sample_count; ++i)
                out_argb[i] = context_biome_color(context, cache[i]);
        }
    }

    free(cache);
    return SAX_OK;
}

static int64_t floor_div64(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    return quotient - ((value % divisor) < 0);
}

/* Engine depth parameter at a quart coordinate on the engine's own block scale
   (same scale as the engine's mapApproxHeight). */
static float height_at_quart(const BiomeNoise *bn, int32_t qx, int32_t qz)
{
    int64_t np[NP_MAX];
    sampleBiomeNoise(bn, np, qx, 0, qz, NULL, SAMPLE_NO_BIOME);
    return (float) (np[NP_DEPTH] / 76.0);
}

/* Island centres of the End height noise for a whole area. The engine's
   getEndHeightNoise evaluates 25x25 simplex samples for every noise cell, though
   neighbouring cells share almost all of them; sampling each once per area makes
   End tiles several times faster with identical results. */
typedef struct end_island_grid {
    int64_t x0, z0, width;
    uint16_t *weights; /* island weight 9..21, 0 where there is no island */
} end_island_grid;

static int end_island_grid_init(end_island_grid *grid, const EndNoise *en,
                                int cell_x0, int cell_z0, int cell_x1, int cell_z1)
{
    int64_t height, i, j;
    /* getEndHeightNoise halves with C division, which is monotonic in the cell. */
    grid->x0 = cell_x0 / 2 - 12;
    grid->z0 = cell_z0 / 2 - 12;
    grid->width = cell_x1 / 2 + 12 - grid->x0 + 1;
    height = cell_z1 / 2 + 12 - grid->z0 + 1;
    grid->weights = (uint16_t *) malloc((size_t) (grid->width * height) * sizeof(*grid->weights));
    if (!grid->weights)
        return 0;
    for (j = 0; j < height; ++j) {
        for (i = 0; i < grid->width; ++i) {
            int64_t rx = grid->x0 + i;
            int64_t rz = grid->z0 + j;
            uint64_t rsq = rx*rx + rz*rz;
            uint16_t v = 0;
            if (rsq > 4096 && sampleSimplex2D(&en->perlin, rx, rz) < -0.9f)
                v = (unsigned int)(fabsf((float)rx) * 3439.0f + fabsf((float)rz) * 147.0f) % 13 + 9;
            grid->weights[j * grid->width + i] = v;
        }
    }
    return 1;
}

/* getEndHeightNoise(en, x, z, 0) with the island centres read from the grid. */
static float end_height_noise(const end_island_grid *grid, int x, int z)
{
    int hx = x / 2;
    int hz = z / 2;
    int oddx = x % 2;
    int oddz = z % 2;
    int i, j;
    int64_t h = 64 * (x*(int64_t)x + z*(int64_t)z);
    for (j = -12; j <= 12; j++) {
        const uint16_t *row = grid->weights + (hz + j - grid->z0) * grid->width + (hx - grid->x0);
        for (i = -12; i <= 12; i++) {
            uint16_t v = row[i];
            if (v) {
                int64_t rx = (oddx - i * 2);
                int64_t rz = (oddz - j * 2);
                uint64_t rsq = rx*rx + rz*rz;
                int64_t noise = rsq * v*v;
                if (noise < h)
                    h = noise;
            }
        }
    }
    float ret = 100 - sqrtf((float) h);
    if (ret < -100) ret = -100;
    if (ret > 80) ret = 80;
    return ret;
}

/* The engine's sampleNoiseColumnEnd on the island grid. */
static void end_noise_column(double column[], const SurfaceNoise *sn, const EndNoise *en,
                             const end_island_grid *grid, int x, int z, int colymin, int colymax)
{
    static const double upper_drop[] = {
           1.0,    1.0,    1.0,    1.0,    1.0,    1.0,    1.0,    1.0,
           1.0,    1.0,    1.0,    1.0,    1.0,    1.0,    1.0, 63./64,
        62./64, 61./64, 60./64, 59./64, 58./64, 57./64, 56./64, 55./64,
        54./64, 53./64, 52./64, 51./64, 50./64, 49./64, 48./64, 47./64,
        46./64
    };
    static const double lower_drop[] = {
          0.0,  0.0, 1./7, 2./7, 3./7, 4./7, 5./7, 6./7,
          1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,
          1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,
          1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,
          1.0,
    };
    int y;
    if (en->mc > MC_1_13) {
        uint64_t rsq = (uint64_t) x * x + (uint64_t) z * z;
        if ((int)rsq < 0) {
            for (y = colymin; y <= colymax; y++)
                column[y - colymin] = nan("");
            return;
        }
    }
    double depth = end_height_noise(grid, x, z) - 8.0f;
    for (y = colymin; y <= colymax; y++) {
        if (lower_drop[y] == 0.0) {
            column[y - colymin] = -30;
            continue;
        }
        double noise = sampleSurfaceNoiseBetween(sn, x, y, z, -128, +128);
        double clamped = noise + depth;
        clamped = lerp(upper_drop[y], -3000, clamped);
        clamped = lerp(lower_drop[y], -30, clamped);
        column[y - colymin] = clamped;
    }
}

/* The engine's getSurfaceHeight (not exported by its headers). */
static int end_surface_height(
        const double ncol00[], const double ncol01[],
        const double ncol10[], const double ncol11[],
        int colymin, int colymax, int blockspercell, double dx, double dz)
{
    int y, celly;
    for (celly = colymax-1; celly >= colymin; celly--) {
        int idx = celly - colymin;
        double v000 = ncol00[idx];
        double v001 = ncol01[idx];
        double v100 = ncol10[idx];
        double v101 = ncol11[idx];
        double v010 = ncol00[idx+1];
        double v011 = ncol01[idx+1];
        double v110 = ncol10[idx+1];
        double v111 = ncol11[idx+1];
        for (y = blockspercell - 1; y >= 0; y--) {
            double dy = y / (double) blockspercell;
            double noise = lerp3(dy, dx, dz,
                v000, v010, v100, v110,
                v001, v011, v101, v111);
            if (noise > 0)
                return celly * blockspercell + y;
        }
    }
    return 0;
}

/* mapEndSurfaceHeight(y, en, sn, x, z, w, h, scale, 0) on a shared island grid. */
static int32_t end_surface_area(float *out, const EndNoise *en, const SurfaceNoise *sn,
                                int x, int z, int w, int h, int scale)
{
    end_island_grid grid;
    int y0 = 2, y1 = 18;
    int yn = y1 - y0 + 1;
    double cellmid = scale > 1 ? scale / 16.0 : 0;
    int cellsiz = 8 / scale;
    int cx = floor_div(x, cellsiz);
    int cz = floor_div(z, cellsiz);
    int cw = floor_div(x + w - 1, cellsiz) - cx + 2;
    int i, j;
    double *buf, *ncol[2];

    if (!end_island_grid_init(&grid, en, cx, cz, cx + cw - 1, floor_div(z + h - 1, cellsiz) + 1))
        return SAX_ERR_ALLOCATION;
    buf = (double *) malloc(sizeof(double) * yn * cw * 2);
    if (!buf) {
        free(grid.weights);
        return SAX_ERR_ALLOCATION;
    }
    ncol[0] = buf;
    ncol[1] = buf + yn * cw;
    for (i = 0; i < cw; i++)
        end_noise_column(ncol[1]+i*yn, sn, en, &grid, cx+i, cz+0, y0, y1);
    for (j = 0; j < h; j++) {
        int cj = floor_div(z + j, cellsiz);
        int dj = z + j - cj * cellsiz;
        if (j == 0 || dj == 0) {
            double *tmp = ncol[0];
            ncol[0] = ncol[1];
            ncol[1] = tmp;
            for (i = 0; i < cw; i++)
                end_noise_column(ncol[1]+i*yn, sn, en, &grid, cx+i, cj+1, y0, y1);
        }
        for (i = 0; i < w; i++) {
            int ci = floor_div(x + i, cellsiz);
            int di = x + i - ci * cellsiz;
            double dx = di / (double) cellsiz + cellmid;
            double dz = dj / (double) cellsiz + cellmid;
            double *ncol0 = ncol[0] + (ci - cx) * yn;
            double *ncol1 = ncol[1] + (ci - cx) * yn;
            out[j*w+i] = end_surface_height(ncol0, ncol1, ncol0+yn, ncol1+yn,
                y0, y1, 4, dx, dz);
        }
    }
    free(buf);
    free(grid.weights);
    return SAX_OK;
}

SAX_API int32_t sax_height_area_sampled(
    const sax_context *context, int32_t dimension,
    int32_t origin_block_x, int32_t origin_block_z,
    int32_t sample_step, int32_t sample_width, int32_t sample_height,
    float *out_heights, int32_t output_capacity)
{
    const BiomeNoise *bn;
    int64_t sample_count, last_x, last_z;
    int32_t x, z;

    if (!out_heights || !sample_step_valid(sample_step) ||
        sample_width <= 0 || sample_height <= 0 || output_capacity < 0 ||
        !context_valid(context) || !dimension_valid(dimension))
        return SAX_ERR_ARGUMENT;
    if (dimension == SAX_DIM_NETHER || context->mc < SAX_TERRAIN_MC)
        return SAX_ERR_UNSUPPORTED;
    sample_count = (int64_t) sample_width * sample_height;
    if (sample_count > SAX_MAX_BIOME_AREA_SAMPLES)
        return SAX_ERR_RANGE_TOO_LARGE;
    if (output_capacity < sample_count)
        return SAX_ERR_ARGUMENT;
    last_x = (int64_t) origin_block_x + (int64_t) sample_width * sample_step - 1;
    last_z = (int64_t) origin_block_z + (int64_t) sample_height * sample_step - 1;
    if (last_x > INT32_MAX || last_z > INT32_MAX)
        return SAX_ERR_RANGE_TOO_LARGE;
    if (dimension == SAX_DIM_END) {
        /* Exact End density terrain from the engine; it only supports 1..8
           block samples on step-aligned origins. 0 means void. */
        if (sample_step > 8)
            return SAX_ERR_UNSUPPORTED;
        if (origin_block_x % sample_step != 0 ||
            origin_block_z % sample_step != 0)
            return SAX_ERR_ARGUMENT;
        return end_surface_area(out_heights,
            &context->biome_generators[2].en, &context->end_height_noise,
            origin_block_x / sample_step, origin_block_z / sample_step,
            sample_width, sample_height, sample_step);
    }
    bn = &context->biome_generators[0].bn;

    if (sample_step >= 4) {
        for (z = 0; z < sample_height; ++z) {
            int64_t bz = (int64_t) origin_block_z + (int64_t) z * sample_step +
                         sample_step / 2;
            for (x = 0; x < sample_width; ++x) {
                int64_t bx = (int64_t) origin_block_x +
                             (int64_t) x * sample_step + sample_step / 2;
                out_heights[(int64_t) z * sample_width + x] = height_at_quart(
                    bn, (int32_t) floor_div64(bx, 4), (int32_t) floor_div64(bz, 4));
            }
        }
        return SAX_OK;
    }
    else {
        /* Cell q covers blocks 4q..4q+3 with its centre at 4q+2. Positions are
           kept in eighths of a cell: p = 2*block + step - 4 is the cell-centre
           relative position of the sample centre. */
        int64_t px0 = 2LL * origin_block_x + sample_step - 4;
        int64_t pz0 = 2LL * origin_block_z + sample_step - 4;
        int64_t qx0 = floor_div64(px0, 8);
        int64_t qz0 = floor_div64(pz0, 8);
        int64_t qx1 = floor_div64(px0 + 2LL * (sample_width - 1) * sample_step, 8) + 1;
        int64_t qz1 = floor_div64(pz0 + 2LL * (sample_height - 1) * sample_step, 8) + 1;
        int64_t lw = qx1 - qx0 + 1, lh = qz1 - qz0 + 1;
        int64_t i, j;
        float *lattice = (float *) malloc((size_t) (lw * lh) * sizeof(*lattice));
        if (!lattice)
            return SAX_ERR_ALLOCATION;
        for (j = 0; j < lh; ++j)
            for (i = 0; i < lw; ++i)
                lattice[j * lw + i] = height_at_quart(
                    bn, (int32_t) (qx0 + i), (int32_t) (qz0 + j));
        for (z = 0; z < sample_height; ++z) {
            int64_t pz = pz0 + 2LL * z * sample_step;
            int64_t cz = floor_div64(pz, 8) - qz0;
            float tz = (float) (pz - (cz + qz0) * 8) / 8.0F;
            for (x = 0; x < sample_width; ++x) {
                int64_t px = px0 + 2LL * x * sample_step;
                int64_t cx = floor_div64(px, 8) - qx0;
                float tx = (float) (px - (cx + qx0) * 8) / 8.0F;
                float a = lattice[cz * lw + cx], b = lattice[cz * lw + cx + 1];
                float c = lattice[(cz + 1) * lw + cx];
                float d = lattice[(cz + 1) * lw + cx + 1];
                float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
                out_heights[(int64_t) z * sample_width + x] =
                    top + (bottom - top) * tz;
            }
        }
        free(lattice);
        return SAX_OK;
    }
}

/* Vanilla noise_settings/nether final_density at a noise-cell corner:
   0.64 * (2.5 + ymul * (-2.5 + 0.9375 + yhigh * (base - 0.9375))), where
   base is old_blended_noise (the engine value divided by 128). Solid when > 0. */
static double nether_density(double base, int y)
{
    double ymul = (y + 8) / 32.0;
    double yhigh = (128 - y) / 24.0;
    ymul = ymul < 0.0 ? 0.0 : (ymul > 1.0 ? 1.0 : ymul);
    yhigh = yhigh < 0.0 ? 0.0 : (yhigh > 1.0 ? 1.0 : yhigh);
    return 0.64 * (2.5 + ymul * (-2.5 + 0.9375 + yhigh * (base - 0.9375)));
}

/* old_blended_noise the way vanilla's BlendedNoise evaluates it: the main noise
   picks the blend first, so a limit noise that a saturated blend ignores is never
   sampled. Same sums in the same order as the engine's sampleSurfaceNoise, so the
   result is bit-identical while skipping up to 16 of 40 octaves. */
static double nether_noise(const SurfaceNoise *sn, int x, int y, int z)
{
    double xzScale = 684.412 * sn->xzScale;
    double yScale = 684.412 * sn->yScale;
    double xzStep = xzScale / sn->xzFactor;
    double yStep = yScale / sn->yFactor;
    double minNoise = 0, maxNoise = 0, mainNoise = 0, blend;
    double persist = 1.0, contrib = 1.0;
    int i;
    for (i = 0; i < 8; i++) {
        double sy = yStep * persist;
        mainNoise += samplePerlin(&sn->octmain.octaves[i], x * xzStep * persist,
            y * yStep * persist, z * xzStep * persist, sy, y * sy) * contrib;
        persist *= 0.5;
        contrib *= 2.0;
    }
    blend = 0.5 + 0.05 * mainNoise;
    persist = 1.0;
    contrib = 1.0;
    for (i = 0; i < 16; i++) {
        double dx = x * xzScale * persist, dy = y * yScale * persist, dz = z * xzScale * persist;
        double sy = yScale * persist;
        if (blend < 1)
            minNoise += samplePerlin(&sn->octmin.octaves[i], dx, dy, dz, sy, y * sy) * contrib;
        if (blend > 0)
            maxNoise += samplePerlin(&sn->octmax.octaves[i], dx, dy, dz, sy, y * sy) * contrib;
        persist *= 0.5;
        contrib *= 2.0;
    }
    return clampedLerp(blend, minNoise / 512.0, maxNoise / 512.0);
}

/* Noise-cell corner densities, sampled on first use: a floor scan only needs the
   cells from its start Y down to the floor, usually about half of the column. */
typedef struct nether_lattice {
    const SurfaceNoise *noise;
    double *values; /* NaN until sampled; stride doubles per corner column */
    int32_t cx0, cz0;
    int64_t width, stride;
} nether_lattice;

static double nether_corner(const nether_lattice *lattice, int64_t i, int64_t j, int k)
{
    double *value = lattice->values + (j * lattice->width + i) * lattice->stride + k;
    if (isnan(*value))
        *value = nether_density(nether_noise(lattice->noise, (int) (lattice->cx0 + i), k,
            (int) (lattice->cz0 + j)) / 128.0, k * 8);
    return *value;
}

/* One sample's density column: its own noise cell, or a bilinear blend of four
   lattice corners. Values are filled in as the scan reaches them. */
typedef struct nether_column {
    const nether_lattice *lattice;
    const SurfaceNoise *noise;
    int64_t i, j; /* lattice corner of an interpolated column */
    int32_t cx, cz; /* noise cell of a direct column */
    double tx, tz;
    int direct;
    double values[17];
} nether_column;

static double nether_column_at(nether_column *column, int k)
{
    double *value = &column->values[k];
    if (isnan(*value)) {
        if (column->direct) {
            *value = nether_density(nether_noise(column->noise, column->cx, k, column->cz) / 128.0, k * 8);
        } else {
            const nether_lattice *l = column->lattice;
            double a = nether_corner(l, column->i, column->j, k);
            double b = nether_corner(l, column->i + 1, column->j, k);
            double c = nether_corner(l, column->i, column->j + 1, k);
            double d = nether_corner(l, column->i + 1, column->j + 1, k);
            double top = a + (b - a) * column->tx;
            double bottom = c + (d - c) * column->tx;
            *value = top + (bottom - top) * column->tz;
        }
    }
    return *value;
}

static int nether_solid(nether_column *column, int y)
{
    int j = y >> 3;
    double low = nether_column_at(column, j);
    return low + (nether_column_at(column, j + 1) - low) * ((y & 7) / 8.0) > 0.0;
}

static int nether_occupied(nether_column *column, int y, const uint64_t *carved)
{
    return nether_solid(column, y) && (!carved || !(carved[y >> 6] & (UINT64_C(1) << (y & 63))));
}

/* Leave the roof, then select the first solid floor, including carver-created pockets. */
static float nether_scan(nether_column *column, int ystart, const uint64_t *carved)
{
    int y = ystart;
    int k;
    for (k = 0; k < 17; ++k)
        column->values[k] = NAN;
    while (y > 0 && nether_occupied(column, y, carved))
        --y;
    while (y > 0 && !nether_occupied(column, y, carved))
        --y;
    return (float) y;
}
static int32_t nether_floor_area(
    const sax_context *context, int32_t origin_block_x, int32_t origin_block_z,
    int32_t y, int32_t sample_step, int32_t sample_width, int32_t sample_height,
    const uint64_t *carved, int32_t carved_capacity,
    float *out_heights, int32_t output_capacity)
{
    int64_t sample_count, last_x, last_z;
    int32_t x, z;
    int ystart, cells;
    double off;
    nether_column column;

    if (!out_heights || !sample_step_valid(sample_step) ||
        sample_width <= 0 || sample_height <= 0 || output_capacity < 0 ||
        !context_valid(context))
        return SAX_ERR_ARGUMENT;
    if (sample_step > 16 || context->mc < SAX_TERRAIN_MC)
        return SAX_ERR_UNSUPPORTED;
    sample_count = (int64_t) sample_width * sample_height;
    if (sample_count > SAX_MAX_BIOME_AREA_SAMPLES)
        return SAX_ERR_RANGE_TOO_LARGE;
    if (output_capacity < sample_count)
        return SAX_ERR_ARGUMENT;
    if (carved && carved_capacity < sample_count * 2)
        return SAX_ERR_ARGUMENT;
    last_x = (int64_t) origin_block_x + (int64_t) sample_width * sample_step;
    last_z = (int64_t) origin_block_z + (int64_t) sample_height * sample_step;
    if (last_x > INT32_MAX - 8 || last_z > INT32_MAX - 8 ||
        origin_block_x < INT32_MIN + 8 || origin_block_z < INT32_MIN + 8)
        return SAX_ERR_RANGE_TOO_LARGE;
    ystart = y < 1 ? 1 : (y > 127 ? 127 : y);
    cells = (ystart >> 3) + 1;
    off = (sample_step - 1) / 2.0; /* block offset of the sample centre */
    memset(&column, 0, sizeof(column));
    column.noise = &context->nether_noise;

    if (sample_step >= 8) {
        /* One column per sample at the nearest noise cell, no interpolation. */
        column.direct = 1;
        for (z = 0; z < sample_height; ++z) {
            int64_t bz = (int64_t) origin_block_z + (int64_t) z * sample_step + sample_step / 2;
            for (x = 0; x < sample_width; ++x) {
                int64_t bx = (int64_t) origin_block_x + (int64_t) x * sample_step + sample_step / 2;
                int64_t index = (int64_t) z * sample_width + x;
                column.cx = (int32_t) floor_div64(bx + 2, 4);
                column.cz = (int32_t) floor_div64(bz + 2, 4);
                out_heights[index] = nether_scan(&column, ystart, carved ? carved + index * 2 : NULL);
            }
        }
        return SAX_OK;
    }
    else {
        nether_lattice lattice;
        int32_t cx1, cz1;
        int64_t lh, i, count;
        lattice.noise = &context->nether_noise;
        lattice.cx0 = (int32_t) floor_div64(origin_block_x, 4);
        lattice.cz0 = (int32_t) floor_div64(origin_block_z, 4);
        cx1 = (int32_t) floor_div64(origin_block_x + (int64_t) (sample_width - 1) * sample_step + (int64_t) ceil(off), 4) + 1;
        cz1 = (int32_t) floor_div64(origin_block_z + (int64_t) (sample_height - 1) * sample_step + (int64_t) ceil(off), 4) + 1;
        lattice.width = (int64_t) cx1 - lattice.cx0 + 1;
        lh = (int64_t) cz1 - lattice.cz0 + 1;
        lattice.stride = cells + 1;
        count = lattice.width * lh * lattice.stride;
        lattice.values = (double *) malloc((size_t) count * sizeof(*lattice.values));
        if (!lattice.values)
            return SAX_ERR_ALLOCATION;
        for (i = 0; i < count; ++i)
            lattice.values[i] = NAN;
        column.lattice = &lattice;
        for (z = 0; z < sample_height; ++z) {
            double bz = (double) origin_block_z + (double) z * sample_step + off;
            column.j = (int64_t) floor(bz / 4.0) - lattice.cz0;
            column.tz = bz / 4.0 - floor(bz / 4.0);
            for (x = 0; x < sample_width; ++x) {
                double bx = (double) origin_block_x + (double) x * sample_step + off;
                int64_t index = (int64_t) z * sample_width + x;
                column.i = (int64_t) floor(bx / 4.0) - lattice.cx0;
                column.tx = bx / 4.0 - floor(bx / 4.0);
                out_heights[index] = nether_scan(&column, ystart, carved ? carved + index * 2 : NULL);
            }
        }
        free(lattice.values);
        return SAX_OK;
    }
}
SAX_API int32_t sax_nether_floor_area_sampled(
    const sax_context *context, int32_t origin_block_x, int32_t origin_block_z,
    int32_t y, int32_t sample_step, int32_t sample_width, int32_t sample_height,
    float *out_heights, int32_t output_capacity)
{
    return nether_floor_area(context, origin_block_x, origin_block_z, y, sample_step,
        sample_width, sample_height, NULL, 0, out_heights, output_capacity);
}

SAX_API int32_t sax_nether_floor_area_carved_sampled(
    const sax_context *context, int32_t origin_block_x, int32_t origin_block_z,
    int32_t y, int32_t sample_step, int32_t sample_width, int32_t sample_height,
    const uint64_t *carved, int32_t carved_capacity,
    float *out_heights, int32_t output_capacity)
{
    if (!carved || carved_capacity < 0)
        return SAX_ERR_ARGUMENT;
    return nether_floor_area(context, origin_block_x, origin_block_z, y, sample_step,
        sample_width, sample_height, carved, carved_capacity, out_heights, output_capacity);
}
SAX_API int32_t sax_biome_name(int32_t biome_id, char *out, int32_t capacity)
{
    if (biome_id < 0 || biome_id >= 256)
        return 0;
    return copy_string(biome2str(MC_NEWEST, biome_id), out, capacity);
}

SAX_API int32_t sax_context_biome_name(const sax_context *context, int32_t biome_id,
                                       char *out, int32_t capacity)
{
    if (!context_valid(context) || biome_id < 0 || biome_id >= 256 ||
        !biomeExists(context->mc, biome_id))
        return 0;
    return copy_string(biome2str(context->mc, biome_id), out, capacity);
}

SAX_API uint32_t sax_context_biome_color(const sax_context *context, int32_t biome_id)
{
    return context_biome_color(context, biome_id);
}

SAX_API int32_t sax_context_has_terrain(const sax_context *context)
{
    return context_valid(context) && context->mc >= SAX_TERRAIN_MC;
}

SAX_API uint32_t sax_biome_color(int32_t biome_id)
{
    unsigned char colors[256][3];
    initBiomeColors(colors);
    return color_from_table(colors, MC_NEWEST, biome_id);
}

static Xoroshiro ore_random_at(Xoroshiro base, int x, int y, int z)
{
    uint64_t value = (uint64_t) ((int64_t) x * INT64_C(3129871)) ^
                     (uint64_t) ((int64_t) z * INT64_C(116129781)) ^
                     (uint64_t) (int64_t) y;
    int64_t shifted;
    /* Java long overflow is defined modulo 2^64; keep that behavior in C. */
    value = value * value * UINT64_C(42317861) + value * UINT64_C(11);
    shifted = (int64_t) value;
    shifted >>= 16;
    base.lo ^= (uint64_t) shifted;
    return base;
}

static double clamped_map(double value, double in0, double in1,
                          double out0, double out1)
{
    double t = (value - in0) / (in1 - in0);
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    return out0 + t * (out1 - out0);
}

static int init_ore_noise(sax_ore_noise *noise, uint64_t seed)
{
    static const uint64_t md5_ore[2] = {
        UINT64_C(0x9b88124de600116d), UINT64_C(0x2ae68055aa4a7761)};
    static const uint64_t md5_veininess[2] = {
        UINT64_C(0x6b86c7820a307171), UINT64_C(0xd87fb0fefd9c1624)};
    static const uint64_t md5_vein_a[2] = {
        UINT64_C(0x4cd8d69b9a841649), UINT64_C(0xcdd63f17bfe8f5ed)};
    static const uint64_t md5_vein_b[2] = {
        UINT64_C(0x6b26220b31f7c6c9), UINT64_C(0xae077edebf6aaec1)};
    static const uint64_t md5_gap[2] = {
        UINT64_C(0x9c4cc6b2fb0be4bb), UINT64_C(0xbd5964705573bb5e)};
    static const double amplitude[] = {1.0};
    Xoroshiro world;
    Xoroshiro ore;
    Xoroshiro random;
    uint64_t lo;
    uint64_t hi;
    int used = 0;

    xSetSeed(&world, seed);
    lo = xNextLong(&world);
    hi = xNextLong(&world);
    ore.lo = lo ^ md5_ore[0];
    ore.hi = hi ^ md5_ore[1];
    noise->positional.lo = xNextLong(&ore);
    noise->positional.hi = xNextLong(&ore);

    random.lo = lo ^ md5_veininess[0];
    random.hi = hi ^ md5_veininess[1];
    used += xDoublePerlinInit(&noise->veininess, &random, noise->oct + used,
                              amplitude, -8, 1, -1);
    random.lo = lo ^ md5_vein_a[0];
    random.hi = hi ^ md5_vein_a[1];
    used += xDoublePerlinInit(&noise->vein_a, &random, noise->oct + used,
                              amplitude, -7, 1, -1);
    random.lo = lo ^ md5_vein_b[0];
    random.hi = hi ^ md5_vein_b[1];
    used += xDoublePerlinInit(&noise->vein_b, &random, noise->oct + used,
                              amplitude, -7, 1, -1);
    random.lo = lo ^ md5_gap[0];
    random.hi = hi ^ md5_gap[1];
    used += xDoublePerlinInit(&noise->gap, &random, noise->oct + used,
                              amplitude, -5, 1, -1);
    return used <= 8;
}

static int ore_vein_at(int x, int y, int z, sax_ore_noise *noise)
{
    double ridge = fmax(fabs(sampleDoublePerlin(&noise->vein_a,
                                                4.0*x, 4.0*y, 4.0*z)),
                        fabs(sampleDoublePerlin(&noise->vein_b,
                                                4.0*x, 4.0*y, 4.0*z)));
    double toggle;
    double density;
    int copper;
    int min_y;
    int max_y;
    int edge;
    Xoroshiro random;
    if (ridge >= 0.08)
        return 0;
    toggle = sampleDoublePerlin(&noise->veininess, 1.5*x, 1.5*y, 1.5*z);
    copper = toggle > 0.0;
    min_y = copper ? 0 : -60;
    max_y = copper ? 50 : -8;
    if (y < min_y || y > max_y)
        return 0;
    density = fabs(toggle);
    edge = (max_y - y < y - min_y) ? max_y - y : y - min_y;
    if (density + clamped_map(edge, 0, 20, -0.2, 0.0) < 0.4)
        return 0;
    random = ore_random_at(noise->positional, x, y, z);
    if (xNextFloat(&random) > 0.7f)
        return 0;
    (void) sampleDoublePerlin(&noise->gap, x, y, z);
    return copper ? SAX_ORE_VEIN_COPPER : SAX_ORE_VEIN_IRON;
}

static int32_t scan_ore_veins(const sax_context *context, int32_t requested_type,
                              int32_t min_x, int32_t min_z,
                              int32_t max_x, int32_t max_z,
                              int32_t *out, int32_t capacity)
{
    const int marker_size = 128;
    sax_ore_noise noise;
    int32_t mx0 = floor_div(min_x, marker_size);
    int32_t mz0 = floor_div(min_z, marker_size);
    int32_t mx1 = floor_div(max_x, marker_size);
    int32_t mz1 = floor_div(max_z, marker_size);
    int32_t count = 0;
    int32_t mx, mz;
    if (range_too_large(mx0, mz0, mx1, mz1))
        return SAX_ERR_RANGE_TOO_LARGE;
    if (context->mc < SAX_TERRAIN_MC)
        return 0;
    if (!init_ore_noise(&noise, context->seed))
        return SAX_ERR_GENERATION;

    for (mz = mz0; mz <= mz1; ++mz) {
        for (mx = mx0; mx <= mx1; ++mx) {
            int found = 0;
            int cz, cx;
            for (cz = 0; cz < 8 && !found; ++cz) {
                for (cx = 0; cx < 8 && !found; ++cx) {
                    int64_t bx64 = (int64_t) mx * marker_size + cx * 16;
                    int64_t bz64 = (int64_t) mz * marker_size + cz * 16;
                    int bx, bz, x, z, y, type;
                    Xoroshiro pick;
                    if (bx64 < INT_MIN || bx64 > INT_MAX ||
                        bz64 < INT_MIN || bz64 > INT_MAX ||
                        bx64 > INT_MAX - 15 || bz64 > INT_MAX - 15)
                        continue;
                    bx = (int) bx64;
                    bz = (int) bz64;
                    pick = ore_random_at(noise.positional, bx, 0, bz);
                    x = bx + xNextInt(&pick, 16);
                    z = bz + xNextInt(&pick, 16);
                    for (y = -60; y <= 50; y += 4) {
                        type = ore_vein_at(x, y, z, &noise);
                        if (!type || (requested_type != SAX_ORE_VEIN &&
                                      type != requested_type))
                            continue;
                        found = 1;
                        if (x < min_x || x > max_x || z < min_z || z > max_z)
                            break;
                        if (!append_result(out, capacity, &count, type, x, y, z,
                                           SAX_RESULT_APPROXIMATE, 0))
                            return capacity + 1;
                        break;
                    }
                }
            }
        }
    }
    return count;
}

static int end_city_has_ship(uint64_t seed, Pos position)
{
    Piece pieces[END_CITY_PIECES_MAX];
    int count = getEndCityPieces(pieces, seed, position.x >> 4, position.z >> 4);
    int i;
    for (i = 0; i < count; ++i) {
        if (pieces[i].type == END_SHIP)
            return 1;
    }
    return 0;
}

static int structure_needs_unavailable_terrain(int structure_type)
{
    switch (structure_type) {
    case Desert_Pyramid:
    case Jungle_Pyramid:
    case Mansion:
    case Desert_Well:
    case Geode:
    case Abandoned_Camp: /* Biome check uses approximate projected surface Y. */
        return 1;
    default:
        return 0;
    }
}

static int32_t scan_end_islands(const sax_context *context,
                                int32_t min_x, int32_t min_z,
                                int32_t max_x, int32_t max_z,
                                int32_t check_biome,
                                int32_t *out_results, int32_t capacity)
{
    const Generator *end_generator =
        context_biome_generator(context, SAX_DIM_END);
    int32_t chunk_x0 = floor_div(min_x, 16);
    int32_t chunk_z0 = floor_div(min_z, 16);
    int32_t chunk_x1 = floor_div(max_x, 16);
    int32_t chunk_z1 = floor_div(max_z, 16);
    int32_t chunk_x, chunk_z;
    int32_t count = 0;

    if (!end_generator)
        return SAX_ERR_ARGUMENT;
    if (range_too_large(chunk_x0, chunk_z0, chunk_x1, chunk_z1))
        return SAX_ERR_RANGE_TOO_LARGE;

    for (chunk_z = chunk_z0; chunk_z <= chunk_z1; ++chunk_z) {
        for (chunk_x = chunk_x0; chunk_x <= chunk_x1; ++chunk_x) {
            EndIsland islands[2];
            int flags = 0;
            int island_count;
            int i;

            /* The placed feature is selected only by the small-end-islands
               biome. Its decorator RNG may otherwise produce a raw attempt
               in any End chunk. */
            if (check_biome) {
                int biome = getBiomeAt(end_generator, 16,
                                       chunk_x, 0, chunk_z);
                if (biome != small_end_islands)
                    continue;
                flags = SAX_RESULT_BIOME_CHECKED |
                        SAX_RESULT_BIOME_VIABLE;
            }

            /* getStructurePos(End_Island) exposes only the rarity draw and
               consumes the following RNG values in the wrong order. The
               dedicated helper reproduces Vanilla's one-or-two island draw,
               in-chunk X/Z, Y, and radius. */
            island_count = getEndIslands(islands, context->mc, context->seed,
                                         chunk_x, chunk_z);
            for (i = 0; i < island_count; ++i) {
                Pos position = {islands[i].x, islands[i].z};
                if (!in_box(position, min_x, min_z, max_x, max_z))
                    continue;
                if (!append_result(out_results, capacity, &count,
                                   SAX_END_ISLAND, islands[i].x,
                                   islands[i].y, islands[i].z, flags,
                                   islands[i].r))
                    return capacity + 1;
            }
        }
    }
    return count;
}

SAX_API int32_t sax_scan_structures(const sax_context *context,
                                    int32_t structure_type,
                                    int32_t min_x, int32_t min_z,
                                    int32_t max_x, int32_t max_z,
                                    int32_t check_biome,
                                    int32_t *out_results,
                                    int32_t capacity)
{
    StructureConfig config;
    Generator generator;
    SurfaceNoise end_surface;
    int engine_type = structure_type;
    int output_type = structure_type;
    int32_t reg_x0, reg_z0, reg_x1, reg_z1;
    int32_t reg_x, reg_z;
    int32_t count = 0;
    int result;

    if (!context_valid(context) || !box_valid(min_x, min_z, max_x, max_z) ||
        capacity < 0 || (capacity > 0 && !out_results))
        return SAX_ERR_ARGUMENT;
    if (structure_type == SAX_ORE_VEIN ||
        structure_type == SAX_ORE_VEIN_COPPER ||
        structure_type == SAX_ORE_VEIN_IRON)
        return scan_ore_veins(context, structure_type, min_x, min_z,
                              max_x, max_z, out_results, capacity);
    if (structure_type == SAX_END_ISLAND)
        return scan_end_islands(context, min_x, min_z, max_x, max_z,
                                check_biome, out_results, capacity);
    if (structure_type == SAX_END_SHIP) {
        engine_type = End_City;
        output_type = SAX_END_SHIP;
    }
    if (engine_type <= Feature || engine_type >= FEATURE_NUM)
        return SAX_ERR_UNSUPPORTED;
    if (!getStructureConfig(engine_type, context->mc, &config)) {
        /* Known to the newest version but not generated in the selected one,
           e.g. Trial Chambers before 1.21: nothing to show. */
        if (getStructureConfig(engine_type, MC_NEWEST, &config))
            return 0;
        return SAX_ERR_UNSUPPORTED;
    }

    reg_x0 = floor_div(min_x, (int32_t) config.regionSize * 16);
    reg_z0 = floor_div(min_z, (int32_t) config.regionSize * 16);
    reg_x1 = floor_div(max_x, (int32_t) config.regionSize * 16);
    reg_z1 = floor_div(max_z, (int32_t) config.regionSize * 16);
    if (range_too_large(reg_x0, reg_z0, reg_x1, reg_z1))
        return SAX_ERR_RANGE_TOO_LARGE;
    if (check_biome) {
        result = setup_dimension_generator(context, config.dim, &generator);
        if (result != SAX_OK)
            return result;
        /* End Cities only generate when the island surface around the start
           chunk is high enough; the biome check alone accepts positions over
           small islands and the void between them. */
        if (engine_type == End_City)
            initSurfaceNoise(&end_surface, DIM_END, context->seed);
    }

    /* The 20 dragon-fight gateways are fixed starts in addition to the
       scattered End gateway decorator positions. */
    if (structure_type == SAX_END_GATEWAY) {
        Pos fixed[20];
        int i;
        getFixedEndGateways(context->mc, context->seed, fixed);
        for (i = 0; i < 20; ++i) {
            if (!in_box(fixed[i], min_x, min_z, max_x, max_z))
                continue;
            if (!append_result(out_results, capacity, &count, output_type,
                               fixed[i].x, SAX_UNKNOWN_Y, fixed[i].z,
                               SAX_RESULT_FIXED, i))
                return capacity + 1;
        }
    }

    for (reg_z = reg_z0; reg_z <= reg_z1; ++reg_z) {
        for (reg_x = reg_x0; reg_x <= reg_x1; ++reg_x) {
            Pos position;
            int flags = 0;
            int detail = engine_type == Abandoned_Camp ? -1 : 0;
            if (!getStructurePos(engine_type, context->mc, context->seed,
                                 reg_x, reg_z, &position) ||
                !in_box(position, min_x, min_z, max_x, max_z))
                continue;
            if (check_biome) {
                int biome = isViableStructurePos(engine_type, &generator,
                                                 position.x, position.z, 0);
                if (!biome)
                    continue;
                if (engine_type == Abandoned_Camp) {
                    StructureVariant variant;
                    detail = biome & SAX_CAMP_BIOME_MASK;
                    if (getVariant(&variant, engine_type, context->mc, context->seed,
                                   position.x, position.z, biome) && variant.special)
                        detail |= SAX_CAMP_SPECIAL_LOOT;
                }
                if (engine_type == End_City &&
                    !isViableEndCityTerrain(&generator, &end_surface,
                                            position.x, position.z))
                    continue;
                flags |= SAX_RESULT_BIOME_CHECKED | SAX_RESULT_BIOME_VIABLE;
            }
            /* Ship pieces are only meaningful for a city that generates, so
               run the (comparatively expensive) piece layout last. */
            if (output_type == SAX_END_SHIP &&
                !end_city_has_ship(context->seed, position))
                continue;
            /* Cubiomes has no exact Overworld WORLD_SURFACE_WG/block
               sampler. Keep these potential starts visible instead of using
               its heuristic terrain filter (which can hide real structures),
               but expose the uncertainty to the map tooltip. */
            if (structure_needs_unavailable_terrain(engine_type))
                flags |= SAX_RESULT_APPROXIMATE;
            if (!append_result(out_results, capacity, &count, output_type,
                               position.x, SAX_UNKNOWN_Y, position.z, flags, detail))
                return capacity + 1;
        }
    }
    return count;
}

SAX_API int32_t sax_scan_strongholds(const sax_context *context,
                                     int32_t min_x, int32_t min_z,
                                     int32_t max_x, int32_t max_z,
                                     int32_t check_biome,
                                     int32_t *out_results,
                                     int32_t capacity)
{
    StrongholdIter iterator;
    Generator generator;
    const Generator *generator_pointer = NULL;
    int32_t count = 0;
    int remaining;
    int result;
    if (!context_valid(context) || !box_valid(min_x, min_z, max_x, max_z) ||
        capacity < 0 || (capacity > 0 && !out_results))
        return SAX_ERR_ARGUMENT;
    /* Up to 1.19.2 the ring positions depend on the biome search itself, so the
       engine always needs the generator and every position is exact. */
    if (context->mc <= MC_1_19_2)
        check_biome = 1;
    if (check_biome) {
        result = setup_dimension_generator(context, SAX_DIM_OVERWORLD, &generator);
        if (result != SAX_OK)
            return result;
        generator_pointer = &generator;
    }
    initFirstStronghold(&iterator, context->mc, context->seed);
    do {
        int flags;
        remaining = nextStronghold(&iterator, generator_pointer);
        if (remaining <= 0)
            break;
        if (!in_box(iterator.pos, min_x, min_z, max_x, max_z))
            continue;
        flags = check_biome ? SAX_RESULT_BIOME_CHECKED | SAX_RESULT_BIOME_VIABLE
                            : SAX_RESULT_APPROXIMATE;
        if (!append_result(out_results, capacity, &count, SAX_STRONGHOLD,
                           iterator.pos.x, SAX_UNKNOWN_Y, iterator.pos.z,
                           flags, iterator.ringnum))
            return capacity + 1;
    } while (remaining > 0);
    return count;
}

SAX_API int32_t sax_spawn(const sax_context *context, int32_t detailed,
                          int32_t out_xyz[3])
{
    Generator generator;
    Pos position;
    int result;
    if (!out_xyz)
        return SAX_ERR_ARGUMENT;
    result = setup_dimension_generator(context, SAX_DIM_OVERWORLD, &generator);
    if (result != SAX_OK)
        return result;
    position = detailed ? getSpawn(&generator) : estimateSpawn(&generator, NULL);
    out_xyz[0] = position.x;
    out_xyz[1] = SAX_UNKNOWN_Y;
    out_xyz[2] = position.z;
    return SAX_OK;
}

SAX_API int32_t sax_scan_slime_chunks(const sax_context *context,
                                      int32_t min_x, int32_t min_z,
                                      int32_t max_x, int32_t max_z,
                                      int32_t *out_results,
                                      int32_t capacity)
{
    int32_t chunk_x0, chunk_z0, chunk_x1, chunk_z1;
    int32_t chunk_x, chunk_z;
    int32_t count = 0;
    if (!context_valid(context) || !box_valid(min_x, min_z, max_x, max_z) ||
        capacity < 0 || (capacity > 0 && !out_results))
        return SAX_ERR_ARGUMENT;
    chunk_x0 = floor_div(min_x, 16);
    chunk_z0 = floor_div(min_z, 16);
    chunk_x1 = floor_div(max_x, 16);
    chunk_z1 = floor_div(max_z, 16);
    if (range_too_large(chunk_x0, chunk_z0, chunk_x1, chunk_z1))
        return SAX_ERR_RANGE_TOO_LARGE;
    for (chunk_z = chunk_z0; chunk_z <= chunk_z1; ++chunk_z) {
        for (chunk_x = chunk_x0; chunk_x <= chunk_x1; ++chunk_x) {
            int64_t block_x;
            int64_t block_z;
            uint64_t random = context->seed;
            uint32_t cx = (uint32_t) chunk_x;
            uint32_t cz = (uint32_t) chunk_z;
            random += (uint64_t) (int64_t) (int32_t) (cx * UINT32_C(0x5ac0db));
            random += (uint64_t) (int64_t) (int32_t)
                (cx * cx * UINT32_C(0x4c1906));
            random += (uint64_t) (int64_t) (int32_t) (cz * UINT32_C(0x5f24f));
            random += (uint64_t) (int64_t) (int32_t) (cz * cz)
                * UINT64_C(0x4307a7);
            random ^= UINT64_C(0x3ad8025f);
            setSeed(&random, random);
            if (nextInt(&random, 10) != 0)
                continue;
            block_x = (int64_t) chunk_x * 16;
            block_z = (int64_t) chunk_z * 16;
            if (!append_result(out_results, capacity, &count, SAX_SLIME_CHUNK,
                               (int32_t) block_x, SAX_UNKNOWN_Y,
                               (int32_t) block_z, 0, 0))
                return capacity + 1;
        }
    }
    return count;
}
