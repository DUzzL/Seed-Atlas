#ifndef FORMSEARCHCONTROL_H
#define FORMSEARCHCONTROL_H

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMessageBox>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include "config.h"
#include "searchthread.h"

#include <deque>

namespace Ui {
class FormSearchControl;
}

class MainWindow;

// The world that listed search results and the position to resume from
// belong to: the version and world type, and for a datapack world the packs
// (file names, as sessions store them), the world preset and the dimension.
struct ResultsWorld
{
    bool set = false;
    int mc = 0;
    int worldType = 0;
    bool datapack = false;
    QString preset;
    QString dimension;
    QStringList packs;  // the switched on packs, in load order

    static ResultsWorld of(const WorldInfo& wi);
    bool operator==(const ResultsWorld& o) const
    {
        return set == o.set && mc == o.mc && worldType == o.worldType && datapack == o.datapack
            && preset == o.preset && dimension == o.dimension && packs == o.packs;
    }
    bool operator!=(const ResultsWorld& o) const { return !(*this == o); }
};

class SeedTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit SeedTableModel(QObject *parent = nullptr) :
        QAbstractTableModel(parent) {}

    enum { COL_SEED, COL_TOP16, COL_HEX48, COL_MAX };

    virtual int rowCount(const QModelIndex&) const override { return seeds.size(); }
    virtual int columnCount(const QModelIndex&) const override { return COL_MAX; }

    virtual QVariant data(const QModelIndex& index, int role) const override;
    virtual QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    int insertSeeds(QVector<uint64_t> seeds);
    void removeRow(int row);
    void reset();

    struct Seed
    {
        uint64_t seed;
        QVariant varSeed, varHex48, varTop16;
        QVariant txtSeed, txtHex48, txtTop16;
    };
    QList<Seed> seeds;
};

class SeedSortProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    SeedSortProxy(QObject *parent = nullptr) : QSortFilterProxyModel(parent),column(),order(Qt::DescendingOrder) {}

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Vertical && role == Qt::DisplayRole)
            return QVariant::fromValue(section + 1);
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }

    virtual bool lessThan(const QModelIndex& a, const QModelIndex& b) const override
    {
        const SeedTableModel *seeds = static_cast<const SeedTableModel*>(sourceModel());
        const uint64_t as = seeds->seeds.at(a.row()).seed;
        const uint64_t bs = seeds->seeds.at(b.row()).seed;
        if (a.column() == SeedTableModel::COL_SEED)
            return (int64_t)bs < (int64_t)as;
        const uint64_t av = a.column() == SeedTableModel::COL_TOP16 ? as >> 48 : as & MASK48;
        const uint64_t bv = b.column() == SeedTableModel::COL_TOP16 ? bs >> 48 : bs & MASK48;
        return bv < av;
    }

    virtual void sort(int column, Qt::SortOrder order) override
    {
        if (column >= columnCount())
            return;
        if (column == -1)
            QSortFilterProxyModel::sort(-1, order);
        else
            QSortFilterProxyModel::sort(column, order);
        this->column = column;
        this->order = order;
    }

    int column;
    Qt::SortOrder order;
};

class FormSearchControl : public QWidget
{
    Q_OBJECT

public:
    void invalidateResults();
    explicit FormSearchControl(MainWindow *parent);
    ~FormSearchControl();

    bool event(QEvent *) Q_DECL_OVERRIDE;

    std::vector<uint64_t> getResults();
    SearchConfig getSearchConfig();
    bool setSearchConfig(SearchConfig s, bool quiet);

    void stopSearch();
    bool setList64(QString path, bool quiet);
    bool setList64(QTextStream& stream);

    void setResultsPath(QString path);

    void searchLockUi(bool lock);

    void setSearchMode(int mode);
    bool isSearching() const;
    // simple mode hides the search type, start seed and the 48-bit columns
    void setAdvanced(bool advanced);

    bool getSeed(int row, uint64_t *seed);

    // The world of the listed results and of the resume position (not set
    // before the first search or list). A search in another world starts
    // from the beginning and replaces them.
    const ResultsWorld& resultsWorld() const { return resultsOf; }
    void setResultsWorld(const ResultsWorld& world) { resultsOf = world; }
    // whether results are listed or the search would resume after seed 0
    bool hasResultsOrProgress();

signals:
    void selectedSeedChanged(uint64_t seed);
    void searchStatusChanged(bool running);
    void resultsAdded(int cnt);
    void advancedChanged(bool advanced);

public slots:
    void setSearchRange(uint64_t smin, uint64_t smax);

    void on_buttonClear_clicked();
    void on_buttonStart_clicked();
    void on_buttonMore_clicked();
    void on_buttonExport_clicked();

    void onSort(int column, Qt::SortOrder);
    void onSeedSelectionChanged();
    void on_results_clicked(const QModelIndex &index);
    void on_results_customContextMenuRequested(const QPoint& pos);

    void on_buttonSearchHelp_clicked();

    void on_comboSearchType_currentIndexChanged(int index);

    void pasteResults();
    int pasteList(bool dummy);
    void onBufferTimeout();
    void searchResults(QVector<uint64_t> seeds);
    int searchResultsAdd(std::vector<uint64_t> seeds, bool countonly);
    void searchProgressReset();
    void updateSearchProgress(uint64_t last, uint64_t end, int64_t seed);
    // the progress text sits in the status line above the thin bar
    void setProgressText(const QString& format);
    void searchFinish(bool done);
    void progressTimeout();
    void removeCurrent();
    void copySeed();
    void copyResults();

protected:
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void updateExportButton();

    MainWindow *parent;
    Ui::FormSearchControl *ui;
    class QCheckBox *checkAdvanced = nullptr;
    bool advanced = true; // shows the raw 64-bit progress
    SeedTableModel *model;
    SeedSortProxy *proxy;
    SearchMaster sthread;
    QElapsedTimer elapsed;
    QTimer stimer;
    QFile resultfile;

    // the seed list option is not stored in a widget but is loaded with the "..." button
    QString slist64path;
    QString slist64fnam; // file name without directory
    std::vector<uint64_t> slist64;

    // min and max seeds values
    uint64_t smin, smax;

    // found seeds that are waiting to be added to results
    std::vector<uint64_t> qbuf;
    QSet<uint64_t> resultSeeds;
    // the world the listed results were found for
    ResultsWorld resultsOf;
    // drops the results and the resume position of another world before a
    // search in @world; returns the position to start from
    uint64_t prepareResultsFor(const WorldInfo& world, uint64_t startseed);
    void startDatapackSearch();
    // locks the UI and starts the search that sthread has been set up for
    void beginSearch(Session& session);
    bool searchRunning = false; // from start until searchFinish()
    // set when the filters change during a search: the search is stopped and
    // whatever it still reports belongs to the old filters
    bool discardSearch = false;
    quint64 nextupdate;
    quint64 updt;
};

#endif // FORMSEARCHCONTROL_H
