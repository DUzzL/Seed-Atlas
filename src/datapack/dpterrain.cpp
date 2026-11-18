#include "dpterrain.h"
#include "dpeval.h"
#include "dpinternal.h"
#include "dpworld.h"

#include <atomic>

#include <algorithm>
#include <cmath>

// The terrain fill of Minecraft 1.21.1 (net.minecraft.world.level.levelgen.
// NoiseChunk, NoiseBasedChunkGenerator.iterateNoiseColumn(), Aquifer),
// checked against the game's bytecode:
//  - final_density is wrapped as cache_all_in_cell(final_density +
//    beardifier); the beardifier (structures) is 0 here.
//  - The cells are size_horizontal * 4 blocks wide and size_vertical * 4
//    blocks high, from floorDiv(min_y, cell height), over the noise range
//    clamped to the dimension.
//  - An "interpolated" argument is evaluated at the eight corners of the
//    block's cell (directly: inner interpolated markers pass their argument
//    through) and interpolated with Mth.lerp3: x, then y, then z, with the
//    fractions (block - cell start) / cell size. The cell cache fills
//    final_density this way (NoiseChunk.fillingCell).
//  - flat_cache: the argument at the quart position (x and z rounded down to
//    multiples of 4) at y 0 (DF_QUART, DF_FLAT); cache_2d, cache_once,
//    cache_all_in_cell pass their argument through; blend_* as in a world
//    without old chunks.
//  - A block is the default block where the density is positive. Elsewhere
//    the global fluid picker of the chunk generator decides: lava below
//    min(-54, sea level), the default fluid below the sea level, else air;
//    with aquifers_enabled the aquifers (dpaquifer.cpp: local water and lava
//    levels, barriers of the default block).
// The arithmetic of the nodes is that of the climate evaluator; only the
// order of evaluation differs: range_choice and mul evaluate their second
// argument only when the game does (the values are the same).

namespace dp
{

static std::atomic<bool> g_prune { true };

void setTerrainPruning(bool prune)
{
    g_prune = prune;
}

bool terrainPruning()
{
    return g_prune.load(std::memory_order_relaxed);
}

namespace
{

inline double lerpD(double t, double a, double b)
{
    return a + t * (b - a);
}

// Mth.lerp3(dx, dy, dz, n000, n100, n010, n110, n001, n101, n011, n111)
inline double lerp3(double dx, double dy, double dz,
    double n000, double n100, double n010, double n110,
    double n001, double n101, double n011, double n111)
{
    double a = lerpD(dy, lerpD(dx, n000, n100), lerpD(dx, n010, n110));
    double b = lerpD(dy, lerpD(dx, n001, n101), lerpD(dx, n011, n111));
    return lerpD(dz, a, b);
}

TerrainDef::Block blockKind(const QJsonValue& v, TerrainDef::Block fallback)
{
    QString name = v.toObject().value(QLatin1String("Name")).toString();
    if (name.isEmpty())
        return fallback;
    if (!name.contains(QLatin1Char(':')))
        name = QStringLiteral("minecraft:") + name;
    if (name == QLatin1String("minecraft:air") || name == QLatin1String("minecraft:cave_air") ||
        name == QLatin1String("minecraft:void_air"))
        return TerrainDef::AIR;
    if (name == QLatin1String("minecraft:water") || name == QLatin1String("minecraft:lava"))
        return TerrainDef::FLUID;
    return TerrainDef::SOLID;
}

} // namespace

bool buildTerrain(const DensityGraph& graph, int finalDensity, const QJsonObject& settings,
    int dimMinY, int dimHeight, TerrainDef *def, QString *reason)
{
    TerrainDef& d = *def;
    d = TerrainDef();
    const QJsonObject noise = settings.value(QLatin1String("noise")).toObject();
    const int nMinY = noise.value(QLatin1String("min_y")).toInt(dimMinY);
    const int nHeight = noise.value(QLatin1String("height")).toInt(dimHeight);
    const int sh = noise.value(QLatin1String("size_horizontal")).toInt(1);
    const int sv = noise.value(QLatin1String("size_vertical")).toInt(2);
    if (sh < 1 || sh > 4 || sv < 1 || sv > 4)
    {
        *reason = QStringLiteral("The noise settings have an invalid cell size.");
        return false;
    }
    if (finalDensity < 0)
    {
        *reason = QStringLiteral("The noise settings have no final_density.");
        return false;
    }
    d.finalDensity = finalDensity;
    d.cellWidth = sh * 4;
    d.cellHeight = sv * 4;
    // NoiseSettings.clampToHeightAccessor()
    d.minY = std::max(nMinY, dimMinY);
    d.height = std::min(nMinY + nHeight, dimMinY + dimHeight) - d.minY;
    if (d.height <= 0)
    {
        *reason = QStringLiteral("The noise range of the noise settings is outside of the dimension.");
        return false;
    }
    d.cellMinY = floorDiv(d.minY, d.cellHeight);
    d.cellCountY = floorDiv(d.height, d.cellHeight);
    d.seaLevel = settings.value(QLatin1String("sea_level")).toInt(63);
    d.defaultBlock = blockKind(settings.value(QLatin1String("default_block")), TerrainDef::SOLID);
    d.defaultFluid = blockKind(settings.value(QLatin1String("default_fluid")), TerrainDef::FLUID);
    d.legacy = settings.value(QLatin1String("legacy_random_source")).toBool(false);
    d.aquifers = settings.value(QLatin1String("aquifers_enabled")).toBool(false);

    const size_t n = graph.nodes.size();
    d.leafOf.assign(n, -1);
    d.inBlock.assign(n, 0);
    d.blockVaries.assign(n, 0);

    // the block part and its leaves
    std::vector<int> stack(1, finalDensity), args;
    while (!stack.empty())
    {
        int i = stack.back();
        stack.pop_back();
        if (d.inBlock[i])
            continue;
        d.inBlock[i] = 1;
        if (graph.nodes[i].op == OP_INTERPOLATED)
        {
            d.leafOf[i] = (int32_t) d.leaves.size();
            d.leaves.push_back(i);
            continue;
        }
        args.clear();
        graph.arguments(i, args);
        for (int a : args)
            if (!d.inBlock[a])
                stack.push_back(a);
    }
    // node indices are in dependency order: arguments come first
    for (size_t i = 0; i < n; i++)
    {
        if (!d.inBlock[i])
            continue;
        const DensityNode& nd = graph.nodes[i];
        bool varies = d.leafOf[i] >= 0 || (nd.flags & DF_USES_Y);
        if (d.leafOf[i] < 0)
        {
            args.clear();
            graph.arguments((int) i, args);
            for (int a : args)
                if (d.blockVaries[a])
                    varies = true;
        }
        d.blockVaries[i] = varies;
        if (!varies && nd.op != OP_CONSTANT)
            d.blockColumn.push_back((int32_t) i);
    }

    std::vector<int> cornerOut;
    for (int32_t l : d.leaves)
        cornerOut.push_back(graph.nodes[l].a);
    d.corner.build(graph, cornerOut);
    d.all.build(graph, std::vector<int>(1, finalDensity));

    d.argStart.assign(n + 1, 0);
    d.args.clear();
    for (size_t i = 0; i < n; i++)
    {
        d.argStart[i] = (int32_t) d.args.size();
        args.clear();
        graph.arguments((int) i, args);
        d.args.insert(d.args.end(), args.begin(), args.end());
    }
    d.argStart[n] = (int32_t) d.args.size();
    d.ok = true;
    return true;
}

//==============================================================================
// state
//==============================================================================

struct TerrainState::CornerColumn
{
    bool valid = false;
    int x = 0, z = 0;
    std::vector<double> col;        // the column part of the corner program
    std::vector<double> value;      // [leaf][corner y index]
    std::vector<uint8_t> have;
};

TerrainState::TerrainState(const DensityGraph& graph, const TerrainDef& def, uint64_t seed, int mc)
    : graph(graph), def(def), ds(graph, def.all, seed, mc, def.legacy)
{
    if (def.ok && def.aquifer.on)
        aq.reset(new AquiferState(graph, def.aquifer, seed, mc));
    const size_t n = graph.nodes.size();
    regC.assign(n, 0.0);
    regB.assign(n, 0.0);
    stampC.assign(n, 0);
    stampB.assign(n, 0);
    for (size_t i = 0; i < n; i++)
        if (graph.nodes[i].op == OP_CONSTANT)
            regC[i] = regB[i] = graph.nodes[i].p[0];
    ivB.assign(n, Interval { 0.0, 0.0 });
    stampIB.assign(n, 0);
    ivC.assign(n, Interval { 0.0, 0.0 });
    stampIC.assign(n, 0);
    scratch.assign(n, 0.0);
    for (size_t i = 0; i < n; i++)
        if (graph.nodes[i].op == OP_CONSTANT)
            scratch[i] = graph.nodes[i].p[0];
    // |ImprovedNoise.noise()| <= 2: the eight corner terms are dot products
    // of a gradient (two components +-1, one 0) with offsets in [-1, 1],
    // and the fade weights are convex
    cellLeafIv.assign(def.leaves.size() * ((size_t) def.cellCountY + 1), Interval { 0.0, 0.0 });
    cellLeafStamp.assign(cellLeafIv.size(), 0u);
    noiseBound.assign(graph.noises.size(), 0.0);
    for (int32_t i : def.all.usedNoises)
    {
        const NormalNoiseState& ns = ds.noises[i];
        double b = 0;
        for (const NoiseLevel& l : ns.first)
            b += std::fabs(l.perlin.amplitude) * l.valueFactor * 2.0;
        for (const NoiseLevel& l : ns.second)
            b += std::fabs(l.perlin.amplitude) * l.valueFactor * 2.0;
        noiseBound[i] = b * std::fabs(ns.valueFactor) * (1.0 + 1e-12);
    }
    reserve(16);
}

TerrainState::~TerrainState()
{
    for (CornerColumn *c : cache)
        delete c;
}

void TerrainState::reserve(int cells)
{
    int w = 4;
    while (w < cells + 2 && w < (1 << 16))
        w *= 2;
    if (w <= cacheW)
        return;
    for (CornerColumn *c : cache)
        delete c;
    cache.assign((size_t) w * 2, nullptr);
    cacheW = w;
    loaded = nullptr;
    for (CornerColumn *&c : cc)
        c = nullptr;
}

TerrainState::CornerColumn *TerrainState::cornerColumn(int x, int z)
{
    const int ix = floorDiv(x, def.cellWidth), iz = floorDiv(z, def.cellWidth);
    CornerColumn *&slot = cache[(size_t) (ix & (cacheW - 1)) + (size_t) (iz & 1) * cacheW];
    if (!slot)
        slot = new CornerColumn;
    CornerColumn *c = slot;
    if (c->valid && c->x == x && c->z == z)
        return c;
    c->valid = true;
    c->x = x;
    c->z = z;
    const size_t ny = (size_t) def.cellCountY + 1;
    c->value.assign(def.leaves.size() * ny, 0.0);
    c->have.assign(def.leaves.size() * ny, 0);
    // the column part, at the corner column
    const std::vector<int32_t>& list = def.corner.column;
    c->col.resize(list.size());
    cornerX = x;
    cornerZ = z;
    cornerY = 0;
    for (size_t i = 0; i < list.size(); i++)
    {
        const int32_t idx = list[i];
        const DensityNode& nd = graph.nodes[idx];
        double v = nd.op == OP_FIND_TOP_SURFACE
            ? ds.findTopSurface(idx, x, z, regC[nd.b])
            : ds.compute(idx, regC.data(), x, 0, z);
        regC[idx] = v;
        c->col[i] = v;
    }
    loaded = c;
    return c;
}

double TerrainState::cornerValue(CornerColumn *c, int leaf, int k)
{
    const size_t i = (size_t) leaf * ((size_t) def.cellCountY + 1) + (size_t) k;
    if (c->have[i])
        return c->value[i];
    loadColumn(c);
    cornerX = c->x;
    cornerZ = c->z;
    cornerY = (def.cellMinY + k) * def.cellHeight;
    if (++curC == 0)
    {
        std::fill(stampC.begin(), stampC.end(), 0u);
        curC = 1;
    }
    double v = evalC(graph.nodes[def.leaves[leaf]].a);
    c->value[i] = v;
    c->have[i] = 1;
    return v;
}

// A node at the current corner: the column part is loaded, the rest is
// evaluated on demand.
double TerrainState::evalC(int32_t node)
{
    const DensityNode& n = graph.nodes[node];
    if (!(n.flags & DF_USES_Y) || stampC[node] == curC)
        return regC[node];
    double v;
    switch ((int) n.op)
    {
    case OP_MUL:
        if (evalC(n.a) != 0.0)
            evalC(n.b);
        v = ds.compute(node, regC.data(), cornerX, cornerY, cornerZ);
        break;
    case OP_RANGE_CHOICE:
    {
        double a = evalC(n.a);
        evalC((a >= n.p[0] && a < n.p[1]) ? n.b : n.c);
        v = ds.compute(node, regC.data(), cornerX, cornerY, cornerZ);
        break;
    }
    case OP_FIND_TOP_SURFACE:
        v = ds.findTopSurface(node, cornerX, cornerZ, evalC(n.b));
        break;
    default:
        for (int32_t k = def.argStart[node]; k < def.argStart[node + 1]; k++)
            evalC(def.args[k]);
        v = ds.compute(node, regC.data(), cornerX, cornerY, cornerZ);
        break;
    }
    regC[node] = v;
    stampC[node] = curC;
    return v;
}

void TerrainState::setBlockColumn(int x, int z)
{
    if (x != colX || z != colZ)
    {
        colX = x;
        colZ = z;
        for (int32_t idx : def.blockColumn)
        {
            const DensityNode& nd = graph.nodes[idx];
            regB[idx] = nd.op == OP_FIND_TOP_SURFACE
                ? ds.findTopSurface(idx, x, z, regB[nd.b])
                : ds.compute(idx, regB.data(), x, 0, z);
        }
    }
    // the cell column and its corners
    const int cw = def.cellWidth;
    const int sx = floorDiv(x, cw) * cw, sz = floorDiv(z, cw) * cw;
    fx = (double) (x - sx) / (double) cw;
    fz = (double) (z - sz) / (double) cw;
    blockX = x;
    blockZ = z;
    // (the four slots of a cell's corners never collide in the cache)
    cc[0] = cornerColumn(sx, sz);
    cc[1] = fx != 0.0 ? cornerColumn(sx + cw, sz) : cc[0];
    cc[2] = fz != 0.0 ? cornerColumn(sx, sz + cw) : cc[0];
    cc[3] = fx != 0.0 && fz != 0.0 ? cornerColumn(sx + cw, sz + cw) : fx != 0.0 ? cc[1] : cc[2];
    // a new cell: the bounds of the leaves from its corners
    if (cc[0] != cellCC[0] || cc[1] != cellCC[1] || cc[2] != cellCC[2] || cc[3] != cellCC[3] ||
        cellX != sx || cellZ != sz)
    {
        for (int i = 0; i < 4; i++)
            cellCC[i] = cc[i];
        cellX = sx;
        cellZ = sz;
        if (++cellStamp == 0)
        {
            std::fill(cellLeafStamp.begin(), cellLeafStamp.end(), 0u);
            cellStamp = 1;
        }
    }
}

double TerrainState::leafValue(int leaf)
{
    // with a fraction of 0 the far corner does not matter (a + 0 * (b - a)
    // is a for any finite b): it is the near one then
    const int k = cellK;
    return lerp3(fx, fy, fz,
        cornerValue(cc[0], leaf, k), cornerValue(cc[1], leaf, k),
        cornerValue(cc[0], leaf, k + 1), cornerValue(cc[1], leaf, k + 1),
        cornerValue(cc[2], leaf, k), cornerValue(cc[3], leaf, k),
        cornerValue(cc[2], leaf, k + 1), cornerValue(cc[3], leaf, k + 1));
}

double TerrainState::evalB(int32_t node)
{
    if (!def.blockVaries[node] || stampB[node] == curB)
        return regB[node];
    const DensityNode& n = graph.nodes[node];
    double v;
    if (def.leafOf[node] >= 0)
    {
        v = leafValue(def.leafOf[node]);
    }
    else
    {
        switch ((int) n.op)
        {
        case OP_MUL:
            if (evalB(n.a) != 0.0)
                evalB(n.b);
            v = ds.compute(node, regB.data(), blockX, blockY, blockZ);
            break;
        case OP_RANGE_CHOICE:
        {
            double a = evalB(n.a);
            evalB((a >= n.p[0] && a < n.p[1]) ? n.b : n.c);
            v = ds.compute(node, regB.data(), blockX, blockY, blockZ);
            break;
        }
        case OP_FIND_TOP_SURFACE:
            v = ds.findTopSurface(node, blockX, blockZ, evalB(n.b));
            break;
        default:
            for (int32_t k = def.argStart[node]; k < def.argStart[node + 1]; k++)
                evalB(def.args[k]);
            v = ds.compute(node, regB.data(), blockX, blockY, blockZ);
            break;
        }
    }
    regB[node] = v;
    stampB[node] = curB;
    return v;
}

namespace
{

const double INF = HUGE_VAL;

// Outwards by at least two units in the last place (2^-50 relative, plus
// a tiny absolute step for values near 0): covers the rounding of a single
// operation.
inline double down(double v) { return v - (std::fabs(v) * 0x1p-50 + 0x1p-1000); }
inline double up(double v) { return v + (std::fabs(v) * 0x1p-50 + 0x1p-1000); }

} // namespace

// The bounds of @node: every value the exact evaluation can give lies
// inside (NaN: the whole line). Block context (CORNER false): over the
// heights ivYlo .. ivYhi of the current block column, the leaves from the
// corners of the cell cellK (or bounded over ivKa .. ivKb, boundLeaves).
// Corner context: over the corner heights ivCylo .. ivCyhi of the corner
// column cornerX, cornerZ, whose column part regC holds.
template <bool CORNER>
TerrainState::Interval TerrainState::interval(int32_t node)
{
    const DensityNode& n = graph.nodes[node];
    if (CORNER ? !(n.flags & DF_USES_Y) : !def.blockVaries[node])
    {
        const double v = CORNER ? regC[node] : regB[node];
        return Interval { v, v };
    }
    std::vector<Interval>& iv = CORNER ? ivC : ivB;
    std::vector<uint32_t>& stamp = CORNER ? stampIC : stampIB;
    const uint32_t cur = CORNER ? curIC : curIB;
    if (stamp[node] == cur)
        return iv[node];
    Interval r { -INF, INF };
    const int32_t leaf = CORNER ? -1 : def.leafOf[node];
    const int ylo = CORNER ? ivCylo : ivYlo, yhi = CORNER ? ivCyhi : ivYhi;
    const int px = CORNER ? cornerX : blockX, pz = CORNER ? cornerZ : blockZ;
    if (leaf >= 0 && boundLeaves)
    {
        r = leafBound(leaf);
    }
    else if (leaf >= 0)
    {
        // lerp3 of the corner values: between their extremes (each lerp
        // a + t * (b - a) can leave [a, b] by one rounding); the same for
        // every block column of the cell: kept per cell
        const int k = cellK;
        const size_t li = (size_t) leaf * ((size_t) def.cellCountY + 1) + (size_t) k;
        if (cellLeafStamp[li] == cellStamp)
        {
            r = cellLeafIv[li];
            goto done;
        }
        double v[8] = {
            cornerValue(cc[0], leaf, k), cornerValue(cc[1], leaf, k),
            cornerValue(cc[0], leaf, k + 1), cornerValue(cc[1], leaf, k + 1),
            cornerValue(cc[2], leaf, k), cornerValue(cc[3], leaf, k),
            cornerValue(cc[2], leaf, k + 1), cornerValue(cc[3], leaf, k + 1) };
        double lo = v[0], hi = v[0];
        for (double x : v)
        {
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
        r = Interval { down(down(down(lo))), up(up(up(hi))) };
        cellLeafIv[li] = r;
        cellLeafStamp[li] = cellStamp;
    }
    else
    {
        auto arg = [&](int32_t a) { return interval<CORNER>(a); };
        auto exact = [](const Interval& i) { return i.lo == i.hi; };
        switch ((int) n.op)
        {
        case OP_CONSTANT:
            r = Interval { n.p[0], n.p[0] };
            break;
        case OP_Y_GRADIENT:
        {
            // clampedMap is monotone (also in floating point)
            const double y0 = (n.flags & DF_FLAT) ? 0.0 : (double) ylo;
            const double y1 = (n.flags & DF_FLAT) ? 0.0 : (double) yhi;
            double a = clampedMap(y0, n.p[0], n.p[1], n.p[2], n.p[3]);
            double b = clampedMap(y1, n.p[0], n.p[1], n.p[2], n.p[3]);
            r = Interval { std::min(a, b), std::max(a, b) };
            break;
        }
        case OP_NOISE:
        case OP_SHIFTED_NOISE:
            r = Interval { -noiseBound[n.noise], noiseBound[n.noise] };
            break;
        case OP_SHIFT_A:
        case OP_SHIFT_B:
        case OP_SHIFT:
            r = Interval { -4.0 * noiseBound[n.noise], 4.0 * noiseBound[n.noise] };
            break;
        case OP_WEIRD_SCALED:
            r = Interval { 0.0, (n.p[0] == 1 ? 2.0 : 3.0) * noiseBound[n.noise] };
            break;
        case OP_BLENDED_NOISE:
        case OP_BLENDED_NOISE_118:
            // between the two limit noises / 65536, each a sum of 16
            // octaves (noise / 2^-r) whose |noise| <= 2
            r = Interval { -2.0, 2.0 };
            break;
        case OP_SLIDE:
        {
            // clampedLerp(target, v, f) of the top, then of the bottom
            // slide: between v and the targets (one rounding each)
            Interval a = arg(n.a);
            const double *e = &graph.extras[n.extra];
            double lo = a.lo, hi = a.hi;
            if (e[4] > 0)
            {
                lo = std::min(lo, e[3]);
                hi = std::max(hi, e[3]);
            }
            if (e[7] > 0)
            {
                lo = std::min(lo, e[6]);
                hi = std::max(hi, e[6]);
            }
            r = Interval { down(down(lo)), up(up(hi)) };
            break;
        }
        case OP_SHAPER_SPLINE:
        case OP_PEAKS_VALLEYS:
        {
            // exact if every argument is; else the clamp of the shaper's
            // spline (peaks and valleys is at most 1)
            bool ok = true;
            for (int32_t k = def.argStart[node]; k < def.argStart[node + 1] && ok; k++)
            {
                Interval a = arg(def.args[k]);
                ok = exact(a);
                scratch[def.args[k]] = a.lo;
            }
            if (ok)
            {
                const double v = ds.compute(node, scratch.data(), px, 0, pz);
                r = Interval { v, v };
            }
            else if (n.op == OP_SHAPER_SPLINE)
                r = Interval { n.p[0], n.p[1] };
            else
                r = Interval { -INF, 1.0 + 1e-6 };
            break;
        }
        case OP_ADD:
        {
            Interval a = arg(n.a), b = arg(n.b);
            r = Interval { down(a.lo + b.lo), up(a.hi + b.hi) };
            break;
        }
        case OP_ADD_CONST:
        {
            Interval a = arg(n.a);
            r = Interval { down(a.lo + n.p[0]), up(a.hi + n.p[0]) };
            break;
        }
        case OP_MUL_CONST:
        case OP_MUL:
        {
            Interval a = arg(n.a);
            if (n.op == OP_MUL && a.lo == 0.0 && a.hi == 0.0)
            {
                r = Interval { 0.0, 0.0 };
                break;
            }
            Interval b = n.op == OP_MUL ? arg(n.b) : Interval { n.p[0], n.p[0] };
            double p[4] = { a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi };
            double lo = p[0], hi = p[0];
            for (double x : p)
            {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
            if (n.op == OP_MUL && a.lo <= 0.0 && a.hi >= 0.0)
            {
                lo = std::min(lo, 0.0);
                hi = std::max(hi, 0.0);
            }
            r = Interval { down(lo), up(hi) };
            break;
        }
        case OP_MIN:
        {
            Interval a = arg(n.a), b = arg(n.b);
            r = Interval { std::min(a.lo, b.lo), std::min(a.hi, b.hi) };
            break;
        }
        case OP_MAX:
        {
            Interval a = arg(n.a), b = arg(n.b);
            r = Interval { std::max(a.lo, b.lo), std::max(a.hi, b.hi) };
            break;
        }
        case OP_ABS:
        case OP_SQUARE:
        {
            Interval a = arg(n.a);
            double lo = a.lo >= 0.0 ? a.lo : a.hi <= 0.0 ? -a.hi : 0.0;
            double hi = std::max(std::fabs(a.lo), std::fabs(a.hi));
            if (n.op == OP_SQUARE)
            {
                lo = down(lo * lo);
                hi = up(hi * hi);
            }
            r = Interval { lo, hi };
            break;
        }
        case OP_CUBE:
        {
            Interval a = arg(n.a);
            r = Interval { down(down(a.lo * a.lo * a.lo)), up(up(a.hi * a.hi * a.hi)) };
            break;
        }
        case OP_HALF_NEGATIVE:
        case OP_QUARTER_NEGATIVE:
        case OP_SQUEEZE:
        case OP_CLAMP:
        {
            // monotone: the values at the ends (one rounding of slack for
            // squeeze, whose two terms round separately)
            Interval a = arg(n.a);
            scratch[n.a] = a.lo;
            double lo = ds.compute(node, scratch.data(), 0, 0, 0);
            scratch[n.a] = a.hi;
            double hi = ds.compute(node, scratch.data(), 0, 0, 0);
            r = Interval { down(down(lo)), up(up(hi)) };
            break;
        }
        case OP_INVERT:
        {
            Interval a = arg(n.a);
            if (a.lo > 0.0 || a.hi < 0.0)
                r = Interval { down(1.0 / a.hi), up(1.0 / a.lo) };
            break;
        }
        case OP_RANGE_CHOICE:
        {
            Interval a = arg(n.a);
            if (a.lo >= n.p[0] && a.hi < n.p[1])
                r = arg(n.b);
            else if (a.hi < n.p[0] || a.lo >= n.p[1])
                r = arg(n.c);
            else
            {
                Interval b = arg(n.b), c = arg(n.c);
                r = Interval { std::min(b.lo, c.lo), std::max(b.hi, c.hi) };
            }
            break;
        }
        case OP_SPLINE:
        case OP_END_ISLANDS:
        case OP_FIND_TOP_SURFACE:
        {
            // exact if every argument is
            bool ok = true;
            for (int32_t k = def.argStart[node]; k < def.argStart[node + 1] && ok; k++)
            {
                Interval a = arg(def.args[k]);
                ok = exact(a);
                scratch[def.args[k]] = a.lo;
            }
            if (ok)
            {
                double v = n.op == OP_FIND_TOP_SURFACE
                    ? ds.findTopSurface(node, px, pz, scratch[n.b])
                    : ds.compute(node, scratch.data(), px, 0, pz);
                r = Interval { v, v };
            }
            break;
        }
        case OP_INTERPOLATED:
            // at the corners: the argument
            r = arg(n.a);
            break;
        default:
            break;
        }
    }
done:
    if (!(r.lo <= r.hi))    // NaN
        r = Interval { -INF, INF };
    iv[node] = r;
    stamp[node] = cur;
    return r;
}

void TerrainState::loadColumn(CornerColumn *c)
{
    if (loaded == c)
        return;
    const std::vector<int32_t>& list = def.corner.column;
    for (size_t j = 0; j < list.size(); j++)
        regC[list[j]] = c->col[j];
    loaded = c;
}

// The bounds of a leaf's corner values at the corner heights ivKa .. ivKb
// of the current cell's corner columns: the exact values where all of them
// are known, else the corner context's bounds.
TerrainState::Interval TerrainState::leafBound(int leaf)
{
    const size_t ny = (size_t) def.cellCountY + 1;
    double lo = INF, hi = -INF;
    for (int q = 0; q < 4; q++)
    {
        CornerColumn *c = cc[q];
        bool seen = false;
        for (int p = 0; p < q; p++)
            seen = seen || cc[p] == c;
        if (seen)
            continue;
        bool all = true;
        for (int k = ivKa; k <= ivKb && all; k++)
            all = c->have[(size_t) leaf * ny + (size_t) k] != 0;
        if (all)
        {
            for (int k = ivKa; k <= ivKb; k++)
            {
                const double v = c->value[(size_t) leaf * ny + (size_t) k];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            continue;
        }
        loadColumn(c);
        cornerX = c->x;
        cornerZ = c->z;
        ivCylo = (def.cellMinY + ivKa) * def.cellHeight;
        ivCyhi = (def.cellMinY + ivKb) * def.cellHeight;
        if (++curIC == 0)
        {
            std::fill(stampIC.begin(), stampIC.end(), 0u);
            curIC = 1;
        }
        Interval a = interval<true>(graph.nodes[def.leaves[leaf]].a);
        lo = std::min(lo, a.lo);
        hi = std::max(hi, a.hi);
    }
    if (!(lo <= hi))
        return Interval { -INF, INF };
    // lerp3 between them: three roundings
    return Interval { down(down(down(lo))), up(up(up(hi))) };
}

// Can a block of the current block column between ylo and yhi (inside the
// cell cellK) be solid? From the exact corners of the cell.
bool TerrainState::cellMaySolid(int ylo, int yhi)
{
    ivYlo = ylo;
    ivYhi = yhi;
    boundLeaves = false;
    if (++curIB == 0)
    {
        std::fill(stampIB.begin(), stampIB.end(), 0u);
        curIB = 1;
    }
    return interval<false>(def.finalDensity).hi > 0.0;
}

// The same for the blocks ylo .. yhi of the cells ka .. kb, from bounds of
// their corners (without evaluating them).
bool TerrainState::cellsMaySolid(int ka, int kb, int ylo, int yhi)
{
    ivYlo = ylo;
    ivYhi = yhi;
    boundLeaves = true;
    ivKa = ka;
    ivKb = kb + 1;
    if (++curIB == 0)
    {
        std::fill(stampIB.begin(), stampIB.end(), 0u);
        curIB = 1;
    }
    const bool may = interval<false>(def.finalDensity).hi > 0.0;
    boundLeaves = false;
    return may;
}

// final_density at the height y of the current block column
double TerrainState::density(int y)
{
    const int ch = def.cellHeight;
    const int cy = floorDiv(y, ch);
    cellK = cy - def.cellMinY;
    fy = (double) (y - cy * ch) / (double) ch;
    blockY = y;
    if (++curB == 0)
    {
        std::fill(stampB.begin(), stampB.end(), 0u);
        curB = 1;
    }
    return evalB(def.finalDensity);
}

// The blocks the fill sets below @top: whole cells, within the dimension
// (false if none). @fluidTop: the highest fluid block among them, lava below
// min(-54, sea level), the default fluid below the sea level, or NO_HEIGHT.
static bool fillRange(const TerrainDef& def, int top, int *ylo, int *yhi, int *fluidTop)
{
    const int ch = def.cellHeight;
    *ylo = std::max(def.cellMinY * ch, def.minY);
    *yhi = std::min(std::min((def.cellMinY + def.cellCountY) * ch - 1, def.minY + def.height - 1), top);
    int limit = std::min(-54, def.seaLevel);
    if (def.defaultFluid != TerrainDef::AIR)
        limit = std::max(limit, def.seaLevel);
    *fluidTop = limit - 1 >= *ylo ? std::min(limit - 1, *yhi) : NO_HEIGHT;
    return *yhi >= *ylo;
}

bool TerrainState::surfaceFromFloor(int top, int *fluidTop) const
{
    int ylo, yhi;
    if (!def.ok || def.defaultBlock != TerrainDef::SOLID || (aq && !fast))
        return false;
    if (!fillRange(def, top, &ylo, &yhi, fluidTop))
        *fluidTop = NO_HEIGHT;
    return true;
}

int TerrainState::height(int x, int z, bool oceanFloor, int top)
{
    if (!aq || fast)
        return densityHeight(x, z, oceanFloor, top);
    int f, s;
    bothHeights(x, z, top, &f, &s);
    return oceanFloor ? f : s;
}

// The range of the flat caches of the noise chunk that fills the column
// (NoiseChunk.forChunk(), or the one cell of iterateNoiseColumn()) and its chunk
AquiferState::Range TerrainState::rangeOf(int x, int z) const
{
    AquiferState::Range r;
    const int cw = def.cellWidth;
    if (baseColumns)
    {
        const int sx = floorDiv(x, cw) * cw, sz = floorDiv(z, cw) * cw;
        r.qx = sx >> 2;
        r.qz = sz >> 2;
        r.n = cw >> 2;
        r.chunkX = sx >> 4;
        r.chunkZ = sz >> 4;
    }
    else
    {
        r.chunkX = x >> 4;
        r.chunkZ = z >> 4;
        r.qx = r.chunkX * 4;
        r.qz = r.chunkZ * 4;
        r.n = ((16 / cw) * cw) >> 2;
    }
    return r;
}

void TerrainState::bothHeights(int x, int z, int top, int *floorOut, int *surfaceOut)
{
    if (!aq || fast)
    {
        *floorOut = densityHeight(x, z, true, top);
        *surfaceOut = densityHeight(x, z, false, top);
        // (the fluids above the floor are the global ones: no air between)
        last.water = *surfaceOut != NO_HEIGHT && *floorOut != NO_HEIGHT && *surfaceOut > *floorOut
            ? *surfaceOut + 1 : INT_MIN;
        last.floorDefault = -1;
        last.valid = false;
        return;
    }
    if (last.valid && last.x == x && last.z == z && last.top == top && last.base == baseColumns)
    {
        *floorOut = last.floor;
        *surfaceOut = last.surface;
        return;
    }
    int ylo, yhi, fluidTop;
    int floor = NO_HEIGHT, surface = NO_HEIGHT;
    int water = INT_MIN, floorDefault = 1;
    if (def.ok && fillRange(def, top, &ylo, &yhi, &fluidTop))
    {
        // the highest block of positive density: the default block (solid)
        const int yd = densityHeight(x, z, true, top);
        const int lowest = yd != NO_HEIGHT ? yd + 1 : ylo;
        // Above yd every block's density is not positive: the aquifer's
        // block. Above globalAbove() that is the global fluid picker's.
        aq->setAbort(ds.abort);
        aq->setColumn(x, z, rangeOf(x, z));
        int scanTop = yhi;
        if (g_prune.load(std::memory_order_relaxed))
            scanTop = (int) std::min((int64_t) yhi, (int64_t) aq->globalAbove() - 1);
        if (scanTop < yhi)
        {
            // the global picker: lava below min(-54, sea level), the default
            // fluid below the sea level
            const AquiferDef& ad = def.aquifer;
            const int lavaTop = std::min(-54, ad.seaLevel) - 1;
            const int from = std::max(scanTop + 1, lowest);
            const int t = std::min(yhi, ad.seaLevel - 1);
            if (ad.defaultFluid != FK_AIR && t >= std::max(from, lavaTop + 1))
            {
                surface = t;
                if (ad.defaultFluid == FK_OTHER)
                    floor = t;
            }
            else if (std::min(yhi, lavaTop) >= from)
                surface = std::min(yhi, lavaTop);
            // (fluids from there down, air above)
            if (surface != NO_HEIGHT)
                water = surface + 1;
            if (floor != NO_HEIGHT)
                floorDefault = 0;
        }
        if (scanTop >= lowest && (floor == NO_HEIGHT || surface == NO_HEIGHT))
        {
            setBlockColumn(x, z);
            const bool prune = g_prune.load(std::memory_order_relaxed);
            for (int y = scanTop; y >= lowest && (floor == NO_HEIGHT || surface == NO_HEIGHT); )
            {
                if (prune)
                {
                    // whole layers of aquifer cells at a time where their
                    // centers decide without the density
                    const int ya = floorDiv(y + 1, 12);
                    const AquiferState::LayerInfo *l = &aq->layer(ya, false);
                    const int bottom = std::max(12 * ya - 1, lowest);
                    if (y < l->globalFrom && !l->exact && aq->isDense())
                        l = &aq->layer(ya, true);
                    if (y >= l->globalFrom)
                    {
                        // air (the global picker above the sea level)
                        y = std::max(l->globalFrom, bottom) - 1;
                        water = INT_MIN;
                        continue;
                    }
                    if (l->uniform)
                    {
                        const int level = l->level;
                        const uint8_t type = l->type;
                        for (; y >= bottom && (floor == NO_HEIGHT || surface == NO_HEIGHT); y--)
                        {
                            const uint8_t k = aq->lavaOr(y, y < level ? type : (uint8_t) FK_AIR);
                            if (k != FK_AIR && surface == NO_HEIGHT)
                                surface = y;
                            if (k == FK_OTHER && floor == NO_HEIGHT)
                            {
                                floor = y;
                                floorDefault = 0;
                            }
                            else if (k == FK_AIR)
                                water = INT_MIN;
                            else if (water == INT_MIN && floor == NO_HEIGHT)
                                water = y + 1;
                        }
                        continue;
                    }
                }
                const uint8_t k = aq->substance(y, [this](int yy) { return density(yy); });
                if (k != FK_AIR && surface == NO_HEIGHT)
                    surface = y;
                if ((k == FK_BARRIER || k == FK_OTHER) && floor == NO_HEIGHT)
                {
                    floor = y;
                    floorDefault = k == FK_BARRIER ? 1 : 0;
                }
                else if (k == FK_AIR)
                    water = INT_MIN;
                else if (water == INT_MIN && floor == NO_HEIGHT)
                    water = y + 1;
                y--;
            }
        }
        if (floor == NO_HEIGHT)
            floor = yd;
        if (surface == NO_HEIGHT)
            surface = yd;
    }
    // (not kept if aborted: the values may be wrong)
    last = { x, z, top, floor, surface, baseColumns, !(ds.abort && ds.abort->load(std::memory_order_relaxed)),
        water, floorDefault };
    *floorOut = floor;
    *surfaceOut = surface;
}

int TerrainState::block(int x, int y, int z)
{
    const int k = blockKind(x, y, z);
    return k == 3 ? 2 : k == 4 ? 1 : k;
}

int TerrainState::blockKind(int x, int y, int z)
{
    if (!def.ok)
        return 0;
    const int ch = def.cellHeight;
    if (y < std::max(def.cellMinY * ch, def.minY) ||
        y > std::min((def.cellMinY + def.cellCountY) * ch - 1, def.minY + def.height - 1))
        return 0;
    setBlockColumn(x, z);
    const double d = density(y);
    if (d > 0.0)
        return def.defaultBlock == TerrainDef::SOLID ? 2 : def.defaultBlock == TerrainDef::FLUID ? 1 : 0;
    uint8_t k;
    if (aq)
    {
        aq->setAbort(ds.abort);
        aq->setColumn(x, z, rangeOf(x, z));
        // (globalAbove(): a lower bound of the skip height of 1.21.9, which
        // saves the chunk's surfaces below it)
        if (y >= aq->globalAbove())
            k = aq->global(y);
        else
            k = aq->substance(y, [d](int) { return d; });
    }
    else
    {
        // the global fluid picker
        int limit = std::min(-54, def.seaLevel);
        k = y < limit ? FK_LAVA : FK_AIR;
        if (y >= limit && y < def.seaLevel)
            k = def.defaultFluid == TerrainDef::AIR ? FK_AIR : def.defaultFluid == TerrainDef::FLUID ? FK_WATER
                : FK_OTHER;
    }
    return k == FK_AIR ? 0 : k == FK_BARRIER ? 2 : k == FK_OTHER ? 3 : k == FK_LAVA ? 4 : 1;
}

int TerrainState::densityHeight(int x, int z, bool oceanFloor, int top)
{
    if (!def.ok)
        return NO_HEIGHT;
    const int ch = def.cellHeight;
    int ylo, yhi, fluidTop;
    if (!fillRange(def, top, &ylo, &yhi, &fluidTop))
        return NO_HEIGHT;
    // fluids count for WORLD_SURFACE only
    if (oceanFloor)
        fluidTop = NO_HEIGHT;
    const bool blockCounts = oceanFloor ? def.defaultBlock == TerrainDef::SOLID
                                        : def.defaultBlock != TerrainDef::AIR;
    if (!blockCounts)
        return fluidTop;

    setBlockColumn(x, z);
    const bool prune = g_prune.load(std::memory_order_relaxed);
    const int stop = fluidTop != NO_HEIGHT ? fluidTop + 1 : ylo;
    // Top down, cell by cell. With pruning, runs of cells whose corners are
    // bounded below 0 everywhere are skipped (the run grows while that
    // works and shrinks where it does not), then single cells whose exact
    // corners bound every block below 0.
    const int stopCell = floorDiv(stop, ch);
    int span = coarse ? 4 : 0;
    // Columns near each other have near heights: one run of all the cells
    // above those around the last height found (a margin of cells above it,
    // wider where the run does not hold near the surface), then those cells
    // one by one, below them as without the hint. Where such long runs do
    // not hold at all, as without the hint, and the long run is only tried
    // now and then.
    const int hint = lastHeight[oceanFloor ? 1 : 0];
    int hintCell = INT_MIN;
    if (prune && hint != NO_HEIGHT && hint >= stop && hint <= yhi)
        hintCell = floorDiv(hint, ch);
    const int margin = hintMargin;
    bool firstRun = hintCell != INT_MIN && floorDiv(yhi, ch) > hintCell + margin - 1 &&
        (hintScore >= 0 || (++hintProbe & 31) == 0);
    bool aboveBand = firstRun;
    if (firstRun)
        span = 64;
    else
        hintCell = INT_MIN;
    for (int y = yhi; y >= stop; )
    {
        const int cy = floorDiv(y, ch);
        if (aboveBand && cy <= hintCell + margin - 1)
        {
            aboveBand = false;
            span = 0;
        }
        if (hintCell != INT_MIN && !aboveBand && span == 0 && coarse && cy == hintCell - 2)
            span = 4;
        if (prune && span > 0)
        {
            const int lo = std::max(std::max(cy - span + 1, stopCell), aboveBand ? hintCell + margin : INT_MIN);
            const int bottom = std::max(lo * ch, stop);
            const bool mayR = cellsMaySolid(lo - def.cellMinY, cy - def.cellMinY, bottom, y);
            if (firstRun)
            {
                firstRun = false;
                if (mayR && hintMargin < 8)
                    hintMargin++;   // (the score stays: a wider margin may hold)
                else
                    hintScore = mayR ? std::max(hintScore - 2, -1) : std::min(hintScore + 1, 8);
                if (!mayR && (++hintWins & 63) == 0 && hintMargin > 2)
                    hintMargin--;
                if (mayR)
                {
                    hintCell = INT_MIN;
                    aboveBand = false;
                    span = coarse ? 4 : 0;
                    continue;
                }
            }
            if (!mayR)
            {
                y = bottom - 1;
                span = std::min(span * 2, aboveBand ? 64 : 16);
                continue;
            }
            if (span > 1)
            {
                span /= 2;
                continue;
            }
            span = 0;   // bounds do not help from here: exact corners
        }
        // the blocks of this cell from y down
        const int bottom = std::max(cy * ch, stop);
        cellK = cy - def.cellMinY;
        if (!prune || cellMaySolid(bottom, y))
        {
            for (; y >= bottom; y--)
            {
                if (density(y) > 0.0)
                {
                    lastHeight[oceanFloor ? 1 : 0] = y;
                    return y;
                }
            }
        }
        y = bottom - 1;
    }
    return fluidTop;
}

bool TerrainState::solid(int x, int y, int z)
{
    if (!def.ok)
        return false;
    const int ch = def.cellHeight;
    if (y < std::max(def.cellMinY * ch, def.minY) ||
        y > std::min((def.cellMinY + def.cellCountY) * ch - 1, def.minY + def.height - 1))
        return false;
    setBlockColumn(x, z);
    return density(y) > 0.0;
}

} // namespace dp
