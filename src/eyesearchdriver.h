#ifndef EYESEARCHDRIVER_H
#define EYESEARCHDRIVER_H

#include "seedatlas-engine/eyesearch.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// One End portal in the compact form the result lists keep (about 48 bytes,
// plain data, so a list of them can be stored as one blob).
struct EyePortalRecord
{
    uint64_t seed;          // world seed
    int32_t x, y, z;        // centre of the 3x3 portal opening
    uint16_t eyes;          // bit i: frame i (engine draw order) has an eye
    uint16_t exact;         // bit i: the state of frame i follows from the seed alone
    uint16_t opening;       // portal blocks, bit 3*(dz+1)+(dx+1) in world orientation
    uint8_t index;          // stronghold index (0-based)
    uint8_t eyeCount;       // eyes, including best-effort frames
    uint8_t exactEyeCount;  // eyes among the exact frames
    uint8_t exactFrames;    // number of exact frames
    uint8_t lit;            // all nine portal blocks are placed
    uint8_t litExact;       // 'lit' does not depend on the terrain
    uint8_t cell[12];       // frame i lies at 5x5 cell (dz+2)*5 + (dx+2) around the centre

    static EyePortalRecord fromPortal(uint64_t seed, int index, const EndPortal& portal);
    // eyes that may be wrong: placed on frames whose state depends on caves
    bool hasUncertainEyes() const { return eyeCount != exactEyeCount; }
    // the 12 frames clockwise from the north-west corner: E/e = eye (certain/
    // uncertain), -/? = empty (certain/uncertain)
    QString frameText() const;
};

// What a search looks for. Two searches with equal settings share their
// position in the family space, so a stopped search can continue.
struct EyeSearchSettings
{
    int mc = 0;
    // the world type, as in WorldInfo (at most one of the two)
    bool large = false;
    bool amplified = false;
    int numStrongholds = 3;
    int minEyes = 12;
    bool exactOnly = true;
    uint64_t start = 0;     // first family (lower 48 bits)

    bool operator==(const EyeSearchSettings& o) const
    {
        return mc == o.mc && large == o.large && amplified == o.amplified
            && numStrongholds == o.numStrongholds
            && minEyes == o.minEyes && exactOnly == o.exactOnly && start == o.start;
    }
    bool operator!=(const EyeSearchSettings& o) const { return !(*this == o); }
    EyeSearchConfig config() const;
};

// The position of a search in the family space and its running totals. The
// families are visited as start, start+1, ... (modulo 2^48); 'next' families
// have been handed out, and those in 'redo' were interrupted and are scanned
// again first. Everything else below 'next' is complete.
struct EyeSearchProgress
{
    uint64_t next = 0;
    std::vector<uint64_t> redo;     // offsets from 'start'
    uint64_t familiesDone = 0;
    uint64_t hits = 0;              // all hits, also those not kept
    uint64_t dropped = 0;           // hits not kept because of the list limit
    uint64_t hitsByEyes[13] = {};   // hits by the counted number of eyes
    double seconds = 0;             // accumulated running time

    static constexpr uint64_t FAMILIES = uint64_t(1) << 48;
    bool exhausted() const { return next >= FAMILIES && redo.empty(); }
};

// Scans single families for one worker thread.
class EyeSearchWorker
{
public:
    enum Engine { CPU, GPU, GPU_FALLBACK };    // GPU_FALLBACK: a GPU worker on the CPU for now
    virtual ~EyeSearchWorker() = default;
    // the contract of scanEyeFamily() in eyesearch.h
    virtual int scanFamily(uint64_t low48, EyeSearchCallback cb, void *user,
        const volatile int *stop) = 0;
    // what the worker scans with at the moment
    virtual Engine engine() const { return CPU; }
};

// Where the families are scanned. The CPU backend runs scanEyeFamily() with
// one EyeSearch context per thread; the GPU backend (eyesearchgpu.h) lets a
// few workers feed the GPU and the others scan on the CPU. The driver owns
// dispatch, hits and progress either way.
class EyeSearchBackend
{
public:
    virtual ~EyeSearchBackend() = default;
    virtual QString name() const = 0;
    virtual int workerCount() const = 0;
    // Workers from this slot on are a reserve: they only scan while no
    // worker uses the GPU (so they replace a failed GPU, but do not slow
    // down the workers that feed it).
    virtual int reserveFrom() const { return workerCount(); }
    // nullptr if the configuration is unsupported (also used to validate it)
    virtual std::unique_ptr<EyeSearchWorker> createWorker(const EyeSearchConfig& cfg) = 0;
    // the worker for thread 'slot' (0 .. workerCount()-1), called on that thread
    virtual std::unique_ptr<EyeSearchWorker> createWorkerFor(const EyeSearchConfig& cfg, size_t slot)
    {
        (void) slot;
        return createWorker(cfg);
    }
};

class CpuEyeSearchBackend : public EyeSearchBackend
{
public:
    explicit CpuEyeSearchBackend(int threads) : threads(threads) {}
    QString name() const override { return QStringLiteral("CPU"); }
    int workerCount() const override { return threads; }
    std::unique_ptr<EyeSearchWorker> createWorker(const EyeSearchConfig& cfg) override;

private:
    int threads;
};

// Stage (b) on the GPU (eyesearchgpu.h): the first 'feeders' workers scan
// their own families with the GPU doing the eye tests; the others up to
// 'threads' are a CPU reserve (see reserveFrom()). A worker whose GPU fails
// scans on the CPU and tries the GPU again later.
class GpuEyeSearchBackend : public EyeSearchBackend
{
public:
    GpuEyeSearchBackend(int feeders, int threads) : feeders(feeders), threads(std::max(feeders, threads)) {}
    QString name() const override { return QStringLiteral("GPU"); }
    int workerCount() const override { return threads; }
    int reserveFrom() const override { return feeders; }
    std::unique_ptr<EyeSearchWorker> createWorker(const EyeSearchConfig& cfg) override;
    std::unique_ptr<EyeSearchWorker> createWorkerFor(const EyeSearchConfig& cfg, size_t slot) override;

    // The backend for a search with up to 'threads' threads: this one if the
    // GPU may be used (the GPU setting), works (self test) and makes the
    // configuration faster; else the CPU backend. The first call sets up and
    // tests the GPU (well below 0.1 s).
    static std::unique_ptr<EyeSearchBackend> choose(int threads, const EyeSearchConfig& cfg);

private:
    int feeders, threads;
};

// Runs a search on the workers of a backend: the workers take the next family
// from a shared position, hits of complete families go into a locked queue
// that the GUI drains with takeHits(), and progress() gives a consistent
// snapshot at any time. Interrupted families are rescanned on the next start,
// so every family is reported exactly once over any number of stops.
class EyeSearchDriver : public QObject
{
    Q_OBJECT
public:
    // Hits a single family may hold back until it is complete. Hits are only
    // published for complete families (an interrupted family is scanned
    // again), so this bounds the memory per worker for very low thresholds.
    static constexpr size_t FAMILY_KEEP_LIMIT = 16384;

    explicit EyeSearchDriver(QObject *parent = nullptr);
    ~EyeSearchDriver() override;    // stops and waits for the workers

    // Starts or continues a search. 'keepLimit' bounds the number of hits
    // that are kept (taken and queued) over the whole search; 'kept' is the
    // number already kept from earlier runs. Returns false (with a message)
    // if the search cannot start.
    bool start(std::unique_ptr<EyeSearchBackend> backend, const EyeSearchSettings& settings,
        const EyeSearchProgress& resume, uint64_t kept, uint64_t keepLimit, QString *error);
    void requestStop();
    bool isRunning() const { return !threads.empty(); }
    bool isStopping() const { return __atomic_load_n(&stopFlag, __ATOMIC_ACQUIRE) != 0; }

    const EyeSearchSettings& settings() const { return cfg; }
    EyeSearchProgress progress() const;     // in-flight families count as redo
    std::vector<EyePortalRecord> takeHits();
    // progress() and takeHits() under one lock: every family counted as done
    // in the result has its hits either in 'hits' or taken earlier. Use this
    // to save a running search, so no hit falls between the two.
    EyeSearchProgress takeProgressAndHits(std::vector<EyePortalRecord>& hits);
    // workers that scan at the moment (reserve workers wait while the GPU works)
    int workerCount() const;
    // "GPU" while at least one worker uses the GPU, "CPU" otherwise
    QString backendName() const;
    // a GPU search whose GPU is unavailable at the moment (it is retried)
    bool gpuUnavailable() const;
    QString errorMessage() const;

signals:
    void finished();

private:
    struct WorkerContext;
    static int onHit(void *user, const EyeSearchHit *hit);
    void workerMain(size_t slot);
    void joinWorkers();
    void setStop();
    bool stopping() const { return __atomic_load_n(&stopFlag, __ATOMIC_ACQUIRE) != 0; }

    bool reserveMayRun() const;

    EyeSearchSettings cfg;
    std::unique_ptr<EyeSearchBackend> backend;
    QString backendLabel;
    std::vector<std::thread> threads;
    int reserveFrom = 0;
    // per worker: EyeSearchWorker::Engine, -1 before it exists; and whether it
    // waits as a reserve
    std::unique_ptr<std::atomic_int[]> engines, waiting;
    std::condition_variable reserveWake;
    /* Polled by the engine through a 'const volatile int *'. All accesses on
     * this side use the compiler's atomic builtins (GCC/Clang, which the
     * engine requires anyway), so they are well-defined among each other. The
     * engine's plain volatile reads of this aligned int are single loads on
     * every supported target (x86-64, arm64, wasm), i.e. relaxed atomic loads
     * in practice; the engine only needs to see the flag eventually. */
    volatile int stopFlag = 0;
    std::atomic_size_t active{0};

    mutable std::mutex mutex;   // guards everything below
    EyeSearchProgress state;
    std::vector<uint64_t> inFlight; // per worker; NONE if idle
    std::vector<EyePortalRecord> queue;
    uint64_t keptTotal = 0;
    uint64_t keepLimit = 0;
    QString error;
    QElapsedTimer runTimer;
    double secondsBefore = 0;
};

#endif // EYESEARCHDRIVER_H
