#ifndef TABBIOMES_H
#define TABBIOMES_H

#include <QWidget>
#include <QThread>
#include <QTreeWidgetItem>
#include <QSortFilterProxyModel>
#include <QHeaderView>

#include "mainwindow.h"
#include "mappingworkers.h"
#include "util.h"

namespace Ui {
class TabBiomes;
}

class AnalysisBiomes : public QThread
{
    Q_OBJECT
public:
    explicit AnalysisBiomes(QObject *parent = nullptr)
        : QThread(parent),stop(false),idx(),workDone(),workTotal(),resultCount() {}

    virtual void run() override;
    void runStatistics(Generator *g);
    bool runLocate(Generator *g);
    // the biome counts of one seed (257 entries, the last is the number of
    // biomes); parallel: over all workers, else in the calling thread
    QVector<uint64_t> countBiomes(uint64_t seed, bool parallel);
    // the same in a datapack world (one entry per biome of it, and the number)
    QVector<uint64_t> countBiomesDatapack(uint64_t seed, bool parallel);
    void runSeedsParallel();

signals:
    void seedDone(uint64_t seed, QVector<uint64_t> cnt);
    void seedItem(QTreeWidgetItem *item);
    void failed(const QString& message);

public:
    std::vector<uint64_t> seeds;
    WorldInfo wi;
    std::atomic_bool stop;
    std::atomic_long idx;
    std::atomic_uint64_t workDone;
    std::atomic_uint64_t workTotal;
    std::atomic_uint64_t resultCount;
    MappingWorkerState workers;
    int dims[3];
    struct Dat {
        int x1, z1, x2, z2;
        int scale;
        int locate;
        int locateDim;
        QVector<int> locateBiomes;
        uint64_t samples;
    } dat;
    int minsize;
    int tolerance;
};

class BiomeTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit BiomeTableModel(QObject *parent = nullptr) :
        QAbstractTableModel(parent), cmp(IdCmp::SORT_LEX, -1, DIM_UNDEF) {}
    virtual ~BiomeTableModel() {}

    virtual int rowCount(const QModelIndex&) const override { return seeds.size(); }
    virtual int columnCount(const QModelIndex&) const override { return ids.size(); }

    virtual QVariant data(const QModelIndex& index, int role) const override;
    virtual QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void insertIds(QSet<int>& ids);
    void insertSeeds(QList<uint64_t>& seeds);
    // @dp: the datapack world of the counts (ids are indices into its biomes)
    void reset(int mc, std::shared_ptr<const DatapackWorld> dp = nullptr);
    qulonglong valueAt(int row, int column) const;
    bool lessId(int a, int b);

    QList<int> ids; // biome column
    QList<uint64_t> seeds; // seed rows
    QMap<int, QMap<uint64_t, QVariant>> cnt; // cnt[id][seed]
    IdCmp cmp;
    // the column of the number of biomes: 256, or the number of biomes of
    // the datapack world
    int countId = 256;
    std::shared_ptr<const DatapackWorld> dp;
};

class BiomeSortProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    BiomeSortProxy(QObject *parent = nullptr) : QSortFilterProxyModel(parent),column(),order(Qt::AscendingOrder) {}

    virtual bool lessThan(const QModelIndex& a, const QModelIndex& b) const override;

    virtual void sort(int column, Qt::SortOrder order) override
    {
        if (column >= columnCount())
            return;
        this->column = column;
        this->order = order;
        sortValues.clear();
        if (column >= 0)
        {
            const BiomeTableModel *table = static_cast<const BiomeTableModel*>(sourceModel());
            sortValues.reserve(table->rowCount(QModelIndex()));
            for (int row = 0; row < table->rowCount(QModelIndex()); ++row)
                sortValues.push_back(table->valueAt(row, column));
        }
        QSortFilterProxyModel::sort(column, order);
    }

    int column;
    Qt::SortOrder order;
    QVector<qulonglong> sortValues;
};

class BiomeHeader : public QHeaderView
{
    Q_OBJECT

public:
    BiomeHeader(QWidget *parent = nullptr);

    void onSectionPress(int section);
    virtual bool event(QEvent *e) override;
    virtual void paintSection(QPainter *painter, const QRect& rect, int section) const override;
    virtual QSize sectionSizeFromContents(int section) const override;

    int hover;
    int pressed;
};

class TabBiomes : public QWidget, public ISaveTab
{
    Q_OBJECT

public:
    explicit TabBiomes(MainWindow *parent = nullptr);
    ~TabBiomes();

    virtual bool event(QEvent *e) override;

    virtual void save(QSettings& settings) override;
    virtual void load(QSettings& settings) override;
    virtual void refresh() override { refreshBiomes(); }
    void stopWork() override;

    void refreshBiomes(int activeid = -1);
    QVector<int> selectedBiomeList() const;
    // the analysis works on a datapack world
    bool datapack() const;
    void updateBiomeSelectionControls();

private slots:
    void onTableSort(int column, Qt::SortOrder);
    void onVHeaderClicked(int row);
    void onAnalysisSeedDone(uint64_t seed, QVector<uint64_t> idcnt);
    void onAnalysisFinished();
    void onBufferTimeout();

    void on_pushStart_clicked();
    void on_pushExport_clicked();
    void on_buttonFromVisible_clicked();
    void on_radioFullSample_toggled(bool checked);
    void on_comboBiome_selectionChanged();
    void on_buttonHighlight_toggled(bool checked);
    void on_treeLocate_itemClicked(QTreeWidgetItem *item, int column);
    void on_tabWidget_currentChanged(int index);

private:
    void exportResults(class CsvWriter& csv);

private:
    Ui::TabBiomes *ui;
    MainWindow *parent;
    AnalysisBiomes thread;
    BiomeTableModel *model;
    BiomeSortProxy *proxy;
    QMap<QString, int> str2biome;
    AnalysisBiomes::Dat dats, datl;
    QSet<int> selectedBiomes;
    // in a datapack world: the selected biomes by id, and the world they
    // were shown for
    QStringList dpSelected;
    std::shared_ptr<const DatapackWorld> dpShown;
    class QLabel *labelDpDimension = nullptr;
    void refreshDatapackBiomes();

    QElapsedTimer elapsed;
    uint64_t updt;
    uint64_t nextupdate;
    QVector<QVector<uint64_t>> qbufs;
};

#endif // TABBIOMES_H
