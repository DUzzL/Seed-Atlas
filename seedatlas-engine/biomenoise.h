#ifndef BIOMENOISE_H_
#define BIOMENOISE_H_

#include "noise.h"
#include "layers.h"


STRUCT(Range)
{
    // Defines an area or volume for the biome generation. It is given by a
    // position, size and scaling in the horizontal axes, and an optional
    // vertical range. The vertical scaling is equal to 1:1 iff scale == 1,
    // and 1:4 (default biome scale) in all other cases!
    //
    // @scale:  Horizontal scale factor, should be one of 1, 4, 16, 64, or 256
    //          additionally a value of zero bypasses scaling and expects a
    //          manual generation entry layer.
    // @x,z:    Horizontal position, i.e. coordinates of north-west corner.
    // @sx,sz:  Horizontal size (width and height for 2D), should be positive.
    // @y       Vertical position, 1:1 iff scale==1, 1:4 otherwise.
    // @sy      Vertical size. Values <= 0 are treated equivalent to 1.
    //
    // Volumes generated with a range are generally indexed as:
    //  out [ i_y*sx*sz + i_z*sx + i_x ]
    // where i_x, i_y, i_z are indecies in their respective directions.
    //
    // EXAMPLES
    // Area at normal biome scale (1:4):
    //  Range r_2d = {4, x,z, sx,sz};
    // (C99 syntax allows ommission of the trailing zero-initialization.)
    //
    // Area at block scale (1:1) at sea level:
    //  Range r_surf = {1, x,z, sx,sz, 63};
    // (Block level scale uses voronoi sampling with 1:1 vertical scaling.)
    //
    // Area at chunk scale (1:16) near sea level:
    //  Range r_surf16 = {16, x,z, sx,sz, 15};
    // (Note that the vertical scaling is always 1:4 for non-voronoi scales.)
    //
    // Volume at scale (1:4):
    //  Range r_vol = {4, x,z, sx,sz, y,sy};

    int scale;
    int x, z, sx, sz;
    int y, sy;
};


// Nether biome generator 1.16+
STRUCT(NetherNoise)
{   // altitude and wierdness don't affect nether biomes
    // and the weight is a 5th noise parameter which is constant
    DoublePerlinNoise temperature;
    DoublePerlinNoise humidity;
    PerlinNoise oct[8]; // buffer for octaves in double perlin noise
};

// End biome generator 1.9+
STRUCT(EndNoise)
{
    PerlinNoise perlin;
    int mc;
};

STRUCT(SurfaceNoise)
{
    double xzScale, yScale;
    double xzFactor, yFactor;
    OctaveNoise octmin;
    OctaveNoise octmax;
    OctaveNoise octmain;
    OctaveNoise octsurf;
    OctaveNoise octdepth;
    PerlinNoise oct[16+16+8+4+16];
};

STRUCT(SurfaceNoiseBeta)
{
    OctaveNoise octmin;
    OctaveNoise octmax;
    OctaveNoise octmain;
    OctaveNoise octcontA;
    OctaveNoise octcontB;
    PerlinNoise oct[16+16+8+10+16];
};

STRUCT(SeaLevelColumnNoiseBeta)
{
    double contASample;
    double contBSample;
    double minSample[2];
    double maxSample[2];
    double mainSample[2];
};

STRUCT(Spline)
{
    int len, typ;
    float loc[12];
    float der[12];
    Spline *val[12];
};

STRUCT(FixSpline)
{
    int len;
    float val;
};

STRUCT(SplineStack)
{   // the stack size here is just sufficient for overworld generation
    Spline stack[42];
    FixSpline fstack[151];
    int len, flen;
    int amplified; // spline values get the transform of the Amplified world type
};


enum
{
    NP_TEMPERATURE      = 0,
    NP_HUMIDITY         = 1,
    NP_CONTINENTALNESS  = 2,
    NP_EROSION          = 3,
    NP_SHIFT            = 4, NP_DEPTH = NP_SHIFT, // not a real climate
    NP_WEIRDNESS        = 5,
    NP_MAX
};
// Overworld biome generator for 1.18+
STRUCT(BiomeNoise)
{
    DoublePerlinNoise climate[NP_MAX];
    PerlinNoise oct[2*23]; // buffer for octaves in double perlin noise
    Spline *sp;
    SplineStack ss;
    int nptype;
    int mc;
    int amplified; // world type Amplified: changes the depth, not the 2D climate
};
// Overworld biome generator for pre-Beta 1.8
STRUCT(BiomeNoiseBeta)
{
    OctaveNoise climate[3];
    PerlinNoise oct[10];
    int nptype;
    int mc;
};


STRUCT(BiomeTree)
{
    const uint32_t *steps;
    const int32_t  *param;
    const uint64_t *nodes;
    uint32_t order;
    uint32_t len;
};

#ifdef __cplusplus
extern "C"
{
#endif

//==============================================================================
// Noise
//==============================================================================

void initSurfaceNoise(SurfaceNoise *sn, int dim, uint64_t seed);
void initSurfaceNoiseBeta(SurfaceNoiseBeta *snb, uint64_t seed);
double sampleSurfaceNoise(const SurfaceNoise *sn, int x, int y, int z);
double sampleSurfaceNoiseBetween(const SurfaceNoise *sn, int x, int y, int z,
    double noiseMin, double noiseMax);

/// The base 3D noise ("old_blended_noise") of the 1.18+ overworld, in the cell
/// units of sampleSurfaceNoise(): x and z are blocks / 4, y is blocks / 8. It
/// is seeded from the world seed with the positional random of
/// "minecraft:terrain". The result of sampleSurfaceNoise() divided by 128 is
/// the value of the density function "overworld/base_3d_noise".
void initSurfaceNoise118(SurfaceNoise *sn, uint64_t seed);


//==============================================================================
// End (1.9+), Nether (1.16+) and Overworld (1.18+) Biome Noise Generation
//==============================================================================

/**
 * Nether biomes are 3D, and generated at scale 1:4. Use voronoiAccess3D() to
 * convert coordinates at 1:1 scale to their 1:4 access. Biome checks for
 * structures are generally done at y=0.
 *
 * The function getNetherBiome() determines the nether biome at a given
 * coordinate at scale 1:4. The parameter 'ndel' is an output noise delta for
 * optimization purposes and can be ignored (nullable).
 *
 * Use mapNether2D() to get a 2D area of nether biomes at y=0, scale 1:4.
 *
 * The mapNether3D() function attempts to optimize the generation of a volume
 * at scale 1:4. The output is indexed as:
 * out[i_y*(r.sx*r.sz) + i_z*r.sx + i_x].
 * If the optimization parameter 'confidence' has a value less than 1.0, the
 * generation will generally be faster, but can yield incorrect results in some
 * circumstances.
 *
 * The output buffer for the map-functions need only be of sufficient size to
 * hold the generated area (i.e. w*h or r.sx*r.sy*r.sz).
 */
void setNetherSeed(NetherNoise *nn, uint64_t seed);
int getNetherBiome(const NetherNoise *nn, int x, int y, int z, float *ndel);
int mapNether2D(const NetherNoise *nn, int *out, int x, int z, int w, int h);
int mapNether3D(const NetherNoise *nn, int *out, Range r, float confidence);
/**
 * The scaled Nether generation supports scales 1, 4, 16, 64, and 256.
 * It is similar to mapNether3D(), but applies voronoi zoom if necessary, and
 * fills the output buffer with nether_wastes for versions older than 1.16.
 */
int genNetherScaled(const NetherNoise *nn, int *out, Range r, int mc, uint64_t sha);

/**
 * End biome generation is based on simplex noise and varies only at a 1:16
 * chunk scale which can be generated with mapEndBiome(). The function mapEnd()
 * is a variation which also scales this up on a regular grid to 1:4. The final
 * access at a 1:1 scale uses voronoi.
 */
void setEndSeed(EndNoise *en, int mc, uint64_t seed);
int mapEndBiome(const EndNoise *en, int *out, int x, int z, int w, int h);
int mapEnd(const EndNoise *en, int *out, int x, int z, int w, int h);
int getEndSurfaceHeight(int mc, uint64_t seed, int x, int z);
int mapEndSurfaceHeight(float *y, const EndNoise *en, const SurfaceNoise *sn,
    int x, int z, int w, int h, int scale, int ymin);

/**
 * The scaled End generation supports scales 1, 4, 16, and 64.
 * The End biomes are usually 2D, but in 1.15+ there is 3D voronoi noise, which
 * is controlled by the 'sha' hash of the seed. For scales higher than 1:1, and
 * versions up to 1.14, 'sha' is ignored.
 */
int genEndScaled(const EndNoise *en, int *out, Range r, int mc, uint64_t sha);

/**
 * In 1.18 the Overworld uses a new noise map system for the biome generation.
 * The random number generation also has updated to a Xiroshiro128 algorithm.
 * The scale is 1:4, and is sampled at each point individually as there is
 * currently not much benefit from generating a volume as a whole.
 *
 * The 1.18 End generation remains similar to 1.17 and does NOT use the
 * biome noise.
 */
enum {
    SAMPLE_NO_SHIFT = 0x1,  // skip local distortions
    SAMPLE_NO_DEPTH = 0x2,  // skip depth sampling for vertical biomes
    SAMPLE_NO_BIOME = 0x4,  // do not apply climate noise to biome mapping
};
void initBiomeNoise(BiomeNoise *bn, int mc);
/**
 * Like initBiomeNoise(), with the world type. With a non-zero @amplified the
 * terrain splines are those of the Amplified world type ("overworld_amplified"
 * density functions). The five horizontal climate parameters are the same as
 * in a default world, but the depth is not, so biomes below and above the
 * surface, and everything that derives from the depth, differ.
 */
void initBiomeNoiseEx(BiomeNoise *bn, int mc, int amplified);
/// Value of a spline, e.g. the offset spline BiomeNoise.sp ("overworld/offset"
/// is this value - 0.50375). @vals: continentalness, erosion, folded ridges
/// -3 * (abs(abs(weirdness) - 2/3) - 1/3), weirdness.
float getSpline(const Spline *sp, const float *vals);
/// Parameters of the double Perlin noise of a 1.18+ climate parameter.
STRUCT(ClimateNoiseSpec)
{
    double amp[9];          // octave amplitudes (len used)
    int len;
    int omin[2];            // first octave: normal, large biomes
    uint64_t md5[2][2];     // seed salt (lo, hi): normal, large biomes
};
const ClimateNoiseSpec *getClimateNoiseSpec(int nptype);

void setBiomeSeed(BiomeNoise *bn, uint64_t seed, int large);
/**
 * Initializes only the climate noises in @mask (bits 1 << NP_*), in the same
 * layout as setBiomeSeed(). The other climate noises are left as they are, so
 * a BiomeNoise can be completed for the same seed in several steps. The
 * result is fully initialized (as with setBiomeSeed()) once every parameter
 * has been initialized for that seed.
 */
void setBiomeSeedParts(BiomeNoise *bn, uint64_t seed, int large, uint32_t mask);
/**
 * Like setBiomeSeedParts(), but returns the octave initializations of the
 * climate noises in @mask as jobs instead of running them, in the order of
 * the parameters and within each noise octave noise A before B. @jobs needs
 * room for one job per entry of bn->oct. Running a subset of the jobs with
 * xPerlinInitJobs() initializes those octaves exactly.
 */
int setBiomeSeedJobs(BiomeNoise *bn, uint64_t seed, int large, uint32_t mask,
    PerlinJob *jobs);
void setBetaBiomeSeed(BiomeNoiseBeta *bnb, uint64_t seed);
int sampleBiomeNoise(const BiomeNoise *bn, int64_t *np, int x, int y, int z,
    uint64_t *dat, uint32_t sample_flags);
/// The (shifted) position at which sampleBiomeNoise() samples the climate.
void getBiomeNoisePos(const BiomeNoise *bn, int x, int z, uint32_t sample_flags,
    double *px, double *pz);
/// A climate parameter (not NP_DEPTH) at such a position, as it enters the
/// biome lookup of sampleBiomeNoise(). Needs only that noise (and the shift
/// for the position) to be initialized.
int64_t sampleBiomeNoisePara(const BiomeNoise *bn, int nptype, double px, double pz);

/**
 * Bounds on the climate parameters at which a set of 1.18+ overworld biomes
 * can generate: wherever a parameter is below lo[] or above hi[], the biome
 * is not in the set (the depth is never bounded). They follow from the
 * parameter tree, so they are exact; a set that never generates gets lo > hi.
 * (Unlike getBiomeParaLimits(), the extent of a biome's parameter boxes: the
 * nearest box decides, so a biome can occur outside of its boxes.)
 * @biomes: set of biome IDs, bit (id & 63) of biomes[id >> 6]
 * Returns 0 (and no bounds) for versions before 1.18.
 */
STRUCT(BiomeClimateBounds)
{
    int32_t lo[NP_MAX], hi[NP_MAX];
};
int getBiomeClimateBounds(BiomeClimateBounds *b, int mc, const uint64_t biomes[4]);
/// The parameters worth testing with a set of climate bounds, in the order
/// of their noise costs (bounds that hardly rule anything out are left out).
/// Returns how many.
int getClimateBoundParas(const BiomeClimateBounds *b, int paras[NP_MAX]);
/// Can the biome at a 1:4 position, as sampleBiomeNoise() with the sample
/// flags, be within the bounds? Tests only the given parameters; the noise
/// has to be initialized for them and the shift.
/// The depth parameter that sampleBiomeNoise() (with the same sample flags,
/// apart from SAMPLE_NO_DEPTH) stores in np[NP_DEPTH], without the temperature,
/// humidity and biome lookup. Requires the full climate initialization.
int64_t sampleBiomeNoiseDepth(const BiomeNoise *bn, int x, int y, int z,
    uint32_t sample_flags);

//==============================================================================
// Overworld (1.18+) Terrain
//==============================================================================

/**
 * The terrain of the 1.18+ overworld is the density function "final_density"
 * of the noise settings. Its value is computed at the corners of cells of
 * 4 by 8 by 4 blocks and interpolated linearly in between; a block is solid
 * where the result is positive. At a corner it is
 *
 *   sloped_cheese = 4 * quarter_negative((depth + jaggedness * half_negative(
 *                       jagged noise)) * factor) + base_3d_noise
 *
 * followed by the cave functions and the slides towards the top and the bottom
 * of the world. depth = 1 - y / 128 + offset; offset, factor and jaggedness
 * are splines over the continentalness, erosion and weirdness, which makes
 * them constant in a column. The world type Amplified has its own splines and
 * slides (BiomeNoise.amplified, see initBiomeNoiseEx()), everything else is
 * shared with the default world type.
 *
 * The functions below evaluate this for both world types, with these limits:
 *  - No caves: the cave density functions are not evaluated. A corner is
 *    solid wherever sloped_cheese says so, so cave entrances and the rare
 *    caves that open the surface are missing. Corners deep inside the terrain
 *    (sloped_cheese >= 1.5625) get the value the cave functions have where
 *    all cave noises are zero.
 *  - No aquifers, surface rules or features: the surface is the top of the
 *    stone, as the heightmap OCEAN_FLOOR_WG after the noise step.
 *  - Only the terrain of chunks generated by this version (no blending with
 *    old chunks).
 * Cell coordinates: x and z in units of 4 blocks (as the 1:4 biome
 * coordinates), k in units of 8 blocks, from TERRAIN_K_MIN (y = -64) to
 * TERRAIN_K_MAX (y = 320).
 */
enum { TERRAIN_K_MIN = -8, TERRAIN_K_MAX = 40 };

/// The noises of the terrain density besides the climate. They only depend on
/// the world seed (not on the version, Large Biomes or Amplified).
STRUCT(TerrainNoise)
{
    SurfaceNoise base;          // "overworld/base_3d_noise"
    DoublePerlinNoise jagged;   // noise "minecraft:jagged"
    PerlinNoise oct[2*16];
};
void initTerrainNoise(TerrainNoise *tn, uint64_t seed);
/// A TerrainNoise for the seed that belongs to the calling thread: it is
/// seeded on the first call and whenever the seed changes, and stays valid
/// until the next call from the same thread.
const TerrainNoise *getThreadTerrainNoise(uint64_t seed);

/// Values of the splines "factor" and "jaggedness" (density functions
/// overworld/factor and overworld/jaggedness, or their overworld_amplified
/// counterparts with a non-zero @amplified). @np_param is as for getSpline():
/// continentalness, erosion, folded ridges, weirdness.
float getTerrainFactor(int amplified, const float np_param[4]);
float getTerrainJaggedness(int amplified, const float np_param[4]);
/// The value of "overworld/base_3d_noise" at a cell corner.
double sampleTerrainBaseNoise(const TerrainNoise *tn, int x, int k, int z);

/// The part of the terrain density that is constant in a column of cell
/// corners (1:4 coordinates).
STRUCT(TerrainColumn)
{
    int x, z;
    int amplified;
    float c, e, w;      // continentalness, erosion, weirdness
    double base;        // depth at y = 0, i.e. 1 + offset
    double factor;
    double jaggedness;
    double jagged;      // jaggedness * half_negative(jagged noise)
};
/// @sample_flags: SAMPLE_NO_SHIFT samples the climate without the shift.
/// @bn needs the continentalness, erosion and weirdness (and the shift).
/// Without @tn the jagged noise counts as zero (jagged = 0).
void sampleTerrainColumn(const BiomeNoise *bn, const TerrainNoise *tn,
    int x, int z, uint32_t sample_flags, TerrainColumn *tc);
/// The density at the cell corner k of the column: the argument of
/// "interpolated" in final_density, without caves (see above).
double sampleTerrainDensity(const TerrainColumn *tc, const TerrainNoise *tn, int k);
/// Height in blocks at which the interpolated density of the column of cell
/// corners at (x, z) (1:4 coordinates) has its topmost change from solid to
/// empty. The top solid block is the one below it: its top face, the value
/// of the heightmap, is at ceil() of the result. At least -64 (the bottom
/// cell corners are always solid). The top slide keeps it below the end of
/// the slide: the highest corner that can be solid is 8 blocks below it and
/// reaches 0.742 at most against -0.078125 above, so the result is below
/// 319.25 in an Amplified world and below 255.25 in a default one.
double sampleTerrainSurface(const BiomeNoise *bn, const TerrainNoise *tn,
    int x, int z, uint32_t sample_flags);
/// The heightmap value (y of the top solid block + 1) of the terrain at a
/// block position, with the interpolation between the four surrounding
/// columns of cell corners as in the game.
int getTerrainHeightAt(const BiomeNoise *bn, const TerrainNoise *tn,
    int blockX, int blockZ);

/// Surface relief (1.18+) for map views. It needs the base 3D noise, which
/// is allocated and seeded on first use, so that views that never need it do
/// not pay for it. Initialize with reliefNoiseInit(), release with
/// reliefNoiseFree(). One instance per thread and seed.
STRUCT(ReliefNoise)
{
    SurfaceNoise *sn;   // default world type: only over mushroom islands
    TerrainNoise *tn;   // Amplified
};
void reliefNoiseInit(ReliefNoise *rn);
void reliefNoiseFree(ReliefNoise *rn);

/// Height in blocks of the terrain surface of the column at the biome
/// coordinates (x, z).
/// Default world type: like the depth-based height of sampleBiomeNoiseDepth()
/// (y / 76) where there is no mushroom island, and the height of the top
/// face of the surface block where there is one. In between, the two are
/// blended by the continentalness.
/// Amplified: the depth says little about the surface there, so every column
/// gets the surface of the terrain density, sampleTerrainSurface() + 0.5 (the
/// expected top face of the surface block).
/// @bn must be set up for the depth.
double sampleBiomeNoiseRelief(const BiomeNoise *bn, ReliefNoise *rn,
    uint64_t seed, int x, int z);
int climateMayBeWithin(const BiomeNoise *bn, const BiomeClimateBounds *b,
    const int *paras, int npara, int x, int z, uint32_t sample_flags);
int sampleBiomeNoiseBeta(const BiomeNoiseBeta *bnb, int64_t *np, double *nv,
    int x, int z);
double approxSurfaceBeta(const BiomeNoiseBeta *bnb, const SurfaceNoiseBeta *snb,
    int x, int z); // doesn't really work yet

/**
 * (Alpha 1.2 - Beta 1.7) 
 * Temperature and humidity values to biome.
 */
int getOldBetaBiome(float t, float h);

/**
 * Uses the global biome tree definitions (see tables/btreeXX.h)
 * to map a noise point (i.e. climate) to the corresponding overworld biome.
 */
int climateToBiome(int mc, const uint64_t np[6], uint64_t *dat);

/**
 * Initialize BiomeNoise for only a single climate parameter.
 * If nptype == NP_DEPTH, the value is sampled at y=0. Note that this value
 * changes linearly with the height (i.e. -= y/128).
 * A maximum of nmax octaves is set, initializing only the most contributing
 * octaves up to that point. Use -1 for a full initialization.
 */
void setClimateParaSeed(BiomeNoise *bn, uint64_t seed, int large, int nptype, int nmax);
double sampleClimatePara(const BiomeNoise *bn, int64_t *np, double x, double z);

/**
 * Currently, in 1.18, we have to generate biomes one chunk at a time to get an
 * accurate mapping of the biomes in the level storage, as there is no longer a
 * unique mapping from noise points to biomes (MC-241546). Note that the results
 * from this are not suitable for chunk population/structure generation.
 * The output is in the form out[x][y][z] for the 64 biome points in the chunk
 * section. The coordinates {cx,cy,cz} are all at scale 1:16 and the 'dat'
 * argument should be the previous noise sampling and can be left NULL.
 */
void genBiomeNoiseChunkSection(const BiomeNoise *bn, int out[4][4][4],
    int cx, int cy, int cz, uint64_t *dat);

/**
 * The scaled biome noise generation applies for the Overworld version 1.18+.
 * The 'sha' hash of the seed is only required for voronoi at scale 1:1.
 * A scale of zero is interpreted as the default 1:4 scale.
 */
int genBiomeNoiseScaled(const BiomeNoise *bn, int *out, Range r, uint64_t sha);

/**
 * Generates the biomes for Beta 1.7, the surface noise is optional and enables
 * ocean mapping in areas that fall below the sea level.
 */
int genBiomeNoiseBetaScaled(const BiomeNoiseBeta *bnb, const SurfaceNoiseBeta *snb,
    int *out, Range r);


int getBiomeDepthAndScale(int id, double *depth, double *scale, int *grass);

// Gets the range in the parent/source layer which may be accessed by voronoi.
Range getVoronoiSrcRange(Range r);


#ifdef __cplusplus
}
#endif

#endif /* BIOMENOISE_H_ */



