#ifndef EYESEARCHGPU_H
#define EYESEARCHGPU_H

#include "seedatlas-engine/eyesearch.h"
#include "eyesearchgpu_backend.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>


/* Stage (b) of the End portal eye search on the GPU: Metal on macOS, Vulkan
 * on Windows and Linux (see eyesearchgpu.comp for the kernel).
 *
 * The CPU runs stage (a) (candidate start chunks, layouts, plans) and stage
 * (c) (exact confirmation with the biome search) through the split scan of
 * eyesearch.h; the GPU runs the eye tests of all plans of a family over its
 * 65536 seeds. The GPU test is coarse and keeps a superset of the (plan,
 * seed) pairs that reach minEyes; the CPU tests every kept pair exactly
 * (eyeSearchKeep), so the hits are exactly those of scanEyeFamily().
 *
 * The GPU is used where it is faster than the CPU alone (useful()), only if
 * the user allows it (the GPU setting, SEED_ATLAS_GPU=0 switches it off) and
 * only after a self test against the CPU model of the coarse test. The self
 * test is repeated periodically while searches run; if it ever fails, no
 * sweeper uses the GPU again in this process. A failed GPU run (error,
 * timeout, lost device) never loses work: the batch is done on the CPU, the
 * sweeper continues on the CPU and tries the GPU again later, with growing
 * pauses (5 s, 10 s, ... up to 10 minutes). ok() tells which one it uses.
 */
class GpuEyeSweeper
{
public:
    GpuEyeSweeper();
    ~GpuEyeSweeper();
    GpuEyeSweeper(const GpuEyeSweeper&) = delete;
    GpuEyeSweeper& operator=(const GpuEyeSweeper&) = delete;

    // Is the GPU allowed and present, and did it pass the self test? (The
    // first call sets the GPU up and runs the self test, about 0.1 s.)
    static bool available();
    // Does the GPU make this configuration faster (measured, see the .cpp)?
    static bool useful(const EyeSearchConfig& cfg);
    // Workers for a search with up to 'threads' threads that feed the GPU
    // with their own families.
    static int feeders(const EyeSearchConfig& cfg, int threads);

    // Sets up the GPU for one worker; false if that fails.
    bool init();
    // Does this sweeper currently use the GPU (false while it falls back to
    // the CPU after a failure, or after a failed periodic self test)?
    bool ok() const;

    // The contract of scanEyeFamily(); 'es' is the worker's context. GPU
    // failures are handled as described above; the hits never depend on them.
    int scanFamily(EyeSearch *es, uint64_t low48, EyeSearchCallback cb, void *user,
        const volatile int *stop);

    // Stage (b) for the plans of one batch of the current family of 'es'
    // (after eyeSearchBegin), within the seed highs [h0, h1): calls
    // eyeSearchKeep() for every pair the GPU keeps. With 'kept', the raw
    // GPU output (plan << 16 | high, plan relative to 'plans') is also
    // returned. A batch may run as several GPU dispatches. False if the GPU
    // failed; then the first *done plans (nullable) are complete, and the
    // caller does the others on the CPU.
    bool sweep(EyeSearch *es, const EyePlan *plans, int n, uint32_t h0, uint32_t h1,
        std::vector<uint32_t> *kept = nullptr, int *done = nullptr);

    // The coarse GPU test of one (plan, seed) on the CPU, as the kernel does
    // it (for tests): does the kernel keep the pair?
    bool coarseKeeps(EyeSearch *es, const uint64_t *deco, uint64_t low48,
        const EyePlan& plan, uint32_t high);

    // The coarse nibble table (32 x 16 words) for a skip of n outputs.
    const uint32_t *table(EyeSearch *es, uint32_t n);

    // Tuning: seeds per thread in pass 1, list items per thread in pass 2.
    static uint32_t seedsPerLane;
    static uint32_t listItemsPerThread;
    // Seconds between the periodic self tests (0: never).
    static double recheckSeconds;

    // Testing: the n-th GPU dispatch from now on (n >= 1) of this sweeper
    // fails as a GPU error would; 0 switches this off. SEED_ATLAS_GPU_FAIL_AT=n
    // sets it for every sweeper at init().
    void injectFailure(long n) { failCountdown = n; }
    // Testing: the pause before the GPU is tried again after a failure is
    // multiplied by this (0: retry with the next family).
    static double retryScale;
    // GPU failures of this sweeper so far.
    long failures() const { return failCount; }

private:
    struct Columns { uint64_t col[128][2]; };
    const Columns& columns(EyeSearch *es, uint32_t n);
    const uint32_t *byteTable(EyeSearch *es, uint32_t n);
    bool sweepRun(EyeSearch *es, const EyePlan *batch, int n, uint32_t h0, uint32_t h1,
        std::vector<uint32_t> *keptOut, uint32_t planOffset);
    void gpuFailed();
    void tryGpuAgain();

    std::unordered_map<uint32_t, std::vector<uint32_t>> byteCache;
    struct { long batches = 0, runs = 0, retries = 0; double prep = 0, gpu = 0, plans = 0,
        units = 0, nibbleTables = 0; } dbg;   // SEED_ATLAS_GPU_DEBUG
    std::unique_ptr<GpuEyeBackend> backend;
    std::unordered_map<uint32_t, std::vector<uint32_t>> tableCache;
    std::unordered_map<uint32_t, Columns> colCache;
    std::vector<EyePlan> plans;
    std::vector<uint32_t> kept;
    std::vector<uint32_t> batchTables;  // skips with a nibble table
    std::vector<GpuEyePlan> staged;     // the GPU plans in batch order
    std::unordered_map<uint64_t, int> keyCount;
    std::vector<std::pair<uint64_t, uint32_t>> unitOrder;
    std::vector<uint32_t> rangeOrder, unitStarts;
    std::vector<uint32_t> order;        // GPU plan index -> batch index
    bool decoLoaded = false;            // the GPU holds the decoration of decoFamily
    uint64_t decoFamily = 0;
    bool stdTablesLoaded = false;       // the constant tables are in the table buffer
    uint32_t runPlans;                  // plans per GPU dispatch (adapted to its duration)
    uint32_t keptCap;                   // output capacity of a dispatch (grows on overflow)
    long failCountdown = 0;             // injectFailure()
    long failCount = 0;
    int backoff = 0;                    // failures since the GPU last worked for a while
    long goodFamilies = 0;              // families since the last failure
    std::chrono::steady_clock::time_point retryAt;
};

#endif // EYESEARCHGPU_H
