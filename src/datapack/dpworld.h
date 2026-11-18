#ifndef DPWORLD_H
#define DPWORLD_H

// Datapack worlds: a generic world generator that is driven by the worldgen
// JSON of Minecraft data packs (density functions, noises, noise settings,
// dimensions, biome sources), next to the built-in engine. It has its own
// biome registry (names and colours come from the packs, there is no limit of
// 256 ids), so nothing of the engine's id space or file formats is involved.
// The engine contributes its noise primitives and, for the vanilla biome list
// (the preset "minecraft:overworld", which the game defines in code), its
// biome tree.
//
// Nothing of a pack is compiled into the app: packs and the vanilla data pack
// are read from files at run time.
//
// This header is the whole public interface. It depends on Qt Core only.

#include <QColor>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace dp
{

// One pack of the world, in load order (later packs override earlier ones).
struct PackSource
{
    QString path;       // zip or jar file, or a folder (with pack.mcmeta or data/)
    bool enabled = true;
};

// What to load.
struct Request
{
    int mc = 0;                 // engine version id (MC_1_21_1, ...)
    // The vanilla data pack of that version: a folder or zip/jar that holds
    // data/minecraft/worldgen/... (for example the game's own client jar, or
    // an extracted copy). Packs refer to vanilla density functions and
    // noises, so it is always loaded first.
    QString vanillaPath;
    std::vector<PackSource> packs;
    // World preset to take the dimensions from when no pack replaces them,
    // e.g. "minecraft:normal", "minecraft:large_biomes", "minecraft:amplified".
    QString preset = QStringLiteral("minecraft:normal");
};

struct Issue
{
    enum Severity { Info, Warning, Error };
    Severity severity = Info;
    QString pack;       // file name of the pack the issue belongs to, or empty
    QString id;         // resource id, e.g. "somepack:overworld/offset", or empty
    QString text;       // one plain sentence
};

// Everything the load found out, for the user.
struct Report
{
    std::vector<Issue> issues;
    bool ok() const
    {
        for (const Issue& i : issues)
            if (i.severity == Issue::Error)
                return false;
        return true;
    }
};

struct PackInfo
{
    QString path;
    QString name;           // file name without the path
    QString description;    // from pack.mcmeta, plain text
    int packFormat = 0;
    bool formatMatches = true;  // the pack declares support for the version
    QString formatProblem;      // why the game reads no formats from it (it loads all the same)
    bool loaded = true;         // false: the game does not load it (pack.mcmeta), nothing is used
    QStringList overlays;       // overlays applied for the version
    int worldgenFiles = 0;      // number of worldgen files the pack contributes
};

struct BiomeInfo
{
    QString id;         // "minecraft:plains", "somepack:alpine_grove"
    QString name;       // display name
    QRgb color = 0;
    bool vanilla = false;
};

struct DimensionInfo
{
    QString id;             // "minecraft:overworld", ...
    int minY = -64;
    int height = 384;
    int seaLevel = 63;
    bool supported = false; // the map can show it
    QString reason;         // if not supported: why, one plain sentence
    std::vector<int> biomes;    // indices into World::biomes() it can generate
    // Terrain (Sampler::surfaceHeight() and the functions after it): the
    // dimension's final_density compiled; if not, terrainReason says why.
    bool terrain = false;
    QString terrainReason;
    // The noise settings have aquifers, but the heights leave them out
    // (the global sea and lava levels only): why. Empty if the aquifers are
    // modelled or the dimension has none.
    QString aquiferReason;
    // The heights of HEIGHTS_SURFACE leave the noise settings' surface rules
    // out: why. Empty if they follow them or no rule changes a height.
    QString surfaceReason;
    // The dimension type has a ceiling (the Nether): the highest block of a
    // column is the roof. Ask for the height below a y instead (see
    // Sampler::surfaceHeight()).
    bool hasCeiling = false;
    // A pack defines the dimension with a file of its own (data/<namespace>/
    // dimension/...), so the world preset does not change it.
    bool packFile = false;
};

// A structure set of the packs: a placement rule and the structures it
// chooses from.
struct StructureSetInfo
{
    QString id;             // "minecraft:villages"
    QString placement;      // "minecraft:random_spread", "minecraft:concentric_rings", ...
    bool supported = false; // its starts can be listed
    QString reason;         // if not supported: why, one plain sentence
    std::vector<int> structures;    // indices into World::structures(), in the set's order
};

struct StructureInfo
{
    QString id;             // "minecraft:village_plains", "somepack:tower"
    QString name;           // display name
    QString set;            // id of the (first) structure set that places it, or empty
    QString type;           // "minecraft:jigsaw", "minecraft:desert_pyramid", ...
    QString pack;           // file name of the pack that defines it (the last one), empty for vanilla
    bool supported = false; // the start position and its checks are known
    QString reason;         // if not supported: why, one plain sentence
};

// Where a structure may start: the result of the game's placement rules for
// one chunk and one structure of a set.
struct StructureStart
{
    enum State
    {
        Valid,      // the game starts the structure here
        Invalid,    // it does not (the biome check fails)
        Unknown,    // it depends on what cannot be computed (yet), see detail
    };
    enum Detail
    {
        Checked,        // decided by the biome check at the start position
        NeedsHeight,    // the start's height comes from the terrain, and the
                        // biome check gives different results at the heights
                        // it can have
        TerrainCheck,   // the biome fits, a check of the terrain is left
                        // (e.g. a temple below sea level, a mansion below 60)
        EarlierUnknown, // a structure the game tries first in this chunk may
                        // start instead
        NotSupported,   // the structure type is not supported
        TiedBiome,      // entries of several biomes are nearest to the
                        // climate at the start (the game's result depends on
                        // the lookup it made before, which is not known)
    };

    int chunkX = 0, chunkZ = 0;
    int set = -1;           // index into World::structureSets()
    int structure = -1;     // index into World::structures()
    // The start position (the position of the game's biome check), in
    // blocks. x and z are always known. hasY: y is the game's. Where the
    // start height comes from the terrain, that is where the dimension's
    // terrain gives it exactly (see Sampler::surfaceHeight(); where the
    // noise settings enable aquifers, as in the overworld, only the highest
    // solid block of a column is certain, not the fluids above it).
    int x = 0, y = 0, z = 0;
    bool hasY = false;
    // If hasY is false: y is the terrain's estimate of the start height
    // (exact unless aquifers change the column). If both are false, y is 0
    // (mineshafts, ruined portals: their height comes from their pieces).
    bool estimatedY = false;
    State state = Unknown;
    Detail detail = Checked;
};

// Which blocks a height counts, like the game's heightmaps of a chunk right
// after its terrain is generated (before carvers, features and structures).
enum HeightKind
{
    // WORLD_SURFACE_WG: every block that is not air, fluids included (the
    // top of the water in an ocean)
    HEIGHT_SURFACE,
    // OCEAN_FLOOR_WG: blocks that block motion, without fluids (the sea bed)
    HEIGHT_OCEAN_FLOOR,
};

// How much of the game's terrain generation a height follows
enum HeightDetail
{
    // the terrain fill: final_density, the default block, the fluid levels
    // and the aquifers (the game's getBaseHeight(), which the structures use)
    HEIGHTS_FILL,
    // the fill and then the surface rules of the noise settings, which can
    // put fluid, air or blocks that do not block motion (powder snow) on top
    // of a column (the heightmaps after the game's surface step; what the
    // game builds there in code, icebergs and eroded badlands pillars, is
    // not modelled)
    HEIGHTS_SURFACE,
    // the fill with the global fluid levels only (no aquifers, no surface
    // rules): the highest solid block is exact, a fluid above it may not be.
    // Much faster where aquifers are on; for maps zoomed far out.
    HEIGHTS_FAST,
};

// A height of a column that has no counted block (also: no terrain)
const int NO_HEIGHT = -0x7fffffff;

// Versions the datapack generator supports (checked against the game).
bool versionSupported(int mc);

class WorldPrivate;

// A loaded set of packs for one version. Immutable and safe to share between
// threads once load() has returned.
class World
{
public:
    // Never returns null. If the report has an error, the world has no
    // supported dimension.
    static std::shared_ptr<const World> load(const Request& request, Report *report);
    ~World();

    const Request& request() const;
    const std::vector<PackInfo>& packs() const;
    const std::vector<BiomeInfo>& biomes() const;
    const std::vector<DimensionInfo>& dimensions() const;
    // index into dimensions(), or -1
    int dimension(const QString& id) const;
    // world presets the packs and the vanilla data offer
    QStringList presets() const;
    // structure sets and structures of the packs and the vanilla data
    const std::vector<StructureSetInfo>& structureSets() const;
    const std::vector<StructureInfo>& structures() const;

    const WorldPrivate *d() const { return p; }

private:
    World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    WorldPrivate *p;
};

class SamplerPrivate;

// Generates the biomes of one dimension of a world for one seed. A sampler
// belongs to one thread at a time (it holds caches); create one per worker.
class Sampler
{
public:
    Sampler(std::shared_ptr<const World> world, int dimension, uint64_t seed);
    ~Sampler();

    bool valid() const;

    // Biomes of the area x .. x+sx-1, z .. z+sz-1 in units of @scale blocks
    // (1, 4, 16, 64, 256, 1024), at block height @y. Writes sx*sz indices into
    // World::biomes() to @out, row by row (out[j*sx + i]), -1 where there is
    // no biome. At scale 1 the biomes get the game's 1:1 zoom (voronoi), at
    // scale 4 they are the game's biome cells, above that one cell is sampled
    // per pixel. Returns false if it was aborted or the sampler is not valid.
    bool genBiomes(int *out, int scale, int x, int z, int sx, int sz, int y,
        const std::atomic_bool *abort = nullptr);

    // The biome at one position, in units of @scale blocks (y in blocks).
    int biomeAt(int scale, int x, int y, int z);

    SamplerPrivate *d() { return p; }

    // Terrain: the dimension's final_density as the game fills a chunk with
    // it (noise cells, interpolation, the default block, the sea and lava
    // levels of the noise settings, the aquifers unless
    // DimensionInfo::aquiferReason says why not). Not modelled: carvers,
    // surface rules, features (trees, lakes, ...) and structures.

    // True if the terrain functions below give heights (see
    // DimensionInfo::terrain).
    bool hasTerrain() const;

    // The block y of the highest block of the column (x, z) that @kind
    // counts, at or below @top (blocks above the dimension or the noise
    // range are air). This is the game's getBaseHeight() - 1, the same
    // convention as the H lines of tools/worldgen-reference. NO_HEIGHT if
    // the column has no such block or there is no terrain. For dimensions
    // with a ceiling pass the y to look down from as @top.
    int surfaceHeight(int x, int z, HeightKind kind = HEIGHT_SURFACE, int top = 0x7fffffff,
        HeightDetail detail = HEIGHTS_FILL);

    // surfaceHeight() of the area x .. x+sx-1, z .. z+sz-1 in units of
    // @scale blocks (any scale >= 1): out[j*sx + i] is the height of the
    // block column (x+i)*scale, (z+j)*scale (the lowest corner of the
    // pixel). Returns false if it was aborted or there is no terrain.
    bool genHeights(int *out, int scale, int x, int z, int sx, int sz,
        HeightKind kind = HEIGHT_SURFACE, int top = 0x7fffffff,
        const std::atomic_bool *abort = nullptr, HeightDetail detail = HEIGHTS_FILL);

    // genHeights() of both kinds: @floor gets HEIGHT_OCEAN_FLOOR, @surface
    // HEIGHT_SURFACE, the same values as two calls. Where the dimension's
    // default block is solid, the surface comes from the floor's search and
    // costs nothing more.
    bool genBothHeights(int *floor, int *surface, int scale, int x, int z, int sx, int sz,
        int top = 0x7fffffff, const std::atomic_bool *abort = nullptr, HeightDetail detail = HEIGHTS_FILL);

    // Is the block at (x, y, z) the default block (stone, netherrack, ...)
    // after the terrain fill, i.e. is final_density positive there?
    bool solidAt(int x, int y, int z);

private:
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    SamplerPrivate *p;
};

class StructureFinderPrivate;

// Lists the structure starts of one dimension of a world for one seed, like
// the game places them. Belongs to one thread at a time (it holds a sampler
// and caches); the results only depend on the world, the dimension, the seed
// and the position.
class StructureFinder
{
public:
    StructureFinder(std::shared_ptr<const World> world, int dimension, uint64_t seed);
    ~StructureFinder();

    bool valid() const;

    // The start candidates of the chunks @chunkX0 .. @chunkX1, @chunkZ0 ..
    // @chunkZ1 (inclusive), by set, structure and chunk. Where a set chooses
    // between several structures, all that the game may try in a chunk are
    // listed, in the game's order, up to the first valid one. With
    // @structures (by index into World::structures()), only the sets that
    // can place a marked structure are searched. Returns false if it was
    // aborted or the finder is not valid.
    bool starts(int chunkX0, int chunkZ0, int chunkX1, int chunkZ1, std::vector<StructureStart> *out,
        const std::atomic_bool *abort = nullptr, const std::vector<char> *structures = nullptr);

    // With false, a start that is not generated because its biome check
    // fails at every height the start can have comes without its height
    // (StructureStart::hasY false), which spares the terrain; every other
    // start is the same. For callers that only show generated starts (the
    // map). Default true.
    void setHeightsOfInvalid(bool exact);

    StructureFinderPrivate *d() { return p; }

private:
    StructureFinder(const StructureFinder&) = delete;
    StructureFinder& operator=(const StructureFinder&) = delete;
    StructureFinderPrivate *p;
};

} // namespace dp

#endif // DPWORLD_H
