#include "tabstructures.h"
#include "ui_tabstructures.h"

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
#include <set>
#include <thread>
#include <utility>


enum { C_SEED, C_STRUCT, C_COUNT, C_X, C_Z, C_DETAIL }; // columns

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

    const size_t hw = mappingThreadCount();
    if (seeds.size() >= hw)
    {
        idx = 0;
        struct Result { size_t order; QTreeWidgetItem *item; bool isquad; };
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
                task.parallelInner = false;
                task.quadHuts = quadHuts;
                task.quadMonuments = quadMonuments;
                task.hutQuality = hutQuality;
                task.monumentCoverage = monumentCoverage;
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

                if (quad)
                    task.runQuads(&g);
                else
                    task.runStructs(&g);
                resultCount.fetch_add(
                    task.resultCount.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
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

        std::sort(results.begin(), results.end(),
            [](const Result& a, const Result& b) { return a.order < b.order; });
        for (const Result& result : results)
        {
            if (result.isquad)
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
        if (quad)
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
                cancel ? cancel : &stop);
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
        stitem->setText(C_STRUCT, struct2str(result.stype));
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


TabStructures::TabStructures(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabStructures)
    , parent(parent)
    , thread(this)
    , sortcols(-1)
    , sortcolq(-1)
    , nextupdate()
    , updt(100)
{
    ui->setupUi(this);
    const int quadTab = ui->tabWidget->indexOf(ui->tabQuads);
    if (quadTab >= 0)
        ui->tabWidget->removeTab(quadTab);
    for (QLineEdit *line : {ui->lineX1, ui->lineZ1, ui->lineX2, ui->lineZ2})
        line->setValidator(new QIntValidator(-30000000, 30000000, this));

    QButtonGroup *structureScope = new QButtonGroup(this);
    structureScope->setExclusive(true);
    structureScope->addButton(ui->radioMap);
    structureScope->addButton(ui->radioAll);
    connect(ui->checkQuadHuts, &QCheckBox::toggled,
        ui->comboHutQuality, &QWidget::setEnabled);
    connect(ui->checkQuadMonuments, &QCheckBox::toggled,
        ui->comboMonumentCoverage, &QWidget::setEnabled);
    ui->comboHutQuality->setCurrentIndex(3);
    ui->comboMonumentCoverage->setCurrentIndex(1);

    ui->treeStructs->sortByColumn(-1, Qt::AscendingOrder);
    ui->treeStructs->header()->setMinimumSectionSize(24);
    ui->treeStructs->header()->setSectionResizeMode(QHeaderView::Stretch);
    ui->treeStructs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(ui->treeStructs->header(), &QHeaderView::sectionClicked, this, [=](){ onHeaderClick(ui->treeStructs); } );

    ui->treeQuads->setColumnWidth(0, 160);
    ui->treeQuads->sortByColumn(-1, Qt::AscendingOrder);
    ui->treeQuads->header()->setMinimumSectionSize(24);
    ui->treeQuads->header()->setSectionResizeMode(QHeaderView::Stretch);
    ui->treeQuads->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(ui->treeQuads->header(), &QHeaderView::sectionClicked, this, [=](){ onHeaderClick(ui->treeQuads); } );

    connect(&thread, &AnalysisStructures::itemDone, this, &TabStructures::onAnalysisItemDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisStructures::quadDone, this, &TabStructures::onAnalysisQuadDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisStructures::finished, this, &TabStructures::onAnalysisFinished);

    connect(ui->treeStructs, &QTreeWidget::itemClicked, this, &TabStructures::onTreeItemClicked);
    connect(ui->treeQuads, &QTreeWidget::itemClicked, this, &TabStructures::onTreeItemClicked);
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
    settings.setValue("analysis/quadhuts", ui->checkQuadHuts->isChecked());
    settings.setValue("analysis/quadmonuments", ui->checkQuadMonuments->isChecked());
    settings.setValue("analysis/hutquality", ui->comboHutQuality->currentIndex());
    settings.setValue("analysis/monumentcoverage", ui->comboMonumentCoverage->currentIndex());
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
    loadCheck(&settings, ui->checkQuadHuts, "analysis/quadhuts");
    loadCheck(&settings, ui->checkQuadMonuments, "analysis/quadmonuments");
    loadCombo(&settings, ui->comboHutQuality, "analysis/hutquality");
    loadCombo(&settings, ui->comboMonumentCoverage, "analysis/monumentcoverage");
    const bool mapOnly = settings.value("analysis/structureScopeExplicit", false).toBool()
        && settings.value("analysis/maponly", false).toBool();
    if (mapOnly)
        ui->radioMap->setChecked(true);
    else
        ui->radioAll->setChecked(true);
}

void TabStructures::onHeaderClick(QTreeView *tree)
{
    int& col = (tree == ui->treeStructs) ? sortcols : sortcolq;
    int section =  tree->header()->sortIndicatorSection();
    if (tree->header()->sortIndicatorOrder() == Qt::AscendingOrder && col == section)
    {
        tree->sortByColumn(-1, Qt::DescendingOrder);
        section = -1;
    }
    col = section;
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
    qbufq.push_back(item);
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
    const bool hasResults = ui->tabWidget->currentWidget() == ui->tabStructures
        ? ui->treeStructs->topLevelItemCount() > 0
        : ui->treeQuads->topLevelItemCount() > 0;
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
    ui->treeStructs->setSortingEnabled(true);
    ui->treeQuads->setSortingEnabled(true);
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabStructures::onBufferTimeout()
{
    uint64_t t = -elapsed.elapsed();
    if (!qbufs.empty())
    {
        ui->treeStructs->setSortingEnabled(false);
        ui->treeStructs->setUpdatesEnabled(false);
        ui->treeStructs->addTopLevelItems(qbufs);
        ui->treeStructs->resizeColumnToContents(C_DETAIL);
        ui->treeStructs->setUpdatesEnabled(true);
        ui->treeStructs->setSortingEnabled(true);
        qbufs.clear();
    }
    if (!qbufq.empty())
    {
        ui->treeQuads->setSortingEnabled(false);
        ui->treeQuads->setUpdatesEnabled(false);
        ui->treeQuads->addTopLevelItems(qbufq);
        for (QTreeWidgetItem *item: std::as_const(qbufq))
            item->setExpanded(true);
        ui->treeQuads->setUpdatesEnabled(true);
        ui->treeQuads->setSortingEnabled(true);
        qbufq.clear();
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
    thread.quadHuts = ui->checkQuadHuts->isChecked();
    thread.quadMonuments = ui->checkQuadMonuments->isChecked();
    thread.hutQuality = ui->comboHutQuality->currentIndex() + CST_IDEAL;
    thread.monumentCoverage = ui->comboMonumentCoverage->currentIndex() == 0 ? 95 : 90;

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

    if (ui->tabWidget->currentWidget() == ui->tabStructures)
    {
        thread.quad = false;
        dats = thread.area;
        ui->treeStructs->setSortingEnabled(false);
        while (ui->treeStructs->topLevelItemCount() > 0)
            delete ui->treeStructs->takeTopLevelItem(0);
        ui->treeStructs->setSortingEnabled(true);
    }
    else
    {
        thread.quad = true;
        datq = thread.area;
        ui->treeQuads->setSortingEnabled(false);
        while (ui->treeQuads->topLevelItemCount() > 0)
            delete ui->treeQuads->takeTopLevelItem(0);
        ui->treeQuads->setSortingEnabled(true);
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
    else if(ui->tabWidget->currentWidget() == ui->tabQuads)
    {
        stream << qte << "#X1" << sep << datq.x1 << qte << "\n";
        stream << qte << "#Z1" << sep << datq.z1 << qte << "\n";
        stream << qte << "#X2" << sep << datq.x2 << qte << "\n";
        stream << qte << "#Z2" << sep << datq.z2 << qte << "\n";

        QStringList header = { tr("seed"), tr("type"), tr("distance"), tr("x"), tr("z"), tr("radius"), tr("spawn area") };
        csvline(stream, qte, sep, header);
        QString seed;
        for (QTreeWidgetItemIterator it(ui->treeQuads); *it; ++it)
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
        if (ui->tabWidget->currentWidget() == ui->tabQuads)
            ok = ui->treeQuads->topLevelItemCount() > 0;
    }
    ui->pushExport->setEnabled(ok);
}
