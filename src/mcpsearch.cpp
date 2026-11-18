// The search tools of the MCP server: start_search, search_status,
// search_results, stop_search and export_search. A search is the app's
// SearchMaster with a session, as in the Seed Finder and the headless mode;
// each one lives in a thread of its own with an event loop (the master and
// its workers talk through queued signals).

#include "mcptools.h"

#include "mcpserver.h"
#include "conditiontext.h"
#include "datapackworld.h"
#include "message.h"
#include "seedatlas-engine/util.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <unordered_set>

using namespace mcp;

class McpSearchJob : public QObject
{
public:
    QString id;
    Session session;
    QStringList summary;            // one sentence per condition
    QJsonObject world;              // as worldJson()
    int threads = 1;
    int maxResults = 1000;
    qint64 limitMs = 0;
    McpRegistry *reg = nullptr;

    QThread thread;
    // stop() and shutdown() one at a time; dead once the thread has ended
    QMutex life;
    bool dead = false;
    // (in the job's thread)
    SearchMaster *master = nullptr;
    QTimer *timer = nullptr;

    mutable QMutex m;
    QString state = "starting";     // running, finished, stopped, failed
    QString reason;                 // why it stopped
    std::vector<uint64_t> results;
    std::unordered_set<uint64_t> seen;
    uint64_t prog = 0, end = 0, current = 0, checked = 0;
    qreal speed = 0;
    QString status;
    QElapsedTimer clock;
    qint64 elapsedMs = 0;
    bool released = false;
    bool ended = false;

    ~McpSearchJob() override
    {
        shutdown();
    }

    // Starts the search; false and @error if the session cannot be searched.
    bool start(QString *error)
    {
        moveToThread(&thread);
        thread.setObjectName("mcp search");
        thread.start();
        bool ok = false;
        QMetaObject::invokeMethod(this, [this, &ok, error]() { ok = begin(error); },
            Qt::BlockingQueuedConnection);
        return ok;
    }

    // Stops the search (any thread but the job's).
    void stop(const QString& why)
    {
        QMutexLocker lock(&life);
        if (dead || !thread.isRunning())
            return;
        QMetaObject::invokeMethod(this, [this, why]() { stopNow(why); },
            Qt::BlockingQueuedConnection);
    }

    // Stops the search and the thread.
    void shutdown()
    {
        QMutexLocker lock(&life);
        if (dead)
            return;
        dead = true;
        if (!thread.isRunning())
        {
            releaseOnce();
            return;
        }
        QMetaObject::invokeMethod(this, [this]() {
            stopNow("server ended");
            delete timer;
            timer = nullptr;
            delete master;  // (stops its workers)
            master = nullptr;
        }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
        releaseOnce();
    }

    QJsonObject statusJson() const
    {
        QMutexLocker lock(&m);
        QJsonObject o;
        o["search_id"] = id;
        o["state"] = state;
        if (!reason.isEmpty())
            o["reason"] = reason;
        o["results_found"] = int(results.size());
        o["max_results"] = maxResults;
        QJsonObject p;
        p["done"] = double(prog);
        p["total"] = double(end);
        p["fraction"] = end ? std::min(1.0, double(prog) / double(end)) : 0.0;
        p["seeds_checked"] = double(checked);
        p["next_seed"] = seedText(current);
        if (speed > 0)
            p["seeds_per_second"] = std::round(speed);
        if (!status.isEmpty())
            p["status"] = status;
        o["progress"] = p;
        o["elapsed_seconds"] = double((ended ? elapsedMs : clock.elapsed()) / 100) / 10.0;
        o["time_limit_seconds"] = double(limitMs / 1000);
        o["threads"] = threads;
        o["conditions"] = QJsonArray::fromStringList(summary);
        o["world"] = world;
        return o;
    }

private:
    void releaseOnce()
    {
        bool release = false;
        {
            QMutexLocker lock(&m);
            if (!released)
            {
                released = true;
                release = true;
            }
        }
        if (release)
            reg->releaseThreads(threads);
    }

    bool begin(QString *error)
    {
        MessageCapture capture;
        master = new SearchMaster(nullptr);
        if (!master->set(nullptr, session))
        {
            *error = capture.texts.join(" ");
            if (error->isEmpty())
                *error = "The search cannot start with these conditions.";
            return false;
        }
        QObject::connect(master, &SearchMaster::searchResults, this,
            [this](QVector<uint64_t> seeds) { onResults(seeds); }, Qt::DirectConnection);
        QObject::connect(master, &SearchMaster::searchFinish, this,
            [this](bool done) { onFinish(done); }, Qt::DirectConnection);
        timer = new QTimer();
        timer->setInterval(500);
        QObject::connect(timer, &QTimer::timeout, this, [this]() { poll(); });
        {
            QMutexLocker lock(&m);
            state = "running";
            clock.start();
        }
        master->startSearch();
        timer->start();
        if (!capture.texts.isEmpty())
            qWarning("search %s: %s", qPrintable(id), qPrintable(capture.texts.join(" ")));
        return true;
    }

    void stopNow(const QString& why)
    {
        {
            QMutexLocker lock(&m);
            if (ended || state != "running")
                return;
            if (reason.isEmpty())
                reason = why;
        }
        if (master)
        {
            MessageCapture capture;
            master->stopSearch();
        }
        // (searchFinish ends the job; a master that had no workers yet
        // does not send it)
        finishState(false);
    }

    void onResults(const QVector<uint64_t>& seeds)
    {
        bool full = false;
        {
            QMutexLocker lock(&m);
            for (uint64_t s : seeds)
            {
                if (int(results.size()) >= maxResults)
                {
                    full = true;
                    break;
                }
                if (seen.insert(s).second)
                    results.push_back(s);
            }
            if (int(results.size()) >= maxResults)
                full = true;
        }
        if (full)
            QTimer::singleShot(0, this, [this]() { stopNow("result limit reached"); });
    }

    void onFinish(bool done)
    {
        finishState(done);
    }

    void finishState(bool done)
    {
        if (timer)
            timer->stop();
        poll();
        {
            QMutexLocker lock(&m);
            if (ended)
                return;
            ended = true;
            elapsedMs = clock.elapsed();
            if (done)
            {
                state = "finished";
                prog = end;
            }
            else
                state = "stopped";
        }
        releaseOnce();
    }

    void poll()
    {
        if (!master)
            return;
        QString st;
        uint64_t p = 0, e = 0, s = 0;
        qreal mn = 0, avg = 0, mx = 0;
        const bool ok = master->getProgress(&st, &p, &e, &s, &mn, &avg, &mx);
        bool overtime = false;
        {
            QMutexLocker lock(&m);
            if (ok)
            {
                st.replace(QChar(0x2007), ' ');
                status = st;
                prog = p;
                end = e;
                current = s;
                speed = avg;
            }
            checked = master->seedsChecked();
            overtime = !ended && limitMs > 0 && clock.elapsed() > limitMs;
        }
        if (overtime)
            QTimer::singleShot(0, this, [this]() { stopNow("time limit reached"); });
    }
};

namespace {

QJsonObject prop(const char *type, const QString& description)
{
    QJsonObject o;
    o["type"] = type;
    o["description"] = description;
    return o;
}

// the filter type of a structure name
int structFilter(const QString& name)
{
    struct E { const char *id; const char *alias; int f; };
    static const E table[] = {
        { "village", nullptr, F_VILLAGE },
        { "desert_pyramid", "desert", F_DESERT },
        { "jungle_pyramid", "jungle_temple", F_JUNGLE },
        { "swamp_hut", "hut", F_HUT },
        { "igloo", nullptr, F_IGLOO },
        { "monument", "ocean_monument", F_MONUMENT },
        { "mansion", "woodland_mansion", F_MANSION },
        { "pillager_outpost", "outpost", F_OUTPOST },
        { "ocean_ruin", "ruins", F_RUINS },
        { "shipwreck", nullptr, F_SHIPWRECK },
        { "buried_treasure", "treasure", F_TREASURE },
        { "mineshaft", nullptr, F_MINESHAFT },
        { "desert_well", "well", F_WELL },
        { "ancient_city", nullptr, F_ANCIENT_CITY },
        { "trail_ruins", "trails", F_TRAILS },
        { "trial_chambers", "chambers", F_CHAMBERS },
        { "camp", nullptr, F_CAMP },
        { "ruined_portal", "portal", F_PORTAL },
        { "ruined_portal_nether", "portaln", F_PORTALN },
        { "fortress", "nether_fortress", F_FORTRESS },
        { "bastion_remnant", "bastion", F_BASTION },
        { "end_city", "endcity", F_ENDCITY },
        { "end_city_ship", "end_ship", F_ENDCITYSHIP },
        { "end_gateway", "gateway", F_GATEWAY },
        { "stronghold", nullptr, F_STRONGHOLD },
    };
    QString n = name.trimmed().toLower();
    if (n.startsWith(QLatin1String("minecraft:")))
        n = n.mid(10);
    for (const E& e : table)
        if (n == QLatin1String(e.id) || (e.alias && n == QLatin1String(e.alias)))
            return e.f;
    return F_SELECT;
}

Condition emptyCondition(int type)
{
    Condition c;
    memset(&c, 0, sizeof(c));
    c.version = Condition::VER_CURRENT;
    c.type = int16_t(type);
    return c;
}

// The area of a condition: x1..z2 (relative to the origin or the referenced
// condition) or a radius (structures: a circle; biomes: a square).
void conditionArea(Condition& c, const QJsonObject& o, bool circle, int defRadius)
{
    const bool rect = o.contains("x1") || o.contains("x2") || o.contains("z1") || o.contains("z2");
    if (rect)
    {
        c.x1 = int32_t(argIntReq(o, "x1", -MAX_COORD, MAX_COORD));
        c.z1 = int32_t(argIntReq(o, "z1", -MAX_COORD, MAX_COORD));
        c.x2 = int32_t(argIntReq(o, "x2", -MAX_COORD, MAX_COORD));
        c.z2 = int32_t(argIntReq(o, "z2", -MAX_COORD, MAX_COORD));
        if (c.x1 > c.x2) std::swap(c.x1, c.x2);
        if (c.z1 > c.z2) std::swap(c.z1, c.z2);
        c.rmax = 0;
        return;
    }
    const int r = int(argInt(o, "radius", defRadius, 0, 100000));
    if (circle)
    {
        c.rmax = r + 1;
        c.x1 = c.z1 = -r;
        c.x2 = c.z2 = r;
    }
    else
    {
        c.x1 = c.z1 = -r;
        c.x2 = c.z2 = r - 1;
    }
}

void checkKeys(const QJsonObject& o, const QStringList& allowed, const QString& what)
{
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!allowed.contains(it.key()))
            throw McpToolError{QString("Unknown field \"%1\" in a %2 condition.").arg(it.key(), what)};
}

// A condition of the built-in generator from its JSON form.
Condition parseCondition(const QJsonObject& o, int mc, int defaultId)
{
    const QString type = o.value("type").toString();
    Condition c;
    if (type == "raw")
    {
        checkKeys(o, { "type", "hex", "id", "relative_to" }, type);
        QString hex = argString(o, "hex").trimmed();
        if (hex.startsWith("#Cond:"))
            hex = hex.mid(6).trimmed();
        if (hex.isEmpty() || hex.size() > 4096 || !c.readHex(hex))
            throw McpToolError{"A raw condition needs \"hex\": the text after \"#Cond:\" in a "
                "session or filter file of the app."};
        if (c.type <= F_SELECT || c.type >= FILTER_MAX)
            throw McpToolError{"The raw condition has no valid type."};
        if (c.type == F_LUA)
            throw McpToolError{"Lua conditions are not available over MCP."};
        c.meta &= ~Condition::DISABLED;
    }
    else if (type == "structure")
    {
        checkKeys(o, { "type", "structure", "count", "radius", "x1", "z1", "x2", "z2", "id",
            "relative_to" }, type);
        const QString name = argString(o, "structure");
        const int f = structFilter(name);
        if (f == F_SELECT)
            throw McpToolError{QString("Unknown structure \"%1\" in a condition.").arg(name)};
        c = emptyCondition(f);
        c.count = int32_t(argInt(o, "count", 1, 0, 128));
        conditionArea(c, o, true, 1000);
    }
    else if (type == "stronghold")
    {
        checkKeys(o, { "type", "count", "radius", "x1", "z1", "x2", "z2", "id", "relative_to" }, type);
        c = emptyCondition(F_STRONGHOLD);
        c.count = int32_t(argInt(o, "count", 1, 0, 3));
        conditionArea(c, o, true, 1500);
    }
    else if (type == "spawn")
    {
        checkKeys(o, { "type", "radius", "x1", "z1", "x2", "z2", "id", "relative_to" }, type);
        c = emptyCondition(F_SPAWN);
        c.count = 1;
        conditionArea(c, o, true, 200);
    }
    else if (type == "biome")
    {
        checkKeys(o, { "type", "biomes", "exclude", "match_any", "scale", "y", "radius", "x1",
            "z1", "x2", "z2", "id", "relative_to" }, type);
        QStringList include, exclude;
        for (const char *key : { "biomes", "exclude" })
        {
            const QJsonValue v = o.value(key);
            if (v.isUndefined() || v.isNull())
                continue;
            if (!v.isArray() || v.toArray().size() > 256)
                throw McpToolError{QString("\"%1\" must be a list of biome names.").arg(key)};
            for (const QJsonValue& e : v.toArray())
                (strcmp(key, "biomes") == 0 ? include : exclude).append(e.toString());
        }
        if (include.isEmpty() && exclude.isEmpty())
            throw McpToolError{"A biome condition needs \"biomes\" (or \"exclude\")."};
        int dim = DIM_UNDEF;
        c = emptyCondition(F_BIOME);
        for (int pass = 0; pass < 2; pass++)
        {
            for (const QString& name : pass ? exclude : include)
            {
                const int id = vanillaBiomeId(mc, name);
                if (id < 0)
                    throw McpToolError{QString("Unknown biome \"%1\" in Minecraft %2.")
                        .arg(name, mc2str(mc))};
                const int d = getDimension(id);
                if (dim != DIM_UNDEF && d != dim)
                    throw McpToolError{"The biomes of one condition must be of one dimension."};
                dim = d;
                uint64_t& lo = pass ? c.biomeToExcl : c.biomeToFind;
                uint64_t& hi = pass ? c.biomeToExclM : c.biomeToFindM;
                if (id < 128)
                    lo |= 1ULL << id;
                else
                    hi |= 1ULL << (id - 128);
            }
        }
        c.type = int16_t(dim == DIM_NETHER ? F_BIOME_NETHER : dim == DIM_END ? F_BIOME_END : F_BIOME);
        if (argBool(o, "match_any", false))
            c.flags |= Condition::FLG_MATCH_ANY;
        const int scale = int(argInt(o, "scale", 16, 1, 256));
        if (scale != 1 && scale != 4 && scale != 16 && scale != 64 && scale != 256)
            throw McpToolError{"The scale of a biome condition is 1, 4, 16, 64 or 256."};
        if (c.type == F_BIOME_END && scale > 64)
            throw McpToolError{"End biome conditions go up to scale 64."};
        c.step = uint16_t(scale);
        c.y = int32_t(argInt(o, "y", 256, -64, 320));
        c.converage = 0.5f;
        c.confidence = 0.95f;
        conditionArea(c, o, false, 500);
    }
    else
        throw McpToolError{QString("Unknown condition type \"%1\": use structure, biome, "
            "spawn, stronghold or raw.").arg(type)};

    c.save = int32_t(argInt(o, "id", defaultId, 1, 99));
    c.relative = int32_t(argInt(o, "relative_to", c.relative, 0, 99));
    // whatever the source, the values must stay in range
    if (std::abs(int64_t(c.x1)) > MAX_COORD || std::abs(int64_t(c.x2)) > MAX_COORD ||
        std::abs(int64_t(c.z1)) > MAX_COORD || std::abs(int64_t(c.z2)) > MAX_COORD ||
        c.rmax < 0 || c.rmax > MAX_COORD || c.count < 0 || c.count > MAX_INSTANCES)
        throw McpToolError{"A condition has coordinates or counts out of range."};
    return c;
}

std::vector<uint64_t> parseSeedList(const QJsonValue& v)
{
    if (!v.isArray())
        throw McpToolError{"seeds must be a list of seeds."};
    const QJsonArray a = v.toArray();
    if (a.isEmpty() || a.size() > MAX_LIST_SEEDS)
        throw McpToolError{QString("seeds must have 1 to %1 entries.").arg(MAX_LIST_SEEDS)};
    std::vector<uint64_t> seeds;
    seeds.reserve(size_t(a.size()));
    for (const QJsonValue& s : a)
        seeds.push_back(parseSeed(s, "a seed of seeds"));
    return seeds;
}

std::shared_ptr<McpSearchJob> findJob(McpRegistry *reg, const QJsonObject& args)
{
    const QString id = argString(args, "search_id").trimmed();
    if (id.isEmpty())
        throw McpToolError{"Missing argument \"search_id\"."};
    QMutexLocker lock(&reg->mutex);
    std::shared_ptr<McpSearchJob> job = reg->searches.value(id);
    if (!job)
        throw McpToolError{QString("There is no search \"%1\".").arg(id)};
    return job;
}

} // namespace

void McpRegistry::shutdown()
{
    QMap<QString, std::shared_ptr<McpSearchJob>> jobs;
    {
        QMutexLocker lock(&mutex);
        jobs.swap(searches);
        datapacks.clear();
    }
    for (const std::shared_ptr<McpSearchJob>& job : std::as_const(jobs))
        job->shutdown();
}

void McpRegistry::stopSearchesIn(const QString& world)
{
    QMap<QString, std::shared_ptr<McpSearchJob>> jobs;
    {
        QMutexLocker lock(&mutex);
        jobs = searches;
    }
    for (const std::shared_ptr<McpSearchJob>& job : std::as_const(jobs))
        if (job->world.value("world").toString() == world)
            job->stop("its datapack world was unloaded");
}

void McpRegistry::addSearchTools()
{
    // ---------------------------------------------------------- start_search
    {
        Tool t;
        t.name = "start_search";
        t.title = "Start a seed search";
        t.description = "Starts a seed search in the background, like the app's Seed Finder, "
            "and returns its search_id at once. Follow it with search_status and "
            "search_results; stop it with stop_search. All conditions must hold (AND). "
            "Built-in generator, condition objects (coordinates in blocks, relative to the "
            "world origin or, with relative_to, to the position where the condition with that "
            "id matched):\n"
            "- {\"type\":\"structure\",\"structure\":\"village\",\"count\":1,\"radius\":500} "
            "(count 0: none in the area; or x1,z1,x2,z2 instead of radius)\n"
            "- {\"type\":\"biome\",\"biomes\":[\"mushroom_fields\"],\"exclude\":[],"
            "\"match_any\":false,\"radius\":1000,\"scale\":16,\"y\":256} (all listed biomes in "
            "the square; match_any: one of them)\n"
            "- {\"type\":\"spawn\",\"radius\":200} (the spawn in the area)\n"
            "- {\"type\":\"stronghold\",\"radius\":1500,\"count\":1}\n"
            "- {\"type\":\"raw\",\"hex\":\"...\"} (a condition of the app: the text after "
            "\"#Cond:\" of a session or filter file; Lua is not allowed)\n"
            "Optional on each: \"id\" (1-99), \"relative_to\" (an id). "
            "Datapack worlds (with \"world\"): conditions as the app's \"#DpCond:\" lines, e.g. "
            "{\"kind\":\"biome_in_area\",\"biomes\":[\"mypack:alpine\"],\"match\":\"any\","
            "\"area\":\"circle\",\"x\":0,\"z\":0,\"r\":500,\"scale\":4}; kinds biome_in_area, "
            "biome_at (x, z), biome_share (percent), structure_in_area (structures, count). "
            "Without seeds, all 2^64 seeds are searched in order from start_seed.";
        QJsonObject p = worldSchemaProperties(false);
        QJsonObject conds = prop("array", "The conditions (see the description).");
        conds["items"] = QJsonObject{ { "type", "object" } };
        conds["minItems"] = 1;
        conds["maxItems"] = 99;
        p["conditions"] = conds;
        QJsonObject seeds = prop("array", QString("Search only these seeds (at most %1).")
            .arg(MAX_LIST_SEEDS));
        seeds["items"] = QJsonObject{ { "type", QJsonArray{ "string", "integer" } } };
        p["seeds"] = seeds;
        QJsonObject start;
        start["type"] = QJsonArray{ "string", "integer" };
        start["description"] = "The first seed of an ordered search (default 0).";
        p["start_seed"] = start;
        p["max_results"] = prop("integer", QString("Stop after this many matching seeds "
            "(default 1000, at most %1).").arg(MAX_SEARCH_RESULTS));
        p["threads"] = prop("integer", "Threads for this search (default: half of the limit "
            "in the app's MCP settings, or what is left of it).");
        p["max_minutes"] = prop("integer", "Stop after this many minutes (default and limit: "
            "the app's MCP setting).");
        t.schema = schemaObject(p, { "conditions" });
        t.run = [this](const QJsonObject& args, McpCall *) {
            World w = parseWorld(this, args, false);
            const QJsonArray conds = args.value("conditions").toArray();
            if (!args.value("conditions").isArray() || conds.isEmpty() || conds.size() > 99)
                throw McpToolError{"conditions must be a list of 1 to 99 conditions."};
            std::shared_ptr<McpSearchJob> job = std::make_shared<McpSearchJob>();
            job->reg = this;
            Session& s = job->session;
            s.wi = w.wi;
            s.wi.seed = 0;
            s.sc.reset();
            s.sc.stoponres = false;
            s.gen48.reset();
            if (w.datapack())
            {
                for (int i = 0; i < conds.size(); i++)
                {
                    if (!conds[i].isObject())
                        throw McpToolError{"Each condition must be an object."};
                    QJsonObject o = conds[i].toObject();
                    if (!o.contains("id"))
                        o["id"] = i + 1;
                    DpCondition c;
                    if (!DpCondition::fromLine(QString::fromUtf8(QJsonDocument(o).toJson(
                            QJsonDocument::Compact)), &c) || c.kind == DpCondition::UNKNOWN)
                        throw McpToolError{QString("Datapack condition %1 cannot be read (kinds: "
                            "biome_in_area, biome_at, biome_share, structure_in_area; id 1-99).")
                            .arg(i + 1)};
                    s.dpcv.push_back(c);
                    job->summary.append(dpConditionText(c, DpBiomeNames{w.wi.dp}));
                }
                s.datapack = true;
                for (const QString& path : w.packs->paths)
                    s.dppacks.append(QFileInfo(path).fileName());
                s.dpdimension = w.dimensionId;
            }
            else
            {
                for (int i = 0; i < conds.size(); i++)
                {
                    if (!conds[i].isObject())
                        throw McpToolError{"Each condition must be an object."};
                    s.cv.push_back(parseCondition(conds[i].toObject(), w.wi.mc, i + 1));
                }
                for (const Condition& c : s.cv)
                    job->summary.append(QString("[%1] %2").arg(c.save, 2, 10, QChar('0'))
                        .arg(conditionSentence(c, w.wi.mc, &s.cv)));
            }
            if (args.contains("seeds") && !args.value("seeds").isNull())
            {
                s.slist = parseSeedList(args.value("seeds"));
                s.sc.searchtype = SEARCH_LIST;
            }
            else
                s.sc.searchtype = SEARCH_INC;
            if (args.contains("start_seed") && !args.value("start_seed").isNull())
            {
                if (s.sc.searchtype == SEARCH_LIST)
                    throw McpToolError{"start_seed is for searches without a seed list."};
                s.sc.startseed = parseSeed(args.value("start_seed"), "start_seed");
            }
            job->maxResults = int(argInt(args, "max_results", 1000, 1, MAX_SEARCH_RESULTS));
            const int minutes = int(argInt(args, "max_minutes", server->settings().searchMinutes,
                1, server->settings().searchMinutes));
            job->limitMs = qint64(minutes) * 60000;
            // by default half the budget, so that a second search can run
            const int budget = server->settings().searchThreads;
            const int wanted = int(argInt(args, "threads", std::max(1, budget / 2), 1, budget));
            job->world = worldJson(w);
            job->world.remove("seed");

            {
                QMutexLocker lock(&mutex);
                if (searches.size() >= MAX_SEARCHES)
                {
                    // forget the oldest search that has ended
                    QString drop;
                    for (auto it = searches.begin(); it != searches.end() && drop.isEmpty(); ++it)
                    {
                        QMutexLocker jl(&it.value()->m);
                        if (it.value()->ended || it.value()->state == "failed")
                            drop = it.key();
                    }
                    if (drop.isEmpty())
                        throw McpToolError{QString("%1 searches are running: stop one first.")
                            .arg(MAX_SEARCHES)};
                    std::shared_ptr<McpSearchJob> old = searches.take(drop);
                    lock.unlock();
                    old->shutdown();
                }
            }
            job->threads = reserveThreads(wanted);
            if (job->threads <= 0)
                throw McpToolError{QString("All %1 search threads that the app's MCP settings "
                    "allow are in use: stop a search first.").arg(server->settings().searchThreads)};
            s.sc.threads = job->threads;
            {
                QMutexLocker lock(&mutex);
                job->id = QString("s%1").arg(nextSearch++);
            }
            QString error;
            if (!job->start(&error))
            {
                job->shutdown(); // (gives the threads back)
                throw McpToolError{"The search cannot start: " + error};
            }
            {
                QMutexLocker lock(&mutex);
                searches.insert(job->id, job);
            }
            QJsonObject o = job->statusJson();
            return result(o, QString("Search %1 started with %2 thread(s).").arg(job->id)
                .arg(job->threads));
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- search_status
    {
        Tool t;
        t.name = "search_status";
        t.title = "Search status";
        t.description = "The state of a search (running, finished, stopped), its progress and "
            "the number of matching seeds so far, with the first ones. Without search_id: all "
            "searches of this session.";
        QJsonObject p;
        p["search_id"] = prop("string", "The id from start_search.");
        t.schema = schemaObject(p, {});
        t.run = [this](const QJsonObject& args, McpCall *) {
            if (!args.contains("search_id"))
            {
                QJsonArray list;
                QMap<QString, std::shared_ptr<McpSearchJob>> jobs;
                {
                    QMutexLocker lock(&mutex);
                    jobs = searches;
                }
                for (const auto& job : std::as_const(jobs))
                    list.append(job->statusJson());
                QJsonObject o;
                o["searches"] = list;
                return result(o);
            }
            std::shared_ptr<McpSearchJob> job = findJob(this, args);
            QJsonObject o = job->statusJson();
            QJsonArray first;
            {
                QMutexLocker lock(&job->m);
                for (size_t i = 0; i < job->results.size() && i < 20; i++)
                    first.append(seedText(job->results[i]));
            }
            o["first_results"] = first;
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- search_results
    {
        Tool t;
        t.name = "search_results";
        t.title = "Search results";
        t.description = "The matching seeds of a search so far (as decimal strings), in the "
            "order they were found, a page at a time.";
        QJsonObject p;
        p["search_id"] = prop("string", "The id from start_search.");
        p["offset"] = prop("integer", "Skip this many results (default 0).");
        p["limit"] = prop("integer", QString("At most this many (default 1000, at most %1).")
            .arg(MAX_RESULT_PAGE));
        t.schema = schemaObject(p, { "search_id" });
        t.run = [this](const QJsonObject& args, McpCall *) {
            std::shared_ptr<McpSearchJob> job = findJob(this, args);
            const int64_t offset = argInt(args, "offset", 0, 0, MAX_SEARCH_RESULTS);
            const int64_t limit = argInt(args, "limit", 1000, 1, MAX_RESULT_PAGE);
            QJsonArray seeds;
            int64_t total = 0;
            QString state;
            {
                QMutexLocker lock(&job->m);
                total = int64_t(job->results.size());
                state = job->state;
                for (int64_t i = offset; i < total && i < offset + limit; i++)
                    seeds.append(seedText(job->results[size_t(i)]));
            }
            QJsonObject o;
            o["search_id"] = job->id;
            o["state"] = state;
            o["total"] = double(total);
            o["offset"] = double(offset);
            o["seeds"] = seeds;
            o["more"] = offset + seeds.size() < total;
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- stop_search
    {
        Tool t;
        t.name = "stop_search";
        t.title = "Stop a search";
        t.description = "Stops a running search. Its results stay available (search_results) "
            "unless discard is true.";
        QJsonObject p;
        p["search_id"] = prop("string", "The id from start_search.");
        p["discard"] = prop("boolean", "Also forget the search and its results (default false).");
        t.schema = schemaObject(p, { "search_id" });
        t.run = [this](const QJsonObject& args, McpCall *) {
            std::shared_ptr<McpSearchJob> job = findJob(this, args);
            const bool discard = argBool(args, "discard", false);
            job->stop("stopped by the client");
            QJsonObject o = job->statusJson();
            if (discard)
            {
                {
                    QMutexLocker lock(&mutex);
                    searches.remove(job->id);
                }
                job->shutdown();
                o["discarded"] = true;
            }
            return result(o);
        };
        tools.push_back(t);
    }

    // ---------------------------------------------------------- export_search
    {
        Tool t;
        t.name = "export_search";
        t.title = "Export search results";
        t.readOnly = false;
        t.description = "Writes the results of a search to a file the client names: a "
            "session file the app can open (format \"session\", with the conditions) or one "
            "seed per line (\"seeds\"). Only if the app's MCP settings allow writing files.";
        QJsonObject p;
        p["search_id"] = prop("string", "The id from start_search.");
        p["path"] = prop("string", "Absolute path of the file to write.");
        QJsonObject fmt = prop("string", "\"session\" (default) or \"seeds\".");
        fmt["enum"] = QJsonArray{ "session", "seeds" };
        p["format"] = fmt;
        p["overwrite"] = prop("boolean", "Replace an existing file (default false).");
        t.schema = schemaObject(p, { "search_id", "path" });
        t.run = [this](const QJsonObject& args, McpCall *) {
            if (server->settings().rights != McpSettings::WRITE_FILES)
                throw McpToolError{"Writing files is not allowed: the app's MCP settings give "
                    "clients \"read and compute\" rights. Change it in Seed Atlas under "
                    "Settings, MCP, then restart the MCP server."};
            std::shared_ptr<McpSearchJob> job = findJob(this, args);
            const QString path = argString(args, "path");
            const QString format = argString(args, "format", "session");
            if (format != "session" && format != "seeds")
                throw McpToolError{"format must be \"session\" or \"seeds\"."};
            if (path.isEmpty() || !QDir::isAbsolutePath(path))
                throw McpToolError{"path must be an absolute path."};
            const QFileInfo info(path);
            if (!info.dir().exists())
                throw McpToolError{"The folder of path does not exist."};
            if (info.exists() && (info.isDir() || !argBool(args, "overwrite", false)))
                throw McpToolError{"The file exists (pass overwrite to replace it)."};
            Session s = job->session;
            {
                QMutexLocker lock(&job->m);
                s.slist = job->results;
                s.sc.startseed = job->current;
            }
            // a session for the file: the seeds as results, not as a seed list
            s.sc.searchtype = s.sc.searchtype == SEARCH_LIST ? SEARCH_INC : s.sc.searchtype;
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
                throw McpToolError{"The file cannot be written: " + file.errorString()};
            QTextStream stream(&file);
            if (format == "session")
                s.save(nullptr, stream);
            else
            {
                for (uint64_t seed : s.slist)
                    stream << seedText(seed) << "\n";
            }
            stream.flush();
            if (!file.commit())
                throw McpToolError{"The file cannot be written: " + file.errorString()};
            QJsonObject o;
            o["search_id"] = job->id;
            o["path"] = QFileInfo(path).absoluteFilePath();
            o["format"] = format;
            o["seeds"] = double(s.slist.size());
            return result(o, QString("Wrote %1 seeds to %2.").arg(s.slist.size()).arg(path));
        };
        tools.push_back(t);
    }
}
