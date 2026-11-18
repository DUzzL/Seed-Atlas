#ifndef DPSTRUCTURES_H
#define DPSTRUCTURES_H

// Structures of datapack worlds: the structure sets, structures and jigsaw
// start pools of the packs, and the game's rules for where structures start
// (StructurePlacement, ChunkGenerator.createStructures, Structure.
// findGenerationPoint of each structure type). Checked against the 1.21.1
// server; see tests/dptest.cpp ("structures").
//
// Internal to src/datapack and its tests.

#include "dpworld.h"

#include <QByteArray>
#include <QString>

#include <climits>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace dp
{

class WorldPrivate;

// The game's heightmap types (Heightmap.Types), in its order
enum Heightmap
{
    HM_WORLD_SURFACE_WG,
    HM_WORLD_SURFACE,
    HM_OCEAN_FLOOR_WG,
    HM_OCEAN_FLOOR,
    HM_MOTION_BLOCKING,
    HM_MOTION_BLOCKING_NO_LEAVES,
};

// Terrain heights for the structure checks that depend on them (the start
// height of most structure types). A finder takes them from the
// dimension's terrain (Sampler::surfaceHeight()); this interface replaces
// that, for tests with the heights of a server. Implementations must be
// pure functions of the position (for one world, dimension and seed).
class TerrainHeights
{
public:
    virtual ~TerrainHeights() {}
    // The game's ChunkGenerator.getBaseHeight(x, z, type): the lowest y
    // above the column's terrain for the heightmap type (its first free
    // block), or INT_MIN if it is not known.
    virtual int baseHeight(int x, int z, Heightmap type) = 0;
};

// Gives a finder other terrain heights than its dimension's (null: the
// dimension's again); a height they do not know (INT_MIN) is unknown. The
// finder keeps the pointer; it must outlive the finder. Call it before the
// first starts() (the finder caches the heights it found).
void setTerrainHeights(StructureFinder& finder, TerrainHeights *heights);

// Test hook: the chunk positions of a concentric_rings set (empty for other
// sets or a set that cannot be in the finder's dimension).
std::vector<std::pair<int, int>> ringPositions(StructureFinder& finder, int set);

// VerticalAnchor
struct VAnchor
{
    enum Kind { ABSOLUTE, ABOVE_BOTTOM, BELOW_TOP };
    Kind kind = ABSOLUTE;
    int value = 0;
    int resolve(int minY, int height) const;
};

// HeightProvider (start_height of jigsaw structures, height of nether fossils)
struct HeightProv
{
    enum Type { CONSTANT, UNIFORM, BIASED_TO_BOTTOM, VERY_BIASED_TO_BOTTOM, TRAPEZOID, WEIGHTED_LIST };
    Type type = CONSTANT;
    VAnchor a, b;           // the value (CONSTANT) or min_inclusive / max_inclusive
    int inner = 1;          // biased: inner; trapezoid: plateau
    std::vector<std::pair<HeightProv, int>> list;
    // Draws like the game from the java.util.Random state @rng.
    int sample(uint64_t *rng, int minY, int height) const;
};

// What the start of a jigsaw structure needs of a structure template
struct Template
{
    bool found = false;
    int sx = 0, sy = 0, sz = 0;
    struct Jigsaw
    {
        int x = 0, y = 0, z = 0;
        QString name;
        int priority = 0;   // selection_priority
    };
    // the jigsaw blocks of each palette, in the game's block order
    std::vector<std::vector<Jigsaw>> palettes;
};

// Reads the parts of a structure template (.nbt, gzip compressed) that
// Template holds. Returns false with a message if it cannot be read.
bool readTemplate(const QByteArray& file, Template *t, QString *error);

struct PoolElement
{
    enum Kind { SINGLE, LIST, FEATURE, EMPTY };
    Kind kind = EMPTY;
    int tmpl = -1;                      // SINGLE: index into StructureData::templates
    std::vector<PoolElement> elements;  // LIST
};

// One structure of the registry worldgen/structure
struct StructureDef
{
    enum Kind
    {
        UNSUPPORTED,
        JIGSAW,
        STRONGHOLD,         // at the chunk's corner, y 0
        FORTRESS,           // at the chunk's corner, y 64
        RUINED_PORTAL,      // at the chunk's corner, y from the terrain
        MINESHAFT,          // at (8, 0), y from its pieces
        MANSION,            // at (7, 7), lowest terrain of a 5x5 box, y >= 60
        END_CITY,           // the same
        CHUNK_CENTER,       // at (8, 8) on the terrain (igloo, swamp hut, ...)
        SINGLE_PIECE,       // the same, the footprint not below sea level
        MONUMENT,           // the same, all biomes around at sea level oceans
        NETHER_FOSSIL,      // a random column of the chunk
    };
    Kind kind = UNSUPPORTED;
    std::vector<char> biomes;           // per index into World::biomes(): allowed
    Heightmap heightmap = HM_WORLD_SURFACE_WG;
    int width = 0, depth = 0;           // SINGLE_PIECE footprint

    // JIGSAW
    HeightProv startHeight;             // also NETHER_FOSSIL: height
    bool projected = false;             // project_start_to_heightmap
    QString startJigsawName;
    std::vector<PoolElement> pool;      // the start pool's elements
    std::vector<int> poolCumulative;    // per element: the sum of the weights up to it
    int poolTotal = 0;                  // the sum of all weights
    // dimension_padding, checked (1.21.4 and later) if the structure has it
    bool padding = false;
    int padBottom = 0, padTop = 0;
};

struct PlacementDef
{
    enum Kind { UNSUPPORTED, RANDOM_SPREAD, CONCENTRIC_RINGS };
    Kind kind = UNSUPPORTED;
    int salt = 0;
    float frequency = 1.0F;
    int reduction = 0;      // 0 default, 1 .. 3 legacy_type_1 .. 3
    int excludeSet = -1;    // exclusion_zone: index into StructureData::sets
    int excludeChunks = 0;
    // RANDOM_SPREAD
    int spacing = 1, separation = 0;
    bool triangular = false;
    // CONCENTRIC_RINGS
    int distance = 0, spread = 0, count = 0;
    std::vector<char> preferred;        // per biome index
};

struct StructureSetDef
{
    PlacementDef placement;
    std::vector<std::pair<int, int>> entries;   // (index into structures, weight)
};

// Everything World::load() builds for structures
struct StructureData
{
    std::vector<StructureSetInfo> setInfo;
    std::vector<StructureInfo> info;
    std::vector<StructureSetDef> sets;
    std::vector<StructureDef> defs;
    std::vector<Template> templates;
    std::vector<char> monumentSurrounding;  // #minecraft:required_ocean_monument_surrounding
};

// Loads the structures of the world's resources into w.structures. Problems
// are warnings in w.report: they never stop the biome map.
void loadStructures(WorldPrivate& w);

} // namespace dp

#endif // DPSTRUCTURES_H
