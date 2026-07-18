#ifndef TABSTRUCTURES_H
#define TABSTRUCTURES_H

#include "mainwindow.h"
#include "mappingworkers.h"

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

signals:
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
    void onHeaderClick(QTreeWidget *tree, int section);

    void onAnalysisItemDone(QTreeWidgetItem *item);
    void onAnalysisQuadDone(QTreeWidgetItem *item);
    void onAnalysisDensityDone(QTreeWidgetItem *item);
    void onAnalysisFortressDone(QTreeWidgetItem *item);
    void onAnalysisFinished();
    void onBufferTimeout();

    void onTreeItemClicked(QTreeWidgetItem *item, int column);

    void on_pushStart_clicked();
    void on_pushExport_clicked();
    void on_buttonFromVisible_clicked();
    void on_tabWidget_currentChanged(int index);

private:
    void exportResults(QTextStream& stream);
    void refreshFortressTab();

private:
    Ui::TabStructures *ui;
    MainWindow *parent;
    AnalysisStructures thread;
    AnalysisStructures::Dat dats, datq, datf;
    QElapsedTimer elapsed;
    uint64_t nextupdate;
    uint64_t updt;
    QList<QTreeWidgetItem*> qbufs;
    QList<QTreeWidgetItem*> qbufq;
    QList<QTreeWidgetItem*> qbufd;
    QList<QTreeWidgetItem*> qbuff;
};

#endif // TABSTRUCTURES_H
