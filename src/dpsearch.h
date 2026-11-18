#ifndef DPSEARCH_H
#define DPSEARCH_H

// Seed search conditions for datapack worlds, and the biome counts of the
// biome analysis in such worlds.
//
// A datapack world has a biome registry of its own (string ids from the
// packs, any number of them), so its conditions do not fit the vanilla
// Condition with its 256-bit biome sets. DpCondition keeps biome ids as
// strings; DpSearch compiles a list of them for one loaded world and tests
// seeds with the datapack generator (dp::Sampler).
//
// Conditions are combined with AND. Each one has a kind; the biome kinds are
// implemented now, and structure or height kinds can be added as further
// kinds (with their own fields and their own branch in DpSearch::test()).
//
// Every call into the datapack generator for searches and analyses lives in
// dpsearch.cpp.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <vector>

struct DatapackWorld;

struct DpCondition
{
    enum Kind
    {
        UNKNOWN = 0,        // written by a newer version: kept, never tested
        BIOME_IN_AREA,      // the biomes occur somewhere in an area
        BIOME_AT,           // the biome at one position
        BIOME_SHARE,        // the biomes cover a share of an area
        STRUCTURE_IN_AREA,  // structures start in an area
        // height kinds follow here
    };
    enum Match
    {
        ANY,                // area: one of them occurs; share: together; at: is one of them;
                            // structures: together at least count
        ALL,                // area: each occurs; share: each one alone; structures: each at
                            // least count
        NONE,               // area: none occurs; share: together at most; at: is none of them;
                            // structures: none starts, not even one that is not decided
    };
    enum Shape
    {
        CIRCLE,             // x, z, r
        SQUARE,             // x, z, r: from x-r to x+r
        RECT,               // x1, z1, x2, z2
    };
    // the topmost level of the dimension, where the surface biomes are
    static constexpr int Y_TOP = INT_MIN;

    int id = 0;             // 1..99, shown as "#id" (as the vanilla conditions)
    bool enabled = true;
    Kind kind = BIOME_IN_AREA;
    Match match = ANY;
    QStringList biomes;     // biome ids, "namespace:path"
    Shape shape = CIRCLE;
    int x = 0, z = 0, r = 500;
    int x1 = -500, z1 = -500, x2 = 500, z2 = 500;
    int y = Y_TOP;          // block level, or Y_TOP
    int scale = 4;          // sampling grid in blocks: 4 (the biome cells), 16 or 64
    int percent = 50;       // BIOME_SHARE
    // STRUCTURE_IN_AREA: the structure ids, how many starts (start positions:
    // a structure set's chunk) at least, and whether starts that the app
    // cannot decide count (or only valid ones)
    QStringList structures;
    int count = 1;
    bool unknownCounts = false;

    // the original line of a condition of an unknown kind (saved again as it was)
    QJsonObject raw;

    // the block area that the condition looks at (inclusive bounds)
    void bounds(int *bx1, int *bz1, int *bx2, int *bz2) const;
    bool hasArea() const
    {
        return kind == BIOME_IN_AREA || kind == BIOME_SHARE || kind == STRUCTURE_IN_AREA;
    }
    bool isStructure() const { return kind == STRUCTURE_IN_AREA; }

    // Session lines: "#DpCond: " followed by a JSON object.
    QString toLine() const;
    static bool fromLine(const QString& line, DpCondition *out);

    bool operator==(const DpCondition& o) const;
    bool operator!=(const DpCondition& o) const { return !(*this == o); }
};

// Names, colours and the biomes of the shown dimension, for texts and pickers.
struct DpBiomeNames
{
    std::shared_ptr<const DatapackWorld> world;
    // the display name of a biome id; the id itself if the world does not have it
    QString name(const QString& id) const;
    bool has(const QString& id) const;
    int index(const QString& id) const;
    // the same for structure ids
    QString structureName(const QString& id) const;
    int structureIndex(const QString& id) const;
};

// One sentence for the condition list ("Amber Plains or Jade Thicket within
// 500 blocks of 0, 0").
QString dpConditionText(const DpCondition& c, const DpBiomeNames& names);
// The name of a kind, for the type picker.
QString dpKindName(DpCondition::Kind kind);

class DpSearch
{
public:
    // Compiles the enabled conditions for a world. Returns null and sets
    // @error if the world cannot be searched or a condition is not valid;
    // @warning names biomes the world does not have (they are left out).
    static std::shared_ptr<const DpSearch> compile(
        const std::shared_ptr<const DatapackWorld>& world,
        const std::vector<DpCondition>& conditions, QString *error, QString *warning = nullptr);

    enum Result { FAILED, OK, ABORTED };
    // Tests a seed in the calling thread (it keeps a sampler per thread).
    // ABORTED: @abort was set during the test, the result is not known.
    Result test(uint64_t seed, const std::atomic_bool *abort) const;

    const std::shared_ptr<const DatapackWorld>& world() const { return dpworld; }
    // number of enabled conditions
    int size() const { return (int) conds.size(); }

private:
    struct Tile { int x, z, w, h; };
    struct Compiled
    {
        DpCondition c;
        std::vector<char> mask;     // per biome of the world: listed
        std::vector<int> listed;    // indices of the listed biomes
        int y = 0;                  // resolved level
        // the cell area (inclusive), in units of c.scale
        int cx1 = 0, cz1 = 0, cx2 = 0, cz2 = 0;
        int64_t cells = 0;          // cells inside the shape
        double cost = 0;            // for the order of the tests
        std::vector<Tile> tiles;    // of the area, nearest to its centre first
        std::vector<int> slot;      // per biome of the world: index into listed, or -1
        // (structure conditions: the same per structure of the world)
    };
    Result testArea(const Compiled& k, void *sampler, const std::atomic_bool *abort) const;
    Result testStructures(const Compiled& k, void *finder, const std::atomic_bool *abort) const;
    bool cellInside(const Compiled& k, int i, int j) const;

    std::shared_ptr<const DatapackWorld> dpworld;
    int nbiomes = 0;
    std::vector<Compiled> conds;
};

// --- the biome analysis (Biomes tool) in a datapack world ---

// The biomes of an area at a scale (as dp::Sampler::genBiomes(), y in blocks),
// in the calling thread. Writes -1 where there is no biome. False if aborted
// or the world cannot be shown.
bool dpGenBiomes(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed,
    int *out, int scale, int x, int z, int w, int h, int y, const std::atomic_bool *abort);
// The biome of one cell at a scale (y in blocks), -1 if none.
int dpBiomeAt(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed,
    int scale, int x, int y, int z);
// The number of biomes of the world (indices of dpGenBiomes() are below it).
int dpBiomeCount(const std::shared_ptr<const DatapackWorld>& world);

#endif // DPSEARCH_H
