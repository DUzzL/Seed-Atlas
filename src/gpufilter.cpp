#include "gpufilter.h"
#include "gpufilter_backend.h"

#include "seedatlas-engine/biomenoise.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

bool GpuClimateFilter::enabled()
{
    const char *env = getenv("SEED_ATLAS_GPU");
    if (!env || (strcmp(env, "1") != 0 && strcmp(env, "force") != 0))
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

// The octaves of the climate noise of a test: their seed salts, amplitudes
// and frequencies. Returns the sum of amplitude * frequency over the octaves
// of both octave noises (for the gradient bound), or a negative value.
static double addOctaves(const GpuClimateFilter::Test& t, bool large, GpuTest& g,
                         std::vector<GpuOct>& octs)
{
    const ClimateNoiseSpec *spec = getClimateNoiseSpec(t.para);
    if (!spec)
        return -1;
    // the amplitudes and frequencies do not depend on the seed
    BiomeNoise bn;
    setClimateParaSeed(&bn, 0, large, t.para, -1);
    const DoublePerlinNoise *dp = &bn.climate[t.para];
    if (dp->octA.octcnt != dp->octB.octcnt)
        return -1;

    int li = large ? 1 : 0;
    g.clo = spec->md5[li][0];
    g.chi = spec->md5[li][1];
    g.noct = dp->octA.octcnt;
    g.oct0 = octs.size();
    g.dpamp = dp->amplitude;

    double gradsum = 0;
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
        gradsum += p->amplitude * p->lacunarity * (1.0 + 337.0 / 331.0);
    }
    return n == (int) g.noct ? gradsum : -1;
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

bool GpuClimateFilter::init(const std::vector<Test>& tests, bool large)
{
    backend.reset();
    if (tests.empty())
        return false;

    std::vector<GpuTest> gtests;
    std::vector<GpuOct> octs;
    uint32_t maxoct = 1;
    for (const Test& t : tests)
    {
        if (gtests.size() == GPU_MAX_TESTS)
            break; // leaving out tests only lets more seeds pass
        GpuTest g = {};
        double gradsum = addOctaves(t, large, g, octs);
        if (gradsum < 0)
            return false;
        maxoct = std::max(maxoct, g.noct);

        double far = std::max({std::abs((double) t.x), std::abs((double) t.x + t.w),
                               std::abs((double) t.z), std::abs((double) t.z + t.h)});
        if (far > MAX_DISTANCE)
            return false;
        double grad = 10000.0 * g.dpamp * PERLIN_GRADIENT_BOUND * gradsum;
        double margin = FLOAT_MARGIN + grad * far * FLOAT_POS_ERROR;

        g.lmin = (float) t.lmin;
        g.lmax = (float) t.lmax;
        g.x = t.x;
        g.z = t.z;
        g.w = t.w;
        g.h = t.h;
        setGridLevels(g, grad, margin);
        if (debugEnabled())
            fprintf(stderr, "gpu test: para=%d area=(%d,%d %dx%d) range=[%g,%g] "
                "octaves=%u levels=%u fine=%d margin=%g\n", t.para, t.x, t.z, t.w, t.h,
                t.lmin, t.lmax, g.noct, g.nlevels, g.step[g.nlevels-1], margin);
        gtests.push_back(g);
    }

    backend.reset(gpuBackendCreate());
    if (!backend || !backend->init(gtests, octs, maxoct))
    {
        backend.reset();
        return false;
    }
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
