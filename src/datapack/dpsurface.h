#ifndef DPSURFACE_H
#define DPSURFACE_H

// The surface rules of a datapack world (the noise settings' surface_rule),
// as far as they decide the top blocks of a column: which heightmaps
// (WORLD_SURFACE_WG, OCEAN_FLOOR_WG) the game's surface step changes where a
// rule puts air, a fluid or a block that does not block motion (powder
// snow) on top of the terrain. The blocks themselves only matter by class.
//
// Not modelled (code, not rules): the icebergs of frozen oceans and the
// pillars of eroded badlands that SurfaceSystem builds itself.
//
// Internal to src/datapack.

#include "dpdensity.h"

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <climits>
#include <functional>
#include <cstdint>
#include <memory>
#include <vector>

namespace dp
{

// What a block is for the heightmaps
enum SurfaceClass : uint8_t
{
    SC_SOLID,   // blocks motion (counts for both)
    SC_SOFT,    // not air, does not block motion (fluids, powder snow, plants)
    SC_AIR,     // air (counts for neither)
};

// The class of a block state of a surface rule's result_state (unknown blocks
// are solid)
SurfaceClass surfaceClassOf(const QJsonObject& state);

// A dimension's surface rule, compiled (immutable, per world)
struct SurfaceRuleDef
{
    bool on = false;            // the heights follow the rules
    QString reason;             // the rules are left out: why (empty if on, or nothing to do)
    // no rule can put anything but a solid block: the rules change no height
    bool noEffect = true;

    enum CondKind : uint8_t
    {
        C_BIOME, C_NOISE, C_GRADIENT, C_Y_ABOVE, C_WATER, C_TEMPERATURE, C_STEEP, C_NOT, C_HOLE,
        C_ABOVE_PRELIMINARY, C_STONE_DEPTH,
    };
    struct Cond
    {
        CondKind kind;
        int cost = 0;           // the order in which the cheap test tries them
        int arg = -1;           // C_NOT: the condition; C_NOISE: the noise slot; C_GRADIENT: the random slot
        int anchorY = 0;        // C_Y_ABOVE (resolved), C_GRADIENT: true at and below
        int falseY = 0;         // C_GRADIENT: false at and above
        int offset = 0;         // C_WATER, C_STONE_DEPTH
        int mult = 0;           // C_Y_ABOVE, C_WATER: surface_depth_multiplier
        bool addStone = false;  // C_Y_ABOVE, C_WATER: add_stone_depth
        bool addSurface = false;    // C_STONE_DEPTH: add_surface_depth
        bool ceiling = false;   // C_STONE_DEPTH: surface_type ceiling
        int secondary = 0;      // C_STONE_DEPTH: secondary_depth_range
        double lo = 0, hi = 0;  // C_NOISE: thresholds
        int biomes = -1;        // C_BIOME: index into biomeSets
    };
    enum RuleKind : uint8_t { R_BLOCK, R_SEQUENCE, R_CONDITION, R_BANDLANDS };
    struct Rule
    {
        RuleKind kind;
        SurfaceClass block = SC_SOLID;  // R_BLOCK
        uint8_t fluid = 0;              // R_BLOCK: 1 water, 2 lava
        int cond = -1, then = -1;       // R_CONDITION
        int first = 0, count = 0;       // R_SEQUENCE: children[first .. first + count - 1]
    };
    std::vector<Cond> conds;
    std::vector<Rule> rules;
    std::vector<int32_t> children;
    int root = -1;
    std::vector<std::vector<char>> biomeSets;   // by World biome index
    // The blocks that are not solid: their rule and the conditions on the
    // way there (sorted by cost)
    struct Leaf { int rule; std::vector<int32_t> path; };
    std::vector<Leaf> softLeaves;

    // the noises the rules sample at (x, 0, z): surface, surface_secondary,
    // then those of noise_threshold (nodes of the graph, one program)
    std::vector<int32_t> noiseNodes;
    DensityProgram program;
    std::vector<QString> randomNames;   // vertical_gradient
    bool usesPreliminary = false, usesTemperature = false, usesSteep = false, usesBiome = false;
    // biome temperatures (by World biome index): base temperature and the
    // frozen modifier
    std::vector<float> temperature;
    std::vector<char> frozen;
    int seaLevel = 63;
    int snowLevel = 80;         // Biome.getHeightAdjustedTemperature(): 80, sea level + 17 from 1.21.2
    int minY = -64;             // the dimension's lowest block (the end of the game's scan)
    bool legacy = false;
};

// Builds @def from the noise settings @settings. @biomeIndex maps biome ids
// to World biome indices; @climate gives the temperature and the frozen
// modifier of a biome by index (false if unknown). Rules the generator
// cannot follow leave def->on false with a reason.
struct SurfaceBuildContext
{
    DensityGraph *graph;
    int pack;
    QString id;
    int legacySettings;
    int mc;
    int seaLevel;
    int minGenY, genDepth;      // WorldGenerationContext: the anchors
    int minY;                   // the dimension's lowest block
    bool legacy;
    bool hasPreliminary;        // the aquifers' preliminary surface is there
    const QHash<QString, int> *biomeIndex;
    int biomeCount;
    std::function<bool(int biome, float *temperature, bool *frozen)> climate;
    // the biome ids of a biome tag (without the '#')
    std::function<QStringList(const QString& tag)> biomeTag;
};
void buildSurfaceRules(const QJsonObject& settings, const SurfaceBuildContext& ctx, SurfaceRuleDef *def);

// What the rules need of the world at a column (the sampler and its terrain)
class SurfaceAccess
{
public:
    virtual ~SurfaceAccess() {}
    // World biome index at a block (the game's 1:1 zoom)
    virtual int biome(int x, int y, int z) = 0;
    // the block after the terrain fill: 0 air, 1 water (a fluid but lava), 2 the default block,
    // 3 another solid block, 4 lava
    virtual int block(int x, int y, int z) = 0;
    // the WORLD_SURFACE_WG height of a column before the surface step
    virtual int surfaceBefore(int x, int z) = 0;
    // NoiseChunk.preliminarySurfaceLevel()
    virtual int preliminary(int x, int z) = 0;
    // the request is being aborted (what was found may be wrong)
    virtual bool aborted() = 0;
};

// Per seed (noises, randoms, caches). Not thread safe: one per sampler.
class SurfaceRuleState
{
public:
    SurfaceRuleState(const DensityGraph& graph, const SurfaceRuleDef& def, uint64_t seed, int mc);
    ~SurfaceRuleState();

    // The column (x, z) before the surface step: @floor the OCEAN_FLOOR_WG
    // height, @surface WORLD_SURFACE_WG, @water the game's water height at
    // the floor (the block above the highest fluid since the last air above
    // it, or INT_MIN), @floorDefault 1 if the floor block is the default
    // block, 0 if not, -1 if not known. Changes floor and surface to the
    // heights after the surface step.
    void apply(SurfaceAccess& access, int x, int z, int water, int floorDefault, int *floor, int *surface);

private:
    const SurfaceRuleDef& def;
    std::unique_ptr<DensityState> noise;
    uint64_t posLo = 0, posHi = 0;      // the world's positional random (legacy: posLo)
    std::vector<uint64_t> gradLo, gradHi;
    PerlinNoise tempNoise, frozenNoise[3], infoNoise;

    // the column
    SurfaceAccess *acc = nullptr;
    int cx = 0, cz = 0;
    bool haveDepth = false, haveSecondary = false, haveMinSurface = false;
    int surfaceDepth = 0, minSurface = 0;
    std::vector<double> noiseValue;
    std::vector<char> haveNoise;
    int runBottom = INT_MIN;            // the lowest block of the solid run below, if known
    int runKnownTo = INT_MAX;           // the run is solid from runTop down to here
    int runTop = INT_MIN;
    // the block
    int by = 0, stoneAbove = 0, waterHeight = INT_MIN;
    int biomeAt = -2;
    int leaf = -1;              // the block rule rule() came to
    std::vector<int8_t> memo;           // per condition: -1 unknown, 0, 1
    int cornerKey[2] = { INT_MIN, INT_MIN };
    int corner[4] = {};

    void setBlock(int y, int stone, int water);
    bool cond(int c);
    bool compute(int c);
    int rule(int r);            // -1: none (the block stays), else a SurfaceClass
    bool maySoft();
    int depth();
    double secondaryValue();
    double noiseAt(int slot);
    int minSurfaceLevel();
    int biome();
    int stoneBelowAtMost(int limit);
    bool coldEnoughToSnow(int b);
};

} // namespace dp

#endif // DPSURFACE_H
