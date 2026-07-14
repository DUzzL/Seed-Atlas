#include "tabbiomes.h"
#include "ui_tabbiomes.h"

#include "message.h"
#include "util.h"

#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QScrollBar>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_set>
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

/*
 * The cubiomes locator has an effective climate pre-filter, but needs a
 * temporary array for its complete input range. Split a large search into
 * modest, overlapping tiles so the pre-filter remains useful and several
 * independent generator instances can scan tiles in parallel.
 */
static QVector<BiomeCenter> locateLargeBiomeAreas(int mc, bool large,
        uint64_t seed, int dim, int biomeId, int x1, int z1, int x2, int z2, int y,
        int minsize, int tolerance, std::atomic_bool *stop, int maxResults,
        std::atomic_ulong *done, std::atomic_ulong *total)
{
    enum { TILE_SIZE = 2048 };
    const int overlap = std::min(256, std::max(32, int(std::ceil(std::sqrt(minsize)))));
    std::vector<LocateTile> tiles;
    for (int64_t x = x1; x <= x2; x += TILE_SIZE)
    {
        const int coreWidth = int(std::min<int64_t>(TILE_SIZE, int64_t(x2) - x + 1));
        for (int64_t z = z1; z <= z2; z += TILE_SIZE)
        {
            const int coreHeight = int(std::min<int64_t>(TILE_SIZE, int64_t(z2) - z + 1));
            const int rx1 = std::max(x1, int(x) - overlap);
            const int rz1 = std::max(z1, int(z) - overlap);
            const int rx2 = std::min(x2, int(x) + coreWidth - 1 + overlap);
            const int rz2 = std::min(z2, int(z) + coreHeight - 1 + overlap);
            tiles.push_back({{4, rx1, rz1, rx2-rx1+1, rz2-rz1+1, y, 1},
                int(x), int(z), int(x) + coreWidth, int(z) + coreHeight});
        }
    }

    std::atomic_size_t nextTile(0);
    std::mutex resultsMutex;
    QVector<BiomeCenter> results;
    done->store(0, std::memory_order_relaxed);
    total->store(tiles.size(), std::memory_order_relaxed);
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    const size_t workerCount = std::min<size_t>(tiles.size(),
        hardwareThreads ? hardwareThreads : 1);

    auto scanTiles = [&]() {
        Generator g;
        setupGenerator(&g, mc, large);
        applySeed(&g, dim, seed);
        Pos pos[4096];
        int siz[4096];
        while (!stop->load(std::memory_order_relaxed))
        {
            const size_t index = nextTile.fetch_add(1, std::memory_order_relaxed);
            if (index >= tiles.size())
                break;
            const LocateTile& tile = tiles[index];
            const int count = getBiomeCenters(pos, siz, 4096, &g, tile.range,
                biomeId, minsize, tolerance, (volatile char*)stop);
            done->fetch_add(1, std::memory_order_relaxed);

            std::lock_guard<std::mutex> lock(resultsMutex);
            for (int i = 0; i < count && results.size() < maxResults; i++)
            {
                const int64_t px = pos[i].x;
                const int64_t pz = pos[i].z;
                if (px < int64_t(tile.x1) * 4 || px >= int64_t(tile.x2) * 4 ||
                        pz < int64_t(tile.z1) * 4 || pz >= int64_t(tile.z2) * 4)
                    continue;
                results.append({pos[i], siz[i]});
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; i++)
        workers.emplace_back(scanTiles);
    for (std::thread& worker : workers)
        worker.join();
    return stop->load(std::memory_order_relaxed) ? QVector<BiomeCenter>() : results;
}

} // namespace

void AnalysisBiomes::run()
{
    stop = false;

    Generator g;
    setupGenerator(&g, wi.mc, wi.large);

    for (idx = 0; idx < (long)seeds.size(); idx++)
    {
        if (stop) break;
        wi.seed = seeds[idx];
        if (!dat.locateBiomes.isEmpty())
            runLocate(&g);
        else
            runStatistics(&g);
    }
}

void AnalysisBiomes::runStatistics(Generator *g)
{
    QVector<uint64_t> idcnt(257);
    int w = dat.x2 - dat.x1 + 1;
    int h = dat.z2 - dat.z1 + 1;
    uint64_t n = w * (uint64_t)h;

    for (int d = 0; d < 3; d++)
    {
        if (dims[d] == DIM_UNDEF)
            continue;
        applySeed(g, dims[d], wi.seed);

        if (dat.samples >= n)
        {   // full area gen => generate 512x512 areas at a time
            const int step = 512;
            for (int x = dat.x1; x <= dat.x2 && !stop; x += step)
            {
                for (int z = dat.z1; z <= dat.z2 && !stop; z += step)
                {
                    int w = dat.x2-x+1 < step ? dat.x2-x+1 : step;
                    int h = dat.z2-z+1 < step ? dat.z2-z+1 : step;
                    Range r = {dat.scale, x, z, w, h, wi.y, 1};
                    int *ids = allocCache(g, r);
                    genBiomes(g, ids, r);
                    for (int i = 0; i < w*h; i++)
                        idcnt[ ids[i] & 0xff ]++;
                    free(ids);
                }
            }
        }
        else
        {   // generate a biome statistic by sampling
            std::vector<uint64_t> order;

            if (dat.samples * 2 >= n)
            {   // dense regime => shuffle indeces
                order.resize(n);
                for (uint64_t i = 0; i < n; i++)
                    order[i] = i;
                for (uint64_t i = 0; i < n; i++)
                {
                    if (!(i & 0xffff) && stop)
                        break;
                    uint64_t idx = getRnd64() % n;
                    uint64_t t = order[i];
                    order[i] = order[idx];
                    order[idx] = t;
                }
                order.resize(dat.samples);
            }
            else
            {   // sparse regime => fill randomly without reuse
                std::unordered_set<uint64_t> used;
                order.reserve(dat.samples);
                used.reserve(dat.samples);
                for (uint64_t i = 0; order.size() < dat.samples; i++)
                {
                    if (!(i & 0xffff) && stop)
                        break;
                    uint64_t idx = getRnd64() % n;
                    auto it = used.insert(idx);
                    if (it.second)
                        order.push_back(idx);
                }
            }

            for (uint64_t i = 0; i < dat.samples && !stop; i++)
            {
                uint64_t idx = order[i];
                int x = (int) (idx % w);
                int z = (int) (idx / w);
                int id = getBiomeAt(g, dat.scale, dat.x1+x, wi.y, dat.z1+z);
                idcnt[ id & 0xff ]++;
            }
        }
    }

    int bcnt = 0;
    for (uint64_t c : qAsConst(idcnt))
        bcnt += !!c;
    idcnt[256] = bcnt;

    if (!stop) // discard partially processed seed
        emit seedDone(wi.seed, idcnt);
}

void AnalysisBiomes::runLocate(Generator *g)
{
    if (dat.locateDim == DIM_END)
        return;
    applySeed(g, dat.locateDim, wi.seed);
    enum { MAX_LOCATE = 4096 };
    const int64_t sx = int64_t(dat.x2) - dat.x1 + 1;
    const int64_t sz = int64_t(dat.z2) - dat.z1 + 1;
    const bool isLargeArea = uint64_t(sx) * uint64_t(sz) > INT_MAX;
    QTreeWidgetItem *seeditem = nullptr;
    for (int biomeId : qAsConst(dat.locateBiomes))
    {
        QVector<BiomeCenter> centers;
        if (isLargeArea)
        {
            centers = locateLargeBiomeAreas(wi.mc, wi.large, wi.seed, dat.locateDim, biomeId,
                dat.x1, dat.z1, dat.x2, dat.z2, wi.y >> 2, minsize,
                tolerance, &stop, MAX_LOCATE, &locateDone, &locateTotal);
        }
        else
        {
            Pos pos[MAX_LOCATE];
            int siz[MAX_LOCATE];
            Range r = {4, dat.x1, dat.z1, int(sx), int(sz), wi.y>>2, 1};
            int n = getBiomeCenters(
                pos, siz, MAX_LOCATE, g, r, biomeId, minsize, tolerance,
                (volatile char*)&stop
            );
            for (int i = 0; i < n; i++)
                centers.append({pos[i], siz[i]});
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
        for (const BiomeCenter& center : qAsConst(centers))
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
}


QVariant BiomeTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant::Invalid;
    static QVariant align = QVariant::fromValue((int)Qt::AlignCenter);
    if (role == Qt::TextAlignmentRole)
        return align;
    if (role == Qt::DisplayRole)
    {
        int id = ids[index.column()];
        uint64_t seed = seeds[index.row()];
        return cnt[id][seed];
    }
    return QVariant();
}

QVariant BiomeTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (section < 0)
        return QVariant::Invalid;
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
    for (int id : qAsConst(nids))
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
        hover = logicalIndexAt(((QHoverEvent*)e)->pos());
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
    if (!rect.isValid() || !model())
        return;

    QStyleOptionHeader opt;
    initStyleOption(&opt);

    QStyle::State state = QStyle::State_None;
    state |= QStyle::State_Enabled;
    state |= QStyle::State_Active;
    if (section == hover)
        state |= QStyle::State_MouseOver;
    if (section == pressed)
        state |= QStyle::State_Sunken;

    QString s = model()->headerData(section, orientation()).toString();
    painter->setFont(font());
    QFontMetrics fm = fontMetrics();
    int indicator_height = 0;
    int margin = 2 + 2 * style()->pixelMetric(QStyle::PM_HeaderMargin, 0, this);
    QStyleOptionHeader::SortIndicator sortindicator = QStyleOptionHeader::None;

    if (isSortIndicatorShown() && sortIndicatorSection() == section)
    {
        if (sortIndicatorOrder() == Qt::AscendingOrder)
            sortindicator = QStyleOptionHeader::SortDown;
        else
            sortindicator = QStyleOptionHeader::SortUp;
        indicator_height = 20;
    }

    int x = -rect.height() + margin + indicator_height;
    int y = rect.left() + (rect.width() + fm.descent()) / 2 + margin;

    opt.rect = rect;
    opt.section = section;
    opt.state = state;

    QPointF oldBO = painter->brushOrigin();
    painter->save();

    painter->setBrushOrigin(opt.rect.topLeft());
    style()->drawControl(QStyle::CE_Header, &opt, painter, this);

    painter->restore();

    painter->rotate(-90);
    painter->drawText(x, y, s);
    painter->rotate(+90);

    if (sortindicator != QStyleOptionHeader::None)
    {
        opt.sortIndicator = sortindicator;
        opt.rect = rect.adjusted(0, rect.bottom()-rect.y()-indicator_height, 0, 0);
        style()->drawControl(QStyle::CE_Header, &opt, painter, this);
        painter->setBrushOrigin(oldBO);
    }
    painter->setBrushOrigin(oldBO);
}

QSize BiomeHeader::sectionSizeFromContents(int section) const
{
    if (!model())
        return QSize();
    int margin = 2 * style()->pixelMetric(QStyle::PM_HeaderMargin, 0, this);
    QString s = model()->headerData(section, orientation()).toString();
    QFontMetrics fm = fontMetrics();
    return QSize(fm.height() + 2*margin, txtWidth(fm, s) + 2*margin);
}


TabBiomes::TabBiomes(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabBiomes)
    , parent(parent)
    , thread()
    , model(new BiomeTableModel(this))
    , proxy(new BiomeSortProxy(this))
    , sortcol(-1)
    , selectedBiomes()
    , elapsed()
    , updt(20)
    , nextupdate()
{
    ui->setupUi(this);

    proxy->setSourceModel(model);
    ui->table->setModel(proxy);

    BiomeHeader *header = new BiomeHeader(ui->table);
    ui->table->setHorizontalHeader(header);
    connect(header, &QHeaderView::sortIndicatorChanged, this, &TabBiomes::onTableSort);
    connect(ui->table->verticalHeader(), &QHeaderView::sectionClicked, this, &TabBiomes::onVHeaderClicked);

    ui->table->setSortingEnabled(true);

    ui->treeLocate->sortByColumn(-1, Qt::DescendingOrder);
    ui->treeLocate->setSortingEnabled(true);
    ui->treeLocate->header()->setMinimumSectionSize(24);
    ui->treeLocate->header()->setSectionResizeMode(QHeaderView::Stretch);
    ui->treeLocate->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(ui->treeLocate->header(), &QHeaderView::sectionClicked, this, &TabBiomes::onLocateHeaderClick);

    QIntValidator *intval = new QIntValidator(-60e6, 60e6, this);
    ui->lineX1->setValidator(intval);
    ui->lineZ1->setValidator(intval);
    ui->lineX2->setValidator(intval);
    ui->lineZ2->setValidator(intval);

    ui->lineBiomeSize->setValidator(new QIntValidator(1, INT_MAX / 16, this));
    ui->lineTolerance->setValidator(new QIntValidator(0, 255, this));
    ui->lineBiomeSize->setText("1");

    connect(&thread, &AnalysisBiomes::seedDone, this, &TabBiomes::onAnalysisSeedDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisBiomes::seedItem, this, &TabBiomes::onAnalysisSeedItem, Qt::BlockingQueuedConnection);
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
    thread.wait(500);
    delete ui;
}

bool TabBiomes::event(QEvent *e)
{
    return QWidget::event(e);
}

void TabBiomes::save(QSettings& settings)
{
    settings.setValue("analysis/x1", ui->lineX1->text().toInt());
    settings.setValue("analysis/z1", ui->lineZ1->text().toInt());
    settings.setValue("analysis/x2", ui->lineX2->text().toInt());
    settings.setValue("analysis/z2", ui->lineZ2->text().toInt());
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
    qlonglong x = line->text().toLongLong();
    line->setText( QString::number(s->value(key, x).toLongLong()) );
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
    for (int id : qAsConst(selectedBiomes))
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

void TabBiomes::onLocateHeaderClick()
{
    int section =  ui->treeLocate->header()->sortIndicatorSection();
    if (ui->treeLocate->header()->sortIndicatorOrder() == Qt::AscendingOrder && sortcol == section)
    {
        ui->treeLocate->sortByColumn(-1, Qt::DescendingOrder);
        section = -1;
    }
    sortcol = section;
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

void TabBiomes::onAnalysisSeedItem(QTreeWidgetItem *item)
{
    qbufl.push_back(item);
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
    on_tabWidget_currentChanged(-1);
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabBiomes::onBufferTimeout()
{
    const auto updateProgress = [&]() {
        QString progress = QString::asprintf(" (%ld/%zu)", thread.idx.load(), thread.seeds.size());
        const unsigned long total = thread.locateTotal.load(std::memory_order_relaxed);
        if (total)
            progress += QString::asprintf(" [%lu/%lu tiles]",
                thread.locateDone.load(std::memory_order_relaxed), total);
        ui->pushStart->setText(tr("Stop") + progress);
    };
    if (qbufs.empty() && qbufl.empty())
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
        ui->table->setSortingEnabled(false);
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
        ui->table->setSortingEnabled(true);

        //ui->table->resizeColumnsToContents();
        for (int i = 0, n = proxy->columnCount(); i < n; i++)
        {
            int id = proxy->headerData(i, Qt::Horizontal, Qt::UserRole).toInt();
            ui->table->setColumnWidth(i, colwidth[id]);
        }
        int rowheight = fm.height() + 4;
        for (int i = 0, n = proxy->rowCount(); i < n; i++)
            ui->table->setRowHeight(i, rowheight);
        qbufs.clear();
    }

    if (!qbufl.empty())
    {
        ui->treeLocate->setSortingEnabled(false);
        ui->treeLocate->setUpdatesEnabled(false);
        ui->treeLocate->addTopLevelItems(qbufl);
        ui->treeLocate->setUpdatesEnabled(true);
        ui->treeLocate->setSortingEnabled(true);
        qbufl.clear();
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
        return;
    }

    updt = 20;
    nextupdate = 0;
    elapsed.start();

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
        //ui->treeWidget->clear();
        ui->treeLocate->setSortingEnabled(false);
        while (ui->treeLocate->topLevelItemCount() > 0)
            delete ui->treeLocate->takeTopLevelItem(0);
        ui->treeLocate->setSortingEnabled(true);
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
    thread.locateDone.store(0, std::memory_order_relaxed);
    thread.locateTotal.store(0, std::memory_order_relaxed);
    ui->pushStart->setChecked(true);
    QString progress = QString::asprintf(" (0/%zu)", thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    thread.start();
    QTimer::singleShot(250, this, &TabBiomes::onBufferTimeout);
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

void TabBiomes::exportResults(QTextStream& stream)
{
    QString qte = parent->config.quote;
    QString sep = parent->config.separator;

    stream << "Sep=" + sep + "\n";
    sep = qte + sep + qte;

    if (ui->tabWidget->currentWidget() == ui->tabStats)
    {
        stream << qte << "#X1" << sep << dats.x1 << sep << "(" << (dats.x1*dats.scale) << ")" << qte << "\n";
        stream << qte << "#Z1" << sep << dats.z1 << sep << "(" << (dats.z1*dats.scale) << ")" << qte << "\n";
        stream << qte << "#X2" << sep << dats.x2 << sep << "(" << (dats.x2*dats.scale) << ")" << qte << "\n";
        stream << qte << "#Z2" << sep << dats.z2 << sep << "(" << (dats.z2*dats.scale) << ")" << qte << "\n";
        stream << qte << "#scale" << sep << "1:" << dats.scale << qte << "\n";
        if (dats.samples != ~0ULL)
            stream << qte << "#samples" << sep << dats.samples << qte << "\n";

        QStringList header = { tr("seed") };
        for (int col = 0, ncol = proxy->columnCount(); col < ncol; col++)
            header.append(proxy->headerData(col, Qt::Horizontal, Qt::UserRole+1).toString());
        csvline(stream, qte, sep, header);

        for (int row = 0, nrow = proxy->rowCount(); row < nrow; row++)
        {
            QStringList cols;
            cols.append(proxy->headerData(row, Qt::Vertical, Qt::UserRole+1).toString());
            for (int col = 0, ncol = proxy->columnCount(); col < ncol; col++)
            {
                QString cntstr = proxy->data(proxy->index(row, col)).toString();
                cols.append(cntstr == "" ? "0" : cntstr);
            }
            csvline(stream, qte, sep, cols);
        }
    }
    else if (ui->tabWidget->currentWidget() == ui->tabLocate)
    {
        stream << qte << "#X1" << sep << datl.x1 << sep << "(" << (datl.x1*datl.scale) << ")" << qte << "\n";
        stream << qte << "#Z1" << sep << datl.z1 << sep << "(" << (datl.z1*datl.scale) << ")" << qte << "\n";
        stream << qte << "#X2" << sep << datl.x2 << sep << "(" << (datl.x2*datl.scale) << ")" << qte << "\n";
        stream << qte << "#Z2" << sep << datl.z2 << sep << "(" << (datl.z2*datl.scale) << ")" << qte << "\n";
        stream << qte << "#scale" << sep << "1:" << datl.scale << qte << "\n";
        QStringList biomeNames;
        for (int id : qAsConst(datl.locateBiomes))
            biomeNames.append(QString::fromLatin1(biome2str(MC_NEWEST, id)));
        stream << qte << "#biomes" << sep << biomeNames.join(",") << qte << "\n";

        QStringList header = { tr("seed"), tr("biome"), tr("area"), tr("x"), tr("z") };
        csvline(stream, qte, sep, header);

        QTreeWidgetItemIterator it(ui->treeLocate);
        QString seed;
        for (; *it; ++it)
        {
            QTreeWidgetItem *item = *it;
            if (item->text(0) != "-")
            {
                seed = item->text(0);
                continue;
            }
            QStringList cols;
            cols.append(seed);
            cols.append(item->text(1));
            cols.append(item->text(2));
            cols.append(item->text(3));
            cols.append(item->text(4));
            csvline(stream, qte, sep, cols);
        }
    }
    stream.flush();
}

void TabBiomes::on_pushExport_clicked()
{
#if WASM
    QByteArray content;
    QTextStream stream(&content);
    exportResults(stream);
    QFileDialog::saveFileContent(content, "biomes.csv");
#else
    QString fnam = QFileDialog::getSaveFileName(
        this, tr("Export biome analysis"), parent->prevdir, tr("Text files (*.txt *csv);;Any files (*)"));
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
