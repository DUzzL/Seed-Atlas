#include "tabstructures.h"
#include "ui_tabstructures.h"

#include "fortresslayout.h"
#include "message.h"
#include "seedtables.h"
#include "util.h"
#include "seedatlas-engine/quadbase.h"

#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QIntValidator>
#include <QTextStream>
#include <QTreeWidgetItem>

#include <map>
#include <iterator>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>


enum { C_SEED, C_STRUCT, C_COUNT, C_X, C_Z, C_DETAIL }; // structure/density columns
enum {
    F_SEED, F_LAYOUT, F_START_X, F_START_Z, F_MATCH_X, F_MATCH_Z,
    F_ORIENTATION, F_QUALIFYING, F_SIZE, F_SPAWNERS, F_WART
};

static QString fortressLayoutName(FortressLayoutType type)
{
    return type == FORTRESS_LAYOUT_2X2 ? QStringLiteral("2x2")
                                      : QStringLiteral("3x1");
}

static QString fortressOrientationName(FortressLineOrientation orientation)
{
    if (orientation == FORTRESS_LINE_X) return QStringLiteral("X");
    if (orientation == FORTRESS_LINE_Z) return QStringLiteral("Z");
    return QString();
}

class TreeIntItem : public QTreeWidgetItem
{
public:
    TreeIntItem(QTreeWidget *parent = nullptr) : QTreeWidgetItem(parent) {}
    bool operator< (const QTreeWidgetItem& x) const
    {
        int col = treeWidget()->sortColumn();
        if (col == C_SEED)
            return data(col, Qt::UserRole).toLongLong() < x.data(col, Qt::UserRole).toLongLong();
        return QTreeWidgetItem::operator< (x);
    }
};

void AnalysisStructures::run()
{
    workers.reset();
    workDone = 0;
    workTotal = 0;
    resultCount = 0;
    workersUsed = 0;

    if (fortress)
    {
        std::stable_sort(seeds.begin(), seeds.end(), [](uint64_t a, uint64_t b) {
            return static_cast<int64_t>(a) < static_cast<int64_t>(b);
        });
        seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
    }

    const size_t hw = mappingThreadCount();
    if (seeds.size() >= hw)
    {
        idx = 0;
        struct Result { size_t order; QTreeWidgetItem *item; int kind; };
        std::atomic_size_t next(0);
        std::mutex resultsMutex;
        std::vector<Result> results;
        const size_t workerCount = std::min<size_t>(seeds.size(), hw);
        workersUsed = workerCount;

        auto scan = [&](size_t) {
            Generator g;
            setupGenerator(&g, wi.mc, wi.large);
            while (!stop)
            {
                const size_t order = next.fetch_add(1);
                if (order >= seeds.size())
                    break;

                AnalysisStructures task;
                task.wi = wi;
                task.wi.seed = seeds[order];
                task.dim = dim;
                task.area = area;
                task.collect = collect;
                task.quad = quad;
                task.density = density;
                task.fortress = fortress;
                task.parallelInner = false;
                task.quadHuts = quadHuts;
                task.quadMonuments = quadMonuments;
                task.hutQuality = hutQuality;
                task.monumentCoverage = monumentCoverage;
                task.densityHuts = densityHuts;
                task.densityMonuments = densityMonuments;
                task.densityRadius = densityRadius;
                task.densityMinimum = densityMinimum;
                task.fortress2x2 = fortress2x2;
                task.fortress3x1 = fortress3x1;
                std::copy(std::begin(mapshow), std::end(mapshow), std::begin(task.mapshow));
                task.stop = false;
                task.cancel = &stop;
                QObject::connect(&task, &AnalysisStructures::itemDone,
                    &task,
                    [&](QTreeWidgetItem *item) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, item, false});
                    }, Qt::DirectConnection);
                QObject::connect(&task, &AnalysisStructures::quadDone,
                    &task,
                    [&](QTreeWidgetItem *item) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, item, true});
                    }, Qt::DirectConnection);
                QObject::connect(&task, &AnalysisStructures::densityDone,
                    &task,
                    [&](QTreeWidgetItem *item) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, item, 2});
                    }, Qt::DirectConnection);
                QObject::connect(&task, &AnalysisStructures::fortressDone,
                    &task,
                    [&](QTreeWidgetItem *item) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, item, 3});
                    }, Qt::DirectConnection);

                if (fortress)
                    task.runFortresses(&g);
                else if (density)
                    task.runDensity(&g);
                else if (quad)
                    task.runQuads(&g);
                else
                    task.runStructs(&g);
                resultCount.fetch_add(
                    task.resultCount.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                if (fortress)
                {
                    workDone.fetch_add(task.workDone.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                    workTotal.fetch_add(task.workTotal.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                }
                idx.fetch_add(1);
            }
        };

        runMappingWorkers(seeds.size(), workers, scan);

        if (stop.load(std::memory_order_relaxed))
        {
            for (const Result& result : results)
                delete result.item;
            return;
        }

        std::sort(results.begin(), results.end(), [&](const Result& a, const Result& b) {
            if (fortress)
            {
                const int64_t sa = static_cast<int64_t>(seeds[a.order]);
                const int64_t sb = static_cast<int64_t>(seeds[b.order]);
                if (sa != sb) return sa < sb;
            }
            return a.order < b.order;
        });
        for (const Result& result : results)
        {
            if (result.kind == 3)
                emit fortressDone(result.item);
            else if (result.kind == 2)
                emit densityDone(result.item);
            else if (result.kind)
                emit quadDone(result.item);
            else
                emit itemDone(result.item);
        }
        return;
    }

    Generator g;
    setupGenerator(&g, wi.mc, wi.large);
    parallelInner = true;

    for (idx = 0; idx < (long)seeds.size(); idx++)
    {
        if (stop) break;
        wi.seed = seeds[idx];
        if (fortress)
            runFortresses(&g);
        else if (density)
            runDensity(&g);
        else if (quad)
            runQuads(&g);
        else
            runStructs(&g);
    }
}

void AnalysisStructures::runStructs(Generator *g)
{
    QTreeWidgetItem *seeditem = new TreeIntItem();
    seeditem->setText(0, QString::asprintf("%" PRId64, wi.seed));
    seeditem->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
    seeditem->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_UNDEF));

    struct StructResult
    {
        int sopt = D_NONE;
        int stype = 0;
        int dim = DIM_UNDEF;
        StructureConfig config = {};
        uint64_t count = 0;
        std::vector<VarPos> positions;
    };
    std::vector<StructResult> results;
    for (int sopt = D_DESERT; sopt < D_SPAWN; sopt++)
    {
        if (!mapshow[sopt])
            continue;
        int stype = mapopt2stype(sopt);
        StructureConfig sconf;
        if (!getStructureConfig_override(stype, wi.mc, &sconf))
            continue;
        if (dim != DIM_UNDEF && dim != sconf.dim)
            continue;
        StructResult result;
        result.sopt = sopt;
        result.stype = stype;
        result.dim = sconf.dim;
        result.config = sconf;
        results.push_back(result);
    }

    struct ScanTask
    {
        size_t resultIndex;
        int x1, z1, x2, z2;
    };
    std::vector<ScanTask> tasks;
    const size_t hardwareThreads = mappingThreadCount();
    const size_t targetTasksPerType = parallelInner ? hardwareThreads * 8 : 1;
    for (size_t resultIndex = 0; resultIndex < results.size(); resultIndex++)
    {
        const int scale = results[resultIndex].config.regionSize * 16;
        const int si0 = floordiv(area.x1, scale);
        const int sj0 = floordiv(area.z1, scale);
        const int si1 = floordiv(area.x2, scale);
        const int sj1 = floordiv(area.z2, scale);
        const int64_t regionWidth = int64_t(si1) - si0 + 1;
        const int64_t regionHeight = int64_t(sj1) - sj0 + 1;
        const uint64_t regionCount = uint64_t(regionWidth) * uint64_t(regionHeight);
        const size_t parts = size_t(std::min<uint64_t>(targetTasksPerType, regionCount));
        int nx = std::max(1, int(std::sqrt(double(parts) * regionWidth /
            std::max<int64_t>(1, regionHeight))));
        int nz = std::max(1, int((parts + nx - 1) / nx));
        nx = int(std::min<int64_t>(nx, regionWidth));
        nz = int(std::min<int64_t>(nz, regionHeight));
        for (int ix = 0; ix < nx; ix++)
        {
            const int64_t sri0 = si0 + regionWidth * ix / nx;
            const int64_t sri1 = si0 + regionWidth * (ix + 1) / nx;
            const int tx1 = ix == 0 ? area.x1 : int(sri0 * scale);
            const int tx2 = ix == nx-1 ? area.x2 + 1 : int(sri1 * scale);
            for (int iz = 0; iz < nz; iz++)
            {
                const int64_t srj0 = sj0 + regionHeight * iz / nz;
                const int64_t srj1 = sj0 + regionHeight * (iz + 1) / nz;
                const int tz1 = iz == 0 ? area.z1 : int(srj0 * scale);
                const int tz2 = iz == nz-1 ? area.z2 + 1 : int(srj1 * scale);
                tasks.push_back({resultIndex, tx1, tz1, tx2, tz2});
            }
        }
    }

    const bool scanSpawn = mapshow[D_SPAWN] && (dim == DIM_UNDEF || dim == DIM_OVERWORLD);
    const bool scanStrongholds = mapshow[D_STRONGHOLD] && (dim == DIM_UNDEF || dim == DIM_OVERWORLD);
    workTotal = tasks.size() + scanSpawn + scanStrongholds;
    std::atomic_size_t nextTask(0);
    std::mutex mergeMutex;
    auto scanTasks = [&]() {
        while (!shouldStop())
        {
            const size_t index = nextTask.fetch_add(1, std::memory_order_relaxed);
            if (index >= tasks.size())
                break;
            ScanTask& task = tasks[index];
            StructResult& result = results[task.resultIndex];
            std::vector<VarPos> positions;
            const uint64_t count = getStructs(collect ? &positions : nullptr,
                result.config, wi, result.dim,
                task.x1, task.z1, task.x2, task.z2, false,
                cancel ? cancel : &stop,
                result.sopt == D_ENDCITYSHIP ? 1 : result.sopt == D_ENDCITY ? -1 : 0);
            if (!shouldStop())
            {
                std::lock_guard<std::mutex> lock(mergeMutex);
                result.count += count;
                resultCount.fetch_add(count, std::memory_order_relaxed);
                if (collect)
                {
                    result.positions.insert(result.positions.end(),
                        std::make_move_iterator(positions.begin()),
                        std::make_move_iterator(positions.end()));
                }
            }
            workDone.fetch_add(1, std::memory_order_relaxed);
        }
    };

    const size_t workerCount = parallelInner
        ? std::min<size_t>(tasks.size(), hardwareThreads)
        : std::min<size_t>(tasks.size(), 1);
    workersUsed = std::max<size_t>(workerCount, workTotal.load() > 0 ? 1 : 0);
    if (parallelInner)
        runMappingWorkers(workerCount, workers, [&](size_t) { scanTasks(); });
    else if (workerCount)
        scanTasks();

    if (shouldStop())
    {
        delete seeditem;
        return;
    }

    for (const StructResult& result : results)
    {
        if (shouldStop())
            break;
        const std::vector<VarPos>& st = result.positions;
        if (!result.count)
            continue;

        QTreeWidgetItem* stitem = new QTreeWidgetItem(seeditem);
        stitem->setText(C_SEED, "-");
        stitem->setText(C_STRUCT, mapopt2display(result.sopt));
        stitem->setData(C_COUNT, Qt::DisplayRole,
            QVariant::fromValue(static_cast<qulonglong>(result.count)));
        if (!collect)
            continue;

        for (size_t i = 0; i < st.size(); i++)
        {
            if (!(i & 63) && shouldStop())
            {
                delete seeditem;
                return;
            }
            const VarPos& vp = st[i];
            QTreeWidgetItem* item = new QTreeWidgetItem(stitem);
            item->setText(C_SEED, "-");
            item->setData(C_X, Qt::DisplayRole, QVariant::fromValue(vp.p.x));
            item->setData(C_Z, Qt::DisplayRole, QVariant::fromValue(vp.p.z));
            item->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
            item->setData(0, Qt::UserRole+1, QVariant::fromValue(result.dim));
            item->setData(0, Qt::UserRole+2, QVariant::fromValue(vp.p));
            QStringList sinfo = vp.detail();
            if (!sinfo.empty())
                item->setText(C_DETAIL, sinfo.join(":"));
        }
    }

    if (!shouldStop() && scanSpawn)
    {
        applySeed(g, 0, wi.seed);
        Pos pos = getSpawn(g);
        if (pos.x >= area.x1 && pos.x <= area.x2 && pos.z >= area.z1 && pos.z <= area.z2)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(seeditem);
            item->setText(C_SEED, "-");
            item->setText(C_STRUCT, "spawn");
            item->setData(C_COUNT, Qt::DisplayRole, QVariant::fromValue(1));
            item->setData(C_X, Qt::DisplayRole, QVariant::fromValue(pos.x));
            item->setData(C_Z, Qt::DisplayRole, QVariant::fromValue(pos.z));
            item->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
            item->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));
            item->setData(0, Qt::UserRole+2, QVariant::fromValue(pos));
            resultCount.fetch_add(1, std::memory_order_relaxed);
        }
        workDone.fetch_add(1, std::memory_order_relaxed);
    }

    if (!shouldStop() && scanStrongholds)
    {
        StrongholdIter sh;
        initFirstStronghold(&sh, wi.mc, wi.seed);
        std::vector<Pos> shp;
        applySeed(g, DIM_OVERWORLD, wi.seed);

        // get the maximum relevant ring number
        const int64_t rx1 = std::abs(int64_t(area.x1));
        const int64_t rx2 = std::abs(int64_t(area.x2));
        const int64_t rz1 = std::abs(int64_t(area.z1));
        const int64_t rz2 = std::abs(int64_t(area.z2));
        const int64_t xt = std::max(rx1, rx2) + 112+8;
        const int64_t zt = std::max(rz1, rz2) + 112+8;
        const int rmax = int((std::sqrt(double(xt*xt + zt*zt)) - 1408) / 3072);

        while (nextStronghold(&sh, g) > 0)
        {
            if (shouldStop() || sh.ringnum > rmax)
                break;
            Pos pos = sh.pos;
            if (pos.x >= area.x1 && pos.x <= area.x2 && pos.z >= area.z1 && pos.z <= area.z2)
                shp.push_back(pos);
        }

        if (!shp.empty())
        {
            resultCount.fetch_add(shp.size(), std::memory_order_relaxed);
            QTreeWidgetItem* stitem = new QTreeWidgetItem(seeditem);
            stitem->setText(C_SEED, "-");
            stitem->setText(C_STRUCT, "stronghold");
            stitem->setData(C_COUNT, Qt::DisplayRole, QVariant::fromValue(shp.size()));

            if (collect)
            {
                for (Pos pos : shp)
                {
                    QTreeWidgetItem* item = new QTreeWidgetItem(stitem);
                    item->setText(C_SEED, "-");
                    item->setData(C_X, Qt::DisplayRole, QVariant::fromValue(pos.x));
                    item->setData(C_Z, Qt::DisplayRole, QVariant::fromValue(pos.z));
                    item->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
                    item->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));
                    item->setData(0, Qt::UserRole+2, QVariant::fromValue(pos));
                }
            }
        }
        workDone.fetch_add(1, std::memory_order_relaxed);
    }

    if (seeditem->childCount() == 0)
    {
        delete seeditem;
        return;
    }
    if (shouldStop())
        seeditem->setText(0, QString::asprintf("%" PRId64, wi.seed) + " " + "(incomplete)");
    emit itemDone(seeditem);
}

void AnalysisStructures::runQuads(Generator *g)
{
    (void)g;
    struct QuadTask
    {
        int stype;
        int rx, rz, rw, rh;
        QVector<QuadInfo> found;
    };
    std::vector<int> types;
    if (quadHuts)
        types.push_back(Swamp_Hut);
    if (quadMonuments)
        types.push_back(Monument);

    auto regionAt = [](int block) {
        const int64_t value = block;
        return int(value >= 0 ? value / 512 : -((-value + 511) / 512));
    };
    const int rx1 = regionAt(area.x1) - 1;
    const int rz1 = regionAt(area.z1) - 1;
    const int rx2 = regionAt(area.x2) + 1;
    const int rz2 = regionAt(area.z2) + 1;
    std::vector<QuadTask> tasks;
    const size_t hardwareThreads = mappingThreadCount();
    const int64_t xSpan = int64_t(rx2) - rx1 + 1;
    const int64_t zSpan = int64_t(rz2) - rz1 + 1;
    const size_t partsPerType = types.empty() ? 0
        : std::max<size_t>(1, hardwareThreads * 8 / types.size());
    for (int stype : types)
    {
        const size_t parts = size_t(std::min<int64_t>(xSpan, partsPerType));
        for (size_t part = 0; part < parts; part++)
        {
            const int start = int(int64_t(rx1) + xSpan * part / parts);
            const int end = int(int64_t(rx1) + xSpan * (part + 1) / parts) - 1;
            tasks.push_back({stype, start, rz1, end-start, int(zSpan-1), {}});
        }
    }

    workTotal = tasks.size();
    std::atomic_size_t nextTask(0);
    auto scanTasks = [&]() {
        Generator local;
        setupGenerator(&local, wi.mc, wi.large);
        applySeed(&local, DIM_OVERWORLD, wi.seed);
        while (!shouldStop())
        {
            const size_t index = nextTask.fetch_add(1, std::memory_order_relaxed);
            if (index >= tasks.size())
                break;
            QuadTask& task = tasks[index];
            findQuadStructsInRegions(task.stype, &local, &task.found,
                task.rx, task.rz, task.rw, task.rh, nullptr);
            workDone.fetch_add(1, std::memory_order_relaxed);
        }
    };
    const size_t workerCount = parallelInner
        ? std::min(tasks.size(), hardwareThreads) : std::min<size_t>(tasks.size(), 1);
    workersUsed = workerCount;
    if (parallelInner)
        runMappingWorkers(workerCount, workers, [&](size_t) { scanTasks(); });
    else if (workerCount)
        scanTasks();

    if (shouldStop())
        return;

    QVector<QuadInfo> qsinfo;
    for (const QuadTask& task : tasks)
    {
        if (shouldStop())
            return;
        qsinfo += task.found;
    }
    if (qsinfo.empty())
        return;

    QTreeWidgetItem *seeditem = new TreeIntItem();
    seeditem->setText(0, QString::asprintf("%" PRId64, wi.seed));
    seeditem->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
    seeditem->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));

    for (QuadInfo& qi : qsinfo)
    {
        if (shouldStop())
        {
            delete seeditem;
            return;
        }
        if (qi.afk.x < area.x1 || qi.afk.x > area.x2 ||
                qi.afk.z < area.z1 || qi.afk.z > area.z2)
            continue;
        if (qi.typ == Swamp_Hut && getQuadHutCst(qi.c & 0xfffff) > hutQuality)
            continue;
        if (qi.typ == Monument &&
                qmonumentQual(qi.c) < 58*58*4*monumentCoverage/100)
            continue;

        QString label;
        if (qi.typ == Swamp_Hut)
            label = "quad-hut";
        else
            label = "quad-monument";

        QTreeWidgetItem *item = new QTreeWidgetItem(seeditem);

        qreal dist = qi.afk.x*(qreal)qi.afk.x + qi.afk.z*(qreal)qi.afk.z;
        dist = sqrt(dist);

        item->setText(0, "-");
        item->setData(1, Qt::DisplayRole, QVariant::fromValue(label));
        item->setData(2, Qt::DisplayRole, QVariant::fromValue((qlonglong)dist));
        item->setData(3, Qt::DisplayRole, QVariant::fromValue(qi.afk.x));
        item->setData(4, Qt::DisplayRole, QVariant::fromValue(qi.afk.z));
        item->setData(5, Qt::DisplayRole, QVariant::fromValue(qi.rad));
        item->setData(6, Qt::DisplayRole, QVariant::fromValue(qi.spcnt));
        item->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
        item->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));
        item->setData(0, Qt::UserRole+2, QVariant::fromValue(qi.afk));
        resultCount.fetch_add(1, std::memory_order_relaxed);
    }

    if (seeditem->childCount() > 0)
        emit quadDone(seeditem);
    else
        delete seeditem;
}

void AnalysisStructures::runDensity(Generator *g)
{
    (void)g;
    struct DensityType { int type; QString name; StructureConfig config; };
    std::vector<DensityType> types;
    const auto addType = [&](int type, const char *name) {
        StructureConfig config;
        if (getStructureConfig_override(type, wi.mc, &config))
            types.push_back({type, QString::fromLatin1(name), config});
    };
    if (densityHuts)
        addType(Swamp_Hut, "swamp hut");
    if (densityMonuments)
        addType(Monument, "ocean monument");
    if (types.empty())
        return;

    // Extend the scan so groups whose centre is near an edge are counted fully.
    const int radius = densityRadius;
    const int sx1 = std::max(-30000000, area.x1 - radius);
    const int sz1 = std::max(-30000000, area.z1 - radius);
    const int sx2 = std::min( 30000000, area.x2 + radius);
    const int sz2 = std::min( 30000000, area.z2 + radius);
    struct Task { size_t type; int x1, z1, x2, z2; };
    std::vector<Task> tasks;
    const size_t target = std::max<size_t>(1, mappingThreadCount() * 8 / types.size());
    for (size_t t = 0; t < types.size(); t++)
    {
        const int scale = types[t].config.regionSize * 16;
        const int ri0 = floordiv(sx1, scale), ri1 = floordiv(sx2, scale);
        const int64_t width = int64_t(ri1) - ri0 + 1;
        const size_t parts = size_t(std::min<int64_t>(target, width));
        for (size_t part = 0; part < parts; part++)
        {
            const int a = int(int64_t(ri0) + width * part / parts);
            const int b = int(int64_t(ri0) + width * (part + 1) / parts);
            tasks.push_back({t, part ? a * scale : sx1, sz1,
                part + 1 == parts ? sx2 + 1 : b * scale, sz2 + 1});
        }
    }

    std::vector<std::vector<VarPos>> positions(types.size());
    std::mutex merge;
    std::atomic_size_t next(0);
    // The two final units represent grouping and constructing the result tree.
    // Keeping them in the progress total avoids showing 100% while the
    // analysis is still doing post-processing work.
    workTotal = tasks.size() + 2;
    const auto scan = [&]() {
        while (!shouldStop())
        {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= tasks.size()) break;
            const Task& task = tasks[i];
            std::vector<VarPos> found;
            // getStructs validates the generated position against the actual biome.
            getStructs(&found, types[task.type].config, wi, DIM_OVERWORLD,
                task.x1, task.z1, task.x2, task.z2, false, cancel ? cancel : &stop);
            if (!shouldStop())
            {
                std::lock_guard<std::mutex> lock(merge);
                positions[task.type].insert(positions[task.type].end(),
                    std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
            }
            workDone.fetch_add(1, std::memory_order_relaxed);
        }
    };
    const size_t count = parallelInner ? std::min(tasks.size(), mappingThreadCount()) : 1;
    workersUsed = count;
    if (parallelInner)
        runMappingWorkers(count, workers, [&](size_t) { scan(); });
    else
        scan();
    if (shouldStop()) return;

    struct Group { size_t type; Pos centre; std::vector<Pos> members; };
    std::vector<Group> groups;
    const int64_t radius2 = int64_t(radius) * radius;
    for (size_t t = 0; t < types.size(); t++)
    {
        const size_t n = positions[t].size();
        std::vector<size_t> parent(n);
        std::iota(parent.begin(), parent.end(), 0);
        const auto find = [&](size_t i, const auto& self) -> size_t {
            return parent[i] == i ? i : parent[i] = self(parent[i], self);
        };

        // Only structures in the same or an adjacent radius-sized cell can
        // be close enough to belong to the same group. The old all-pairs
        // comparison became quadratic after the scan and made large areas
        // appear to hang at 100%.
        std::unordered_map<uint64_t, std::vector<size_t>> cells;
        cells.reserve(n);
        const auto cellKey = [](int x, int z) {
            return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
        };
        for (size_t i = 0; i < n; i++)
        {
            if (!(i & 255) && shouldStop())
                return;
            const int cx = floordiv(positions[t][i].p.x, radius);
            const int cz = floordiv(positions[t][i].p.z, radius);
            for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++)
            {
                const auto it = cells.find(cellKey(cx + dx, cz + dz));
                if (it == cells.end())
                    continue;
                for (size_t j : it->second)
                {
                    const int64_t px = int64_t(positions[t][i].p.x) - positions[t][j].p.x;
                    const int64_t pz = int64_t(positions[t][i].p.z) - positions[t][j].p.z;
                    if (px*px + pz*pz <= radius2)
                    {
                        const size_t a = find(i, find), b = find(j, find);
                        if (a != b) parent[a] = b;
                    }
                }
            }
            cells[cellKey(cx, cz)].push_back(i);
        }
        std::map<size_t, std::vector<Pos>> components;
        for (size_t i = 0; i < n; i++)
        {
            if (!(i & 1023) && shouldStop())
                return;
            components[find(i, find)].push_back(positions[t][i].p);
        }
        for (auto& component : components)
        {
            if (shouldStop())
                return;
            std::vector<Pos>& members = component.second;
            bool inRequestedArea = false;
            int64_t xsum = 0, zsum = 0;
            for (const Pos& p : members)
            {
                inRequestedArea |= p.x >= area.x1 && p.x <= area.x2 &&
                    p.z >= area.z1 && p.z <= area.z2;
                xsum += p.x;
                zsum += p.z;
            }
            if (inRequestedArea && int(members.size()) >= densityMinimum)
                groups.push_back({t, {int(xsum / int64_t(members.size())),
                    int(zsum / int64_t(members.size()))}, std::move(members)});
        }
    }
    workDone.fetch_add(1, std::memory_order_relaxed);
    if (shouldStop()) return;
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.members.size() != b.members.size()) return a.members.size() > b.members.size();
        if (a.type != b.type) return a.type < b.type;
        if (a.centre.x != b.centre.x) return a.centre.x < b.centre.x;
        return a.centre.z < b.centre.z;
    });
    if (groups.empty())
    {
        workDone.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    QTreeWidgetItem *seeditem = new TreeIntItem();
    seeditem->setText(C_SEED, QString::asprintf("%" PRId64, wi.seed));
    seeditem->setData(C_SEED, Qt::UserRole, QVariant::fromValue(wi.seed));
    seeditem->setData(C_SEED, Qt::UserRole + 1, QVariant::fromValue((int)DIM_OVERWORLD));
    for (const Group& group : groups)
    {
        if (shouldStop())
        {
            delete seeditem;
            return;
        }
        QTreeWidgetItem *item = new QTreeWidgetItem(seeditem);
        item->setText(C_SEED, "-");
        item->setText(C_STRUCT, types[group.type].name);
        item->setData(C_COUNT, Qt::DisplayRole, int(group.members.size()));
        item->setData(C_X, Qt::DisplayRole, group.centre.x);
        item->setData(C_Z, Qt::DisplayRole, group.centre.z);
        item->setData(5, Qt::DisplayRole, radius);
        item->setText(6, tr("Group of %1 structures").arg(group.members.size()));
        item->setData(C_SEED, Qt::UserRole, QVariant::fromValue(wi.seed));
        item->setData(C_SEED, Qt::UserRole + 1, QVariant::fromValue((int)DIM_OVERWORLD));
        item->setData(C_SEED, Qt::UserRole + 2, QVariant::fromValue(group.centre));
        item->setData(C_SEED, Qt::UserRole + 3, radius);
        for (const Pos& p : group.members)
        {
            if (shouldStop())
            {
                delete seeditem;
                return;
            }
            QTreeWidgetItem *member = new QTreeWidgetItem(item);
            member->setText(C_SEED, "-");
            member->setText(C_STRUCT, types[group.type].name);
            member->setData(C_X, Qt::DisplayRole, p.x);
            member->setData(C_Z, Qt::DisplayRole, p.z);
            member->setData(C_SEED, Qt::UserRole, QVariant::fromValue(wi.seed));
            member->setData(C_SEED, Qt::UserRole + 1, QVariant::fromValue((int)DIM_OVERWORLD));
            member->setData(C_SEED, Qt::UserRole + 2, QVariant::fromValue(p));
            member->setData(C_SEED, Qt::UserRole + 3, radius);
        }
    }
    workDone.fetch_add(1, std::memory_order_relaxed);
    resultCount.fetch_add(groups.size(), std::memory_order_relaxed);
    emit densityDone(seeditem);
}

void AnalysisStructures::runFortresses(Generator *g)
{
    (void)g;
    StructureConfig config;
    if (!fortress2x2 && !fortress3x1)
        return;
    if (!getStructureConfig_override(Fortress, wi.mc, &config))
        return;

    const int scale = config.regionSize * 16;
    const int rx0 = floordiv(area.x1, scale);
    const int rz0 = floordiv(area.z1, scale);
    const int rx1 = floordiv(area.x2, scale);
    const int rz1 = floordiv(area.z2, scale);
    const int64_t width = int64_t(rx1) - rx0 + 1;
    const int64_t height = int64_t(rz1) - rz0 + 1;
    if (width <= 0 || height <= 0)
        return;

    struct FortressRecord
    {
        Pos start;
        int pieceCount;
        int eligibleCount;
        int spawners;
        int wartFields;
        std::vector<FortressLayoutMatch> matches;
    };
    struct Task
    {
        int rx0, rx1;
        std::vector<FortressRecord> records;
    };

    const size_t targetTasks = parallelInner
        ? std::max<size_t>(1, mappingThreadCount() * 8) : 1;
    const size_t taskCount = size_t(std::min<int64_t>(width, targetTasks));
    std::vector<Task> tasks;
    tasks.reserve(taskCount);
    for (size_t part = 0; part < taskCount; part++)
    {
        const int begin = int(int64_t(rx0) + width * part / taskCount);
        const int end = int(int64_t(rx0) + width * (part+1) / taskCount) - 1;
        tasks.push_back({begin, end, {}});
    }

    // Two final work units cover deterministic merging/sorting and result-tree
    // construction. Progress therefore cannot display 100% during either
    // post-processing phase.
    const uint64_t regionCount = uint64_t(width) * uint64_t(height);
    workTotal = size_t(regionCount + 2);
    std::atomic_size_t nextTask(0);
    const auto scan = [&]() {
        Generator local;
        setupGenerator(&local, wi.mc, wi.large);
        applySeed(&local, DIM_NETHER, wi.seed);
        Piece pieces[1024];

        while (!shouldStop())
        {
            const size_t taskIndex = nextTask.fetch_add(1, std::memory_order_relaxed);
            if (taskIndex >= tasks.size())
                break;
            Task& task = tasks[taskIndex];
            for (int rx = task.rx0; rx <= task.rx1 && !shouldStop(); rx++)
            {
                for (int rz = rz0; rz <= rz1; rz++)
                {
                    if (shouldStop())
                        break;
                    Pos start;
                    const bool candidate = getStructurePos(Fortress, wi.mc,
                        wi.seed, rx, rz, &start);
                    if (candidate && start.x >= area.x1 && start.x <= area.x2 &&
                            start.z >= area.z1 && start.z <= area.z2 &&
                            isViableStructurePos(Fortress, &local,
                                start.x, start.z, 0))
                    {
                        if (shouldStop())
                            break;
                        const int count = getFortressPieces(pieces,
                            int(sizeof(pieces) / sizeof(pieces[0])), wi.mc,
                            wi.seed, start.x >> 4, start.z >> 4);
                        if (shouldStop())
                            break;
                        FortressLayoutResult layouts = findFortressLayouts(
                            pieces, count, fortress2x2, fortress3x1);
                        if (!layouts.matches.empty())
                        {
                            int eligible = 0, spawners = 0, wart = 0;
                            for (int i = 0; i < count; i++)
                            {
                                if (!(i & 63) && shouldStop())
                                    break;
                                eligible += isFortressCrossingPiece(pieces[i]);
                                spawners += pieces[i].type == BRIDGE_SPAWNER;
                                wart += pieces[i].type == CORRIDOR_NETHER_WART;
                            }
                            if (!shouldStop())
                            {
                                task.records.push_back({start, count, eligible,
                                    spawners, wart, std::move(layouts.matches)});
                            }
                        }
                    }
                    workDone.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    const size_t workerCount = parallelInner
        ? std::min(tasks.size(), mappingThreadCount())
        : std::min<size_t>(tasks.size(), 1);
    workersUsed = workerCount;
    if (parallelInner)
        runMappingWorkers(workerCount, workers, [&](size_t) { scan(); });
    else if (workerCount)
        scan();
    if (shouldStop())
        return;

    std::vector<FortressRecord> records;
    for (Task& task : tasks)
    {
        records.insert(records.end(),
            std::make_move_iterator(task.records.begin()),
            std::make_move_iterator(task.records.end()));
    }
    std::sort(records.begin(), records.end(),
        [](const FortressRecord& a, const FortressRecord& b) {
            return std::tie(a.start.x, a.start.z) <
                std::tie(b.start.x, b.start.z);
        });
    workDone.fetch_add(1, std::memory_order_relaxed);
    if (shouldStop())
        return;
    if (records.empty())
    {
        workDone.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    QTreeWidgetItem *seeditem = new TreeIntItem();
    seeditem->setText(F_SEED, QString::asprintf("%" PRId64, wi.seed));
    seeditem->setData(F_SEED, Qt::UserRole, QVariant::fromValue(wi.seed));
    seeditem->setData(F_SEED, Qt::UserRole + 1, QVariant::fromValue((int)DIM_NETHER));
    for (const FortressRecord& record : records)
    {
        if (shouldStop())
        {
            delete seeditem;
            return;
        }
        bool has2x2 = false, has3x1 = false;
        for (const FortressLayoutMatch& match : record.matches)
        {
            has2x2 |= match.type == FORTRESS_LAYOUT_2X2;
            has3x1 |= match.type == FORTRESS_LAYOUT_3X1;
        }
        QStringList layoutNames;
        if (has2x2) layoutNames << QStringLiteral("2x2");
        if (has3x1) layoutNames << QStringLiteral("3x1");

        QTreeWidgetItem *fortressItem = new QTreeWidgetItem(seeditem);
        fortressItem->setText(F_SEED, QStringLiteral("-"));
        fortressItem->setText(F_LAYOUT, layoutNames.join(QStringLiteral(" + ")));
        fortressItem->setData(F_START_X, Qt::DisplayRole, record.start.x);
        fortressItem->setData(F_START_Z, Qt::DisplayRole, record.start.z);
        fortressItem->setData(F_QUALIFYING, Qt::DisplayRole, record.eligibleCount);
        fortressItem->setData(F_SIZE, Qt::DisplayRole, record.pieceCount);
        fortressItem->setData(F_SPAWNERS, Qt::DisplayRole, record.spawners);
        fortressItem->setData(F_WART, Qt::DisplayRole, record.wartFields);
        fortressItem->setToolTip(F_QUALIFYING,
            tr("%1 eligible crossing/start pieces; %2 selected layout matches")
                .arg(record.eligibleCount).arg(record.matches.size()));
        fortressItem->setData(F_SEED, Qt::UserRole, QVariant::fromValue(wi.seed));
        fortressItem->setData(F_SEED, Qt::UserRole + 1,
            QVariant::fromValue((int)DIM_NETHER));
        fortressItem->setData(F_SEED, Qt::UserRole + 2,
            QVariant::fromValue(record.start));

        for (const FortressLayoutMatch& match : record.matches)
        {
            if (shouldStop())
            {
                delete seeditem;
                return;
            }
            QTreeWidgetItem *matchItem = new QTreeWidgetItem(fortressItem);
            matchItem->setText(F_SEED, QStringLiteral("-"));
            matchItem->setText(F_LAYOUT, fortressLayoutName(match.type));
            matchItem->setData(F_START_X, Qt::DisplayRole, record.start.x);
            matchItem->setData(F_START_Z, Qt::DisplayRole, record.start.z);
            matchItem->setData(F_MATCH_X, Qt::DisplayRole, match.position.x);
            matchItem->setData(F_MATCH_Z, Qt::DisplayRole, match.position.z);
            matchItem->setText(F_ORIENTATION,
                fortressOrientationName(match.orientation));
            matchItem->setData(F_QUALIFYING, Qt::DisplayRole, match.pieceCount);
            matchItem->setData(F_SIZE, Qt::DisplayRole, record.pieceCount);
            matchItem->setData(F_SPAWNERS, Qt::DisplayRole, record.spawners);
            matchItem->setData(F_WART, Qt::DisplayRole, record.wartFields);
            matchItem->setData(F_SEED, Qt::UserRole,
                QVariant::fromValue(wi.seed));
            matchItem->setData(F_SEED, Qt::UserRole + 1,
                QVariant::fromValue((int)DIM_NETHER));
            matchItem->setData(F_SEED, Qt::UserRole + 2,
                QVariant::fromValue(match.position));
        }
    }
    resultCount.fetch_add(records.size(), std::memory_order_relaxed);
    workDone.fetch_add(1, std::memory_order_relaxed);
    emit fortressDone(seeditem);
}


TabStructures::TabStructures(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabStructures)
    , parent(parent)
    , thread(this)
    , nextupdate()
    , updt(100)
{
    ui->setupUi(this);
    for (QLineEdit *line : {ui->lineX1, ui->lineZ1, ui->lineX2, ui->lineZ2})
        line->setValidator(new QIntValidator(-30000000, 30000000, this));

    QButtonGroup *structureScope = new QButtonGroup(this);
    structureScope->setExclusive(true);
    structureScope->addButton(ui->radioMap);
    structureScope->addButton(ui->radioAll);

    configureResultTree(ui->treeStructs);
    ui->treeStructs->header()->setMinimumSectionSize(24);
    ui->treeStructs->header()->setSectionResizeMode(QHeaderView::Stretch);
    ui->treeStructs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(ui->treeStructs->header(), &QHeaderView::sectionClicked, this,
        [=](int section){ onHeaderClick(ui->treeStructs, section); });

    ui->treeDensity->setColumnWidth(0, 160);
    configureResultTree(ui->treeDensity);
    setResultTreeSort(ui->treeDensity, C_COUNT, Qt::DescendingOrder, false);
    ui->treeDensity->header()->setMinimumSectionSize(24);
    ui->treeDensity->header()->setSectionResizeMode(QHeaderView::Stretch);
    ui->treeDensity->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(ui->treeDensity->header(), &QHeaderView::sectionClicked, this,
        [=](int section){ onHeaderClick(ui->treeDensity, section); });

    // Results are constructed in the full deterministic key order (seed,
    // fortress coordinates, layout, match coordinates). Start without a live
    // sort column so equal parent/child display cells cannot disturb it.
    configureResultTree(ui->treeFortresses);
    ui->treeFortresses->header()->setMinimumSectionSize(24);
    ui->treeFortresses->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->treeFortresses->header()->setStretchLastSection(true);
    connect(ui->treeFortresses->header(), &QHeaderView::sectionClicked,
        this, [=](int section){ onHeaderClick(ui->treeFortresses, section); });

    connect(&thread, &AnalysisStructures::itemDone, this, &TabStructures::onAnalysisItemDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisStructures::quadDone, this, &TabStructures::onAnalysisQuadDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisStructures::densityDone, this, &TabStructures::onAnalysisDensityDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisStructures::fortressDone, this,
        &TabStructures::onAnalysisFortressDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisStructures::finished, this, &TabStructures::onAnalysisFinished);

    connect(ui->treeStructs, &QTreeWidget::itemClicked, this, &TabStructures::onTreeItemClicked);
    connect(ui->treeDensity, &QTreeWidget::itemClicked, this, &TabStructures::onTreeItemClicked);
    connect(ui->treeFortresses, &QTreeWidget::itemClicked,
        this, &TabStructures::onTreeItemClicked);
    connect(parent, &MainWindow::mapUpdated, this, &TabStructures::refresh);
    refreshFortressTab();
}

TabStructures::~TabStructures()
{
    thread.stop = true;
    waitForMappingThread(thread);
    delete ui;
}

bool TabStructures::event(QEvent *e)
{
    return QWidget::event(e);
}

void TabStructures::save(QSettings& settings)
{
    const auto saveCoordinate = [&settings](const char *key, QLineEdit *line) {
        if (line->text().trimmed().isEmpty())
            settings.remove(key);
        else
            settings.setValue(key, line->text().toInt());
    };
    saveCoordinate("analysis/x1", ui->lineX1);
    saveCoordinate("analysis/z1", ui->lineZ1);
    saveCoordinate("analysis/x2", ui->lineX2);
    saveCoordinate("analysis/z2", ui->lineZ2);
    settings.setValue("analysis/seedsrc", ui->comboSeedSource->currentIndex());
    settings.setValue("analysis/maponly", ui->radioMap->isChecked());
    settings.setValue("analysis/structureScopeExplicit", true);
    settings.setValue("analysis/collect", ui->checkCollect->isChecked());
    settings.setValue("analysis/densityhuts", ui->checkDensityHuts->isChecked());
    settings.setValue("analysis/densitymonuments", ui->checkDensityMonuments->isChecked());
    settings.setValue("analysis/densityradius", ui->spinDensityRadius->value());
    settings.setValue("analysis/densityminimum", ui->spinDensityMinimum->value());
    settings.setValue("analysis/fortress2x2", ui->checkFortress2x2->isChecked());
    settings.setValue("analysis/fortress3x1", ui->checkFortress3x1->isChecked());
}

static void loadCheck(QSettings *s, QCheckBox *cb, const char *key)
{
    cb->setChecked( s->value(key, cb->isChecked()).toBool() );
}
static void loadCombo(QSettings *s, QComboBox *combo, const char *key)
{
    combo->setCurrentIndex( s->value(key, combo->currentIndex()).toInt() );
}
static void loadLine(QSettings *s, QLineEdit *line, const char *key)
{
    if (s->contains(key))
        line->setText(QString::number(s->value(key).toLongLong()));
}
void TabStructures::load(QSettings& settings)
{
    loadLine(&settings, ui->lineX1, "analysis/x1");
    loadLine(&settings, ui->lineZ1, "analysis/z1");
    loadLine(&settings, ui->lineX2, "analysis/x2");
    loadLine(&settings, ui->lineZ2, "analysis/z2");
    loadCombo(&settings, ui->comboSeedSource, "analysis/seedsrc");
    loadCheck(&settings, ui->checkCollect, "analysis/collect");
    loadCheck(&settings, ui->checkDensityHuts, "analysis/densityhuts");
    loadCheck(&settings, ui->checkDensityMonuments, "analysis/densitymonuments");
    ui->spinDensityRadius->setValue(settings.value("analysis/densityradius", ui->spinDensityRadius->value()).toInt());
    ui->spinDensityMinimum->setValue(settings.value("analysis/densityminimum", ui->spinDensityMinimum->value()).toInt());
    loadCheck(&settings, ui->checkFortress2x2, "analysis/fortress2x2");
    loadCheck(&settings, ui->checkFortress3x1, "analysis/fortress3x1");
    const bool mapOnly = settings.value("analysis/structureScopeExplicit", false).toBool()
        && settings.value("analysis/maponly", false).toBool();
    if (mapOnly)
        ui->radioMap->setChecked(true);
    else
        ui->radioAll->setChecked(true);
    refreshFortressTab();
}

void TabStructures::refresh()
{
    refreshFortressTab();
}

void TabStructures::refreshFortressTab()
{
    const int index = ui->tabWidget->indexOf(ui->tabFortresses);
    const bool nether = parent->getDim() == DIM_NETHER;
    ui->tabWidget->setTabEnabled(index, nether);
    if (!nether && ui->tabWidget->currentWidget() == ui->tabFortresses)
        ui->tabWidget->setCurrentWidget(ui->tabStructures);
    if (!nether && thread.isRunning() && thread.fortress)
        thread.stop.store(true, std::memory_order_relaxed);
    on_tabWidget_currentChanged(-1);
}

void TabStructures::onHeaderClick(QTreeWidget *tree, int section)
{
    cycleResultTreeSort(tree, section);
}

void TabStructures::onAnalysisItemDone(QTreeWidgetItem *item)
{
    qbufs.push_back(item);
    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &TabStructures::onBufferTimeout);
    }
}

void TabStructures::onAnalysisQuadDone(QTreeWidgetItem *item)
{
    delete item;
}

void TabStructures::onAnalysisDensityDone(QTreeWidgetItem *item)
{
    qbufd.push_back(item);
    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &TabStructures::onBufferTimeout);
    }
}

void TabStructures::onAnalysisFortressDone(QTreeWidgetItem *item)
{
    qbuff.push_back(item);
    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &TabStructures::onBufferTimeout);
    }
}

void TabStructures::onAnalysisFinished()
{
    onBufferTimeout();
    const bool hasResults = thread.fortress
        ? ui->treeFortresses->topLevelItemCount() > 0
        : thread.density ? ui->treeDensity->topLevelItemCount() > 0
                         : ui->treeStructs->topLevelItemCount() > 0;
    const QString details = tr("%1/%2 work units, %3 threads, %4 ms")
        .arg(thread.workDone.load(std::memory_order_relaxed))
        .arg(thread.workTotal.load(std::memory_order_relaxed))
        .arg(thread.workers.peak.load(std::memory_order_relaxed))
        .arg(elapsed.elapsed());
    if (thread.stop)
        ui->labelStatus->setText(tr("Analysis stopped. ") + details);
    else if (!hasResults)
        ui->labelStatus->setText(tr("No matching structures found. ") + details);
    else
        ui->labelStatus->setText(tr("Analysis complete. ") + details);
    on_tabWidget_currentChanged(-1);
    QTreeWidget *resultTree = thread.fortress ? ui->treeFortresses
        : thread.density ? ui->treeDensity : ui->treeStructs;
    resortResultTree(resultTree);
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabStructures::onBufferTimeout()
{
    uint64_t t = -elapsed.elapsed();
    if (!qbufs.empty())
    {
        ui->treeStructs->setUpdatesEnabled(false);
        ui->treeStructs->addTopLevelItems(qbufs);
        ui->treeStructs->setUpdatesEnabled(true);
        qbufs.clear();
    }
    if (!qbufd.empty())
    {
        ui->treeDensity->setUpdatesEnabled(false);
        ui->treeDensity->addTopLevelItems(qbufd);
        for (QTreeWidgetItem *item: std::as_const(qbufd))
            item->setExpanded(true);
        ui->treeDensity->setUpdatesEnabled(true);
        qbufd.clear();
    }
    if (!qbuff.empty())
    {
        ui->treeFortresses->setUpdatesEnabled(false);
        ui->treeFortresses->addTopLevelItems(qbuff);
        for (QTreeWidgetItem *item : std::as_const(qbuff))
        {
            item->setExpanded(true);
            for (int i = 0; i < item->childCount(); i++)
                item->child(i)->setExpanded(true);
        }
        ui->treeFortresses->setUpdatesEnabled(true);
        qbuff.clear();
    }
    if (thread.isRunning())
    {
        if (thread.stop.load(std::memory_order_relaxed))
            ui->pushStart->setText(tr("Stopping..."));
        else
        {
            QString progress;
            const size_t total = thread.workTotal.load(std::memory_order_relaxed);
            if (thread.seeds.size() == 1 && total > 0)
                progress = QString::asprintf(" (%zu/%zu, %zu threads)",
                    thread.workDone.load(std::memory_order_relaxed), total,
                    thread.workers.peak.load(std::memory_order_relaxed));
            else
                progress = QString::asprintf(" (%d/%zu, %zu threads)", thread.idx.load(),
                    thread.seeds.size(), thread.workers.peak.load(std::memory_order_relaxed));
            ui->pushStart->setText(tr("Stop") + progress);
        }
    }

    QApplication::processEvents(); // force processing of events so we can time correctly

    t += elapsed.elapsed();
    if (8*t > updt)
        updt = 4*t;
    nextupdate = elapsed.nsecsElapsed() + 1e6 * updt;
    if (thread.isRunning())
        QTimer::singleShot(100, this, &TabStructures::onBufferTimeout);
}

void TabStructures::onTreeItemClicked(QTreeWidgetItem *item, int column)
{
    (void) column;
    QVariant dat;
    dat = item->data(0, Qt::UserRole);
    if (dat.isValid())
    {
        uint64_t seed = qvariant_cast<uint64_t>(dat);
        int dim = item->data(0, Qt::UserRole+1).toInt();
        WorldInfo wi;
        parent->getSeed(&wi);
        wi.seed = seed;
        parent->setSeed(wi, dim);
    }

    dat = item->data(0, Qt::UserRole+2);
    if (dat.isValid())
    {
        Pos p = qvariant_cast<Pos>(dat);
        parent->getMapView()->setView(p.x+0.5, p.z+0.5);
        const int radius = item->data(0, Qt::UserRole+3).toInt();
        if (radius > 0)
        {
            Shape circle = {Shape::CIRCLE, DIM_OVERWORLD, radius, p, p};
            parent->getMapView()->setShapes({circle});
        }
    }
}

void TabStructures::on_pushStart_clicked()
{
    if (thread.isRunning())
    {
        thread.stop = true;
        ui->pushStart->setText(tr("Stopping..."));
        return;
    }
    const bool fortressMode = ui->tabWidget->currentWidget() == ui->tabFortresses;
    if (fortressMode && parent->getDim() != DIM_NETHER)
    {
        ui->labelStatus->setText(tr("Fortress analysis is only available in the Nether."));
        refreshFortressTab();
        return;
    }
    if (fortressMode && !ui->checkFortress2x2->isChecked() &&
            !ui->checkFortress3x1->isChecked())
    {
        ui->labelStatus->setText(tr("Select at least one Fortress layout."));
        return;
    }
    updt = 20;
    nextupdate = 0;
    elapsed.start();
    ui->labelStatus->clear();
    thread.stop = false;
    thread.workDone = 0;
    thread.workTotal = 0;
    thread.workersUsed = 0;
    thread.workers.reset();

    parent->getSeed(&thread.wi);
    thread.seeds.clear();
    if (ui->comboSeedSource->currentIndex() == 0)
        thread.seeds.push_back(thread.wi.seed);
    else
        thread.seeds = parent->formControl->getResults();

    int x1 = ui->lineX1->text().toInt();
    int z1 = ui->lineZ1->text().toInt();
    int x2 = ui->lineX2->text().toInt();
    int z2 = ui->lineZ2->text().toInt();
    if (x2 < x1) std::swap(x1, x2);
    if (z2 < z1) std::swap(z1, z2);
    thread.area = AnalysisStructures::Dat{x1, z1, x2, z2};

    thread.collect = ui->checkCollect->isChecked();
    thread.parallelInner = thread.seeds.size() == 1;
    thread.densityHuts = ui->checkDensityHuts->isChecked();
    thread.densityMonuments = ui->checkDensityMonuments->isChecked();
    thread.densityRadius = ui->spinDensityRadius->value();
    thread.densityMinimum = ui->spinDensityMinimum->value();
    thread.fortress2x2 = ui->checkFortress2x2->isChecked();
    thread.fortress3x1 = ui->checkFortress3x1->isChecked();

    if (ui->radioMap->isChecked())
    {
        for (int sopt = 0; sopt < D_STRUCT_NUM; sopt++)
            thread.mapshow[sopt] = parent->getMapView()->getShow(sopt);
        thread.dim = parent->getDim();
    }
    else
    {
        for (int sopt = 0; sopt < D_STRUCT_NUM; sopt++)
            thread.mapshow[sopt] = true;
        thread.dim = DIM_UNDEF;
    }

    thread.quad = false;
    thread.density = false;
    thread.fortress = false;

    if (ui->tabWidget->currentWidget() == ui->tabStructures)
    {
        dats = thread.area;
        ui->treeStructs->clear();
    }
    else if (ui->tabWidget->currentWidget() == ui->tabDensity)
    {
        thread.density = true;
        datq = thread.area;
        ui->treeDensity->clear();
    }
    else
    {
        // Fortress availability follows the canonical active map dimension,
        // independently of the structure overlay selection controls.
        thread.fortress = true;
        thread.dim = DIM_NETHER;
        datf = thread.area;
        ui->treeFortresses->clear();
    }

    ui->pushExport->setEnabled(false);
    ui->pushStart->setChecked(true);
    QString progress = QString::asprintf(" (0/%zu)", thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    thread.start();
    QTimer::singleShot(50, this, &TabStructures::onBufferTimeout);
}

static void csvline(QTextStream& stream, const QString& qte, const QString& sep, QStringList& cols)
{
    if (qte.isEmpty())
    {
        for (QString& s : cols)
            if (s.contains(sep))
                s = "\"" + s + "\"";
    }
    stream << qte << cols.join(sep) << qte << "\n";
}

void TabStructures::exportResults(QTextStream& stream)
{
    QString qte = parent->config.quote;
    QString sep = parent->config.separator;

    stream << "Sep=" + sep + "\n";
    sep = qte + sep + qte;

    if (ui->tabWidget->currentWidget() == ui->tabStructures)
    {
        stream << qte << "#X1" << sep << dats.x1 << qte << "\n";
        stream << qte << "#Z1" << sep << dats.z1 << qte << "\n";
        stream << qte << "#X2" << sep << dats.x2 << qte << "\n";
        stream << qte << "#Z2" << sep << dats.z2 << qte << "\n";

        if (ui->checkCollect->isChecked())
        {
            QStringList header = { tr("seed"), tr("structure"), tr("x"), tr("z"), tr("details") };
            csvline(stream, qte, sep, header);
            QString seed;
            QString structure;
            for (QTreeWidgetItemIterator it(ui->treeStructs); *it; ++it)
            {
                QTreeWidgetItem *item = *it;
                if (item->text(C_SEED) != "-")
                    seed = item->text(C_SEED);
                if (!item->text(C_STRUCT).isEmpty())
                    structure = item->text(C_STRUCT);
                if (!item->data(0, Qt::UserRole+2).isValid())
                    continue;

                QStringList cols;
                cols.append(seed);
                cols.append(structure);
                cols.append(item->text(C_X));
                cols.append(item->text(C_Z));
                cols.append(item->text(C_DETAIL));
                csvline(stream, qte, sep, cols);
            }
        }
        else
        {
            std::set<QString> structures;
            std::map<uint64_t, std::map<QString, QString>> cnt; // [seed][stype]

            uint64_t seed = 0;
            QString structure;
            for (QTreeWidgetItemIterator it(ui->treeStructs); *it; ++it)
            {
                QTreeWidgetItem *item = *it;
                if (item->data(0, Qt::UserRole).isValid())
                    seed = item->data(0, Qt::UserRole).toLongLong();
                if (!item->text(C_STRUCT).isEmpty())
                    structures.insert((structure = item->text(C_STRUCT)));
                if (!item->text(C_COUNT).isEmpty())
                    cnt[seed][structure] = item->text(C_COUNT);
            }

            QStringList header = { tr("seed") };
            for (auto& sit : structures)
                header.append(sit);
            csvline(stream, qte, sep, header);
            for (auto& m : cnt)
            {
                QStringList cols;
                cols << QString::asprintf("%" PRId64, m.first);
                for (auto& sit : structures)
                {
                    QString cntstr = m.second[sit];
                    if (cntstr.isEmpty())
                        cntstr = "0";
                    cols.append(cntstr);
                }
                csvline(stream, qte, sep, cols);
            }
        }
    }
    else if(ui->tabWidget->currentWidget() == ui->tabDensity)
    {
        stream << qte << "#X1" << sep << datq.x1 << qte << "\n";
        stream << qte << "#Z1" << sep << datq.z1 << qte << "\n";
        stream << qte << "#X2" << sep << datq.x2 << qte << "\n";
        stream << qte << "#Z2" << sep << datq.z2 << qte << "\n";

        QStringList header = { tr("seed"), tr("structure"), tr("count"), tr("x"), tr("z"), tr("radius"), tr("details") };
        csvline(stream, qte, sep, header);
        QString seed;
        for (QTreeWidgetItemIterator it(ui->treeDensity); *it; ++it)
        {
            QTreeWidgetItem *item = *it;
            if (item->text(0) != "-")
            {
                seed = item->text(0);
                continue;
            }
            QStringList cols = { seed };
            for (int i = 1, n = item->columnCount(); i < n; i++)
                cols.append(item->text(i));
            csvline(stream, qte, sep, cols);
        }
    }
    else if (ui->tabWidget->currentWidget() == ui->tabFortresses)
    {
        stream << qte << "#X1" << sep << datf.x1 << qte << "\n";
        stream << qte << "#Z1" << sep << datf.z1 << qte << "\n";
        stream << qte << "#X2" << sep << datf.x2 << qte << "\n";
        stream << qte << "#Z2" << sep << datf.z2 << qte << "\n";

        QStringList header = {
            tr("seed"), tr("layout"), tr("fortress start x"),
            tr("fortress start z"), tr("match x"), tr("match z"),
            tr("orientation"), tr("qualifying pieces"), tr("fortress size"),
            tr("blaze spawners"), tr("nether wart fields")
        };
        csvline(stream, qte, sep, header);
        QString seed;
        for (QTreeWidgetItemIterator it(ui->treeFortresses); *it; ++it)
        {
            QTreeWidgetItem *item = *it;
            if (item->parent() == nullptr)
            {
                seed = item->text(F_SEED);
                continue;
            }
            // Fortress rows are summaries. Individual match rows provide one
            // unambiguous representative coordinate and orientation per CSV row.
            if (!item->data(F_MATCH_X, Qt::DisplayRole).isValid())
                continue;
            QStringList cols = {seed};
            for (int column = F_LAYOUT; column <= F_WART; column++)
                cols.append(item->text(column));
            csvline(stream, qte, sep, cols);
        }
    }
    stream.flush();
}

void TabStructures::on_pushExport_clicked()
{
#if WASM
    QByteArray content;
    QTextStream stream(&content);
    exportResults(stream);
    QFileDialog::saveFileContent(content, "structures.csv");
#else
    QString fnam = QFileDialog::getSaveFileName(
        this, tr("Export structure analysis"), parent->prevdir, tr("Text files (*.txt *csv);;Any files (*)"));
    if (fnam.isEmpty())
        return;

    QFileInfo finfo(fnam);
    QFile file(fnam);
    parent->prevdir = finfo.absolutePath();

    if (!file.open(QIODevice::WriteOnly))
    {
        warn(parent, tr("Failed to open file for export:\n\"%1\"").arg(fnam));
        return;
    }

    QTextStream stream(&file);
    exportResults(stream);
#endif
}

void TabStructures::on_buttonFromVisible_clicked()
{
    MapView *mapview = parent->getMapView();

    int x1, z1, x2, z2;
    mapview->getVisible(&x1, &z1, &x2, &z2);

    ui->lineX1->setText( QString::number(x1) );
    ui->lineZ1->setText( QString::number(z1) );
    ui->lineX2->setText( QString::number(x2) );
    ui->lineZ2->setText( QString::number(z2) );
}

void TabStructures::on_tabWidget_currentChanged(int)
{
    bool ok = false;
    if (!thread.isRunning())
    {
        if (ui->tabWidget->currentWidget() == ui->tabStructures)
            ok = ui->treeStructs->topLevelItemCount() > 0;
        if (ui->tabWidget->currentWidget() == ui->tabDensity)
            ok = ui->treeDensity->topLevelItemCount() > 0;
        if (ui->tabWidget->currentWidget() == ui->tabFortresses)
            ok = ui->treeFortresses->topLevelItemCount() > 0;
    }
    ui->pushExport->setEnabled(ok);
}
