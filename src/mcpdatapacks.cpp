// The datapack tools of the MCP server: load_datapacks, datapack_info and
// unload_datapacks. The packs of a client are its own: they are loaded for
// the session only and never enter the app's pack list or settings. The
// vanilla data comes from where the app finds it (the file the user chose,
// the Minecraft installation, the app's download cache); the server never
// downloads it.

#include "mcptools.h"

#include "mcpserver.h"
#include "datapackworld.h"
#include "dpmap.h"
#include "vanilladata.h"
#include "seedatlas-engine/util.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

using namespace mcp;

namespace {

QJsonObject prop(const char *type, const QString& description)
{
    QJsonObject o;
    o["type"] = type;
    o["description"] = description;
    return o;
}

const char *severityName(dp::Issue::Severity s)
{
    switch (s)
    {
    case dp::Issue::Error:      return "error";
    case dp::Issue::Warning:    return "warning";
    default:                    return "info";
    }
}

QJsonObject reportJson(const dp::Report& report, int max)
{
    QJsonArray issues;
    int errors = 0, warnings = 0;
    for (const dp::Issue& i : report.issues)
    {
        errors += i.severity == dp::Issue::Error;
        warnings += i.severity == dp::Issue::Warning;
        if (issues.size() >= max)
            continue;
        QJsonObject o;
        o["severity"] = severityName(i.severity);
        if (!i.pack.isEmpty())
            o["pack"] = i.pack;
        if (!i.id.isEmpty())
            o["id"] = i.id;
        o["text"] = i.text;
        issues.append(o);
    }
    QJsonObject o;
    o["ok"] = report.ok();
    o["errors"] = errors;
    o["warnings"] = warnings;
    o["issues"] = issues;
    o["truncated"] = int(report.issues.size()) > max;
    return o;
}

QJsonArray dimensionsJson(const dp::World& world)
{
    QJsonArray list;
    for (const dp::DimensionInfo& d : world.dimensions())
    {
        QJsonObject o;
        o["id"] = d.id;
        o["supported"] = d.supported;
        if (!d.supported)
            o["reason"] = d.reason;
        o["min_y"] = d.minY;
        o["height"] = d.height;
        o["sea_level"] = d.seaLevel;
        o["heights"] = d.terrain;
        if (!d.terrain && !d.terrainReason.isEmpty())
            o["heights_reason"] = d.terrainReason;
        o["ceiling"] = d.hasCeiling;
        o["biome_count"] = int(d.biomes.size());
        list.append(o);
    }
    return list;
}

QJsonObject packsSummary(const McpDatapacks& p)
{
    QJsonObject o;
    o["world"] = p.id;
    o["mc"] = mc2str(p.mc);
    o["world_type"] = p.worldType == WORLD_LARGE_BIOMES ? "large_biomes"
        : p.worldType == WORLD_AMPLIFIED ? "amplified" : "default";
    o["vanilla_data"] = p.vanillaSource;
    QJsonArray packs;
    if (p.world)
    {
        for (const dp::PackInfo& info : p.world->packs())
        {
            QJsonObject e;
            e["path"] = info.path;
            e["name"] = info.name;
            if (!info.description.isEmpty())
                e["description"] = info.description;
            e["pack_format"] = info.packFormat;
            e["format_matches"] = info.formatMatches;
            if (!info.overlays.isEmpty())
                e["overlays"] = QJsonArray::fromStringList(info.overlays);
            e["worldgen_files"] = info.worldgenFiles;
            packs.append(e);
        }
    }
    o["packs"] = packs;
    return o;
}

bool testRun()
{
    return !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_DIR")
        || !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_ID");
}

// Where the app takes the vanilla data of a version from. Test runs (with
// settings of their own) can name it with SEED_ATLAS_TEST_VANILLA.
QString vanillaData(int mc, QString *source)
{
    const QString test = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_VANILLA"));
    if (testRun() && !test.isEmpty())
    {
        *source = "test data (SEED_ATLAS_TEST_VANILLA)";
        return test;
    }
    VanillaData vanilla;
    const VanillaSource found = vanilla.find(mc);
    switch (found.kind)
    {
    case VanillaSource::Chosen:
        *source = "the file chosen in the app";
        break;
    case VanillaSource::Installation:
        *source = QString("the Minecraft installation (%1)").arg(found.detail);
        break;
    case VanillaSource::Downloaded:
        *source = "the app's download cache";
        break;
    case VanillaSource::None:
        break;
    }
    return found.path;
}

} // namespace

void McpRegistry::addDatapackTools()
{
    // ---------------------------------------------------------- load_datapacks
    {
        Tool t;
        t.name = "load_datapacks";
        t.title = "Load datapacks";
        t.description = "Loads datapacks or mod jars (zip, jar or folder; local paths, read "
            "only) for a Minecraft version and returns a world handle for the other tools "
            "(\"world\" argument), with the load report: errors, warnings, unsupported types. "
            "The packs belong to this MCP session only. The game's vanilla data is taken from "
            "where the app finds it (the file chosen in the app, the Minecraft installation or "
            "the app's download cache); if it is missing, get it in the app (Datapacks).";
        QJsonObject p;
        QJsonObject paths = prop("array", "Absolute paths of the packs in load order (later "
            "packs override earlier ones).");
        paths["items"] = QJsonObject{ { "type", "string" } };
        paths["minItems"] = 1;
        paths["maxItems"] = MAX_PACKS;
        p["paths"] = paths;
        p["mc"] = prop("string", "Minecraft version, e.g. \"1.21.1\" (see datapack_versions of "
            "server_info).");
        QJsonObject wt = prop("string", "World preset for dimensions that no pack replaces "
            "(default \"default\").");
        wt["enum"] = QJsonArray{ "default", "large_biomes", "amplified" };
        p["world_type"] = wt;
        t.schema = schemaObject(p, { "paths", "mc" });
        t.run = [this](const QJsonObject& args, McpCall *call) {
            QJsonObject wargs;
            wargs["mc"] = args.value("mc");
            if (args.contains("world_type"))
                wargs["world_type"] = args.value("world_type");
            World w = parseWorld(this, wargs, false);
            const int mc = w.wi.mc;
            if (!dp::versionSupported(mc) || VanillaData::gameVersion(mc).isEmpty())
                throw McpToolError{QString("Datapack worlds are not available for Minecraft %1 "
                    "(server_info lists the versions).").arg(mc2str(mc))};
            const QJsonValue pv = args.value("paths");
            if (!pv.isArray() || pv.toArray().isEmpty() || pv.toArray().size() > MAX_PACKS)
                throw McpToolError{QString("paths must be a list of 1 to %1 paths.").arg(MAX_PACKS)};
            QStringList paths;
            for (const QJsonValue& v : pv.toArray())
            {
                const QString path = v.toString();
                if (!v.isString() || path.isEmpty() || path.size() > 4096)
                    throw McpToolError{"Each path must be a non-empty string."};
                if (!QDir::isAbsolutePath(path))
                    throw McpToolError{QString("\"%1\" is not an absolute path.").arg(path)};
                const QFileInfo info(path);
                if (!info.exists())
                    throw McpToolError{QString("\"%1\" does not exist.").arg(path)};
                if (!isDatapackPath(path))
                    throw McpToolError{QString("\"%1\" is not a datapack: use a zip or jar file "
                        "or a folder.").arg(path)};
                const QString canonical = info.canonicalFilePath();
                if (paths.contains(canonical))
                    throw McpToolError{QString("\"%1\" is listed twice.").arg(path)};
                paths.append(canonical);
            }
            {
                QMutexLocker lock(&mutex);
                if (datapacks.size() >= MAX_DATAPACK_WORLDS)
                    throw McpToolError{QString("%1 datapack worlds are loaded: unload one first.")
                        .arg(MAX_DATAPACK_WORLDS)};
            }
            QString source;
            const QString vanilla = vanillaData(mc, &source);
            if (vanilla.isEmpty())
            {
                const QString game = VanillaData::gameVersion(mc);
                throw McpToolError{VanillaData::dataInGameFiles(mc)
                    ? QString("The game data of Minecraft %1 is missing. Open Seed Atlas, then "
                        "Datapacks: choose the game's %1.jar or download it there. The MCP server "
                        "never downloads it.").arg(game)
                    : QString("The game data of Minecraft %1 is missing. The game's files do not "
                        "contain it: choose the folder that its data generator writes in Seed "
                        "Atlas (Datapacks).").arg(game)};
            }
            std::shared_ptr<McpDatapacks> packs = std::make_shared<McpDatapacks>();
            packs->mc = mc;
            packs->worldType = w.wi.worldType();
            packs->paths = paths;
            packs->vanillaSource = source;
            dp::Request request;
            request.mc = mc;
            request.vanillaPath = vanilla;
            request.preset = datapackPreset(packs->worldType);
            for (const QString& path : paths)
            {
                dp::PackSource src;
                src.path = path;
                request.packs.push_back(src);
            }
            call->progress(0, 1, "loading the datapacks");
            packs->report = std::make_shared<dp::Report>();
            packs->world = dp::World::load(request, packs->report.get());
            if (call->cancel)
                throw McpToolError{"Cancelled."};
            {
                QMutexLocker lock(&mutex);
                if (datapacks.size() >= MAX_DATAPACK_WORLDS)
                    throw McpToolError{QString("%1 datapack worlds are loaded: unload one first.")
                        .arg(MAX_DATAPACK_WORLDS)};
                packs->id = QString("dp%1").arg(nextDatapacks++);
                datapacks.insert(packs->id, packs);
            }
            QJsonObject o = packsSummary(*packs);
            o["report"] = reportJson(*packs->report, 200);
            o["dimensions"] = dimensionsJson(*packs->world);
            o["biome_count"] = int(packs->world->biomes().size());
            int supported = 0, total = 0;
            QJsonArray unsupported;
            for (const dp::StructureInfo& s : packs->world->structures())
            {
                total++;
                supported += s.supported;
                if (!s.supported && unsupported.size() < 100)
                {
                    QJsonObject e;
                    e["id"] = s.id;
                    e["type"] = s.type;
                    e["reason"] = s.reason;
                    unsupported.append(e);
                }
            }
            o["structure_count"] = total;
            o["structures_supported"] = supported;
            o["unsupported_structures"] = unsupported;
            return result(o, QString("Loaded %1 pack(s) for %2 as world \"%3\"%4.")
                .arg(paths.size()).arg(mc2str(mc)).arg(packs->id)
                .arg(packs->report->ok() ? "" : " (with errors: see report)"));
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- datapack_info
    {
        Tool t;
        t.name = "datapack_info";
        t.title = "Datapack world details";
        t.description = "The dimensions, biomes, structures and packs of a datapack world "
            "handle, and its load report.";
        QJsonObject p;
        p["world"] = prop("string", "The handle from load_datapacks.");
        p["dimension"] = prop("string", "List only the biomes this dimension can generate.");
        t.schema = schemaObject(p, { "world" });
        t.run = [this](const QJsonObject& args, McpCall *) {
            const QString id = argString(args, "world");
            std::shared_ptr<McpDatapacks> packs = findDatapacks(id);
            if (!packs)
                throw McpToolError{QString("There is no datapack world \"%1\".").arg(id)};
            QJsonObject o = packsSummary(*packs);
            o["report"] = reportJson(*packs->report, 1000);
            o["dimensions"] = dimensionsJson(*packs->world);
            o["presets"] = QJsonArray::fromStringList(packs->world->presets());
            const std::vector<dp::BiomeInfo>& biomes = packs->world->biomes();
            std::vector<char> shown(biomes.size(), 1);
            const QString dim = argString(args, "dimension");
            if (!dim.isEmpty())
            {
                QString did = dim.contains(':') ? dim : "minecraft:" + dim;
                if (did == "minecraft:nether") did = "minecraft:the_nether";
                if (did == "minecraft:end") did = "minecraft:the_end";
                const int d = packs->world->dimension(did);
                if (d < 0)
                    throw McpToolError{QString("The world has no dimension \"%1\".").arg(dim)};
                std::fill(shown.begin(), shown.end(), 0);
                for (int b : packs->world->dimensions()[size_t(d)].biomes)
                    if (b >= 0 && b < int(shown.size()))
                        shown[size_t(b)] = 1;
            }
            QJsonArray blist;
            for (size_t i = 0; i < biomes.size(); i++)
            {
                if (!shown[i])
                    continue;
                QJsonObject b;
                b["id"] = biomes[i].id;
                b["name"] = biomes[i].name;
                b["vanilla"] = biomes[i].vanilla;
                blist.append(b);
            }
            o["biomes"] = blist;
            QJsonArray slist;
            for (const dp::StructureInfo& s : packs->world->structures())
            {
                QJsonObject e;
                e["id"] = s.id;
                e["name"] = s.name;
                e["type"] = s.type;
                if (!s.set.isEmpty())
                    e["set"] = s.set;
                if (!s.pack.isEmpty())
                    e["pack"] = s.pack;
                e["supported"] = s.supported;
                if (!s.supported)
                    e["reason"] = s.reason;
                slist.append(e);
            }
            o["structures"] = slist;
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- unload_datapacks
    {
        Tool t;
        t.name = "unload_datapacks";
        t.title = "Unload datapacks";
        t.description = "Forgets a datapack world handle and frees its memory. Searches that "
            "run in it are stopped.";
        QJsonObject p;
        p["world"] = prop("string", "The handle from load_datapacks.");
        t.schema = schemaObject(p, { "world" });
        t.run = [this](const QJsonObject& args, McpCall *) {
            const QString id = argString(args, "world").trimmed();
            std::shared_ptr<McpDatapacks> packs;
            {
                QMutexLocker lock(&mutex);
                packs = datapacks.take(id);
            }
            if (!packs)
                throw McpToolError{QString("There is no datapack world \"%1\".").arg(id)};
            stopSearchesIn(id);
            // the finders of the map's pool keep the world alive
            dpClearFinderPool();
            QJsonObject o;
            o["world"] = id;
            o["unloaded"] = true;
            return result(o);
        };
        tools.push_back(t);
    }
}
