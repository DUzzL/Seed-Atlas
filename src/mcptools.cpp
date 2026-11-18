#include "mcptools.h"

#include "mcpserver.h"
#include "datapackworld.h"
#include "dpmap.h"
#include "dpsearch.h"
#include "tabbiomes.h"
#include "tabportals.h"
#include "tabstructures.h"
#include "world.h"

#include "seedatlas-engine/finders.h"
#include "seedatlas-engine/util.h"

#include <QJsonDocument>
#include <QMutexLocker>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <thread>

using namespace mcp;

// ============================================================ helpers

namespace mcp {

static QJsonValue argValue(const QJsonObject& args, const char *key)
{
    return args.value(QLatin1String(key));
}

static bool present(const QJsonObject& args, const char *key)
{
    const QJsonValue v = argValue(args, key);
    return !v.isUndefined() && !v.isNull();
}

int64_t argIntReq(const QJsonObject& args, const char *key, int64_t lo, int64_t hi)
{
    if (!present(args, key))
        throw McpToolError{QString("Missing argument \"%1\".").arg(key)};
    return argInt(args, key, lo, lo, hi);
}

int64_t argInt(const QJsonObject& args, const char *key, int64_t def, int64_t lo, int64_t hi)
{
    const QJsonValue v = argValue(args, key);
    if (v.isUndefined() || v.isNull())
        return def;
    double d;
    if (v.isDouble())
        d = v.toDouble();
    else if (v.isString())
    {
        bool ok = false;
        d = double(v.toString().trimmed().toLongLong(&ok));
        if (!ok)
            throw McpToolError{QString("\"%1\" must be an integer.").arg(key)};
    }
    else
        throw McpToolError{QString("\"%1\" must be an integer.").arg(key)};
    if (!std::isfinite(d) || d != std::floor(d))
        throw McpToolError{QString("\"%1\" must be an integer.").arg(key)};
    if (d < double(lo) || d > double(hi))
        throw McpToolError{QString("\"%1\" must be between %2 and %3.").arg(key).arg(lo).arg(hi)};
    return int64_t(d);
}

bool argBool(const QJsonObject& args, const char *key, bool def)
{
    const QJsonValue v = argValue(args, key);
    if (v.isUndefined() || v.isNull())
        return def;
    if (!v.isBool())
        throw McpToolError{QString("\"%1\" must be true or false.").arg(key)};
    return v.toBool();
}

QString argString(const QJsonObject& args, const char *key, const QString& def)
{
    const QJsonValue v = argValue(args, key);
    if (v.isUndefined() || v.isNull())
        return def;
    if (!v.isString())
        throw McpToolError{QString("\"%1\" must be a string.").arg(key)};
    const QString s = v.toString();
    if (s.size() > 4096)
        throw McpToolError{QString("\"%1\" is too long.").arg(key)};
    return s;
}

uint64_t parseSeed(const QJsonValue& v, const QString& what)
{
    if (v.isDouble())
    {
        const double d = v.toDouble();
        if (!std::isfinite(d) || d != std::floor(d))
            throw McpToolError{QString("%1 must be an integer.").arg(what)};
        if (std::fabs(d) > 9007199254740992.0)
            throw McpToolError{QString("%1 is too large for a JSON number: pass it as a "
                "decimal string.").arg(what)};
        return uint64_t(int64_t(d));
    }
    if (!v.isString())
        throw McpToolError{QString("%1 must be a decimal string (or an integer).").arg(what)};
    const QString s = v.toString().trimmed();
    if (s.isEmpty())
        throw McpToolError{QString("%1 is empty.").arg(what)};
    if (s.size() > 64)
        throw McpToolError{QString("%1 is too long.").arg(what)};
    bool ok = false;
    const qlonglong sv = s.toLongLong(&ok);
    if (ok)
        return uint64_t(sv);
    const qulonglong uv = s.toULongLong(&ok);
    if (ok)
        return uint64_t(uv);
    // A text seed, as Minecraft takes it: the Java hash code of the text.
    int32_t h = 0;
    for (QChar c : s)
        h = int32_t(uint32_t(h) * 31u + c.unicode());
    return uint64_t(int64_t(h));
}

QString seedText(uint64_t seed)
{
    return QString::number(int64_t(seed));
}

int vanillaBiomeId(int mc, const QString& name)
{
    QString n = name.trimmed().toLower();
    if (n.startsWith(QLatin1String("minecraft:")))
        n = n.mid(10);
    if (n.isEmpty())
        return -1;
    const QByteArray latin = n.toLatin1();
    for (int id = 0; id < 256; id++)
    {
        const char *s = biome2str(mc, id);
        if (s && latin == s)
            return id;
    }
    return -1;
}

QString biomeName(const World& w, int id)
{
    if (id < 0)
        return QString();
    if (w.datapack())
    {
        const std::vector<dp::BiomeInfo>& list = w.wi.dp->world->biomes();
        return id < (int) list.size() ? list[size_t(id)].id : QString();
    }
    const char *s = biome2str(w.wi.mc, id);
    return s ? QStringLiteral("minecraft:") + s : QString::number(id);
}

QJsonObject schemaObject(const QJsonObject& properties, const QStringList& required)
{
    QJsonObject o;
    o["type"] = "object";
    o["properties"] = properties;
    if (!required.isEmpty())
        o["required"] = QJsonArray::fromStringList(required);
    o["additionalProperties"] = false;
    return o;
}

static QJsonObject prop(const char *type, const QString& description)
{
    QJsonObject o;
    o["type"] = type;
    o["description"] = description;
    return o;
}

static QJsonObject seedProp(const QString& description)
{
    QJsonObject o;
    o["type"] = QJsonArray{ "string", "integer" };
    o["description"] = description;
    return o;
}

QJsonObject worldSchemaProperties(bool withSeed)
{
    QJsonObject p;
    if (withSeed)
        p["seed"] = seedProp("World seed as a decimal string (64-bit; an integer works for "
            "small seeds). Text that is not a number is hashed as Minecraft does.");
    p["mc"] = prop("string", "Minecraft version, e.g. \"1.21.1\", \"1.20\", \"26.3\" "
        "(default: the newest). Ignored with \"world\".");
    QJsonObject wt = prop("string", "World type (default \"default\"). Amplified needs 1.18+. "
        "Ignored with \"world\".");
    wt["enum"] = QJsonArray{ "default", "large_biomes", "amplified" };
    p["world_type"] = wt;
    p["dimension"] = prop("string", "\"overworld\" (default), \"nether\" or \"end\"; in a "
        "datapack world also any dimension id of its packs, e.g. \"mypack:skylands\".");
    p["world"] = prop("string", "A datapack world handle from load_datapacks (e.g. \"dp1\"). "
        "Without it the built-in generator is used.");
    return p;
}

QJsonObject result(const QJsonObject& data, const QString& summary)
{
    QJsonObject text;
    text["type"] = "text";
    QString body = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    if (!summary.isEmpty())
        body = summary + "\n" + body;
    text["text"] = body;
    QJsonObject r;
    r["content"] = QJsonArray{ text };
    r["structuredContent"] = data;
    r["isError"] = false;
    return r;
}

QJsonObject errorResult(const QString& message)
{
    QJsonObject text;
    text["type"] = "text";
    text["text"] = message;
    QJsonObject r;
    r["content"] = QJsonArray{ text };
    r["isError"] = true;
    return r;
}

// "overworld" -> engine dimension; returns DIM_UNDEF for other names
static int dimensionByName(const QString& name)
{
    QString n = name.trimmed().toLower();
    if (n.startsWith(QLatin1String("minecraft:")))
        n = n.mid(10);
    if (n == QLatin1String("overworld"))
        return DIM_OVERWORLD;
    if (n == QLatin1String("nether") || n == QLatin1String("the_nether"))
        return DIM_NETHER;
    if (n == QLatin1String("end") || n == QLatin1String("the_end"))
        return DIM_END;
    return DIM_UNDEF;
}

static QString dimensionId(int dim)
{
    switch (dim)
    {
    case DIM_NETHER:    return QStringLiteral("minecraft:the_nether");
    case DIM_END:       return QStringLiteral("minecraft:the_end");
    default:            return QStringLiteral("minecraft:overworld");
    }
}

static QString worldTypeId(int type)
{
    switch (type)
    {
    case WORLD_LARGE_BIOMES:    return QStringLiteral("large_biomes");
    case WORLD_AMPLIFIED:       return QStringLiteral("amplified");
    default:                    return QStringLiteral("default");
    }
}

static int parseMc(const QString& text)
{
    if (text.isEmpty())
        return MC_NEWEST;
    const QByteArray latin = text.trimmed().toLatin1();
    const int mc = str2mc(latin.constData());
    if (mc == MC_UNDEF || mc > MC_NEWEST)
        throw McpToolError{QString("Unknown Minecraft version \"%1\" (see server_info for the "
            "versions).").arg(text)};
    return mc;
}

static int parseWorldType(const QString& text, int mc)
{
    int type = WORLD_DEFAULT;
    const QString t = text.trimmed().toLower();
    if (t.isEmpty() || t == QLatin1String("default") || t == QLatin1String("normal"))
        type = WORLD_DEFAULT;
    else if (t == QLatin1String("large_biomes") || t == QLatin1String("large biomes"))
        type = WORLD_LARGE_BIOMES;
    else if (t == QLatin1String("amplified"))
        type = WORLD_AMPLIFIED;
    else
        throw McpToolError{QString("Unknown world type \"%1\": use default, large_biomes or "
            "amplified.").arg(text)};
    if (!worldTypeAvailable(type, mc))
        throw McpToolError{QString("The world type %1 does not exist in Minecraft %2.")
            .arg(worldTypeName(type), mc2str(mc))};
    return type;
}

// The DatapackWorld of a dimension of loaded packs (as the app's snapshots).
static std::shared_ptr<const DatapackWorld> datapackSnapshot(McpDatapacks& p, const QString& dimId)
{
    QMutexLocker lock(&p.mutex);
    auto it = p.snapshots.find(dimId);
    if (it != p.snapshots.end())
        return it.value();
    std::shared_ptr<DatapackWorld> w = std::make_shared<DatapackWorld>();
    w->mc = p.mc;
    w->preset = datapackPreset(p.worldType);
    w->dimensionId = dimId;
    w->world = p.world;
    w->report = *p.report;
    w->state = DatapackWorld::Problem;
    if (p.world && p.report->ok())
    {
        const int dim = p.world->dimension(dimId);
        if (dim >= 0 && p.world->dimensions()[size_t(dim)].supported)
        {
            w->state = DatapackWorld::Ready;
            w->dimension = dim;
            w->presetMatters = !p.world->dimensions()[size_t(dim)].packFile;
            const std::vector<dp::BiomeInfo>& biomes = p.world->biomes();
            for (const dp::BiomeInfo& biome : biomes)
            {
                w->colors.push_back(biome.color);
                w->labels.push_back(biome.name);
            }
        }
    }
    p.snapshots.insert(dimId, w);
    return w;
}

World parseWorld(McpRegistry *reg, const QJsonObject& args, bool needSeed)
{
    World w;
    w.wi.reset();
    const QString handle = argString(args, "world");
    const QString dimText = argString(args, "dimension");
    if (!handle.isEmpty())
    {
        w.packs = reg->findDatapacks(handle);
        if (!w.packs)
            throw McpToolError{QString("There is no datapack world \"%1\" (load_datapacks gives "
                "the handles; they last as long as the MCP session).").arg(handle)};
        w.wi.mc = w.packs->mc;
        w.wi.setWorldType(w.packs->worldType);
        QString id = dimText.trimmed();
        if (id.isEmpty())
            id = QStringLiteral("minecraft:overworld");
        else if (!id.contains(':'))
        {
            const int d = dimensionByName(id);
            id = d != DIM_UNDEF ? dimensionId(d) : QStringLiteral("minecraft:") + id;
        }
        else if (dimensionByName(id) != DIM_UNDEF)
            id = dimensionId(dimensionByName(id));
        const std::shared_ptr<const DatapackWorld> snap = datapackSnapshot(*w.packs, id);
        if (!snap->usable())
        {
            QString reason;
            const int dim = w.packs->world ? w.packs->world->dimension(id) : -1;
            if (!w.packs->report->ok())
                reason = "the datapacks have errors (see the report of load_datapacks)";
            else if (dim < 0)
                reason = "the world has no such dimension (datapack_info lists them)";
            else
                reason = w.packs->world->dimensions()[size_t(dim)].reason;
            throw McpToolError{QString("The dimension %1 of %2 cannot be computed: %3.")
                .arg(id, handle, reason)};
        }
        w.wi.dp = snap;
        w.dimensionId = id;
        w.dim = dpEngineDim(*snap);
    }
    else
    {
        w.wi.mc = parseMc(argString(args, "mc"));
        w.wi.setWorldType(parseWorldType(argString(args, "world_type"), w.wi.mc));
        w.dim = dimText.isEmpty() ? DIM_OVERWORLD : dimensionByName(dimText);
        if (w.dim == DIM_UNDEF)
            throw McpToolError{QString("Unknown dimension \"%1\": use overworld, nether or end "
                "(other dimensions need a datapack world).").arg(dimText)};
        if (w.dim == DIM_NETHER && w.wi.mc < MC_1_16_1)
            ; // (the engine knows the single nether biome of older versions)
        if (w.dim == DIM_END && w.wi.mc < MC_1_9)
            throw McpToolError{"The End has biomes from Minecraft 1.9 on."};
        w.dimensionId = dimensionId(w.dim);
    }
    if (needSeed)
    {
        if (!present(args, "seed"))
            throw McpToolError{"Missing argument \"seed\"."};
        w.wi.seed = parseSeed(argValue(args, "seed"), "seed");
    }
    // the default level of biome queries: the surface
    if (w.datapack())
        w.wi.y = w.wi.dp->maxY();
    else if (w.dim == DIM_OVERWORLD)
        w.wi.y = surfaceLevel(w.wi.amplified);
    else
        w.wi.y = 64;
    return w;
}

QJsonObject worldJson(const World& w)
{
    QJsonObject o;
    o["seed"] = seedText(w.wi.seed);
    o["mc"] = mc2str(w.wi.mc);
    o["world_type"] = worldTypeId(w.wi.worldType());
    o["dimension"] = w.dimensionId;
    if (w.packs)
        o["world"] = w.packs->id;
    return o;
}

} // namespace mcp

// The y of a call: "y" or the default of the world.
static int argY(const World& w, const QJsonObject& args)
{
    int lo = -64, hi = 320;
    if (w.datapack())
    {
        lo = w.wi.dp->minY();
        hi = w.wi.dp->maxY();
    }
    return int(argInt(args, "y", w.wi.y, lo, hi));
}

// Runs @work in the calling thread while a watcher reports progress and
// passes on a cancellation (through @stop).
static void runWatched(McpCall *call, std::atomic_bool *stop, const std::function<void()>& work,
    const std::function<void()>& tick)
{
    std::atomic_bool finished(false);
    std::thread watcher([&]() {
        while (!finished.load())
        {
            if (call->cancel.load())
                stop->store(true);
            if (tick)
                tick();
            for (int i = 0; i < 10 && !finished.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    work();
    finished = true;
    watcher.join();
}

static void checkCancel(McpCall *call)
{
    if (call->cancel.load())
        throw McpToolError{"Cancelled."};
}

struct Area
{
    int x1, z1, x2, z2;
};

// An area from x1, z1, x2, z2 (inclusive block bounds) or x, z, radius.
static Area parseArea(const QJsonObject& args, int64_t maxSide, int defRadius = -1)
{
    Area a;
    const bool rect = present(args, "x1") || present(args, "x2") || present(args, "z1")
        || present(args, "z2");
    if (rect)
    {
        a.x1 = int(argIntReq(args, "x1", -MAX_COORD, MAX_COORD));
        a.z1 = int(argIntReq(args, "z1", -MAX_COORD, MAX_COORD));
        a.x2 = int(argIntReq(args, "x2", -MAX_COORD, MAX_COORD));
        a.z2 = int(argIntReq(args, "z2", -MAX_COORD, MAX_COORD));
        if (a.x1 > a.x2) std::swap(a.x1, a.x2);
        if (a.z1 > a.z2) std::swap(a.z1, a.z2);
    }
    else
    {
        const int x = int(argInt(args, "x", 0, -MAX_COORD, MAX_COORD));
        const int z = int(argInt(args, "z", 0, -MAX_COORD, MAX_COORD));
        const int r = defRadius >= 0
            ? int(argInt(args, "radius", defRadius, 0, maxSide / 2))
            : int(argIntReq(args, "radius", 0, maxSide / 2));
        a = { x - r, z - r, x + r, z + r };
    }
    if (int64_t(a.x2) - a.x1 + 1 > maxSide || int64_t(a.z2) - a.z1 + 1 > maxSide)
        throw McpToolError{QString("The area is too large: at most %1 blocks a side.").arg(maxSide)};
    return a;
}

static QJsonObject areaProps(bool withRadius)
{
    QJsonObject p;
    p["x1"] = prop("integer", "West edge of the area (block x, inclusive).");
    p["z1"] = prop("integer", "North edge of the area (block z, inclusive).");
    p["x2"] = prop("integer", "East edge (inclusive).");
    p["z2"] = prop("integer", "South edge (inclusive).");
    if (withRadius)
    {
        p["x"] = prop("integer", "Centre x of a square area (instead of x1..z2; default 0).");
        p["z"] = prop("integer", "Centre z of a square area (default 0).");
        p["radius"] = prop("integer", "Half the side of the square area around x, z.");
    }
    return p;
}

// --- the built-in generator in one thread ---

struct Gen
{
    Generator g;
    Gen(const World& w)
    {
        setupGenerator(&g, w.wi.mc, w.wi.genFlags());
        applySeed(&g, w.dim, w.wi.seed);
    }
};

// ============================================================ structures

namespace {

struct StructType
{
    const char *id;     // the name in the tools (Minecraft's id where there is one)
    int opt;            // D_ map option
    const char *alias;
};

const StructType g_structTypes[] = {
    { "village",                D_VILLAGE,      nullptr },
    { "desert_pyramid",         D_DESERT,       "desert" },
    { "jungle_pyramid",         D_JUNGLE,       "jungle_temple" },
    { "swamp_hut",              D_HUT,          "hut" },
    { "igloo",                  D_IGLOO,        nullptr },
    { "monument",               D_MONUMENT,     "ocean_monument" },
    { "mansion",                D_MANSION,      "woodland_mansion" },
    { "pillager_outpost",       D_OUTPOST,      "outpost" },
    { "ocean_ruin",             D_OCEANRUIN,    "ruins" },
    { "shipwreck",              D_SHIPWRECK,    nullptr },
    { "buried_treasure",        D_TREASURE,     "treasure" },
    { "mineshaft",              D_MINESHAFT,    nullptr },
    { "desert_well",            D_WELL,         "well" },
    { "geode",                  D_GEODE,        nullptr },
    { "ancient_city",           D_ANCIENTCITY,  nullptr },
    { "trail_ruins",            D_TRAILRUINS,   "trails" },
    { "trial_chambers",         D_CHAMBERS,     "chambers" },
    { "camp",                   D_CAMP,         nullptr },
    { "ruined_portal",          D_PORTAL,       "portal" },
    { "ruined_portal_nether",   D_PORTALN,      "portaln" },
    { "fortress",               D_FORTESS,      "nether_fortress" },
    { "bastion_remnant",        D_BASTION,      "bastion" },
    { "end_city",               D_ENDCITY,      "endcity" },
    { "end_city_ship",          D_ENDCITYSHIP,  "end_ship" },
    { "end_gateway",            D_GATEWAY,      "gateway" },
    { "stronghold",             D_STRONGHOLD,   nullptr },
};

int structOpt(const QString& name)
{
    QString n = name.trimmed().toLower();
    if (n.startsWith(QLatin1String("minecraft:")))
        n = n.mid(10);
    for (const StructType& t : g_structTypes)
        if (n == QLatin1String(t.id) || (t.alias && n == QLatin1String(t.alias)))
            return t.opt;
    return D_NONE;
}

const char *structId(int opt)
{
    for (const StructType& t : g_structTypes)
        if (t.opt == opt)
            return t.id;
    return "";
}

// the dimension of a structure type in a version, DIM_UNDEF if it does not exist there
int structDim(int opt, int mc)
{
    if (opt == D_STRONGHOLD)
        return mc >= MC_B1_8 ? DIM_OVERWORLD : DIM_UNDEF;
    StructureConfig sconf;
    if (!getStructureConfig_override(mapopt2stype(opt), mc, &sconf))
        return DIM_UNDEF;
    return sconf.dim;
}

QStringList structNames(int mc, int dim)
{
    QStringList list;
    for (const StructType& t : g_structTypes)
    {
        const int d = structDim(t.opt, mc);
        if (d != DIM_UNDEF && (dim == DIM_UNDEF || d == dim))
            list.append(t.id);
    }
    return list;
}

struct Found
{
    QString type;
    int x, z;
    int y = INT_MIN;
    QString detail;
    QString state;      // datapack worlds: valid / unknown
};

// The vanilla structures of an area (AnalysisStructures::runStructs of the
// Structures tool, one seed).
std::vector<Found> vanillaStructures(const World& w, const Area& a, const std::vector<int>& opts,
    bool collect, McpCall *call, std::map<int, uint64_t> *counts, bool report = true)
{
    AnalysisStructures task;
    task.wi = w.wi;
    task.dim = w.dim;
    task.area = { a.x1, a.z1, a.x2, a.z2 };
    std::fill(std::begin(task.mapshow), std::end(task.mapshow), false);
    for (int opt : opts)
        task.mapshow[opt] = true;
    task.collect = collect;
    task.parallelInner = true;
    task.stop = false;
    task.cancel = &call->cancel;
    StructSeedResultPtr found;
    QObject::connect(&task, &AnalysisStructures::structsDone, &task,
        [&found](StructSeedResultPtr r) { found = r; }, Qt::DirectConnection);
    Generator g;
    setupGenerator(&g, w.wi.mc, w.wi.genFlags());
    runWatched(call, &task.stop, [&]() { task.runStructs(&g); }, [&]() {
        const size_t total = task.workTotal.load();
        if (total && report)
            call->progress(double(task.workDone.load()), double(total), "scanning regions");
    });
    checkCancel(call);
    std::vector<Found> out;
    if (!found)
        return out;
    for (const StructGroup& group : found->groups)
    {
        const QString type = structId(group.sopt);
        if (counts)
            (*counts)[group.sopt] += group.count;
        if (!collect)
            continue;
        for (const StructInstance& in : group.items)
        {
            Found f;
            f.type = type;
            f.x = in.p.x;
            f.z = in.p.z;
            if (in.detail >= 0)
                f.detail = group.details.value(in.detail);
            else if (group.stype >= 0)
            {
                VarPos vp(in.p, group.stype);
                vp.v = in.v;
                f.detail = vp.detail().join(":");
            }
            out.push_back(f);
        }
    }
    return out;
}

// The structure starts of a datapack world in an area (dpStartsInArea, the
// code of the map's structure layer and of the Structures tool).
std::vector<Found> datapackStructures(const World& w, const Area& a, const std::vector<char>& mask,
    bool unknown, McpCall *call, bool report = true)
{
    const std::shared_ptr<const DatapackWorld>& dpw = w.wi.dp;
    std::vector<Found> out;
    const std::vector<dp::StructureInfo>& infos = dpw->world->structures();
    struct Tile { int x0, z0, x1, z1; };
    std::vector<Tile> tiles;
    for (int64_t z = a.z1; z <= a.z2; z += DP_STRUCT_TILE)
        for (int64_t x = a.x1; x <= a.x2; x += DP_STRUCT_TILE)
            tiles.push_back({int(x), int(z), int(std::min<int64_t>(a.x2, x + DP_STRUCT_TILE - 1)),
                int(std::min<int64_t>(a.z2, z + DP_STRUCT_TILE - 1))});
    std::vector<std::vector<dp::StructureStart>> found(tiles.size());
    std::atomic_size_t next(0), done(0);
    std::atomic_bool stop(false);
    MappingWorkerState state;
    runWatched(call, &stop, [&]() {
        runMappingWorkers(tiles.size(), state, [&](size_t) {
            DpFinderLease finder(dpw, w.wi.seed, &stop);
            if (!finder.get())
                return;
            while (!stop)
            {
                const size_t t = next.fetch_add(1);
                if (t >= tiles.size())
                    break;
                dpStartsInArea(finder.get(), tiles[t].x0, tiles[t].z0, tiles[t].x1, tiles[t].z1,
                    &mask, false, &found[t], &stop);
                done.fetch_add(1);
            }
            releaseDatapackSampler();
        });
    }, [&]() {
        if (report)
            call->progress(double(done.load()), double(tiles.size()), "scanning tiles");
    });
    checkCancel(call);
    for (const auto& list : found)
    {
        for (const dp::StructureStart& st : list)
        {
            if (st.structure < 0 || st.structure >= (int) infos.size())
                continue;
            if (st.state == dp::StructureStart::Invalid)
                continue;
            if (st.state == dp::StructureStart::Unknown && !unknown)
                continue;
            Found f;
            f.type = infos[size_t(st.structure)].id;
            f.x = st.x;
            f.z = st.z;
            if (st.hasY || st.estimatedY)
                f.y = st.y;
            f.state = st.state == dp::StructureStart::Valid ? "valid" : "unknown";
            f.detail = dpStartStateText(st);
            if (!st.hasY && st.estimatedY)
                f.detail += " (y estimated)";
            out.push_back(f);
        }
    }
    return out;
}

// the structure ids of a datapack world from the names of a call; all if none
std::vector<char> datapackMask(const World& w, const QStringList& names)
{
    const std::vector<dp::StructureInfo>& infos = w.wi.dp->world->structures();
    std::vector<char> mask(infos.size(), names.isEmpty() ? 1 : 0);
    for (const QString& name : names)
    {
        QString id = name.trimmed();
        if (!id.contains(':'))
            id = "minecraft:" + id;
        bool hit = false;
        for (size_t i = 0; i < infos.size(); i++)
        {
            if (infos[i].id == id)
            {
                mask[i] = 1;
                hit = true;
            }
        }
        if (!hit)
            throw McpToolError{QString("The datapack world has no structure \"%1\" "
                "(datapack_info lists them).").arg(name)};
    }
    for (size_t i = 0; i < infos.size(); i++)
        if (!infos[i].supported)
            mask[i] = 0;
    return mask;
}

QStringList stringList(const QJsonObject& args, const char *key, int max)
{
    QStringList list;
    const QJsonValue v = args.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull())
        return list;
    if (v.isString())
        return { v.toString() };
    if (!v.isArray())
        throw McpToolError{QString("\"%1\" must be a list of strings.").arg(key)};
    const QJsonArray a = v.toArray();
    if (a.size() > max)
        throw McpToolError{QString("\"%1\" has more than %2 entries.").arg(key).arg(max)};
    for (const QJsonValue& e : a)
    {
        if (!e.isString() || e.toString().size() > 256)
            throw McpToolError{QString("\"%1\" must be a list of strings.").arg(key)};
        list.append(e.toString());
    }
    return list;
}

std::vector<int> vanillaOpts(const World& w, const QStringList& names, bool withStronghold)
{
    std::vector<int> opts;
    if (names.isEmpty())
    {
        // (geodes are left out unless asked for: there are hundreds in any area)
        for (const StructType& t : g_structTypes)
            if ((withStronghold || t.opt != D_STRONGHOLD) && t.opt != D_GEODE &&
                    structDim(t.opt, w.wi.mc) == w.dim)
                opts.push_back(t.opt);
        return opts;
    }
    for (const QString& name : names)
    {
        const int opt = structOpt(name);
        if (opt == D_NONE)
            throw McpToolError{QString("Unknown structure \"%1\". Known: %2.")
                .arg(name, structNames(w.wi.mc, DIM_UNDEF).join(", "))};
        const int d = structDim(opt, w.wi.mc);
        if (d == DIM_UNDEF)
            throw McpToolError{QString("There are no %1 structures in Minecraft %2.")
                .arg(structId(opt), mc2str(w.wi.mc))};
        if (d != w.dim)
            throw McpToolError{QString("%1 structures are not in this dimension (%2).")
                .arg(structId(opt), dimensionId(d))};
        opts.push_back(opt);
    }
    return opts;
}

QJsonObject foundJson(const Found& f)
{
    QJsonObject o;
    o["type"] = f.type;
    o["x"] = f.x;
    o["z"] = f.z;
    if (f.y != INT_MIN)
        o["y"] = f.y;
    if (!f.detail.isEmpty())
        o["detail"] = f.detail;
    if (!f.state.isEmpty())
        o["state"] = f.state;
    return o;
}

} // namespace

// ============================================================ the registry

McpRegistry::McpRegistry(McpServer *server)
    : server(server)
{
    addTools();
    addSearchTools();
    addDatapackTools();
}

McpRegistry::~McpRegistry()
{
    shutdown();
}

QJsonArray McpRegistry::toolList() const
{
    QJsonArray list;
    for (const Tool& t : tools)
    {
        QJsonObject o;
        o["name"] = t.name;
        o["title"] = t.title;
        o["description"] = t.description;
        o["inputSchema"] = t.schema;
        QJsonObject ann;
        ann["title"] = t.title;
        ann["readOnlyHint"] = t.readOnly;
        ann["destructiveHint"] = false;
        ann["openWorldHint"] = false;
        o["annotations"] = ann;
        list.append(o);
    }
    return list;
}

bool McpRegistry::hasTool(const QString& name) const
{
    for (const Tool& t : tools)
        if (t.name == name)
            return true;
    return false;
}

QJsonObject McpRegistry::call(const QString& name, const QJsonObject& args, McpCall *call)
{
    for (const Tool& t : tools)
    {
        if (t.name != name)
            continue;
        try
        {
            // the schemas forbid unknown arguments; say which one
            const QJsonObject props = t.schema.value("properties").toObject();
            for (auto it = args.begin(); it != args.end(); ++it)
                if (!props.contains(it.key()))
                    throw McpToolError{QString("Unknown argument \"%1\" for %2.").arg(it.key(), name)};
            return t.run(args, call);
        }
        catch (const McpToolError& e)
        {
            return errorResult(e.message);
        }
        catch (const std::bad_alloc&)
        {
            return errorResult("Out of memory: try a smaller area or fewer seeds.");
        }
    }
    return errorResult("Unknown tool: " + name);
}

int McpRegistry::reserveThreads(int wanted)
{
    QMutexLocker lock(&mutex);
    const int left = server->settings().searchThreads - threadsInUse;
    const int n = std::min(wanted, left);
    if (n > 0)
        threadsInUse += n;
    return n;
}

void McpRegistry::releaseThreads(int n)
{
    QMutexLocker lock(&mutex);
    threadsInUse = std::max(0, threadsInUse - n);
}

std::shared_ptr<McpDatapacks> McpRegistry::findDatapacks(const QString& id)
{
    QMutexLocker lock(&mutex);
    return datapacks.value(id.trimmed());
}

// ============================================================ query tools

void McpRegistry::addTools()
{
    // ---------------------------------------------------------- server_info
    {
        Tool t;
        t.name = "server_info";
        t.title = "Server information";
        t.description = "What this Seed Atlas server can do: Minecraft versions, world types, "
            "dimensions, structure types and biomes of a version, the rights and limits that "
            "the app's settings give MCP clients, and the datapack worlds and searches of this "
            "session.";
        QJsonObject p;
        p["mc"] = prop("string", "Version for the lists of structures and biomes (default: the "
            "newest).");
        t.schema = schemaObject(p, {});
        t.run = [this](const QJsonObject& args, McpCall *) {
            const int mc = parseMc(argString(args, "mc"));
            QJsonObject o;
            QJsonArray versions;
            for (int v = MC_B1_7; v <= MC_NEWEST; v++)
            {
                const char *s = mc2str(v);
                if (s && *s)
                    versions.append(s);
            }
            o["versions"] = versions;
            o["newest"] = mc2str(MC_NEWEST);
            QJsonArray dpVersions;
            for (int v = MC_1_18; v <= MC_NEWEST; v++)
                if (dp::versionSupported(v))
                    dpVersions.append(mc2str(v));
            o["datapack_versions"] = dpVersions;
            QJsonArray types;
            for (int type = 0; type < WORLD_TYPE_NUM; type++)
                if (worldTypeAvailable(type, mc))
                    types.append(worldTypeId(type));
            o["mc"] = mc2str(mc);
            o["world_types"] = types;
            QJsonObject structs;
            for (int dim : { DIM_OVERWORLD, DIM_NETHER, DIM_END })
                structs[dimensionId(dim)] = QJsonArray::fromStringList(structNames(mc, dim));
            o["structures"] = structs;
            QJsonObject biomes;
            for (int dim : { DIM_OVERWORLD, DIM_NETHER, DIM_END })
            {
                QJsonArray list;
                for (int id = 0; id < 256; id++)
                {
                    const char *s = biome2str(mc, id);
                    if (s && getDimension(id) == dim && (dim != DIM_OVERWORLD || isOverworld(mc, id)))
                        list.append(QStringLiteral("minecraft:") + s);
                }
                biomes[dimensionId(dim)] = list;
            }
            o["biomes"] = biomes;
            const McpSettings& s = server->settings();
            QJsonObject rights;
            rights["mode"] = s.rights == McpSettings::WRITE_FILES ? "write_files" : "read_and_compute";
            rights["write_files"] = s.rights == McpSettings::WRITE_FILES;
            rights["search_threads"] = s.searchThreads;
            rights["max_search_minutes"] = s.searchMinutes;
            rights["note"] = "Set in the app: Settings, MCP. The server reads them at its start.";
            o["rights"] = rights;
            QJsonObject limits;
            limits["biome_map_cells"] = double(MAX_MAP_CELLS);
            limits["analysis_cells_per_seed"] = double(MAX_STAT_CELLS);
            limits["structure_area_side"] = MAX_STRUCT_AREA_SIDE;
            limits["structure_results"] = MAX_STRUCT_RESULTS;
            limits["nearest_radius"] = MAX_NEAREST_RADIUS;
            limits["points"] = MAX_POINTS;
            limits["analysis_seeds"] = MAX_STAT_SEEDS;
            limits["search_results"] = MAX_SEARCH_RESULTS;
            limits["search_list_seeds"] = MAX_LIST_SEEDS;
            limits["searches"] = MAX_SEARCHES;
            limits["datapack_worlds"] = MAX_DATAPACK_WORLDS;
            o["limits"] = limits;
            {
                QMutexLocker lock(&mutex);
                o["datapack_worlds"] = QJsonArray::fromStringList(datapacks.keys());
                o["searches"] = QJsonArray::fromStringList(searches.keys());
                o["search_threads_in_use"] = threadsInUse;
            }
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- biome_at
    {
        Tool t;
        t.name = "biome_at";
        t.title = "Biome at a position";
        t.description = "The biome at a block position (or at up to 1000 positions), as the "
            "game generates it. 1.18+ has 3D biomes: y selects the level (default: the surface "
            "level of the map, y 256 or 319 in Amplified). resolution \"block\" applies the "
            "game's 1:1 zoom, \"cell\" gives the 4x4x4 biome cell.";
        QJsonObject p = worldSchemaProperties(true);
        p["x"] = prop("integer", "Block x.");
        p["z"] = prop("integer", "Block z.");
        p["y"] = prop("integer", "Block y (default: the surface level).");
        QJsonObject pts = prop("array", "Several positions instead of x, z: [[x, z], ...] or "
            "[{\"x\":..,\"z\":..,\"y\":..}, ...].");
        pts["maxItems"] = MAX_POINTS;
        p["points"] = pts;
        QJsonObject res = prop("string", "\"block\" (default) or \"cell\".");
        res["enum"] = QJsonArray{ "block", "cell" };
        p["resolution"] = res;
        t.schema = schemaObject(p, { "seed" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args);
            const int y0 = argY(w, args);
            const QString resText = argString(args, "resolution", "block");
            if (resText != "block" && resText != "cell")
                throw McpToolError{"resolution must be \"block\" or \"cell\"."};
            const int scale = resText == "cell" ? 4 : 1;
            struct P { int x, y, z; };
            std::vector<P> points;
            if (present(args, "points"))
            {
                const QJsonValue v = args.value("points");
                if (!v.isArray() || v.toArray().size() > MAX_POINTS || v.toArray().isEmpty())
                    throw McpToolError{QString("points must be a list of 1 to %1 positions.").arg(MAX_POINTS)};
                for (const QJsonValue& e : v.toArray())
                {
                    QJsonObject o;
                    if (e.isArray() && e.toArray().size() >= 2)
                    {
                        o["x"] = e.toArray().at(0);
                        o["z"] = e.toArray().at(1);
                        if (e.toArray().size() > 2)
                            o["y"] = e.toArray().at(2);
                    }
                    else if (e.isObject())
                        o = e.toObject();
                    else
                        throw McpToolError{"Each point is [x, z] or {\"x\":.., \"z\":..}."};
                    P pt;
                    pt.x = int(argIntReq(o, "x", -MAX_COORD, MAX_COORD));
                    pt.z = int(argIntReq(o, "z", -MAX_COORD, MAX_COORD));
                    pt.y = int(argInt(o, "y", y0, w.datapack() ? w.wi.dp->minY() : -64,
                        w.datapack() ? w.wi.dp->maxY() : 320));
                    points.push_back(pt);
                }
            }
            else
            {
                P pt;
                pt.x = int(argIntReq(args, "x", -MAX_COORD, MAX_COORD));
                pt.z = int(argIntReq(args, "z", -MAX_COORD, MAX_COORD));
                pt.y = y0;
                points.push_back(pt);
            }
            QJsonArray list;
            std::unique_ptr<Gen> gen;
            if (!w.datapack())
                gen.reset(new Gen(w));
            for (const P& pt : points)
            {
                checkCancel(call);
                int id;
                if (w.datapack())
                {
                    id = scale == 1
                        ? dpBiomeAt(w.wi.dp, w.wi.seed, 1, pt.x, pt.y, pt.z)
                        : dpBiomeAt(w.wi.dp, w.wi.seed, 4, pt.x >> 2, pt.y, pt.z >> 2);
                }
                else
                {
                    id = scale == 1
                        ? getBiomeAt(&gen->g, 1, pt.x, pt.y, pt.z)
                        : getBiomeAt(&gen->g, 4, pt.x >> 2, pt.y >> 2, pt.z >> 2);
                }
                QJsonObject o;
                o["x"] = pt.x;
                o["y"] = pt.y;
                o["z"] = pt.z;
                o["biome"] = id >= 0 ? QJsonValue(biomeName(w, id)) : QJsonValue();
                list.append(o);
            }
            QJsonObject o = worldJson(w);
            o["resolution"] = resText;
            if (list.size() == 1)
            {
                const QJsonObject one = list.at(0).toObject();
                for (auto it = one.begin(); it != one.end(); ++it)
                    o[it.key()] = it.value();
            }
            else
                o["points"] = list;
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- surface_height
    {
        Tool t;
        t.name = "surface_height";
        t.title = "Surface height";
        t.description = "The terrain height of block columns: the y of the highest solid "
            "block from the terrain noise (no caves, trees, lakes or structures). Built-in "
            "generator: exact terrain for the Overworld of 1.18+ (in about 80% of columns "
            "block-exact against the game), an estimate before 1.18 and in the End, none in "
            "the Nether. Datapack worlds: ocean_floor (solid blocks) and world_surface (with "
            "fluids) as the game's heightmaps after the terrain step.";
        QJsonObject p = worldSchemaProperties(true);
        p["x"] = prop("integer", "Block x.");
        p["z"] = prop("integer", "Block z.");
        QJsonObject pts = prop("array", "Several columns instead of x, z: [[x, z], ...].");
        pts["maxItems"] = MAX_POINTS;
        p["points"] = pts;
        p["below_y"] = prop("integer", "Datapack dimensions with a ceiling (Nether): look down "
            "from this y (default: just below the roof).");
        t.schema = schemaObject(p, { "seed" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args);
            std::vector<std::pair<int, int>> cols;
            if (present(args, "points"))
            {
                const QJsonValue v = args.value("points");
                if (!v.isArray() || v.toArray().size() > MAX_POINTS || v.toArray().isEmpty())
                    throw McpToolError{QString("points must be a list of 1 to %1 columns.").arg(MAX_POINTS)};
                for (const QJsonValue& e : v.toArray())
                {
                    QJsonObject o;
                    if (e.isArray() && e.toArray().size() >= 2)
                    {
                        o["x"] = e.toArray().at(0);
                        o["z"] = e.toArray().at(1);
                    }
                    else if (e.isObject())
                        o = e.toObject();
                    else
                        throw McpToolError{"Each point is [x, z] or {\"x\":.., \"z\":..}."};
                    cols.push_back({int(argIntReq(o, "x", -MAX_COORD, MAX_COORD)),
                        int(argIntReq(o, "z", -MAX_COORD, MAX_COORD))});
                }
            }
            else
                cols.push_back({int(argIntReq(args, "x", -MAX_COORD, MAX_COORD)),
                    int(argIntReq(args, "z", -MAX_COORD, MAX_COORD))});

            QJsonObject o = worldJson(w);
            QJsonArray list;
            if (w.datapack())
            {
                dp::Sampler *s = datapackSampler(w.wi.dp, w.wi.seed);
                if (!s || !s->hasTerrain())
                {
                    const dp::DimensionInfo *d = w.wi.dp->dimensionInfo();
                    throw McpToolError{"This dimension has no terrain heights: "
                        + (d ? d->terrainReason : QString("it cannot be computed"))};
                }
                int top = INT_MAX;
                if (w.wi.dp->dimensionInfo()->hasCeiling)
                {
                    top = present(args, "below_y")
                        ? int(argInt(args, "below_y", 0, w.wi.dp->minY(), w.wi.dp->maxY()))
                        : dpBelowRoofLevel(w.wi.dp, w.wi.seed);
                    if (top == INT_MIN)
                        top = INT_MAX;
                    o["below_y"] = top == INT_MAX ? QJsonValue() : QJsonValue(top);
                }
                for (const auto& c : cols)
                {
                    checkCancel(call);
                    const int of = s->surfaceHeight(c.first, c.second, dp::HEIGHT_OCEAN_FLOOR, top);
                    const int ws = s->surfaceHeight(c.first, c.second, dp::HEIGHT_SURFACE, top);
                    QJsonObject e;
                    e["x"] = c.first;
                    e["z"] = c.second;
                    e["ocean_floor"] = of == dp::NO_HEIGHT ? QJsonValue() : QJsonValue(of);
                    e["world_surface"] = ws == dp::NO_HEIGHT ? QJsonValue() : QJsonValue(ws);
                    list.append(e);
                }
                o["method"] = "datapack terrain (final_density; no aquifers away from sea level, "
                    "carvers or features)";
            }
            else
            {
                if (w.dim == DIM_NETHER)
                    throw McpToolError{"The built-in generator has no terrain heights for the Nether."};
                Gen gen(w);
                SurfaceNoise sn;
                const bool terrain = w.dim == DIM_OVERWORLD && w.wi.mc >= MC_1_18;
                if (!terrain)
                    initSurfaceNoise(&sn, w.dim, w.wi.seed);
                for (const auto& c : cols)
                {
                    checkCancel(call);
                    int y;
                    if (terrain)
                        y = getTerrainHeightAt(&gen.g.bn, getThreadTerrainNoise(w.wi.seed),
                            c.first, c.second) - 1;
                    else
                    {
                        float fy = 0;
                        if (w.dim == DIM_END)
                        {
                            mapEndSurfaceHeight(&fy, &gen.g.en, &sn, c.first, c.second, 1, 1, 1, 0);
                            mapEndIslandHeight(&fy, &gen.g.en, gen.g.seed, c.first, c.second, 1, 1, 1);
                        }
                        else
                            mapApproxHeight(&fy, 0, &gen.g, &sn, c.first >> 2, c.second >> 2, 1, 1);
                        y = (int) std::floor(fy);
                    }
                    QJsonObject e;
                    e["x"] = c.first;
                    e["z"] = c.second;
                    e["y"] = y;
                    if (terrain)    // (the same name as in datapack worlds)
                        e["ocean_floor"] = y;
                    list.append(e);
                }
                o["method"] = terrain
                    ? "terrain density of the game (top solid block; the sea bed in oceans; no "
                      "caves, aquifers, trees)"
                    : "estimate (the map's height layer)";
                o["exact"] = false;
                if (terrain)
                    o["sea_level"] = 63;
            }
            if (list.size() == 1)
            {
                const QJsonObject one = list.at(0).toObject();
                for (auto it = one.begin(); it != one.end(); ++it)
                    o[it.key()] = it.value();
            }
            else
                o["points"] = list;
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- biome_map
    {
        Tool t;
        t.name = "biome_map";
        t.title = "Biome map of an area";
        t.description = "The biomes of an area as a compact grid: one character per cell "
            "(row by row from north to south, west to east) and a legend of the characters. "
            "Each cell covers scale x scale blocks. scale \"auto\" (default) takes the finest "
            "scale whose grid has at most max_cells cells; a fixed scale that gives more "
            "cells is refused.";
        QJsonObject p = worldSchemaProperties(true);
        const QJsonObject area = areaProps(true);
        for (auto it = area.begin(); it != area.end(); ++it)
            p[it.key()] = it.value();
        p["y"] = prop("integer", "Block y (default: the surface level).");
        QJsonObject sc;
        sc["type"] = QJsonArray{ "integer", "string" };
        sc["description"] = "Blocks per cell: 1, 4, 16, 64, 256 or \"auto\".";
        p["scale"] = sc;
        p["max_cells"] = prop("integer", QString("Most cells of the grid (default 4096, at most "
            "%1).").arg(MAX_MAP_CELLS));
        t.schema = schemaObject(p, { "seed" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args);
            const int y = argY(w, args);
            const Area a = parseArea(args, 2 * MAX_COORD, 256);
            const int64_t maxCells = argInt(args, "max_cells", 4096, 1, MAX_MAP_CELLS);
            const int64_t wb = int64_t(a.x2) - a.x1 + 1, hb = int64_t(a.z2) - a.z1 + 1;
            int scale = 0;
            const QJsonValue sv = args.value("scale");
            const int scales[] = { 1, 4, 16, 64, 256 };
            auto cellsAt = [&](int s) {
                const int64_t x1 = a.x1 >= 0 ? a.x1 / s : -((-int64_t(a.x1) + s - 1) / s);
                const int64_t z1 = a.z1 >= 0 ? a.z1 / s : -((-int64_t(a.z1) + s - 1) / s);
                const int64_t x2 = a.x2 >= 0 ? a.x2 / s : -((-int64_t(a.x2) + s - 1) / s);
                const int64_t z2 = a.z2 >= 0 ? a.z2 / s : -((-int64_t(a.z2) + s - 1) / s);
                return (x2 - x1 + 1) * (z2 - z1 + 1);
            };
            (void) wb; (void) hb;
            if (sv.isUndefined() || sv.isNull() || sv.toString() == "auto")
            {
                for (int s : scales)
                {
                    if (cellsAt(s) <= maxCells)
                    {
                        scale = s;
                        break;
                    }
                }
                if (!scale)
                    throw McpToolError{QString("The area needs more than %1 cells even at 1:256. "
                        "Choose a smaller area.").arg(maxCells)};
            }
            else
            {
                scale = int(argInt(args, "scale", 4, 1, 256));
                if (std::find(std::begin(scales), std::end(scales), scale) == std::end(scales))
                    throw McpToolError{"scale must be 1, 4, 16, 64, 256 or \"auto\"."};
                if (cellsAt(scale) > maxCells)
                {
                    int better = 0;
                    for (int s : scales)
                        if (!better && cellsAt(s) <= maxCells)
                            better = s;
                    throw McpToolError{QString("The area has %1 cells at scale %2, more than "
                        "max_cells (%3). %4").arg(cellsAt(scale)).arg(scale).arg(maxCells)
                        .arg(better ? QString("Scale %1 fits.").arg(better)
                            : QString("Choose a smaller area."))};
                }
            }
            auto fdiv = [](int64_t v, int s) {
                return v >= 0 ? v / s : -((-v + s - 1) / s);
            };
            const int cx1 = int(fdiv(a.x1, scale)), cz1 = int(fdiv(a.z1, scale));
            const int cw = int(fdiv(a.x2, scale) - cx1 + 1), ch = int(fdiv(a.z2, scale) - cz1 + 1);
            std::vector<int> ids(size_t(cw) * ch, -1);
            const int tile = 64;
            int tilesDone = 0;
            const int tilesTotal = ((cw + tile - 1) / tile) * ((ch + tile - 1) / tile);
            std::unique_ptr<Gen> gen;
            if (!w.datapack())
                gen.reset(new Gen(w));
            for (int tz = 0; tz < ch; tz += tile)
            {
                for (int tx = 0; tx < cw; tx += tile)
                {
                    checkCancel(call);
                    const int tw = std::min(tile, cw - tx), th = std::min(tile, ch - tz);
                    std::vector<int> buf(size_t(tw) * th, -1);
                    if (w.datapack())
                    {
                        if (!dpGenBiomes(w.wi.dp, w.wi.seed, buf.data(), scale, cx1 + tx, cz1 + tz,
                                tw, th, y, &call->cancel))
                            checkCancel(call);
                    }
                    else if (w.dim == DIM_OVERWORLD && w.wi.mc >= MC_1_18 && scale > 4)
                    {   // the exact biome at the cell's centre (as the analysis)
                        const int s = scale / 4, mid = s / 2;
                        for (int j = 0; j < th; j++)
                            for (int i = 0; i < tw; i++)
                                buf[size_t(j) * tw + i] = sampleBiomeNoise(&gen->g.bn, NULL,
                                    (cx1 + tx + i) * s + mid, y >> 2, (cz1 + tz + j) * s + mid, NULL, 0);
                    }
                    else
                    {
                        Range r = { scale, cx1 + tx, cz1 + tz, tw, th, scale == 1 ? y : y >> 2, 1 };
                        int *cache = allocCache(&gen->g, r);
                        if (!cache)
                            throw McpToolError{"Out of memory."};
                        genBiomes(&gen->g, cache, r);
                        std::copy(cache, cache + size_t(tw) * th, buf.begin());
                        free(cache);
                    }
                    for (int j = 0; j < th; j++)
                        for (int i = 0; i < tw; i++)
                            ids[size_t(tz + j) * cw + tx + i] = buf[size_t(j) * tw + i];
                    call->progress(++tilesDone, tilesTotal, "generating biomes");
                }
            }
            // the legend, by frequency
            std::map<int, int64_t> freq;
            for (int id : ids)
                freq[id]++;
            std::vector<std::pair<int, int64_t>> order(freq.begin(), freq.end());
            std::stable_sort(order.begin(), order.end(), [](const auto& l, const auto& r) {
                return l.second > r.second;
            });
            static const char alphabet[] =
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                "#$%&*+-=?@^~<>()[]{}!/|:;,_";
            const int nchars = int(sizeof(alphabet)) - 1;
            const bool chars = (int) order.size() <= nchars;
            std::map<int, int> index;
            QJsonArray legend;
            for (size_t i = 0; i < order.size(); i++)
            {
                index[order[i].first] = int(i);
                QJsonObject l;
                if (chars)
                    l["char"] = QString(QChar(alphabet[i]));
                else
                    l["index"] = int(i);
                l["biome"] = order[i].first >= 0 ? QJsonValue(biomeName(w, order[i].first)) : QJsonValue();
                l["cells"] = double(order[i].second);
                legend.append(l);
            }
            QJsonArray rows;
            QString textGrid;
            for (int j = 0; j < ch; j++)
            {
                if (chars)
                {
                    QString row;
                    row.reserve(cw);
                    for (int i = 0; i < cw; i++)
                        row += QChar(alphabet[index[ids[size_t(j) * cw + i]]]);
                    rows.append(row);
                    textGrid += row + "\n";
                }
                else
                {
                    QJsonArray row;
                    for (int i = 0; i < cw; i++)
                        row.append(index[ids[size_t(j) * cw + i]]);
                    rows.append(row);
                }
            }
            QJsonObject o = worldJson(w);
            o["scale"] = scale;
            o["y"] = y;
            o["x1"] = cx1 * scale;
            o["z1"] = cz1 * scale;
            o["x2"] = (cx1 + cw) * scale - 1;
            o["z2"] = (cz1 + ch) * scale - 1;
            o["width"] = cw;
            o["height"] = ch;
            o["legend"] = legend;
            o["rows"] = rows;
            o["encoding"] = chars ? "one character per cell, see legend"
                : "legend indices per cell";
            return result(o, QString("Biome map %1x%2 cells at 1:%3, north-west corner %4, %5.")
                .arg(cw).arg(ch).arg(scale).arg(cx1 * scale).arg(cz1 * scale));
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- structures_in_area
    {
        Tool t;
        t.name = "structures_in_area";
        t.title = "Structures in an area";
        t.description = "The structures that generate in an area, with their positions, as "
            "the map and the Structures tool show them (with the game's biome checks). "
            "Built-in generator: types as in server_info (default: all of the dimension but "
            "geodes, plus strongholds in the Overworld). Datapack worlds: structure ids of the packs; starts "
            "that cannot be decided are listed with state \"unknown\" if include_unknown.";
        QJsonObject p = worldSchemaProperties(true);
        const QJsonObject area = areaProps(true);
        for (auto it = area.begin(); it != area.end(); ++it)
            p[it.key()] = it.value();
        QJsonObject types = prop("array", "Structure types, e.g. [\"village\", "
            "\"ancient_city\"] or datapack ids (default: all).");
        types["items"] = QJsonObject{ { "type", "string" } };
        p["types"] = types;
        p["max_results"] = prop("integer", QString("Most positions returned (default 500, at "
            "most %1); the counts cover all.").arg(MAX_STRUCT_RESULTS));
        p["include_unknown"] = prop("boolean", "Datapack worlds: also starts that depend on "
            "what cannot be computed (default false).");
        t.schema = schemaObject(p, { "seed" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args);
            const Area a = parseArea(args, MAX_STRUCT_AREA_SIDE, 1024);
            const int maxResults = int(argInt(args, "max_results", 500, 1, MAX_STRUCT_RESULTS));
            const QStringList names = stringList(args, "types", 200);
            std::vector<Found> found;
            if (w.datapack())
                found = datapackStructures(w, a, datapackMask(w, names),
                    argBool(args, "include_unknown", false), call);
            else
                found = vanillaStructures(w, a, vanillaOpts(w, names, true), true, call, nullptr);
            const int cx = int((int64_t(a.x1) + a.x2) / 2), cz = int((int64_t(a.z1) + a.z2) / 2);
            std::stable_sort(found.begin(), found.end(), [&](const Found& l, const Found& r) {
                const double dl = std::hypot(double(l.x - cx), double(l.z - cz));
                const double dr = std::hypot(double(r.x - cx), double(r.z - cz));
                return dl < dr;
            });
            QJsonObject counts;
            for (const Found& f : found)
                counts[f.type] = counts.value(f.type).toInt() + 1;
            QJsonArray list;
            for (size_t i = 0; i < found.size() && int(i) < maxResults; i++)
                list.append(foundJson(found[i]));
            QJsonObject o = worldJson(w);
            o["x1"] = a.x1;
            o["z1"] = a.z1;
            o["x2"] = a.x2;
            o["z2"] = a.z2;
            o["counts"] = counts;
            o["total"] = int(found.size());
            o["structures"] = list;
            o["truncated"] = int(found.size()) > maxResults;
            o["order"] = "nearest to the centre of the area first";
            return result(o, QString("%1 structures in the area.").arg(found.size()));
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- nearest_structure
    {
        Tool t;
        t.name = "nearest_structure";
        t.title = "Nearest structure";
        t.description = "The nearest structure of a type to a position (straight-line "
            "distance of the structure position), searched outwards up to max_radius blocks.";
        QJsonObject p = worldSchemaProperties(true);
        p["type"] = prop("string", "Structure type (e.g. \"village\") or datapack structure id.");
        p["x"] = prop("integer", "Block x to measure from (default 0).");
        p["z"] = prop("integer", "Block z (default 0).");
        p["max_radius"] = prop("integer", QString("Search up to this distance (default 5000, at "
            "most %1).").arg(MAX_NEAREST_RADIUS));
        p["include_unknown"] = prop("boolean", "Datapack worlds: accept starts that cannot be "
            "decided (default false).");
        t.schema = schemaObject(p, { "seed", "type" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args);
            const QString type = argString(args, "type");
            if (type.isEmpty())
                throw McpToolError{"Missing argument \"type\"."};
            const int x = int(argInt(args, "x", 0, -MAX_COORD, MAX_COORD));
            const int z = int(argInt(args, "z", 0, -MAX_COORD, MAX_COORD));
            const int maxR = int(argInt(args, "max_radius", 5000, 1, MAX_NEAREST_RADIUS));
            const bool unknown = argBool(args, "include_unknown", false);
            std::vector<int> opts;
            std::vector<char> mask;
            if (w.datapack())
                mask = datapackMask(w, { type });
            else
                opts = vanillaOpts(w, { type }, true);
            int r = 256;
            while (true)
            {
                r = std::min(r, maxR);
                checkCancel(call);
                const Area a = { int(std::max<int64_t>(-MAX_COORD, int64_t(x) - r)),
                    int(std::max<int64_t>(-MAX_COORD, int64_t(z) - r)),
                    int(std::min<int64_t>(MAX_COORD, int64_t(x) + r)),
                    int(std::min<int64_t>(MAX_COORD, int64_t(z) + r)) };
                std::vector<Found> found = w.datapack()
                    ? datapackStructures(w, a, mask, unknown, call, false)
                    : vanillaStructures(w, a, opts, true, call, nullptr, false);
                const Found *best = nullptr;
                double bestD = 0;
                for (const Found& f : found)
                {
                    const double d = std::hypot(double(f.x) - x, double(f.z) - z);
                    if (!best || d < bestD)
                    {
                        best = &f;
                        bestD = d;
                    }
                }
                // Anything nearer than r lies inside the square, so a structure
                // within r is the nearest one; one further out may not be.
                if (best && bestD > r)
                    best = nullptr;
                if (best || r >= maxR)
                {
                    QJsonObject o = worldJson(w);
                    o["from_x"] = x;
                    o["from_z"] = z;
                    o["max_radius"] = maxR;
                    if (best)
                    {
                        o["found"] = true;
                        o["structure"] = foundJson(*best);
                        o["distance"] = std::round(bestD * 10) / 10;
                        return result(o, QString("Nearest %1 at %2, %3 (%4 blocks away).")
                            .arg(best->type).arg(best->x).arg(best->z).arg(int(bestD)));
                    }
                    o["found"] = false;
                    return result(o, QString("No %1 within %2 blocks.").arg(type).arg(maxR));
                }
                call->progress(r, maxR, QString("searched %1 blocks around").arg(r));
                r *= 2;
            }
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- spawn
    {
        Tool t;
        t.name = "spawn";
        t.title = "World spawn";
        t.description = "The world spawn point of a seed (built-in generator, Overworld). The "
            "game moves the spawn to a grass block nearby, which the generator does not know, "
            "so the result can be off by some blocks.";
        QJsonObject p = worldSchemaProperties(true);
        p.remove("dimension");
        p.remove("world");
        t.schema = schemaObject(p, { "seed" });
        t.run = [this](const QJsonObject& args, McpCall *) {
            World w = parseWorld(this, args);
            Gen gen(w);
            const Pos pos = getSpawn(&gen.g);
            QJsonObject o = worldJson(w);
            o["x"] = pos.x;
            o["z"] = pos.z;
            return result(o, QString("Spawn at %1, %2.").arg(pos.x).arg(pos.z));
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- strongholds
    {
        Tool t;
        t.name = "strongholds";
        t.title = "Strongholds";
        t.description = "The first strongholds of a seed in the game's order (nearest ring "
            "first), with the game's biome adjustment (built-in generator, Overworld). In "
            "datapack worlds, use structures_in_area with \"minecraft:stronghold\".";
        QJsonObject p = worldSchemaProperties(true);
        p.remove("dimension");
        p.remove("world");
        p["count"] = prop("integer", "How many (default 3, at most 128).");
        t.schema = schemaObject(p, { "seed" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args);
            const int count = int(argInt(args, "count", 3, 1, 128));
            Gen gen(w);
            StrongholdIter sh;
            initFirstStronghold(&sh, w.wi.mc, w.wi.seed);
            QJsonArray list;
            for (int i = 0; i < count; i++)
            {
                checkCancel(call);
                const int more = nextStronghold(&sh, &gen.g);
                QJsonObject o;
                o["index"] = i;
                o["ring"] = sh.ringnum;
                o["x"] = sh.pos.x;
                o["z"] = sh.pos.z;
                o["chunk_x"] = sh.pos.x >> 4;
                o["chunk_z"] = sh.pos.z >> 4;
                list.append(o);
                call->progress(i + 1, count);
                if (more <= 0)
                    break;
            }
            QJsonObject o = worldJson(w);
            o["strongholds"] = list;
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- end_portals
    {
        Tool t;
        t.name = "end_portals";
        t.title = "End portals and eyes";
        t.description = "The End portals of the first strongholds (Minecraft 1.18+, built-in "
            "generator), as the End Portals tool shows them: the portal centre, the eyes of "
            "ender that the 12 frames hold, which frames are exact (their state follows from "
            "the seed alone; others depend on caves), and whether the portal is lit. frames: "
            "the 12 frames clockwise from the north-west corner, E/e = eye (certain/"
            "uncertain), -/? = empty (certain/uncertain).";
        QJsonObject p = worldSchemaProperties(true);
        p.remove("dimension");
        p.remove("world");
        p["count"] = prop("integer", "Strongholds to look at (default 3, at most 128).");
        t.schema = schemaObject(p, { "seed" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args);
            if (w.wi.mc < MC_1_18)
                throw McpToolError{"End portal eyes are computed for Minecraft 1.18 and later."};
            AnalysisPortals task;
            task.wi = w.wi;
            task.count = int(argInt(args, "count", 3, 1, 128));
            task.stop = false;
            runWatched(call, &task.stop, [&]() { task.run(); }, nullptr);
            checkCancel(call);
            QJsonArray list;
            for (const EyePortalRecord& r : task.rows)
            {
                QJsonObject o;
                o["index"] = int(r.index);
                o["x"] = r.x;
                o["y"] = r.y;
                o["z"] = r.z;
                o["eyes"] = int(r.eyeCount);
                o["exact_eyes"] = int(r.exactEyeCount);
                o["exact_frames"] = int(r.exactFrames);
                o["certain"] = !r.hasUncertainEyes() && r.exactFrames == 12;
                o["lit"] = bool(r.lit);
                o["lit_certain"] = bool(r.litExact);
                o["frames"] = r.frameText();
                list.append(o);
            }
            QJsonObject o = worldJson(w);
            o["portals"] = list;
            if (w.wi.mc == MC_26_3)
                o["note"] = "In 26.3 about 0.3% of the stronghold positions differ from the game; "
                    "confirm hits in the game.";
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- biome_stats
    {
        Tool t;
        t.name = "biome_stats";
        t.title = "Biome statistics";
        t.description = "How many cells of each biome an area has, for one or more seeds (the "
            "Biomes tool's analysis). The area is counted at a scale (blocks per cell); "
            "samples limits the cells sampled at random from a large area.";
        QJsonObject p = worldSchemaProperties(false);
        p["seed"] = seedProp("A seed (or use seeds).");
        QJsonObject seeds = prop("array", QString("Several seeds (at most %1).").arg(MAX_STAT_SEEDS));
        seeds["items"] = QJsonObject{ { "type", QJsonArray{ "string", "integer" } } };
        p["seeds"] = seeds;
        const QJsonObject area = areaProps(true);
        for (auto it = area.begin(); it != area.end(); ++it)
            p[it.key()] = it.value();
        p["scale"] = prop("integer", "Blocks per cell: 1, 4 (default), 16, 64 or 256.");
        p["y"] = prop("integer", "Block y (default: the surface level).");
        p["samples"] = prop("integer", "Sample this many random cells instead of all (default: "
            "all).");
        t.schema = schemaObject(p, {});
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args, false);
            std::vector<uint64_t> seeds;
            if (present(args, "seed"))
                seeds.push_back(parseSeed(args.value("seed"), "seed"));
            if (present(args, "seeds"))
            {
                const QJsonValue v = args.value("seeds");
                if (!v.isArray() || v.toArray().size() > MAX_STAT_SEEDS)
                    throw McpToolError{QString("seeds must be a list of at most %1 seeds.").arg(MAX_STAT_SEEDS)};
                for (const QJsonValue& s : v.toArray())
                    seeds.push_back(parseSeed(s, "a seed of seeds"));
            }
            if (seeds.empty() || seeds.size() > size_t(MAX_STAT_SEEDS))
                throw McpToolError{"Give a seed or seeds."};
            const int scale = int(argInt(args, "scale", 4, 1, 256));
            if (scale != 1 && scale != 4 && scale != 16 && scale != 64 && scale != 256)
                throw McpToolError{"scale must be 1, 4, 16, 64 or 256."};
            const Area a = parseArea(args, 2 * MAX_COORD, 512);
            auto fdiv = [](int64_t v, int s) { return v >= 0 ? v / s : -((-v + s - 1) / s); };
            AnalysisBiomes task;
            task.wi = w.wi;
            task.wi.y = argY(w, args);
            task.dims[0] = w.datapack() ? DIM_OVERWORLD : w.dim;
            task.dims[1] = task.dims[2] = DIM_UNDEF;
            task.dat.x1 = int(fdiv(a.x1, scale));
            task.dat.z1 = int(fdiv(a.z1, scale));
            task.dat.x2 = int(fdiv(a.x2, scale));
            task.dat.z2 = int(fdiv(a.z2, scale));
            task.dat.scale = scale;
            task.dat.locate = 0;
            task.dat.locateDim = DIM_UNDEF;
            const int64_t cells = (int64_t(task.dat.x2) - task.dat.x1 + 1)
                * (int64_t(task.dat.z2) - task.dat.z1 + 1);
            const int64_t samples = argInt(args, "samples", std::min(cells, MAX_STAT_CELLS), 1,
                MAX_STAT_CELLS);
            if (std::min(cells, samples) > MAX_STAT_CELLS)
                throw McpToolError{QString("The area has %1 cells at scale %2: more than %3. Use "
                    "a coarser scale or samples.").arg(cells).arg(scale).arg(MAX_STAT_CELLS)};
            task.dat.samples = uint64_t(samples);
            task.stop = false;
            QJsonArray perSeed;
            for (size_t i = 0; i < seeds.size(); i++)
            {
                checkCancel(call);
                QVector<uint64_t> counts;
                task.workDone = 0;
                task.workTotal = 0;
                runWatched(call, &task.stop, [&]() { counts = task.countBiomes(seeds[i], true); },
                    [&]() {
                        const double total = double(task.workTotal.load());
                        if (total > 0)
                            call->progress(i + std::min(1.0, task.workDone.load() / total),
                                double(seeds.size()), "counting biomes");
                    });
                checkCancel(call);
                const int nb = counts.size() - 1;
                uint64_t sum = 0;
                for (int id = 0; id < nb; id++)
                    sum += counts[id];
                std::vector<std::pair<int, uint64_t>> order;
                for (int id = 0; id < nb; id++)
                    if (counts[id])
                        order.push_back({id, counts[id]});
                std::stable_sort(order.begin(), order.end(), [](const auto& l, const auto& r) {
                    return l.second > r.second;
                });
                QJsonArray biomes;
                for (const auto& e : order)
                {
                    QJsonObject b;
                    b["biome"] = biomeName(w, e.first);
                    b["cells"] = double(e.second);
                    b["share"] = sum ? std::round(10000.0 * e.second / sum) / 10000.0 : 0.0;
                    biomes.append(b);
                }
                QJsonObject s;
                s["seed"] = seedText(seeds[i]);
                s["cells"] = double(sum);
                s["biome_count"] = int(order.size());
                s["biomes"] = biomes;
                perSeed.append(s);
                call->progress(double(i + 1), double(seeds.size()));
            }
            World shown = w;
            QJsonObject o = worldJson(shown);
            o.remove("seed");
            o["scale"] = scale;
            o["y"] = task.wi.y;
            o["x1"] = task.dat.x1 * scale;
            o["z1"] = task.dat.z1 * scale;
            o["x2"] = (task.dat.x2 + 1) * scale - 1;
            o["z2"] = (task.dat.z2 + 1) * scale - 1;
            o["sampled"] = samples < cells;
            o["seeds"] = perSeed;
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- structure_stats
    {
        Tool t;
        t.name = "structure_stats";
        t.title = "Structure statistics";
        t.description = "How many structures of each type generate in an area, for one or "
            "more seeds (the Structures tool's analysis).";
        QJsonObject p = worldSchemaProperties(false);
        p["seed"] = seedProp("A seed (or use seeds).");
        QJsonObject seeds = prop("array", QString("Several seeds (at most %1).").arg(MAX_STAT_SEEDS));
        seeds["items"] = QJsonObject{ { "type", QJsonArray{ "string", "integer" } } };
        p["seeds"] = seeds;
        const QJsonObject area = areaProps(true);
        for (auto it = area.begin(); it != area.end(); ++it)
            p[it.key()] = it.value();
        QJsonObject types = prop("array", "Structure types (default: all of the dimension).");
        types["items"] = QJsonObject{ { "type", "string" } };
        p["types"] = types;
        p["include_unknown"] = prop("boolean", "Datapack worlds: also count starts that cannot "
            "be decided (default false).");
        t.schema = schemaObject(p, {});
        t.run = [this](const QJsonObject& args, McpCall *call) {
            World w = parseWorld(this, args, false);
            std::vector<uint64_t> seeds;
            if (present(args, "seed"))
                seeds.push_back(parseSeed(args.value("seed"), "seed"));
            if (present(args, "seeds"))
            {
                const QJsonValue v = args.value("seeds");
                if (!v.isArray() || v.toArray().size() > MAX_STAT_SEEDS)
                    throw McpToolError{QString("seeds must be a list of at most %1 seeds.").arg(MAX_STAT_SEEDS)};
                for (const QJsonValue& s : v.toArray())
                    seeds.push_back(parseSeed(s, "a seed of seeds"));
            }
            if (seeds.empty() || seeds.size() > size_t(MAX_STAT_SEEDS))
                throw McpToolError{"Give a seed or seeds."};
            const Area a = parseArea(args, MAX_STRUCT_AREA_SIDE, 2048);
            const QStringList names = stringList(args, "types", 200);
            std::vector<int> opts;
            std::vector<char> mask;
            if (w.datapack())
                mask = datapackMask(w, names);
            else
                opts = vanillaOpts(w, names, true);
            QJsonArray perSeed;
            for (size_t i = 0; i < seeds.size(); i++)
            {
                checkCancel(call);
                World ws = w;
                ws.wi.seed = seeds[i];
                QJsonObject counts;
                if (w.datapack())
                {
                    for (const Found& f : datapackStructures(ws, a, mask,
                            argBool(args, "include_unknown", false), call, false))
                        counts[f.type] = counts.value(f.type).toInt() + 1;
                }
                else
                {
                    std::map<int, uint64_t> c;
                    vanillaStructures(ws, a, opts, false, call, &c, false);
                    for (const auto& e : c)
                        counts[structId(e.first)] = double(e.second);
                }
                QJsonObject s;
                s["seed"] = seedText(seeds[i]);
                s["counts"] = counts;
                perSeed.append(s);
                call->progress(double(i + 1), double(seeds.size()));
            }
            QJsonObject o = worldJson(w);
            o.remove("seed");
            o["x1"] = a.x1;
            o["z1"] = a.z1;
            o["x2"] = a.x2;
            o["z2"] = a.z2;
            o["seeds"] = perSeed;
            return result(o);
        };
        tools.push_back(t);
    }
}
