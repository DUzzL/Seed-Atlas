#include "eyesearchdriver.h"
#include "appactivity.h"
#include "eyesearchgpu.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

/* Search workers yield to interactive work (the GUI, map rendering) but use
 * the whole machine when nothing else runs. macOS: QoS "user initiated", one
 * step below the GUI's "user interactive"; lower classes (utility) move work
 * to the efficiency cores and were measured to cost throughput. Windows:
 * below normal priority. Linux: nice +5 for this thread. */
void lowerThreadPriority()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    const pid_t tid = pid_t(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, id_t(tid), getpriority(PRIO_PROCESS, id_t(tid)) + 5);
#endif
}

constexpr uint64_t NONE = ~uint64_t(0);
constexpr uint64_t LOW48 = (uint64_t(1) << 48) - 1;

int floorDiv16(int v)
{
    return v >> 4; // arithmetic shift: floor division for negative values too
}

class CpuEyeSearchWorker : public EyeSearchWorker
{
public:
    explicit CpuEyeSearchWorker(EyeSearch *es) : es(es) {}
    ~CpuEyeSearchWorker() override { freeEyeSearch(es); }
    int scanFamily(uint64_t low48, EyeSearchCallback cb, void *user,
        const volatile int *stop) override
    {
        return scanEyeFamily(es, low48, cb, user, stop);
    }

private:
    EyeSearch *es;
};

class GpuEyeSearchWorker : public EyeSearchWorker
{
public:
    explicit GpuEyeSearchWorker(EyeSearch *es) : es(es) {}
    ~GpuEyeSearchWorker() override { freeEyeSearch(es); }
    bool init() { return sweeper.init(); }
    int scanFamily(uint64_t low48, EyeSearchCallback cb, void *user,
        const volatile int *stop) override
    {
        return sweeper.scanFamily(es, low48, cb, user, stop);
    }
    Engine engine() const override { return sweeper.ok() ? GPU : GPU_FALLBACK; }

private:
    EyeSearch *es;
    GpuEyeSweeper sweeper;
};

} // namespace

EyePortalRecord EyePortalRecord::fromPortal(uint64_t seed, int index, const EndPortal& portal)
{
    EyePortalRecord r;
    std::memset(&r, 0, sizeof(r));
    r.seed = seed;
    r.index = uint8_t(index);
    r.x = portal.center.x;
    r.y = portal.center.y;
    r.z = portal.center.z;
    r.eyes = portal.eyes;
    r.exact = portal.exact;
    r.eyeCount = uint8_t(portal.eyeCount);
    r.exactEyeCount = uint8_t(portal.exactEyeCount);
    r.exactFrames = uint8_t(__builtin_popcount(portal.exact & 0xfff));
    r.lit = portal.lit ? 1 : 0;
    for (int i = 0; i < 12; i++)
    {
        const int dx = std::clamp(portal.frame[i].x - portal.center.x, -2, 2);
        const int dz = std::clamp(portal.frame[i].z - portal.center.z, -2, 2);
        r.cell[i] = uint8_t((dz + 2) * 5 + (dx + 2));
    }

    // The portal blocks are given in portal room coordinates (bit 3*dz+dx at
    // local x = 4+dx, z = 9+dz, the centre being local (5, 10)); rotate them
    // into world orientation like the frames (vanilla getWorldX/Z).
    bool allExact = true, missingExact = false;
    for (int b = 0; b < 9; b++)
    {
        const int lx = b % 3 - 1, lz = b / 3 - 1;
        int wx, wz;
        switch (portal.facing)
        {
        case SH_NORTH: wx = lx;  wz = -lz; break;
        case SH_SOUTH: wx = lx;  wz = lz;  break;
        case SH_WEST:  wx = -lz; wz = lx;  break;
        default:       wx = lz;  wz = lx;  break; // SH_EAST
        }
        const bool placed = (portal.portalBlocks >> b) & 1;
        if (placed)
            r.opening |= uint16_t(1u << (3 * (wz + 1) + (wx + 1)));
        // a portal block has the exactness of the frames in its chunk
        const int cx = floorDiv16(portal.center.x + wx);
        const int cz = floorDiv16(portal.center.z + wz);
        bool exact = false, found = false;
        for (int i = 0; i < 12 && !found; i++)
        {
            if (floorDiv16(portal.frame[i].x) == cx && floorDiv16(portal.frame[i].z) == cz)
            {
                found = true;
                exact = (portal.exact >> i) & 1;
            }
        }
        allExact &= exact;
        missingExact |= exact && !placed;
    }
    // lit needs every block, so one certainly missing block settles "not lit"
    r.litExact = (allExact || (!r.lit && missingExact)) ? 1 : 0;
    return r;
}

QString EyePortalRecord::frameText() const
{
    // the ring of the 5x5 grid without corners, clockwise from the north-west
    static const int ring[12] = { 1, 2, 3, 9, 14, 19, 23, 22, 21, 15, 10, 5 };
    QString text;
    for (int c : ring)
    {
        QChar ch = ' ';
        for (int i = 0; i < 12; i++)
        {
            if (cell[i] != c)
                continue;
            const bool eye = (eyes >> i) & 1;
            const bool sure = (exact >> i) & 1;
            ch = eye ? (sure ? 'E' : 'e') : (sure ? '-' : '?');
        }
        text += ch;
    }
    return text;
}

EyeSearchConfig EyeSearchSettings::config() const
{
    EyeSearchConfig c;
    std::memset(&c, 0, sizeof(c));
    c.mc = mc;
    c.large = large ? 1 : 0;
    c.amplified = amplified ? 1 : 0;
    c.numStrongholds = numStrongholds;
    c.minEyes = minEyes;
    c.exactOnly = exactOnly ? 1 : 0;
    return c;
}

std::unique_ptr<EyeSearchWorker> CpuEyeSearchBackend::createWorker(const EyeSearchConfig& cfg)
{
    EyeSearch *es = createEyeSearch(&cfg);
    if (!es)
        return nullptr;
    return std::make_unique<CpuEyeSearchWorker>(es);
}

std::unique_ptr<EyeSearchWorker> GpuEyeSearchBackend::createWorker(const EyeSearchConfig& cfg)
{
    return CpuEyeSearchBackend(1).createWorker(cfg);
}

std::unique_ptr<EyeSearchWorker> GpuEyeSearchBackend::createWorkerFor(const EyeSearchConfig& cfg,
    size_t slot)
{
    if (int(slot) >= feeders)
        return createWorker(cfg);   // the CPU reserve
    EyeSearch *es = createEyeSearch(&cfg);
    if (!es)
        return nullptr;
    auto w = std::make_unique<GpuEyeSearchWorker>(es);
    if (!w->init())
        return createWorker(cfg); // no GPU for this worker: scan on the CPU
    return w;
}

std::unique_ptr<EyeSearchBackend> GpuEyeSearchBackend::choose(int threads, const EyeSearchConfig& cfg)
{
    if (GpuEyeSweeper::useful(cfg) && GpuEyeSweeper::available())
        return std::make_unique<GpuEyeSearchBackend>(GpuEyeSweeper::feeders(cfg, threads), threads);
    return std::make_unique<CpuEyeSearchBackend>(threads);
}


struct EyeSearchDriver::WorkerContext
{
    bool exactOnly = false;
    bool outOfMemory = false;   // a record could not be kept; the scan was aborted
    size_t budget = 0;      // records this family may keep
    uint64_t count = 0;
    uint64_t byEyes[13] = {};
    std::vector<EyePortalRecord> records;
};

EyeSearchDriver::EyeSearchDriver(QObject *parent)
    : QObject(parent)
{
}

EyeSearchDriver::~EyeSearchDriver()
{
    requestStop();
    joinWorkers();
}

bool EyeSearchDriver::start(std::unique_ptr<EyeSearchBackend> newBackend,
    const EyeSearchSettings& settings, const EyeSearchProgress& resume,
    uint64_t kept, uint64_t limit, QString *message)
{
    if (isRunning())
    {
        if (message)
            *message = tr("A search is already running.");
        return false;
    }
    // validate the configuration once here, so that a failure is reported
    // before any thread starts
    const EyeSearchConfig c = settings.config();
    if (!newBackend || !newBackend->createWorker(c))
    {
        if (message)
            *message = tr("This search configuration is not supported "
                "(End portal eyes need Minecraft 1.18 to 26.3).");
        return false;
    }
    if (resume.exhausted())
    {
        if (message)
            *message = tr("The whole seed space has been searched.");
        return false;
    }

    cfg = settings;
    backend = std::move(newBackend);
    backendLabel = backend->name();
    const size_t workers = size_t(std::max(1, backend->workerCount()));
    reserveFrom = std::clamp(backend->reserveFrom(), 1, int(workers));
    engines.reset(new std::atomic_int[workers]);
    waiting.reset(new std::atomic_int[workers]);
    for (size_t i = 0; i < workers; i++)
    {
        engines[i] = -1;
        waiting[i] = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        state = resume;
        queue.clear();
        keptTotal = kept;
        keepLimit = limit;
        error.clear();
        inFlight.assign(workers, NONE);
        secondsBefore = resume.seconds;
        runTimer.start();
    }
    __atomic_store_n(&stopFlag, 0, __ATOMIC_RELEASE);
    active.store(workers);
    threads.reserve(workers);
    for (size_t i = 0; i < workers; i++)
        threads.emplace_back(&EyeSearchDriver::workerMain, this, i);
    return true;
}

void EyeSearchDriver::requestStop()
{
    setStop();
}

void EyeSearchDriver::setStop()
{
    __atomic_store_n(&stopFlag, 1, __ATOMIC_RELEASE);
    reserveWake.notify_all();
}

// Reserve workers run while no worker uses the GPU (and every GPU worker
// exists, so they do not start in the moment before the GPU is set up).
bool EyeSearchDriver::reserveMayRun() const
{
    for (int i = 0; i < reserveFrom; i++)
    {
        const int e = engines[i];
        if (e < 0 || e == EyeSearchWorker::GPU)
            return false;
    }
    return true;
}

bool EyeSearchDriver::gpuUnavailable() const
{
    if (!isRunning() || backendLabel != QLatin1String("GPU") || !engines)
        return false;
    for (int i = 0; i < reserveFrom; i++)
        if (engines[i] == EyeSearchWorker::GPU || engines[i] < 0)
            return false;
    return true;
}

QString EyeSearchDriver::backendName() const
{
    return gpuUnavailable() ? QStringLiteral("CPU") : backendLabel;
}

int EyeSearchDriver::workerCount() const
{
    int n = int(threads.size());
    for (size_t i = 0; waiting && i < threads.size(); i++)
        n -= waiting[i] != 0;
    return n;
}

void EyeSearchDriver::joinWorkers()
{
    for (std::thread& t : threads)
        if (t.joinable())
            t.join();
    threads.clear();
    backend.reset();
    std::lock_guard<std::mutex> lock(mutex);
    if (runTimer.isValid())
    {
        state.seconds = secondsBefore + runTimer.elapsed() / 1000.0;
        secondsBefore = state.seconds;
        runTimer.invalidate();
    }
}

EyeSearchProgress EyeSearchDriver::progress() const
{
    std::lock_guard<std::mutex> lock(mutex);
    EyeSearchProgress p = state;
    for (uint64_t off : inFlight)
        if (off != NONE)
            p.redo.push_back(off);
    if (runTimer.isValid())
        p.seconds = secondsBefore + runTimer.elapsed() / 1000.0;
    return p;
}

std::vector<EyePortalRecord> EyeSearchDriver::takeHits()
{
    std::vector<EyePortalRecord> out;
    std::lock_guard<std::mutex> lock(mutex);
    out.swap(queue);
    return out;
}

EyeSearchProgress EyeSearchDriver::takeProgressAndHits(std::vector<EyePortalRecord>& hits)
{
    std::lock_guard<std::mutex> lock(mutex);
    hits.clear();
    hits.swap(queue);
    EyeSearchProgress p = state;
    for (uint64_t off : inFlight)
        if (off != NONE)
            p.redo.push_back(off);
    if (runTimer.isValid())
        p.seconds = secondsBefore + runTimer.elapsed() / 1000.0;
    return p;
}

QString EyeSearchDriver::errorMessage() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

int EyeSearchDriver::onHit(void *user, const EyeSearchHit *hit)
{
    WorkerContext *ctx = static_cast<WorkerContext*>(user);
    const int eyes = ctx->exactOnly ? hit->portal.exactEyeCount : hit->portal.eyeCount;
    ctx->count++;
    ctx->byEyes[std::clamp(eyes, 0, 12)]++;
    if (ctx->records.size() < ctx->budget)
    {
        // No exception may leave this callback: it would unwind through the
        // C engine's frames. Abort the scan instead; the family is redone.
        try
        {
            ctx->records.push_back(EyePortalRecord::fromPortal(hit->seed, hit->index, hit->portal));
        }
        catch (...)
        {
            ctx->outOfMemory = true;
            return 1;
        }
    }
    return 0;
}

void EyeSearchDriver::workerMain(size_t slot)
{
    BusyActivity busy; // not to be throttled while out of view
    lowerThreadPriority();
    std::unique_ptr<EyeSearchWorker> worker;
    try
    {
        worker = backend->createWorkerFor(cfg.config(), slot);
    }
    catch (...)
    {
        worker.reset();
    }
    WorkerContext ctx;
    ctx.exactOnly = cfg.exactOnly;
    const bool reserve = int(slot) >= reserveFrom;
    if (worker)
        engines[slot] = worker->engine();
    reserveWake.notify_all();

    while (worker && !stopping())
    {
        uint64_t off;
        {
            std::unique_lock<std::mutex> lock(mutex);
            // a reserve worker waits while the GPU works (polling, so it also
            // sees a GPU worker that falls back in the middle of a family)
            while (reserve && !stopping() && !reserveMayRun())
            {
                waiting[slot] = 1;
                reserveWake.wait_for(lock, std::chrono::milliseconds(500));
            }
            waiting[slot] = 0;
            if (stopping())
                break;
            if (!state.redo.empty())
            {
                off = state.redo.back();
                state.redo.pop_back();
            }
            else if (state.next < EyeSearchProgress::FAMILIES)
                off = state.next++;
            else
                break;
            inFlight[slot] = off;
            const uint64_t room = keepLimit > keptTotal ? keepLimit - keptTotal : 0;
            ctx.budget = size_t(std::min<uint64_t>(room, uint64_t(FAMILY_KEEP_LIMIT)));
        }
        ctx.count = 0;
        std::fill(std::begin(ctx.byEyes), std::end(ctx.byEyes), 0);
        ctx.records.clear();
        ctx.outOfMemory = false;

        int result;
        try
        {
            result = worker->scanFamily((cfg.start + off) & LOW48, &EyeSearchDriver::onHit,
                &ctx, &stopFlag);
        }
        catch (...)
        {   // thrown by a backend's own C++ code (never through the engine:
            // the hit callback catches everything itself)
            result = -2;
        }

        const int engine = worker->engine();
        if (engines[slot].exchange(engine) != engine)
            reserveWake.notify_all();
        std::lock_guard<std::mutex> lock(mutex);
        inFlight[slot] = NONE;
        if (result < 0)
        {   // incomplete: scanned again on the next start
            state.redo.push_back(off);
            if (ctx.outOfMemory)
            {
                error = tr("The search stopped: not enough memory to keep its results. "
                    "Raise the minimum eyes.");
                setStop();
            }
            else if (!stopping())
            {
                error = tr("The search stopped because a family could not be scanned.");
                setStop();
            }
            break;
        }
        state.familiesDone++;
        state.hits += ctx.count;
        for (int i = 0; i <= 12; i++)
            state.hitsByEyes[i] += ctx.byEyes[i];
        const uint64_t room = keepLimit > keptTotal ? keepLimit - keptTotal : 0;
        const size_t take = size_t(std::min<uint64_t>(room, ctx.records.size()));
        queue.insert(queue.end(), ctx.records.begin(), ctx.records.begin() + take);
        keptTotal += take;
        state.dropped += ctx.count - take;
    }

    if (!worker)
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = tr("A search worker could not be created.");
        setStop();
    }
    // a finished GPU worker no longer holds the reserve back (they see that
    // no family is left and finish too)
    engines[slot] = EyeSearchWorker::CPU;
    reserveWake.notify_all();
    // the last worker tells the GUI thread, which joins all of them
    if (active.fetch_sub(1) == 1)
    {
        QMetaObject::invokeMethod(this, [this]() {
            joinWorkers();
            emit finished();
        }, Qt::QueuedConnection);
    }
}
