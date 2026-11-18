#include "dpdensity.h"
#include "dpeval.h"
#include "dpinternal.h"

extern "C"
{
#include "biomes.h"
}

#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>

// The density functions and noises follow Minecraft 1.21.1
// (net.minecraft.world.level.levelgen.DensityFunctions, NoiseChunk,
// RandomState, synth.NormalNoise / PerlinNoise / BlendedNoise,
// net.minecraft.util.CubicSpline), checked against the game's bytecode: the
// order of the floating point operations is the game's. The TypeScript port
// in deepslate (Copyright (c) 2021 Misode, MIT License) served as a readable
// reference for the JSON format and the structure.
//
// This file must be compiled without fused multiply-adds (-ffp-contract=off),
// like the rest of the app: Java never contracts a * b + c.

namespace dp
{

// Mth.floor() of 1.21.9 to 1.21.11: (int) d, minus one if d is below it; the
// cast saturates like Java's d2i and NaN gives 0 (so -1e20 gives INT_MIN - 1,
// which wraps to INT_MAX)
int floorOld(double d)
{
    if (d != d)
        return 0;
    if (d >= 2147483647.0)
        return INT_MAX;
    if (d < -2147483648.0)
        return INT_MAX;
    const int i = (int) d;
    return d < (double) i ? i - 1 : i;
}

// Mth.floor() since 26.1: (int) Math.floor(d), saturating
int floorSat(double d)
{
    const double f = std::floor(d);
    if (f != f)
        return 0;
    if (f >= 2147483647.0)
        return INT_MAX;
    if (f <= -2147483648.0)
        return INT_MIN;
    return (int) f;
}

//==============================================================================
// noises
//==============================================================================

int32_t javaStringHash(const QString& s)
{
    uint32_t h = 0;
    for (QChar c : s)
        h = 31u * h + (uint32_t) c.unicode();
    return (int32_t) h;
}

void hashOf(const QByteArray& name, uint64_t out[2])
{
    // RandomSupport.seedFromHashOf(): md5, the first and second eight bytes
    // as big-endian longs
    QByteArray h = QCryptographicHash::hash(name, QCryptographicHash::Md5);
    const unsigned char *b = (const unsigned char*) h.constData();
    for (int k = 0; k < 2; k++)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v = (v << 8) | b[k * 8 + i];
        out[k] = v;
    }
}

namespace
{

// A xoroshiro random with the state of Xoroshiro128PlusPlus(lo, hi)
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

// PerlinNoise.wrap()
inline double wrapCoord(double x)
{
    if (x >= -1.6777216E7 && x < 1.6777216E7)
        return x;   // the general formula gives exactly x here
    return x - std::floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}

// PerlinNoise.getValue(x, y, z)
inline double sampleLevels(const std::vector<NoiseLevel>& levels, double x, double y, double z)
{
    double v = 0;
    for (const NoiseLevel& l : levels)
    {
        const double e = l.perlin.lacunarity;
        double g = samplePerlin(&l.perlin, wrapCoord(x * e), wrapCoord(y * e), wrapCoord(z * e), 0, 0);
        v += l.perlin.amplitude * g * l.valueFactor;
    }
    return v;
}

// The xoroshiro branch of the PerlinNoise constructor: one level per
// non-zero amplitude, each seeded from the name of its octave.
void initLevels(std::vector<NoiseLevel>& levels, Xoroshiro *rng, const NoiseDef& def)
{
    // forkPositional()
    uint64_t lo = xNextLong(rng);
    uint64_t hi = xNextLong(rng);
    const int n = (int) def.amplitudes.size();
    double e = std::ldexp(1.0, def.firstOctave);                        // lowestFreqInputFactor
    double f = std::ldexp(1.0, n - 1) / (std::ldexp(1.0, n) - 1.0);     // lowestFreqValueFactor
    levels.clear();
    for (int i = 0; i < n; i++)
    {
        if (def.amplitudes[i] != 0.0)
        {
            uint64_t h[2];
            hashOf(QByteArray("octave_") + QByteArray::number(def.firstOctave + i), h);
            Xoroshiro x = xoroshiro(lo ^ h[0], hi ^ h[1]);
            NoiseLevel l;
            xPerlinInit(&l.perlin, &x);
            l.perlin.amplitude = def.amplitudes[i];
            l.perlin.lacunarity = e;
            l.valueFactor = f;
            levels.push_back(l);
        }
        e *= 2.0;
        f /= 2.0;
    }
}

// NormalNoise's valueFactor: 1/6 / expectedDeviation(imax - imin), int
// arithmetic wraps like Java's
double expectedValueFactor(const NoiseDef& def)
{
    int imin = 0x7fffffff, imax = (int) 0x80000000;
    for (int i = 0; i < (int) def.amplitudes.size(); i++)
    {
        if (def.amplitudes[i] != 0.0)
        {
            imin = std::min(imin, i);
            imax = std::max(imax, i);
        }
    }
    int span = (int) ((unsigned) imax - (unsigned) imin);
    double dev = 0.1 * (1.0 + 1.0 / (double) (int) ((unsigned) span + 1u));
    return 0.16666666666666666 / dev;
}

// NormalNoise.create(random.fromHashOf(id), parameters)
void initNormalNoise(NormalNoiseState *s, uint64_t factoryLo, uint64_t factoryHi, const NoiseDef& def)
{
    Xoroshiro rng = xoroshiro(factoryLo ^ def.hash[0], factoryHi ^ def.hash[1]);
    initLevels(s->first, &rng, def);
    initLevels(s->second, &rng, def);
    s->valueFactor = expectedValueFactor(def);
}

// A Java random (LegacyRandomSource) seeded with @seed
inline uint64_t javaRandom(uint64_t seed)
{
    uint64_t s;
    setSeed(&s, seed);
    return s;
}

// The positional branch of the PerlinNoise constructor with a
// LegacyRandomSource: forkPositional() takes nextLong() as the seed of the
// factory, fromHashOf(name) seeds a new random with name.hashCode() ^ seed.
void initLevelsLegacy(std::vector<NoiseLevel>& levels, uint64_t *rng, const NoiseDef& def)
{
    const uint64_t factory = nextLong(rng);
    const int n = (int) def.amplitudes.size();
    double e = std::ldexp(1.0, def.firstOctave);
    double f = std::ldexp(1.0, n - 1) / (std::ldexp(1.0, n) - 1.0);
    levels.clear();
    for (int i = 0; i < n; i++)
    {
        if (def.amplitudes[i] != 0.0)
        {
            int32_t h = javaStringHash(QStringLiteral("octave_%1").arg(def.firstOctave + i));
            uint64_t r = javaRandom((uint64_t) (int64_t) h ^ factory);
            NoiseLevel l;
            perlinInit(&l.perlin, &r);
            l.perlin.amplitude = def.amplitudes[i];
            l.perlin.lacunarity = e;
            l.valueFactor = f;
            levels.push_back(l);
        }
        e *= 2.0;
        f /= 2.0;
    }
}

// The non-positional ("legacy") branch of the PerlinNoise constructor, as
// PerlinNoise.createLegacyForLegacyNetherBiome() uses it: the octave of
// frequency 1 is created first (and dropped if the noise has no such
// octave), then the lower ones; a missing octave skips its 262 calls.
void initLevelsSequential(std::vector<NoiseLevel>& levels, uint64_t *rng, const NoiseDef& def)
{
    const int n = (int) def.amplitudes.size();
    const int l = -def.firstOctave;
    std::vector<PerlinNoise> made(n);
    std::vector<char> has(n, 0);
    {
        PerlinNoise first;
        perlinInit(&first, rng);
        if (l >= 0 && l < n && def.amplitudes[l] != 0.0)
        {
            made[l] = first;
            has[l] = 1;
        }
    }
    for (int k = l - 1; k >= 0; k--)
    {
        if (k < n && def.amplitudes[k] != 0.0)
        {
            perlinInit(&made[k], rng);
            has[k] = 1;
        }
        else
        {
            skipNextN(rng, 262);
        }
    }
    double e = std::ldexp(1.0, def.firstOctave);
    double f = std::ldexp(1.0, n - 1) / (std::ldexp(1.0, n) - 1.0);
    levels.clear();
    for (int i = 0; i < n; i++)
    {
        if (has[i])
        {
            NoiseLevel lv;
            lv.perlin = made[i];
            lv.perlin.amplitude = def.amplitudes[i];
            lv.perlin.lacunarity = e;
            lv.valueFactor = f;
            levels.push_back(lv);
        }
        e *= 2.0;
        f /= 2.0;
    }
}

// The Nether's biome noises of the legacy generator (NormalNoise.
// createLegacyNetherBiome()): LegacyRandomSource(world seed + @offset),
// the octaves created one after the other.
void initLegacyNetherBiome(NormalNoiseState *s, uint64_t worldSeed, int offset, const NoiseDef& def)
{
    uint64_t r = javaRandom(worldSeed + (uint64_t) offset);
    initLevelsSequential(s->first, &r, def);
    initLevelsSequential(s->second, &r, def);
    s->valueFactor = expectedValueFactor(def);
}

// 26.1 and 26.2 (RandomState's NoiseWiringHelper): the noises
// minecraft:nether/temperature and minecraft:nether/vegetation are the
// Nether's legacy biome noises (world seed + 0 / + 1) with the parameters of
// their files, with and without legacy_random_source. Returns false for the
// other noises.
bool initNetherBiomeNoise26(NormalNoiseState *s, uint64_t worldSeed, const NoiseDef& def, int mc)
{
    if (mc < MC_26_1)
        return false;
    if (def.id == QLatin1String("minecraft:nether/temperature"))
        initLegacyNetherBiome(s, worldSeed, 0, def);
    else if (def.id == QLatin1String("minecraft:nether/vegetation"))
        initLegacyNetherBiome(s, worldSeed, 1, def);
    else
        return false;
    return true;
}

// RandomState with legacy_random_source: the noise @def of the registry.
// Up to 1.21.11 the game replaces three noises by name (RandomState's
// NoiseWiringHelper): minecraft:temperature and minecraft:vegetation by the
// Nether's biome noises of the legacy generator (seeded with the world seed
// + 0 / + 1, first octave -7, amplitudes 1, 1), minecraft:offset (the
// shifts) by a noise without octaves (always 0). From 26.1 see
// initNetherBiomeNoise26(). The others come from the positional factory of
// the world seed.
void initNormalNoiseLegacy(NormalNoiseState *s, uint64_t worldSeed, uint64_t factory, const NoiseDef& def,
    int mc)
{
    if (initNetherBiomeNoise26(s, worldSeed, def, mc))
        return;
    if (mc < MC_26_1 &&
        (def.id == QLatin1String("minecraft:temperature") || def.id == QLatin1String("minecraft:vegetation")))
    {
        NoiseDef nether;
        nether.id = def.id;
        nether.firstOctave = -7;
        nether.amplitudes = { 1.0, 1.0 };
        initLegacyNetherBiome(s, worldSeed, def.id == QLatin1String("minecraft:vegetation") ? 1 : 0, nether);
        return;
    }
    if (mc < MC_26_1 && def.id == QLatin1String("minecraft:offset"))
    {
        s->first.clear();
        s->second.clear();
        s->valueFactor = 0.0;
        return;
    }
    uint64_t r = javaRandom((uint64_t) (int64_t) javaStringHash(def.id) ^ factory);
    initLevelsLegacy(s->first, &r, def);
    initLevelsLegacy(s->second, &r, def);
    s->valueFactor = expectedValueFactor(def);
}

inline float lerpF(float t, float a, float b)
{
    return a + t * (b - a);
}

} // namespace

double NormalNoiseState::sample(double x, double y, double z) const
{
    const double F = 1.0181268882175227;
    double a = sampleLevels(first, x, y, z);
    double b = sampleLevels(second, x * F, y * F, z * F);
    return (a + b) * valueFactor;
}

//==============================================================================
// compiler
//==============================================================================

namespace
{

QString typeName(const QJsonObject& o)
{
    QString t = o.value(QLatin1String("type")).toString();
    if (t.startsWith(QLatin1String("minecraft:")))
        t = t.mid(10);
    return t;
}

} // namespace

DensityGraph::DensityGraph(const Resources& res, Report *report, int mc)
    : res(res), report(report), mc(mc)
{
}

void DensityGraph::error(const Context& ctx, const QString& text)
{
    Issue i;
    i.severity = Issue::Error;
    i.pack = res.packName(ctx.pack);
    i.id = ctx.id;
    i.text = text;
    for (const Issue& o : report->issues)
        if (o.severity == i.severity && o.pack == i.pack && o.id == i.id && o.text == i.text)
            return;
    report->issues.push_back(i);
}

int DensityGraph::addNode(const DensityNode& node)
{
    DensityNode n = node;
    n.flags &= DF_FLAT | DF_QUART;

    // does the value change with y?
    bool usesY = false;
    const bool flat = (n.flags & DF_FLAT) != 0;
    switch ((int) n.op)
    {
    case OP_NOISE:
    case OP_SHIFTED_NOISE:
        usesY = !flat && n.p[1] != 0.0;     // y * 0 is 0
        break;
    case OP_Y_GRADIENT:
    case OP_SHIFT:
    case OP_BLENDED_NOISE:
    case OP_WEIRD_SCALED:
    case OP_SLIDE:
    case OP_BLENDED_NOISE_118:
    case OP_BLENDED_NOISE_26:
    case OP_DISTANCE:
        usesY = !flat;
        break;
    case OP_GRADIENT:
        usesY = (int) n.p[4] / 4 == 1;      // along y
        break;
    default:
        break;
    }
    // slice at a fixed y: does not see the sample's y
    const bool fixedY = n.op == OP_RELOCATE && ((int) n.p[0] & 2);
    std::vector<int> args;
    if (n.op == OP_FIND_TOP_SURFACE)
    {
        // scans the density itself: only the upper bound sees the sample's y
        if (n.b >= 0)
            args.push_back(n.b);
    }
    else
    {
        if (n.a >= 0) args.push_back(n.a);
        if (n.b >= 0) args.push_back(n.b);
        if (n.c >= 0) args.push_back(n.c);
        if (n.spline >= 0)
            splineArguments(n.spline, args);
    }
    for (int a : args)
        if ((nodes[a].flags & DF_USES_Y) && !fixedY)
            usesY = true;
    if (usesY)
        n.flags |= DF_USES_Y;

    QByteArray key;
    key.reserve(64);
    key.append((char) n.op);
    key.append((char) n.flags);
    key.append((const char*) &n.a, sizeof(n.a));
    key.append((const char*) &n.b, sizeof(n.b));
    key.append((const char*) &n.c, sizeof(n.c));
    key.append((const char*) &n.noise, sizeof(n.noise));
    key.append((const char*) &n.spline, sizeof(n.spline));
    key.append((const char*) n.p, sizeof(n.p));
    key.append((const char*) &n.extra, sizeof(n.extra));
    auto it = unique.constFind(key);
    if (it != unique.constEnd())
        return it.value();
    // the length of the longest path to a leaf: the evaluators recurse
    // along it (nested splines count one level each)
    int32_t h = 0;
    for (int a : { n.a, n.b, n.c })
        if (a >= 0)
            h = std::max(h, heights[(size_t) a]);
    if (n.spline >= 0)
        h = std::max(h, splineHeights[(size_t) n.spline]);
    // 26.3: interpolated inside interpolated (each level evaluates the
    // inner one at 8 corners)
    int32_t nest = 0;
    for (int a : { n.a, n.b, n.c })
        if (a >= 0)
            nest = std::max(nest, interpNesting[(size_t) a]);
    nodes.push_back(n);
    heights.push_back(h + 1);
    interpNesting.push_back(nest + (n.op == OP_INTERPOLATE ? 1 : 0));
    int idx = (int) nodes.size() - 1;
    unique.insert(key, idx);
    return idx;
}

int DensityGraph::intervalSelect(int input, const std::vector<int>& fn, const std::vector<double>& th, int lo, int hi)
{
    // The game takes the first function whose threshold is above the input
    // (the last one if there is none, also for NaN), with a search over the
    // thresholds. A balanced tree of range choices makes the same choice:
    // "input < threshold" is false up to some index and true after it (the
    // thresholds are ordered), so each node halves the candidates. A chain
    // would make the graph as deep as there are thresholds.
    if (lo == hi)
        return fn[(size_t) lo];
    const int mid = lo + (hi - lo) / 2;
    DensityNode c;
    c.op = OP_RANGE_CHOICE;
    c.a = input;
    c.p[0] = -std::numeric_limits<double>::infinity();
    c.p[1] = th[(size_t) mid];
    c.b = intervalSelect(input, fn, th, lo, mid);
    c.c = intervalSelect(input, fn, th, mid + 1, hi);
    if (c.b < 0 || c.c < 0)
        return -1;
    return addNode(c);
}

int DensityGraph::constant(double v)
{
    DensityNode n;
    n.op = OP_CONSTANT;
    n.p[0] = v;
    return addNode(n);
}

void DensityGraph::splineArguments(int spline, std::vector<int>& out) const
{
    const SplineDef& s = splines[spline];
    out.push_back(s.coordinate);
    for (int i = 0; i < s.count; i++)
    {
        const SplinePoint& p = points[s.first + i];
        if (p.spline >= 0)
            splineArguments(p.spline, out);
    }
}

void DensityGraph::arguments(int node, std::vector<int>& out) const
{
    const DensityNode& n = nodes[node];
    if (n.a >= 0) out.push_back(n.a);
    if (n.b >= 0) out.push_back(n.b);
    if (n.c >= 0) out.push_back(n.c);
    if (n.spline >= 0)
        splineArguments(n.spline, out);
}

int DensityGraph::compile(const QJsonValue& v, int pack, const QString& id, int settings)
{
    Context ctx;
    ctx.pack = pack;
    ctx.id = id;
    ctx.flat = false;
    ctx.settings = settings;
    return compileValue(v, ctx);
}

int DensityGraph::compilePlain(const QJsonValue& v, int pack, const QString& id, int settings)
{
    Context ctx;
    ctx.pack = pack;
    ctx.id = id;
    ctx.flat = false;
    ctx.plain = true;
    ctx.settings = settings;
    return compileValue(v, ctx);
}

int DensityGraph::compileSurface(const QJsonValue& v, int pack, const QString& id, int settings)
{
    Context ctx;
    ctx.pack = pack;
    ctx.id = id;
    ctx.flat = false;
    ctx.plain = true;
    ctx.keepCache2d = true;
    ctx.settings = settings;
    return compileValue(v, ctx);
}

int DensityGraph::addExtras(const double *v, int n)
{
    int first = (int) extras.size();
    extras.insert(extras.end(), v, v + n);
    return first;
}

int DensityGraph::compileReference(const QString& id, int pack, const QString& from)
{
    Context ctx;
    ctx.pack = pack;
    ctx.id = from;
    ctx.flat = false;
    return compileRef(normalizeId(id), ctx);
}

void DensityGraph::checkNoise(const QString& id)
{
    Context ctx;
    const Resource *r = res.find(REG_NOISE, id);
    ctx.pack = r ? r->pack : -1;
    ctx.id = id;
    ctx.flat = false;
    noiseIndex(QJsonValue(id), ctx);
}

int DensityGraph::compileRef(const QString& id, const Context& ctx)
{
    QString key = id + (ctx.flat ? QLatin1String("|1") : QLatin1String("|0")) +
        (ctx.plain ? QLatin1String("p") : QLatin1String("")) + (ctx.keepCache2d ? QLatin1String("c") : QLatin1String(""));
    if (ctx.settings >= 0)
        key += QLatin1Char('|') + QString::number(ctx.settings);
    auto it = refs.constFind(key);
    if (it != refs.constEnd())
    {
        if (it.value() == -2)
        {
            error(ctx, QStringLiteral("The density function %1 refers to itself (through %2).")
                .arg(id, ctx.id));
            return -1;
        }
        return it.value();
    }
    const Resource *r = res.find(REG_DENSITY_FUNCTION, id);
    if (!r)
    {
        error(ctx, QStringLiteral("The density function %1 does not exist.").arg(id));
        refs.insert(key, -1);
        return -1;
    }
    refs.insert(key, -2);
    Context sub;
    sub.pack = r->pack;
    sub.id = id;
    sub.flat = ctx.flat;
    sub.plain = ctx.plain;
    sub.keepCache2d = ctx.keepCache2d;
    sub.settings = ctx.settings;
    QJsonValue v;
    QString err;
    int node = -1;
    if (!parseJson(r->data, &v, &err))
        error(sub, QStringLiteral("The density function is not valid JSON: %1.").arg(err));
    else
        node = compileValue(v, sub);
    refs.insert(key, node);
    return node;
}

int DensityGraph::compileValue(const QJsonValue& v, const Context& ctx)
{
    // JSON nesting and chains of references both recurse here: limit the
    // depth (the loader runs on a thread with a small stack), and the
    // height of the graph that the evaluators recurse along. One message per
    // compile() call.
    if (depth == 0)
        depthReported = false;
    if (depth >= MAX_COMPILE_DEPTH)
    {
        if (!depthReported)
            error(ctx, tooDeep());
        depthReported = true;
        return -1;
    }
    depth++;
    deepest = std::max(deepest, depth);
    int node = compileValueAt(v, ctx);
    depth--;
    if (node >= 0 && heights[(size_t) node] > MAX_DEPTH)
    {
        if (!depthReported)
            error(ctx, tooDeep());
        depthReported = true;
        return -1;
    }
    return node;
}

QString DensityGraph::tooDeep() const
{
    return QStringLiteral("The density function is nested too deeply (more than %1 functions inside each "
        "other, or more than %2 levels of JSON and references to other files).").arg(MAX_DEPTH).arg(MAX_COMPILE_DEPTH);
}

int DensityGraph::compileValueAt(const QJsonValue& v, const Context& ctx)
{
    if (v.isString())
        return compileRef(normalizeId(v.toString()), ctx);
    if (v.isDouble())
    {
        if (mc >= MC_26_3)
        {
            // a float within +-1000000
            if (!(std::fabs(v.toDouble()) <= 1000000.0))
            {
                error(ctx, QStringLiteral("A constant density function must be within +-1000000."));
                return -1;
            }
            return constant((double) (float) v.toDouble());
        }
        return constant(v.toDouble());
    }
    if (v.isObject())
        return compileObject(v.toObject(), ctx);
    error(ctx, QStringLiteral("A density function must be a number, a reference or an object."));
    return -1;
}

int DensityGraph::noiseIndex(const QJsonValue& v, const Context& ctx)
{
    if (!v.isString())
    {
        if (v.isObject())
            error(ctx, QStringLiteral("A noise is defined inline; the game can only seed noises "
                "that are files in worldgen/noise."));
        else
            error(ctx, QStringLiteral("The reference to a noise is missing."));
        return -1;
    }
    QString id = normalizeId(v.toString());
    auto it = noiseIds.constFind(id);
    if (it != noiseIds.constEnd())
        return it.value();

    int idx = -1;
    const Resource *r = res.find(REG_NOISE, id);
    if (!r)
    {
        error(ctx, QStringLiteral("The noise %1 does not exist.").arg(id));
    }
    else
    {
        Context sub;
        sub.pack = r->pack;
        sub.id = id;
        sub.flat = false;
        QJsonValue nv;
        QString err;
        if (!parseJson(r->data, &nv, &err) || !nv.isObject())
        {
            error(sub, QStringLiteral("The noise is not valid JSON: %1.").arg(err));
        }
        else
        {
            QJsonObject o = nv.toObject();
            QJsonValue fo = o.value(QLatin1String("firstOctave"));
            QJsonValue amps = o.value(QLatin1String("amplitudes"));
            NoiseDef def26;
            QString err26;
            if (mc >= MC_26_3)
            {
                // the format of 26.3 (dpformats.cpp)
                if (!parseNoise26(o, &def26, &err26))
                {
                    error(sub, err26);
                }
                else
                {
                    def26.id = id;
                    hashOf(id.toUtf8(), def26.hash);
                    noises.push_back(def26);
                    idx = (int) noises.size() - 1;
                }
            }
            else if (!fo.isDouble() || !amps.isArray())
            {
                error(sub, QStringLiteral("The noise needs the fields firstOctave and amplitudes."));
            }
            else
            {
                NoiseDef def;
                def.id = id;
                def.firstOctave = fo.toInt();
                for (const QJsonValue& a : amps.toArray())
                    def.amplitudes.push_back(a.toDouble());
                int n = (int) def.amplitudes.size();
                if (n == 0 || n > 64 || def.firstOctave < -1000 || def.firstOctave > 1000)
                {
                    error(sub, QStringLiteral("The noise has an invalid number of amplitudes or first octave."));
                }
                else
                {
                    hashOf(id.toUtf8(), def.hash);
                    noises.push_back(def);
                    idx = (int) noises.size() - 1;
                }
            }
        }
    }
    noiseIds.insert(id, idx);
    return idx;
}

int DensityGraph::compileSpline(const QJsonValue& v, const Context& ctx, bool *ok,
    const QHash<QString, int> *names)
{
    if (depth >= MAX_COMPILE_DEPTH)
    {
        if (!depthReported)
            error(ctx, tooDeep());
        depthReported = true;
        *ok = false;
        return -1;
    }
    depth++;
    deepest = std::max(deepest, depth);
    int r = compileSplineAt(v, ctx, ok, names);
    depth--;
    return r;
}

int DensityGraph::compileSplineAt(const QJsonValue& v, const Context& ctx, bool *ok,
    const QHash<QString, int> *names)
{
    // the caller handles constant splines (numbers)
    if (!v.isObject())
    {
        error(ctx, QStringLiteral("A spline must be a number or an object."));
        *ok = false;
        return -1;
    }
    QJsonObject o = v.toObject();
    if (!o.contains(QLatin1String("coordinate")) || !o.value(QLatin1String("points")).isArray())
    {
        error(ctx, QStringLiteral("A spline needs the fields coordinate and points."));
        *ok = false;
        return -1;
    }
    int coord;
    if (names)
    {
        QString name = o.value(QLatin1String("coordinate")).toString();
        if (name.startsWith(QLatin1String("minecraft:")))
            name = name.mid(10);
        coord = names->value(name, -1);
        if (coord < 0)
            error(ctx, QStringLiteral("Unknown spline coordinate \"%1\".")
                .arg(o.value(QLatin1String("coordinate")).toString()));
    }
    else
        coord = compileValue(o.value(QLatin1String("coordinate")), ctx);
    if (coord < 0)
    {
        *ok = false;
        return -1;
    }
    QJsonArray pts = o.value(QLatin1String("points")).toArray();
    if (pts.isEmpty())
    {
        error(ctx, QStringLiteral("A spline has no points."));
        *ok = false;
        return -1;
    }
    std::vector<SplinePoint> list;
    for (const QJsonValue& pv : pts)
    {
        QJsonObject po = pv.toObject();
        QJsonValue loc = po.value(QLatin1String("location"));
        QJsonValue der = po.value(QLatin1String("derivative"));
        QJsonValue val = po.value(QLatin1String("value"));
        if (!loc.isDouble() || !der.isDouble() || (!val.isDouble() && !val.isObject()))
        {
            error(ctx, QStringLiteral("A spline point needs the fields location, value and derivative."));
            *ok = false;
            return -1;
        }
        SplinePoint p;
        p.location = (float) loc.toDouble();
        p.derivative = (float) der.toDouble();
        p.value = 0;
        p.spline = -1;
        if (val.isDouble())
        {
            p.value = (float) val.toDouble();
        }
        else
        {
            p.spline = compileSpline(val, ctx, ok, names);
            if (!*ok)
                return -1;
        }
        list.push_back(p);
    }

    // identical splines (vanilla repeats many) share one definition
    QByteArray key;
    key.append((const char*) &coord, sizeof(coord));
    for (const SplinePoint& p : list)
    {
        key.append((const char*) &p.location, sizeof(float));
        key.append((const char*) &p.derivative, sizeof(float));
        key.append((const char*) &p.value, sizeof(float));
        key.append((const char*) &p.spline, sizeof(int32_t));
    }
    auto it = splineUnique.constFind(key);
    if (it != splineUnique.constEnd())
        return it.value();

    SplineDef s;
    s.coordinate = coord;
    s.first = (int32_t) points.size();
    s.count = (int32_t) list.size();
    int32_t h = heights[(size_t) coord];
    for (const SplinePoint& p : list)
        if (p.spline >= 0)
            h = std::max(h, splineHeights[(size_t) p.spline]);
    points.insert(points.end(), list.begin(), list.end());
    splines.push_back(s);
    splineHeights.push_back(h + 1);
    int idx = (int) splines.size() - 1;
    splineUnique.insert(key, idx);
    return idx;
}

int DensityGraph::compileObject(const QJsonObject& o, const Context& ctx)
{
    const QString type = typeName(o);
    if (type.isEmpty())
    {
        error(ctx, QStringLiteral("A density function has no type."));
        return -1;
    }
    // straight to the format of 26.3 (dpformats.cpp), without the stack
    // frame of the older types: the compiler recurses through it
    if (mc >= MC_26_3)
        return compile26(type, o, ctx);
    return compileObjectOld(type, o, ctx);
}

int DensityGraph::compileObjectOld(const QString& type, const QJsonObject& o, const Context& ctx)
{

    // reads a required number
    bool ok = true;
    auto num = [&](const char *key) -> double {
        QJsonValue v = o.value(QLatin1String(key));
        if (!v.isDouble())
        {
            if (ok)
                error(ctx, QStringLiteral("The density function of type %1 needs the number \"%2\".")
                    .arg(type, QLatin1String(key)));
            ok = false;
            return 0;
        }
        return v.toDouble();
    };
    // compiles a required argument
    auto arg = [&](const char *key, const Context& c) -> int {
        if (!o.contains(QLatin1String(key)))
        {
            if (ok)
                error(ctx, QStringLiteral("The density function of type %1 needs the argument \"%2\".")
                    .arg(type, QLatin1String(key)));
            ok = false;
            return -1;
        }
        int n = compileValue(o.value(QLatin1String(key)), c);
        if (n < 0)
            ok = false;
        return n;
    };
    // is the argument a literal constant in the file (the game's Constant)?
    auto literal = [&](const char *key, double *value) -> bool {
        QJsonValue v = o.value(QLatin1String(key));
        if (v.isDouble())
        {
            *value = v.toDouble();
            return true;
        }
        if (v.isObject() && typeName(v.toObject()) == QLatin1String("constant") &&
            v.toObject().value(QLatin1String("argument")).isDouble())
        {
            *value = v.toObject().value(QLatin1String("argument")).toDouble();
            return true;
        }
        return false;
    };

    DensityNode n;
    if (ctx.flat)
        n.flags = DF_FLAT;
    // nodes inside a flat_cache are sampled at quart positions
    auto add = [&](DensityNode& node) {
        if (ctx.flat)
            node.flags |= DF_QUART;
        return addNode(node);
    };

    if (mc <= MC_1_18_2)
    {
        // the types (or fields) that differ in 1.18.2 (dpformats.cpp)
        bool handled = false;
        int r = compileLegacy(type, o, ctx, &handled);
        if (handled)
            return r;
    }

    // markers ------------------------------------------------------------
    if (type == QLatin1String("flat_cache"))
    {
        // NoiseChunk.FlatCache: the argument sampled at the quart position
        // at y 0. Biomes are sampled at quart positions, so only y changes.
        // The plain sampler has no cache: the argument as it is.
        if (ctx.plain)
            return arg("argument", ctx);
        Context flat = ctx;
        flat.flat = true;
        return arg("argument", flat);
    }
    if (type == QLatin1String("cache_2d") && ctx.keepCache2d)
    {
        int a = arg("argument", ctx);
        if (!ok)
            return a;
        n.op = OP_CACHE_2D;
        n.a = a;
        return add(n);
    }
    if (type == QLatin1String("cache_2d") || type == QLatin1String("cache_once") ||
        type == QLatin1String("cache_all_in_cell"))
    {
        // These cache values the game computes anyway: the value is the
        // argument's (cache_2d holds the last column it was asked for).
        return arg("argument", ctx);
    }
    if (type == QLatin1String("interpolated"))
    {
        // Inside a flat_cache the game samples the argument directly.
        int a = arg("argument", ctx);
        if (!ok || ctx.flat)
            return a;
        n.op = OP_INTERPOLATED;
        n.a = a;
        return add(n);
    }
    if (type == QLatin1String("blend_density"))
        return arg("argument", ctx);    // no old chunks to blend with
    if (type == QLatin1String("blend_alpha"))
        return constant(1.0);
    if (type == QLatin1String("blend_offset"))
        return constant(0.0);
    if (type == QLatin1String("beardifier"))
        return constant(0.0);           // no structures

    // leaves -------------------------------------------------------------
    if (type == QLatin1String("constant"))
    {
        double v = num("argument");
        return ok ? constant(v) : -1;
    }
    if (type == QLatin1String("y_clamped_gradient"))
    {
        n.op = OP_Y_GRADIENT;
        n.p[0] = num("from_y");
        n.p[1] = num("to_y");
        n.p[2] = num("from_value");
        n.p[3] = num("to_value");
        if (!ok)
            return -1;
        n.p[0] = (double) (int) n.p[0];
        n.p[1] = (double) (int) n.p[1];
        if (ctx.flat)
            return constant(clampedMap(0.0, n.p[0], n.p[1], n.p[2], n.p[3]));
        return add(n);
    }
    if (type == QLatin1String("noise"))
    {
        n.op = OP_NOISE;
        n.noise = noiseIndex(o.value(QLatin1String("noise")), ctx);
        n.p[0] = num("xz_scale");
        n.p[1] = num("y_scale");
        if (!ok || n.noise < 0)
            return -1;
        if (n.p[1] == 0.0)
            n.flags |= DF_FLAT;     // y * 0 is 0
        if (n.flags & DF_FLAT)
            n.p[1] = 0.0;
        return add(n);
    }
    if (type == QLatin1String("shifted_noise"))
    {
        n.op = OP_SHIFTED_NOISE;
        n.noise = noiseIndex(o.value(QLatin1String("noise")), ctx);
        n.p[0] = num("xz_scale");
        n.p[1] = num("y_scale");
        n.a = arg("shift_x", ctx);
        n.b = arg("shift_y", ctx);
        n.c = arg("shift_z", ctx);
        if (!ok || n.noise < 0)
            return -1;
        if (n.p[1] == 0.0)
            n.flags |= DF_FLAT;
        if (n.flags & DF_FLAT)
            n.p[1] = 0.0;
        return add(n);
    }
    if (type == QLatin1String("shift_a") || type == QLatin1String("shift_b") ||
        type == QLatin1String("shift"))
    {
        n.op = type == QLatin1String("shift_a") ? OP_SHIFT_A
             : type == QLatin1String("shift_b") ? OP_SHIFT_B : OP_SHIFT;
        n.noise = noiseIndex(o.value(QLatin1String("argument")), ctx);
        if (n.noise < 0)
            return -1;
        if (n.op != OP_SHIFT)
            n.flags = 0;    // these do not use y
        return add(n);
    }
    if (type == QLatin1String("old_blended_noise"))
    {
        n.op = OP_BLENDED_NOISE;
        n.p[0] = num("xz_scale");
        n.p[1] = num("y_scale");
        n.p[2] = num("xz_factor");
        n.p[3] = num("y_factor");
        n.p[4] = num("smear_scale_multiplier");
        if (!ok)
            return -1;
        return add(n);
    }
    if (type == QLatin1String("end_islands"))
    {
        n.op = OP_END_ISLANDS;
        n.flags = 0;
        return add(n);
    }

    // one argument ---------------------------------------------------------
    static const struct { const char *name; DensityOp op; int since; } mapped[] = {
        { "abs", OP_ABS, 0 },
        { "square", OP_SQUARE, 0 },
        { "cube", OP_CUBE, 0 },
        { "half_negative", OP_HALF_NEGATIVE, 0 },
        { "quarter_negative", OP_QUARTER_NEGATIVE, 0 },
        { "squeeze", OP_SQUEEZE, 0 },
        { "invert", OP_INVERT, MC_1_21_9 },
    };
    for (const auto& m : mapped)
    {
        if (type != QLatin1String(m.name) || mc < m.since)
            continue;
        n.op = m.op;
        n.flags = 0;
        n.a = arg("argument", ctx);
        if (!ok)
            return -1;
        return add(n);
    }
    if (type == QLatin1String("clamp"))
    {
        n.op = OP_CLAMP;
        n.flags = 0;
        n.a = arg("input", ctx);
        n.p[0] = num("min");
        n.p[1] = num("max");
        if (!ok)
            return -1;
        return add(n);
    }
    if (type == QLatin1String("weird_scaled_sampler"))
    {
        n.op = OP_WEIRD_SCALED;
        n.a = arg("input", ctx);
        n.noise = noiseIndex(o.value(QLatin1String("noise")), ctx);
        QString mapper = o.value(QLatin1String("rarity_value_mapper")).toString();
        if (mapper == QLatin1String("type_1"))
            n.p[0] = 1;
        else if (mapper == QLatin1String("type_2"))
            n.p[0] = 2;
        else
        {
            if (ok)
                error(ctx, QStringLiteral("weird_scaled_sampler needs the rarity_value_mapper "
                    "\"type_1\" or \"type_2\"."));
            ok = false;
        }
        if (!ok || n.noise < 0)
            return -1;
        return add(n);
    }

    // two arguments --------------------------------------------------------
    if (type == QLatin1String("add") || type == QLatin1String("mul") ||
        type == QLatin1String("min") || type == QLatin1String("max"))
    {
        n.flags = 0;
        const bool isAdd = type == QLatin1String("add");
        const bool isMul = type == QLatin1String("mul");
        int a = arg("argument1", ctx);
        int b = arg("argument2", ctx);
        if (!ok)
            return -1;
        if (isAdd || isMul)
        {
            // With a literal constant the game computes "other (op) constant"
            // (MulOrAdd), without the short cut of the general mul.
            double c;
            int other = -1;
            if (literal("argument1", &c))
                other = b;
            else if (literal("argument2", &c))
                other = a;
            if (other >= 0)
            {
                if (nodes[other].op == OP_CONSTANT)
                    return constant(isAdd ? nodes[other].p[0] + c : nodes[other].p[0] * c);
                n.op = (DensityOp) (isAdd ? OP_ADD_CONST : OP_MUL_CONST);
                n.a = other;
                n.p[0] = c;
                return add(n);
            }
        }
        const bool ca = nodes[a].op == OP_CONSTANT;
        const bool cb = nodes[b].op == OP_CONSTANT;
        const double va = nodes[a].p[0], vb = nodes[b].p[0];
        if (isMul && ca && va == 0.0)
            return constant(0.0);   // the general mul: a == 0 gives 0
        if (ca && cb)
        {
            if (isAdd) return constant(va + vb);
            if (isMul) return constant(va * vb);
            if (type == QLatin1String("min")) return constant(javaMin(va, vb));
            return constant(javaMax(va, vb));
        }
        n.op = isAdd ? OP_ADD : isMul ? OP_MUL : type == QLatin1String("min") ? OP_MIN : OP_MAX;
        n.a = a;
        n.b = b;
        return add(n);
    }
    if (type == QLatin1String("range_choice"))
    {
        n.op = OP_RANGE_CHOICE;
        n.flags = 0;
        n.a = arg("input", ctx);
        n.p[0] = num("min_inclusive");
        n.p[1] = num("max_exclusive");
        n.b = arg("when_in_range", ctx);
        n.c = arg("when_out_of_range", ctx);
        if (!ok)
            return -1;
        return add(n);
    }
    if (type == QLatin1String("interval_select") && mc >= MC_26_2)
    {
        // IntervalSelect (26.2): the first function whose threshold is above
        // the input, else the last one; a chain of range choices
        const int in = arg("input", ctx);
        QJsonValue tv = o.value(QLatin1String("thresholds"));
        QJsonValue fv = o.value(QLatin1String("functions"));
        if (!ok)
            return -1;
        QJsonArray ts = tv.toArray(), fs = fv.toArray();
        bool valid = tv.isArray() && fv.isArray() && fs.size() >= 2 && ts.size() == fs.size() - 1;
        for (int i = 0; valid && i < ts.size(); i++)
            valid = ts[i].isDouble() && (i == 0 || ts[i - 1].toDouble() <= ts[i].toDouble());
        if (!valid)
        {
            error(ctx, QStringLiteral("interval_select needs ordered thresholds and one function more "
                "than thresholds (at least two)."));
            return -1;
        }
        std::vector<int> fn;
        for (const QJsonValue& f : fs)
        {
            fn.push_back(compileValue(f, ctx));
            if (fn.back() < 0)
                ok = false;
        }
        if (!ok)
            return -1;
        std::vector<double> th;
        for (const QJsonValue& t : ts)
            th.push_back(t.toDouble());
        return intervalSelect(in, fn, th, 0, (int) fn.size() - 1);
    }
    if (type == QLatin1String("spline"))
    {
        QJsonValue sv = o.value(QLatin1String("spline"));
        if (sv.isDouble())
            return constant((double) (float) sv.toDouble());
        n.op = OP_SPLINE;
        n.flags = 0;
        n.spline = compileSpline(sv, ctx, &ok);
        if (!ok)
            return -1;
        return add(n);
    }
    if (type == QLatin1String("find_top_surface") && mc >= MC_1_21_9)
    {
        n.op = OP_FIND_TOP_SURFACE;
        n.flags = 0;
        // the density is scanned over y, whatever cache this is in
        Context scan = ctx;
        scan.flat = false;
        n.a = arg("density", scan);
        n.b = arg("upper_bound", ctx);
        n.p[0] = num("lower_bound");
        n.p[1] = num("cell_height");
        if (!ok)
            return -1;
        // the game's codec: lower_bound an int from 2 * MIN_Y to 2 * MAX_Y,
        // cell_height a positive int
        if (!(n.p[0] >= -4064.0 && n.p[0] <= 4062.0 && n.p[0] == std::floor(n.p[0])))
        {
            error(ctx, QStringLiteral("find_top_surface: lower_bound must be a whole number from -4064 to 4062."));
            return -1;
        }
        if (!(n.p[1] >= 1.0 && n.p[1] <= 2147483647.0 && n.p[1] == std::floor(n.p[1])))
        {
            error(ctx, QStringLiteral("find_top_surface needs a whole cell_height of at least 1."));
            return -1;
        }
        return add(n);
    }

    error(ctx, QStringLiteral("Unknown density function type \"%1\".")
        .arg(o.value(QLatin1String("type")).toString()));
    return -1;
}

//==============================================================================
// program
//==============================================================================

namespace
{

// The nodes that evaluating @root needs. The density of a find_top_surface
// is scanned by that node itself: of its nodes only those that do not change
// with y are needed beforehand.
void collect(const DensityGraph& g, int root, std::vector<char>& mark)
{
    std::vector<int> stack;
    stack.push_back(root);
    std::vector<int> args;
    while (!stack.empty())
    {
        int i = stack.back();
        stack.pop_back();
        if (mark[i])
            continue;
        mark[i] = 1;
        const DensityNode& n = g.nodes[i];
        args.clear();
        if (n.op == OP_FIND_TOP_SURFACE)
        {
            args.push_back(n.b);
            std::vector<char> sub(g.nodes.size(), 0);
            collect(g, n.a, sub);
            for (size_t k = 0; k < sub.size(); k++)
                if (sub[k] && !(g.nodes[k].flags & DF_USES_Y))
                    args.push_back((int) k);
        }
        else
        {
            g.arguments(i, args);
        }
        for (int a : args)
            if (!mark[a])
                stack.push_back(a);
    }
}

} // namespace

void DensityProgram::build(const DensityGraph& graph, const std::vector<int>& outputs)
{
    column.clear();
    vertical.clear();
    usedNoises.clear();
    scans.clear();
    relocs.clear();
    usesBlended = usesEndIslands = false;

    std::vector<char> mark(graph.nodes.size(), 0);
    for (int o : outputs)
        if (o >= 0)
            collect(graph, o, mark);

    std::vector<char> noiseUsed(graph.noises.size(), 0);
    auto note = [&](const DensityNode& n) {
        if (n.noise >= 0)
            noiseUsed[n.noise] = 1;
        if (n.op == OP_BLENDED_NOISE || n.op == OP_BLENDED_NOISE_118)
            usesBlended = true;
        if (n.op == OP_END_ISLANDS || n.op == OP_END_OUTER)
            usesEndIslands = true;
    };

    // node indices are in dependency order: arguments come first
    for (size_t i = 0; i < graph.nodes.size(); i++)
    {
        if (!mark[i])
            continue;
        const DensityNode& n = graph.nodes[i];
        note(n);
        if (n.op == OP_CONSTANT)
            continue;
        if (n.op == OP_FIND_TOP_SURFACE)
        {
            Scan s;
            s.node = (int32_t) i;
            std::vector<char> sub(graph.nodes.size(), 0);
            collect(graph, n.a, sub);
            for (size_t k = 0; k < sub.size(); k++)
            {
                if (!sub[k])
                    continue;
                note(graph.nodes[k]);
                if ((graph.nodes[k].flags & DF_USES_Y) && graph.nodes[k].op != OP_CONSTANT)
                    s.nodes.push_back((int32_t) k);
            }
            scans.push_back(s);
        }
        if (n.op == OP_RELOCATE || n.op == OP_INTERPOLATE)
        {
            // the argument's nodes, to evaluate again at other positions
            Scan s;
            s.node = (int32_t) i;
            std::vector<char> sub(graph.nodes.size(), 0);
            collect(graph, n.a, sub);
            for (size_t k = 0; k < sub.size(); k++)
                if (sub[k] && graph.nodes[k].op != OP_CONSTANT)
                    s.nodes.push_back((int32_t) k);
            relocs.push_back(s);
        }
        if (n.flags & DF_USES_Y)
            vertical.push_back((int32_t) i);
        else
            column.push_back((int32_t) i);
    }
    for (size_t i = 0; i < noiseUsed.size(); i++)
        if (noiseUsed[i])
            usedNoises.push_back((int32_t) i);
}

//==============================================================================
// evaluation
//==============================================================================

DensityState::DensityState(const DensityGraph& graph, const DensityProgram& program, uint64_t seed, int mc,
    bool legacy)
    : graph(graph), program(program)
{
    floorSaturates = mc >= MC_26_1;
    reg.assign(graph.nodes.size(), 0.0);
    for (size_t i = 0; i < graph.nodes.size(); i++)
        if (graph.nodes[i].op == OP_CONSTANT)
            reg[i] = graph.nodes[i].p[0];

    // RandomState: the world seed's xoroshiro random, forked positionally
    Xoroshiro world;
    xSetSeed(&world, seed);
    uint64_t lo = xNextLong(&world);
    uint64_t hi = xNextLong(&world);

    noises.resize(graph.noises.size());
    if (legacy)
    {
        // LegacyRandomSource(seed).forkPositional()
        uint64_t r = javaRandom(seed);
        const uint64_t factory = nextLong(&r);
        for (int32_t i : program.usedNoises)
            if (!graph.noises[i].v26)
                initNormalNoiseLegacy(&noises[i], seed, factory, graph.noises[i], mc);
    }
    else
    {
        for (int32_t i : program.usedNoises)
            if (!graph.noises[i].v26 && !initNetherBiomeNoise26(&noises[i], seed, graph.noises[i], mc))
                initNormalNoise(&noises[i], lo, hi, graph.noises[i]);
    }
    // 26.3: float evaluation; its noises are seeded by init26() (the
    // loader does not offer 26.3 dimensions with legacy_random_source)
    f32 = mc >= MC_26_3;
    if (f32)
        init26(lo, hi, seed);   // dpformats.cpp

    if (program.usesBlended && legacy)
    {
        // BlendedNoise from LegacyRandomSource(seed), the same order
        uint64_t r = javaRandom(seed);
        for (int i = 0; i < 16; i++) perlinInit(&blended.minLimit[i], &r);
        for (int i = 0; i < 16; i++) perlinInit(&blended.maxLimit[i], &r);
        for (int i = 0; i < 8; i++) perlinInit(&blended.main[i], &r);
    }
    else if (program.usesBlended)
    {
        // BlendedNoise from the random of "minecraft:terrain": the two limit
        // noises with 16 octaves, the main noise with 8, created one after
        // the other; the first created is the octave with the frequency 1.
        uint64_t h[2];
        hashOf(QByteArray("minecraft:terrain"), h);
        Xoroshiro x = xoroshiro(lo ^ h[0], hi ^ h[1]);
        for (int i = 0; i < 16; i++) xPerlinInit(&blended.minLimit[i], &x);
        for (int i = 0; i < 16; i++) xPerlinInit(&blended.maxLimit[i], &x);
        for (int i = 0; i < 8; i++) xPerlinInit(&blended.main[i], &x);
    }
    memset(&endNoise, 0, sizeof(endNoise));
    if (program.usesEndIslands)
        setEndSeed(&endNoise, mc, seed);
}

float DensityState::spline(int index, const double *r) const
{
    const SplineDef& s = graph.splines[index];
    const SplinePoint *pt = &graph.points[s.first];
    const int n = s.count;
    const float f = (float) r[s.coordinate];

    // Mth.binarySearch(0, n, k -> f < location[k]) - 1
    int lo = 0, len = n;
    while (len > 0)
    {
        int half = len / 2;
        int k = lo + half;
        if (f < pt[k].location)
            len = half;
        else
        {
            lo = k + 1;
            len -= half + 1;
        }
    }
    const int i = lo - 1;

    if (i < 0 || i == n - 1)
    {
        // linearExtend()
        const SplinePoint& p = pt[i < 0 ? 0 : i];
        float v = p.spline >= 0 ? spline(p.spline, r) : p.value;
        if (p.derivative == 0.0F)
            return v;
        return v + p.derivative * (f - p.location);
    }
    const SplinePoint& p0 = pt[i];
    const SplinePoint& p1 = pt[i + 1];
    const float loc0 = p0.location, loc1 = p1.location;
    const float t = (f - loc0) / (loc1 - loc0);
    const float v0 = p0.spline >= 0 ? spline(p0.spline, r) : p0.value;
    const float v1 = p1.spline >= 0 ? spline(p1.spline, r) : p1.value;
    const float a = p0.derivative * (loc1 - loc0) - (v1 - v0);
    const float b = -p1.derivative * (loc1 - loc0) + (v1 - v0);
    return lerpF(t, v0, v1) + t * (1.0F - t) * lerpF(t, a, b);
}

double DensityState::blendedNoise(const DensityNode& n, int x, int y, int z) const
{
    // BlendedNoise.compute()
    const double xzMul = 684.412 * n.p[0];
    const double yMul = 684.412 * n.p[1];
    const double xzFactor = n.p[2], yFactor = n.p[3], smear = n.p[4];
    const double d = x * xzMul;
    const double e = y * yMul;
    const double f = z * xzMul;
    const double g = d / xzFactor;
    const double h = e / yFactor;
    const double i = f / xzFactor;
    const double j = yMul * smear;
    const double k = j / yFactor;
    double l = 0, m = 0, nn = 0;
    double o = 1.0;
    for (int p = 0; p < 8; p++)
    {
        nn += samplePerlinSmeared(&blended.main[p], wrapCoord(g * o), wrapCoord(h * o), wrapCoord(i * o),
            k * o, h * o) / o;
        o /= 2.0;
    }
    const double q = (nn / 10.0 + 1.0) / 2.0;
    const bool high = q >= 1.0;
    const bool low = q <= 0.0;
    o = 1.0;
    for (int r = 0; r < 16; r++)
    {
        const double s = wrapCoord(d * o);
        const double t = wrapCoord(e * o);
        const double u = wrapCoord(f * o);
        const double v = j * o;
        if (!high)
            l += samplePerlinSmeared(&blended.minLimit[r], s, t, u, v, e * o) / o;
        if (!low)
            m += samplePerlinSmeared(&blended.maxLimit[r], s, t, u, v, e * o) / o;
        o /= 2.0;
    }
    return clampedLerpD(l / 512.0, m / 512.0, q) / 128.0;
}

double DensityState::blendedNoise118(const DensityNode& n, int x, int y, int z) const
{
    // BlendedNoise.compute() of 1.18.2: sampled at the noise cell corners
    const double *e = &graph.extras[n.extra];
    const double xzScale = e[0], yScale = e[1], xzMain = e[2], yMain = e[3];
    const int cw = (int) e[4], ch = (int) e[5];
    const int i = floorDiv(x, cw), j = floorDiv(y, ch), k = floorDiv(z, cw);
    double l = 0, m = 0, nn = 0;
    double g = 1.0;
    for (int p = 0; p < 8; p++)
    {
        nn += samplePerlinSmeared(&blended.main[p], wrapCoord((double) i * xzMain * g),
            wrapCoord((double) j * yMain * g), wrapCoord((double) k * xzMain * g),
            yMain * g, (double) j * yMain * g) / g;
        g /= 2.0;
    }
    const double h = (nn / 10.0 + 1.0) / 2.0;
    const bool high = h >= 1.0;
    const bool low = h <= 0.0;
    g = 1.0;
    for (int r = 0; r < 16; r++)
    {
        const double s = wrapCoord((double) i * xzScale * g);
        const double t = wrapCoord((double) j * yScale * g);
        const double u = wrapCoord((double) k * xzScale * g);
        const double w = yScale * g;
        if (!high)
            l += samplePerlinSmeared(&blended.minLimit[r], s, t, u, w, (double) j * w) / g;
        if (!low)
            m += samplePerlinSmeared(&blended.maxLimit[r], s, t, u, w, (double) j * w) / g;
        g /= 2.0;
    }
    return clampedLerpD(l / 512.0, m / 512.0, h) / 128.0;
}

void DensityState::evalColumn(int x, int z)
{
    cx = x;
    cz = z;
    run(program.column.data(), program.column.size(), 0);
}

void DensityState::saveColumn(double *dst) const
{
    for (int32_t i : program.column)
        *dst++ = reg[i];
}

void DensityState::loadColumn(const double *src, int x, int z)
{
    cx = x;
    cz = z;
    for (int32_t i : program.column)
        reg[i] = *src++;
}

void DensityState::evalVertical(int y)
{
    run(program.vertical.data(), program.vertical.size(), y);
}

void DensityState::run(const int32_t *list, size_t count, int y)
{
    if (f32)
    {
        run26(list, count, y);
        return;
    }
    const DensityNode *nodes = graph.nodes.data();
    double *r = reg.data();
    const int x = cx, z = cz;
    for (size_t li = 0; li < count; li++)
    {
        const int32_t idx = list[li];
        if (nodes[idx].op != OP_FIND_TOP_SURFACE)
        {
            r[idx] = compute(idx, r, x, y, z);
            continue;
        }
        const DensityNode& n = nodes[idx];
        // Unverified port (the game has this type since 1.21.9): the
        // highest cell at or below the upper bound whose density is
        // positive, in steps of cell_height down to lower_bound.
        const DensityProgram::Scan *scan = nullptr;
        for (const DensityProgram::Scan& s : program.scans)
            if (s.node == idx)
                scan = &s;
        // FindTopSurface.compute(): Mth.floor(upper / cell) * cell and the
        // steps down in int arithmetic, as the game (it wraps); the scan
        // stops when the caller aborts (an upper bound far above the world
        // means billions of steps, in the game as well)
        const int lower = (int) n.p[0], cell = (int) n.p[1];
        const double q = r[n.b] / (double) cell;
        const int top = (int) ((uint32_t) (floorSaturates ? floorSat(q) : floorOld(q)) * (uint32_t) cell);
        double v = lower;
        if (scan && top > lower)
        {
            unsigned steps = 0;
            for (int sy = top; sy >= lower; sy = (int) ((uint32_t) sy - (uint32_t) cell))
            {
                if ((++steps & 63) == 0 && abort && abort->load(std::memory_order_relaxed))
                    break;
                run(scan->nodes.data(), scan->nodes.size(), sy);
                if (r[n.a] > 0.0)
                {
                    v = sy;
                    break;
                }
            }
            // nodes shared with the surrounding program hold the values
            // of the scan now: put back those of this sample
            if (n.flags & DF_USES_Y)
                run(scan->nodes.data(), scan->nodes.size(), y);
        }
        r[idx] = v;
    }
}

double DensityState::findTopSurface(int32_t idx, int x, int z, double upper)
{
    evalColumn(x, z);
    reg[graph.nodes[idx].b] = upper;
    run(&idx, 1, 0);
    return reg[idx];
}

} // namespace dp
