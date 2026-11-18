#ifndef MCPTOOLS_H
#define MCPTOOLS_H

// The tools of the MCP server (see mcpserver.h): their schemas and what they
// do. They reuse the app's code: the engine and QWorld helpers for queries,
// the analysis threads of the Biomes, Structures and End Portals tools,
// SearchMaster and the session format for searches, and the datapack
// generator for datapack worlds.
//
// Every argument comes from an untrusted client: ranges are checked, and
// results that could grow without bound (areas, lists, seeds) are capped.

#include "config.h"
#include "searchthread.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class McpServer;
struct DatapackWorld;
namespace dp { class World; struct Report; }

// One tools/call in progress.
struct McpCall
{
    QJsonValue id;
    QJsonValue progressToken;       // null: the client wants no progress
    std::atomic_bool cancel{false}; // notifications/cancelled arrived
    McpServer *server = nullptr;
    // under the server's write lock
    bool done = false;
    double lastProgress = -1;
    qint64 lastProgressMs = -1000000;
    QElapsedTimer clock;

    // a progress notification (rate limited, only increasing values)
    void progress(double done, double total, const QString& message = QString());
};

// A tool error: the call ends with isError and this text.
struct McpToolError
{
    QString message;
};

// A loaded set of datapacks (a world handle of load_datapacks).
struct McpDatapacks
{
    QString id;
    int mc = 0;
    int worldType = WORLD_DEFAULT;
    QString vanillaSource;          // where the vanilla data came from
    QStringList paths;
    std::shared_ptr<const dp::World> world;
    std::shared_ptr<dp::Report> report;
    // snapshots per dimension id (as DatapackWorld for the app's code)
    QMutex mutex;
    QMap<QString, std::shared_ptr<const DatapackWorld>> snapshots;
};

class McpSearchJob;

class McpRegistry
{
public:
    explicit McpRegistry(McpServer *server);
    ~McpRegistry();

    QJsonArray toolList() const;
    bool hasTool(const QString& name) const;
    // Runs a tool (in a worker thread). Returns the result of tools/call;
    // tool errors are results with isError.
    QJsonObject call(const QString& name, const QJsonObject& args, McpCall *call);
    // Stops all searches (the server ends).
    void shutdown();
    // Stops the searches in a datapack world (it is unloaded).
    void stopSearchesIn(const QString& world);

    // the search thread budget
    int reserveThreads(int wanted);
    void releaseThreads(int n);

    struct Tool
    {
        QString name;
        QString title;
        QString description;
        QJsonObject schema;
        bool readOnly = true;       // writes no files
        std::function<QJsonObject(const QJsonObject&, McpCall*)> run;
    };

private:
    void addTools();
    void addSearchTools();
    void addDatapackTools();

public:
    // (used by the tool implementations)
    McpServer *server;
    std::vector<Tool> tools;

    QMutex mutex;
    int threadsInUse = 0;
    int nextDatapacks = 1;
    int nextSearch = 1;
    QMap<QString, std::shared_ptr<McpDatapacks>> datapacks;
    QMap<QString, std::shared_ptr<McpSearchJob>> searches;

    std::shared_ptr<McpDatapacks> findDatapacks(const QString& id);
};

// --- helpers shared by the tool implementations (mcptools.cpp) ---

namespace mcp {

// limits
constexpr int MAX_COORD = 30000000;         // the world border
constexpr int64_t MAX_MAP_CELLS = 65536;    // biome_map
constexpr int64_t MAX_STAT_CELLS = int64_t(1) << 24;    // per seed, analyses
constexpr int MAX_STRUCT_AREA_SIDE = 65536;     // structures_in_area
constexpr int MAX_STRUCT_RESULTS = 5000;
constexpr int MAX_NEAREST_RADIUS = 50000;
constexpr int MAX_POINTS = 1000;
constexpr int MAX_STAT_SEEDS = 64;
constexpr int MAX_DATAPACK_WORLDS = 8;
constexpr int MAX_PACKS = 64;
constexpr int MAX_SEARCHES = 32;            // kept, running or not
constexpr int MAX_LIST_SEEDS = 1000000;
constexpr int MAX_SEARCH_RESULTS = 100000;
constexpr int MAX_RESULT_PAGE = 5000;

// argument readers; they throw McpToolError
int64_t argInt(const QJsonObject& args, const char *key, int64_t def, int64_t lo, int64_t hi);
int64_t argIntReq(const QJsonObject& args, const char *key, int64_t lo, int64_t hi);
bool argBool(const QJsonObject& args, const char *key, bool def);
QString argString(const QJsonObject& args, const char *key, const QString& def = QString());
uint64_t parseSeed(const QJsonValue& v, const QString& what);
QString seedText(uint64_t seed);

// The world a call is about.
struct World
{
    WorldInfo wi;           // mc, world type, seed (wi.dp: the datapack world)
    int dim = DIM_OVERWORLD;    // engine dimension (also for datapack worlds)
    std::shared_ptr<McpDatapacks> packs;
    QString dimensionId;    // "minecraft:overworld", ...
    bool datapack() const { return wi.dp != nullptr; }
};
// Reads seed, mc, world_type, dimension and world (datapack handle).
// @needSeed: false for tools that take several seeds.
World parseWorld(McpRegistry *reg, const QJsonObject& args, bool needSeed = true);
QJsonObject worldJson(const World& w);

// a biome id as "minecraft:plains" (vanilla) or the pack's id
QString biomeName(const World& w, int id);
// a vanilla biome by name ("plains" or "minecraft:plains"), -1 if unknown
int vanillaBiomeId(int mc, const QString& name);

// the JSON schema snippets of the common arguments
QJsonObject worldSchemaProperties(bool withSeed);
QJsonObject schemaObject(const QJsonObject& properties, const QStringList& required);

// a result: structured content plus the same JSON as text
QJsonObject result(const QJsonObject& data, const QString& summary = QString());
QJsonObject errorResult(const QString& message);

} // namespace mcp

#endif // MCPTOOLS_H
