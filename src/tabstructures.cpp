#include "tabstructures.h"
#include "ui_tabstructures.h"

#include "fortresslayout.h"
#include "message.h"
#include "seedtables.h"
#include "util.h"
#include "seedatlas-engine/quadbase.h"
#include "resultlist.h"

#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QIntValidator>
#include <QTextStream>
#include <QTreeWidgetItem>
#include <QMetaMethod>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMenu>
#include <QTimer>

#include <functional>
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

// ---------------------------------------------------------------------------
// StructureResultModel

namespace {

enum : quintptr { INSTANCE_LEVEL = quintptr(1) << 63 };

// Sort keys with the semantics of the other result lists: empty cells first,
// numbers by value, then text in locale order.
struct SortKey
{
    enum Kind { Empty, Number, Text } kind = Empty;
    long double number = 0;
    QString text;
};

static int compareKeys(const SortKey& a, const SortKey& b)
{
    if (a.kind != b.kind)
        return a.kind < b.kind ? -1 : 1;
    if (a.kind == SortKey::Number)
        return a.number < b.number ? -1 : a.number > b.number ? 1 : 0;
    if (a.kind == SortKey::Text)
        return QString::localeAwareCompare(a.text, b.text);
    return 0;
}

static SortKey numberKey(long double v) { SortKey k; k.kind = SortKey::Number; k.number = v; return k; }
static SortKey textKey(const QString& t) { SortKey k; k.kind = SortKey::Text; k.text = t; return k; }

template <class Index, class KeyOf>
static void sortOrder(std::vector<Index>& order, size_t count, Qt::SortOrder dir, KeyOf keyOf)
{
    if (order.size() != count)
    {
        order.resize(count);
        for (size_t i = 0; i < count; i++)
            order[i] = Index(i);
    }
    std::vector<std::pair<SortKey, Index>> keys;
    keys.reserve(count);
    for (Index i : order)
        keys.push_back({keyOf(i), i});
    std::stable_sort(keys.begin(), keys.end(), [dir](const auto& a, const auto& b) {
        const int c = compareKeys(a.first, b.first);
        return dir == Qt::AscendingOrder ? c < 0 : c > 0;
    });
    for (size_t i = 0; i < count; i++)
        order[i] = keys[i].second;
}

} // namespace

StructureResultModel::StructureResultModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

int StructureResultModel::level(const QModelIndex& index) const
{
    if (!index.isValid())
        return -1;
    const quintptr id = index.internalId();
    return id == 0 ? 0 : (id & INSTANCE_LEVEL) ? 2 : 1;
}

QModelIndex StructureResultModel::index(int row, int column, const QModelIndex& parent) const
{
    if (row < 0 || column < 0 || column >= columnCount())
        return {};
    switch (level(parent))
    {
    case -1:
        if (row >= int(seeds.size()))
            return {};
        return createIndex(row, column, quintptr(0));
    case 0:
    {
        const int s = seedOrder[parent.row()];
        if (row >= int(seeds[s].seed->groups.size()))
            return {};
        return createIndex(row, column, quintptr(s) + 1);
    }
    case 1:
    {
        const int s = int(parent.internalId() - 1);
        const int g = seeds[s].groupOrder[parent.row()];
        if (row >= int(seeds[s].seed->groups[g].items.size()))
            return {};
        return createIndex(row, column, INSTANCE_LEVEL | (quintptr(s) << 16) | quintptr(g));
    }
    }
    return {};
}

QModelIndex StructureResultModel::parent(const QModelIndex& index) const
{
    switch (level(index))
    {
    case 1:
    {
        const int s = int(index.internalId() - 1);
        return createIndex(seedRow[s], 0, quintptr(0));
    }
    case 2:
    {
        const int s = int((index.internalId() & ~INSTANCE_LEVEL) >> 16);
        const int g = int(index.internalId() & 0xffff);
        return createIndex(seeds[s].groupRow[g], 0, quintptr(s) + 1);
    }
    }
    return {};
}

int StructureResultModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() && parent.column() != 0)
        return 0;
    switch (level(parent))
    {
    case -1:
        return int(seeds.size());
    case 0:
        return int(seeds[seedOrder[parent.row()]].seed->groups.size());
    case 1:
    {
        const int s = int(parent.internalId() - 1);
        const StructGroup& group = seeds[s].seed->groups[seeds[s].groupOrder[parent.row()]];
        return int(std::min<size_t>(group.items.size(), INT_MAX));
    }
    }
    return 0;
}

int StructureResultModel::columnCount(const QModelIndex&) const
{
    return headers.size();
}

QVariant StructureResultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole &&
            section >= 0 && section < headers.size())
        return headers[section];
    return {};
}

QString StructureResultModel::detail(const StructGroup& group, size_t item) const
{
    const StructInstance& in = group.items[item];
    if (in.detail >= 0)
        return group.details.value(in.detail);
    if (group.stype < 0)
        return QString();
    VarPos vp(in.p, group.stype);
    vp.v = in.v;
    return vp.detail().join(":");
}

QString StructureResultModel::text(const QModelIndex& index, int column) const
{
    switch (level(index))
    {
    case 0:
    {
        const StructSeedResult& r = *seeds[seedOrder[index.row()]].seed;
        if (column != C_SEED)
            return QString();
        QString t = QString::asprintf("%" PRId64, (int64_t)r.seed);
        if (r.incomplete)
            t += " (incomplete)";
        return t;
    }
    case 1:
    {
        const int s = int(index.internalId() - 1);
        const StructGroup& g = seeds[s].seed->groups[seeds[s].groupOrder[index.row()]];
        switch (column)
        {
        case C_SEED: return "-";
        case C_STRUCT: return g.name;
        case C_COUNT: return QString::number(g.count);
        case C_X: return g.single ? QString::number(g.pos.x) : QString();
        case C_Z: return g.single ? QString::number(g.pos.z) : QString();
        }
        return QString();
    }
    case 2:
    {
        const int s = int((index.internalId() & ~INSTANCE_LEVEL) >> 16);
        const int gi = int(index.internalId() & 0xffff);
        const Node& node = seeds[s];
        const StructGroup& g = node.seed->groups[gi];
        const std::vector<uint32_t>& order = node.itemOrder[gi];
        const size_t item = order.empty() ? size_t(index.row()) : order[index.row()];
        switch (column)
        {
        case C_SEED: return "-";
        case C_X: return QString::number(g.items[item].p.x);
        case C_Z: return QString::number(g.items[item].p.z);
        case C_DETAIL: return detail(g, item);
        }
        return QString();
    }
    }
    return QString();
}

QVariant StructureResultModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};
    if (role == Qt::DisplayRole)
    {
        const QString t = text(index, index.column());
        if (t.isEmpty())
            return {};
        return t;
    }
    return {};
}

bool StructureResultModel::position(const QModelIndex& index, uint64_t *seed, int *dim, Pos *pos) const
{
    switch (level(index))
    {
    case 1:
    {
        const int s = int(index.internalId() - 1);
        const StructGroup& g = seeds[s].seed->groups[seeds[s].groupOrder[index.row()]];
        if (!g.single)
            return false;
        *seed = seeds[s].seed->seed;
        *dim = g.dim;
        *pos = g.pos;
        return true;
    }
    case 2:
    {
        const int s = int((index.internalId() & ~INSTANCE_LEVEL) >> 16);
        const int gi = int(index.internalId() & 0xffff);
        const Node& node = seeds[s];
        const StructGroup& g = node.seed->groups[gi];
        const std::vector<uint32_t>& order = node.itemOrder[gi];
        const size_t item = order.empty() ? size_t(index.row()) : order[index.row()];
        *seed = node.seed->seed;
        *dim = g.dim;
        *pos = g.items[item].p;
        return true;
    }
    }
    return false;
}

void StructureResultModel::append(const std::vector<StructSeedResultPtr>& added)
{
    if (added.empty())
        return;
    const int first = int(seeds.size());
    beginInsertRows(QModelIndex(), first, first + int(added.size()) - 1);
    for (const StructSeedResultPtr& r : added)
    {
        Node node;
        node.seed = r;
        const size_t n = r->groups.size();
        node.groupOrder.resize(n);
        node.groupRow.resize(n);
        for (size_t g = 0; g < n; g++)
            node.groupOrder[g] = node.groupRow[g] = int(g);
        node.itemOrder.resize(n);
        seedOrder.push_back(int(seeds.size()));
        seedRow.push_back(int(seeds.size()));
        seeds.push_back(std::move(node));
    }
    endInsertRows();
}

void StructureResultModel::clear()
{
    beginResetModel();
    seeds.clear();
    seedOrder.clear();
    seedRow.clear();
    endResetModel();
}

void StructureResultModel::sortRows(int column, Qt::SortOrder dir)
{
    if (column < 0 || seeds.empty())
        return;
    emit layoutAboutToBeChanged();
    const QModelIndexList persistent = persistentIndexList();
    std::vector<std::pair<QModelIndex, std::pair<quintptr, size_t>>> keep;
    // remember persistent indexes by their data, not their row
    for (const QModelIndex& i : persistent)
    {
        size_t key = 0;
        switch (level(i))
        {
        case 0: key = size_t(seedOrder[i.row()]); break;
        case 1: key = size_t(seeds[i.internalId() - 1].groupOrder[i.row()]); break;
        case 2:
        {
            const int s = int((i.internalId() & ~INSTANCE_LEVEL) >> 16);
            const int g = int(i.internalId() & 0xffff);
            const std::vector<uint32_t>& order = seeds[s].itemOrder[g];
            key = order.empty() ? size_t(i.row()) : order[i.row()];
            break;
        }
        }
        keep.push_back({i, {i.internalId(), key}});
    }

    for (Node& node : seeds)
    {
        const StructSeedResult& r = *node.seed;
        for (size_t g = 0; g < r.groups.size(); g++)
        {
            const StructGroup& group = r.groups[g];
            if (group.items.size() < 2)
                continue;
            sortOrder(node.itemOrder[g], group.items.size(), dir, [&](uint32_t i) {
                const StructInstance& in = group.items[i];
                switch (column)
                {
                case C_SEED: return numberKey((long double)(int64_t)r.seed);
                case C_X: return numberKey(in.p.x);
                case C_Z: return numberKey(in.p.z);
                case C_DETAIL:
                {
                    const QString d = detail(group, i);
                    return d.isEmpty() ? SortKey() : textKey(d);
                }
                }
                return SortKey();
            });
        }
        sortOrder(node.groupOrder, r.groups.size(), dir, [&](int g) {
            const StructGroup& group = r.groups[g];
            switch (column)
            {
            case C_SEED: return group.single
                ? numberKey((long double)(int64_t)r.seed) : textKey("-");
            case C_STRUCT: return textKey(group.name);
            case C_COUNT: return numberKey((long double)group.count);
            case C_X: return group.single ? numberKey(group.pos.x) : SortKey();
            case C_Z: return group.single ? numberKey(group.pos.z) : SortKey();
            }
            return SortKey();
        });
        for (size_t row = 0; row < node.groupOrder.size(); row++)
            node.groupRow[node.groupOrder[row]] = int(row);
    }
    sortOrder(seedOrder, seeds.size(), dir, [&](int s) {
        if (column != C_SEED)
            return SortKey();
        return numberKey((long double)(int64_t)seeds[s].seed->seed);
    });
    for (size_t row = 0; row < seedOrder.size(); row++)
        seedRow[seedOrder[row]] = int(row);

    for (const auto& k : keep)
    {
        const QModelIndex& from = k.first;
        int row = 0;
        switch (level(from))
        {
        case 0: row = seedRow[k.second.second]; break;
        case 1: row = seeds[k.second.first - 1].groupRow[k.second.second]; break;
        case 2:
        {
            const int s = int((k.second.first & ~INSTANCE_LEVEL) >> 16);
            const int g = int(k.second.first & 0xffff);
            const std::vector<uint32_t>& order = seeds[s].itemOrder[g];
            row = order.empty() ? int(k.second.second) : int(std::find(order.begin(),
                order.end(), uint32_t(k.second.second)) - order.begin());
            break;
        }
        }
        changePersistentIndex(from, createIndex(row, from.column(), k.second.first));
    }
    emit layoutChanged();
}

// ---------------------------------------------------------------------------
// StructureResultView

StructureResultView::StructureResultView(QWidget *parent)
    : QTreeView(parent)
    , model(new StructureResultModel(this))
{
    model->setHeaders({ TabStructures::tr("seed"), TabStructures::tr("structure"),
        TabStructures::tr("count"), TabStructures::tr("x"), TabStructures::tr("z"),
        TabStructures::tr("details") });
    setModel(model);
    setUniformRowHeights(true);
    setAnimated(false);
    setSortingEnabled(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    header()->setSortIndicatorShown(false);
    header()->setSectionsClickable(true);
    connect(header(), &QHeaderView::sectionClicked, this, [this](int section) {
        // descending, ascending, then unsorted (the rows keep their order)
        if (sortColumn != section)
        {
            sortColumn = section;
            sortOrder = Qt::DescendingOrder;
        }
        else if (sortOrder == Qt::DescendingOrder)
            sortOrder = Qt::AscendingOrder;
        else
            sortColumn = -1;
        header()->setSortIndicatorShown(sortColumn >= 0);
        if (sortColumn >= 0)
        {
            header()->setSortIndicator(sortColumn, sortOrder);
            model->sortRows(sortColumn, sortOrder);
        }
    });
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        uint64_t seed;
        int dim;
        Pos pos;
        if (model->position(index, &seed, &dim, &pos))
            emit positionClicked(seed, dim, pos);
        else if (index.isValid() && !index.parent().isValid())
        {   // a seed row selects the seed
            const QString t = model->text(index.siblingAtColumn(0), 0);
            emit positionClicked(uint64_t(t.section(' ', 0, 0).toLongLong()), DIM_UNDEF,
                Pos{INT_MIN, INT_MIN});
        }
    });
}

void StructureResultView::post(const StructSeedResultPtr& seed)
{
    bool schedule = false;
    {
        QMutexLocker locker(&mutex);
        pending.push_back(seed);
        schedule = !flushQueued;
        flushQueued = true;
    }
    if (schedule)
    {   // collect what arrives in the meantime and add it in one batch
        QMetaObject::invokeMethod(this, [this]() {
            QTimer::singleShot(150, this, &StructureResultView::flush);
        }, Qt::QueuedConnection);
    }
}

void StructureResultView::flush()
{
    std::vector<StructSeedResultPtr> items;
    {
        QMutexLocker locker(&mutex);
        items.swap(pending);
        flushQueued = false;
    }
    if (items.empty())
        return;
    model->append(items);
    if (!sized)
    {   // size the columns once from the first rows
        sized = true;
        for (int c = 0, n = model->columnCount(); c < n; c++)
            resizeColumnToContents(c);
    }
}

void StructureResultView::finish()
{
    flush();
    if (sortColumn >= 0)
        model->sortRows(sortColumn, sortOrder);
}

void StructureResultView::clearResults()
{
    {
        QMutexLocker locker(&mutex);
        pending.clear();
    }
    model->clear();
    sized = false;
}

void StructureResultView::collectRows(const QModelIndex& parent, QStringList& lines, bool all) const
{
    QItemSelectionModel *sel = selectionModel();
    for (int row = 0, n = model->rowCount(parent); row < n; row++)
    {
        const QModelIndex index = model->index(row, 0, parent);
        if (all || (sel && sel->isRowSelected(row, parent)))
        {
            QStringList fields;
            for (int c = 0, nc = model->columnCount(); c < nc; c++)
            {
                const QString t = model->text(index, c);
                fields.append(t == "-" ? QString() : t);
            }
            lines.append(fields.join('\t'));
        }
        if (model->rowCount(index) && (all || isExpanded(index)))
            collectRows(index, lines, all);
    }
}

void StructureResultView::copyRows(bool all)
{
    QStringList lines;
    collectRows(QModelIndex(), lines, all);
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join('\n') + '\n');
}

void StructureResultView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *copy = menu.addAction(QIcon::fromTheme("edit-copy"), TabStructures::tr("Copy selected rows"),
        QKeySequence::Copy, this, [this]() { copyRows(false); });
    copy->setEnabled(selectionModel() && selectionModel()->hasSelection());
    QAction *copyAll = menu.addAction(TabStructures::tr("Copy all rows"), this, [this]() { copyRows(true); });
    copyAll->setEnabled(topLevelItemCount() > 0);
    menu.addSeparator();
    QAction *save = menu.addAction(QIcon::fromTheme("document-save"), TabStructures::tr("Export..."),
        this, &StructureResultView::exportRequested);
    save->setEnabled(topLevelItemCount() > 0);
    menu.exec(event->globalPos());
}

void StructureResultView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy))
        copyRows(false);
    else
        QTreeView::keyPressEvent(event);
}

// ---------------------------------------------------------------------------

void AnalysisStructures::emitStructs(const StructSeedResultPtr& result)
{
    emit structsDone(result);
    if (!isSignalConnected(QMetaMethod::fromSignal(&AnalysisStructures::itemDone)))
        return;
    // the same rows as a tree of items, for listeners that want one
    QTreeWidgetItem *seeditem = new TreeIntItem();
    QString seedText = QString::asprintf("%" PRId64, (int64_t)result->seed);
    if (result->incomplete)
        seedText += " (incomplete)";
    seeditem->setText(0, seedText);
    seeditem->setData(0, Qt::UserRole+0, QVariant::fromValue(result->seed));
    seeditem->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_UNDEF));
    for (const StructGroup& group : result->groups)
    {
        QTreeWidgetItem* stitem = new QTreeWidgetItem(seeditem);
        stitem->setText(C_SEED, "-");
        stitem->setText(C_STRUCT, group.name);
        stitem->setData(C_COUNT, Qt::DisplayRole,
            QVariant::fromValue(static_cast<qulonglong>(group.count)));
        if (group.single)
        {
            stitem->setData(C_X, Qt::DisplayRole, QVariant::fromValue(group.pos.x));
            stitem->setData(C_Z, Qt::DisplayRole, QVariant::fromValue(group.pos.z));
            stitem->setData(0, Qt::UserRole+0, QVariant::fromValue(result->seed));
            stitem->setData(0, Qt::UserRole+1, QVariant::fromValue(group.dim));
            stitem->setData(0, Qt::UserRole+2, QVariant::fromValue(group.pos));
        }
        for (const StructInstance& in : group.items)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(stitem);
            item->setText(C_SEED, "-");
            item->setData(C_X, Qt::DisplayRole, QVariant::fromValue(in.p.x));
            item->setData(C_Z, Qt::DisplayRole, QVariant::fromValue(in.p.z));
            item->setData(0, Qt::UserRole+0, QVariant::fromValue(result->seed));
            item->setData(0, Qt::UserRole+1, QVariant::fromValue(group.dim));
            item->setData(0, Qt::UserRole+2, QVariant::fromValue(in.p));
            QString detail;
            if (in.detail >= 0)
                detail = group.details.value(in.detail);
            else if (group.stype >= 0)
            {
                VarPos vp(in.p, group.stype);
                vp.v = in.v;
                detail = vp.detail().join(":");
            }
            if (!detail.isEmpty())
                item->setText(C_DETAIL, detail);
        }
    }
    emit itemDone(seeditem);
}

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
        struct Result { size_t order; QTreeWidgetItem *item; int kind; StructSeedResultPtr structs; };
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
                QObject::connect(&task, &AnalysisStructures::structsDone,
                    &task,
                    [&](StructSeedResultPtr r) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, nullptr, 0, r});
                    }, Qt::DirectConnection);
                QObject::connect(&task, &AnalysisStructures::quadDone,
                    &task,
                    [&](QTreeWidgetItem *item) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, item, 1, {}});
                    }, Qt::DirectConnection);
                QObject::connect(&task, &AnalysisStructures::densityDone,
                    &task,
                    [&](QTreeWidgetItem *item) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, item, 2, {}});
                    }, Qt::DirectConnection);
                QObject::connect(&task, &AnalysisStructures::fortressDone,
                    &task,
                    [&](QTreeWidgetItem *item) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results.push_back({order, item, 3, {}});
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
                emitStructs(result.structs);
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
    StructSeedResultPtr seedResult(new StructSeedResult);
    seedResult->seed = wi.seed;

    struct StructResult
    {
        int sopt = D_NONE;
        int stype = 0;
        int dim = DIM_UNDEF;
        StructureConfig config = {};
        uint64_t count = 0;
        int shipResult = -1; // End cities: also fills this result with the ships
        bool scan = true;    // false: filled by another result's scan
        StructGroup group;
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
    // End cities with and without a ship come from the same generation:
    // generate them once and split them
    for (size_t i = 0; i < results.size(); i++)
    {
        if (results[i].sopt != D_ENDCITY)
            continue;
        for (size_t j = 0; j < results.size(); j++)
        {
            if (results[j].sopt == D_ENDCITYSHIP)
            {
                results[i].shipResult = int(j);
                results[j].scan = false;
            }
        }
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
        if (!results[resultIndex].scan)
            continue;
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

    // the compact form of the instances; details that need the generated
    // pieces (End cities, fortresses) are kept as text
    const auto addInstances = [](StructGroup& group, std::vector<VarPos>& positions) {
        for (VarPos& vp : positions)
        {
            StructInstance in = {vp.p, vp.v, -1};
            if (!vp.pieces.empty())
            {
                const QStringList detail = vp.detail();
                if (!detail.isEmpty())
                {
                    in.detail = int(group.details.size());
                    group.details.append(detail.join(":"));
                }
            }
            group.items.push_back(in);
        }
    };

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
            const bool split = result.shipResult >= 0;
            std::vector<VarPos> positions;
            std::vector<VarPos> ships;
            uint64_t count = getStructs((collect || split) ? &positions : nullptr,
                result.config, wi, result.dim,
                task.x1, task.z1, task.x2, task.z2, false,
                cancel ? cancel : &stop,
                split ? 0 : result.sopt == D_ENDCITYSHIP ? 1 : result.sopt == D_ENDCITY ? -1 : 0);
            uint64_t shipCount = 0;
            if (split)
            {   // the positions with a ship go to the ship result
                std::vector<VarPos> without;
                for (VarPos& vp : positions)
                {
                    bool ship = false;
                    for (const Piece& piece : vp.pieces)
                        ship |= piece.type == END_SHIP;
                    (ship ? ships : without).push_back(std::move(vp));
                }
                positions.swap(without);
                count = positions.size();
                shipCount = ships.size();
            }
            if (!shouldStop())
            {
                std::lock_guard<std::mutex> lock(mergeMutex);
                result.count += count;
                resultCount.fetch_add(count + shipCount, std::memory_order_relaxed);
                if (collect)
                    addInstances(result.group, positions);
                if (split)
                {
                    StructResult& shipResult = results[result.shipResult];
                    shipResult.count += shipCount;
                    if (collect)
                        addInstances(shipResult.group, ships);
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
        return;

    for (StructResult& result : results)
    {
        if (!result.count)
            continue;
        StructGroup& group = result.group;
        group.sopt = result.sopt;
        group.stype = result.stype;
        group.dim = result.dim;
        group.name = mapopt2display(result.sopt);
        group.count = result.count;
        seedResult->groups.push_back(std::move(group));
    }

    if (!shouldStop() && scanSpawn)
    {
        applySeed(g, 0, wi.seed);
        Pos pos = getSpawn(g);
        if (pos.x >= area.x1 && pos.x <= area.x2 && pos.z >= area.z1 && pos.z <= area.z2)
        {
            StructGroup group;
            group.sopt = D_SPAWN;
            group.dim = DIM_OVERWORLD;
            group.name = "spawn";
            group.count = 1;
            group.single = true;
            group.pos = pos;
            seedResult->groups.push_back(std::move(group));
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
            StructGroup group;
            group.sopt = D_STRONGHOLD;
            group.dim = DIM_OVERWORLD;
            group.name = "stronghold";
            group.count = shp.size();
            if (collect)
                for (Pos pos : shp)
                    group.items.push_back({pos, StructureVariant(), -1});
            seedResult->groups.push_back(std::move(group));
        }
        workDone.fetch_add(1, std::memory_order_relaxed);
    }

    if (seedResult->groups.empty())
        return;
    seedResult->incomplete = shouldStop();
    emitStructs(seedResult);
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
{
    ui->setupUi(this);
    for (QLineEdit *line : {ui->lineX1, ui->lineZ1, ui->lineX2, ui->lineZ2})
        line->setValidator(new QIntValidator(-30000000, 30000000, this));

    QButtonGroup *structureScope = new QButtonGroup(this);
    structureScope->setExclusive(true);
    structureScope->addButton(ui->radioMap);
    structureScope->addButton(ui->radioAll);

    ui->treeStructs->header()->setMinimumSectionSize(24);
    ui->treeStructs->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    ui->treeDensity->setColumnWidth(0, 160);
    setResultTreeSort(ui->treeDensity, C_COUNT, Qt::DescendingOrder, false);
    ui->treeDensity->header()->setMinimumSectionSize(24);
    ui->treeDensity->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    ui->treeDensity->setExpandDepth(1);

    // Results are constructed in the full deterministic key order (seed,
    // fortress coordinates, layout, match coordinates). Start without a live
    // sort column so equal parent/child display cells cannot disturb it.
    ui->treeFortresses->header()->setMinimumSectionSize(24);
    ui->treeFortresses->header()->setStretchLastSection(true);
    ui->treeFortresses->setExpandDepth(2);

    // The analysis thread hands its rows straight to the lists, which add
    // them in a few batches without holding up the thread.
    qRegisterMetaType<StructSeedResultPtr>();
    connect(&thread, &AnalysisStructures::structsDone, ui->treeStructs,
        [this](StructSeedResultPtr result) { ui->treeStructs->post(result); }, Qt::DirectConnection);
    connect(&thread, &AnalysisStructures::quadDone, this,
        [](QTreeWidgetItem *item) { delete item; }, Qt::DirectConnection);
    connect(&thread, &AnalysisStructures::densityDone, ui->treeDensity,
        [this](QTreeWidgetItem *item) { ui->treeDensity->post(item); }, Qt::DirectConnection);
    connect(&thread, &AnalysisStructures::fortressDone, ui->treeFortresses,
        [this](QTreeWidgetItem *item) { ui->treeFortresses->post(item); }, Qt::DirectConnection);
    connect(&thread, &AnalysisStructures::finished, this, &TabStructures::onAnalysisFinished);
    for (ResultTree *tree : {ui->treeDensity, ui->treeFortresses})
        connect(tree, &ResultTree::exportRequested, this, &TabStructures::on_pushExport_clicked);
    connect(ui->treeStructs, &StructureResultView::exportRequested,
        this, &TabStructures::on_pushExport_clicked);
    connect(ui->treeStructs, &StructureResultView::positionClicked,
        this, &TabStructures::onPositionClicked);
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

void TabStructures::onAnalysisFinished()
{
    if (thread.fortress)
        ui->treeFortresses->finish();
    else if (thread.density)
        ui->treeDensity->finish();
    else
        ui->treeStructs->finish();
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
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabStructures::onProgressTimeout()
{
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
        QTimer::singleShot(250, this, &TabStructures::onProgressTimeout);
    }
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

void TabStructures::onPositionClicked(uint64_t seed, int dim, Pos pos)
{
    WorldInfo wi;
    parent->getSeed(&wi);
    wi.seed = seed;
    parent->setSeed(wi, dim);
    if (pos.x != INT_MIN)
        parent->getMapView()->setView(pos.x+0.5, pos.z+0.5);
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
        ui->treeStructs->clearResults();
    }
    else if (ui->tabWidget->currentWidget() == ui->tabDensity)
    {
        thread.density = true;
        datq = thread.area;
        ui->treeDensity->clearResults();
    }
    else
    {
        // Fortress availability follows the canonical active map dimension,
        // independently of the structure overlay selection controls.
        thread.fortress = true;
        thread.dim = DIM_NETHER;
        datf = thread.area;
        ui->treeFortresses->clearResults();
    }

    ui->pushExport->setEnabled(false);
    ui->pushStart->setChecked(true);
    QString progress = QString::asprintf(" (0/%zu)", thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    thread.start();
    QTimer::singleShot(250, this, &TabStructures::onProgressTimeout);
}

void TabStructures::exportResults(CsvWriter& csv)
{
    const auto area = [&](const auto& dat) {
        csv.row({"#X1", QString::number(dat.x1)});
        csv.row({"#Z1", QString::number(dat.z1)});
        csv.row({"#X2", QString::number(dat.x2)});
        csv.row({"#Z2", QString::number(dat.z2)});
    };
    if (ui->tabWidget->currentWidget() == ui->tabStructures)
    {
        area(dats);
        // the rows in display order, as the tree shows them
        const StructureResultModel *model = ui->treeStructs->resultModel();
        std::function<void(const QModelIndex&, const std::function<void(const QModelIndex&)>&)> walk =
            [&](const QModelIndex& parent, const std::function<void(const QModelIndex&)>& visit) {
                for (int row = 0, n = model->rowCount(parent); row < n; row++)
                {
                    const QModelIndex index = model->index(row, 0, parent);
                    visit(index);
                    walk(index, visit);
                }
            };
        if (ui->checkCollect->isChecked())
        {
            csv.row({ tr("seed"), tr("structure"), tr("x"), tr("z"), tr("details") });
            QString seed;
            QString structure;
            walk(QModelIndex(), [&](const QModelIndex& index) {
                const QString seedText = model->text(index, C_SEED);
                if (seedText != "-")
                    seed = seedText;
                const QString structText = model->text(index, C_STRUCT);
                if (!structText.isEmpty())
                    structure = structText;
                uint64_t s;
                int d;
                Pos p;
                if (!model->position(index, &s, &d, &p))
                    return;
                csv.row({ seed, structure, model->text(index, C_X),
                    model->text(index, C_Z), model->text(index, C_DETAIL) });
            });
        }
        else
        {
            std::set<QString> structures;
            std::map<uint64_t, std::map<QString, QString>> cnt; // [seed][stype]

            uint64_t seed = 0;
            QString structure;
            walk(QModelIndex(), [&](const QModelIndex& index) {
                if (!index.parent().isValid())
                    seed = uint64_t(model->text(index, C_SEED).section(' ', 0, 0).toLongLong());
                const QString structText = model->text(index, C_STRUCT);
                if (!structText.isEmpty())
                    structures.insert((structure = structText));
                const QString count = model->text(index, C_COUNT);
                if (!count.isEmpty())
                    cnt[seed][structure] = count;
            });

            QStringList header = { tr("seed") };
            for (auto& sit : structures)
                header.append(sit);
            csv.row(header);
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
                csv.row(cols);
            }
        }
    }
    else if (ui->tabWidget->currentWidget() == ui->tabDensity)
    {
        area(datq);
        csv.row({ tr("seed"), tr("structure"), tr("count"), tr("x"), tr("z"), tr("radius"), tr("details") });
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
            csv.row(cols);
        }
    }
    else if (ui->tabWidget->currentWidget() == ui->tabFortresses)
    {
        area(datf);
        csv.row({
            tr("seed"), tr("layout"), tr("fortress start x"),
            tr("fortress start z"), tr("match x"), tr("match z"),
            tr("orientation"), tr("qualifying pieces"), tr("fortress size"),
            tr("blaze spawners"), tr("nether wart fields")
        });
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
            csv.row(cols);
        }
    }
}

void TabStructures::on_pushExport_clicked()
{
    exportCsv(this, parent->prevdir, tr("Export structure analysis"), "structures.csv",
        parent->config, [this](CsvWriter& csv) { exportResults(csv); });
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

void TabStructures::on_tabWidget_currentChanged(int index)
{
    if (index >= 0 && !thread.isRunning())
        ui->labelStatus->setText(tr("Ready. Analyze this view to update its results."));
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
