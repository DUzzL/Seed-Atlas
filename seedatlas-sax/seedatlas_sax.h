#ifndef SEEDATLAS_SAX_H
#define SEEDATLAS_SAX_H

#include <stdint.h>

/* SAX_STATIC: linked statically (no export or import); SAX_BUILD (or the
 * older SEEDATLAS_XAERO_BUILD): building the shared library. */
#if defined(SAX_STATIC)
#  define SAX_API
#elif defined(_WIN32)
#  if defined(SAX_BUILD) || defined(SEEDATLAS_XAERO_BUILD)
#    define SAX_API __declspec(dllexport)
#  else
#    define SAX_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define SAX_API __attribute__((visibility("default")))
#else
#  define SAX_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable, allocation-free (apart from the opaque context) C ABI of the Seed
 * Atlas engine for the mods: the Fabric mod calls it through Java's FFM, the
 * Horizon mod (Meridian) through Rust's FFI. Keep primitive widths and output
 * array layouts stable. A context is read-only after sax_create*(), so one
 * context may be used from several threads at once.
 */
enum sax_error {
    SAX_OK = 0,
    SAX_ERR_ARGUMENT = -1,
    SAX_ERR_ALLOCATION = -2,
    SAX_ERR_UNSUPPORTED = -3,
    SAX_ERR_RANGE_TOO_LARGE = -4,
    SAX_ERR_GENERATION = -5
};

enum sax_capability {
    SAX_CAP_BIOMES = 1u << 0,
    SAX_CAP_STRUCTURES = 1u << 1,
    SAX_CAP_STRONGHOLDS = 1u << 2,
    SAX_CAP_SPAWN = 1u << 3,
    SAX_CAP_SLIME = 1u << 4,
    SAX_CAP_ORE_VEINS = 1u << 5,
    SAX_CAP_END_SHIPS = 1u << 6,
    SAX_CAP_BIOME_SAMPLING = 1u << 7,
    SAX_CAP_BIOME_AREAS = 1u << 8,
    SAX_CAP_HEIGHTS = 1u << 9,
    SAX_CAP_NETHER_FLOOR = 1u << 10,
    SAX_CAP_NETHER_CARVING = 1u << 11,
    SAX_CAP_MC_VERSIONS = 1u << 12
};

enum sax_dimension {
    SAX_DIM_NETHER = -1,
    SAX_DIM_OVERWORLD = 0,
    SAX_DIM_END = 1
};

enum sax_world_flags {
    SAX_WORLD_NORMAL = 0,
    SAX_WORLD_LARGE_BIOMES = 1u << 0
};

/* Engine StructureType values are retained verbatim for 1..25. */
enum sax_structure_type {
    SAX_DESERT_PYRAMID = 1,
    SAX_JUNGLE_TEMPLE = 2,
    SAX_SWAMP_HUT = 3,
    SAX_IGLOO = 4,
    SAX_VILLAGE = 5,
    SAX_OCEAN_RUIN = 6,
    SAX_SHIPWRECK = 7,
    SAX_MONUMENT = 8,
    SAX_MANSION = 9,
    SAX_OUTPOST = 10,
    SAX_RUINED_PORTAL = 11,
    SAX_RUINED_PORTAL_NETHER = 12,
    SAX_ANCIENT_CITY = 13,
    SAX_BURIED_TREASURE = 14,
    SAX_MINESHAFT = 15,
    SAX_DESERT_WELL = 16,
    SAX_AMETHYST_GEODE = 17,
    SAX_NETHER_FORTRESS = 18,
    SAX_BASTION = 19,
    SAX_END_CITY = 20,
    SAX_END_GATEWAY = 21,
    SAX_END_ISLAND = 22,
    SAX_TRAIL_RUINS = 23,
    SAX_TRIAL_CHAMBERS = 24,
    SAX_ABANDONED_CAMP = 25,

    /* Wrapper-only marker types. */
    SAX_STRONGHOLD = 1001,
    SAX_SPAWN = 1002,
    SAX_SLIME_CHUNK = 1003,
    SAX_ORE_VEIN_COPPER = 1004,
    SAX_ORE_VEIN_IRON = 1005,
    SAX_END_SHIP = 1006,
    SAX_ORE_VEIN = 1007
};

/* Camp detail is -1 for unchecked candidates, else biome ID | special loot. */
enum sax_camp_detail {
    SAX_CAMP_BIOME_MASK = 0xff,
    SAX_CAMP_SPECIAL_LOOT = 1u << 8
};

enum sax_result_flags {
    SAX_RESULT_BIOME_CHECKED = 1u << 0,
    SAX_RESULT_BIOME_VIABLE = 1u << 1,
    SAX_RESULT_APPROXIMATE = 1u << 2,
    SAX_RESULT_FIXED = 1u << 3
};

enum {
    SAX_ABI_VERSION = 6,
    SAX_CHUNK_SAMPLE_COUNT = 16 * 16,
    SAX_MAX_BIOME_AREA_SAMPLES = 4 * 1024 * 1024,
    SAX_RESULT_STRIDE = 6,
    SAX_UNKNOWN_Y = INT32_MIN
};

typedef struct sax_context sax_context;

SAX_API int32_t sax_abi_version(void);
SAX_API uint32_t sax_capabilities(void);

/* Copies a NUL-terminated version into out. Returns required bytes incl. NUL. */
SAX_API int32_t sax_engine_version(char *out, int32_t capacity);

/* Generates the newest supported Minecraft version. */
SAX_API sax_context *sax_create(int64_t seed, uint32_t world_flags);

/*
 * Additive ABI 6 capability (SAX_CAP_MC_VERSIONS): generates the Minecraft
 * version named like the Seed Atlas app does ("26.2", "1.21.4", "1.20" for the
 * latest 1.20.x). NULL selects the newest version. Returns NULL for versions
 * before 1.1 or names the engine does not know. Before 1.18 there are no
 * terrain heights, Nether floors or ore veins (SAX_ERR_UNSUPPORTED / no results).
 */
SAX_API sax_context *sax_create_versioned(int64_t seed, uint32_t world_flags,
                                          const char *mc_version);
/* 1 when sax_create_versioned accepts the version name, else 0. */
SAX_API int32_t sax_mc_version_supported(const char *mc_version);
SAX_API void sax_destroy(sax_context *context);

/* Returns SAX_OK and writes a biome id, or a negative sax_error. */
SAX_API int32_t sax_biome_at(const sax_context *context, int32_t dimension,
                            int32_t x, int32_t y, int32_t z,
                            int32_t *out_biome_id);

/*
 * Generates the 16x16 block plane belonging to (chunk_x, chunk_z).
 * Output is indexed as local_z * 16 + local_x. Either output may be NULL.
 * Colors are opaque ARGB (0xFFRRGGBB).
 */
SAX_API int32_t sax_biome_chunk(const sax_context *context, int32_t dimension,
                               int32_t chunk_x, int32_t chunk_z, int32_t y,
                               int32_t out_ids[SAX_CHUNK_SAMPLE_COUNT],
                               uint32_t out_argb[SAX_CHUNK_SAMPLE_COUNT]);

/*
 * Generates the same 16x16 output plane with reduced sampling density.
 * sample_step must be one of 1, 2, 4, 8, or 16. At step 1 this uses the
 * normal bulk generator. Larger steps sample the center block of each
 * step-sized cell and fill that complete cell with the sampled biome.
 */
SAX_API int32_t sax_biome_chunk_sampled(
    const sax_context *context, int32_t dimension,
    int32_t chunk_x, int32_t chunk_z, int32_t y, int32_t sample_step,
    int32_t out_ids[SAX_CHUNK_SAMPLE_COUNT],
    uint32_t out_argb[SAX_CHUNK_SAMPLE_COUNT]);

/*
 * Generates one compact, regularly sampled biome area for map rendering.
 * The block origin is the north-west corner of sample (0, 0). The output
 * contains sample_width * sample_height entries in row-major order and is
 * intentionally not expanded to block resolution. Each entry represents a
 * sample_step by sample_step block cell.
 *
 * sample_step must be a power of two in [1, 1024]. Origins for steps >= 4
 * must be aligned to sample_step, matching Seed Atlas' native map levels.
 * Steps 1 and 2 retain block-resolution/Voronoi semantics; steps >= 4 use
 * cubiomes' scaled map generator, like the Seed Atlas desktop renderer.
 * Either output may be NULL, but not both. output_capacity is measured in
 * entries and must cover sample_width * sample_height.
 */
SAX_API int32_t sax_biome_area_sampled(
    const sax_context *context, int32_t dimension,
    int32_t origin_block_x, int32_t origin_block_z, int32_t y,
    int32_t sample_step, int32_t sample_width, int32_t sample_height,
    int32_t *out_ids, uint32_t *out_argb, int32_t output_capacity);

/*
 * Approximate Overworld terrain height in blocks, for display shading only.
 * It is the 1.18+ continentalness/erosion/weirdness offset spline sampled on
 * the biome climate lattice, not the real surface: caves, overhangs, trees and
 * structures are not modelled and mountains are smoothed. Each entry is the
 * estimate at the centre of a sample_step x sample_step block cell; steps
 * below 4 interpolate between climate cells. Origins need no alignment.
 * The End returns the engine's density-based island heights in blocks (0 is
 * void) for steps 1..8 on step-aligned origins (SAX_ERR_UNSUPPORTED above 8,
 * SAX_ERR_ARGUMENT if unaligned). The Nether returns SAX_ERR_UNSUPPORTED here;
 * use sax_nether_floor_area_sampled.
 */
SAX_API int32_t sax_height_area_sampled(
    const sax_context *context, int32_t dimension,
    int32_t origin_block_x, int32_t origin_block_z,
    int32_t sample_step, int32_t sample_width, int32_t sample_height,
    float *out_heights, int32_t output_capacity);

/*
 * Vanilla Nether terrain on the noise-router final_density: the cave-mode
 * surface at layer y (clamped to 1..127). Pass 127 for a full cave mode map,
 * which starts at the roof, NOT the biome sampling Y. It skips any
 * rock at y, then returns the first solid block below the open space (0 when
 * there is none). A value below 31 is a seabed under the lava sea. Steps 1..4
 * interpolate noise cells and need no alignment; steps 8 and 16 sample the
 * nearest noise cell. Larger steps return SAX_ERR_UNSUPPORTED.
 */
SAX_API int32_t sax_nether_floor_area_sampled(
    const sax_context *context, int32_t origin_block_x, int32_t origin_block_z,
    int32_t y, int32_t sample_step, int32_t sample_width, int32_t sample_height,
    float *out_heights, int32_t output_capacity);

/* Additive ABI 6 capability: two uint64 words per sample, bit y marks carved air.
 * The mask uses the same sample centres/order as the floor output. Capacities
 * count words and float samples respectively; neither buffer is retained. */
SAX_API int32_t sax_nether_floor_area_carved_sampled(
    const sax_context *context, int32_t origin_block_x, int32_t origin_block_z,
    int32_t y, int32_t sample_step, int32_t sample_width, int32_t sample_height,
    const uint64_t *carved, int32_t carved_capacity,
    float *out_heights, int32_t output_capacity);
/* Returns required bytes including NUL; 0 means the id has no known name. */
SAX_API int32_t sax_biome_name(int32_t biome_id, char *out, int32_t capacity);
SAX_API uint32_t sax_biome_color(int32_t biome_id);
/* The same for the context's version, e.g. "mountains" before 1.18 (SAX_CAP_MC_VERSIONS). */
SAX_API int32_t sax_context_biome_name(const sax_context *context, int32_t biome_id,
                                       char *out, int32_t capacity);
SAX_API uint32_t sax_context_biome_color(const sax_context *context, int32_t biome_id);
/* 1 when the context's version has terrain heights and Nether floors (1.18+). */
SAX_API int32_t sax_context_has_terrain(const sax_context *context);

/*
 * Finds starts/markers whose X/Z coordinate is inside the inclusive block
 * bounding box. When check_biome != 0, non-viable candidates are omitted.
 *
 * out_results contains SAX_RESULT_STRIDE int32 values per result:
 *   [type, block_x, block_y_or_SAX_UNKNOWN_Y, block_z, flags, detail]
 * End Island results include their generated Y and use detail for the radius.
 * Camp detail uses sax_camp_detail; unchecked camp detail is -1.
 *
 * Returns the number written, capacity + 1 when truncated, or sax_error.
 * A structure the context's version does not generate yields 0 results.
 */
SAX_API int32_t sax_scan_structures(const sax_context *context,
                                    int32_t structure_type,
                                    int32_t min_x, int32_t min_z,
                                    int32_t max_x, int32_t max_z,
                                    int32_t check_biome,
                                    int32_t *out_results,
                                    int32_t capacity);

/* Exact biome-relocated locations when check_biome != 0, ring estimates else. */
SAX_API int32_t sax_scan_strongholds(const sax_context *context,
                                     int32_t min_x, int32_t min_z,
                                     int32_t max_x, int32_t max_z,
                                     int32_t check_biome,
                                     int32_t *out_results,
                                     int32_t capacity);

/* detailed != 0 runs the slower biome search (still cannot validate grass). */
SAX_API int32_t sax_spawn(const sax_context *context, int32_t detailed,
                          int32_t out_xyz[3]);

SAX_API int32_t sax_scan_slime_chunks(const sax_context *context,
                                      int32_t min_x, int32_t min_z,
                                      int32_t max_x, int32_t max_z,
                                      int32_t *out_results,
                                      int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
