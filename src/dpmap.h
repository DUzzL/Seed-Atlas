#ifndef DPMAP_H
#define DPMAP_H

// Structures and terrain heights of datapack worlds in the application: what
// the map, the structure analysis and the seed search share.
//
// - DpStructureCatalog: the structures of a datapack world as the app shows
//   them (name, namespace, pack, the vanilla map option of a vanilla
//   structure, a stable badge colour), built once per world snapshot.
// - Structure finders: a dp::StructureFinder costs most on its first query
//   (the game's stronghold rings are searched for every concentric_rings set
//   it asks for, up to a second or more), so finders of the map and of single-seed
//   analyses are kept in a pool that outlives the map's worker threads and
//   are warmed up off those threads. Every starts() query of the app goes
//   through dpStartsInArea().
// - Heights: tiles of terrain heights with the one-pixel border of the
//   height shading, and the colouring of the height and terrain layers.

#include "datapackworld.h"

#include <QPixmap>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <vector>

// ------------------------------------------------------------ structures

struct DpStructureEntry
{
    QString id;         // "minecraft:village_plains", "somepack:tower"
    QString name;       // display name
    QString label;      // the name, with the id where two structures have the same name
    QString ns;         // namespace
    QString pack;       // file name of the pack that defines it, "Minecraft" for vanilla data
    QString type;       // structure type id
    QString set;        // structure set id
    QString reason;     // why it is not supported
    bool supported = false;
    int vanillaOpt = -1;    // D_ map option of a vanilla structure, or D_NONE
    QRgb color = 0;     // badge colour, stable per id
    QChar letter;       // badge letter
};

struct DpStructureCatalog
{
    std::vector<DpStructureEntry> entries;  // in the order of dp::World::structures()
    QStringList namespaces;                 // sorted, "minecraft" first
};

// The catalogue of a usable world (null otherwise). Cached per snapshot.
std::shared_ptr<const DpStructureCatalog> dpStructureCatalog(
    const std::shared_ptr<const DatapackWorld>& world);

// The D_ map option whose icon and toggle a vanilla structure id uses, or D_NONE.
int dpVanillaStructureOpt(const QString& id);
// The engine dimension (DIM_OVERWORLD, DIM_NETHER, DIM_END) that a datapack
// dimension resembles: the vanilla ids by name, a dimension with a ceiling
// as the Nether, everything else as the Overworld.
int dpEngineDim(const DatapackWorld& world);

// The map marker of a structure (GUI thread): the vanilla icon, or a badge in
// the structure's colour with its letter. @unknown: faded, with a "?".
const QPixmap& dpStructureIcon(const DpStructureEntry& entry, bool unknown);
// A badge for a namespace (the structure layer button).
QPixmap dpNamespaceBadge(const QString& ns, int size);

// "valid", "unknown (the start height depends on the terrain)", ...
QString dpStartStateText(const dp::StructureStart& start);

// The structure starts whose start position (x, z in blocks) lies in the
// block area x0 .. x1, z0 .. z1 (inclusive), for the structures marked in
// @structures (one flag per dp::World::structures(); null: all). Invalid
// candidates are left out unless @invalid. Returns false if aborted.
bool dpStartsInArea(dp::StructureFinder *finder, int x0, int z0, int x1, int z1,
    const std::vector<char> *structures, bool invalid,
    std::vector<dp::StructureStart> *out, const std::atomic_bool *abort);

// A warm finder from the pool, for the world, dimension and seed. Waits
// (polling @abort) while all finders of that key are in use or warming up.
// Null if aborted or the world cannot be shown. Give it back with
// dpReturnFinder() (also after an aborted query: a finder stays valid).
std::unique_ptr<dp::StructureFinder> dpTakeFinder(const std::shared_ptr<const DatapackWorld>& world,
    uint64_t seed, const std::atomic_bool *abort);
void dpReturnFinder(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed,
    std::unique_ptr<dp::StructureFinder> finder);
// Drops all pooled finders (tests: cold timings).
void dpClearFinderPool();

// Borrows a pooled finder for a scope.
class DpFinderLease
{
public:
    DpFinderLease(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed,
        const std::atomic_bool *abort)
        : world(world), seed(seed), finder(dpTakeFinder(world, seed, abort)) {}
    ~DpFinderLease() { if (finder) dpReturnFinder(world, seed, std::move(finder)); }
    dp::StructureFinder *get() const { return finder.get(); }
private:
    std::shared_ptr<const DatapackWorld> world;
    uint64_t seed;
    std::unique_ptr<dp::StructureFinder> finder;
};

// The area of a map tile of datapack structures, in blocks, and the zoom out
// to which the map shows them (blocks per pixel).
constexpr int DP_STRUCT_TILE = 2048;
constexpr double DP_STRUCT_MAX_SCALE = 32;

// ------------------------------------------------------------ heights

// The height layers of a datapack world: what the y picker means for them.
// For a dimension with a ceiling, the heights look down from the chosen
// level; elsewhere they are the top of each column.
int dpHeightTop(const DatapackWorld& world, int y);

// The detail of the heights the map shows at @scale (blocks per pixel): the
// terrain with its aquifers and surface rules up to 1:4; from 1:16 on the
// highest solid block with the global fluid levels only (dp::HEIGHTS_FAST),
// as the aquifers would cost several times the tile's time there.
dp::HeightDetail dpMapHeightDetail(int scale);

// Heights of a tile of w x h pixels at @scale with a one-pixel border, in the
// layout of sampleHeightGrid() ((w+2) x (h+2), pixel (i, j) at
// (j+1)*(w+2) + i+1). dp::NO_HEIGHT where a column has no block.
bool dpTileHeights(dp::Sampler *sampler, int scale, int x, int z, int w, int h,
    dp::HeightKind kind, int top, std::vector<int> *out, const std::atomic_bool *abort);
// Both kinds at once (the terrain layer), in the same layout.
bool dpTileBothHeights(dp::Sampler *sampler, int scale, int x, int z, int w, int h, int top,
    std::vector<int> *floor, std::vector<int> *surface, const std::atomic_bool *abort);

// Colours a tile of the height layer (one of the HV_ modes) from the biome
// colours in @rgb and the ocean floor heights of dpTileHeights().
void dpShadeHeights(unsigned char *rgb, int w, int h, int scale, const std::vector<int>& heights,
    int mode, int minY, int maxY, int level);

// Colours a tile of the terrain layer: vanilla biomes get the surface
// colours of the built-in terrain map, biomes of the packs their own colour;
// water where the surface is above the ocean floor, hill shading from the
// floor heights.
void dpTerrainStyle(unsigned char *rgb, const DatapackWorld& world, const int *biomes,
    int x, int z, int w, int h, int scale, uint64_t seed,
    const std::vector<int>& floor, const std::vector<int>& surface);

// The highest level below the roof of a dimension with a ceiling (the level
// to look down from), or INT_MIN if it has none or it cannot be found.
int dpBelowRoofLevel(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed);

#endif // DPMAP_H
