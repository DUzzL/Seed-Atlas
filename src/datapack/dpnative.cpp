#include "dpworld_p.h"

extern "C"
{
#include "biomes.h"
// The engine's tables of the vanilla overworld parameter tree, one per
// group of versions (static data of headers that biomenoise.c includes).
#include "tables/btree18.h"
#include "tables/btree192.h"
#include "tables/btree19.h"
#include "tables/btree20.h"
#include "tables/btree21wd.h"
}

#include "dplistorder.h"

namespace dp
{

// The entries of the vanilla overworld list of a version, read from the
// engine's table of that version (the list itself is in the game's code, not
// in its data pack). They are in the order of the engine's tree, not in list
// order, and so only good for questions that do not depend on the order:
// which entries are nearest to a climate, and whether they are of one biome.
// @groups gets the engine's biome id of each entry, or a unique negative
// number where the engine decides the biome of the entry later.
// @nodeOfEntry and @entryOfNode translate between these entries and the
// node numbers of the engine's tree (what climateToBiome() takes and returns
// as its search hint).
bool nativeOverworldEntries(int mc, std::vector<ClimateEntry> *entries, std::vector<int32_t> *groups,
    std::vector<int32_t> *nodeOfEntry, std::vector<int32_t> *entryOfNode)
{
    // the same choice as getBiomeTree() in the engine's biomenoise.c
    const uint64_t *nodes;
    const int32_t (*param)[2];
    size_t len;
    if (mc >= MC_1_21_4)      { nodes = btree21wd_nodes; param = btree21wd_param; len = sizeof(btree21wd_nodes) / 8; }
    else if (mc >= MC_1_20_6) { nodes = btree20_nodes;   param = btree20_param;   len = sizeof(btree20_nodes) / 8; }
    else if (mc >= MC_1_19_4) { nodes = btree19_nodes;   param = btree19_param;   len = sizeof(btree19_nodes) / 8; }
    else if (mc >= MC_1_19_2) { nodes = btree192_nodes;  param = btree192_param;  len = sizeof(btree192_nodes) / 8; }
    else if (mc >= MC_1_18)   { nodes = btree18_nodes;   param = btree18_param;   len = sizeof(btree18_nodes) / 8; }
    else
        return false;
    // Later versions change the biome of some entries (or add entries) in the
    // engine's lookup, not in the table. 26.1 - 26.1.2 have the list of
    // 1.21.11 (their biome_parameters reports are the same).
    if (mc > MC_26_1_2)
        return false;

    entries->clear();
    groups->clear();
    nodeOfEntry->clear();
    entryOfNode->assign(len, -1);
    for (size_t i = 0; i < len; i++)
    {
        const uint64_t node = nodes[i];
        if ((node >> 56) != 0xFF)
            continue;   // an inner node
        (*entryOfNode)[i] = (int32_t) entries->size();
        nodeOfEntry->push_back((int32_t) i);
        ClimateEntry e;
        for (int d = 0; d < 6; d++)
        {
            const int32_t *p = param[(node >> (8 * d)) & 0xFF];
            e.lo[d] = p[0];
            e.hi[d] = p[1];
        }
        e.lo[CLIMATE_OFFSET] = e.hi[CLIMATE_OFFSET] = 0;
        int biome = (int) ((node >> 48) & 0xFF);
        // 1.21.5 turned some Dark Forest entries into Pale Garden ones, which
        // the engine tells apart when it looks a climate up: such an entry
        // counts as a biome of its own here.
        if (mc >= MC_1_21_5 && biome == dark_forest)
            biome = -1 - (int) entries->size();
        entries->push_back(e);
        groups->push_back(biome);
    }
    return !entries->empty();
}

// The vanilla overworld list of 26.2 and later in the game's order (see
// dplistorder.h), with the engine's biome id of every entry. The engine looks
// these versions up in the tree of 1.21.5 and corrects the result; the game
// builds a new tree from the list, which tells exact ties apart in another
// way (and from 26.3 with another number of children per node).
bool nativeOverworldList(int mc, std::vector<ClimateEntry> *entries, std::vector<int32_t> *ids)
{
    if (mc < MC_26_2)
        return false;
    const size_t len = sizeof(btree21wd_nodes) / 8;
    entries->clear();
    ids->clear();
    for (size_t k = 0; k <= sizeof(dp_order_btree21wd) / sizeof(dp_order_btree21wd[0]); k++)
    {
        ClimateEntry e;
        e.lo[CLIMATE_OFFSET] = e.hi[CLIMATE_OFFSET] = 0;
        if (k == DP_ORDER_SULFUR)
        {
            // OverworldBiomeBuilder of 26.2: the Sulfur Caves entry (the same
            // parameters as the engine's correction in climateToBiome())
            static const int32_t sulfur[6][2] = {
                { -10000, 10000 }, { -10000, 10000 }, { -1900, 5500 },
                { 4500, 10000 }, { 2000, 9000 }, { -11000, -8500 },
            };
            for (int d = 0; d < 6; d++)
            {
                e.lo[d] = sulfur[d][0];
                e.hi[d] = sulfur[d][1];
            }
            entries->push_back(e);
            ids->push_back(sulfur_caves);
        }
        if (k == sizeof(dp_order_btree21wd) / sizeof(dp_order_btree21wd[0]))
            break;
        const size_t node = dp_order_btree21wd[k];
        if (node >= len || (btree21wd_nodes[node] >> 56) != 0xFF)
            return false;
        uint64_t mid[6];
        for (int d = 0; d < 6; d++)
        {
            const int32_t *p = btree21wd_param[(btree21wd_nodes[node] >> (8 * d)) & 0xFF];
            e.lo[d] = p[0];
            e.hi[d] = p[1];
            mid[d] = (uint64_t) (int64_t) ((p[0] + p[1]) / 2);
        }
        // The biome of the leaf in this version (the engine relabels some
        // entries): a search that starts on the leaf, at a climate inside
        // it, stays there. (No such climate is inside the Sulfur Caves box
        // with Deep Dark as the leaf: Deep Dark is at depth 1.1.)
        uint64_t dat = node;
        int id = climateToBiome(mc, mid, &dat);
        if (dat != node)
            return false;
        entries->push_back(e);
        ids->push_back(id);
    }
    return true;
}

// Children per node of the game's climate search tree
int climateArity(int mc)
{
    if (mc >= MC_26_3)
        return 19;
    return mc >= MC_1_19_4 ? 6 : 10;
}

} // namespace dp
