#ifndef TABSTRUCTURES_H
#define TABSTRUCTURES_H

#include "mainwindow.h"
#include "mappingworkers.h"

#include <QAbstractItemModel>
#include <QMutex>
#include <QSharedPointer>
#include <QTreeView>

#include <memory>
#include <vector>

// The structures of one seed in a compact form: millions of instances (e.g.
// geodes) cost a few dozen bytes each instead of a tree widget item.
struct StructInstance
{
    Pos p;
    StructureVariant v;
    int detail; // index into StructGroup::details, or -1 (computed from v)
};

struct StructGroup
{
    int sopt = D_NONE;
    int stype = -1;   // structure type of the instances, -1 if none
    int dim = DIM_UNDEF;
    QString name;
    uint64_t count = 0;
    bool single = false; // one position on the group row itself (spawn)
    Pos pos = {0, 0};
    std::vector<StructInstance> items;
    QStringList details; // details that need the generated pieces
};

struct StructSeedResult
{
    uint64_t seed = 0;
    bool incomplete = false;
    std::vector<StructGroup> groups;
};
using StructSeedResultPtr = QSharedPointer<StructSeedResult>;
Q_DECLARE_METATYPE(StructSeedResultPtr)

// Seeds, structure groups and instances as a three-level tree. Each level
// is shown in a display order (a permutation), so sorting moves no data.
class StructureResultModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    explicit StructureResultModel(QObject *parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void setHeaders(const QStringList& headers) { this->headers = headers; }
    void append(const std::vector<StructSeedResultPtr>& seeds);
    void clear();
    void sortRows(int column, Qt::SortOrder order);
    // the seed, dimension and position of a row, if it has a position
    bool position(const QModelIndex& index, uint64_t *seed, int *dim, Pos *pos) const;
    // the displayed text of a cell; "-" placeholders are kept
    QString text(const QModelIndex& index, int column) const;

private:
    struct Node
    {
        StructSeedResultPtr seed;
        std::vector<int> groupOrder, groupRow;            // display order, inverse
        std::vector<std::vector<uint32_t>> itemOrder;     // per group, empty = natural
    };
    int level(const QModelIndex& index) const;
    QString detail(const StructGroup& group, size_t item) const;

    QStringList headers;
    std::vector<Node> seeds;
    std::vector<int> seedOrder, seedRow;
};

// The structure result list: the same sort cycle, context menu, copy and
// export as the other result lists, on the compact model. Analysis threads
// hand over their seeds with post() from any thread.
class StructureResultView : public QTreeView
{
    Q_OBJECT
public:
    explicit StructureResultView(QWidget *parent = nullptr);

    void post(const StructSeedResultPtr& seed);
    void flush();
    void finish();
    void clearResults();
    int topLevelItemCount() const { return model->rowCount(); }
    StructureResultModel *resultModel() const { return model; }

signals:
    void exportRequested();
    void positionClicked(uint64_t seed, int dim, Pos pos);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void copyRows(bool all);
    void collectRows(const QModelIndex& parent, QStringList& lines, bool all) const;

    StructureResultModel *model;
    QMutex mutex;
    std::vector<StructSeedResultPtr> pending;
    bool flushQueued = false;
    bool sized = false;
    int sortColumn = -1;
    Qt::SortOrder sortOrder = Qt::DescendingOrder;
};

namespace Ui {
class TabStructures;
}

class AnalysisStructures : public QThread
{
    Q_OBJECT
public:
    explicit AnalysisStructures(QObject *parent = nullptr)
        : QThread(parent),stop(false),idx(),workDone(),workTotal(),resultCount(),workersUsed() {}

    virtual void run() override;
    void runStructs(Generator *g);
    void runQuads(Generator *g);
    void runDensity(Generator *g);
    void runFortresses(Generator *g);
    bool shouldStop() const { return stop || (cancel && *cancel); }
    void emitStructs(const StructSeedResultPtr& result);

signals:
    // the structures of a seed (the tab uses this)
    void structsDone(StructSeedResultPtr result);
    // the same as a tree of items; only built when something is connected
    void itemDone(QTreeWidgetItem *item);
    void quadDone(QTreeWidgetItem *item);
    void densityDone(QTreeWidgetItem *item);
    void fortressDone(QTreeWidgetItem *item);

public:
    std::vector<uint64_t> seeds;
    WorldInfo wi;
    int dim;
    std::atomic_bool stop;
    std::atomic_int idx;
    std::atomic_size_t workDone;
    std::atomic_size_t workTotal;
    std::atomic_uint64_t resultCount;
    std::atomic_size_t workersUsed;
    MappingWorkerState workers;
    struct Dat { int x1, z1, x2, z2; } area;
    bool mapshow[D_STRUCT_NUM];
    bool collect = false;
    bool quad = false;
    bool parallelInner = false;
    bool quadHuts = false;
    bool quadMonuments = false;
    int hutQuality = 0;
    int monumentCoverage = 90;
    bool density = false;
    bool densityHuts = false;
    bool densityMonuments = false;
    int densityRadius = 128;
    int densityMinimum = 3;
    bool fortress = false;
    bool fortress2x2 = true;
    bool fortress3x1 = true;
    std::atomic_bool *cancel = nullptr;
};

class TabStructures : public QWidget, public ISaveTab
{
    Q_OBJECT

public:
    explicit TabStructures(MainWindow *parent = nullptr);
    ~TabStructures();

    virtual bool event(QEvent *e) override;

    virtual void save(QSettings& settings) override;
    virtual void load(QSettings& settings) override;
    virtual void refresh() override;

private slots:

    void onAnalysisFinished();
    void onProgressTimeout();

    void onTreeItemClicked(QTreeWidgetItem *item, int column);
    void onPositionClicked(uint64_t seed, int dim, Pos pos);

    void on_pushStart_clicked();
    void on_pushExport_clicked();
    void on_buttonFromVisible_clicked();
    void on_tabWidget_currentChanged(int index);

private:
    void exportResults(class CsvWriter& csv);
    void refreshFortressTab();

private:
    Ui::TabStructures *ui;
    MainWindow *parent;
    AnalysisStructures thread;
    AnalysisStructures::Dat dats, datq, datf;
    QElapsedTimer elapsed;
};

#endif // TABSTRUCTURES_H
