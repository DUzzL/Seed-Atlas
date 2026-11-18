#include "dpdensity.h"
#include "dpinternal.h"

extern "C"
{
#include "biomes.h"
}

#include <QJsonArray>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

// Density function formats of other game versions than the one dpdensity.cpp
// follows (1.21.1), lowered to the same node graph.
//
// 1.18.2 (net.minecraft.world.level.levelgen.DensityFunctions, NoiseRouterData,
// NoiseSettings, NoiseSlider, synth.BlendedNoise and
// net.minecraft.world.level.biome.TerrainShaper of that version, read from the
// game's bytecode with the official mappings): terrain_shaper_spline, slide
// and old_blended_noise have no parameters of their own but take them from
// the noise settings whose router uses them (the game binds them when it
// builds the router; unbound, terrain_shaper_spline gives 0 and slide its
// input). The terrain shaper's splines name their coordinates.
//
// 26.3 (net.minecraft.world.level.levelgen.densityfunction and synth of
// 26.3, read from the game's bytecode): new names and fields for most types,
// a new noise format, and every density function and noise is evaluated in
// float. A 26.3 graph is evaluated by DensityState::run26(). Where the game
// samples a whole chunk's climate at once (its volume path) and a single
// position differently (the zero short cut of mul and div, NaN and signed
// zero in min and max), this follows the volume path, which fills the chunks.

namespace dp
{

int DensityGraph::legacySettings(const QString& id, const QJsonObject& settings)
{
    if (mc > MC_1_18_2)
        return -1;
    for (size_t i = 0; i < legacy.size(); i++)
        if (legacy[i].id == id)
            return (int) i;

    const QJsonObject noise = settings.value(QLatin1String("noise")).toObject();
    // World::load() checks the noise settings like the game first; never
    // divide by a cell size outside 1 .. 4
    const int sizeV = noise.value(QLatin1String("size_vertical")).toInt(2);
    const int sizeH = noise.value(QLatin1String("size_horizontal")).toInt(1);
    if (sizeV < 1 || sizeV > 4 || sizeH < 1 || sizeH > 4)
        return -1;
    Legacy l;
    l.id = id;
    l.shaper = noise.value(QLatin1String("terrain_shaper")).toObject();
    const int minY = noise.value(QLatin1String("min_y")).toInt(-64);
    const int height = noise.value(QLatin1String("height")).toInt(384);
    // NoiseSettings.getCellHeight() / getCellWidth(): QuartPos.toBlock()
    const int cellHeight = sizeV * 4;
    const int cellWidth = sizeH * 4;
    const QJsonObject top = noise.value(QLatin1String("top_slide")).toObject();
    const QJsonObject bottom = noise.value(QLatin1String("bottom_slide")).toObject();
    l.slide[0] = cellHeight;
    l.slide[1] = floorDiv(minY, cellHeight);     // getMinCellY()
    l.slide[2] = height / cellHeight;               // getCellCountY()
    l.slide[3] = top.value(QLatin1String("target")).toDouble();
    l.slide[4] = top.value(QLatin1String("size")).toInt();
    l.slide[5] = top.value(QLatin1String("offset")).toInt();
    l.slide[6] = bottom.value(QLatin1String("target")).toDouble();
    l.slide[7] = bottom.value(QLatin1String("size")).toInt();
    l.slide[8] = bottom.value(QLatin1String("offset")).toInt();
    // BlendedNoise(random, NoiseSamplingSettings, cellWidth, cellHeight)
    const QJsonObject sampling = noise.value(QLatin1String("sampling")).toObject();
    const double xzScale = 684.412 * sampling.value(QLatin1String("xz_scale")).toDouble(1.0);
    const double yScale = 684.412 * sampling.value(QLatin1String("y_scale")).toDouble(1.0);
    l.blended[0] = xzScale;
    l.blended[1] = yScale;
    l.blended[2] = xzScale / sampling.value(QLatin1String("xz_factor")).toDouble(80.0);
    l.blended[3] = yScale / sampling.value(QLatin1String("y_factor")).toDouble(160.0);
    l.blended[4] = cellWidth;
    l.blended[5] = cellHeight;
    legacy.push_back(l);
    return (int) legacy.size() - 1;
}

int DensityGraph::compileLegacy(const QString& type, const QJsonObject& o, const Context& ctx, bool *handled)
{
    *handled = true;
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
    auto arg = [&](const char *key) -> int {
        if (!o.contains(QLatin1String(key)))
        {
            if (ok)
                error(ctx, QStringLiteral("The density function of type %1 needs the argument \"%2\".")
                    .arg(type, QLatin1String(key)));
            ok = false;
            return -1;
        }
        int n = compileValue(o.value(QLatin1String(key)), ctx);
        if (n < 0)
            ok = false;
        return n;
    };
    const Legacy *settings = ctx.settings >= 0 ? &legacy[ctx.settings] : nullptr;

    if (type == QLatin1String("terrain_shaper_spline"))
    {
        const int c = arg("continentalness");
        const int e = arg("erosion");
        const int w = arg("weirdness");
        const double lo = num("min_value");
        const double hi = num("max_value");
        const QString which = o.value(QLatin1String("spline")).toString();
        if (ok && which != QLatin1String("offset") && which != QLatin1String("factor") &&
            which != QLatin1String("jaggedness"))
        {
            error(ctx, QStringLiteral("terrain_shaper_spline needs the spline \"offset\", \"factor\" or "
                "\"jaggedness\"."));
            ok = false;
        }
        if (!ok)
            return -1;
        if (!settings)
            return constant(0.0);   // not bound to noise settings
        // TerrainShaper.offset() adds the global offset, in float
        const bool offset = which == QLatin1String("offset");
        const float add = offset ? -0.50375F : 0.0F;
        const QJsonValue sv = settings->shaper.value(which);
        auto clamp = [lo, hi](double d) { return d < lo ? lo : d > hi ? hi : d; };
        if (sv.isDouble())
        {
            float f = (float) sv.toDouble();
            if (offset)
                f = f + add;
            return constant(clamp((double) f));
        }
        DensityNode r;
        r.op = OP_PEAKS_VALLEYS;
        r.a = w;
        QHash<QString, int> names;
        names.insert(QStringLiteral("continents"), c);
        names.insert(QStringLiteral("erosion"), e);
        names.insert(QStringLiteral("weirdness"), w);
        names.insert(QStringLiteral("ridges"), addNode(r));
        Context sc = ctx;
        sc.id = settings->id;
        DensityNode n;
        n.op = OP_SHAPER_SPLINE;
        n.spline = compileSpline(sv, sc, &ok, &names);
        if (!ok)
            return -1;
        n.p[0] = lo;
        n.p[1] = hi;
        n.p[2] = add;
        if (!offset)
        {
            // no addition (it would turn -0 into +0)
            n.op = OP_SPLINE;
            const int s = addNode(n);
            DensityNode cl;
            cl.op = OP_CLAMP;
            cl.a = s;
            cl.p[0] = lo;
            cl.p[1] = hi;
            return addNode(cl);
        }
        return addNode(n);
    }
    if (type == QLatin1String("slide"))
    {
        const int a = arg("argument");
        if (!ok)
            return -1;
        if (!settings)
            return a;
        DensityNode n;
        n.op = OP_SLIDE;
        n.flags = ctx.flat ? DF_FLAT : 0;
        n.a = a;
        n.extra = addExtras(settings->slide, 9);
        return addNode(n);
    }
    if (type == QLatin1String("old_blended_noise"))
    {
        // a unit type in 1.18.2: the sampling of the noise settings
        static const double unbound[6] = {
            684.412, 684.412, 684.412 / 80.0, 684.412 / 160.0, 4, 8
        };
        DensityNode n;
        n.op = OP_BLENDED_NOISE_118;
        // inside a flat_cache: sampled at the quart position at y 0
        n.flags = ctx.flat ? DF_FLAT | DF_QUART : 0;
        n.extra = addExtras(settings ? settings->blended : unbound, 6);
        return addNode(n);
    }
    if (type == QLatin1String("spline"))
    {
        // the coordinates are density functions; the result is clamped
        const double lo = num("min_value");
        const double hi = num("max_value");
        QJsonValue sv = o.value(QLatin1String("spline"));
        if (!ok)
            return -1;
        if (sv.isDouble())
        {
            double d = (double) (float) sv.toDouble();
            return constant(d < lo ? lo : d > hi ? hi : d);
        }
        DensityNode n;
        n.op = OP_SPLINE;
        n.spline = compileSpline(sv, ctx, &ok);
        if (!ok)
            return -1;
        DensityNode cl;
        cl.op = OP_CLAMP;
        cl.a = addNode(n);
        cl.p[0] = lo;
        cl.p[1] = hi;
        return addNode(cl);
    }
    *handled = false;
    return -1;
}

//==============================================================================
// 26.3: compiler
//==============================================================================

namespace
{

QString typeName26(const QJsonObject& o)
{
    QString t = o.value(QLatin1String("type")).toString();
    if (t.startsWith(QLatin1String("minecraft:")))
        t = t.mid(10);
    return t;
}

inline double f32(double v) { return (double) (float) v; }

} // namespace

// Whether a value is a ConstantFunction once the game has inlined the
// references (a number, a "constant" object, or a reference to one)
bool DensityGraph::isConstant26(const QJsonValue& v, int depth) const
{
    if (v.isDouble())
        return true;
    if (v.isObject())
        return typeName26(v.toObject()) == QLatin1String("constant");
    if (v.isString() && depth < 32)
    {
        const Resource *r = res.find(REG_DENSITY_FUNCTION, normalizeId(v.toString()));
        QJsonValue inner;
        QString err;
        if (r && parseJson(r->data, &inner, &err))
            return isConstant26(inner, depth + 1);
    }
    return false;
}

// NormalNoise$Parameters of 26.3
bool DensityGraph::parseNoise26(const QJsonObject& o, NoiseDef *def, QString *err) const
{
    QJsonValue bo = o.value(QLatin1String("base_octave"));
    if (!bo.isDouble() || bo.toDouble() != std::floor(bo.toDouble()) || bo.toInt() < -32 || bo.toInt() > 32)
    {
        *err = QStringLiteral("The noise needs a base_octave from -32 to 32.");
        return false;
    }
    int count = 1;
    if (o.contains(QLatin1String("octave_count")))
    {
        QJsonValue c = o.value(QLatin1String("octave_count"));
        if (!c.isDouble() || c.toDouble() != std::floor(c.toDouble()) || c.toInt() < 1 || c.toInt() > 32)
        {
            *err = QStringLiteral("The noise's octave_count must be from 1 to 32.");
            return false;
        }
        count = c.toInt();
    }
    double base = 1.0;
    if (o.contains(QLatin1String("base_amplitude")))
    {
        QJsonValue b = o.value(QLatin1String("base_amplitude"));
        if (!b.isDouble() || !(b.toDouble() >= 9.999999747378752E-6 && b.toDouble() <= 1000000.0))
        {
            *err = QStringLiteral("The noise's base_amplitude must be from 0.00001 to 1000000.");
            return false;
        }
        base = b.toDouble();
    }
    int normalize = 1;
    if (o.contains(QLatin1String("normalize")))
    {
        QJsonValue n = o.value(QLatin1String("normalize"));
        if (n.isBool())
            normalize = n.toBool() ? 1 : 0;
        else if (n.toString() == QLatin1String("legacy"))
            normalize = 2;
        else
        {
            *err = QStringLiteral("The noise's normalize must be true, false or \"legacy\".");
            return false;
        }
    }
    std::vector<double> mods;
    if (o.contains(QLatin1String("amplitude_modifiers")))
    {
        QJsonValue m = o.value(QLatin1String("amplitude_modifiers"));
        if (!m.isArray() || m.toArray().size() > 32)
        {
            *err = QStringLiteral("The noise's amplitude_modifiers must be a list of up to 32 numbers.");
            return false;
        }
        for (const QJsonValue& e : m.toArray())
        {
            if (!e.isDouble() || !(e.toDouble() >= 0.0 && e.toDouble() <= 1000000.0))
            {
                *err = QStringLiteral("The noise's amplitude_modifiers must be from 0 to 1000000.");
                return false;
            }
            mods.push_back(e.toDouble());
        }
        if (!mods.empty() && (int) mods.size() != count)
        {
            *err = QStringLiteral("The noise has %1 amplitude_modifiers for %2 octaves.").arg(mods.size()).arg(count);
            return false;
        }
    }
    if (mods.empty())
        mods.assign(count, 1.0);
    def->v26 = true;
    def->firstOctave = bo.toInt();
    def->amplitudes = mods;
    def->baseAmplitude = base;
    def->normalize = normalize;
    return true;
}

int DensityGraph::compile26(const QString& type, const QJsonObject& o, const Context& ctx)
{
    bool ok = true;
    auto fail = [&](const QString& text) {
        if (ok)
            error(ctx, text);
        ok = false;
    };
    auto has = [&](const char *key) { return o.contains(QLatin1String(key)); };
    // a required float (the game's NOISE_VALUE_CODEC: within +-1000000)
    auto num = [&](const char *key, bool ranged = true) -> double {
        QJsonValue v = o.value(QLatin1String(key));
        if (!v.isDouble() || (ranged && !(std::fabs(v.toDouble()) <= 1000000.0)))
        {
            fail(QStringLiteral("The density function of type %1 needs the number \"%2\"%3.")
                .arg(type, QLatin1String(key), ranged ? QStringLiteral(" (within +-1000000)") : QString()));
            return 0;
        }
        return v.toDouble();
    };
    auto integer = [&](const char *key) -> int {
        QJsonValue v = o.value(QLatin1String(key));
        if (!v.isDouble() || v.toDouble() != std::floor(v.toDouble()) || std::fabs(v.toDouble()) > 2147483647.0)
        {
            fail(QStringLiteral("The density function of type %1 needs the whole number \"%2\".")
                .arg(type, QLatin1String(key)));
            return 0;
        }
        return v.toInt();
    };
    auto arg = [&](const char *key) -> int {
        if (!has(key))
        {
            fail(QStringLiteral("The density function of type %1 needs the argument \"%2\".")
                .arg(type, QLatin1String(key)));
            return -1;
        }
        int n = compileValue(o.value(QLatin1String(key)), ctx);
        if (n < 0)
            ok = false;
        return n;
    };
    auto node = [&](DensityOp op, int a = -1, int b = -1, int c = -1) {
        DensityNode n;
        n.op = op;
        n.a = a;
        n.b = b;
        n.c = c;
        return n;
    };
    auto axisOf = [&](const char *key) -> int {
        QString a = o.value(QLatin1String(key)).toString();
        if (a == QLatin1String("x")) return 0;
        if (a == QLatin1String("y")) return 1;
        if (a == QLatin1String("z")) return 2;
        fail(QStringLiteral("The density function of type %1 needs the axis \"x\", \"y\" or \"z\".").arg(type));
        return 0;
    };

    // generators -----------------------------------------------------------
    if (type == QLatin1String("constant"))
    {
        double v = num("value");
        return ok ? constant(f32(v)) : -1;
    }
    if (type == QLatin1String("blend_alpha"))
        return constant(1.0);
    if (type == QLatin1String("blend_offset") || type == QLatin1String("beardifier"))
        return constant(0.0);
    if (type == QLatin1String("noise"))
    {
        DensityNode n = node(OP_NOISE);
        n.noise = noiseIndex(o.value(QLatin1String("noise")), ctx);
        n.p[0] = num("xz_scale", false);
        n.p[1] = num("y_scale", false);
        int sx = has("shift_x") ? arg("shift_x") : constant(0.0);
        int sy = has("shift_y") ? arg("shift_y") : constant(0.0);
        int sz = has("shift_z") ? arg("shift_z") : constant(0.0);
        if (!ok || n.noise < 0)
            return -1;
        auto zero = [&](int k) { return nodes[k].op == OP_CONSTANT && nodes[k].p[0] == 0.0; };
        if (!(zero(sx) && zero(sy) && zero(sz)))
        {
            n.op = OP_SHIFTED_NOISE;
            n.a = sx;
            n.b = sy;
            n.c = sz;
        }
        return addNode(n);
    }
    if (type == QLatin1String("shift_a") || type == QLatin1String("shift_b") || type == QLatin1String("shift"))
    {
        DensityNode n = node(type == QLatin1String("shift_a") ? OP_SHIFT_A
            : type == QLatin1String("shift_b") ? OP_SHIFT_B : OP_SHIFT);
        n.noise = noiseIndex(o.value(QLatin1String("noise")), ctx);
        if (n.noise < 0)
            return -1;
        return addNode(n);
    }
    if (type == QLatin1String("gradient"))
    {
        const int axis = axisOf("axis");
        int tiling = 0;
        if (has("tiling"))
        {
            QString t = o.value(QLatin1String("tiling")).toString();
            if (t == QLatin1String("clamp_to_edge")) tiling = 0;
            else if (t == QLatin1String("repeat")) tiling = 1;
            else if (t == QLatin1String("mirrored_repeat")) tiling = 2;
            else fail(QStringLiteral("gradient needs the tiling \"clamp_to_edge\", \"repeat\" or \"mirrored_repeat\"."));
        }
        const int from = integer("from_coordinate");
        const int to = integer("to_coordinate");
        const float fv = (float) num("from_value");
        const float tv = (float) num("to_value");
        if (ok && from == to)
            fail(QStringLiteral("gradient: from_coordinate cannot be equal to to_coordinate."));
        if (!ok)
            return -1;
        DensityNode n = node(OP_GRADIENT);
        n.p[0] = from;
        n.p[1] = to;
        n.p[2] = fv;
        // the slope as the game computes it when it compiles
        n.p[3] = (double) ((tv - fv) / (float) (int) ((unsigned) to - (unsigned) from));
        n.p[4] = axis * 4 + tiling;
        return addNode(n);
    }
    if (type == QLatin1String("distance_to_point"))
    {
        QJsonArray pt = o.value(QLatin1String("point")).toArray();
        if (pt.size() != 3 || !pt[0].isDouble() || !pt[1].isDouble() || !pt[2].isDouble())
            fail(QStringLiteral("distance_to_point needs a point [x, y, z]."));
        static const char *const metrics[] = { "euclidean", "euclidean_squared", "manhattan", "chebyshev" };
        int metric = -1;
        for (int i = 0; i < 4; i++)
            if (o.value(QLatin1String("metric")).toString() == QLatin1String(metrics[i]))
                metric = i;
        if (metric < 0)
            fail(QStringLiteral("distance_to_point needs the metric euclidean, euclidean_squared, manhattan or chebyshev."));
        if (!ok)
            return -1;
        DensityNode n = node(OP_DISTANCE);
        for (int i = 0; i < 3; i++)
            n.p[i] = pt[i].toInt();
        n.p[3] = metric;
        return addNode(n);
    }
    if (type == QLatin1String("end_outer_islands"))
        return addNode(node(OP_END_OUTER));
    if (type == QLatin1String("old_blended_noise"))
    {
        double v[5];
        static const char *const keys[5] = { "xz_scale", "y_scale", "xz_factor", "y_factor", "smear_scale_multiplier" };
        for (int i = 0; i < 5; i++)
        {
            v[i] = num(keys[i], false);
            const double lo = i < 4 ? 0.001 : 1.0, hi = i < 4 ? 1000.0 : 8.0;
            if (ok && !(v[i] >= lo && v[i] <= hi))
                fail(QStringLiteral("old_blended_noise: %1 must be from %2 to %3.").arg(QLatin1String(keys[i])).arg(lo).arg(hi));
        }
        if (!ok)
            return -1;
        DensityNode n = node(OP_BLENDED_NOISE_26);
        n.extra = addExtras(v, 5);
        return addNode(n);
    }

    // one input ------------------------------------------------------------
    static const struct { const char *name; DensityOp op; } unary[] = {
        { "abs", OP_ABS }, { "square", OP_SQUARE }, { "cube", OP_CUBE }, { "sqrt", OP_SQRT },
        { "half_negative", OP_HALF_NEGATIVE }, { "quarter_negative", OP_QUARTER_NEGATIVE },
        { "reciprocal", OP_INVERT }, { "negate", OP_NEGATE }, { "squeeze", OP_SQUEEZE },
        { "log", OP_LOG }, { "sign", OP_SIGN },
    };
    for (const auto& u : unary)
    {
        if (type != QLatin1String(u.name))
            continue;
        int a = arg("input");
        return ok ? addNode(node(u.op, a)) : -1;
    }
    static const char *const rounds[] = { "floor", "round", "ceil", "truncate" };
    for (int k = 0; k < 4; k++)
    {
        if (type != QLatin1String(rounds[k]))
            continue;
        int a = arg("input");
        int m = -1;
        // a multiple of the constant 1 rounds to whole numbers
        if (has("multiple") && !(isConstant26(o.value(QLatin1String("multiple"))) &&
            [&]() { int c = compileValue(o.value(QLatin1String("multiple")), ctx);
                    return c >= 0 && nodes[c].p[0] == 1.0; }()))
            m = arg("multiple");
        if (!ok)
            return -1;
        DensityNode n = node(OP_ROUND, a, m);
        n.p[0] = k;
        return addNode(n);
    }
    if (type == QLatin1String("cache") || type == QLatin1String("blend_density"))
        return arg("input");    // memoization; no old chunks to blend with
    if (type == QLatin1String("clamp"))
    {
        int a = arg("input");
        const float lo = (float) num("min");
        const float hi = (float) num("max");
        if (ok && hi < lo)
            fail(QStringLiteral("clamp: min (%1) must be less than or equal to max (%2).").arg(lo).arg(hi));
        if (!ok)
            return -1;
        DensityNode n = node(OP_CLAMP, a);
        n.p[0] = lo;
        n.p[1] = hi;
        return addNode(n);
    }
    if (type == QLatin1String("slice"))
    {
        const int axis = axisOf("axis");
        const int c = integer("coordinate");
        int a = arg("input");
        if (!ok)
            return -1;
        DensityNode n = node(OP_RELOCATE, a);
        n.p[0] = 1 << axis;
        n.p[1] = c;
        return addNode(n);
    }
    if (type == QLatin1String("interpolated"))
    {
        int a = arg("input");
        const int cxz = integer("cell_size_xz");
        const int cy = integer("cell_size_y");
        if (ok && (cxz < 1 || cy < 1))
            fail(QStringLiteral("interpolated needs cell sizes of at least 1."));
        if (!ok)
            return -1;
        if (cxz == 1 && cy == 1)
            return a;
        DensityNode n = node(OP_INTERPOLATE, a);
        n.p[0] = cxz;
        n.p[1] = cy;
        return addNode(n);
    }

    // two and more inputs ----------------------------------------------------
    static const char *const binary[] = { "add", "sub", "mul", "div", "min", "max" };
    for (int k = 0; k < 6; k++)
    {
        if (type != QLatin1String(binary[k]))
            continue;
        int l = arg("left");
        int r = arg("right");
        if (!ok)
            return -1;
        if (k == 3 && isConstant26(o.value(QLatin1String("right"))))
        {
            // division by a constant: multiplication by its float reciprocal
            DensityNode n = node(OP_MUL_CONST, l);
            n.p[0] = (double) (1.0F / (float) nodes[r].p[0]);
            return addNode(n);
        }
        static const DensityOp ops[] = { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MIN, OP_MAX };
        return addNode(node(ops[k], l, r));
    }
    if (type == QLatin1String("pow"))
    {
        int b = arg("base");
        int e = arg("exponent");
        if (!ok)
            return -1;
        if (!isConstant26(o.value(QLatin1String("base"))) && isConstant26(o.value(QLatin1String("exponent"))))
        {
            // PowFunction.compileConstExponent()
            const float x = (float) nodes[e].p[0];
            const float ax = std::fabs(x);
            int r = -1;
            if (ax == 0.5F) r = addNode(node(OP_SQRT, b));
            else if (ax == 1.0F) r = b;
            else if (ax == 2.0F) r = addNode(node(OP_SQUARE, b));
            else if (ax == 3.0F) r = addNode(node(OP_CUBE, b));
            if (r >= 0)
                return x < 0 ? addNode(node(OP_INVERT, r)) : r;
        }
        return addNode(node(OP_POW, b, e));
    }
    if (type == QLatin1String("lerp"))
    {
        int a = arg("alpha");
        int f = arg("first");
        int s = arg("second");
        return ok ? addNode(node(OP_LERP, a, f, s)) : -1;
    }
    if (type == QLatin1String("range_choice"))
    {
        int a = arg("input");
        const float lo = (float) num("min_inclusive");
        const float hi = (float) num("max_exclusive");
        int in = arg("when_in_range");
        int out = arg("when_out_of_range");
        if (!ok)
            return -1;
        DensityNode n = node(OP_RANGE_CHOICE, a, in, out);
        n.p[0] = lo;
        n.p[1] = hi;
        return addNode(n);
    }
    if (type == QLatin1String("interval_select"))
    {
        const int in = arg("input");
        QJsonValue tv = o.value(QLatin1String("thresholds"));
        QJsonValue fv = o.value(QLatin1String("functions"));
        if (!ok)
            return -1;
        QJsonArray ts = tv.toArray(), fs = fv.toArray();
        bool valid = tv.isArray() && fv.isArray() && fs.size() >= 2 && ts.size() == fs.size() - 1;
        for (int i = 0; valid && i < ts.size(); i++)
            valid = ts[i].isDouble() && std::fabs(ts[i].toDouble()) <= 1000000.0 &&
                (i == 0 || (float) ts[i - 1].toDouble() <= (float) ts[i].toDouble());
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
            th.push_back((float) t.toDouble());
        return intervalSelect(in, fn, th, 0, (int) fn.size() - 1);
    }
    if (type == QLatin1String("spline"))
    {
        QJsonValue sv = o.value(QLatin1String("spline"));
        if (sv.isDouble())
            return constant(f32(sv.toDouble()));
        DensityNode n = node(OP_SPLINE);
        n.spline = compileSpline(sv, ctx, &ok);
        return ok ? addNode(n) : -1;
    }
    if (type == QLatin1String("find_top_surface"))
    {
        DensityNode n = node(OP_FIND_TOP_SURFACE);
        n.a = arg("density");
        // the game samples the whole function at y 0
        int ub = arg("upper_bound");
        const int lower = integer("lower_bound");
        const int cell = integer("cell_height");
        if (ok && (lower < -4064 || lower > 4062))
            fail(QStringLiteral("find_top_surface: lower_bound must be from -4064 to 4062."));
        if (ok && cell < 1)
            fail(QStringLiteral("find_top_surface needs a cell_height of at least 1."));
        if (!ok)
            return -1;
        DensityNode at0 = node(OP_RELOCATE, ub);
        at0.p[0] = 2;
        at0.p[1] = 0;
        n.b = addNode(at0);
        n.p[0] = lower;
        n.p[1] = cell;
        return addNode(n);
    }

    error(ctx, QStringLiteral("Unknown density function type \"%1\".")
        .arg(o.value(QLatin1String("type")).toString()));
    return -1;
}

//==============================================================================
// 26.3: noises in float
//==============================================================================

namespace
{

// GradientNoise.GRADIENT
const float g_gradient[16][3] = {
    { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
    { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
    { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 },
    { 1, 1, 0 }, { 0, -1, 1 }, { -1, 1, 0 }, { 0, -1, -1 },
};

inline double wrap26(double x)
{
    // GradientNoise.wrap()
    if (x >= -1.6777216E7 && x < 1.6777216E7)
        return x;
    return x - std::floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}

inline int floorInt(double v)
{
    // Mth.floor(): (int) Math.floor(), saturating like Java's d2i
    double f = std::floor(v);
    if (f != f)
        return 0;
    if (f >= 2147483647.0)
        return INT_MAX;
    if (f <= -2147483648.0)
        return INT_MIN;
    return (int) f;
}

inline float gradDot(int h, float x, float y, float z)
{
    const float *g = g_gradient[h & 15];
    return (g[0] * x + g[1] * y) + g[2] * z;
}

inline float smoothstep(float t)
{
    return ((t * t) * t) * (t * (t * 6.0F - 15.0F) + 10.0F);
}

inline float lerp(float t, float a, float b)
{
    return a + t * (b - a);
}

// PerlinNoise.sampleAndLerp() of 26.3
inline float sampleAndLerp(const PerlinNoise& p, int x, int y, int z, float fx, float fy, float fz, float fySmooth)
{
    const uint8_t *d = p.d;
    auto perm = [d](int i) { return (int) d[i & 255]; };
    const int a = perm(x), b = perm(x + 1);
    const int aa = perm(a + y), ab = perm(a + y + 1), ba = perm(b + y), bb = perm(b + y + 1);
    const float c000 = gradDot(perm(aa + z), fx, fy, fz);
    const float c100 = gradDot(perm(ba + z), fx - 1.0F, fy, fz);
    const float c010 = gradDot(perm(ab + z), fx, fy - 1.0F, fz);
    const float c110 = gradDot(perm(bb + z), fx - 1.0F, fy - 1.0F, fz);
    const float c001 = gradDot(perm(aa + z + 1), fx, fy, fz - 1.0F);
    const float c101 = gradDot(perm(ba + z + 1), fx - 1.0F, fy, fz - 1.0F);
    const float c011 = gradDot(perm(ab + z + 1), fx, fy - 1.0F, fz - 1.0F);
    const float c111 = gradDot(perm(bb + z + 1), fx - 1.0F, fy - 1.0F, fz - 1.0F);
    const float tx = smoothstep(fx), ty = smoothstep(fySmooth), tz = smoothstep(fz);
    const float l0 = lerp(ty, lerp(tx, c000, c100), lerp(tx, c010, c110));
    const float l1 = lerp(ty, lerp(tx, c001, c101), lerp(tx, c011, c111));
    return lerp(tz, l0, l1);
}

// PerlinNoise.get() of 26.3
inline float perlinGet(const PerlinNoise& p, double x, double y, double z)
{
    const double dx = wrap26(x) + p.a, dy = wrap26(y) + p.b, dz = wrap26(z) + p.c;
    const int ix = floorInt(dx), iy = floorInt(dy), iz = floorInt(dz);
    const float fx = (float) (dx - ix), fy = (float) (dy - iy), fz = (float) (dz - iz);
    return sampleAndLerp(p, ix, iy, iz, fx, fy, fz, fy);
}

// SmearedPerlinNoise.get() of 26.3
inline float smearedGet(const PerlinNoise& p, double smear, double x, double y, double z)
{
    const double dx = wrap26(x) + p.a, dy = wrap26(y) + p.b, dz = wrap26(z) + p.c;
    const int ix = floorInt(dx), iy = floorInt(dy), iz = floorInt(dz);
    const float fx = (float) (dx - ix), fz = (float) (dz - iz);
    const double fy = dy - iy;
    const double m = (y >= 0.0 && y < fy) ? y : fy;
    const double fudge = (double) floorInt(m / smear + 1.0000000116860974E-7) * smear;
    return sampleAndLerp(p, ix, iy, iz, fx, (float) (fy - fudge), fz, (float) fy);
}

inline Xoroshiro xoroshiro26(uint64_t lo, uint64_t hi)
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

// DoubleStream.sum(): the compensated sum of the JDK
double javaSum(const std::vector<double>& v)
{
    double s0 = 0, s1 = 0, simple = 0;
    for (double d : v)
    {
        double tmp = d - s1;
        double velvel = s0 + tmp;
        s1 = (velvel - s0) - tmp;
        s0 = velvel;
        simple += d;
    }
    double tmp = s0 - s1;
    if (tmp != tmp && std::isinf(simple))
        return simple;
    return tmp;
}

} // namespace

float NormalNoiseState::sample26(double x, double y, double z) const
{
    // NoiseStack.get(): octave by octave, first then second, in float
    float acc = 0.0F;
    for (const Layer& l : layers)
        acc = acc + l.amplitude * perlinGet(l.perlin, x * l.frequency, y * l.frequency, z * l.frequency);
    return acc;
}

float NormalNoiseState::sample26Volume(int x, int y, int z, double xzScale, double yScale) const
{
    // NoiseStack.addToVolume() -> PerlinNoise.addToVolume(): the layer gets
    // xzScale * frequency (the single position path multiplies the scaled
    // coordinate by the frequency instead; the two differ by an ulp for
    // scales that are not powers of two)
    float acc = 0.0F;
    for (const Layer& l : layers)
    {
        const double fx = xzScale * l.frequency, fy = yScale * l.frequency;
        acc = acc + perlinGet(l.perlin, (double) x * fx, (double) y * fy, (double) z * fx) * l.amplitude;
    }
    return acc;
}

// NormalNoise of 26.3: the octave table, the normalisation and the seeding
static void initNormalNoise26(NormalNoiseState *s, uint64_t factoryLo, uint64_t factoryHi, const NoiseDef& def)
{
    const int n = (int) def.amplitudes.size();
    // buildOctaves()
    double freq = std::pow(2.0, def.firstOctave);
    double amp = def.baseAmplitude;
    if (def.normalize != 0)
        amp = amp * std::pow(0.5, -(n - 1)) / (std::pow(0.5, -n) - 1.0);
    struct Octave { int index; double frequency, amplitude; };
    std::vector<Octave> octaves;
    for (int i = 0; i < n; i++)
    {
        const double m = def.amplitudes[i];
        if (m != 0.0)
            octaves.push_back({ def.firstOctave + i, freq, amp * m });
        freq *= 2.0;
        amp *= 0.5;
    }
    // the normalisation factor
    std::vector<double> abs;
    double acc = 0.0;
    for (const Octave& o : octaves)
    {
        abs.push_back(std::fabs(o.amplitude));
        const double v = 0.2702247831245211 * std::fabs(o.amplitude);
        acc += v * v;
    }
    const double sumAbs = javaSum(abs);
    const double dev = std::sqrt(acc);
    double nf = dev == 0.0 ? 0.0 : (sumAbs * 0.3333333333333333) / (dev * std::sqrt(2.0));
    if (def.normalize == 2 && nf != 0.0)
    {
        // computeParityNormalizationFactor()
        int imin = INT_MAX, imax = INT_MIN;
        for (int i = 0; i < n; i++)
            if (def.amplitudes[i] != 0.0)
            {
                imin = std::min(imin, i);
                imax = std::max(imax, i);
            }
        const double pdev = 0.1 * (1.0 + 1.0 / (double) (int) ((unsigned) imax - (unsigned) imin + 1u));
        nf = def.baseAmplitude * 0.5 * 0.3333333333333333 / pdev;
    }

    // seeding: as before (two positional factories, "octave_<index>")
    Xoroshiro rng = xoroshiro26(factoryLo ^ def.hash[0], factoryHi ^ def.hash[1]);
    uint64_t f[2][2];
    for (int k = 0; k < 2; k++)
    {
        f[k][0] = xNextLong(&rng);
        f[k][1] = xNextLong(&rng);
    }
    s->layers.clear();
    for (const Octave& o : octaves)
    {
        uint64_t h[2];
        hashOf(QByteArray("octave_") + QByteArray::number(o.index), h);
        const float a = (float) (nf * o.amplitude);
        for (int k = 0; k < 2; k++)
        {
            Xoroshiro x = xoroshiro26(f[k][0] ^ h[0], f[k][1] ^ h[1]);
            NormalNoiseState::Layer l;
            xPerlinInit(&l.perlin, &x);
            l.frequency = k == 0 ? o.frequency : o.frequency * 1.0181268882175227;
            l.amplitude = a;
            s->layers.push_back(l);
        }
    }
}

void DensityState::init26(uint64_t lo, uint64_t hi, uint64_t seed)
{
    for (int32_t i : program.usedNoises)
    {
        if (graph.noises[i].v26)
            initNormalNoise26(&noises[i], lo, hi, graph.noises[i]);
    }
    // old_blended_noise: three stacks from the random of "minecraft:terrain"
    blended26.clear();
    for (size_t i = 0; i < graph.nodes.size(); i++)
    {
        const DensityNode& n = graph.nodes[i];
        if (n.op != OP_BLENDED_NOISE_26)
            continue;
        const double *e = &graph.extras[n.extra];
        if (blended26.size() <= (size_t) n.extra)
            blended26.resize(n.extra + 1);
        BlendedNoise26State& b = blended26[n.extra];
        uint64_t h[2];
        hashOf(QByteArray("minecraft:terrain"), h);
        Xoroshiro x = xoroshiro26(lo ^ h[0], hi ^ h[1]);
        b.xzMul = 684.412 * e[0];
        b.yMul = 684.412 * e[1];
        const double smear = b.yMul * e[4];
        auto fbm = [&x](std::vector<BlendedNoise26State::Layer>& out, int first, double smearScale, double factor) {
            const int count = -first + 1;
            double freq = 1.0;
            double amp = factor / (std::pow(2.0, count) - 1.0);
            out.clear();
            for (int i = count - 1; i >= 0; i--)
            {
                BlendedNoise26State::Layer l;
                xPerlinInit(&l.perlin, &x);
                l.smear = smearScale * freq;
                l.frequency = freq;
                l.amplitude = (float) amp;
                out.push_back(l);
                freq /= 2.0;
                amp *= 2.0;
            }
        };
        fbm(b.minLimit, -15, smear, 0.9999847412109375);
        fbm(b.maxLimit, -15, smear, 0.9999847412109375);
        fbm(b.main, -7, smear / e[3], 12.75);
        b.xzMain = b.xzMul / e[2];
        b.yMain = b.yMul / e[3];
    }
    (void) seed;
}

//==============================================================================
// 26.3: evaluation in float
//==============================================================================

namespace
{

inline float javaMinF(float a, float b)
{
    if (a != a)
        return a;
    if (a == 0.0F && b == 0.0F && std::signbit(b))
        return b;
    return a <= b ? a : b;
}

float roundToInteger(float v, int kind)
{
    switch (kind)
    {
    case 0: return (float) std::floor((double) v);
    case 1:
    {
        // Math.round(float): half up, NaN 0, saturating
        if (v != v)
            return 0.0F;
        double r = std::floor((double) v + 0.5);
        if (r >= 2147483647.0) return 2147483647.0F;
        if (r <= -2147483648.0) return -2147483648.0F;
        return (float) (int) r;
    }
    case 2: return (float) std::ceil((double) v);
    default: return v > 0.0F ? (float) std::floor((double) v) : (float) std::ceil((double) v);
    }
}

// EndIslandFunction.getHeightValue() of 26.3, without the central island
float endOuterHeight(const EndNoise& en, int x, int z)
{
    const int i = x / 2, j = z / 2, k = x % 2, l = z % 2;
    float f = -100.0F;
    for (int m = -12; m <= 12; m++)
    {
        for (int n = -12; n <= 12; n++)
        {
            const int64_t o = (int64_t) (i + m), p = (int64_t) (j + n);
            if (o * o + p * p > 4096 && (float) sampleSimplex2D(&en.perlin, (double) o, (double) p) < -0.9F)
            {
                const float g = std::fmod(std::fabs((float) o) * 3439.0F + std::fabs((float) p) * 147.0F, 13.0F) + 9.0F;
                const float h = (float) (k - m * 2), q = (float) (l - n * 2);
                float r = 100.0F - (float) std::sqrt((double) (h * h + q * q)) * g;
                r = r < -100.0F ? -100.0F : javaMinF(r, 80.0F);
                f = std::max(f, r);
            }
        }
    }
    return f;
}

} // namespace

float DensityState::evalAt(const DensityProgram::Scan& s, int32_t node, int x, int y, int z)
{
    const int ox = cx, oz = cz;
    cx = x;
    cz = z;
    run26(s.nodes.data(), s.nodes.size(), y);
    const float v = (float) reg[node];
    cx = ox;
    cz = oz;
    return v;
}

void DensityState::run26(const int32_t *list, size_t count, int y)
{
    const DensityNode *nodes = graph.nodes.data();
    double *r = reg.data();
    const int x = cx, z = cz;
    for (size_t li = 0; li < count; li++)
    {
        const int32_t idx = list[li];
        const DensityNode& n = nodes[idx];
        const float a = n.a >= 0 ? (float) r[n.a] : 0.0F;
        const float b = n.b >= 0 ? (float) r[n.b] : 0.0F;
        float v;
        switch ((int) n.op)
        {
        case OP_GRADIENT:
        {
            const int axis = (int) n.p[4] / 4, tiling = (int) n.p[4] % 4;
            const int c = axis == 0 ? x : axis == 1 ? y : z;
            const int from = (int) n.p[0], to = (int) n.p[1];
            const int range = (int) ((unsigned) to - (unsigned) from);
            const float fv = (float) n.p[2], slope = (float) n.p[3];
            if (tiling == 0)
            {
                const int cc = std::min(std::max(c, std::min(from, to)), std::max(from, to));
                v = fv + (float) (int) ((unsigned) cc - (unsigned) from) * slope;
            }
            else if (tiling == 1)
            {
                v = fv + (float) floorMod((int) ((unsigned) c - (unsigned) from), range) * slope;
            }
            else
            {
                const int d = (int) ((unsigned) c - (unsigned) from);
                const int q = floorDiv(d, range);
                const int rr = (int) ((unsigned) d - (unsigned) q * (unsigned) range);
                v = (q & 1) == 0 ? fv + (float) rr * slope : fv + (float) (int) ((unsigned) range - (unsigned) rr) * slope;
            }
            break;
        }
        case OP_NOISE:
            // chunks sample unshifted noises as a volume (NoiseStack.addToVolume)
            v = noises[n.noise].sample26Volume(x, y, z, n.p[0], n.p[1]);
            break;
        case OP_SHIFTED_NOISE:
            v = noises[n.noise].sample26((double) x * n.p[0] + r[n.a], (double) y * n.p[1] + r[n.b],
                (double) z * n.p[0] + r[n.c]);
            break;
        case OP_SHIFT_A:
            v = noises[n.noise].sample26Volume(x, 0, z, 0.25, 0.0) * 4.0F;
            break;
        case OP_SHIFT_B:
            v = noises[n.noise].sample26((double) z * 0.25, (double) x * 0.25, 0.0) * 4.0F;
            break;
        case OP_SHIFT:
            v = noises[n.noise].sample26Volume(x, y, z, 0.25, 0.25) * 4.0F;
            break;
        case OP_ADD: v = a + b; break;
        case OP_ADD_CONST: v = a + (float) n.p[0]; break;
        case OP_SUB: v = a - b; break;
        case OP_MUL: v = a * b; break;
        case OP_MUL_CONST: v = a * (float) n.p[0]; break;
        case OP_DIV: v = a / b; break;
        case OP_MIN: v = b < a ? b : a; break;
        case OP_MAX: v = b > a ? b : a; break;
        case OP_ABS: v = std::fabs(a); break;
        case OP_SQUARE: v = a * a; break;
        case OP_CUBE: v = (a * a) * a; break;
        case OP_SQRT: v = (float) std::sqrt((double) a); break;
        case OP_HALF_NEGATIVE: v = a > 0.0F ? a : a * 0.5F; break;
        case OP_QUARTER_NEGATIVE: v = a > 0.0F ? a : a * 0.25F; break;
        case OP_INVERT: v = 1.0F / a; break;
        case OP_NEGATE: v = -a; break;
        case OP_SQUEEZE:
        {
            const float e = a < -1.0F ? -1.0F : javaMinF(a, 1.0F);
            v = e / 2.0F - ((e * e) * e) / 24.0F;
            break;
        }
        case OP_LOG: v = (float) std::log((double) a); break;
        case OP_SIGN: v = a > 0.0F ? 1.0F : a < 0.0F ? -1.0F : a; break;
        case OP_ROUND:
        {
            const int kind = (int) n.p[0];
            if (n.b < 0)
                v = roundToInteger(a, kind);
            else
                v = b == 0.0F ? a : roundToInteger(a / b, kind) * b;
            break;
        }
        case OP_POW: v = (float) std::pow((double) a, (double) b); break;
        case OP_LERP:
            v = a == 0.0F ? b : a == 1.0F ? (float) r[n.c] : b + a * ((float) r[n.c] - b);
            break;
        case OP_CLAMP:
            v = a < (float) n.p[0] ? (float) n.p[0] : javaMinF(a, (float) n.p[1]);
            break;
        case OP_RANGE_CHOICE:
            v = (a >= n.p[0] && a < n.p[1]) ? b : (float) r[n.c];
            break;
        case OP_SPLINE:
            v = spline(n.spline, r);
            break;
        case OP_DISTANCE:
        {
            const float dx = (float) (int) ((unsigned) (int) n.p[0] - (unsigned) x);
            const float dy = (float) (int) ((unsigned) (int) n.p[1] - (unsigned) y);
            const float dz = (float) (int) ((unsigned) (int) n.p[2] - (unsigned) z);
            switch ((int) n.p[3])
            {
            case 0: v = (float) std::sqrt((double) ((dx * dx + dy * dy) + dz * dz)); break;
            case 1: v = (dx * dx + dy * dy) + dz * dz; break;
            case 2: v = (std::fabs(dx) + std::fabs(dy)) + std::fabs(dz); break;
            default: v = std::max(std::max(std::fabs(dx), std::fabs(dy)), std::fabs(dz)); break;
            }
            break;
        }
        case OP_RELOCATE:
        case OP_INTERPOLATE:
        {
            const DensityProgram::Scan *s = nullptr;
            for (const DensityProgram::Scan& sc : program.relocs)
                if (sc.node == idx)
                    s = &sc;
            if (!s)
            {
                v = a;
                break;
            }
            if (n.op == OP_RELOCATE)
            {
                const int mask = (int) n.p[0], c = (int) n.p[1];
                v = evalAt(*s, n.a, mask & 1 ? c : x, mask & 2 ? c : y, mask & 4 ? c : z);
            }
            else
            {
                // InterpolatedFunction: trilinear between the corners of
                // the cell on an absolute grid
                const int cxz = (int) n.p[0], cy = (int) n.p[1];
                const int fx = floorMod(x, cxz), fy = floorMod(y, cy), fz = floorMod(z, cxz);
                if (fx == 0 && fy == 0 && fz == 0)
                {
                    v = a;
                    break;
                }
                const int x0 = x - fx, y0 = y - fy, z0 = z - fz;
                float c[8];
                for (int k = 0; k < 8; k++)
                    c[k] = evalAt(*s, n.a, x0 + (k & 1) * cxz, y0 + ((k >> 1) & 1) * cy, z0 + ((k >> 2) & 1) * cxz);
                const float tx = (float) fx / (float) cxz, ty = (float) fy / (float) cy, tz = (float) fz / (float) cxz;
                const float l0 = lerp(ty, lerp(tx, c[0], c[1]), lerp(tx, c[2], c[3]));
                const float l1 = lerp(ty, lerp(tx, c[4], c[5]), lerp(tx, c[6], c[7]));
                v = lerp(tz, l0, l1);
            }
            // nodes shared with the rest of the program: back to this position
            run26(s->nodes.data(), s->nodes.size(), y);
            break;
        }
        case OP_INTERPOLATED:
            // (not in 26.3 graphs: compile26() makes OP_INTERPOLATE)
            v = a;
            break;
        case OP_END_OUTER:
            v = (endOuterHeight(endNoise, x / 8, z / 8) - 8.0F) / 128.0F;
            break;
        case OP_FIND_TOP_SURFACE:
        {
            const DensityProgram::Scan *scan = nullptr;
            for (const DensityProgram::Scan& s : program.scans)
                if (s.node == idx)
                    scan = &s;
            const int lower = (int) n.p[0], cell = (int) n.p[1];
            const int start = (int) ((uint32_t) floorInt((double) (b / (float) cell)) * (uint32_t) cell);
            v = (float) lower;
            if (scan && start > lower)
            {
                unsigned steps = 0;
                for (int sy = start; sy >= lower; sy = (int) ((uint32_t) sy - (uint32_t) cell))
                {
                    if ((++steps & 63) == 0 && abort && abort->load(std::memory_order_relaxed))
                        break;
                    run26(scan->nodes.data(), scan->nodes.size(), sy);
                    if ((float) r[n.a] > 0.0F)
                    {
                        v = (float) sy;
                        break;
                    }
                }
                if (n.flags & DF_USES_Y)
                    run26(scan->nodes.data(), scan->nodes.size(), y);
            }
            break;
        }
        case OP_BLENDED_NOISE_26:
        {
            const BlendedNoise26State& bn = blended26[n.extra];
            auto stack = [](const std::vector<BlendedNoise26State::Layer>& ls, double X, double Y, double Z) {
                float acc = 0.0F;
                for (const BlendedNoise26State::Layer& l : ls)
                    acc = acc + l.amplitude * smearedGet(l.perlin, l.smear, X * l.frequency, Y * l.frequency, Z * l.frequency);
                return acc;
            };
            const float m = stack(bn.main, (double) x * bn.xzMain, (double) y * bn.yMain, (double) z * bn.xzMain) + 0.5F;
            const float alpha = m < 0.0F ? 0.0F : javaMinF(m, 1.0F);
            const double X = (double) x * bn.xzMul, Y = (double) y * bn.yMul, Z = (double) z * bn.xzMul;
            if (alpha == 0.0F)
                v = stack(bn.minLimit, X, Y, Z);
            else if (alpha == 1.0F)
                v = stack(bn.maxLimit, X, Y, Z);
            else
            {
                const float lo = stack(bn.minLimit, X, Y, Z);
                v = lo + alpha * (stack(bn.maxLimit, X, Y, Z) - lo);
            }
            break;
        }
        default:
            // every op of a 26.3 graph needs a case here
            v = 0.0F;
            break;
        }
        r[idx] = (double) v;
    }
}

} // namespace dp
