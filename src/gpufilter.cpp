#include "gpufilter.h"
#include "gpufilter_backend.h"

#include "seedatlas-engine/biomenoise.h"
#include "seedatlas-engine/finders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Bound on the directional derivative of a Perlin octave per unit of its
// input. The largest value observed in 4e7 random samples is 3.16; the
// engine's own range search assumes 3.75.
static const double PERLIN_GRADIENT_BOUND = 4.0;
// Margin for the single precision noise (in units of 1/10000). The observed
// error is below 1 near the origin. Further out, the rounding of the scaled
// coordinates (relative error ~6e-8) adds a positional error, which the
// gradient bound converts into a value error; it gets a safety factor of 10.
static const double FLOAT_MARGIN = 50.0;
static const double FLOAT_POS_ERROR = 10 * 1.2e-7;
// Single precision is not good enough beyond this distance (in quarts).
static const double MAX_DISTANCE = 1 << 22;
// Samples of the coarsest grid level of a test, the most samples of the
// finest level and the largest gradient part of its bound. Seeds that are
// still undecided at the finest level are left to the CPU.
static const double COARSE_GRID_SAMPLES = 64.0;
static const double MAX_GRID_SAMPLES = 16384.0;
static const double MAX_GRID_BOUND = 400.0;

// md5 of "octave_-12" ... "octave_0" (as in xOctaveInit())
const uint64_t md5_octave_n[][2] = {
    {0xb198de63a8012672, 0x7b84cad43ef7b5a8}, {0x0fd787bfbc403ec3, 0x74a4a31ca21b48b8},
    {0x36d326eed40efeb2, 0x5be9ce18223c636a}, {0x082fe255f8be6631, 0x4e96119e22dedc81},
    {0x0ef68ec68504005e, 0x48b6bf93a2789640}, {0xf11268128982754f, 0x257a1d670430b0aa},
    {0xe51c98ce7d1de664, 0x5f9478a733040c45}, {0x6d7b49e7e429850a, 0x2e3063c622a24777},
    {0xbd90d5377ba1b762, 0xc07317d419a7548d}, {0x53d39c6752dac858, 0xbcd1c5a80ab65b3e},
    {0xb4a24d7a84e7677b, 0x023ff9668e89b5c4}, {0xdffa22b534c5f608, 0xb9b67517d3665ca9},
    {0xd50708086cef4d7c, 0x6e1651ecc7f43309},
};

static bool debugEnabled()
{
    return getenv("SEED_ATLAS_GPU_DEBUG") != nullptr;
}

static std::atomic_bool g_allowed(true);

void GpuClimateFilter::setAllowed(bool allowed)
{
    g_allowed = allowed;
}

bool GpuClimateFilter::enabled()
{
    const char *env = getenv("SEED_ATLAS_GPU");
    if (env && strcmp(env, "0") == 0)
        return false;
    if (!g_allowed && !forced())
        return false;
    return gpuBackendAvailable();
}

bool GpuClimateFilter::forced()
{
    const char *env = getenv("SEED_ATLAS_GPU");
    return env && strcmp(env, "force") == 0;
}

GpuClimateFilter::GpuClimateFilter() {}
GpuClimateFilter::~GpuClimateFilter() {}

// A climate noise in the octave buffer.
struct NoiseInfo
{
    uint64_t clo, chi;      // md5 salt of the noise
    uint32_t noct, oct0;    // octaves per octave noise, first octave
    double dpamp;           // double Perlin amplitude
    double gradsum;         // amplitude * frequency over all octaves (A and B)
};

// Adds the octaves of a climate noise: their seed salts, amplitudes,
// frequencies and the bound on what the octave pairs after each one add.
static bool addNoise(int para, bool large, std::vector<GpuOct>& octs, NoiseInfo& info)
{
    const ClimateNoiseSpec *spec = getClimateNoiseSpec(para);
    if (!spec)
        return false;
    // the amplitudes and frequencies do not depend on the seed
    // (setClimateParaSeed() would take NP_SHIFT for NP_DEPTH)
    BiomeNoise bn;
    setBiomeSeedParts(&bn, 0, large, 1U << para);
    const DoublePerlinNoise *dp = &bn.climate[para];
    if (dp->octA.octcnt != dp->octB.octcnt)
        return false;

    int li = large ? 1 : 0;
    info.clo = spec->md5[li][0];
    info.chi = spec->md5[li][1];
    info.noct = dp->octA.octcnt;
    info.oct0 = octs.size();
    info.dpamp = dp->amplitude;
    info.gradsum = 0;

    int n = 0;
    for (int i = 0; i < spec->len; i++)
    {
        if (spec->amp[i] == 0)
            continue; // octaves without amplitude are not initialized
        const PerlinNoise *p = &dp->octA.octaves[n++];
        GpuOct o = {};
        o.mlo = md5_octave_n[12 + spec->omin[li] + i][0];
        o.mhi = md5_octave_n[12 + spec->omin[li] + i][1];
        o.amp = (float) p->amplitude;
        o.lac = (float) p->lacunarity;
        octs.push_back(o);
        // octave B samples at 337/331 times the frequency
        info.gradsum += p->amplitude * p->lacunarity * (1.0 + 337.0 / 331.0);
    }
    if (n != (int) info.noct)
        return false;
    // octave A and B of a pair have the same amplitude
    double rest = 0;
    for (int i = n - 1; i >= 0; i--)
    {
        octs[info.oct0 + i].rest = (float) (10000.0 * info.dpamp * PERLIN_VALUE_BOUND * rest);
        rest += 2 * std::fabs(dp->octA.octaves[i].amplitude);
    }
    return true;
}

// Grid levels from coarse to fine (steps divided by 4). The CPU samples
// integer positions only, so a grid with step s leaves every position within
// sqrt(2) * (s/2) of a sample, which the gradient bound turns into a bound on
// the value.
static void setGridLevels(GpuTest& g, double grad, double margin)
{
    double area = (double) g.w * g.h;
    // the finest level: the widest grid with a small gradient bound, but at
    // least a step of 4 (finer grids cost more than they save)
    int fine = 4;
    while (fine < 64 && grad * std::sqrt(2.0) * fine <= MAX_GRID_BOUND)
        fine *= 2;
    while (area / ((double) fine * fine) > MAX_GRID_SAMPLES)
        fine *= 2;
    int coarse = fine;
    while (area / ((double) coarse * coarse) > COARSE_GRID_SAMPLES)
        coarse *= 2;

    int n = 0;
    for (int step = coarse; n < 4; step /= 4)
    {
        if (step < fine || n == 3)
            step = fine;
        g.step[n] = step;
        g.bound[n] = (float) (grad * std::sqrt(2.0) * (step / 2) + margin);
        n++;
        if (step == fine)
            break;
    }
    g.nlevels = n;
    g.margin = (float) margin;
}

// The largest distance of an area from the origin (quart coordinates).
static double farthest(int x, int z, int w, int h)
{
    return std::max({std::abs((double) x), std::abs((double) x + w),
                     std::abs((double) z), std::abs((double) z + h)});
}

static bool initClimateTest(const GpuClimateFilter::Test& t, bool large,
                            std::vector<GpuOct>& octs, GpuTest& g)
{
    NoiseInfo info;
    if (!addNoise(t.para, large, octs, info))
        return false;
    double far = farthest(t.x, t.z, t.w, t.h);
    if (far > MAX_DISTANCE)
        return false;
    double grad = 10000.0 * info.dpamp * PERLIN_GRADIENT_BOUND * info.gradsum;
    double margin = FLOAT_MARGIN + grad * far * FLOAT_POS_ERROR;

    g.clo = info.clo;
    g.chi = info.chi;
    g.noct = info.noct;
    g.oct0 = info.oct0;
    g.dpamp = (float) info.dpamp;
    g.lmin = (float) t.lmin;
    g.lmax = (float) t.lmax;
    g.x = t.x;
    g.z = t.z;
    g.w = t.w;
    g.h = t.h;
    setGridLevels(g, grad, margin);
    if (debugEnabled())
        fprintf(stderr, "gpu climate test: para=%d area=(%d,%d %dx%d) range=[%g,%g] "
            "octaves=%u levels=%u fine=%d margin=%g\n", t.para, t.x, t.z, t.w, t.h,
            t.lmin, t.lmax, g.noct, g.nlevels, g.step[g.nlevels-1], margin);
    return true;
}

static bool initSampleTest(const GpuClimateFilter::SampleTest& t, bool large,
                           std::vector<GpuOct>& octs, GpuSampleTest& g)
{
    if (t.paras.empty() || t.paras.size() >= GPU_MAX_NOISES || t.w <= 0 || t.h <= 0)
        return false;
    Range r = {4, t.x, t.z, t.w, t.h, 0, 1};
    MonteCarloRule rule;
    initMonteCarloRule(&rule, r, t.coverage, t.confidence);
    if (rule.shuffle)
        return false; // the positions come from a shuffled buffer

    // The decisions of the stopping rule after m samples. The GPU counts a
    // superset of the CPU's successes, so it can reject a seed only where
    // every count up to its own fails (and none of them can pass before).
    int first = 0; // the fewest samples that can fail
    for (int m = 1; m <= (int) GPU_MAX_POSITIONS; m++)
    {
        int fail = -1, pass = m + 1;
        for (int x = m; x >= 0; x--)
            if (monteCarloDecide(&rule, m, x) == 1)
                pass = x;
        for (int x = 0; x <= m && monteCarloDecide(&rule, m, x) == 0; x++)
            fail = x;
        g.fail[m-1] = fail;
        g.pass[m-1] = pass;
        if (!first && fail >= 0)
            first = m;
    }
    if (!first)
        return false;
    // enough positions for a few more samples (inside the radius) than that
    double inside = 1;
    if (t.rmaxsq)
        inside = std::min(1.0, 3.14159 * t.rmaxsq / (16.0 * t.w * t.h));
    double npos = std::ceil((first + 2) / inside) + 1;
    g.npos = (uint32_t) std::min<double>(npos, GPU_MAX_POSITIONS);
    if ((double) t.w * t.h <= g.npos)
        return false;
    g.x = t.x;
    g.z = t.z;
    g.w = t.w;
    g.h = t.h;
    g.rmaxsq = t.rmaxsq;

    // The rounding errors: of the shift noise, which moves the positions,
    // and of each parameter, including that position error.
    double far = farthest(t.x, t.z, t.w, t.h) + 16;
    if (far > MAX_DISTANCE)
        return false;
    NoiseInfo shift;
    if (!addNoise(NP_SHIFT, large, octs, shift))
        return false;
    double sgrad = 10000.0 * shift.dpamp * PERLIN_GRADIENT_BOUND * shift.gradsum;
    double serr = (FLOAT_MARGIN + sgrad * far * FLOAT_POS_ERROR) / 10000.0;
    double poserr = far * FLOAT_POS_ERROR + 4.0 * serr;
    GpuNoise& sn = g.noise[0];
    sn = GpuNoise();
    sn.clo = shift.clo;
    sn.chi = shift.chi;
    sn.noct = shift.noct;
    sn.oct0 = shift.oct0;
    sn.dpamp = (float) shift.dpamp;
    g.nnoise = 1;
    for (size_t i = 0; i < t.paras.size(); i++)
    {
        NoiseInfo info;
        if (!addNoise(t.paras[i], large, octs, info))
            return false;
        double grad = 10000.0 * info.dpamp * PERLIN_GRADIENT_BOUND * info.gradsum;
        GpuNoise& n = g.noise[g.nnoise++];
        n = GpuNoise();
        n.clo = info.clo;
        n.chi = info.chi;
        n.noct = info.noct;
        n.oct0 = info.oct0;
        n.dpamp = (float) info.dpamp;
        // the CPU truncates the scaled value to an integer
        n.margin = (float) (FLOAT_MARGIN + grad * (far * FLOAT_POS_ERROR + poserr) + 2);
        n.lo = (float) t.lo[i];
        n.hi = (float) t.hi[i];
        if (debugEnabled())
            fprintf(stderr, "gpu sample test: para=%d area=(%d,%d %dx%d) bounds=[%d,%d] "
                "positions=%u margin=%g\n", t.paras[i], t.x, t.z, t.w, t.h,
                t.lo[i], t.hi[i], g.npos, n.margin);
    }
    return true;
}

bool GpuClimateFilter::init(const Tests& tests, bool large)
{
    backend.reset();
    if (!forced() && !selfTest())
        return false;
    return initBackend(tests, large);
}

bool GpuClimateFilter::initBackend(const Tests& tests, bool large)
{
    backend.reset();
    if (tests.empty())
        return false;

    // leaving out tests only lets more seeds pass
    GpuTestBuf buf = {};
    std::vector<GpuOct> octs;
    uint32_t maxoct = 0;
    for (const Test& t : tests.climate)
    {
        if (buf.ntests == GPU_MAX_TESTS)
            break;
        GpuTest& g = buf.tests[buf.ntests];
        if (!initClimateTest(t, large, octs, g))
            return false;
        maxoct = std::max(maxoct, g.noct);
        buf.ntests++;
    }
    for (const SampleTest& t : tests.samples)
    {
        if (buf.nsamples == GPU_MAX_SAMPLE_TESTS)
            break;
        size_t n = octs.size();
        if (initSampleTest(t, large, octs, buf.samples[buf.nsamples]))
            buf.nsamples++;
        else
            octs.resize(n);
    }
    if (buf.ntests + buf.nsamples == 0 || octs.size() > GPU_MAX_OCTS)
        return false;

    auto t0 = std::chrono::steady_clock::now();
    backend.reset(gpuBackendCreate());
    if (!backend || !backend->init(buf, octs, maxoct))
    {
        backend.reset();
        return false;
    }
    if (debugEnabled())
        fprintf(stderr, "gpu init: %.1f ms\n", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count());
    return true;
}

bool GpuClimateFilter::filter(uint64_t start, uint32_t count, std::vector<uint64_t>& passed)
{
    if (!backend)
        return false;
    size_t old = passed.size();
    if (count && !backend->run(start, count, passed))
    {
        passed.resize(old);
        return false;
    }
    std::sort(passed.begin() + old, passed.end());
    return true;
}

bool GpuClimateFilter::cpuPasses(const Test& t, bool large, uint64_t seed)
{
    // as F_CLIMATE_NOISE in search.cpp
    BiomeNoise bn;
    setBiomeSeedParts(&bn, seed, large, 1U << t.para);
    double pmin, pmax;
    getParaRange(&bn.climate[t.para], &pmin, &pmax, t.x, t.z, t.w, t.h, NULL, NULL);
    return pmin <= t.lmax && pmax >= t.lmin;
}

// monteCarloBiomes() with the positions whose climate is within the bounds
// as the successes (biomeMayBeIncluded() in search.cpp); the real biomes can
// only succeed there.
struct SampleEval
{
    const GpuClimateFilter::SampleTest *t;
    const BiomeNoise *bn;
};

static int evalSample(Generator *, int scale, int x, int, int z, void *data)
{
    const SampleEval *e = (const SampleEval*) data;
    if (e->t->rmaxsq)
    {
        int64_t dx = x * scale, dz = z * scale;
        if (dx*dx + dz*dz >= e->t->rmaxsq)
            return -1;
    }
    double px, pz;
    getBiomeNoisePos(e->bn, x, z, 0, &px, &pz);
    for (size_t i = 0; i < e->t->paras.size(); i++)
    {
        int64_t v = sampleBiomeNoisePara(e->bn, e->t->paras[i], px, pz);
        if (v < e->t->lo[i] || v > e->t->hi[i])
            return 0;
    }
    return 1;
}

bool GpuClimateFilter::cpuPasses(const SampleTest& t, bool large, uint64_t seed)
{
    BiomeNoise bn;
    setBiomeSeed(&bn, seed, large);
    SampleEval e = { &t, &bn };
    Range r = {4, t.x, t.z, t.w, t.h, 0, 1};
    uint64_t rng;
    setSeed(&rng, seed);
    return monteCarloBiomes(NULL, r, &rng, t.coverage, t.confidence, evalSample, &e) == 1;
}

// Does the GPU keep every seed that passes the exact CPU tests? This guards
// against drivers that compile the kernel wrongly. The tests pass often on
// the CPU (a third and a few percent of the seeds), so a wrong GPU shows.
bool GpuClimateFilter::selfTest()
{
    static std::mutex mutex;
    static int result = -1;
    std::lock_guard<std::mutex> lock(mutex);
    if (result >= 0)
        return result;

    Tests climate, sample;
    climate.climate.push_back({ NP_TEMPERATURE, -16, -16, 32, 32, 2000, 1e9 });
    SampleTest st;
    st.x = st.z = -25;
    st.w = st.h = 50;
    st.rmaxsq = 99 * 99 + 1;
    st.coverage = 0.4;
    st.confidence = 0.95f;
    uint64_t desert[4] = { 1ULL << 2, 0, 0, 0 };
    BiomeClimateBounds cb;
    if (getBiomeClimateBounds(&cb, MC_NEWEST, desert))
    {
        for (int p : { NP_TEMPERATURE, NP_HUMIDITY })
        {
            st.paras.push_back(p);
            st.lo.push_back(cb.lo[p]);
            st.hi.push_back(cb.hi[p]);
        }
        sample.samples.push_back(st);
    }

    auto t0 = std::chrono::steady_clock::now();
    const uint64_t seed0 = 0x5EED;
    const uint32_t n = 512;
    bool ok = true;
    int cpupass = 0;
    for (const Tests *tests : { &climate, &sample })
    {
        GpuClimateFilter f;
        std::vector<uint64_t> kept;
        if (tests->empty() || !f.initBackend(*tests, false) || !f.filter(seed0, n, kept))
        {
            ok = false;
            break;
        }
        for (uint64_t seed = seed0; seed < seed0 + n && ok; seed++)
        {
            bool pass = tests->climate.empty()
                ? cpuPasses(tests->samples[0], false, seed)
                : cpuPasses(tests->climate[0], false, seed);
            cpupass += pass;
            if (pass && !std::binary_search(kept.begin(), kept.end(), seed))
                ok = false;
        }
    }
    if (debugEnabled())
        fprintf(stderr, "gpu self test: %s (%d seeds pass on the CPU, %.1f ms)\n",
            ok ? "passed" : "FAILED", cpupass, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count());
    result = ok;
    return ok;
}
