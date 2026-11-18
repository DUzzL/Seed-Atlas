#include "dpaquifer.h"
#include "dpeval.h"
#include "dpinternal.h"

extern "C"
{
#include "rng.h"
}

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

// The aquifers of the game (net.minecraft.world.level.levelgen.Aquifer.
// NoiseBasedAquifer and NoiseChunk.preliminarySurfaceLevel), checked against
// the bytecode of 1.18.2, 1.19.2, 1.19.4, 1.20.6, 1.21.1 to 1.21.11 and 26.1:
//  - Where final_density is not positive, the global fluid picker's lava (below
//    min(-54, sea level)) stays. Elsewhere the block's three nearest aquifer
//    centers (one per 16 x 12 x 16 grid cell, placed at random by the
//    positional random "minecraft:aquifer") decide: the fluid of the nearest
//    one, or the default block (a barrier) where two of them with different
//    fluid levels are about equally near and the pressure between them
//    outweighs the density.
//  - The fluid level of a center comes from the preliminary surface of
//    thirteen columns around it, the noises fluid_level_floodedness,
//    fluid_level_spread and lava of the noise router, and (since 1.19) the
//    deep dark region (erosion and depth of the router).
//  - Since 1.21.9 a chunk takes the global fluid picker's block above a
//    height derived from the highest preliminary surface around it.
// What the game's caches return is a function of the position, except for
// the flat caches of the noise chunk: inside the chunk they hold the value at
// the quart position and y 0, outside the function is evaluated directly. The
// functions where that makes a difference are evaluated for the chunk the
// block is in (or the single cell of getBaseHeight()).

namespace dp
{

namespace
{

const int WAY_BELOW_MIN_Y = -2032 << 4;

// The surface samples around an aquifer center, in chunks, in the game's order
const int SAMPLES_OLD[13][2] = {
    { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { -3, 0 }, { -2, 0 }, { -1, 0 }, { 0, 0 }, { 1, 0 },
    { -2, 1 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
const int SAMPLES[13][2] = {
    { 0, 0 }, { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { -3, 0 }, { -2, 0 }, { -1, 0 }, { 1, 0 },
    { -2, 1 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };

// Xoroshiro128PlusPlus(lo, hi)
inline Xoroshiro xoroshiro(uint64_t lo, uint64_t hi)
{
    Xoroshiro x;
    if ((lo | hi) == 0)
    {
        lo = 0x9e3779b97f4a7c15ULL;
        hi = 0x6a09e667f3bcc909ULL;
    }
    x.lo = lo;
    x.hi = hi;
    return x;
}

// Mth.getSeed(x, y, z)
inline int64_t positionSeed(int x, int y, int z)
{
    const int64_t a = (int64_t) (int32_t) ((uint32_t) x * 3129871u);
    uint64_t l = (uint64_t) (a ^ (int64_t) ((uint64_t) (int64_t) z * 116129781ULL) ^ (int64_t) y);
    l = l * l * 42317861ULL + l * 11ULL;
    return (int64_t) l >> 16;
}

// BlockPos.asLong() and back: x and z keep 26 bits, y 12 (sign extended)
inline int wrapBits(int v, int bits)
{
    const int s = 32 - bits;
    return (int) ((uint32_t) v << s) >> s;
}

inline int add32(int a, int b)
{
    return (int) ((uint32_t) a + (uint32_t) b);
}

inline double lerpD(double t, double a, double b)
{
    return a + t * (b - a);
}

// Mth.map(v, fromMin, fromMax, toMin, toMax)
inline double mapD(double v, double a, double b, double c, double d)
{
    return lerpD((v - a) / (b - a), c, d);
}

// Mth.clamp(double, double, double)
inline double clampD(double v, double lo, double hi)
{
    return v < lo ? lo : javaMin(v, hi);
}

inline uint8_t fluidAt(int level, uint8_t type, int y)
{
    return y < level ? type : FK_AIR;
}

uint8_t fluidKind(const QJsonValue& v)
{
    QString name = v.toObject().value(QLatin1String("Name")).toString();
    if (!name.contains(QLatin1Char(':')))
        name = QStringLiteral("minecraft:") + name;
    if (name == QLatin1String("minecraft:air") || name == QLatin1String("minecraft:cave_air") ||
        name == QLatin1String("minecraft:void_air"))
        return FK_AIR;
    if (name == QLatin1String("minecraft:water"))
        return FK_WATER;
    if (name == QLatin1String("minecraft:lava"))
        return FK_LAVA;
    return FK_OTHER;
}

// Is the normal compile @n of a function (flat_cache: the value at the quart
// position at y 0) the same function as its plain compile @p (no caches) at
// points whose x and z are multiples of 4 (@aligned), or at any point? The
// two only differ in the flags of what is inside a flat_cache, and in the
// interpolated markers, which pass their argument through at single points.
bool sameFunction(const DensityGraph& g, int n, int p, bool aligned, std::unordered_map<uint64_t, bool>& memo)
{
    if (n == p)
        return true;
    if (n < 0 || p < 0)
        return false;
    const DensityNode& a = g.nodes[(size_t) n];
    const DensityNode& b = g.nodes[(size_t) p];
    if (a.op == OP_INTERPOLATED || a.op == OP_CACHE_2D)
        return sameFunction(g, a.a, p, aligned, memo);
    if (b.op == OP_INTERPOLATED || b.op == OP_CACHE_2D)
        return sameFunction(g, n, b.a, aligned, memo);
    const uint64_t key = (uint64_t) (uint32_t) n << 32 | (uint32_t) p;
    auto it = memo.find(key);
    if (it != memo.end())
        return it->second;
    bool same;
    if (a.flags & DF_QUART)
        same = aligned && !(b.flags & DF_USES_Y);
    else if (a.op != b.op)
        same = false;
    else
    {
        std::vector<int> aa, ba;
        g.arguments(n, aa);
        g.arguments(p, ba);
        same = aa.size() == ba.size();
        for (size_t i = 0; i < aa.size() && same; i++)
            same = sameFunction(g, aa[i], ba[i], aligned, memo);
    }
    memo[key] = same;
    return same;
}

bool hasInterpolated(const DensityGraph& g, int n, std::vector<char>& seen)
{
    if (n < 0 || seen[(size_t) n])
        return false;
    seen[(size_t) n] = 1;
    if (g.nodes[(size_t) n].op == OP_INTERPOLATED)
        return true;
    std::vector<int> args;
    g.arguments(n, args);
    for (int a : args)
        if (hasInterpolated(g, a, seen))
            return true;
    return false;
}

} // namespace

//==============================================================================
// definition
//==============================================================================

void buildAquifer(DensityGraph& graph, const QJsonObject& settings, int pack, const QString& id,
    int legacySettings, int mc, int minY, int height, int cellHeight, bool solidDefaultBlock, AquiferDef *def)
{
    AquiferDef& d = *def;
    d = AquiferDef();
    if (!settings.value(QLatin1String("aquifers_enabled")).toBool(false))
        return;
    d.variant = mc <= MC_1_18_2 ? AquiferDef::V1_18_2 : mc <= MC_1_19_2 ? AquiferDef::V1_19_2
        : mc < MC_1_21_9 ? AquiferDef::V1_19_4 : AquiferDef::V1_21_9;
    if (mc >= MC_26_3)
    {
        d.reason = QStringLiteral("Aquifers of Minecraft %1 are not modelled.").arg(versionName(mc));
        return;
    }
    if (!solidDefaultBlock)
    {
        d.reason = QStringLiteral("Aquifers are not modelled where the default block is not solid.");
        return;
    }
    d.seaLevel = settings.value(QLatin1String("sea_level")).toInt(63);
    d.defaultFluid = fluidKind(settings.value(QLatin1String("default_fluid")));
    d.legacy = settings.value(QLatin1String("legacy_random_source")).toBool(false);
    d.floorSaturates = mc >= MC_26_1;
    d.minY = minY;
    d.height = height;
    d.cellHeight = cellHeight;
    if (d.variant == AquiferDef::V1_18_2)
    {
        // the noise settings as given (not clamped to the dimension)
        const QJsonObject noise = settings.value(QLatin1String("noise")).toObject();
        const int nMinY = noise.value(QLatin1String("min_y")).toInt(minY);
        const int nHeight = noise.value(QLatin1String("height")).toInt(height);
        const QJsonObject top = noise.value(QLatin1String("top_slide")).toObject();
        const QJsonObject bottom = noise.value(QLatin1String("bottom_slide")).toObject();
        d.slide[0] = cellHeight;
        d.slide[1] = floorDiv(nMinY, cellHeight);
        d.slide[2] = nHeight / cellHeight;
        d.slide[3] = top.value(QLatin1String("target")).toDouble();
        d.slide[4] = top.value(QLatin1String("size")).toInt();
        d.slide[5] = top.value(QLatin1String("offset")).toInt();
        d.slide[6] = bottom.value(QLatin1String("target")).toDouble();
        d.slide[7] = bottom.value(QLatin1String("size")).toInt();
        d.slide[8] = bottom.value(QLatin1String("offset")).toInt();
    }

    const QJsonObject router = settings.value(QLatin1String("noise_router")).toObject();
    const bool newCenter = d.variant != AquiferDef::V1_18_2;
    const char *surfaceKey = d.variant == AquiferDef::V1_21_9 ? "preliminary_surface_level"
                                                              : "initial_density_without_jaggedness";
    // n: the normal compile, p: the plain one (the router functions of
    // 1.18.2 are not wrapped by the noise chunk: always plain)
    auto compile = [&](const char *key, int out[2]) {
        const QJsonValue v = router.value(QLatin1String(key));
        if (v.isUndefined())
            return false;
        out[1] = graph.compilePlain(v, pack, id, legacySettings);
        out[0] = d.variant == AquiferDef::V1_18_2 ? out[1] : graph.compile(v, pack, id, legacySettings);
        return out[0] >= 0 && out[1] >= 0;
    };
    int barrier[2], surface[2];
    bool ok = compile("barrier", barrier) && compile("fluid_level_floodedness", d.flood) &&
        compile("fluid_level_spread", d.spread) && compile("lava", d.lava) && compile(surfaceKey, surface);
    if (ok)
    {
        // the plain compile with the caches the scan needs
        surface[1] = graph.compileSurface(router.value(QLatin1String(surfaceKey)), pack, id, legacySettings);
        ok = surface[1] >= 0;
    }
    if (ok && newCenter)
        ok = compile("erosion", d.erosion) && compile("depth", d.depth);
    if (!ok)
    {
        d.reason = QStringLiteral("The noise router's aquifer functions have errors.");
        return;
    }
    // the barrier is evaluated at the block, in the noise chunk: its flat
    // caches give the quart values (the normal compile), its interpolated
    // markers would interpolate (not modelled)
    d.barrier = barrier[0];
    if (newCenter)
    {
        std::vector<char> seen(graph.nodes.size(), 0);
        if (hasInterpolated(graph, d.barrier, seen))
        {
            d.reason = QStringLiteral("Aquifers are not modelled where the barrier function is interpolated.");
            return;
        }
    }
    std::unordered_map<uint64_t, bool> memo;
    // The preliminary surface is sampled at quart positions: its flat caches
    // must not see the sample's y (in 1.21.9 and later it is sampled at y 0).
    if (!sameFunction(graph, surface[0], surface[1], true, memo))
    {
        d.reason = QStringLiteral("Aquifers are not modelled where the preliminary surface function caches values "
            "that change with y.");
        return;
    }
    d.surface = surface[1];
    auto sensitive = [&](const int f[2]) { return !sameFunction(graph, f[0], f[1], false, memo); };
    if (newCenter)
    {
        d.rangeCenter = sensitive(d.flood) || sensitive(d.erosion) || sensitive(d.depth);
        d.rangeSpread = sensitive(d.spread);
        d.rangeLava = sensitive(d.lava);
    }
    if (!d.rangeCenter)
    {
        d.flood[0] = d.flood[1];
        d.erosion[0] = d.erosion[1];
        d.depth[0] = d.depth[1];
    }
    if (!d.rangeSpread)
        d.spread[0] = d.spread[1];
    if (!d.rangeLava)
        d.lava[0] = d.lava[1];

    // one program per slot (the two variants of a slot share one if equal)
    auto add = [&](int slot, const std::vector<int>& outputs) {
        DensityProgram prog;
        prog.build(graph, outputs);
        d.programs.push_back(prog);
        d.program[slot] = (int) d.programs.size() - 1;
    };
    add(AquiferDef::BARRIER, { d.barrier });
    std::vector<int> c1 { d.flood[1] };
    if (newCenter)
    {
        c1.push_back(d.erosion[1]);
        c1.push_back(d.depth[1]);
    }
    add(AquiferDef::CENTER1, c1);
    if (d.rangeCenter)
        add(AquiferDef::CENTER0, { d.flood[0], d.erosion[0], d.depth[0] });
    else
        d.program[AquiferDef::CENTER0] = d.program[AquiferDef::CENTER1];
    add(AquiferDef::SPREAD1, { d.spread[1] });
    if (d.rangeSpread)
        add(AquiferDef::SPREAD0, { d.spread[0] });
    else
        d.program[AquiferDef::SPREAD0] = d.program[AquiferDef::SPREAD1];
    add(AquiferDef::LAVA1, { d.lava[1] });
    if (d.rangeLava)
        add(AquiferDef::LAVA0, { d.lava[0] });
    else
        d.program[AquiferDef::LAVA0] = d.program[AquiferDef::LAVA1];
    add(AquiferDef::SURFACE, { d.surface });
    // the surface scan's nodes and the caches that keep their first value
    if (d.variant != AquiferDef::V1_21_9)
    {
        std::vector<char> reach(graph.nodes.size(), 0);
        d.isSurfaceCache.assign(graph.nodes.size(), 0);
        std::vector<int> stack(1, d.surface), args;
        while (!stack.empty())
        {
            const int i = stack.back();
            stack.pop_back();
            if (i < 0 || reach[(size_t) i])
                continue;
            reach[(size_t) i] = 1;
            const DensityNode& nd = graph.nodes[(size_t) i];
            if (nd.op == OP_CACHE_2D && (nd.flags & DF_USES_Y))
            {
                d.surfaceCaches.push_back(i);
                d.isSurfaceCache[(size_t) i] = 1;
                continue;
            }
            args.clear();
            graph.arguments(i, args);
            for (int a : args)
                stack.push_back(a);
        }
        for (int32_t i : d.programs[(size_t) d.program[AquiferDef::SURFACE]].vertical)
            if (reach[(size_t) i])
                d.surfaceScan.push_back(i);
    }
    d.on = true;
}

//==============================================================================
// state
//==============================================================================

AquiferState::AquiferState(const DensityGraph& graph, const AquiferDef& def, uint64_t seed, int mc)
    : graph(graph), def(def), mc(mc)
{
    for (const DensityProgram& prog : def.programs)
        states.emplace_back(new DensityState(graph, prog, seed, mc, def.legacy));
    std::vector<double> memo(graph.nodes.size(), -1.0);
    barrierBound = absBound(*states[(size_t) def.program[AquiferDef::BARRIER]], def.barrier, memo);

    // RandomState.aquiferRandom(): random.fromHashOf("minecraft:aquifer").forkPositional()
    if (def.legacy)
    {
        uint64_t r;
        setSeed(&r, seed);
        const uint64_t factory = nextLong(&r);
        setSeed(&r, (uint64_t) (int64_t) javaStringHash(QStringLiteral("minecraft:aquifer")) ^ factory);
        posLo = nextLong(&r);
    }
    else
    {
        Xoroshiro w;
        xSetSeed(&w, seed);
        const uint64_t lo = xNextLong(&w);
        const uint64_t hi = xNextLong(&w);
        uint64_t h[2];
        hashOf(QByteArray("minecraft:aquifer"), h);
        Xoroshiro f = xoroshiro(lo ^ h[0], hi ^ h[1]);
        posLo = xNextLong(&f);
        posHi = xNextLong(&f);
    }
}

AquiferState::~AquiferState()
{
}

void AquiferState::setAbort(const std::atomic_bool *abort)
{
    abortFlag = abort;
    for (std::unique_ptr<DensityState>& s : states)
        s->abort = abort;
}

// A bound of |value| of @node over all positions (HUGE_VAL if not known),
// from the noises of @s
double AquiferState::absBound(const DensityState& s, int node, std::vector<double>& memo)
{
    const DensityGraph& g = graph;
    if (node < 0)
        return HUGE_VAL;
    double& m = memo[(size_t) node];
    if (m >= 0.0)
        return m;
    const DensityNode& n = g.nodes[(size_t) node];
    auto arg = [&](int a) { return absBound(s, a, memo); };
    auto noise = [&](int i) {
        // |ImprovedNoise.noise()| <= 1.5: the noise is a convex combination
        // (the fade weights) of the corner terms g . d, g with components
        // in {-1, 0, 1}, so |g . d| <= |dx| + |dy| + |dz|. Per axis the
        // weighted sum of |d| is t (1 - s(t)) + (1 - t) s(t) = t + s(t) (1 - 2t)
        // with s the fade curve, which is below the diagonal for t < 1/2 and
        // symmetric: at most 1/2.
        const NormalNoiseState& ns = s.noises[(size_t) i];
        double b = 0;
        for (const NoiseLevel& l : ns.first)
            b += std::fabs(l.perlin.amplitude) * l.valueFactor * 1.5;
        for (const NoiseLevel& l : ns.second)
            b += std::fabs(l.perlin.amplitude) * l.valueFactor * 1.5;
        return b * std::fabs(ns.valueFactor);
    };
    double r = HUGE_VAL;
    switch ((int) n.op)
    {
    case OP_CONSTANT: r = std::fabs(n.p[0]); break;
    case OP_NOISE:
    case OP_SHIFTED_NOISE: r = noise(n.noise); break;
    case OP_SHIFT_A:
    case OP_SHIFT_B:
    case OP_SHIFT: r = 4.0 * noise(n.noise); break;
    case OP_WEIRD_SCALED: r = (n.p[0] == 1 ? 2.0 : 3.0) * noise(n.noise); break;
    case OP_Y_GRADIENT: r = std::max(std::fabs(n.p[2]), std::fabs(n.p[3])); break;
    case OP_ADD: r = arg(n.a) + arg(n.b); break;
    case OP_ADD_CONST: r = arg(n.a) + std::fabs(n.p[0]); break;
    case OP_MUL: r = arg(n.a) * arg(n.b); break;
    case OP_MUL_CONST: r = arg(n.a) * std::fabs(n.p[0]); break;
    case OP_MIN:
    case OP_MAX: r = std::max(arg(n.a), arg(n.b)); break;
    case OP_ABS:
    case OP_HALF_NEGATIVE:
    case OP_QUARTER_NEGATIVE:
    case OP_INTERPOLATED:
    case OP_CACHE_2D: r = arg(n.a); break;
    case OP_SQUARE: r = arg(n.a) * arg(n.a); break;
    case OP_CUBE: r = arg(n.a) * arg(n.a) * arg(n.a); break;
    case OP_CLAMP: r = std::max(std::fabs(n.p[0]), std::fabs(n.p[1])); break;
    case OP_SQUEEZE: r = 0.5 + 1.0 / 24.0; break;
    case OP_RANGE_CHOICE: r = std::max(arg(n.b), arg(n.c)); break;
    default: break;
    }
    if (!(r < HUGE_VAL))
        r = HUGE_VAL;
    else
        r = r * (1.0 + 1e-9) + 1e-12;   // the rounding of the operations
    m = r;
    return r;
}

DensityState& AquiferState::at(int slot, int x, int y, int z)
{
    DensityState& s = *states[(size_t) def.program[slot]];
    s.evalColumn(x, z);
    s.evalVertical(y);
    return s;
}

AquiferState::Status AquiferState::picker(int y) const
{
    // NoiseBasedChunkGenerator.createFluidPicker()
    if (y < std::min(-54, def.seaLevel))
        return Status { -54, FK_LAVA };
    return Status { def.seaLevel, def.defaultFluid };
}

uint8_t AquiferState::global(int y) const
{
    const Status g = picker(y);
    return fluidAt(g.level, g.type, y);
}

bool AquiferState::inRange(int x, int z) const
{
    const int qx = (x >> 2) - range.qx, qz = (z >> 2) - range.qz;
    return qx >= 0 && qz >= 0 && qx <= range.n && qz <= range.n;
}

int AquiferState::preliminarySurface(int x, int z)
{
    const int qx = x & ~3, qz = z & ~3;
    if (surfaceCache.empty())
        surfaceCache.assign(1u << 15, SurfaceEntry { 0, 0, 0, false });
    SurfaceEntry& e = surfaceCache[(size_t) ((((uint64_t) (uint32_t) qx * 0x9E3779B97F4A7C15ull) ^
        ((uint64_t) (uint32_t) qz * 0xC2B2AE3D27D4EB4Full)) >> 49)];
    if (e.used && e.qx == qx && e.qz == qz)
        return e.v;
    DensityState& s = *states[(size_t) def.program[AquiferDef::SURFACE]];
    s.evalColumn(qx, qz);
    int v = INT_MAX;
    if (def.variant == AquiferDef::V1_21_9)
    {
        // NoiseChunk.computePreliminarySurfaceLevel(): the router's
        // preliminary_surface_level at y 0, Mth.floor()
        s.evalVertical(0);
        const double f = s.value(def.surface);
        v = def.floorSaturates ? floorSat(f) : floorOld(f);
    }
    else
    {
        // From the top of the noise range, a cell height at a time, the
        // first height where the function exceeds 0.390625 (1.18.2:
        // NoiseRouterData.computePreliminarySurfaceLevelScanning(), which
        // shifts, clamps and slides the value first). Runs of heights whose
        // bounds stay at or below the threshold are skipped.
        const bool old = def.variant == AquiferDef::V1_18_2;
        const double *e = def.slide;
        const int ch = old ? (int) e[0] : def.cellHeight;
        const int top = old ? ((int) e[1] + (int) e[2]) * ch : def.minY + def.height;
        const int count = old ? (int) e[2] + 1 : (top - def.minY) / ch + 1;
        auto value118 = [&](double f, int y) {
            f = f + -0.703125;
            f = f < -64.0 ? -64.0 : f > 64.0 ? 64.0 : f;
            // NoiseRouterData.applySlide(): the slide of the noise settings
            const double dc = (double) (y / ch - (int) e[1]);
            if (e[4] > 0)
                f = clampedLerpD(e[3], f, (e[2] - dc - e[5]) / e[4]);
            if (e[7] > 0)
                f = clampedLerpD(e[6], f, (dc - e[8]) / e[7]);
            return f;
        };
        // the bounds of a run: the shift, the clamp and the slides are
        // monotone in the value, and a slide moves it towards its target
        double limit = 0.390625;
        bool never = false;
        if (old)
        {
            if ((e[4] > 0 && e[3] > 0.390625) || (e[7] > 0 && e[6] > 0.390625))
                never = true;   // (a slide target above the threshold: no bounds)
            limit = 0.390625 + 0.703125;
        }
        const bool prune = terrainPruning() && !never;
        const bool caches = !def.surfaceCaches.empty();
        auto exceeds = [&](int y, bool first) {
            if (first || !caches)
                s.evalVertical(y);
            else
            {
                // the caches keep the first height's value
                for (int32_t idx : def.surfaceScan)
                    if (!def.isSurfaceCache[(size_t) idx])
                        s.run(&idx, 1, y);
            }
            const double f = s.value(def.surface);
            return (old ? value118(f, y) : f) > 0.390625;
        };
        // (k: the k-th height from the top; a stack of index ranges, the
        // upper part first)
        int stack[64][2], sp = 0;
        int k0 = 0;
        if (caches)
        {
            // the top first: the scan's first call fixes the caches
            if (exceeds(top, true))
                v = top;
            k0 = 1;
        }
        // Near columns have near surfaces: the heights above the last
        // surface found, but the two nearest, first as one run, then those
        // two, then the rest (the first height that exceeds is the same in
        // any order of the runs).
        const int hk = lastSurface != INT_MAX && (top - lastSurface) % ch == 0 ? (top - lastSurface) / ch : -1;
        if (v == INT_MAX && prune && hk - 2 > k0 && hk < count)
        {
            const int runs[4][2] = { { hk + 1, count - 1 }, { hk, hk }, { hk - 1, hk - 1 }, { k0, hk - 2 } };
            for (const auto& r : runs)
                if (r[0] <= r[1])
                {
                    stack[sp][0] = r[0];
                    stack[sp][1] = r[1];
                    sp++;
                }
        }
        else
        {
            stack[sp][0] = k0;
            stack[sp][1] = count - 1;
            sp += k0 <= count - 1 && v == INT_MAX;
        }
        while (sp > 0 && v == INT_MAX)
        {
            sp--;
            const int ka = stack[sp][0], kb = stack[sp][1];
            // (1.18.2: the clamp at 64 keeps a value above the shifted
            // threshold above it)
            if (prune && kb - ka >= 1 && !mayExceed(s, top - kb * ch, top - ka * ch, limit))
                continue;
            if (!prune || kb - ka < 2)
            {
                for (int k = ka; k <= kb; k++)
                {
                    const int y = top - k * ch;
                    if (exceeds(y, false))
                    {
                        v = y;
                        break;
                    }
                }
                continue;
            }
            const int mid = (ka + kb) / 2;
            stack[sp][0] = mid + 1;
            stack[sp][1] = kb;
            sp++;
            stack[sp][0] = ka;
            stack[sp][1] = mid;
            sp++;
        }
    }
    if (aborted())
        return v;
    if (v != INT_MAX)
        lastSurface = v;
    e = SurfaceEntry { qx, qz, v, true };
    return v;
}

void AquiferState::setColumn(int x, int z, const Range& r)
{
    colX = x;
    colZ = z;
    const int gx = (x - 5) >> 4, gz = (z - 5) >> 4;
    const bool sameRange = r.qx == range.qx && r.qz == range.qz && r.n == range.n;
    // the statuses depend on the range only through the flat caches
    const bool rangeMatters = def.rangeCenter || def.rangeSpread || def.rangeLava;
    if (gx != gx0 || gz != gz0 || (!sameRange && rangeMatters))
    {
        // keep the tables of recent anchor cells (the map visits a row of
        // noise cells, then the next one)
        if (ctxs.empty())
            ctxs.resize(32);
        Ctx *slot = nullptr;
        for (Ctx& c : ctxs)
            if (!c.valid || !slot || (slot->valid && c.used < slot->used))
            {
                slot = &c;
                if (!c.valid)
                    break;
            }
        if (layers > 0)
        {
            slot->valid = true;
            slot->gx0 = gx0;
            slot->gz0 = gz0;
            slot->range = range;
            slot->used = ++ctxTick;
            slot->centers.swap(centers);
            slot->centerSet.swap(centerSet);
            slot->layerInfo.swap(layerInfo);
            slot->layer0 = layer0;
            slot->layers = layers;
        }
        gx0 = gx;
        gz0 = gz;
        layers = 0;
        centers.clear();
        centerSet.clear();
        layerInfo.clear();
        for (Ctx& c : ctxs)
        {
            if (!c.valid || c.gx0 != gx || c.gz0 != gz)
                continue;
            if (rangeMatters && (c.range.qx != r.qx || c.range.qz != r.qz || c.range.n != r.n))
                continue;
            c.valid = false;
            centers.swap(c.centers);
            centerSet.swap(c.centerSet);
            layerInfo.swap(c.layerInfo);
            layer0 = c.layer0;
            layers = c.layers;
            break;
        }
    }
    if (!sameRange || r.chunkX != range.chunkX || r.chunkZ != range.chunkZ)
    {
        skipKnown = false;
        skipLow = INT_MIN;
    }
    range = r;
    boundKnown = false;
    barrierX = 0x7fffffff;
}

// The highest fluid level any center of the column can have: the global
// levels, or the lowest preliminary surface of its samples. Each of the four
// candidate cells has a sample in the column's anchor cell (the offsets
// (0, 0), (-1, 0), (0, -1), (-1, -1)) at the quart of its center.
int AquiferState::levelBound()
{
    if (boundGX != gx0 || boundGZ != gz0)
    {
        const size_t bi = (size_t) (((uint32_t) gx0 * 0x9E3779B1u ^ (uint32_t) gz0 * 0x85EBCA77u) >> 26);
        if (boundTable.empty())
            boundTable.assign(64, BoundEntry { 0, 0, 0, false });
        BoundEntry& be = boundTable[bi];
        if (be.used && be.gx == gx0 && be.gz == gz0)
        {
            boundGX = gx0;
            boundGZ = gz0;
            boundValue = be.p;
            return std::max(std::max(def.seaLevel, -54), boundValue);
        }
        int p = INT_MIN;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                p = std::max(p, preliminarySurface(gx0 * 16 + i * 4, gz0 * 16 + j * 4));
        boundGX = aborted() ? 0x7fffffff : gx0;
        boundGZ = gz0;
        boundValue = p;
        if (!aborted())
            be = BoundEntry { gx0, gz0, p, true };
    }
    return std::max(std::max(def.seaLevel, -54), boundValue);
}

namespace
{

const double INF = HUGE_VAL;

// outwards by at least two units in the last place (see dpterrain.cpp)
inline double down(double v) { return v - (std::fabs(v) * 0x1p-50 + 0x1p-1000); }
inline double up(double v) { return v + (std::fabs(v) * 0x1p-50 + 0x1p-1000); }

} // namespace

// Can the surface function of the column s holds exceed @threshold at a
// height in ylo .. yhi? From bounds of its nodes that change with y (the
// others are exact in s).
bool AquiferState::mayExceed(DensityState& s, int ylo, int yhi, double threshold)
{
    if (iv.empty())
    {
        const size_t n = graph.nodes.size();
        iv.assign(n, Interval { 0.0, 0.0 });
        ivScratch.assign(n, 0.0);
        for (size_t i = 0; i < n; i++)
            if (graph.nodes[i].op == OP_CONSTANT)
                ivScratch[i] = graph.nodes[i].p[0];
        noiseBound.assign(graph.noises.size(), INF);
        for (int32_t i : def.programs[(size_t) def.program[AquiferDef::SURFACE]].usedNoises)
        {
            // |ImprovedNoise.noise()| <= 2 (dpterrain.cpp)
            const NormalNoiseState& ns = s.noises[(size_t) i];
            double b = 0;
            for (const NoiseLevel& l : ns.first)
                b += std::fabs(l.perlin.amplitude) * l.valueFactor * 2.0;
            for (const NoiseLevel& l : ns.second)
                b += std::fabs(l.perlin.amplitude) * l.valueFactor * 2.0;
            noiseBound[(size_t) i] = b * std::fabs(ns.valueFactor) * (1.0 + 1e-12);
        }
    }
    ivYlo = ylo;
    ivYhi = yhi;
    // the nodes that change with y, in dependency order (with caches that
    // keep their value: the nodes the scan evaluates)
    const std::vector<int32_t>& list = def.surfaceCaches.empty()
        ? def.programs[(size_t) def.program[AquiferDef::SURFACE]].vertical : def.surfaceScan;
    for (int32_t i : list)
        iv[(size_t) i] = interval(s, i);
    const DensityNode& out = graph.nodes[(size_t) def.surface];
    return ((out.flags & DF_USES_Y) ? iv[(size_t) def.surface].hi : s.reg[(size_t) def.surface]) > threshold;
}

// The bounds of @node over the heights ivYlo .. ivYhi of the column s holds,
// from those of its arguments (iv of the arguments that change with y, the
// values of the others): TerrainState::interval() of a corner column,
// without leaves
AquiferState::Interval AquiferState::interval(const DensityState& s, int32_t node)
{
    const DensityNode& n = graph.nodes[(size_t) node];
    Interval r { -INF, INF };
    auto arg = [&](int32_t a) {
        if (!(graph.nodes[(size_t) a].flags & DF_USES_Y))
            return Interval { s.reg[(size_t) a], s.reg[(size_t) a] };
        return iv[(size_t) a];
    };
    auto exact = [](const Interval& i) { return i.lo == i.hi; };
    std::vector<int>& args = ivArgs;
    args.clear();
    switch ((int) n.op)
    {
    case OP_CONSTANT:
        r = Interval { n.p[0], n.p[0] };
        break;
    case OP_Y_GRADIENT:
    {
        const double y0 = (n.flags & DF_FLAT) ? 0.0 : (double) ivYlo;
        const double y1 = (n.flags & DF_FLAT) ? 0.0 : (double) ivYhi;
        double a = clampedMap(y0, n.p[0], n.p[1], n.p[2], n.p[3]);
        double b = clampedMap(y1, n.p[0], n.p[1], n.p[2], n.p[3]);
        r = Interval { std::min(a, b), std::max(a, b) };
        break;
    }
    case OP_NOISE:
    case OP_SHIFTED_NOISE:
        r = Interval { -noiseBound[(size_t) n.noise], noiseBound[(size_t) n.noise] };
        break;
    case OP_SHIFT_A:
    case OP_SHIFT_B:
    case OP_SHIFT:
        r = Interval { -4.0 * noiseBound[(size_t) n.noise], 4.0 * noiseBound[(size_t) n.noise] };
        break;
    case OP_WEIRD_SCALED:
        r = Interval { 0.0, (n.p[0] == 1 ? 2.0 : 3.0) * noiseBound[(size_t) n.noise] };
        break;
    case OP_BLENDED_NOISE:
    case OP_BLENDED_NOISE_118:
        r = Interval { -2.0, 2.0 };
        break;
    case OP_SLIDE:
    {
        // clampedLerp(target, v, f) of the top, then of the bottom slide:
        // between v and the targets (one rounding each)
        Interval a = arg(n.a);
        const double *e = &graph.extras[(size_t) n.extra];
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
        // exact if every argument is; else the clamp of the shaper's spline
        // (peaks and valleys is at most 1)
        graph.arguments(node, args);
        bool ok = true;
        for (size_t k = 0; k < args.size() && ok; k++)
        {
            Interval a = arg(args[k]);
            ok = exact(a);
            ivScratch[(size_t) args[k]] = a.lo;
        }
        if (ok)
        {
            const double v = s.compute(node, ivScratch.data(), s.cx, 0, s.cz);
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
        // monotone: the values at the ends
        Interval a = arg(n.a);
        ivScratch[(size_t) n.a] = a.lo;
        double lo = s.compute(node, ivScratch.data(), 0, 0, 0);
        ivScratch[(size_t) n.a] = a.hi;
        double hi = s.compute(node, ivScratch.data(), 0, 0, 0);
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
    {
        // exact if every argument is
        graph.arguments(node, args);
        bool ok = true;
        for (size_t k = 0; k < args.size() && ok; k++)
        {
            Interval a = arg(args[k]);
            ok = exact(a);
            ivScratch[(size_t) args[k]] = a.lo;
        }
        if (ok)
        {
            const double v = s.compute(node, ivScratch.data(), s.cx, 0, s.cz);
            r = Interval { v, v };
        }
        break;
    }
    case OP_INTERPOLATED:
        r = arg(n.a);
        break;
    case OP_CACHE_2D:
        // the value of the scan's first height (see preliminarySurface())
        if (def.isSurfaceCache.empty() || !def.isSurfaceCache[(size_t) node])
            r = arg(n.a);
        else
            r = Interval { s.reg[(size_t) node], s.reg[(size_t) node] };
        break;
    default:
        break;
    }
    if (!(r.lo <= r.hi))    // NaN
        r = Interval { -INF, INF };
    return r;
}

int AquiferState::computeSkip(int chunkX, int chunkZ)
{
    const uint64_t key = (uint64_t) (uint32_t) chunkX << 32 | (uint32_t) chunkZ;
    auto it = skipCache.find(key);
    if (it != skipCache.end())
        return it->second;
    // Aquifer.NoiseBasedAquifer(): the highest preliminary surface of the
    // columns of the chunk's grid cells (a step of 4)
    const int minGX = (chunkX * 16 - 5) >> 4, maxGX = ((chunkX * 16 + 15 - 5) >> 4) + 1;
    const int minGZ = (chunkZ * 16 - 5) >> 4, maxGZ = ((chunkZ * 16 + 15 - 5) >> 4) + 1;
    int maxP = INT_MIN;
    for (int bz = minGZ * 16; bz <= maxGZ * 16 + 9; bz += 4)
        for (int bx = minGX * 16; bx <= maxGX * 16 + 9; bx += 4)
            maxP = std::max(maxP, preliminarySurface(bx, bz));
    const int adjusted = add32(maxP, 8);
    const int gy = add32(floorDiv(add32(adjusted, 12), 12), 1);
    const int skip = add32((int) ((uint32_t) gy * 12u), 11) - 1;
    if (aborted())
        return skip;
    if (skipCache.size() >= (1u << 12))
        skipCache.clear();
    skipCache.emplace(key, skip);
    return skip;
}

int AquiferState::globalAbove()
{
    if (boundKnown)
        return boundY;
    boundKnown = true;
    const int bound = levelBound();
    const double nb = barrierBound;
    int64_t above = INT64_MAX;
    if (bound != INT_MAX && nb < 1e6)
        above = (int64_t) bound + (int64_t) std::ceil(2.5 * nb) + 1;
    if (def.variant == AquiferDef::V1_21_9)
    {
        // the chunk takes the global picker's block above its skip height:
        // known without the chunk's surfaces if they cannot be below the
        // height found here (the anchor cell's centers are among them)
        bool need = true;
        if (boundValue > INT_MIN && boundValue < (1 << 30))
        {
            skipLow = 12 * (floorDiv(boundValue + 20, 12) + 1) + 10;
            need = (int64_t) skipLow + 1 < above;
        }
        if (need)
        {
            if (!skipKnown)
            {
                skipY = computeSkip(range.chunkX, range.chunkZ);
                skipKnown = true;
            }
            above = std::min(above, (int64_t) skipY + 1);
        }
    }
    boundY = (int) std::min(above, (int64_t) INT_MAX);
    return boundY;
}

// Makes room for the layers gyLo .. gyHi of the column's centers (the table
// moves: no pointer into it may be held across this)
uint8_t AquiferState::lavaOr(int y, uint8_t fluid) const
{
    const Status g = picker(y);
    return fluidAt(g.level, g.type, y) == FK_LAVA ? FK_LAVA : fluid;
}

const AquiferState::LayerInfo& AquiferState::layer(int ya, bool exact)
{
    layersFor(ya - 1, ya + 1);
    {
        const LayerInfo& known = layerInfo[(size_t) (ya - layer0)];
        if (known.set && (known.exact || !exact))
            return known;
    }
    LayerInfo l { INT_MAX, false, 0, FK_AIR, false, true };
    // The statuses the caches already hold give the levels; else the bound
    // of each center's level: the global levels or the preliminary surface
    // of its sample in the column's anchor cell (see levelBound()), which
    // spares the statuses where the columns share none (far scales).
    int maxLevel = std::max(def.seaLevel, -54);
    bool known = true, same = true;
    Status first { 0, FK_AIR };
    for (int y1 = -1; y1 <= 1; y1++)
        for (int x1 = 0; x1 <= 1; x1++)
            for (int z1 = 0; z1 <= 1; z1++)
            {
                Center& c = center(gx0 + x1, ya + y1, gz0 + z1);
                Status st;
                if (exact)
                    st = status(c);
                else if (!peekStatus(c, &st))
                {
                    known = false;
                    const int p = preliminarySurface(gx0 * 16 + ((c.x & 15) & ~3), gz0 * 16 + ((c.z & 15) & ~3));
                    maxLevel = std::max(maxLevel, p);
                    continue;
                }
                if (y1 == -1 && x1 == 0 && z1 == 0)
                    first = st;
                same = same && st.level == first.level && st.type == first.type;
                maxLevel = std::max(maxLevel, st.level);
            }
    // (see globalAbove(): above every level by the barrier's margin)
    if (barrierBound < 1e6 && maxLevel != INT_MAX)
        l.globalFrom = (int) std::min((int64_t) INT_MAX, (int64_t) maxLevel + (int64_t) std::ceil(2.5 * barrierBound) + 1);
    // the skip above the surface of 1.21.9 gives the global picker's blocks
    // instead: only below its known bound
    l.uniform = known && same && (def.variant != AquiferDef::V1_21_9 || 12 * ya + 10 <= skipLow);
    l.level = first.level;
    l.type = first.type;
    l.exact = known;
    if (aborted())
    {
        scratchLayer = l;
        return scratchLayer;
    }
    // (center() above may not grow the tables: layersFor() made room)
    LayerInfo& slot = layerInfo[(size_t) (ya - layer0)];
    slot = l;
    return slot;
}

// status() if it is known without computing it
bool AquiferState::peekStatus(Center& cc, Status *out)
{
    if (cc.hasStatus)
    {
        *out = cc.st;
        return true;
    }
    if (statusCache.empty())
        return false;
    const uint64_t key = statusKey(cc);
    const StatusEntry& e = statusCache[(size_t) ((key * 0x9E3779B97F4A7C15ull) >> 50)];
    if (!e.used || e.key != key)
        return false;
    cc.st = e.st;
    cc.hasStatus = true;
    *out = e.st;
    return true;
}

uint64_t AquiferState::statusKey(const Center& cc) const
{
    // the flat caches the status depends on: inside the noise chunk or not
    int mask = 0;
    if (def.rangeCenter && inRange(cc.x, cc.z))
        mask |= 1;
    if (def.rangeSpread && inRange(floorDiv(cc.x, 16), floorDiv(cc.z, 16)))
        mask |= 2;
    if (def.rangeLava && inRange(floorDiv(cc.x, 64), floorDiv(cc.z, 64)))
        mask |= 4;
    const int gx = cc.x >> 4, gy = floorDiv(cc.y, 12), gz = cc.z >> 4;
    return ((uint64_t) (gx & 0x3fffff) << 42) | ((uint64_t) (gz & 0x3fffff) << 20) |
        ((uint64_t) (gy & 0x1ffff) << 3) | (uint64_t) mask;
}

void AquiferState::layersFor(int gyLo, int gyHi)
{
    if (layers > 0 && gyLo >= layer0 && gyHi < layer0 + layers)
        return;
    const int lo = layers > 0 ? std::min(layer0, gyLo - 4) : gyLo - 4;
    const int hi = layers > 0 ? std::max(layer0 + layers, gyHi + 5) : gyHi + 5;
    std::vector<Center> c((size_t) (hi - lo) * 4);
    std::vector<char> s((size_t) (hi - lo) * 4, 0);
    std::vector<LayerInfo> li((size_t) (hi - lo), LayerInfo { 0, false, 0, FK_AIR, false, false });
    for (int i = 0; i < layers * 4; i++)
    {
        c[(size_t) ((layer0 - lo) * 4 + i)] = centers[(size_t) i];
        s[(size_t) ((layer0 - lo) * 4 + i)] = centerSet[(size_t) i];
    }
    for (int i = 0; i < layers; i++)
        li[(size_t) (layer0 - lo + i)] = layerInfo[(size_t) i];
    centers.swap(c);
    centerSet.swap(s);
    layerInfo.swap(li);
    layer0 = lo;
    layers = hi - lo;
}

AquiferState::Center& AquiferState::center(int gx, int gy, int gz)
{
    const size_t i = (size_t) ((gy - layer0) * 4 + (gx - gx0) * 2 + (gz - gz0));
    Center& c = centers[i];
    if (!centerSet[i])
    {
        int rx, ry, rz;
        if (def.legacy)
        {
            uint64_t r;
            setSeed(&r, (uint64_t) positionSeed(gx, gy, gz) ^ posLo);
            rx = nextInt(&r, 10);
            ry = nextInt(&r, 9);
            rz = nextInt(&r, 10);
        }
        else
        {
            Xoroshiro r = xoroshiro((uint64_t) positionSeed(gx, gy, gz) ^ posLo, posHi);
            rx = xNextInt(&r, 10);
            ry = xNextInt(&r, 9);
            rz = xNextInt(&r, 10);
        }
        c.x = wrapBits(add32((int) ((uint32_t) gx * 16u), rx), 26);
        c.y = wrapBits(add32((int) ((uint32_t) gy * 12u), ry), 12);
        c.z = wrapBits(add32((int) ((uint32_t) gz * 16u), rz), 26);
        c.hasStatus = false;
        centerSet[i] = 1;
    }
    return c;
}

const AquiferState::Status& AquiferState::status(Center& cc)
{
    if (cc.hasStatus)
        return cc.st;
    const uint64_t key = statusKey(cc);
    const int mask = (int) (key & 7);
    if (statusCache.empty())
        statusCache.assign(1u << 14, StatusEntry { 0, Status { 0, FK_AIR }, false });
    StatusEntry& e = statusCache[(size_t) ((key * 0x9E3779B97F4A7C15ull) >> 50)];
    if (!e.used || e.key != key)
    {
        const Status st = computeStatus(cc.x, cc.y, cc.z, mask);
        if (aborted())
        {
            scratchStatus = st;
            return scratchStatus;
        }
        e = StatusEntry { key, st, true };
    }
    cc.st = e.st;
    cc.hasStatus = true;
    return cc.st;
}

// Aquifer.NoiseBasedAquifer.computeFluid()
AquiferState::Status AquiferState::computeStatus(int x, int y, int z, int mask)
{
    const Status g = picker(y);
    int lowest = INT_MAX;
    const int top = y + 12, bottom = y - 12;
    bool flooded = false;
    const bool old = def.variant <= AquiferDef::V1_19_2;
    for (int k = 0; k < 13; k++)
    {
        const int *o = old ? SAMPLES_OLD[k] : SAMPLES[k];
        const int sx = add32(x, o[0] * 16), sz = add32(z, o[1] * 16);
        const int p = preliminarySurface(sx, sz);
        const int adj = add32(p, 8);
        const bool start = o[0] == 0 && o[1] == 0;
        if (start && bottom > adj)
            return g;
        const bool pokes = top > adj;
        if (pokes || start)
        {
            const Status s = picker(adj);
            if (fluidAt(s.level, s.type, adj) != FK_AIR)
            {
                if (start)
                    flooded = true;
                if (pokes)
                    return s;
            }
        }
        lowest = std::min(lowest, p);
    }

    const int cs = (mask & 1) ? AquiferDef::CENTER0 : AquiferDef::CENTER1;
    const int ci = (mask & 1) ? 0 : 1;
    const double f = flooded ? clampedMap((double) add32(add32(lowest, 8), -y), 0.0, 64.0, 1.0, 0.0) : 0.0;
    if (def.variant == AquiferDef::V1_18_2)
    {
        const double fl = at(cs, x, y, z).value(def.flood[ci]);
        const double gv = fl < -1.0 ? -1.0 : fl > 1.0 ? 1.0 : fl;
        if (gv > mapD(f, 1.0, 0.0, -0.3, 0.8))
            return g;
        if (gv <= mapD(f, 1.0, 0.0, -0.8, 0.4))
            return Status { WAY_BELOW_MIN_Y, g.type };
        const int fx = floorDiv(x, 16), fy = floorDiv(y, 40), fz = floorDiv(z, 16);
        const int ss = (mask & 2) ? AquiferDef::SPREAD0 : AquiferDef::SPREAD1;
        const double sp = at(ss, fx, fy, fz).value(def.spread[(mask & 2) ? 0 : 1]) * 10.0;
        const int q = floorOld(sp / 3.0) * 3;
        const int target = fy * 40 + 20 + q;
        const int level = std::min(lowest, target);
        if (target <= -10)
        {
            const int ls = (mask & 4) ? AquiferDef::LAVA0 : AquiferDef::LAVA1;
            const double lv = at(ls, floorDiv(x, 64), floorDiv(y, 40), floorDiv(z, 64)).value(def.lava[(mask & 4) ? 0 : 1]);
            if (std::fabs(lv) > 0.3)
                return Status { level, FK_LAVA };
        }
        return Status { level, g.type };
    }

    // computeSurfaceLevel()
    double partial, full;
    // the three functions at the center: one program
    DensityState& cst = at(cs, x, y, z);
    if (cst.value(def.erosion[ci]) < (double) -0.225F && cst.value(def.depth[ci]) > (double) 0.9F)
    {
        partial = -1.0;
        full = -1.0;
    }
    else
    {
        const double gv = clampD(cst.value(def.flood[ci]), -1.0, 1.0);
        const double h = mapD(f, 1.0, 0.0, -0.3, 0.8);
        const double k = mapD(f, 1.0, 0.0, -0.8, 0.4);
        partial = gv - k;
        full = gv - h;
    }
    int level;
    if (full > 0.0)
        level = g.level;
    else if (partial > 0.0)
    {
        // computeRandomizedFluidSurfaceLevel()
        const int fx = floorDiv(x, 16), fy = floorDiv(y, 40), fz = floorDiv(z, 16);
        const int ss = (mask & 2) ? AquiferDef::SPREAD0 : AquiferDef::SPREAD1;
        const double sp = at(ss, fx, fy, fz).value(def.spread[(mask & 2) ? 0 : 1]) * 10.0;
        const double qd = sp / 3.0;
        const int q = (def.floorSaturates ? floorSat(qd) : floorOld(qd)) * 3;
        level = std::min(lowest, fy * 40 + 20 + q);
    }
    else
        level = WAY_BELOW_MIN_Y;
    // computeFluidType()
    uint8_t type = g.type;
    if (level <= -10 && level != WAY_BELOW_MIN_Y && g.type != FK_LAVA)
    {
        const int ls = (mask & 4) ? AquiferDef::LAVA0 : AquiferDef::LAVA1;
        const double lv = at(ls, floorDiv(x, 64), floorDiv(y, 40), floorDiv(z, 64)).value(def.lava[(mask & 4) ? 0 : 1]);
        if (std::fabs(lv) > 0.3)
            type = FK_LAVA;
    }
    return Status { level, type };
}

// calculatePressure()
double AquiferState::pressure(int y, double *barrier, const Status& a, const Status& b)
{
    const uint8_t ta = fluidAt(a.level, a.type, y), tb = fluidAt(b.level, b.type, y);
    if ((ta == FK_LAVA && tb == FK_WATER) || (ta == FK_WATER && tb == FK_LAVA))
        return 2.0;
    const int diff = std::abs(a.level - b.level);
    if (diff == 0)
        return 0.0;
    const double avg = 0.5 * (double) add32(a.level, b.level);
    const double e = (double) y + 0.5 - avg;
    const double f = (double) diff / 2.0;
    const double o = f - std::fabs(e);
    double q;
    if (e > 0.0)
    {
        const double p = 0.0 + o;
        q = p > 0.0 ? p / 1.5 : p / 2.5;
    }
    else
    {
        const double p = 3.0 + o;
        q = p > 0.0 ? p / 3.0 : p / 10.0;
    }
    double r;
    if (!(q < -2.0) && !(q > 2.0))
    {
        if (*barrier != *barrier)
        {
            DensityState& s = *states[(size_t) def.program[AquiferDef::BARRIER]];
            if (barrierX != colX || barrierZ != colZ)
            {
                s.evalColumn(colX, colZ);
                barrierX = colX;
                barrierZ = colZ;
            }
            s.evalVertical(y);
            *barrier = s.value(def.barrier);
        }
        r = *barrier;
    }
    else
        r = 0.0;
    return 2.0 * (r + q);
}

// computeSubstance() up to the three nearest centers: returns the block if
// the global picker decides it, else 0xff and the centers and distances
uint8_t AquiferState::closest(int y, int *pd1, int *pd2, int *pd3, Center **pc1, Center **pc2, Center **pc3)
{
    const Status g = picker(y);
    if (def.variant == AquiferDef::V1_21_9 && y > skipLow)
    {
        if (!skipKnown)
        {
            skipY = computeSkip(range.chunkX, range.chunkZ);
            skipKnown = true;
        }
        if (y > skipY)
            return fluidAt(g.level, g.type, y);
    }
    if (fluidAt(g.level, g.type, y) == FK_LAVA)
        return FK_LAVA;
    const int ya = floorDiv(y + 1, 12);
    layersFor(ya - 1, ya + 1);
    int d1 = INT_MAX, d2 = INT_MAX, d3 = INT_MAX;
    Center *c1 = nullptr, *c2 = nullptr, *c3 = nullptr;
    for (int x1 = 0; x1 <= 1; x1++)
        for (int y1 = -1; y1 <= 1; y1++)
            for (int z1 = 0; z1 <= 1; z1++)
            {
                Center& c = center(gx0 + x1, ya + y1, gz0 + z1);
                const int dx = c.x - colX, dy = c.y - y, dz = c.z - colZ;
                const int dist = dx * dx + dy * dy + dz * dz;
                if (d1 >= dist)
                {
                    c3 = c2; c2 = c1; c1 = &c;
                    d3 = d2; d2 = d1; d1 = dist;
                }
                else if (d2 >= dist)
                {
                    c3 = c2; c2 = &c;
                    d3 = d2; d2 = dist;
                }
                else if (d3 >= dist)
                {
                    c3 = &c;
                    d3 = dist;
                }
            }
    *pd1 = d1; *pd2 = d2; *pd3 = d3;
    *pc1 = c1; *pc2 = c2; *pc3 = c3;
    return 0xff;
}

} // namespace dp
