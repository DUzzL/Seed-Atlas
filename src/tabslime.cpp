#include "tabslime.h"

#include "message.h"

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
    stop = false;
    const int64_t width64 = int64_t(x2) - x1 + 1;
    const int64_t height64 = int64_t(z2) - z1 + 1;
    if (width64 < window || height64 < window)
    {
        emit failed(tr("The search area must be at least as large as the window."));
        return;
    }
    /*
     * Keep the memory use bounded just like the biome locator: each worker
     * evaluates a modest tile and carries a one-candidate border, so windows
     * that straddle two tiles still get exactly the same score.
     */
    constexpr int TILE_SIZE = 1024;
    const int64_t candidateWidth = width64 - window + 1;
    const int64_t candidateHeight = height64 - window + 1;
    const uint64_t tilesX = (candidateWidth + TILE_SIZE - 1) / TILE_SIZE;
    const uint64_t tilesZ = (candidateHeight + TILE_SIZE - 1) / TILE_SIZE;
    const uint64_t totalTiles = tilesX * tilesZ;
    tileDone.store(0, std::memory_order_relaxed);
    tileTotal.store(totalTiles, std::memory_order_relaxed);

    std::atomic_uint64_t nextTile(0);
    std::mutex resultMutex;
    std::vector<SlimeResult> found;
    const unsigned int hw = std::thread::hardware_concurrency();
    const size_t workerCount = std::min<uint64_t>(totalTiles, hw ? hw : 1);

    auto scanTiles = [&]() {
        std::vector<SlimeResult> local;
        while (!stop)
        {
            const uint64_t tileIndex = nextTile.fetch_add(1, std::memory_order_relaxed);
            if (tileIndex >= totalTiles)
                break;
            const int64_t tileX = int64_t(tileIndex % tilesX) * TILE_SIZE;
            const int64_t tileZ = int64_t(tileIndex / tilesX) * TILE_SIZE;
            const int tileWidth = int(std::min<int64_t>(TILE_SIZE, candidateWidth - tileX));
            const int tileHeight = int(std::min<int64_t>(TILE_SIZE, candidateHeight - tileZ));
            const int dataWidth = tileWidth + window + 1;
            const int dataHeight = tileHeight + window + 1;
            const size_t stride = size_t(dataWidth) + 1;
            std::vector<uint32_t> prefix(stride * (size_t(dataHeight) + 1), 0);
            const int64_t dataX = int64_t(x1) + tileX - 1;
            const int64_t dataZ = int64_t(z1) + tileZ - 1;
            for (int z = 0; z < dataHeight; z++)
            {
                uint32_t rowSum = 0;
                for (int x = 0; x < dataWidth; x++)
                {
                    rowSum += isSlimeChunk(wi.seed, dataX+x, dataZ+z);
                    prefix[size_t(z+1) * stride + x+1] =
                        prefix[size_t(z) * stride + x+1] + rowSum;
                }
            }
            const auto countAt = [&](int x, int z) {
                const size_t a = size_t(z) * stride + x;
                const size_t b = size_t(z+window) * stride + x+window;
                return int(prefix[b] - prefix[size_t(z) * stride + x+window]
                    - prefix[size_t(z+window) * stride + x] + prefix[a]);
            };
            for (int z = 0; z < tileHeight && !stop; z++)
            for (int x = 0; x < tileWidth; x++)
            {
                const int count = countAt(x+1, z+1);
                if (count < minimum)
                    continue;
                bool localMaximum = true;
                for (int dz = -1; dz <= 1 && localMaximum; dz++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    if (!dx && !dz)
                        continue;
                    const int64_t nx = tileX + x + dx;
                    const int64_t nz = tileZ + z + dz;
                    if (nx < 0 || nz < 0 || nx >= candidateWidth || nz >= candidateHeight)
                        continue;
                    const int other = countAt(x+dx+1, z+dz+1);
                    if (other > count || (other == count && (nz < tileZ+z ||
                            (nz == tileZ+z && nx < tileX+x))))
                    {
                        localMaximum = false;
                        break;
                    }
                }
                if (localMaximum)
                    local.push_back({count, int(int64_t(x1) + tileX + x),
                        int(int64_t(z1) + tileZ + z)});
            }
            tileDone.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lock(resultMutex);
        found.insert(found.end(), local.begin(), local.end());
    };
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; i++)
        workers.emplace_back(scanTiles);
    for (std::thread& worker : workers)
        worker.join();
    if (stop)
        return;

    std::sort(found.begin(), found.end(), [](const SlimeResult& a, const SlimeResult& b) {
        if (a.count != b.count) return a.count > b.count;
        if (a.z != b.z) return a.z < b.z;
        return a.x < b.x;
    });
    if (found.size() > 4096)
        found.resize(4096);
    for (const SlimeResult& result : found)
    {
        const int bx1 = result.x * 16;
        const int bz1 = result.z * 16;
        const int bx2 = (result.x + window) * 16 - 1;
        const int bz2 = (result.z + window) * 16 - 1;
        QTreeWidgetItem *item = new QTreeWidgetItem;
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
        emit resultReady(item);
    }
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
    results->setSortingEnabled(true);
    results->sortByColumn(0, Qt::DescendingOrder);
    results->header()->setMinimumSectionSize(24);
    results->header()->setSectionResizeMode(QHeaderView::Stretch);
    results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
    thread.wait(500);
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
    writeLine({"#minimum_slime_chunks", QString::number(spinMinimum->value())});

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
    results->addTopLevelItem(item);
}

void TabSlime::onFailed(const QString& message)
{
    warn(this, message);
}

void TabSlime::onFinished()
{
    pushStart->setText(tr("Analyze"));
    pushExport->setEnabled(!thread.stop && results->topLevelItemCount() > 0);
}

void TabSlime::onProgress()
{
    if (!thread.isRunning())
        return;
    const unsigned long total = thread.tileTotal.load(std::memory_order_relaxed);
    if (total)
        pushStart->setText(QString::asprintf("%s [%lu/%lu tiles]",
            qPrintable(tr("Stop")),
            thread.tileDone.load(std::memory_order_relaxed), total));
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
