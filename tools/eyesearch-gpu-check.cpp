/* Checks the GPU stage of the End portal eye search against the CPU scan and
 * measures the speed of both on the whole machine.
 * Build the engine first: make -C seedatlas-engine release
 *
 * macOS (Metal):
 *   c++ -std=gnu++17 -O2 -fobjc-arc -fwrapv -ffp-contract=off -I. -Isrc -Iseedatlas-engine
 *     tools/eyesearch-gpu-check.cpp src/eyesearchgpu.cpp src/gpufilter.cpp
 *     -x objective-c++ src/eyesearchgpu_metal.mm src/gpufilter_metal.mm -x none
 *     seedatlas-engine/libseedatlas-engine.a -framework Metal -framework Foundation
 *     -o eyesearch-gpu-check
 *
 * Windows, Linux (Vulkan; SEED_ATLAS_GPU_ALLOW_CPU=1 also accepts lavapipe):
 *   c++ -std=gnu++17 -O2 -fwrapv -ffp-contract=off -I. -Isrc -Iseedatlas-engine
 *     tools/eyesearch-gpu-check.cpp src/eyesearchgpu.cpp src/gpufilter.cpp
 *     src/eyesearchgpu_vulkan.cpp src/gpufilter_vulkan.cpp
 *     seedatlas-engine/libseedatlas-engine.a -ldl -lpthread -o eyesearch-gpu-check
 *
 * Usage:
 *   eyesearch-gpu-check equiv [-quick]     GPU path == scanEyeFamily() on sample families
 *   eyesearch-gpu-check real DIR           the real 12-eye portals of DIR/portals_1.21.1.txt
 *   eyesearch-gpu-check bench MC N MINEYES EXACT SECONDS THREADS FEEDERS
 *                                          seeds/s with THREADS workers, FEEDERS of
 *                                          which drive the GPU (0: CPU only)
 */

#include "eyesearchgpu.h"
#include "seedatlas-engine/util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

// The engine of the app is built with STRUCT_CONFIG_OVERRIDE; this tool may
// link either build.
extern "C" int getStructureConfig_override(int stype, int mc, StructureConfig *sconf)
{
    return getStructureConfig(stype, mc, sconf);
}

static double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

struct Hit
{
    uint64_t seed;
    int index, cx, cz, eyes, exactEyes;
    uint16_t eyeMask, exactMask;
    bool operator<(const Hit& o) const { return seed != o.seed ? seed < o.seed : index < o.index; }
    bool operator==(const Hit& o) const
    {
        return seed == o.seed && index == o.index && cx == o.cx && cz == o.cz && eyes == o.eyes &&
            exactEyes == o.exactEyes && eyeMask == o.eyeMask && exactMask == o.exactMask;
    }
};

static int collect(void *user, const EyeSearchHit *h)
{
    static_cast<std::vector<Hit>*>(user)->push_back({ h->seed, h->index, h->startChunk.x,
        h->startChunk.z, h->portal.eyeCount, h->portal.exactEyeCount, h->portal.eyes,
        h->portal.exact });
    return 0;
}

static int failures_ = 0;

// One family on the CPU and through the GPU path: the same hits?
static bool compareFamily(const EyeSearchConfig& cfg, uint64_t low48, GpuEyeSweeper& sw,
    long *nhits, double *tcpu, double *tgpu, EyeSearchStats *gst)
{
    EyeSearch *a = createEyeSearch(&cfg), *b = createEyeSearch(&cfg);
    std::vector<Hit> ha, hb;
    double t0 = now();
    int na = scanEyeFamily(a, low48, collect, &ha, nullptr);
    double t1 = now();
    int nb = sw.scanFamily(b, low48, collect, &hb, nullptr);
    double t2 = now();
    *tcpu += t1 - t0;
    *tgpu += t2 - t1;
    EyeSearchStats sa, st;
    getEyeSearchStats(a, &sa);
    getEyeSearchStats(b, &st);
    gst->survivors += st.survivors;
    gst->tests += st.tests;
    gst->plans += st.plans;
    std::sort(ha.begin(), ha.end());
    std::sort(hb.begin(), hb.end());
    // stage (b): the (plan, seed) pairs that reach minEyes are the same
    // (after an injected GPU failure the sweeper is on the CPU for now)
    bool ok = na == nb && ha == hb && (sw.ok() || sw.failures() > 0) &&
        sa.survivors == st.survivors && sa.plans == st.plans;
    if (!ok)
    {
        failures_++;
        printf("FAIL %s large=%d n=%d min=%d exact=%d family %#llx: cpu %d hits, gpu %d hits, "
            "survivors %llu / %llu, plans %llu / %llu%s\n",
            mc2str(cfg.mc), cfg.large, cfg.numStrongholds, cfg.minEyes, cfg.exactOnly,
            (unsigned long long) low48, na, nb, (unsigned long long) sa.survivors,
            (unsigned long long) st.survivors, (unsigned long long) sa.plans,
            (unsigned long long) st.plans, sw.ok() ? "" : " (GPU failed)");
    }
    *nhits += long(ha.size());
    freeEyeSearch(a);
    freeEyeSearch(b);
    return ok;
}

static void equiv(bool quick)
{
    struct Case { int mc, large, nsh, minEyes, exactOnly, fams; };
    // thresholds from where the GPU path is used (7) up to 12; the low ones
    // have many hits, the high ones exercise the plans without misses
    const Case cases[] = {
        { MC_26_3,   0, 3,  8, 1, 3 }, { MC_26_3,   0, 3,  7, 1, 1 },
        { MC_26_3,   0, 1,  7, 0, 8 }, { MC_1_21_1, 0, 3,  8, 0, 1 },
        { MC_1_19_2, 0, 1,  8, 1, 2 }, { MC_1_18,   0, 1,  7, 1, 3 },
        { MC_26_3,   0, 3, 12, 1, 6 }, { MC_26_3,   0, 3, 12, 0, 6 },
        { MC_26_3,   0, 3, 11, 1, 4 }, { MC_26_3,   0, 3, 10, 0, 3 },
        { MC_26_3,   0, 3,  9, 1, 2 }, { MC_26_3,   1, 3, 12, 1, 3 },
        { MC_26_3,   0, 1, 11, 0, 4 }, { MC_26_3,   0, 8, 12, 1, 2 },
        { MC_26_3,   0, 8, 10, 1, 1 },
        { MC_1_21_1, 0, 3, 12, 0, 4 }, { MC_1_21_1, 0, 3, 10, 1, 3 },
        { MC_1_21_1, 0, 1,  9, 0, 2 },
        { MC_1_19_4, 0, 3, 11, 0, 2 },
        { MC_1_19_2, 0, 3, 12, 1, 2 }, { MC_1_19_2, 0, 3, 11, 0, 1 },
        { MC_1_19_2, 0, 1, 10, 1, 2 },
        { MC_1_18,   0, 3, 12, 0, 1 }, { MC_1_18,   1, 1, 11, 1, 2 },
    };
    GpuEyeSweeper sw;
    if (!sw.init())
    {
        printf("FAIL: GPU not available\n");
        failures_++;
        return;
    }
    uint64_t rng = 77;
    int runs = 0;
    long hits = 0;
    double tc = 0, tg = 0;
    EyeSearchStats gst;
    std::memset(&gst, 0, sizeof(gst));
    for (const Case& c : cases)
    {
        EyeSearchConfig cfg = { c.mc, c.large, c.nsh, c.minEyes, c.exactOnly };
        int fams = quick ? std::max(1, c.fams / 2) : c.fams;
        long h0 = hits;
        for (int f = 0; f < fams; f++)
        {
            // the family of a real 12-eye portal (1.21.1) first, then random ones
            uint64_t low48 = f == 0 && c.mc == MC_1_21_1 ?
                uint64_t(-6695163795316312869LL) & MASK48 : splitmix64(&rng) & MASK48;
            compareFamily(cfg, low48, sw, &hits, &tc, &tg, &gst);
            runs++;
        }
        printf("%-6s large=%d n=%d min=%-2d exact=%d: %d families, %ld hits identical\n",
            mc2str(c.mc), c.large, c.nsh, c.minEyes, c.exactOnly, fams, hits - h0);
        fflush(stdout);
    }
    printf("equivalence: %d families (%d seeds), %ld hits identical; GPU kept %llu (plan, seed) "
        "pairs that passed the exact test (cpu %.1f s, gpu path %.1f s, one thread each)\n",
        runs, runs * 65536, hits, (unsigned long long) gst.survivors, tc, tg);
}

// A GPU failure in the middle of a family (injected at the n-th dispatch):
// the family is completed on the CPU with the same hits, the next one runs
// on the GPU again.
static void failures()
{
    struct Case { int mc, nsh, minEyes, exactOnly, failAt; };
    // 1.18 and 1.19.2 with 3 strongholds and 26.3 with 16 strongholds have
    // several GPU dispatches (at most 1024 plans each) per family
    const Case cases[] = {
        { MC_1_18, 3, 12, 0, 2 }, { MC_1_19_2, 3, 8, 1, 2 }, { MC_26_3, 16, 10, 1, 2 },
        { MC_26_3, 3, 7, 1, 1 }, { MC_1_21_1, 3, 9, 0, 1 }, { MC_26_3, 16, 8, 1, 2 },
    };
    const double scale = GpuEyeSweeper::retryScale, recheck = GpuEyeSweeper::recheckSeconds;
    GpuEyeSweeper::retryScale = 0;  // retry with the next family
    GpuEyeSweeper::recheckSeconds = 1e-6;   // and run the periodic self test every family
    uint64_t rng = 4242;
    for (const Case& c : cases)
    {
        GpuEyeSweeper sw;
        if (!sw.init())
        {
            printf("FAIL: GPU not available\n");
            failures_++;
            return;
        }
        EyeSearchConfig cfg = { c.mc, 0, c.nsh, c.minEyes, c.exactOnly };
        long hits = 0;
        double tc = 0, tg = 0;
        EyeSearchStats gst;
        std::memset(&gst, 0, sizeof(gst));
        sw.injectFailure(c.failAt);
        compareFamily(cfg, splitmix64(&rng) & MASK48, sw, &hits, &tc, &tg, &gst);
        const bool failed = sw.failures() == 1 && !sw.ok();
        // the next family: the GPU again
        compareFamily(cfg, splitmix64(&rng) & MASK48, sw, &hits, &tc, &tg, &gst);
        const bool back = sw.ok() && sw.failures() == 1;
        if (!failed || !back)
        {
            printf("FAIL: injected failure %s n=%d min=%d exact=%d: failed %d, GPU again %d\n",
                mc2str(c.mc), c.nsh, c.minEyes, c.exactOnly, failed, back);
            failures_++;
        }
        printf("%-6s n=%d min=%-2d exact=%d: GPU failure at dispatch %d, 2 families, %ld hits identical, "
            "GPU used again\n", mc2str(c.mc), c.nsh, c.minEyes, c.exactOnly, c.failAt, hits);
        fflush(stdout);
    }
    GpuEyeSweeper::retryScale = scale;
    GpuEyeSweeper::recheckSeconds = recheck;
    // the periodic self tests above passed (a failed one stops all GPU use)
    if (!GpuEyeSweeper::available())
    {
        printf("FAIL: the periodic GPU self test failed\n");
        failures_++;
    }
}

// The families of the real 12-eye portals through the GPU path.
static void real(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/portals_1.21.1.txt", dir);
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        printf("FAIL: cannot open %s\n", path);
        failures_++;
        return;
    }
    GpuEyeSweeper sw;
    if (!sw.init())
    {
        printf("FAIL: GPU not available\n");
        failures_++;
        fclose(fp);
        return;
    }
    char line[4096];
    int nreal = 0, found = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '#')
            continue;
        char ver[32];
        long long seed;
        int idx, cx, cz, off;
        if (sscanf(line, "%31s %lld %d %d %d%n", ver, &seed, &idx, &cx, &cz, &off) != 5)
            continue;
        int eyes = 0;
        const char *p = line + off;
        for (int i = 0; i < 12; i++)
        {
            int x, y, z, e, o;
            if (sscanf(p, "%d %d %d %d%n", &x, &y, &z, &e, &o) != 4)
                break;
            eyes += e;
            p += o;
        }
        if (eyes != 12)
            continue;
        nreal++;
        for (int exactOnly = 0; exactOnly <= 1; exactOnly++)
        {
            EyeSearchConfig cfg = { MC_1_21_1, 0, 3, 12, exactOnly };
            EyeSearch *es = createEyeSearch(&cfg);
            std::vector<Hit> hits;
            double t0 = now();
            int n = sw.scanFamily(es, uint64_t(seed), collect, &hits, nullptr);
            double t1 = now();
            bool hit = false;
            for (const Hit& h : hits)
                hit |= h.seed == uint64_t(seed) && h.cx == cx && h.cz == cz && h.eyes == 12;
            printf("real 12-eye portal %lld (chunk %d,%d), exactOnly %d: GPU path %.2f s, %d hits, %s\n",
                seed, cx, cz, exactOnly, t1 - t0, n, hit ? "found" : "NOT FOUND");
            found += hit && sw.ok();
            freeEyeSearch(es);
        }
    }
    fclose(fp);
    // all four are found with exactOnly 0 and 1 by the CPU scan (test-eyesearch -real)
    if (nreal != 4 || found != 8)
    {
        printf("FAIL: %d real 12-eye portals, %d of %d searches found them\n", nreal, found, 2 * nreal);
        failures_++;
    }
}

// seeds/s on the whole machine: 'threads' workers take families from a
// shared counter for 'seconds'; the first 'feeders' of them use the GPU.
static void bench(int mc, int nsh, int minEyes, int exactOnly, double seconds, int threads,
    int feeders)
{
    EyeSearchConfig cfg = { mc, 0, nsh, minEyes, exactOnly };
    std::atomic<uint64_t> next(0);
    std::atomic<bool> stop(false);
    std::vector<uint64_t> done(threads, 0), gpuDone(threads, 0);
    std::atomic<long> hits(0);
    std::vector<std::thread> pool;
    const uint64_t base = 0x0000123456789abcULL;
    double t0 = now();
    for (int t = 0; t < threads; t++)
    {
        pool.emplace_back([&, t]() {
#ifdef __APPLE__
            // SEED_ATLAS_BENCH_QOS: the QoS class of the workers (the app
            // uses "initiated", see eyesearchdriver.cpp)
            if (const char *q = getenv("SEED_ATLAS_BENCH_QOS"))
                pthread_set_qos_class_self_np(!strcmp(q, "interactive") ? QOS_CLASS_USER_INTERACTIVE :
                    !strcmp(q, "initiated") ? QOS_CLASS_USER_INITIATED :
                    !strcmp(q, "utility") ? QOS_CLASS_UTILITY :
                    !strcmp(q, "background") ? QOS_CLASS_BACKGROUND : QOS_CLASS_DEFAULT, 0);
#endif
            EyeSearch *es = createEyeSearch(&cfg);
            std::unique_ptr<GpuEyeSweeper> sw;
            if (t < feeders)
            {
                sw.reset(new GpuEyeSweeper);
                if (!sw->init())
                    sw.reset();
            }
            while (!stop)
            {
                uint64_t low48 = (base + next++ * 0x9e3779b97f4aULL) & MASK48;
                int n = sw ? sw->scanFamily(es, low48, nullptr, nullptr, nullptr)
                           : scanEyeFamily(es, low48, nullptr, nullptr, nullptr);
                if (n >= 0)
                    hits += n;
                done[t]++;
                if (sw && sw->ok())
                    gpuDone[t]++;
            }
            freeEyeSearch(es);
        });
    }
    while (now() - t0 < seconds)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop = true;
    for (std::thread& th : pool)
        th.join();
    double t = now() - t0;
    uint64_t fams = 0, gfams = 0;
    for (int i = 0; i < threads; i++)
    {
        fams += done[i];
        gfams += gpuDone[i];
    }
    double rate = fams * 65536.0 / t;
    printf("%-6s n=%d min=%-2d exact=%d threads=%-2d gpu feeders=%d: %10.0f seeds/s "
        "(%llu families in %.1f s, %llu by GPU feeders, %ld hits)\n",
        mc2str(mc), nsh, minEyes, exactOnly, threads, feeders, rate,
        (unsigned long long) fams, t, (unsigned long long) gfams, (long) hits);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "equiv"))
    {
        if (!GpuEyeSweeper::available())
        {
            printf("FAIL: GPU not available or self test failed\n");
            return 1;
        }
        equiv(argc >= 3 && !strcmp(argv[2], "-quick"));
        failures();
    }
    else if (argc == 3 && !strcmp(argv[1], "real"))
    {
        if (!GpuEyeSweeper::available())
        {
            printf("FAIL: GPU not available or self test failed\n");
            return 1;
        }
        real(argv[2]);
    }
    else if (argc >= 9 && !strcmp(argv[1], "bench"))
    {
        if (argc >= 10)
            GpuEyeSweeper::seedsPerLane = uint32_t(atoi(argv[9]));
        if (argc >= 11)
            GpuEyeSweeper::listItemsPerThread = uint32_t(atoi(argv[10]));
        if (atoi(argv[8]) > 0 && !GpuEyeSweeper::available())
        {
            printf("FAIL: GPU not available or self test failed\n");
            return 1;
        }
        bench(str2mc(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atof(argv[6]),
            atoi(argv[7]), atoi(argv[8]));
        return 0;
    }
    else
    {
        printf("usage: %s equiv [-quick] | real DIR | bench MC N MINEYES EXACT SECONDS THREADS FEEDERS\n",
            argv[0]);
        return 2;
    }
    if (failures_)
        printf("%d FAILURES\n", failures_);
    else
        printf("eye search GPU checks passed\n");
    return failures_ != 0;
}
