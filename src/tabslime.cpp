#include "tabslime.h"

#include "message.h"
#include "util.h"

#include <QGridLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <new>
#include <thread>

namespace {

struct SlimeResult
{
    int count, x, z;
};

static int blockToChunk(int block)
{
    return int(std::floor(block / 16.0));
}

} // namespace

void AnalysisSlime::run()
{
    workers.reset();
    resultCount.store(0, std::memory_order_relaxed);
    const int64_t width64 = int64_t(x2) - x1 + 1;
    const int64_t height64 = int64_t(z2) - z1 + 1;
    if (width64 < window || height64 < window)
    {
        emit failed(tr("The search area must be at least as large as the window."));
        return;
    }
    // Evaluate modest, independent tiles. Each tile includes the trailing
    // window cells needed by its candidate starts, including at tile seams.
    const int64_t candidateWidth = width64 - window + 1;
    const int64_t candidateHeight = height64 - window + 1;
    const uint64_t targetJobs = std::max<uint64_t>(1, mappingThreadCount() * 8);
    const double idealTile = std::sqrt(double(candidateWidth) * double(candidateHeight)
        / double(targetJobs));
    const int tileSize = std::max(128, std::min(int(std::ceil(idealTile)), 1024));
    const uint64_t tilesX = (candidateWidth + tileSize - 1) / tileSize;
    const uint64_t tilesZ = (candidateHeight + tileSize - 1) / tileSize;
    const uint64_t totalTiles = tilesX * tilesZ;
    tileDone.store(0, std::memory_order_relaxed);
    tileTotal.store(totalTiles, std::memory_order_relaxed);

    std::atomic_uint64_t nextTile(0);
    std::atomic_bool allocationFailed(false);
    std::mutex resultMutex;
    std::vector<SlimeResult> found;
    auto scanTiles = [&](size_t) {
        try
        {
            std::vector<SlimeResult> local;
            while (!stop)
            {
                const uint64_t tileIndex = nextTile.fetch_add(1, std::memory_order_relaxed);
                if (tileIndex >= totalTiles)
                    break;
                const int64_t tileX = int64_t(tileIndex % tilesX) * tileSize;
                const int64_t tileZ = int64_t(tileIndex / tilesX) * tileSize;
                const int tileWidth = int(std::min<int64_t>(tileSize, candidateWidth - tileX));
                const int tileHeight = int(std::min<int64_t>(tileSize, candidateHeight - tileZ));
                const int dataWidth = tileWidth + window - 1;
                const int dataHeight = tileHeight + window - 1;
                const size_t stride = size_t(dataWidth) + 1;
                std::vector<uint32_t> prefix(stride * (size_t(dataHeight) + 1), 0);
                const int64_t dataX = int64_t(x1) + tileX;
                const int64_t dataZ = int64_t(z1) + tileZ;
                for (int z = 0; z < dataHeight; z++)
                {
                    if (stop.load(std::memory_order_relaxed))
                        break;
                    uint32_t rowSum = 0;
                    for (int x = 0; x < dataWidth; x++)
                    {
                        if (!(x & 63) && stop.load(std::memory_order_relaxed))
                            break;
                        rowSum += isSlimeChunk(wi.seed, dataX+x, dataZ+z);
                        prefix[size_t(z+1) * stride + x+1] =
                            prefix[size_t(z) * stride + x+1] + rowSum;
                    }
                }
                if (stop.load(std::memory_order_relaxed))
                    break;
                const auto countAt = [&](int x, int z) {
                    const size_t a = size_t(z) * stride + x;
                    const size_t b = size_t(z+window) * stride + x+window;
                    return int(prefix[b] - prefix[size_t(z) * stride + x+window]
                        - prefix[size_t(z+window) * stride + x] + prefix[a]);
                };
                local.clear();
                for (int z = 0; z < tileHeight && !stop; z++)
                for (int x = 0; x < tileWidth && !stop; x++)
                {
                    const int count = countAt(x, z);
                    if (count < minimum)
                        continue;
                    local.push_back({count, int(int64_t(x1) + tileX + x),
                        int(int64_t(z1) + tileZ + z)});
                }
                if (stop.load(std::memory_order_relaxed))
                    break;
                resultCount.fetch_add(local.size(), std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    found.insert(found.end(), local.begin(), local.end());
                }
                tileDone.fetch_add(1, std::memory_order_relaxed);
            }
        }
        catch (const std::bad_alloc&)
        {
            allocationFailed.store(true, std::memory_order_relaxed);
            stop.store(true, std::memory_order_relaxed);
        }
    };
    runMappingWorkers(totalTiles, workers, scanTiles);
    if (allocationFailed.load(std::memory_order_relaxed))
    {
        emit failed(tr("Not enough memory to store all slime-chunk results. "
            "Use a smaller area or a higher minimum."));
        return;
    }
    if (stop)
        return;

    QTreeWidgetItem *batch = nullptr;
    try
    {
        std::sort(found.begin(), found.end(), [](const SlimeResult& a, const SlimeResult& b) {
            if (a.count != b.count) return a.count > b.count;
            if (a.z != b.z) return a.z < b.z;
            return a.x < b.x;
        });
        resultCount.store(found.size(), std::memory_order_relaxed);
        for (const SlimeResult& result : found)
        {
            if (stop.load(std::memory_order_relaxed))
                break;
            if (!batch)
                batch = new QTreeWidgetItem;
            const int bx1 = result.x * 16;
            const int bz1 = result.z * 16;
            const int bx2 = (result.x + window) * 16 - 1;
            const int bz2 = (result.z + window) * 16 - 1;
            QTreeWidgetItem *item = new QTreeWidgetItem(batch);
            item->setData(0, Qt::DisplayRole, result.count);
            item->setData(1, Qt::DisplayRole, window);
            item->setData(2, Qt::DisplayRole, result.x);
            item->setData(3, Qt::DisplayRole, result.z);
            item->setData(4, Qt::DisplayRole, result.x + window - 1);
            item->setData(5, Qt::DisplayRole, result.z + window - 1);
            item->setData(6, Qt::DisplayRole, bx1);
            item->setData(7, Qt::DisplayRole, bz1);
            item->setData(8, Qt::DisplayRole, bx2);
            item->setData(9, Qt::DisplayRole, bz2);
            item->setData(0, Qt::UserRole, wi.seed);
            item->setData(0, Qt::UserRole+1, bx1);
            item->setData(0, Qt::UserRole+2, bz1);
            item->setData(0, Qt::UserRole+3, bx2);
            item->setData(0, Qt::UserRole+4, bz2);
            if (batch->childCount() >= 256)
            {
                emit resultReady(batch);
                batch = nullptr;
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        delete batch;
        stop.store(true, std::memory_order_relaxed);
        emit failed(tr("Not enough memory to display all slime-chunk results. "
            "Use a smaller area or a higher minimum."));
        return;
    }
    if (batch && !stop.load(std::memory_order_relaxed))
        emit resultReady(batch);
    else
        delete batch;
}

TabSlime::TabSlime(MainWindow *parent)
    : QWidget(parent), parent(parent), thread(this)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Find dense slime-chunk areas in the Overworld."), this));

    const auto inputWithInfo = [this](QLineEdit *&edit, const QString& value,
            const QString& info) {
        QWidget *box = new QWidget(this);
        QHBoxLayout *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        edit = new QLineEdit(value, box);
        QToolButton *button = new QToolButton(box);
        button->setText("i");
        button->setAutoRaise(true);
        button->setToolTip(info);
        button->setWhatsThis(info);
        connect(button, &QToolButton::clicked, box, [box, info]() {
            QMessageBox::information(box, QObject::tr("Slime chunk search"), info);
        });
        row->addWidget(edit, 1);
        row->addWidget(button);
        return box;
    };
    const auto spinWithInfo = [this](QSpinBox *&spin, int value, int maximum,
            const QString& info) {
        QWidget *box = new QWidget(this);
        QHBoxLayout *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        spin = new QSpinBox(box);
        spin->setRange(1, maximum);
        spin->setValue(value);
        QToolButton *button = new QToolButton(box);
        button->setText("i");
        button->setAutoRaise(true);
        button->setToolTip(info);
        button->setWhatsThis(info);
        connect(button, &QToolButton::clicked, box, [box, info]() {
            QMessageBox::information(box, QObject::tr("Slime chunk search"), info);
        });
        row->addWidget(spin, 1);
        row->addWidget(button);
        return box;
    };

    QGridLayout *options = new QGridLayout;
    options->addWidget(new QLabel(tr("Chunk X₁:"), this), 0, 0);
    options->addWidget(inputWithInfo(lineX1, "-500",
        tr("First X corner of the search area, in chunks.")), 0, 1);
    options->addWidget(new QLabel(tr("Chunk Z₁:"), this), 0, 2);
    options->addWidget(inputWithInfo(lineZ1, "-500",
        tr("First Z corner of the search area, in chunks.")), 0, 3);
    options->addWidget(new QLabel(tr("Chunk X₂:"), this), 1, 0);
    options->addWidget(inputWithInfo(lineX2, "500",
        tr("Second X corner of the search area, in chunks.")), 1, 1);
    options->addWidget(new QLabel(tr("Chunk Z₂:"), this), 1, 2);
    options->addWidget(inputWithInfo(lineZ2, "500",
        tr("Second Z corner of the search area, in chunks.")), 1, 3);
    QPushButton *fromVisible = new QPushButton(tr("From visible"), this);
    options->addWidget(fromVisible, 0, 4, 2, 1);
    options->addWidget(new QLabel(tr("Window (chunks):"), this), 2, 0, 1, 2);
    options->addWidget(spinWithInfo(spinWindow, 10, 256,
        tr("Tests every possible square with this side length. A value of 10 means a 10 by 10 chunk square.")), 2, 2);
    options->addWidget(new QLabel(tr("Minimum slime chunks:"), this), 3, 0, 1, 2);
    options->addWidget(spinWithInfo(spinMinimum, 15, 65536,
        tr("Only squares with at least this many slime chunks are listed. Higher values return fewer, denser places.")), 3, 2);
    layout->addLayout(options);

    results = new QTreeWidget(this);
    results->setColumnCount(10);
    results->setHeaderLabels({tr("slime"), tr("window"), tr("chunk X1"), tr("chunk Z1"),
        tr("chunk X2"), tr("chunk Z2"), tr("block X1"), tr("block Z1"),
        tr("block X2"), tr("block Z2")});
    configureResultTree(results);
    setResultTreeSort(results, 0, Qt::DescendingOrder, false);
    results->header()->setMinimumSectionSize(24);
    results->header()->setSectionResizeMode(QHeaderView::Stretch);
    results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(results->header(), &QHeaderView::sectionClicked, this,
        [this](int section) { cycleResultTreeSort(results, section); });
    layout->addWidget(results, 1);

    QHBoxLayout *actions = new QHBoxLayout;
    pushExport = new QPushButton(tr("Export..."), this);
    pushExport->setEnabled(false);
    pushStart = new QPushButton(tr("Analyze"), this);
    actions->addWidget(pushExport);
    actions->addWidget(pushStart);
    layout->addLayout(actions);
    for (QLineEdit *line : {lineX1, lineZ1, lineX2, lineZ2})
        line->setValidator(new QIntValidator(-10000000, 10000000, this));

    connect(fromVisible, &QPushButton::clicked, this, &TabSlime::onFromVisible);
    connect(pushStart, &QPushButton::clicked, this, &TabSlime::onStartClicked);
    connect(pushExport, &QPushButton::clicked, this, &TabSlime::onExportClicked);
    connect(results, &QTreeWidget::itemClicked, this, &TabSlime::onItemClicked);
    connect(&thread, &AnalysisSlime::resultReady, this, &TabSlime::onResultReady, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisSlime::failed, this, &TabSlime::onFailed, Qt::QueuedConnection);
    connect(&thread, &AnalysisSlime::finished, this, &TabSlime::onFinished, Qt::QueuedConnection);
}

TabSlime::~TabSlime()
{
    thread.stop = true;
    waitForMappingThread(thread);
}

void TabSlime::save(QSettings& settings)
{
    settings.setValue("slime/x1", lineX1->text());
    settings.setValue("slime/z1", lineZ1->text());
    settings.setValue("slime/x2", lineX2->text());
    settings.setValue("slime/z2", lineZ2->text());
    settings.setValue("slime/window", spinWindow->value());
    settings.setValue("slime/minimum", spinMinimum->value());
}

void TabSlime::load(QSettings& settings)
{
    lineX1->setText(settings.value("slime/x1", lineX1->text()).toString());
    lineZ1->setText(settings.value("slime/z1", lineZ1->text()).toString());
    lineX2->setText(settings.value("slime/x2", lineX2->text()).toString());
    lineZ2->setText(settings.value("slime/z2", lineZ2->text()).toString());
    spinWindow->setValue(settings.value("slime/window", spinWindow->value()).toInt());
    spinMinimum->setValue(settings.value("slime/minimum", spinMinimum->value()).toInt());
    refresh();
}

void TabSlime::refresh()
{
    setEnabled(parent->getDim() == DIM_OVERWORLD);
}

void TabSlime::onStartClicked()
{
    if (thread.isRunning())
    {
        thread.stop = true;
        pushStart->setText(tr("Stopping..."));
        return;
    }
    parent->getSeed(&thread.wi);
    thread.x1 = lineX1->text().toInt();
    thread.z1 = lineZ1->text().toInt();
    thread.x2 = lineX2->text().toInt();
    thread.z2 = lineZ2->text().toInt();
    if (thread.x2 < thread.x1) std::swap(thread.x1, thread.x2);
    if (thread.z2 < thread.z1) std::swap(thread.z1, thread.z2);
    thread.window = spinWindow->value();
    thread.minimum = std::min(spinMinimum->value(), thread.window * thread.window);
    results->clear();
    pushExport->setEnabled(false);
    pushStart->setText(tr("Stop"));
    thread.stop = false;
    thread.workers.reset();
    thread.start();
    QTimer::singleShot(250, this, &TabSlime::onProgress);
}

void TabSlime::exportResults(QTextStream& stream)
{
    const QString quote = parent->config.quote;
    const QString separator = parent->config.separator;
    const auto writeLine = [&](QStringList fields) {
        for (QString& field : fields)
        {
            if (!quote.isEmpty())
                field.replace(quote, quote + quote);
            if (field.contains(separator) || field.contains('\n'))
                field = quote + field + quote;
        }
        stream << fields.join(separator) << '\n';
    };

    stream << "Sep=" << separator << '\n';
    writeLine({"#seed", QString::number(thread.wi.seed)});
    writeLine({"#chunk_x1", lineX1->text()});
    writeLine({"#chunk_z1", lineZ1->text()});
    writeLine({"#chunk_x2", lineX2->text()});
    writeLine({"#chunk_z2", lineZ2->text()});
    writeLine({"#window_chunks", QString::number(spinWindow->value())});
    writeLine({"#minimum_slime_chunks", QString::number(thread.minimum)});

    QStringList headers;
    for (int column = 0; column < results->columnCount(); column++)
        headers.append(results->headerItem()->text(column));
    writeLine(headers);
    for (int row = 0; row < results->topLevelItemCount(); row++)
    {
        QTreeWidgetItem *item = results->topLevelItem(row);
        QStringList fields;
        for (int column = 0; column < results->columnCount(); column++)
            fields.append(item->text(column));
        writeLine(fields);
    }
}

void TabSlime::onExportClicked()
{
#if WASM
    QByteArray content;
    QTextStream stream(&content);
    exportResults(stream);
    QFileDialog::saveFileContent(content, "slime-chunks.csv");
#else
    const QString fileName = QFileDialog::getSaveFileName(this, tr("Export slime-chunk analysis"),
        parent->prevdir, tr("Text files (*.txt *csv);;Any files (*)"));
    if (fileName.isEmpty())
        return;
    QFile file(fileName);
    parent->prevdir = QFileInfo(fileName).absolutePath();
    if (!file.open(QIODevice::WriteOnly))
    {
        warn(parent, tr("Failed to open file for export:\n\"%1\"").arg(fileName));
        return;
    }
    QTextStream stream(&file);
    exportResults(stream);
#endif
}

void TabSlime::onFromVisible()
{
    int x1, z1, x2, z2;
    parent->getMapView()->getVisible(&x1, &z1, &x2, &z2);
    lineX1->setText(QString::number(blockToChunk(x1)));
    lineZ1->setText(QString::number(blockToChunk(z1)));
    lineX2->setText(QString::number(blockToChunk(x2)));
    lineZ2->setText(QString::number(blockToChunk(z2)));
}

void TabSlime::onResultReady(QTreeWidgetItem *item)
{
    results->setUpdatesEnabled(false);
    results->addTopLevelItems(item->takeChildren());
    results->setUpdatesEnabled(true);
    delete item;
}

void TabSlime::onFailed(const QString& message)
{
    warn(this, message);
}

void TabSlime::onFinished()
{
    resortResultTree(results);
    pushStart->setText(tr("Analyze"));
    pushExport->setEnabled(!thread.stop && results->topLevelItemCount() > 0);
}

void TabSlime::onProgress()
{
    if (!thread.isRunning())
        return;
    const uint64_t total = thread.tileTotal.load(std::memory_order_relaxed);
    if (thread.stop.load(std::memory_order_relaxed))
        pushStart->setText(tr("Stopping..."));
    else if (total)
        pushStart->setText(QString::asprintf(
            "%s [%llu/%llu tiles, %zu threads, %llu results]",
            qPrintable(tr("Stop")),
            static_cast<unsigned long long>(thread.tileDone.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(total),
            thread.workers.peak.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(
                thread.resultCount.load(std::memory_order_relaxed))));
    QTimer::singleShot(250, this, &TabSlime::onProgress);
}

void TabSlime::onItemClicked(QTreeWidgetItem *item, int)
{
    const int x1 = item->data(0, Qt::UserRole+1).toInt();
    const int z1 = item->data(0, Qt::UserRole+2).toInt();
    const int x2 = item->data(0, Qt::UserRole+3).toInt();
    const int z2 = item->data(0, Qt::UserRole+4).toInt();
    const qreal viewScale = std::max<qreal>(1.0, (x2 - x1 + 1) / 400.0);
    parent->getMapView()->setView((x1 + x2 + 1) / 2.0, (z1 + z2 + 1) / 2.0, viewScale);
    Shape square;
    square.type = Shape::RECT;
    square.dim = DIM_OVERWORLD;
    square.r = 0;
    square.p1 = Pos{x1, z1};
    square.p2 = Pos{x2+1, z2+1};
    parent->getMapView()->setShapes({square});
}
