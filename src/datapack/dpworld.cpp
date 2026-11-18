#include "dpworld_p.h"
#include "dpeval.h"

extern "C"
{
#include "biomes.h"
#include "biomenoise.h"
#include "layers.h"
#include "util.h"
}

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace dp
{

static std::atomic<int> g_cursorPolicy { CURSOR_CHUNK };
static std::atomic<bool> g_allowUnverified { false };

void setCursorPolicy(CursorPolicy policy)
{
    g_cursorPolicy = policy;
}

void setAllowUnverifiedVersions(bool allow)
{
    g_allowUnverified = allow;
}

bool versionSupported(int mc)
{
    // Only versions whose vanilla data, loaded through this generator, was
    // compared with the native engine ("dptest vanilla", see
    // tests/dptest.cpp): climate values and biomes at the scales 1 and 4 for
    // the presets normal, large_biomes and amplified, with the preset's list
    // and with the same list as a pack. 1.21.1 was also compared with real
    // servers, with the vanilla data and with a pack of its own; 1.18.2,
    // 26.1 and 26.2 with the vanilla data, 26.3 with the vanilla data and a
    // pack of its own (from 26.2 the engine's own lookup is not the game's,
    // the reference is then the server).
    //
    // 1.18.2 has the data pack format of its own (terrain shaper, presets in
    // the game's code), see dpformats.cpp. 1.18 and 1.18.1 have no noise
    // router in their data.
    switch (mc)
    {
    case MC_1_18_2:
    case MC_1_19_2:     // 1.19 - 1.19.2 data; tree with 10 children per node
    case MC_1_19_4:
    case MC_1_20_6:     // checked with the data of 1.20.1 and of 1.20.6
    case MC_1_21_1:
    case MC_1_21_2:
    case MC_1_21_3:
    case MC_1_21_4:
    case MC_1_21_5:
    case MC_1_21_6:
    case MC_1_21_7:
    case MC_1_21_8:
    case MC_1_21_9:
    case MC_1_21_10:
    case MC_1_21_11:
    case MC_26_1:       // 26.1 - 26.1.2: the data of 1.21.11 with new fields
    case MC_26_1_1:
    case MC_26_1_2:
    case MC_26_2:       // interval_select; the list gets Sulfur Caves (dplistorder.h)
    case MC_26_3:       // new format, float evaluation (dpformats.cpp), tree arity 19
        return true;
    default:
        return false;
    }
}

QString versionName(int mc)
{
    // the engine names the 1.18 group after its first version
    if (mc == MC_1_18_2)
        return QStringLiteral("1.18.2");
    return QString::fromLatin1(mc2str(mc));
}

// The versions whose terrain fill (dpterrain.cpp) was checked: compared with
// real servers (1.18.2, 1.20.6, 1.21.1, 26.1, 26.2) or the same router format
// and fill (1.19.2 to 1.21.11, 26.1.1, 26.1.2). Not 26.3: its chunks are
// filled in float with an interpolation of their own.
static bool terrainVersionVerified(int mc)
{
    return mc >= MC_1_18_2 && mc <= MC_26_2;
}

//==============================================================================
// world
//==============================================================================

void WorldPrivate::issue(Issue::Severity sev, int pack, const QString& id, const QString& text)
{
    Issue i;
    i.severity = sev;
    i.pack = res.packName(pack);
    i.id = id;
    i.text = text;
    for (const Issue& o : report->issues)
        if (o.severity == i.severity && o.pack == i.pack && o.id == i.id && o.text == i.text)
            return;
    report->issues.push_back(i);
}

// Reads the registry entry @id as a JSON object; reports an error (for the
// file @fromId of @fromPack that refers to it) if it is missing or broken.
bool WorldPrivate::parseJsonFile(const char *registry, const QString& id, int fromPack,
    const QString& fromId, QJsonObject *obj, int *pack)
{
    const Resource *r = res.find(registry, id);
    if (!r)
    {
        issue(Issue::Error, fromPack, fromId,
            QStringLiteral("%1 does not exist (%2).").arg(id, QLatin1String(registry)));
        return false;
    }
    QJsonValue v;
    QString err;
    if (!parseJson(r->data, &v, &err) || !v.isObject())
    {
        issue(Issue::Error, r->pack, id,
            QStringLiteral("The file is not valid JSON (%1): %2.").arg(QLatin1String(registry), err));
        return false;
    }
    *obj = v.toObject();
    if (pack)
        *pack = r->pack;
    return true;
}

// 26.3: the deepest nesting of "interpolated" in climate functions that is
// offered (each level costs 8 evaluations of the level below per sample)
static const int MAX_INTERP_NESTING = 4;

// The game's codecs of DimensionType and NoiseSettings, with the same limits
// in 1.18.2 to 26.3 (DimensionType.MIN_Y = -2032, MAX_Y = 2031, Y_SIZE =
// 4064): a pack outside of them does not load in the game.
static bool intField(const QJsonObject& o, const char *key, int lo, int hi, int *out, QString *why)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble())
    {
        *why = QStringLiteral("\"%1\" is missing or not a number").arg(QLatin1String(key));
        return false;
    }
    const double d = v.toDouble();
    if (d != std::floor(d) || d < lo || d > hi)
    {
        *why = QStringLiteral("\"%1\" must be a whole number from %2 to %3").arg(QLatin1String(key)).arg(lo).arg(hi);
        return false;
    }
    *out = (int) d;
    return true;
}

static bool checkDimensionType(const QJsonObject& t, QString *why)
{
    int minY = 0, height = 0, logical = 0;
    if (!intField(t, "min_y", -2032, 2031, &minY, why) || !intField(t, "height", 16, 4064, &height, why))
        return false;
    const bool hasLogical = t.contains(QLatin1String("logical_height"));
    if (hasLogical && !intField(t, "logical_height", 0, 4064, &logical, why))
        return false;
    if (minY + height > 2032)
        *why = QStringLiteral("min_y + height cannot be higher than 2032");
    else if (hasLogical && logical > height)
        *why = QStringLiteral("logical_height cannot be higher than height");
    else if (height % 16 != 0)
        *why = QStringLiteral("height has to be a multiple of 16");
    else if (minY % 16 != 0)
        *why = QStringLiteral("min_y has to be a multiple of 16");
    else
        return true;
    return false;
}

static bool checkNoiseSettings(const QJsonObject& settings, int mc, QString *why)
{
    const QJsonValue nv = settings.value(QLatin1String("noise"));
    if (!nv.isObject())
    {
        *why = QStringLiteral("\"noise\" is missing");
        return false;
    }
    const QJsonObject noise = nv.toObject();
    int minY = 0, height = 0, size = 0;
    if (!intField(noise, "min_y", -2032, 2031, &minY, why) || !intField(noise, "height", 0, 4064, &height, why))
        return false;
    // 26.3 moved the cell sizes out of the noise settings
    if (mc < MC_26_3 && (!intField(noise, "size_horizontal", 1, 4, &size, why) ||
        !intField(noise, "size_vertical", 1, 4, &size, why)))
        return false;
    if (minY + height > 2032)
        *why = QStringLiteral("min_y + height cannot be higher than 2032");
    else if (height % 16 != 0)
        *why = QStringLiteral("height has to be a multiple of 16");
    else if (minY % 16 != 0)
        *why = QStringLiteral("min_y has to be a multiple of 16");
    else
        return true;
    return false;
}

void WorldPrivate::loadColors()
{
    // 1. the colour files of the packs (a convention of the map tool by
    //    Jochen Jacobs): {"ns:biome": {"r": .., "g": .., "b": .., "name": ".."}}
    for (const Resource& r : res.colorFiles)
    {
        QJsonValue v;
        QString err;
        if (!parseJson(r.data, &v, &err) || !v.isObject())
        {
            issue(Issue::Warning, r.pack, QStringLiteral("c:worldgen/biome_colors"),
                QStringLiteral("The biome colour file is not valid JSON: %1.").arg(err));
            continue;
        }
        const QJsonObject o = v.toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        {
            // like the map tool: a key without namespace gets the file's
            QString id = it.key().contains(QLatin1Char(':')) ? it.key() : QStringLiteral("c:") + it.key();
            QJsonObject e = it.value().toObject();
            if (!e.value(QLatin1String("r")).isDouble() || !e.value(QLatin1String("g")).isDouble() ||
                !e.value(QLatin1String("b")).isDouble())
                continue;
            auto ch = [&e](const char *k) { return std::max(0, std::min(255, e.value(QLatin1String(k)).toInt())); };
            packColors.insert(id, qRgb(ch("r"), ch("g"), ch("b")));
            QString name = e.value(QLatin1String("name")).toString();
            if (!name.isEmpty())
                packNames.insert(id, name);
        }
    }
    // 2. the engine's default colours of the vanilla biomes
    unsigned char colors[256][3];
    initBiomeColors(colors);
    for (int id = 0; id < 256; id++)
    {
        const char *name = biome2str(request.mc, id);
        if (name)
            vanillaColors.insert(QStringLiteral("minecraft:") + QLatin1String(name),
                qRgb(colors[id][0], colors[id][1], colors[id][2]));
    }
}

// The colour of a biome without one of its own (no colour file, not
// vanilla). The biomes a pack defines get their colours together, in the
// order of their ids: each takes the first variant of its colour
// (fallbackBiomeColor) that is not too close to those of the pack's biomes
// before it, or else the variant furthest from them. So the colours depend
// only on the pack's own biomes: the same on every load, in every version
// with the same biomes, whatever other packs are loaded.
QRgb WorldPrivate::fallbackColor(const QString& id)
{
    const Resource *r = res.find(REG_BIOME, id);
    if (!r)
        return fallbackBiomeColor(id, QJsonObject());
    auto cached = packBiomeColors.constFind(r->pack);
    if (cached == packBiomeColors.constEnd())
    {
        QHash<QString, QRgb> colors;
        std::vector<QRgb> placed;
        const QStringList ids = res.ids(REG_BIOME);     // sorted
        std::vector<QString> own;
        for (const QString& b : ids)
        {
            const Resource *br = res.find(REG_BIOME, b);
            if (!br || br->pack != r->pack || vanillaColors.contains(b))
                continue;
            if (packColors.contains(b))
                placed.push_back(packColors.value(b));
            else
                own.push_back(b);
        }
        for (const QString& b : own)
        {
            QJsonObject json;
            QJsonValue v;
            QString err;
            if (parseJson(res.find(REG_BIOME, b)->data, &v, &err))
                json = v.toObject();
            QRgb best = 0;
            double bestDist = -1;
            for (int k = 0; k < BIOME_COLOR_VARIANTS; k++)
            {
                const QRgb c = fallbackBiomeColor(b, json, k);
                double d = 1e9;
                for (QRgb p : placed)
                    d = std::min(d, colorDistance(c, p));
                if (d > bestDist)
                {
                    best = c;
                    bestDist = d;
                }
                if (d >= MIN_BIOME_COLOR_DISTANCE)
                    break;
            }
            colors.insert(b, best);
            placed.push_back(best);
        }
        cached = packBiomeColors.insert(r->pack, colors);
    }
    return cached.value().value(id, fallbackBiomeColor(id, QJsonObject()));
}

int WorldPrivate::biome(const QString& id)
{
    auto it = biomeIndex.constFind(id);
    if (it != biomeIndex.constEnd())
        return it.value();
    BiomeInfo b;
    b.id = id;
    b.vanilla = vanillaColors.contains(id);
    b.name = packNames.value(id);
    if (b.name.isEmpty())
        b.name = biomeDisplayName(id);
    if (packColors.contains(id))
        b.color = packColors.value(id);
    else if (b.vanilla)
        b.color = vanillaColors.value(id);
    else
        b.color = fallbackColor(id);
    biomes.push_back(b);
    int idx = (int) biomes.size() - 1;
    biomeIndex.insert(id, idx);
    return idx;
}

// One span of a parameter point: a number or [min, max], within -2 .. 2
static bool readParameter(const QJsonValue& v, float lo, float hi, int64_t *qlo, int64_t *qhi, QString *err)
{
    float a, b;
    if (v.isDouble())
    {
        a = b = (float) v.toDouble();
    }
    else if (v.isArray() && v.toArray().size() == 2 && v.toArray().at(0).isDouble() &&
        v.toArray().at(1).isDouble())
    {
        a = (float) v.toArray().at(0).toDouble();
        b = (float) v.toArray().at(1).toDouble();
    }
    else
    {
        *err = QStringLiteral("is not a number or a [min, max] pair");
        return false;
    }
    if (a > b)
    {
        *err = QStringLiteral("has its minimum above its maximum");
        return false;
    }
    if (!(a >= lo && b <= hi))
    {
        *err = QStringLiteral("is outside of %1 .. %2").arg(lo).arg(hi);
        return false;
    }
    *qlo = quantizeClimate(a);
    *qhi = quantizeClimate(b);
    return true;
}

bool WorldPrivate::loadBiomeSource(const QJsonObject& src, int pack, const QString& dimId,
    DimensionInfo& info, DimData& d, bool *needsClimate)
{
    *needsClimate = false;
    QString type = src.value(QLatin1String("type")).toString();
    if (type.startsWith(QLatin1String("minecraft:")))
        type = type.mid(10);

    QSet<int> listed;
    auto add = [&](const QString& id) {
        int b = biome(normalizeId(id));
        if (!listed.contains(b))
        {
            listed.insert(b);
            info.biomes.push_back(b);
        }
        return b;
    };
    // a biome that no pack defines: the game would refuse to load
    QStringList missing;
    auto check = [&](const QString& id) {
        QString n = normalizeId(id);
        if (!res.find(REG_BIOME, n) && !missing.contains(n))
            missing.push_back(n);
    };
    auto reportMissing = [&]() {
        if (missing.isEmpty())
            return;
        QString names = missing.mid(0, 5).join(QStringLiteral(", "));
        if (missing.size() > 5)
            names += QStringLiteral(", ...");
        issue(Issue::Warning, pack, dimId,
            QStringLiteral("%1 biome(s) of the biome source are not defined by any loaded pack (%2); "
                "the game would not load this.").arg(missing.size()).arg(names));
    };

    if (type == QLatin1String("fixed"))
    {
        QString b = src.value(QLatin1String("biome")).toString();
        if (b.isEmpty())
        {
            issue(Issue::Error, pack, dimId, QStringLiteral("The fixed biome source names no biome."));
            return false;
        }
        check(b);
        d.fixedBiome = add(b);
        d.source = DimData::FIXED;
        reportMissing();
        return true;
    }
    if (type == QLatin1String("checkerboard"))
    {
        QStringList ids;
        QJsonValue bv = src.value(QLatin1String("biomes"));
        if (bv.isString())
        {
            QString s = bv.toString();
            if (s.startsWith(QLatin1Char('#')))
            {
                QString err;
                ids = res.tagValues(REG_BIOME_TAG, s.mid(1), &err);
                if (!err.isEmpty())
                {
                    issue(Issue::Error, pack, dimId, err);
                    return false;
                }
            }
            else
                ids.push_back(s);
        }
        else
        {
            for (const QJsonValue& e : bv.toArray())
                ids.push_back(e.toString());
        }
        if (ids.isEmpty())
        {
            issue(Issue::Error, pack, dimId, QStringLiteral("The checkerboard biome source has no biomes."));
            return false;
        }
        for (const QString& b : ids)
        {
            check(b);
            d.checker.push_back(add(b));
        }
        // CheckerboardColumnBiomeSource: squares of 2^(scale + 2) biome cells;
        // the codec takes a scale from 0 to 62 (the shift of an int by 32 or
        // more is taken mod 32 at the lookup, as in Java)
        int scale = 2;
        if (src.contains(QLatin1String("scale")))
        {
            const QJsonValue sv = src.value(QLatin1String("scale"));
            const double sd = sv.toDouble(-1.0);
            if (!sv.isDouble() || sd != std::floor(sd) || sd < 0 || sd > 62)
            {
                issue(Issue::Error, pack, dimId,
                    QStringLiteral("The checkerboard scale must be a whole number from 0 to 62."));
                return false;
            }
            scale = (int) sd;
        }
        d.checkerShift = scale + 2;
        d.source = DimData::CHECKERBOARD;
        reportMissing();
        return true;
    }
    if (type == QLatin1String("multi_noise"))
    {
        *needsClimate = true;
        if (src.contains(QLatin1String("preset")))
        {
            QString preset = normalizeId(src.value(QLatin1String("preset")).toString());
            if (request.mc >= MC_1_19_4)
            {
                // the preset names a parameter list file, which names the
                // built-in list
                QJsonObject o;
                if (!parseJsonFile(REG_PARAMETER_LIST, preset, pack, dimId, &o, nullptr))
                    return false;
                preset = normalizeId(o.value(QLatin1String("preset")).toString());
            }
            if (preset == QLatin1String("minecraft:overworld") && request.mc >= MC_26_2)
            {
                // the list in the game's order, through the generic tree
                std::vector<ClimateEntry> entries;
                std::vector<int32_t> ids;
                if (!nativeOverworldList(request.mc, &entries, &ids))
                {
                    issue(Issue::Error, pack, dimId, QStringLiteral("The built-in overworld biome list is "
                        "not available for this version."));
                    return false;
                }
                for (int32_t id : ids)
                {
                    const char *name = biome2str(request.mc, id);
                    d.entryBiome.push_back(add(QStringLiteral("minecraft:") + QLatin1String(name ? name : "?")));
                }
                d.tree.build(entries, climateArity(request.mc));
                d.tree.setGroups(d.entryBiome);
                d.source = DimData::MULTI_NOISE;
                return true;
            }
            if (preset == QLatin1String("minecraft:overworld"))
            {
                // the list is defined in the game's code: the engine has it
                for (int id = 0; id < 256; id++)
                {
                    d.nativeBiome[id] = -1;
                    const char *name = biome2str(request.mc, id);
                    if (name && isOverworld(request.mc, id))
                        d.nativeBiome[id] = add(QStringLiteral("minecraft:") + QLatin1String(name));
                }
                d.source = DimData::NATIVE_OVERWORLD;
                std::vector<ClimateEntry> entries;
                if (nativeOverworldEntries(request.mc, &entries, &d.nativeGroups, &d.nativeNode, &d.nativeEntry))
                {
                    d.nativeTree.build(entries, 6);
                    d.nativeTree.setGroups(d.nativeGroups);
                }
                return true;
            }
            if (preset == QLatin1String("minecraft:nether"))
            {
                // MultiNoiseBiomeSourceParameterList.Preset.NETHER, defined in
                // the game's code: temperature, humidity, continentalness,
                // erosion, depth, weirdness, offset
                static const struct { const char *biome; float p[7]; } nether[] = {
                    { "minecraft:nether_wastes",    { 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F } },
                    { "minecraft:soul_sand_valley", { 0.0F, -0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F } },
                    { "minecraft:crimson_forest",   { 0.4F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F } },
                    { "minecraft:warped_forest",    { 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.375F } },
                    { "minecraft:basalt_deltas",    { -0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.175F } },
                };
                std::vector<ClimateEntry> entries;
                for (const auto& e : nether)
                {
                    ClimateEntry c;
                    for (int k = 0; k < CLIMATE_DIMS; k++)
                        c.lo[k] = c.hi[k] = quantizeClimate(e.p[k]);
                    check(QLatin1String(e.biome));
                    entries.push_back(c);
                    d.entryBiome.push_back(add(QLatin1String(e.biome)));
                }
                d.tree.build(entries, request.mc >= MC_1_19_4 ? 6 : 10);
                d.tree.setGroups(d.entryBiome);
                d.source = DimData::MULTI_NOISE;
                reportMissing();
                return true;
            }
            issue(Issue::Error, pack, dimId,
                QStringLiteral("Unknown biome list preset \"%1\".").arg(preset));
            return false;
        }

        QJsonValue bv = src.value(QLatin1String("biomes"));
        if (!bv.isArray() || bv.toArray().isEmpty())
        {
            issue(Issue::Error, pack, dimId,
                QStringLiteral("The multi_noise biome source needs a preset or a list of biomes."));
            return false;
        }
        static const char *keys[CLIMATE_DIMS] = {
            "temperature", "humidity", "continentalness", "erosion", "depth", "weirdness", "offset"
        };
        std::vector<ClimateEntry> entries;
        int n = 0;
        for (const QJsonValue& ev : bv.toArray())
        {
            QJsonObject eo = ev.toObject();
            QString b = eo.value(QLatin1String("biome")).toString();
            QJsonObject po = eo.value(QLatin1String("parameters")).toObject();
            if (b.isEmpty())
            {
                issue(Issue::Error, pack, dimId,
                    QStringLiteral("Entry %1 of the biome list names no biome.").arg(n));
                return false;
            }
            ClimateEntry e;
            for (int k = 0; k < CLIMATE_DIMS; k++)
            {
                QString err;
                const bool offset = k == CLIMATE_OFFSET;
                if (!readParameter(po.value(QLatin1String(keys[k])), offset ? 0.0F : -2.0F,
                    offset ? 1.0F : 2.0F, &e.lo[k], &e.hi[k], &err) || (offset && e.lo[k] != e.hi[k]))
                {
                    if (err.isEmpty())
                        err = QStringLiteral("must be a single number");
                    issue(Issue::Error, pack, dimId,
                        QStringLiteral("Entry %1 of the biome list (%2): the parameter %3 %4.")
                            .arg(n).arg(b, QLatin1String(keys[k]), err));
                    return false;
                }
            }
            check(b);
            entries.push_back(e);
            d.entryBiome.push_back(add(b));
            n++;
        }
        d.tree.build(entries, climateArity(request.mc));
        d.tree.setGroups(d.entryBiome);
        d.source = DimData::MULTI_NOISE;
        reportMissing();
        return true;
    }
    if (type == QLatin1String("the_end"))
    {
        // TheEndBiomeSource: the five biomes are fixed ids (a pack can
        // redefine them, not replace them); the router's erosion decides
        static const char *const ids[5] = {
            "minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands",
            "minecraft:small_end_islands", "minecraft:end_barrens"
        };
        for (int k = 0; k < 5; k++)
        {
            check(QLatin1String(ids[k]));
            d.endBiomes[k] = add(QLatin1String(ids[k]));
        }
        *needsClimate = true;
        d.source = DimData::THE_END;
        reportMissing();
        return true;
    }
    issue(Issue::Error, pack, dimId,
        QStringLiteral("Unknown biome source type \"%1\".").arg(src.value(QLatin1String("type")).toString()));
    return false;
}

void WorldPrivate::loadDimension(const QString& id, const QJsonValue& json, int pack)
{
    dims.emplace_back();
    data.emplace_back();
    DimensionInfo& info = dims.back();
    DimData& d = data.back();
    info.id = id;
    info.supported = false;

    if (!json.isObject())
    {
        issue(Issue::Error, pack, id, QStringLiteral("The dimension is not a JSON object."));
        info.reason = QStringLiteral("The dimension's file has errors.");
        return;
    }
    const QJsonObject o = json.toObject();

    // height range of the dimension type
    bool haveType = false;
    QJsonValue tv = o.value(QLatin1String("type"));
    QJsonObject to;
    if (tv.isObject())
    {
        to = tv.toObject();
        haveType = true;
    }
    int typePack = pack;
    QString typeId = id;
    if (tv.isString())
    {
        typeId = normalizeId(tv.toString());
        haveType = parseJsonFile(REG_DIMENSION_TYPE, typeId, pack, id, &to, &typePack);
    }
    if (haveType)
    {
        QString why;
        if (!checkDimensionType(to, &why))
        {
            issue(Issue::Error, typePack, typeId, QStringLiteral("The dimension type is invalid: %1.").arg(why));
            info.reason = QStringLiteral("The dimension type is invalid.");
            return;
        }
        info.minY = to.value(QLatin1String("min_y")).toInt(info.minY);
        info.height = to.value(QLatin1String("height")).toInt(info.height);
        info.hasCeiling = to.value(QLatin1String("has_ceiling")).toBool(false);
    }

    const QJsonObject gen = o.value(QLatin1String("generator")).toObject();
    QString gtype = gen.value(QLatin1String("type")).toString();
    if (gtype.startsWith(QLatin1String("minecraft:")))
        gtype = gtype.mid(10);
    if (gtype != QLatin1String("noise"))
    {
        if (gtype == QLatin1String("flat") || gtype == QLatin1String("debug"))
            info.reason = QStringLiteral("The generator type \"%1\" is not supported yet.").arg(gtype);
        else
        {
            issue(Issue::Error, pack, id, QStringLiteral("Unknown generator type \"%1\".")
                .arg(gen.value(QLatin1String("type")).toString()));
            info.reason = QStringLiteral("The dimension's generator is unknown.");
        }
        return;
    }

    // noise settings: a reference or inline
    QJsonObject settings;
    QString settingsId = id;
    int settingsPack = pack;
    QJsonValue sv = gen.value(QLatin1String("settings"));
    if (sv.isObject())
    {
        settings = sv.toObject();
    }
    else
    {
        settingsId = normalizeId(sv.toString());
        if (!sv.isString() || !parseJsonFile(REG_NOISE_SETTINGS, settingsId, pack, id, &settings, &settingsPack))
        {
            if (!sv.isString())
                issue(Issue::Error, pack, id, QStringLiteral("The generator names no noise settings."));
            info.reason = QStringLiteral("The dimension's noise settings have errors.");
            return;
        }
    }
    {
        QString why;
        if (!checkNoiseSettings(settings, request.mc, &why))
        {
            issue(Issue::Error, settingsPack, settingsId, QStringLiteral("The noise settings are invalid: %1.").arg(why));
            info.reason = QStringLiteral("The dimension's noise settings are invalid.");
            return;
        }
    }
    info.seaLevel = settings.value(QLatin1String("sea_level")).toInt(info.seaLevel);
    const QJsonObject noise = settings.value(QLatin1String("noise")).toObject();
    d.noiseMinY = noise.value(QLatin1String("min_y")).toInt(info.minY);
    d.noiseHeight = noise.value(QLatin1String("height")).toInt(info.height);
    if (!haveType)
    {
        info.minY = noise.value(QLatin1String("min_y")).toInt(info.minY);
        info.height = noise.value(QLatin1String("height")).toInt(info.height);
    }

    bool needsClimate = false;
    QString reason;
    if (!loadBiomeSource(gen.value(QLatin1String("biome_source")).toObject(), pack, id, info, d, &needsClimate))
    {
        if (info.reason.isEmpty())
            info.reason = QStringLiteral("The dimension's biome source has errors.");
        info.biomes.clear();
        d.source = DimData::NONE;
        return;
    }

    d.legacy = settings.value(QLatin1String("legacy_random_source")).toBool(false);
    if (d.legacy && request.mc >= MC_26_3)
    {
        // the float noises of 26.3 are only seeded the xoroshiro way here
        info.reason = QStringLiteral("legacy_random_source is not supported in %1 yet.")
            .arg(versionName(request.mc));
        info.biomes.clear();
        d.source = DimData::NONE;
        return;
    }
    const QJsonObject router = settings.value(QLatin1String("noise_router")).toObject();
    // 1.18.2: the noise settings that terrain_shaper_spline, slide and
    // old_blended_noise take their parameters from (-1 in other versions)
    const int legacySettings = graph->legacySettings(settingsId, settings);
    if (needsClimate)
    {
        static const char *keys[6] = {
            "temperature", "vegetation", "continents", "erosion", "depth", "ridges"
        };
        bool ok = true;
        std::vector<int> outputs;
        for (int k = 0; k < 6; k++)
        {
            if (!router.contains(QLatin1String(keys[k])))
            {
                issue(Issue::Error, settingsPack, settingsId,
                    QStringLiteral("The noise router has no \"%1\".").arg(QLatin1String(keys[k])));
                ok = false;
                continue;
            }
            d.outputs[k] = graph->compile(router.value(QLatin1String(keys[k])), settingsPack, settingsId,
                legacySettings);
            if (d.outputs[k] < 0)
                ok = false;
            outputs.push_back(d.outputs[k]);
        }
        if (!ok)
        {
            info.reason = QStringLiteral("The dimension's climate functions have errors.");
            info.biomes.clear();
            d.source = DimData::NONE;
            return;
        }
        d.program.build(*graph, outputs);
        std::vector<int> plain;
        for (int k = 0; k < 6; k++)
        {
            d.plainOutputs[k] = graph->compilePlain(router.value(QLatin1String(keys[k])), settingsPack, settingsId,
                legacySettings);
            plain.push_back(d.plainOutputs[k]);
        }
        d.plainProgram.build(*graph, plain);
        // 26.3 makes minecraft:nether/temperature and nether/vegetation the
        // Nether's legacy biome noises in every dimension (in float, not
        // ported): a climate that uses them is not offered
        if (request.mc >= MC_26_3)
        {
            // "interpolated" evaluates its argument at the 8 corners of its
            // cell; one inside another multiplies that (the game fills whole
            // chunks with cached corners instead). Vanilla nests none.
            int nest = 0;
            for (int k = 0; k < 6; k++)
            {
                if (d.outputs[k] >= 0)
                    nest = std::max(nest, (int) graph->interpNesting[(size_t) d.outputs[k]]);
                if (d.plainOutputs[k] >= 0)
                    nest = std::max(nest, (int) graph->interpNesting[(size_t) d.plainOutputs[k]]);
            }
            if (nest > MAX_INTERP_NESTING)
            {
                info.reason = QStringLiteral("The climate functions nest \"interpolated\" %1 levels deep; more "
                    "than %2 are not supported yet.").arg(nest).arg(MAX_INTERP_NESTING);
                info.biomes.clear();
                d.source = DimData::NONE;
                return;
            }
            for (int32_t i : d.program.usedNoises)
            {
                const QString& nid = graph->noises[(size_t) i].id;
                if (nid == QLatin1String("minecraft:nether/temperature") ||
                    nid == QLatin1String("minecraft:nether/vegetation"))
                {
                    info.reason = QStringLiteral("The noise %1 is not supported in %2 yet.")
                        .arg(nid, versionName(request.mc));
                    info.biomes.clear();
                    d.source = DimData::NONE;
                    return;
                }
            }
        }
    }

    // terrain: final_density
    {
        QString why;
        int fd = -1;
        if (router.contains(QLatin1String("final_density")))
            fd = graph->compile(router.value(QLatin1String("final_density")), settingsPack, settingsId,
                legacySettings);
        else
            why = QStringLiteral("The noise router has no final_density.");
        if (!terrainVersionVerified(request.mc))
        {
            d.terrain = TerrainDef();
            info.terrainReason = QStringLiteral("Terrain heights of Minecraft %1 are not supported yet.")
                .arg(versionName(request.mc));
        }
        else if (buildTerrain(*graph, fd, settings, info.minY, info.height, &d.terrain, &why))
        {
            info.terrain = true;
            if (d.terrain.aquifers)
            {
                buildAquifer(*graph, settings, settingsPack, settingsId, legacySettings, request.mc, d.terrain.minY,
                    d.terrain.height, d.terrain.cellHeight, d.terrain.defaultBlock == TerrainDef::SOLID,
                    &d.terrain.aquifer);
                if (!d.terrain.aquifer.on)
                    info.aquiferReason = d.terrain.aquifer.reason;
            }
            // the surface rules
            SurfaceBuildContext sc;
            sc.graph = graph.get();
            sc.pack = settingsPack;
            sc.id = settingsId;
            sc.legacySettings = legacySettings;
            sc.mc = request.mc;
            sc.seaLevel = d.terrain.seaLevel;
            sc.minGenY = std::max(info.minY, d.noiseMinY);
            sc.genDepth = std::min(info.height, d.noiseHeight);
            sc.minY = info.minY;
            sc.legacy = d.terrain.legacy;
            sc.hasPreliminary = d.terrain.aquifers && d.terrain.aquifer.on;
            sc.biomeIndex = &biomeIndex;
            sc.biomeCount = (int) biomes.size();
            sc.climate = [this](int b, float *t, bool *frozen) {
                const Resource *r = res.find(REG_BIOME, biomes[(size_t) b].id);
                QJsonValue v;
                QString err;
                if (!r || !parseJson(r->data, &v, &err) || !v.isObject())
                    return false;
                const QJsonObject o = v.toObject();
                if (!o.value(QLatin1String("temperature")).isDouble())
                    return false;
                *t = (float) o.value(QLatin1String("temperature")).toDouble();
                *frozen = o.value(QLatin1String("temperature_modifier")).toString() == QLatin1String("frozen");
                return true;
            };
            sc.biomeTag = [this](const QString& tag) { return res.tagValues(REG_BIOME_TAG, tag); };
            buildSurfaceRules(settings, sc, &d.surface);
            if (!d.surface.on && !d.surface.reason.isEmpty())
                info.surfaceReason = d.surface.reason;
        }
        else
            info.terrainReason = why.isEmpty() ? QStringLiteral("final_density has errors.") : why;
    }
    d.minCellY = info.minY >> 2;
    d.maxCellY = std::max(d.minCellY, ((info.minY + info.height) >> 2) - 1);
    info.supported = true;
}

// The world types of 1.18.2, which the game defines in code
// (WorldGenSettings, DimensionType): the dimensions as a world preset file of
// later versions would list them.
static bool builtinPreset118(const QString& id, QJsonObject *preset)
{
    QString settings;
    if (id == QLatin1String("minecraft:normal"))
        settings = QStringLiteral("minecraft:overworld");
    else if (id == QLatin1String("minecraft:large_biomes"))
        settings = QStringLiteral("minecraft:large_biomes");
    else if (id == QLatin1String("minecraft:amplified"))
        settings = QStringLiteral("minecraft:amplified");
    else
        return false;
    auto dim = [](const char *type, const QString& settings, const QJsonObject& source) {
        QJsonObject gen;
        gen[QStringLiteral("type")] = QStringLiteral("minecraft:noise");
        gen[QStringLiteral("settings")] = settings;
        gen[QStringLiteral("biome_source")] = source;
        QJsonObject d;
        d[QStringLiteral("type")] = QLatin1String(type);
        d[QStringLiteral("generator")] = gen;
        return d;
    };
    auto multiNoise = [](const char *list) {
        QJsonObject o;
        o[QStringLiteral("type")] = QStringLiteral("minecraft:multi_noise");
        o[QStringLiteral("preset")] = QLatin1String(list);
        return o;
    };
    QJsonObject end;
    end[QStringLiteral("type")] = QStringLiteral("minecraft:the_end");
    QJsonObject dims;
    dims[QStringLiteral("minecraft:overworld")] = dim("minecraft:overworld", settings, multiNoise("minecraft:overworld"));
    dims[QStringLiteral("minecraft:the_nether")] = dim("minecraft:the_nether", QStringLiteral("minecraft:nether"),
        multiNoise("minecraft:nether"));
    dims[QStringLiteral("minecraft:the_end")] = dim("minecraft:the_end", QStringLiteral("minecraft:end"), end);
    QJsonObject p;
    p[QStringLiteral("dimensions")] = dims;
    *preset = p;
    return true;
}

void WorldPrivate::load()
{
    loadResources(request, &res, report);
    for (size_t i = 1; i < res.packs.size(); i++)
        packs.push_back(res.packs[i]);
    presets = res.ids(REG_WORLD_PRESET);
    if (request.mc <= MC_1_18_2)
    {
        for (const char *p : { "minecraft:amplified", "minecraft:large_biomes", "minecraft:normal" })
            if (!presets.contains(QLatin1String(p)))
                presets.push_back(QLatin1String(p));
        presets.sort();
    }

    if (!versionSupported(request.mc) && !g_allowUnverified)
    {
        const char *name = mc2str(request.mc);
        issue(Issue::Error, -1, QString(),
            QStringLiteral("Datapack worlds are not available for Minecraft %1.")
                .arg(QLatin1String(name ? name : "?")));
        return;
    }
    if (!report->ok())
        return;

    for (const PackInfo& p : packs)
    {
        if (p.loaded && !p.formatMatches)
            issue(Issue::Warning, (int) (&p - packs.data()) + 1, QString(),
                QStringLiteral("The pack does not declare support for Minecraft %1 (pack format %2)%3.")
                    .arg(QLatin1String(mc2str(request.mc))).arg(p.packFormat)
                    .arg(p.formatProblem.isEmpty() ? QString() : QStringLiteral(": ") + p.formatProblem));
    }

    graph.reset(new DensityGraph(res, report, request.mc));
    loadColors();

    // Everything the game would parse when it loads the packs: a broken or
    // unknown density function, noise or noise router anywhere stops it.
    for (const QString& id : res.ids(REG_DENSITY_FUNCTION))
        graph->compileReference(id, res.find(REG_DENSITY_FUNCTION, id)->pack, id);
    for (const QString& id : res.ids(REG_NOISE))
        graph->checkNoise(id);
    for (const QString& id : res.ids(REG_NOISE_SETTINGS))
    {
        QJsonObject o;
        int pack = -1;
        if (!parseJsonFile(REG_NOISE_SETTINGS, id, -1, id, &o, &pack))
            continue;
        QJsonValue rv = o.value(QLatin1String("noise_router"));
        if (!rv.isObject())
        {
            issue(Issue::Error, pack, id, QStringLiteral("The noise settings have no noise_router."));
            continue;
        }
        QString why;
        if (!checkNoiseSettings(o, request.mc, &why))
        {
            issue(Issue::Error, pack, id, QStringLiteral("The noise settings are invalid: %1.").arg(why));
            continue;
        }
        const QJsonObject router = rv.toObject();
        const int legacy = graph->legacySettings(id, o);
        for (auto it = router.constBegin(); it != router.constEnd(); ++it)
            graph->compile(it.value(), pack, id, legacy);
    }

    // The dimensions of the world preset, replaced or extended by the
    // dimension files of the packs.
    QMap<QString, QPair<QJsonValue, int>> found;
    QString presetId = normalizeId(request.preset);
    QJsonObject preset;
    int presetPack = -1;
    if (request.mc <= MC_1_18_2 && !res.find(REG_WORLD_PRESET, presetId))
    {
        // 1.18.2 defines its world types in code (WorldGenSettings)
        if (builtinPreset118(presetId, &preset))
        {
            const QJsonObject dimsObj = preset.value(QLatin1String("dimensions")).toObject();
            for (auto it = dimsObj.constBegin(); it != dimsObj.constEnd(); ++it)
                found.insert(normalizeId(it.key()), qMakePair(it.value(), 0));
        }
        else
        {
            issue(Issue::Error, -1, presetId,
                QStringLiteral("Unknown world preset %1 (Minecraft %2 has normal, large_biomes and amplified).")
                    .arg(presetId, QLatin1String(mc2str(request.mc))));
        }
    }
    else if (parseJsonFile(REG_WORLD_PRESET, presetId, -1, presetId, &preset, &presetPack))
    {
        const QJsonObject dimsObj = preset.value(QLatin1String("dimensions")).toObject();
        for (auto it = dimsObj.constBegin(); it != dimsObj.constEnd(); ++it)
            found.insert(normalizeId(it.key()), qMakePair(it.value(), presetPack));
    }
    for (const QString& id : res.ids(REG_DIMENSION))
    {
        const Resource *r = res.find(REG_DIMENSION, id);
        QJsonValue v;
        QString err;
        if (!parseJson(r->data, &v, &err))
        {
            issue(Issue::Error, r->pack, id, QStringLiteral("The dimension is not valid JSON: %1.").arg(err));
            continue;
        }
        found.insert(id, qMakePair(v, r->pack));
    }

    // overworld, nether, end, then the others by name
    QStringList order = found.keys();
    auto rank = [](const QString& id) {
        if (id == QLatin1String("minecraft:overworld")) return 0;
        if (id == QLatin1String("minecraft:the_nether")) return 1;
        if (id == QLatin1String("minecraft:the_end")) return 2;
        return 3;
    };
    std::stable_sort(order.begin(), order.end(), [&](const QString& a, const QString& b) {
        return rank(a) < rank(b);
    });
    for (const QString& id : order)
        loadDimension(id, found[id].first, found[id].second);
    for (DimensionInfo& info : dims)
    {
        const Resource *r = res.find(REG_DIMENSION, info.id);
        info.packFile = r && r->pack > 0;   // packs[0] is the vanilla data
    }

    // structures need the dimensions' biomes; their problems are warnings
    if (report->ok())
        loadStructures(*this);

    if (!report->ok())
    {
        for (DimensionInfo& info : dims)
        {
            if (info.supported)
                info.reason = QStringLiteral("The packs have errors.");
            info.supported = false;
        }
    }
}

World::World() : p(new WorldPrivate)
{
}

World::~World()
{
    delete p;
}

std::shared_ptr<const World> World::load(const Request& request, Report *report)
{
    Report local;
    if (!report)
        report = &local;
    std::shared_ptr<World> w(new World);
    w->p->request = request;
    w->p->report = report;
    w->p->load();
    w->p->report = nullptr;
    // the files are not needed any more (the graph does not compile further)
    w->p->res.files.clear();
    w->p->res.tags.clear();
    w->p->res.colorFiles.clear();
    return w;
}

const Request& World::request() const { return p->request; }
const std::vector<PackInfo>& World::packs() const { return p->packs; }
const std::vector<BiomeInfo>& World::biomes() const { return p->biomes; }
const std::vector<DimensionInfo>& World::dimensions() const { return p->dims; }
QStringList World::presets() const { return p->presets; }
const std::vector<StructureSetInfo>& World::structureSets() const { return p->structures.setInfo; }
const std::vector<StructureInfo>& World::structures() const { return p->structures.info; }

int World::dimension(const QString& id) const
{
    QString n = normalizeId(id);
    for (size_t i = 0; i < p->dims.size(); i++)
        if (p->dims[i].id == n)
            return (int) i;
    return -1;
}

//==============================================================================
// sampler
//==============================================================================

// Chunk order (CURSOR_CHUNK): what is known about a cell of the chunk that is
// being worked on.
struct ChunkCell
{
    enum
    {
        INFO = 1,       // nearest, dist, TIE and RIVAL are set
        TIE = 2,        // more than one entry is nearest
        RIVAL = 4,      // entries of different biomes are nearest
        STATE = 8,      // state is set
        EXACT = 16,     // the cell's chain is followed exactly (see chunkState())
        SHORT = 32,     // the cell's chain starts at its block (see chunkState())
        SECTION = 64,   // fromSection is set
    };
    uint32_t gen = 0;       // valid if it is the sampler's chunk generation
    uint8_t flags = 0;
    int32_t nearest = -1;   // an entry nearest to the cell's climate
    int32_t state = -1;     // the entry the game's search ends on here
    int32_t fromSection = -1;   // the same, for a chain from the cell's block
    int64_t dist = 0;       // distance of the nearest entries
    int64_t np[6];          // the quantised climate
};

class SamplerPrivate
{
public:
    std::shared_ptr<const World> world;
    const DimData *dim = nullptr;
    int mc = 0;
    std::unique_ptr<DensityState> state;
    std::unique_ptr<TerrainState> terrain;      // created on first use
    std::unique_ptr<SurfaceRuleState> surface;  // created on first use
    // biome cells for the biomes of single blocks (direct mapped; a cell's
    // biome is a function of its position)
    struct CellEntry { int qx, qy, qz, biome; bool used; };
    std::vector<CellEntry> cellCache;
    int blockBiome(Sampler *s, int x, int y, int z);
    // the End of 1.18.2: its biome source samples the island noise itself
    bool endIslandBiomes = false;
    EndNoise endNoise;
    uint64_t seed = 0;
    uint64_t sha = 0;
    int policy = CURSOR_CHUNK;
    // the "last result" of the parameter search (CURSOR_CARRY)
    int cursor = -1;
    uint64_t nativeCursor = 0;
    // a leaf of an earlier lookup, which only speeds up searches without history
    int hint = -1;
    std::vector<int> buf;

    // chunk order: the chunk that is being worked on
    uint32_t chunkGen = 0;
    int chunkX = 0, chunkZ = 0;
    int baseCell = 0;                   // y of the cells of order 0
    int curColumn = -1;                 // column whose values the density state holds
    std::vector<ChunkCell> cells;       // by position in the game's order
    std::vector<double> columns;        // saved column values, 16 columns
    uint32_t columnGen[16] = {};

    bool usesClimate() const
    {
        return dim->source == DimData::MULTI_NOISE || dim->source == DimData::NATIVE_OVERWORLD;
    }
    // chunk order needs to know which entries are nearest to a climate
    bool canChunkOrder() const
    {
        return dim->source == DimData::MULTI_NOISE ||
            (dim->source == DimData::NATIVE_OVERWORLD && !dim->nativeTree.empty());
    }
    void resetCursor()
    {
        cursor = -1;
        nativeCursor = 0;
    }
    inline void climate(int64_t np[6]) const;
    inline int searchCarry(const int64_t np[6]);
    inline int searchPlain(const int64_t np[6]);
    bool fill(int *out, int x0, int z0, int sx, int sz, int step, int mid, int qy0, int sy,
        bool carry, const std::atomic_bool *abort);

    void initChunkOrder();
    void beginChunk(int cx, int cz);
    inline int order(int l, int qy, int n) const
    {
        return ((((qy - baseCell) >> 2) * 4 + l) * 4 + ((qy - baseCell) & 3)) * 4 + n;
    }
    ChunkCell& cellClimate(int ord);
    ChunkCell& chunkCell(int ord);
    int chunkState(int ord);
    inline int entryBiome(int entry, const int64_t np[6]);
    inline int chunkBiome(int ord);
    bool fillChunkOrder(int *out, int x0, int z0, int sx, int sz, int qy0, int sy,
        const std::atomic_bool *abort);
    int cellChunkOrder(int qx, int qy, int qz);
};

// The quantised climate that the density state holds
inline void SamplerPrivate::climate(int64_t np[6]) const
{
    for (int k = 0; k < 6; k++)
        np[k] = quantizeClimate((float) state->value(dim->outputs[k]));
}

// The game's search: starts from the last result, which it updates
inline int SamplerPrivate::searchCarry(const int64_t np[6])
{
    const DimData& d = *dim;
    if (d.source == DimData::NATIVE_OVERWORLD)
    {
        int id = climateToBiome(mc, (const uint64_t*) np, &nativeCursor);
        return (unsigned) id < 256u ? d.nativeBiome[id] : -1;
    }
    int e = d.tree.find(np, &cursor);
    return e >= 0 ? d.entryBiome[e] : -1;
}

// A search without history that leaves the last result alone
inline int SamplerPrivate::searchPlain(const int64_t np[6])
{
    const DimData& d = *dim;
    if (d.source == DimData::NATIVE_OVERWORLD)
    {
        int id = climateToBiome(mc, (const uint64_t*) np, nullptr);
        return (unsigned) id < 256u ? d.nativeBiome[id] : -1;
    }
    int e = d.tree.findFresh(np, &hint);
    return e >= 0 ? d.entryBiome[e] : -1;
}

// Fills @out (sx * sz * sy, index [k][j][i]) with the biomes of the cells
// ((x0 + i) * step + mid, qy0 + k, (z0 + j) * step + mid) in 1:4 coordinates.
// @carry: one cursor through all samples, else each one without history.
bool SamplerPrivate::fill(int *out, int x0, int z0, int sx, int sz, int step, int mid, int qy0, int sy,
    bool carry, const std::atomic_bool *abort)
{
    const DimData& d = *dim;
    const size_t plane = (size_t) sx * (size_t) sz;
    for (int i = 0; i < sx; i++)
    {
        if (abort && abort->load(std::memory_order_relaxed))
            return false;
        const int qx = (int) ((unsigned) (x0 + i) * (unsigned) step) + mid;
        for (int j = 0; j < sz; j++)
        {
            const int qz = (int) ((unsigned) (z0 + j) * (unsigned) step) + mid;
            int *o = out + (size_t) j * sx + i;
            if (d.source == DimData::FIXED)
            {
                for (int k = 0; k < sy; k++)
                    o[k * plane] = d.fixedBiome;
                continue;
            }
            if (d.source == DimData::THE_END)
            {
                // TheEndBiomeSource.getNoiseBiome(): by the section of the
                // cell's block position; the router's erosion in the middle
                // of the 2x2 sections around it, at the cell's y
                const int bx = (int) ((unsigned) qx << 2), bz = (int) ((unsigned) qz << 2);
                const int64_t sx = bx >> 4, sz = bz >> 4;
                if (sx * sx + sz * sz <= 4096)
                {
                    for (int k = 0; k < sy; k++)
                        o[k * plane] = d.endBiomes[0];
                    continue;
                }
                if (endIslandBiomes)
                {
                    // 1.18.2: TheEndBiomeSource.getHeightValue() of its own
                    // island noise at (sx * 2 + 1, sz * 2 + 1), any y
                    const float f = getEndHeightNoise(&endNoise, (int) (sx * 2 + 1), (int) (sz * 2 + 1), 0);
                    const int b = d.endBiomes[f > 40.0F ? 1 : f >= 0.0F ? 2 : f < -20.0F ? 3 : 4];
                    for (int k = 0; k < sy; k++)
                        o[k * plane] = b;
                    continue;
                }
                state->evalColumn((int) ((sx * 2 + 1) * 8), (int) ((sz * 2 + 1) * 8));
                for (int k = 0; k < sy; k++)
                {
                    state->evalVertical((int) ((unsigned) (qy0 + k) << 2));
                    const double e = state->value(d.outputs[3]);
                    o[k * plane] = d.endBiomes[e > 0.25 ? 1 : e >= -0.0625 ? 2 : e < -0.21875 ? 3 : 4];
                }
                continue;
            }
            if (d.source == DimData::CHECKERBOARD)
            {
                // Math.floorMod((x >> shift) + (z >> shift), count) on
                // Java ints: the shift count is taken mod 32, the sum wraps
                const int sh = d.checkerShift & 31;
                const int32_t v = (int32_t) ((uint32_t) ((int32_t) qx >> sh) + (uint32_t) ((int32_t) qz >> sh));
                const int32_t n = (int32_t) d.checker.size();
                int b = d.checker[(size_t) (((v % n) + n) % n)];
                for (int k = 0; k < sy; k++)
                    o[k * plane] = b;
                continue;
            }
            // the climate is sampled at the block position of the cell
            state->evalColumn((int) ((unsigned) qx << 2), (int) ((unsigned) qz << 2));
            for (int k = 0; k < sy; k++)
            {
                state->evalVertical((int) ((unsigned) (qy0 + k) << 2));
                int64_t np[6];
                climate(np);
                o[k * plane] = carry ? searchCarry(np) : searchPlain(np);
            }
        }
    }
    return true;
}

//------------------------------------------------------------------------------
// Chunk order (CURSOR_CHUNK): the biome of a cell as the game finds it when it
// fills the cell's chunk.
//
// The game searches the cells of a chunk in a fixed order: the sections
// bottom up, in a section x, then y, then z. Every search starts from the
// entry the search before ended on (the "last result") and only leaves it
// for a strictly nearer entry. So the entry of a cell is the last result if
// that is one of the entries nearest to the cell's climate, and otherwise the
// first nearest entry in the search order of the tree.
//
// This is computed exactly, but only as far back as it matters:
//  - A cell whose nearest entries are all of one biome has that biome,
//    whatever was searched before.
//  - Otherwise the last result is needed. It is carried forward, like the
//    game does, from the nearest cell before it (in the game's order) whose
//    entry is known: one with a single nearest entry (its search ends there
//    whatever came before), one this chunk has already worked out, or the
//    first cell of the chunk.
// This is exact as long as the chain is short. Where entries of different
// biomes are equally near over whole regions (packs with overlapping boxes),
// it could reach down to the bottom of the chunk, hundreds of searches for
// one cell (that made such packs 16 times slower at 1:4 and 45 times slower
// above). So if all of the CHAIN_LIMIT cells before a cell tie, its chain
// starts without history at the first cell of its block of SHORT_BLOCK cells
// (the cells of one x in a section) instead. What a thread of the game
// searched before a chunk (its previous chunk) cannot be known anyway. Which
// of the two a cell gets depends only on the climates below it, so the
// result is a function of the cell's position alone. Against whole chunks in
// the game's order, vanilla does not differ, the test packs of
// tools/worldgen-reference in at most 21 of 2.3 million cells, and a synthetic
// pack with broadly overlapping entries in about 5 % of its cells.
//
// The cost is one search per cell where entries do not tie, and at most
// about CHAIN_LIMIT + SHORT_BLOCK cells where they do, shared by the cells of
// a chunk in one request.
//------------------------------------------------------------------------------

enum
{
    CHAIN_LIMIT = 16,   // the longest chain of ties that is followed exactly
    SHORT_BLOCK = 16,   // where other chains start: the cells of one x in a section
};

void SamplerPrivate::initChunkOrder()
{
    baseCell = dim->minCellY & ~3;
    const int sections = ((dim->maxCellY - baseCell) >> 2) + 1;
    cells.assign((size_t) sections * 64, ChunkCell());
    columns.assign(16 * state->columnSize() + 1, 0.0);
    chunkGen = 0;
}

void SamplerPrivate::beginChunk(int cx, int cz)
{
    if (++chunkGen == 0)
    {
        // the generation counter wrapped: forget everything
        for (ChunkCell& c : cells)
            c.gen = 0;
        for (uint32_t& g : columnGen)
            g = 0;
        chunkGen = 1;
    }
    chunkX = cx;
    chunkZ = cz;
    curColumn = -1;
}

// The climate of a cell of the current chunk
ChunkCell& SamplerPrivate::cellClimate(int ord)
{
    ChunkCell& c = cells[ord];
    if (c.gen == chunkGen)
        return c;
    const int n = ord & 3, m = (ord >> 2) & 3, l = (ord >> 4) & 3, sec = ord >> 6;
    const int col = l * 4 + n;
    const size_t ncol = state->columnSize();
    if (columnGen[col] != chunkGen)
    {
        state->evalColumn((int) ((unsigned) (chunkX * 4 + l) << 2), (int) ((unsigned) (chunkZ * 4 + n) << 2));
        state->saveColumn(&columns[col * ncol]);
        columnGen[col] = chunkGen;
        curColumn = col;
    }
    else if (curColumn != col)
    {
        state->loadColumn(&columns[col * ncol], (int) ((unsigned) (chunkX * 4 + l) << 2),
            (int) ((unsigned) (chunkZ * 4 + n) << 2));
        curColumn = col;
    }
    state->evalVertical((int) ((unsigned) (baseCell + sec * 4 + m) << 2));
    climate(c.np);
    c.gen = chunkGen;
    c.flags = 0;
    return c;
}

// The same, with the entries nearest to it
ChunkCell& SamplerPrivate::chunkCell(int ord)
{
    ChunkCell& c = cellClimate(ord);
    if (c.flags & ChunkCell::INFO)
        return c;
    const DimData& d = *dim;
    ClimateTree::Nearest r = d.source == DimData::MULTI_NOISE
        ? d.tree.findNearest(c.np, &hint)
        : d.nativeTree.findNearest(c.np, &hint);
    c.nearest = r.entry;
    c.dist = r.dist;
    c.flags |= (uint8_t) (ChunkCell::INFO | (r.tie ? ChunkCell::TIE : 0) | (r.rival ? ChunkCell::RIVAL : 0));
    if (!r.tie)
    {
        // a single nearest entry: the game's search ends on it
        c.state = r.entry;
        c.flags |= ChunkCell::STATE;
    }
    return c;
}

// The entry the game's search ends on in the cell @ord of the current chunk
int SamplerPrivate::chunkState(int ord)
{
    ChunkCell& cell = chunkCell(ord);
    if (!(cell.flags & (ChunkCell::EXACT | ChunkCell::SHORT)))
    {
        // exact if one of the last CHAIN_LIMIT cells has a single nearest
        // entry, or the chain reaches the first cell of the chunk
        bool exact = false;
        for (int cur = ord; cur >= 0 && ord - cur <= CHAIN_LIMIT; cur--)
        {
            if (cur == 0 || !(chunkCell(cur).flags & ChunkCell::TIE))
            {
                exact = true;
                break;
            }
        }
        cell.flags |= exact ? ChunkCell::EXACT : ChunkCell::SHORT;
    }

    const bool exact = cell.flags & ChunkCell::EXACT;
    if (exact && (cell.flags & ChunkCell::STATE))
        return cell.state;
    if (!exact && (cell.flags & ChunkCell::SECTION))
        return cell.fromSection;

    // Back to a cell whose entry is known: any cell this chunk has worked out
    // exactly, for a short chain only one found the same way, down to the
    // first cell of the chunk or of the block, which starts without history.
    const int stop = exact ? 0 : ord & ~(SHORT_BLOCK - 1);
    int last = -1, first = stop;
    for (int cur = ord; ; cur--)
    {
        const ChunkCell& c = cells[cur];
        if (c.gen == chunkGen && (c.flags & (exact ? ChunkCell::STATE : ChunkCell::SECTION)))
        {
            last = exact ? c.state : c.fromSection;
            first = cur + 1;
            break;
        }
        if (cur == stop)
            break;
    }

    // And forward again, carrying the last result like the game
    const DimData& d = *dim;
    const bool native = d.source == DimData::NATIVE_OVERWORLD;
    const ClimateTree& tree = native ? d.nativeTree : d.tree;
    for (int o = first; o <= ord; o++)
    {
        ChunkCell& c = chunkCell(o);
        if (!(c.flags & ChunkCell::TIE))
        {
            last = c.nearest;   // a single nearest entry: the search ends there
        }
        else if (last < 0 || tree.distance(last, c.np) != c.dist)
        {
            // the last result is not among the nearest: the first nearest
            // entry in the game's search order
            if (native)
            {
                uint64_t dat = 0;   // the engine's root node: no history
                climateToBiome(mc, (const uint64_t*) c.np, &dat);
                last = dat < d.nativeEntry.size() ? d.nativeEntry[dat] : -1;
                if (last < 0)
                    last = c.nearest;
            }
            else
            {
                int node = tree.nodeOf(c.nearest);
                last = tree.findFresh(c.np, &node);
            }
        }
        if (exact)
        {
            c.state = last;
            c.flags |= ChunkCell::STATE;
        }
        else
        {
            c.fromSection = last;
            c.flags |= ChunkCell::SECTION;
        }
    }
    return last;
}

// The biome of an entry that is nearest to the climate @np
inline int SamplerPrivate::entryBiome(int entry, const int64_t np[6])
{
    const DimData& d = *dim;
    if (entry < 0)
        return -1;
    if (d.source == DimData::MULTI_NOISE)
        return d.entryBiome[entry];
    int g = d.nativeGroups[entry];
    if (g < 0)
    {
        // the engine decides the biome of this entry: a search that starts
        // on it stays on it
        uint64_t dat = (uint64_t) d.nativeNode[entry];
        g = climateToBiome(mc, (const uint64_t*) np, &dat);
    }
    return (unsigned) g < 256u ? d.nativeBiome[g] : -1;
}

inline int SamplerPrivate::chunkBiome(int ord)
{
    ChunkCell& c = chunkCell(ord);
    if (!(c.flags & ChunkCell::RIVAL))
        return entryBiome(c.nearest, c.np);
    return entryBiome(chunkState(ord), cells[ord].np);
}

// The cells (x0 + i, qy0 + k, z0 + j), chunk by chunk
bool SamplerPrivate::fillChunkOrder(int *out, int x0, int z0, int sx, int sz, int qy0, int sy,
    const std::atomic_bool *abort)
{
    const size_t plane = (size_t) sx * (size_t) sz;
    const int x1 = x0 + sx - 1, z1 = z0 + sz - 1;
    for (int cz = z0 >> 2; cz <= z1 >> 2; cz++)
    {
        if (abort && abort->load(std::memory_order_relaxed))
            return false;
        for (int cx = x0 >> 2; cx <= x1 >> 2; cx++)
        {
            beginChunk(cx, cz);
            const int l0 = std::max(x0, cx * 4) - cx * 4, l1 = std::min(x1, cx * 4 + 3) - cx * 4;
            const int n0 = std::max(z0, cz * 4) - cz * 4, n1 = std::min(z1, cz * 4 + 3) - cz * 4;
            for (int l = l0; l <= l1; l++)
            {
                for (int n = n0; n <= n1; n++)
                {
                    int *o = out + (size_t) (cz * 4 + n - z0) * sx + (cx * 4 + l - x0);
                    for (int k = 0; k < sy; k++)
                        o[k * plane] = chunkBiome(order(l, qy0 + k, n));
                }
            }
        }
    }
    return true;
}

// The biome of the single cell (qx, qy, qz), the same as fillChunkOrder()
// gives it: for requests that are not a dense block of cells (scales above
// 1:4).
int SamplerPrivate::cellChunkOrder(int qx, int qy, int qz)
{
    beginChunk(qx >> 2, qz >> 2);
    return chunkBiome(order(qx & 3, qy, qz & 3));
}

// Sampler::biomeAt(1, x, y, z) from cached cells: the game's 1:1 zoom picks
// one of the cells around the block
int SamplerPrivate::blockBiome(Sampler *s, int x, int y, int z)
{
    const bool chunkOrder = policy == CURSOR_CHUNK && usesClimate() && canChunkOrder();
    if (!chunkOrder && usesClimate())
        return s->biomeAt(1, x, y, z);  // (the cursor carries over: in order)
    int x4, y4, z4;
    voronoiAccess3D(sha, x, y, z, &x4, &y4, &z4);
    y4 = std::min(std::max(y4, dim->minCellY), dim->maxCellY);
    if (cellCache.empty())
        cellCache.assign(1u << 12, CellEntry { 0, 0, 0, -1, false });
    CellEntry& e = cellCache[(size_t) (((uint32_t) x4 * 0x9E3779B1u ^ (uint32_t) z4 * 0x85EBCA77u ^
        (uint32_t) y4 * 0xC2B2AE35u) >> 20)];
    if (e.used && e.qx == x4 && e.qy == y4 && e.qz == z4)
        return e.biome;
    int b = -1;
    if (chunkOrder)
        b = cellChunkOrder(x4, y4, z4);
    else if (!s->genBiomes(&b, 4, x4, z4, 1, 1, y4 << 2))
        return -1;
    e = CellEntry { x4, y4, z4, b, true };
    return b;
}

Sampler::Sampler(std::shared_ptr<const World> world, int dimension, uint64_t seed)
    : p(new SamplerPrivate)
{
    p->world = world;
    if (!world)
        return;
    const WorldPrivate *w = world->d();
    if (dimension < 0 || (size_t) dimension >= w->dims.size() || !w->dims[dimension].supported)
        return;
    p->dim = &w->data[dimension];
    p->mc = w->request.mc;
    p->policy = g_cursorPolicy;
    p->seed = seed;
    p->sha = getVoronoiSHA(seed);
    if (p->dim->source == DimData::THE_END)
    {
        p->state.reset(new DensityState(*w->graph, p->dim->program, seed, p->mc, p->dim->legacy));
        p->endIslandBiomes = p->mc <= MC_1_18_2;
        if (p->endIslandBiomes)
            setEndSeed(&p->endNoise, p->mc, seed);
    }
    if (p->usesClimate())
    {
        p->state.reset(new DensityState(*w->graph, p->dim->program, seed, p->mc, p->dim->legacy));
        if (p->canChunkOrder())
            p->initChunkOrder();
    }
}

Sampler::~Sampler()
{
    delete p;
}

bool Sampler::valid() const
{
    return p->dim != nullptr;
}

bool Sampler::genBiomes(int *out, int scale, int x, int z, int sx, int sz, int y,
    const std::atomic_bool *abort)
{
    // long scans inside the density functions check the flag too
    AbortScope scope(p->state.get(), abort);
    if (!p->dim || sx <= 0 || sz <= 0)
        return false;
    p->resetCursor();
    const bool chunkOrder = p->policy == CURSOR_CHUNK && p->usesClimate() && p->canChunkOrder();
    const bool carry = p->policy == CURSOR_CARRY;

    // A chunk only has biome cells within the height of its dimension;
    // above and below, the game takes the nearest one.
    const int ylo = p->dim->minCellY, yhi = p->dim->maxCellY;
    auto clampY = [ylo, yhi](int q) { return q < ylo ? ylo : q > yhi ? yhi : q; };

    if (scale == 1)
    {
        // The game's 1:1 zoom: every block takes the biome of one of the
        // eight cells around it. The cells that can be asked for:
        Range r = { 1, x, z, sx, sz, y, 1 };
        Range s = getVoronoiSrcRange(r);
        const int y0 = clampY(s.y), y1 = clampY(s.y + s.sy - 1);
        const int ny = y1 - y0 + 1;
        p->buf.resize((size_t) s.sx * ny * s.sz);
        bool ok = chunkOrder
            ? p->fillChunkOrder(p->buf.data(), s.x, s.z, s.sx, s.sz, y0, ny, abort)
            : p->fill(p->buf.data(), s.x, s.z, s.sx, s.sz, 1, 0, y0, ny, carry, abort);
        if (!ok)
            return false;
        const int *src = p->buf.data();
        for (int j = 0; j < sz; j++)
        {
            for (int i = 0; i < sx; i++)
            {
                int x4, y4, z4;
                voronoiAccess3D(p->sha, x + i, y, z + j, &x4, &y4, &z4);
                x4 -= s.x; z4 -= s.z;
                y4 = clampY(y4) - y0;
                out[(size_t) j * sx + i] = src[(size_t) y4 * s.sx * s.sz + (size_t) z4 * s.sx + x4];
            }
        }
        return !(abort && abort->load(std::memory_order_relaxed));
    }
    if (scale < 4 || (scale & 3))
        return false;
    // The biome cell that holds the height y. At 1:4 these are the game's
    // cells; above that one cell per pixel, the one in the middle.
    const int qy = clampY(y >> 2);
    if (scale == 4)
    {
        const bool ok = chunkOrder
            ? p->fillChunkOrder(out, x, z, sx, sz, qy, 1, abort)
            : p->fill(out, x, z, sx, sz, 1, 0, qy, 1, carry, abort);
        return ok && !(abort && abort->load(std::memory_order_relaxed));
    }
    const int step = scale / 4;
    if (chunkOrder)
    {
        // The same biome that the cell has at 1:4, so that all scales show
        // one world.
        const int mid = step / 2;
        for (int j = 0; j < sz; j++)
        {
            if (abort && abort->load(std::memory_order_relaxed))
                return false;
            const int qz = (int) ((unsigned) (z + j) * (unsigned) step) + mid;
            for (int i = 0; i < sx; i++)
            {
                const int qx = (int) ((unsigned) (x + i) * (unsigned) step) + mid;
                out[(size_t) j * sx + i] = p->cellChunkOrder(qx, qy, qz);
            }
        }
        return !(abort && abort->load(std::memory_order_relaxed));
    }
    return p->fill(out, x, z, sx, sz, step, step / 2, qy, 1, carry, abort) &&
        !(abort && abort->load(std::memory_order_relaxed));
}

int Sampler::biomeAt(int scale, int x, int y, int z)
{
    int b = -1;
    if (!genBiomes(&b, scale, x, z, 1, 1, y))
        return -1;
    return b;
}

bool sampleCellsCarry(Sampler& sampler, const int *qx, const int *qz, int n, int qy, int *out)
{
    SamplerPrivate *p = sampler.d();
    if (!p->dim)
        return false;
    p->resetCursor();
    bool ok = true;
    for (int i = 0; i < n && ok; i++)
        ok = p->fill(out + i, qx[i], qz[i], 1, 1, 1, 0, qy, 1, true, nullptr);
    // the density state now holds another column than chunk order thinks
    p->curColumn = -1;
    return ok;
}

bool Sampler::hasTerrain() const
{
    return p->dim && p->dim->terrain.ok;
}

// The terrain state of a sampler, created on first use
static TerrainState *terrainOf(SamplerPrivate *p)
{
    if (!p->dim || !p->dim->terrain.ok)
        return nullptr;
    if (!p->terrain)
        p->terrain.reset(new TerrainState(*p->world->d()->graph, p->dim->terrain, p->seed, p->mc));
    return p->terrain.get();
}

void setBaseColumns(Sampler& sampler, bool base)
{
    if (TerrainState *t = terrainOf(sampler.d()))
        t->setBaseColumns(base);
}

int blockAt(Sampler& sampler, int x, int y, int z)
{
    TerrainState *t = terrainOf(sampler.d());
    return t ? t->block(x, y, z) : 0;
}

bool hasAquifers(Sampler& sampler)
{
    TerrainState *t = terrainOf(sampler.d());
    return t && t->hasAquifers();
}

void samplerStates(Sampler& sampler, DensityState **climate, DensityState **terrain)
{
    SamplerPrivate *p = sampler.d();
    *climate = p->state.get();
    TerrainState *t = terrainOf(p);
    *terrain = t ? &t->density() : nullptr;
}

int Sampler::surfaceHeight(int x, int z, HeightKind kind, int top, HeightDetail detail)
{
    int h = NO_HEIGHT;
    if (!genHeights(&h, 1, x, z, 1, 1, kind, top, nullptr, detail))
        return NO_HEIGHT;
    return h;
}

// Calls f(index, block x, block z) for the columns of the area x .. x+sx-1,
// z .. z+sz-1 in units of @scale blocks, in an order that lets the columns
// of a noise cell share its corners. Returns false if it was aborted.
template <class F>
static bool forColumns(TerrainState *t, int cw, int scale, int x, int z, int sx, int sz,
    const std::atomic_bool *abort, F f)
{
    t->setCoarse(scale >= cw || (sx == 1 && sz == 1));
    t->setDenseColumns(scale < 16 && (sx > 1 || sz > 1));
    if (scale >= cw)
    {
        // one block column per cell; at the scale of the cells the cells of
        // neighbouring columns share corners (keep a row of them)
        t->reserve(scale == cw ? sx + 2 : 4);
        for (int j = 0; j < sz; j++)
        {
            if (abort && abort->load(std::memory_order_relaxed))
                return false;
            const int bz = (int) ((unsigned) (z + j) * (unsigned) scale);
            for (int i = 0; i < sx; i++)
                f((size_t) j * sx + i, (int) ((unsigned) (x + i) * (unsigned) scale), bz);
        }
        return !(abort && abort->load(std::memory_order_relaxed));
    }
    // Cell by cell, so that the columns of a cell share its corners: the
    // cells of a row of cells, then the next row.
    const int x0 = x * scale, z0 = z * scale;
    const int cx0 = (x0 >= 0 ? x0 / cw : -((-x0 + cw - 1) / cw));
    const int cz0 = (z0 >= 0 ? z0 / cw : -((-z0 + cw - 1) / cw));
    const int x1 = (x + sx - 1) * scale, z1 = (z + sz - 1) * scale;
    const int cx1 = (x1 >= 0 ? x1 / cw : -((-x1 + cw - 1) / cw));
    const int cz1 = (z1 >= 0 ? z1 / cw : -((-z1 + cw - 1) / cw));
    t->reserve(cx1 - cx0 + 2);
    for (int cz = cz0; cz <= cz1; cz++)
    {
        if (abort && abort->load(std::memory_order_relaxed))
            return false;
        for (int cx = cx0; cx <= cx1; cx++)
        {
            // the requested columns in this cell: (x + i) * scale, (z + j) * scale
            const int i0 = (std::max(cx * cw, x0) - x0 + scale - 1) / scale;
            const int j0 = (std::max(cz * cw, z0) - z0 + scale - 1) / scale;
            for (int j = j0; j < sz && z0 + j * scale <= cz * cw + cw - 1; j++)
                for (int i = i0; i < sx && x0 + i * scale <= cx * cw + cw - 1; i++)
                    f((size_t) j * sx + i, x0 + i * scale, z0 + j * scale);
        }
    }
    return !(abort && abort->load(std::memory_order_relaxed));
}

// What the surface rules ask of a sampler's world
class SamplerSurfaceAccess : public SurfaceAccess
{
public:
    SamplerSurfaceAccess(Sampler *s, TerrainState *t, int top) : s(s), t(t), top(top) {}
    int biome(int x, int y, int z) override { return s->d()->blockBiome(s, x, y, z); }
    int block(int x, int y, int z) override { return t->blockKind(x, y, z); }
    int surfaceBefore(int x, int z) override
    {
        if (x == sx && z == sz)
            return sv;
        int f, v;
        t->bothHeights(x, z, top, &f, &v);
        sx = x;
        sz = z;
        sv = v;
        return v;
    }
    int preliminary(int x, int z) override
    {
        AquiferState *a = t->aquifer();
        return a ? a->preliminarySurface(x, z) : 0;
    }
    bool aborted() override
    {
        const std::atomic_bool *f = t->density().abort;
        return f && f->load(std::memory_order_relaxed);
    }
private:
    Sampler *s;
    TerrainState *t;
    int top;
    int sx = 0x7fffffff, sz = 0, sv = 0;
};

// The rules of the sampler's dimension, if they can change heights below @top
static SurfaceRuleState *surfaceRulesOf(SamplerPrivate *p, TerrainState *t, int top)
{
    const SurfaceRuleDef& def = p->dim->surface;
    if (!def.on || def.noEffect || t->capped(top))
        return nullptr;
    if (!p->surface)
        p->surface.reset(new SurfaceRuleState(*p->world->d()->graph, def, p->seed, p->mc));
    return p->surface.get();
}

// The detail of the heights for one request
struct DetailScope
{
    TerrainState *t;
    DetailScope(TerrainState *t, HeightDetail d) : t(t) { t->setFast(d == HEIGHTS_FAST); }
    ~DetailScope() { t->setFast(false); }
};

bool Sampler::genHeights(int *out, int scale, int x, int z, int sx, int sz,
    HeightKind kind, int top, const std::atomic_bool *abort, HeightDetail detail)
{
    TerrainState *t = terrainOf(p);
    if (!t || scale < 1 || sx <= 0 || sz <= 0)
        return false;
    // long scans inside the density functions check the flag too
    AbortScope scope(&t->density(), abort);
    DetailScope ds(t, detail);
    const bool floor = kind == HEIGHT_OCEAN_FLOOR;
    SurfaceRuleState *rules = detail == HEIGHTS_SURFACE ? surfaceRulesOf(p, t, top) : nullptr;
    if (rules)
    {
        SamplerSurfaceAccess access(this, t, top);
        return forColumns(t, p->dim->terrain.cellWidth, scale, x, z, sx, sz, abort,
            [&](size_t i, int bx, int bz) {
                int f, s;
                t->bothHeights(bx, bz, top, &f, &s);
                rules->apply(access, bx, bz, t->lastWater(), t->lastFloorDefault(), &f, &s);
                out[i] = floor ? f : s;
            });
    }
    return forColumns(t, p->dim->terrain.cellWidth, scale, x, z, sx, sz, abort,
        [&](size_t i, int bx, int bz) { out[i] = t->height(bx, bz, floor, top); });
}

bool Sampler::genBothHeights(int *floor, int *surface, int scale, int x, int z, int sx, int sz,
    int top, const std::atomic_bool *abort, HeightDetail detail)
{
    TerrainState *t = terrainOf(p);
    SurfaceRuleState *rules = t && detail == HEIGHTS_SURFACE ? surfaceRulesOf(p, t, top) : nullptr;
    if (t && rules)
    {
        if (scale < 1 || sx <= 0 || sz <= 0)
            return false;
        AbortScope scope(&t->density(), abort);
        SamplerSurfaceAccess access(this, t, top);
        return forColumns(t, p->dim->terrain.cellWidth, scale, x, z, sx, sz, abort,
            [&](size_t i, int bx, int bz) {
                t->bothHeights(bx, bz, top, &floor[i], &surface[i]);
                rules->apply(access, bx, bz, t->lastWater(), t->lastFloorDefault(), &floor[i], &surface[i]);
            });
    }
    if (t && t->hasAquifers() && detail != HEIGHTS_FAST)
    {
        // one search of each column for both
        if (scale < 1 || sx <= 0 || sz <= 0)
            return false;
        AbortScope scope(&t->density(), abort);
        return forColumns(t, p->dim->terrain.cellWidth, scale, x, z, sx, sz, abort,
            [&](size_t i, int bx, int bz) { t->bothHeights(bx, bz, top, &floor[i], &surface[i]); });
    }
    if (!genHeights(floor, scale, x, z, sx, sz, HEIGHT_OCEAN_FLOOR, top, abort, detail))
        return false;
    int fluidTop = NO_HEIGHT;
    bool fromFloor;
    {
        DetailScope ds(t, detail);
        fromFloor = t->surfaceFromFloor(top, &fluidTop);
    }
    if (!fromFloor)
        return genHeights(surface, scale, x, z, sx, sz, HEIGHT_SURFACE, top, abort, detail);
    // The surface search goes down from the same top as the floor's and
    // stops above the highest fluid block: it finds the floor if that is
    // above, else the fluid (or nothing, like the floor).
    const size_t n = (size_t) sx * (size_t) sz;
    for (size_t i = 0; i < n; i++)
        surface[i] = fluidTop == NO_HEIGHT || (floor[i] != NO_HEIGHT && floor[i] > fluidTop) ? floor[i] : fluidTop;
    return true;
}

bool Sampler::solidAt(int x, int y, int z)
{
    TerrainState *t = terrainOf(p);
    return t && t->solid(x, y, z);
}

} // namespace dp
