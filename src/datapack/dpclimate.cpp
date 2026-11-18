#include "dpclimate.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

// Port of net.minecraft.world.level.biome.Climate (RTree, Parameter,
// ParameterPoint) of Minecraft 1.21.1, checked against the game's bytecode.
// The TypeScript port in deepslate (Copyright (c) 2021 Misode, MIT License)
// served as a readable reference; where the two differ (node arity, integer
// arithmetic, no early exit on distance 0) this follows the game.

namespace dp
{

namespace
{

struct BuildNode
{
    int64_t lo[CLIMATE_DIMS];
    int64_t hi[CLIMATE_DIMS];
    std::vector<BuildNode*> children;   // empty for a leaf
    int entry = -1;
};

typedef std::deque<BuildNode> Pool;

inline int64_t absl(int64_t v) { return v < 0 ? -v : v; }

inline int64_t centre(const BuildNode *n, int dim, bool absolute)
{
    int64_t c = (n->lo[dim] + n->hi[dim]) / 2;   // truncating, like Java
    return absolute ? absl(c) : c;
}

// RTree.sort(): by the centre of dimension @dim, ties by the following
// dimensions in cyclic order. Stable, like List.sort().
void sortNodes(std::vector<BuildNode*>& nodes, int dim, bool absolute)
{
    std::stable_sort(nodes.begin(), nodes.end(),
        [dim, absolute](const BuildNode *a, const BuildNode *b) {
            for (int i = 0; i < CLIMATE_DIMS; i++)
            {
                int d = (dim + i) % CLIMATE_DIMS;
                int64_t ca = centre(a, d, absolute);
                int64_t cb = centre(b, d, absolute);
                if (ca != cb)
                    return ca < cb;
            }
            return false;
        });
}

BuildNode *makeSubTree(Pool& pool, const std::vector<BuildNode*>& children)
{
    pool.emplace_back();
    BuildNode *n = &pool.back();
    n->children = children;
    for (int d = 0; d < CLIMATE_DIMS; d++)
    {
        n->lo[d] = children[0]->lo[d];
        n->hi[d] = children[0]->hi[d];
        for (const BuildNode *c : children)
        {
            n->lo[d] = std::min(n->lo[d], c->lo[d]);
            n->hi[d] = std::max(n->hi[d], c->hi[d]);
        }
    }
    return n;
}

// RTree.bucketize(): consecutive groups of arity^floor(log_arity(size - 0.01))
std::vector<BuildNode*> bucketize(Pool& pool, const std::vector<BuildNode*>& nodes, int arity)
{
    double e = std::floor(std::log((double) nodes.size() - 0.01) / std::log((double) arity));
    size_t n = 1;
    for (int i = 0; i < (int) e; i++)
        n *= (size_t) arity;
    std::vector<BuildNode*> buckets;
    std::vector<BuildNode*> cur;
    for (BuildNode *node : nodes)
    {
        cur.push_back(node);
        if (cur.size() < n)
            continue;
        buckets.push_back(makeSubTree(pool, cur));
        cur.clear();
    }
    if (!cur.empty())
        buckets.push_back(makeSubTree(pool, cur));
    return buckets;
}

int64_t cost(const BuildNode *n)
{
    int64_t c = 0;
    for (int d = 0; d < CLIMATE_DIMS; d++)
        c += absl(n->hi[d] - n->lo[d]);
    return c;
}

BuildNode *buildNode(Pool& pool, std::vector<BuildNode*>& nodes, int arity)
{
    if (nodes.size() == 1)
        return nodes[0];
    if ((int) nodes.size() <= arity)
    {
        std::stable_sort(nodes.begin(), nodes.end(),
            [](const BuildNode *a, const BuildNode *b) {
                int64_t ka = 0, kb = 0;
                for (int d = 0; d < CLIMATE_DIMS; d++)
                {
                    ka += centre(a, d, true);
                    kb += centre(b, d, true);
                }
                return ka < kb;
            });
        return makeSubTree(pool, nodes);
    }

    int64_t best = std::numeric_limits<int64_t>::max();
    int bestDim = -1;
    std::vector<BuildNode*> bestBuckets;
    for (int d = 0; d < CLIMATE_DIMS; d++)
    {
        // sorts in place: each sort starts from the order of the one before
        sortNodes(nodes, d, false);
        std::vector<BuildNode*> buckets = bucketize(pool, nodes, arity);
        int64_t c = 0;
        for (const BuildNode *b : buckets)
            c += cost(b);
        if (best > c)
        {
            best = c;
            bestDim = d;
            bestBuckets = buckets;
        }
    }
    sortNodes(bestBuckets, bestDim, true);
    std::vector<BuildNode*> children;
    for (BuildNode *b : bestBuckets)
        children.push_back(buildNode(pool, b->children, arity));
    return makeSubTree(pool, children);
}

inline int32_t toInt32(int64_t v)
{
    // the game only accepts parameters within -2 .. 2 (quantised +-20000)
    const int64_t lim = 1 << 29;
    return (int32_t) std::max(-lim, std::min(lim, v));
}

} // namespace

bool ClimateTree::build(const std::vector<ClimateEntry>& entries, int arity)
{
    nodes.clear();
    leafOf.clear();
    nentries = entries.size();
    if (entries.empty())
        return false;
    if (arity < 2)
        arity = 2;

    Pool pool;
    std::vector<BuildNode*> leaves;
    for (size_t i = 0; i < entries.size(); i++)
    {
        pool.emplace_back();
        BuildNode *n = &pool.back();
        for (int d = 0; d < CLIMATE_DIMS; d++)
        {
            n->lo[d] = toInt32(entries[i].lo[d]);
            n->hi[d] = toInt32(entries[i].hi[d]);
        }
        n->entry = (int) i;
        leaves.push_back(n);
    }
    BuildNode *root = buildNode(pool, leaves, arity);

    // flatten: the children of a node are contiguous, in search order
    leafOf.assign(entries.size(), -1);
    std::vector<const BuildNode*> src;
    nodes.emplace_back();
    src.push_back(root);
    for (size_t i = 0; i < src.size(); i++)
    {
        const BuildNode *b = src[i];
        Node n;
        for (int d = 0; d < CLIMATE_DIMS; d++)
        {
            n.lo[d] = (int32_t) b->lo[d];
            n.hi[d] = (int32_t) b->hi[d];
        }
        if (b->children.empty())
        {
            n.first = b->entry;
            n.count = 0;
            leafOf[b->entry] = (int32_t) i;
        }
        else
        {
            n.first = (int32_t) nodes.size();
            n.count = (int32_t) b->children.size();
            for (const BuildNode *c : b->children)
            {
                nodes.emplace_back();
                src.push_back(c);
            }
        }
        nodes[i] = n;
    }
    return true;
}

namespace
{

// Sum over the six target dimensions and the offset of the squared distance
// to the box. Stops once @limit is reached: the caller only asks whether the
// distance is smaller than @limit.
template <class N>
inline int64_t nodeDistance(const N& n, const int64_t *t, int64_t limit)
{
    int64_t sum = 0;
    for (int d = 0; d < 6; d++)
    {
        int64_t a = t[d] - n.hi[d];
        int64_t b = n.lo[d] - t[d];
        int64_t v = a > 0 ? a : (b > 0 ? b : 0);
        sum += v * v;
    }
    if (sum >= limit)
        return sum;
    // the offset: the target is 0
    int64_t a = -(int64_t) n.hi[6];
    int64_t b = n.lo[6];
    int64_t v = a > 0 ? a : (b > 0 ? b : 0);
    return sum + v * v;
}

} // namespace

void ClimateTree::search(const Node& node, const int64_t *t, int& best, int64_t& bestDist) const
{
    const Node *c = &nodes[node.first];
    const Node *end = c + node.count;
    for (; c != end; ++c)
    {
        int64_t m = nodeDistance(*c, t, bestDist);
        if (bestDist <= m)
            continue;
        if (c->count == 0)
        {
            best = (int) (c - nodes.data());
            bestDist = m;
        }
        else
        {
            search(*c, t, best, bestDist);
        }
    }
}

int ClimateTree::find(const int64_t target[6], int *cursor) const
{
    if (nodes.empty())
        return -1;
    const Node& root = nodes[0];
    if (root.count == 0)
        return root.first;

    // Keep the squares inside 64 bits for targets that are far out of any
    // sensible range (infinite or huge climate values).
    const int64_t lim = (int64_t) 1 << 29;
    int64_t t[6];
    for (int d = 0; d < 6; d++)
        t[d] = std::max(-lim, std::min(lim, target[d]));

    int best = cursor ? *cursor : -1;
    int64_t bestDist = std::numeric_limits<int64_t>::max();
    if (best >= 0)
        bestDist = nodeDistance(nodes[best], t, bestDist);
    if (bestDist != 0)
        search(root, t, best, bestDist);
    if (cursor)
        *cursor = best;
    return nodes[best].first;
}

int ClimateTree::findFresh(const int64_t target[6], int *hint) const
{
    if (nodes.empty())
        return -1;
    const Node& root = nodes[0];
    if (root.count == 0)
        return root.first;

    const int64_t lim = (int64_t) 1 << 29;
    int64_t t[6];
    for (int d = 0; d < 6; d++)
        t[d] = std::max(-lim, std::min(lim, target[d]));

    // A search without history returns the first leaf (in search order) of
    // the smallest distance. No leaf further away than the hint can be that
    // one, so the search may start with the hint's distance + 1 as its bound
    // and finds the same leaf while pruning more.
    int best = -1;
    int64_t bestDist = std::numeric_limits<int64_t>::max();
    if (hint && *hint >= 0)
        bestDist = nodeDistance(nodes[*hint], t, bestDist) + 1;
    search(root, t, best, bestDist);
    if (hint)
        *hint = best;
    return nodes[best].first;
}

void ClimateTree::setGroups(const std::vector<int32_t>& groups)
{
    group = groups;
    group.resize(nentries, -1);
    kind.assign(nentries, -1);
    // entries with the same box and biome get the same number
    std::vector<int32_t> order(nentries);
    for (size_t i = 0; i < nentries; i++)
        order[i] = (int32_t) i;
    auto less = [this](int32_t a, int32_t b) {
        const Node& na = nodes[leafOf[a]];
        const Node& nb = nodes[leafOf[b]];
        for (int d = 0; d < CLIMATE_DIMS; d++)
        {
            if (na.lo[d] != nb.lo[d]) return na.lo[d] < nb.lo[d];
            if (na.hi[d] != nb.hi[d]) return na.hi[d] < nb.hi[d];
        }
        return group[a] < group[b];
    };
    std::sort(order.begin(), order.end(), less);
    int32_t k = -1;
    for (size_t i = 0; i < order.size(); i++)
    {
        if (i == 0 || less(order[i - 1], order[i]))
            k++;
        kind[order[i]] = k;
    }
}

struct ClimateTree::Ties
{
    int best;
    int64_t bestDist;
    int32_t group;
    int32_t kind;
    bool tie;
    bool rival;
};

void ClimateTree::searchTies(const Node& node, const int64_t *t, Ties& s) const
{
    const Node *c = &nodes[node.first];
    const Node *end = c + node.count;
    for (; c != end; ++c)
    {
        // unlike the game's search this one has to see the entries that are
        // exactly as near as the best one
        const int64_t limit = s.bestDist == std::numeric_limits<int64_t>::max() ? s.bestDist : s.bestDist + 1;
        const int64_t m = nodeDistance(*c, t, limit);
        if (m > s.bestDist)
            continue;
        if (c->count != 0)
        {
            searchTies(*c, t, s);
            continue;
        }
        if (m < s.bestDist)
        {
            s.bestDist = m;
            s.best = (int) (c - nodes.data());
            s.group = group[c->first];
            s.kind = kind[c->first];
            s.tie = false;
            s.rival = false;
        }
        else if (kind[c->first] != s.kind)
        {
            s.tie = true;
            if (group[c->first] != s.group)
                s.rival = true;
        }
    }
}

ClimateTree::Nearest ClimateTree::findNearest(const int64_t target[6], int *hint) const
{
    Nearest r;
    if (nodes.empty() || kind.empty())
        return r;
    const Node& root = nodes[0];
    if (root.count == 0)
    {
        r.entry = root.first;
        return r;
    }

    const int64_t lim = (int64_t) 1 << 29;
    int64_t t[6];
    for (int d = 0; d < 6; d++)
        t[d] = std::max(-lim, std::min(lim, target[d]));

    Ties s;
    s.best = -1;
    s.bestDist = std::numeric_limits<int64_t>::max();
    s.group = 0;
    s.kind = -1;
    s.tie = false;
    s.rival = false;
    if (hint && *hint >= 0)
    {
        // start from the hint: the search meets it again, as its own kind
        s.best = *hint;
        s.bestDist = nodeDistance(nodes[*hint], t, s.bestDist);
        s.group = group[nodes[*hint].first];
        s.kind = kind[nodes[*hint].first];
    }
    searchTies(root, t, s);
    if (hint)
        *hint = s.best;
    r.entry = nodes[s.best].first;
    r.dist = s.bestDist;
    r.tie = s.tie;
    r.rival = s.rival;
    return r;
}

int64_t ClimateTree::distance(int entry, const int64_t target[6]) const
{
    if (entry < 0 || (size_t) entry >= leafOf.size())
        return -1;
    const int64_t lim = (int64_t) 1 << 29;
    int64_t t[6];
    for (int d = 0; d < 6; d++)
        t[d] = std::max(-lim, std::min(lim, target[d]));
    return nodeDistance(nodes[leafOf[entry]], t, std::numeric_limits<int64_t>::max());
}

} // namespace dp
