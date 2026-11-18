#include "dpsurface.h"
#include "dpeval.h"
#include "dpinternal.h"
#include "dpworld.h"

extern "C"
{
#include "rng.h"
}

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <functional>

namespace dp
{

namespace
{

// Blocks that do not block motion (BlockStateBase.blocksMotion() false): the
// game's properties noCollision, forceSolidOff, dynamicShape, and the blocks
// whose collision shape is too thin (by name below). Fluids are soft too.
const char *const SOFT_BLOCKS[] = {
    "water", "lava", "bubble_column", "powder_snow", "snow", "scaffolding", "cobweb", "bamboo_sapling",
    "short_grass", "grass", "tall_grass", "fern", "large_fern", "short_dry_grass", "tall_dry_grass", "bush",
    "dead_bush", "firefly_bush", "seagrass", "tall_seagrass", "kelp", "kelp_plant", "vine", "glow_lichen",
    "hanging_roots", "pale_hanging_moss", "leaf_litter", "pink_petals", "wildflowers", "resin_clump",
    "dandelion", "golden_dandelion", "poppy", "blue_orchid", "allium", "azure_bluet", "oxeye_daisy",
    "cornflower", "lily_of_the_valley", "wither_rose", "torchflower", "sunflower", "lilac", "rose_bush",
    "peony", "pitcher_plant", "open_eyeblossom", "closed_eyeblossom", "cactus_flower", "spore_blossom",
    "brown_mushroom", "red_mushroom", "crimson_fungus", "warped_fungus", "nether_sprouts", "sweet_berry_bush",
    "sugar_cane", "wheat", "carrots", "potatoes", "beetroots", "nether_wart", "torchflower_crop",
    "pitcher_crop", "melon_stem", "pumpkin_stem", "attached_melon_stem", "attached_pumpkin_stem",
    "cave_vines", "cave_vines_plant", "big_dripleaf_stem", "small_dripleaf", "chorus_plant", "chorus_flower",
    "frogspawn", "lily_pad", "sea_pickle", "flower_pot", "ladder", "lever", "tripwire", "tripwire_hook",
    "redstone_wire", "fire", "soul_fire", "end_portal", "end_gateway", "nether_portal", "structure_void",
    "end_rod", "light", "big_dripleaf", "azalea", "flowering_azalea", "mangrove_propagule",
    "string", "candle", "turtle_egg", "sniffer_egg",
};
const char *const SOFT_SUFFIXES[] = {
    "_sapling", "_tulip", "_coral", "_coral_fan", "_coral_wall_fan", "_button", "_pressure_plate", "_sign",
    "_banner", "_torch", "_rail", "_roots", "_vines", "_vines_plant", "_carpet", "_candle",
};

const QHash<QString, char>& softBlocks()
{
    static const QHash<QString, char> set = [] {
        QHash<QString, char> s;
        for (const char *b : SOFT_BLOCKS)
            s.insert(QString::fromLatin1(b), 1);
        return s;
    }();
    return set;
}

QString typeOf(const QJsonObject& o)
{
    QString t = o.value(QLatin1String("type")).toString();
    if (!t.contains(QLatin1Char(':')))
        t.prepend(QLatin1String("minecraft:"));
    return t;
}

QString idOf(const QString& s)
{
    return s.contains(QLatin1Char(':')) ? s : QLatin1String("minecraft:") + s;
}

const int NOISE_SURFACE = 0, NOISE_SECONDARY = 1;

// the costs of the conditions for the cheap test (lowest first)
int costOf(const SurfaceRuleDef::Cond& c)
{
    switch (c.kind)
    {
    case SurfaceRuleDef::C_Y_ABOVE:
    case SurfaceRuleDef::C_WATER:
        return c.mult != 0 ? 2 : 0;
    case SurfaceRuleDef::C_STONE_DEPTH:
        return c.ceiling ? 5 : (c.addSurface || c.secondary != 0) ? 2 : 0;
    case SurfaceRuleDef::C_GRADIENT:
        return 1;
    case SurfaceRuleDef::C_HOLE:
        return 2;
    case SurfaceRuleDef::C_NOISE:
        return 3;
    case SurfaceRuleDef::C_ABOVE_PRELIMINARY:
        return 4;
    case SurfaceRuleDef::C_BIOME:
        return 6;
    case SurfaceRuleDef::C_TEMPERATURE:
        return 7;
    case SurfaceRuleDef::C_STEEP:
        return 8;
    case SurfaceRuleDef::C_NOT:
        return 0;   // (set from its argument)
    }
    return 9;
}

struct Builder
{
    const SurfaceBuildContext& ctx;
    SurfaceRuleDef *def;
    QString error;
    QHash<QString, int> noiseSlots;
    QHash<QString, int> randomSlots;
    int depth = 0;

    bool anchor(const QJsonValue& v, int *y)
    {
        const QJsonObject o = v.toObject();
        if (o.contains(QLatin1String("absolute")))
            *y = o.value(QLatin1String("absolute")).toInt();
        else if (o.contains(QLatin1String("above_bottom")))
            *y = ctx.minGenY + o.value(QLatin1String("above_bottom")).toInt();
        else if (o.contains(QLatin1String("below_top")))
            *y = ctx.genDepth - 1 + ctx.minGenY - o.value(QLatin1String("below_top")).toInt();
        else
        {
            error = QStringLiteral("A surface rule has a vertical anchor of an unknown kind.");
            return false;
        }
        return true;
    }

    int noiseSlot(const QString& id)
    {
        auto it = noiseSlots.find(id);
        if (it != noiseSlots.end())
            return it.value();
        const int slot = (int) noiseSlots.size();
        noiseSlots.insert(id, slot);
        return slot;
    }

    int cond(const QJsonValue& v)
    {
        if (++depth > 256)
        {
            error = QStringLiteral("The surface rule nests too deep.");
            return -1;
        }
        struct Guard { int& d; ~Guard() { d--; } } guard { depth };
        const QJsonObject o = v.toObject();
        const QString type = typeOf(o);
        SurfaceRuleDef::Cond c;
        if (type == QLatin1String("minecraft:biome"))
        {
            c.kind = SurfaceRuleDef::C_BIOME;
            // a list of ids (1.18.2 to 26.1), a holder set from 26.2 on: an
            // id, a "#tag" or a list
            std::vector<char> set((size_t) ctx.biomeCount, 0);
            const QJsonValue is = o.value(QLatin1String("biome_is"));
            QStringList ids;
            const QJsonArray list = is.isArray() ? is.toArray() : QJsonArray { is };
            for (const QJsonValue& b : list)
            {
                const QString s = b.toString();
                if (s.startsWith(QLatin1Char('#')))
                    ids += ctx.biomeTag(s.mid(1));
                else
                    ids.push_back(s);
            }
            for (const QString& id : ids)
            {
                auto it = ctx.biomeIndex->find(idOf(id));
                if (it != ctx.biomeIndex->end() && it.value() >= 0 && it.value() < ctx.biomeCount)
                    set[(size_t) it.value()] = 1;
            }
            c.biomes = (int) def->biomeSets.size();
            def->biomeSets.push_back(std::move(set));
            def->usesBiome = true;
        }
        else if (type == QLatin1String("minecraft:noise_threshold"))
        {
            c.kind = SurfaceRuleDef::C_NOISE;
            const QString nid = idOf(o.value(QLatin1String("noise")).toString());
            noiseSlot(QStringLiteral("minecraft:surface"));
            noiseSlot(QStringLiteral("minecraft:surface_secondary"));
            c.arg = noiseSlot(nid);
            c.lo = o.value(QLatin1String("min_threshold")).toDouble();
            c.hi = o.value(QLatin1String("max_threshold")).toDouble();
        }
        else if (type == QLatin1String("minecraft:vertical_gradient"))
        {
            c.kind = SurfaceRuleDef::C_GRADIENT;
            const QString name = idOf(o.value(QLatin1String("random_name")).toString());
            auto it = randomSlots.find(name);
            if (it == randomSlots.end())
            {
                it = randomSlots.insert(name, (int) def->randomNames.size());
                def->randomNames.push_back(name);
            }
            c.arg = it.value();
            if (!anchor(o.value(QLatin1String("true_at_and_below")), &c.anchorY) ||
                !anchor(o.value(QLatin1String("false_at_and_above")), &c.falseY))
                return -1;
        }
        else if (type == QLatin1String("minecraft:y_above"))
        {
            c.kind = SurfaceRuleDef::C_Y_ABOVE;
            if (!anchor(o.value(QLatin1String("anchor")), &c.anchorY))
                return -1;
            c.mult = o.value(QLatin1String("surface_depth_multiplier")).toInt();
            c.addStone = o.value(QLatin1String("add_stone_depth")).toBool();
        }
        else if (type == QLatin1String("minecraft:water"))
        {
            c.kind = SurfaceRuleDef::C_WATER;
            c.offset = o.value(QLatin1String("offset")).toInt();
            c.mult = o.value(QLatin1String("surface_depth_multiplier")).toInt();
            c.addStone = o.value(QLatin1String("add_stone_depth")).toBool();
        }
        else if (type == QLatin1String("minecraft:temperature"))
        {
            c.kind = SurfaceRuleDef::C_TEMPERATURE;
            def->usesTemperature = def->usesBiome = true;
        }
        else if (type == QLatin1String("minecraft:steep"))
        {
            c.kind = SurfaceRuleDef::C_STEEP;
            def->usesSteep = true;
        }
        else if (type == QLatin1String("minecraft:not"))
        {
            c.kind = SurfaceRuleDef::C_NOT;
            c.arg = cond(o.value(QLatin1String("invert")));
            if (c.arg < 0)
                return -1;
        }
        else if (type == QLatin1String("minecraft:hole"))
            c.kind = SurfaceRuleDef::C_HOLE;
        else if (type == QLatin1String("minecraft:above_preliminary_surface"))
        {
            c.kind = SurfaceRuleDef::C_ABOVE_PRELIMINARY;
            def->usesPreliminary = true;
        }
        else if (type == QLatin1String("minecraft:stone_depth"))
        {
            c.kind = SurfaceRuleDef::C_STONE_DEPTH;
            c.offset = o.value(QLatin1String("offset")).toInt();
            c.addSurface = o.value(QLatin1String("add_surface_depth")).toBool();
            c.secondary = o.value(QLatin1String("secondary_depth_range")).toInt();
            c.ceiling = o.value(QLatin1String("surface_type")).toString() == QLatin1String("ceiling");
        }
        else
        {
            error = QStringLiteral("The surface rule has a condition of the type %1, which is not supported.")
                .arg(type);
            return -1;
        }
        c.cost = c.kind == SurfaceRuleDef::C_NOT ? def->conds[(size_t) c.arg].cost : costOf(c);
        def->conds.push_back(c);
        return (int) def->conds.size() - 1;
    }

    int rule(const QJsonValue& v)
    {
        if (++depth > 256)
        {
            error = QStringLiteral("The surface rule nests too deep.");
            return -1;
        }
        struct Guard { int& d; ~Guard() { d--; } } guard { depth };
        const QJsonObject o = v.toObject();
        const QString type = typeOf(o);
        SurfaceRuleDef::Rule r;
        if (type == QLatin1String("minecraft:block"))
        {
            r.kind = SurfaceRuleDef::R_BLOCK;
            const QJsonObject state = o.value(QLatin1String("result_state")).toObject();
            r.block = surfaceClassOf(state);
            const QString name = idOf(state.value(QLatin1String("Name")).toString());
            r.fluid = name == QLatin1String("minecraft:lava") ? 2
                : name == QLatin1String("minecraft:water") || name == QLatin1String("minecraft:bubble_column") ? 1 : 0;
        }
        else if (type == QLatin1String("minecraft:bandlands"))
            r.kind = SurfaceRuleDef::R_BANDLANDS;
        else if (type == QLatin1String("minecraft:sequence"))
        {
            r.kind = SurfaceRuleDef::R_SEQUENCE;
            std::vector<int32_t> kids;
            for (const QJsonValue& k : o.value(QLatin1String("sequence")).toArray())
            {
                const int c = rule(k);
                if (c < 0)
                    return -1;
                kids.push_back(c);
            }
            r.first = (int) def->children.size();
            r.count = (int) kids.size();
            def->children.insert(def->children.end(), kids.begin(), kids.end());
        }
        else if (type == QLatin1String("minecraft:condition"))
        {
            r.kind = SurfaceRuleDef::R_CONDITION;
            r.cond = cond(o.value(QLatin1String("if_true")));
            if (r.cond < 0)
                return -1;
            r.then = rule(o.value(QLatin1String("then_run")));
            if (r.then < 0)
                return -1;
        }
        else
        {
            error = QStringLiteral("The surface rule has a rule of the type %1, which is not supported.").arg(type);
            return -1;
        }
        def->rules.push_back(r);
        return (int) def->rules.size() - 1;
    }

    // the blocks that are not solid and the conditions on their way
    void leaves(int r, std::vector<int32_t>& path)
    {
        const SurfaceRuleDef::Rule& rr = def->rules[(size_t) r];
        switch (rr.kind)
        {
        case SurfaceRuleDef::R_BLOCK:
            if (rr.block != SC_SOLID)
            {
                SurfaceRuleDef::Leaf l { r, path };
                std::stable_sort(l.path.begin(), l.path.end(), [this](int32_t a, int32_t b) {
                    return def->conds[(size_t) a].cost < def->conds[(size_t) b].cost;
                });
                def->softLeaves.push_back(std::move(l));
            }
            break;
        case SurfaceRuleDef::R_BANDLANDS:
            break;
        case SurfaceRuleDef::R_SEQUENCE:
            for (int i = 0; i < rr.count; i++)
                leaves(def->children[(size_t) (rr.first + i)], path);
            break;
        case SurfaceRuleDef::R_CONDITION:
            path.push_back(rr.cond);
            leaves(rr.then, path);
            path.pop_back();
            break;
        }
    }
};

} // namespace

SurfaceClass surfaceClassOf(const QJsonObject& state)
{
    QString name = state.value(QLatin1String("Name")).toString();
    if (!name.contains(QLatin1Char(':')))
        name.prepend(QLatin1String("minecraft:"));
    if (!name.startsWith(QLatin1String("minecraft:")))
        return SC_SOLID;    // (a block of a mod: unknown)
    const QString b = name.mid(10);
    if (b == QLatin1String("air") || b == QLatin1String("cave_air") || b == QLatin1String("void_air"))
        return SC_AIR;
    if (softBlocks().contains(b))
        return SC_SOFT;
    for (const char *s : SOFT_SUFFIXES)
        if (b.endsWith(QLatin1String(s)))
            return SC_SOFT;
    if (b.startsWith(QLatin1String("potted_")))
        return SC_SOFT;
    return SC_SOLID;
}

void buildSurfaceRules(const QJsonObject& settings, const SurfaceBuildContext& ctx, SurfaceRuleDef *def)
{
    *def = SurfaceRuleDef();
    def->seaLevel = ctx.seaLevel;
    def->snowLevel = ctx.mc >= MC_1_21_2 ? ctx.seaLevel + 17 : 80;
    def->minY = ctx.minY;
    def->legacy = ctx.legacy;
    if (!settings.contains(QLatin1String("surface_rule")))
        return;
    Builder b { ctx, def, QString(), {}, {}, 0 };
    def->root = b.rule(settings.value(QLatin1String("surface_rule")));
    if (def->root < 0)
    {
        def->reason = b.error;
        return;
    }
    std::vector<int32_t> path;
    b.leaves(def->root, path);
    def->noEffect = def->softLeaves.empty();
    if (def->noEffect)
        return;
    if (def->usesPreliminary && !ctx.hasPreliminary)
    {
        def->reason = QStringLiteral("The surface rule asks for the preliminary surface, which is only "
            "modelled with the aquifers.");
        return;
    }
    if (def->usesTemperature)
    {
        def->temperature.assign((size_t) ctx.biomeCount, 0.0F);
        def->frozen.assign((size_t) ctx.biomeCount, 0);
        for (int i = 0; i < ctx.biomeCount; i++)
        {
            float t = 0.0F;
            bool f = false;
            if (!ctx.climate(i, &t, &f))
            {
                def->reason = QStringLiteral("The surface rule asks for biome temperatures, which are not "
                    "known for every biome.");
                return;
            }
            def->temperature[(size_t) i] = t;
            def->frozen[(size_t) i] = f;
        }
    }
    // the noises: surface and surface_secondary first
    b.noiseSlot(QStringLiteral("minecraft:surface"));
    b.noiseSlot(QStringLiteral("minecraft:surface_secondary"));
    std::vector<QString> ids(b.noiseSlots.size());
    for (auto it = b.noiseSlots.begin(); it != b.noiseSlots.end(); ++it)
        ids[(size_t) it.value()] = it.key();
    for (const QString& nid : ids)
    {
        QJsonObject f;
        f.insert(QLatin1String("type"), QLatin1String("minecraft:noise"));
        f.insert(QLatin1String("noise"), nid);
        f.insert(QLatin1String("xz_scale"), 1.0);
        f.insert(QLatin1String("y_scale"), 0.0);
        const int node = ctx.graph->compile(f, ctx.pack, ctx.id, ctx.legacySettings);
        if (node < 0 || ctx.graph->nodes[(size_t) node].op != OP_NOISE)
        {
            def->reason = QStringLiteral("The surface rule's noise %1 cannot be sampled.").arg(nid);
            return;
        }
        def->noiseNodes.push_back(node);
    }
    def->program.build(*ctx.graph, def->noiseNodes);
    def->on = true;
}

//==============================================================================
// state
//==============================================================================

namespace
{

// Mth.getSeed(x, y, z)
inline int64_t positionSeed(int x, int y, int z)
{
    const int64_t a = (int64_t) (int32_t) ((uint32_t) x * 3129871u);
    uint64_t l = (uint64_t) (a ^ (int64_t) ((uint64_t) (int64_t) z * 116129781ULL) ^ (int64_t) y);
    l = l * l * 42317861ULL + l * 11ULL;
    return (int64_t) l >> 16;
}

inline Xoroshiro xoroshiroOf(uint64_t lo, uint64_t hi)
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

// the game's PerlinSimplexNoise of the octaves 0 (one level) at (x, y)
inline double simplex(const PerlinNoise& n, double x, double y)
{
    return sampleSimplex2D(&n, x, y);
}

} // namespace

SurfaceRuleState::SurfaceRuleState(const DensityGraph& graph, const SurfaceRuleDef& def, uint64_t seed, int mc)
    : def(def)
{
    if (!def.on)
        return;
    noise.reset(new DensityState(graph, def.program, seed, mc, def.legacy));
    noiseValue.assign(def.noiseNodes.size(), 0.0);
    haveNoise.assign(def.noiseNodes.size(), 0);
    memo.assign(def.conds.size(), -1);
    // RandomState.random (the world's positional random), and per
    // vertical_gradient random_name: random.fromHashOf(name).forkPositional()
    if (def.legacy)
    {
        uint64_t r;
        setSeed(&r, seed);
        posLo = nextLong(&r);
        for (const QString& name : def.randomNames)
        {
            setSeed(&r, (uint64_t) (int64_t) javaStringHash(name) ^ posLo);
            gradLo.push_back(nextLong(&r));
            gradHi.push_back(0);
        }
    }
    else
    {
        Xoroshiro w;
        xSetSeed(&w, seed);
        posLo = xNextLong(&w);
        posHi = xNextLong(&w);
        for (const QString& name : def.randomNames)
        {
            uint64_t h[2];
            hashOf(name.toUtf8(), h);
            Xoroshiro f = xoroshiroOf(posLo ^ h[0], posHi ^ h[1]);
            gradLo.push_back(xNextLong(&f));
            gradHi.push_back(xNextLong(&f));
        }
    }
    if (def.usesTemperature)
    {
        // Biome.TEMPERATURE_NOISE (1234, octave 0), FROZEN_TEMPERATURE_NOISE
        // (3456, octaves -2 .. 0), BIOME_INFO_NOISE (2345, octave 0): Java
        // randoms, one SimplexNoise per octave in the order of the levels
        uint64_t s;
        setSeed(&s, 1234);
        perlinInit(&tempNoise, &s);
        setSeed(&s, 3456);
        for (PerlinNoise& p : frozenNoise)
            perlinInit(&p, &s);
        setSeed(&s, 2345);
        perlinInit(&infoNoise, &s);
    }
}

SurfaceRuleState::~SurfaceRuleState()
{
}

int SurfaceRuleState::depth()
{
    if (!haveDepth)
    {
        // SurfaceSystem.getSurfaceDepth()
        const double n = noiseAt(NOISE_SURFACE);
        double r;
        if (def.legacy)
        {
            uint64_t s;
            setSeed(&s, (uint64_t) positionSeed(cx, 0, cz) ^ posLo);
            r = nextDouble(&s);
        }
        else
        {
            Xoroshiro x = xoroshiroOf((uint64_t) positionSeed(cx, 0, cz) ^ posLo, posHi);
            r = xNextDouble(&x);
        }
        surfaceDepth = (int) (n * 2.75 + 3.0 + r * 0.25);
        haveDepth = true;
    }
    return surfaceDepth;
}

double SurfaceRuleState::noiseAt(int slot)
{
    if (!haveNoise[(size_t) slot])
    {
        noiseValue[(size_t) slot] = noise->compute(def.noiseNodes[(size_t) slot], nullptr, cx, 0, cz);
        haveNoise[(size_t) slot] = 1;
    }
    return noiseValue[(size_t) slot];
}

double SurfaceRuleState::secondaryValue()
{
    return noiseAt(NOISE_SECONDARY);
}

int SurfaceRuleState::minSurfaceLevel()
{
    if (!haveMinSurface)
    {
        // Context.getMinSurfaceLevel(): the preliminary surface at the
        // corners of the 16 x 16 cell, interpolated
        const int sx = cx >> 4, sz = cz >> 4;
        if (cornerKey[0] != sx || cornerKey[1] != sz)
        {
            cornerKey[0] = sx;
            cornerKey[1] = sz;
            corner[0] = acc->preliminary(sx << 4, sz << 4);
            corner[1] = acc->preliminary((sx + 1) << 4, sz << 4);
            corner[2] = acc->preliminary(sx << 4, (sz + 1) << 4);
            corner[3] = acc->preliminary((sx + 1) << 4, (sz + 1) << 4);
            if (acc->aborted())
                cornerKey[0] = INT_MIN;     // (not kept)
        }
        const double fx = (double) ((float) (cx & 15) / 16.0F), fz = (double) ((float) (cz & 15) / 16.0F);
        const double a = corner[0] + fx * (corner[1] - corner[0]);
        const double b = corner[2] + fx * (corner[3] - corner[2]);
        const double v = a + fz * (b - a);
        minSurface = floorOld(v) + depth() - 8;
        haveMinSurface = true;
    }
    return minSurface;
}

int SurfaceRuleState::biome()
{
    if (biomeAt == -2)
        biomeAt = acc->biome(cx, by, cz);
    return biomeAt;
}

// Is the solid run below the block (the game's stone depth below, counted
// from the block) at most @limit blocks? Returns the depth if it is, else
// limit + 1.
int SurfaceRuleState::stoneBelowAtMost(int limit)
{
    // the run holds the block; its bottom is the lowest solid block above
    // the first block that is not solid (below the noise range: not solid)
    if (runBottom != INT_MIN && by >= runBottom && by <= runTop)
        return std::min(by - runBottom + 1, limit + 1);
    if (runTop != by || runKnownTo > by)
    {
        runTop = by;
        runKnownTo = by;
        runBottom = INT_MIN;
    }
    while (by - runKnownTo + 1 <= limit)
    {
        const int y = runKnownTo - 1;
        const int k = y < def.minY ? 0 : acc->block(cx, y, cz);
        if (k <= 1)
        {
            runBottom = runKnownTo;
            return by - runBottom + 1;
        }
        runKnownTo = y;
    }
    return limit + 1;
}

bool SurfaceRuleState::coldEnoughToSnow(int b)
{
    if (b < 0 || b >= (int) def.temperature.size())
        return false;
    // Biome.getHeightAdjustedTemperature() < 0.15
    float t = def.temperature[(size_t) b];
    const int x = cx, y = by, z = cz;
    if (def.frozen[(size_t) b])
    {
        double large = 0;
        double f = 1.0, vf = 1.0 / 7.0;
        for (const PerlinNoise& p : frozenNoise)
        {
            large += simplex(p, x * 0.05 * f, z * 0.05 * f) * vf;
            f /= 2.0;
            vf *= 2.0;
        }
        large *= 7.0;
        const double edge = simplex(infoNoise, x * 0.2, z * 0.2);
        if (large + edge < 0.3 && simplex(infoNoise, x * 0.09, z * 0.09) < 0.8)
            t = 0.2F;
    }
    if (y > def.snowLevel)
    {
        const float v = (float) (simplex(tempNoise, (double) ((float) x / 8.0F), (double) ((float) z / 8.0F)) * 8.0);
        t = t - (v + (float) y - (float) def.snowLevel) * 0.05F / 40.0F;
    }
    return !(t >= 0.15F);
}

bool SurfaceRuleState::compute(int ci)
{
    const SurfaceRuleDef::Cond& c = def.conds[(size_t) ci];
    switch (c.kind)
    {
    case SurfaceRuleDef::C_BIOME:
    {
        const int b = biome();
        const std::vector<char>& set = def.biomeSets[(size_t) c.biomes];
        return b >= 0 && b < (int) set.size() && set[(size_t) b];
    }
    case SurfaceRuleDef::C_NOISE:
    {
        const double v = noiseAt(c.arg);
        return v >= c.lo && v <= c.hi;
    }
    case SurfaceRuleDef::C_GRADIENT:
    {
        if (by <= c.anchorY)
            return true;
        if (by >= c.falseY)
            return false;
        // Mth.map(y, trueY, falseY, 1, 0)
        const double d = ((double) by - c.anchorY) / ((double) c.falseY - c.anchorY);
        const double p = 1.0 + d * (0.0 - 1.0);
        float f;
        if (def.legacy)
        {
            uint64_t s;
            setSeed(&s, (uint64_t) positionSeed(cx, by, cz) ^ gradLo[(size_t) c.arg]);
            f = nextFloat(&s);
        }
        else
        {
            Xoroshiro x = xoroshiroOf((uint64_t) positionSeed(cx, by, cz) ^ gradLo[(size_t) c.arg],
                gradHi[(size_t) c.arg]);
            f = xNextFloat(&x);
        }
        return (double) f < p;
    }
    case SurfaceRuleDef::C_Y_ABOVE:
        return (int64_t) by + (c.addStone ? stoneAbove : 0) >=
            (int64_t) c.anchorY + (int64_t) (c.mult != 0 ? depth() : 0) * c.mult;
    case SurfaceRuleDef::C_WATER:
        return waterHeight == INT_MIN || (int64_t) by + (c.addStone ? stoneAbove : 0) >=
            (int64_t) waterHeight + c.offset + (int64_t) (c.mult != 0 ? depth() : 0) * c.mult;
    case SurfaceRuleDef::C_TEMPERATURE:
        return coldEnoughToSnow(biome());
    case SurfaceRuleDef::C_STEEP:
    {
        // WORLD_SURFACE_WG of the neighbours within the chunk
        const int bx = cx & 15, bz = cz & 15;
        const int x0 = cx - bx, z0 = cz - bz;
        const int n = acc->surfaceBefore(cx, z0 + std::max(bz - 1, 0));
        const int s = acc->surfaceBefore(cx, z0 + std::min(bz + 1, 15));
        if (s >= n + 4)
            return true;
        const int w = acc->surfaceBefore(x0 + std::max(bx - 1, 0), cz);
        const int e = acc->surfaceBefore(x0 + std::min(bx + 1, 15), cz);
        return w >= e + 4;
    }
    case SurfaceRuleDef::C_NOT:
        return !cond(c.arg);
    case SurfaceRuleDef::C_HOLE:
        return depth() <= 0;
    case SurfaceRuleDef::C_ABOVE_PRELIMINARY:
        return by >= minSurfaceLevel();
    case SurfaceRuleDef::C_STONE_DEPTH:
    {
        int limit = 1 + c.offset + (c.addSurface ? depth() : 0);
        if (c.secondary != 0)
        {
            // Mth.map(secondary, -1, 1, 0, range), (int)
            const double s = secondaryValue();
            limit += (int) (0.0 + (s + 1.0) / 2.0 * (c.secondary - 0.0));
        }
        if (!c.ceiling)
            return stoneAbove <= limit;
        return limit >= 1 && stoneBelowAtMost(limit) <= limit;
    }
    }
    return false;
}

bool SurfaceRuleState::cond(int c)
{
    int8_t& m = memo[(size_t) c];
    if (m < 0)
        m = compute(c) ? 1 : 0;
    return m != 0;
}

int SurfaceRuleState::rule(int r)
{
    const SurfaceRuleDef::Rule& rr = def.rules[(size_t) r];
    switch (rr.kind)
    {
    case SurfaceRuleDef::R_BLOCK:
        leaf = r;
        return rr.block;
    case SurfaceRuleDef::R_BANDLANDS:
        return SC_SOLID;
    case SurfaceRuleDef::R_SEQUENCE:
        for (int i = 0; i < rr.count; i++)
        {
            const int v = rule(def.children[(size_t) (rr.first + i)]);
            if (v >= 0)
                return v;
        }
        return -1;
    case SurfaceRuleDef::R_CONDITION:
        return cond(rr.cond) ? rule(rr.then) : -1;
    }
    return -1;
}

// Can a block that is not solid be the result at this block? (false: the
// rules leave a solid block; true: maybe)
bool SurfaceRuleState::maySoft()
{
    for (const SurfaceRuleDef::Leaf& l : def.softLeaves)
    {
        bool all = true;
        for (int32_t c : l.path)
            if (!cond(c))
            {
                all = false;
                break;
            }
        if (all)
            return true;
    }
    return false;
}

void SurfaceRuleState::setBlock(int y, int stone, int water)
{
    by = y;
    stoneAbove = stone;
    waterHeight = water;
    biomeAt = -2;
    std::fill(memo.begin(), memo.end(), (int8_t) -1);
}

void SurfaceRuleState::apply(SurfaceAccess& access, int x, int z, int water, int floorDefault, int *floor,
    int *surface)
{
    if (!def.on || def.noEffect || *floor == NO_HEIGHT)
        return;
    acc = &access;
    cx = x;
    cz = z;
    haveDepth = haveSecondary = haveMinSurface = false;
    std::fill(haveNoise.begin(), haveNoise.end(), (char) 0);
    runBottom = INT_MIN;
    runTop = INT_MIN;
    runKnownTo = INT_MAX;

    // The floor is the highest solid block: the first of its run from the
    // top (the game counts the stone depth above from the last air).
    const int f = *floor;
    setBlock(f, 1, water);
    if (!maySoft())
        return;
    if (floorDefault < 0)
        floorDefault = acc->block(x, f, z) == 2 ? 1 : 0;
    int cls = floorDefault ? rule(def.root) : -1;
    if (cls < 0 || cls == SC_SOLID)
        return;
    // A fluid of a rule below another fluid: what the game's fluid ticks
    // make of that after the generation is not modelled (lava below water
    // may stay, become obsidian or give way to the water): as before the
    // surface step.
    auto mixes = [&](uint8_t placed, int above) {
        return placed != 0 && (above == 1 || above == 4) && (above == 4) != (placed == 2);
    };
    const uint8_t placedF = def.rules[(size_t) leaf].fluid;
    int aboveKind = water != INT_MIN ? acc->block(x, f + 1, z) : 0;
    if (mixes(placedF, aboveKind))
        return;
    aboveKind = placedF == 2 ? 4 : placedF == 1 ? 1 : cls == SC_AIR ? 0 : 2;

    // The floor's block is not solid any more: down the column, as the
    // game's scan, until a block counts for each heightmap.
    int newFloor = NO_HEIGHT, newSurface = *surface != NO_HEIGHT && *surface > f ? *surface : NO_HEIGHT;
    int stone = 1, wh = water;
    if (cls != SC_AIR && newSurface == NO_HEIGHT)
        newSurface = f;
    const int LIMIT = 256;
    int y = f - 1;
    for (int n = 0; n < LIMIT && y >= def.minY && (newFloor == NO_HEIGHT || newSurface == NO_HEIGHT); n++, y--)
    {
        const int k = acc->block(x, y, z);
        if (k == 0)
        {
            stone = 0;
            wh = INT_MIN;
            aboveKind = 0;
            continue;
        }
        if (k == 1 || k == 4)
        {
            if (wh == INT_MIN)
                wh = y + 1;
            if (newSurface == NO_HEIGHT)
                newSurface = y;
            aboveKind = k;
            continue;
        }
        stone++;
        int c = SC_SOLID;
        if (k == 2)
        {
            setBlock(y, stone, wh);
            if (maySoft())
            {
                const int v = rule(def.root);
                if (v >= 0)
                {
                    c = v;
                    const uint8_t placed = def.rules[(size_t) leaf].fluid;
                    if (mixes(placed, aboveKind))
                        return;
                    aboveKind = placed == 2 ? 4 : placed == 1 ? 1 : c == SC_AIR ? 0 : 2;
                }
                else
                    aboveKind = 2;
            }
            else
                aboveKind = 2;
        }
        else
            aboveKind = 3;
        if (c != SC_AIR && newSurface == NO_HEIGHT)
            newSurface = y;
        if (c == SC_SOLID && newFloor == NO_HEIGHT)
            newFloor = y;
    }
    if ((newFloor == NO_HEIGHT || newSurface == NO_HEIGHT) && y >= def.minY)
        return;     // (a column of more than LIMIT such blocks: as before the surface step)
    *floor = newFloor;
    *surface = newSurface;
}

} // namespace dp
