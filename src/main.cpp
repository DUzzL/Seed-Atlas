#include "aboutdialog.h"
#include "conditiondialog.h"
#include "datapackworld.h"
#include "dpsearch.h"
#include "formdpconditions.h"
#include "formsearchcontrol.h"
#include "exportdialog.h"
#include "headless.h"
#include "mcpserver.h"
#include "fortresslayout.h"
#include "mainwindow.h"
#include "dpstructuresdialog.h"
#include "dpmap.h"
#include "dpcheck.h"
#include "motioncontroller.h"
#include "mapview.h"
#include "tabbiomes.h"
#include "tabdatapacks.h"
#include "tablocations.h"
#include "tabportals.h"
#include "tabslime.h"
#include "resultlist.h"
#include "tabstructures.h"
#include "world.h"
#include "seedtables.h"
#include "util.h"
#include "vanilladata.h"
#include "widgets.h"
#include "conditiontext.h"
#include "glyphs.h"

#include "seedatlas-engine/util.h"
#include "seedatlas-engine/quadbase.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QSpinBox>
#include <QHeaderView>
#include <QTextStream>
#include <QTreeWidgetItem>
#include <QTreeView>
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__) && defined(__AVX2__)
// The x86-64 builds need x86-64-v3 (AVX2 and friends, see seed-atlas.pro).
// On an older CPU, say so instead of crashing on an illegal instruction. This
// runs before the other static initializers (which may already use those
// instructions) and is itself compiled for plain x86-64.
__attribute__((constructor(101), target("arch=x86-64")))
static void requireCpuLevel()
{
    __builtin_cpu_init();
    if (__builtin_cpu_supports("x86-64-v3"))
        return;
    const char *msg = "Seed Atlas needs a CPU with AVX2 (Intel Haswell, AMD Excavator "
        "or newer). This processor does not support it.";
#ifdef Q_OS_WIN
    MessageBoxA(NULL, msg, "Seed Atlas", MB_OK | MB_ICONERROR);
#endif
    fputs(msg, stderr); // not fprintf(), which fortified headers inline
    fputs("\n", stderr);
    _Exit(1);
}
#endif

extern "C"
int getStructureConfig_override(int stype, int mc, StructureConfig *sconf)
{
    if unlikely(mc == INT_MAX) // check whether salt overrides are enabled in the engine
        mc = 0;
    int ok = getStructureConfig(stype, mc, sconf);
    // worker threads call this: read the published copy, not g_extgen
    const ExtGenConfig& extgen = activeExtGen();
    if (ok && extgen.saltOverride && stype >= 0 && stype < FEATURE_NUM)
    {
        uint64_t salt = extgen.salts[stype];
        if (salt <= MASK48)
            sconf->salt = salt;
    }
    return ok;
}

namespace {

static quint64 processCpuTime100ns()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    {
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime;
        k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;
        u.HighPart = user.dwHighDateTime;
        return quint64(k.QuadPart + u.QuadPart);
    }
#endif
    return 0;
}

bool testResultTreeSorting(QTextStream& report)
{
    constexpr int count = 50000;
    QTreeWidget tree;
    tree.setColumnCount(1);
    configureResultTree(&tree);
    QList<QTreeWidgetItem*> items;
    items.reserve(count);
    for (int index = 0; index < count; ++index)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem;
        item->setData(0, Qt::DisplayRole, (index * 7919) % count);
        items.push_back(item);
    }
    tree.addTopLevelItems(items);

    QElapsedTimer elapsed;
    elapsed.start();
    setResultTreeSort(&tree, 0, Qt::AscendingOrder);
    const qint64 ascendingMs = elapsed.restart();
    bool ascending = true;
    for (int index = 0; index < count; ++index)
        ascending &= tree.topLevelItem(index)->data(0, Qt::DisplayRole).toInt() == index;
    setResultTreeSort(&tree, 0, Qt::DescendingOrder);
    const qint64 descendingMs = elapsed.elapsed();
    bool descending = true;
    for (int index = 0; index < count; ++index)
        descending &= tree.topLevelItem(index)->data(0, Qt::DisplayRole).toInt() == count-1-index;

    const bool ok = ascending && descending && tree.uniformRowHeights() &&
        !tree.isSortingEnabled();
    report << "result-table-sort-50000: " << (ok ? "PASS" : "FAIL")
        << ", ascending_ms=" << ascendingMs
        << ", descending_ms=" << descendingMs << '\n';
    report.flush();
    return ok;
}

template <class Analysis>
uint64_t mappingResultCount(const Analysis&) { return 0; }

uint64_t mappingResultCount(const AnalysisBiomes& analysis)
{
    return analysis.resultCount.load(std::memory_order_relaxed);
}

uint64_t mappingResultCount(const AnalysisSlime& analysis)
{
    return analysis.resultCount.load(std::memory_order_relaxed);
}

uint64_t mappingResultCount(const AnalysisStructures& analysis)
{
    return analysis.resultCount.load(std::memory_order_relaxed);
}

template <class Analysis>
bool testMappingStop(const QString& name, Analysis& analysis, QTextStream& report,
        qint64 loadedTargetMs = 1000)
{
    const size_t expected = mappingThreadCount();
    report << name << ": START, expected=" << expected << '\n';
    report.flush();
    analysis.start();
    QElapsedTimer startup;
    startup.start();
    while (analysis.isRunning() &&
            (analysis.workers.started.load(std::memory_order_relaxed) < expected ||
             analysis.workers.active.load(std::memory_order_relaxed) < expected) &&
            startup.elapsed() < 5000)
        QThread::msleep(1);

    const quint64 cpuStart = processCpuTime100ns();
    QElapsedTimer loaded;
    loaded.start();
    while (analysis.isRunning() &&
            analysis.workers.active.load(std::memory_order_relaxed) == expected &&
            loaded.elapsed() < loadedTargetMs)
        QThread::msleep(1);
    const qint64 loadedMs = loaded.elapsed();
    const quint64 cpuEnd = processCpuTime100ns();
    const double cpuPercent = cpuStart && cpuEnd >= cpuStart && loadedMs > 0
        ? 100.0 * double(cpuEnd - cpuStart) /
            (double(loadedMs) * 10000.0 * double(expected)) : -1.0;
    const bool wasRunning = analysis.isRunning();
    analysis.stop.store(true, std::memory_order_relaxed);
    QElapsedTimer stopping;
    stopping.start();
    bool joined = analysis.wait(2000);
    const qint64 stopMs = stopping.elapsed();
    if (!joined)
    {
        // Test-only containment: production analysis never terminates threads
        // forcibly because doing so can corrupt engine/Qt state.
        analysis.terminate();
        analysis.wait();
    }

    const size_t planned = analysis.workers.planned.load(std::memory_order_relaxed);
    const size_t started = analysis.workers.started.load(std::memory_order_relaxed);
    const size_t peak = analysis.workers.peak.load(std::memory_order_relaxed);
    const uint64_t results = mappingResultCount(analysis);
    const bool ok = wasRunning && joined && planned == expected &&
        started >= expected && peak == expected &&
        loadedMs >= loadedTargetMs * 9 / 10 &&
        (cpuPercent < 0 || cpuPercent >= 75.0) && stopMs <= 1000;
    report << name << ": " << (ok ? "PASS" : "FAIL")
        << ", expected=" << expected
        << ", planned=" << planned
        << ", started=" << started
        << ", peak=" << peak
        << ", results=" << results
        << ", loaded_ms=" << loadedMs;
    if (cpuPercent >= 0)
        report << ", cpu=" << QString::number(cpuPercent, 'f', 1) << '%';
    report
        << ", stop_ms=" << stopMs << '\n';
    report.flush();
    return ok;
}

static void fnvInt32(uint64_t& hash, int value)
{
    const uint32_t bits = static_cast<uint32_t>(value);
    for (int byte = 0; byte < 4; byte++)
    {
        hash ^= (bits >> (byte * 8)) & 0xff;
        hash *= 1099511628211ULL;
    }
}

bool testSlimeCompleteness(const QString& name, QTextStream& report,
        int x1, int z1, int x2, int z2, int window, int minimum,
        uint64_t expectedCount, int64_t expectedSumX, int64_t expectedSumZ,
        int64_t expectedSumCount, uint64_t expectedHash)
{
    AnalysisSlime analysis;
    analysis.wi.reset();
    analysis.wi.seed = 6384876098956146605ULL;
    analysis.x1 = x1;
    analysis.z1 = z1;
    analysis.x2 = x2;
    analysis.z2 = z2;
    analysis.window = window;
    analysis.minimum = minimum;
    uint64_t delivered = 0;
    uint64_t hash = 14695981039346656037ULL;
    int64_t sumX = 0, sumZ = 0, sumCount = 0;
    bool sorted = true;
    bool first = true;
    int previousCount = 0, previousX = 0, previousZ = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    analysis.start();
    const bool joined = analysis.wait(120000);
    if (!joined)
    {
        analysis.stop.store(true, std::memory_order_relaxed);
        analysis.wait(2000);
    }
    // the rows handed to the result table: slime count, window, chunk x, chunk z, ...
    for (size_t row = 0; row + AnalysisSlime::COLUMNS <= analysis.rows.size();
            row += AnalysisSlime::COLUMNS)
    {
        const int count = analysis.rows[row + 0];
        const int x = analysis.rows[row + 2];
        const int z = analysis.rows[row + 3];
        if (!first && (count > previousCount ||
                (count == previousCount && (z < previousZ ||
                (z == previousZ && x < previousX)))))
            sorted = false;
        first = false;
        previousCount = count;
        previousX = x;
        previousZ = z;
        delivered++;
        sumX += x;
        sumZ += z;
        sumCount += count;
        fnvInt32(hash, count);
        fnvInt32(hash, x);
        fnvInt32(hash, z);
    }
    const uint64_t count = analysis.resultCount.load(std::memory_order_relaxed);
    const uint64_t done = analysis.tileDone.load(std::memory_order_relaxed);
    const uint64_t total = analysis.tileTotal.load(std::memory_order_relaxed);
    const bool ok = joined && !analysis.stop.load(std::memory_order_relaxed) &&
        count == expectedCount && delivered == count && done == total && sorted &&
        sumX == expectedSumX && sumZ == expectedSumZ &&
        sumCount == expectedSumCount && hash == expectedHash &&
        analysis.workers.peak.load(std::memory_order_relaxed) == mappingThreadCount();
    report << name << ": " << (ok ? "PASS" : "FAIL")
        << ", results=" << count
        << ", delivered=" << delivered
        << ", tiles=" << done << '/' << total
        << ", threads=" << analysis.workers.peak.load(std::memory_order_relaxed)
        << ", sum=" << sumX << '/' << sumZ << '/' << sumCount
        << ", fnv=0x" << QString("%1").arg(hash, 16, 16, QLatin1Char('0'))
        << ", elapsed_ms=" << elapsed.elapsed() << '\n';
    report.flush();
    return ok;
}

struct RawQuadTuple
{
    int rx, rz;
    uint64_t canonical;

    bool operator<(const RawQuadTuple& other) const
    {
        return std::tie(rx, rz, canonical) <
            std::tie(other.rx, other.rz, other.canonical);
    }
};

static QString rawQuadHash(std::vector<RawQuadTuple> tuples)
{
    std::sort(tuples.begin(), tuples.end());
    QByteArray bytes;
    bytes.reserve(qsizetype(tuples.size() * 16));
    const auto appendLittleEndian = [&](uint64_t value, int count) {
        for (int byte = 0; byte < count; byte++)
            bytes.append(char((value >> (byte * 8)) & 0xff));
    };
    for (const RawQuadTuple& tuple : tuples)
    {
        appendLittleEndian(static_cast<uint32_t>(tuple.rx), 4);
        appendLittleEndian(static_cast<uint32_t>(tuple.rz), 4);
        appendLittleEndian(tuple.canonical, 8);
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool testQuadWorld(const QString& name, QTextStream& report, int stype,
        uint64_t seed, uint64_t expectedCount, const QString& expectedHash,
        int knownRx = INT_MAX, int knownRz = INT_MAX)
{
    WorldInfo wi;
    wi.mc = MC_NEWEST;
    wi.seed = seed;
    Generator g;
    setupGenerator(&g, wi.mc, wi.genFlags());
    applySeed(&g, DIM_OVERWORLD, seed);
    StructureConfig sconf;
    if (!getStructureConfig_override(stype, wi.mc, &sconf))
    {
        report << name << ": FAIL, unsupported structure\n";
        return false;
    }

    QVector<QuadInfo> found;
    QElapsedTimer elapsed;
    elapsed.start();
    findQuadStructsInRegions(stype, &g, &found,
        -58595, -58595, 117189, 117189);
    const qint64 elapsedMs = elapsed.elapsed();

    std::vector<RawQuadTuple> tuples;
    tuples.reserve(found.size());
    const int scale = sconf.regionSize * 16;
    for (const QuadInfo& quad : found)
    {
        const int rx = floordiv(quad.p[0].x, scale);
        const int rz = floordiv(quad.p[0].z, scale);
        const uint64_t canonical =
            (moveStructure(seed & MASK48, -rx, -rz) + sconf.salt) & MASK48;
        tuples.push_back({rx, rz, canonical});
    }
    const QString hash = rawQuadHash(tuples);
    const bool noDuplicates = [&]() {
        std::sort(tuples.begin(), tuples.end());
        return std::adjacent_find(tuples.begin(), tuples.end(),
            [](const RawQuadTuple& a, const RawQuadTuple& b) {
                return a.rx == b.rx && a.rz == b.rz &&
                    a.canonical == b.canonical;
            }) == tuples.end();
    }();
    bool knownFound = knownRx == INT_MAX;
    QString viability = "-";
    if (knownRx != INT_MAX)
    {
        knownFound = std::any_of(tuples.begin(), tuples.end(),
            [&](const RawQuadTuple& tuple) {
                return tuple.rx == knownRx && tuple.rz == knownRz;
            });
        Pos positions[4];
        getStructurePos(stype, wi.mc, seed, knownRx, knownRz, positions+0);
        getStructurePos(stype, wi.mc, seed, knownRx, knownRz+1, positions+1);
        getStructurePos(stype, wi.mc, seed, knownRx+1, knownRz, positions+2);
        getStructurePos(stype, wi.mc, seed, knownRx+1, knownRz+1, positions+3);
        viability.clear();
        for (const Pos& pos : positions)
            viability += isViableStructurePos(stype, &g, pos.x, pos.z, 0) ? '1' : '0';
    }
    const bool ok = uint64_t(found.size()) == expectedCount &&
        hash.compare(expectedHash, Qt::CaseInsensitive) == 0 &&
        noDuplicates && knownFound;
    report << name << ": " << (ok ? "PASS" : "FAIL")
        << ", raw_candidates=" << found.size()
        << ", sha256=" << hash
        << ", known_anchor=" << (knownFound ? "yes" : "no")
        << ", biome_viability=" << viability
        << ", elapsed_ms=" << elapsedMs << '\n';
    report.flush();
    return ok;
}

bool testQuadAnalysis(const QString& name, QTextStream& report, int stype,
        uint64_t seed, uint64_t expectedCount)
{
    AnalysisStructures analysis;
    analysis.wi.reset();
    analysis.wi.mc = MC_NEWEST;
    analysis.seeds = {seed};
    analysis.dim = DIM_OVERWORLD;
    analysis.area = {-30000000, -30000000, 30000000, 30000000};
    std::fill(std::begin(analysis.mapshow), std::end(analysis.mapshow), false);
    analysis.collect = false;
    analysis.quad = true;
    analysis.parallelInner = true;
    analysis.quadHuts = stype == Swamp_Hut;
    analysis.quadMonuments = stype == Monument;
    analysis.hutQuality = CST_BARELY;
    analysis.monumentCoverage = 90;
    uint64_t delivered = 0;
    QObject::connect(&analysis, &AnalysisStructures::quadDone, &analysis,
        [&](QTreeWidgetItem *item) {
            delivered += item->childCount();
            delete item;
        }, Qt::DirectConnection);

    const quint64 cpuStart = processCpuTime100ns();
    QElapsedTimer elapsed;
    elapsed.start();
    analysis.start();
    const bool joined = analysis.wait(120000);
    const qint64 elapsedMs = elapsed.elapsed();
    const quint64 cpuEnd = processCpuTime100ns();
    if (!joined)
    {
        analysis.stop.store(true, std::memory_order_relaxed);
        analysis.wait(2000);
    }
    const uint64_t count = analysis.resultCount.load(std::memory_order_relaxed);
    const size_t peak = analysis.workers.peak.load(std::memory_order_relaxed);
    const uint64_t done = analysis.workDone.load(std::memory_order_relaxed);
    const uint64_t total = analysis.workTotal.load(std::memory_order_relaxed);
    const double cpuPercent = cpuStart && cpuEnd >= cpuStart && elapsedMs >= 100
        ? 100.0 * double(cpuEnd-cpuStart) /
            (double(elapsedMs) * 10000.0 * double(mappingThreadCount())) : -1.0;
    const bool ok = joined && !analysis.stop.load(std::memory_order_relaxed) &&
        count == expectedCount && delivered == expectedCount && done == total &&
        peak == mappingThreadCount();
    report << name << ": " << (ok ? "PASS" : "FAIL")
        << ", results=" << count
        << ", delivered=" << delivered
        << ", tasks=" << done << '/' << total
        << ", threads=" << peak
        << ", elapsed_ms=" << elapsedMs;
    if (cpuPercent >= 0)
        report << ", cpu=" << QString::number(cpuPercent, 'f', 1) << '%';
    report << '\n';
    report.flush();
    return ok;
}

bool testDensityAnalysis(QTextStream& report)
{
    AnalysisStructures analysis;
    analysis.wi.reset();
    analysis.wi.mc = MC_NEWEST;
    analysis.seeds = {6384876098956146605ULL};
    analysis.dim = DIM_OVERWORLD;
    analysis.area = {-100000, -100000, 100000, 100000};
    std::fill(std::begin(analysis.mapshow), std::end(analysis.mapshow), false);
    analysis.collect = false;
    analysis.quad = false;
    analysis.density = true;
    analysis.parallelInner = true;
    analysis.densityHuts = true;
    analysis.densityMonuments = true;
    analysis.densityRadius = 128;
    analysis.densityMinimum = 2;
    uint64_t delivered = 0;
    QObject::connect(&analysis, &AnalysisStructures::densityDone, &analysis,
        [&](QTreeWidgetItem *item) {
            delivered += item->childCount();
            delete item;
        }, Qt::DirectConnection);

    QElapsedTimer elapsed;
    elapsed.start();
    analysis.start();
    const bool joined = analysis.wait(120000);
    const qint64 elapsedMs = elapsed.elapsed();
    if (!joined)
    {
        analysis.stop.store(true, std::memory_order_relaxed);
        analysis.wait(2000);
    }
    const uint64_t count = analysis.resultCount.load(std::memory_order_relaxed);
    const uint64_t done = analysis.workDone.load(std::memory_order_relaxed);
    const uint64_t total = analysis.workTotal.load(std::memory_order_relaxed);
    const size_t peak = analysis.workers.peak.load(std::memory_order_relaxed);
    const bool ok = joined && !analysis.stop.load(std::memory_order_relaxed) &&
        delivered == count && done == total && peak == mappingThreadCount();
    report << "density-analysis-minimum-2: " << (ok ? "PASS" : "FAIL")
        << ", results=" << count
        << ", delivered=" << delivered
        << ", tasks=" << done << '/' << total
        << ", threads=" << peak
        << ", elapsed_ms=" << elapsedMs << '\n';
    report.flush();
    return ok;
}

static Piece syntheticFortressPiece(int type, int x0, int z0, int x1, int z1,
        int y = 64)
{
    Piece piece = {};
    piece.type = type;
    piece.bb0 = {x0, y, z0};
    piece.bb1 = {x1, y + 8, z1};
    return piece;
}

static bool hasLayout(const FortressLayoutResult& result,
    FortressLayoutType type, FortressLineOrientation orientation = FORTRESS_LINE_NONE,
    int pieceCount = 0)
{
    return std::any_of(result.matches.begin(), result.matches.end(),
        [&](const FortressLayoutMatch& match) {
            return match.type == type &&
                (orientation == FORTRESS_LINE_NONE || match.orientation == orientation) &&
                (!pieceCount || match.pieceCount == pieceCount);
        });
}

static bool legacySquareReference(const std::vector<Piece>& source, Pos *position)
{
    std::vector<Piece> pieces;
    for (const Piece& piece : source)
        if (piece.type == FORTRESS_START || piece.type == BRIDGE_CROSSING)
            pieces.push_back(piece);
    if (pieces.size() < 4)
        return false;
    for (size_t i = 0; i < pieces.size(); i++)
    {
        int adjacent = 0;
        for (size_t j = 0; j < pieces.size(); j++)
        {
            if (pieces[i].bb0.y != pieces[j].bb0.y) continue;
            if (pieces[i].bb1.x != pieces[j].bb1.x &&
                    pieces[i].bb1.x+1 != pieces[j].bb0.x) continue;
            if (pieces[i].bb1.z != pieces[j].bb1.z &&
                    pieces[i].bb1.z+1 != pieces[j].bb0.z) continue;
            adjacent++;
        }
        if (adjacent >= 4)
        {
            *position = {pieces[i].bb1.x, pieces[i].bb1.z};
            return true;
        }
    }
    return false;
}

bool testFortressLayoutDetector(QTextStream& report)
{
    const auto crossing = [](int x0, int z0, int x1, int z1, int y = 64) {
        return syntheticFortressPiece(BRIDGE_CROSSING, x0, z0, x1, z1, y);
    };
    const std::vector<Piece> square = {
        crossing(0, 0, 9, 9), crossing(10, 0, 19, 9),
        crossing(0, 10, 9, 19), crossing(10, 10, 19, 19),
    };
    const std::vector<Piece> horizontal = {
        crossing(0, 0, 9, 9), crossing(10, 0, 19, 9), crossing(20, 0, 29, 9),
    };
    const std::vector<Piece> vertical = {
        crossing(0, 0, 9, 9), crossing(0, 10, 9, 19), crossing(0, 20, 9, 29),
    };
    const std::vector<Piece> four = {
        crossing(0, 0, 9, 9), crossing(10, 0, 19, 9),
        crossing(20, 0, 29, 9), crossing(30, 0, 39, 9),
    };
    const std::vector<Piece> lshape = {
        crossing(0, 0, 9, 9), crossing(10, 0, 19, 9), crossing(10, 10, 19, 19),
    };
    const std::vector<Piece> levels = {
        crossing(0, 0, 9, 9), crossing(10, 0, 19, 9), crossing(20, 0, 29, 9, 65),
    };
    const std::vector<Piece> gap = {
        crossing(0, 0, 9, 9), crossing(11, 0, 20, 9), crossing(21, 0, 30, 9),
    };
    std::vector<Piece> withStart = horizontal;
    withStart[1].type = FORTRESS_START;
    std::vector<Piece> unrelated = horizontal;
    unrelated[1].type = BRIDGE_STRAIGHT;

    const FortressLayoutResult squareResult = findFortressLayouts(
        square.data(), int(square.size()), true, true);
    const FortressLayoutResult horizontalResult = findFortressLayouts(
        horizontal.data(), int(horizontal.size()), true, true);
    const FortressLayoutResult verticalResult = findFortressLayouts(
        vertical.data(), int(vertical.size()), true, true);
    const FortressLayoutResult fourResult = findFortressLayouts(
        four.data(), int(four.size()), false, true);
    const FortressLayoutResult lResult = findFortressLayouts(
        lshape.data(), int(lshape.size()), true, true);
    const FortressLayoutResult levelsResult = findFortressLayouts(
        levels.data(), int(levels.size()), true, true);
    const FortressLayoutResult gapResult = findFortressLayouts(
        gap.data(), int(gap.size()), true, true);
    const FortressLayoutResult startResult = findFortressLayouts(
        withStart.data(), int(withStart.size()), false, true);
    const FortressLayoutResult unrelatedResult = findFortressLayouts(
        unrelated.data(), int(unrelated.size()), false, true);

    Pos legacy = {};
    const bool oldMatch = legacySquareReference(square, &legacy);
    const bool compatibility = oldMatch && squareResult.has2x2 &&
        legacy.x == squareResult.legacy2x2Position.x &&
        legacy.z == squareResult.legacy2x2Position.z &&
        legacy.x == 9 && legacy.z == 9;
    const bool ok = hasLayout(squareResult, FORTRESS_LAYOUT_2X2) &&
        hasLayout(horizontalResult, FORTRESS_LAYOUT_3X1, FORTRESS_LINE_X, 3) &&
        hasLayout(verticalResult, FORTRESS_LAYOUT_3X1, FORTRESS_LINE_Z, 3) &&
        hasLayout(fourResult, FORTRESS_LAYOUT_3X1, FORTRESS_LINE_X, 4) &&
        !lResult.has3x1 && !levelsResult.has3x1 && !gapResult.has3x1 &&
        startResult.has3x1 && !unrelatedResult.has3x1 && compatibility;
    report << "fortress-layout-detector: " << (ok ? "PASS" : "FAIL")
        << ", square=" << squareResult.has2x2
        << ", horizontal=" << horizontalResult.has3x1
        << ", vertical=" << verticalResult.has3x1
        << ", four-piece=" << fourResult.has3x1
        << ", l-shape=" << lResult.has3x1
        << ", levels=" << levelsResult.has3x1
        << ", gap=" << gapResult.has3x1
        << ", start=" << startResult.has3x1
        << ", unrelated=" << unrelatedResult.has3x1
        << ", legacy-pos=" << legacy.x << '/' << legacy.z << '\n';
    report.flush();
    return ok;
}

struct FortressAnalysisSnapshot
{
    QStringList rows;
    std::vector<std::pair<int,int>> starts;
    uint64_t resultCount = 0;
    size_t workDone = 0;
    size_t workTotal = 0;
    size_t peak = 0;
};

static FortressAnalysisSnapshot runFortressSnapshot(bool parallel)
{
    AnalysisStructures analysis;
    analysis.wi.reset();
    analysis.wi.mc = MC_NEWEST;
    analysis.wi.seed = 6384876098956146605ULL;
    analysis.dim = DIM_NETHER;
    analysis.area = {-16384, -16384, 16384, 16384};
    analysis.parallelInner = parallel;
    analysis.fortress = true;
    analysis.fortress2x2 = true;
    analysis.fortress3x1 = true;
    FortressAnalysisSnapshot snapshot;
    QObject::connect(&analysis, &AnalysisStructures::fortressDone, &analysis,
        [&](QTreeWidgetItem *seedItem) {
            for (int fortressIndex = 0;
                    fortressIndex < seedItem->childCount(); fortressIndex++)
            {
                QTreeWidgetItem *fortress = seedItem->child(fortressIndex);
                const int startX = fortress->data(2, Qt::DisplayRole).toInt();
                const int startZ = fortress->data(3, Qt::DisplayRole).toInt();
                snapshot.starts.emplace_back(startX, startZ);
                for (int matchIndex = 0;
                        matchIndex < fortress->childCount(); matchIndex++)
                {
                    QTreeWidgetItem *match = fortress->child(matchIndex);
                    QStringList columns;
                    for (int column = 1; column <= 10; column++)
                        columns << match->text(column);
                    snapshot.rows << columns.join(QLatin1Char('|'));
                }
            }
            delete seedItem;
        }, Qt::DirectConnection);
    Generator generator;
    setupGenerator(&generator, analysis.wi.mc, analysis.wi.genFlags());
    analysis.workers.reset();
    analysis.runFortresses(&generator);
    snapshot.resultCount = analysis.resultCount.load(std::memory_order_relaxed);
    snapshot.workDone = analysis.workDone.load(std::memory_order_relaxed);
    snapshot.workTotal = analysis.workTotal.load(std::memory_order_relaxed);
    snapshot.peak = analysis.workers.peak.load(std::memory_order_relaxed);
    return snapshot;
}

bool testFortressAnalysis(QTextStream& report)
{
    QElapsedTimer elapsed;
    elapsed.start();
    FortressAnalysisSnapshot serial = runFortressSnapshot(false);
    FortressAnalysisSnapshot parallel = runFortressSnapshot(true);
    const bool sorted = std::is_sorted(parallel.starts.begin(), parallel.starts.end());
    const bool noDuplicates = std::adjacent_find(parallel.starts.begin(),
        parallel.starts.end()) == parallel.starts.end();
    const bool same = serial.rows == parallel.rows &&
        serial.starts == parallel.starts &&
        serial.resultCount == parallel.resultCount;
    const bool progress = serial.workDone == serial.workTotal &&
        parallel.workDone == parallel.workTotal;
    const QByteArray rows = parallel.rows.join(QLatin1Char('\n')).toUtf8();
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(rows, QCryptographicHash::Sha256).toHex());
    const bool expectedRegression = parallel.resultCount == 300 &&
        parallel.rows.size() == 311 &&
        hash.compare(QStringLiteral(
            "7bb43b382790f67e584ab820c9671027729366eb15edb7953b7c93f3d35526c1"),
            Qt::CaseInsensitive) == 0;
    const bool ok = same && expectedRegression && sorted && noDuplicates &&
        parallel.resultCount == parallel.starts.size() && progress &&
        parallel.peak == mappingThreadCount();
    report << "fortress-analysis: " << (ok ? "PASS" : "FAIL")
        << ", fortresses=" << parallel.resultCount
        << ", matches=" << parallel.rows.size()
        << ", duplicate-free=" << noDuplicates
        << ", serial-equal=" << same
        << ", tasks=" << parallel.workDone << '/' << parallel.workTotal
        << ", threads=" << parallel.peak
        << ", sha256=" << hash
        << ", elapsed_ms=" << elapsed.elapsed() << '\n';
    report.flush();
    return ok;
}

static std::vector<std::pair<int,int>> structureCoordinates(
        const std::vector<VarPos>& positions)
{
    std::vector<std::pair<int,int>> coordinates;
    coordinates.reserve(positions.size());
    for (const VarPos& position : positions)
        coordinates.emplace_back(position.p.x, position.p.z);
    std::sort(coordinates.begin(), coordinates.end());
    return coordinates;
}

bool testStructureCompleteness(QTextStream& report)
{
    WorldInfo wi;
    wi.mc = MC_NEWEST;
    wi.seed = 6384876098956146605ULL;
    StructureConfig sconf;
    if (!getStructureConfig_override(Geode, wi.mc, &sconf))
    {
        report << "structures-geode-completeness: FAIL, unsupported structure\n";
        return false;
    }
    std::vector<VarPos> reference;
    QElapsedTimer referenceElapsed;
    referenceElapsed.start();
    const uint64_t referenceCount = getStructs(&reference, sconf, wi,
        sconf.dim, -4096, -4096, 4096, 4096, false);
    const qint64 referenceMs = referenceElapsed.elapsed();
    const std::vector<std::pair<int,int>> expected = structureCoordinates(reference);

    AnalysisStructures analysis;
    analysis.wi = wi;
    analysis.seeds = {wi.seed};
    analysis.dim = DIM_OVERWORLD;
    analysis.area = {-4096, -4096, 4095, 4095};
    std::fill(std::begin(analysis.mapshow), std::end(analysis.mapshow), false);
    analysis.mapshow[D_GEODE] = true;
    analysis.collect = true;
    analysis.quad = false;
    analysis.parallelInner = true;
    analysis.quadHuts = analysis.quadMonuments = false;
    analysis.hutQuality = CST_BARELY;
    analysis.monumentCoverage = 90;
    uint64_t reportedCount = 0;
    std::vector<std::pair<int,int>> delivered;
    QObject::connect(&analysis, &AnalysisStructures::itemDone, &analysis,
        [&](QTreeWidgetItem *seedItem) {
            for (int groupIndex = 0; groupIndex < seedItem->childCount(); groupIndex++)
            {
                QTreeWidgetItem *group = seedItem->child(groupIndex);
                if (group->text(1) != mapopt2display(D_GEODE))
                    continue;
                reportedCount += group->data(2, Qt::DisplayRole).toULongLong();
                for (int index = 0; index < group->childCount(); index++)
                {
                    const QTreeWidgetItem *item = group->child(index);
                    delivered.emplace_back(
                        item->data(3, Qt::DisplayRole).toInt(),
                        item->data(4, Qt::DisplayRole).toInt());
                }
            }
            delete seedItem;
        }, Qt::DirectConnection);

    QElapsedTimer elapsed;
    elapsed.start();
    analysis.start();
    const bool joined = analysis.wait(120000);
    const qint64 elapsedMs = elapsed.elapsed();
    if (!joined)
    {
        analysis.stop.store(true, std::memory_order_relaxed);
        analysis.wait(2000);
    }
    std::sort(delivered.begin(), delivered.end());
    const bool noDuplicates = std::adjacent_find(delivered.begin(), delivered.end()) ==
        delivered.end();
    const bool ok = joined && referenceCount > 4096 &&
        referenceCount == reference.size() && reportedCount == referenceCount &&
        analysis.resultCount.load(std::memory_order_relaxed) == referenceCount &&
        delivered == expected && noDuplicates &&
        analysis.workDone.load(std::memory_order_relaxed) ==
            analysis.workTotal.load(std::memory_order_relaxed) &&
        analysis.workers.peak.load(std::memory_order_relaxed) == mappingThreadCount();
    report << "structures-geode-completeness: " << (ok ? "PASS" : "FAIL")
        << ", reference=" << referenceCount
        << ", reported=" << reportedCount
        << ", delivered=" << delivered.size()
        << ", tasks=" << analysis.workDone.load(std::memory_order_relaxed)
        << '/' << analysis.workTotal.load(std::memory_order_relaxed)
        << ", threads=" << analysis.workers.peak.load(std::memory_order_relaxed)
        << ", reference_ms=" << referenceMs
        << ", analysis_ms=" << elapsedMs << '\n';
    report.flush();
    return ok;
}

bool testStructureWorldWidthSeams(QTextStream& report)
{
    WorldInfo wi;
    wi.mc = MC_NEWEST;
    wi.seed = 6384876098956146605ULL;
    StructureConfig sconf;
    if (!getStructureConfig_override(Desert_Pyramid, wi.mc, &sconf))
    {
        report << "structures-world-width-seams: FAIL, unsupported structure\n";
        return false;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    std::vector<VarPos> whole;
    const uint64_t wholeCount = getStructs(&whole, sconf, wi, sconf.dim,
        -30000000, -1024, 30000001, 1025, true);
    std::vector<std::pair<int,int>> expected = structureCoordinates(whole);
    std::vector<VarPos>().swap(whole);

    const std::array<int,8> boundaries = {
        -30000000, -20000003, -7777777, -1,
        1234567, 9876543, 21000001, 30000001
    };
    uint64_t partitionCount = 0;
    std::vector<std::pair<int,int>> actual;
    actual.reserve(expected.size());
    for (size_t part = 0; part+1 < boundaries.size(); part++)
    {
        std::vector<VarPos> positions;
        partitionCount += getStructs(&positions, sconf, wi, sconf.dim,
            boundaries[part], -1024, boundaries[part+1], 1025, true);
        for (const VarPos& position : positions)
            actual.emplace_back(position.p.x, position.p.z);
    }
    std::sort(actual.begin(), actual.end());
    const bool noDuplicates = std::adjacent_find(actual.begin(), actual.end()) == actual.end();
    uint64_t hash = 14695981039346656037ULL;
    for (const auto& coordinate : actual)
    {
        fnvInt32(hash, coordinate.first);
        fnvInt32(hash, coordinate.second);
    }
    const bool ok = wholeCount > 4096 && wholeCount == expected.size() &&
        partitionCount == actual.size() && actual == expected && noDuplicates;
    report << "structures-world-width-seams: " << (ok ? "PASS" : "FAIL")
        << ", whole=" << wholeCount
        << ", partitioned=" << partitionCount
        << ", fnv=0x" << QString("%1").arg(hash, 16, 16, QLatin1Char('0'))
        << ", elapsed_ms=" << elapsed.elapsed() << '\n';
    report.flush();
    return ok;
}

bool testBiomeBeyondOldLimit(QTextStream& report)
{
    AnalysisBiomes analysis;
    analysis.wi.reset();
    analysis.wi.y = 256;
    analysis.seeds = {6384876098956146605ULL};
    analysis.dims[0] = analysis.dims[1] = analysis.dims[2] = DIM_UNDEF;
    analysis.dat = {-7500000, -7500000, 7500000, 7500000, 4, birch_forest,
        DIM_OVERWORLD, {birch_forest}, ~0ULL};
    analysis.minsize = 16;
    analysis.tolerance = 0;

    analysis.start();
    QElapsedTimer elapsed;
    elapsed.start();
    while (analysis.isRunning() &&
            analysis.resultCount.load(std::memory_order_relaxed) <= 4096 &&
            elapsed.elapsed() < 30000)
        QThread::msleep(10);

    const uint64_t count = analysis.resultCount.load(std::memory_order_relaxed);
    const uint64_t done = analysis.workDone.load(std::memory_order_relaxed);
    const uint64_t total = analysis.workTotal.load(std::memory_order_relaxed);
    const bool keptRunning = analysis.isRunning();
    analysis.stop.store(true, std::memory_order_relaxed);
    const bool joined = analysis.wait(2000);
    if (!joined)
    {
        analysis.terminate();
        analysis.wait();
    }
    const bool ok = count > 4096 && keptRunning && joined && done < total &&
        analysis.workers.peak.load(std::memory_order_relaxed) == mappingThreadCount();
    report << "biome-beyond-4096: " << (ok ? "PASS" : "FAIL")
        << ", results=" << count
        << ", tiles=" << done << '/' << total
        << ", elapsed_ms=" << elapsed.elapsed() << '\n';
    report.flush();
    return ok;
}

int runMappingSelfTest(const QString& reportPath)
{
    QFile file(reportPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return 2;
    QTextStream report(&file);
    bool ok = true;

    Config defaults;
    const int ideal = std::max(1, QThread::idealThreadCount());
    const bool cpuLimitOk = defaults.cpuUsagePercent == 80 &&
        defaults.mapThreads == threadCountForCpuUsage(80) &&
        threadCountForCpuUsage(100) == ideal && threadCountForCpuUsage(1) >= 1;
    report << "cpu-usage-limit: " << (cpuLimitOk ? "PASS" : "FAIL")
        << ", logical=" << ideal << ", default_percent=" << defaults.cpuUsagePercent
        << ", default_threads=" << defaults.mapThreads << '\n';
    ok &= cpuLimitOk;
    ok &= testResultTreeSorting(report);

    ok &= testFortressLayoutDetector(report);
    ok &= testFortressAnalysis(report);
    {
        AnalysisStructures analysis;
        analysis.wi.reset();
        analysis.wi.mc = MC_NEWEST;
        analysis.seeds = {6384876098956146605ULL};
        analysis.dim = DIM_NETHER;
        analysis.area = {-30000000, -30000000, 30000000, 30000000};
        std::fill(std::begin(analysis.mapshow), std::end(analysis.mapshow), false);
        analysis.parallelInner = true;
        analysis.fortress = true;
        analysis.fortress2x2 = true;
        analysis.fortress3x1 = true;
        ok &= testMappingStop("fortress-analysis-stop", analysis, report);
    }

    {
        AnalysisBiomes analysis;
        analysis.wi.reset();
        analysis.wi.y = 256;
        analysis.seeds = {6384876098956146605ULL};
        analysis.dims[0] = analysis.dims[1] = analysis.dims[2] = DIM_UNDEF;
        analysis.dat = {-7500000, -7500000, 7500000, 7500000, 4, birch_forest,
            DIM_OVERWORLD, {birch_forest}, ~0ULL};
        analysis.minsize = 16;
        analysis.tolerance = 0;
        ok &= testMappingStop("biome-locate", analysis, report);
    }
    ok &= testBiomeBeyondOldLimit(report);
    {
        AnalysisBiomes analysis;
        analysis.wi.reset();
        analysis.seeds = {0x123456789abcdef0ULL};
        analysis.dims[0] = DIM_OVERWORLD;
        analysis.dims[1] = analysis.dims[2] = DIM_UNDEF;
        analysis.dat = {-200000, -200000, 200000, 200000, 4, -1,
            DIM_UNDEF, {}, ~0ULL};
        analysis.minsize = 1;
        analysis.tolerance = 0;
        ok &= testMappingStop("biome-statistics", analysis, report);
    }
    {
        AnalysisStructures analysis;
        analysis.wi.reset();
        analysis.seeds = {6384876098956146605ULL};
        analysis.dim = DIM_OVERWORLD;
        analysis.area = {-30000000, -30000000, 30000000, 30000000};
        std::fill(std::begin(analysis.mapshow), std::end(analysis.mapshow), false);
        analysis.mapshow[D_DESERT] = true;
        analysis.collect = false;
        analysis.quad = false;
        analysis.parallelInner = true;
        analysis.quadHuts = analysis.quadMonuments = false;
        analysis.hutQuality = CST_BARELY;
        analysis.monumentCoverage = 90;
        ok &= testMappingStop("structures", analysis, report);
    }
    ok &= testStructureCompleteness(report);
    ok &= testStructureWorldWidthSeams(report);
    ok &= testQuadWorld("quad-huts-current-seed", report, Swamp_Hut,
        6384876098956146605ULL, 30,
        "00bbac83c6af870bc54ac52c73dcd710b30daf1ccd45d3f7a0450591ea95ac18");
    ok &= testQuadWorld("quad-monuments-current-seed", report, Monument,
        6384876098956146605ULL, 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ok &= testQuadWorld("quad-huts-positive-world", report, Swamp_Hut,
        3740936736371ULL, 39,
        "99b2839f2dfa1abd1e71b05ddf84ec30c02925f4eb960bd5129178006a7001c9",
        -1, -1);
    ok &= testQuadWorld("quad-monuments-positive-world", report, Monument,
        3981767442920597ULL, 1,
        "de7c7dc9e4ead399e0c9ac10f94aada15828cbbfc6ca318804e05a464e361d43",
        -1, -1);
    ok &= testQuadAnalysis("quad-huts-analysis", report, Swamp_Hut,
        3740936736371ULL, 39);
    ok &= testQuadAnalysis("quad-monuments-analysis", report, Monument,
        3981767442920597ULL, 1);
    ok &= testDensityAnalysis(report);
    {
        AnalysisStructures analysis;
        analysis.wi.reset();
        const size_t count = 4096;
        for (size_t i = 0; i < count; i++)
            analysis.seeds.push_back(0x123456789abcdef0ULL + i);
        analysis.dim = DIM_OVERWORLD;
        analysis.area = {-30000000, -30000000, 30000000, 30000000};
        std::fill(std::begin(analysis.mapshow), std::end(analysis.mapshow), false);
        analysis.collect = false;
        analysis.quad = true;
        analysis.parallelInner = true;
        analysis.quadHuts = analysis.quadMonuments = true;
        analysis.hutQuality = CST_BARELY;
        analysis.monumentCoverage = 90;
        ok &= testMappingStop("quad-structures", analysis, report);
    }
    {
        AnalysisSlime analysis;
        analysis.wi.reset();
        analysis.wi.seed = 6384876098956146605ULL;
        analysis.x1 = analysis.z1 = -1875000;
        analysis.x2 = analysis.z2 = 1874999;
        analysis.window = 16;
        analysis.minimum = 200;
        ok &= testMappingStop("slime-chunks", analysis, report);
    }
    ok &= testSlimeCompleteness("slime-all-chunks", report,
        0, 0, 511, 511, 1, 1,
        26189, 6704560, 6699269, 26189, 0x6C74F48313BF6FA5ULL);
    ok &= testSlimeCompleteness("slime-large-exact", report,
        -1024, -1024, 1023, 1023, 16, 35,
        157359, -1117663, -2516671, 5769821, 0x94BFCAF646C14D34ULL);
    {
        AnalysisLocations analysis;
        WorldInfo wi;
        const QString error = analysis.set(wi, {});
        analysis.seeds = {0x123456789abcdef0ULL};
        analysis.pos.resize(2000000);
        if (!error.isEmpty())
        {
            report << "locations: FAIL, setup=" << error << '\n';
            ok = false;
        }
        else
        {
            ok &= testMappingStop("locations", analysis, report, 100);
        }
    }

    report << "result: " << (ok ? "PASS" : "FAIL") << '\n';
    report.flush();
    return ok ? 0 : 1;
}

} // namespace

// UI test: the condition editor turns the same inputs into the same
// conditions. For each condition of the input file it writes the condition
// that the editor accepts unchanged ("EDIT"), and for every condition type
// and area shape the condition of a new editor with fixed inputs ("NEW").
// Warnings of the editor are answered with Cancel and reported ("WARN").
static void selectDialogType(ConditionDialog *dialog, int type)
{
    dialog->applyTestPreset(QString::number(type));
}

static bool testConditionEditor(MainWindow& mw, const QString& inpath, const QString& outpath)
{
    QFile in(inpath), out(outpath);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly))
        return false;
    QTextStream report(&out);
    WorldInfo wi;
    mw.getSeed(&wi);
    QTimer answer;
    bool warned = false;
    QObject::connect(&answer, &QTimer::timeout, [&warned]() {
        if (QMessageBox *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            warned = true;
            box->reject();
        }
    });
    answer.start(20);
    const auto accept = [&](ConditionDialog *dialog, const QString& label) {
        Condition result;
        bool emitted = false;
        QObject::connect(dialog, &ConditionDialog::setCond, [&](QListWidgetItem *, Condition c, int) {
            result = c;
            emitted = true;
        });
        warned = false;
        dialog->onAccept();
        report << label << " " << (emitted ? result.toHex() : QString("-"))
               << (warned ? " WARN" : "") << "\n";
        delete dialog;
    };
    QTextStream stream(&in);
    while (!stream.atEnd())
    {
        const QString line = stream.readLine();
        Condition c;
        if (!line.startsWith("#Cond:") || !c.readHex(line.mid(6).trimmed()))
            continue;
        ConditionDialog *dialog = new ConditionDialog(mw.formCond, mw.getMapView(), nullptr, wi,
            new QListWidgetItem(), &c);
        accept(dialog, "EDIT " + c.toHex());
    }
    const char *shapes[] = { "radioRadius", "radioSquare", "radioCustom" };
    for (int type = 1; type < FILTER_MAX; type++)
    {
        if (!g_filterinfo.list[type].name || !g_filterinfo.list[type].name[0])
            continue; // legacy ids
        for (const char *shape : shapes)
        {
            ConditionDialog *dialog = new ConditionDialog(mw.formCond, mw.getMapView(), nullptr, wi);
            selectDialogType(dialog, type);
            QRadioButton *radio = dialog->findChild<QRadioButton *>(shape);
            if (radio && radio->isEnabled())
                radio->setChecked(true);
            const std::pair<const char *, const char *> fields[] = {
                {"lineRadius", "300"}, {"lineSquare", "641"}, {"lineEditX1", "-100"},
                {"lineEditZ1", "-50"}, {"lineEditX2", "200"}, {"lineEditZ2", "150"},
            };
            for (const auto& f : fields)
                if (QLineEdit *e = dialog->findChild<QLineEdit *>(f.first))
                    e->setText(f.second);
            if (QSpinBox *count = dialog->findChild<QSpinBox *>("spinBox"))
                count->setValue(2);
            accept(dialog, QString("NEW %1 %2").arg(type).arg(shape));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SEED_ATLAS_UI_TEST_DATAPACK: datapack worlds in the window, checked against
// the datapack generator itself. It needs data that is not part of the
// repository or of a build:
//   SEED_ATLAS_TEST_VANILLA       the vanilla data of Minecraft 1.21.1 (a jar,
//                                 zip or folder)
//   SEED_ATLAS_TEST_DATAPACK_DIR  a folder with the packs "testpack" (the
//                                 project's test pack) and optionally
//                                 "collide" (two biomes with one name),
//                                 "nodim" (a pack without a dimension file)
//                                 and "broken" (a pack that cannot be loaded);
//                                 see .github/workflows/app-tests.sh
// With the value "restore" it only checks that a second start of the app
// brings the packs of the previous run back.
// SEED_ATLAS_TEST_DATAPACK_REPORT names a file for timings and details.

namespace {

struct DatapackTest
{
    MainWindow& mw;
    MapView *map;
    DatapackManager *packs;
    QStringList log;
    bool ok = true;

    explicit DatapackTest(MainWindow& mw)
        : mw(mw), map(mw.getMapView()), packs(mw.datapacks) {}

    bool check(bool condition, const QString& what)
    {
        if (!condition)
        {
            ok = false;
            log << "FAIL: " + what;
        }
        return condition;
    }

    static void pump(int ms = 0)
    {
        QElapsedTimer wait;
        wait.start();
        do
        {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
            if (ms > 0)
                QThread::msleep(5);
        }
        while (wait.elapsed() < ms);
    }

    // until the packs are loaded and the window has taken the world over
    void waitLoaded()
    {
        QElapsedTimer limit;
        limit.start();
        do
        {
            packs->waitForLoaded();
            pump(30);
        }
        while (limit.elapsed() < 120000 && (packs->loading() ||
            (packs->current() && packs->current()->state == DatapackWorld::Loading)));
        pump(30);
    }

    // until every tile of the view is done; returns false after a minute
    bool waitMap()
    {
        QElapsedTimer limit;
        limit.start();
        int calm = 0;
        while (limit.elapsed() < 60000)
        {
            map->grab(); // paints, which schedules the tiles of the view
            QWorld *world = map->world;
            if (!world)
                return false;
            world->waitForIdle();
            pump();
            map->grab();
            bool complete = !world->isBusy() && world->activelv >= 0
                && world->activelv < (int) world->lvb.size()
                && !world->lvb[world->activelv].cells.empty();
            if (complete)
                for (Quad *quad : world->lvb[world->activelv].cells)
                    complete = complete && quad->img && quad->done;
            calm = complete ? calm + 1 : 0;
            if (calm >= 2)
                return true;
        }
        return false;
    }

    // until every structure tile of a datapack world in view is done (the
    // map has to show structures); false after two minutes
    bool waitStructures()
    {
        QElapsedTimer limit;
        limit.start();
        int calm = 0;
        while (limit.elapsed() < 120000)
        {
            map->grab();
            QWorld *world = map->world;
            if (!world)
                return false;
            world->waitForIdle();
            pump();
            map->grab();
            bool complete = !world->isBusy() && !world->lvdp.cells.empty();
            if (complete)
                for (Quad *quad : world->lvdp.cells)
                    complete = complete && quad->done && quad->dpstarts;
            calm = complete ? calm + 1 : 0;
            if (calm >= 2)
                return true;
        }
        return false;
    }

    // the finished tiles of the shown level, by position
    std::map<std::tuple<int,int,int>, QImage> tiles()
    {
        std::map<std::tuple<int,int,int>, QImage> out;
        QWorld *world = map->world;
        if (!world || world->activelv < 0 || world->activelv >= (int) world->lvb.size())
            return out;
        for (Quad *quad : world->lvb[world->activelv].cells)
            if (QImage *image = quad->img)
                out[std::make_tuple(quad->scale, quad->ti, quad->tj)] = image->copy();
        return out;
    }

    void setWorld(int mc, uint64_t seed)
    {
        WorldInfo wi;
        mw.getSeed(&wi, false);
        wi.mc = mc;
        wi.seed = seed;
        wi.setWorldType(WORLD_DEFAULT);
        mw.setSeed(wi, DIM_OVERWORLD);
    }

    void setLevel(int y)
    {
        WorldInfo wi;
        mw.getSeed(&wi, false);
        wi.y = y;
        mw.setSeed(wi);
    }

    // Compares the tiles of the shown level with a sampler of the oracle
    // world: every pixel of every tile against genBiomes(), and a part of
    // them also against biomeAt(), in the colours of the oracle's biomes
    // (with the highlight of @marked, if given).
    bool checkTiles(const std::shared_ptr<const dp::World>& oracle, int dim, uint64_t seed,
        int y, const QString& what, const QSet<int> *marked = nullptr, int *checked = nullptr)
    {
        QWorld *world = map->world;
        if (!check(world && world->wi.dp, what + ": no datapack world on the map"))
            return false;
        dp::Sampler sampler(oracle, dim, seed);
        const std::vector<dp::BiomeInfo>& biomes = oracle->biomes();
        qint64 pixels = 0, wrong = 0, wrongAt = 0;
        int tilesChecked = 0;
        for (Quad *quad : world->lvb[world->activelv].cells)
        {
            QImage *image = quad->img;
            if (!image || image->isNull())
                continue;
            const int w = quad->pixs, h = quad->pixs;
            const int x = quad->ti * quad->pixs, z = quad->tj * quad->pixs;
            if (image->width() != w || image->height() != h)
            {
                wrong++;
                continue;
            }
            std::vector<int> ids(size_t(w) * size_t(h), -2);
            if (!sampler.genBiomes(ids.data(), quad->scale, x, z, w, h, y))
            {
                wrong++;
                continue;
            }
            for (int j = 0; j < h; j++)
            {
                for (int i = 0; i < w; i++)
                {
                    const int id = ids[size_t(j) * w + i];
                    QRgb want = id >= 0 && id < (int) biomes.size() ? biomes[id].color
                        : qRgb(16, 18, 17);
                    if (marked)
                    {
                        if (marked->contains(id))
                            want = qRgb(std::min(255, 24 + qRed(want)),
                                std::min(255, 24 + qGreen(want)), std::min(255, 24 + qBlue(want)));
                        else
                        {
                            int gray = (54*qRed(want) + 183*qGreen(want) + 19*qBlue(want)) >> 8;
                            gray = 18 + gray / 5;
                            want = qRgb(gray, gray, gray);
                        }
                    }
                    pixels++;
                    wrong += (image->pixel(i, j) & 0xffffff) != (want & 0xffffff);
                    // single lookups have to agree with the area
                    if ((i * 7 + j * 13) % 97 == 0)
                        wrongAt += sampler.biomeAt(quad->scale, x + i, y, z + j) != id;
                }
            }
            tilesChecked++;
        }
        if (checked)
            *checked = tilesChecked;
        log << QString("%1: %2 tiles, %3 pixels, %4 wrong, %5 single lookups differ")
            .arg(what).arg(tilesChecked).arg(pixels).arg(wrong).arg(wrongAt);
        return check(tilesChecked > 0 && wrong == 0 && wrongAt == 0, what + ": tiles differ");
    }
};

bool copyFolder(const QString& from, const QString& to)
{
    QDir().mkpath(to);
    QDirIterator it(from, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        const QString target = to + "/" + QDir(from).relativeFilePath(path);
        if (it.fileInfo().isDir())
            QDir().mkpath(target);
        else
        {
            QDir().mkpath(QFileInfo(target).absolutePath());
            if (!QFile::copy(path, target))
                return false;
        }
    }
    return true;
}

double maxMemoryMb()
{
#ifdef Q_OS_UNIX
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
#ifdef Q_OS_MACOS
        return usage.ru_maxrss / 1048576.0; // bytes
#else
        return usage.ru_maxrss / 1024.0;    // kilobytes
#endif
    }
#endif
    return 0;
}

bool testDatapackRestore(MainWindow& mw)
{
    // A second start: the packs, their switches, the dimension, the level and
    // the highlight of the previous run are back, and the map shows the world.
    DatapackTest t(mw);
    const QString dir = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_DATAPACK_DIR"));
    const std::vector<PackEntry>& entries = t.packs->entries();
    t.check(entries.size() == 2, "restore: two packs in the list");
    if (entries.size() == 2)
    {
        t.check(QFileInfo(entries[0].path) == QFileInfo(dir + "/testpack") && entries[0].enabled,
            "restore: the test pack, switched on");
        t.check(entries[1].path.endsWith("/gone-pack") && !entries[1].enabled
            && !t.packs->packExists(1), "restore: the missing pack is kept, switched off");
    }
    t.check(t.packs->dimension() == "minecraft:overworld", "restore: the dimension");
    t.waitLoaded();
    t.check(mw.datapackMode() && t.packs->current() && t.packs->current()->usable(),
        "restore: the datapack world is loaded");
    // the packs are loaded once, for the stored version (not first for the
    // default of the version picker)
    t.check(t.packs->loadsStarted() == 1, QString("restore: %1 loads at the start, not 1")
        .arg(t.packs->loadsStarted()));
    t.check(t.waitMap(), "restore: the map is complete");
    QWorld *world = t.map->world;
    t.check(world && world->wi.dp == t.packs->current(), "restore: the map shows the datapack world");
    t.check(world && world->wi.y == 40, "restore: the level of the datapack world");
    t.check(t.map->getDatapackHighlights() == QStringList{"testpack:amber_plains"},
        "restore: the highlight");
    if (world && world->wi.dp && world->wi.dp->usable())
    {
        const std::shared_ptr<const DatapackWorld> shown = world->wi.dp;
        dp::Request request = shown->world->request();
        dp::Report report;
        const std::shared_ptr<const dp::World> oracle = dp::World::load(request, &report);
        const int dim = oracle->dimension("minecraft:overworld");
        QSet<int> marked;
        for (int i = 0; i < (int) oracle->biomes().size(); i++)
            if (oracle->biomes()[i].id == "testpack:amber_plains")
                marked.insert(i);
        t.check(report.ok() && dim >= 0 && marked.size() == 1, "restore: the oracle world");
        if (t.ok)
            t.checkTiles(oracle, dim, world->wi.seed, 40, "restore", &marked);
    }
    for (const QString& line : std::as_const(t.log))
        fprintf(stderr, "datapack test: %s\n", qPrintable(line));
    return t.ok;
}

// ---------------------------------------------------------------------------
// Search and analyses in a datapack world (part of SEED_ATLAS_UI_TEST_DATAPACK;
// with the value "search" only this part runs). The searches of the Seed
// Finder and the counts and areas of the biome analysis are checked against
// a brute force of their own: a single-threaded dp::Sampler of an own load
// of the world, one biomeAt() per cell, without early exits.

struct DpOracle
{
    std::shared_ptr<const dp::World> world;
    int dim = -1;
    int top = 0;
    // biomeAt() results, by seed, then by scale, y and cell
    std::map<uint64_t, std::unordered_map<uint64_t, int>> cache;
    std::map<uint64_t, std::unique_ptr<dp::Sampler>> samplers;

    int biome(uint64_t seed, int scale, int x, int y, int z)
    {
        const int code = scale == 1 ? 0 : scale == 4 ? 1 : scale == 16 ? 2 : 3;
        const uint64_t key = uint64_t(code) | uint64_t(y + 2048) << 2
            | uint64_t(x + (1 << 19)) << 14 | uint64_t(z + (1 << 19)) << 34;
        std::unordered_map<uint64_t, int>& bySeed = cache[seed];
        auto it = bySeed.find(key);
        if (it != bySeed.end())
            return it->second;
        std::unique_ptr<dp::Sampler>& s = samplers[seed];
        if (!s)
            s.reset(new dp::Sampler(world, dim, seed));
        const int id = s->biomeAt(scale, x, y, z);
        bySeed[key] = id;
        return id;
    }
    int index(const QString& id) const
    {
        for (int i = 0; i < (int) world->biomes().size(); i++)
            if (world->biomes()[i].id == id)
                return i;
        return -1;
    }
    static int floorDiv(int a, int b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

    // the cells of an area condition, as the definition says: every cell of
    // the bounds at the scale, for a circle those whose centre is inside
    std::vector<std::pair<int,int>> cells(const DpCondition& c) const
    {
        int x1, z1, x2, z2;
        if (c.shape == DpCondition::RECT)
        {
            x1 = std::min(c.x1, c.x2); x2 = std::max(c.x1, c.x2);
            z1 = std::min(c.z1, c.z2); z2 = std::max(c.z1, c.z2);
        }
        else
        {
            x1 = c.x - c.r; x2 = c.x + c.r; z1 = c.z - c.r; z2 = c.z + c.r;
        }
        std::vector<std::pair<int,int>> v;
        const int s = c.scale;
        for (int j = floorDiv(z1, s); j <= floorDiv(z2, s); j++)
            for (int i = floorDiv(x1, s); i <= floorDiv(x2, s); i++)
            {
                if (c.shape == DpCondition::CIRCLE)
                {
                    const double dx = (i + 0.5) * s - (c.x + 0.5), dz = (j + 0.5) * s - (c.z + 0.5);
                    if (dx * dx + dz * dz > double(c.r) * c.r)
                        continue;
                }
                v.push_back({i, j});
            }
        return v;
    }

    bool test(uint64_t seed, const DpCondition& c)
    {
        const int y = c.y == DpCondition::Y_TOP ? top : c.y;
        std::set<int> listed;
        for (const QString& id : c.biomes)
            if (index(id) >= 0)
                listed.insert(index(id));
        if (c.kind == DpCondition::BIOME_AT)
        {
            const bool in = listed.count(biome(seed, 1, c.x, y, c.z)) != 0;
            return c.match == DpCondition::NONE ? !in : in;
        }
        std::map<int, int64_t> count;
        int64_t n = 0, together = 0;
        for (const auto& cell : cells(c))
        {
            n++;
            const int id = biome(seed, c.scale, cell.first, y, cell.second);
            if (listed.count(id))
            {
                count[id]++;
                together++;
            }
        }
        if (c.kind == DpCondition::BIOME_IN_AREA)
        {
            if (c.match == DpCondition::ANY)
                return together > 0;
            if (c.match == DpCondition::NONE)
                return together == 0;
            return count.size() == listed.size();
        }
        if (c.match == DpCondition::ANY)
            return together * 100 >= int64_t(c.percent) * n;
        if (c.match == DpCondition::NONE)
            return together * 100 <= int64_t(c.percent) * n;
        for (int b : listed)
            if (count[b] * 100 < int64_t(c.percent) * n)
                return false;
        return true;
    }
    // the percentage of the cells of an area with the listed biomes
    double share(uint64_t seed, const DpCondition& c)
    {
        const int y = c.y == DpCondition::Y_TOP ? top : c.y;
        int64_t n = 0, together = 0;
        for (const auto& cell : cells(c))
        {
            n++;
            const int id = biome(seed, c.scale, cell.first, y, cell.second);
            together += id >= 0 && c.biomes.contains(world->biomes()[id].id);
        }
        return n ? 100.0 * together / n : 0;
    }
};

// answers the message boxes of the app and keeps their texts
struct MessageCatcher
{
    QTimer timer;
    QStringList texts;
    MessageCatcher()
    {
        QObject::connect(&timer, &QTimer::timeout, [this]() {
            if (QMessageBox *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            {
                texts << box->text() + " " + box->informativeText();
                box->reject();
            }
        });
        timer.start(20);
    }
};

bool testDatapackSearch(DatapackTest& t, const std::shared_ptr<const dp::World>& oracleWorld,
    int dim, QStringList& report)
{
    MainWindow& mw = t.mw;
    FormSearchControl *control = mw.formControl;
    FormDpConditions *conditions = mw.formDpCond;
    QTabWidget *tabs = mw.findChild<QTabWidget*>("tabContainer");
    QPushButton *start = control->findChild<QPushButton*>("buttonStart");
    MessageCatcher messages;
    const std::shared_ptr<const DatapackWorld> shown = t.packs->current();
    if (!t.check(shown && shown->usable() && start && tabs && conditions, "search: a datapack world"))
        return false;

    DpOracle oracle;
    oracle.world = oracleWorld;
    oracle.dim = dim;
    const dp::DimensionInfo& dimension = oracleWorld->dimensions()[dim];
    oracle.top = dimension.minY + dimension.height - 1;
    t.setLevel(oracle.top);

    // --- what the Seed Finder and the analyses offer in a datapack world ---
    QWidget *searchPage = mw.findChild<QWidget*>("tabSearch");
    TabBiomes *biomesTab = mw.findChild<TabBiomes*>();
    tabs->setCurrentWidget(searchPage);
    t.pump(30);
    t.check(searchPage->isEnabled() && conditions->isVisibleTo(&mw) && !mw.formCond->isVisibleTo(&mw),
        "search: the Seed Finder shows the datapack conditions");
    t.check(!mw.datapackNotice->isVisibleTo(&mw), "search: no vanilla-only notice above the Seed Finder");
    t.check(mw.tabLocations && !mw.tabLocations->isEnabled(), "search: Locations stays off");
    QWidget *gen48 = mw.findChild<QWidget*>("collapseGen48");
    t.check(gen48 && !gen48->isVisibleTo(&mw), "search: no seed generator");
    t.check(biomesTab && biomesTab->isEnabled(), "search: the biome analysis is on");

    const uint64_t first = 7000;
    const int count = 40;
    std::vector<uint64_t> seeds;
    for (int i = 0; i < count; i++)
        seeds.push_back(first + i);

    // the biomes of the test: chosen from what the oracle sees, so that the
    // searches match some seeds of the range, but not all of them
    DpCondition probe;
    probe.kind = DpCondition::BIOME_IN_AREA;
    probe.shape = DpCondition::CIRCLE;
    probe.r = 256;
    std::map<int, int> present; // biome -> seeds that have it in the circle
    for (uint64_t s : seeds)
    {
        std::set<int> here;
        for (const auto& cell : oracle.cells(probe))
            here.insert(oracle.biome(s, 4, cell.first, oracle.top, cell.second));
        for (int b : here)
            if (b >= 0)
                present[b]++;
    }
    const auto pick = [&](int avoid) {
        int best = -1;
        double bestScore = 1e9;
        for (const auto& p : present)
        {
            if (p.first == avoid || p.second <= 0 || p.second >= count)
                continue;
            const bool pack = oracleWorld->biomes()[p.first].id.startsWith("testpack:");
            const double score = std::abs(p.second - count / 2.0) - (pack ? count : 0);
            if (score < bestScore)
            {
                bestScore = score;
                best = p.first;
            }
        }
        return best;
    };
    const int T = pick(-1), U = pick(T);
    if (!t.check(T >= 0 && U >= 0, "search: two biomes that occur in some seeds of the range"))
        return false;
    const QString idT = oracleWorld->biomes()[T].id, idU = oracleWorld->biomes()[U].id;
    report << QString("search biomes: %1 (%2 of %3 seeds), %4 (%5 of %3 seeds)")
        .arg(idT).arg(present[T]).arg(count).arg(idU).arg(present[U]);

    const auto runSearch = [&](const std::vector<DpCondition>& cv, int type,
            const std::vector<uint64_t>& candidates, const QString& what, qint64 *ms = nullptr) {
        conditions->setConditions(cv);
        control->invalidateResults();
        SearchConfig sc = control->getSearchConfig();
        sc.searchtype = type;
        sc.stoponres = false;
        sc.startseed = candidates.front();
        sc.smin = candidates.front();
        sc.smax = candidates.back();
        sc.slist64path.clear();
        control->setSearchConfig(sc, true);
        control->setSearchRange(sc.smin, sc.smax);
        if (type == SEARCH_LIST)
        {
            QString text;
            for (uint64_t s : candidates)
                text += QString::number(qint64(s)) + "\n";
            QTextStream stream(&text);
            control->setList64(stream);
        }
        QElapsedTimer timer;
        timer.start();
        start->click();
        QElapsedTimer limit;
        limit.start();
        while (control->isSearching() && limit.elapsed() < 300000)
            t.pump(20);
        if (ms)
            *ms = timer.elapsed();
        t.check(!control->isSearching(), what + ": the search ends");
        const std::vector<uint64_t> got = control->getResults();
        return std::set<uint64_t>(got.begin(), got.end());
    };
    const auto compare = [&](const std::vector<DpCondition>& cv, int type,
            const std::vector<uint64_t>& candidates, const QString& what, bool someNotAll) {
        const std::set<uint64_t> got = runSearch(cv, type, candidates, what);
        std::set<uint64_t> want;
        for (uint64_t s : candidates)
        {
            bool ok = true;
            for (const DpCondition& c : cv)
                ok = ok && (!c.enabled || oracle.test(s, c));
            if (ok)
                want.insert(s);
        }
        t.log << QString("%1: %2 of %3 seeds match, the search found %4")
            .arg(what).arg(want.size()).arg(candidates.size()).arg(got.size());
        t.check(got == want, what + ": the results are those of the oracle");
        if (someNotAll)
            t.check(!want.empty() && want.size() < candidates.size(),
                what + ": some seeds match, not all");
        return got;
    };

    DpCondition c1;
    c1.id = 1;
    c1.kind = DpCondition::BIOME_IN_AREA;
    c1.biomes = {idT};
    c1.shape = DpCondition::CIRCLE;
    c1.r = 256;
    const std::set<uint64_t> hits1 = compare({c1}, SEARCH_INC, seeds, "search 1 (biome in a circle)", true);

    DpCondition c2 = c1;
    c2.id = 2;
    c2.match = DpCondition::ALL;
    c2.biomes = {idT, idU};
    c2.shape = DpCondition::SQUARE;
    c2.x = 30;
    c2.z = -20;
    c2.r = 300;
    DpCondition c3;
    c3.id = 3;
    c3.kind = DpCondition::BIOME_AT;
    c3.match = DpCondition::NONE;
    c3.x = 40;
    c3.z = -24;
    c3.y = 40;
    c3.biomes = {oracleWorld->biomes()[std::max(0, oracle.biome(first, 1, 40, 40, -24))].id};
    compare({c2, c3}, SEARCH_INC, seeds, "search 2 (all of two biomes in a square, not a biome at a block)", false);

    // a share near the middle of what the seeds have
    DpCondition c4 = c1;
    c4.id = 4;
    c4.kind = DpCondition::BIOME_SHARE;
    c4.biomes = {idT, idU};
    std::vector<double> shares;
    for (uint64_t s : seeds)
        shares.push_back(oracle.share(s, c4));
    std::sort(shares.begin(), shares.end());
    c4.percent = std::max(1, int(shares[count / 2]));
    compare({c4}, SEARCH_INC, seeds, QString("search 3 (together at least %1%)").arg(c4.percent), true);

    DpCondition c5 = c4;
    c5.id = 5;
    c5.match = DpCondition::NONE;
    c5.scale = 16;
    c5.shape = DpCondition::RECT;
    c5.x1 = -300;
    c5.z1 = -170;
    c5.x2 = 420;
    c5.z2 = 260;
    DpCondition c6 = c1;
    c6.id = 6;
    c6.match = DpCondition::NONE;
    c6.biomes = {idU};
    c6.scale = 64;
    c6.r = 700;
    c6.enabled = false; // switched off: ignored
    compare({c5, c6}, SEARCH_INC, seeds, "search 4 (at most a share, 1:16, a condition off)", false);

    // each of two biomes, below the surface: the share of the rarer one
    // decides; the median of that share splits the seeds
    DpCondition c7 = c4;
    c7.id = 7;
    c7.match = DpCondition::ALL;
    c7.y = 0;
    {   // the two biomes that are most common at that level
        std::map<int, int64_t> total;
        for (uint64_t s : seeds)
            for (const auto& cell : oracle.cells(c7))
                total[oracle.biome(s, c7.scale, cell.first, c7.y, cell.second)]++;
        total.erase(-1);
        std::vector<std::pair<int64_t, int>> byCount;
        for (const auto& b : total)
            byCount.push_back({b.second, b.first});
        std::sort(byCount.rbegin(), byCount.rend());
        if (t.check(byCount.size() >= 2, "search 5: two biomes at Y 0"))
            c7.biomes = {oracleWorld->biomes()[byCount[0].second].id,
                oracleWorld->biomes()[byCount[1].second].id};
    }
    {
        std::vector<double> least;
        for (uint64_t s : seeds)
        {
            DpCondition one = c7;
            one.biomes = {c7.biomes.value(0)};
            const double a = oracle.share(s, one);
            one.biomes = {c7.biomes.value(1)};
            least.push_back(std::min(a, oracle.share(s, one)));
        }
        std::sort(least.begin(), least.end());
        c7.percent = std::max(1, int(least[count / 2]));
    }
    compare({c7}, SEARCH_INC, seeds, QString("search 5 (each of %1 at least %2% at Y 0)")
        .arg(c7.biomes.join(", ")).arg(c7.percent), true);

    // seeds from a list: every third seed and two from far away
    std::vector<uint64_t> list;
    for (int i = 0; i < count; i += 3)
        list.push_back(seeds[i]);
    list.push_back(uint64_t(-123456789));
    list.push_back(9000000000ULL);
    compare({c1}, SEARCH_LIST, list, "search 6 (seed list)", false);

    QLineEdit *resumeAt = control->findChild<QLineEdit*>("lineStart");
    // --- a search stopped half way and resumed: together the results of
    // the oracle ---
    {
        DpCondition c8 = c1;
        c8.id = 8;
        c8.r = 200;
        std::vector<uint64_t> range;
        // long enough that a fast machine is still searching when it stops
        for (int i = 0; i < 1000; i++)
            range.push_back(first + i);
        conditions->setConditions({c8});
        control->invalidateResults();
        SearchConfig sc = control->getSearchConfig();
        sc.searchtype = SEARCH_INC;
        sc.stoponres = false;
        sc.startseed = sc.smin = range.front();
        sc.smax = range.back();
        control->setSearchConfig(sc, true);
        control->setSearchRange(sc.smin, sc.smax);
        start->click();
        QElapsedTimer limit;
        limit.start();
        // a few results and one progress update (every 250 ms) before the stop
        while (control->isSearching() && (control->getResults().size() < 2 || limit.elapsed() < 400)
                && limit.elapsed() < 60000)
            t.pump(2);
        const bool wasRunning = control->isSearching();
        start->click(); // abort
        while (control->isSearching() && limit.elapsed() < 120000)
            t.pump(20);
        const uint64_t stoppedAt = resumeAt ? resumeAt->text().toULongLong() : 0;
        const size_t partial = control->getResults().size();
        // (the position to resume from may lag behind what was tested; the
        // seeds after it are tested again, their results are not doubled)
        t.check(wasRunning && partial > 0 && stoppedAt >= range.front() && stoppedAt < range.back(),
            QString("stop: the search stopped half way (to resume at %1)").arg(stoppedAt));
        start->click(); // resume from there, the results stay
        while (control->isSearching() && limit.elapsed() < 300000)
            t.pump(20);
        const std::vector<uint64_t> all = control->getResults();
        const std::set<uint64_t> got(all.begin(), all.end());
        std::set<uint64_t> want;
        for (uint64_t s : range)
            if (oracle.test(s, c8))
                want.insert(s);
        t.log << QString("stop and resume: %1 found before the stop, %2 in all, the oracle has %3")
            .arg(partial).arg(got.size()).arg(want.size());
        t.check(got == want && all.size() == got.size(),
            "stop and resume: the results are those of the oracle, once each");
    }

    // --- a datapack search ends when its world goes ---
    {
        DpCondition typical = c1;
        typical.r = 500;
        conditions->setConditions({typical});
        control->invalidateResults();
        SearchConfig sc = control->getSearchConfig();
        sc.searchtype = SEARCH_INC;
        sc.stoponres = false;
        sc.startseed = sc.smin = 300000;
        sc.smax = 900000;
        control->setSearchConfig(sc, true);
        control->setSearchRange(sc.smin, sc.smax);
        start->click();
        t.pump(300);
        const bool running = control->isSearching();
        t.packs->setPackEnabled(0, false);
        t.waitLoaded();
        QElapsedTimer limit;
        limit.start();
        while (control->isSearching() && limit.elapsed() < 20000)
            t.pump(20);
        t.check(running && !control->isSearching(), "the search stops when the packs are switched off");
        t.packs->setPackEnabled(0, true);
        t.waitLoaded();
        t.check(mw.datapackMode() && t.packs->current() && t.packs->current()->usable(),
            "packs on again after the stopped search");
    }

    // --- the position of a datapack search does not carry over to a
    // vanilla search, also without results ---
    {
        mw.formCond->clearConditions();
        QPushButton *quickAdd = mw.formCond->findChild<QPushButton*>("quickAdd");
        if (quickAdd)
            quickAdd->click(); // the first quick choice: a structure near the origin
        t.check(quickAdd && mw.formCond->getConditions().size() == 1, "a vanilla condition");
        DpCondition never = c4;
        never.id = 9;
        never.percent = 100;
        never.biomes = {idT};
        std::vector<uint64_t> few;
        for (int i = 0; i < 10; i++)
            few.push_back(first + 50 + i);
        const std::set<uint64_t> none = runSearch({never}, SEARCH_INC, few, "a search without results");
        const uint64_t dpAt = resumeAt ? resumeAt->text().toULongLong() : 0;
        t.check(none.empty() && dpAt >= few.front(), "the datapack search ends without results");
        t.packs->setPackEnabled(0, false);
        t.waitLoaded();
        SearchConfig sc = control->getSearchConfig(); // keeps the position shown
        sc.searchtype = SEARCH_INC;
        sc.smin = 0;
        sc.smax = ~(uint64_t)0;
        control->setSearchConfig(sc, true);
        control->setSearchRange(sc.smin, sc.smax);
        start->click();
        const QString startedAt = resumeAt ? resumeAt->text() : QString();
        start->click(); // abort
        QElapsedTimer limit;
        limit.start();
        while (control->isSearching() && limit.elapsed() < 60000)
            t.pump(20);
        t.check(!mw.datapackMode() && startedAt == "0",
            "a vanilla search after a datapack search starts at 0, not at " + startedAt);
        mw.formCond->clearConditions();
        control->invalidateResults();
        t.packs->setPackEnabled(0, true);
        t.waitLoaded();
        t.check(mw.datapackMode() && t.packs->current() && t.packs->current()->usable(),
            "packs on again after the vanilla search");
    }

    // the search types without whole seeds are refused with a reason
    messages.texts.clear();
    runSearch({c1}, SEARCH_BLOCKS, seeds, "structure seeds first");
    t.check(messages.texts.join(" ").contains("seed by seed"),
        "search: a structure seed search is refused: " + messages.texts.join(" | "));

    // --- speed of a typical condition, in one thread and with all threads ---
    {
        const auto single = [&](const DpCondition& c) {
            QString error;
            std::shared_ptr<const DpSearch> one = DpSearch::compile(shown, {c}, &error);
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 40 && one; i++)
                one->test(200000 + i, nullptr);
            releaseDatapackSampler();
            return 40 * 1000.0 / std::max<qint64>(1, timer.elapsed());
        };
        std::vector<uint64_t> range;
        for (int i = 0; i < 400; i++)
            range.push_back(100000 + i);
        DpCondition typical = c1;
        typical.r = 500;
        qint64 ms = 0;
        runSearch({typical}, SEARCH_INC, range, "speed 1:4", &ms);
        report << QString("search speed: biome within 500 blocks at 1:4, %1 threads: %2 seeds/s "
            "(one thread: %3 seeds/s)").arg(configuredThreadCount())
            .arg(400 * 1000.0 / std::max<qint64>(1, ms), 0, 'f', 1).arg(single(typical), 0, 'f', 1);
        DpCondition coarse = c4;
        coarse.shape = DpCondition::SQUARE;
        coarse.r = 1000;
        coarse.scale = 16;
        runSearch({coarse}, SEARCH_INC, range, "speed 1:16", &ms);
        report << QString("search speed: share of +-1000 blocks at 1:16, %1 threads: %2 seeds/s "
            "(one thread: %3 seeds/s)").arg(configuredThreadCount())
            .arg(400 * 1000.0 / std::max<qint64>(1, ms), 0, 'f', 1).arg(single(coarse), 0, 'f', 1);
    }

    // --- sessions: written and read again, and read without the packs ---
    {
        const std::vector<DpCondition> cv = {c1, c2, c3, c4};
        conditions->setConditions(cv);
        const QString path = appConfigDir() + "/datapack-search-session.txt";
        QString error;
        t.check(mw.saveSession(path, true, &error), "session: saved " + error);
        QFile file(path);
        QString text;
        if (file.open(QIODevice::ReadOnly))
            text = QString::fromUtf8(file.readAll());
        t.check(text.contains("#Datapacks: ") && text.count("#DpCond: ") == int(cv.size()),
            "session: the datapack lines are written");
        conditions->clearConditions();
        messages.texts.clear();
        t.check(mw.loadSession(path, false), "session: loaded");
        t.pump(50);
        t.check(conditions->getConditions() == cv, "session: the datapack conditions come back");
        t.check(messages.texts.isEmpty(), "session: no message with the packs loaded: "
            + messages.texts.join(" | "));
        // without packs: the conditions are kept, and a message says why
        // they do not apply
        t.packs->setPackEnabled(0, false);
        t.waitLoaded();
        messages.texts.clear();
        t.check(!mw.datapackMode() && mw.loadSession(path, false), "session: loaded without packs");
        t.pump(100);
        t.check(messages.texts.join(" ").contains("saved with datapacks"),
            "session: the message without packs: " + messages.texts.join(" | "));
        t.check(conditions->getConditions() == cv && mw.formCond->isVisibleTo(&mw)
            && !conditions->isVisibleTo(&mw), "session: kept, the vanilla conditions are shown");
        t.packs->setPackEnabled(0, true);
        t.waitLoaded();
        t.check(mw.datapackMode() && conditions->isVisibleTo(&mw), "session: packs on again");
        QFile::remove(path);
    }

    // --- sessions: the results and the progress of a vanilla search never
    // continue as a datapack search, and are saved as vanilla ones ---
    {
        const QString path = appConfigDir() + "/vanilla-search-session.txt";
        {
            QFile file(path);
            t.check(file.open(QIODevice::WriteOnly | QIODevice::Text), "session: vanilla file written");
            QTextStream s(&file);
            s << "#Version:  1.0.0\n#MC:       1.21.1\n#Large:    0\n#Search:   0\n"
                 "#Progress: 5000000\n#Threads:  1\n#ResStop:  0\n";
            s << c1.toLine() << "\n" << "111\n222\n333\n";
        }
        const QSet<QString> old = {"111", "222", "333"};
        QTableView *results = control->findChild<QTableView*>("results");
        const auto listed = [&]() {
            QStringList rows;
            for (int r = 0; results && r < results->model()->rowCount(); r++)
                rows << results->model()->index(r, 0).data().toString();
            return rows;
        };
        messages.texts.clear();
        t.check(mw.datapackMode() && mw.loadSession(path, false), "vanilla session: loaded with packs");
        t.pump(100);
        t.check(listed().size() == 3 && control->resultsWorld().set && !control->resultsWorld().datapack,
            "vanilla session: its results are listed as results of a vanilla world");
        t.check(messages.texts.join(" ").contains("vanilla world")
            && messages.texts.join(" ").contains("starts from the beginning"),
            "vanilla session: the message says that a search starts over: " + messages.texts.join(" | "));
        // saved again while the packs are on: still a vanilla session
        const QString again = appConfigDir() + "/vanilla-search-session-again.txt";
        QString error;
        t.check(mw.saveSession(again, true, &error), "vanilla session: saved again " + error);
        QFile file(again);
        QString text;
        if (file.open(QIODevice::ReadOnly))
            text = QString::fromUtf8(file.readAll());
        file.close();
        t.check(!text.contains("#Datapacks:") && text.contains("#Progress: 5000000"),
            "vanilla session: saved with packs on, it stays a vanilla session");
        messages.texts.clear();
        t.check(mw.loadSession(again, false) && control->resultsWorld().set
            && !control->resultsWorld().datapack, "vanilla session: read again");
        // a datapack search starts at 0 and drops the vanilla results
        start->click();
        const QString startedAt = resumeAt ? resumeAt->text() : QString();
        QStringList kept;
        for (const QString& row : listed())
            if (old.contains(row))
                kept << row;
        if (control->isSearching())
            start->click(); // abort
        QElapsedTimer limit;
        limit.start();
        while (control->isSearching() && limit.elapsed() < 60000)
            t.pump(20);
        t.check(startedAt == "0" && kept.isEmpty(), "vanilla session: a datapack search starts at "
            + startedAt + " and keeps " + kept.join(","));
        t.check(control->resultsWorld().datapack, "vanilla session: the results now belong to the datapack world");
        // a datapack session names its dimension
        QString dpText;
        t.check(mw.saveSession(again, true, &error), "datapack session: saved " + error);
        if (file.open(QIODevice::ReadOnly))
            dpText = QString::fromUtf8(file.readAll());
        file.close();
        t.check(dpText.contains("#Datapacks: ") && dpText.contains("#DpDimension: minecraft:overworld"),
            "datapack session: the datapack lines with the dimension");

        // a datapack session of the world that is shown resumes where it stopped
        control->invalidateResults();
        control->searchResultsAdd({444, 555}, false);
        SearchConfig sc = control->getSearchConfig();
        sc.searchtype = SEARCH_INC;
        sc.startseed = 7777;
        control->setSearchConfig(sc, true);
        t.check(mw.saveSession(again, true, &error), "datapack session: saved with progress " + error);
        control->invalidateResults();
        messages.texts.clear();
        WorldInfo shown;
        mw.getSeed(&shown, false);
        t.check(mw.loadSession(again, false) && listed().size() == 2
            && control->resultsWorld() == ResultsWorld::of(shown) && messages.texts.isEmpty(),
            "datapack session: read again as results of the shown world: " + messages.texts.join(" | "));
        start->click();
        const QString resumedAt = resumeAt ? resumeAt->text() : QString();
        const int rowsKept = listed().size();
        if (control->isSearching())
            start->click(); // abort
        limit.start();
        while (control->isSearching() && limit.elapsed() < 60000)
            t.pump(20);
        t.check(resumedAt == "7777" && rowsKept >= 2, "datapack session: the search resumes at "
            + resumedAt + " with " + QString::number(rowsKept) + " rows");

        // the same for a vanilla session read without packs
        t.packs->setPackEnabled(0, false);
        t.waitLoaded();
        control->invalidateResults();
        control->searchResultsAdd({666}, false);
        sc = control->getSearchConfig();
        sc.startseed = 8888;
        control->setSearchConfig(sc, true);
        t.check(!mw.datapackMode() && mw.saveSession(again, true, &error), "vanilla session: saved " + error);
        control->invalidateResults();
        mw.getSeed(&shown, false);
        t.check(mw.loadSession(again, false) && listed().size() == 1
            && control->resultsWorld() == ResultsWorld::of(shown)
            && control->getSearchConfig().startseed == 8888,
            "vanilla session: read again, it would resume at its position");
        t.packs->setPackEnabled(0, true);
        t.waitLoaded();
        control->invalidateResults();
        QFile::remove(path);
        QFile::remove(again);
    }

    // --- sessions: "#Amplified" before "#Large" ---
    {
        Session amplified;
        QString text = "#Version:  1.0.0\n#MC:       1.21.1\n#Amplified: 1\n#Large:    0\n";
        QTextStream stream(&text);
        t.check(amplified.load(nullptr, stream, true) && amplified.wi.amplified && !amplified.wi.large,
            "session: \"#Amplified: 1\" before \"#Large\" keeps the world Amplified");
    }

    // --- the biome analysis: counts of one seed and of the matching seeds ---
    {
        tabs->setCurrentWidget(biomesTab);
        t.pump(30);
        WorldInfo wi;
        mw.getSeed(&wi, false);
        wi.seed = first;
        wi.y = oracle.top;
        mw.setSeed(wi);
        t.waitLoaded();
        QTabWidget *sub = biomesTab->findChild<QTabWidget*>("tabWidget");
        QPushButton *analyze = biomesTab->findChild<QPushButton*>("pushStart");
        QComboBox *source = biomesTab->findChild<QComboBox*>("comboSeedSource");
        QComboBox *scale = biomesTab->findChild<QComboBox*>("comboScale");
        QTableView *table = biomesTab->findChild<QTableView*>("table");
        QTreeWidget *located = biomesTab->findChild<QTreeWidget*>("treeLocate");
        CheckableComboBox *pickBiomes = biomesTab->findChild<CheckableComboBox*>("comboBiome");
        biomesTab->findChild<QRadioButton*>("radioFullSample")->setChecked(true);
        const auto setArea = [&](int x1, int z1, int x2, int z2) {
            biomesTab->findChild<QLineEdit*>("lineX1")->setText(QString::number(x1));
            biomesTab->findChild<QLineEdit*>("lineZ1")->setText(QString::number(z1));
            biomesTab->findChild<QLineEdit*>("lineX2")->setText(QString::number(x2));
            biomesTab->findChild<QLineEdit*>("lineZ2")->setText(QString::number(z2));
        };
        const auto analyse = [&]() {
            analyze->click(); // the button says "Stop" until the analysis ends
            QElapsedTimer limit;
            limit.start();
            while (!analyze->text().startsWith("Analyze") && limit.elapsed() < 300000)
                t.pump(20);
            t.pump(300);
            return analyze->text().startsWith("Analyze");
        };
        // the counts of a table row against the oracle
        const auto checkRow = [&](int row, uint64_t s, int sc, int x1, int z1, int x2, int z2,
                const QString& what) {
            std::map<int, uint64_t> want;
            for (int j = DpOracle::floorDiv(z1, sc); j <= DpOracle::floorDiv(z2, sc); j++)
                for (int i = DpOracle::floorDiv(x1, sc); i <= DpOracle::floorDiv(x2, sc); i++)
                    want[oracle.biome(s, sc, i, oracle.top, j)]++;
            QAbstractItemModel *model = table->model();
            const int biomeCount = (int) oracleWorld->biomes().size();
            std::map<int, uint64_t> got;
            int listed = -1;
            for (int col = 0; col < model->columnCount(); col++)
            {
                const int id = model->headerData(col, Qt::Horizontal, Qt::UserRole).toInt();
                const uint64_t n = model->data(model->index(row, col)).toULongLong();
                if (id == biomeCount)
                    listed = int(n);
                else if (n)
                    got[id] = n;
            }
            want.erase(-1);
            const uint64_t rowSeed = model->headerData(row, Qt::Vertical, Qt::UserRole).toULongLong();
            t.log << QString("%1: %2 biomes, %3 counted").arg(what).arg(want.size()).arg(got.size());
            t.check(rowSeed == s && got == want && listed == int(want.size()),
                what + ": the counts are those of the oracle");
            // the column names are those of the packs
            bool names = model->columnCount() > 1;
            for (int col = 0; col < model->columnCount(); col++)
            {
                const int id = model->headerData(col, Qt::Horizontal, Qt::UserRole).toInt();
                if (id < biomeCount)
                    names = names && model->headerData(col, Qt::Horizontal, Qt::DisplayRole).toString()
                        == shown->labels[id];
            }
            t.check(names, what + ": the columns carry the names of the packs");
        };
        sub->setCurrentWidget(biomesTab->findChild<QWidget*>("tabStats"));
        setArea(-256, -256, 255, 255);
        scale->setCurrentIndex(1); // 1:4
        source->setCurrentIndex(0); // the seed of the map
        t.check(analyse(), "analysis: the statistics of one seed end");
        t.check(table->model()->rowCount() == 1, "analysis: one row");
        if (table->model()->rowCount() == 1)
            checkRow(0, first, 4, -256, -256, 255, 255, "analysis 1:4");

        // the matching seeds of the first search, at 1:16
        runSearch({c1}, SEARCH_INC, seeds, "search for the analysis");
        tabs->setCurrentWidget(biomesTab);
        setArea(-512, -512, 511, 511);
        scale->setCurrentIndex(2); // 1:16
        source->setCurrentIndex(1);
        t.check(analyse(), "analysis: the statistics of the matching seeds end");
        t.check(table->model()->rowCount() == int(hits1.size()), QString("analysis: %1 rows for %2 seeds")
            .arg(table->model()->rowCount()).arg(hits1.size()));
        for (int row = 0; row < table->model()->rowCount() && row < 3; row++)
        {
            const uint64_t s = table->model()->headerData(row, Qt::Vertical, Qt::UserRole).toULongLong();
            checkRow(row, s, 16, -512, -512, 511, 511, QString("analysis 1:16, seed %1").arg(s));
        }

        // the areas of a biome (Locate) against a flood fill of the oracle
        source->setCurrentIndex(0);
        sub->setCurrentWidget(biomesTab->findChild<QWidget*>("tabLocate"));
        setArea(-768, -768, 767, 767);
        biomesTab->findChild<QLineEdit*>("lineBiomeSize")->setText("1");
        int pickIndex = -1;
        for (int i = 0; i < pickBiomes->count(); i++)
            if (pickBiomes->itemData(i).toInt() == T)
                pickIndex = i;
        t.check(pickIndex >= 0 && pickBiomes->count() == (int) dimension.biomes.size(),
            "locate: the biomes of the dimension to pick from");
        pickBiomes->setCheckedData({T, U});
        emit pickBiomes->selectionChanged();
        t.check(analyse(), "locate: ends");
        std::multiset<std::tuple<QString,int,int,int>> got, want;
        for (QTreeWidgetItemIterator it(located); *it; ++it)
            if ((*it)->text(0) == "-")
                got.insert({(*it)->text(1), (*it)->text(2).toInt(), (*it)->text(3).toInt(),
                    (*it)->text(4).toInt()});
        for (int B : {T, U})
        {
            const int w = 384, h = 384, x0 = -192, z0 = -192;
            std::vector<int> grid(w * h), seen(w * h, 0);
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    grid[j * w + i] = oracle.biome(first, 4, x0 + i, oracle.top, z0 + j);
            for (int k = 0; k < w * h; k++)
            {
                if (grid[k] != B || seen[k])
                    continue;
                std::vector<int> stack = {k};
                seen[k] = 1;
                int64_t n = 0, sx = 0, sz = 0;
                while (!stack.empty())
                {
                    const int c = stack.back();
                    stack.pop_back();
                    const int ci = c % w, cj = c / w;
                    n++;
                    sx += x0 + ci;
                    sz += z0 + cj;
                    const int nb[4][2] = {{ci-1, cj}, {ci+1, cj}, {ci, cj-1}, {ci, cj+1}};
                    for (const auto& q : nb)
                    {
                        if (q[0] < 0 || q[0] >= w || q[1] < 0 || q[1] >= h)
                            continue;
                        const int cc = q[1] * w + q[0];
                        if (!seen[cc] && grid[cc] == B)
                        {
                            seen[cc] = 1;
                            stack.push_back(cc);
                        }
                    }
                }
                if (n >= 16)
                    want.insert({shown->labels[B], int(n), int(std::round((sx / double(n) + 0.5) * 4)),
                        int(std::round((sz / double(n) + 0.5) * 4))});
            }
        }
        t.log << QString("locate: %1 areas of %2 and %3, the analysis found %4")
            .arg(want.size()).arg(idT, idU).arg(got.size());
        // (the names in the results are those of the packs)
        t.check(want.size() >= 3 && got == want, "locate: the areas are those of the oracle");
        pickBiomes->setCheckedData({});
        emit pickBiomes->selectionChanged();
    }
    conditions->clearConditions();
    control->invalidateResults();
    return t.ok;
}

// ---------------------------------------------------------------------------
// Structures, heights, the Nether and the End and the pack check in a
// datapack world (part of SEED_ATLAS_UI_TEST_DATAPACK, alone with the value
// "structures"). It loads the pack SEED_ATLAS_TEST_DATAPACK_DIR/
// testpack-structures and compares everything with a finder or sampler of an
// own load of the same packs (the oracle), never with fixed numbers: the
// structure checks of the generator may still decide more starts.

namespace {

using StartKey = std::tuple<int, int, int, int, int, int, int, int, int>;

StartKey startKey(const dp::StructureStart& s)
{
    return std::make_tuple(s.structure, s.set, s.chunkX, s.chunkZ, s.x, s.z, int(s.state),
        int(s.detail), s.hasY ? s.y : INT_MIN);
}

int floorDiv16(int v)
{
    return v / 16 - ((v % 16 != 0) && (v < 0));
}

// the starts of the oracle in a block area (valid and unknown ones), with a
// margin of its own around the area
std::vector<dp::StructureStart> oracleStarts(dp::StructureFinder& finder, int x0, int z0,
    int x1, int z1, const std::vector<char> *mask)
{
    std::vector<dp::StructureStart> all, out;
    const int pad = 8;
    finder.starts(floorDiv16(x0) - pad, floorDiv16(z0) - pad, floorDiv16(x1) + pad,
        floorDiv16(z1) + pad, &all);
    for (const dp::StructureStart& s : all)
    {
        if (s.x < x0 || s.x > x1 || s.z < z0 || s.z > z1 || s.state == dp::StructureStart::Invalid)
            continue;
        if (mask && (s.structure < 0 || s.structure >= (int) mask->size() || !(*mask)[s.structure]))
            continue;
        out.push_back(s);
    }
    return out;
}

} // namespace

bool testDatapackStructures(DatapackTest& t, const QString& vanillaPath, const QString& packPath,
    QStringList& report)
{
    MainWindow& mw = t.mw;
    const int mc = MC_1_21_1;
    const uint64_t seed = 12345;
    QString error;
    QTabWidget *tabs = mw.findChild<QTabWidget*>("tabContainer");
    TabStructures *structTab = mw.findChild<TabStructures*>();
    if (!t.check(QFileInfo(packPath).isDir() && tabs && structTab,
            "structures: the test needs SEED_ATLAS_TEST_DATAPACK_DIR/testpack-structures"))
        return false;
    t.packs->clearPacks();
    t.waitLoaded();
    t.setWorld(mc, seed);
    t.check(mw.vanillaData->choose(mc, vanillaPath, &error), "structures: vanilla data: " + error);
    t.packs->setDimension("minecraft:overworld");
    t.packs->addPacks({packPath});
    t.waitLoaded();
    const std::shared_ptr<const DatapackWorld> shown = t.packs->current();
    if (!t.check(shown && shown->usable(), "structures: the pack loads"))
        return false;
    dp::Report oracleReport;
    const std::shared_ptr<const dp::World> oracle = dp::World::load(shown->world->request(), &oracleReport);
    const int dim = oracle->dimension("minecraft:overworld");
    const std::shared_ptr<const DpStructureCatalog> catalog = dpStructureCatalog(shown);
    if (!t.check(oracleReport.ok() && dim >= 0 && catalog
            && catalog->entries.size() == oracle->structures().size(), "structures: the oracle world"))
        return false;
    const int top = shown->maxY();
    t.setLevel(top);

    // the pack's structures and the villages (sidebar entry)
    QStringList packIds;
    for (const DpStructureEntry& e : catalog->entries)
        if (e.ns != QLatin1String("minecraft") && e.supported)
            packIds << e.id;
    t.check(packIds.size() >= 3, "structures: the test pack has structures of its own");
    for (const DpStructureEntry& e : catalog->entries)
        t.check(e.ns == QLatin1String("minecraft") || e.pack == QFileInfo(packPath).fileName(),
            "structures: the pack of " + e.id);

    // compares the structure tiles of the map with the oracle
    const auto compareMap = [&](int odim, const QString& what) {
        QWorld *world = t.map->world;
        if (!t.check(world && world->wi.dp && world->dpShowAny, what + ": structures on the map"))
            return 0;
        const std::vector<char> mask = t.map->getDatapackStructureMask();
        dp::StructureFinder finder(oracle, odim, seed);
        int tiles = 0, starts = 0, wrong = 0, unknown = 0;
        for (Quad *q : world->lvdp.cells)
        {
            const std::vector<dp::StructureStart> *got = q->dpstarts;
            if (!got)
                continue;
            const int x0 = q->ti * q->blocks, z0 = q->tj * q->blocks;
            std::vector<dp::StructureStart> want = oracleStarts(finder, x0, z0,
                x0 + q->blocks - 1, z0 + q->blocks - 1, &mask);
            std::multiset<StartKey> a, b;
            for (const dp::StructureStart& s : *got)
                if (s.structure >= 0 && s.structure < (int) mask.size() && mask[s.structure])
                    a.insert(startKey(s));
            for (const dp::StructureStart& s : want)
            {
                b.insert(startKey(s));
                unknown += s.state == dp::StructureStart::Unknown;
            }
            wrong += a != b;
            starts += int(b.size());
            tiles++;
        }
        t.log << QString("%1: %2 structure tiles, %3 starts (%4 not decided), %5 tiles differ")
            .arg(what).arg(tiles).arg(starts).arg(unknown).arg(wrong);
        t.check(tiles > 0 && wrong == 0, what + ": the structure tiles are those of the oracle");
        return starts;
    };

    // --- 1. structures on the map ---
    QElapsedTimer timer;
    dpClearFinderPool(); // a cold start: the stronghold rings are searched again
    timer.start();
    t.map->setDatapackStructures(packIds);
    mw.saction[D_VILLAGE]->setChecked(true);
    t.map->setView(0, 0, 4);
    t.check(t.waitMap() && t.waitStructures(), "structures: map complete");
    report << QString("structure tiles of the view, cold: %1 ms").arg(timer.elapsed());
    const int mapStarts = compareMap(dim, "structures overworld");
    t.check(mapStarts > 0, "structures: some starts in the view");
    {
        QWorld *world = t.map->world;
        // hiding a structure only filters, showing one more makes the tiles again
        const auto made = world ? world->lvdp.dpMask : nullptr;
        mw.saction[D_VILLAGE]->setChecked(false);
        t.pump(30);
        t.check(world && world->lvdp.dpMask == made, "structures: hiding does not make the tiles again");
        bool villageShown = false;
        for (size_t i = 0; world && i < catalog->entries.size(); i++)
            villageShown = villageShown || (catalog->entries[i].vanillaOpt == D_VILLAGE && world->dpShow[i]);
        t.check(!villageShown, "structures: the villages are hidden");
        mw.saction[D_VILLAGE]->setChecked(true);
        t.pump(30);
        t.check(world && world->lvdp.dpMask == made, "structures: showing them again needs no new tiles");
        t.map->setDatapackStructures(packIds.mid(0, 1));
        t.pump(30);
        t.check(world && world->lvdp.dpMask == made, "structures: fewer pack structures need no new tiles");
        t.map->setDatapackStructures(packIds);
        t.check(t.waitStructures(), "structures: map complete again");

        // the faded look of a start that is not decided
        const DpStructureEntry& e = catalog->entries[(size_t) catalog->entries.size() - 1];
        const QImage plain = dpStructureIcon(e, false).toImage(), faded = dpStructureIcon(e, true).toImage();
        t.check(!plain.isNull() && plain.size() == faded.size() && plain != faded,
            "structures: a start that is not decided has its own marker");

        // The map's paint keeps the address of each marker it has made while
        // it makes the markers of more structures, so a marker must stay
        // where it is while the cache of markers grows (it once held the
        // pixmaps themselves, which moved on growth, and a view with many
        // structures drew from freed memory). The made-up entries below grow
        // the cache well past any size it had.
        {
            std::vector<const QPixmap*> held;
            std::vector<qint64> keys;
            for (const DpStructureEntry& ce : catalog->entries)
                for (bool unknown : {false, true})
                {
                    const QPixmap& p = dpStructureIcon(ce, unknown);
                    held.push_back(&p);
                    keys.push_back(p.cacheKey());
                }
            DpStructureEntry extra = catalog->entries.front();
            extra.vanillaOpt = D_NONE;
            for (int i = 0; i < 4096; i++)
            {
                extra.id = QString("markercheck:structure_%1").arg(i);
                extra.letter = QChar('A' + i % 26);
                dpStructureIcon(extra, i % 2);
            }
            int moved = 0, changed = 0;
            size_t k = 0;
            for (const DpStructureEntry& ce : catalog->entries)
                for (bool unknown : {false, true})
                {
                    const QPixmap *now = &dpStructureIcon(ce, unknown);
                    if (now != held[k])
                        moved++;    // the old address is not read: it may be freed
                    else if (now->cacheKey() != keys[k])
                        changed++;
                    k++;
                }
            t.log << QString("structure markers: %1 held while 4096 more were made: %2 moved, %3 changed")
                .arg(held.size()).arg(moved).arg(changed);
            t.check(moved == 0 && changed == 0, "structures: a marker stays where the map's paint keeps it");
        }

        // a click on a start selects it (one with no other marker within
        // reach, so that the click can only mean that one)
        world = t.map->world;
        bool selected = false;
        std::vector<std::pair<QPointF, dp::StructureStart>> onScreen;
        for (Quad *q : world ? world->lvdp.cells : std::vector<Quad*>())
        {
            const std::vector<dp::StructureStart> *list = q->dpstarts;
            for (size_t k = 0; list && k < list->size(); k++)
            {
                const dp::StructureStart& s = (*list)[k];
                if (!world->dpShow[s.structure])
                    continue;
                onScreen.push_back({QPointF(t.map->width() / 2.0 + (s.x - t.map->getX()) / t.map->getScale(),
                    t.map->height() / 2.0 + (s.z - t.map->getZ()) / t.map->getScale()), s});
            }
        }
        for (size_t a = 0; a < onScreen.size() && !selected; a++)
        {
            const QPointF p = onScreen[a].first;
            const dp::StructureStart& s = onScreen[a].second;
            if (p.x() < 60 || p.y() < 80 || p.x() > t.map->width() - 60 || p.y() > t.map->height() - 80)
                continue;
            bool alone = true;
            for (size_t b = 0; b < onScreen.size() && alone; b++)
                alone = b == a || QLineF(p, onScreen[b].first).length() > 40;
            if (!alone)
                continue;
            world->setSelectPos(p.toPoint());
            t.map->grab();
            selected = true;
            t.check(world->dpSelected && world->dpSel.structure == s.structure
                && world->dpSel.x == s.x && world->dpSel.z == s.z,
                "structures: a click selects the start under it");
            world->setSelectPos(QPoint(-1000, -1000));
            t.map->grab();
        }
        t.check(selected, "structures: a start to select in the view");
    }

    // --- 2. heights and terrain ---
    const auto compareHeights = [&](int odim, int ytop, bool both, const QString& what) {
        QWorld *world = t.map->world;
        if (!t.check(world && world->activelv >= 0, what + ": a map"))
            return;
        dp::Sampler sampler(oracle, odim, seed);
        int tiles = 0, wrong = 0, shaded = 0;
        for (Quad *q : world->lvb[world->activelv].cells)
        {
            if (!q->img || !q->biomes)
                continue;
            const int w = q->r.sx, h = q->r.sz;
            std::vector<int> want(size_t(w + 2) * (h + 2));
            sampler.genHeights(want.data(), q->scale, q->r.x - 1, q->r.z - 1, w + 2, h + 2,
                dp::HEIGHT_OCEAN_FLOOR, ytop, nullptr, dpMapHeightDetail(q->scale));
            wrong += q->dpFloor != want;
            if (both)
            {
                sampler.genHeights(want.data(), q->scale, q->r.x - 1, q->r.z - 1, w + 2, h + 2,
                    dp::HEIGHT_SURFACE, ytop, nullptr, dpMapHeightDetail(q->scale));
                wrong += q->dpSurface != want;
            }
            // the layer changes the colours of the biome map
            for (int j = 0; j < h; j += 7)
                for (int i = 0; i < w; i += 7)
                {
                    const int id = q->biomes[size_t(j) * w + i];
                    const QRgb flat = id >= 0 && id < (int) shown->colors.size() ? shown->colors[id] : 0;
                    shaded += (q->img.loadRelaxed()->pixel(i, j) & 0xffffff) != (flat & 0xffffff);
                }
            tiles++;
        }
        t.log << QString("%1: %2 tiles, %3 differ, %4 shaded samples").arg(what).arg(tiles)
            .arg(wrong).arg(shaded);
        t.check(tiles > 0 && wrong == 0, what + ": the heights are those of the oracle");
        t.check(shaded > 0, what + ": the layer shades the map");
    };
    const auto layer = [&](int mode, int disp) {
        QMetaObject::invokeMethod(&mw, "onActionBiomeLayerSelect", Qt::DirectConnection,
            Q_ARG(int, mode), Q_ARG(int, disp));
        t.pump(30);
    };
    t.map->setDatapackStructures({});
    mw.saction[D_VILLAGE]->setChecked(false);
    timer.start();
    layer(LOPT_HEIGHT, 1);
    t.check(t.waitMap() && t.map->world && t.map->world->lopt.mode == LOPT_HEIGHT,
        "heights: the height layer is shown");
    report << QString("height layer of the view at 1:4: %1 ms").arg(timer.elapsed());
    compareHeights(dim, 0x7fffffff, false, "heights overworld");
    {   // the height under the cursor
        dp::Sampler sampler(oracle, dim, seed);
        QWorld *world = t.map->world;
        const int scale = world->lvb[world->activelv].blocks / world->lvb[world->activelv].pixs;
        int wrong = 0;
        for (int k = 0; k < 12; k++)
        {
            const Pos p = {k * 97 - 500, 300 - k * 61};
            const auto fl = [scale](int v) { return (v / scale - ((v % scale != 0) && v < 0)) * scale; };
            wrong += world->getDatapackHeight(p) != sampler.surfaceHeight(fl(p.x), fl(p.z),
                dp::HEIGHT_OCEAN_FLOOR, 0x7fffffff, dp::HEIGHTS_SURFACE);
        }
        t.check(wrong == 0, QString("heights: %1 of 12 cursor heights differ").arg(wrong));
    }
    layer(LOPT_TERRAIN, -1);
    t.check(t.waitMap() && t.map->world && t.map->world->lopt.mode == LOPT_TERRAIN,
        "terrain: the terrain layer is shown");
    compareHeights(dim, 0x7fffffff, true, "terrain overworld");
    layer(LOPT_BIOMES, -1);
    t.check(t.waitMap(), "heights: back to the biomes");

    // --- 3. the structure analysis of the current seed ---
    tabs->setCurrentWidget(structTab);
    t.pump(50);
    QLineEdit *x1 = structTab->findChild<QLineEdit*>("lineX1");
    QLineEdit *z1 = structTab->findChild<QLineEdit*>("lineZ1");
    QLineEdit *x2 = structTab->findChild<QLineEdit*>("lineX2");
    QLineEdit *z2 = structTab->findChild<QLineEdit*>("lineZ2");
    QComboBox *source = structTab->findChild<QComboBox*>("comboSeedSource");
    QAbstractButton *all = structTab->findChild<QAbstractButton*>("radioAll");
    QAbstractButton *onMap = structTab->findChild<QAbstractButton*>("radioMap");
    QCheckBox *collect = structTab->findChild<QCheckBox*>("checkCollect");
    QCheckBox *unknownBox = structTab->findChild<QCheckBox*>("checkDpUnknown");
    QPushButton *analyze = structTab->findChild<QPushButton*>("pushStart");
    StructureResultView *results = structTab->findChild<StructureResultView*>("treeStructs");
    QTabWidget *sub = structTab->findChild<QTabWidget*>("tabWidget");
    if (!t.check(x1 && z1 && x2 && z2 && source && all && onMap && collect && unknownBox && analyze
            && results && sub, "analysis: the widgets of the Structures tab"))
        return false;
    t.check(unknownBox->isVisibleTo(structTab), "analysis: the option for starts not decided");
    t.check(!sub->isTabEnabled(sub->indexOf(structTab->findChild<QWidget*>("tabDensity")))
        && !sub->isTabEnabled(sub->indexOf(structTab->findChild<QWidget*>("tabFortresses"))),
        "analysis: density and fortresses are off for datapack worlds");
    const int ax0 = -1500, az0 = -1200, ax1 = 1300, az1 = 1500;
    const auto runAnalysis = [&](int sourceIndex) {
        x1->setText(QString::number(ax0));
        z1->setText(QString::number(az0));
        x2->setText(QString::number(ax1));
        z2->setText(QString::number(az1));
        source->setCurrentIndex(sourceIndex);
        all->setChecked(true);
        collect->setChecked(true);
        unknownBox->setChecked(true);
        QElapsedTimer limit;
        limit.start();
        analyze->click();
        // the button says "Stop" while the analysis runs
        while (analyze->text() != QLatin1String("Analyze") && limit.elapsed() < 300000)
            t.pump(20);
        t.pump(50);
        // seed -> group name -> count
        std::map<uint64_t, std::map<QString, uint64_t>> got;
        StructureResultModel *model = results->resultModel();
        for (int r = 0; r < model->rowCount(); r++)
        {
            const QModelIndex si = model->index(r, 0);
            const uint64_t s = uint64_t(model->text(si, 0).section(' ', 0, 0).toLongLong());
            for (int g = 0; g < model->rowCount(si); g++)
            {
                const QModelIndex gi = model->index(g, 0, si);
                got[s][model->text(gi, 1)] = model->text(gi, 2).toULongLong();
            }
        }
        return got;
    };
    const auto oracleCounts = [&](const std::vector<dp::StructureStart>& starts) {
        std::map<QString, uint64_t> want;
        for (const dp::StructureStart& s : starts)
        {
            if (s.x < ax0 || s.x > ax1 || s.z < az0 || s.z > az1)
                continue;
            if (!catalog->entries[(size_t) s.structure].supported)
                continue;
            const QString label = catalog->entries[(size_t) s.structure].label;
            want[s.state == dp::StructureStart::Valid ? label : label + " (not decided)"]++;
        }
        return want;
    };
    {
        timer.start();
        const auto got = runAnalysis(0);
        const qint64 ms = timer.elapsed();
        dp::StructureFinder finder(oracle, dim, seed);
        const auto want = oracleCounts(oracleStarts(finder, ax0, az0, ax1, az1, nullptr));
        uint64_t total = 0;
        for (const auto& w : want)
            total += w.second;
        t.log << QString("analysis (current seed, %1 starts in %2 kinds): %3 ms")
            .arg(total).arg(want.size()).arg(ms);
        report << QString("structure analysis, one seed, %1 x %2 blocks: %3 ms")
            .arg(ax1 - ax0 + 1).arg(az1 - az0 + 1).arg(ms);
        const std::map<QString, uint64_t> mine = got.count(seed) ? got.at(seed)
            : std::map<QString, uint64_t>();
        int shownDiffs = 0;
        for (const auto& w : want)
            if (mine.count(w.first) == 0 || mine.at(w.first) != w.second)
                if (shownDiffs++ < 6)
                    t.log << QString("analysis: %1: %2 in the oracle, %3 in the tab").arg(w.first)
                        .arg(w.second).arg(mine.count(w.first) ? qint64(mine.at(w.first)) : -1);
        int onlyTab = 0;
        for (const auto& m : mine)
            if (want.count(m.first) == 0 && onlyTab++ < 6)
                t.log << QString("analysis: %1: only in the tab (%2)").arg(m.first).arg(m.second);
        if (shownDiffs || onlyTab)
            t.log << QString("analysis: %1 kinds in the oracle, %2 in the tab, %3 seeds").arg(want.size())
                .arg(mine.size()).arg(got.size());
        t.check(got.size() == 1 && got.count(seed) && got.at(seed) == want,
            "analysis: the counts of the current seed are those of the oracle");
    }

    // --- 4. structure conditions in the search ---
    FormSearchControl *control = mw.formControl;
    FormDpConditions *conditions = mw.formDpCond;
    QPushButton *start = control->findChild<QPushButton*>("buttonStart");
    const auto runSearch = [&](const std::vector<DpCondition>& cv, const std::vector<uint64_t>& seeds,
            qint64 *ms) {
        tabs->setCurrentWidget(mw.findChild<QWidget*>("tabSearch"));
        t.pump(30);
        conditions->setConditions(cv);
        control->invalidateResults();
        SearchConfig sc = control->getSearchConfig();
        sc.searchtype = SEARCH_INC;
        sc.stoponres = false;
        sc.startseed = seeds.front();
        sc.smin = seeds.front();
        sc.smax = seeds.back();
        sc.slist64path.clear();
        control->setSearchConfig(sc, true);
        control->setSearchRange(sc.smin, sc.smax);
        QElapsedTimer limit;
        limit.start();
        start->click();
        while (control->isSearching() && limit.elapsed() < 300000)
            t.pump(20);
        *ms = limit.elapsed();
        const std::vector<uint64_t> got = control->getResults();
        return std::set<uint64_t>(got.begin(), got.end());
    };
    // the oracle: the starts of each seed in an area that holds every condition
    const auto holds = [&](const DpCondition& c, const std::vector<dp::StructureStart>& starts) {
        std::set<std::tuple<int, int, int>> together;
        std::map<QString, std::set<std::tuple<int, int, int>>> each;
        for (const dp::StructureStart& s : starts)
        {
            const QString id = oracle->structures()[(size_t) s.structure].id;
            if (!c.structures.contains(id))
                continue;
            const bool undecidedCounts = c.unknownCounts || c.match == DpCondition::NONE;
            if (!(s.state == dp::StructureStart::Valid
                    || (undecidedCounts && s.state == dp::StructureStart::Unknown)))
                continue;
            int bx1, bz1, bx2, bz2;
            c.bounds(&bx1, &bz1, &bx2, &bz2);
            if (s.x < bx1 || s.x > bx2 || s.z < bz1 || s.z > bz2)
                continue;
            if (c.shape == DpCondition::CIRCLE && (int64_t(s.x) - c.x) * (s.x - c.x)
                    + (int64_t(s.z) - c.z) * (s.z - c.z) > int64_t(c.r) * c.r)
                continue;
            together.insert(std::make_tuple(s.set, s.chunkX, s.chunkZ));
            each[id].insert(std::make_tuple(s.set, s.chunkX, s.chunkZ));
        }
        if (c.match == DpCondition::NONE)
            return together.empty();
        if (c.match == DpCondition::ALL)
        {
            for (const QString& id : c.structures)
                if ((int) each[id].size() < c.count)
                    return false;
            return true;
        }
        return (int) together.size() >= c.count;
    };
    // the oracle's starts of a seed (-1600 .. 1600), made once per seed
    std::map<std::pair<int, uint64_t>, std::vector<dp::StructureStart>> oracleCache;
    const auto seedOracle = [&](int odim, uint64_t s) -> const std::vector<dp::StructureStart>& {
        auto it = oracleCache.find({odim, s});
        if (it == oracleCache.end())
        {
            dp::StructureFinder finder(oracle, odim, s);
            it = oracleCache.emplace(std::make_pair(odim, s),
                oracleStarts(finder, -1600, -1600, 1600, 1600, nullptr)).first;
        }
        return it->second;
    };
    const auto compareSearch = [&](int odim, const std::vector<DpCondition>& cv, int seedCount,
            const QString& what, std::map<uint64_t, std::vector<dp::StructureStart>> *keep) {
        std::vector<uint64_t> seeds;
        for (int i = 0; i < seedCount; i++)
            seeds.push_back(7000 + i);
        qint64 ms = 0;
        const std::set<uint64_t> got = runSearch(cv, seeds, &ms);
        std::set<uint64_t> want;
        for (uint64_t s : seeds)
        {
            const std::vector<dp::StructureStart>& starts = seedOracle(odim, s);
            bool ok = true;
            for (const DpCondition& c : cv)
                ok = ok && holds(c, starts);
            if (ok)
                want.insert(s);
            if (keep)
                (*keep)[s] = starts;
        }
        t.log << QString("%1: %2 of %3 seeds match, the search found %4 (%5 ms, %6 seeds/s)")
            .arg(what).arg(want.size()).arg(seedCount).arg(got.size()).arg(ms)
            .arg(QString::number(seedCount * 1000.0 / std::max<qint64>(1, ms), 'f', 1));
        report << QString("%1: %2 seeds/s").arg(what)
            .arg(QString::number(seedCount * 1000.0 / std::max<qint64>(1, ms), 'f', 1));
        t.check(!control->isSearching() && got == want, what + ": the results are those of the oracle");
        return want;
    };
    // structures with some, not all seeds of the range: chosen from the oracle
    std::map<uint64_t, std::vector<dp::StructureStart>> seedStarts;
    const int seedCount = 12;
    {
        DpCondition probe;
        probe.id = 1;
        probe.kind = DpCondition::STRUCTURE_IN_AREA;
        probe.shape = DpCondition::CIRCLE;
        probe.r = 600;
        probe.structures = packIds;
        compareSearch(dim, {probe}, seedCount, "structure search 1 (a pack structure within 600)", &seedStarts);
        // per structure: in how many seeds it starts (valid) within 600 blocks
        std::map<QString, int> present;
        for (const auto& ss : seedStarts)
            for (const QString& id : packIds)
            {
                DpCondition c = probe;
                c.structures = {id};
                present[id] += holds(c, ss.second);
            }
        QString a, b;
        for (const auto& p : present)
        {
            if (p.second <= 0 || p.second >= seedCount)
                continue;
            if (a.isEmpty())
                a = p.first;
            else if (b.isEmpty())
                b = p.first;
        }
        if (t.check(!a.isEmpty(), "search: a structure that some seeds have within 600 blocks"))
        {
            DpCondition c1 = probe;
            c1.structures = {a};
            const auto want = compareSearch(dim, {c1}, seedCount,
                QString("structure search 2 (%1 within 600)").arg(a), nullptr);
            t.check(!want.empty() && (int) want.size() < seedCount, "search 2: some seeds match, not all");
            DpCondition c2 = probe;
            c2.match = DpCondition::ALL;
            c2.structures = b.isEmpty() ? QStringList{a} : QStringList{a, b};
            c2.count = 1;
            c2.unknownCounts = true;
            c2.shape = DpCondition::SQUARE;
            c2.x = 100;
            c2.z = -50;
            c2.r = 700;
            DpCondition c3 = probe;
            c3.id = 2;
            c3.match = DpCondition::NONE;
            c3.shape = DpCondition::RECT;
            c3.x1 = -300;
            c3.z1 = -250;
            c3.x2 = 280;
            c3.z2 = 320;
            c3.structures = {packIds.last()};
            compareSearch(dim, {c2, c3}, seedCount, "structure search 3 (all, also not decided; none in a rectangle)", nullptr);
            DpCondition c4 = probe;
            c4.count = 2;
            compareSearch(dim, {c4}, seedCount, "structure search 4 (at least 2 starts)", nullptr);
        }
    }
    // --- 5. the analysis of the search results (seed list) ---
    {
        const std::vector<uint64_t> found = control->getResults();
        if (t.check(!found.empty(), "analysis: search results to analyze"))
        {
            tabs->setCurrentWidget(structTab);
            t.pump(30);
            timer.start();
            const auto got = runAnalysis(1);
            report << QString("structure analysis, %1 seeds: %2 ms").arg(found.size()).arg(timer.elapsed());
            int wrong = 0;
            for (uint64_t s : found)
            {
                const auto want = oracleCounts(seedOracle(dim, s));
                wrong += !got.count(s) || got.at(s) != want;
            }
            t.check(got.size() == found.size() && wrong == 0,
                QString("analysis: the counts of %1 seeds are those of the oracle (%2 differ)")
                    .arg(found.size()).arg(wrong));
        }
    }

    // --- 6. the Nether and the End ---
    for (const char *name : {"minecraft:the_nether", "minecraft:the_end"})
    {
        const QString id = QString::fromLatin1(name);
        const bool nether = id.contains("nether");
        t.packs->setDimension(id);
        t.waitLoaded();
        t.pump(50);
        const std::shared_ptr<const DatapackWorld> w = t.packs->current();
        const int odim = oracle->dimension(id);
        if (!t.check(w && w->usable() && w->dimensionId == id && odim >= 0, id + ": shown"))
            continue;
        t.map->setView(0, 0, 4);
        t.check(t.waitMap(), id + ": map complete");
        QWorld *world = t.map->world;
        const int y = world ? world->wi.y : 0;
        {   // no slime chunks outside the Overworld, also when they are switched on
            const bool slimeShown = t.map->getShow(D_SLIME);
            t.map->setShow(D_SLIME, true);
            t.pump(20);
            TabSlime *slime = mw.findChild<TabSlime*>();
            if (slime)
                slime->refresh();
            QWidget *slimeBody = slime ? slime->findChild<QWidget*>("slimeBody") : nullptr;
            t.check(t.map->world && !t.map->world->sshow[D_SLIME]
                && (!slimeBody || !slimeBody->isEnabled()), id + ": no slime chunks");
            t.map->setShow(D_SLIME, slimeShown);
        }
        if (nether)
        {   // the picker starts below the roof
            const int roof = dpBelowRoofLevel(w, seed);
            QComboBox *levels = mw.findChild<QComboBox*>("comboY");
            bool listed = false;
            for (int i = 0; levels && i < levels->count(); i++)
                listed = listed || levels->itemText(i).contains("below the roof");
            t.log << QString("%1: below the roof at y %2, the map at y %3").arg(id).arg(roof).arg(y);
            t.check(roof != INT_MIN && roof < w->maxY() && y == roof && listed,
                id + ": the level below the roof");
        }
        t.checkTiles(oracle, odim, seed, y, id);
        // structures of the dimension
        const int opt = nether ? D_FORTESS : D_ENDCITY;
        mw.saction[opt]->setChecked(true);
        t.map->setView(0, 0, nether ? 2 : 8);
        t.check(t.waitMap() && t.waitStructures(), id + ": structures complete");
        compareMap(odim, id + " structures");
        mw.saction[opt]->setChecked(false);
        // heights: the Nether looks down from the chosen level
        layer(LOPT_HEIGHT, 1);
        t.map->setView(0, 0, 4);
        t.check(t.waitMap(), id + ": height layer complete");
        compareHeights(odim, nether ? y : 0x7fffffff, false, id + " heights");
        layer(LOPT_BIOMES, -1);
        t.check(t.waitMap(), id + ": back to the biomes");
    }
    t.packs->setDimension("minecraft:overworld");
    t.waitLoaded();

    // --- 7. the pack check ---
    {
        DpCheckInput input;
        input.mc = mc;
        input.vanillaPath = vanillaPath;
        input.preset = shown->preset;
        input.packs = {packPath};
        input.dimension = "minecraft:overworld";
        input.seed = seed;
        timer.start();
        const QString text = dpCheckReport(input, nullptr);
        report << QString("pack check of one pack: %1 ms").arg(timer.elapsed());
        const QString name = QFileInfo(packPath).fileName();
        int structures = 0;
        for (const DpStructureEntry& e : catalog->entries)
            structures += e.ns != QLatin1String("minecraft");
        for (const QString& want : {QString("== All 1 packs together =="),
                QString("== Pack 1: %1 ==").arg(name), QString("worldgen files per registry:"),
                QString("worldgen/structure: %1").arg(structures), QString("dimension files:"),
                QString("load alone:"), QString("speed sample"), QString("biomes at 1:4:"),
                QString("heights at 1:4:"), QString("structures: first query")})
            t.check(text.contains(want), "pack check: the report has \"" + want + "\"");
        t.check(!text.contains(QFileInfo(packPath).absolutePath())
            && !text.contains(QFileInfo(vanillaPath).absolutePath()), "pack check: no folders in the report");
        t.check(text.contains(QString("delete this line if the seed is private): %1").arg(qint64(seed))),
            "pack check: the seed on a line of its own");
        // files that are gone when the check reads them: the load names
        // them with their folders, the report must not
        const QString away = appConfigDir() + "/Users/jane.doe";
        const auto noFolders = [&](const QString& report, const QString& what) {
            QStringList leaks;
            for (const QString& line : report.split('\n'))
                if (line.contains("jane.doe") || line.contains(appConfigDir())
                    || line.contains(QDir::homePath() + "/") || line.contains("\"/"))
                    leaks << line.trimmed();
            t.check(!report.isEmpty() && leaks.isEmpty(), what + ": " + leaks.join(" | "));
        };
        DpCheckInput gone = input;
        gone.packs = {packPath, away + "/packs/Other Pack"};
        const QString goneText = dpCheckReport(gone, nullptr);
        noFolders(goneText, "pack check: a pack that is gone, no folders");
        t.check(goneText.contains("\"Other Pack\""), "pack check: the missing pack is named");
        gone.vanillaPath = away + "/Minecraft data/server-1.21.1.jar";
        gone.packs = {away + "/packs/Other Pack"};
        noFolders(dpCheckReport(gone, nullptr), "pack check: vanilla data that is gone, no folders");
        noFolders(dpScrubPaths(QString("a \"%1\" b %2 \"C:\\\\Users\\\\jane.doe\\\\x.zip\" "
            "\"\\\\\\\\server\\\\share\\\\jane.doe\\\\y.zip\"").arg(away + "/z.zip", QDir::homePath() + "/w"),
            {}), "pack check: other absolute paths");
        // through the panel
        mw.tabDatapacks->checkPacks();
        QElapsedTimer limit;
        limit.start();
        while (mw.tabDatapacks->checking() && limit.elapsed() < 300000)
            t.pump(50);
        t.check(mw.tabDatapacks->lastCheckReport().contains(QString("== Pack 1: %1 ==").arg(name)),
            "pack check: the panel's report");
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->objectName() == "dpCheckDialog")
                top->close();
    }

    t.map->setDatapackStructures({});
    t.packs->clearPacks();
    t.waitLoaded();
    return t.ok;
}

// The resident memory of the process now, in MiB (0 where not known)
double currentMemoryMb()
{
#ifdef Q_OS_MACOS
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return info.resident_size / 1048576.0;
#elif defined(Q_OS_LINUX)
    QFile statm("/proc/self/statm");
    if (statm.open(QIODevice::ReadOnly))
    {
        const QList<QByteArray> f = statm.readAll().split(' ');
        if (f.size() > 1)
            return f[1].toDouble() * 4096.0 / 1048576.0;
    }
#endif
    return 0;
}

// Times of the map in a datapack world (SEED_ATLAS_UI_DP_TIMING=<file>, with
// the packs of SEED_ATLAS_UI_DATAPACKS): for the biome, height and terrain
// layers at several scales, how long the map takes to finish a view it has
// not shown before; the biome tiles of the views at 1:1, 1:4 and 1:16 against
// the generator; the
// structure tiles with every supported structure of the packs shown, against
// a finder of the generator; and the memory after loading the packs again
// (SEED_ATLAS_UI_DP_RELOADS times, default 3). Writes one line per
// measurement; returns false if a comparison failed.
bool runDatapackTiming(DatapackTest& t, const QString& outPath)
{
    QFile out(outPath);
    if (!t.check(out.open(QIODevice::WriteOnly | QIODevice::Text), "timing: cannot write " + outPath))
        return false;
    QTextStream ts(&out);
    MainWindow& mw = t.mw;
    std::shared_ptr<const DatapackWorld> shown = t.packs->current();
    if (!t.check(shown && shown->usable(), "timing: a usable datapack world"))
        return false;
    WorldInfo wi;
    mw.getSeed(&wi, false);
    dp::Report report;
    const std::shared_ptr<const dp::World> oracle = dp::World::load(shown->world->request(), &report);
    const int dim = oracle->dimension(shown->dimensionId);
    if (!t.check(dim >= 0, "timing: the oracle world"))
        return false;
    const dp::DimensionInfo& info = oracle->dimensions()[(size_t) dim];
    ts << "window " << t.map->width() << " x " << t.map->height() << ", " << QThread::idealThreadCount()
       << " cores, dimension " << (shown->dimensionId.startsWith("minecraft:") ? shown->dimensionId : "of a pack")
       << ", level y " << wi.y << ", memory " << QString::number(currentMemoryMb(), 'f', 0) << " MiB\n";

    int place = 0;
    const auto freshView = [&](double scale) {
        place++;
        t.map->setView(36000.0 * place + 8, -22000.0 * place + 8, scale);
    };
    const auto activeTiles = [&]() {
        QWorld *world = t.map->world;
        return world && world->activelv >= 0 && world->activelv < (int) world->lvb.size()
            ? (int) world->lvb[world->activelv].cells.size() : 0;
    };
    struct LayerCase { const char *name; int mode; };
    static const LayerCase layers[] = { {"biomes", LOPT_BIOMES}, {"heights", LOPT_HEIGHT}, {"terrain", LOPT_TERRAIN} };
    for (const LayerCase& layer : layers)
    {
        if (layer.mode != LOPT_BIOMES && !info.terrain)
            continue;
        QMetaObject::invokeMethod(&mw, "onActionBiomeLayerSelect", Qt::DirectConnection,
            Q_ARG(int, layer.mode), Q_ARG(int, -1));
        t.pump(50);
        for (double scale : {1.0, 4.0, 16.0, 64.0, 256.0})
        {
            freshView(scale);
            QElapsedTimer timer;
            timer.start();
            const bool done = t.waitMap();
            const qint64 ms = timer.elapsed();
            QString line = QString("%1 1:%2: %3 ms for %4 tiles%5").arg(layer.name).arg(scale).arg(ms)
                .arg(activeTiles()).arg(done ? "" : " (not finished after a minute)");
            if (done && layer.mode == LOPT_BIOMES && (scale == 1.0 || scale == 4.0 || scale == 16.0))
            {
                int checked = 0;
                const bool same = t.checkTiles(oracle, dim, wi.seed, wi.y,
                    QString("timing biomes 1:%1").arg(scale), nullptr, &checked);
                line += QString(", %1 tiles against the generator: %2").arg(checked).arg(same ? "same" : "DIFFERENT");
            }
            ts << line << "\n";
            ts.flush();
        }
    }
    QMetaObject::invokeMethod(&mw, "onActionBiomeLayerSelect", Qt::DirectConnection,
        Q_ARG(int, LOPT_BIOMES), Q_ARG(int, -1));

    // every supported structure of the packs on the map
    mw.openDatapackStructures();
    if (mw.dpStructDialog)
    {
        mw.dpStructDialog->showAll(true);
        mw.dpStructDialog->close();
    }
    t.pump(50);
    for (double scale : {4.0, 16.0, 4.0})
    {
        freshView(scale);
        QElapsedTimer timer;
        timer.start();
        const bool done = t.waitMap() && t.waitStructures();
        const qint64 ms = timer.elapsed();
        QWorld *world = t.map->world;
        int tiles = 0, starts = 0, wrong = 0;
        if (done && world)
        {
            const std::vector<char> mask = t.map->getDatapackStructureMask();
            dp::StructureFinder finder(oracle, dim, wi.seed);
            for (Quad *q : world->lvdp.cells)
            {
                const std::vector<dp::StructureStart> *got = q->dpstarts;
                if (!got)
                    continue;
                const int x0 = q->ti * q->blocks, z0 = q->tj * q->blocks;
                std::vector<dp::StructureStart> want = oracleStarts(finder, x0, z0, x0 + q->blocks - 1,
                    z0 + q->blocks - 1, &mask);
                std::multiset<StartKey> a, b;
                for (const dp::StructureStart& s : *got)
                    if (s.structure >= 0 && s.structure < (int) mask.size() && mask[s.structure])
                        a.insert(startKey(s));
                for (const dp::StructureStart& s : want)
                    b.insert(startKey(s));
                wrong += a != b;
                starts += (int) b.size();
                tiles++;
            }
        }
        t.check(done && wrong == 0, QString("timing: structure tiles at 1:%1").arg(scale));
        ts << QString("structures 1:%1: %2 ms for %3 structure tiles, %4 starts, tiles against the generator: %5%6\n")
            .arg(scale).arg(ms).arg(tiles).arg(starts).arg(wrong ? "DIFFERENT" : "same")
            .arg(done ? "" : " (not finished)");
        ts.flush();
    }
    if (mw.dpStructDialog)
    {
        mw.openDatapackStructures();
        mw.dpStructDialog->showAll(false);
        mw.dpStructDialog->close();
    }

    // load the packs again, with the memory after each load
    QStringList paths;
    for (const PackEntry& e : t.packs->entries())
        paths << e.path;
    const QString dimension = shown->dimensionId;
    const int reloads = qEnvironmentVariableIsSet("SEED_ATLAS_UI_DP_RELOADS")
        ? qEnvironmentVariableIntValue("SEED_ATLAS_UI_DP_RELOADS") : 3;
    for (int i = 1; i <= reloads; i++)
    {
        QElapsedTimer timer;
        timer.start();
        t.packs->clearPacks();
        t.waitLoaded();
        t.packs->addPacks(paths);
        t.packs->setDimension(dimension);
        t.waitLoaded();
        const qint64 loadMs = timer.elapsed();
        freshView(4.0);
        const bool done = t.waitMap();
        t.check(done && t.packs->current() && t.packs->current()->usable(), "timing: loaded again");
        ts << QString("load %1: %2 ms, map %3, memory %4 MiB (peak %5 MiB)\n").arg(i).arg(loadMs)
            .arg(done ? "done" : "not finished").arg(currentMemoryMb(), 0, 'f', 0).arg(maxMemoryMb(), 0, 'f', 0);
        ts.flush();
    }
    return t.ok;
}

bool testDatapacks(MainWindow& mw)
{
    if (qgetenv("SEED_ATLAS_UI_TEST_DATAPACK") == "restore")
        return testDatapackRestore(mw);
    if (qgetenv("SEED_ATLAS_UI_TEST_DATAPACK") == "structures")
    {   // only structures, heights, Nether and End and the pack check
        DatapackTest t(mw);
        const QString vanillaPath = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_VANILLA"));
        const QString dir = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_DATAPACK_DIR"));
        QStringList report;
        testDatapackStructures(t, vanillaPath, dir + "/testpack-structures", report);
        for (const QString& line : std::as_const(report))
            fprintf(stderr, "datapack test: %s\n", qPrintable(line));
        for (const QString& line : std::as_const(t.log))
            fprintf(stderr, "datapack test: %s\n", qPrintable(line));
        return t.ok;
    }
    if (qgetenv("SEED_ATLAS_UI_TEST_DATAPACK") == "search")
    {   // only the search and the analyses (see testDatapackSearch())
        DatapackTest t(mw);
        const QString vanillaPath = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_VANILLA"));
        const QString packPath = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_DATAPACK_DIR"))
            + "/testpack";
        QString error;
        t.packs->clearPacks();
        t.waitLoaded();
        t.setWorld(MC_1_21_1, 20261116);
        t.check(mw.vanillaData->choose(MC_1_21_1, vanillaPath, &error), "vanilla data: " + error);
        t.packs->addPacks({packPath});
        t.waitLoaded();
        dp::Request request;
        request.mc = MC_1_21_1;
        request.vanillaPath = vanillaPath;
        request.packs.push_back(dp::PackSource{packPath, true});
        dp::Report oracleReport;
        const std::shared_ptr<const dp::World> oracle = dp::World::load(request, &oracleReport);
        const int dim = oracle->dimension("minecraft:overworld");
        QStringList report;
        if (t.check(oracleReport.ok() && dim >= 0 && mw.datapackMode(), "search: setup"))
            testDatapackSearch(t, oracle, dim, report);
        t.packs->clearPacks();
        t.waitLoaded();
        for (const QString& line : std::as_const(report))
            fprintf(stderr, "datapack test: %s\n", qPrintable(line));
        for (const QString& line : std::as_const(t.log))
            fprintf(stderr, "datapack test: %s\n", qPrintable(line));
        return t.ok;
    }

    DatapackTest t(mw);
    const QString vanillaPath = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_VANILLA"));
    const QString dir = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_TEST_DATAPACK_DIR"));
    const QString packPath = dir + "/testpack";
    const int mc = MC_1_21_1;
    const uint64_t seed = 20261116;
    TabDatapacks *tab = mw.tabDatapacks;
    QTabWidget *tabs = mw.findChild<QTabWidget*>("tabContainer");
    QStringList report;

    if (!t.check(!vanillaPath.isEmpty() && QFileInfo::exists(vanillaPath)
            && QFileInfo(packPath).isDir() && tab && tabs && t.packs,
            "the test needs SEED_ATLAS_TEST_VANILLA and SEED_ATLAS_TEST_DATAPACK_DIR/testpack"))
    {
        fprintf(stderr, "datapack test: %s\n", qPrintable(t.log.join("\n")));
        return false;
    }
    // nothing of an earlier, interrupted run
    t.packs->clearPacks();
    mw.vanillaData->forget(mc);
    t.map->setDatapackHighlights({});
    mw.datapackLevel = INT_MIN;
    t.waitLoaded();
    t.check(t.packs->entries().empty() && !mw.datapackMode(), "starts without packs");

    // --- 1. the built-in world, as the reference for "nothing changes" ---
    t.setWorld(mc, seed);
    t.map->setView(0, 0, 4);
    t.check(t.waitMap(), "vanilla map complete");
    const auto vanillaTiles = t.tiles();
    WorldInfo vanillaWi = t.map->world ? t.map->world->wi : WorldInfo();
    t.check(vanillaTiles.size() >= 4 && !vanillaWi.dp, "vanilla tiles");
    const bool spawnShown = t.map->getShow(D_SPAWN);
    t.check(spawnShown && t.map->world && t.map->world->sshow[D_SPAWN],
        "the spawn is shown in the vanilla world");

    // --- 2. vanilla data: none at first (test runs do not look into the
    // Minecraft installation of the machine), then the given path ---
    t.check(mw.vanillaData->find(mc).kind == VanillaSource::None, "no vanilla data at first");
    t.packs->addPacks({packPath});
    t.waitLoaded();
    t.check(mw.datapackMode() && t.packs->current()
        && t.packs->current()->state == DatapackWorld::NoVanillaData && !t.map->world,
        "without vanilla data the map stays empty");
    t.check(tab->stateLine().contains("game data"), "the panel says that the game data is missing");
    QString error;
    t.check(!mw.vanillaData->choose(mc, packPath, &error) && !error.isEmpty(),
        "a data pack is refused as vanilla data");
    t.packs->clearPacks();
    t.waitLoaded();
    t.check(!mw.datapackMode() && t.map->world && !t.map->world->wi.dp,
        "without packs the built-in world is back");
    t.check(mw.vanillaData->choose(mc, vanillaPath, &error), "vanilla data accepted: " + error);
    t.check(mw.vanillaData->find(mc).kind == VanillaSource::Chosen, "the chosen path is found");

    // --- 3. load the test pack: time until the first complete map ---
    const double memoryBefore = maxMemoryMb();
    t.map->setView(0, 0, 4); // the default zoom of a new window is 1:8, level 1:4
    QElapsedTimer timer;
    timer.start();
    t.packs->addPacks({packPath});
    t.waitLoaded();
    const qint64 loadMs = timer.elapsed();
    t.check(mw.datapackMode() && t.packs->current() && t.packs->current()->usable(),
        "the test pack loads: " + tab->stateLine() + " " + tab->issuesText());
    t.check(t.waitMap(), "datapack map complete");
    const qint64 firstMapMs = timer.elapsed();
    report << QString("add pack -> world loaded: %1 ms; -> first complete map (1:4, %2 tiles, "
        "window %3x%4): %5 ms").arg(loadMs).arg(t.tiles().size()).arg(t.map->width())
        .arg(t.map->height()).arg(firstMapMs);
    if (!t.ok || !t.map->world || !t.map->world->wi.dp)
    {
        fprintf(stderr, "datapack test: %s\n", qPrintable(t.log.join("\n")));
        return false;
    }

    // the oracle: an own load of the same request, an own sampler
    const std::shared_ptr<const DatapackWorld> shown = t.packs->current();
    dp::Request request;
    request.mc = mc;
    request.vanillaPath = vanillaPath;
    request.packs.push_back(dp::PackSource{packPath, true});
    dp::Report oracleReport;
    const std::shared_ptr<const dp::World> oracle = dp::World::load(request, &oracleReport);
    const int dim = oracle->dimension("minecraft:overworld");
    if (!t.check(oracleReport.ok() && dim >= 0 && oracle->dimensions()[dim].supported,
            "oracle world"))
    {
        fprintf(stderr, "datapack test: %s\n", qPrintable(t.log.join("\n")));
        return false;
    }
    const dp::DimensionInfo& dimension = oracle->dimensions()[dim];
    const int top = dimension.minY + dimension.height - 1;
    t.check(shown->world->biomes().size() == oracle->biomes().size()
        && shown->dimensionId == "minecraft:overworld" && !shown->presetMatters,
        "the shown world is the oracle's (the test pack has its own overworld file)");
    t.check(t.map->world->wi.y == top && t.map->world->dim == DIM_OVERWORLD,
        "a datapack world starts at the top of its dimension");
    QComboBox *levels = mw.findChild<QComboBox*>("comboY");
    QComboBox *worldType = mw.findChild<QComboBox*>("comboWorldType");
    t.check(levels && levels->itemText(0).startsWith(QString::number(top))
        && levels->itemText(levels->count() - 1).startsWith(QString::number(dimension.minY)),
        "the Y picker has the levels of the dimension");
    t.check(worldType && !worldType->isEnabled(), "the world type is off for a pack with its own overworld");

    // --- 4. tiles at every zoom level, also at negative coordinates and at
    // levels below the surface ---
    struct View { qreal x, z, scale; int y; };
    const View views[] = {
        {0, 0, 4, top}, {-3000, -1700, 1, 40}, {-640, 900, 1, top}, {5000, -7000, 4, -20},
        {-20000, 15000, 16, top}, {60000, -90000, 64, 0}, {-300000, -200000, 256, top},
        {0, 0, 1024, 63},
    };
    for (const View& view : views)
    {
        t.setLevel(view.y);
        t.map->setView(view.x, view.z, view.scale);
        const QString what = QString("1:%1 at %2,%3 y %4").arg(view.scale).arg(view.x)
            .arg(view.z).arg(view.y);
        t.check(t.waitMap(), what + ": map complete");
        QWorld *world = t.map->world;
        if (!t.check(world && world->wi.dp && world->wi.y == view.y, what + ": level"))
            continue;
        const int scale = world->lvb[world->activelv].scale;
        t.check(scale == int(view.scale), what + QString(": shown at 1:%1").arg(scale));
        t.checkTiles(oracle, dim, seed, view.y, what);

        // --- 5. the names under the cursor, at the zoom of the view ---
        dp::Sampler sampler(oracle, dim, seed);
        int names = 0, wrongNames = 0;
        for (int k = 0; k < 40; k++)
        {
            const Pos p = {int(view.x) + (k * 37 - 700) * scale / 2, int(view.z) + (k * 53 - 900) * scale / 2};
            const auto floorDiv = [](int a, int b) { return a / b - (a % b < 0); };
            const int id = sampler.biomeAt(scale, floorDiv(p.x, scale), view.y, floorDiv(p.z, scale));
            const QString want = id >= 0 ? oracle->biomes()[id].name : QString();
            names++;
            wrongNames += world->getBiomeName(p) != want || world->getDatapackBiome(p) != id;
        }
        t.check(wrongNames == 0, what + QString(": %1 of %2 names differ").arg(wrongNames).arg(names));
    }

    // --- 6. what is switched off in a datapack world, and what stays ---
    t.setLevel(top);
    t.map->setView(0, 0, 4);
    t.check(t.waitMap(), "map complete");
    {
        QWorld *world = t.map->world;
        // the built-in generator's structures, spawn and strongholds stay
        // off; the datapack world's structures come from its own level
        t.check(world && !world->sshow[D_SPAWN] && !world->sshow[D_STRONGHOLD]
            && !world->sshow[D_VILLAGE] && world->sshow[D_GRID] && !world->spawn,
            "no structures of the built-in generator, no spawn on a datapack map");
        t.check(t.map->getShow(D_SPAWN) == spawnShown, "the choice of the spawn layer is kept");
        t.check(mw.saction[D_SPAWN] && !mw.saction[D_SPAWN]->isEnabled()
            && mw.saction[D_SPAWN]->isChecked(), "the spawn layer is disabled, not unchecked");
        t.check(mw.saction[D_GRID]->isEnabled() && mw.saction[D_SLIME]->isEnabled(),
            "grid and slime chunks stay available");
        t.check(mw.terrainAction && mw.terrainAction->isEnabled(),
            "the terrain view is on (the test pack's overworld has terrain)");
        t.check(mw.laction[LOPT_HEIGHT]->isEnabled() && !mw.laction[LOPT_NOISE_T_4]->isEnabled()
            && !mw.laction[LOPT_STRUCTS]->isEnabled(),
            "the height layer is on; climate and structure-only layers are off");
        t.check(mw.dpStructAction && mw.dpStructAction->isVisible() && mw.dpStructAction->isEnabled(),
            "the Pack structures entry is there");
        for (int i = 0; i < 3; i++)
            t.check(!mw.dimactions[i]->isEnabled(), "the dimension buttons are off");
        TabBiomes *biomes = mw.findChild<TabBiomes*>();
        TabSlime *slime = mw.findChild<TabSlime*>();
        QWidget *search = mw.findChild<QWidget*>("tabSearch");
        t.check(mw.findChild<TabStructures*>()->isEnabled()
            && !mw.findChild<TabPortals*>()->isEnabled(),
            "the structure analysis is on, End Portals (vanilla only) is off");
        t.check(biomes && biomes->isEnabled() && search && search->isEnabled(),
            "the Seed Finder and the biome analysis have a datapack side");
        t.check(slime && slime->isEnabled() && tab->isEnabled(), "slime chunks and datapacks stay on");
        tabs->setCurrentWidget(mw.findChild<TabStructures*>());
        t.check(mw.datapackNotice && !mw.datapackNotice->isVisibleTo(&mw),
            "no notice above the structure analysis");
        tabs->setCurrentWidget(mw.findChild<TabPortals*>());
        t.check(mw.datapackNotice && mw.datapackNotice->isVisibleTo(&mw)
            && mw.datapackNotice->text().contains("vanilla worlds"), "the notice above a vanilla-only tool");
        tabs->setCurrentWidget(biomes);
        t.check(!mw.datapackNotice->isVisibleTo(&mw), "no notice above the biome analysis");
        tabs->setCurrentWidget(slime);
        t.check(!mw.datapackNotice->isVisibleTo(&mw), "no notice above the slime chunks");
        tabs->setCurrentWidget(tab);
        t.check(mw.packChip && mw.packChip->isVisibleTo(&mw) && mw.packChip->text() == "1",
            "the pack chip of the world card");
        // slime chunks are drawn (seed only)
        mw.saction[D_SLIME]->setChecked(true);
        t.pump(30);
        t.check(t.map->world && t.map->world->sshow[D_SLIME], "slime chunks can be shown");
        mw.saction[D_SLIME]->setChecked(false);
    }

    // --- 7. highlight ---
    {
        dp::Sampler sampler(oracle, dim, seed);
        const int id = sampler.biomeAt(4, 0, top, 0);
        t.check(id >= 0, "a biome at the origin");
        if (id >= 0)
        {
            const QSet<int> marked = {id};
            tab->setHighlightedBiomes({oracle->biomes()[id].id});
            t.check(t.waitMap(), "map complete");
            t.checkTiles(oracle, dim, seed, top, "highlight", &marked);
            tab->setHighlightedBiomes({});
            t.check(t.waitMap(), "map complete");
            t.checkTiles(oracle, dim, seed, top, "highlight cleared");
        }
    }

    // --- 7b. search and analyses in the datapack world ---
    {
        QElapsedTimer searchTimer;
        searchTimer.start();
        testDatapackSearch(t, oracle, dim, report);
        report << QString("search and analysis checks: %1 ms").arg(searchTimer.elapsed());
        t.setLevel(top);
        t.map->setView(0, 0, 4);
        t.setWorld(mc, seed);
        t.check(t.waitMap(), "map complete after the search checks");
    }

    // --- 7c. structures, heights, Nether and End, the pack check (with the
    // structure test pack; the list is the test pack alone again afterwards) ---
    {
        QElapsedTimer structTimer;
        structTimer.start();
        testDatapackStructures(t, vanillaPath, dir + "/testpack-structures", report);
        report << QString("structure, height and dimension checks: %1 ms").arg(structTimer.elapsed());
        t.packs->clearPacks();
        t.packs->addPacks({packPath});
        t.waitLoaded();
        t.setWorld(mc, seed);
        t.setLevel(top);
        t.map->setView(0, 0, 4);
        t.check(mw.datapackMode() && t.packs->entries().size() == 1 && t.waitMap(),
            "the test pack again after the structure checks");
    }

    // --- 8. switch the pack off and on ---
    t.packs->setPackEnabled(0, false);
    t.waitLoaded();
    t.check(!mw.datapackMode() && t.packs->entries().size() == 1, "switched off: no datapack world");
    t.check(t.waitMap(), "map complete");
    {
        QWorld *world = t.map->world;
        t.check(world && !world->wi.dp && world->wi.equals(vanillaWi),
            "switched off: the built-in world and its level are back");
        const auto now = t.tiles();
        int same = 0;
        for (const auto& tile : vanillaTiles)
        {
            auto it = now.find(tile.first);
            same += it != now.end() && it->second == tile.second;
        }
        t.check(same == (int) vanillaTiles.size(), QString("switched off: %1 of %2 tiles are as before")
            .arg(same).arg(vanillaTiles.size()));
        t.check(world && world->sshow[D_SPAWN] && mw.saction[D_SPAWN]->isEnabled()
            && mw.terrainAction->isEnabled() && mw.dimactions[1]->isEnabled()
            && mw.findChild<TabBiomes*>()->isEnabled() && worldType->isEnabled()
            && !mw.packChip->isVisibleTo(&mw), "switched off: everything is available again");
        t.check(levels->itemText(0).contains("surface"), "switched off: the Y picker of the built-in worlds");
    }
    t.packs->setPackEnabled(0, true);
    t.waitLoaded();
    t.check(mw.datapackMode() && t.waitMap(), "switched on again");
    t.check(t.map->world && t.map->world->wi.y == top, "switched on: back at the level of the datapack world");
    t.checkTiles(oracle, dim, seed, top, "switched on again");

    // --- 9. an unsupported version (before 1.18.2 the climate is not data driven) ---
    mw.setVersion(MC_1_17_1);
    t.waitLoaded();
    t.check(t.packs->current() && t.packs->current()->state == DatapackWorld::UnsupportedVersion
        && !t.map->world, "an unsupported version: no map, no built-in world");
    t.check(tab->stateLine().contains("not available") && tab->stateLine().contains("1.21.1"),
        "an unsupported version: the panel names the versions: " + tab->stateLine());
    mw.setVersion(mc);
    t.waitLoaded();
    t.check(t.packs->current() && t.packs->current()->usable() && t.waitMap(),
        "back to a supported version");
    t.checkTiles(oracle, dim, seed, top, "supported version again");

    // --- 10. a pack whose folder disappears ---
    // (scratch files go next to the test's own settings)
    const QString scratch = appConfigDir() + "/datapack-test";
    QDir(scratch).removeRecursively();
    const QString gone = scratch + "/copy/gone-pack";
    t.check(QDir().mkpath(scratch) && copyFolder(packPath, gone), "copy of the pack");
    t.packs->addPacks({gone});
    t.waitLoaded();
    t.check(t.packs->entries().size() == 2 && t.packs->current() && t.packs->current()->usable(),
        "two packs load");
    QDir(gone).removeRecursively();
    t.packs->reload();
    t.waitLoaded();
    t.check(t.packs->entries().size() == 2 && !t.packs->packExists(1) && mw.datapackMode()
        && t.packs->current() && t.packs->current()->usable(),
        "a missing pack stays in the list, the others load");
    t.check(tab->stateLine().contains("not found"), "the panel says that a pack was not found: " + tab->stateLine());
    t.check(t.waitMap(), "map complete");
    t.checkTiles(oracle, dim, seed, top, "with a missing pack");
    {   // the folder comes back: the pack is loaded again, without Reload
        const auto loadedWith = [&](const QString& path) {
            const std::shared_ptr<const DatapackWorld> w = t.packs->current();
            bool found = false;
            if (w && w->world)
                for (const dp::PackInfo& info : w->world->packs())
                    found = found || QFileInfo(info.path) == QFileInfo(path);
            return found;
        };
        t.check(!loadedWith(gone) && copyFolder(packPath, gone), "the missing pack comes back");
        WorldInfo wi;
        mw.getSeed(&wi, false); // any look at the world notices it
        t.waitLoaded();
        t.check(t.packs->packExists(1) && loadedWith(gone) && t.packs->current()->usable(),
            "a pack that comes back is loaded again");
        QDir(gone).removeRecursively();
        mw.getSeed(&wi, false);
        t.waitLoaded();
        t.check(!loadedWith(gone) && t.packs->current() && t.packs->current()->usable(),
            "the pack is gone again");
    }
    t.packs->setPackEnabled(0, false);
    t.waitLoaded();
    t.check(!mw.datapackMode() && t.map->world && !t.map->world->wi.dp,
        "only a missing pack switched on: the built-in world");
    t.packs->setPackEnabled(0, true);
    t.packs->setPackEnabled(1, false);
    t.waitLoaded();

    // --- 11. removal: back to the built-in generator, pixel for pixel ---
    {
        const std::vector<PackEntry> kept = t.packs->entries();
        t.packs->clearPacks();
        t.waitLoaded();
        t.check(!mw.datapackMode() && t.waitMap(), "removed: map complete");
        QWorld *world = t.map->world;
        t.check(world && !world->wi.dp && world->wi.equals(vanillaWi), "removed: the built-in world");
        const auto now = t.tiles();
        int same = 0;
        for (const auto& tile : vanillaTiles)
        {
            auto it = now.find(tile.first);
            same += it != now.end() && it->second == tile.second;
        }
        t.check(same == (int) vanillaTiles.size(), QString("removed: %1 of %2 tiles are as before")
            .arg(same).arg(vanillaTiles.size()));

        // --- 12. the settings: a list written and read again ---
        QSettings ini(scratch + "/packs.ini", QSettings::IniFormat);
        VanillaData otherVanilla;
        DatapackManager first(&otherVanilla), second(&otherVanilla);
        first.addPacks({packPath});
        first.setDimension("testpack:somewhere");
        first.save(ini);
        ini.sync();
        second.load(ini);
        t.check(second.entries().size() == 1 && second.entries() == first.entries()
            && second.dimension() == "testpack:somewhere", "settings round trip of the list");
        // one pack under several names: one entry, named after the pack
        DatapackManager names(&otherVanilla);
        const QString link = scratch + "/link-to-testpack";
        QFile::remove(link);
        t.check(QFile::link(packPath, link), "a link to the pack");
        names.addPacks({packPath + "/", packPath, link, packPath + "/./", link + "/"});
        t.check(names.entries().size() == 1
            && QFileInfo(names.entries()[0].path).fileName() == QFileInfo(packPath).fileName(),
            QString("a pack with a slash at the end or through a link is listed once (%1 entries)")
            .arg(names.entries().size()));
        QFile::remove(link);
        Q_UNUSED(kept);
    }

    // --- 13. optional packs ---
    const auto loadOnly = [&](const QString& name) {
        t.packs->clearPacks();
        t.packs->addPacks({dir + "/" + name});
        t.waitLoaded();
        return t.packs->current();
    };
    if (QFileInfo(dir + "/collide").isDir())
    {   // two biomes with the same name: the map names them with their id
        const std::shared_ptr<const DatapackWorld> world = loadOnly("collide");
        bool found = false;
        if (t.check(world && world->usable(), "the pack with a name collision loads"))
        {
            const std::vector<dp::BiomeInfo>& biomes = world->world->biomes();
            for (int i = 0; i < (int) biomes.size(); i++)
            {
                if (biomes[i].id != "testpack:plains")
                    continue;
                found = true;
                t.check(world->labels[i] == biomes[i].name + " (testpack:plains)",
                    "a biome with a shared name shows its id: " + world->labels[i]);
            }
            int plain = 0;
            for (int i = 0; i < (int) biomes.size(); i++)
                plain += world->labels[i] == biomes[i].name;
            t.check(plain >= (int) biomes.size() - 2, "other biomes keep their plain name");
        }
        t.check(found, "the pack with a name collision has testpack:plains");
    }
    if (QFileInfo(dir + "/nodim").isDir())
    {   // no dimension file: the world type picks the world preset
        std::shared_ptr<const DatapackWorld> world = loadOnly("nodim");
        if (t.check(world && world->usable() && world->presetMatters
                && world->preset == "minecraft:normal", "the pack without a dimension file loads"))
        {
            t.check(worldType->isEnabled(), "the world type is on for a pack without a dimension file");
            dp::Request other = request;
            other.packs = {dp::PackSource{dir + "/nodim", true}};
            for (int type : {WORLD_LARGE_BIOMES, WORLD_AMPLIFIED, WORLD_DEFAULT})
            {
                worldType->setCurrentIndex(type);
                t.waitLoaded();
                world = t.packs->current();
                other.preset = datapackPreset(type);
                const QString what = "world type " + other.preset;
                if (!t.check(world && world->usable() && world->preset == other.preset, what))
                    continue;
                dp::Report presetReport;
                const std::shared_ptr<const dp::World> presetWorld = dp::World::load(other, &presetReport);
                t.map->setView(1500, -2500, 4);
                t.check(t.waitMap(), what + ": map complete");
                t.checkTiles(presetWorld, presetWorld->dimension("minecraft:overworld"), seed,
                    t.map->world ? t.map->world->wi.y : 0, what);
            }
        }
    }
    if (QFileInfo(dir + "/broken").isDir())
    {   // a pack that cannot be loaded: errors, no map, and no built-in world
        const std::shared_ptr<const DatapackWorld> world = loadOnly("broken");
        t.check(world && world->state == DatapackWorld::Problem && !world->report.ok()
            && !t.map->world, "a broken pack: a problem, no map");
        t.check(tab->stateLine().startsWith("Problem") && tab->issuesText().contains("Error"),
            "a broken pack: the panel lists the errors: " + tab->stateLine());
        t.check(tab->detailsText().contains("ERROR") && tab->detailsText().contains("broken"),
            "a broken pack: the details name pack and error");
        t.check(mw.packChip->property("problem").toBool(), "a broken pack: the chip shows a problem");
    }

    // --- 14. the image export renders the datapack world, too ---
    {
        t.packs->clearPacks();
        t.packs->addPacks({packPath});
        t.waitLoaded();
        WorldInfo wi;
        mw.getSeed(&wi, false);
        wi.seed = seed;
        t.check(wi.dp && wi.dp->usable(), "export: the datapack world");
        dp::Sampler sampler(oracle, dim, seed);
        for (int scale : {1, 4, 64})
        {
            if (!wi.dp || !wi.dp->usable())
                break;
            const int w = 96, h = 80;
            Range r = {scale, -40, 25, w, h, wi.y, 1};
            std::vector<uchar> rgb(size_t(3) * w * h);
            std::vector<int> ids(size_t(w) * h);
            std::atomic_bool stop(false);
            const bool rendered = renderExportRange(rgb.data(), wi, DIM_OVERWORLD, seed, r, -1, &stop);
            sampler.genBiomes(ids.data(), scale, r.x, r.z, w, h, wi.y);
            int wrong = 0;
            for (int i = 0; i < w * h; i++)
            {
                const QRgb want = ids[i] >= 0 ? oracle->biomes()[ids[i]].color : qRgb(16, 18, 17);
                wrong += qRgb(rgb[3*i], rgb[3*i+1], rgb[3*i+2]) != want;
            }
            t.check(rendered && wrong == 0, QString("export at 1:%1: %2 pixels differ").arg(scale).arg(wrong));
        }
    }

    // --- 15. packs dropped onto the window ---
    {
        t.packs->clearPacks();
        t.waitLoaded();
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(packPath), QUrl::fromLocalFile(vanillaPath + ".txt")});
        const QPoint at(mw.width() / 2, mw.height() / 2);
        QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&mw, &enter);
        t.check(enter.isAccepted() && mw.dropHighlight && mw.dropHighlight->isVisible(),
            "a dragged pack is accepted and the window shows the drop highlight");
        QMimeData text;
        text.setText("not a file");
        QDragEnterEvent other(at, Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier);
        other.ignore();
        QCoreApplication::sendEvent(&mw, &other);
        t.check(!other.isAccepted(), "dragged text is not accepted");
        // (the drop itself opens a message for the file that is not a pack,
        // so only the pack is dropped here)
        QMimeData one;
        one.setUrls({QUrl::fromLocalFile(packPath)});
        QDropEvent drop(at, Qt::CopyAction, &one, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&mw, &drop);
        t.pump(50);
        t.waitLoaded();
        t.check(!mw.dropHighlight->isVisible() && t.packs->entries().size() == 1
            && mw.datapackMode() && tabs->currentWidget() == tab,
            "a dropped pack is added and the Datapacks tool opens");
    }

    // --- timings: tiles of 128 x 128 per scale, on one thread ---
    {
        dp::Sampler sampler(oracle, dim, seed);
        Generator g;
        setupGenerator(&g, mc, FORCE_OCEAN_VARIANTS);
        applySeed(&g, DIM_OVERWORLD, seed);
        std::vector<int> ids(128 * 128);
        for (int scale : {1, 4, 16, 64, 256, 1024})
        {
            const int count = 12;
            QElapsedTimer time;
            time.start();
            for (int k = 0; k < count; k++)
                sampler.genBiomes(ids.data(), scale, (k - 6) * 128 * 3, (k * 5 - 20) * 128, 128, 128, top);
            const double packMs = time.nsecsElapsed() / 1e6 / count;
            time.start();
            for (int k = 0; k < count; k++)
            {
                Range r = {scale, (k - 6) * 128 * 3, (k * 5 - 20) * 128, 128, 128,
                    scale == 1 ? 256 : 64, 1};
                int *cache = allocCache(&g, r);
                genBiomes(&g, cache, r);
                free(cache);
            }
            const double engineMs = time.nsecsElapsed() / 1e6 / count;
            report << QString("tile 128x128 at 1:%1: test pack %2 ms (%3 us per sample), "
                "built-in engine (vanilla world) %4 ms").arg(scale).arg(packMs, 0, 'f', 2)
                .arg(packMs * 1000 / (128 * 128), 0, 'f', 2).arg(engineMs, 0, 'f', 2);
        }
        report << QString("peak memory: %1 MB before the first pack, %2 MB at the end")
            .arg(memoryBefore, 0, 'f', 0).arg(maxMemoryMb(), 0, 'f', 0);
    }

    // --- the state for the second start (see testDatapackRestore) ---
    t.packs->clearPacks();
    t.packs->addPacks({packPath});
    // an entry whose folder does not exist any more, switched off
    const QString lost = scratch + "/gone-pack";
    QDir().mkpath(lost);
    t.packs->addPacks({lost});
    t.packs->setPackEnabled(1, false);
    QDir(lost).removeRecursively();
    t.waitLoaded();
    t.setLevel(40);
    t.map->setView(0, 0, 4);
    tab->setHighlightedBiomes({"testpack:amber_plains"});
    t.check(t.waitMap() && t.map->world && t.map->world->wi.dp, "the state for the second start");

    t.log << report;
    for (const QString& line : std::as_const(t.log))
        fprintf(stderr, "datapack test: %s\n", qPrintable(line));
    const QByteArray reportPath = qgetenv("SEED_ATLAS_TEST_DATAPACK_REPORT");
    if (!reportPath.isEmpty())
    {
        QFile file(QString::fromLocal8Bit(reportPath));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write((t.log.join("\n") + "\n").toUtf8());
    }
    return t.ok;
}

} // namespace

int main(int argc, char *argv[])
{
    initBiomeColors(g_biomeColors);
    initBiomeTypeColors(g_tempsColors);

    QCoreApplication::setApplicationName(APP_STRING);

    bool version = false;
    bool nogui = false;
    bool clear = false;
    bool reset = false;
    bool usage = false;
    bool mappingSelfTest = false;
    bool mcp = false;
    QString sessionpath;
    QString resultspath;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--version") == 0)
            version = true;
        else if (strcmp(argv[i], "--nogui") == 0)
            nogui = true;
        else if (strcmp(argv[i], "--reset") == 0)
            clear = true;
        else if (strcmp(argv[i], "--reset-all") == 0)
            reset = true;
        else if (strcmp(argv[i], "--mapping-self-test") == 0)
            mappingSelfTest = true;
        else if (strcmp(argv[i], "--mcp") == 0)
            mcp = true;
        else if (strncmp(argv[i], "--session=", 10) == 0)
            sessionpath = argv[i] + 10;
        else if (strncmp(argv[i], "--session", 9) == 0 && i+1 < argc)
            sessionpath = argv[++i];
        else if (strncmp(argv[i], "--out=", 6) == 0)
            resultspath = argv[i] + 6;
        else if (strncmp(argv[i], "--out", 5) == 0 && i+1 < argc)
            resultspath = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            usage = true;
    }

    if (usage)
    {
        const char *msg =
                "Usage: seed-atlas [options]\n"
                "Options:\n"
                "      --help                 Display this help and exit.\n"
                "      --version              Output version information and exit.\n"
                "      --nogui                Run in headless search mode.\n"
                "      --mcp                  Run as an MCP server on stdin/stdout (for AI\n"
                "                             clients; see Settings, MCP).\n"
                "      --reset                Discard results and reset starting seed.\n"
                "      --reset-all            Clear settings and remove all session data.\n"
                "      --session=file         Open this session file.\n"
                "      --out=file             Write matching seeds to this file while searching.\n"
                "\n";
        printf("%s", msg);
        exit(0);
    }
    if (version)
    {
        printf("%s %s\n", APP_STRING, getVersStr().toLocal8Bit().data());
        exit(0);
    }

    // Before anything that could create files or folders: the MCP server
    // only reads the settings.
    if (mcp)
        return runMcpServer(argc, argv);

    if (mappingSelfTest)
    {
        QString reportPath = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_MAPPING_TEST_REPORT"));
        if (reportPath.isEmpty())
            reportPath = QDir::current().absoluteFilePath("mapping-self-test.txt");
        QApplication app(argc, argv);
        QThreadPool::globalInstance()->setMaxThreadCount(configuredThreadCount());
        return runMappingSelfTest(reportPath);
    }

    if (reset)
    {
        QSettings settings(appSettingsId(), appSettingsId());
        settings.clear();

        QString path = appConfigDir();
        QDir dir(path);
        if (dir.exists() && path.contains(APP_STRING))
        {
            dir.removeRecursively();
        }
    }

    // The automatic session (restored at start, saved on quit and by the
    // autosave). A session file named on the command line is opened in its
    // place, but in the GUI it is not overwritten on quit.
    const QString autoSessionPath = appConfigDir() + "/session.save";

    if (nogui)
    {
        if (sessionpath.isEmpty())
            sessionpath = autoSessionPath;
        QCoreApplication app(argc, argv);
        QThreadPool::globalInstance()->setMaxThreadCount(configuredThreadCount());
        Headless headless(sessionpath, resultspath, clear, &app);

        QObject::connect(&headless, &Headless::finished, &app, &QCoreApplication::quit);
        QTimer::singleShot(0, &headless, &Headless::run);

        return app.exec();
    }
    else
    {
        QGuiApplication::setDesktopFileName("org.seedatlas.SeedAtlas");
        QApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, false);

        QApplication app(argc, argv);
        QThreadPool::globalInstance()->setMaxThreadCount(configuredThreadCount());

        // Installed before any windows are constructed so dynamically created
        // dialogs, menus, stacked pages and tabs share the same motion language.
        MotionController motion(&app);

        // Opt-in design check: a contact sheet of the glyphs (see glyphs.h),
        // at the device pixel ratio SEED_ATLAS_UI_GLYPH_SCALE (default 2).
        const QByteArray glyphSheet = qgetenv("SEED_ATLAS_UI_GLYPH_SHEET");
        if (!glyphSheet.isEmpty())
        {
            const qreal scale = qgetenv("SEED_ATLAS_UI_GLYPH_SCALE").toDouble();
            return renderGlyphSheet(QString::fromLocal8Bit(glyphSheet),
                                    scale > 0 ? scale : 2.0) ? 0 : 2;
        }

        // Keep automated first-run checks separate from the user's real,
        // registry-backed settings. QSettings needs the application object to
        // exist before its test-only backend is selected.
        const QByteArray testSettingsDir = qgetenv("SEED_ATLAS_TEST_SETTINGS_DIR");
        if (!testSettingsDir.isEmpty())
        {
            QSettings::setDefaultFormat(QSettings::IniFormat);
            const QString path = QDir::fromNativeSeparators(
                QString::fromLocal8Bit(testSettingsDir));
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path);
            QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, path);
            // The (organization, application) constructor used throughout the
            // app always selects the native backend, and that backend cannot
            // be redirected to a directory on every platform (macOS ignores
            // setPath for it). Route the test instance to its own settings
            // domain instead so the real user settings stay untouched.
            if (qgetenv("SEED_ATLAS_TEST_SETTINGS_ID").isEmpty())
                qputenv("SEED_ATLAS_TEST_SETTINGS_ID", "seed-atlas-uitest");
        }

        MainWindow mw(autoSessionPath, resultspath, sessionpath);
        mw.show();

        // Opt-in visual regression hook. It is intentionally environment-only
        // so normal users never see a testing option in the command-line UI.
        QByteArray snapshotPath = qgetenv("SEED_ATLAS_UI_SNAPSHOT");
        if (!snapshotPath.isEmpty())
        {
            QTimer::singleShot(750, &app, [&app, &mw, snapshotPath]() {
                bool interactionOk = true;
                // Hooks that change the pack list, the choice of game data or
                // files only run with settings of their own: without them they
                // would act on the user's real settings.
                if (qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_DIR"))
                {
                    for (const char *name : {"SEED_ATLAS_UI_TEST_DATAPACK", "SEED_ATLAS_UI_VANILLA",
                            "SEED_ATLAS_UI_DATAPACKS", "SEED_ATLAS_UI_DATAPACK_VANISH",
                            "SEED_ATLAS_TEST_VANILLA_DOWNLOAD", "SEED_ATLAS_UI_DP_DIMENSION",
                            "SEED_ATLAS_UI_TEST_VANILLA_CONSENT", "SEED_ATLAS_UI_DP_TIMING"})
                    {
                        if (!qEnvironmentVariableIsSet(name))
                            continue;
                        fprintf(stderr, "%s needs SEED_ATLAS_TEST_SETTINGS_DIR: it would change "
                            "the real settings or files\n", name);
                        app.exit(2);
                        return;
                    }
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_EMPTY_FIRST_RUN"))
                {
                    QLineEdit *seedEdit = mw.findChild<QLineEdit *>("seedEdit");
                    QComboBox *versionCombo = mw.findChild<QComboBox *>("comboBoxMC");
                    TabBiomes *biomes = mw.findChild<TabBiomes *>();
                    interactionOk = seedEdit && seedEdit->text().isEmpty()
                        && versionCombo && versionCombo->currentIndex() < 0
                        && biomes && biomes->selectedBiomeList().isEmpty();
                    if (interactionOk)
                    {
                        const char *coordinates[] = {"lineX1", "lineZ1", "lineX2", "lineZ2"};
                        for (const char *name : coordinates)
                        {
                            QLineEdit *line = biomes->findChild<QLineEdit *>(name);
                            interactionOk = interactionOk && line && line->text().isEmpty();
                        }
                    }
                    interactionOk = interactionOk && mw.getDim() == 0
                        && mw.saction[D_GRID] && mw.saction[D_GRID]->isChecked()
                        && mw.saction[D_SPAWN] && mw.saction[D_SPAWN]->isChecked();
                    for (int option = 0; interactionOk && option < D_STRUCT_NUM; ++option)
                    {
                        if (option != D_GRID && option != D_SPAWN && mw.saction[option])
                            interactionOk = !mw.saction[option]->isChecked();
                    }
                    // the Seed Finder tab must start clean: no conditions, no
                    // matching seeds and no running search; it is always shown
                    QListWidget *conditions = mw.findChild<QListWidget *>("listConditions");
                    QTableView *results = mw.findChild<QTableView *>("results");
                    QPushButton *startButton = mw.findChild<QPushButton *>("buttonStart");
                    QTabWidget *tabs = mw.findChild<QTabWidget *>("tabContainer");
                    QWidget *searchTab = mw.findChild<QWidget *>("tabSearch");
                    interactionOk = interactionOk
                        && conditions && conditions->count() == 0
                        && results && results->model() && results->model()->rowCount() == 0
                        && startButton && !startButton->isChecked()
                        && tabs && searchTab && tabs->indexOf(searchTab) >= 0
                        && tabs->isTabVisible(tabs->indexOf(searchTab));
                }
                // A world type for design screenshots: the index of the
                // picker (0 Default, 1 Large Biomes, 2 Amplified).
                bool worldTypeOk = false;
                const int testWorldType = qEnvironmentVariableIntValue(
                    "SEED_ATLAS_UI_WORLD_TYPE", &worldTypeOk);
                if (worldTypeOk)
                {
                    if (QComboBox *worldType = mw.findChild<QComboBox *>("comboWorldType"))
                        worldType->setCurrentIndex(testWorldType);
                    else
                        interactionOk = false;
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_WORLD_TYPE"))
                {
                    // Amplified exists from 1.18 on: the picker has to fall
                    // back to the default world for older versions, with and
                    // without a seed, and the surface level has to follow.
                    QComboBox *worldType = mw.findChild<QComboBox *>("comboWorldType");
                    QComboBox *versionCombo = mw.findChild<QComboBox *>("comboBoxMC");
                    QComboBox *levels = mw.findChild<QComboBox *>("comboY");
                    QLineEdit *seedEdit = mw.findChild<QLineEdit *>("seedEdit");
                    interactionOk = interactionOk && worldType && versionCombo && levels
                        && seedEdit && worldType->count() == WORLD_TYPE_NUM;
                    const auto amplifiedEnabled = [&]() {
                        return bool(worldType->model()->flags(worldType->model()->index(
                            WORLD_AMPLIFIED, 0)) & Qt::ItemIsEnabled);
                    };
                    for (int pass = 0; interactionOk && pass < 2; pass++)
                    {
                        WorldInfo wi;
                        mw.getSeed(&wi, false);
                        wi.mc = MC_NEWEST;
                        wi.seed = 1234567;
                        wi.setWorldType(WORLD_DEFAULT);
                        wi.y = surfaceLevel(false);
                        mw.setSeed(wi, DIM_OVERWORLD);
                        if (pass == 1)
                        {   // without a seed, the map is not regenerated
                            seedEdit->clear();
                            versionCombo->setCurrentText(mc2str(MC_NEWEST));
                        }
                        const bool hasSeed = pass == 0;
                        interactionOk = interactionOk && amplifiedEnabled();
                        worldType->setCurrentIndex(WORLD_AMPLIFIED);
                        interactionOk = interactionOk && mw.getSeed(&wi, false) == hasSeed
                            && wi.amplified && !wi.large
                            && wi.genFlags() == uint32_t(AMPLIFIED)
                            && wi.y == surfaceLevel(true) && levels->currentIndex() == 0;
                        if (hasSeed)
                        {   // the generator of the map carries the world type
                            MapView *map = mw.getMapView();
                            interactionOk = interactionOk && map->world
                                && map->world->wi.amplified
                                && (map->world->g.flags & AMPLIFIED)
                                && map->world->g.bn.amplified;
                        }
                        versionCombo->setCurrentText("1.17");
                        interactionOk = interactionOk && mw.getSeed(&wi, false) == hasSeed
                            && wi.mc == MC_1_17 && !wi.amplified && !wi.large
                            && wi.genFlags() == 0 && wi.y == surfaceLevel(false)
                            && worldType->currentIndex() == WORLD_DEFAULT
                            && levels->currentIndex() == 0 && !amplifiedEnabled();
                        versionCombo->setCurrentText(mc2str(MC_NEWEST));
                        interactionOk = interactionOk
                            && worldType->currentIndex() == WORLD_DEFAULT && amplifiedEnabled();
                        mw.getSeed(&wi, false);
                        interactionOk = interactionOk && !wi.amplified;
                    }
                    // The settings: "map/large" alone is what earlier
                    // versions wrote; Amplified has its own key.
                    QTemporaryDir settingsDir;
                    const QString ini = settingsDir.filePath("world.ini");
                    const auto stored = [&ini](int mc, bool large, bool amplified, bool both) {
                        QFile::remove(ini);
                        {
                            QSettings out(ini, QSettings::IniFormat);
                            if (both)
                            {   // as written by this version
                                WorldInfo wi;
                                wi.mc = mc;
                                wi.large = large;
                                wi.amplified = amplified;
                                wi.save(out);
                            }
                            else
                            {   // as written by earlier versions
                                out.setValue("map/mc", mc2str(mc));
                                out.setValue("map/large", large);
                            }
                        }
                        QSettings in(ini, QSettings::IniFormat);
                        WorldInfo wi;
                        wi.amplified = true; // must not survive a file without the key
                        wi.load(in);
                        return wi;
                    };
                    interactionOk = interactionOk && settingsDir.isValid()
                        && stored(MC_NEWEST, false, true, true).worldType() == WORLD_AMPLIFIED
                        && stored(MC_NEWEST, true, false, true).worldType() == WORLD_LARGE_BIOMES
                        && stored(MC_NEWEST, false, false, true).worldType() == WORLD_DEFAULT
                        && stored(MC_NEWEST, true, true, true).worldType() == WORLD_LARGE_BIOMES
                        && stored(MC_1_17, false, true, true).worldType() == WORLD_DEFAULT
                        && stored(MC_NEWEST, true, false, false).worldType() == WORLD_LARGE_BIOMES;
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_DATAPACK"))
                {
                    interactionOk = interactionOk && testDatapacks(mw);
                    // the second start reads what this run leaves behind
                    // (and the second start leaves settings without packs)
                    if (qgetenv("SEED_ATLAS_UI_TEST_DATAPACK") == "restore")
                    {
                        mw.datapacks->clearPacks();
                        mw.vanillaData->forget(MC_1_21_1);
                        mw.getMapView()->setDatapackHighlights({});
                        DatapackTest(mw).waitLoaded();
                    }
                    app.setQuitOnLastWindowClosed(false);
                    mw.grab().save(QString::fromLocal8Bit(snapshotPath));
                    mw.close(); // saves the settings
                    app.exit(interactionOk ? 0 : 2);
                    return;
                }
                // Datapack worlds for design screenshots: a world
                // ("1.21.1:12345": version and seed), the vanilla data of that
                // version, packs (separated like PATH), then a highlight
                // (biome ids, comma separated) and a search text.
                const QByteArray uiWorld = qgetenv("SEED_ATLAS_UI_WORLD");
                if (uiWorld.contains(':'))
                {
                    WorldInfo wi;
                    mw.getSeed(&wi, false);
                    wi.mc = str2mc(uiWorld.left(uiWorld.indexOf(':')).constData());
                    wi.seed = uiWorld.mid(uiWorld.indexOf(':') + 1).toLongLong();
                    if (wi.mc == MC_UNDEF)
                        interactionOk = false;
                    else
                        mw.setSeed(wi, DIM_OVERWORLD);
                }
                const QByteArray uiVanilla = qgetenv("SEED_ATLAS_UI_VANILLA");
                if (!uiVanilla.isEmpty())
                {
                    WorldInfo wi;
                    mw.getSeed(&wi, false);
                    QString error;
                    if (!mw.vanillaData->choose(wi.mc, QString::fromLocal8Bit(uiVanilla), &error))
                    {
                        fprintf(stderr, "vanilla data: %s\n", qPrintable(error));
                        interactionOk = false;
                    }
                }
                const QByteArray uiPacks = qgetenv("SEED_ATLAS_UI_DATAPACKS");
                if (!uiPacks.isEmpty())
                {
                    DatapackTest packs(mw);
                    mw.datapacks->addPacks(QString::fromLocal8Bit(uiPacks)
                        .split(QDir::listSeparator(), Qt::SkipEmptyParts));
                    // "name=off" switches a pack of the list off
                    for (const QByteArray& off : qgetenv("SEED_ATLAS_UI_DATAPACKS_OFF").split(','))
                    {
                        bool isNumber = false;
                        const int index = off.toInt(&isNumber);
                        if (isNumber)
                            mw.datapacks->setPackEnabled(index, false);
                    }
                    // a pack folder that is deleted after it was added
                    const QByteArray vanish = qgetenv("SEED_ATLAS_UI_DATAPACK_VANISH");
                    if (!vanish.isEmpty() && vanish.contains("vanish"))
                    {
                        QDir(QString::fromLocal8Bit(vanish)).removeRecursively();
                        mw.datapacks->reload();
                    }
                    packs.waitLoaded();
                    const QByteArray view = qgetenv("SEED_ATLAS_UI_VIEW"); // "x,z,scale"
                    const QList<QByteArray> parts = view.split(',');
                    if (parts.size() == 3)
                        mw.getMapView()->setView(parts[0].toDouble(), parts[1].toDouble(),
                            parts[2].toDouble());
                    const QByteArray marked = qgetenv("SEED_ATLAS_UI_DATAPACK_HIGHLIGHT");
                    if (!marked.isEmpty())
                        mw.tabDatapacks->setHighlightedBiomes(QString::fromLatin1(marked).split(','));
                    const QByteArray filter = qgetenv("SEED_ATLAS_UI_DATAPACK_FILTER");
                    if (!filter.isEmpty())
                        mw.tabDatapacks->setBiomeFilter(QString::fromLocal8Bit(filter));
                    // another dimension of the datapack world
                    const QByteArray dimension = qgetenv("SEED_ATLAS_UI_DP_DIMENSION");
                    if (!dimension.isEmpty())
                    {
                        mw.datapacks->setDimension(QString::fromLocal8Bit(dimension));
                        packs.waitLoaded();
                        DatapackTest::pump(50);
                        interactionOk &= mw.datapacks->current()
                            && mw.datapacks->current()->dimensionId == QString::fromLocal8Bit(dimension);
                    }
                    // a layer: "height[:display mode]" or "terrain"
                    const QByteArray layer = qgetenv("SEED_ATLAS_UI_LAYER");
                    if (!layer.isEmpty())
                    {
                        const int mode = layer.startsWith("terrain") ? LOPT_TERRAIN : LOPT_HEIGHT;
                        const int disp = layer.contains(':') ? layer.mid(layer.indexOf(':') + 1).toInt() : -1;
                        QMetaObject::invokeMethod(&mw, "onActionBiomeLayerSelect", Qt::DirectConnection,
                            Q_ARG(int, mode), Q_ARG(int, disp));
                    }
                    // structures of the packs on the map: "all", or ids
                    // (comma separated); vanilla ones tick their sidebar entry
                    const QByteArray structures = qgetenv("SEED_ATLAS_UI_DP_STRUCTURES");
                    if (!structures.isEmpty())
                    {
                        mw.openDatapackStructures();
                        if (mw.dpStructDialog)
                        {
                            if (structures == "all")
                                mw.dpStructDialog->showAll(true);
                            else
                                mw.dpStructDialog->setShown(QString::fromLocal8Bit(structures).split(','), true);
                            mw.dpStructDialog->close();
                        }
                        DatapackTest::pump(50);
                    }
                    if (mw.getMapView()->world)
                        packs.waitMap();
                    if (!structures.isEmpty() && mw.getMapView()->world)
                        interactionOk &= packs.waitStructures();
                    // a structure selected on the map: "x,z" in blocks
                    const QByteArray select = qgetenv("SEED_ATLAS_UI_DP_SELECT");
                    if (!select.isEmpty() && mw.getMapView()->world)
                    {
                        QList<QByteArray> xz = select.split(',');
                        MapView *mv = mw.getMapView();
                        if (select == "first")
                        {   // the shown valid start nearest to the middle of the view
                            double best = 1e300;
                            for (Quad *q : mv->world->lvdp.cells)
                            {
                                const std::vector<dp::StructureStart> *list = q->dpstarts;
                                for (size_t k = 0; list && k < list->size(); k++)
                                {
                                    const dp::StructureStart& st = (*list)[k];
                                    if (st.state != dp::StructureStart::Valid
                                            || !mv->world->dpShow[st.structure])
                                        continue;
                                    const double dx = st.x - mv->getX(), dz = st.z - mv->getZ();
                                    if (dx * dx + dz * dz < best)
                                    {
                                        best = dx * dx + dz * dz;
                                        xz = {QByteArray::number(st.x), QByteArray::number(st.z)};
                                    }
                                }
                            }
                        }
                        const qreal px = mv->width() / 2.0 + (xz.value(0).toDouble() - mv->getX()) / mv->getScale();
                        const qreal pz = mv->height() / 2.0 + (xz.value(1).toDouble() - mv->getZ()) / mv->getScale();
                        mv->world->setSelectPos(QPoint(int(px), int(pz)));
                        mv->grab();
                        interactionOk &= mv->world->dpSelected;
                    }
                    // datapack conditions of the Seed Finder: a file with
                    // "#DpCond:" lines, as in a session
                    const QByteArray dpConditions = qgetenv("SEED_ATLAS_UI_DP_CONDITIONS");
                    if (!dpConditions.isEmpty())
                    {
                        QFile file(QString::fromLocal8Bit(dpConditions));
                        std::vector<DpCondition> cv;
                        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
                        {
                            QTextStream stream(&file);
                            while (!stream.atEnd())
                            {
                                DpCondition c;
                                const QString line = stream.readLine();
                                if (line.startsWith("#DpCond:") && DpCondition::fromLine(line, &c))
                                    cv.push_back(c);
                            }
                        }
                        interactionOk &= !cv.empty();
                        mw.formDpCond->setConditions(cv);
                    }
                    // times and checks of the map (see runDatapackTiming())
                    const QByteArray timingOut = qgetenv("SEED_ATLAS_UI_DP_TIMING");
                    if (!timingOut.isEmpty())
                    {
                        interactionOk &= runDatapackTiming(packs, QString::fromLocal8Bit(timingOut));
                        for (const QString& line : packs.log)
                            fprintf(stderr, "%s\n", qPrintable(line));
                    }
                    // a search that runs for this many milliseconds before the
                    // shot ("all seeds in order" from 0, results kept)
                    const int searchMs = qgetenv("SEED_ATLAS_UI_SEARCH_RUN").toInt();
                    if (searchMs > 0)
                    {
                        if (QCheckBox *stop = mw.formControl->findChild<QCheckBox*>("checkStop"))
                            stop->setChecked(false);
                        QPushButton *start = mw.formControl->findChild<QPushButton*>("buttonStart");
                        interactionOk &= start != nullptr;
                        if (start)
                            start->click();
                        DatapackTest::pump(searchMs);
                    }
                }
                // the structure analysis of the visible area, run before the shot
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_STRUCTURES_RUN"))
                {
                    TabStructures *structures = mw.findChild<TabStructures*>();
                    QPushButton *analyze = structures ? structures->findChild<QPushButton*>("pushStart") : nullptr;
                    if (structures && analyze)
                    {
                        if (QTabWidget *tools = mw.findChild<QTabWidget*>("tabContainer"))
                            tools->setCurrentWidget(structures);
                        DatapackTest::pump(100);
                        if (QPushButton *visible = structures->findChild<QPushButton*>("buttonFromVisible"))
                            visible->click();
                        analyze->click();
                        QElapsedTimer limit;
                        limit.start();
                        while (analyze->text() != QLatin1String("Analyze") && limit.elapsed() < 300000)
                            DatapackTest::pump(20);
                        if (StructureResultView *view = structures->findChild<StructureResultView*>("treeStructs"))
                            if (view->resultModel()->rowCount())
                                view->expand(view->resultModel()->index(0, 0));
                    }
                    else
                        interactionOk = false;
                }
                // the biome analysis, run before the shot: "stats", or
                // "locate:<biome id>,..." (the area of the tab)
                const QString analysis = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_UI_BIOMES_RUN"));
                if (!analysis.isEmpty())
                {
                    TabBiomes *biomes = mw.findChild<TabBiomes*>();
                    QTabWidget *sub = biomes ? biomes->findChild<QTabWidget*>("tabWidget") : nullptr;
                    QPushButton *analyze = biomes ? biomes->findChild<QPushButton*>("pushStart") : nullptr;
                    if (sub && analyze)
                    {   // the biomes of the datapack world, the area of the view
                        // (the tab reads its settings when it is shown first)
                        if (QTabWidget *tools = mw.findChild<QTabWidget*>("tabContainer"))
                            tools->setCurrentWidget(biomes);
                        DatapackTest::pump(100);
                        biomes->refresh();
                        if (QPushButton *visible = biomes->findChild<QPushButton*>("buttonFromVisible"))
                            visible->click();
                        if (analysis.startsWith("locate:"))
                        {
                            sub->setCurrentWidget(biomes->findChild<QWidget*>("tabLocate"));
                            CheckableComboBox *pick = biomes->findChild<CheckableComboBox*>("comboBiome");
                            QVector<int> ids;
                            for (const QString& id : analysis.mid(7).split(','))
                                for (int i = 0; pick && i < pick->count(); i++)
                                    if (pick->itemData(i, Qt::ToolTipRole).toString() == id
                                            || pick->itemText(i) == id)
                                        ids.append(pick->itemData(i).toInt());
                            if (pick)
                            {
                                pick->setCheckedData(ids);
                                emit pick->selectionChanged();
                            }
                        }
                        else
                            sub->setCurrentWidget(biomes->findChild<QWidget*>("tabStats"));
                        analyze->click();
                        QElapsedTimer limit;
                        limit.start();
                        while (!analyze->text().startsWith("Analyze") && limit.elapsed() < 120000)
                            DatapackTest::pump(20);
                        DatapackTest::pump(300);
                        if (QTreeWidget *located = biomes->findChild<QTreeWidget*>("treeLocate"))
                            located->expandAll();
                    }
                    else
                        interactionOk = false;
                }
                // The download of the vanilla data, for real: "ok" expects the
                // file to arrive from Mojang's servers and the world to load,
                // "fail" expects a readable error (with
                // SEED_ATLAS_TEST_VANILLA_MANIFEST naming an address that
                // does not answer), "cancel" stops the transfer half way.
                // The consent question is answered by this variable.
                const QByteArray download = qgetenv("SEED_ATLAS_TEST_VANILLA_DOWNLOAD");
                if (!download.isEmpty())
                {
                    VanillaData *vanilla = mw.vanillaData;
                    WorldInfo wi;
                    mw.getSeed(&wi, false);
                    bool finished = false, succeeded = false, cancelled = false;
                    qint64 seen = 0, size = 0;
                    QString message;
                    QElapsedTimer timer;
                    timer.start();
                    QObject context;
                    QObject::connect(vanilla, &VanillaData::infoReady, &context,
                        [&](int, qint64 bytes, const QString& host) {
                            size = bytes;
                            fprintf(stderr, "vanilla download: %lld bytes from %s\n",
                                (long long) bytes, qPrintable(host));
                        });
                    QObject::connect(vanilla, &VanillaData::progress, &context,
                        [&](int, qint64 received, qint64) {
                            seen = received;
                            if (download == "cancel" && received > 0 && !cancelled)
                            {
                                cancelled = true;
                                QTimer::singleShot(0, vanilla, &VanillaData::cancel);
                            }
                        });
                    QObject::connect(vanilla, &VanillaData::finished, &context,
                        [&](int, bool ok, const QString& error) {
                            finished = true;
                            succeeded = ok;
                            message = error;
                        });
                    interactionOk = interactionOk && vanilla->find(wi.mc).kind == VanillaSource::None;
                    mw.tabDatapacks->downloadVanilla();
                    while (!finished && timer.elapsed() < 300000)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    const qint64 ms = timer.elapsed();
                    DatapackTest(mw).waitLoaded();
                    const VanillaSource source = vanilla->find(wi.mc);
                    // (downloads write to "<file>.<pid>-<random>.part")
                    const QStringList parts = QDir(VanillaData::cacheDir())
                        .entryList({QFileInfo(VanillaData::cachedFile(wi.mc)).fileName() + ".*.part"},
                            QDir::Files);
                    fprintf(stderr, "vanilla download (%s): finished %d ok %d after %lld ms, "
                        "%lld of %lld bytes seen, message \"%s\", source kind %d %s\n",
                        download.constData(), finished, succeeded, (long long) ms,
                        (long long) seen, (long long) size, qPrintable(message), source.kind,
                        qPrintable(source.path));
                    if (download == "ok")
                    {
                        const std::shared_ptr<const DatapackWorld> world = mw.datapacks->current();
                        interactionOk = interactionOk && finished && succeeded
                            && source.kind == VanillaSource::Downloaded
                            && QFileInfo(source.path).size() == size && parts.isEmpty()
                            && (!mw.datapackMode() || (world && world->usable()));
                    }
                    else
                    {
                        interactionOk = interactionOk && finished && !succeeded
                            && !message.isEmpty() && source.kind == VanillaSource::None
                            && parts.isEmpty() && !vanilla->busy();
                    }
                }
                // the highlight while a pack is dragged over the window
                QMimeData dragged;
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_DROP_HIGHLIGHT"))
                {
                    dragged.setUrls({QUrl::fromLocalFile(QDir::homePath())});
                    QDragEnterEvent enter(QPoint(mw.width() / 2, mw.height() / 2), Qt::CopyAction,
                        &dragged, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(&mw, &enter);
                }
                const QByteArray testVersion = qgetenv("SEED_ATLAS_UI_VERSION");
                if (!testVersion.isEmpty())
                {
                    if (QLineEdit *seedEdit = mw.findChild<QLineEdit *>("seedEdit"))
                        seedEdit->clear();
                    if (QComboBox *versionCombo = mw.findChild<QComboBox *>("comboBoxMC"))
                        versionCombo->setCurrentText(QString::fromLocal8Bit(testVersion));
                }
                bool tabOk = false;
                int tabIndex = qEnvironmentVariableIntValue("SEED_ATLAS_UI_TAB", &tabOk);
                if (tabOk)
                {
                    if (QTabWidget *tabs = mw.findChild<QTabWidget *>("tabContainer"))
                        tabs->setCurrentIndex(tabIndex);
                }
                QVector<int> biomeIds;
                const QByteArray highlight = qgetenv("SEED_ATLAS_UI_HIGHLIGHT");
                for (const QByteArray& value : highlight.split(','))
                {
                    bool ok = false;
                    int biomeId = value.toInt(&ok);
                    if (ok)
                        biomeIds.append(biomeId);
                }
                if (!biomeIds.isEmpty())
                    mw.getMapView()->setBiomeHighlights(biomeIds);
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_DIMENSIONS"))
                {
                    QAction *overworld = mw.findChild<QAction *>("dimensionAction0");
                    QAction *nether = mw.findChild<QAction *>("dimensionAction1");
                    QAction *end = mw.findChild<QAction *>("dimensionAction2");
                    interactionOk = overworld && nether && end;
                    if (interactionOk)
                    {
                        overworld->setChecked(true);
                        overworld->trigger(); // active action must stay selected
                        interactionOk = overworld->isChecked() && !nether->isChecked() && !end->isChecked();
                        nether->trigger();
                        interactionOk = interactionOk && !overworld->isChecked()
                            && nether->isChecked() && !end->isChecked();
                        nether->trigger(); // the newly active action must also stay selected
                        interactionOk = interactionOk && !overworld->isChecked()
                            && nether->isChecked() && !end->isChecked();
                    }
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_FORTRESSES"))
                {
                    TabStructures *structures = mw.findChild<TabStructures *>();
                    QTabWidget *analysisTabs = structures
                        ? structures->findChild<QTabWidget *>("tabWidget") : nullptr;
                    QWidget *fortressTab = structures
                        ? structures->findChild<QWidget *>("tabFortresses") : nullptr;
                    QAction *overworld = mw.findChild<QAction *>("dimensionAction0");
                    QAction *nether = mw.findChild<QAction *>("dimensionAction1");
                    QAction *end = mw.findChild<QAction *>("dimensionAction2");
                    const int fortressIndex = analysisTabs && fortressTab
                        ? analysisTabs->indexOf(fortressTab) : -1;
                    interactionOk = interactionOk && structures && analysisTabs &&
                        fortressTab && fortressIndex >= 0 && overworld && nether && end;
                    if (interactionOk)
                    {
                        overworld->trigger();
                        interactionOk = !analysisTabs->isTabEnabled(fortressIndex);
                        nether->trigger();
                        interactionOk = interactionOk &&
                            analysisTabs->isTabEnabled(fortressIndex);
                        analysisTabs->setCurrentIndex(fortressIndex);
                        end->trigger();
                        interactionOk = interactionOk &&
                            !analysisTabs->isTabEnabled(fortressIndex) &&
                            analysisTabs->currentWidget() != fortressTab;
                        nether->trigger();
                        analysisTabs->setCurrentIndex(fortressIndex);
                    }
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_MAP_CACHE"))
                {
                    MapView *map = mw.getMapView();
                    map->setView(0, 0, 4);
                    map->animateView(24000, 12000, 4);
                    for (int frame = 0; frame < 8; frame++)
                    {
                        QThread::msleep(20);
                        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                        map->grab();
                    }
                    const std::array<qreal,7> positions = {
                        0, 8000, 16000, 8000, 0, -8000, 0
                    };
                    for (qreal position : positions)
                    {
                        map->setView(position, position / 2, 4);
                        map->grab();
                        if (map->world)
                            map->world->waitForIdle();
                        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                        map->grab();
                    }
                    const auto validIndex = [](const std::vector<Quad*>& cache,
                            const QuadCacheIndex& index) {
                        size_t live = 0;
                        for (Quad *quad : cache)
                            live += quad != nullptr;
                        if (live != index.size())
                            return false;
                        for (const auto& entry : index)
                        {
                            const CachedQuad& cached = entry.second;
                            if (!cached.quad || cached.index >= cache.size() ||
                                    cache[cached.index] != cached.quad)
                                return false;
                            const QuadCacheKey expected = {cached.quad->blocks,
                                cached.quad->sopt, cached.quad->dim,
                                cached.quad->ti, cached.quad->tj};
                            if (!(entry.first == expected))
                                return false;
                        }
                        return true;
                    };
                    interactionOk = interactionOk && map->world &&
                        validIndex(map->world->cachedbiomes,
                            map->world->cachedBiomeIndex) &&
                        validIndex(map->world->cachedstruct,
                            map->world->cachedStructIndex);
                }
                const QByteArray conditionTest = qgetenv("SEED_ATLAS_UI_TEST_CONDITIONS");
                if (!conditionTest.isEmpty())
                {
                    const QString in = QString::fromLocal8Bit(conditionTest);
                    interactionOk &= testConditionEditor(mw, in, in + ".out");
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_AUDIT"))
                {
                    // Regression: changing filters must invalidate matching seeds and export.
                    mw.formControl->searchResultsAdd({1234567}, false);
                    interactionOk &= !mw.formControl->getResults().empty();
                    QMetaObject::invokeMethod(mw.formCond, "changed", Qt::DirectConnection);
                    interactionOk &= mw.formControl->getResults().empty();
                    auto exportButton = mw.formControl->findChild<QPushButton *>("buttonExport");
                    interactionOk &= exportButton && !exportButton->isEnabled();
                    // Regression: short statistics headers and reachable full structure names.
                    auto biomes = mw.findChild<TabBiomes *>();
                    auto table = biomes->findChild<QTableView *>("table");
                    interactionOk &= table && table->horizontalHeader()->sizeHint().height() < 80;
                    auto structures = mw.findChild<TabStructures *>();
                    // the structure results are a QTreeView (StructureResultView)
                    auto tree = structures->findChild<QTreeView *>("treeStructs");
                    interactionOk &= tree && tree->horizontalScrollBarPolicy() == Qt::ScrollBarAsNeeded;
                    // Regression: minimum cannot exceed the square's total chunk count.
                    auto slime = mw.findChild<TabSlime *>();
                    auto spins = slime->findChildren<QSpinBox *>();
                    interactionOk &= spins.size() == 2;
                    if (spins.size() == 2) {
                        spins[0]->setValue(3);
                        interactionOk &= spins[1]->maximum() == 9;
                        spins[1]->setValue(101);
                        interactionOk &= spins[1]->value() == 9;
                    }
                    WorldInfo auditWorld;
                    mw.getSeed(&auditWorld, false);
                    auditWorld.mc = MC_NEWEST;
                    auditWorld.seed = 1234567;
                    mw.setSeed(auditWorld, DIM_OVERWORLD);
                    QTabWidget *auditTabs = mw.findChild<QTabWidget *>("tabContainer");
                    auditTabs->setCurrentWidget(slime);
                    QList<QLineEdit *> coordinates;
                    for (auto line : slime->findChildren<QLineEdit *>())
                        if (!line->parentWidget()->inherits("QAbstractSpinBox"))
                            coordinates.append(line);
                    if (coordinates.size() == 4 && spins.size() == 2) {
                        for (int i=0; i<4; ++i) coordinates[i]->setText(i<2 ? "-10" : "10");
                        spins[1]->setValue(1);
                        QMetaObject::invokeMethod(slime, "onStartClicked", Qt::DirectConnection);
                        QElapsedTimer timeout; timeout.start();
                        auto analysis = slime->findChild<AnalysisSlime *>();
                        while (analysis && analysis->isRunning() && timeout.elapsed() < 5000)
                            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                        auto rows = slime->findChild<ResultTable *>();
                        interactionOk &= rows && rows->rowCount() > 0;
                        auditWorld.seed = 42;
                        mw.setSeed(auditWorld, DIM_OVERWORLD);
                        interactionOk &= rows && rows->rowCount() == 0;
                    } else interactionOk = false;
                    for (const char *name : {"village", "ancient_city", "treasure", "spawn"}) {
                        auto icon = getPix(name, 20);
                        interactionOk &= !icon.isNull() && icon.size() == QSize(20,20);
                    }
                    // every condition type and map layer finds its icon: a
                    // glyph, or a bitmap that is still in the resources
                    QStringList iconNames = {"terrain"};
                    for (int type = 1; type < FILTER_MAX; ++type)
                        iconNames << filterIconName(type);
                    for (int opt = 0; opt < D_STRUCT_NUM; ++opt)
                        if (opt != D_PORTALN)
                            iconNames << mapopt2str(opt);
                    for (const QString& name : iconNames)
                        interactionOk &= name.isEmpty() || hasGlyph(name)
                            || !getPix(name, 20).isNull();
                    QTabWidget *tabs = mw.findChild<QTabWidget *>("tabContainer");
                    for (int i=0; i<tabs->count(); ++i) tabs->setCurrentIndex(i);
                    interactionOk &= mw.findChildren<QWidget *>("seedAtlasPageMotionOverlay").isEmpty();
                }
                // Opt-in layout states for design screenshots: a window size
                // ("1100x700"), the expanded layer sidebar, the collapsed side
                // panel and an open dialog, which is then grabbed instead.
                const auto settle = [](int ms) {
                    QElapsedTimer wait;
                    wait.start();
                    while (wait.elapsed() < ms)
                    {
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                        QThread::msleep(10);
                    }
                };
                const QList<QByteArray> uiSize = qgetenv("SEED_ATLAS_UI_SIZE").split('x');
                if (uiSize.size() == 2 && uiSize[0].toInt() > 0 && uiSize[1].toInt() > 0)
                {
                    mw.showNormal();
                    mw.resize(uiSize[0].toInt(), uiSize[1].toInt());
                    settle(300);
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_LAYERS_EXPANDED"))
                {
                    if (ScrollableToolBar *layers = mw.findChild<ScrollableToolBar *>())
                        layers->setCollapsed(false, false);
                    else
                        interactionOk = false;
                }
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_PANEL_COLLAPSED"))
                {
                    if (QToolButton *toggle = mw.findChild<QToolButton *>("sidePanelToggle"))
                        toggle->click();
                    else
                        interactionOk = false;
                }
                settle(400);
                QWidget *grabTarget = &mw;
                // the open list of a combo box (by object name) is grabbed
                // instead of the window
                const QByteArray popupName = qgetenv("SEED_ATLAS_UI_POPUP");
                if (!popupName.isEmpty())
                {
                    QComboBox *combo = mw.findChild<QComboBox *>(QString::fromLocal8Bit(popupName));
                    if (combo)
                    {
                        combo->showPopup();
                        settle(400);
                        grabTarget = combo->view()->window();
                    }
                    else
                        interactionOk = false;
                }
                // The consent question of the game data download: shown, saved
                // as the snapshot and answered with Cancel. Nothing may have
                // been requested from Mojang by then.
                if (qEnvironmentVariableIsSet("SEED_ATLAS_UI_TEST_VANILLA_CONSENT"))
                {
                    QTimer::singleShot(0, mw.tabDatapacks, &TabDatapacks::downloadVanilla);
                    QTimer *poll = new QTimer(&app);
                    QObject::connect(poll, &QTimer::timeout, &app,
                        [&app, &mw, poll, snapshotPath, interactionOk]() {
                            QMessageBox *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                            if (!box || box->objectName() != "vanillaConsent")
                                return;
                            poll->stop();
                            const bool asked = !mw.vanillaData->busy()
                                && box->informativeText().contains("piston-data.mojang.com");
                            const bool saved = box->grab().save(QString::fromLocal8Bit(snapshotPath));
                            box->reject();
                            QTimer::singleShot(200, &app, [&app, &mw, asked, saved, interactionOk]() {
                                const bool ok = interactionOk && asked && saved && !mw.vanillaData->busy();
                                fprintf(stderr, "vanilla consent: asked first %d, saved %d, idle after "
                                    "cancel %d\n", asked, saved, !mw.vanillaData->busy());
                                app.exit(ok ? 0 : 2);
                            });
                        });
                    poll->start(100);
                    return;
                }
                const QByteArray dialogName = qgetenv("SEED_ATLAS_UI_DIALOG");
                if (!dialogName.isEmpty())
                {
                    // "condition" opens the condition editor, and
                    // "condition:<type>[:option...]" opens it preset to a type
                    // (see ConditionDialog::applyTestPreset)
                    if (dialogName == "dpcheck")
                    {   // the pack check of the Datapacks panel, run to its end;
                        // SEED_ATLAS_UI_DP_CHECK_OUT names a file for the report
                        mw.tabDatapacks->checkPacks();
                        QElapsedTimer limit;
                        limit.start();
                        while (mw.tabDatapacks->checking() && limit.elapsed() < 600000)
                            DatapackTest::pump(50);
                        DatapackTest::pump(100);
                        const QString out = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_UI_DP_CHECK_OUT"));
                        QFile file(out);
                        if (!out.isEmpty() && file.open(QIODevice::WriteOnly | QIODevice::Text))
                            file.write(mw.tabDatapacks->lastCheckReport().toUtf8());
                        interactionOk &= !mw.tabDatapacks->lastCheckReport().isEmpty();
                    }
                    else if (dialogName == "dpcondition" || dialogName.startsWith("dpcondition:"))
                    {   // the datapack condition editor, new or for the
                        // condition with that position in the list
                        const int row = QString::fromLocal8Bit(dialogName).section(':', 1).toInt();
                        const std::vector<DpCondition> cv = mw.formDpCond->getConditions();
                        if (dialogName.contains(':') && row >= 0 && row < (int) cv.size())
                            mw.formDpCond->openEditor(&cv[row]);
                        else
                            mw.formDpCond->openEditor();
                    }
                    else if (dialogName == "condition" || dialogName.startsWith("condition:"))
                    {
                        if (mw.formCond)
                            QMetaObject::invokeMethod(mw.formCond, "openConditionEditor",
                                Qt::DirectConnection);
                        const QString spec = QString::fromLocal8Bit(dialogName).section(':', 1);
                        for (QWidget *top : QApplication::topLevelWidgets())
                        {
                            ConditionDialog *dialog = qobject_cast<ConditionDialog *>(top);
                            if (dialog && dialog->isVisible() && !spec.isEmpty())
                                dialog->applyTestPreset(spec);
                        }
                    }
                    else if (QAction *action = mw.findChild<QAction *>(QString::fromLocal8Bit(dialogName)))
                        action->trigger();
                    settle(500);
                    grabTarget = nullptr;
                    for (QWidget *top : QApplication::topLevelWidgets())
                        if (top != &mw && top->isVisible() && top->inherits("QDialog"))
                            grabTarget = top;
                    if (!grabTarget)
                    {
                        interactionOk = false;
                        grabTarget = &mw;
                    }
                }
                bool saved = grabTarget->grab().save(QString::fromLocal8Bit(snapshotPath));
                app.exit(saved && interactionOk ? 0 : 2);
            });
        }
        return app.exec();
    }
}
