#include "aboutdialog.h"
#include "headless.h"
#include "mainwindow.h"
#include "mapview.h"
#include "tabbiomes.h"
#include "tablocations.h"
#include "tabslime.h"
#include "tabstructures.h"
#include "world.h"
#include "seedtables.h"

#include "seedatlas-engine/util.h"
#include "seedatlas-engine/quadbase.h"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QTextStream>
#include <QTreeWidgetItem>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <tuple>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

extern "C"
int getStructureConfig_override(int stype, int mc, StructureConfig *sconf)
{
    if unlikely(mc == INT_MAX) // check whether salt overrides are enabled in the engine
        mc = 0;
    int ok = getStructureConfig(stype, mc, sconf);
    if (ok && g_extgen.saltOverride)
    {
        uint64_t salt = g_extgen.salts[stype];
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
    QObject::connect(&analysis, &AnalysisSlime::resultReady, &analysis,
        [&](QTreeWidgetItem *batch) {
            for (int index = 0; index < batch->childCount(); index++)
            {
                const QTreeWidgetItem *item = batch->child(index);
                const int count = item->data(0, Qt::DisplayRole).toInt();
                const int x = item->data(2, Qt::DisplayRole).toInt();
                const int z = item->data(3, Qt::DisplayRole).toInt();
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
            delete batch;
        }, Qt::DirectConnection);

    QElapsedTimer elapsed;
    elapsed.start();
    analysis.start();
    const bool joined = analysis.wait(120000);
    if (!joined)
    {
        analysis.stop.store(true, std::memory_order_relaxed);
        analysis.wait(2000);
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
    setupGenerator(&g, wi.mc, wi.large);
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
                if (group->text(1) != struct2str(Geode))
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

    if (mappingSelfTest)
    {
        QString reportPath = QString::fromLocal8Bit(qgetenv("SEED_ATLAS_MAPPING_TEST_REPORT"));
        if (reportPath.isEmpty())
            reportPath = QDir::current().absoluteFilePath("mapping-self-test.txt");
        QApplication app(argc, argv);
        QThreadPool::globalInstance()->setMaxThreadCount(
            std::max(1, QThread::idealThreadCount()));
        return runMappingSelfTest(reportPath);
    }

    if (reset)
    {
        QSettings settings(appSettingsId(), appSettingsId());
        settings.clear();

        QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir dir(path);
        if (dir.exists() && path.contains(APP_STRING))
        {
            dir.removeRecursively();
        }
    }

    if (sessionpath.isEmpty())
    {
        QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir dir(path);
        if (!dir.exists())
            dir.mkpath(".");
        sessionpath = path + "/session.save";
    }

    if (nogui)
    {
        QCoreApplication app(argc, argv);
        QThreadPool::globalInstance()->setMaxThreadCount(
            std::max(1, QThread::idealThreadCount()));
        Headless headless(sessionpath, resultspath, clear, &app);

        QObject::connect(&headless, SIGNAL(finished()), &app, SLOT(quit()));
        QTimer::singleShot(0, &headless, SLOT(run()));

        return app.exec();
    }
    else
    {
        QGuiApplication::setDesktopFileName("org.seedatlas.SeedAtlas");
        QApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, false);

        QApplication app(argc, argv);
        QThreadPool::globalInstance()->setMaxThreadCount(
            std::max(1, QThread::idealThreadCount()));

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
        }

        MainWindow mw(sessionpath, resultspath);
        mw.show();

        // Opt-in visual regression hook. It is intentionally environment-only
        // so normal users never see a testing option in the command-line UI.
        QByteArray snapshotPath = qgetenv("SEED_ATLAS_UI_SNAPSHOT");
        if (!snapshotPath.isEmpty())
        {
            QTimer::singleShot(750, &app, [&app, &mw, snapshotPath]() {
                bool interactionOk = true;
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
                bool saved = mw.grab().save(QString::fromLocal8Bit(snapshotPath));
                app.exit(saved && interactionOk ? 0 : 2);
            });
        }
        return app.exec();
    }
}
