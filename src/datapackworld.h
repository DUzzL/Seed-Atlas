#ifndef DATAPACKWORLD_H
#define DATAPACKWORLD_H

// Datapack worlds in the application: the list of data packs the user has
// loaded, and the world that the datapack generator (src/datapack, namespace
// dp) builds from them for the selected version.
//
// A world is a datapack world when at least one pack of the list is switched
// on and its file exists. Then WorldInfo::dp points to a DatapackWorld, an
// immutable snapshot of everything that defines the world besides seed and y:
// the packs, the world preset, the dimension and the loaded generator. Every
// change makes a new snapshot, and WorldInfo::equals() compares the pointer,
// so the map and its caches notice any change. Without packs WorldInfo::dp is
// null and nothing differs from the built-in generator.

#include "datapack/dpworld.h"

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

class QSettings;
class QThread;
class VanillaData;

// One entry of the pack list, in load order.
struct PackEntry
{
    QString path;
    bool enabled = true;

    bool operator==(const PackEntry& o) const { return path == o.path && enabled == o.enabled; }
};

struct DatapackWorld
{
    enum State
    {
        Loading,            // the packs are being read
        Ready,              // the map can show the dimension
        Problem,            // see report: errors, or no dimension to show
        UnsupportedVersion, // no datapack worlds for this Minecraft version
        NoVanillaData,      // the vanilla data of the version is missing
    };
    State state = Loading;

    // what defines the world
    int mc = 0;
    QString preset;                 // "minecraft:normal", ...
    QString vanillaPath;
    std::vector<PackEntry> packs;   // the whole list, as the user arranged it
    QString dimensionId;            // the dimension the map shows

    // the result of loading
    std::shared_ptr<const dp::World> world;
    dp::Report report;
    int dimension = -1;             // index into world->dimensions(), or -1
    // whether the world preset changes the shown dimension: it does not when
    // a pack brings its own file for that dimension
    bool presetMatters = true;

    // for the map, per biome of world->biomes()
    std::vector<QRgb> colors;
    // display name; with the id where two biomes have the same name
    std::vector<QString> labels;

    // the map can show this world
    bool usable() const { return state == Ready && world && dimension >= 0; }
    const dp::DimensionInfo *dimensionInfo() const
    {
        return usable() ? &world->dimensions()[dimension] : nullptr;
    }
    // the lowest and the highest block level of the dimension
    int minY() const { const dp::DimensionInfo *d = dimensionInfo(); return d ? d->minY : -64; }
    int maxY() const { const dp::DimensionInfo *d = dimensionInfo(); return d ? d->minY + d->height - 1 : 319; }
    // what the empty map says while the world cannot be shown (two lines)
    QString stateText() const;
    // the same in a few words, for the status strip of the map
    QString shortText() const;
};

// The world preset of a world type (WorldType in config.h).
QString datapackPreset(int worldType);
// Whether a path can be a data pack: a folder, or a zip or jar file.
bool isDatapackPath(const QString& path);

// A sampler of the calling thread for the world and seed. It is created on
// first use and kept until the thread asks for another world or seed, or
// ends. Returns null if the world cannot be shown.
dp::Sampler *datapackSampler(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed);
// Lets the calling thread drop its sampler (and with it the world).
void releaseDatapackSampler();

class DatapackManager : public QObject
{
    Q_OBJECT
public:
    explicit DatapackManager(VanillaData *vanilla, QObject *parent = nullptr);
    ~DatapackManager();

    VanillaData *vanillaData() const { return vanilla; }

    // --- the list of packs (kept in the settings, not in sessions) ---
    const std::vector<PackEntry>& entries() const { return list; }
    // Adds packs at the end of the list; paths that are already listed (also
    // under another name, e.g. through a link) or cannot be packs are
    // skipped. Returns the number of packs added.
    int addPacks(const QStringList& paths);
    void removePack(int index);
    void movePack(int index, int delta);
    void setPackEnabled(int index, bool enabled);
    void clearPacks();
    // whether the file or folder of an entry exists
    bool packExists(int index) const;
    // What is known about a pack from the last time it was loaded (name,
    // description, ...); null if it has not been loaded in this run.
    const dp::PackInfo *packInfo(const QString& path) const;

    // The dimension the map shows, e.g. "minecraft:overworld". A dimension
    // the world does not have or cannot show falls back to the first one it
    // can show; the choice itself is kept.
    QString dimension() const { return dimensionChoice; }
    void setDimension(const QString& id);

    // at least one pack is switched on and exists: worlds are datapack worlds
    bool active() const;

    // The world for a version and world type: null if no datapack world is
    // active. Starts loading if this is not the world loaded last; the
    // snapshot is then in the state Loading, and worldChanged() follows.
    std::shared_ptr<const DatapackWorld> resolve(int mc, int worldType);
    // the snapshot resolve() returned last (null if not active)
    std::shared_ptr<const DatapackWorld> current() const { return snapshot; }
    // Reads the packs again, e.g. after a pack file was replaced.
    void reload();
    // a load runs or waits to run
    bool loading() const { return !threads.isEmpty() || queued; }
    // Blocks until no load is running and its result has arrived (tests).
    void waitForLoaded();
    // the number of loads started so far (tests)
    int loadsStarted() const { return started; }

    // While a hold exists, resolve() makes snapshots but starts no load; the
    // last world asked for is loaded when the hold ends. At the start of the
    // app the packs are known before the stored version is: without a hold
    // they would be loaded for the version picker's default first.
    class Hold
    {
    public:
        explicit Hold(DatapackManager *manager) : manager(manager) { manager->holds++; }
        ~Hold() { manager->endHold(); }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
    private:
        DatapackManager *manager;
    };

    void load(QSettings& settings);
    void save(QSettings& settings) const;

signals:
    // the list, a switch or the dimension changed
    void listChanged();
    // current() changed or will change with the next resolve(): the map has
    // to be set up again
    void worldChanged();

private:
    struct LoadKey
    {
        int mc = 0;
        QString preset;
        QString vanillaPath;
        std::vector<PackEntry> packs;
        // whether the file of each pack exists: a pack that comes back (a
        // drive mounted again, a folder renamed back) is loaded again
        std::vector<bool> present;
        bool operator==(const LoadKey& o) const
        {
            return mc == o.mc && preset == o.preset && vanillaPath == o.vanillaPath
                && packs == o.packs && present == o.present;
        }
    };
    struct LoadResult
    {
        std::shared_ptr<const dp::World> world;
        dp::Report report;
    };

    // Starts the load of @key, or queues it: besides the newest load at most
    // one older one runs (there is no way to stop a load yet), and nothing
    // starts during a hold.
    void startLoad(const LoadKey& key);
    void launch(const LoadKey& key);
    void startQueued();
    void endHold();
    void finishLoad(const LoadKey& key, const std::shared_ptr<LoadResult>& result);
    std::shared_ptr<const DatapackWorld> makeSnapshot(const LoadKey& key,
        const LoadResult *result, DatapackWorld::State state) const;
    void changed(bool listToo);

    VanillaData *vanilla;
    std::vector<PackEntry> list;
    QString dimensionChoice;

    LoadKey snapshotKey;                // what the snapshot was made for
    bool snapshotKeyValid = false;
    QString snapshotChoice;             // the dimension choice it was made for
    std::shared_ptr<const DatapackWorld> snapshot;
    std::shared_ptr<LoadResult> loaded; // the result behind a loaded snapshot
    LoadKey pendingKey;
    quint64 generation = 0;
    QList<QThread*> threads;
    LoadKey queuedKey;          // a load that waits (see startLoad)
    bool queued = false;
    int holds = 0;
    int started = 0;
    QHash<QString, dp::PackInfo> infos;
};

#endif // DATAPACKWORLD_H
