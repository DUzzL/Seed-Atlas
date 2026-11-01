#include "tabbiomes.h"
#include "ui_tabbiomes.h"

#include "message.h"
#include "util.h"
#include "resultlist.h"

#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QScrollBar>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct BiomeCenter
{
    Pos pos;
    int size;
};

struct LocateTile
{
    Range range;
    int x1, z1, x2, z2;
};

static uint64_t greatestCommonDivisor(uint64_t a, uint64_t b)
{
    while (b)
    {
        uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

/*
 * The generation-engine locator has an effective climate pre-filter, but needs a
 * temporary array for its complete input range. Split a large search into
 * modest, overlapping tiles so the pre-filter remains useful and several
 * independent generator instances can scan tiles in parallel.
 */
static int mappingTileSize(int64_t width, int64_t height, int minimum, int maximum)
{
    const uint64_t area = uint64_t(width) * uint64_t(height);
    const uint64_t targetJobs = std::max<uint64_t>(1, mappingThreadCount() * 8);
    const double ideal = std::sqrt(double(area) / double(targetJobs));
    return std::max(minimum, std::min(int(std::ceil(ideal)), maximum));
}

static QVector<BiomeCenter> locateBiomeAreas(int mc, bool large,
        uint64_t seed, int dim, int biomeId, int x1, int z1, int x2, int z2, int y,
        int minsize, int tolerance, std::atomic_bool *stop,
        std::atomic_uint64_t *done, std::atomic_uint64_t *total,
        std::atomic_uint64_t *resultCount, MappingWorkerState *workerState,
        std::atomic_bool *failed)
{
    const int64_t width = int64_t(x2) - x1 + 1;
    const int64_t height = int64_t(z2) - z1 + 1;
    const int tileSize = mappingTileSize(width, height, 64, 512);
    const int overlap = std::min(256, std::max(32, int(std::ceil(std::sqrt(minsize)))));
    const uint64_t tilesX = (uint64_t(width) + tileSize - 1) / tileSize;
    const uint64_t tilesZ = (uint64_t(height) + tileSize - 1) / tileSize;
    const uint64_t tileCount = tilesX * tilesZ;

    std::atomic_uint64_t nextTile(0);
    std::mutex resultsMutex;
    QVector<BiomeCenter> results;
    total->fetch_add(tileCount, std::memory_order_relaxed);

    auto scanTiles = [&](size_t) {
        Generator g;
        setupGenerator(&g, mc, large);
        applySeed(&g, dim, seed);
        std::vector<Pos> pos(4096);
        std::vector<int> siz(4096);
        while (!stop->load(std::memory_order_relaxed) &&
                !failed->load(std::memory_order_relaxed))
        {
            const uint64_t index = nextTile.fetch_add(1, std::memory_order_relaxed);
            if (index >= tileCount)
                break;
            const int64_t coreX = int64_t(x1) + int64_t(index % tilesX) * tileSize;
            const int64_t coreZ = int64_t(z1) + int64_t(index / tilesX) * tileSize;
            const int coreWidth = int(std::min<int64_t>(tileSize, int64_t(x2) - coreX + 1));
            const int coreHeight = int(std::min<int64_t>(tileSize, int64_t(z2) - coreZ + 1));
            const int rx1 = std::max(x1, int(coreX) - overlap);
            const int rz1 = std::max(z1, int(coreZ) - overlap);
            const int rx2 = std::min(x2, int(coreX) + coreWidth - 1 + overlap);
            const int rz2 = std::min(z2, int(coreZ) + coreHeight - 1 + overlap);
            const LocateTile tile = {{4, rx1, rz1, rx2-rx1+1, rz2-rz1+1, y, 1},
                int(coreX), int(coreZ), int(coreX) + coreWidth, int(coreZ) + coreHeight};
            const size_t maxEntries = size_t(tile.range.sx) * size_t(tile.range.sz);
            int count = 0;
            for (;;)
            {
                int status = 0;
                count = getBiomeCentersEx(pos.data(), siz.data(), int(pos.size()),
                    &g, tile.range, biomeId, minsize, tolerance,
                    (volatile char*)stop, &status);
                if (status < 0)
                {
                    failed->store(true, std::memory_order_relaxed);
                    break;
                }
                if (status != 1 || pos.size() >= maxEntries ||
                        stop->load(std::memory_order_relaxed))
                    break;
                const size_t capacity = std::min(maxEntries, pos.size() * 2);
                pos.resize(capacity);
                siz.resize(capacity);
            }
            done->fetch_add(1, std::memory_order_relaxed);
            if (failed->load(std::memory_order_relaxed))
                break;

            std::lock_guard<std::mutex> lock(resultsMutex);
            uint64_t added = 0;
            for (int i = 0; i < count; i++)
            {
                const int64_t px = pos[i].x;
                const int64_t pz = pos[i].z;
                if (px < int64_t(tile.x1) * 4 || px >= int64_t(tile.x2) * 4 ||
                        pz < int64_t(tile.z1) * 4 || pz >= int64_t(tile.z2) * 4)
                    continue;
                results.append({pos[i], siz[i]});
                added++;
            }
            resultCount->fetch_add(added, std::memory_order_relaxed);
        }
    };

    runMappingWorkers(tileCount, *workerState, scanTiles);
    return stop->load(std::memory_order_relaxed) || failed->load(std::memory_order_relaxed)
        ? QVector<BiomeCenter>() : results;
}

} // namespace

void AnalysisBiomes::run()
{
    workers.reset();
    workDone.store(0, std::memory_order_relaxed);
    workTotal.store(0, std::memory_order_relaxed);
    resultCount.store(0, std::memory_order_relaxed);
    Generator g;
    setupGenerator(&g, wi.mc, wi.large);

    for (idx = 0; idx < (long)seeds.size(); idx++)
    {
        if (stop) break;
        wi.seed = seeds[idx];
        if (!dat.locateBiomes.isEmpty())
        {
            if (!runLocate(&g))
                break;
        }
        else
            runStatistics(&g);
    }
}

void AnalysisBiomes::runStatistics(Generator *g)
{
    (void)g;
    QVector<uint64_t> idcnt(257);
    int w = dat.x2 - dat.x1 + 1;
    int h = dat.z2 - dat.z1 + 1;
    uint64_t n = w * (uint64_t)h;
    for (int d = 0; d < 3; d++)
    {
        if (dims[d] == DIM_UNDEF)
            continue;

        if (dat.samples >= n)
        {   // full area generation in small cancellable parallel tiles
            const int step = mappingTileSize(w, h, 64, 256);
            const uint64_t tilesX = (uint64_t(w) + step - 1) / step;
            const uint64_t tilesZ = (uint64_t(h) + step - 1) / step;
            const uint64_t tileCount = tilesX * tilesZ;
            workTotal.fetch_add(tileCount, std::memory_order_relaxed);
            std::atomic_uint64_t nextTile(0);
            const size_t workerCount = size_t(std::min<uint64_t>(tileCount, mappingThreadCount()));
            std::vector<QVector<uint64_t>> counts(workerCount, QVector<uint64_t>(257));
            auto scanTiles = [&](size_t workerIndex) {
                Generator local;
                setupGenerator(&local, wi.mc, wi.large);
                applySeed(&local, dims[d], wi.seed);
                QVector<uint64_t>& localCounts = counts[workerIndex];
                while (!stop.load(std::memory_order_relaxed))
                {
                    const uint64_t index = nextTile.fetch_add(1, std::memory_order_relaxed);
                    if (index >= tileCount)
                        break;
                    const int tx = int(int64_t(dat.x1) + int64_t(index % tilesX) * step);
                    const int tz = int(int64_t(dat.z1) + int64_t(index / tilesX) * step);
                    const int tw = std::min(step, dat.x2-tx+1);
                    const int th = std::min(step, dat.z2-tz+1);
                    Range r = {dat.scale, tx, tz, tw, th, wi.y, 1};
                    int *ids = allocCache(&local, r);
                    genBiomes(&local, ids, r);
                    if (stop.load(std::memory_order_relaxed))
                    {
                        free(ids);
                        break;
                    }
                    for (int i = 0; i < tw*th; i++)
                        localCounts[ids[i] & 0xff]++;
                    free(ids);
                    workDone.fetch_add(1, std::memory_order_relaxed);
                }
            };
            runMappingWorkers(tileCount, workers, scanTiles);
            for (const QVector<uint64_t>& localCounts : counts)
                for (int id = 0; id < 256; id++)
                    idcnt[id] += localCounts[id];
        }
        else
        {   // generate a biome statistic from a random permutation of the area
            const uint64_t sampleCount = std::min(dat.samples, n);
            workTotal.fetch_add(sampleCount, std::memory_order_relaxed);
            uint64_t offset = n ? getRnd64() % n : 0;
            uint64_t stride = n ? getRnd64() % n : 1;
            if (!stride)
                stride = 1;
            while (n > 1 && greatestCommonDivisor(stride, n) != 1)
                stride = stride == n-1 ? 1 : stride+1;

            std::atomic_uint64_t nextSample(0);
            const size_t workerCount = size_t(std::min<uint64_t>(sampleCount, mappingThreadCount()));
            std::vector<QVector<uint64_t>> counts(workerCount, QVector<uint64_t>(257));
            auto scanSamples = [&](size_t workerIndex) {
                Generator local;
                setupGenerator(&local, wi.mc, wi.large);
                applySeed(&local, dims[d], wi.seed);
                QVector<uint64_t>& localCounts = counts[workerIndex];
                uint64_t localDone = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    const uint64_t i = nextSample.fetch_add(1, std::memory_order_relaxed);
                    if (i >= sampleCount)
                        break;
                    const uint64_t index = uint64_t(
                        (static_cast<unsigned __int128>(i) * stride + offset) % n);
                    const int x = int(index % w);
                    const int z = int(index / w);
                    const int id = getBiomeAt(&local, dat.scale,
                        dat.x1+x, wi.y, dat.z1+z);
                    localCounts[id & 0xff]++;
                    if (++localDone == 256)
                    {
                        workDone.fetch_add(localDone, std::memory_order_relaxed);
                        localDone = 0;
                    }
                }
                workDone.fetch_add(localDone, std::memory_order_relaxed);
            };
            runMappingWorkers(sampleCount, workers, scanSamples);
            for (const QVector<uint64_t>& localCounts : counts)
                for (int id = 0; id < 256; id++)
                    idcnt[id] += localCounts[id];
        }
    }

    int bcnt = 0;
    for (uint64_t c : std::as_const(idcnt))
        bcnt += !!c;
    idcnt[256] = bcnt;

    if (!stop) // discard partially processed seed
        emit seedDone(wi.seed, idcnt);
}

bool AnalysisBiomes::runLocate(Generator *g)
{
    if (dat.locateDim == DIM_END)
        return true;
    applySeed(g, dat.locateDim, wi.seed);
    std::atomic_bool allocationFailed(false);
    QTreeWidgetItem *seeditem = nullptr;
    for (int biomeId : std::as_const(dat.locateBiomes))
    {
        QVector<BiomeCenter> centers = locateBiomeAreas(wi.mc, wi.large, wi.seed,
            dat.locateDim, biomeId, dat.x1, dat.z1, dat.x2, dat.z2, wi.y >> 2,
            minsize, tolerance, &stop, &workDone, &workTotal, &resultCount,
            &workers, &allocationFailed);
        if (allocationFailed.load(std::memory_order_relaxed))
        {
            delete seeditem;
            emit failed(tr("Not enough memory to complete the biome search."));
            return false;
        }
        if (stop)
            break;
        if (!centers.isEmpty() && !seeditem)
        {
            seeditem = new QTreeWidgetItem();
            seeditem->setData(0, Qt::DisplayRole, QVariant::fromValue((qlonglong)wi.seed));
            seeditem->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
            seeditem->setData(0, Qt::UserRole+1, QVariant::fromValue(dat.locateDim));
        }
        for (const BiomeCenter& center : std::as_const(centers))
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(seeditem);
            item->setText(0, "-");
            item->setData(1, Qt::DisplayRole, getBiomeDisplay(wi.mc, biomeId));
            item->setData(1, Qt::UserRole, biomeId);
            item->setData(2, Qt::DisplayRole, QVariant::fromValue(center.size));
            item->setData(3, Qt::DisplayRole, QVariant::fromValue(center.pos.x));
            item->setData(4, Qt::DisplayRole, QVariant::fromValue(center.pos.z));
            item->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
            item->setData(0, Qt::UserRole+1, QVariant::fromValue(dat.locateDim));
            item->setData(0, Qt::UserRole+2, QVariant::fromValue(center.pos));
        }
    }
    if (seeditem && !stop)
        emit seedItem(seeditem);
    else
        delete seeditem;
    return !stop.load(std::memory_order_relaxed);
}


QVariant BiomeTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();
    static QVariant align = QVariant::fromValue((int)Qt::AlignCenter);
    if (role == Qt::TextAlignmentRole)
        return align;
    if (role == Qt::DisplayRole)
    {
        const int id = ids[index.column()];
        const auto biomes = cnt.constFind(id);
        if (biomes == cnt.cend())
            return QVariant();
        return biomes->value(seeds[index.row()]);
    }
    return QVariant();
}

qulonglong BiomeTableModel::valueAt(int row, int column) const
{
    if (row < 0 || row >= seeds.size() || column < 0 || column >= ids.size())
        return 0;
    const auto biomes = cnt.constFind(ids[column]);
    if (biomes == cnt.cend())
        return 0;
    const auto value = biomes->constFind(seeds[row]);
    return value == biomes->cend() ? 0 : value->toULongLong();
}

bool BiomeSortProxy::lessThan(const QModelIndex& a, const QModelIndex& b) const
{
    const BiomeTableModel *table = static_cast<const BiomeTableModel*>(sourceModel());
    const qulonglong av = a.row() < sortValues.size()
        ? sortValues[a.row()] : table->valueAt(a.row(), a.column());
    const qulonglong bv = b.row() < sortValues.size()
        ? sortValues[b.row()] : table->valueAt(b.row(), b.column());
    return bv < av;
}

QVariant BiomeTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (section < 0)
        return QVariant();
    if (role == Qt::InitialSortOrderRole)
        return QVariant::fromValue(Qt::AscendingOrder);
    if (orientation == Qt::Vertical && section < seeds.size())
    {
        int64_t seed = seeds[section];
        if (role == Qt::UserRole || role == Qt::UserRole+1)
            return QVariant::fromValue(seed); // identifier and export
        if (role == Qt::DisplayRole)
            return QVariant::fromValue(QString(" %1 ").arg(seed));
        static QVariant align = QVariant::fromValue((int)Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::TextAlignmentRole)
            return align;
    }
    if (orientation == Qt::Horizontal && section < ids.size())
    {
        int id = ids[section];
        const char *bname = biome2str(cmp.mc, id);
        if (role == Qt::UserRole)
            return id; // identifier
        if (role == Qt::UserRole+1)
            return bname ? bname : "#"; // export role
        if (role == Qt::DisplayRole)
            return id == 256 ? tr("Biomes") : getBiomeDisplay(cmp.mc, id);
        if (role == Qt::ToolTipRole && bname)
            return QVariant::fromValue(QString("%1:%2").arg(id).arg(bname));
    }
    return QVariant();
}

void BiomeTableModel::insertIds(QSet<int>& nids)
{
    for (int id : std::as_const(nids))
    {
        QList<int>::iterator it = std::lower_bound(ids.begin(), ids.end(), id, cmp);
        if (it == ids.end() || *it != id)
        {
            int i = std::distance(ids.begin(), it);
            beginInsertColumns(QModelIndex(), i, i);
            ids.insert(i, id);
            endInsertColumns();
        }
    }
}

void BiomeTableModel::insertSeeds(QList<uint64_t>& nseeds)
{
    int i = seeds.size();
    beginInsertRows(QModelIndex(), i, i+nseeds.size()-1);
    seeds.append(nseeds);
    endInsertRows();
}

void BiomeTableModel::reset(int mc)
{
    beginResetModel();
    seeds.clear();
    ids.clear();
    cnt.clear();
    cmp.mode = IdCmp::SORT_DIM;
    cmp.dim = DIM_UNDEF;
    cmp.mc = mc;
    endResetModel();
}

BiomeHeader::BiomeHeader(QWidget *parent)
    : QHeaderView(Qt::Horizontal, parent)
    , hover(-1)
    , pressed(-1)
{
    setSectionsClickable(true);
    setHighlightSections(true);
    connect(this, &QHeaderView::sectionPressed, this, &BiomeHeader::onSectionPress);
}

void BiomeHeader::onSectionPress(int section)
{
    pressed = section;
}

bool BiomeHeader::event(QEvent *e)
{
    switch (e->type())
    {
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
        hover = logicalIndexAt(static_cast<QHoverEvent*>(e)->position().toPoint());
        break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
        hover = -1;
        break;
    default: break;
    }
    return QHeaderView::event(e);
}

void BiomeHeader::paintSection(QPainter *painter, const QRect& rect, int section) const
{
    QHeaderView::paintSection(painter, rect, section);
}

QSize BiomeHeader::sectionSizeFromContents(int section) const
{
    return QHeaderView::sectionSizeFromContents(section);
}

TabBiomes::TabBiomes(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabBiomes)
    , parent(parent)
    , thread()
    , model(new BiomeTableModel(this))
    , proxy(new BiomeSortProxy(this))
    , selectedBiomes()
    , elapsed()
    , updt(20)
    , nextupdate()
{
    ui->setupUi(this);

    proxy->setSourceModel(model);
    proxy->setDynamicSortFilter(false);
    ui->table->setModel(proxy);

    BiomeHeader *header = new BiomeHeader(ui->table);
    ui->table->setHorizontalHeader(header);
    connect(header, &QHeaderView::sortIndicatorChanged, this, &TabBiomes::onTableSort);
    connect(ui->table->verticalHeader(), &QHeaderView::sectionClicked, this, &TabBiomes::onVHeaderClicked);

    ui->table->setSortingEnabled(true);
    // all rows are single-line text: ResizeToContents would measure every row
    // (and every biome column) again after each batch of up to 65536 seeds
    ui->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    ui->table->verticalHeader()->setDefaultSectionSize(ui->table->fontMetrics().height() + 4);
    ui->table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    ui->treeLocate->header()->setMinimumSectionSize(24);
    ui->treeLocate->header()->setSectionResizeMode(QHeaderView::Stretch);
    ui->treeLocate->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(ui->treeLocate, &ResultTree::exportRequested, this, &TabBiomes::on_pushExport_clicked);
    addResultMenu(ui->table, [this]() { on_pushExport_clicked(); });

    QIntValidator *intval = new QIntValidator(-60e6, 60e6, this);
    ui->lineX1->setValidator(intval);
    ui->lineZ1->setValidator(intval);
    ui->lineX2->setValidator(intval);
    ui->lineZ2->setValidator(intval);

    ui->lineBiomeSize->setValidator(new QIntValidator(1, INT_MAX / 16, this));
    ui->lineTolerance->setValidator(new QIntValidator(0, 255, this));
    ui->lineBiomeSize->setText("1");
    ui->lineTolerance->setText("0");

    // the arguments are copies, so the analysis thread need not wait for the GUI
    connect(&thread, &AnalysisBiomes::seedDone, this, &TabBiomes::onAnalysisSeedDone, Qt::QueuedConnection);
    connect(&thread, &AnalysisBiomes::seedItem, ui->treeLocate,
        [this](QTreeWidgetItem *item) { ui->treeLocate->post(item); }, Qt::DirectConnection);
    connect(&thread, &AnalysisBiomes::failed, this,
        [this](const QString& message) { warn(this, message); }, Qt::QueuedConnection);
    connect(&thread, &AnalysisBiomes::finished, this, &TabBiomes::onAnalysisFinished,
        Qt::QueuedConnection);

    for (int id = 0; id < 256; id++)
    {
        QString s;
        if (!(s = getBiomeDisplay(MC_1_17, id)).isEmpty())
            str2biome[s] = id;
        if (!(s = getBiomeDisplay(MC_NEWEST, id)).isEmpty())
            str2biome[s] = id;
    }

}

TabBiomes::~TabBiomes()
{
    thread.stop = true;
    waitForMappingThread(thread);
    delete ui;
}

bool TabBiomes::event(QEvent *e)
{
    return QWidget::event(e);
}

void TabBiomes::save(QSettings& settings)
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
    settings.setValue("analysis/scaleidx", ui->comboScale->currentIndex());
    settings.setValue("analysis/overworld", ui->checkOverworld->isChecked());
    settings.setValue("analysis/nether", ui->checkNether->isChecked());
    settings.setValue("analysis/end", ui->checkEnd->isChecked());
    settings.setValue("analysis/samples", ui->lineSamples->text().toULongLong());
    settings.setValue("analysis/fullarea", ui->radioFullSample->isChecked());
    QVector<int> selected = selectedBiomeList();
    settings.setValue("analysis/biomeid", selected.isEmpty() ? -1 : selected.constFirst());
    QVariantList biomeIds;
    for (int id : selectedBiomeList())
        biomeIds.append(id);
    settings.setValue("analysis/biomeids", biomeIds);
    settings.setValue("analysis/biomesize_chunks", ui->lineBiomeSize->text().toInt());
    settings.setValue("analysis/tolerance", ui->lineTolerance->text().toInt());
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
void TabBiomes::load(QSettings& settings)
{
    loadLine(&settings, ui->lineX1, "analysis/x1");
    loadLine(&settings, ui->lineZ1, "analysis/z1");
    loadLine(&settings, ui->lineX2, "analysis/x2");
    loadLine(&settings, ui->lineZ2, "analysis/z2");
    loadCombo(&settings, ui->comboSeedSource, "analysis/seedsrc");
    loadCombo(&settings, ui->comboScale, "analysis/scaleidx");
    loadCheck(&settings, ui->checkOverworld, "analysis/overworld");
    loadCheck(&settings, ui->checkNether, "analysis/nether");
    loadCheck(&settings, ui->checkEnd, "analysis/end");
    loadLine(&settings, ui->lineSamples, "analysis/samples");
    if (settings.value("analysis/fullarea", true).toBool())
        ui->radioFullSample->setChecked(true);
    else
        ui->radioStochastic->setChecked(true);
    selectedBiomes.clear();
    const QVariantList storedBiomes = settings.value("analysis/biomeids").toList();
    for (const QVariant& value : storedBiomes)
        selectedBiomes.insert(value.toInt());
    refreshBiomes(settings.value("analysis/biomeid", -1).toInt());
    if (settings.contains("analysis/biomesize_chunks"))
    {
        loadLine(&settings, ui->lineBiomeSize, "analysis/biomesize_chunks");
    }
    else
    {   // migrate the former 1:4-cell input to whole chunks without weakening it
        const int oldArea = settings.value("analysis/biomesize", 16).toInt();
        ui->lineBiomeSize->setText(QString::number(std::max(1, (oldArea + 15) / 16)));
    }
    loadLine(&settings, ui->lineTolerance, "analysis/tolerance");
}

void TabBiomes::refreshBiomes(int activeid)
{
    WorldInfo wi;
    parent->getSeed(&wi);
    if (!ui->labelDescription->property("originalText").isValid())
        ui->labelDescription->setProperty("originalText", ui->labelDescription->text());
    // thread.wi.seed is advanced by the worker; the seed list is set at the start
    const std::vector<uint64_t>& analysed = thread.seeds;
    const bool previous = ui->treeLocate->topLevelItemCount() && !analysed.empty() &&
        std::find(analysed.begin(), analysed.end(), wi.seed) == analysed.end();
    ui->labelDescription->setText(!previous
        ? ui->labelDescription->property("originalText").toString()
        : analysed.size() == 1
        ? tr("Results from seed %1. Analyze again for the current map.").arg(qint64(analysed.front()))
        : tr("Results from other seeds. Analyze again for the current map."));
    const int dim = parent->getDim();
    const int locateTab = ui->tabWidget->indexOf(ui->tabLocate);
    const bool canLocate = dim != DIM_END;
    ui->tabWidget->tabBar()->setTabVisible(locateTab, true);
    ui->tabLocate->setEnabled(canLocate);

    const auto isLocateBiome = [=](int id) {
        if (!biomeExists(wi.mc, id))
            return false;
        return dim == DIM_OVERWORLD ? isOverworld(wi.mc, id) != 0
                                    : getDimension(id) == dim;
    };
    QSet<int> supportedBiomes;
    for (int id : std::as_const(selectedBiomes))
        if (isLocateBiome(id))
            supportedBiomes.insert(id);
    selectedBiomes = supportedBiomes;
    if (!canLocate)
    {
        ui->comboBiome->clear();
        updateBiomeSelectionControls();
        return;
    }
    if (activeid >= 0 && !isLocateBiome(activeid))
        activeid = -1;
    if (activeid == -1)
    {
        if (!selectedBiomes.isEmpty())
            activeid = *selectedBiomes.constBegin();
    }
    std::vector<int> ids;
    for (int i = 0; i < 256; i++)
        if (isLocateBiome(i))
            ids.push_back(i);
    IdCmp cmp(IdCmp::SORT_LEX, wi.mc, dim);
    std::sort(ids.begin(), ids.end(), cmp);
    ui->comboBiome->clear();
    for (int i : ids)
        ui->comboBiome->addItem(getBiomeIcon(i), getBiomeDisplay(wi.mc, i), QVariant::fromValue(i));
    if (activeid >= 0)
    {
        int idx = ui->comboBiome->findText(getBiomeDisplay(wi.mc, activeid));
        ui->comboBiome->setCurrentIndex(idx);
        if (selectedBiomes.isEmpty() && idx >= 0)
            selectedBiomes.insert(activeid);
    }
    else
    {
        ui->comboBiome->setCurrentIndex(-1);
    }
    ui->comboBiome->setCheckedData(selectedBiomeList());
    updateBiomeSelectionControls();
}

QVector<int> TabBiomes::selectedBiomeList() const
{
    QVector<int> ids = selectedBiomes.values().toVector();
    std::sort(ids.begin(), ids.end());
    return ids;
}

void TabBiomes::updateBiomeSelectionControls()
{
    QStringList names;
    WorldInfo wi;
    parent->getSeed(&wi);
    for (int id : selectedBiomeList())
        names.append(getBiomeDisplay(wi.mc, id));
    QString summary = tr("Selected biomes: %1").arg(names.join(", "));
    ui->buttonHighlight->setToolTip(summary);
    if (selectedBiomes.isEmpty() && ui->buttonHighlight->isChecked())
        ui->buttonHighlight->setChecked(false);
    ui->buttonHighlight->setEnabled(!selectedBiomes.isEmpty());
    if (!ui->buttonHighlight->isChecked())
    {
        ui->buttonHighlight->setText(selectedBiomes.size() > 1
                ? tr("Highlight %n biomes", "", selectedBiomes.size())
                : tr("Highlight on map"));
    }
}

void TabBiomes::onTableSort(int, Qt::SortOrder)
{
    QHeaderView *header = ui->table->horizontalHeader();

    if (proxy->order == Qt::DescendingOrder && proxy->column != -1)
    {
        header->setSortIndicatorShown(false);
        header->setSortIndicator(-1, Qt::AscendingOrder);
        proxy->column = -1;
    }
    else
    {
        header->setSortIndicatorShown(true);
    }
}

void TabBiomes::onVHeaderClicked(int row)
{
    QVariant dat = proxy->headerData(row, Qt::Vertical, Qt::UserRole);
    if (dat.isValid())
    {
        uint64_t seed = dat.toULongLong();
        WorldInfo wi;
        parent->getSeed(&wi);
        wi.seed = seed;
        parent->setSeed(wi);
    }
}

void TabBiomes::onAnalysisSeedDone(uint64_t seed, QVector<uint64_t> idcnt)
{
    idcnt.push_back(seed);
    qbufs.push_back(idcnt);
    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &TabBiomes::onBufferTimeout);
    }
}

void TabBiomes::onAnalysisFinished()
{
    onBufferTimeout();
    if (!thread.dat.locateBiomes.isEmpty())
        ui->treeLocate->finish();
    else if (proxy->column >= 0)
        proxy->sort(proxy->column, proxy->order);
    on_tabWidget_currentChanged(-1);
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabBiomes::onBufferTimeout()
{
    const auto updateProgress = [&]() {
        if (thread.stop.load(std::memory_order_relaxed))
        {
            ui->pushStart->setText(tr("Stopping..."));
            return;
        }
        QString progress = QString::asprintf(" (%ld/%zu)", thread.idx.load(), thread.seeds.size());
        const uint64_t total = thread.workTotal.load(std::memory_order_relaxed);
        if (total)
            progress += QString::asprintf(" [%llu/%llu, %zu threads]",
                static_cast<unsigned long long>(thread.workDone.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(total),
                thread.workers.peak.load(std::memory_order_relaxed));
        if (!thread.dat.locateBiomes.isEmpty())
            progress += QString::asprintf(" [%llu results]",
                static_cast<unsigned long long>(
                    thread.resultCount.load(std::memory_order_relaxed)));
        ui->pushStart->setText(tr("Stop") + progress);
    };
    if (qbufs.empty())
    {
        if (!thread.isRunning())
            return;
        updateProgress();
        QTimer::singleShot(250, this, &TabBiomes::onBufferTimeout);
        return;
    }

    uint64_t t = -elapsed.elapsed();

    if (!qbufs.empty())
    {
        ui->table->setUpdatesEnabled(false);

        // store column widths to track which columns need to widen
        QMap<int, int> colwidth;
        for (int c = 0, n = model->ids.size(); c < n; c++)
            colwidth[model->ids[c]] = ui->table->columnWidth(c);

        QList<uint64_t> new_seeds;
        QSet<int> new_ids;
        QFontMetrics fm = fontMetrics();

        for (int i = 0, n = qbufs.size(); i < n; i++)
        {
            QVector<uint64_t>& scnt = qbufs[i];
            uint64_t seed = scnt.back();
            scnt.resize(scnt.size()-1);

            new_seeds.push_back(seed);
            for (int id = 0, idn = scnt.size(); id < idn; id++)
            {
                uint64_t cnt = scnt[id];
                if (cnt == 0)
                    continue;
                new_ids.insert(id);
                model->cnt[id][seed] = QVariant::fromValue(cnt);
                int w = txtWidth(fm, QString::number(cnt) + "#");
                if (w > colwidth[id])
                    colwidth[id] = w;
            }
        }
        model->insertIds(new_ids);
        model->insertSeeds(new_seeds);

        ui->table->setUpdatesEnabled(true);

        //ui->table->resizeColumnsToContents();
        for (int i = 0, n = proxy->columnCount(); i < n; i++)
        {
            int id = proxy->headerData(i, Qt::Horizontal, Qt::UserRole).toInt();
            ui->table->setColumnWidth(i, qMax(80, colwidth[id] + 28));
        }
        int rowheight = fm.height() + 4;
        ui->table->verticalHeader()->setDefaultSectionSize(rowheight);
        qbufs.clear();
    }

    if (thread.isRunning())
        updateProgress();

    QApplication::processEvents(); // force processing of events so we can time correctly

    t += elapsed.elapsed();
    if (8*t > updt)
        updt = 4*t;
    nextupdate = elapsed.nsecsElapsed() + 1e6 * updt;
    if (thread.isRunning())
        QTimer::singleShot(250, this, &TabBiomes::onBufferTimeout);
}

void TabBiomes::on_pushStart_clicked()
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
    thread.stop = false;

    parent->getSeed(&thread.wi);
    thread.seeds.clear();
    if (ui->comboSeedSource->currentIndex() == 0)
        thread.seeds.push_back(thread.wi.seed);
    else
        thread.seeds = parent->formControl->getResults();

    thread.dims[0] = ui->checkOverworld->isChecked() ? DIM_OVERWORLD : DIM_UNDEF;
    thread.dims[1] = ui->checkNether->isChecked() ? DIM_NETHER : DIM_UNDEF;
    thread.dims[2] = ui->checkEnd->isChecked() ? DIM_END : DIM_UNDEF;

    int x1 = ui->lineX1->text().toInt();
    int z1 = ui->lineZ1->text().toInt();
    int x2 = ui->lineX2->text().toInt();
    int z2 = ui->lineZ2->text().toInt();
    if (x2 < x1) std::swap(x1, x2);
    if (z2 < z1) std::swap(z1, z2);

    int s = ui->comboScale->currentIndex() * 2; // combo index matches a power of 4 scale
    int scale = 1 << s;

    if (ui->radioFullSample->isChecked())
    {
        thread.dat.samples = ~0ULL;
    }
    else
    {
        thread.dat.samples = ui->lineSamples->text().toULongLong();
        scale = 4;
        s = 2;
    }

    if (ui->tabWidget->currentWidget() == ui->tabLocate)
    {
        if (selectedBiomes.isEmpty())
        {
            warn(parent, tr("Select at least one biome."));
            return;
        }
        ui->treeLocate->clearResults();
        thread.dat.locateBiomes = selectedBiomeList();
        thread.dat.locate = thread.dat.locateBiomes.isEmpty()
                ? -1 : thread.dat.locateBiomes.constFirst();
        thread.dat.locateDim = parent->getDim();
        const int chunks = ui->lineBiomeSize->text().toInt();
        thread.minsize = chunks > INT_MAX / 16 ? INT_MAX : chunks * 16;
        thread.tolerance = ui->lineTolerance->text().toInt();
        if (thread.minsize <= 0)
            thread.minsize = 1;
        scale = 4;
        s = 2;
    }
    else
    {
        model->reset(thread.wi.mc);
        thread.dat.locate = -1;
        thread.dat.locateBiomes.clear();
    }

    thread.dat.scale = scale;
    thread.dat.x1 = x1 >> s;
    thread.dat.z1 = z1 >> s;
    thread.dat.x2 = x2 >> s;
    thread.dat.z2 = z2 >> s;

    if (thread.dat.locate < 0)
        dats = thread.dat;
    else
        datl = thread.dat;

    ui->pushExport->setEnabled(false);
    thread.workDone.store(0, std::memory_order_relaxed);
    thread.workTotal.store(0, std::memory_order_relaxed);
    thread.resultCount.store(0, std::memory_order_relaxed);
    thread.workers.reset();
    ui->pushStart->setChecked(true);
    QString progress = QString::asprintf(" (0/%zu)", thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    thread.start();
    QTimer::singleShot(250, this, &TabBiomes::onBufferTimeout);
}


void TabBiomes::exportResults(CsvWriter& csv)
{
    const auto area = [&](const char *key, qint64 v, qint64 scale) {
        csv.row({key, QString::number(v), QString("(%1)").arg(v * scale)});
    };
    if (ui->tabWidget->currentWidget() == ui->tabStats)
    {
        area("#X1", dats.x1, dats.scale);
        area("#Z1", dats.z1, dats.scale);
        area("#X2", dats.x2, dats.scale);
        area("#Z2", dats.z2, dats.scale);
        csv.row({"#scale", QString("1:%1").arg(dats.scale)});
        if (dats.samples != ~0ULL)
            csv.row({"#samples", QString::number(dats.samples)});

        QStringList header = { tr("seed") };
        for (int col = 0, ncol = proxy->columnCount(); col < ncol; col++)
            header.append(proxy->headerData(col, Qt::Horizontal, Qt::UserRole+1).toString());
        csv.row(header);

        for (int row = 0, nrow = proxy->rowCount(); row < nrow; row++)
        {
            QStringList cols;
            cols.append(proxy->headerData(row, Qt::Vertical, Qt::UserRole+1).toString());
            for (int col = 0, ncol = proxy->columnCount(); col < ncol; col++)
            {
                QString cntstr = proxy->data(proxy->index(row, col)).toString();
                cols.append(cntstr == "" ? "0" : cntstr);
            }
            csv.row(cols);
        }
    }
    else if (ui->tabWidget->currentWidget() == ui->tabLocate)
    {
        area("#X1", datl.x1, datl.scale);
        area("#Z1", datl.z1, datl.scale);
        area("#X2", datl.x2, datl.scale);
        area("#Z2", datl.z2, datl.scale);
        csv.row({"#scale", QString("1:%1").arg(datl.scale)});
        QStringList biomeNames;
        for (int id : std::as_const(datl.locateBiomes))
            biomeNames.append(QString::fromLatin1(biome2str(MC_NEWEST, id)));
        csv.row({"#biomes", biomeNames.join(",")});

        csv.row({ tr("seed"), tr("biome"), tr("area"), tr("x"), tr("z") });
        QString seed;
        for (QTreeWidgetItemIterator it(ui->treeLocate); *it; ++it)
        {
            QTreeWidgetItem *item = *it;
            if (item->text(0) != "-")
            {
                seed = item->text(0);
                continue;
            }
            csv.row({ seed, item->text(1), item->text(2), item->text(3), item->text(4) });
        }
    }
}

void TabBiomes::on_pushExport_clicked()
{
    exportCsv(this, parent->prevdir, tr("Export biome analysis"), "biomes.csv",
        parent->config, [this](CsvWriter& csv) { exportResults(csv); });
}

void TabBiomes::on_buttonFromVisible_clicked()
{
    MapView *mapview = parent->getMapView();

    int x1, z1, x2, z2;
    mapview->getVisible(&x1, &z1, &x2, &z2);

    ui->lineX1->setText( QString::number(x1) );
    ui->lineZ1->setText( QString::number(z1) );
    ui->lineX2->setText( QString::number(x2) );
    ui->lineZ2->setText( QString::number(z2) );
}

void TabBiomes::on_radioFullSample_toggled(bool checked)
{
    ui->comboScale->setEnabled(checked);
    ui->lineSamples->setEnabled(!checked);
}

void TabBiomes::on_comboBiome_selectionChanged()
{
    selectedBiomes.clear();
    for (int biomeId : ui->comboBiome->checkedData())
        selectedBiomes.insert(biomeId);
    updateBiomeSelectionControls();
    if (ui->buttonHighlight->isChecked())
        parent->getMapView()->setBiomeHighlights(selectedBiomeList());
}

void TabBiomes::on_buttonHighlight_toggled(bool checked)
{
    parent->getMapView()->setBiomeHighlights(checked ? selectedBiomeList() : QVector<int>());
    ui->buttonHighlight->setText(checked ? tr("Clear highlight") : tr("Highlight on map"));
    updateBiomeSelectionControls();
}

void TabBiomes::on_treeLocate_itemClicked(QTreeWidgetItem *item, int column)
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

void TabBiomes::on_tabWidget_currentChanged(int)
{
    bool ok = false;
    if (!thread.isRunning())
    {
        if (ui->tabWidget->currentWidget() == ui->tabStats)
            ok = !model->ids.empty();
        if (ui->tabWidget->currentWidget() == ui->tabLocate)
            ok = ui->treeLocate->topLevelItemCount() > 0;
    }
    ui->pushExport->setEnabled(ok);
}
