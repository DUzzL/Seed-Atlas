// Tests of the datapack world generator (src/datapack) against the native
// engine and against data of real vanilla servers.
//
// Usage: dptest <command> [arguments]
//
//   tree <biome_parameters report .json> [mc]
//       The R-tree alone: feeds a parameter list in game order (the official
//       data generator report generated/reports/biome_parameters/minecraft/
//       overworld.json of that version) through the generic builder and
//       compares the lookups with the engine's native tree. 1.18.2 has no
//       such report: its data generator's dimension/overworld.json (in the
//       1.18.2-data branch of misode/mcmeta) holds the list in game order and
//       serves as well, also for --report below.
//   vanilla <vanilla data> <mc> [--report <report .json> --work <dir>]
//           [--server <tools/worldgen-reference/data>] [--quick]
//       The vanilla data pack of a version through the generic path, for
//       the presets normal, large_biomes and amplified: climate values and
//       biomes at the scales 1 and 4 against the engine, the larger scales
//       against the 1:4 cells, that the biome of a cell only depends on its
//       position, and the biome cells of real servers. With --report the
//       same again with the vanilla list as a pack (the generic tree).
//   pack <vanilla data> <mc> --pack <pack> [...] <server file> ...
//       A data pack against the biome cells of a server that ran it.
//   terrain <vanilla data folder> <mc> <work dir> [--server <dir>]
//       Density function types outside the climate (old_blended_noise, the
//       cave functions) against the engine's terrain and server heightmaps.
//   heights <vanilla data> <mc> [--pack <pack>]... [--every N] [--list N]
//           [--min-exact <percent>] [--max-fluid N] [--biome-tolerance N] <server file> ...
//       Terrain heights (H lines; the Nether's floor, F lines) and biome
//       cells (B lines) of server files of any dimension (# dimension)
//       against the generic path; for the built-in overworld also the
//       engine's heights. The H lines are of finished chunks (trees,
//       carvers, structures): the exact share and tolerances are printed.
//       --max-fluid: at most N columns per file whose ocean floor the
//       server has higher under a fluid (the aquifers' class; for files of
//       worlds without features, carvers and structures, # terrain_only).
//   aqperf <vanilla data> <mc> [--pack <pack>]...
//       Microseconds per preliminary surface of the aquifers, with the
//       bounds and with every height scanned (the same values).
//   hperf <vanilla data> <mc> [--pack <pack>]... [--compare] [--surface] [--far]
//       Microseconds per column of genHeights at 1:1, 1:4, 1:16 for the
//       overworld (normal, amplified), the Nether and the End; --compare:
//       every height against a scan without bounds (setTerrainPruning);
//       --surface: WORLD_SURFACE instead of OCEAN_FLOOR.
//   robust <vanilla data> <mc> <work dir>
//       Broken packs (each must give an error naming the pack and the
//       resource), overlays, tags, pack order, colours, fixed and
//       checkerboard biome sources.
//   same <vanilla data> <mc> <pack> <the same pack in another form> ...
//       A pack as folder, zip and jar must give the same world.
//   zip <archive>
//       Unpacks and checks (CRC-32) every entry of a zip or jar file.
//   truncate <archive> <work dir> [<vanilla data> <mc>]
//       Cut off and damaged copies of an archive: errors, never a crash or
//       wrong data.
//   perf <vanilla data> <mc> [--report <report .json> --work <dir>] [pack ...]
//       Load time, sampler creation, microseconds per sample.
//   cell <vanilla data> <mc> <seed> <x> <y> <z> [pack ...]
//       Prints the climate and biome of one cell, next to the engine's.
//   structures <vanilla data> <mc> [--pack <pack> ...] [--engine] [--map] <structure file> ...
//       Structure starts of real servers (tools/worldgen-reference/data/
//       structures_*.txt): every start must be a candidate of the same
//       structure that the map calls valid or unknown, jigsaw start
//       positions must match; counts the valid candidates without a server
//       start. --engine: the placement of the vanilla sets against the
//       engine (getStructurePos, mineshafts, the stronghold positions).
//       --map: the finder as the map uses it (setHeightsOfInvalid(false)).
//   starts <vanilla data> <mc> <seed> <chunk x0> <chunk z0> <chunk x1> <chunk z1>
//          [--pack <pack> ...] [--preset <id>] [--structure <id>]
//       Lists the structure start candidates of an area.
//   structcheck <vanilla data> <mc> <seed> [radius] [--dimension <id>]
//       A version's vanilla data without server data: no warnings, all sets
//       and structures supported, placement equal to the engine's (the
//       overworld), and the candidates (with their heights) the same for an
//       area and its parts in any order, and with a filter for one structure.
//   structperf <vanilla data> <mc> [--pack <pack> ...] [--dimension <id>] [--seed <n>] [--size <n>]
//              [--terrain-new] [--map]
//       Milliseconds per StructureFinder::starts() of map viewports (32 x 32
//       to 512 x 512 chunks): a new finder per query and one finder panning
//       over neighbouring areas, with the terrain heights and without them
//       (the checks that need a height left unknown). --terrain-new: only
//       the first case; --map: the finder as the map uses it.
//   structrobust <vanilla data> <mc> <work dir>
//       Broken and unknown structure data (a mod's placement and structure
//       types, missing references, an exclusion cycle, a damaged template):
//       warnings naming pack and resource, never an error.
//   limits <vanilla data> <mc> <work dir> [case ...]
//       Crafted packs and archives (made by the test) against the loader's
//       limits: zip64 offsets that wrap, overlapping entries, files that
//       unpack to too much data, an archive cut off while it is open, the
//       CRC table from several threads, density functions and biome tags
//       nested too deeply (loaded and evaluated on threads with a 512 KiB
//       stack, as in the app), interval_select with 100,000 thresholds,
//       dimension types and noise settings outside the game's ranges, the
//       checkerboard source's int shifts, find_top_surface's int arithmetic
//       and abort, structure codec ranges, jigsaw dimension_padding. Cases:
//       crc zip budget truncated nesting tags tagfan isel ranges checker fts
//       structures padding interp (all by default; each checks what its
//       version has). DPTEST_STACK_KIB sets the threads' stack size.
//   graphdepth <vanilla data> <mc> [pack ...]
//       The highest density function of a world and the compiler's deepest
//       recursion, against the limits.
//   areas <vanilla data> <mc> <seed> [--pack <pack>]... [--dimension <id>] [--preset <id>]
//         [--radius <blocks>] [--count <n>] [--size <chunks>] [--quiet]
//       Picks areas for the reference data of a pack world (a line of a
//       worlds file of worldgen_reference.py): the area origin and the
//       areas where the generator expects the most biomes of the dimension,
//       those of the packs first. Only a choice of places: the comparison is
//       with what the server generated there.
//   packcheck <vanilla data> <mc> --pack <pack>... [--preset <id>] [--timing] <reference file> ...
//       Heights/biomes files and structure files of real servers against the
//       generator, like heights and structures, but the output holds counts,
//       shares and times only (no ids, names, coordinates or values of the
//       packs): for packs that must not be shown
//       (tools/worldgen-reference/check_pack.py).
//   mcmeta <work dir> <mcmeta file> ... [--vanilla <vanilla data> <mc>]
//       pack.mcmeta of probe packs on real servers (tools/worldgen-reference/
//       data/mcmeta_<mc>.txt): whether the game loads each pack and which
//       overlays it applies, against the loader's rules at the server's
//       pack format; --vanilla: the file of that version once more through
//       the whole loader.
//
// <vanilla data>: a folder or zip/jar with data/minecraft/worldgen/... of the
// version (the game's jar, or the <version>-data branch of misode/mcmeta).
// No game data is part of the repository. Every command exits with a
// non-zero status if a check fails.

#include "dpclimate.h"
#include "dpworld_p.h"
#include "dpzip.h"
#include "dpstructures.h"

extern "C"
{
#include "finders.h"
#include "generator.h"
#include "layers.h"
#include "util.h"
}

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <climits>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

static int g_fail;
static bool g_chainOnly;   // pack: the server files only give the chunk positions

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; \
    printf("FAILED %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

//==============================================================================
// tree
//==============================================================================

struct ParamList
{
    std::vector<dp::ClimateEntry> entries;
    QStringList biomes; // per entry
};

static bool readSpan(const QJsonValue& v, int64_t *lo, int64_t *hi)
{
    if (v.isDouble())
    {
        *lo = *hi = dp::quantizeClimate((float) v.toDouble());
        return true;
    }
    QJsonArray a = v.toArray();
    if (a.size() != 2)
        return false;
    *lo = dp::quantizeClimate((float) a[0].toDouble());
    *hi = dp::quantizeClimate((float) a[1].toDouble());
    return true;
}

// The entries of a biome_parameters report, or of a dimension file with an
// inline list (1.18.2 has no such report; the dimension files that its data
// generator writes have the list in game order).
static QJsonArray reportBiomes(const QJsonObject& o)
{
    if (o.contains("biomes"))
        return o.value("biomes").toArray();
    return o.value("generator").toObject().value("biome_source").toObject().value("biomes").toArray();
}

static bool loadParamReport(const QString& path, ParamList *list)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        printf("cannot read %s\n", qPrintable(path));
        return false;
    }
    QJsonArray biomes = reportBiomes(QJsonDocument::fromJson(f.readAll()).object());
    static const char *keys[] = {
        "temperature", "humidity", "continentalness", "erosion", "depth",
        "weirdness", "offset"
    };
    for (const QJsonValue& bv : biomes)
    {
        QJsonObject o = bv.toObject();
        QJsonObject p = o.value("parameters").toObject();
        dp::ClimateEntry e;
        for (int d = 0; d < dp::CLIMATE_DIMS; d++)
            if (!readSpan(p.value(keys[d]), &e.lo[d], &e.hi[d]))
                return false;
        list->entries.push_back(e);
        list->biomes.push_back(o.value("biome").toString());
    }
    return !list->entries.empty();
}

static int cmdTree(const QStringList& args)
{
    if (args.size() < 1)
    {
        printf("usage: dptest tree <report.json> [mc]\n");
        return 2;
    }
    int mc = args.size() > 1 ? str2mc(qPrintable(args[1])) : MC_1_21_1;
    if (mc < MC_1_18)
    {
        printf("unknown or unsupported version\n");
        return 2;
    }
    ParamList list;
    if (!loadParamReport(args[0], &list))
        return 2;

    int arity = dp::climateArity(mc);
    dp::ClimateTree tree;
    QElapsedTimer timer;
    timer.start();
    CHECK(tree.build(list.entries, arity), "build failed");
    printf("tree: %zu entries, arity %d, built in %.1f ms\n",
        list.entries.size(), arity, timer.nsecsElapsed() / 1e6);

    // the engine's biome ids of the entries
    std::vector<int> entryId(list.entries.size(), -1);
    for (int id = 0; id < 256; id++)
    {
        const char *s = biome2str(mc, id);
        if (!s)
            continue;
        QString name = QStringLiteral("minecraft:") + s;
        for (int i = 0; i < list.biomes.size(); i++)
            if (list.biomes[i] == name)
                entryId[i] = id;
    }
    for (int i = 0; i < list.biomes.size(); i++)
        CHECK(entryId[i] >= 0, "biome %s unknown to the engine", qPrintable(list.biomes[i]));

    // From 26.2 the engine's tree is not the game's (it corrects the lookup
    // of the tree of 1.21.5 for the changes): the reference is then the
    // vanilla list that the generator rebuilds in game order (dplistorder.h),
    // which must be the report's list, entry by entry.
    const bool listRef = mc >= MC_26_2;
    dp::ClimateTree refTree;
    std::vector<int32_t> refIds;
    if (listRef)
    {
        std::vector<dp::ClimateEntry> ref;
        CHECK(dp::nativeOverworldList(mc, &ref, &refIds), "no built-in list");
        int differ = 0;
        for (size_t i = 0; i < ref.size() && i < list.entries.size(); i++)
        {
            bool same = refIds[i] == entryId[i];
            for (int d = 0; d < dp::CLIMATE_DIMS; d++)
                same = same && ref[i].lo[d] == list.entries[i].lo[d] && ref[i].hi[d] == list.entries[i].hi[d];
            if (!same)
                differ++;
        }
        CHECK(ref.size() == list.entries.size() && differ == 0,
            "the built-in list differs from the report: %zu / %zu entries, %d differ",
            ref.size(), list.entries.size(), differ);
        printf("built-in list in game order: %zu entries, %d differ from the report\n", ref.size(), differ);
        refTree.build(ref, arity);
    }
    // the reference lookup: the engine, or the rebuilt list (cursor in dat,
    // 0 for none)
    auto reference = [&](const int64_t np[6], uint64_t *dat) -> int {
        if (!listRef)
            return climateToBiome(mc, (const uint64_t*) np, dat);
        int cursor = dat ? (int) *dat - 1 : -1;
        int e = refTree.find(np, &cursor);
        if (dat)
            *dat = (uint64_t) (cursor + 1);
        return e >= 0 ? refIds[e] : -1;
    };
    const char *refName = listRef ? "the rebuilt list" : "the engine";

    std::mt19937_64 rng(20261115);
    auto rnd = [&rng](int64_t lo, int64_t hi) {
        return lo + (int64_t) (rng() % (uint64_t) (hi - lo + 1));
    };

    // Three kinds of points: uniform in a box wider than all parameters;
    // inside or on the border of a random entry; and near the borders of a
    // random entry (off by -1, 0, +1), which gives many exact ties.
    auto makePoint = [&](int kind, int64_t np[6]) {
        if (kind == 0)
        {
            for (int d = 0; d < 6; d++)
                np[d] = rnd(-13000, 13000);
            return;
        }
        const dp::ClimateEntry& e = list.entries[rng() % list.entries.size()];
        for (int d = 0; d < 6; d++)
        {
            if (kind == 1)
                np[d] = rnd(e.lo[d], e.hi[d]);
            else
            {
                switch (rng() % 4)
                {
                case 0: np[d] = e.lo[d] + rnd(-1, 1); break;
                case 1: np[d] = e.hi[d] + rnd(-1, 1); break;
                case 2: np[d] = rnd(e.lo[d], e.hi[d]); break;
                default: np[d] = rnd(-13000, 13000); break;
                }
            }
        }
    };

    static const char *kindName[] = { "uniform", "inside an entry", "near entry borders" };
    const int N = 2000000;

    // 1. fresh cursor per query
    for (int kind = 0; kind < 3; kind++)
    {
        int64_t diff = 0, ties = 0, engineDiff = 0;
        for (int i = 0; i < N; i++)
        {
            int64_t np[6];
            makePoint(kind, np);
            int cursor = -1;
            int e = tree.find(np, &cursor);
            int id = reference(np, NULL);
            if (listRef && entryId[e] != climateToBiome(mc, (const uint64_t*) np, NULL))
                engineDiff++;
            if (entryId[e] != id)
            {
                if (diff < 5)
                    printf("  differs at %lld %lld %lld %lld %lld %lld: %s, engine %s\n",
                        (long long) np[0], (long long) np[1], (long long) np[2],
                        (long long) np[3], (long long) np[4], (long long) np[5],
                        qPrintable(list.biomes[e]), biome2str(mc, id));
                diff++;
            }
            // how many points have a second entry at the same distance with
            // another biome is counted with a brute force scan on a sample
            if (i % 200 == 0)
            {
                int64_t dmin = tree.distance(e, np);
                for (size_t k = 0; k < list.entries.size(); k++)
                {
                    if (entryId[k] != entryId[e] && tree.distance((int) k, np) == dmin)
                    {
                        ties++;
                        break;
                    }
                    CHECK(tree.distance((int) k, np) >= dmin, "entry %zu is closer than the result", k);
                }
            }
        }
        printf("fresh cursor, %s: %d points, %lld differ from %s "
            "(%lld of %d brute-force checked points tie between two biomes)\n",
            kindName[kind], N, (long long) diff, refName, (long long) ties, N / 200);
        if (listRef)
            printf("  (%lld differ from the engine, which does not build the game's tree for this version)\n",
                (long long) engineDiff);
        CHECK(diff == 0, "fresh lookups differ from %s", refName);
    }

    // 2. cursor carried from one query to the next, on both sides. The
    // engine's hint is the index of a node of its own tree; it starts from
    // the leaf of a first lookup.
    for (int kind = 0; kind < 3; kind++)
    {
        int64_t diff = 0, hist = 0;
        int cursor = -1;
        uint64_t dat = 0;
        bool first = true;
        int64_t prev[6] = {};
        for (int i = 0; i < N; i++)
        {
            int64_t np[6];
            // walk: most points stay close to the one before, so that the
            // previous result often is still one of the nearest entries
            if (!first && rng() % 8 != 0)
            {
                for (int d = 0; d < 6; d++)
                    np[d] = prev[d] + (rng() % 3 == 0 ? rnd(-2, 2) : 0);
            }
            else
            {
                makePoint(kind, np);
            }
            memcpy(prev, np, sizeof(prev));
            int fresh = -1;
            int ef = tree.find(np, &fresh);
            int e = tree.find(np, &cursor);
            int id = reference(np, &dat);
            if (first)
            {
                // the engine's first lookup starts at its root; from here on
                // both sides hold the same leaf if they agree
                first = false;
                if (entryId[e] != id)
                    diff++;
                continue;
            }
            if (entryId[e] != id)
            {
                if (diff < 5)
                    printf("  carried differs at %lld %lld %lld %lld %lld %lld: %s, engine %s\n",
                        (long long) np[0], (long long) np[1], (long long) np[2],
                        (long long) np[3], (long long) np[4], (long long) np[5],
                        qPrintable(list.biomes[e]), biome2str(mc, id));
                diff++;
            }
            if (entryId[e] != entryId[ef])
                hist++;
        }
        printf("carried cursor, %s walk: %d points, %lld differ from %s, "
            "%lld depend on the previous lookup\n",
            kindName[kind], N, (long long) diff, refName, (long long) hist);
        CHECK(diff == 0, "carried lookups differ from %s", refName);
    }

    // The engine's hint 0 (its root node) must behave like no history: the
    // sampler resets its cursor that way.
    {
        int64_t diff = 0;
        for (int i = 0; i < N; i++)
        {
            int64_t np[6];
            makePoint(i % 3, np);
            uint64_t dat = 0;
            int a = climateToBiome(mc, (const uint64_t*) np, &dat);
            int b = climateToBiome(mc, (const uint64_t*) np, NULL);
            if (a != b)
                diff++;
        }
        printf("engine with hint 0 against engine without hint: %lld of %d differ\n", (long long) diff, N);
        CHECK(diff == 0, "the engine's hint 0 is not a search without history");
    }

    // speed of the generic search
    {
        std::vector<int64_t> pts((size_t) N * 6);
        for (int i = 0; i < N; i++)
            makePoint(1, &pts[(size_t) i * 6]);
        int64_t sum = 0;
        timer.restart();
        for (int i = 0; i < N; i++)
        {
            int cursor = -1;
            sum += tree.find(&pts[(size_t) i * 6], &cursor);
        }
        double a = timer.nsecsElapsed() / 1e3 / N;
        timer.restart();
        for (int i = 0; i < N; i++)
            sum += climateToBiome(mc, (const uint64_t*) &pts[(size_t) i * 6], NULL);
        double b = timer.nsecsElapsed() / 1e3 / N;
        printf("random lookups without history: generic %.3f us, engine %.3f us (%lld)\n",
            a, b, (long long) sum);
    }
    return g_fail != 0;
}


//==============================================================================
// helpers
//==============================================================================

static void printReport(const dp::Report& report)
{
    static const char *sev[] = { "info", "warning", "ERROR" };
    for (const dp::Issue& i : report.issues)
        printf("  [%s] %s%s%s: %s\n", sev[i.severity], qPrintable(i.pack),
            i.pack.isEmpty() || i.id.isEmpty() ? "" : " / ", qPrintable(i.id), qPrintable(i.text));
}

static std::shared_ptr<const dp::World> loadWorld(int mc, const QString& vanilla,
    const QStringList& packs, const QString& preset, dp::Report *report, bool print = true)
{
    dp::Request req;
    req.mc = mc;
    req.vanillaPath = vanilla;
    for (const QString& p : packs)
    {
        dp::PackSource ps;
        ps.path = p;
        req.packs.push_back(ps);
    }
    req.preset = preset;
    std::shared_ptr<const dp::World> w = dp::World::load(req, report);
    if (print)
        printReport(*report);
    // a pack the game would not load is left out (with a warning only): a
    // test with it would silently run on the vanilla world
    for (const dp::PackInfo& p : w ? w->packs() : std::vector<dp::PackInfo>())
        CHECK(p.loaded, "the pack %s is not loaded (pack.mcmeta), the test would run without it",
            qPrintable(p.name));
    return w;
}

// engine biome id of every biome of a world, -1 if the engine has none
static std::vector<int> engineIds(const dp::World& w, int mc)
{
    std::vector<int> ids(w.biomes().size(), -1);
    for (int id = 0; id < 256; id++)
    {
        const char *s = biome2str(mc, id);
        if (!s)
            continue;
        QString name = QStringLiteral("minecraft:") + s;
        for (size_t i = 0; i < w.biomes().size(); i++)
            if (w.biomes()[i].id == name)
                ids[i] = id;
    }
    return ids;
}

struct Counter
{
    int64_t n = 0, diff = 0;
    void add(bool same) { n++; if (!same) diff++; }
    double rate() const { return n ? 100.0 * (double) diff / (double) n : 0.0; }
};

// Writes a pack that replaces the overworld by one with the given noise
// settings and an inline parameter list (the entries of a biome_parameters
// report have the format of the data pack).
static bool writeListPack(const QString& dir, const QString& reportPath, const QString& settings, int packFormat)
{
    QFile f(reportPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonArray biomes = reportBiomes(QJsonDocument::fromJson(f.readAll()).object());
    if (biomes.isEmpty())
        return false;
    QJsonObject source;
    source["type"] = "minecraft:multi_noise";
    source["biomes"] = biomes;
    QJsonObject gen;
    gen["type"] = "minecraft:noise";
    gen["settings"] = settings;
    gen["biome_source"] = source;
    QJsonObject dim;
    dim["type"] = "minecraft:overworld";
    dim["generator"] = gen;

    QDir().mkpath(dir + "/data/minecraft/dimension");
    QFile d(dir + "/data/minecraft/dimension/overworld.json");
    if (!d.open(QIODevice::WriteOnly))
        return false;
    d.write(QJsonDocument(dim).toJson(QJsonDocument::Compact));
    d.close();
    QFile m(dir + "/pack.mcmeta");
    if (!m.open(QIODevice::WriteOnly))
        return false;
    // from format 82 (1.21.9) on the game reads min_format / max_format
    m.write(QString(packFormat < 82 ? "{\"pack\":{\"pack_format\":%1,\"description\":\"vanilla list as a pack\"}}"
        : "{\"pack\":{\"min_format\":%1,\"max_format\":%1,\"description\":\"vanilla list as a pack\"}}")
        .arg(packFormat).toUtf8());
    return true;
}

static const uint64_t g_seeds[] = {
    12345ULL, (uint64_t) -42LL, (uint64_t) -4172144997902289642LL, 6027786629280759191ULL,
    0ULL, 1ULL, 0x7fffffffffffffffULL, (uint64_t) -1LL,
};

//==============================================================================
// vanilla: the vanilla data through the generic path against the engine
//==============================================================================

struct PresetCase
{
    const char *preset;
    const char *settings;
    uint32_t flags;
    const char *serverPrefix;   // files of tools/worldgen-reference, or null
};
static const PresetCase g_presets[] = {
    { "minecraft:normal", "minecraft:overworld", 0, "normal" },
    { "minecraft:large_biomes", "minecraft:large_biomes", LARGE_BIOMES, nullptr },
    { "minecraft:amplified", "minecraft:amplified", AMPLIFIED, "amplified" },
};

// Climate values of the generic evaluator against the engine's, quantised
// like the game does: all six parameters, at random cells.
static void compareClimate(const dp::World& w, int dim, int mc, uint32_t flags, const char *label)
{
    const dp::WorldPrivate *wp = w.d();
    const dp::DimData& d = wp->data[dim];
    static const char *names[6] = { "temperature", "humidity", "continentalness", "erosion", "depth", "weirdness" };
    Counter c[6];
    int64_t far = 0, clamped = 0;
    std::mt19937_64 rng(4711);
    Generator g;
    setupGenerator(&g, mc, flags);
    for (int si = 0; si < 4; si++)
    {
        uint64_t seed = g_seeds[si];
        applySeed(&g, DIM_OVERWORLD, seed);
        dp::DensityState st(*wp->graph, d.program, seed, mc, d.legacy);
        for (int i = 0; i < 200000; i++)
        {
            // within +-100000 blocks, a tenth of the samples far out
            int range = i % 10 == 0 ? 3000000 : 25000;
            int x = (int) (rng() % (2 * range)) - range;
            int z = (int) (rng() % (2 * range)) - range;
            int y = (int) (rng() % 96) - 16;
            int64_t np[6];
            sampleBiomeNoise(&g.bn, np, x, y, z, NULL, 0);
            st.evalColumn(x * 4, z * 4);
            st.evalVertical(y * 4);
            for (int k = 0; k < 6; k++)
            {
                int64_t q = dp::quantizeClimate((float) st.value(d.outputs[k]));
                bool same = q == np[k];
                if (!same && (c[k].diff < 4 || q - np[k] > 1 || np[k] - q > 1) && getenv("DPTEST_VERBOSE"))
                    printf("  %s at %d %d %d: %.17g -> %lld, engine %lld\n", names[k], x, y, z,
                        st.value(d.outputs[k]), (long long) q, (long long) np[k]);
                c[k].add(same);
                // 1.18.2 clamps the terrain shaper's offset at min_value
                // (-0.81), the engine does not: there its depth is lower
                if (mc == MC_1_18_2 && k == dp::CLIMATE_DEPTH && np[k] < q - 1)
                    clamped++;
                else if (q - np[k] > 1 || np[k] - q > 1)
                    far++;
            }
        }
    }
    printf("%s: quantised climate against the engine, %lld cells:", label, (long long) c[0].n);
    for (int k = 0; k < 6; k++)
    {
        printf(" %s %lld", names[k], (long long) c[k].diff);
        // The engine adds up the depth in single precision; the game (and
        // this generator) evaluates the density functions in double and
        // casts once. Where the depth lies on a quantisation step (constant
        // ocean offsets do) the engine's value is one off.
        // From 26.3 the game evaluates the climate in float, the engine in
        // double: a value on a quantisation step is one off now and then
        // (the servers are the reference then).
        if (k != dp::CLIMATE_DEPTH && mc < MC_26_3)
            CHECK(c[k].diff == 0, "%s: %s differs from the engine", label, names[k]);
    }
    printf(" differ (depth: by one unit, the engine's single precision sum%s)\n",
        mc >= MC_26_3 ? "; from 26.3 all by one unit: the game's float evaluation" : "");
    if (mc == MC_1_18_2)
        printf("%s: %lld depth values lower in the engine, which does not clamp the offset at -0.81 like 1.18.2\n",
            label, (long long) clamped);
    CHECK(far == 0, "%s: %lld climate values differ by more than one unit", label, (long long) far);
}

// genBiomes of the generic path against the engine's
static Counter compareArea(dp::Sampler& sampler, const std::vector<int>& ids, const Generator *g,
    int scale, int x, int z, int sx, int sz, int y, bool print = false)
{
    Counter c;
    std::vector<int> a((size_t) sx * sz);
    Range r = { scale, x, z, sx, sz, scale == 1 ? y : y >> 2, 1 };
    int *b = allocCache(g, r);
    bool ok = sampler.genBiomes(a.data(), scale, x, z, sx, sz, y);
    CHECK(ok, "genBiomes failed");
    genBiomes(g, b, r);
    for (size_t i = 0; i < a.size(); i++)
    {
        bool same = a[i] >= 0 && ids[a[i]] == b[i];
        if (!same && print && c.diff < 3)
            printf("    differs at scale %d (%d, %d, %d): %d / engine %s\n", scale,
                x + (int) (i % sx), y, z + (int) (i / sx), a[i], biome2str(g->mc, b[i]));
        c.add(same);
    }
    free(b);
    return c;
}

struct ServerFile
{
    struct Cell { int x, y, z; QString biome; };
    std::vector<Cell> cells;
    struct Area { int x0, z0, x1, z1; };
    std::vector<Area> areas;
    int minY = -64, height = 384;
};

static bool readServerFile(const QString& path, ServerFile *sf)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    while (!f.atEnd())
    {
        QByteArray line = f.readLine().trimmed();
        if (line.startsWith("# area ") && line.count(' ') >= 6)
        {
            QList<QByteArray> p = line.split(' ');
            ServerFile::Area a = { p[3].toInt(), p[4].toInt(), p[5].toInt(), p[6].toInt() };
            sf->areas.push_back(a);
        }
        else if (line.startsWith("# min_y "))
        {
            QList<QByteArray> p = line.split(' ');
            if (p.size() >= 5)
            {
                sf->minY = p[2].toInt();
                sf->height = p[4].toInt();
            }
        }
        else if (line.startsWith("B "))
        {
            QList<QByteArray> p = line.split(' ');
            if (p.size() != 5)
                continue;
            ServerFile::Cell c;
            c.x = p[1].toInt();
            c.y = p[2].toInt();
            c.z = p[3].toInt();
            c.biome = QString::fromLatin1(p[4]);
            if (!c.biome.contains(':'))
                c.biome = "minecraft:" + c.biome;
            sf->cells.push_back(c);
        }
    }
    return !sf->cells.empty();
}

// The biome cells of a real server against the generic path: genBiomes over
// the file's areas with each cursor policy, and as the reference for what the
// game does, whole chunks searched in the order in which the game fills them
// (sections bottom up; x, y, z), carrying the search's last result.
// @g: also the native engine, as the reference rate. @total: sums [chunk
// order, one cursor, no history, engine, chain cells, cells with history].
static void compareServer(const dp::World& w, int dim, int mc, uint64_t seed, const ServerFile& sf,
    const Generator *g, const char *label, Counter *total)
{
    const dp::WorldPrivate *wp = w.d();
    const dp::DimData& d = wp->data[dim];
    const std::vector<dp::BiomeInfo>& biomes = w.biomes();
    std::vector<int> ids = engineIds(w, mc);

    Counter fresh, chunk, engine, chain;
    int64_t chainTies = 0;
    Counter tile[3];
    dp::setCursorPolicy(dp::CURSOR_CHUNK);
    dp::Sampler chunkSampler(std::shared_ptr<const dp::World>(std::shared_ptr<const dp::World>(), &w), dim, seed);
    dp::DensityState st(*wp->graph, d.program, seed, mc, d.legacy);

    auto lookup = [&](const int64_t np[6], int *cursor, uint64_t *dat) -> int {
        if (d.source == dp::DimData::NATIVE_OVERWORLD)
        {
            int id = climateToBiome(mc, (const uint64_t*) np, dat);
            return (unsigned) id < 256u ? d.nativeBiome[id] : -1;
        }
        int e = d.tree.find(np, cursor);
        return e >= 0 ? d.entryBiome[e] : -1;
    };

    // cells by chunk
    std::map<std::pair<int, int>, std::vector<const ServerFile::Cell*>> chunks;
    for (const ServerFile::Cell& c : sf.cells)
        chunks[std::make_pair(c.x >> 4, c.z >> 4)].push_back(&c);

    const int qy0 = sf.minY >> 2, qn = sf.height >> 2;
    std::vector<int64_t> np((size_t) 16 * qn * 6);
    std::vector<int> res((size_t) 16 * qn);
    // the game's search order only matters for a source that searches a
    // climate list (multi_noise); the others are compared as they are
    const bool climateList = d.source == dp::DimData::MULTI_NOISE || d.source == dp::DimData::NATIVE_OVERWORLD;
    for (const auto& ch : chunks)
    {
        if (!climateList)
            break;
        const int qx0 = ch.first.first * 4, qz0 = ch.first.second * 4;
        // climate of all cells of the chunk
        for (int l = 0; l < 4; l++)
        {
            for (int n = 0; n < 4; n++)
            {
                st.evalColumn((qx0 + l) * 4, (qz0 + n) * 4);
                for (int q = 0; q < qn; q++)
                {
                    st.evalVertical((qy0 + q) * 4);
                    int64_t *p = &np[((size_t) (l * 4 + n) * qn + q) * 6];
                    for (int k = 0; k < 6; k++)
                        p[k] = dp::quantizeClimate((float) st.value(d.outputs[k]));
                }
            }
        }
        // the game's order: sections bottom up; in a section x, y, z
        int cursor = -1;
        uint64_t dat = 0;
        for (int sec = 0; sec < qn / 4; sec++)
            for (int l = 0; l < 4; l++)
                for (int m = 0; m < 4; m++)
                    for (int n = 0; n < 4; n++)
                    {
                        int q = sec * 4 + m;
                        res[(size_t) (l * 4 + n) * qn + q] =
                            lookup(&np[((size_t) (l * 4 + n) * qn + q) * 6], &cursor, &dat);
                    }
        // genBiomes in chunk order against that full chain, in all cells of
        // the chunk at some layers (every position within a section)
        {
            static const int layers[] = { 0, 1, 2, 3, 4, 7, 16, 18, 31, 32, 33, 48, 61, 95 };
            int out[16];
            for (int q : layers)
            {
                if (q >= qn)
                    continue;
                chunkSampler.genBiomes(out, 4, qx0, qz0, 4, 4, (qy0 + q) * 4);
                for (int l = 0; l < 4; l++)
                    for (int n = 0; n < 4; n++)
                    {
                        int full = res[(size_t) (l * 4 + n) * qn + q];
                        chain.add(out[n * 4 + l] == full);
                        int fc = -1;
                        if (lookup(&np[((size_t) (l * 4 + n) * qn + q) * 6], &fc, nullptr) != full)
                            chainTies++;
                    }
            }
        }
        for (const ServerFile::Cell *c : ch.second)
        {
            int l = (c->x >> 2) - qx0, n = (c->z >> 2) - qz0, q = (c->y >> 2) - qy0;
            if (l < 0 || l > 3 || n < 0 || n > 3 || q < 0 || q >= qn)
                continue;
            int b = res[(size_t) (l * 4 + n) * qn + q];
            chunk.add(b >= 0 && biomes[b].id == c->biome);
            if (getenv("DPTEST_VERBOSE") && !(b >= 0 && biomes[b].id == c->biome) && chunk.diff <= 40)
            {
                const int64_t *v = &np[((size_t) (l * 4 + n) * qn + q) * 6];
                printf("  cell %d %d %d: server %s, here %s (climate %lld %lld %lld %lld %lld %lld)\n",
                    c->x, c->y, c->z, qPrintable(c->biome), b >= 0 ? qPrintable(biomes[b].id) : "-",
                    (long long) v[0], (long long) v[1], (long long) v[2], (long long) v[3], (long long) v[4],
                    (long long) v[5]);
            }
            int fc = -1;
            b = lookup(&np[((size_t) (l * 4 + n) * qn + q) * 6], &fc, nullptr);
            fresh.add(b >= 0 && biomes[b].id == c->biome);
            if (g)
            {
                int id = getBiomeAt(g, 4, c->x >> 2, c->y >> 2, c->z >> 2);
                const char *s = biome2str(mc, id);
                engine.add(s && c->biome == QStringLiteral("minecraft:") + s);
            }
        }
    }

    // genBiomes over each area at each y level of the cells, with each way
    // of treating the last result
    static const dp::CursorPolicy policies[3] = { dp::CURSOR_CHUNK, dp::CURSOR_CARRY, dp::CURSOR_FRESH };
    for (int pi = 0; pi < 3; pi++)
    {
        dp::setCursorPolicy(policies[pi]);
        dp::Sampler sampler(std::shared_ptr<const dp::World>(std::shared_ptr<const dp::World>(), &w), dim, seed);
        std::map<int, std::vector<const ServerFile::Cell*>> byY;
        for (const ServerFile::Cell& c : sf.cells)
            byY[c.y].push_back(&c);
        for (const ServerFile::Area& a : sf.areas)
        {
            int x = a.x0 >> 2, z = a.z0 >> 2, sx = ((a.x1 + 1) >> 2) - x, sz = ((a.z1 + 1) >> 2) - z;
            std::vector<int> out((size_t) sx * sz);
            for (const auto& yl : byY)
            {
                sampler.genBiomes(out.data(), 4, x, z, sx, sz, yl.first);
                for (const ServerFile::Cell *c : yl.second)
                {
                    int i = (c->x >> 2) - x, j = (c->z >> 2) - z;
                    if (i < 0 || j < 0 || i >= sx || j >= sz)
                        continue;
                    int b = out[(size_t) j * sx + i];
                    tile[pi].add(b >= 0 && biomes[b].id == c->biome);
                }
            }
        }
    }
    dp::setCursorPolicy(dp::CURSOR_CHUNK);

    if (g_chainOnly)
    {
        printf("%s: genBiomes in chunk order against whole chunks searched in the game's order: %lld of %lld cells "
            "differ (%.4f %%; %lld of the cells depend on the history at all)\n", label, (long long) chain.diff,
            (long long) chain.n, chain.rate(), (long long) chainTies);
        if (total)
        {
            total[4].n += chain.n;
            total[4].diff += chain.diff;
            total[5].n += chain.n;
            total[5].diff += chainTies;
        }
        return;
    }
    printf("%s: %lld server cells; differing with genBiomes at scale 4: chunk order (default) %lld, "
        "one cursor per call %lld, no history %lld",
        label, (long long) tile[0].n, (long long) tile[0].diff, (long long) tile[1].diff, (long long) tile[2].diff);
    if (g)
        printf("; native engine %lld", (long long) engine.diff);
    printf("; whole chunks in the game's order %lld\n", (long long) chunk.diff);
    if (climateList)
        printf("%s: genBiomes in chunk order against whole chunks searched in the game's order: %lld of %lld cells "
            "differ (%lld of them depend on the history at all)\n", label, (long long) chain.diff, (long long) chain.n,
            (long long) chainTies);
    else
        printf("%s: the biome source searches no climate list: no reference in the game's order\n", label);
    if (total)
    {
        total[4].n += chain.n;
        total[4].diff += chain.diff;
        total[5].n += chain.n;
        total[5].diff += chainTies;
    }
    if (total)
    {
        for (int pi = 0; pi < 3; pi++)
        {
            total[pi].n += tile[pi].n;
            total[pi].diff += tile[pi].diff;
        }
        total[3].n += engine.n;
        total[3].diff += engine.diff;
    }
    CHECK(!climateList || tile[2].diff == fresh.diff, "%s: genBiomes without history differs from the direct lookups",
        label);
    if (g)
        CHECK(tile[0].diff <= engine.diff, "%s: more cells differ from the server than with the native engine", label);
    CHECK(tile[0].rate() < 0.1 && tile[1].rate() < 0.1 && tile[2].rate() < 0.1 && chunk.rate() < 0.1,
        "%s: too many cells differ from the server", label);
}

// Chunk order: the biome of a cell must only depend on its position: not on
// the area that was asked for, not on the scale, not on what was asked
// before.
static void checkCellPurity(std::shared_ptr<const dp::World> w, int dim, uint64_t seed, const char *label, bool quick)
{
    static const int origins[][2] = { { -128, -128 }, { 20000, -31000 }, { -250000, 180000 } };
    dp::setCursorPolicy(dp::CURSOR_FRESH);
    dp::Sampler freshSampler(w, dim, seed);
    dp::setCursorPolicy(dp::CURSOR_CHUNK);
    {
        dp::Sampler sampler(w, dim, seed);
        dp::Sampler other(w, dim, seed);  // one that is asked in another order
        std::mt19937_64 rng(99);
        int64_t bad = 0, cells = 0, badAt = 0, at = 0;
        for (int scale : { 4, 1 })
        {
            const int n = 96;
            for (int y : { -64, -61, 0, 5, 62, 64, 130 })
            {
                int bx = (int) (rng() % 4000) - 2000, bz = (int) (rng() % 4000) - 2000;
                std::vector<int> big((size_t) n * n);
                sampler.genBiomes(big.data(), scale, bx, bz, n, n, y);
                for (int t = 0; t < 40; t++)
                {
                    int ox = (int) (rng() % (n - 1)), oz = (int) (rng() % (n - 1));
                    int sw = 1 + (int) (rng() % std::min(17, n - ox)), sh = 1 + (int) (rng() % std::min(17, n - oz));
                    std::vector<int> part((size_t) sw * sh);
                    // alternately on the same sampler and on another one
                    (t & 1 ? sampler : other).genBiomes(part.data(), scale, bx + ox, bz + oz, sw, sh, y);
                    for (int j = 0; j < sh; j++)
                        for (int i = 0; i < sw; i++)
                        {
                            cells++;
                            if (part[(size_t) j * sw + i] != big[(size_t) (oz + j) * n + ox + i])
                                bad++;
                        }
                }
                for (int t = 0; t < 300; t++)
                {
                    int i = (int) (rng() % n), j = (int) (rng() % n);
                    at++;
                    if (other.biomeAt(scale, bx + i, y, bz + j) != big[(size_t) j * n + i])
                        badAt++;
                }
            }
        }
        printf("%s: chunk order, %lld cells asked for again in smaller areas: %lld differ; %lld single "
            "cells with biomeAt: %lld differ\n", label, (long long) cells, (long long) bad,
            (long long) at, (long long) badAt);
        CHECK(bad == 0, "%s: the biome of a cell depends on the requested area", label);
        CHECK(badAt == 0, "%s: biomeAt differs from genBiomes", label);

        // Larger scales: every pixel is the 1:4 cell in its middle,
        // with the biome that cell has at 1:4.
        int64_t pixels = 0, wrong = 0, sensitive = 0;
        for (const auto& o : origins)
        {
            for (int y : { -40, 64, 77, 150 })
            {
                const int n = quick ? 64 : 128;
                std::vector<int> cells4((size_t) 4 * n * 4 * n), px((size_t) n * n), fr((size_t) n * n);
                sampler.genBiomes(cells4.data(), 4, o[0], o[1], 4 * n, 4 * n, y);
                other.genBiomes(px.data(), 16, o[0] / 4, o[1] / 4, n, n, y);
                freshSampler.genBiomes(fr.data(), 16, o[0] / 4, o[1] / 4, n, n, y);
                for (int j = 0; j < n; j++)
                    for (int i = 0; i < n; i++)
                    {
                        pixels++;
                        // o[] are multiples of 4 here
                        if (px[(size_t) j * n + i] != cells4[(size_t) (j * 4 + 2) * 4 * n + i * 4 + 2])
                            wrong++;
                        if (px[(size_t) j * n + i] != fr[(size_t) j * n + i])
                            sensitive++;
                    }
                for (int scale : { 64, 256, 1024 })
                {
                    int m = scale / 4, k = 20;
                    std::vector<int> a((size_t) k * k);
                    other.genBiomes(a.data(), scale, o[0] / m, o[1] / m, k, k, y);
                    for (int j = 0; j < k; j++)
                        for (int i = 0; i < k; i++)
                        {
                            pixels++;
                            int qx = (o[0] / m + i) * m + m / 2, qz = (o[1] / m + j) * m + m / 2;
                            if (a[(size_t) j * k + i] != sampler.biomeAt(4, qx, y, qz))
                                wrong++;
                            if (a[(size_t) j * k + i] != other.biomeAt(scale, o[0] / m + i, y, o[1] / m + j))
                                wrong++;
                        }
                }
            }
        }
        // 1:1: every block shows the 1:4 cell that the game's zoom picks for
        // it, with the biome that cell has at 1:4 (below and above the
        // dimension the nearest cell, like a chunk of the game).
        {
            const uint64_t sha = getVoronoiSHA(seed);
            const int n = quick ? 40 : 64;
            int64_t blocks = 0, bad1 = 0;
            std::vector<int> tile((size_t) n * n);
            for (int y : { -64, -63, -1, 62, 63, 64, 65, 66, 67, 199, 318, 319 })
            {
                int bx = (int) (rng() % 6000) - 3000, bz = (int) (rng() % 6000) - 3000;
                other.genBiomes(tile.data(), 1, bx, bz, n, n, y);
                std::map<std::tuple<int, int, int>, int> cache;
                for (int j = 0; j < n; j++)
                    for (int i = 0; i < n; i++)
                    {
                        int x4, y4, z4;
                        voronoiAccess3D(sha, bx + i, y, bz + j, &x4, &y4, &z4);
                        auto key = std::make_tuple(x4, y4, z4);
                        auto it = cache.find(key);
                        if (it == cache.end())
                            it = cache.insert(std::make_pair(key, sampler.biomeAt(4, x4, y4 * 4, z4))).first;
                        blocks++;
                        if (tile[(size_t) j * n + i] != it->second)
                            bad1++;
                    }
            }
            printf("%s: chunk order, %lld blocks at 1:1 against the 1:4 cells the zoom picks: %lld differ\n",
                label, (long long) blocks, (long long) bad1);
            CHECK(bad1 == 0, "%s: 1:1 does not show the 1:4 cells", label);
        }
        printf("%s: chunk order, %lld pixels at the scales 16 to 1024 against the 1:4 cell in their "
            "middle: %lld differ (%lld of the pixels depend on the history)\n", label,
            (long long) pixels, (long long) wrong, (long long) sensitive);
        CHECK(wrong == 0, "%s: larger scales do not show the 1:4 cells", label);
    }
}

static int cmdVanilla(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QString reportPath, serverDir, work;
    bool quick = false;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--report" && i + 1 < args.size()) { reportPath = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--server" && i + 1 < args.size()) { serverDir = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--work" && i + 1 < args.size()) { work = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--quick") { quick = true; args.remove(i); }
        else i++;
    }
    if (args.size() < 2)
    {
        printf("usage: dptest vanilla <vanilla data> <mc> [--report <biome_parameters.json> --work <dir>]\n"
            "           [--server <tools/worldgen-reference/data>] [--quick]\n");
        return 2;
    }
    const QString vanilla = args[0];
    const int mc = str2mc(qPrintable(args[1]));
    if (mc < MC_1_18)
    {
        printf("unknown or unsupported version %s\n", qPrintable(args[1]));
        return 2;
    }
    // the engine's id MC_1_18 stands for 1.18.2, the version of the data
    const char *mcName = mc == MC_1_18_2 ? "1.18.2" : mc2str(mc);
    dp::setAllowUnverifiedVersions(true);
    int fmin = 0, fmax = 0;
    dp::packFormatRange(mc, &fmin, &fmax);

    const int nseeds = quick ? 2 : 6;
    static const int ys[] = { -60, 0, 63, 64, 128, 201, 256, 316 };
    static const int origins[][2] = { { -128, -128 }, { 20000, -31000 }, { -250000, 180000 } };

    Counter serverTotal[6];
    for (const PresetCase& pc : g_presets)
    {
        Generator g;
        setupGenerator(&g, mc, pc.flags);

        for (int variant = 0; variant < 2; variant++)
        {
            // variant 0: the preset's built-in list (the engine's native
            // tree); variant 1: the same list from the report, as a pack,
            // through the generic tree
            QStringList packs;
            if (variant == 1)
            {
                if (reportPath.isEmpty() || work.isEmpty())
                    continue;
                QString dir = work + "/listpack-" + QString(pc.settings).replace(':', '_');
                CHECK(writeListPack(dir, reportPath, pc.settings, fmax), "cannot write the list pack");
                packs << dir;
            }
            QString label = QStringLiteral("%1 %2 %3").arg(mcName, pc.preset + 10,
                variant ? "list pack" : "preset list");
            QByteArray lb = label.toUtf8();

            dp::Report report;
            QElapsedTimer timer;
            timer.start();
            std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, packs, pc.preset, &report);
            double loadMs = timer.nsecsElapsed() / 1e6;
            CHECK(report.ok(), "%s: the world has errors", lb.constData());
            int dim = w->dimension("minecraft:overworld");
            CHECK(dim >= 0 && w->dimensions()[dim].supported, "%s: no supported overworld (%s)",
                lb.constData(), dim >= 0 ? qPrintable(w->dimensions()[dim].reason) : "missing");
            if (dim < 0 || !w->dimensions()[dim].supported)
                continue;
            std::vector<int> ids = engineIds(*w, mc);
            const dp::DimData& dd = w->d()->data[dim];
            printf("%s: loaded in %.1f ms, %zu dimensions, %zu biomes in the overworld, "
                "%zu column + %zu vertical nodes, %zu noises\n", lb.constData(), loadMs,
                w->dimensions().size(), w->dimensions()[dim].biomes.size(),
                dd.program.column.size(), dd.program.vertical.size(), dd.program.usedNoises.size());

            if (variant == 0)
                compareClimate(*w, dim, mc, pc.flags, lb.constData());
            {
                // the biomes the dimension lists: for the preset list what
                // the engine says generates, for the pack its entries
                QStringList names;
                for (int b : w->dimensions()[dim].biomes)
                    names << w->biomes()[b].id;
                names.sort();
                static QStringList presetNames;
                if (variant == 0)
                    presetNames = names;
                else
                    CHECK(names == presetNames, "%s: the biome list differs from the preset's (%d / %d)",
                        lb.constData(), (int) names.size(), (int) presetNames.size());
            }

            // scale 4, searched without history on both sides
            dp::setCursorPolicy(dp::CURSOR_FRESH);
            Counter s4, s1, s16, s64, s256, carry;
            const int T = quick ? 96 : 192;
            for (int si = 0; si < nseeds; si++)
            {
                uint64_t seed = g_seeds[si];
                applySeed(&g, DIM_OVERWORLD, seed);
                dp::Sampler sampler(w, dim, seed);
                CHECK(sampler.valid(), "sampler not valid");
                for (const auto& o : origins)
                {
                    for (int y : ys)
                    {
                        Counter c = compareArea(sampler, ids, &g, 4, o[0], o[1], T, T, y, true);
                        s4.n += c.n; s4.diff += c.diff;
                    }
                    // 1:1 around the same place, at heights that are not
                    // multiples of four as well
                    for (int y : { -59, 63, 70, 201 })
                    {
                        Counter c = compareArea(sampler, ids, &g, 1, o[0] * 4 + 37, o[1] * 4 - 11, 2 * T, 2 * T, y, true);
                        s1.n += c.n; s1.diff += c.diff;
                    }
                    // larger scales: the engine samples those without the
                    // coordinate shift and with a search hint, so they are
                    // not expected to be the same
                    for (int y : { 0, 64, 131 })
                    {
                        Counter c = compareArea(sampler, ids, &g, 16, o[0] / 4, o[1] / 4, T, T, y);
                        s16.n += c.n; s16.diff += c.diff;
                        c = compareArea(sampler, ids, &g, 64, o[0] / 16, o[1] / 16, T / 2, T / 2, y);
                        s64.n += c.n; s64.diff += c.diff;
                        c = compareArea(sampler, ids, &g, 256, o[0] / 64, o[1] / 64, T / 4, T / 4, y);
                        s256.n += c.n; s256.diff += c.diff;
                    }
                    // larger scales against the 1:4 cells they stand for
                    for (int scale : { 16, 64, 256, 1024 })
                    {
                        int m = scale / 4, n = 24, y = 77;
                        std::vector<int> a((size_t) n * n);
                        sampler.genBiomes(a.data(), scale, o[0] / m, o[1] / m, n, n, y);
                        for (int j = 0; j < n; j++)
                            for (int i = 0; i < n; i++)
                            {
                                int b = sampler.biomeAt(4, (o[0] / m + i) * m + m / 2, y, (o[1] / m + j) * m + m / 2);
                                CHECK(b == a[(size_t) j * n + i], "scale %d does not sample the middle cell", scale);
                            }
                    }
                }
                // in chunk order
                dp::setCursorPolicy(dp::CURSOR_CHUNK);
                {
                    dp::Sampler carried(w, dim, seed);
                    for (const auto& o : origins)
                        for (int y : ys)
                        {
                            Counter c = compareArea(carried, ids, &g, 4, o[0], o[1], T, T, y);
                            carry.n += c.n; carry.diff += c.diff;
                        }
                }
                dp::setCursorPolicy(dp::CURSOR_FRESH);
            }
            printf("%s: against the engine's genBiomes, differing cells: scale 4 %lld of %lld (%.5f %%), "
                "scale 1 %lld of %lld (%.5f %%)\n", lb.constData(),
                (long long) s4.diff, (long long) s4.n, s4.rate(), (long long) s1.diff, (long long) s1.n, s1.rate());
            printf("%s: scale 4 in chunk order (the default): %lld of %lld (%.5f %%) differ from the engine, "
                "which searches without history\n",
                lb.constData(), (long long) carry.diff, (long long) carry.n, carry.rate());
            printf("%s: engine's coarse scales (no shift, search hint), differing: 16: %.3f %%, 64: %.3f %%, "
                "256: %.3f %% (expected, see the comment)\n", lb.constData(), s16.rate(), s64.rate(), s256.rate());
            // from 26.2 the engine's lookup and from 26.3 its double
            // precision climate are not the game's: only a rough check
            const double tol = mc >= MC_26_2 ? 0.2 : 0.01;
            CHECK(s4.rate() <= tol, "%s: scale 4 differs from the engine in %.5f %%", lb.constData(), s4.rate());
            CHECK(s1.rate() <= tol, "%s: scale 1 differs from the engine in %.5f %%", lb.constData(), s1.rate());
            CHECK(carry.rate() <= 0.2, "%s: chunk order differs from the engine in %.5f %%", lb.constData(), carry.rate());

            checkCellPurity(w, dim, g_seeds[0], lb.constData(), quick);

            // real servers
            if (!serverDir.isEmpty() && pc.serverPrefix)
            {
                for (int si = 0; si < 4; si++)
                {
                    QString file = QStringLiteral("%1/%2_%3_%4.txt").arg(serverDir, pc.serverPrefix, mcName)
                        .arg((qlonglong) g_seeds[si]);
                    ServerFile sf;
                    if (!QFileInfo::exists(file))
                    {
                        // not every version and seed has server data
                        printf("%s: no server file %s\n", lb.constData(), qPrintable(file));
                        continue;
                    }
                    if (!readServerFile(file, &sf))
                    {
                        CHECK(false, "%s: the server file %s cannot be read or has no B lines", lb.constData(),
                            qPrintable(file));
                        continue;
                    }
                    applySeed(&g, DIM_OVERWORLD, g_seeds[si]);
                    QByteArray l2 = lb + " seed " + QByteArray::number((qlonglong) g_seeds[si]);
                    compareServer(*w, dim, mc, g_seeds[si], sf, &g, l2.constData(), serverTotal);
                }
            }
        }
    }
    if (serverTotal[0].n)
        printf("%s, all server files: %lld cells; differing: chunk order (default) %lld (%.4f %%), one cursor "
            "per call %lld (%.4f %%), no history %lld (%.4f %%), native engine %lld of %lld (%.4f %%)\n", mcName,
            (long long) serverTotal[0].n, (long long) serverTotal[0].diff, serverTotal[0].rate(),
            (long long) serverTotal[1].diff, serverTotal[1].rate(),
            (long long) serverTotal[2].diff, serverTotal[2].rate(),
            (long long) serverTotal[3].diff, (long long) serverTotal[3].n, serverTotal[3].rate());
    if (serverTotal[4].n)
        printf("%s, all chunks of the server files: genBiomes in chunk order against whole chunks in the game's "
            "order: %lld of %lld cells differ; %lld cells depend on the history\n", mcName,
            (long long) serverTotal[4].diff, (long long) serverTotal[4].n, (long long) serverTotal[5].diff);

    // The Nether and the End of the vanilla data (legacy random source, the
    // Nether's built-in list, the End's biome source) against the engine
    {
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList(), "minecraft:normal", &report, false);
        CHECK(report.ok(), "the vanilla world has errors");
        const std::vector<int> ids = engineIds(*w, mc);
        static const struct { const char *id; int dim; } dims[] = {
            { "minecraft:the_nether", DIM_NETHER }, { "minecraft:the_end", DIM_END } };
        for (const auto& dd : dims)
        {
            const int dim = w->dimension(dd.id);
            if (mc >= MC_26_3)
            {
                // legacy_random_source is not supported with the float
                // noises of 26.3: both must say so
                CHECK(dim >= 0 && !w->dimensions()[dim].supported && !w->dimensions()[dim].reason.isEmpty(),
                    "%s of %s must be unsupported with a reason", dd.id, mcName);
                if (dim >= 0)
                    printf("%s %s: not supported (%s)\n", mcName, dd.id,
                        qPrintable(w->dimensions()[dim].reason));
                continue;
            }
            CHECK(dim >= 0 && w->dimensions()[dim].supported, "%s is not supported", dd.id);
            if (dim < 0 || !w->dimensions()[dim].supported)
                continue;
            Counter c4, c1, c16;
            Generator g;
            setupGenerator(&g, mc, 0);
            for (int si = 0; si < nseeds; si++)
            {
                applySeed(&g, dd.dim, g_seeds[si]);
                dp::Sampler s(w, dim, g_seeds[si]);
                const int n = quick ? 48 : 96;
                for (int y : { 0, 40, 100, 200 })
                {
                    const int x0 = (int) (g_seeds[si] % 4000) - 2000, z0 = (int) ((g_seeds[si] >> 20) % 4000) - 2000;
                    Counter a = compareArea(s, ids, &g, 4, x0 / 4, z0 / 4, n, n, y, true);
                    c4.n += a.n; c4.diff += a.diff;
                    Counter b = compareArea(s, ids, &g, 1, x0, z0, n, n, y, true);
                    c1.n += b.n; c1.diff += b.diff;
                    Counter e = compareArea(s, ids, &g, 16, x0 / 16 - 40, z0 / 16 + 30, n / 2, n / 2, y, true);
                    c16.n += e.n; c16.diff += e.diff;
                }
            }
            printf("%s %s against the engine: differing at 1:4 %lld of %lld, 1:1 %lld of %lld, 1:16 %lld of %lld\n",
                mcName, dd.id, (long long) c4.diff, (long long) c4.n, (long long) c1.diff, (long long) c1.n,
                (long long) c16.diff, (long long) c16.n);
            // The engine's Nether compares float distances of the
            // unquantized climate (the game: quantized longs and the search
            // tree with its last result), so near ties may differ. The
            // server files are the reference ("dptest heights").
            if (dd.dim == DIM_END)
                CHECK(c4.diff == 0 && c1.diff == 0 && c16.diff == 0, "%s differs from the engine", dd.id);
            else
                CHECK(c4.rate() < 0.1 && c1.rate() < 0.1, "%s differs from the engine too often", dd.id);
        }
    }

    // What the version does not support must say so (and the rest must be
    // there): terrain heights and structure sets of the normal preset
    {
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList(), "minecraft:normal", &report, false);
        const bool terrain = mc >= MC_1_18_2 && mc <= MC_26_2;
        const bool structures = mc >= MC_1_19_2 && mc <= MC_26_2;
        for (const dp::DimensionInfo& d : w->dimensions())
        {
            if (!d.supported)
                continue;
            CHECK(d.terrain == terrain && (d.terrain || !d.terrainReason.isEmpty()),
                "%s %s: terrain %d, expected %d (with a reason if not)", mcName, qPrintable(d.id), d.terrain, terrain);
            printf("%s %s: terrain %s\n", mcName, qPrintable(d.id),
                d.terrain ? "yes" : qPrintable(QStringLiteral("no (%1)").arg(d.terrainReason)));
        }
        const std::vector<dp::StructureSetInfo>& sets = w->structureSets();
        int supported = 0, withReason = 0;
        for (const dp::StructureSetInfo& si : sets)
        {
            supported += si.supported;
            withReason += !si.supported && !si.reason.isEmpty();
        }
        printf("%s structure sets: %d, supported %d, unsupported with a reason %d%s%s\n", mcName,
            (int) sets.size(), supported, withReason, sets.empty() ? "" : ", e.g. ",
            sets.empty() ? "" : qPrintable(sets[0].supported ? QStringLiteral("supported") : sets[0].reason));
        CHECK(!sets.empty(), "%s: no structure sets", mcName);
        if (structures)
            CHECK(supported > 0, "%s: no structure set is supported", mcName);
        else
            CHECK(supported == 0 && withReason == (int) sets.size(),
                "%s: structure sets must be unsupported with a reason", mcName);
    }
    return g_fail != 0;
}

//==============================================================================
// perf
//==============================================================================

static int cmdPerf(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QString reportPath, work;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--report" && i + 1 < args.size()) { reportPath = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--work" && i + 1 < args.size()) { work = args[i + 1]; args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 2)
    {
        printf("usage: dptest perf <vanilla data> <mc> [--report <biome_parameters.json> --work <dir>] [pack ...]\n");
        return 2;
    }
    const QString vanilla = args[0];
    const int mc = str2mc(qPrintable(args[1]));
    QStringList userPacks = args.mid(2);
    dp::setAllowUnverifiedVersions(true);
    int fmin = 0, fmax = 0;
    dp::packFormatRange(mc, &fmin, &fmax);

    const int T = 256, tiles = 12;
    std::vector<int> out((size_t) T * T);
    QElapsedTimer timer;

    // the native engine
    {
        Generator g;
        setupGenerator(&g, mc, 0);
        timer.start();
        for (int i = 0; i < 200; i++)
            applySeed(&g, DIM_OVERWORLD, 1000 + i);
        double seedUs = timer.nsecsElapsed() / 1e3 / 200;
        Range r = { 4, 0, 0, T, T, 16, 1 };
        int *cache = allocCache(&g, r);
        timer.restart();
        int64_t sum = 0;
        for (int t = 0; t < tiles; t++)
        {
            r.x = t * 3001 - 20000;
            r.z = t * 1777 - 9000;
            genBiomes(&g, cache, r);
            sum += cache[t];
        }
        double us = timer.nsecsElapsed() / 1e3 / ((double) tiles * T * T);
        free(cache);
        printf("native engine: %.3f us per sample at scale 4, %.0f us to apply a seed (%lld)\n",
            us, seedUs, (long long) sum);
    }

    for (int variant = 0; variant < 3; variant++)
    {
        QStringList packs;
        const char *label = "vanilla, preset list (engine tree)";
        if (variant == 1)
        {
            if (reportPath.isEmpty() || work.isEmpty())
                continue;
            QString dir = work + "/listpack-minecraft_overworld";
            if (!writeListPack(dir, reportPath, "minecraft:overworld", fmax))
                continue;
            packs << dir;
            label = "vanilla, list as a pack (generic tree)";
        }
        else if (variant == 2)
        {
            if (userPacks.isEmpty())
                continue;
            packs = userPacks;
            label = "with the given packs";
        }
        dp::Report report;
        timer.restart();
        std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, packs, "minecraft:normal", &report, variant == 2);
        double loadMs = timer.nsecsElapsed() / 1e6;
        // a second load, with the files in the system's cache
        dp::Report report2;
        timer.restart();
        w = loadWorld(mc, vanilla, packs, "minecraft:normal", &report2, false);
        double loadMs2 = timer.nsecsElapsed() / 1e6;
        int dim = w->dimension("minecraft:overworld");
        if (dim < 0 || !w->dimensions()[dim].supported)
        {
            printf("%s: no supported overworld\n", label);
            g_fail++;
            continue;
        }
        const dp::DimData& dd = w->d()->data[dim];
        timer.restart();
        for (int i = 0; i < 200; i++)
        {
            dp::Sampler s(w, dim, 1000 + i);
            if (!s.valid())
                g_fail++;
        }
        double samplerUs = timer.nsecsElapsed() / 1e3 / 200;
        printf("%s: load %.1f ms (again: %.1f ms), sampler %.0f us, %zu biomes, %zu + %zu nodes, %zu noises\n",
            label, loadMs, loadMs2, samplerUs, w->dimensions()[dim].biomes.size(),
            dd.program.column.size(), dd.program.vertical.size(), dd.program.usedNoises.size());

        static const struct { dp::CursorPolicy p; const char *name; } pol[] = {
            { dp::CURSOR_CHUNK, "chunk order (default)" },
            { dp::CURSOR_CARRY, "one cursor per call" },
            { dp::CURSOR_FRESH, "no history" },
        };
        for (const auto& po : pol)
        {
            dp::setCursorPolicy(po.p);
            dp::Sampler s(w, dim, 1000);
            double best[4] = { 1e9, 1e9, 1e9, 1e9 };
            static const int scales[4] = { 4, 1, 16, 256 };
            for (int rep = 0; rep < 2; rep++)
            {
                for (int si = 0; si < 4; si++)
                {
                    int64_t sum = 0;
                    timer.restart();
                    int nt = si == 0 ? tiles : 4;
                    for (int t = 0; t < nt; t++)
                    {
                        s.genBiomes(out.data(), scales[si], t * 3001 - 20000, t * 1777 - 9000, T, T, 64);
                        sum += out[t];
                    }
                    double us = timer.nsecsElapsed() / 1e3 / ((double) nt * T * T);
                    best[si] = std::min(best[si], us);
                    if (sum == -1)
                        g_fail++;
                }
            }
            printf("  %-22s us per sample: scale 4 %.3f, scale 1 %.3f, scale 16 %.3f, scale 256 %.3f\n",
                po.name, best[0], best[1], best[2], best[3]);
        }
        dp::setCursorPolicy(dp::CURSOR_CHUNK);
    }
    return g_fail != 0;
}

//==============================================================================
// zip, truncate: the archive reader
//==============================================================================

static int cmdZip(const QStringList& args)
{
    if (args.size() < 1)
    {
        printf("usage: dptest zip <zip or jar file>\n");
        return 2;
    }
    dp::ZipReader zip;
    QString err;
    QElapsedTimer timer;
    timer.start();
    if (!zip.open(args[0], &err))
    {
        printf("cannot open: %s\n", qPrintable(err));
        return 1;
    }
    int64_t bytes = 0, bad = 0, deflated = 0, stored = 0;
    for (size_t i = 0; i < zip.entries().size(); i++)
    {
        QByteArray data;
        if (!zip.read(i, &data, &err))
        {
            if (bad < 5)
                printf("  %s: %s\n", qPrintable(zip.entries()[i].name), qPrintable(err));
            bad++;
            continue;
        }
        bytes += data.size();
        if (zip.entries()[i].method == 8)
            deflated++;
        else
            stored++;
    }
    printf("%s: %zu entries (%lld deflated, %lld stored), %lld bytes unpacked and checked (CRC-32) in %.0f ms, "
        "%lld failed\n", qPrintable(QFileInfo(args[0]).fileName()), zip.entries().size(),
        (long long) deflated, (long long) stored, (long long) bytes, timer.nsecsElapsed() / 1e6, (long long) bad);
    CHECK(bad == 0, "entries failed");
    CHECK(deflated > 0, "the archive has no deflated entry: the test did not test inflate");
    return g_fail != 0;
}

static bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(data);
    return true;
}

// Cut off and damaged copies of an archive: never a crash, never wrong data
// (an entry either fails or has its checksum).
static int cmdTruncate(const QStringList& args)
{
    if (args.size() < 2)
    {
        printf("usage: dptest truncate <zip or jar file> <work dir> [<vanilla data> <mc>]\n");
        return 2;
    }
    QFile f(args[0]);
    if (!f.open(QIODevice::ReadOnly))
        return 2;
    const QByteArray all = f.readAll();
    const QString tmp = args[1] + "/damaged.zip";

    // the entries of the intact archive
    std::map<QString, QByteArray> good;
    {
        dp::ZipReader zip;
        QString err;
        CHECK(zip.open(args[0], &err), "the intact archive does not open: %s", qPrintable(err));
        for (size_t i = 0; i < zip.entries().size(); i++)
        {
            QByteArray d;
            CHECK(zip.read(i, &d, &err), "intact entry fails");
            good[zip.entries()[i].name] = d;
        }
    }

    std::vector<qsizetype> lengths;
    for (int i = 0; i <= 200; i++)
        lengths.push_back(all.size() * i / 200);
    for (qsizetype d : { 1, 2, 3, 4, 10, 21, 22, 23, 45, 46, 47, 100, 1000 })
    {
        if (d < all.size())
        {
            lengths.push_back(all.size() - d);
            lengths.push_back(d);
        }
    }
    int64_t opened = 0, refused = 0, entriesOk = 0, entriesFailed = 0, wrong = 0;
    auto probe = [&](const QByteArray& data) {
        writeFile(tmp, data);
        dp::ZipReader zip;
        QString err;
        if (!zip.open(tmp, &err))
        {
            refused++;
            CHECK(!err.isEmpty(), "no message for an archive that does not open");
            return;
        }
        opened++;
        for (size_t i = 0; i < zip.entries().size(); i++)
        {
            QByteArray d;
            if (!zip.read(i, &d, &err))
            {
                entriesFailed++;
                continue;
            }
            entriesOk++;
            auto it = good.find(zip.entries()[i].name);
            // a damaged copy may only hand out data that is the original's
            // (or, for damage in the directory, an entry the checksum covers)
            if (it != good.end() && it->second != d && dp::crc32((const unsigned char*) d.constData(),
                (size_t) d.size()) != zip.entries()[i].crc)
                wrong++;
        }
    };
    for (qsizetype len : lengths)
        if (len < all.size())
            probe(all.left(len));
    printf("cut off at %zu lengths: %lld refused, %lld opened (entries: %lld read, %lld failed)\n",
        lengths.size(), (long long) refused, (long long) opened, (long long) entriesOk, (long long) entriesFailed);
    CHECK(opened == 0, "a cut off archive opened");

    // random damage: bytes changed, ranges zeroed
    std::mt19937_64 rng(7);
    int64_t before = refused;
    opened = entriesOk = entriesFailed = 0;
    for (int t = 0; t < 300; t++)
    {
        QByteArray d = all;
        int n = 1 + (int) (rng() % 8);
        for (int k = 0; k < n; k++)
        {
            qsizetype pos = (qsizetype) (rng() % (uint64_t) d.size());
            if (rng() % 4 == 0)
            {
                qsizetype len = std::min<qsizetype>(d.size() - pos, 1 + (qsizetype) (rng() % 64));
                memset(d.data() + pos, (int) (rng() % 2) * 255, (size_t) len);
            }
            else
                d[pos] = (char) (rng() & 0xff);
        }
        probe(d);
    }
    printf("300 randomly damaged copies: %lld refused, %lld opened (entries: %lld read, %lld failed), "
        "%lld entries with wrong data\n", (long long) (refused - before), (long long) opened,
        (long long) entriesOk, (long long) entriesFailed, (long long) wrong);
    CHECK(wrong == 0, "damaged entries were handed out");

    // a cut off pack through World::load()
    if (args.size() >= 4)
    {
        dp::setAllowUnverifiedVersions(true);
        int mc = str2mc(qPrintable(args[3]));
        for (int frac : { 10, 50, 90, 99 })
        {
            writeFile(tmp, all.left(all.size() * frac / 100));
            dp::Report report;
            std::shared_ptr<const dp::World> w = loadWorld(mc, args[2], QStringList() << tmp, "minecraft:normal",
                &report, false);
            bool named = false;
            for (const dp::Issue& i : report.issues)
                if (i.severity == dp::Issue::Error && i.pack == "damaged.zip")
                    named = true;
            CHECK(!report.ok() && named, "a pack cut off at %d %% gives no error that names it", frac);
            for (const dp::DimensionInfo& d : w->dimensions())
                CHECK(!d.supported, "a dimension is supported although the pack is cut off");
            if (frac == 50)
                printReport(report);
        }
    }
    QFile::remove(tmp);
    return g_fail != 0;
}

//==============================================================================
// same: a pack as folder, zip and jar
//==============================================================================

static uint64_t worldDigest(const dp::World& w, QString *text)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](const void *p, size_t n) {
        const unsigned char *b = (const unsigned char*) p;
        for (size_t i = 0; i < n; i++)
            h = (h ^ b[i]) * 0x100000001b3ULL;
    };
    QString t;
    for (const dp::DimensionInfo& d : w.dimensions())
        t += QStringLiteral("%1 %2 %3 %4 %5 %6\n").arg(d.id).arg(d.minY).arg(d.height).arg(d.seaLevel)
            .arg(d.supported).arg(d.biomes.size());
    for (const dp::BiomeInfo& b : w.biomes())
        t += QStringLiteral("%1 %2 %3 %4\n").arg(b.id, b.name).arg(b.color, 0, 16).arg(b.vanilla);
    t += w.presets().join(' ');
    QByteArray tb = t.toUtf8();
    mix(tb.constData(), (size_t) tb.size());
    for (size_t dim = 0; dim < w.dimensions().size(); dim++)
    {
        if (!w.dimensions()[dim].supported)
            continue;
        dp::Sampler s(std::shared_ptr<const dp::World>(std::shared_ptr<const dp::World>(), &w), (int) dim, 12345);
        std::vector<int> out(96 * 96);
        for (int scale : { 1, 4, 64 })
        {
            for (int y : { -20, 64, 200 })
            {
                s.genBiomes(out.data(), scale, -300, 150, 96, 96, y);
                mix(out.data(), out.size() * sizeof(int));
            }
        }
    }
    if (text)
        *text = t;
    return h;
}

static int cmdSame(const QStringList& args)
{
    if (args.size() < 3)
    {
        printf("usage: dptest same <vanilla data> <mc> <pack> <the same pack in another form> ...\n"
            "       (a pack \"-\" stands for no pack: the vanilla data in two forms can be compared with\n"
            "        dptest same <folder> <mc> - and dptest same <jar> <mc> -, which print a digest)\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    uint64_t first = 0;
    QString firstText;
    for (int i = 2; i < args.size(); i++)
    {
        dp::Report report;
        QStringList packs;
        if (args[i] != "-")
            packs << args[i];
        QElapsedTimer timer;
        timer.start();
        std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, "minecraft:normal", &report, false);
        double ms = timer.nsecsElapsed() / 1e6;
        QString text;
        uint64_t h = worldDigest(*w, &text);
        int nsup = 0;
        for (const dp::DimensionInfo& d : w->dimensions())
            nsup += d.supported;
        printf("%s: loaded in %.1f ms, %zu issues, %d of %zu dimensions supported, %zu biomes, digest %016llx\n",
            qPrintable(QFileInfo(args[i]).fileName()), ms, report.issues.size(), nsup, w->dimensions().size(),
            w->biomes().size(), (unsigned long long) h);
        if (!report.ok() || getenv("DPTEST_VERBOSE"))
            printReport(report);
        CHECK(report.ok(), "the pack has errors");
        CHECK(nsup > 0, "no supported dimension");
        if (i == 2)
        {
            first = h;
            firstText = text;
        }
        else
        {
            CHECK(h == first, "the forms of the pack give different worlds");
            CHECK(text == firstText, "the forms of the pack give different lists");
        }
    }
    return g_fail != 0;
}

//==============================================================================
// robust: broken packs, overlays, tags, colours
//==============================================================================

struct TestPack
{
    QString dir;
    explicit TestPack(const QString& d) : dir(d)
    {
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
    }
    void meta(int format, const QByteArray& extra = QByteArray())
    {
        // from format 82 (1.21.9) on the game reads min_format / max_format
        const QByteArray f = QByteArray::number(format);
        writeFile(dir + "/pack.mcmeta", "{\"pack\":{" + (format < 82 ? "\"pack_format\":" + f
            : "\"min_format\":" + f + ",\"max_format\":" + f) + ",\"description\":\"test\"}" + extra + "}");
    }
    void file(const QString& rel, const QByteArray& data)
    {
        writeFile(dir + "/" + rel, data);
    }
};

static bool hasIssue(const dp::Report& r, dp::Issue::Severity sev, const QString& pack, const QString& id,
    const QString& part)
{
    for (const dp::Issue& i : r.issues)
        if (i.severity == sev && i.pack == pack && (id.isNull() || i.id == id) &&
            i.text.contains(part, Qt::CaseInsensitive))
            return true;
    return false;
}

static int cmdRobust(const QStringList& args)
{
    if (args.size() < 3)
    {
        printf("usage: dptest robust <vanilla data> <mc> <work dir>\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const QString vanilla = args[0];
    const int mc = str2mc(qPrintable(args[1]));
    const QString work = args[2];
    int fmin = 0, fmax = 0;
    dp::packFormatRange(mc, &fmin, &fmax);

    // Every broken pack must give an error that names the pack and the
    // resource, and leave no dimension marked as supported.
    struct Case
    {
        const char *name;
        const char *path;       // file below the pack
        const char *content;
        const char *id;         // id the error must name
        const char *text;       // part of its text
    };
    static const Case cases[] = {
        { "unknown-type", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\":\"minecraft:does_not_exist\",\"argument\":1}",
          "minecraft:overworld/continents", "unknown density function type" },
        { "unknown-type-nested", "data/minecraft/worldgen/density_function/overworld/erosion.json",
          "{\"type\":\"minecraft:add\",\"argument1\":0.5,\"argument2\":{\"type\":\"somemod:fancy\"}}",
          "minecraft:overworld/erosion", "unknown density function type" },
        { "unknown-type-unused", "data/broken/worldgen/density_function/unused.json",
          "{\"type\":\"minecraft:nope\"}", "broken:unused", "unknown density function type" },
        { "missing-reference", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\":\"minecraft:add\",\"argument1\":\"broken:not_there\",\"argument2\":0.1}",
          "minecraft:overworld/continents", "does not exist" },
        { "cycle", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\":\"minecraft:abs\",\"argument\":\"minecraft:overworld/continents\"}",
          "minecraft:overworld/continents", "refers to itself" },
        { "broken-json", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\": \"minecraft:add\", \"argument1\": ", "minecraft:overworld/continents", "not valid JSON" },
        { "missing-field", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\":\"minecraft:clamp\",\"input\":1.0,\"min\":0.0}", "minecraft:overworld/continents", "needs the number" },
        { "missing-noise", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\":\"minecraft:noise\",\"noise\":\"broken:nothing\",\"xz_scale\":1,\"y_scale\":1}",
          "minecraft:overworld/continents", "does not exist" },
        { "inline-noise", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\":\"minecraft:noise\",\"noise\":{\"firstOctave\":-3,\"amplitudes\":[1]},\"xz_scale\":1,\"y_scale\":1}",
          "minecraft:overworld/continents", "inline" },
        { "broken-noise", "data/minecraft/worldgen/noise/continentalness.json",
          "{\"firstOctave\":-9}", "minecraft:continentalness", "firstOctave and amplitudes" },
        { "empty-spline", "data/minecraft/worldgen/density_function/overworld/continents.json",
          "{\"type\":\"minecraft:spline\",\"spline\":{\"coordinate\":0.5,\"points\":[]}}",
          "minecraft:overworld/continents", "no points" },
        { "unknown-biome-source", "data/minecraft/dimension/overworld.json",
          "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":"
          "\"minecraft:overworld\",\"biome_source\":{\"type\":\"somemod:voronoi\"}}}",
          "minecraft:overworld", "unknown biome source type" },
        { "unknown-generator", "data/minecraft/dimension/overworld.json",
          "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"somemod:gen\"}}",
          "minecraft:overworld", "unknown generator type" },
        { "missing-settings", "data/minecraft/dimension/overworld.json",
          "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":"
          "\"broken:none\",\"biome_source\":{\"type\":\"minecraft:fixed\",\"biome\":\"minecraft:plains\"}}}",
          "minecraft:overworld", "does not exist" },
        { "bad-parameter", "data/minecraft/dimension/overworld.json",
          "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":"
          "\"minecraft:overworld\",\"biome_source\":{\"type\":\"minecraft:multi_noise\",\"biomes\":[{\"biome\":"
          "\"minecraft:plains\",\"parameters\":{\"temperature\":[0.5,-0.5],\"humidity\":0,\"continentalness\":0,"
          "\"erosion\":0,\"depth\":0,\"weirdness\":0,\"offset\":0}}]}}}",
          "minecraft:overworld", "minimum above its maximum" },
        { "broken-dimension-json", "data/minecraft/dimension/overworld.json",
          "{\"type\":", "minecraft:overworld", "not valid JSON" },
        { "broken-noise-settings", "data/minecraft/worldgen/noise_settings/overworld.json",
          "{\"sea_level\":63}", "minecraft:overworld", "noise_router" },
    };
    for (const Case& c : cases)
    {
        TestPack pack(work + "/robust-" + c.name);
        pack.meta(fmax);
        pack.file(c.path, c.content);
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList() << pack.dir, "minecraft:normal",
            &report, false);
        QString packName = QFileInfo(pack.dir).fileName();
        bool ok = hasIssue(report, dp::Issue::Error, packName, c.id, c.text);
        CHECK(ok, "%s: no error naming %s / %s with \"%s\"", c.name, qPrintable(packName), c.id, c.text);
        int nsup = 0;
        for (const dp::DimensionInfo& d : w->dimensions())
            nsup += d.supported;
        CHECK(nsup == 0, "%s: %d dimensions are still supported", c.name, nsup);
        dp::Sampler s(w, w->dimension("minecraft:overworld"), 1);
        CHECK(!s.valid(), "%s: a sampler of a broken world is valid", c.name);
        int out = 0;
        CHECK(!s.genBiomes(&out, 4, 0, 0, 1, 1, 64), "%s: genBiomes of an invalid sampler succeeds", c.name);
        const dp::Issue *first = nullptr;
        for (const dp::Issue& i : report.issues)
            if (i.severity == dp::Issue::Error && !first)
                first = &i;
        printf("%-22s %s: [%s / %s] %s\n", c.name, ok ? "ok" : "MISSING", first ? qPrintable(first->pack) : "",
            first ? qPrintable(first->id) : "", first ? qPrintable(first->text) : "(no error)");
    }

    // a pack that does not exist, an empty folder, a file that is no archive
    {
        dp::Report report;
        loadWorld(mc, vanilla, QStringList() << work + "/no-such-pack.zip", "minecraft:normal", &report, false);
        CHECK(hasIssue(report, dp::Issue::Error, "no-such-pack.zip", QString(), "not found"), "missing pack: no error");
        TestPack empty(work + "/robust-empty");
        dp::Report r2;
        std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList() << empty.dir, "minecraft:normal", &r2, false);
        CHECK(r2.ok(), "an empty folder is an error");
        CHECK(hasIssue(r2, dp::Issue::Warning, "robust-empty", QString(), "pack.mcmeta"), "empty folder: no warning");
        CHECK(w->dimension("minecraft:overworld") >= 0 && w->dimensions()[w->dimension("minecraft:overworld")].supported,
            "an empty pack breaks the world");
        writeFile(work + "/robust-notazip.zip", QByteArray(5000, 'x'));
        dp::Report r3;
        loadWorld(mc, vanilla, QStringList() << work + "/robust-notazip.zip", "minecraft:normal", &r3, false);
        CHECK(hasIssue(r3, dp::Issue::Error, "robust-notazip.zip", QString(), "archive"), "not a zip: no error");
        dp::Report r4;
        loadWorld(mc, work + "/no-vanilla-here", QStringList(), "minecraft:normal", &r4, false);
        CHECK(!r4.ok(), "missing vanilla data is no error");
        dp::Report r5;
        std::shared_ptr<const dp::World> w5 = loadWorld(mc, vanilla, QStringList(), "minecraft:no_such_preset", &r5, false);
        CHECK(!r5.ok() && w5->dimensions().empty(), "an unknown preset is no error");
        printf("missing pack, empty folder, not an archive, missing vanilla data, unknown preset: checked\n");
    }

    // JSON with comments loads; the result is that of the same file without
    {
        TestPack a(work + "/robust-comments");
        a.meta(fmax);
        a.file("data/minecraft/worldgen/density_function/overworld/continents.json",
            "// a comment\n{\n  \"type\": \"minecraft:add\", /* inline */\n  \"argument1\": 0.25, // why not\n"
            "  \"argument2\": \"minecraft:overworld/erosion\" # hash comment\n}\n");
        TestPack b(work + "/robust-nocomments");
        b.meta(fmax);
        b.file("data/minecraft/worldgen/density_function/overworld/continents.json",
            "{\"type\":\"minecraft:add\",\"argument1\":0.25,\"argument2\":\"minecraft:overworld/erosion\"}");
        dp::Report ra, rb;
        std::shared_ptr<const dp::World> wa = loadWorld(mc, vanilla, QStringList() << a.dir, "minecraft:normal", &ra, false);
        std::shared_ptr<const dp::World> wb = loadWorld(mc, vanilla, QStringList() << b.dir, "minecraft:normal", &rb, false);
        CHECK(ra.ok() && rb.ok(), "comments: errors");
        if (!ra.ok()) printReport(ra);
        CHECK(worldDigest(*wa, nullptr) == worldDigest(*wb, nullptr), "comments change the world");
        dp::Report rv;
        std::shared_ptr<const dp::World> wv = loadWorld(mc, vanilla, QStringList(), "minecraft:normal", &rv, false);
        CHECK(worldDigest(*wa, nullptr) != worldDigest(*wv, nullptr), "the override has no effect");
        printf("JSON with comments: loads, same world as without comments, differs from vanilla\n");
    }

    // overlays, pack order, tags, fixed and checkerboard sources, colours
    {
        TestPack a(work + "/robust-layers-a");
        // the notation of the version: "formats" before 1.21.9 (format 82),
        // min_format / max_format from then on
        const QByteArray f0 = QByteArray::number(fmax);
        QByteArray overlays = fmax < 82 ? ",\"overlays\":{\"entries\":["
            "{\"formats\":[1,5],\"directory\":\"old\"},"
            "{\"formats\":{\"min_inclusive\":" + f0 + ",\"max_inclusive\":" + QByteArray::number(fmax + 5) + "},\"directory\":\"first\"},"
            "{\"formats\":" + f0 + ",\"directory\":\"second\"},"
            "{\"formats\":[" + QByteArray::number(fmax + 1) + "," + QByteArray::number(fmax + 9) + "],\"directory\":\"future\"}]}"
            : ",\"overlays\":{\"entries\":["
            "{\"min_format\":82,\"max_format\":[" + QByteArray::number(fmax - 1) + ",5],\"directory\":\"old\"},"
            "{\"min_format\":" + f0 + ",\"max_format\":" + QByteArray::number(fmax + 5) + ",\"directory\":\"first\"},"
            "{\"min_format\":[" + f0 + "],\"max_format\":[" + f0 + "],\"directory\":\"second\"},"
            "{\"min_format\":" + QByteArray::number(fmax + 1) + ",\"max_format\":" + QByteArray::number(fmax + 9) + ",\"directory\":\"future\"}]}";
        a.meta(fmax, overlays);
        // main: constant 1; "old" 2 (not applied); "first" 3; "second" 4 (wins); "future" 5 (not applied)
        const QString df = "data/layers/worldgen/density_function/";
        a.file(df + "value.json", "1.0");
        a.file("old/" + df + "value.json", "2.0");
        a.file("first/" + df + "value.json", "3.0");
        a.file("second/" + df + "value.json", "4.0");
        a.file("future/" + df + "value.json", "5.0");
        a.file(df + "only_main.json", "10.0");
        a.file("first/" + df + "only_first.json", "30.0");
        a.file("old/" + df + "only_old.json", "20.0");
        const QString tg = "data/layers/tags/worldgen/biome/";
        a.file(tg + "list.json", "{\"values\":[\"minecraft:plains\",\"minecraft:desert\"]}");
        a.file(tg + "other.json", "{\"values\":[\"minecraft:taiga\",\"#layers:list\",{\"id\":\"minecraft:plains\",\"required\":false}]}");
        a.file(tg + "replaced.json", "{\"values\":[\"minecraft:ocean\"]}");
        a.file("data/c/worldgen/biome_colors.json",
            "{\"minecraft:plains\":{\"r\":1,\"g\":2,\"b\":3,\"name\":\"Flatland\"},\"layers:custom\":{\"r\":10,\"g\":20,\"b\":30}}");
        a.file("data/layers/worldgen/biome/custom.json", "{\"effects\":{\"grass_color\":8431445}}");
        a.file("data/layers/worldgen/biome/other_biome.json", "{\"effects\":{}}");

        TestPack b(work + "/robust-layers-b");
        b.meta(fmax);
        b.file(tg + "list.json", "{\"values\":[\"minecraft:forest\"]}");
        b.file(tg + "replaced.json", "{\"replace\":true,\"values\":[\"minecraft:river\",\"minecraft:beach\"]}");
        b.file(df + "only_main.json", "11.0");
        b.file("data/minecraft/dimension/overworld.json",
            "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":"
            "\"minecraft:overworld\",\"biome_source\":{\"type\":\"minecraft:checkerboard\",\"scale\":1,"
            "\"biomes\":\"#layers:other\"}}}");
        b.file("data/layers/dimension/flatland.json",
            "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":"
            "\"minecraft:overworld\",\"biome_source\":{\"type\":\"minecraft:fixed\",\"biome\":\"layers:custom\"}}}");
        b.file("data/layers/dimension/other.json",
            "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":"
            "\"minecraft:overworld\",\"biome_source\":{\"type\":\"minecraft:fixed\",\"biome\":\"layers:other_biome\"}}}");
        b.file("data/layers/dimension/flat.json",
            "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:flat\",\"settings\":{}}}");

        dp::Request req;
        req.mc = mc;
        req.vanillaPath = vanilla;
        dp::PackSource pa, pb;
        pa.path = a.dir;
        pb.path = b.dir;
        req.packs = { pa, pb };
        dp::Resources res;
        dp::Report report;
        dp::loadResources(req, &res, &report);
        auto value = [&](const char *id) {
            const dp::Resource *r = res.find(dp::REG_DENSITY_FUNCTION, id);
            return r ? QString::fromUtf8(r->data) + "@" + QString::number(r->pack) : QString("-");
        };
        // overlays exist since 1.20.2 (format 18)
        const bool withOverlays = fmax >= 18;
        CHECK(value("layers:value") == (withOverlays ? "4.0@1" : "1.0@1"), "overlay order: got %s",
            qPrintable(value("layers:value")));
        CHECK(value("layers:only_first") == (withOverlays ? "30.0@1" : "-"), "overlay file: got %s",
            qPrintable(value("layers:only_first")));
        CHECK(res.packs[1].overlays == (withOverlays ? QStringList({ "first", "second" }) : QStringList()),
            "applied overlays: %s", qPrintable(res.packs[1].overlays.join(',')));
        CHECK(value("layers:only_main") == "11.0@2", "pack order: got %s", qPrintable(value("layers:only_main")));
        CHECK(value("layers:only_old") == "-", "an overlay outside its formats was applied");
        CHECK(res.tagValues(dp::REG_BIOME_TAG, "layers:list") ==
            QStringList({ "minecraft:plains", "minecraft:desert", "minecraft:forest" }), "tag merge: %s",
            qPrintable(res.tagValues(dp::REG_BIOME_TAG, "layers:list").join(',')));
        CHECK(res.tagValues(dp::REG_BIOME_TAG, "layers:replaced") ==
            QStringList({ "minecraft:river", "minecraft:beach" }), "tag replace: %s",
            qPrintable(res.tagValues(dp::REG_BIOME_TAG, "layers:replaced").join(',')));
        CHECK(res.tagValues(dp::REG_BIOME_TAG, "layers:other") ==
            QStringList({ "minecraft:taiga", "minecraft:plains", "minecraft:desert", "minecraft:forest" }),
            "nested tag: %s", qPrintable(res.tagValues(dp::REG_BIOME_TAG, "layers:other").join(',')));
        // vanilla tags are there and merged with nothing
        CHECK(res.tagValues(dp::REG_BIOME_TAG, "minecraft:is_overworld").size() > 40, "vanilla tag missing");

        // disabled packs are skipped
        req.packs[1].enabled = false;
        dp::Report rd;
        std::shared_ptr<const dp::World> wd = dp::World::load(req, &rd);
        CHECK(wd->packs().size() == 1 && wd->dimension("layers:flatland") < 0, "a disabled pack was loaded");
        req.packs[1].enabled = true;

        dp::Report rw;
        std::shared_ptr<const dp::World> w = dp::World::load(req, &rw);
        if (!rw.ok())
            printReport(rw);
        CHECK(rw.ok(), "layers: the world has errors");
        CHECK(w->packs().size() == 2 && w->packs()[0].worldgenFiles == (withOverlays ? 6 : 5) &&
            w->packs()[0].packFormat == fmax &&
            w->packs()[0].description == "test", "pack info: %d files", w->packs().empty() ? -1 : w->packs()[0].worldgenFiles);
        int ow = w->dimension("minecraft:overworld");
        int fl = w->dimension("layers:flatland");
        int ot = w->dimension("layers:other");
        int ft = w->dimension("layers:flat");
        CHECK(ow >= 0 && fl >= 0 && ot >= 0 && ft >= 0, "dimensions missing");
        if (ow >= 0 && fl >= 0 && ot >= 0 && ft >= 0)
        {
            CHECK(!w->dimensions()[ft].supported && !w->dimensions()[ft].reason.isEmpty(), "flat generator");
            CHECK(w->dimensions()[ow].supported && w->dimensions()[ow].biomes.size() == 4, "checkerboard biomes");
            // squares of 2^(1+2) = 8 cells: taiga, plains, desert, forest along x
            dp::Sampler s(w, ow, 5);
            int row[40];
            CHECK(s.genBiomes(row, 4, -8, 0, 40, 1, 64), "checkerboard genBiomes");
            QStringList names;
            for (int i = 0; i < 40; i += 8)
                names << w->biomes()[row[i]].id;
            CHECK(names == QStringList({ "minecraft:forest", "minecraft:taiga", "minecraft:plains",
                "minecraft:desert", "minecraft:forest" }), "checkerboard: %s", qPrintable(names.join(',')));
            CHECK(row[0] == row[7] && row[8] == row[15] && row[7] != row[8], "checkerboard squares");
            int v = 0;
            CHECK(s.genBiomes(&v, 4, 3, -1, 1, 1, 64) && w->biomes()[v].id == "minecraft:forest",
                "checkerboard with negative z");
            // colours and names
            dp::Sampler sf(w, fl, 5);
            int fb = sf.biomeAt(4, 100, 64, -100);
            CHECK(fb >= 0 && w->biomes()[fb].id == "layers:custom", "fixed biome");
            if (fb >= 0)
                CHECK(w->biomes()[fb].color == qRgb(10, 20, 30) && w->biomes()[fb].name == "Custom" &&
                    !w->biomes()[fb].vanilla, "pack colour");
            int plains = -1, taiga = -1, other = -1;
            for (size_t i = 0; i < w->biomes().size(); i++)
            {
                if (w->biomes()[i].id == "minecraft:plains") plains = (int) i;
                if (w->biomes()[i].id == "minecraft:taiga") taiga = (int) i;
                if (w->biomes()[i].id == "layers:other_biome") other = (int) i;
            }
            CHECK(plains >= 0 && w->biomes()[plains].color == qRgb(1, 2, 3) &&
                w->biomes()[plains].name == "Flatland" && w->biomes()[plains].vanilla, "colour file over vanilla");
            unsigned char colors[256][3];
            initBiomeColors(colors);
            CHECK(taiga >= 0 && w->biomes()[taiga].color == qRgb(colors[::taiga][0], colors[::taiga][1], colors[::taiga][2]) &&
                w->biomes()[taiga].name == "Taiga", "engine colour of a vanilla biome");
            CHECK(other >= 0 && w->biomes()[other].name == "Other Biome" && qAlpha(w->biomes()[other].color) == 255,
                "fallback colour");
        }
        printf("overlays, pack order, tags, disabled packs, fixed and checkerboard sources, colours: checked\n");
    }

    // pack.mcmeta as the game reads it: numbers with a fraction are their
    // integer part; before 1.21.9 (format 82) overlay entries are read from
    // "formats" alone, and one entry without it drops all overlays; before
    // 1.20.2 (format 18) there are no overlays
    {
        TestPack c(work + "/robust-overlay-rules");
        const QByteArray f = QByteArray::number(fmax);
        writeFile(c.dir + "/pack.mcmeta", "{\"pack\":{\"pack_format\":" + f + ".7,\"supported_formats\":[" + f +
            ",999.5],\"description\":\"test\"},\"overlays\":{\"entries\":["
            "{\"formats\":[" + f + "," + f + ".9],\"directory\":\"with_formats\"},"
            "{\"min_format\":" + f + ",\"max_format\":" + f + ",\"directory\":\"new_notation\"}]}}");
        const QString df = "data/rules/worldgen/density_function/";
        c.file(df + "value.json", "1.0");
        c.file("with_formats/" + df + "value.json", "2.0");
        c.file("new_notation/" + df + "only_new.json", "3.0");
        dp::Request req;
        req.mc = mc;
        req.vanillaPath = vanilla;
        dp::PackSource pc;
        pc.path = c.dir;
        req.packs = { pc };
        dp::Resources res;
        dp::Report report;
        dp::loadResources(req, &res, &report);
        const dp::Resource *v = res.find(dp::REG_DENSITY_FUNCTION, "rules:value");
        const dp::Resource *n = res.find(dp::REG_DENSITY_FUNCTION, "rules:only_new");
        const QString pack = QFileInfo(c.dir).fileName();
        const bool dropped = hasIssue(report, dp::Issue::Warning, pack, "pack.mcmeta", "ignores the overlays");
        if (fmax < 82)
            CHECK(res.packs.size() == 2 && res.packs[1].packFormat == fmax && res.packs[1].formatMatches,
                "fractional formats: pack format %d, %s", res.packs.size() == 2 ? res.packs[1].packFormat : -1,
                res.packs.size() == 2 && res.packs[1].formatMatches ? "matches" : "does not match");
        // from 1.21.9 on "supported_formats" and overlay "formats" beyond 81
        // are errors: the pack's formats are unknown, and its overlays are
        // ignored (1.21.9 .. 1.21.11) or the pack is not loaded (26.1 on)
        if (fmax >= 101)
            CHECK(res.packs.size() == 2 && !res.packs[1].loaded && !v && !n
                && hasIssue(report, dp::Issue::Warning, pack, "pack.mcmeta", "does not load this pack"),
                "a pack with broken overlays from 26.1 on: %s", v ? "loaded" : "not loaded");
        else if (fmax >= 82)
            CHECK(res.packs.size() == 2 && res.packs[1].loaded && !res.packs[1].formatMatches && v
                && v->data == "1.0" && !n && dropped, "broken overlays in 1.21.9 .. 1.21.11: %s, %s",
                v ? v->data.constData() : "-", dropped ? "dropped" : "not dropped");
        if (fmax < 18)
            CHECK(v && v->data == "1.0" && !n && !dropped, "overlays before 1.20.2");
        else if (fmax < 82)
            CHECK(v && v->data == "1.0" && !n && dropped, "an overlay without formats before 1.21.9: %s, %s",
                v ? v->data.constData() : "-", n ? "the other overlay applied" : "dropped");
        printf("pack.mcmeta numbers with a fraction, overlay entries without formats: checked\n");
    }

    // versions: load() refuses what versionSupported() does not list
    {
        dp::setAllowUnverifiedVersions(false);
        CHECK(dp::versionSupported(MC_1_21_1) && dp::versionSupported(MC_1_19_2) && dp::versionSupported(MC_1_21_11) &&
            dp::versionSupported(MC_1_18_2), "versionSupported() misses verified versions");
        CHECK(!dp::versionSupported(MC_1_17_1) && !dp::versionSupported(MC_1_17) && !dp::versionSupported(-1),
            "versionSupported() lists unverified versions");
        if (dp::versionSupported(mc))
        {
            dp::Report r;
            std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList(), "minecraft:normal", &r, false);
            CHECK(r.ok() && w->dimension("minecraft:overworld") >= 0 &&
                w->dimensions()[w->dimension("minecraft:overworld")].supported, "a supported version does not load");
        }
        dp::Report r2;
        std::shared_ptr<const dp::World> w2 = loadWorld(MC_1_17_1, vanilla, QStringList(), "minecraft:normal", &r2, false);
        CHECK(!r2.ok() && w2->dimensions().empty(), "an unsupported version loads");
        std::shared_ptr<const dp::World> w3 = dp::World::load(dp::Request(), nullptr);
        CHECK(w3 && w3->dimensions().empty() && w3->dimension("minecraft:overworld") == -1, "an empty request");
        dp::Sampler s3(w3, 0, 1);
        dp::Sampler s4(nullptr, 0, 1);
        CHECK(!s3.valid() && !s4.valid() && s4.biomeAt(4, 0, 0, 0) == -1, "samplers of nothing are valid");
        dp::setAllowUnverifiedVersions(true);
        printf("versionSupported(), unsupported versions, empty requests: checked\n");
    }

    // the colours of a pack's biomes that share one grass colour: apart from
    // each other, the same with the tint in the notation of 26.1 ("#rrggbb"),
    // on every load and whatever other packs are loaded
    {
        const int n = 24;
        auto makePack = [&](const QString& name, const QString& ns, bool hex) {
            TestPack p(work + "/" + name);
            p.meta(fmax);
            QByteArray list;
            for (int i = 0; i < n; i++)
            {
                const QByteArray tint = hex ? QByteArray("\"#4f8f6a\"") : QByteArray::number(0x4f8f6a);
                p.file("data/" + ns + "/worldgen/biome/b" + QString::number(i) + ".json",
                    "{\"effects\":{\"grass_color\":" + tint + "}}");
                list += (i ? ",\"" : "\"") + ns.toUtf8() + ":b" + QByteArray::number(i) + "\"";
            }
            p.file("data/" + ns + "/dimension/board.json",
                "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":"
                "\"minecraft:overworld\",\"biome_source\":{\"type\":\"minecraft:checkerboard\",\"scale\":0,"
                "\"biomes\":[" + list + "]}}}");
            return p.dir;
        };
        auto colorsOf = [&](const QStringList& dirs, const QString& ns) {
            dp::Request req;
            req.mc = mc;
            req.vanillaPath = vanilla;
            for (const QString& d : dirs)
            {
                dp::PackSource ps;
                ps.path = d;
                req.packs.push_back(ps);
            }
            dp::Report r;
            std::shared_ptr<const dp::World> w = dp::World::load(req, &r);
            std::map<QString, QRgb> out;
            for (const dp::BiomeInfo& b : w->biomes())
                if (b.id.startsWith(ns + ":"))
                    out[b.id] = b.color;
            return out;
        };
        const QString a = makePack("robust-colors-a", "tinted", false);
        const QString h = makePack("robust-colors-hex", "tinted", true);
        const QString o = makePack("robust-colors-other", "other", false);
        const auto ca = colorsOf({a}, "tinted");
        double minDist = 1e9;
        for (auto i = ca.begin(); i != ca.end(); ++i)
            for (auto j = std::next(i); j != ca.end(); ++j)
                minDist = std::min(minDist, dp::colorDistance(i->second, j->second));
        printf("%zu biomes of one grass colour: closest colours %.0f apart\n", ca.size(), minDist);
        CHECK((int) ca.size() == n && minDist >= dp::MIN_BIOME_COLOR_DISTANCE,
            "biomes of one pack with one grass colour: %zu biomes, closest %.0f", ca.size(), minDist);
        CHECK(colorsOf({h}, "tinted") == ca, "the tint as \"#rrggbb\" gives other colours");
        CHECK(colorsOf({a}, "tinted") == ca, "colours differ between two loads");
        CHECK(colorsOf({o, a}, "tinted") == ca && colorsOf({a, o}, "tinted") == ca,
            "another pack changes the colours of a pack's biomes");
    }

    // fallback colours: stable, apart from each other, not glaring
    {
        QJsonObject none;
        QStringList ids;
        for (int i = 0; i < 300; i++)
            ids << QStringLiteral("somepack:biome_%1").arg(i);
        int close = 0, dull = 0;
        std::vector<QRgb> cols;
        for (const QString& id : ids)
        {
            QRgb c = dp::fallbackBiomeColor(id, none);
            CHECK(c == dp::fallbackBiomeColor(id, none), "fallback colour is not stable");
            int mx = std::max(qRed(c), std::max(qGreen(c), qBlue(c)));
            int mn = std::min(qRed(c), std::min(qGreen(c), qBlue(c)));
            if (mx - mn < 40 || mx < 90 || mn > 215)
                dull++;
            cols.push_back(c);
        }
        for (size_t i = 0; i < cols.size(); i++)
            for (size_t j = i + 1; j < cols.size(); j++)
                if (abs(qRed(cols[i]) - qRed(cols[j])) + abs(qGreen(cols[i]) - qGreen(cols[j])) +
                    abs(qBlue(cols[i]) - qBlue(cols[j])) < 12)
                    close++;
        printf("fallback colours of 300 ids: %d grey or extreme, %d of %zu pairs nearly equal\n",
            dull, close, cols.size() * (cols.size() - 1) / 2);
        CHECK(dull == 0, "dull fallback colours");
        CHECK(close < 60, "fallback colours are too close");
        CHECK(dp::biomeDisplayName("somepack:alpine_grove") == "Alpine Grove", "display name");
        CHECK(dp::biomeDisplayName("ns:cave/deep_dark") == "Cave / Deep Dark", "display name with folder");
    }
    return g_fail != 0;
}

//==============================================================================
// terrain: density function types that the climate does not use
//==============================================================================

// The vanilla terrain functions (old_blended_noise, the cave functions with
// weird_scaled_sampler, range_choice, clamp, min, max, the "interpolated"
// argument at cell corners) are put into the six climate slots of a probe
// dimension and compared with the engine's terrain model and with the
// heightmaps of real servers.
static int cmdTerrain(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QString serverDir;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--server" && i + 1 < args.size()) { serverDir = args[i + 1]; args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 3)
    {
        printf("usage: dptest terrain <vanilla data folder> <mc> <work dir> [--server <tools/worldgen-reference/data>]\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const QString vanilla = args[0];
    const int mc = str2mc(qPrintable(args[1]));
    const char *mcName = mc2str(mc);
    int fmin = 0, fmax = 0;
    dp::packFormatRange(mc, &fmin, &fmax);

    QFile nf(vanilla + "/data/minecraft/worldgen/noise_settings/overworld.json");
    if (!nf.open(QIODevice::ReadOnly))
    {
        printf("cannot read the vanilla noise settings (the vanilla data must be a folder here)\n");
        return 2;
    }
    QJsonObject settings = QJsonDocument::fromJson(nf.readAll()).object();
    QJsonObject vrouter = settings.value("noise_router").toObject();

    TestPack pack(args[2] + "/probe-terrain");
    pack.meta(fmax);
    pack.file("data/probe/worldgen/density_function/slides.json",
        "{\"type\":\"minecraft:add\",\"argument1\":0.1171875,\"argument2\":{\"type\":\"minecraft:mul\","
        "\"argument1\":{\"type\":\"minecraft:y_clamped_gradient\",\"from_y\":-64,\"to_y\":-40,\"from_value\":0.0,\"to_value\":1.0},"
        "\"argument2\":{\"type\":\"minecraft:add\",\"argument1\":-0.1171875,\"argument2\":{\"type\":\"minecraft:add\","
        "\"argument1\":-0.078125,\"argument2\":{\"type\":\"minecraft:mul\",\"argument1\":{\"type\":"
        "\"minecraft:y_clamped_gradient\",\"from_y\":240,\"to_y\":256,\"from_value\":1.0,\"to_value\":0.0},"
        "\"argument2\":{\"type\":\"minecraft:add\",\"argument1\":0.078125,\"argument2\":"
        "\"minecraft:overworld/sloped_cheese\"}}}}}}");
    QJsonObject router;
    router["temperature"] = "minecraft:overworld/base_3d_noise";
    router["vegetation"] = "probe:slides";
    router["continents"] = "minecraft:overworld/sloped_cheese";
    router["erosion"] = vrouter.value("final_density");
    router["depth"] = "minecraft:overworld/caves/spaghetti_2d";
    router["ridges"] = "minecraft:overworld/caves/pillars";
    QJsonObject ns;
    ns["sea_level"] = 63;
    ns["legacy_random_source"] = false;
    ns["noise"] = settings.value("noise");
    ns["noise_router"] = router;
    pack.file("data/probe/worldgen/noise_settings/probe.json", QJsonDocument(ns).toJson(QJsonDocument::Compact));
    pack.file("data/probe/dimension/probe.json",
        "{\"type\":\"minecraft:overworld\",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":\"probe:probe\","
        "\"biome_source\":{\"type\":\"minecraft:multi_noise\",\"biomes\":[{\"biome\":\"minecraft:plains\",\"parameters\":{"
        "\"temperature\":0,\"humidity\":0,\"continentalness\":0,\"erosion\":0,\"depth\":0,\"weirdness\":0,\"offset\":0}}]}}}");

    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList() << pack.dir, "minecraft:normal", &report);
    CHECK(report.ok(), "the probe pack has errors");
    int dim = w->dimension("probe:probe");
    CHECK(dim >= 0 && w->dimensions()[dim].supported, "no probe dimension");
    if (dim < 0 || !w->dimensions()[dim].supported)
        return 1;
    const dp::WorldPrivate *wp = w->d();
    const dp::DimData& d = wp->data[dim];
    printf("%s probe: %zu column + %zu vertical nodes, %zu noises, blended noise %s\n", mcName,
        d.program.column.size(), d.program.vertical.size(), d.program.usedNoises.size(),
        d.program.usesBlended ? "yes" : "no");

    // against the engine's terrain model, at cell corners
    {
        Generator g;
        setupGenerator(&g, mc, 0);
        std::mt19937_64 rng(31);
        Counter base;
        double maxDiff = 0, sumDiff = 0;
        int64_t nd = 0, nonFinite = 0, solid = 0, total = 0;
        double lo[6], hi[6];
        for (int k = 0; k < 6; k++) { lo[k] = 1e300; hi[k] = -1e300; }
        for (int si = 0; si < 3; si++)
        {
            uint64_t seed = g_seeds[si];
            applySeed(&g, DIM_OVERWORLD, seed);
            TerrainNoise tn;
            initTerrainNoise(&tn, seed);
            dp::DensityState st(*wp->graph, d.program, seed, mc, d.legacy);
            for (int i = 0; i < 6000; i++)
            {
                int x = (int) (rng() % 40000) - 20000, z = (int) (rng() % 40000) - 20000;
                TerrainColumn tc;
                sampleTerrainColumn(&g.bn, &tn, x, z, 0, &tc);
                st.evalColumn(x * 4, z * 4);
                for (int k = TERRAIN_K_MIN; k < TERRAIN_K_MAX; k += 1 + (int) (rng() % 4))
                {
                    st.evalVertical(k * 8);
                    double v[6];
                    for (int q = 0; q < 6; q++)
                    {
                        v[q] = st.value(d.outputs[q]);
                        if (!(v[q] == v[q]) || v[q] > 1e30 || v[q] < -1e30)
                            nonFinite++;
                        lo[q] = std::min(lo[q], v[q]);
                        hi[q] = std::max(hi[q], v[q]);
                    }
                    base.add(v[0] == sampleTerrainBaseNoise(&tn, x, k, z));
                    total++;
                    if (v[3] > 0)
                        solid++;
                    if (v[2] < 1.5625)
                    {
                        // outside the cave branch the engine's density is
                        // the slides of sloped_cheese
                        double e = sampleTerrainDensity(&tc, &tn, k);
                        double df = fabs(e - v[1]);
                        maxDiff = std::max(maxDiff, df);
                        sumDiff += df;
                        nd++;
                    }
                }
            }
        }
        printf("old_blended_noise (overworld/base_3d_noise) against the engine: %lld of %lld cell corners differ\n",
            (long long) base.diff, (long long) base.n);
        printf("slides(sloped_cheese) against the engine's terrain density: %lld corners, largest difference %.3g, "
            "mean %.3g\n", (long long) nd, maxDiff, nd ? sumDiff / nd : 0.0);
        static const char *names[6] = { "base_3d_noise", "slides", "sloped_cheese", "final_density", "spaghetti_2d", "pillars" };
        for (int q = 0; q < 6; q++)
            printf("  %-14s %.4f .. %.4f\n", names[q], lo[q], hi[q]);
        printf("  final_density positive at %.1f %% of the corners, %lld values not finite\n",
            100.0 * solid / (double) total, (long long) nonFinite);
        CHECK(base.diff == 0, "old_blended_noise differs from the engine");
        CHECK(maxDiff < 1e-5, "sloped_cheese with slides differs from the engine by %.3g", maxDiff);
        CHECK(nonFinite == 0, "non-finite densities");
    }

    // against the heightmaps of real servers: in the columns of cell corners
    // the block at a corner height is solid exactly where final_density is
    // positive (before carvers and features)
    if (!serverDir.isEmpty())
    {
        for (int si = 0; si < 4; si++)
        {
            QString file = QStringLiteral("%1/normal_%2_%3.txt").arg(serverDir, mcName).arg((qlonglong) g_seeds[si]);
            QFile f(file);
            if (!QFileInfo::exists(file))
            {
                printf("no server file %s\n", qPrintable(file));
                continue;
            }
            if (!f.open(QIODevice::ReadOnly))
            {
                CHECK(false, "the server file %s cannot be read", qPrintable(file));
                continue;
            }
            dp::DensityState st(*wp->graph, d.program, g_seeds[si], mc, d.legacy);
            int64_t cols = 0, bracket = 0, above = 0, below = 0, n = 0;
            while (!f.atEnd())
            {
                QByteArray line = f.readLine().trimmed();
                if (!line.startsWith("H "))
                    continue;
                if (n++ % 5 != 0)
                    continue;
                QList<QByteArray> p = line.split(' ');
                if (p.size() != 5)
                    continue;
                int x = p[1].toInt(), z = p[2].toInt(), floor = p[4].toInt();
                st.evalColumn(x, z);
                int top = -64;      // the highest corner with a positive density
                for (int y = 312; y >= -64; y -= 8)
                {
                    st.evalVertical(y);
                    if (st.value(d.outputs[3]) > 0)
                    {
                        top = y;
                        break;
                    }
                }
                cols++;
                if (floor >= top && floor < top + 8)
                    bracket++;      // the surface is in the cell above that corner
                else if (floor >= top + 8)
                    above++;        // something higher: trees and other features
                else
                    below++;        // the corner block is missing: carvers, or a wrong density
            }
            if (cols == 0)
            {
                CHECK(false, "the server file %s has no H lines", qPrintable(file));
                continue;
            }
            printf("final_density against the server's ocean_floor, seed %lld: %lld columns; surface within the "
                "cell above the highest solid corner %.2f %%, higher (trees, features) %.2f %%, lower (carved "
                "or wrong) %.2f %%\n", (long long) g_seeds[si], (long long) cols, 100.0 * bracket / cols,
                100.0 * above / cols, 100.0 * below / cols);
            CHECK(100.0 * below / cols < 2.0, "the density is positive above the server's surface too often");
            CHECK(100.0 * bracket / cols > 70.0, "the surface is too rarely where final_density puts it");
        }
    }
    return g_fail != 0;
}

//==============================================================================
// pack: a data pack against the biome cells of a real server that ran it
//==============================================================================

static int cmdPack(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QString preset = "minecraft:normal", dimName = "minecraft:overworld";
    QStringList packs;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--preset" && i + 1 < args.size()) { preset = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--dimension" && i + 1 < args.size()) { dimName = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--chain-only") { g_chainOnly = true; args.remove(i); }
        else i++;
    }
    if (args.size() < 2 || packs.isEmpty())
    {
        printf("usage: dptest pack <vanilla data> <mc> --pack <pack> [--pack <pack> ...] [--preset <id>]\n"
            "           [--dimension <id>] <server file> ...\n"
            "       server files: tools/worldgen-reference format, the seed is read from their header\n"
            "       --chain-only: the files are of another world and only give chunk positions; compares\n"
            "       genBiomes with whole chunks searched in the game's order, not with the server's biomes\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    dp::Report report;
    QElapsedTimer timer;
    timer.start();
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, preset, &report);
    printf("loaded in %.1f ms\n", timer.nsecsElapsed() / 1e6);
    CHECK(report.ok(), "the world has errors");
    for (const dp::PackInfo& p : w->packs())
        printf("pack %s: format %d%s, overlays [%s], %d worldgen files, \"%s\"\n", qPrintable(p.name),
            p.packFormat, p.formatMatches ? "" : " (does not match the version)",
            qPrintable(p.overlays.join(", ")), p.worldgenFiles, qPrintable(p.description));
    for (const dp::DimensionInfo& d : w->dimensions())
        printf("dimension %s: y %d .. %d, sea level %d, %s%s, %zu biomes\n", qPrintable(d.id), d.minY,
            d.minY + d.height, d.seaLevel, d.supported ? "supported" : "not supported: ",
            qPrintable(d.reason), d.biomes.size());
    int dim = w->dimension(dimName);
    CHECK(dim >= 0 && w->dimensions()[dim].supported, "the dimension %s is not supported", qPrintable(dimName));
    if (dim < 0 || !w->dimensions()[dim].supported)
        return 1;
    const dp::DimData& dd = w->d()->data[dim];
    printf("%s: %zu column + %zu vertical nodes, %zu noises, %zu list entries\n", qPrintable(dimName),
        dd.program.column.size(), dd.program.vertical.size(), dd.program.usedNoises.size(), dd.tree.entryCount());

    {
        // which operations the climate of the dimension uses
        static const char *opNames[] = {
            "constant", "y_clamped_gradient", "noise", "shifted_noise", "shift_a", "shift_b", "shift",
            "add", "mul", "min", "max", "abs", "square", "cube", "half_negative", "quarter_negative",
            "squeeze", "invert", "clamp", "range_choice", "spline", "old_blended_noise",
            "weird_scaled_sampler", "end_islands", "find_top_surface",
            "peaks_and_valleys", "terrain_shaper_spline", "slide", "old_blended_noise (1.18.2)",
            "gradient", "sqrt", "negate", "log", "sign", "round", "sub", "div", "pow", "lerp",
            "distance_to_point", "slice", "interpolated", "end_outer_islands", "old_blended_noise (26.3)"
        };
        std::map<QString, int> ops;
        const dp::DensityGraph& graph = *w->d()->graph;
        for (const std::vector<int32_t> *list : { &dd.program.column, &dd.program.vertical })
            for (int32_t i : *list)
            {
                int op = (int) graph.nodes[i].op;
                QString name = op < (int) (sizeof(opNames) / sizeof(*opNames)) ? opNames[op]
                    : op == 64 ? "add (constant)" : op == 65 ? "mul (constant)" : "?";
                if (graph.nodes[i].flags & dp::DF_FLAT)
                    name += " [y 0]";
                ops[name]++;
            }
        QStringList parts;
        for (const auto& o : ops)
            parts << QStringLiteral("%1 x%2").arg(o.first).arg(o.second);
        printf("%s: operations of the climate: %s\n", qPrintable(dimName), qPrintable(parts.join(", ")));
    }
    checkCellPurity(w, dim, 12345, qPrintable(dimName), false);
    {
        // how much of the world depends on the history of the search
        dp::setCursorPolicy(dp::CURSOR_FRESH);
        dp::Sampler fresh(w, dim, 12345);
        dp::setCursorPolicy(dp::CURSOR_CHUNK);
        dp::Sampler chunk(w, dim, 12345);
        const int n = 256;
        std::vector<int> a((size_t) n * n), b((size_t) n * n);
        int64_t diff = 0, cells = 0;
        std::map<int, int64_t> hist;
        for (int y : { -32, 40, 64, 100, 200 })
        {
            fresh.genBiomes(a.data(), 4, -128, -128, n, n, y);
            chunk.genBiomes(b.data(), 4, -128, -128, n, n, y);
            for (size_t i = 0; i < a.size(); i++)
            {
                cells++;
                diff += a[i] != b[i];
                hist[b[i]]++;
            }
        }
        printf("%s: %lld of %lld cells (%.3f %%) get another biome in chunk order than without history; "
            "%zu different biomes in the sample\n", qPrintable(dimName), (long long) diff, (long long) cells,
            100.0 * diff / cells, hist.size());
    }

    Counter total[6];
    for (int i = 2; i < args.size(); i++)
    {
        // the seed is in the header
        QFile f(args[i]);
        if (!f.open(QIODevice::ReadOnly))
        {
            printf("cannot read %s\n", qPrintable(args[i]));
            g_fail++;
            continue;
        }
        bool haveSeed = false;
        qlonglong seed = 0;
        while (!f.atEnd())
        {
            QByteArray line = f.readLine().trimmed();
            if (line.startsWith("# seed "))
            {
                seed = line.mid(7).trimmed().toLongLong(&haveSeed);
                break;
            }
            if (!line.startsWith("#"))
                break;
        }
        f.close();
        ServerFile sf;
        if (!haveSeed || !readServerFile(args[i], &sf))
        {
            printf("%s: no seed or no biome cells\n", qPrintable(args[i]));
            g_fail++;
            continue;
        }
        // which biomes the file has that the world does not know
        QStringList unknown;
        for (const ServerFile::Cell& c : sf.cells)
        {
            bool known = false;
            for (const dp::BiomeInfo& b : w->biomes())
                if (b.id == c.biome)
                    known = true;
            if (!known && !unknown.contains(c.biome))
                unknown << c.biome;
        }
        CHECK(g_chainOnly || unknown.isEmpty(), "the server has biomes the world does not list: %s",
            qPrintable(unknown.join(", ")));
        QByteArray label = QFileInfo(args[i]).fileName().toUtf8();
        compareServer(*w, dim, mc, (uint64_t) seed, sf, nullptr, label.constData(), total);
    }
    if (g_chainOnly && total[4].n)
        printf("all files: genBiomes in chunk order against whole chunks in the game's order: %lld of %lld cells "
            "differ (%.4f %%); %lld cells depend on the history\n", (long long) total[4].diff, (long long) total[4].n,
            total[4].rate(), (long long) total[5].diff);
    if (total[0].n)
    {
        printf("all files: %lld cells; differing: chunk order (default) %lld (%.4f %%), one cursor per call "
            "%lld (%.4f %%), no history %lld (%.4f %%)\n", (long long) total[0].n,
            (long long) total[0].diff, total[0].rate(), (long long) total[1].diff, total[1].rate(),
            (long long) total[2].diff, total[2].rate());
        printf("all files: genBiomes in chunk order against whole chunks in the game's order: %lld of %lld cells "
            "differ; %lld cells depend on the history\n", (long long) total[4].diff, (long long) total[4].n,
            (long long) total[5].diff);
    }
    return g_fail != 0;
}

//==============================================================================
// cell: the climate and biome of one cell, next to the engine's
//==============================================================================

static int cmdCell(const QStringList& args)
{
    if (args.size() < 6)
    {
        printf("usage: dptest cell <vanilla data> <mc> <seed> <x> <y> <z> [pack ...]\n"
            "       x, z in units of 4 blocks, y in blocks; preset minecraft:normal\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    const uint64_t seed = (uint64_t) args[2].toLongLong();
    const int x = args[3].toInt(), y = args[4].toInt(), z = args[5].toInt();
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], args.mid(6), "minecraft:normal", &report);
    int dim = w->dimension("minecraft:overworld");
    if (dim < 0 || !w->dimensions()[dim].supported)
        return 1;
    const dp::DimData& d = w->d()->data[dim];
    static const char *names[6] = { "temperature", "humidity", "continentalness", "erosion", "depth", "weirdness" };
    if (d.source == dp::DimData::MULTI_NOISE || d.source == dp::DimData::NATIVE_OVERWORLD)
    {
        dp::DensityState st(*w->d()->graph, d.program, seed, mc, d.legacy);
        st.evalColumn(x * 4, z * 4);
        st.evalVertical((y >> 2) * 4);
        Generator g;
        setupGenerator(&g, mc, 0);
        applySeed(&g, DIM_OVERWORLD, seed);
        int64_t np[6];
        int id = sampleBiomeNoise(&g.bn, np, x, y >> 2, z, NULL, 0);
        for (int k = 0; k < 6; k++)
            printf("%-16s %.17g -> %lld   (vanilla engine: %lld)\n", names[k], st.value(d.outputs[k]),
                (long long) dp::quantizeClimate((float) st.value(d.outputs[k])), (long long) np[k]);
        printf("vanilla engine biome: %s\n", biome2str(mc, id));
    }
    static const struct { dp::CursorPolicy p; const char *name; } pol[] = {
        { dp::CURSOR_CHUNK, "chunk order (default)" }, { dp::CURSOR_FRESH, "no history" },
    };
    for (const auto& po : pol)
    {
        dp::setCursorPolicy(po.p);
        dp::Sampler s(w, dim, seed);
        int b = s.biomeAt(4, x, y, z);
        printf("%s: %s\n", po.name, b >= 0 ? qPrintable(w->biomes()[b].id) : "(none)");
    }
    dp::setCursorPolicy(dp::CURSOR_CHUNK);
    return 0;
}

//==============================================================================

//==============================================================================
// structures
//==============================================================================

// A structure start reference file (worldgen_reference.py --structures)
struct StructFile
{
    struct Start
    {
        QString id;
        int cx = 0, cz = 0;
        QString piece;
        int bb[6] = { 0, 0, 0, 0, 0, 0 };
        QMap<QString, QString> ex;
    };
    uint64_t seed = 0;
    QString levelType = "normal";
    QString datapack;
    QString dimension = "minecraft:overworld";
    std::vector<Start> starts;
    QSet<qint64> covered;
    int cx0 = INT_MAX, cz0 = INT_MAX, cx1 = INT_MIN, cz1 = INT_MIN;
};

static qint64 chunkKey(int cx, int cz)
{
    return ((qint64) cx << 32) ^ (quint32) cz;
}

static bool readStructFile(const QString& path, StructFile *sf)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    while (!f.atEnd())
    {
        QByteArray line = f.readLine().trimmed();
        QList<QByteArray> p = line.split(' ');
        if (line.startsWith("# seed ") && p.size() >= 3)
            sf->seed = (uint64_t) p[2].toLongLong();
        else if (line.startsWith("# level_type ") && p.size() >= 3)
            sf->levelType = QString::fromLatin1(p[2]);
        else if (line.startsWith("# datapack ") && p.size() >= 3)
            sf->datapack = QString::fromLatin1(p[2]);
        else if (line.startsWith("# dimension ") && p.size() >= 3)
            sf->dimension = QString::fromLatin1(p[2]);
        else if (line.startsWith("C ") && p.size() == 4)
        {
            int cz = p[1].toInt(), a = p[2].toInt(), b = p[3].toInt();
            for (int cx = a; cx <= b; cx++)
                sf->covered.insert(chunkKey(cx, cz));
            sf->cx0 = std::min(sf->cx0, a);
            sf->cx1 = std::max(sf->cx1, b);
            sf->cz0 = std::min(sf->cz0, cz);
            sf->cz1 = std::max(sf->cz1, cz);
        }
        else if (line.startsWith("S ") && p.size() >= 11)
        {
            StructFile::Start s;
            s.id = QString::fromLatin1(p[1]);
            s.cx = p[2].toInt();
            s.cz = p[3].toInt();
            s.piece = QString::fromLatin1(p[4]);
            for (int k = 0; k < 6; k++)
                s.bb[k] = p[5 + k].toInt();
            for (int k = 11; k < p.size(); k++)
            {
                int eq = p[k].indexOf('=');
                if (eq > 0)
                    s.ex.insert(QString::fromLatin1(p[k].left(eq)), QString::fromLatin1(p[k].mid(eq + 1)));
            }
            sf->starts.push_back(s);
        }
    }
    return !sf->covered.isEmpty();
}

static const char *stateName(const dp::StructureStart& s)
{
    static const char *const st[] = { "valid", "invalid", "unknown" };
    static const char *const de[] = { "checked", "needs height", "terrain check", "earlier unknown", "not supported",
        "tied biome" };
    static char buf[64];
    snprintf(buf, sizeof buf, "%s (%s)", st[s.state], de[s.detail]);
    return buf;
}

// engine structure type of a vanilla structure set, or -1
static int engineStructType(const QString& set)
{
    static const struct { const char *id; int type; } map[] = {
        { "minecraft:villages", Village }, { "minecraft:desert_pyramids", Desert_Pyramid },
        { "minecraft:jungle_temples", Jungle_Pyramid }, { "minecraft:swamp_huts", Swamp_Hut },
        { "minecraft:igloos", Igloo }, { "minecraft:ocean_ruins", Ocean_Ruin },
        { "minecraft:shipwrecks", Shipwreck }, { "minecraft:ocean_monuments", Monument },
        { "minecraft:woodland_mansions", Mansion }, { "minecraft:pillager_outposts", Outpost },
        { "minecraft:ruined_portals", Ruined_Portal }, { "minecraft:ancient_cities", Ancient_City },
        { "minecraft:buried_treasures", Treasure }, { "minecraft:mineshafts", Mineshaft },
        { "minecraft:trail_ruins", Trail_Ruins }, { "minecraft:trial_chambers", Trial_Chambers },
    };
    for (const auto& m : map)
        if (set == QLatin1String(m.id))
            return m.type;
    return -1;
}

struct StructCount
{
    int server = 0, matchedValid = 0, matchedUnknown = 0, candValid = 0, candUnknown = 0, candInvalid = 0;
    int falseValid = 0, unknownWithout = 0, posChecked = 0, yChecked = 0;
    int unknownBy[6] = { 0, 0, 0, 0, 0, 0 };   // unknown candidates by StructureStart::Detail
    int hChecked = 0, hExact = 0;               // start heights from the terrain against the server's
};

// The placement of the vanilla structure sets against the engine
// (getStructurePos, getMineshafts, the stronghold iterator).
static void compareEngine(std::shared_ptr<const dp::World> w, int dim, int mc, uint64_t seed,
    int cx0, int cz0, int cx1, int cz1, const std::vector<dp::StructureStart>& cands)
{
    const std::vector<dp::StructureSetInfo>& sets = w->structureSets();
    for (size_t si = 0; si < sets.size(); si++)
    {
        int type = engineStructType(sets[si].id);
        if (type < 0)
            continue;
        std::set<std::pair<int, int>> ours, engine;
        for (const dp::StructureStart& s : cands)
            if (s.set == (int) si)
                ours.insert({ s.chunkX, s.chunkZ });
        if (type == Mineshaft)
        {
            std::vector<Pos> out((size_t) (cx1 - cx0 + 1) * (cz1 - cz0 + 1) / 16 + 64);
            // (the engine takes the last chunk, not the size)
            int n = getMineshafts(mc, seed, cx0, cz0, cx1, cz1, out.data(), (int) out.size());
            for (int i = 0; i < n && i < (int) out.size(); i++)
                engine.insert({ out[i].x >> 4, out[i].z >> 4 });
        }
        else
        {
            StructureConfig sc;
            if (!getStructureConfig(type, mc, &sc))
                continue;
            const int rs = sc.regionSize;
            for (int rz = (int) floor((double) cz0 / rs); rz <= (int) floor((double) cz1 / rs); rz++)
                for (int rx = (int) floor((double) cx0 / rs); rx <= (int) floor((double) cx1 / rs); rx++)
                {
                    Pos p;
                    if (!getStructurePos(type, mc, seed, rx, rz, &p))
                        continue;
                    int x = p.x >> 4, z = p.z >> 4;
                    if (x >= cx0 && x <= cx1 && z >= cz0 && z <= cz1)
                        engine.insert({ x, z });
                }
        }
        int onlyOurs = 0, onlyEngine = 0;
        for (const auto& c : ours)
            onlyOurs += !engine.count(c);
        for (const auto& c : engine)
            onlyEngine += !ours.count(c);
        // the engine does not apply exclusion zones
        const bool exclusion = sets[si].id == QLatin1String("minecraft:pillager_outposts");
        printf("  engine %-30s ours %5zu engine %5zu only ours %d only engine %d%s\n", qPrintable(sets[si].id),
            ours.size(), engine.size(), onlyOurs, onlyEngine, exclusion ? " (engine: no exclusion zone)" : "");
        CHECK(onlyOurs == 0, "%s: %d placement chunks the engine does not have", qPrintable(sets[si].id), onlyOurs);
        CHECK(exclusion || onlyEngine == 0, "%s: %d engine placement chunks missing", qPrintable(sets[si].id),
            onlyEngine);
    }
    // strongholds: every ring position
    for (size_t si = 0; si < sets.size(); si++)
    {
        if (sets[si].id != QLatin1String("minecraft:strongholds"))
            continue;
        dp::StructureFinder f(w, dim, seed);
        std::vector<std::pair<int, int>> ours = dp::ringPositions(f, (int) si);
        Generator g;
        setupGenerator(&g, mc, 0);
        applySeed(&g, DIM_OVERWORLD, seed);
        StrongholdIter sh;
        initFirstStronghold(&sh, mc, seed);
        std::vector<std::pair<int, int>> engine;
        int more;
        do
        {
            more = nextStronghold(&sh, &g);
            engine.push_back({ sh.pos.x >> 4, sh.pos.z >> 4 });
        }
        while (more > 0);
        // the iterator's last step yields one position more than the 128
        if (engine.size() > ours.size())
            engine.resize(ours.size());
        int diff = 0;
        for (size_t i = 0; i < ours.size() && i < engine.size(); i++)
            if (ours[i] != engine[i])
            {
                if (diff < 5)
                    printf("  stronghold %zu: ours %d %d, engine %d %d\n", i, ours[i].first, ours[i].second,
                        engine[i].first, engine[i].second);
                diff++;
            }
        printf("  engine minecraft:strongholds: %zu ring positions, engine %zu, %d differ\n", ours.size(),
            engine.size(), diff);
        CHECK(ours.size() == engine.size() && diff == 0, "stronghold positions differ from the engine");
    }
}

// Stronghold start chunks of real servers (tools/portal-reference data,
// "S <mc> <seed> <start_cx> <start_cz> <n>") against the ring positions.
static int cmdStrongholds(const QStringList& args, const QStringList& packs, const QStringList& files, bool engine)
{
    const int mc = str2mc(qPrintable(args[1]));
    const QString mcName = args[1];
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, "minecraft:normal", &report);
    CHECK(report.ok(), "the world has errors");
    const int dim = w->dimension("minecraft:overworld");
    int set = -1;
    for (size_t i = 0; i < w->structureSets().size(); i++)
        if (w->structureSets()[i].id == QLatin1String("minecraft:strongholds"))
            set = (int) i;
    CHECK(set >= 0, "no stronghold set");
    std::map<uint64_t, std::vector<std::pair<int, int>>> bySeed;
    for (const QString& path : files)
    {
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly), "cannot read %s", qPrintable(path));
        while (!f.atEnd())
        {
            QList<QByteArray> p = f.readLine().trimmed().split(' ');
            if (p.size() >= 5 && p[0] == "S" && QString::fromLatin1(p[1]) == mcName)
                bySeed[(uint64_t) p[2].toLongLong()].push_back({ p[3].toInt(), p[4].toInt() });
        }
    }
    int n = 0, found = 0, engineFound = 0;
    for (const auto& kv : bySeed)
    {
        dp::StructureFinder f(w, dim, kv.first);
        std::vector<std::pair<int, int>> ring = dp::ringPositions(f, set);
        std::set<std::pair<int, int>> ours(ring.begin(), ring.end()), eng;
        if (engine)
        {
            Generator g;
            setupGenerator(&g, mc, 0);
            applySeed(&g, DIM_OVERWORLD, kv.first);
            StrongholdIter sh;
            initFirstStronghold(&sh, mc, kv.first);
            int more;
            do
            {
                more = nextStronghold(&sh, &g);
                eng.insert({ sh.pos.x >> 4, sh.pos.z >> 4 });
            }
            while (more > 0);
        }
        for (const auto& c : kv.second)
        {
            n++;
            bool ok = ours.count(c) > 0;
            found += ok;
            engineFound += eng.count(c) > 0;
            CHECK(ok, "seed %lld: the server's stronghold at chunk %d %d is no ring position",
                (long long) kv.first, c.first, c.second);
        }
    }
    printf("strongholds of real servers: %d, ring positions %d%s\n", n, found,
        engine ? qPrintable(QString(", engine %1").arg(engineFound)) : "");
    return g_fail ? 1 : 0;
}

// dptest structures --map
static bool g_structMap = false;

static int cmdStructures(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QStringList packs, strongholdFiles;
    bool engine = false;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--map") { g_structMap = true; args.remove(i); }
        else if (args[i] == "--strongholds" && i + 1 < args.size()) { strongholdFiles << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--engine") { engine = true; args.remove(i); }
        else i++;
    }
    if (args.size() == 2 && !strongholdFiles.isEmpty())
        return cmdStrongholds(args, packs, strongholdFiles, engine);
    if (args.size() < 3)
    {
        printf("usage: dptest structures <vanilla data> <mc> [--pack <pack> ...] [--engine] [--map] <structure file> ...\n"
            "       (--map: setHeightsOfInvalid(false), as the map)\n"
            "       structure files: tools/worldgen-reference/data/structures_*.txt (worldgen_reference.py\n"
            "       --structures); every start of the server must be a candidate of the same structure\n"
            "       that is valid or unknown; --engine: also the placement against the native engine\n"
            "       dptest structures <vanilla data> <mc> --strongholds <tools/portal-reference/data/pieces_*.txt>\n"
            "       [--engine]: the stronghold start chunks of real servers must be ring positions\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    std::map<QString, std::shared_ptr<const dp::World>> worlds;
    std::map<QString, StructCount> total;
    for (int fi = 2; fi < args.size(); fi++)
    {
        StructFile sf;
        if (!readStructFile(args[fi], &sf))
        {
            CHECK(false, "cannot read %s", qPrintable(args[fi]));
            continue;
        }
        QString preset = "minecraft:" + sf.levelType;
        if (!worlds.count(preset))
        {
            dp::Report report;
            QElapsedTimer t;
            t.start();
            worlds[preset] = loadWorld(mc, args[0], packs, preset, &report, false);
            int warnings = 0;
            for (const dp::Issue& i : report.issues)
                warnings += i.severity == dp::Issue::Warning;
            printf("%s: loaded in %.1f ms, %zu issues (%d warnings), %zu structure sets, %zu structures\n",
                qPrintable(preset), t.nsecsElapsed() / 1e6, report.issues.size(), warnings,
                worlds[preset]->structureSets().size(), worlds[preset]->structures().size());
            printReport(report);
            CHECK(report.ok(), "the world has errors");
            for (const dp::StructureSetInfo& s : worlds[preset]->structureSets())
                if (!s.supported)
                    printf("  set %s not supported: %s\n", qPrintable(s.id), qPrintable(s.reason));
            for (const dp::StructureInfo& s : worlds[preset]->structures())
                if (!s.supported)
                    printf("  structure %s not supported: %s\n", qPrintable(s.id), qPrintable(s.reason));
        }
        std::shared_ptr<const dp::World> w = worlds[preset];
        const int dim = w->dimension(sf.dimension);
        CHECK(dim >= 0 && w->dimensions()[(size_t) dim].supported, "%s: dimension %s not supported",
            qPrintable(args[fi]), qPrintable(sf.dimension));
        if (dim < 0)
            continue;
        const std::vector<dp::StructureInfo>& infos = w->structures();
        const dp::StructureData& sd = w->d()->structures;
        QHash<QString, int> index;
        for (size_t i = 0; i < infos.size(); i++)
            index.insert(infos[i].id, (int) i);

        dp::StructureFinder finder(w, dim, sf.seed);
        CHECK(finder.valid(), "no finder");
        finder.setHeightsOfInvalid(!g_structMap);
        std::vector<dp::StructureStart> cands;
        QElapsedTimer t;
        t.start();
        finder.starts(sf.cx0, sf.cz0, sf.cx1, sf.cz1, &cands);
        printf("%s: seed %lld, %d covered chunks, %zu server starts, %zu candidates in %.0f ms\n",
            qPrintable(QFileInfo(args[fi]).fileName()), (long long) sf.seed, (int) sf.covered.size(),
            sf.starts.size(), cands.size(), t.nsecsElapsed() / 1e6);

        QHash<QString, const dp::StructureStart *> byKey;     // structure, chunk
        for (const dp::StructureStart& s : cands)
            byKey.insert(QString("%1 %2 %3").arg(infos[(size_t) s.structure].id).arg(s.chunkX).arg(s.chunkZ), &s);
        QSet<QString> serverKeys;
        int shown = 0;
        for (const StructFile::Start& s : sf.starts)
        {
            QString key = QString("%1 %2 %3").arg(s.id).arg(s.cx).arg(s.cz);
            serverKeys.insert(key);
            StructCount& c = total[s.id];
            c.server++;
            if (!index.contains(s.id))
            {
                CHECK(false, "%s: unknown structure", qPrintable(key));
                continue;
            }
            const dp::StructureStart *cand = byKey.value(key);
            if (!cand)
            {
                CHECK(false, "%s: the server has a start, the map no candidate", qPrintable(key));
                continue;
            }
            if (cand->state == dp::StructureStart::Invalid)
            {
                if (shown++ < 20)
                    printf("  server start called invalid: %s at %d %d %d\n", qPrintable(key), cand->x, cand->y,
                        cand->z);
                CHECK(false, "%s: the server has a start, the map calls it invalid", qPrintable(key));
                continue;
            }
            if (cand->state == dp::StructureStart::Valid)
                c.matchedValid++;
            else
                c.matchedUnknown++;
            // jigsaw: the start position is the centre of the start piece
            const dp::StructureDef& def = sd.defs[(size_t) index.value(s.id)];
            if ((def.kind == dp::StructureDef::NETHER_FOSSIL || def.kind == dp::StructureDef::END_CITY) &&
                s.ex.contains("tp"))
            {
                // the (first) template is placed at the start position
                QStringList tp = s.ex.value("tp").split(',');
                const int tx = tp.value(0).toInt(), ty = tp.value(1).toInt(), tz = tp.value(2).toInt();
                c.posChecked++;
                CHECK(tx == cand->x && tz == cand->z, "%s: start position %d %d, the map's %d %d",
                    qPrintable(key), tx, tz, cand->x, cand->z);
                c.hChecked++;
                c.hExact += cand->hasY && cand->y == ty;
                CHECK(!cand->hasY || cand->y == ty, "%s: start height %d, the map's %d", qPrintable(key), ty,
                    cand->y);
            }
            if (def.kind == dp::StructureDef::JIGSAW && s.ex.contains("element"))
            {
                int x = (int) (((int64_t) s.bb[0] + s.bb[3]) / 2), z = (int) (((int64_t) s.bb[2] + s.bb[5]) / 2);
                c.posChecked++;
                CHECK(x == cand->x && z == cand->z, "%s: start position %d %d, the map's %d %d", qPrintable(key),
                    x, z, cand->x, cand->z);
                if (!def.projected && def.startJigsawName.isEmpty() && cand->hasY)
                {
                    // the start piece was moved so that its ground is at the start height
                    int y = s.bb[1] + s.ex.value("gld").toInt();
                    c.yChecked++;
                    CHECK(y == cand->y, "%s: start height %d, the map's %d", qPrintable(key), y, cand->y);
                }
            }
        }
        // what the map lists in the covered chunks
        int shownFalse = 0;
        for (const dp::StructureStart& s : cands)
        {
            if (!sf.covered.contains(chunkKey(s.chunkX, s.chunkZ)))
                continue;
            const QString& id = infos[(size_t) s.structure].id;
            StructCount& c = total[id];
            QString key = QString("%1 %2 %3").arg(id).arg(s.chunkX).arg(s.chunkZ);
            const bool has = serverKeys.contains(key);
            if (s.state == dp::StructureStart::Valid)
            {
                c.candValid++;
                if (!has)
                {
                    c.falseValid++;
                    if (shownFalse++ < 20)
                        printf("  valid without a server start: %s at %d %d %d (%s)\n", qPrintable(key), s.x,
                            s.hasY ? s.y : 0, s.z, stateName(s));
                }
            }
            else if (s.state == dp::StructureStart::Unknown)
            {
                c.candUnknown++;
                c.unknownWithout += !has;
                c.unknownBy[s.detail]++;
            }
            else
                c.candInvalid++;
        }
        // The terrain height path: projected jigsaw starts without a start
        // jigsaw name (villages, outposts) give the server's base height at
        // their start column (y0 + gld = start height + base height). With
        // those heights the map must call them valid at that height.
        struct ServerHeights : dp::TerrainHeights
        {
            QHash<qint64, int> h;   // (x, z) -> base height, WORLD_SURFACE_WG
            int baseHeight(int x, int z, dp::Heightmap type) override
            {
                if (type != dp::HM_WORLD_SURFACE_WG)
                    return INT_MIN;
                return h.value(chunkKey(x, z), INT_MIN);
            }
        } heights;
        std::vector<const StructFile::Start *> projected;
        for (const StructFile::Start& s : sf.starts)
        {
            if (!index.contains(s.id) || !s.ex.contains("element"))
                continue;
            const dp::StructureDef& def = sd.defs[(size_t) index.value(s.id)];
            if (def.kind != dp::StructureDef::JIGSAW || !def.projected || !def.startJigsawName.isEmpty() ||
                def.heightmap != dp::HM_WORLD_SURFACE_WG || def.startHeight.type != dp::HeightProv::CONSTANT)
                continue;
            int x = (int) (((int64_t) s.bb[0] + s.bb[3]) / 2), z = (int) (((int64_t) s.bb[2] + s.bb[5]) / 2);
            heights.h.insert(chunkKey(x, z), s.bb[1] + s.ex.value("gld").toInt() - def.startHeight.a.value);
            projected.push_back(&s);
        }
        if (!projected.empty())
        {
            dp::StructureFinder hf(w, dim, sf.seed);
            dp::setTerrainHeights(hf, &heights);
            std::vector<dp::StructureStart> hc;
            hf.starts(sf.cx0, sf.cz0, sf.cx1, sf.cz1, &hc);
            QHash<QString, const dp::StructureStart *> hk;
            for (const dp::StructureStart& s : hc)
                hk.insert(QString("%1 %2 %3").arg(infos[(size_t) s.structure].id).arg(s.chunkX).arg(s.chunkZ), &s);
            int ok = 0;
            for (const StructFile::Start *s : projected)
            {
                QString key = QString("%1 %2 %3").arg(s->id).arg(s->cx).arg(s->cz);
                const dp::StructureStart *c = hk.value(key);
                const int y = s->bb[1] + s->ex.value("gld").toInt();
                // an earlier structure of the set may still be unknown, and a
                // biome check of tied entries cannot be predicted
                bool good = c && c->hasY && c->y == y && (c->state == dp::StructureStart::Valid ||
                    (c->state == dp::StructureStart::Unknown && (c->detail == dp::StructureStart::EarlierUnknown
                        || c->detail == dp::StructureStart::TiedBiome)));
                ok += good;
                CHECK(good, "%s with the server's height: %s at y %d, expected valid at y %d", qPrintable(key),
                    c ? stateName(*c) : "no candidate", c ? c->y : 0, y);
            }
            printf("  projected jigsaw starts with the server's heights: %d of %zu valid at the server's height"
                " (or undecided)\n", ok, projected.size());
            // the same starts with the terrain of the finder: its start
            // height (exact or estimated) against the server's
            int shownH = 0;
            // The server's getFirstOccupiedHeight() at the start column
            // against the terrain fill: equal without aquifers; with them
            // never below the highest block of positive density (the
            // finder's lower bound).
            dp::Sampler hs(w, dim, sf.seed);
            const bool aquifers = w->d()->data[(size_t) dim].terrain.aquifers;
            int bounds = 0;
            for (const StructFile::Start *s : projected)
            {
                QString key = QString("%1 %2 %3").arg(s->id).arg(s->cx).arg(s->cz);
                const int y = s->bb[1] + s->ex.value("gld").toInt();
                if (hs.hasTerrain())
                {
                    const dp::StructureDef& def = sd.defs[(size_t) index.value(s->id)];
                    const int x = (int) (((int64_t) s->bb[0] + s->bb[3]) / 2);
                    const int z = (int) (((int64_t) s->bb[2] + s->bb[5]) / 2);
                    const int occ = y - def.startHeight.a.value - 1;
                    const int none = w->dimensions()[(size_t) dim].minY - 1;
                    int floor = hs.surfaceHeight(x, z, dp::HEIGHT_OCEAN_FLOOR);
                    int surf = hs.surfaceHeight(x, z, dp::HEIGHT_SURFACE);
                    floor = floor == dp::NO_HEIGHT ? none : floor;
                    surf = surf == dp::NO_HEIGHT ? none : surf;
                    const bool good = aquifers ? occ >= floor : occ == surf;
                    bounds += good;
                    CHECK(good, "%s: the server's occupied height %d, the terrain fill's %d (highest solid %d)%s",
                        qPrintable(key), occ, surf, floor, aquifers ? ", below the lower bound" : "");
                    if (occ != surf)
                        printf("  occupied height of %s at %d %d: server %d, fill %d, highest solid %d (%s)\n",
                            qPrintable(key), x, z, occ, surf, floor, good ? "within the bounds" : "OUTSIDE");
                }
                const dp::StructureStart *c = byKey.value(key);
                if (!c || c->state == dp::StructureStart::Invalid)
                    continue;
                StructCount& sc = total[s->id];
                sc.hChecked++;
                sc.hExact += c->y == y;
                if (c->y != y && shownH++ < 20)
                    printf("  start height of %s: server %d, terrain %d (%s, %s)\n", qPrintable(key), y, c->y,
                        c->hasY ? "exact" : "estimate", stateName(*c));
            }
            if (hs.hasTerrain())
                printf("  projected jigsaw starts: the server's height %s the terrain's in %d of %zu\n",
                    aquifers ? "within the bounds of" : "equal to", bounds, projected.size());
        }
        if (engine)
            compareEngine(w, dim, mc, sf.seed, sf.cx0, sf.cz0, sf.cx1, sf.cz1, cands);
    }
    printf("\n%-34s %6s %6s %6s | %6s %6s %6s | %6s %6s | %5s %5s\n", "structure", "server", "valid", "unkn",
        "c.val", "c.unk", "c.inv", "falseV", "unk-no", "pos", "y");
    StructCount sum;
    for (const auto& kv : total)
    {
        const StructCount& c = kv.second;
        printf("%-34s %6d %6d %6d | %6d %6d %6d | %6d %6d | %5d %5d\n", qPrintable(kv.first), c.server,
            c.matchedValid, c.matchedUnknown, c.candValid, c.candUnknown, c.candInvalid, c.falseValid,
            c.unknownWithout, c.posChecked, c.yChecked);
        sum.server += c.server; sum.matchedValid += c.matchedValid; sum.matchedUnknown += c.matchedUnknown;
        sum.candValid += c.candValid; sum.candUnknown += c.candUnknown; sum.candInvalid += c.candInvalid;
        sum.falseValid += c.falseValid; sum.unknownWithout += c.unknownWithout;
        sum.posChecked += c.posChecked; sum.yChecked += c.yChecked;
    }
    printf("%-34s %6d %6d %6d | %6d %6d %6d | %6d %6d | %5d %5d\n", "all", sum.server, sum.matchedValid,
        sum.matchedUnknown, sum.candValid, sum.candUnknown, sum.candInvalid, sum.falseValid, sum.unknownWithout,
        sum.posChecked, sum.yChecked);
    printf("(server: starts in the files; valid/unkn: of those, the map's state; c.*: the map's candidates in\n"
        " covered chunks; falseV: valid candidates without a server start; unk-no: unknown candidates\n"
        " without a server start; pos/y: jigsaw start positions compared)\n");
    printf("\nunknown candidates by detail, and start heights from the terrain against the server's\n");
    printf("%-34s %6s %6s %6s %6s %6s %6s | %6s %6s\n", "structure", "c.unk", "height", "terr", "earlier", "unsup",
        "tied", "h.cmp", "h.same");
    StructCount u;
    for (const auto& kv : total)
    {
        const StructCount& c = kv.second;
        printf("%-34s %6d %6d %6d %6d %6d %6d | %6d %6d\n", qPrintable(kv.first), c.candUnknown, c.unknownBy[1],
            c.unknownBy[2], c.unknownBy[3], c.unknownBy[4], c.unknownBy[5], c.hChecked, c.hExact);
        for (int k = 0; k < 6; k++)
            u.unknownBy[k] += c.unknownBy[k];
        u.candUnknown += c.candUnknown;
        u.hChecked += c.hChecked;
        u.hExact += c.hExact;
    }
    printf("%-34s %6d %6d %6d %6d %6d %6d | %6d %6d\n", "all", u.candUnknown, u.unknownBy[1], u.unknownBy[2],
        u.unknownBy[3], u.unknownBy[4], u.unknownBy[5], u.hChecked, u.hExact);
    printf("(height: needs height, terr: terrain check left, earlier: an earlier structure of the set unknown,\n"
        " unsup: not supported, tied: tied biome; h.cmp/h.same: starts whose height the server's pieces give\n"
        " (projected jigsaws, nether fossils, end cities), and how many of them the terrain gives the same)\n");
    return g_fail ? 1 : 0;
}

// Broken and unknown structure data in a pack: warnings that name the pack
// and the resource, never an error (the biome map stays), and a finder that
// still works.
static int cmdStructRobust(const QStringList& args)
{
    if (args.size() < 3)
    {
        printf("usage: dptest structrobust <vanilla data> <mc> <work dir>\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    QString root = QDir(args[2]).filePath("structrobust");
    QDir(root).removeRecursively();
    auto put = [&](const QString& rel, const QByteArray& data) {
        QString path = QDir(root).filePath(rel);
        QDir().mkpath(QFileInfo(path).path());
        CHECK(writeFile(path, data), "cannot write %s", qPrintable(path));
    };
    put("pack.mcmeta", "{\"pack\": {\"pack_format\": 48, \"description\": \"structure robustness\"}}");
    const QByteArray spread = "{\"type\": \"minecraft:random_spread\", \"salt\": 1, \"spacing\": 8, \"separation\": 2}";
    const QByteArray jig = "{\"type\": \"minecraft:jigsaw\", \"biomes\": \"#minecraft:is_overworld\", "
        "\"step\": \"surface_structures\", \"spawn_overrides\": {}, \"size\": 1, \"start_height\": {\"absolute\": 0}, "
        "\"max_distance_from_center\": 32, \"start_pool\": \"%1\"}";
    // a mod's placement type and a mod's structure type
    put("data/r/worldgen/structure_set/modplace.json", "{\"placement\": {\"type\": \"somemod:grid\", \"salt\": 1}, "
        "\"structures\": [{\"structure\": \"r:ok\", \"weight\": 1}]}");
    put("data/r/worldgen/structure/modtype.json", "{\"type\": \"somemod:castle\", \"biomes\": \"#minecraft:is_overworld\", "
        "\"step\": \"surface_structures\", \"spawn_overrides\": {}}");
    put("data/r/worldgen/structure_set/modtypes.json", "{\"placement\": " + spread + ", "
        "\"structures\": [{\"structure\": \"r:modtype\", \"weight\": 1}, {\"structure\": \"r:ok\", \"weight\": 1}]}");
    // a working jigsaw structure
    put("data/r/worldgen/structure/ok.json", QByteArray(jig).replace("%1", "minecraft:village/plains/town_centers"));
    put("data/r/worldgen/structure_set/okset.json", "{\"placement\": " + spread + ", "
        "\"structures\": [{\"structure\": \"r:ok\", \"weight\": 1}]}");
    // a set naming a missing structure, one with a missing exclusion set, two that exclude each other
    put("data/r/worldgen/structure_set/missing.json", "{\"placement\": " + spread + ", "
        "\"structures\": [{\"structure\": \"r:nothere\", \"weight\": 1}]}");
    put("data/r/worldgen/structure_set/noexcl.json", "{\"placement\": {\"type\": \"minecraft:random_spread\", "
        "\"salt\": 2, \"spacing\": 8, \"separation\": 2, \"exclusion_zone\": {\"other_set\": \"r:gone\", "
        "\"chunk_count\": 2}}, \"structures\": [{\"structure\": \"r:ok\", \"weight\": 1}]}");
    put("data/r/worldgen/structure_set/cycle_a.json", "{\"placement\": {\"type\": \"minecraft:random_spread\", "
        "\"salt\": 3, \"spacing\": 8, \"separation\": 2, \"exclusion_zone\": {\"other_set\": \"r:cycle_b\", "
        "\"chunk_count\": 2}}, \"structures\": [{\"structure\": \"r:ok\", \"weight\": 1}]}");
    put("data/r/worldgen/structure_set/cycle_b.json", "{\"placement\": {\"type\": \"minecraft:random_spread\", "
        "\"salt\": 4, \"spacing\": 8, \"separation\": 2, \"exclusion_zone\": {\"other_set\": \"r:cycle_a\", "
        "\"chunk_count\": 2}}, \"structures\": [{\"structure\": \"r:ok\", \"weight\": 1}]}");
    // spacing not above separation; broken JSON
    put("data/r/worldgen/structure_set/badspacing.json", "{\"placement\": {\"type\": \"minecraft:random_spread\", "
        "\"salt\": 5, \"spacing\": 4, \"separation\": 4}, \"structures\": [{\"structure\": \"r:ok\", \"weight\": 1}]}");
    put("data/r/worldgen/structure_set/broken.json", "{\"placement\": ");
    // a jigsaw with a missing pool, one with a damaged template
    put("data/r/worldgen/structure/nopool.json", QByteArray(jig).replace("%1", "r:none"));
    put("data/r/worldgen/structure/badtemplate.json", QByteArray(jig).replace("%1", "r:bad"));
    put("data/r/worldgen/template_pool/bad.json", "{\"fallback\": \"minecraft:empty\", \"elements\": [{\"weight\": 1, "
        "\"element\": {\"element_type\": \"minecraft:single_pool_element\", \"location\": \"r:bad\", "
        "\"projection\": \"rigid\", \"processors\": \"minecraft:empty\"}}]}");
    put("data/r/structure/bad.nbt", QByteArray("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03garbage-garbage-garbage", 33));

    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], QStringList() << root, "minecraft:normal", &report);
    CHECK(report.ok(), "structure problems must not be errors");
    auto has = [&](const char *id, const char *text) {
        bool f = hasIssue(report, dp::Issue::Warning, "structrobust", id, text);
        CHECK(f, "no warning for %s containing \"%s\"", id, text);
    };
    has("r:modplace", "placement type");
    has("r:modtype", "structure type");
    has("r:missing", "does not exist");
    has("r:noexcl", "does not exist");
    has("r:cycle_a", "cycle");
    has("r:badspacing", "spacing");
    has("r:broken", "JSON");
    has("r:nopool", "start pool");
    has("r:bad", "cannot be read");
    std::map<QString, const dp::StructureSetInfo *> sets;
    for (const dp::StructureSetInfo& s : w->structureSets())
        sets[s.id] = &s;
    for (const char *id : { "r:modplace", "r:missing", "r:noexcl", "r:cycle_a", "r:cycle_b", "r:badspacing", "r:broken" })
        CHECK(sets.count(id) && !sets[id]->supported && !sets[id]->reason.isEmpty(), "%s should be unsupported", id);
    CHECK(sets.count("r:okset") && sets["r:okset"]->supported, "r:okset should be supported");
    CHECK(sets.count("r:modtypes") && sets["r:modtypes"]->supported, "r:modtypes should be supported");
    for (const dp::StructureInfo& s : w->structures())
        if (s.id.startsWith("r:"))
            CHECK(s.supported == (s.id == QLatin1String("r:ok")), "%s: supported %d (%s)", qPrintable(s.id),
                (int) s.supported, qPrintable(s.reason));
    // the finder: where the game tries the unknown r:modtype of r:modtypes
    // first, r:ok after it is unknown, else it is decided
    dp::StructureFinder f(w, w->dimension("minecraft:overworld"), 1234);
    std::vector<dp::StructureStart> out;
    CHECK(f.starts(-40, -40, 40, 40, &out), "the finder failed");
    int okset = 0, first = 0, after = 0;
    for (const dp::StructureStart& s : out)
    {
        const QString& set = w->structureSets()[(size_t) s.set].id;
        okset += set == QLatin1String("r:okset");
        if (set == QLatin1String("r:modtypes"))
        {
            if (s.detail == dp::StructureStart::EarlierUnknown)
                after++;
            else
                first++;
            CHECK(s.state != dp::StructureStart::Valid || s.detail != dp::StructureStart::EarlierUnknown,
                "a start after an unknown structure type is valid");
        }
    }
    printf("structrobust: %zu issues, %zu candidates (r:okset %d, r:modtypes %d tried first, %d after r:modtype)\n",
        report.issues.size(), out.size(), okset, first, after);
    CHECK(okset > 0 && first > 0 && after > 0, "no candidates of each kind");
    return g_fail ? 1 : 0;
}

// The vanilla data of a version without server data: it loads without a
// warning, every set and structure is supported, the placement equals the
// engine's, and the candidates are a pure function of the position (the
// same for a whole area and for its parts in any order, fresh or reused
// finder, ring positions computed before or after other lookups).
static int cmdStructCheck(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QString dimId = "minecraft:overworld";
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--dimension" && i + 1 < args.size()) { dimId = args[i + 1]; args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 3)
    {
        printf("usage: dptest structcheck <vanilla data> <mc> <seed> [radius in chunks, default 150]\n"
            "       [--dimension <id>] (default the overworld)\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    const uint64_t seed = (uint64_t) args[2].toLongLong();
    const int r = args.size() > 3 ? args[3].toInt() : 150;
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], QStringList(), "minecraft:normal", &report);
    CHECK(report.ok() && report.issues.empty(), "the vanilla data gives %zu issues", report.issues.size());
    int unsupported = 0;
    for (const dp::StructureSetInfo& s : w->structureSets())
        if (!s.supported)
            unsupported++, printf("  set %s: %s\n", qPrintable(s.id), qPrintable(s.reason));
    for (const dp::StructureInfo& s : w->structures())
        if (!s.supported)
            unsupported++, printf("  structure %s: %s\n", qPrintable(s.id), qPrintable(s.reason));
    CHECK(unsupported == 0, "%d sets or structures not supported", unsupported);
    const int dim = w->dimension(dimId);
    CHECK(dim >= 0 && w->dimensions()[(size_t) dim].supported, "dimension %s not supported", qPrintable(dimId));
    if (dim < 0)
        return 1;

    auto key = [](const dp::StructureStart& s) {
        return std::make_tuple(s.set, s.structure, s.chunkZ, s.chunkX, s.x, s.y, (int) s.hasY * 2 + s.estimatedY, s.z,
            (int) s.state, (int) s.detail);
    };
    auto sorted = [&](std::vector<dp::StructureStart> v) {
        std::vector<decltype(key(v[0]))> k;
        for (const dp::StructureStart& s : v)
            k.push_back(key(s));
        std::sort(k.begin(), k.end());
        return k;
    };
    std::vector<dp::StructureStart> whole;
    {
        dp::StructureFinder f(w, dim, seed);
        f.starts(-r, -r, r, r, &whole);
    }
    std::vector<dp::StructureStart> parts, partsRev;
    {
        // reused: another area first, then the quarters
        dp::StructureFinder f(w, dim, seed);
        std::vector<dp::StructureStart> elsewhere;
        f.starts(r + 300, r + 300, r + 340, r + 340, &elsewhere);
        f.starts(-r, -r, -1, -1, &parts);
        f.starts(0, -r, r, -1, &parts);
        f.starts(-r, 0, -1, r, &parts);
        f.starts(0, 0, r, r, &parts);
    }
    {
        // the quarters in the other order, with ring positions first
        dp::StructureFinder f(w, dim, seed);
        for (size_t i = 0; i < w->structureSets().size(); i++)
            dp::ringPositions(f, (int) i);
        f.starts(0, 0, r, r, &partsRev);
        f.starts(-r, 0, -1, r, &partsRev);
        f.starts(0, -r, r, -1, &partsRev);
        f.starts(-r, -r, -1, -1, &partsRev);
    }
    const bool same1 = sorted(whole) == sorted(parts), same2 = sorted(whole) == sorted(partsRev);
    printf("%s: %zu candidates in %d x %d chunks; parts %s, reversed parts %s\n", qPrintable(args[1]), whole.size(),
        2 * r + 1, 2 * r + 1, same1 ? "same" : "DIFFER", same2 ? "same" : "DIFFER");
    CHECK(same1 && same2, "the candidates depend on the requested area or the order of requests");
    // a structure filter only leaves out the sets of the other structures
    int filterDiffers = 0;
    {
        dp::StructureFinder f(w, dim, seed);
        for (size_t i = 0; i < w->structures().size(); i++)
        {
            std::vector<char> mask(w->structures().size(), 0);
            mask[i] = 1;
            std::vector<dp::StructureStart> one, want;
            f.starts(-r, -r, r, r, &one, nullptr, &mask);
            one.erase(std::remove_if(one.begin(), one.end(), [i](const dp::StructureStart& s) {
                return s.structure != (int) i; }), one.end());
            for (const dp::StructureStart& s : whole)
                if (s.structure == (int) i)
                    want.push_back(s);
            filterDiffers += sorted(one) != sorted(want);
        }
    }
    printf("one structure at a time: %s\n", filterDiffers ? "DIFFER" : "same");
    CHECK(filterDiffers == 0, "%d structures differ with a structure filter", filterDiffers);
    // setHeightsOfInvalid(false) (the map): the same starts, only those that
    // are not generated may come without their height
    {
        dp::StructureFinder f(w, dim, seed);
        f.setHeightsOfInvalid(false);
        std::vector<dp::StructureStart> map;
        f.starts(-r, -r, r, r, &map);
        auto noY = [](std::vector<dp::StructureStart> v) {
            for (dp::StructureStart& s : v)
                if (s.state == dp::StructureStart::Invalid)
                {
                    s.y = 0;
                    s.hasY = s.estimatedY = false;
                }
            return v;
        };
        int64_t without = 0;
        for (const dp::StructureStart& s : map)
            without += s.state == dp::StructureStart::Invalid && !s.hasY && !s.estimatedY;
        const bool same = sorted(noY(map)) == sorted(noY(whole));
        printf("without the heights of starts that are not generated: %s (%lld of them without a height)\n",
            same ? "same" : "DIFFER", (long long) without);
        CHECK(same, "setHeightsOfInvalid(false) changes the starts");
    }
    if (dimId == "minecraft:overworld")
        compareEngine(w, dim, mc, seed, -r, -r, r, r, whole);
    return g_fail ? 1 : 0;
}

// Milliseconds per viewport query of the structure finder
static int cmdStructPerf(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QStringList packs;
    QString dimId = "minecraft:overworld";
    uint64_t seed = 12345;
    int only = 0;
    bool terrainNew = false;     // only the case with the terrain and a new finder per query
    bool mapMode = false;        // setHeightsOfInvalid(false), as the map
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--size" && i + 1 < args.size()) { only = args[i + 1].toInt(); args.remove(i, 2); }
        else if (args[i] == "--terrain-new") { terrainNew = true; args.removeAt(i); }
        else if (args[i] == "--map") { mapMode = true; args.removeAt(i); }
        else if (args[i] == "--dimension" && i + 1 < args.size()) { dimId = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--seed" && i + 1 < args.size()) { seed = (uint64_t) args[i + 1].toLongLong(); args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 2)
    {
        printf("usage: dptest structperf <vanilla data> <mc> [--pack <pack> ...] [--dimension <id>] [--seed <n>]\n"
            "       [--size <chunks>] (only that viewport size) [--terrain-new] (only with the terrain, a new finder)\n"
            "       [--map] (the finder as the map uses it)\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, "minecraft:normal", &report);
    CHECK(report.ok(), "the world has errors");
    const int dim = w->dimension(dimId);
    CHECK(dim >= 0 && w->dimensions()[(size_t) dim].supported, "dimension %s not supported", qPrintable(dimId));
    if (dim < 0)
        return 1;
    struct NoHeights : dp::TerrainHeights
    {
        int baseHeight(int, int, dp::Heightmap) override { return INT_MIN; }
    } none;
    printf("%s, %s, seed %lld, %zu structure sets\n", qPrintable(args[1]), qPrintable(dimId), (long long) seed,
        w->structureSets().size());
    printf("%9s %-10s %-8s %10s %10s %8s %8s %8s\n", "chunks", "heights", "finder", "ms/query", "max ms",
        "cands", "valid", "unknown");
    const int sizes[] = { 32, 128, 512 };
    for (int n : sizes)
    {
        if (only && n != only)
            continue;
        // areas spread over the world (at far zoom fewer of them)
        const int areas = n >= 512 ? 3 : n >= 128 ? 6 : 24;
        for (int mode = 0; mode < (terrainNew ? 1 : 2); mode++)
        {
            for (int reuse = 0; reuse < (terrainNew ? 1 : 2); reuse++)
            {
                std::unique_ptr<dp::StructureFinder> f;
                double total = 0, worst = 0;
                size_t cands = 0, valid = 0, unknown = 0;
                for (int a = 0; a < areas; a++)
                {
                    // fresh: areas far apart; panning: the next area to the east
                    int x0 = reuse ? -n * areas / 2 + a * n : (a * 7919 % 4001) - 2000 - n / 2;
                    int z0 = reuse ? -n / 2 : (a * 104729 % 4001) - 2000 - n / 2;
                    QElapsedTimer t;
                    t.start();
                    if (!reuse || !f)
                    {
                        f.reset(new dp::StructureFinder(w, dim, seed));
                        f->setHeightsOfInvalid(!mapMode);
                        if (mode == 1)
                            dp::setTerrainHeights(*f, &none);
                    }
                    std::vector<dp::StructureStart> out;
                    f->starts(x0, z0, x0 + n - 1, z0 + n - 1, &out);
                    const double ms = t.nsecsElapsed() / 1e6;
                    total += ms;
                    worst = std::max(worst, ms);
                    cands += out.size();
                    for (const dp::StructureStart& s : out)
                    {
                        valid += s.state == dp::StructureStart::Valid;
                        unknown += s.state == dp::StructureStart::Unknown;
                    }
                }
                printf("%4d x %-3d %-10s %-8s %10.2f %10.2f %8zu %8zu %8zu\n", n, n, mode ? "without" : "terrain",
                    reuse ? "panning" : "new", total / areas, worst, cands / areas, valid / areas, unknown / areas);
                fflush(stdout);
            }
        }
    }
    printf("(ms/query includes creating the finder for each \"new\" query; cands, valid, unknown per query)\n");
    return g_fail ? 1 : 0;
}

// Lists the structure start candidates of an area.
static int cmdStarts(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QStringList packs;
    QString only, preset = "minecraft:normal";
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--structure" && i + 1 < args.size()) { only = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--preset" && i + 1 < args.size()) { preset = args[i + 1]; args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 7)
    {
        printf("usage: dptest starts <vanilla data> <mc> <seed> <chunk x0> <chunk z0> <chunk x1> <chunk z1>\n"
            "           [--pack <pack> ...] [--preset <id>] [--structure <id>]\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(str2mc(qPrintable(args[1])), args[0], packs, preset, &report);
    dp::StructureFinder f(w, w->dimension("minecraft:overworld"), (uint64_t) args[2].toLongLong());
    std::vector<dp::StructureStart> out;
    if (!f.starts(args[3].toInt(), args[4].toInt(), args[5].toInt(), args[6].toInt(), &out))
        return 1;
    for (const dp::StructureStart& s : out)
    {
        const QString& id = w->structures()[(size_t) s.structure].id;
        if (!only.isEmpty() && id != only)
            continue;
        printf("%s %d %d  at %d %s %d  %s\n", qPrintable(id), s.chunkX, s.chunkZ, s.x,
            s.hasY ? qPrintable(QString::number(s.y)) : "?", s.z, stateName(s));
    }
    return 0;
}

//==============================================================================
// heights: terrain heights and biomes against the files of real servers
//==============================================================================

struct HeightStats
{
    int64_t n = 0, exact = 0, within2 = 0, within8 = 0, above = 0;
    double sumAbs = 0, sum = 0;
    void add(int e)
    {
        n++;
        exact += e == 0;
        within2 += std::abs(e) <= 2;
        within8 += std::abs(e) <= 8;
        above += e > 0;
        sumAbs += std::abs(e);
        sum += e;
    }
    void print(const char *label) const
    {
        if (!n)
            return;
        printf("  %-22s n=%lld: exact %.2f %%, within 2: %.2f %%, within 8: %.2f %%, mean abs %.2f, bias %+.2f, "
            "computed above the server %.2f %%\n", label, (long long) n, 100.0 * exact / n, 100.0 * within2 / n,
            100.0 * within8 / n, sumAbs / n, sum / n, 100.0 * above / n);
    }
};

// The terrain heights (H lines: WORLD_SURFACE and OCEAN_FLOOR of finished
// chunks, so with trees, carvers and structures) and biome cells (B lines)
// of a server file against the generic path, for the overworld, the Nether
// and the End (# dimension). For the built-in overworld also the engine's
// height (the Amplified work's model) as the reference.
static int cmdHeights(const QStringList& argsIn)
{
    QStringList args = argsIn, packs;
    int every = 1, list = 0, biomeTolerance = 0;
    double minExact = -1;
    int64_t maxFluid = -1;
    // the heights after the surface rules (the map's), or with --fill those
    // of the terrain fill (the structures')
    dp::HeightDetail detail = dp::HEIGHTS_SURFACE;
    bool notWorse = false;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--fill") { detail = dp::HEIGHTS_FILL; args.removeAt(i); }
        // fails where the surface rules match fewer columns than the fill
        else if (args[i] == "--not-worse") { notWorse = true; args.removeAt(i); }
        else if (args[i] == "--every" && i + 1 < args.size()) { every = args[i + 1].toInt(); args.remove(i, 2); }
        else if (args[i] == "--list" && i + 1 < args.size()) { list = args[i + 1].toInt(); args.remove(i, 2); }
        else if (args[i] == "--min-exact" && i + 1 < args.size()) { minExact = args[i + 1].toDouble(); args.remove(i, 2); }
        else if (args[i] == "--biome-tolerance" && i + 1 < args.size()) { biomeTolerance = args[i + 1].toInt(); args.remove(i, 2); }
        else if (args[i] == "--max-fluid" && i + 1 < args.size()) { maxFluid = args[i + 1].toLongLong(); args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 3)
    {
        printf("usage: dptest heights <vanilla data> <mc> [--pack <pack>]... [--every N] [--list N] "
            "[--min-exact <percent>] [--max-fluid N] [--biome-tolerance N] [--fill] [--not-worse] <server file> ...\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const QString vanilla = args[0];
    const int mc = str2mc(qPrintable(args[1]));
    std::map<QString, std::shared_ptr<const dp::World>> worlds;
    for (int fi = 2; fi < args.size(); fi++)
    {
        const QString path = args[fi];
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
        {
            printf("cannot read %s\n", qPrintable(path));
            g_fail++;
            continue;
        }
        QString levelType = "normal", dimId = "minecraft:overworld";
        bool hasPack = false, terrainOnly = false;
        int64_t seed = 0;
        struct H { int x, z, ws, of; };
        std::vector<H> hs;
        struct F { int x, z, y; };
        std::vector<F> fls;     // the Nether: the floor below y 100
        int floorBelow = 100;
        std::vector<ServerFile::Cell> bs;
        std::vector<ServerFile::Cell> zs;   // Z: the biome of single blocks (the 1:1 zoom)
        int line = 0, fline = 0;
        while (!f.atEnd())
        {
            QByteArray l = f.readLine().trimmed();
            if (l.startsWith("Z "))
            {
                QList<QByteArray> p = l.split(' ');
                if (p.size() == 5)
                    zs.push_back({ p[1].toInt(), p[2].toInt(), p[3].toInt(), QString::fromLatin1(p[4]) });
            }
            else if (l.startsWith("# level_type ")) levelType = QString::fromLatin1(l.mid(13));
            else if (l.startsWith("# dimension ")) dimId = QString::fromLatin1(l.mid(12));
            else if (l.startsWith("# seed ")) seed = l.mid(7).toLongLong();
            else if (l.startsWith("# datapack ")) hasPack = true;
            else if (l.startsWith("# terrain_only")) terrainOnly = true;
            else if (l.startsWith("# F ") && l.contains("below y "))
                floorBelow = l.mid(l.indexOf("below y ") + 8).split(' ').value(0).toInt();
            else if (l.startsWith("F "))
            {
                QList<QByteArray> p = l.split(' ');
                if (p.size() == 4 && fline++ % every == 0)
                    fls.push_back({ p[1].toInt(), p[2].toInt(), p[3].toInt() });
            }
            else if (l.startsWith("H "))
            {
                if (line++ % every)
                    continue;
                QList<QByteArray> p = l.split(' ');
                if (p.size() == 5)
                    hs.push_back({ p[1].toInt(), p[2].toInt(), p[3].toInt(), p[4].toInt() });
            }
            else if (l.startsWith("B "))
            {
                QList<QByteArray> p = l.split(' ');
                if (p.size() != 5)
                    continue;
                ServerFile::Cell c { p[1].toInt(), p[2].toInt(), p[3].toInt(), QString::fromLatin1(p[4]) };
                if (!c.biome.contains(':'))
                    c.biome = "minecraft:" + c.biome;
                bs.push_back(c);
            }
        }
        if (hasPack && packs.isEmpty())
        {
            printf("%s: the file is of a data pack world: pass the pack with --pack\n", qPrintable(path));
            g_fail++;
            continue;
        }
        if (hs.empty() && fls.empty() && bs.empty())
        {
            printf("%s: no H, F or B lines\n", qPrintable(path));
            g_fail++;
            continue;
        }
        const QString preset = "minecraft:" + levelType;
        if (!worlds.count(preset))
        {
            dp::Report report;
            worlds[preset] = loadWorld(mc, vanilla, packs, preset, &report);
            CHECK(report.ok(), "%s: the world has errors", qPrintable(preset));
        }
        std::shared_ptr<const dp::World> w = worlds[preset];
        const int dim = w->dimension(dimId);
        if (dim < 0 || !w->dimensions()[dim].supported)
        {
            printf("%s: dimension %s not supported (%s)\n", qPrintable(path), qPrintable(dimId),
                dim >= 0 ? qPrintable(w->dimensions()[dim].reason) : "missing");
            g_fail++;
            continue;
        }
        const dp::DimensionInfo& info = w->dimensions()[dim];
        printf("%s (%s, seed %lld): terrain %s%s\n", qPrintable(QFileInfo(path).fileName()), qPrintable(dimId),
            (long long) seed, info.terrain ? "yes" : "no", info.hasCeiling ? ", has a ceiling" : "");
        if (!info.aquiferReason.isEmpty())
            printf("  aquifers not modelled: %s\n", qPrintable(info.aquiferReason));
        dp::Sampler s(w, dim, (uint64_t) seed);

        // biomes
        int engineDim = dimId == "minecraft:the_nether" ? DIM_NETHER : dimId == "minecraft:the_end" ? DIM_END
            : DIM_OVERWORLD;
        Generator g;
        const bool builtIn = !hasPack && (levelType == "normal" || levelType == "amplified");
        if (builtIn)
        {
            setupGenerator(&g, mc, levelType == "amplified" ? AMPLIFIED : 0);
            applySeed(&g, engineDim, (uint64_t) seed);
        }
        Counter bc, be;
        int shown = 0;
        for (const ServerFile::Cell& c : bs)
        {
            int b = s.biomeAt(4, c.x >> 2, c.y, c.z >> 2);
            QString got = b >= 0 ? w->biomes()[b].id : QString("-");
            bc.add(got == c.biome);
            if (got != c.biome && shown++ < list)
                printf("    biome %d %d %d: server %s, generic %s\n", c.x, c.y, c.z, qPrintable(c.biome), qPrintable(got));
            if (builtIn)
            {
                const char *e = biome2str(mc, getBiomeAt(&g, 4, c.x >> 2, c.y >> 2, c.z >> 2));
                be.add(e && c.biome == QString("minecraft:") + e);
            }
        }
        printf("  biome cells: %lld, differing: generic %lld", (long long) bc.n, (long long) bc.diff);
        if (builtIn)
            printf(", native engine %lld", (long long) be.diff);
        printf("\n");
        CHECK(bc.diff <= biomeTolerance, "%s: %lld biome cells differ", qPrintable(path), (long long) bc.diff);

        // the 1:1 zoom (Z lines): single blocks where the zoom decides
        // between cells, one by one and as a pixel of a 1:1 tile
        if (!zs.empty())
        {
            Counter zc, zt, ze;
            int zshown = 0;
            for (const ServerFile::Cell& c : zs)
            {
                const QString want = c.biome.contains(':') ? c.biome : "minecraft:" + c.biome;
                const int b = s.biomeAt(1, c.x, c.y, c.z);
                const QString got = b >= 0 ? w->biomes()[b].id : QString("-");
                zc.add(got == want);
                int tile[5 * 5];
                const bool ok = s.genBiomes(tile, 1, c.x - 2, c.z - 2, 5, 5, c.y);
                const QString tgot = ok && tile[12] >= 0 ? w->biomes()[tile[12]].id : QString("-");
                zt.add(tgot == want);
                if ((got != want || tgot != want) && zshown++ < list)
                    printf("    block %d %d %d: server %s, generic %s, tile %s\n", c.x, c.y, c.z, qPrintable(want),
                        qPrintable(got), qPrintable(tgot));
                if (builtIn)
                {
                    const char *e = biome2str(mc, getBiomeAt(&g, 1, c.x, c.y, c.z));
                    const bool same = e && want == QString("minecraft:") + e;
                    ze.add(same);
                    if (!same && zshown++ < list)
                        printf("    block %d %d %d: server %s, native engine %s\n", c.x, c.y, c.z, qPrintable(want),
                            e ? e : "-");
                }
            }
            printf("  1:1 blocks (Z): %lld, differing: generic %lld, in tiles %lld", (long long) zc.n,
                (long long) zc.diff, (long long) zt.diff);
            if (builtIn)
                printf(", native engine %lld", (long long) ze.diff);
            printf("\n");
            CHECK(zc.diff == 0 && zt.diff == 0, "%s: %lld blocks differ at 1:1", qPrintable(path),
                (long long) (zc.diff + zt.diff));
        }

        // heights
        if (!info.terrain || hs.empty())
        {
            if (!info.terrain)
                printf("  no terrain: %s\n", qPrintable(info.terrainReason));
            continue;
        }
        if (detail == dp::HEIGHTS_SURFACE && !info.surfaceReason.isEmpty())
            printf("  surface rules left out: %s\n", qPrintable(info.surfaceReason));
        const bool engineHeights = builtIn && engineDim == DIM_OVERWORLD;
        TerrainNoise tn;
        if (engineHeights)
            initTerrainNoise(&tn, (uint64_t) seed);
        HeightStats of, ws, eng, ofOn, ofOff;
        int64_t engOnly = 0, dpOnly = 0;
        // the columns whose ocean floor differs, by what the server has there
        int64_t ofFluid = 0, ofSoftTop = 0, ofOther = 0;
        // --not-worse: exact columns of the fill and of the surface rules
        int64_t fillOf = 0, fillWs = 0, rulesOf = 0, rulesWs = 0;
        shown = 0;
        QElapsedTimer t;
        int64_t ns = 0;
        for (const H& h : hs)
        {
            t.start();
            int a = s.surfaceHeight(h.x, h.z, dp::HEIGHT_OCEAN_FLOOR, 0x7fffffff, detail);
            int b = s.surfaceHeight(h.x, h.z, dp::HEIGHT_SURFACE, 0x7fffffff, detail);
            ns += t.nsecsElapsed();
            const int floorNone = info.minY - 1;
            if (a == dp::NO_HEIGHT) a = floorNone;
            if (b == dp::NO_HEIGHT) b = floorNone;
            if (notWorse)
            {
                int fa = s.surfaceHeight(h.x, h.z, dp::HEIGHT_OCEAN_FLOOR, 0x7fffffff, dp::HEIGHTS_FILL);
                int fb = s.surfaceHeight(h.x, h.z, dp::HEIGHT_SURFACE, 0x7fffffff, dp::HEIGHTS_FILL);
                if (fa == dp::NO_HEIGHT) fa = floorNone;
                if (fb == dp::NO_HEIGHT) fb = floorNone;
                fillOf += fa == h.of;
                fillWs += fb == h.ws;
                rulesOf += a == h.of;
                rulesWs += b == h.ws;
            }
            of.add(a - h.of);
            ws.add(b - h.ws);
            if (a != h.of)
            {
                if (a < h.of && h.ws > h.of)
                    ofFluid++;      // a solid block below fluid, above the terrain fill's floor
                else if (a == h.ws && b == h.ws && h.of < h.ws)
                    ofSoftTop++;    // the top block does not block motion
                else
                    ofOther++;
            }
            if (!engineHeights && (a != h.of || b != h.ws) && shown++ < list)
                printf("    column %d %d: server %d %d, generic %d %d (world_surface, ocean_floor)\n", h.x, h.z,
                    h.ws, h.of, b, a);
            // columns off the 4 block grid (files made with --h-step 1 or 2)
            if ((h.x | h.z) & 3)
                ofOff.add(a - h.of);
            else
                ofOn.add(a - h.of);
            if (engineHeights)
            {
                int e = getTerrainHeightAt(&g.bn, &tn, h.x, h.z) - 1;
                eng.add(e - h.of);
                if (e == h.of && a != h.of)
                {
                    engOnly++;
                    if (shown++ < list)
                        printf("    column %d %d: server ocean_floor %d, engine %d, generic %d\n", h.x, h.z, h.of, e, a);
                }
                if (e != h.of && a == h.of)
                    dpOnly++;
            }
        }
        of.print("ocean_floor");
        ws.print("world_surface");
        if (notWorse)
        {
            printf("  exact columns, terrain fill -> surface rules: ocean_floor %lld -> %lld, world_surface %lld -> %lld\n",
                (long long) fillOf, (long long) rulesOf, (long long) fillWs, (long long) rulesWs);
            CHECK(rulesOf >= fillOf && rulesWs >= fillWs, "%s: the surface rules match fewer columns than the fill",
                qPrintable(path));
        }
        printf("  ocean_floor differs in %lld columns: %lld with the server's floor higher under a fluid (aquifers),"
            " %lld under a top block that does not block motion, %lld other%s\n", (long long) (ofFluid + ofSoftTop
            + ofOther), (long long) ofFluid, (long long) ofSoftTop, (long long) ofOther, terrainOnly
            ? (detail == dp::HEIGHTS_SURFACE
                ? " (what the surface step builds in code: icebergs, eroded badlands pillars)"
                : " (surface rules that place a fluid or air, or what the aquifers place without a fluid)")
            : " (features such as trees, carvers, structures, surface rules)");
        if (ofOff.n)
        {
            ofOn.print("ocean_floor on grid");
            ofOff.print("ocean_floor off grid");
        }
        if (engineHeights)
        {
            eng.print("engine ocean_floor");
            printf("  exact with the engine but not generic: %lld columns; generic but not the engine: %lld\n",
                (long long) engOnly, (long long) dpOnly);
        }
        printf("  %.1f us per column (both heights)\n", ns / 1000.0 / hs.size());
        if (!fls.empty())
        {
            // the floor of the Nether below a height (lava counts as a
            // fluid, not as the floor)
            HeightStats fs;
            for (const F& fl : fls)
            {
                int a = s.surfaceHeight(fl.x, fl.z, dp::HEIGHT_OCEAN_FLOOR, floorBelow);
                fs.add((a == dp::NO_HEIGHT ? info.minY - 1 : a) - fl.y);
            }
            fs.print("floor below y");
        }
        if (minExact >= 0)
            CHECK(100.0 * of.exact / of.n >= minExact, "%s: only %.2f %% of the ocean_floor heights exact",
                qPrintable(path), 100.0 * of.exact / of.n);
        if (maxFluid >= 0)
            CHECK(ofFluid <= maxFluid, "%s: %lld columns with the server's floor higher under a fluid",
                qPrintable(path), (long long) ofFluid);
    }
    return g_fail != 0;
}

// aqperf: microseconds per preliminary surface of the aquifers (with the
// bounds and every height scanned, the same values) for the overworld
static int cmdAquiferPerf(const QStringList& argsIn)
{
    QStringList args = argsIn, packs;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 2)
    {
        printf("usage: dptest aqperf <vanilla data> <mc> [--pack <pack>]...\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, "minecraft:normal", &report, false);
    const int dim = w->dimension("minecraft:overworld");
    if (dim < 0 || !w->d()->data[(size_t) dim].terrain.aquifer.on)
    {
        printf("no modelled aquifers\n");
        return 0;
    }
    const dp::AquiferDef& def = w->d()->data[(size_t) dim].terrain.aquifer;
    int64_t differ = 0;
    double us[2] = { 0, 0 };
    const int n = 64;
    for (int pass = 0; pass < 2; pass++)
    {
        dp::setTerrainPruning(pass == 0);
        for (int k = 0; k < 4; k++)
        {
            dp::AquiferState a(*w->d()->graph, def, g_seeds[k], mc);
            QElapsedTimer t;
            t.start();
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++)
                    a.preliminarySurface(k * 7919 + i * 20, k * -4513 + j * 20);
            us[pass] += t.nsecsElapsed() / 1000.0 / (n * n) / 4;
        }
    }
    dp::setTerrainPruning(true);
    for (int k = 0; k < 4; k++)
    {
        dp::AquiferState a(*w->d()->graph, def, g_seeds[k], mc), b(*w->d()->graph, def, g_seeds[k], mc);
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 16; i++)
            {
                const int x = k * 7919 + i * 20, z = k * -4513 + j * 20;
                const int va = a.preliminarySurface(x, z);
                dp::setTerrainPruning(false);
                const int vb = b.preliminarySurface(x, z);
                dp::setTerrainPruning(true);
                differ += va != vb;
            }
    }
    printf("preliminary surface: %.2f us with bounds, %.2f us every height (%lld of 1024 differ)\n", us[0], us[1],
        (long long) differ);
    CHECK(differ == 0, "the bounds change the preliminary surface");
    return g_fail != 0;
}

// tileperf: milliseconds per map tile of the terrain layer (genBothHeights
// of 130 x 130 columns, as dpTileBothHeights) at the scales 1 to 256, four
// neighbouring tiles per scale with one sampler (as a map thread)
static int cmdTilePerf(const QStringList& argsIn)
{
    QStringList args = argsIn, packs;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else i++;
    }
    if (args.size() < 2)
    {
        printf("usage: dptest tileperf <vanilla data> <mc> [--pack <pack>]...\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, "minecraft:normal", &report, false);
    const int dim = w->dimension("minecraft:overworld");
    if (dim < 0 || !w->dimensions()[dim].terrain)
        return 1;
    // DPTEST_TILE_APP=1: the tiles of the app's timing views (place 11 + i
    // for the terrain layer at 1:4^i, 3 x 3 tiles around the view's center);
    // with DPTEST_TILE_KIND=heights the height layer's (place 6 + i, tiles
    // of 34 x 34 ocean floor heights one scale coarser, 5 x 5 tiles)
    const bool app = qEnvironmentVariableIsSet("DPTEST_TILE_APP");
    const bool heightsKind = qgetenv("DPTEST_TILE_KIND") == "heights";
    const int tw = heightsKind ? 34 : 130;
    printf("ms per tile of %d x %d columns:", tw, tw);
    std::vector<int> scales { 1, 4, 16, 64, 256 };
    if (qEnvironmentVariableIsSet("DPTEST_TILE_SCALE"))
        scales = { qEnvironmentVariableIntValue("DPTEST_TILE_SCALE") };
    for (int scale : scales)
    {
        double ms = 0;
        int tiles = 0;
        const int rounds = qEnvironmentVariableIsSet("DPTEST_TILE_ROUNDS") ? qEnvironmentVariableIntValue("DPTEST_TILE_ROUNDS") : 2;
        int si = 0;
        for (int v = scale; v > 1; v /= 4)
            si++;
        const int layer = heightsKind ? std::min(scale * 4, 1024) : scale;
        const int pix = tw - 2;
        for (int k = 0; k < rounds; k++)
        {
            dp::Sampler s(w, dim, app ? 12345 : g_seeds[k % 4]);
            std::vector<int> fl(size_t(tw) * tw), sf(fl.size());
            const int n = app ? (heightsKind ? 25 : 9) : 4;
            for (int t = 0; t < n; t++)
            {
                int x, z;
                if (app)
                {
                    const int place = (heightsKind ? 6 : 11) + si + k * 20;
                    const int64_t cx = 36000LL * place + 8, cz = -22000LL * place + 8;
                    const int side = heightsKind ? 5 : 3;
                    const int tx = (int) std::floor((double) cx / (pix * layer)) + t % side - side / 2;
                    const int tz = (int) std::floor((double) cz / (pix * layer)) + t / side - side / 2;
                    x = tx * pix - 1;
                    z = tz * pix - 1;
                }
                else
                {
                    x = (k * 5000) / scale + t * 128 - 1;
                    z = (k * -3000) / scale - 1 + (k / 4) * 300;
                }
                QElapsedTimer timer;
                timer.start();
                // the map's detail (dpMapHeightDetail())
                const dp::HeightDetail detail = layer >= 16 ? dp::HEIGHTS_FAST : dp::HEIGHTS_SURFACE;
                if (heightsKind)
                    s.genHeights(fl.data(), layer, x, z, tw, tw, dp::HEIGHT_OCEAN_FLOOR, 0x7fffffff, nullptr, detail);
                else
                    s.genBothHeights(fl.data(), sf.data(), layer, x, z, tw, tw, 0x7fffffff, nullptr, detail);
                ms += timer.nsecsElapsed() / 1e6;
                tiles++;
            }
        }
        printf(" 1:%d %.2f", scale, ms / tiles);
        fflush(stdout);
    }
    printf("\n");
    return 0;
}

// hperf: microseconds per column of genHeights at the scales 1, 4 and 16,
// for the overworld (normal, amplified), the Nether (below y 100) and the
// End; with --compare also each height against surfaceHeight() of the column
// and, if pruning can be switched off, the pruned against the full scan.
static int cmdHeightPerf(const QStringList& argsIn)
{
    QStringList args = argsIn, packs;
    bool compare = false, far = false;     // far: the scales 64 and 256 instead
    dp::HeightKind kind = dp::HEIGHT_OCEAN_FLOOR;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--compare") { compare = true; args.removeAt(i); }
        else if (args[i] == "--surface") { kind = dp::HEIGHT_SURFACE; args.removeAt(i); }
        else if (args[i] == "--far") { far = true; args.removeAt(i); }
        else i++;
    }
    if (args.size() < 2)
    {
        printf("usage: dptest hperf <vanilla data> <mc> [--pack <pack>]... [--compare] [--surface]\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    struct Case { const char *preset, *dim; int top; };
    static const Case cases[] = {
        { "minecraft:normal", "minecraft:overworld", 0x7fffffff },
        { "minecraft:amplified", "minecraft:overworld", 0x7fffffff },
        { "minecraft:normal", "minecraft:the_nether", 100 },
        { "minecraft:normal", "minecraft:the_end", 0x7fffffff },
    };
    for (const Case& c : cases)
    {
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, c.preset, &report, false);
        const int dim = w->dimension(c.dim);
        if (dim < 0 || !w->dimensions()[dim].terrain)
        {
            printf("%s %s: no terrain\n", c.preset, c.dim);
            continue;
        }
        printf("%s %s:", c.preset, c.dim);
        static const int nearScales[3] = { 1, 4, 16 }, farScales[3] = { 64, 256, 1024 };
        const int *scales = far ? farScales : nearScales;
        int64_t bothChecked = 0, bothDiffer = 0;
        for (int si = 0; si < 3; si++)
        {
            const int scale = scales[si];
            const int size = scale >= 16 ? 24 : 48;
            int64_t cols = 0, ns = 0, differ = 0, checked = 0;
            for (int k = 0; k < 4; k++)
            {
                const uint64_t seed = g_seeds[k];
                dp::Sampler s(w, dim, seed);
                std::vector<int> out((size_t) size * size);
                const int x = (k * 2311 - 3000) / scale, z = (k * -1733 + 1500) / scale;
                QElapsedTimer t;
                t.start();
                s.genHeights(out.data(), scale, x, z, size, size, kind, c.top);
                ns += t.nsecsElapsed();
                cols += size * size;
                if (compare)
                {
                    // every column without bounds (every block scanned)
                    dp::setTerrainPruning(false);
                    dp::Sampler s2(w, dim, seed);
                    for (int j = 0; j < size; j += 1 + (scale == 1))
                        for (int i = 0; i < size; i += 1 + (scale == 1))
                        {
                            checked++;
                            int h = s2.surfaceHeight((x + i) * scale, (z + j) * scale, kind, c.top);
                            if (h != out[(size_t) j * size + i])
                            {
                                if (differ++ < 5)
                                    printf("\n  differs at %d %d: genHeights %d, full scan %d", (x + i) * scale,
                                        (z + j) * scale, out[(size_t) j * size + i], h);
                            }
                        }
                    dp::setTerrainPruning(true);
                    // both kinds at once against one call per kind
                    std::vector<int> fl((size_t) size * size), sf(fl.size()), bf(fl.size()), bs(fl.size());
                    dp::Sampler s3(w, dim, seed), s4(w, dim, seed);
                    s3.genHeights(fl.data(), scale, x, z, size, size, dp::HEIGHT_OCEAN_FLOOR, c.top);
                    s3.genHeights(sf.data(), scale, x, z, size, size, dp::HEIGHT_SURFACE, c.top);
                    CHECK(s4.genBothHeights(bf.data(), bs.data(), scale, x, z, size, size, c.top),
                        "genBothHeights failed");
                    for (size_t q = 0; q < fl.size(); q++)
                    {
                        bothChecked++;
                        bothDiffer += fl[q] != bf[q] || sf[q] != bs[q];
                    }
                }
            }
            printf(" scale %d %.1f us", scale, ns / 1000.0 / cols);
            if (compare)
            {
                printf(" (%lld of %lld differ)", (long long) differ, (long long) checked);
                CHECK(differ == 0, "genHeights differs from surfaceHeight");
            }
        }
        printf("\n");
        if (compare)
        {
            printf("  both kinds at once (genBothHeights): %lld of %lld columns differ\n", (long long) bothDiffer,
                (long long) bothChecked);
            CHECK(bothDiffer == 0, "genBothHeights differs from genHeights");
        }
    }
    return g_fail != 0;
}

//==============================================================================
// limits: crafted packs and archives against the loader's limits
//==============================================================================

// The app loads packs and evaluates terrain on threads with the platform's
// default stack (512 KiB on macOS); the tests run there too. Sanitizer
// builds have frames several times larger.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DPTEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define DPTEST_ASAN 1
#endif
#ifdef DPTEST_ASAN
static const unsigned kStack = 16u * 512u * 1024u;
static const long kRssSlack = 400;
#else
static const unsigned kStack = 512u * 1024u;
static const long kRssSlack = 100;
#endif

// DPTEST_STACK_KIB overrides the stack size (to measure the margin)
static void onThread(const std::function<void()>& fn, unsigned stack = kStack)
{
    if (qEnvironmentVariableIsSet("DPTEST_STACK_KIB"))
        stack = (unsigned) qEnvironmentVariableIntValue("DPTEST_STACK_KIB") * 1024u;
    std::unique_ptr<QThread> t(QThread::create(fn));
    t->setStackSize(stack);
    t->start();
    t->wait();
}

static long maxRssMiB()
{
#ifdef _WIN32
    return 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (long) (ru.ru_maxrss >> 20);     // bytes
#else
    return (long) (ru.ru_maxrss >> 10);     // KiB
#endif
#endif
}

// A raw deflate stream of @data (qCompress gives a zlib stream after a
// 4 byte length: 2 bytes of header in front, 4 of checksum at the end)
static QByteArray rawDeflate(const QByteArray& data)
{
    QByteArray z = qCompress(data, 9);
    return z.mid(6, z.size() - 10);
}

static void put16(QByteArray& b, uint32_t v) { b.append((char) (v & 0xff)); b.append((char) ((v >> 8) & 0xff)); }
static void put32(QByteArray& b, uint32_t v) { put16(b, v & 0xffff); put16(b, v >> 16); }
static void put64(QByteArray& b, uint64_t v) { put32(b, (uint32_t) v); put32(b, (uint32_t) (v >> 32)); }

// A small zip writer for crafted archives
struct ZipBuild
{
    struct Entry
    {
        QByteArray name;
        uint32_t crc = 0;
        uint64_t size = 0;
        QByteArray stored;      // the bytes in the archive
        bool deflated = false;
        uint64_t offset = 0;    // of the local header
    };
    std::vector<Entry> entries;
    QByteArray body;            // the local entries

    // a local entry with its own header
    void add(const QByteArray& name, const QByteArray& data, bool deflate = true)
    {
        Entry e;
        e.name = name;
        e.crc = dp::crc32((const unsigned char*) data.constData(), (size_t) data.size());
        e.size = (uint64_t) data.size();
        e.deflated = deflate;
        e.stored = deflate ? rawDeflate(data) : data;
        addRaw(e);
    }
    void addRaw(Entry e)
    {
        e.offset = (uint64_t) body.size();
        put32(body, 0x04034b50u);
        put16(body, 20); put16(body, 0); put16(body, e.deflated ? 8 : 0); put16(body, 0); put16(body, 0);
        put32(body, e.crc); put32(body, (uint32_t) e.stored.size()); put32(body, (uint32_t) e.size);
        put16(body, (uint32_t) e.name.size()); put16(body, 0);
        body.append(e.name);
        body.append(e.stored);
        entries.push_back(e);
    }
    // another directory entry for the local entry of entries[@of]
    void alias(const QByteArray& name, size_t of)
    {
        Entry e = entries[of];
        e.name = name;
        entries.push_back(e);
    }
    // @beforeEocd: bytes put between the directory and its end record
    QByteArray finish(const QByteArray& beforeEocd = QByteArray()) const
    {
        QByteArray out = body;
        QByteArray dir;
        for (const Entry& e : entries)
        {
            put32(dir, 0x02014b50u);
            put16(dir, 20); put16(dir, 20); put16(dir, 0); put16(dir, e.deflated ? 8 : 0); put16(dir, 0); put16(dir, 0);
            put32(dir, e.crc); put32(dir, (uint32_t) e.stored.size()); put32(dir, (uint32_t) e.size);
            put16(dir, (uint32_t) e.name.size()); put16(dir, 0); put16(dir, 0); put16(dir, 0); put16(dir, 0);
            put32(dir, 0); put32(dir, (uint32_t) e.offset);
            dir.append(e.name);
        }
        const uint64_t dirOffset = (uint64_t) out.size();
        out.append(dir);
        out.append(beforeEocd);
        put32(out, 0x06054b50u);
        put16(out, 0); put16(out, 0);
        put16(out, (uint32_t) entries.size()); put16(out, (uint32_t) entries.size());
        put32(out, (uint32_t) dir.size()); put32(out, (uint32_t) dirOffset);
        put16(out, 0);
        return out;
    }
};

static const char *kMeta = "{\"pack\":{\"pack_format\":48,\"supported_formats\":[1,999],\"description\":\"limits\"}}";

// noise settings with only what a dimension's terrain needs
static QByteArray noiseSettings(int mc, const QByteArray& finalDensity, const QByteArray& noise = QByteArray())
{
    QByteArray n = noise;
    if (n.isEmpty())
        n = mc >= MC_26_3 ? "{\"min_y\":-64,\"height\":384}"
                          : "{\"min_y\":-64,\"height\":384,\"size_horizontal\":1,\"size_vertical\":2}";
    return "{\"sea_level\":63,\"disable_mob_generation\":false,\"aquifers_enabled\":false,\"ore_veins_enabled\":false,"
        "\"legacy_random_source\":false,\"default_block\":{\"Name\":\"minecraft:stone\"},"
        "\"default_fluid\":{\"Name\":\"minecraft:water\",\"Properties\":{\"level\":\"0\"}},\"noise\":" + n +
        ",\"noise_router\":{\"final_density\":" + finalDensity + "}}";
}

static const char *kType = "{\"min_y\":-64,\"height\":384,\"logical_height\":384,\"has_ceiling\":false}";

// a dimension with a fixed biome and the terrain @finalDensity
static QByteArray customDim(int mc, const QByteArray& finalDensity, const QByteArray& type = kType,
    const QByteArray& noise = QByteArray(), const QByteArray& source = QByteArray())
{
    QByteArray src = source.isEmpty() ? QByteArray("{\"type\":\"minecraft:fixed\",\"biome\":\"minecraft:plains\"}") : source;
    return "{\"type\":" + type + ",\"generator\":{\"type\":\"minecraft:noise\",\"settings\":" +
        noiseSettings(mc, finalDensity, noise) + ",\"biome_source\":" + src + "}}";
}

// 1 at y -64 to -1 at y 320 (the format of 26.3 has its own type)
static QByteArray gradient(int mc, int fromY = -64, int toY = 320, double fromV = 1.0, double toV = -1.0)
{
    if (mc >= MC_26_3)
        return "{\"type\":\"minecraft:gradient\",\"axis\":\"y\",\"from_coordinate\":" + QByteArray::number(fromY) +
            ",\"to_coordinate\":" + QByteArray::number(toY) + ",\"from_value\":" + QByteArray::number(fromV) +
            ",\"to_value\":" + QByteArray::number(toV) + "}";
    return "{\"type\":\"minecraft:y_clamped_gradient\",\"from_y\":" + QByteArray::number(fromY) + ",\"to_y\":" +
        QByteArray::number(toY) + ",\"from_value\":" + QByteArray::number(fromV) + ",\"to_value\":" +
        QByteArray::number(toV) + "}";
}

// abs(@arg) (26.3 names the argument "input")
static QByteArray absOf(int mc, const QByteArray& arg)
{
    return QByteArray("{\"type\":\"minecraft:abs\",\"") + (mc >= MC_26_3 ? "input" : "argument") + "\":" + arg + "}";
}

// @n levels of abs() around @leaf
static QByteArray nested(int mc, int n, const QByteArray& leaf)
{
    QByteArray s;
    for (int i = 0; i < n; i++)
        s += QByteArray("{\"type\":\"minecraft:abs\",\"") + (mc >= MC_26_3 ? "input" : "argument") + "\":";
    s += leaf;
    for (int i = 0; i < n; i++)
        s += "}";
    return s;
}

// loads on a thread with the app's stack
static std::shared_ptr<const dp::World> loadOnThread(int mc, const QString& vanilla, const QStringList& packs,
    dp::Report *report, const QString& preset = "minecraft:normal")
{
    std::shared_ptr<const dp::World> w;
    onThread([&] { w = loadWorld(mc, vanilla, packs, preset, report, false); });
    return w;
}

// heights of a 16 x 16 area on a thread with the app's stack
static bool heightsOnThread(std::shared_ptr<const dp::World> w, int dim, std::vector<int> *out = nullptr)
{
    bool ok = false;
    std::vector<int> h(256, 0);
    onThread([&] {
        dp::Sampler s(w, dim, 12345);
        ok = s.genHeights(h.data(), 1, 0, 0, 16, 16);
    });
    if (out)
        *out = h;
    return ok;
}

static bool errorNaming(const dp::Report& r, const QString& pack, const QString& id, const QString& part)
{
    return hasIssue(r, dp::Issue::Error, pack, id, part);
}

static void limitsZip(const QString& work)
{
    // C1: a zip64 locator whose offset wraps around when 56 is added
    for (uint64_t k : { 56ull, 48ull, 8ull })
    {
        ZipBuild z;
        z.add("a.txt", "hello", false);
        QByteArray loc;
        put32(loc, 0x07064b50u);
        put32(loc, 0);
        put64(loc, 0 - k);
        put32(loc, 1);
        const QByteArray data = z.finish(loc);
        dp::ZipReader r;
        QString err;
        if (r.openData(data, &err))
        {
            QByteArray d;
            for (size_t i = 0; i < r.entries().size(); i++)
                r.read(i, &d, &err);
        }
        const QString path = work + QString("/z64wrap_%1.zip").arg(k);
        writeFile(path, data);
        dp::ZipReader f;
        f.open(path, &err);
        // in a server bundle (the inner jar is opened from memory)
        ZipBuild outer;
        outer.add("META-INF/versions/1.21.1/server-1.21.1.jar", data);
        const QString bundle = work + QString("/bundle_z64wrap_%1.jar").arg(k);
        writeFile(bundle, outer.finish());
        dp::ZipReader b;
        b.open(bundle, &err);
    }
    printf("zip64 locator offsets that wrap: no read outside the archive (ASan checks)\n");

    // C3: directory entries that share one local entry (a zip bomb)
    {
        ZipBuild z;
        z.add("data/limits/worldgen/density_function/b0.json", QByteArray(64 << 20, ' '));
        for (int i = 1; i < 4; i++)
            z.alias(QByteArray("data/limits/worldgen/density_function/b") + QByteArray::number(i) + ".json", 0);
        const QString path = work + "/limits-overlap.zip";
        writeFile(path, z.finish());
        dp::ZipReader r;
        QString err;
        CHECK(!r.open(path, &err) && err.contains("overlap"), "overlapping entries open: %s", qPrintable(err));
        printf("overlapping entries: %s\n", qPrintable(err));
    }
    // an entry whose local extra field moves its data into the next entry:
    // the directory looks fine, reading the entry is refused
    {
        ZipBuild z;
        z.add("a.json", "{\"a\":1}", false);
        z.add("b.json", "{\"b\":2}", false);
        QByteArray data = z.finish();
        data[28] = (char) 12;       // local extra length of a.json
        dp::ZipReader r;
        QString err;
        CHECK(r.openData(data, &err), "the archive does not open: %s", qPrintable(err));
        QByteArray d;
        CHECK(!r.read(0, &d, &err) && err.contains("overlap"), "an entry reaching into the next one is read: %s",
            qPrintable(err));
        CHECK(r.read(1, &d, &err) && d == "{\"b\":2}", "the next entry: %s", qPrintable(err));
    }
}

static void limitsBudget(const QString& vanilla, int mc, const QString& work)
{
    const long rss0 = maxRssMiB();
    // C3: one JSON file above the per-file limit, in a zip and in a folder
    {
        ZipBuild z;
        z.add("pack.mcmeta", kMeta);
        z.add("data/limits/worldgen/density_function/big.json", "1.0" + QByteArray(65 << 20, ' '));
        const QString path = work + "/limits-bigfile.zip";
        writeFile(path, z.finish());
        dp::Report report;
        loadWorld(mc, vanilla, QStringList() << path, "minecraft:normal", &report, false);
        CHECK(errorNaming(report, "limits-bigfile.zip", "limits:big", "too large"), "a 65 MiB JSON file is read");
        TestPack f(work + "/limits-bigfolder");
        f.file("pack.mcmeta", kMeta);
        f.file("data/limits/worldgen/density_function/big.json", "1.0" + QByteArray(65 << 20, ' '));
        dp::Report r2;
        loadWorld(mc, vanilla, QStringList() << f.dir, "minecraft:normal", &r2, false);
        CHECK(errorNaming(r2, "limits-bigfolder", "limits:big", "too large"), "a 65 MiB JSON file of a folder is read");
    }
    // C3: files that together unpack to more than the budget of a pack
    {
        ZipBuild z;
        z.add("pack.mcmeta", kMeta);
        const QByteArray big = "1.0" + QByteArray(60 << 20, ' ');
        const QByteArray packed = rawDeflate(big);
        for (int i = 0; i < 10; i++)
        {
            ZipBuild::Entry e;
            e.name = "data/limits/worldgen/density_function/f" + QByteArray::number(i) + ".json";
            e.crc = dp::crc32((const unsigned char*) big.constData(), (size_t) big.size());
            e.size = (uint64_t) big.size();
            e.deflated = true;
            e.stored = packed;
            z.addRaw(e);
        }
        const QString path = work + "/limits-budget.zip";
        writeFile(path, z.finish());
        dp::Report report;
        loadWorld(mc, vanilla, QStringList() << path, "minecraft:normal", &report, false);
        CHECK(errorNaming(report, "limits-budget.zip", QString(), "more than 512 MiB"),
            "10 files of 60 MiB are read without an error");
    }
    printf("JSON above 64 MiB, packs above 512 MiB: errors; peak RSS %ld -> %ld MiB\n", rss0, maxRssMiB());
}

static void limitsTruncated(const QString& work)
{
    // P2: the archive gets shorter while it is open: errors, no SIGBUS
    ZipBuild z;
    std::vector<QByteArray> datas;
    std::mt19937 rng(5);
    for (int i = 0; i < 50; i++)
    {
        QByteArray d;
        for (int k = 0; k < 65536; k++)
            d.append((char) ('a' + rng() % 20));
        datas.push_back(d);
        z.add("e" + QByteArray::number(i), d);
    }
    const QString path = work + "/limits-truncated.zip";
    const QByteArray all = z.finish();
    writeFile(path, all);
    dp::ZipReader r;
    QString err;
    CHECK(r.open(path, &err), "the archive does not open: %s", qPrintable(err));
    CHECK(QFile::resize(path, all.size() / 3), "cannot truncate");
    int good = 0, failed = 0;
    for (size_t i = 0; i < r.entries().size(); i++)
    {
        QByteArray d;
        if (r.read(i, &d, &err))
        {
            CHECK(d == datas[(size_t) r.entries()[i].name.mid(1).toInt()], "wrong data from a cut off archive");
            good++;
        }
        else
            failed++;
    }
    CHECK(good > 0 && failed > 0, "truncated: %d read, %d failed", good, failed);
    printf("an archive cut off while it is open: %d entries read, %d failed (no SIGBUS)\n", good, failed);
}

static void limitsCrc()
{
    // C9: the CRC table is built once, also when several threads ask first
    const QByteArray text = "123456789";
    std::atomic_int wrong(0);
    std::vector<std::unique_ptr<QThread>> ts;
    std::atomic_bool go(false);
    for (int i = 0; i < 8; i++)
    {
        ts.emplace_back(QThread::create([&] {
            while (!go.load())
                ;
            if (dp::crc32((const unsigned char*) text.constData(), 9) != 0xcbf43926u)
                wrong++;
        }));
        ts.back()->start();
    }
    go = true;
    for (auto& t : ts)
        t->wait();
    CHECK(wrong == 0, "%d threads computed a wrong CRC", wrong.load());
    printf("crc32 from 8 threads at once: %s\n", wrong == 0 ? "right" : "WRONG");
}

static void limitsNesting(const QString& vanilla, int mc, const QString& work)
{
    const int limit = 256;
    const bool terrain = mc >= MC_1_19_2 && mc <= MC_26_2;
    auto deepPack = [&](const char *name, int jsonDepth, int chain, int splines) {
        TestPack p(work + "/" + name);
        p.file("pack.mcmeta", kMeta);
        const QString df = "data/limits/worldgen/density_function/";
        QByteArray leaf = gradient(mc);
        if (chain > 0)
        {
            for (int i = 0; i < chain; i++)
                p.file(df + QString("c%1.json").arg(i), i + 1 < chain
                    ? absOf(mc, "\"limits:c" + QByteArray::number(i + 1) + "\"")
                    : gradient(mc));
            leaf = "\"limits:c0\"";
        }
        if (splines > 0)
        {
            QByteArray sp = "0.5";
            for (int i = 0; i < splines; i++)
                sp = "{\"coordinate\":" + gradient(mc) + ",\"points\":[{\"location\":0,\"derivative\":0,"
                    "\"value\":" + sp + "},{\"location\":1,\"derivative\":0,\"value\":1}]}";
            // 1.18.2 clamps the spline to min_value .. max_value
            leaf = "{\"type\":\"minecraft:spline\",\"spline\":" + sp +
                (mc <= MC_1_18_2 ? ",\"min_value\":-10,\"max_value\":10}" : "}");
        }
        p.file(df + "deep.json", nested(mc, jsonDepth, leaf));
        p.file("data/limits/dimension/deep.json", customDim(mc, "\"limits:deep\""));
        return p.dir;
    };
    struct Case { const char *name; int json, chain, splines; bool ok; const char *where; };
    const Case cases[] = {
        { "limits-nest-250", 250, 0, 0, true, "" },
        { "limits-nest-600", 600, 0, 0, false, "limits:deep" },
        { "limits-chain-120", 0, 120, 0, true, "" },
        { "limits-chain-3000", 0, 3000, 0, false, "limits:c" },
        { "limits-spline-100", 0, 0, 100, true, "" },
        { "limits-spline-300", 0, 0, 300, false, "limits:deep" },
        // a reference compiled before (depth 1) reused deep down: the graph
        // is higher than any compile depth
        { "limits-reuse", 140, 120, 0, false, "limits:deep" },
    };
    for (const Case& c : cases)
    {
        const QString dir = deepPack(c.name, c.json, c.chain, c.splines);
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadOnThread(mc, vanilla, QStringList() << dir, &report);
        const int dim = w->dimension("limits:deep");
        bool named = false;
        for (const dp::Issue& i : report.issues)
            if (i.severity == dp::Issue::Error && i.pack == c.name && i.id.startsWith(c.where) &&
                i.text.contains("nested too deeply"))
                named = true;
        int height = 0;
        for (int32_t h : w->d()->graph->heights)
            height = std::max(height, (int) h);
        if (c.ok)
        {
            CHECK(report.ok() && dim >= 0 && w->dimensions()[(size_t) dim].supported, "%s: does not load", c.name);
            if (!report.ok())
                printReport(report);
            CHECK(height <= limit, "%s: graph height %d", c.name, height);
            if (terrain && dim >= 0)
                CHECK(heightsOnThread(w, dim), "%s: no heights", c.name);
        }
        else
        {
            CHECK(named, "%s: no error naming the pack and %s", c.name, c.where);
            if (!named)
                printReport(report);
        }
        printf("%-20s %s (graph height %d)\n", c.name, c.ok ? "loads" : named ? "refused with an error" : "NOT REFUSED", height);
    }
}

static void limitsTags(const QString& vanilla, int mc, const QString& work)
{
    // C2: a chain of 3000 nested biome tags, used by a structure and by a
    // checkerboard source
    TestPack p(work + "/limits-tags");
    p.file("pack.mcmeta", kMeta);
    const int n = 3000;
    for (int i = 0; i < n; i++)
        p.file(QString("data/limits/tags/worldgen/biome/t%1.json").arg(i), i + 1 < n
            ? "{\"values\":[\"#limits:t" + QByteArray::number(i + 1) + "\"]}"
            : QByteArray("{\"values\":[\"minecraft:plains\"]}"));
    p.file("data/limits/dimension/checker.json", customDim(mc, "0.0", kType, QByteArray(),
        "{\"type\":\"minecraft:checkerboard\",\"scale\":2,\"biomes\":\"#limits:t0\"}"));
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadOnThread(mc, vanilla, QStringList() << p.dir, &report);
    CHECK(errorNaming(report, "limits-tags", "limits:checker", "levels deep"), "a deep tag in a dimension: no error");
    // structures are only loaded without errors: a pack of its own
    QDir(p.dir + "/data/limits/dimension").removeRecursively();
    p.file("data/limits/worldgen/structure/tagged.json", "{\"type\":\"minecraft:jigsaw\",\"biomes\":\"#limits:t0\","
        "\"step\":\"surface_structures\",\"spawn_overrides\":{},\"size\":1,\"start_height\":{\"absolute\":0},"
        "\"max_distance_from_center\":32,\"start_pool\":\"minecraft:village/plains/town_centers\"}");
    dp::Report r2;
    loadOnThread(mc, vanilla, QStringList() << p.dir, &r2);
    if (mc >= MC_1_19_2 && mc <= MC_26_2)
        CHECK(hasIssue(r2, dp::Issue::Warning, "limits-tags", "limits:tagged", "levels deep"),
            "a deep tag in a structure: no warning");
    printf("3000 nested biome tags: refused with a message\n");
}

static void limitsTagFan(const QString& vanilla, int mc, const QString& work)
{
    // every tag lists the next one twice: 2^40 paths, one set of biomes
    TestPack p(work + "/limits-tagfan");
    p.file("pack.mcmeta", kMeta);
    for (int i = 0; i < 40; i++)
        p.file(QString("data/limits/tags/worldgen/biome/f%1.json").arg(i), i + 1 < 40
            ? "{\"values\":[\"#limits:f" + QByteArray::number(i + 1) + "\",\"minecraft:desert\",\"#limits:f" +
              QByteArray::number(i + 1) + "\"]}"
            : QByteArray("{\"values\":[\"minecraft:plains\"]}"));
    p.file("data/limits/dimension/checker.json", customDim(mc, "0.0", kType, QByteArray(),
        "{\"type\":\"minecraft:checkerboard\",\"scale\":2,\"biomes\":\"#limits:f0\"}"));
    QElapsedTimer t;
    t.start();
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadOnThread(mc, vanilla, QStringList() << p.dir, &report);
    const int dim = w->dimension("limits:checker");
    CHECK(dim >= 0 && w->dimensions()[(size_t) dim].biomes.size() == 2, "fanned tags: wrong biomes");
    CHECK(t.elapsed() < 20000, "fanned tags take %lld ms", (long long) t.elapsed());
    printf("40 tags that list the next one twice: %lld ms\n", (long long) t.elapsed());
}

static void limitsIntervalSelect(const QString& vanilla, int mc, const QString& work)
{
    if (mc < MC_26_2)
        return;
    // the choice of the tree against a linear search, with equal thresholds
    {
        dp::Resources res;
        dp::Report report;
        dp::DensityGraph g(res, &report, mc);
        std::mt19937 rng(9);
        std::vector<double> th;
        for (int i = 0; i < 1000; i++)
            th.push_back((double) (rng() % 1200) - 100.0 + ((rng() & 1) ? 0.5 : 0.0));
        std::sort(th.begin(), th.end());
        QByteArray ts, fs;
        for (size_t i = 0; i < th.size(); i++)
            ts += (i ? "," : "") + QByteArray::number(th[i], 'g', 17);
        for (size_t i = 0; i <= th.size(); i++)
            fs += (i ? "," : "") + QByteArray::number((int) i);
        auto build = [&](const QByteArray& input) {
            QByteArray js = "{\"type\":\"minecraft:interval_select\",\"input\":" + input + ",\"thresholds\":[" + ts +
                "],\"functions\":[" + fs + "]}";
            QJsonValue v;
            QString err;
            dp::parseJson(js, &v, &err);
            return g.compile(v, -1, "limits:isel");
        };
        // input about y (the reference takes the input's value)
        const QByteArray in = gradient(mc, -2000, 2000, -2000.0, 2000.0);
        const int node = build(in);
        QJsonValue iv;
        QString ierr;
        dp::parseJson(in, &iv, &ierr);
        const int inNode = g.compile(iv, -1, "limits:input");
        CHECK(node >= 0 && inNode >= 0, "interval_select does not compile");
        int height = 0;
        for (int32_t h : g.heights)
            height = std::max(height, (int) h);
        CHECK(height <= 20, "interval_select of 1000 thresholds is %d nodes high", height);
        dp::DensityProgram prog;
        prog.build(g, std::vector<int>{ node, inNode });
        dp::DensityState st(g, prog, 1, mc);
        st.evalColumn(0, 0);
        int bad = 0;
        for (int y = -150; y <= 1150; y++)
        {
            st.evalVertical(y);
            const double input = st.value(inNode);
            size_t want = th.size();
            for (size_t i = 0; i < th.size(); i++)
                if (input < (mc >= MC_26_3 ? (double) (float) th[i] : th[i])) { want = i; break; }
            if (st.value(node) != (double) want)
                bad++;
        }
        CHECK(bad == 0, "interval_select: %d wrong choices", bad);
        if (mc < MC_26_3)
        {
            // NaN takes the last function: inf * 0
            const int nan = build("{\"type\":\"minecraft:mul\",\"argument1\":{\"type\":\"minecraft:add\","
                "\"argument1\":1e308,\"argument2\":1e308},\"argument2\":{\"type\":\"minecraft:y_clamped_gradient\","
                "\"from_y\":0,\"to_y\":10,\"from_value\":0,\"to_value\":0}}");
            dp::DensityProgram p2;
            p2.build(g, std::vector<int>{ nan });
            dp::DensityState s2(g, p2, 1, mc);
            s2.evalColumn(0, 0);
            s2.evalVertical(5);
            CHECK(s2.value(nan) == (double) th.size(), "interval_select of NaN: %g", s2.value(nan));
        }
        printf("interval_select of 1000 ordered thresholds: %d wrong of 1301, tree height %d\n", bad, height);
    }
    // C5: 100,000 thresholds in final_density: loads, heights on the app's stack
    {
        TestPack p(work + "/limits-isel");
        p.file("pack.mcmeta", kMeta);
        // all thresholds below the input (-1 .. 1): every sample takes the
        // last function (a chain of choices would be walked to its end)
        QByteArray ts, fs;
        for (int i = 0; i < 100000; i++)
            ts += (i ? "," : "") + QByteArray::number(-3.0 + i / 100000.0, 'g', 17);
        for (int i = 0; i <= 100000; i++)
            fs += (i ? "," : "") + (i < 100000 ? QByteArray("-1") : mc >= MC_26_3 ? gradient(mc) :
                "{\"type\":\"minecraft:add\",\"argument1\":-0.25,\"argument2\":" + gradient(mc) + "}");
        p.file("data/limits/worldgen/density_function/isel.json", "{\"type\":\"minecraft:interval_select\",\"input\":" +
            gradient(mc) + ",\"thresholds\":[" + ts + "],\"functions\":[" + fs + "]}");
        p.file("data/limits/dimension/isel.json", customDim(mc, "\"limits:isel\""));
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadOnThread(mc, vanilla, QStringList() << p.dir, &report);
        const int dim = w->dimension("limits:isel");
        CHECK(report.ok() && dim >= 0, "100,000 thresholds do not load");
        if (!report.ok())
            printReport(report);
        if (dim >= 0 && mc <= MC_26_2)
        {
            // the last function is positive below y 80 (the gradient
            // crosses 0.25 there)
            std::vector<int> h;
            CHECK(heightsOnThread(w, dim, &h), "100,000 thresholds: no heights");
            CHECK(h[0] == 79, "100,000 thresholds: height %d", h[0]);
        }
        printf("interval_select with 100,000 thresholds: loads%s\n", mc <= MC_26_2 ? ", heights on a 512 KiB thread" : "");
    }
}

static void limitsRanges(const QString& vanilla, int mc, const QString& work)
{
    // C4: dimension types and noise settings outside the game's codecs
    struct Case { const char *name; QByteArray type, noise; const char *what; };
    const QByteArray okNoise = mc >= MC_26_3 ? "{\"min_y\":-64,\"height\":384}"
        : "{\"min_y\":-64,\"height\":384,\"size_horizontal\":1,\"size_vertical\":2}";
    auto type = [](const char *minY, const char *height, const char *logical = "384") {
        return QByteArray("{\"min_y\":") + minY + ",\"height\":" + height + ",\"logical_height\":" + logical + "}";
    };
    auto noise = [&](const char *minY, const char *height, const char *sh = "1", const char *sv = "2") {
        if (mc >= MC_26_3)
            return QByteArray("{\"min_y\":") + minY + ",\"height\":" + height + "}";
        return QByteArray("{\"min_y\":") + minY + ",\"height\":" + height + ",\"size_horizontal\":" + sh +
            ",\"size_vertical\":" + sv + "}";
    };
    std::vector<Case> cases = {
        { "tall", type("-64", "1000000000", "384"), okNoise, "dimension type" },
        { "huge", type("-64", "1e12", "384"), okNoise, "dimension type" },
        { "not16", type("-64", "392", "384"), okNoise, "dimension type" },
        { "miny8", type("8", "384", "384"), okNoise, "dimension type" },
        { "low", type("-2048", "384", "384"), okNoise, "dimension type" },
        { "over", type("2000", "64", "64"), okNoise, "dimension type" },
        { "logical", type("-64", "384", "500"), okNoise, "dimension type" },
        { "noiseheight", type("-64", "384"), noise("-64", "1e12"), "noise settings" },
        { "noiseminy", type("-64", "384"), noise("-56", "384"), "noise settings" },
        { "noiseover", type("-64", "384"), noise("1024", "2048"), "noise settings" },
    };
    if (mc < MC_26_3)
    {
        cases.push_back({ "size0", type("-64", "384"), noise("-64", "384", "0"), "noise settings" });
        cases.push_back({ "size5", type("-64", "384"), noise("-64", "384", "1", "5"), "noise settings" });
        // C7: 1.18.2 divides by size_vertical * 4
        cases.push_back({ "sizehuge", type("-64", "384"), noise("-64", "384", "1", "1073741824"), "noise settings" });
    }
    TestPack p(work + "/limits-ranges");
    p.file("pack.mcmeta", kMeta);
    for (const Case& c : cases)
        p.file(QString("data/limits/dimension/%1.json").arg(c.name), customDim(mc, gradient(mc), c.type, c.noise));
    if (mc < MC_26_3)
        p.file("data/limits/worldgen/noise_settings/unused.json", noiseSettings(mc, "0.0",
            noise("-64", "384", "1", "1073741824")));
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadOnThread(mc, vanilla, QStringList() << p.dir, &report);
    int refused = 0;
    for (const Case& c : cases)
    {
        const QString id = QString("limits:%1").arg(c.name);
        const int dim = w->dimension(id);
        const bool err = errorNaming(report, "limits-ranges", id, QString(c.what) + " are invalid") ||
            errorNaming(report, "limits-ranges", id, QString(c.what) + " is invalid");
        CHECK(err && dim >= 0 && !w->dimensions()[(size_t) dim].supported, "%s: no error about the %s", c.name, c.what);
        refused += err;
    }
    if (mc < MC_26_3)
        CHECK(errorNaming(report, "limits-ranges", "limits:unused", "noise settings are invalid"),
            "unused noise settings with size_vertical 1073741824: no error");
    // the widest range the game takes (a world with errors has no supported
    // dimension: a pack of its own)
    TestPack pw(work + "/limits-widest");
    pw.file("pack.mcmeta", kMeta);
    pw.file("data/limits/dimension/widest.json", customDim(mc, gradient(mc), type("-2032", "4064", "4064"),
        noise("-2032", "4064")));
    dp::Report rw;
    w = loadOnThread(mc, vanilla, QStringList() << pw.dir, &rw);
    const int widest = w->dimension("limits:widest");
    CHECK(rw.ok() && widest >= 0 && w->dimensions()[(size_t) widest].supported, "the widest valid range is refused");
    if (!rw.ok())
        printReport(rw);
    if (widest >= 0)
    {
        QElapsedTimer t;
        t.start();
        dp::Sampler s(w, widest, 1);
        std::vector<int> b(64 * 64);
        CHECK(s.genBiomes(b.data(), 4, 0, 0, 64, 64, 0), "the widest range: no biomes");
        CHECK(t.elapsed() < 5000, "the widest range takes %lld ms", (long long) t.elapsed());
    }
    printf("dimension types and noise settings outside the game's ranges: %d of %zu refused\n", refused, cases.size());
}

static void limitsChecker(const QString& vanilla, int mc, const QString& work)
{
    // C8: CheckerboardColumnBiomeSource shifts Java ints (the count mod 32)
    TestPack p(work + "/limits-checker");
    p.file("pack.mcmeta", kMeta);
    const int scales[] = { 0, 2, 29, 30, 31, 45, 61, 62 };
    for (int sc : scales)
        p.file(QString("data/limits/dimension/c%1.json").arg(sc), customDim(mc, "0.0", kType, QByteArray(),
            "{\"type\":\"minecraft:checkerboard\",\"scale\":" + QByteArray::number(sc) +
            ",\"biomes\":[\"minecraft:plains\",\"minecraft:desert\",\"minecraft:forest\"]}"));
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList() << p.dir, "minecraft:normal", &report, false);
    {
        TestPack bad(work + "/limits-checker63");
        bad.file("pack.mcmeta", kMeta);
        bad.file("data/limits/dimension/c63.json", customDim(mc, "0.0", kType, QByteArray(),
            "{\"type\":\"minecraft:checkerboard\",\"scale\":63,\"biomes\":[\"minecraft:plains\"]}"));
        dp::Report r2;
        loadWorld(mc, vanilla, QStringList() << bad.dir, "minecraft:normal", &r2, false);
        CHECK(errorNaming(r2, "limits-checker63", "limits:c63", "scale"), "checkerboard scale 63: no error");
    }
    std::mt19937 rng(3);
    int bad = 0, total = 0;
    for (int sc : scales)
    {
        const int dim = w->dimension(QString("limits:c%1").arg(sc));
        CHECK(dim >= 0 && w->dimensions()[(size_t) dim].supported, "checkerboard scale %d not loaded", sc);
        if (dim < 0)
            continue;
        const std::vector<QString> ids = { "minecraft:plains", "minecraft:desert", "minecraft:forest" };
        dp::Sampler s(w, dim, 1);
        for (int t = 0; t < 200; t++)
        {
            // quart positions within +-30 million blocks
            const int qx = (int) (rng() % 15000000) - 7500000, qz = (int) (rng() % 15000000) - 7500000;
            int b = -1;
            s.genBiomes(&b, 4, qx, qz, 1, 1, 64);
            const int sh = (sc + 2) & 31;
            const int32_t v = (int32_t) ((uint32_t) (qx >> sh) + (uint32_t) (qz >> sh));
            const int want = ((v % 3) + 3) % 3;
            total++;
            if (b < 0 || w->biomes()[(size_t) b].id != ids[(size_t) want])
                bad++;
        }
    }
    CHECK(bad == 0, "checkerboard: %d of %d cells differ from the game's formula", bad, total);
    printf("checkerboard scales 0 .. 62: %d of %d cells differ\n", bad, total);
}

static void limitsFindTopSurface(const QString& vanilla, int mc, const QString& work)
{
    if (mc < MC_1_21_9)
        return;
    // the game's arithmetic: Mth.floor(upper / cell) * cell in ints
    {
        dp::Resources res;
        dp::Report report;
        dp::DensityGraph g(res, &report, mc);
        auto value = [&](const char *upper, int cell) {
            QByteArray js = "{\"type\":\"minecraft:find_top_surface\",\"density\":" + gradient(mc) +
                ",\"upper_bound\":" + upper + ",\"lower_bound\":-64,\"cell_height\":" + QByteArray::number(cell) + "}";
            QJsonValue v;
            QString err;
            dp::parseJson(js, &v, &err);
            const int node = g.compile(v, -1, "limits:fts");
            if (node < 0)
                return std::nan("");
            dp::DensityProgram prog;
            prog.build(g, std::vector<int>{ node });
            dp::DensityState st(g, prog, 1, mc);
            st.evalColumn(0, 0);
            st.evalVertical(0);
            return st.value(node);
        };
        // the gradient is positive below y 128
        CHECK(value("100.7", 4) == 100.0, "find_top_surface 100.7 / 4: %g", value("100.7", 4));
        if (mc < MC_26_3)
        {
            // -1e20 / 2: d2i saturates; before 26.1 INT_MIN - 1 wraps to
            // INT_MAX, times 2 wraps to -2; since 26.1 INT_MIN times 2 is 0
            const double want = mc >= MC_26_1 ? 0.0 : -2.0;
            CHECK(value("-1e20", 2) == want, "find_top_surface -1e20 / 2: %g, the game %g", value("-1e20", 2), want);
        }
        printf("find_top_surface: the game's int arithmetic\n");
    }
    // the range of lower_bound; an upper bound far above the world (2^31
    // steps): the scan stops when the caller aborts
    TestPack p(work + "/limits-fts");
    p.file("pack.mcmeta", kMeta);
    p.file("data/limits/worldgen/density_function/far.json", "{\"type\":\"minecraft:find_top_surface\",\"density\":" +
        gradient(mc) + ",\"upper_bound\":1e20,\"lower_bound\":-64,\"cell_height\":1}");
    p.file("data/limits/dimension/far.json", customDim(mc, "\"limits:far\""));
    // a structure on the terrain: its height check scans too
    p.file("data/limits/worldgen/template_pool/one.json", "{\"fallback\":\"minecraft:empty\",\"elements\":["
        "{\"weight\":1,\"element\":{\"element_type\":\"minecraft:feature_pool_element\",\"feature\":"
        "\"minecraft:oak\",\"projection\":\"rigid\"}}]}");
    p.file("data/limits/worldgen/structure/onterrain.json", "{\"type\":\"minecraft:jigsaw\",\"biomes\":"
        "\"#minecraft:is_overworld\",\"step\":\"surface_structures\",\"spawn_overrides\":{},\"size\":1,"
        "\"max_distance_from_center\":32,\"start_pool\":\"limits:one\",\"start_height\":{\"absolute\":0},"
        "\"project_start_to_heightmap\":\"WORLD_SURFACE_WG\"}");
    p.file("data/limits/worldgen/structure_set/onterrain.json", "{\"placement\":{\"type\":\"minecraft:random_spread\","
        "\"salt\":7,\"spacing\":2,\"separation\":1},\"structures\":[{\"structure\":\"limits:onterrain\",\"weight\":1}]}");
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList() << p.dir, "minecraft:normal", &report, false);
    // 26.3 takes constants within +-1000000 only
    CHECK(report.ok() == (mc < MC_26_3), "find_top_surface from 1e20: errors %d", (int) !report.ok());
    {
        TestPack low(work + "/limits-ftslow");
        low.file("pack.mcmeta", kMeta);
        low.file("data/limits/worldgen/density_function/low.json", "{\"type\":\"minecraft:find_top_surface\","
            "\"density\":" + gradient(mc) + ",\"upper_bound\":100,\"lower_bound\":-5000,\"cell_height\":1}");
        dp::Report r2;
        loadWorld(mc, vanilla, QStringList() << low.dir, "minecraft:normal", &r2, false);
        CHECK(errorNaming(r2, "limits-ftslow", "limits:low", "lower_bound"), "lower_bound -5000: no error");
    }
    const int dim = w->dimension("limits:far");
    if (dim >= 0 && mc <= MC_26_2)
    {
        std::atomic_bool abort(false);
        std::unique_ptr<QThread> stopper(QThread::create([&] {
            QThread::msleep(300);
            abort = true;
        }));
        QElapsedTimer t;
        t.start();
        stopper->start();
        dp::Sampler s(w, dim, 1);
        std::vector<int> h(16 * 16);
        const bool ok = s.genHeights(h.data(), 1, 0, 0, 16, 16, dp::HEIGHT_SURFACE, 0x7fffffff, &abort);
        const qint64 ms = t.elapsed();
        stopper->wait();
        CHECK(!ok && ms < 5000, "an aborted scan: ok %d after %lld ms", (int) ok, (long long) ms);
        printf("find_top_surface from 1e20: aborted after %lld ms\n", (long long) ms);

        // the same scan inside StructureFinder::starts() (the height of a
        // start projected to the terrain)
        std::atomic_bool abort2(false);
        std::unique_ptr<QThread> stopper2(QThread::create([&] {
            QThread::msleep(300);
            abort2 = true;
        }));
        t.restart();
        stopper2->start();
        dp::StructureFinder f(w, dim, 1);
        std::vector<dp::StructureStart> out;
        const bool ok2 = f.starts(-4, -4, 4, 4, &out, &abort2);
        const qint64 ms2 = t.elapsed();
        stopper2->wait();
        CHECK(f.valid() && !ok2 && ms2 < 5000, "an aborted structure search: valid %d, ok %d after %lld ms",
            (int) f.valid(), (int) ok2, (long long) ms2);
        printf("find_top_surface from 1e20 in a structure's height check: aborted after %lld ms\n", (long long) ms2);
    }
    // the same scan as the preliminary surface of the aquifers (the
    // router's preliminary_surface_level)
    if (mc <= MC_26_2)
    {
        TestPack aq(work + "/limits-ftsaq");
        aq.file("pack.mcmeta", kMeta);
        aq.file("data/limits/worldgen/density_function/far.json", "{\"type\":\"minecraft:find_top_surface\",\"density\":" +
            gradient(mc) + ",\"upper_bound\":1e20,\"lower_bound\":-64,\"cell_height\":1}");
        QByteArray settings = noiseSettings(mc, gradient(mc, -64, 320, 1.0, -1.0));
        settings.replace("\"aquifers_enabled\":false", "\"aquifers_enabled\":true");
        settings.replace("\"noise_router\":{", "\"noise_router\":{\"barrier\":0,\"fluid_level_floodedness\":0,"
            "\"fluid_level_spread\":0,\"lava\":0,\"erosion\":0,\"depth\":0,"
            "\"preliminary_surface_level\":\"limits:far\",");
        aq.file("data/limits/dimension/faraq.json", "{\"type\":" + QByteArray(kType) + ",\"generator\":{\"type\":"
            "\"minecraft:noise\",\"settings\":" + settings + ",\"biome_source\":{\"type\":\"minecraft:fixed\","
            "\"biome\":\"minecraft:plains\"}}}");
        dp::Report ra;
        std::shared_ptr<const dp::World> wa = loadWorld(mc, vanilla, QStringList() << aq.dir, "minecraft:normal", &ra,
            false);
        const int da = wa->dimension("limits:faraq");
        CHECK(ra.ok() && da >= 0 && wa->dimensions()[da].terrain && wa->dimensions()[da].aquiferReason.isEmpty(),
            "find_top_surface in the aquifers: the dimension (aquifers modelled)");
        if (da >= 0)
        {
            std::atomic_bool abort3(false);
            std::unique_ptr<QThread> stopper3(QThread::create([&] {
                QThread::msleep(300);
                abort3 = true;
            }));
            QElapsedTimer t3;
            t3.start();
            stopper3->start();
            dp::Sampler s(wa, da, 1);
            std::vector<int> h(16 * 16);
            const bool ok3 = s.genHeights(h.data(), 1, 0, 0, 16, 16, dp::HEIGHT_SURFACE, 0x7fffffff, &abort3);
            const qint64 ms3 = t3.elapsed();
            stopper3->wait();
            CHECK(!ok3 && ms3 < 5000, "an aborted aquifer scan: ok %d after %lld ms", (int) ok3, (long long) ms3);
            printf("find_top_surface from 1e20 as the aquifers' preliminary surface: aborted after %lld ms\n",
                (long long) ms3);
        }
    }
}

// @padding: only the dimension_padding structures (C11), else only the
// codec ranges (C6)
static void limitsStructures(const QString& vanilla, int mc, const QString& work, bool padding)
{
    if (mc < MC_1_19_2 || mc > MC_26_2)
        return;
    const long rss0 = maxRssMiB();
    TestPack p(work + "/limits-struct");
    p.file("pack.mcmeta", kMeta);
    const QString base = "data/limits/worldgen/";
    auto pool = [&](const char *name, const QByteArray& weights) {
        QByteArray es;
        for (const QByteArray& wt : weights.split(','))
            es += QByteArray(es.isEmpty() ? "" : ",") + "{\"weight\":" + wt + ",\"element\":{\"element_type\":"
                "\"minecraft:feature_pool_element\",\"feature\":\"minecraft:oak\",\"projection\":\"rigid\"}}";
        p.file(base + "template_pool/" + name + ".json", "{\"fallback\":\"minecraft:empty\",\"elements\":[" + es + "]}");
    };
    // C6: weights outside 1 .. 150 (a weight of 50 million expanded to a
    // list took 200 MB)
    pool("heavy", "50000000");
    pool("zero", "0");
    pool("over", "151");
    pool("fine", "1,150,7");
    int salt = 100;
    auto structure = [&](const char *name, const char *poolId, const QByteArray& extra) {
        p.file(base + "structure/" + name + ".json", "{\"type\":\"minecraft:jigsaw\",\"biomes\":\"#minecraft:is_overworld\","
            "\"step\":\"surface_structures\",\"spawn_overrides\":{},\"size\":1,\"max_distance_from_center\":32,"
            "\"start_pool\":\"limits:" + QByteArray(poolId) + "\"" + extra + "}");
        p.file(base + "structure_set/" + name + ".json", "{\"placement\":{\"type\":\"minecraft:random_spread\","
            "\"salt\":" + QByteArray::number(salt++) + ",\"spacing\":4,\"separation\":1},"
            "\"structures\":[{\"structure\":\"limits:" + QByteArray(name) + "\",\"weight\":1}]}");
    };
    const QByteArray abs0 = ",\"start_height\":{\"absolute\":0}";
    structure("heavy", "heavy", abs0);
    structure("zero", "zero", abs0);
    structure("over", "over", abs0);
    structure("fine", "fine", abs0);
    if (padding)
    {
        QDir(p.dir + "/data/limits/worldgen/structure").removeRecursively();
        QDir(p.dir + "/data/limits/worldgen/structure_set").removeRecursively();
        structure("fine", "fine", abs0);
    }
    // placements outside the codecs
    auto set = [&](const char *name, const QByteArray& placement) {
        p.file(base + "structure_set/" + name + ".json", "{\"placement\":" + placement +
            ",\"structures\":[{\"structure\":\"limits:fine\",\"weight\":1}]}");
    };
    if (!padding)
    {
    set("rings_count", "{\"type\":\"minecraft:concentric_rings\",\"salt\":0,\"distance\":32,\"spread\":3,\"count\":2000000000}");
    set("rings_distance", "{\"type\":\"minecraft:concentric_rings\",\"salt\":0,\"distance\":1024,\"spread\":3,\"count\":3}");
    set("spacing", "{\"type\":\"minecraft:random_spread\",\"salt\":1,\"spacing\":5000,\"separation\":2}");
    set("separation", "{\"type\":\"minecraft:random_spread\",\"salt\":1,\"spacing\":40,\"separation\":4097}");
    set("frequency", "{\"type\":\"minecraft:random_spread\",\"salt\":1,\"spacing\":8,\"separation\":2,\"frequency\":1.5}");
    }
    // C11: dimension_padding
    if (padding)
    {
    structure("pad_bottom", "fine", ",\"start_height\":{\"absolute\":-60},\"dimension_padding\":10");
    structure("pad_none", "fine", ",\"start_height\":{\"absolute\":-60}");
    structure("pad_zero", "fine", ",\"start_height\":{\"absolute\":-64},\"dimension_padding\":0");
    structure("pad_top", "fine", ",\"start_height\":{\"absolute\":100},\"dimension_padding\":{\"bottom\":10,\"top\":300}");
    structure("pad_lenient", "fine", ",\"start_height\":{\"absolute\":100},\"dimension_padding\":{\"bottom\":-5}");
    structure("pad_bad", "fine", ",\"start_height\":{\"absolute\":100},\"dimension_padding\":-1");
    structure("pad_projected", "fine", ",\"start_height\":{\"absolute\":0},\"project_start_to_heightmap\":"
        "\"WORLD_SURFACE_WG\",\"dimension_padding\":{\"top\":400}");
    }

    dp::Report report;
    std::shared_ptr<const dp::World> w = loadOnThread(mc, vanilla, QStringList() << p.dir, &report);
    const long rss1 = maxRssMiB();
    CHECK(report.ok(), "structure problems are errors");
    CHECK(rss1 - rss0 < kRssSlack, "loading took %ld MiB more", rss1 - rss0);
    auto warned = [&](const char *id, const char *text) {
        CHECK(hasIssue(report, dp::Issue::Warning, "limits-struct", id, text), "%s: no warning \"%s\"", id, text);
    };
    std::map<QString, const dp::StructureInfo *> st;
    for (const dp::StructureInfo& s : w->structures())
        st[s.id] = &s;
    if (!padding)
    {
    warned("limits:heavy", "from 1 to 150");
    warned("limits:zero", "from 1 to 150");
    warned("limits:over", "from 1 to 150");
    warned("limits:rings_count", "count");
    warned("limits:rings_distance", "distance");
    warned("limits:spacing", "spacing");
    warned("limits:separation", "separation");
    warned("limits:frequency", "frequency");
    for (const char *id : { "limits:heavy", "limits:zero", "limits:over" })
        CHECK(st.count(id) && !st[id]->supported, "%s is supported", id);
    CHECK(st.count("limits:fine") && st["limits:fine"]->supported, "limits:fine is not supported");
    for (const dp::StructureSetInfo& s : w->structureSets())
        if (s.id == "limits:rings_count" || s.id == "limits:rings_distance" || s.id == "limits:spacing" ||
            s.id == "limits:separation" || s.id == "limits:frequency")
            CHECK(!s.supported, "%s is supported", qPrintable(s.id));
    printf("structure codec ranges: warnings; RSS +%ld MiB\n", rss1 - rss0);
    return;
    }
    warned("limits:pad_bad", "dimension_padding");
    CHECK(st.count("limits:pad_bad") && !st["limits:pad_bad"]->supported, "limits:pad_bad is supported");
    for (const char *id : { "limits:fine", "limits:pad_bottom", "limits:pad_none", "limits:pad_zero", "limits:pad_top",
        "limits:pad_lenient", "limits:pad_projected" })
        CHECK(st.count(id) && st[id]->supported, "%s is not supported", id);

    // the padding: checked since 1.21.4
    dp::StructureFinder f(w, w->dimension("minecraft:overworld"), 1234);
    std::vector<dp::StructureStart> out;
    CHECK(f.starts(-24, -24, 24, 24, &out), "the finder failed");
    std::map<QString, std::pair<int, int>> count;   // structure -> (invalid, other)
    for (const dp::StructureStart& s : out)
    {
        auto& c = count[w->structures()[(size_t) s.structure].id];
        (s.state == dp::StructureStart::Invalid ? c.first : c.second)++;
    }
    const bool padded = mc >= MC_1_21_4;
    auto allInvalid = [&](const char *id, bool want) {
        const auto& c = count[id];
        CHECK(c.first + c.second > 0, "%s: no candidates", id);
        CHECK((c.second == 0) == want, "%s: %d invalid, %d not (expected %s)", id, c.first, c.second,
            want ? "all invalid" : "some not invalid");
    };
    allInvalid("limits:pad_bottom", padded);
    allInvalid("limits:pad_none", false);
    allInvalid("limits:pad_zero", padded);
    allInvalid("limits:pad_top", padded);
    allInvalid("limits:pad_lenient", false);
    allInvalid("limits:pad_projected", padded);
    allInvalid("limits:fine", false);
    printf("dimension_padding %s\n", padded ? "checked" : "not checked (before 1.21.4)");
}

static void limitsInterpolated(const QString& vanilla, int mc, const QString& work)
{
    if (mc < MC_26_3)
        return;
    // P4: "interpolated" inside "interpolated" in a climate function costs 8
    // evaluations per level and sample: up to 4 levels are offered
    auto pack = [&](const char *name, int levels) {
        TestPack p(work + "/" + name);
        p.file("pack.mcmeta", kMeta);
        QByteArray f = "\"minecraft:overworld/continents\"";
        for (int i = 0; i < levels; i++)
            f = "{\"type\":\"minecraft:interpolated\",\"input\":" + f + ",\"cell_size_xz\":" +
                QByteArray::number(3 + 2 * i) + ",\"cell_size_y\":" + QByteArray::number(5 + 2 * i) + "}";
        p.file("data/minecraft/worldgen/density_function/overworld/temperature.json", f);
        return p.dir;
    };
    for (int levels : { 4, 7 })
    {
        const QString name = QString("limits-interp-%1").arg(levels);
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadWorld(mc, vanilla, QStringList() << pack(qPrintable(name), levels),
            "minecraft:normal", &report, false);
        const int ow = w->dimension("minecraft:overworld");
        CHECK(report.ok() && ow >= 0, "%s: errors", qPrintable(name));
        if (ow < 0)
            continue;
        const dp::DimensionInfo& info = w->dimensions()[(size_t) ow];
        if (levels <= 4)
        {
            CHECK(info.supported, "%s: not supported (%s)", qPrintable(name), qPrintable(info.reason));
            QElapsedTimer t;
            t.start();
            dp::Sampler s(w, ow, 1);
            std::vector<int> b(4 * 4);
            CHECK(s.genBiomes(b.data(), 4, 0, 0, 4, 4, 64), "%s: no biomes", qPrintable(name));
            printf("interpolated nested %d deep: 16 cells in %lld ms\n", levels, (long long) t.elapsed());
        }
        else
        {
            CHECK(!info.supported && info.reason.contains("interpolated"), "%s: offered (%s)", qPrintable(name),
                qPrintable(info.reason));
            printf("interpolated nested %d deep: %s\n", levels, qPrintable(info.reason));
            if (info.supported)
            {
                // the old cost: 8^7 evaluations per sample
                QElapsedTimer t;
                t.start();
                dp::Sampler s(w, ow, 1);
                std::vector<int> b(4 * 4);
                s.genBiomes(b.data(), 4, 0, 0, 4, 4, 64);
                printf("interpolated nested %d deep: 16 cells in %lld ms\n", levels, (long long) t.elapsed());
            }
        }
    }
}

static int cmdLimits(const QStringList& args)
{
    if (args.size() < 3)
    {
        printf("usage: dptest limits <vanilla data> <mc> <work dir> [case ...]\n"
            "cases: zip budget truncated crc nesting tags tagfan isel ranges checker fts structures padding interp\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const QString vanilla = args[0];
    const int mc = str2mc(qPrintable(args[1]));
    const QString work = QDir(args[2]).filePath("limits");
    QDir().mkpath(work);
    QStringList cases = args.mid(3);
    if (cases.isEmpty())
        cases = QStringList({ "crc", "zip", "budget", "truncated", "nesting", "tags", "tagfan", "isel", "ranges",
            "checker", "fts", "structures", "padding", "interp" });
    for (const QString& c : cases)
    {
        printf("-- %s\n", qPrintable(c));
        fflush(stdout);
        if (c == "zip") limitsZip(work);
        else if (c == "budget") limitsBudget(vanilla, mc, work);
        else if (c == "truncated") limitsTruncated(work);
        else if (c == "crc") limitsCrc();
        else if (c == "nesting") limitsNesting(vanilla, mc, work);
        else if (c == "tags") limitsTags(vanilla, mc, work);
        else if (c == "tagfan") limitsTagFan(vanilla, mc, work);
        else if (c == "isel") limitsIntervalSelect(vanilla, mc, work);
        else if (c == "ranges") limitsRanges(vanilla, mc, work);
        else if (c == "checker") limitsChecker(vanilla, mc, work);
        else if (c == "fts") limitsFindTopSurface(vanilla, mc, work);
        else if (c == "structures") limitsStructures(vanilla, mc, work, false);
        else if (c == "padding") limitsStructures(vanilla, mc, work, true);
        else if (c == "interp") limitsInterpolated(vanilla, mc, work);
        else
        {
            printf("unknown case %s\n", qPrintable(c));
            return 2;
        }
        fflush(stdout);
    }
    QDir(work).removeRecursively();
    return g_fail != 0;
}

//==============================================================================
// graphdepth: the deepest density function of a world
//==============================================================================

static int cmdGraphDepth(const QStringList& args)
{
    if (args.size() < 2)
    {
        printf("usage: dptest graphdepth <vanilla data> <mc> [pack ...]\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    int worst = 0, deepestCompile = 0;
    for (const char *preset : { "minecraft:normal", "minecraft:large_biomes", "minecraft:amplified" })
    {
        dp::Report report;
        std::shared_ptr<const dp::World> w = loadWorld(str2mc(qPrintable(args[1])), args[0], args.mid(2), preset,
            &report, false);
        const dp::DensityGraph& g = *w->d()->graph;
        int h = 0, nest = 0;
        for (int32_t v : g.heights)
            h = std::max(h, (int) v);
        for (int32_t v : g.interpNesting)
            nest = std::max(nest, (int) v);
        if (nest > 1)
            printf("interpolated nested %d deep\n", nest);
        printf("%s: %zu nodes, height %d, compiler depth %d, %zu issues%s\n", preset, g.nodes.size(), h, g.deepest,
            report.issues.size(), report.ok() ? "" : " (errors)");
        worst = std::max(worst, h);
        deepestCompile = std::max(deepestCompile, g.deepest);
    }
    printf("deepest: %d (limit %d), compiler depth %d (limit %d)\n", worst, dp::DensityGraph::MAX_DEPTH, deepestCompile,
        dp::DensityGraph::MAX_COMPILE_DEPTH);
    return 0;
}

//==============================================================================
// areas: where a server should generate a dimension of a pack
//==============================================================================

// Picks areas for worldgen_reference.py (--worlds) in which the generator
// expects as many of the biomes of a dimension as possible, those of the
// packs first: squares of --size chunks on a grid of that size within
// --radius blocks of 0,0 (not overlapping the area "origin" of worlds.txt),
// sampled at 1:4 every 64 blocks at ten heights. The choice uses the
// generator under test, so it is only a guess; what the areas really hold is
// what the server's files say.
static int cmdAreas(const QStringList& argsIn)
{
    QStringList args = argsIn, packs;
    QString dimId = "minecraft:overworld", preset = "minecraft:normal";
    int radius = 16384, count = 4, size = 16;
    bool quiet = false;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--dimension" && i + 1 < args.size()) { dimId = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--preset" && i + 1 < args.size()) { preset = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--radius" && i + 1 < args.size()) { radius = args[i + 1].toInt(); args.remove(i, 2); }
        else if (args[i] == "--count" && i + 1 < args.size()) { count = args[i + 1].toInt(); args.remove(i, 2); }
        else if (args[i] == "--size" && i + 1 < args.size()) { size = args[i + 1].toInt(); args.remove(i, 2); }
        else if (args[i] == "--quiet") { quiet = true; args.removeAt(i); }
        else i++;
    }
    if (args.size() != 3 || size < 1 || size > 64 || radius < size * 16 || count < 0)
    {
        printf("usage: dptest areas <vanilla data> <mc> <seed> [--pack <pack>]... [--dimension <id>] [--preset <id>]\n"
            "           [--radius <blocks>] [--count <n>] [--size <chunks>] [--quiet]\n"
            "       prints a line of a worlds file of worldgen_reference.py: the area origin and up to n\n"
            "       areas of size x size chunks where the generator expects the most biomes not yet covered,\n"
            "       the packs' own first; --quiet: counts instead of biome ids\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    const uint64_t seed = (uint64_t) args[2].toLongLong();
    dp::Report report;
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, preset, &report, !quiet);
    const int dim = w->dimension(dimId);
    if (dim < 0 || !w->dimensions()[(size_t) dim].supported)
    {
        printf("dimension %s not supported\n", quiet ? "(of the packs)" : qPrintable(dimId));
        return 1;
    }
    const dp::DimensionInfo& info = w->dimensions()[(size_t) dim];
    std::vector<int> levels;
    for (int i = 0; i < 10; i++)
        levels.push_back(info.minY + 4 + (info.height - 8) * i / 9 / 4 * 4);

    // the biomes of every area of the grid
    const int span = size * 16, n = 2 * (radius / span);
    const int nb = (int) w->biomes().size();
    std::vector<std::vector<char>> has((size_t) n * n, std::vector<char>((size_t) nb, 0));
    std::atomic_int next(0);
    auto work = [&]() {
        dp::Sampler s(w, dim, seed);
        for (int row; (row = next++) < n; )
        {
            for (int col = 0; col < n; col++)
            {
                std::vector<char>& h = has[(size_t) row * n + col];
                const int x0 = (col - n / 2) * span, z0 = (row - n / 2) * span;
                for (int dz = 32; dz < span; dz += 64)
                    for (int dx = 32; dx < span; dx += 64)
                        for (int y : levels)
                        {
                            int b = s.biomeAt(4, (x0 + dx) >> 2, y, (z0 + dz) >> 2);
                            if (b >= 0)
                                h[(size_t) b] = 1;
                        }
            }
        }
    };
    std::vector<std::thread> threads;
    const int nt = std::max(1, std::min(16, QThread::idealThreadCount()));
    for (int t = 0; t < nt; t++)
        threads.emplace_back(work);
    for (std::thread& t : threads)
        t.join();

    // greedy choice: the area with the most uncovered biomes (a biome of a
    // pack counts as much as a thousand vanilla ones), nearest to 0,0 first
    std::vector<char> covered((size_t) nb, 0), seen((size_t) nb, 0);
    for (const std::vector<char>& h : has)
        for (int b = 0; b < nb; b++)
            seen[(size_t) b] |= h[(size_t) b];
    std::vector<std::pair<int, int>> picks;
    std::vector<std::vector<int>> pickBiomes;
    for (int k = 0; k < count; k++)
    {
        int64_t best = 0, bestDist = 0;
        int bi = -1;
        for (int row = 0; row < n; row++)
            for (int col = 0; col < n; col++)
            {
                const int cx = (col - n / 2) * size, cz = (row - n / 2) * size;
                if (cx < 16 && cx + size > -16 && cz < 16 && cz + size > -16)
                    continue;   // overlaps the area origin (chunks -16 .. 15)
                const std::vector<char>& h = has[(size_t) row * n + col];
                int64_t score = 0;
                for (int b = 0; b < nb; b++)
                    if (h[(size_t) b] && !covered[(size_t) b])
                        score += w->biomes()[(size_t) b].vanilla ? 1 : 1000;
                const int64_t dist = (int64_t) cx * cx + (int64_t) cz * cz;
                if (score > best || (score == best && score > 0 && dist < bestDist))
                    best = score, bestDist = dist, bi = row * n + col;
            }
        if (bi < 0)
            break;
        picks.push_back({ (bi % n - n / 2) * size, (bi / n - n / 2) * size });
        std::vector<int> got;
        for (int b = 0; b < nb; b++)
            if (has[(size_t) bi][(size_t) b] && !covered[(size_t) b])
                got.push_back(b), covered[(size_t) b] = 1;
        pickBiomes.push_back(got);
    }
    int packBiomes = 0, packSeen = 0, packCovered = 0;
    QStringList notSeen;
    for (int b : info.biomes)
    {
        if (w->biomes()[(size_t) b].vanilla)
            continue;
        packBiomes++;
        packSeen += seen[(size_t) b];
        packCovered += covered[(size_t) b];
        if (!seen[(size_t) b])
            notSeen << w->biomes()[(size_t) b].id;
    }
    printf("# %s, seed %lld: %d biomes of packs in the dimension's list, %d of them found within %d blocks,"
        " %d in the picked areas\n", quiet ? "dimension" : qPrintable(dimId), (long long) seed, packBiomes,
        packSeen, radius, packCovered);
    if (!quiet && !notSeen.isEmpty())
        printf("# not found: %s\n", qPrintable(notSeen.join(' ')));
    QString line = QString("%1 origin:-16,-16,32,32").arg((long long) seed);
    for (size_t k = 0; k < picks.size(); k++)
    {
        line += QString(" p%1:%2,%3,%4,%4").arg(k + 1).arg(picks[k].first).arg(picks[k].second).arg(size);
        QStringList ids;
        for (int b : pickBiomes[k])
            ids << w->biomes()[(size_t) b].id;
        if (quiet)
            printf("# p%zu: %d new biomes\n", k + 1, (int) pickBiomes[k].size());
        else
            printf("# p%zu: %s\n", k + 1, qPrintable(ids.join(' ')));
    }
    printf("%s\n", qPrintable(line));
    return 0;
}

//==============================================================================
// packcheck: a pack against server reference files, aggregates only
//==============================================================================

// For packs whose content must not be shown (tools/worldgen-reference/
// check_pack.py): the comparisons of heights and structures, but the output
// holds only counts, shares and times. No resource ids, names, file names,
// coordinates or values of the pack are printed; the only names are the
// vanilla dimensions and the types the generator does not know.

// The type a load issue names as unknown, or empty: only the fixed sentences
// of the loader that name a type (an issue's text can name resources too).
static QString unknownTypeOf(const QString& text)
{
    static const char *const patterns[] = {
        "^Unknown density function type \"([^\"]+)\"",
        "^The structure type \"([^\"]+)\" is not known",
        "^The placement type \"([^\"]+)\" is not known",
        "^Unknown biome source type \"([^\"]+)\"",
        "^Unknown generator type \"([^\"]+)\"",
        "unknown type \"([^\"]+)\"",
    };
    static const char *const kinds[] = {
        "density function type", "structure type", "placement type", "biome source type", "generator type",
        "pool element type",
    };
    for (size_t k = 0; k < sizeof(patterns) / sizeof(*patterns); k++)
    {
        QRegularExpressionMatch m = QRegularExpression(patterns[k]).match(text);
        if (m.hasMatch() && QRegularExpression("^[a-z0-9_.-]+:[a-z0-9_./-]+$").match(m.captured(1)).hasMatch())
            return QString("%1 %2").arg(kinds[k], m.captured(1));
    }
    return QString();
}

static QString dimLabel(const dp::World& w, const QString& id)
{
    if (id == "minecraft:overworld" || id == "minecraft:the_nether" || id == "minecraft:the_end")
        return id.mid(10);
    int n = 0;
    for (const dp::DimensionInfo& d : w.dimensions())
    {
        if (d.id == "minecraft:overworld" || d.id == "minecraft:the_nether" || d.id == "minecraft:the_end")
            continue;
        n++;
        if (d.id == id)
            return QString("dimension %1 of the packs").arg(n);
    }
    return QString("a dimension the packs do not define");
}

struct PackCheckHeights
{
    std::map<int, Counter> byY;     // biome cells by y
    Counter all;
    int64_t packCells = 0;
    QSet<QString> packBiomes;       // only counted
    HeightStats of, ws, fl;
    double ns = 0;
    bool terrainOnly = false;           // made without features, carvers, structures
    int64_t ofFluid = 0, ofSoftTop = 0, ofOther = 0;    // why the ocean floor differs
};

static bool packCheckHeights(const dp::World& w, std::shared_ptr<const dp::World> wp, const QString& path,
    QString *dimId, PackCheckHeights *r)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    int64_t seed = 0;
    int floorBelow = 100;
    struct H { int x, z, ws, of; };
    std::vector<H> hs;
    struct F { int x, z, y; };
    std::vector<F> fls;
    std::vector<ServerFile::Cell> bs;
    *dimId = "minecraft:overworld";
    while (!f.atEnd())
    {
        QByteArray l = f.readLine().trimmed();
        QList<QByteArray> p = l.split(' ');
        if (l.startsWith("# dimension ")) *dimId = QString::fromLatin1(l.mid(12));
        else if (l.startsWith("# terrain_only")) r->terrainOnly = true;
        else if (l.startsWith("# seed ")) seed = l.mid(7).toLongLong();
        else if (l.startsWith("# F ") && l.contains("below y "))
            floorBelow = l.mid(l.indexOf("below y ") + 8).split(' ').value(0).toInt();
        else if (l.startsWith("H ") && p.size() == 5)
            hs.push_back({ p[1].toInt(), p[2].toInt(), p[3].toInt(), p[4].toInt() });
        else if (l.startsWith("F ") && p.size() == 4)
            fls.push_back({ p[1].toInt(), p[2].toInt(), p[3].toInt() });
        else if (l.startsWith("B ") && p.size() == 5)
        {
            ServerFile::Cell c { p[1].toInt(), p[2].toInt(), p[3].toInt(), QString::fromLatin1(p[4]) };
            if (!c.biome.contains(':'))
                c.biome = "minecraft:" + c.biome;
            bs.push_back(c);
        }
    }
    const int dim = w.dimension(*dimId);
    if (dim < 0 || !w.dimensions()[(size_t) dim].supported)
        return false;
    const dp::DimensionInfo& info = w.dimensions()[(size_t) dim];
    dp::Sampler s(wp, dim, (uint64_t) seed);
    for (const ServerFile::Cell& c : bs)
    {
        int b = s.biomeAt(4, c.x >> 2, c.y, c.z >> 2);
        bool same = b >= 0 && w.biomes()[(size_t) b].id == c.biome;
        r->byY[c.y].add(same);
        r->all.add(same);
        if (!c.biome.startsWith("minecraft:"))
        {
            r->packCells++;
            r->packBiomes.insert(c.biome);
        }
    }
    if (info.terrain)
    {
        const int none = info.minY - 1;
        QElapsedTimer t;
        for (const H& h : hs)
        {
            t.start();
            int a = s.surfaceHeight(h.x, h.z, dp::HEIGHT_OCEAN_FLOOR, 0x7fffffff, dp::HEIGHTS_SURFACE);
            int b = s.surfaceHeight(h.x, h.z, dp::HEIGHT_SURFACE, 0x7fffffff, dp::HEIGHTS_SURFACE);
            r->ns += (double) t.nsecsElapsed();
            a = a == dp::NO_HEIGHT ? none : a;
            b = b == dp::NO_HEIGHT ? none : b;
            r->of.add(a - h.of);
            r->ws.add(b - h.ws);
            if (a != h.of)
                (a < h.of && h.ws > h.of ? r->ofFluid : a == h.ws && b == h.ws && h.of < h.ws ? r->ofSoftTop
                    : r->ofOther)++;
        }
        for (const F& fl : fls)
        {
            int a = s.surfaceHeight(fl.x, fl.z, dp::HEIGHT_OCEAN_FLOOR, floorBelow);
            r->fl.add((a == dp::NO_HEIGHT ? none : a) - fl.y);
        }
    }
    return true;
}

struct PackCheckStructures
{
    int server = 0, unknownStructure = 0, noCandidate = 0, calledInvalid = 0, matchedValid = 0, matchedUnknown = 0;
    int candValid = 0, falseValid = 0, candUnknown = 0, unknownWithout = 0, candInvalid = 0;
    int posChecked = 0, posSame = 0;
    QSet<QString> withStarts, packWithStarts;     // only counted
    double ms = 0;
};

static bool packCheckStructures(const dp::World& w, std::shared_ptr<const dp::World> wp, const QString& path,
    QString *dimId, PackCheckStructures *r)
{
    StructFile sf;
    if (!readStructFile(path, &sf))
        return false;
    *dimId = sf.dimension;
    const int dim = w.dimension(sf.dimension);
    if (dim < 0 || !w.dimensions()[(size_t) dim].supported)
        return false;
    const std::vector<dp::StructureInfo>& infos = w.structures();
    const dp::StructureData& sd = w.d()->structures;
    QHash<QString, int> index;
    for (size_t i = 0; i < infos.size(); i++)
        index.insert(infos[i].id, (int) i);
    dp::StructureFinder finder(wp, dim, sf.seed);
    std::vector<dp::StructureStart> cands;
    QElapsedTimer t;
    t.start();
    finder.starts(sf.cx0, sf.cz0, sf.cx1, sf.cz1, &cands);
    r->ms += (double) t.nsecsElapsed() / 1e6;
    QHash<QString, const dp::StructureStart *> byKey;
    for (const dp::StructureStart& s : cands)
        byKey.insert(QString("%1 %2 %3").arg(infos[(size_t) s.structure].id).arg(s.chunkX).arg(s.chunkZ), &s);
    QSet<QString> serverKeys;
    for (const StructFile::Start& s : sf.starts)
    {
        const QString key = QString("%1 %2 %3").arg(s.id).arg(s.cx).arg(s.cz);
        serverKeys.insert(key);
        r->server++;
        r->withStarts.insert(s.id);
        if (!s.id.startsWith("minecraft:"))
            r->packWithStarts.insert(s.id);
        if (!index.contains(s.id))
        {
            r->unknownStructure++;
            continue;
        }
        const dp::StructureStart *c = byKey.value(key);
        if (!c)
        {
            r->noCandidate++;
            continue;
        }
        if (c->state == dp::StructureStart::Invalid)
        {
            r->calledInvalid++;
            continue;
        }
        (c->state == dp::StructureStart::Valid ? r->matchedValid : r->matchedUnknown)++;
        const dp::StructureDef& def = sd.defs[(size_t) index.value(s.id)];
        if (def.kind == dp::StructureDef::JIGSAW && s.ex.contains("element"))
        {
            const int x = (int) (((int64_t) s.bb[0] + s.bb[3]) / 2), z = (int) (((int64_t) s.bb[2] + s.bb[5]) / 2);
            r->posChecked++;
            r->posSame += x == c->x && z == c->z;
        }
    }
    for (const dp::StructureStart& s : cands)
    {
        if (!sf.covered.contains(chunkKey(s.chunkX, s.chunkZ)))
            continue;
        const bool has = serverKeys.contains(
            QString("%1 %2 %3").arg(infos[(size_t) s.structure].id).arg(s.chunkX).arg(s.chunkZ));
        if (s.state == dp::StructureStart::Valid)
        {
            r->candValid++;
            r->falseValid += !has;
        }
        else if (s.state == dp::StructureStart::Unknown)
        {
            r->candUnknown++;
            r->unknownWithout += !has;
        }
        else
            r->candInvalid++;
    }
    return true;
}

static int cmdPackCheck(const QStringList& argsIn)
{
    QStringList args = argsIn, packs;
    QString preset = "minecraft:normal";
    bool timing = false;
    for (int i = 0; i < args.size(); )
    {
        if (args[i] == "--pack" && i + 1 < args.size()) { packs << args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--preset" && i + 1 < args.size()) { preset = args[i + 1]; args.remove(i, 2); }
        else if (args[i] == "--timing") { timing = true; args.removeAt(i); }
        else i++;
    }
    if (args.size() < 2 || packs.isEmpty())
    {
        printf("usage: dptest packcheck <vanilla data> <mc> --pack <pack>... [--preset <id>] [--timing]\n"
            "           [<reference file of worldgen_reference.py> ...]\n"
            "       heights/biomes files and structure files of real servers against the generator;\n"
            "       prints counts, shares and times only (no ids, names, coordinates or values)\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const int mc = str2mc(qPrintable(args[1]));
    dp::Report report;
    QElapsedTimer t;
    t.start();
    std::shared_ptr<const dp::World> w = loadWorld(mc, args[0], packs, preset, &report, false);
    const double loadMs = (double) t.nsecsElapsed() / 1e6;

    // the load
    int sev[3] = { 0, 0, 0 };
    std::map<QString, int> types;
    for (const dp::Issue& i : report.issues)
    {
        sev[i.severity]++;
        QString ty = unknownTypeOf(i.text);
        if (!ty.isEmpty())
            types[ty]++;
    }
    printf("load: %.0f ms; %d errors, %d warnings, %d notes (the app's pack report has the details)\n", loadMs,
        sev[dp::Issue::Error], sev[dp::Issue::Warning], sev[dp::Issue::Info]);
    for (const dp::PackInfo& p : w->packs())
        if (!p.formatMatches)
            printf("load: a pack does not declare support for this version\n");
    int setsOff = 0, structsOff = 0;
    for (const dp::StructureSetInfo& s : w->structureSets())
        setsOff += !s.supported;
    for (const dp::StructureInfo& s : w->structures())
        structsOff += !s.supported;
    printf("load: %zu structure sets (%d not supported), %zu structures (%d not supported)\n",
        w->structureSets().size(), setsOff, w->structures().size(), structsOff);
    for (const auto& ty : types)
        printf("unsupported: %s (%d)\n", qPrintable(ty.first), ty.second);
    int packBiomes = 0;
    for (const dp::BiomeInfo& b : w->biomes())
        packBiomes += !b.vanilla;
    printf("biomes: %zu, of them %d not vanilla\n", w->biomes().size(), packBiomes);
    for (const dp::DimensionInfo& d : w->dimensions())
    {
        int own = 0;
        for (int b : d.biomes)
            own += !w->biomes()[(size_t) b].vanilla;
        printf("%s: %s, terrain %s, %zu biomes (%d not vanilla)%s\n", qPrintable(dimLabel(*w, d.id)),
            d.supported ? "supported" : "not supported", d.terrain ? "yes" : "no", d.biomes.size(), own,
            d.packFile ? ", defined by a pack" : "");
    }

    // the reference files
    for (int fi = 2; fi < args.size(); fi++)
    {
        QFile f(args[fi]);
        if (!f.open(QIODevice::ReadOnly))
        {
            printf("reference %d: cannot be read\n", fi - 1);
            g_fail++;
            continue;
        }
        bool structFile = false;
        while (!f.atEnd())
        {
            QByteArray l = f.readLine();
            if (!l.startsWith("#"))
                break;
            structFile |= l.startsWith("# grid ");
        }
        f.close();
        QString dimId;
        if (structFile)
        {
            PackCheckStructures r;
            if (!packCheckStructures(*w, w, args[fi], &dimId, &r))
            {
                printf("reference %d (structures): not read, or its dimension is not supported\n", fi - 1);
                g_fail++;
                continue;
            }
            printf("reference %d, structures, %s: %d server starts of %d structures (%d of the packs)\n", fi - 1,
                qPrintable(dimLabel(*w, dimId)), r.server, (int) r.withStarts.size(), (int) r.packWithStarts.size());
            printf("  server starts: valid here %d, undecided here %d, called invalid %d, no candidate %d,"
                " structure unknown %d\n", r.matchedValid, r.matchedUnknown, r.calledInvalid, r.noCandidate,
                r.unknownStructure);
            printf("  candidates in the covered chunks: valid %d (without a server start: %d), undecided %d"
                " (without: %d), invalid %d\n", r.candValid, r.falseValid, r.candUnknown, r.unknownWithout,
                r.candInvalid);
            printf("  jigsaw start positions compared %d, the same %d; search %.0f ms\n", r.posChecked, r.posSame,
                r.ms);
            CHECK(r.calledInvalid == 0 && r.noCandidate == 0 && r.unknownStructure == 0 && r.falseValid == 0 &&
                r.posSame == r.posChecked, "reference %d: structure starts differ", fi - 1);
        }
        else
        {
            PackCheckHeights r;
            if (!packCheckHeights(*w, w, args[fi], &dimId, &r))
            {
                printf("reference %d (heights and biomes): not read, or its dimension is not supported\n", fi - 1);
                g_fail++;
                continue;
            }
            printf("reference %d, heights and biomes%s, %s: %lld biome cells, %lld differ; %lld cells of %d"
                " biomes of the packs\n", fi - 1, r.terrainOnly ? " (terrain only)" : "",
                qPrintable(dimLabel(*w, dimId)), (long long) r.all.n, (long long) r.all.diff,
                (long long) r.packCells, (int) r.packBiomes.size());
            for (const auto& y : r.byY)
                printf("  biomes at y %d: %lld cells, %lld differ\n", y.first, (long long) y.second.n,
                    (long long) y.second.diff);
            r.of.print("ocean_floor");
            r.ws.print("world_surface");
            r.fl.print("floor below the roof");
            if (r.of.n)
            {
                printf("  ocean_floor differs in %lld columns: %lld with the server's floor higher under a fluid"
                    " (aquifers), %lld under a top block that does not block motion, %lld other%s\n",
                    (long long) (r.ofFluid + r.ofSoftTop + r.ofOther), (long long) r.ofFluid,
                    (long long) r.ofSoftTop, (long long) r.ofOther, r.terrainOnly
                    ? " (surface rules that place a fluid or air, or what the aquifers place without a fluid)"
                    : " (features such as trees, carvers, structures, surface rules)");
                printf("  %.1f us per column (both heights)\n", r.ns / 1000.0 / (double) r.of.n);
            }
            CHECK(r.all.diff == 0, "reference %d: biome cells differ", fi - 1);
        }
    }

    // times per map tile
    if (timing)
    {
        for (size_t di = 0; di < w->dimensions().size(); di++)
        {
            const dp::DimensionInfo& d = w->dimensions()[di];
            if (!d.supported)
                continue;
            const int y = d.hasCeiling ? d.minY + 32 : std::max(d.minY, std::min(d.seaLevel, d.minY + d.height - 1));
            dp::Sampler s(w, (int) di, 12345);
            std::vector<int> out(256 * 256);
            QString line;
            for (int scale : { 1, 4, 16, 64 })
            {
                t.start();
                s.genBiomes(out.data(), scale, 3 * 256, -2 * 256, 256, 256, y);
                line += QString(" 1:%1 %2 ms").arg(scale).arg((double) t.nsecsElapsed() / 1e6, 0, 'f', 1);
            }
            printf("%s: biome tile 256 x 256: %s\n", qPrintable(dimLabel(*w, d.id)), qPrintable(line.trimmed()));
            if (d.terrain)
            {
                line.clear();
                for (int scale : { 1, 4, 16 })
                {
                    t.start();
                    s.genHeights(out.data(), scale, 3 * 256, -2 * 256, 256, 256, dp::HEIGHT_OCEAN_FLOOR,
                        d.hasCeiling ? d.minY + 100 : 0x7fffffff);
                    line += QString(" 1:%1 %2 ms").arg(scale).arg((double) t.nsecsElapsed() / 1e6, 0, 'f', 1);
                }
                printf("%s: height tile 256 x 256: %s\n", qPrintable(dimLabel(*w, d.id)), qPrintable(line.trimmed()));
            }
            // structures: a map tile of 2048 blocks, a new finder, then its neighbour
            dp::StructureFinder sfind(w, (int) di, 12345);
            std::vector<dp::StructureStart> cands;
            t.start();
            sfind.starts(0, 0, 127, 127, &cands);
            const double first = (double) t.nsecsElapsed() / 1e6;
            const size_t n1 = cands.size();
            cands.clear();
            t.start();
            sfind.starts(128, 0, 255, 127, &cands);
            printf("%s: structure tile 128 x 128 chunks: first %.0f ms (%zu candidates), next %.0f ms (%zu)\n",
                qPrintable(dimLabel(*w, d.id)), first, n1, (double) t.nsecsElapsed() / 1e6, cands.size());
        }
    }
    return g_fail ? 1 : 0;
}

//==============================================================================
// mcmeta: pack.mcmeta against real servers
//==============================================================================

// tools/worldgen-reference/data/mcmeta_<mc>.txt (mcmeta_reference.py): what a
// server did with each probe pack, against readPackMeta() at the server's
// format; with --vanilla the file of that version once more through the
// loader, with the probe packs written to the work directory.
static int cmdMcmeta(const QStringList& argsIn)
{
    QStringList args = argsIn;
    QString vanilla, vmc;
    int k = args.indexOf("--vanilla");
    if (k >= 0 && k + 2 < args.size())
    {
        vanilla = args[k + 1];
        vmc = args[k + 2];
        args.erase(args.begin() + k, args.begin() + k + 3);
    }
    if (args.size() < 2)
    {
        printf("usage: dptest mcmeta <work dir> <mcmeta file> ... [--vanilla <vanilla data> <mc>]\n");
        return 2;
    }
    dp::setAllowUnverifiedVersions(true);
    const QString work = args[0];
    int loaderFiles = 0;
    for (int fi = 1; fi < args.size(); fi++)
    {
        QFile f(args[fi]);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            CHECK(false, "cannot read %s", qPrintable(args[fi]));
            continue;
        }
        struct Case
        {
            QString name;
            QStringList dirs;
            QByteArray text;
            bool loaded = false;
            QSet<QString> applied;
            QString winner;
        };
        std::vector<Case> cases;
        QString mcName;
        int fmaj = -1, fmin = 0;
        while (!f.atEnd())
        {
            const QByteArray line = f.readLine().trimmed();
            if (line.startsWith("# mc "))
                mcName = QString::fromUtf8(line.mid(5));
            else if (line.startsWith("# pack_format "))
            {
                const QList<QByteArray> v = line.mid(14).split('.');
                fmaj = v.value(0).toInt();
                fmin = v.value(1).toInt();
            }
            else if (line.startsWith("C "))
            {
                // C <case> <dirs or -> <pack.mcmeta>
                const int a = line.indexOf(' ', 2), b = line.indexOf(' ', a + 1);
                Case c;
                c.name = QString::fromUtf8(line.mid(2, a - 2));
                const QString dirs = QString::fromUtf8(line.mid(a + 1, b - a - 1));
                if (dirs != "-")
                    for (const QString& d : dirs.split(','))
                        c.dirs.push_back(QString(d).replace("%20", " "));
                c.text = line.mid(b + 1);
                cases.push_back(c);
            }
            else if (!cases.empty() && (line.startsWith("R ") || line.startsWith("O ") || line.startsWith("W ")))
            {
                const QStringList p = QString::fromUtf8(line).split(' ');
                Case& c = cases.back();
                if (p.size() < 3 || p[1] != c.name)
                    continue;
                if (p[0] == "R")
                    c.loaded = p[2] == "loaded";
                else if (p[0] == "O" && p.size() > 3 && p[3] == "applied")
                    c.applied.insert(QString(p[2]).replace("%20", " "));
                else if (p[0] == "W")
                    c.winner = p[2];
            }
        }
        if (fmaj < 0 || cases.empty())
        {
            CHECK(false, "%s: no pack format or no cases", qPrintable(args[fi]));
            continue;
        }
        const dp::PackFormat version = dp::packFormat(fmaj, fmin);
        int differ = 0, overlays = 0, loaded = 0;
        for (const Case& c : cases)
        {
            const dp::PackMeta pm = dp::readPackMeta(c.text, version);
            bool same = pm.loaded == c.loaded;
            loaded += c.loaded;
            for (const QString& d : c.dirs)
            {
                overlays++;
                const bool mine = pm.loaded && pm.overlays.contains(d);
                if (mine != c.applied.contains(d))
                {
                    same = false;
                    printf("  %s %s: overlay %s: server %s, generator %s\n", qPrintable(mcName), qPrintable(c.name),
                        qPrintable(d), c.applied.contains(d) ? "applied" : "absent", mine ? "applied" : "absent");
                }
            }
            if (!c.winner.isEmpty())
            {
                const QString mine = pm.overlays.isEmpty() ? QString("root") : pm.overlays.last();
                if (mine != c.winner)
                {
                    same = false;
                    printf("  %s %s: the shared file of %s (server), %s (generator)\n", qPrintable(mcName),
                        qPrintable(c.name), qPrintable(c.winner), qPrintable(mine));
                }
            }
            if (pm.loaded != c.loaded)
                printf("  %s %s: server %s, generator %s (%s)\n", qPrintable(mcName), qPrintable(c.name),
                    c.loaded ? "loads it" : "does not load it", pm.loaded ? "loads it" : "does not load it",
                    qPrintable(pm.reason));
            differ += !same;
        }
        printf("%s (format %d.%d): %d cases (%d loaded by the server), %d overlays: %d differ\n",
            qPrintable(mcName), fmaj, fmin, (int) cases.size(), loaded, overlays, differ);
        CHECK(differ == 0, "%s: %d cases differ", qPrintable(mcName), differ);

        // the same through the loader, for the engine version of the file
        if (vanilla.isEmpty() || mcName != vmc)
            continue;
        loaderFiles++;
        const int mc = str2mc(qPrintable(vmc));
        int fa = 0, fb = 0;
        dp::packFormatRange(mc, &fa, &fb);
        CHECK(fb == fmaj, "the engine version %s has format %d, the file %d", qPrintable(vmc), fb, fmaj);
        int ldiffer = 0;
        for (const Case& c : cases)
        {
            const QString dir = work + "/mcmeta-" + c.name;
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString df = "data/probe/worldgen/density_function/";
            writeFile(dir + "/pack.mcmeta", c.text);
            writeFile(dir + "/" + df + c.name + "_base.json", "1.0");
            for (int i = 0; i < c.dirs.size(); i++)
                writeFile(dir + "/" + c.dirs[i] + "/" + df + c.name + "_ov" + QByteArray::number(i) + ".json", "2.0");
            dp::Request req;
            req.mc = mc;
            req.vanillaPath = vanilla;
            dp::PackSource ps;
            ps.path = dir;
            req.packs = { ps };
            dp::Resources res;
            dp::Report report;
            dp::loadResources(req, &res, &report);
            bool same = res.packs.size() == 2;
            const bool isLoaded = same && res.packs[1].loaded;
            same = same && isLoaded == c.loaded
                && (res.find(dp::REG_DENSITY_FUNCTION, "probe:" + c.name + "_base") != nullptr) == c.loaded;
            for (int i = 0; i < c.dirs.size(); i++)
            {
                const bool there = res.find(dp::REG_DENSITY_FUNCTION,
                    "probe:" + c.name + "_ov" + QString::number(i)) != nullptr;
                same = same && there == (c.loaded && c.applied.contains(c.dirs[i]));
            }
            if (!same)
                printf("  loader %s %s: differs from the server\n", qPrintable(vmc), qPrintable(c.name));
            ldiffer += !same;
        }
        printf("%s through the loader: %d cases: %d differ\n", qPrintable(vmc), (int) cases.size(), ldiffer);
        CHECK(ldiffer == 0, "loader %s: %d cases differ", qPrintable(vmc), ldiffer);
    }
    CHECK(vanilla.isEmpty() || loaderFiles == 1, "--vanilla %s: no file of that version", qPrintable(vmc));
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    QStringList args;
    for (int i = 1; i < argc; i++)
        args.push_back(QString::fromLocal8Bit(argv[i]));
    if (args.isEmpty())
    {
        printf("usage: dptest <command> [arguments]\n"
            "commands: tree, vanilla, pack, terrain, heights, hperf, robust, same, zip, truncate, perf, cell,\n"
            "structures, starts, structrobust, structcheck, structperf, limits, graphdepth, areas,\n"
            "packcheck, mcmeta\n"
            "(each prints its arguments when called without; see the comment at the top of dptest.cpp)\n");
        return 2;
    }
    QString cmd = args.takeFirst();
    int ret = 2;
    if (cmd == "tree")
        ret = cmdTree(args);
    else if (cmd == "vanilla")
        ret = cmdVanilla(args);
    else if (cmd == "perf")
        ret = cmdPerf(args);
    else if (cmd == "zip")
        ret = cmdZip(args);
    else if (cmd == "truncate")
        ret = cmdTruncate(args);
    else if (cmd == "same")
        ret = cmdSame(args);
    else if (cmd == "robust")
        ret = cmdRobust(args);
    else if (cmd == "terrain")
        ret = cmdTerrain(args);
    else if (cmd == "pack")
        ret = cmdPack(args);
    else if (cmd == "cell")
        ret = cmdCell(args);
    else if (cmd == "structures")
        ret = cmdStructures(args);
    else if (cmd == "starts")
        ret = cmdStarts(args);
    else if (cmd == "structrobust")
        ret = cmdStructRobust(args);
    else if (cmd == "structcheck")
        ret = cmdStructCheck(args);
    else if (cmd == "structperf")
        ret = cmdStructPerf(args);
    else if (cmd == "aqperf")
        ret = cmdAquiferPerf(args);
    else if (cmd == "tileperf")
        ret = cmdTilePerf(args);
    else if (cmd == "heights")
        ret = cmdHeights(args);
    else if (cmd == "graphdepth")
        ret = cmdGraphDepth(args);
    else if (cmd == "limits")
        ret = cmdLimits(args);
    else if (cmd == "hperf")
        ret = cmdHeightPerf(args);
    else if (cmd == "areas")
        ret = cmdAreas(args);
    else if (cmd == "packcheck")
        ret = cmdPackCheck(args);
    else if (cmd == "mcmeta")
        ret = cmdMcmeta(args);
    else
        printf("unknown command %s\n", qPrintable(cmd));
    if (ret == 0)
        printf("OK\n");
    else if (ret == 1)
        printf("FAILED (%d checks)\n", g_fail);
    return ret;
}
