#include "eyesearchgpu.h"
#include "eyesearchgpu_backend.h"
#include "gpufilter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

// Plans per batch from the engine (eyeSearchNextPlans).
const int MAX_PLANS = 4096;
// Plans per GPU dispatch: at most this many (26.3 with 3 strongholds has
// about 300 per family, so a family stays one dispatch), fewer while
// dispatches take long. This bounds the list of pass 2 (about 30 MB) and the
// time of one dispatch (GPU watchdogs). Measured for 1.18 with 8 strongholds
// (4096 plans per batch, 6 workers): 512 slightly slower than unbounded
// dispatches, 1024 as fast; peak memory 1.40 GB unbounded, 0.30 GB with 1024.
const uint32_t RUN_PLANS_MAX = 1024;
const uint32_t RUN_PLANS_MIN = 8;
const double RUN_SLOW = 0.25, RUN_FAST = 0.05;  // seconds per dispatch
const uint32_t NSEEDS = 0x10000;

bool debugEnabled()
{
    return getenv("SEED_ATLAS_GPU_DEBUG") != nullptr;
}

// The coarse test: certainly no eye if the coarse sum + 2 <= FAIL_MAX, the
// last 10-bit value v with v * 2^14 + 2^14 - 1 <= PORTAL_EYE_THRESHOLD.
const uint32_t FAIL_MAX = (PORTAL_EYE_THRESHOLD + 1) / 16384 - 1;
static_assert(FAIL_MAX == 920, "eye threshold");

// The bits of a state that the coarse test uses, linear over GF(2):
// lo[46:37] at 0..9, hi[46:37] at 10..19, lo[63:54] at 20..29.
inline uint32_t coarsePack(uint64_t lo, uint64_t hi)
{
    return uint32_t((lo >> 37) & 1023) | uint32_t(((hi >> 37) & 1023) << 10) |
        uint32_t((lo >> 54) << 20);
}

void coarseTable(uint32_t *t, const uint64_t col[128][2])
{
    for (int r = 0; r < 32; r++)
    {
        uint32_t pc[4];
        for (int b = 0; b < 4; b++)
            pc[b] = coarsePack(col[4 * r + b][0], col[4 * r + b][1]);
        uint32_t *row = t + 16 * r;
        row[0] = 0;
        for (int v = 1; v < 16; v++)
            row[v] = row[v & (v - 1)] ^ pc[__builtin_ctz(v)];
    }
}

inline uint64_t mix64(uint64_t v)
{
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

// One coarse test as in the kernel: does the draw certainly give no eye?
inline bool coarseMiss(const uint32_t *t, uint64_t v)
{
    uint64_t l = v ^ 0x6a09e667f3bcc909ULL;
    uint64_t lo = mix64(l), hi = mix64(l + 0x9e3779b97f4a7c15ULL);
    uint32_t p = 0;
    for (int r = 0; r < 16; r++)
        p ^= t[16 * r + ((lo >> (4 * r)) & 15)];
    for (int r = 0; r < 16; r++)
        p ^= t[16 * (16 + r) + ((hi >> (4 * r)) & 15)];
    uint32_t top = ((p & 1023) + ((p >> 10) & 1023) + ((p >> 20) & 1023)) & 1023;
    return top + 2 <= FAIL_MAX;
}

std::mutex g_mutex;
int g_available = -1;   // -1: not tried yet
// A periodic self test failed: the GPU computes wrong results, so no sweeper
// uses it again in this process.
std::atomic<bool> g_broken(false);
std::mutex g_checkMutex;    // one periodic self test at a time
std::chrono::steady_clock::time_point g_lastCheck = std::chrono::steady_clock::now();

// Plans for the self test with high pass rates, so that a wrong GPU shows:
// shared and other skips, with and without allowed misses.
std::vector<EyePlan> selfTestPlans()
{
    std::vector<EyePlan> plans;
    uint64_t r = 0x5EED;
    auto rnd = [&r]() {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        return uint32_t(r >> 33);
    };
    const int shapes[][2] = { {1, 0}, {2, 1}, {3, 1}, {4, 2}, {12, 10}, {6, 4}, {2, 0}, {5, 3} };
    for (int i = 0; i < 32; i++)
    {
        EyePlan p;
        std::memset(&p, 0, sizeof(p));
        p.group = 0;
        p.ntests = shapes[i % 8][0];
        p.k = shapes[i % 8][1];
        for (int j = 0; j < p.ntests; j++)
        {
            p.test[j].mx = int32_t(rnd() % 4001) * 16 - 32000;
            p.test[j].mz = int32_t(rnd() % 4001) * 16 - 32000;
            // mostly the shared skips, sometimes another one
            p.test[j].n = (rnd() % 4) ? 760 + rnd() % 12 : 772 + rnd() % 3000;
        }
        plans.push_back(p);
    }
    // plans that share their first test (units of several plans in pass 1)
    for (int i = 0; i < 8; i++)
    {
        EyePlan p = plans[8 * (i % 2)]; // a plan with one shared test
        p.ntests = 1 + i % 3;
        p.k = 0;
        for (int j = 1; j < p.ntests; j++)
        {
            p.test[j].mx = int32_t(rnd() % 4001) * 16 - 32000;
            p.test[j].mz = int32_t(rnd() % 4001) * 16 - 32000;
            p.test[j].n = (j == 2) ? 772 + rnd() % 3000 : 760 + rnd() % 12;
        }
        plans.push_back(p);
    }
    return plans;
}

// Does the GPU keep exactly the pairs that the coarse CPU model keeps, and
// thus every pair that the exact test keeps? Run once, before the first use.
bool selfTest()
{
    auto t0 = std::chrono::steady_clock::now();
    // a default world: the test is about the kernel, not the world type
    EyeSearchConfig cfg = { MC_1_21_1, 0, 1, 12, 1, 0 };
    EyeSearch *es = createEyeSearch(&cfg);
    if (!es)
        return false;
    GpuEyeSweeper sw;
    bool ok = sw.init();
    long pairs = 0, exactKept = 0, gpuKept = 0;
    if (ok)
    {
        const uint64_t low48 = 0x00007a3b1c2d3e4fULL;
        eyeSearchBegin(es, low48);
        std::vector<uint64_t> deco(2 * NSEEDS);
        eyeSearchGetDecoration(es, deco.data());
        std::vector<EyePlan> plans = selfTestPlans();
        const uint32_t h0 = 1000, h1 = 1000 + 2048;
        std::vector<uint32_t> kept;
        ok = sw.sweep(es, plans.data(), int(plans.size()), h0, h1, &kept);
        std::sort(kept.begin(), kept.end());
        gpuKept = long(kept.size());
        for (size_t i = 0; ok && i < plans.size(); i++)
        {
            for (uint32_t h = h0; h < h1 && ok; h++)
            {
                const bool gpu = std::binary_search(kept.begin(), kept.end(), uint32_t(i << 16 | h));
                const bool model = sw.coarseKeeps(es, deco.data(), low48, plans[i], h);
                const bool exact = eyeSearchKeep(es, &plans[i], int(h)) == 1;
                pairs++;
                exactKept += exact;
                if (gpu != model || (exact && !gpu))
                {
                    ok = false;
                    if (debugEnabled())
                        fprintf(stderr, "gpu eye search self test: plan %zu (%d tests, k %d) seed high %u: "
                            "gpu %d, cpu model %d, exact %d\n", i, plans[i].ntests, plans[i].k, h,
                            gpu, model, exact);
                }
            }
        }
        // a meaningful test: the exact test keeps many pairs
        ok = ok && exactKept > 1000;
    }
    freeEyeSearch(es);
    if (debugEnabled())
        fprintf(stderr, "gpu eye search self test: %s (%ld pairs, %ld kept exactly, %ld by the GPU, %.1f ms)\n",
            ok ? "passed" : "FAILED", pairs, exactKept, gpuKept,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return ok;
}


double secondsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

uint32_t GpuEyeSweeper::seedsPerLane = 64;
uint32_t GpuEyeSweeper::listItemsPerThread = 8;
double GpuEyeSweeper::recheckSeconds = 1800;
double GpuEyeSweeper::retryScale = 1;

GpuEyeSweeper::GpuEyeSweeper() : runPlans(RUN_PLANS_MAX), keptCap(65536) {}
GpuEyeSweeper::~GpuEyeSweeper() {}

bool GpuEyeSweeper::available()
{
    const char *env = getenv("SEED_ATLAS_GPU");
    if (env && strcmp(env, "0") == 0)
        return false;
    if (!GpuClimateFilter::allowed() && !GpuClimateFilter::forced())
        return false;
    if (g_broken)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_available < 0)
        g_available = gpuEyeBackendAvailable() && (GpuClimateFilter::forced() || selfTest());
    return g_available == 1;
}

/* Measured on an Apple M6 (12 CPU cores) against scanEyeFamily() on all 12
 * threads (tools/eyesearch-gpu-check bench; seeds/s, CPU -> GPU path):
 *   26.3, 3 strongholds, exactOnly 1, minEyes 12: 6.0-6.5 M -> 46-49 M
 *                                      minEyes 10: 1.6 M -> 11.5 M
 *                                      minEyes  9: 0.97 M -> 8.7 M
 *                                      minEyes  8: 0.56 M -> 2.3 M
 *                                      minEyes  7: 130 k -> 160 k
 *                                      minEyes  6: 17.8 k -> 18.4 k (noise)
 *   26.3, 3 strongholds, exactOnly 0, minEyes 12: 2.3 M -> 20 M,
 *                                      minEyes  7: 46 k -> 81 k
 *   1.19.2 / 1.18, 3 strongholds, minEyes 12: about 9x; 1.19.2 minEyes 8: 2x
 * Below 7 the exact confirmation (biome searches) dominates. */
bool GpuEyeSweeper::useful(const EyeSearchConfig& cfg)
{
    if (GpuClimateFilter::forced())
        return true;
    return cfg.minEyes >= 7;
}

/* With the GPU, every worker feeds it. From minEyes 9 on the GPU is the limit
 * and 5 feeders keep it busy (26.3, 3 strongholds, minEyes 12, measured
 * twice: 4 feeders 34-39 M seeds/s, 5: 47 M, 6: 47-48.5 M, 8: 48 M, 12:
 * 47-48 M); 6 leave room for slower CPUs. At lower thresholds the
 * confirmation on the CPU needs all threads. The driver keeps the other
 * threads in reserve and runs them on the CPU while the GPU is unavailable. */
int GpuEyeSweeper::feeders(const EyeSearchConfig& cfg, int threads)
{
    return cfg.minEyes >= 9 ? std::min(threads, 6) : threads;
}

bool GpuEyeSweeper::init()
{
    backend.reset(gpuEyeBackendCreate());
    if (!backend || !backend->init())
    {
        backend.reset();
        return false;
    }
    plans.resize(MAX_PLANS);
    decoLoaded = false;
    stdTablesLoaded = false;
    if (const char *env = getenv("SEED_ATLAS_GPU_FAIL_AT"))
        failCountdown = atol(env);
    return true;
}

bool GpuEyeSweeper::ok() const
{
    return backend != nullptr && !g_broken;
}

const GpuEyeSweeper::Columns& GpuEyeSweeper::columns(EyeSearch *es, uint32_t n)
{
    auto it = colCache.find(n);
    if (it != colCache.end())
        return it->second;
    // the columns of M^n: a few steps on from a cached skip (the draws of a
    // chunk follow each other), else a jump
    Columns c;
    bool stepped = false;
    for (uint32_t d = 1; d <= 12 && d <= n && !stepped; d++)
    {
        auto ci = colCache.find(n - d);
        if (ci == colCache.end())
            continue;
        c = ci->second;
        for (uint32_t s = 0; s < d; s++)
        {
            for (int j = 0; j < 128; j++)
            {
                Xoroshiro xr = { c.col[j][0], c.col[j][1] };
                xNextLong(&xr);
                c.col[j][0] = xr.lo;
                c.col[j][1] = xr.hi;
            }
        }
        stepped = true;
    }
    if (!stepped)
        eyeSearchJumpColumns(es, n, c.col);
    if (colCache.size() >= 4096)
        colCache.clear();   // 8 MB
    return colCache[n] = c;
}

const uint32_t *GpuEyeSweeper::table(EyeSearch *es, uint32_t n)
{
    auto it = tableCache.find(n);
    if (it != tableCache.end())
        return it->second.data();
    if (tableCache.size() >= 8192)
        tableCache.clear(); // 16 MB
    std::vector<uint32_t>& t = tableCache[n];
    t.resize(GPU_EYE_NIBBLE_WORDS);
    coarseTable(t.data(), columns(es, n).col);
    return t.data();
}

const uint32_t *GpuEyeSweeper::byteTable(EyeSearch *es, uint32_t n)
{
    auto it = byteCache.find(n);
    if (it != byteCache.end())
        return it->second.data();
    if (byteCache.size() >= 1024)
        byteCache.clear();  // 16 MB
    std::vector<uint32_t>& t = byteCache[n];
    t.resize(GPU_EYE_BYTE_WORDS);
    const Columns& c = columns(es, n);
    for (int r = 0; r < 16; r++)
    {
        uint32_t pc[8];
        for (int b = 0; b < 8; b++)
            pc[b] = coarsePack(c.col[8 * r + b][0], c.col[8 * r + b][1]);
        uint32_t *row = t.data() + 256 * r;
        row[0] = 0;
        for (int v = 1; v < 256; v++)
            row[v] = row[v & (v - 1)] ^ pc[__builtin_ctz(v)];
    }
    return t.data();
}

bool GpuEyeSweeper::coarseKeeps(EyeSearch *es, const uint64_t *deco, uint64_t low48,
    const EyePlan& plan, uint32_t high)
{
    const uint64_t salt = eyeSearchSalt(es);
    const uint64_t seed = (uint64_t(high) << 48) | (low48 & MASK48);
    int miss = 0;
    for (int i = 0; i < plan.ntests; i++)
    {
        const EyePlanTest& t = plan.test[i];
        uint64_t v = ((uint64_t(int64_t(t.mx)) * deco[2 * high] +
            uint64_t(int64_t(t.mz)) * deco[2 * high + 1]) ^ seed) + salt;
        miss += coarseMiss(table(es, t.n), v);
    }
    return miss <= plan.k;
}

bool GpuEyeSweeper::sweep(EyeSearch *es, const EyePlan *batch, int n, uint32_t h0, uint32_t h1,
    std::vector<uint32_t> *keptOut, int *done)
{
    if (done)
        *done = 0;
    if (!ok() || n <= 0 || n > MAX_PLANS || h0 >= h1 || h1 > NSEEDS)
        return false;
    dbg.batches++;
    for (int off = 0; off < n; )
    {
        // a dispatch keeps its pairs only once it succeeded completely
        const int m = std::min(n - off, int(runPlans));
        if (!sweepRun(es, batch + off, m, h0, h1, keptOut, uint32_t(off)))
            return false;
        off += m;
        if (done)
            *done = off;
    }
    return true;
}

// One GPU dispatch for the plans batch[0 .. n-1].
bool GpuEyeSweeper::sweepRun(EyeSearch *es, const EyePlan *batch, int n, uint32_t h0,
    uint32_t h1, std::vector<uint32_t> *keptOut, uint32_t planOffset)
{
    const auto tPrep = std::chrono::steady_clock::now();
    // the decoration multipliers of the family (the same for every batch)
    const uint64_t low48 = eyeSearchFamily(es);
    if (!decoLoaded || decoFamily != low48)
    {
        uint64_t *deco = backend->deco();
        if (!deco)
            return false;
        eyeSearchGetDecoration(es, deco);
        decoLoaded = true;
        decoFamily = low48;
    }

    // the plans, with the skip in place of the table for now
    staged.resize(size_t(n));
    for (int i = 0; i < n; i++)
    {
        const EyePlan& p = batch[i];
        GpuEyePlan& g = staged[i];
        std::memset(&g, 0, sizeof(g));
        g.ntests = uint32_t(p.ntests);
        g.k = uint32_t(p.k);
        for (int j = 0; j < p.ntests; j++)
        {
            g.test[j].mx = p.test[j].mx;
            g.test[j].mz = p.test[j].mz;
            g.test[j].table = p.test[j].n;
        }
    }

    // Plans without allowed misses may run their tests in any order. Those
    // with a test of a shared skip start with the one that most of them
    // share, and plans with the same first test form a unit: pass 1 runs
    // that test once for the unit, with its byte table in shared memory (17%
    // fewer first tests for exactOnly, more without, in 26.3). The other
    // plans run the whole cascade in pass 1. (Units with other first tests,
    // with byte tables built for them, were measured to be slower: 15 instead
    // of 19 M seeds/s for 26.3, 3 strongholds, exactOnly 0.)
    auto isStd = [](uint32_t skip) {
        return skip >= PORTAL_ROOM_CALLS_BEFORE_EYES &&
            skip < PORTAL_ROOM_CALLS_BEFORE_EYES + GPU_EYE_STD_TABLES;
    };
    auto key = [](const GpuEyeTest& t) {
        return (uint64_t(uint32_t(t.mx >> 4)) & 0x3fffff) |
            ((uint64_t(uint32_t(t.mz >> 4)) & 0x3fffff) << 22) | (uint64_t(t.table) << 44);
    };
    keyCount.clear();
    for (const GpuEyePlan& g : staged)
        if (g.k == 0)
            for (uint32_t j = 0; j < g.ntests; j++)
                if (isStd(g.test[j].table))
                    keyCount[key(g.test[j])]++;
    unitOrder.clear();      // (first test, plan) of the plans in units
    rangeOrder.clear();     // the other plans
    for (int i = 0; i < n; i++)
    {
        GpuEyePlan& g = staged[i];
        int best = -1, bestCount = 0;
        for (uint32_t j = 0; g.k == 0 && j < g.ntests; j++)
        {
            if (!isStd(g.test[j].table))
                continue;
            const int c = keyCount[key(g.test[j])];
            if (c > bestCount)
            {
                bestCount = c;
                best = int(j);
            }
        }
        if (best < 0)
        {
            rangeOrder.push_back(uint32_t(i));
            continue;
        }
        std::swap(g.test[0], g.test[best]);
        unitOrder.emplace_back(key(g.test[0]), uint32_t(i));
    }
    std::sort(unitOrder.begin(), unitOrder.end());

    // The table buffer (see eyesearchgpu.comp): byte tables of the shared
    // skips, nibble tables of the shared skips (both constant, written once
    // per backend), then nibble tables of the other skips of this dispatch.
    const uint32_t nbyte = GPU_EYE_STD_TABLES;
    const uint32_t nibbleBase = nbyte * GPU_EYE_BYTE_WORDS;
    batchTables.clear();    // the other skips
    for (GpuEyePlan& g : staged)
    {
        for (uint32_t j = 0; j < g.ntests; j++)
        {
            const uint32_t skip = g.test[j].table;
            if (isStd(skip))
            {
                g.test[j].table = skip - PORTAL_ROOM_CALLS_BEFORE_EYES;
                continue;
            }
            auto it = std::find(batchTables.begin(), batchTables.end(), skip);
            g.test[j].table = nbyte + uint32_t(it - batchTables.begin());
            if (it == batchTables.end())
                batchTables.push_back(skip);
        }
    }
    const uint32_t words = nibbleBase + (nbyte + uint32_t(batchTables.size())) * GPU_EYE_NIBBLE_WORDS;
    uint32_t *tb = backend->tables(words);
    if (!tb)
        return false;
    if (!stdTablesLoaded)
    {
        for (uint32_t i = 0; i < nbyte; i++)
        {
            std::memcpy(tb + size_t(i) * GPU_EYE_BYTE_WORDS,
                byteTable(es, PORTAL_ROOM_CALLS_BEFORE_EYES + i), GPU_EYE_BYTE_WORDS * sizeof(uint32_t));
            std::memcpy(tb + nibbleBase + size_t(i) * GPU_EYE_NIBBLE_WORDS,
                table(es, PORTAL_ROOM_CALLS_BEFORE_EYES + i), GPU_EYE_NIBBLE_WORDS * sizeof(uint32_t));
        }
        stdTablesLoaded = true;
    }
    for (size_t i = 0; i < batchTables.size(); i++)
        std::memcpy(tb + nibbleBase + (nbyte + i) * GPU_EYE_NIBBLE_WORDS,
            table(es, batchTables[i]), GPU_EYE_NIBBLE_WORDS * sizeof(uint32_t));

    GpuEyePlan *gp = backend->plans(uint32_t(n));
    if (!gp)
        return false;
    order.clear();
    std::vector<uint32_t>& units = unitStarts;
    units.clear();
    for (size_t i = 0; i < unitOrder.size(); i++)
    {
        if (i == 0 || unitOrder[i].first != unitOrder[i - 1].first)
            units.push_back(uint32_t(i));
        order.push_back(unitOrder[i].second);
    }
    for (uint32_t i : rangeOrder)
        order.push_back(i);
    for (size_t i = 0; i < order.size(); i++)
        gp[i] = staged[order[i]];
    for (size_t u = 0; u < units.size(); u++)
    {
        const uint32_t start = units[u];
        const uint32_t end = u + 1 < units.size() ? units[u + 1] : uint32_t(unitOrder.size());
        gp[u].unit = start | (end - start) << 16;
    }
    const uint64_t firstPairs = uint64_t(unitOrder.size()) * (h1 - h0);

    GpuEyeParams prm;
    std::memset(&prm, 0, sizeof(prm));
    prm.low48 = low48;
    prm.salt = eyeSearchSalt(es);
    prm.h0 = h0;
    prm.h1 = h1;
    prm.nplans = uint32_t(n);
    prm.maxOut = keptCap;
    prm.seedsPerGroup = GPU_EYE_THREADS * seedsPerLane;
    prm.groupsPerPlan = (h1 - h0 + prm.seedsPerGroup - 1) / prm.seedsPerGroup;
    prm.failMax = FAIL_MAX;
    prm.nunits = uint32_t(units.size());
    prm.firstRange = uint32_t(unitOrder.size());
    prm.nbyte = nbyte;
    // About 10.25% of the first tests pass (the binomial spread over a
    // dispatch is far below the margin); a larger list is retried below.
    const uint64_t expected = firstPairs * 41 / 400 + 1;
    prm.maxList = uint32_t(std::min<uint64_t>(firstPairs * 11 / 100 + 65536, firstPairs + 1));
    const uint32_t listGroups = uint32_t(std::clamp<uint64_t>(
        expected / (GPU_EYE_THREADS * listItemsPerThread), 1, 65535));

    kept.clear();
    const auto tRun = std::chrono::steady_clock::now();
    GpuEyeRunResult r = GPU_EYE_RUN_FAILED;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        uint32_t need[2] = { 0, 0 };
        if (failCountdown > 0 && --failCountdown == 0)
            r = GPU_EYE_RUN_FAILED; // injected (tests)
        else
            r = backend->run(prm, listGroups, kept, need);
        if (r != GPU_EYE_RUN_OVERFLOW)
            break;
        // too small: run again with the sizes this run needed
        dbg.retries++;
        if (need[0] > prm.maxOut)
            prm.maxOut = keptCap = std::max(need[0] + need[0] / 4, keptCap);
        if (need[1] > prm.maxList)
            prm.maxList = uint32_t(std::min<uint64_t>(uint64_t(need[1]) + need[1] / 8, firstPairs + 1));
        kept.clear();
    }
    if (r != GPU_EYE_RUN_OK)
        return false;
    const auto tEnd = std::chrono::steady_clock::now();
    // dispatches of about RUN_FAST .. RUN_SLOW seconds (including the wait
    // for other workers' dispatches)
    const double runSeconds = std::chrono::duration<double>(tEnd - tRun).count();
    if (runSeconds > RUN_SLOW && runPlans > RUN_PLANS_MIN)
        runPlans /= 2;
    else if (runSeconds < RUN_FAST && uint32_t(n) == runPlans && runPlans < RUN_PLANS_MAX)
        runPlans *= 2;
    if (debugEnabled())
    {   // per worker: where the time of a dispatch goes
        using ms = std::chrono::duration<double, std::milli>;
        dbg.runs++;
        dbg.prep += ms(tRun - tPrep).count();
        dbg.gpu += ms(tEnd - tRun).count();
        dbg.plans += n;
        dbg.units += units.size();
        dbg.nibbleTables += batchTables.size();
        if (dbg.runs % 500 == 0)
            fprintf(stderr, "gpu eye search: %ld batches in %ld dispatches (%ld retried), per dispatch "
                "%.2f ms preparation, %.2f ms GPU; %.0f plans, %.0f units, %.0f nibble tables; "
                "up to %u plans per dispatch\n", dbg.batches, dbg.runs, dbg.retries,
                dbg.prep / dbg.runs, dbg.gpu / dbg.runs, dbg.plans / dbg.runs, dbg.units / dbg.runs,
                dbg.nibbleTables / dbg.runs, runPlans);
    }
    // back to the plan indices of the batch
    for (uint32_t& e : kept)
    {
        const uint32_t pi = e >> 16, h = e & 0xffff;
        if (pi >= uint32_t(n) || h < h0 || h >= h1)
            return false;
        e = order[pi] << 16 | h;
    }
    for (uint32_t e : kept)
        eyeSearchKeep(es, &batch[e >> 16], int(e & 0xffff));
    if (keptOut)
        for (uint32_t e : kept)
            keptOut->push_back(((e >> 16) + planOffset) << 16 | (e & 0xffff));
    return true;
}

// The GPU failed: continue on the CPU and try the GPU again after a pause
// that doubles with every failure (5 s .. 10 min).
void GpuEyeSweeper::gpuFailed()
{
    failCount++;
    goodFamilies = 0;
    backoff = std::min(backoff + 1, 8);
    const double pause = retryScale * std::min(5.0 * double(1 << (backoff - 1)), 600.0);
    retryAt = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(pause));
    if (debugEnabled())
        fprintf(stderr, "gpu eye search: GPU failed (%ld), on the CPU for %.0f s\n", failCount, pause);
    backend.reset();
}

void GpuEyeSweeper::tryGpuAgain()
{
    if (g_broken || std::chrono::steady_clock::now() < retryAt)
        return;
    const long countdown = failCountdown;
    if (!init())
    {
        gpuFailed();
        return;
    }
    failCountdown = countdown;  // an injected failure stays where it was
    if (debugEnabled())
        fprintf(stderr, "gpu eye search: using the GPU again\n");
}

int GpuEyeSweeper::scanFamily(EyeSearch *es, uint64_t low48, EyeSearchCallback cb, void *user,
    const volatile int *stop)
{
    if (!backend && failCount > 0)
        tryGpuAgain();
    if (backend && g_broken)
        backend.reset();
    if (!backend)
        return scanEyeFamily(es, low48, cb, user, stop);

    // The periodic self test (one worker at a time): a GPU that computes
    // wrong results would lose hits, so it is not used again.
    if (recheckSeconds > 0 && secondsSince(g_lastCheck) > recheckSeconds)
    {
        std::unique_lock<std::mutex> lock(g_checkMutex, std::try_to_lock);
        if (lock.owns_lock() && secondsSince(g_lastCheck) > recheckSeconds)
        {
            g_lastCheck = std::chrono::steady_clock::now();
            if (!selfTest())
            {
                g_broken = true;
                if (debugEnabled())
                    fprintf(stderr, "gpu eye search: periodic self test FAILED, the GPU is not used again\n");
            }
        }
        if (g_broken)
        {
            backend.reset();
            return scanEyeFamily(es, low48, cb, user, stop);
        }
    }

    eyeSearchBegin(es, low48);
    decoLoaded = false;
    bool usedCpu = false;
    for (;;)
    {
        const int n = eyeSearchNextPlans(es, plans.data(), MAX_PLANS, stop);
        if (n < 0)
            return -1;
        if (n == 0)
        {
            // 0 plans must mean "all groups handled", else hits would be
            // lost; the engine guarantees it, this check keeps it so
            if (!eyeSearchPlansDone(es))
                return -1;
            break;
        }
        int done = 0;
        if (backend && !g_broken && sweep(es, plans.data(), n, 0, NSEEDS, nullptr, &done))
            continue;
        // the GPU failed: the rest of this batch and of the family on the CPU
        if (backend)
            gpuFailed();
        usedCpu = true;
        if (eyeSearchSweep(es, plans.data() + done, n - done, stop) < 0)
            return -1;
    }
    if (!usedCpu && ++goodFamilies >= 100)
        backoff = 0;    // working again for a while: short pauses after the next failure
    return eyeSearchFinish(es, cb, user, stop);
}
