#ifndef DPAQUIFER_H
#define DPAQUIFER_H

// The aquifers of a datapack world: what the game's Aquifer.NoiseBasedAquifer
// puts where final_density is not positive (local fluid levels, barriers of
// the default block between them), for the terrain heights (dpterrain.cpp).
//
// Internal to src/datapack.

#include "dpdensity.h"

#include <QJsonObject>

#include <atomic>
#include <QString>

#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dp
{

// The blocks the aquifer and the global fluid picker give. OTHER is a
// default fluid that is neither air, water nor lava (a solid block for the
// heightmaps); BARRIER is the default block.
enum FluidKind : uint8_t { FK_AIR, FK_WATER, FK_LAVA, FK_OTHER, FK_BARRIER };

// What the aquifers of a dimension need, built once per world (immutable)
struct AquiferDef
{
    // the game's code changed: 1.18.2 (raw router functions, no deep dark
    // check, the scanning preliminary surface), 1.19.2 (the old order of the
    // surface samples), 1.19.4 (the center sampled first), 1.21.9 (the
    // preliminary_surface_level field and the skip above the surface)
    enum Variant { V1_18_2, V1_19_2, V1_19_4, V1_21_9 };

    bool on = false;        // aquifers_enabled and modelled
    QString reason;         // aquifers_enabled but not modelled: why
    Variant variant = V1_19_4;

    // Functions, each with a program of its own. A function evaluated at a
    // single point inside the range of the noise chunk's flat caches takes
    // their values (the normal compile, [0]), outside of it the direct ones
    // (the plain compile, [1]); where both compile the same, the range does
    // not matter (the index of [1] is that of [0]).
    enum Slot { BARRIER, CENTER0, CENTER1, SPREAD0, SPREAD1, LAVA0, LAVA1, SURFACE, SLOTS };
    int program[SLOTS];     // index into programs per slot
    std::vector<DensityProgram> programs;
    int barrier = -1;       // at the block
    int flood[2] = { -1, -1 }, erosion[2] = { -1, -1 }, depth[2] = { -1, -1 };   // at an aquifer's center
    int spread[2] = { -1, -1 }, lava[2] = { -1, -1 };
    int surface = -1;       // initial_density_without_jaggedness or preliminary_surface_level
    // The surface's scan (before 1.21.9): the cache_2d nodes whose argument
    // changes with y keep the value of the scan's first height (the game's
    // Cache2D holds one value per column); the nodes of the surface's
    // vertical part the other heights evaluate, in order (the caches
    // included, which keep their value).
    std::vector<int32_t> surfaceCaches;
    std::vector<int32_t> surfaceScan;
    std::vector<char> isSurfaceCache;   // per node
    bool rangeCenter = false, rangeSpread = false, rangeLava = false;
    // the global fluid picker
    int seaLevel = 63;
    uint8_t defaultFluid = FK_WATER;
    // the noise settings, for the preliminary surface
    int minY = -64, height = 384, cellHeight = 8;
    double slide[9] = {};   // 1.18.2: cell height, min cell y, cell count y, top target, size, offset, bottom ...
    bool floorSaturates = false;    // Mth.floor() of 26.1 and later
    bool legacy = false;            // legacy_random_source
};

// Builds @def for the noise settings @settings (aquifers_enabled) whose
// final_density the terrain @finalDensity of the cell size @cellHeight
// fills. Compiles the router fields the aquifer uses. The aquifers are not
// modelled (def->on false, def->reason says why) where the game's result
// would depend on what this model leaves out.
void buildAquifer(DensityGraph& graph, const QJsonObject& settings, int pack, const QString& id,
    int legacySettings, int mc, int minY, int height, int cellHeight, bool solidDefaultBlock, AquiferDef *def);

// Per-seed state (caches). Not thread safe: one per terrain state.
class AquiferState
{
public:
    AquiferState(const DensityGraph& graph, const AquiferDef& def, uint64_t seed, int mc);
    ~AquiferState();

    // The noise chunk whose blocks are asked for: the range of its flat
    // caches in quarts (first quart x, z and their count - 1), and its chunk
    // (the skip above the surface of 1.21.9 and later).
    struct Range { int qx = 0, qz = 0, n = 4, chunkX = 0, chunkZ = 0; };
    void setColumn(int x, int z, const Range& range);
    // The lowest y of the current column above which every block whose
    // density is not positive is what the global fluid picker gives (no
    // barrier, no local level).
    int globalAbove();
    // The block at y of the current column where the density is not
    // positive; density(y) gives it, asked only if it matters
    template <class D> uint8_t substance(int y, D density);
    // The global fluid picker's block at y
    uint8_t global(int y) const;

    // the preliminary surface of the column (x, z) (quantized to quarts)
    int preliminarySurface(int x, int z);

    // The blocks of the current column whose cell layer is @ya (the heights
    // y with floorDiv(y + 1, 12) == ya; their nearest centers are in the
    // layers ya - 1 .. ya + 1) where the density is not positive: from
    // globalFrom up the global picker's (air). If uniform, those centers
    // all have one status: no barrier, every block is the global picker's
    // lava or else fluid() of that status.
    struct LayerInfo { int globalFrom; bool uniform; int level; uint8_t type; bool exact; bool set; };
    // @exact: with every status of those centers (else the bounds of the
    // levels the caches do not hold; exact if they hold all)
    const LayerInfo& layer(int ya, bool exact);
    bool isDense() const { return dense; }
    uint8_t lavaOr(int y, uint8_t fluid) const;
    // The columns asked for share aquifer cells (a map at fine scales): the
    // layers compute every status, which the next columns take from the
    // caches; else only those the caches hold
    void setDense(bool d) { dense = d; }
    // The flag of DensityState::abort for long scans inside the functions;
    // what was found while it is set is not kept
    void setAbort(const std::atomic_bool *abort);

private:
    struct Status { int level; uint8_t type; };
    struct Center { int x, y, z; Status st; bool hasStatus; };
    const DensityGraph& graph;
    const AquiferDef& def;
    int mc;
    std::vector<std::unique_ptr<DensityState>> states;  // per program
    uint64_t posLo = 0, posHi = 0;      // aquiferRandom (xoroshiro), or posLo the legacy seed
    // direct mapped caches (the values are functions of the key: an entry
    // that another key takes is just found again)
    struct SurfaceEntry { int32_t qx, qz, v; bool used; };
    std::vector<SurfaceEntry> surfaceCache;
    int lastSurface = INT_MAX;      // the last preliminary surface computed (a hint)
    struct StatusEntry { uint64_t key; Status st; bool used; };
    std::vector<StatusEntry> statusCache;
    const std::atomic_bool *abortFlag = nullptr;
    bool aborted() const { return abortFlag && abortFlag->load(std::memory_order_relaxed); }
    std::unordered_map<uint64_t, int> skipCache;

    // the current column
    int colX = 0, colZ = 0, gx0 = 0, gz0 = 0;
    Range range;
    int skipY = 0x7fffffff;
    bool skipKnown = false;
    int skipLow = INT_MIN;          // a lower bound of skipY
    double barrierBound = HUGE_VAL; // |barrier| <= barrierBound everywhere
    int boundY = 0x7fffffff;
    bool boundKnown = false;
    int boundGX = 0x7fffffff, boundGZ = 0x7fffffff, boundValue = 0;
    // the column's centers by layer: (gy - layer0) * 4 + x1 * 2 + z1
    std::vector<Center> centers;
    std::vector<LayerInfo> layerInfo;    // by layer, as centers
    std::vector<char> centerSet;
    int layer0 = 0, layers = 0;
    // the tables of recently left anchor cells
    struct Ctx
    {
        bool valid = false;
        int gx0 = 0, gz0 = 0;
        Range range;
        uint64_t used = 0;
        std::vector<Center> centers;
        std::vector<char> centerSet;
        std::vector<LayerInfo> layerInfo;
        int layer0 = 0, layers = 0;
    };
    std::vector<Ctx> ctxs;
    bool dense = false;
    uint64_t ctxTick = 0;
    struct BoundEntry { int gx, gz, p; bool used; };
    std::vector<BoundEntry> boundTable;
    int barrierX = 0x7fffffff, barrierZ = 0x7fffffff;

    DensityState& at(int slot, int x, int y, int z);
    bool inRange(int x, int z) const;
    void layersFor(int gyLo, int gyHi);
    Center& center(int gx, int gy, int gz);
    const Status& status(Center& c);
    bool peekStatus(Center& c, Status *out);
    uint64_t statusKey(const Center& c) const;
    Status computeStatus(int x, int y, int z, int mask);
    Status scratchStatus;           // a status found while aborted (not kept)
    LayerInfo scratchLayer;
    int surfaceLevel(int x, int y, int z, const Status& global, int lowest, bool flooded, int mask);
    double pressure(int y, double *barrier, const Status& a, const Status& b);
    Status picker(int y) const;
    int computeSkip(int chunkX, int chunkZ);
    int levelBound();
    double absBound(const DensityState& s, int node, std::vector<double>& memo);
    uint8_t closest(int y, int *d1, int *d2, int *d3, Center **c1, Center **c2, Center **c3);

    // bounds of the preliminary surface function over a range of heights of
    // the column the surface state holds
    struct Interval { double lo, hi; };
    std::vector<Interval> iv;
    std::vector<double> ivScratch;
    std::vector<double> noiseBound;
    std::vector<int> ivArgs;
    int ivYlo = 0, ivYhi = 0;
    Interval interval(const DensityState& s, int32_t node);
    bool mayExceed(DensityState& s, int ylo, int yhi, double threshold);
};

// substance(): the barriers need the density only where the pressure is
// positive (the density is not positive here)
template <class D>
uint8_t AquiferState::substance(int y, D density)
{
    int d1, d2, d3;
    Center *c1, *c2, *c3;
    const uint8_t early = closest(y, &d1, &d2, &d3, &c1, &c2, &c3);
    if (early != 0xff)
        return early;
    // (the statuses by value: computing one may clear the cache)
    const Status s1 = status(*c1);
    const double sim12 = 1.0 - (double) std::abs(d2 - d1) / 25.0;
    const uint8_t fs = y < s1.level ? s1.type : FK_AIR;
    if (sim12 <= 0.0)
        return fs;
    if (fs == FK_WATER)
    {
        const Status gb = picker(y - 1);
        if (y - 1 < gb.level && gb.type == FK_LAVA)
            return fs;
    }
    double barrier = NAN, d = NAN;
    auto solid = [&](double b) {
        if (!(b > 0.0))
            return false;   // d + b > 0 needs b > 0
        if (d != d)
            d = density(y);
        return d + b > 0.0;
    };
    const Status s2 = status(*c2);
    if (solid(sim12 * pressure(y, &barrier, s1, s2)))
        return FK_BARRIER;
    const Status s3 = status(*c3);
    const double sim13 = 1.0 - (double) std::abs(d3 - d1) / 25.0;
    if (sim13 > 0.0 && solid(sim12 * sim13 * pressure(y, &barrier, s1, s3)))
        return FK_BARRIER;
    const double sim23 = 1.0 - (double) std::abs(d3 - d2) / 25.0;
    if (sim23 > 0.0 && solid(sim12 * sim23 * pressure(y, &barrier, s2, s3)))
        return FK_BARRIER;
    return fs;
}

} // namespace dp

#endif // DPAQUIFER_H
