#ifndef TABSLIME_H
#define TABSLIME_H

#include "mainwindow.h"

class QLineEdit;
class QSpinBox;
class QTreeWidget;
class QPushButton;

class AnalysisSlime : public QThread
{
    Q_OBJECT
public:
    explicit AnalysisSlime(QObject *parent = nullptr)
        : QThread(parent), tileDone(), tileTotal() {}
    void run() override;

signals:
    void resultReady(QTreeWidgetItem *item);
    void failed(const QString& message);

public:
    WorldInfo wi;
    std::atomic_bool stop;
    std::atomic_ulong tileDone;
    std::atomic_ulong tileTotal;
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
    void onResultReady(QTreeWidgetItem *item);
    void onFailed(const QString& message);
    void onFinished();
    void onProgress();
    void onItemClicked(QTreeWidgetItem *item, int column);

private:
    void exportResults(QTextStream& stream);

private:
    MainWindow *parent;
    AnalysisSlime thread;
    QLineEdit *lineX1, *lineZ1, *lineX2, *lineZ2;
    QSpinBox *spinWindow, *spinMinimum;
    QTreeWidget *results;
    QPushButton *pushStart, *pushExport;
};

#endif // TABSLIME_H
