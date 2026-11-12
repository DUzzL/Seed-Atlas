#ifndef EXPORTDIALOG_H
#define EXPORTDIALOG_H

#include <QDialog>
#include <QAbstractButton>
#include <QVector>
#include <QList>
#include <QThread>
#include <QMutex>
#include <QDir>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "config.h"

namespace Ui {
class ExportDialog;
}
class MainWindow;

// A single (untiled) image is generated in bands of rows by all workers;
// the worker that finishes the last band saves it.
struct ExportImage
{
    std::mutex mutex;
    std::vector<unsigned char> rgb; // 3 bytes per pixel, allocated by the first band
    size_t bytes = 0;
    std::atomic_int remaining{0};
    std::atomic_bool failed{false};

    unsigned char *data()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (rgb.size() != bytes && !failed)
        {
            try { rgb.resize(bytes); }
            catch (const std::bad_alloc&) { failed = true; }
        }
        return failed ? nullptr : rgb.data();
    }
};

struct ExportWorkItem
{
    uint64_t seed;
    int tx, tz;
    QString fnam;
    int row0 = 0, rows = 0; // band of an untiled image
    std::shared_ptr<ExportImage> image;
};

// Colours the biomes of r into rgb (3 bytes per pixel, r.sx pixels per row)
// as the export does, with optional height shading (heightvis >= 0).
bool renderExportRange(unsigned char *rgb, const WorldInfo& wi, int dim,
        uint64_t seed, Range r, int heightvis, const std::atomic_bool *stop);

class ExportDialog;
struct ExportWorker : QThread
{
    Q_OBJECT

 public:
    ExportWorker(ExportDialog *parent) : parent(parent) {}
    virtual ~ExportWorker() {}

    bool runWorkItem(const ExportWorkItem& work);

    virtual void run() override;

signals:
    void workItemDone();

public:
    ExportDialog *parent;
};

class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExportDialog(MainWindow *parent);
    ~ExportDialog();

    bool initWork(ExportWorkItem *work, uint64_t seed, int tx, int tz);
    bool requestWork(ExportWorkItem *work);

    void startWorkers();

signals:
    void exportFinished();
    void workItemDone();

private slots:
    void cancel() { stop = true; }
    void onWorkerFinished();

    void update();

    void on_buttonFromVisible_clicked();
    void on_buttonDirSelect_clicked();

    void on_buttonBox_clicked(QAbstractButton *button);


private:
    Ui::ExportDialog *ui;
    MainWindow *mainwindow;

public:
    QDir dir;
    QString pattern;
    WorldInfo wi;
    int dim;
    int scale;
    int x, z, w, h, y; // block area
    int tilesize; // tile coordinates
    int bgmode;
    int heightvis;
    QMutex mutex;
    QList<ExportWorkItem> workitems;
    QVector<ExportWorker*> workers;
    QVector<QString> paths;

    std::atomic_bool stop;
    std::atomic_bool failed;
    int activeWorkers;
};

#endif // EXPORTDIALOG_H
