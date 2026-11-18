#include "datapackworld.h"

#include "config.h"
#include "vanilladata.h"
#include "seedatlas-engine/util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QThread>

namespace {

struct SamplerSlot
{
    std::shared_ptr<const DatapackWorld> world;
    uint64_t seed = 0;
    std::unique_ptr<dp::Sampler> sampler;
};

thread_local SamplerSlot t_sampler;

} // namespace

QString datapackPreset(int worldType)
{
    switch (worldType)
    {
    case WORLD_LARGE_BIOMES:    return QStringLiteral("minecraft:large_biomes");
    case WORLD_AMPLIFIED:       return QStringLiteral("minecraft:amplified");
    default:                    return QStringLiteral("minecraft:normal");
    }
}

bool isDatapackPath(const QString& path)
{
    const QFileInfo info(path);
    if (info.isDir())
        return true;
    const QString suffix = info.suffix().toLower();
    return info.isFile() && (suffix == QLatin1String("zip") || suffix == QLatin1String("jar"));
}

dp::Sampler *datapackSampler(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed)
{
    if (!world || !world->usable())
        return nullptr;
    SamplerSlot& slot = t_sampler;
    if (!slot.sampler || slot.world != world || slot.seed != seed)
    {
        slot.sampler.reset(new dp::Sampler(world->world, world->dimension, seed));
        slot.world = world;
        slot.seed = seed;
    }
    return slot.sampler->valid() ? slot.sampler.get() : nullptr;
}

void releaseDatapackSampler()
{
    t_sampler = SamplerSlot();
}

QString DatapackWorld::stateText() const
{
    switch (state)
    {
    case Loading:
        return QCoreApplication::translate("DatapackWorld", "Loading the datapacks...");
    case Ready:
        return QCoreApplication::translate("DatapackWorld", "Datapack world");
    case UnsupportedVersion:
        return QCoreApplication::translate("DatapackWorld",
            "Datapack worlds are not available for Minecraft %1.\n"
            "Pick another version, or switch off the datapacks.").arg(mc2str(mc));
    case NoVanillaData:
        if (!VanillaData::dataInGameFiles(mc))  // (it cannot be downloaded)
            return QCoreApplication::translate("DatapackWorld",
                "The game data of Minecraft %1 is missing.\n"
                "Open Datapacks to see how to get it.").arg(mc2str(mc));
        return QCoreApplication::translate("DatapackWorld",
            "The game data of Minecraft %1 is missing.\n"
            "Open Datapacks to find or download it.").arg(mc2str(mc));
    case Problem:
        break;
    }
    return QCoreApplication::translate("DatapackWorld",
        "The datapacks could not be loaded.\nOpen Datapacks to see what is wrong.");
}

QString DatapackWorld::shortText() const
{
    switch (state)
    {
    case Loading:
        return QCoreApplication::translate("DatapackWorld", "Loading the datapacks...");
    case Ready:
        return QCoreApplication::translate("DatapackWorld", "Datapack world");
    case UnsupportedVersion:
        return QCoreApplication::translate("DatapackWorld", "No datapack worlds for %1")
            .arg(mc2str(mc));
    case NoVanillaData:
        return QCoreApplication::translate("DatapackWorld", "Game data missing");
    case Problem:
        break;
    }
    return QCoreApplication::translate("DatapackWorld", "Datapacks not loaded");
}

DatapackManager::DatapackManager(VanillaData *vanilla, QObject *parent)
    : QObject(parent)
    , vanilla(vanilla)
    , dimensionChoice(QStringLiteral("minecraft:overworld"))
{
    if (vanilla)
    {
        connect(vanilla, &VanillaData::changed, this, [this]() {
            // another vanilla file, or the same file downloaded again
            snapshotKeyValid = false;
            changed(false);
        });
    }
}

DatapackManager::~DatapackManager()
{
    // The load threads only read files; let them end before the results go.
    // (A load cannot be stopped yet; a queued one is not started.)
    queued = false;
    for (QThread *thread : std::as_const(threads))
    {
        thread->disconnect(this);
        thread->wait();
        delete thread;
    }
}

void DatapackManager::changed(bool listToo)
{
    if (listToo)
        emit listChanged();
    emit worldChanged();
}

namespace {

// A pack path as the list keeps it: absolute, without "." parts and without
// a slash at the end (which would leave the pack without a file name).
QString packPath(const QString& given)
{
    return QDir::cleanPath(QFileInfo(given).absoluteFilePath());
}

// Whether @path names a pack of @list, also under another name (a link).
bool listedPack(const std::vector<PackEntry>& list, const QString& path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (const PackEntry& entry : list)
    {
        if (entry.path == path)
            return true;
        // (a missing file has no canonical path)
        if (!canonical.isEmpty() && QFileInfo(entry.path).canonicalFilePath() == canonical)
            return true;
    }
    return false;
}

} // namespace

int DatapackManager::addPacks(const QStringList& paths)
{
    int added = 0;
    for (const QString& given : paths)
    {
        if (!isDatapackPath(given))
            continue;
        const QString path = packPath(given);
        if (listedPack(list, path))
            continue;
        PackEntry entry;
        entry.path = path;
        list.push_back(entry);
        added++;
    }
    if (added)
        changed(true);
    return added;
}

void DatapackManager::removePack(int index)
{
    if (index < 0 || index >= (int) list.size())
        return;
    list.erase(list.begin() + index);
    changed(true);
}

void DatapackManager::movePack(int index, int delta)
{
    const int target = index + delta;
    if (index < 0 || index >= (int) list.size() || target < 0 || target >= (int) list.size()
        || delta == 0)
        return;
    std::swap(list[index], list[target]);
    changed(true);
}

void DatapackManager::setPackEnabled(int index, bool enabled)
{
    if (index < 0 || index >= (int) list.size() || list[index].enabled == enabled)
        return;
    list[index].enabled = enabled;
    changed(true);
}

void DatapackManager::clearPacks()
{
    if (list.empty())
        return;
    list.clear();
    changed(true);
}

bool DatapackManager::packExists(int index) const
{
    return index >= 0 && index < (int) list.size() && QFileInfo::exists(list[index].path);
}

const dp::PackInfo *DatapackManager::packInfo(const QString& path) const
{
    auto it = infos.constFind(path);
    return it == infos.constEnd() ? nullptr : &it.value();
}

void DatapackManager::setDimension(const QString& id)
{
    if (id.isEmpty() || id == dimensionChoice)
        return;
    dimensionChoice = id;
    changed(true);
}

bool DatapackManager::active() const
{
    for (const PackEntry& entry : list)
    {
        if (entry.enabled && QFileInfo::exists(entry.path))
            return true;
    }
    return false;
}

std::shared_ptr<const DatapackWorld> DatapackManager::makeSnapshot(const LoadKey& key,
    const LoadResult *result, DatapackWorld::State state) const
{
    std::shared_ptr<DatapackWorld> w = std::make_shared<DatapackWorld>();
    w->state = state;
    w->mc = key.mc;
    w->preset = key.preset;
    w->vanillaPath = key.vanillaPath;
    w->packs = key.packs;
    w->dimensionId = dimensionChoice;
    if (!result)
        return w;

    w->world = result->world;
    w->report = result->report;
    if (!result->report.ok())
    {
        w->state = DatapackWorld::Problem;
        return w;
    }
    // the chosen dimension, or else the first one the map can show
    const std::vector<dp::DimensionInfo>& dims = w->world->dimensions();
    int dim = w->world->dimension(dimensionChoice);
    if (dim < 0 || !dims[dim].supported)
    {
        dim = -1;
        for (int i = 0; i < (int) dims.size() && dim < 0; i++)
            if (dims[i].supported)
                dim = i;
    }
    if (dim < 0)
    {
        w->state = DatapackWorld::Problem;
        return w;
    }
    w->state = DatapackWorld::Ready;
    w->dimension = dim;
    w->dimensionId = dims[dim].id;
    w->presetMatters = !dims[dim].packFile;

    const std::vector<dp::BiomeInfo>& biomes = w->world->biomes();
    QHash<QString, int> names;
    for (const dp::BiomeInfo& biome : biomes)
        names[biome.name]++;
    w->colors.reserve(biomes.size());
    w->labels.reserve(biomes.size());
    for (const dp::BiomeInfo& biome : biomes)
    {
        w->colors.push_back(biome.color);
        w->labels.push_back(names.value(biome.name) > 1
            ? QStringLiteral("%1 (%2)").arg(biome.name, biome.id) : biome.name);
    }
    return w;
}

std::shared_ptr<const DatapackWorld> DatapackManager::resolve(int mc, int worldType)
{
    if (!active())
    {
        if (snapshot)
        {
            snapshot.reset();
            loaded.reset();
            snapshotKeyValid = false;
            generation++; // drop the result of a running load
            queued = false;
        }
        return nullptr;
    }

    LoadKey key;
    key.mc = mc;
    key.preset = datapackPreset(worldType);
    key.packs = list;
    for (const PackEntry& entry : list)
        key.present.push_back(QFileInfo::exists(entry.path));
    const bool supported = dp::versionSupported(mc) && !VanillaData::gameVersion(mc).isEmpty();
    if (supported && vanilla)
        key.vanillaPath = vanilla->find(mc).path;

    if (snapshot && snapshotKeyValid && snapshotKey == key)
    {
        if (snapshotChoice == dimensionChoice || !loaded)
            return snapshot;
        // only another dimension of the loaded world
        std::shared_ptr<const DatapackWorld> next = makeSnapshot(key, loaded.get(),
            DatapackWorld::Ready);
        snapshotChoice = dimensionChoice;
        if (next->dimension != snapshot->dimension || next->state != snapshot->state)
            snapshot = next;
        return snapshot;
    }

    snapshotKey = key;
    snapshotKeyValid = true;
    snapshotChoice = dimensionChoice;
    loaded.reset();
    generation++;
    queued = false; // a waiting load is for an older world
    if (!supported)
        snapshot = makeSnapshot(key, nullptr, DatapackWorld::UnsupportedVersion);
    else if (key.vanillaPath.isEmpty())
        snapshot = makeSnapshot(key, nullptr, DatapackWorld::NoVanillaData);
    else
    {
        snapshot = makeSnapshot(key, nullptr, DatapackWorld::Loading);
        startLoad(key);
    }
    // the panel shows the new state (the caller already has the snapshot)
    QMetaObject::invokeMethod(this, &DatapackManager::listChanged, Qt::QueuedConnection);
    return snapshot;
}

void DatapackManager::reload()
{
    snapshotKeyValid = false;
    changed(false);
}

void DatapackManager::startLoad(const LoadKey& key)
{
    if (holds > 0 || threads.size() >= 2)
    {
        queuedKey = key;
        queued = true;
        return;
    }
    launch(key);
}

void DatapackManager::startQueued()
{
    if (queued && holds == 0 && threads.size() < 2)
    {
        queued = false;
        launch(queuedKey);
    }
}

void DatapackManager::endHold()
{
    if (holds > 0 && --holds == 0)
        startQueued();
}

void DatapackManager::launch(const LoadKey& key)
{
    // (a queued key is replaced or dropped whenever the generation changes,
    // so it belongs to the current one)
    const quint64 gen = generation;
    started++;
    dp::Request request;
    request.mc = key.mc;
    request.vanillaPath = key.vanillaPath;
    request.preset = key.preset;
    for (int i = 0; i < (int) key.packs.size(); i++)
    {
        const PackEntry& entry = key.packs[i];
        // a pack whose file is gone stays in the list, but cannot be loaded
        if (i < (int) key.present.size() && !key.present[i])
            continue;
        dp::PackSource source;
        source.path = entry.path;
        source.enabled = entry.enabled;
        request.packs.push_back(source);
    }

    std::shared_ptr<LoadResult> result = std::make_shared<LoadResult>();
    QThread *thread = QThread::create([request, result]() {
        result->world = dp::World::load(request, &result->report);
    });
    thread->setObjectName("datapack loader");
    // The generator limits the nesting of what it compiles; the default
    // stack of secondary threads (512 KiB on macOS) is enough for that in
    // release builds, not in debug or sanitizer builds.
    thread->setStackSize(8u << 20);
    connect(thread, &QThread::finished, this, [this, thread, gen, key, result]() {
        threads.removeOne(thread);
        thread->deleteLater();
        // (the result of an older request is dropped)
        if (gen == generation)
        {
            finishLoad(key, result);
            changed(true);
        }
        startQueued();
    });
    threads.append(thread);
    thread->start();
}

void DatapackManager::finishLoad(const LoadKey& key, const std::shared_ptr<LoadResult>& result)
{
    loaded = result;
    for (const dp::PackInfo& info : result->world->packs())
        infos.insert(info.path, info);
    snapshotChoice = dimensionChoice;
    snapshot = makeSnapshot(key, result.get(), DatapackWorld::Ready);
}

void DatapackManager::waitForLoaded()
{
    // (a finished load starts the queued one from the event loop)
    while (!threads.isEmpty())
    {
        threads.first()->wait();
        // the finished handlers run from the event loop
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
}

void DatapackManager::load(QSettings& settings)
{
    list.clear();
    const int count = settings.beginReadArray("datapacks/packs");
    for (int i = 0; i < count; i++)
    {
        settings.setArrayIndex(i);
        PackEntry entry;
        entry.path = settings.value("path").toString();
        entry.enabled = settings.value("enabled", true).toBool();
        if (entry.path.isEmpty())
            continue;
        entry.path = packPath(entry.path);
        if (!listedPack(list, entry.path))
            list.push_back(entry);
    }
    settings.endArray();
    dimensionChoice = settings.value("datapacks/dimension", dimensionChoice).toString();
    if (dimensionChoice.isEmpty())
        dimensionChoice = QStringLiteral("minecraft:overworld");
    snapshotKeyValid = false;
    changed(true);
}

void DatapackManager::save(QSettings& settings) const
{
    settings.remove("datapacks/packs");
    settings.beginWriteArray("datapacks/packs", (int) list.size());
    for (int i = 0; i < (int) list.size(); i++)
    {
        settings.setArrayIndex(i);
        settings.setValue("path", list[i].path);
        settings.setValue("enabled", list[i].enabled);
    }
    settings.endArray();
    settings.setValue("datapacks/dimension", dimensionChoice);
}
