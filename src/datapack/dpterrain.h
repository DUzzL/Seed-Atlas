#ifndef DPTERRAIN_H
#define DPTERRAIN_H

// Terrain heights of a datapack world: the noise router's final_density
// evaluated the way the game's NoiseChunk fills a chunk with it.
//
// Internal to src/datapack.

#include "dpaquifer.h"
#include "dpdensity.h"

#include <QJsonObject>
#include <QString>

#include <climits>
#include <cstdint>
#include <vector>

namespace dp
{

// What a dimension's terrain needs, built once per world (immutable)
struct TerrainDef
{
    bool ok = false;
    int finalDensity = -1;      // node
    int cellWidth = 4;          // blocks: noise.size_horizontal * 4
    int cellHeight = 8;         // blocks: noise.size_vertical * 4
    // the noise range clamped to the dimension (NoiseSettings.clampToHeightAccessor)
    int minY = -64;
    int height = 384;
    int cellMinY = -8;          // floorDiv(minY, cellHeight)
    int cellCountY = 48;        // floorDiv(height, cellHeight)
    int seaLevel = 63;
    // the default block and fluid: what the fill puts where the density is
    // positive, and below the sea level where it is not
    enum Block { AIR, FLUID, SOLID };
    Block defaultBlock = SOLID;
    Block defaultFluid = FLUID;
    bool legacy = false;        // legacy_random_source
    // aquifers_enabled: the game's aquifers decide the blocks where the
    // density is not positive (local fluid levels, barriers). Where the
    // density is positive the block is the default block either way. Where
    // aquifer.on is false the fill only knows the global levels.
    bool aquifers = false;
    AquiferDef aquifer;

    // "interpolated" nodes that final_density reaches without passing
    // through another one: the game interpolates them between the corners
    // of the noise cells. Their arguments are evaluated at the corners.
    std::vector<int32_t> leaves;
    std::vector<int32_t> leafOf;        // node -> index into leaves, or -1
    // nodes of final_density outside of the leaves
    std::vector<char> inBlock;          // node -> 1 if it is one
    std::vector<char> blockVaries;      // node -> 1 if it changes with y or depends on a leaf
    std::vector<int32_t> blockColumn;   // the others, in evaluation order
    DensityProgram corner;              // the arguments of the leaves
    DensityProgram all;                 // everything (which noises to create)
    // arguments of every node for the lazy evaluation (CSR)
    std::vector<int32_t> argStart, args;
};

// Builds @def for the final_density node @finalDensity of the noise
// settings @settings in a dimension of the height range minY, height.
// Returns false with @reason if the terrain cannot be generated.
bool buildTerrain(const DensityGraph& graph, int finalDensity, const QJsonObject& settings,
    int dimMinY, int dimHeight, TerrainDef *def, QString *reason);

// Per-seed state (caches). Not thread safe: one per sampler.
class TerrainState
{
public:
    TerrainState(const DensityGraph& graph, const TerrainDef& def, uint64_t seed, int mc);
    ~TerrainState();

    // The highest block y at or below @top whose block counts: solid
    // (oceanFloor), or solid or fluid; NO_HEIGHT if none.
    int height(int x, int z, bool oceanFloor, int top);
    // height() of both kinds
    void bothHeights(int x, int z, int top, int *floor, int *surface);
    // For the surface rules, of the last column of bothHeights(): the game's
    // water height at the floor (the block above the highest fluid since
    // the last air above it, or INT_MIN) and whether the floor block is the
    // default block (1, 0, -1 not known)
    int lastWater() const { return last.water; }
    int lastFloorDefault() const { return last.floorDefault; }
    // Does @top cut the noise range (a dimension with a ceiling)?
    bool capped(int top) const { return top < def.minY + def.height - 1; }
    // block() with the default block and lava apart: 0 air, 1 a fluid but
    // lava, 2 the default block, 3 another solid block, 4 lava
    int blockKind(int x, int y, int z);
    // Are the aquifers modelled (heights and blocks with them)?
    bool hasAquifers() const { return aq != nullptr; }
    // Heights with the global fluid levels only, without the aquifers
    // (dp::HEIGHTS_FAST); block() keeps them
    void setFast(bool f) { fast = f; }
    // The blocks of the noise chunk of a single cell, as the game's
    // getBaseHeight() and getBaseColumn() make them (the structures), instead
    // of those of a chunk. Only the aquifers can differ (the flat caches of
    // the noise chunk cover the cell, not the chunk).
    void setBaseColumns(bool b) { baseColumns = b; }
    // The block at (x, y, z) after the fill: 0 air, 1 a fluid, 2 solid
    int block(int x, int y, int z);
    // For the heights of both kinds from one search: if a block that counts
    // for the ocean floor counts for the surface too (a solid default
    // block), returns true and sets @fluidTop to the highest fluid block of
    // the fill at or below @top (NO_HEIGHT if none). The surface height of
    // a column is then its floor where that is above @fluidTop, else
    // @fluidTop.
    bool surfaceFromFloor(int top, int *fluidTop) const;
    // final_density at a block > 0
    bool solid(int x, int y, int z);
    // Prepares the corner cache for a request of about @cells cell columns
    // in a row.
    void reserve(int cells);
    // Bound runs of cells before evaluating their corners: worth it where a
    // block column has its corners to itself (no neighbour in the request
    // shares them), not where the columns of a cell share them.
    void setCoarse(bool c) { coarse = c; }
    // The columns of a request share aquifer cells (see AquiferState::setDense())
    void setDenseColumns(bool d) { if (aq) aq->setDense(d); }
    // the state that evaluates the nodes (DensityState::abort)
    DensityState& density() { return ds; }
    // the aquifers (null without)
    AquiferState *aquifer() { return aq.get(); }

private:
    struct CornerColumn;
    const DensityGraph& graph;
    const TerrainDef& def;
    DensityState ds;            // the noises, and the arithmetic of the nodes
    std::unique_ptr<AquiferState> aq;
    bool baseColumns = false;
    bool fast = false;
    // the last column of bothHeights()
    struct { int x, z, top, floor, surface; bool base, valid; int water, floorDefault; } last =
        { 0, 0, 0, 0, 0, false, false, INT_MIN, -1 };
    int densityHeight(int x, int z, bool oceanFloor, int top);
    int lastHeight[2] = { -0x7fffffff, -0x7fffffff };  // of densityHeight() by kind (a hint; NO_HEIGHT)
    int hintScore = 0;          // > 0: the runs above the hint mostly hold
    unsigned hintProbe = 0;
    int hintMargin = 2;         // cells above the hint the run stops short of
    unsigned hintWins = 0;
    AquiferState::Range rangeOf(int x, int z) const;

    // corner context: the registers of a corner of a cell
    std::vector<double> regC;
    std::vector<uint32_t> stampC;
    uint32_t curC = 0;
    int cornerX = 0, cornerY = 0, cornerZ = 0;
    const CornerColumn *loaded = nullptr;   // whose column part regC holds

    // block context
    std::vector<double> regB;
    std::vector<uint32_t> stampB;
    uint32_t curB = 0;
    int blockX = 0, blockY = 0, blockZ = 0;
    int colX = 0x7fffffff, colZ = 0x7fffffff;   // block column whose column part regB holds
    // the cell of the block and the corners its leaves use
    CornerColumn *cc[4] = {};
    double fx = 0, fy = 0, fz = 0;
    // the bounds of the leaves in the cell of the block column, per leaf
    // and corner height (valid while cellStamp matches)
    const CornerColumn *cellCC[4] = {};
    int cellX = 0x7fffffff, cellZ = 0x7fffffff;
    uint32_t cellStamp = 1;
    int cellK = 0;              // corner y index of the cell's bottom

    // corner columns: direct mapped, (x index & (cacheW - 1), z index & 1)
    std::vector<CornerColumn*> cache;
    int cacheW = 0;

    // Bounds: an interval that holds every value a node takes over a range
    // of heights in the current block column (block context: the cell's
    // leaves between the extremes of their corner values) or corner column
    // (corner context). Used to skip whole cells that cannot be solid.
    struct Interval { double lo, hi; };
    std::vector<Interval> ivB;
    std::vector<Interval> cellLeafIv;
    std::vector<uint32_t> cellLeafStamp;
    std::vector<uint32_t> stampIB;
    uint32_t curIB = 0;
    int ivYlo = 0, ivYhi = 0;
    std::vector<double> scratch;        // exact argument values for compute()
    std::vector<double> noiseBound;     // by DensityGraph::noises index
    // corner context: the corner column cornerX, cornerZ (loaded in regC),
    // corner heights ivCylo .. ivCyhi
    std::vector<Interval> ivC;
    std::vector<uint32_t> stampIC;
    uint32_t curIC = 0;
    int ivCylo = 0, ivCyhi = 0;
    // block context: the leaves from their corners at the corner heights
    // ivKa .. ivKb bounded without evaluating them (else their exact values)
    bool boundLeaves = false;
    bool coarse = true;
    int ivKa = 0, ivKb = 0;
    template <bool CORNER> Interval interval(int32_t node);
    Interval leafBound(int leaf);
    void loadColumn(CornerColumn *c);
    bool cellMaySolid(int ylo, int yhi);
    bool cellsMaySolid(int ka, int kb, int ylo, int yhi);

    CornerColumn *cornerColumn(int x, int z);
    double cornerValue(CornerColumn *c, int leaf, int k);
    double evalC(int32_t node);
    double evalB(int32_t node);
    double leafValue(int leaf);
    void setBlockColumn(int x, int z);
    double density(int y);
    bool counts(int y, double d, bool oceanFloor) const;
};

} // namespace dp

#endif // DPTERRAIN_H
