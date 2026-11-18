#ifndef DPCLIMATE_H
#define DPCLIMATE_H

// The nearest-point search of the multi-noise biome source for arbitrary
// parameter lists: a port of the R-tree of Minecraft's Climate class (build
// and search), working on the game's quantised values.
//
// Internal to src/datapack.

#include <cstdint>
#include <limits>
#include <vector>

namespace dp
{

enum
{
    CLIMATE_TEMPERATURE,
    CLIMATE_HUMIDITY,
    CLIMATE_CONTINENTALNESS,
    CLIMATE_EROSION,
    CLIMATE_DEPTH,
    CLIMATE_WEIRDNESS,
    CLIMATE_OFFSET,     // only in entries: a point, the target is always 0
    CLIMATE_DIMS
};

// The game's quantisation of a climate value: (long)(value * 10000.0F), with
// Java's float-to-long conversion (NaN gives 0, out of range saturates).
inline int64_t quantizeClimate(float value)
{
    const float f = value * 10000.0F;
    if (f > -9.0e18F && f < 9.0e18F)
        return (int64_t) f;
    // Java's f2l: NaN gives 0, the rest saturates
    if (f != f)
        return 0;
    return f > 0 ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
}

// One entry of a parameter list: a box in the seven quantised dimensions.
struct ClimateEntry
{
    int64_t lo[CLIMATE_DIMS];
    int64_t hi[CLIMATE_DIMS];
};

class ClimateTree
{
public:
    // Builds the tree for the entries in list order. @arity is the number of
    // children per node of the game version (6 from 1.19.4, 10 before).
    // Returns false if there are no entries.
    bool build(const std::vector<ClimateEntry>& entries, int arity);

    bool empty() const { return nodes.empty(); }
    size_t entryCount() const { return nentries; }

    // The index (in list order) of the entry that the game finds for the
    // quantised target (six values, the offset target is 0).
    //
    // @cursor is the game's "last result": the search starts from the leaf of
    // the previous lookup and only replaces it by a strictly closer one, so
    // where entries overlap or tie the result depends on the lookup before.
    // Pass a cursor initialised to -1 for a search without history; it is
    // updated to the found leaf.
    int find(const int64_t target[6], int *cursor) const;

    // The same result as find() with a cursor of -1 (a search without
    // history), but faster: @hint may hold the leaf of an earlier lookup,
    // which only bounds the search. It is set to the found leaf, which can
    // serve as the cursor of a following find().
    int findFresh(const int64_t target[6], int *hint) const;

    // Tells the tree the biome (any number) of every entry, for
    // findNearest(). Entries with the same box and the same biome are
    // interchangeable: the game's search behaves the same on either.
    void setGroups(const std::vector<int32_t>& groups);

    // An entry that is nearest to the target (any of them, not the one the
    // game's search order would give), its distance, and what else is that
    // near: @tie if an entry is that is not interchangeable with it, @rival
    // if an entry of another biome is.
    // Without a tie the game's search ends on this entry (or one like it)
    // whatever it started from; without a rival the biome of the target is
    // that of this entry. @hint as in findFresh(). Needs setGroups().
    struct Nearest
    {
        int entry = -1;
        int64_t dist = 0;
        bool tie = false;
        bool rival = false;
    };
    Nearest findNearest(const int64_t target[6], int *hint) const;

    // The node of an entry, as a hint or cursor for the searches above.
    int nodeOf(int entry) const { return leafOf[entry]; }

    // Squared distance of an entry to a target, as the game computes it.
    int64_t distance(int entry, const int64_t target[6]) const;

private:
    struct Node
    {
        int32_t lo[CLIMATE_DIMS];
        int32_t hi[CLIMATE_DIMS];
        int32_t first;  // inner node: index of the first child; leaf: entry index
        int32_t count;  // number of children, 0 for a leaf
    };
    std::vector<Node> nodes;        // nodes[0] is the root, children are contiguous
    std::vector<int32_t> leafOf;    // entry index -> node index
    std::vector<int32_t> group;     // entry index -> biome (setGroups())
    std::vector<int32_t> kind;      // entry index -> number of its box and biome
    size_t nentries = 0;

    void search(const Node& node, const int64_t *t, int& best, int64_t& bestDist) const;
    struct Ties;
    void searchTies(const Node& node, const int64_t *t, Ties& s) const;
};

} // namespace dp

#endif // DPCLIMATE_H
