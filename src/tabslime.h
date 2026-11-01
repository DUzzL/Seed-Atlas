#ifndef TABSLIME_H
#define TABSLIME_H

#include "mainwindow.h"
#include "mappingworkers.h"

class QLineEdit;
class QSpinBox;
class QPushButton;
class ResultTable;

class AnalysisSlime : public QThread
{
    Q_OBJECT
public:
    explicit AnalysisSlime(QObject *parent = nullptr)
        : QThread(parent), stop(false), tileDone(), tileTotal(), resultCount() {}
    void run() override;

signals:
    void failed(const QString& message);

public:
    // the sorted results, COLUMNS values per row (see TabSlime)
    static constexpr int COLUMNS = 10;
    std::vector<int> rows;
    WorldInfo wi;
    std::atomic_bool stop;
    std::atomic_uint64_t tileDone;
    std::atomic_uint64_t tileTotal;
    std::atomic_uint64_t resultCount;
    MappingWorkerState workers;
    int x1, z1, x2, z2;
    int window;
    int minimum;
};

class TabSlime : public QWidget, public ISaveTab
{
    Q_OBJECT
public:
    explicit TabSlime(MainWindow *parent = nullptr);
    ~TabSlime();

    void save(QSettings& settings) override;
    void load(QSettings& settings) override;
    void refresh() override;

private slots:
    void onStartClicked();
    void onExportClicked();
    void onFromVisible();
    void onFailed(const QString& message);
    void onFinished();
    void onProgress();
    void onRowClicked(int row);

private:
    MainWindow *parent;
    AnalysisSlime thread;
    QLineEdit *lineX1, *lineZ1, *lineX2, *lineZ2;
    QSpinBox *spinWindow, *spinMinimum;
    ResultTable *results;
    QPushButton *pushStart, *pushExport;
    class QLabel *labelStatus;
    class QLabel *labelDimension; // shown outside the Overworld
    QWidget *body;
};

#endif // TABSLIME_H
