R"METAL(
// GPU prefilter for 1.18+ climate parameter conditions (see gpufilter.h).
// The permutations are built exactly as on the CPU (integer arithmetic). The
// noise is sampled in single precision on a grid; with a bound on the noise
// gradient, every seed rejected here also fails the exact test on the CPU.
#include <metal_stdlib>
using namespace metal;

#ifndef NOCT
#define NOCT 9      // most octaves of a test, per octave noise A or B
#endif
#ifndef TG
#define TG 2        // threads per threadgroup
#endif

struct Oct { ulong mlo, mhi; float amp, lac; };

struct Test {
    ulong clo, chi;     // md5 salt of the climate parameter
    uint  noct;         // octaves in each of octA and octB
    uint  oct0;         // index of the first octave in the octave buffer
    float dpamp;        // amplitude of the double Perlin noise
    float margin;       // rounding error margin
    float lmin, lmax;   // the bounds must overlap this required range
    int   x, z, w, h;   // area in quart coordinates
    uint  nlevels;      // grid levels, coarse to fine
    int   step[4];      // grid spacing of each level
    float bound[4];     // gradient bound * grid radius + margin of each level
};

struct Params {
    ulong seed0;
    uint  count;
    uint  ntests;
};

static inline ulong rotl(ulong x, uint b) { return (x << b) | (x >> (64 - b)); }

static inline ulong xnext(thread ulong &lo, thread ulong &hi)
{
    ulong l = lo, h = hi;
    ulong n = rotl(l + h, 17) + l;
    h ^= l;
    lo = rotl(l, 49) ^ h ^ (h << 21);
    hi = rotl(h, 28);
    return n;
}

static inline uint xnextint(thread ulong &lo, thread ulong &hi, uint n)
{
    ulong r = (xnext(lo, hi) & 0xFFFFFFFFul) * n;
    if ((uint)r < n)
    {
        while ((uint)r < (0u - n) % n)
            r = (xnext(lo, hi) & 0xFFFFFFFFul) * n;
    }
    return (uint)(r >> 32);
}

static inline float xnextdouble(thread ulong &lo, thread ulong &hi)
{
    return float(xnext(lo, hi) >> 11) * 0x1.0p-53f;
}

struct Perlin { float a, b, c, d2, t2; uint h2; };

static inline float grad(uint idx, float a, float b, float c)
{
    idx &= 15u;
    float u = ((0xAF00u >> idx) & 1u) ? b : a;
    float v = ((0x500Fu >> idx) & 1u) ? b : c;
    u = ((0xEAAAu >> idx) & 1u) ? -u : u;
    v = ((0x8CCCu >> idx) & 1u) ? -v : v;
    return u + v;
}

static inline float lerpf(float t, float a, float b) { return a + t * (b - a); }

static inline float sampleP(thread const Perlin &p, threadgroup const uchar *d, float x, float z)
{
    float d1 = x + p.a, d3 = z + p.c;
    float i1 = floor(d1), i3 = floor(d3);
    d1 -= i1; d3 -= i3;
    uint h1 = uint(int(i1)) & 255u, h3 = uint(int(i3)) & 255u;
    float d2 = p.d2; uint h2 = p.h2;
    float t1 = d1*d1*d1 * (d1 * (d1*6.0f-15.0f) + 10.0f);
    float t3 = d3*d3*d3 * (d3 * (d3*6.0f-15.0f) + 10.0f);
    float t2 = p.t2;
    uint a1 = (d[h1] + h2) & 255u, b1 = (d[h1+1] + h2) & 255u;
    uint a2 = (d[a1] + h3) & 255u, b2 = (d[b1] + h3) & 255u;
    uint a3 = (d[a1+1] + h3) & 255u, b3 = (d[b1+1] + h3) & 255u;
    float l1 = grad(d[a2],   d1,   d2,   d3);
    float l2 = grad(d[b2],   d1-1, d2,   d3);
    float l3 = grad(d[a3],   d1,   d2-1, d3);
    float l4 = grad(d[b3],   d1-1, d2-1, d3);
    float l5 = grad(d[a2+1], d1,   d2,   d3-1);
    float l6 = grad(d[b2+1], d1-1, d2,   d3-1);
    float l7 = grad(d[a3+1], d1,   d2-1, d3-1);
    float l8 = grad(d[b3+1], d1-1, d2-1, d3-1);
    l1 = lerpf(t1, l1, l2); l3 = lerpf(t1, l3, l4);
    l5 = lerpf(t1, l5, l6); l7 = lerpf(t1, l7, l8);
    l1 = lerpf(t2, l1, l3); l5 = lerpf(t2, l5, l7);
    return lerpf(t3, l1, l5);
}

static void initPerlin(thread Perlin &p, threadgroup uchar *d, ulong lo, ulong hi)
{
    p.a = xnextdouble(lo, hi) * 256.0f;
    p.b = xnextdouble(lo, hi) * 256.0f;
    p.c = xnextdouble(lo, hi) * 256.0f;
    for (uint i = 0; i < 256; i++) d[i] = uchar(i);
    for (uint i = 0; i < 256; i++)
    {
        uint j = xnextint(lo, hi, 256 - i) + i;
        uchar t = d[i]; d[i] = d[j]; d[j] = t;
    }
    d[256] = d[0];
    float i2 = floor(p.b);
    float d2 = p.b - i2;
    p.h2 = uint(int(i2)) & 255u;
    p.d2 = d2;
    p.t2 = d2*d2*d2 * (d2 * (d2*6.0f-15.0f) + 10.0f);
}

kernel void climateFilter(
    constant Params &P [[buffer(0)]],
    constant Test *tests [[buffer(1)]],
    constant Oct *octs [[buffer(2)]],
    device atomic_uint *npass [[buffer(3)]],
    device ulong *passed [[buffer(4)]],
    uint gid [[thread_position_in_grid]],
    uint lid [[thread_position_in_threadgroup]])
{
    threadgroup uchar permall[TG][2*NOCT][260];
    if (gid >= P.count) return;
    ulong seed = P.seed0 + gid;
    threadgroup uchar (*perm)[260] = permall[lid];

    // xSetSeed()
    ulong l = seed ^ 0x6a09e667f3bcc909ul;
    ulong h = l + 0x9e3779b97f4a7c15ul;
    l = (l ^ (l >> 30)) * 0xbf58476d1ce4e5b9ul;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ul;
    l = (l ^ (l >> 27)) * 0x94d049bb133111ebul;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebul;
    l = l ^ (l >> 31);
    h = h ^ (h >> 31);
    const ulong xlo = xnext(l, h), xhi = xnext(l, h);
    const float f = 337.0f / 331.0f;

    for (uint t = 0; t < P.ntests; t++)
    {
        constant Test &T = tests[t];
        constant Oct *oct = octs + T.oct0;
        const uint n = T.noct;
        Perlin pn[2*NOCT];
        ulong plo = xlo ^ T.clo, phi = xhi ^ T.chi;
        for (uint k = 0; k < 2; k++)
        {
            ulong olo = xnext(plo, phi), ohi = xnext(plo, phi);
            for (uint i = 0; i < n; i++)
                initPerlin(pn[k*n+i], perm[k*n+i], olo ^ oct[i].mlo, ohi ^ oct[i].mhi);
        }

        // Coarse to fine: the samples alone overlapping the range (up to the
        // rounding margin) means the test cannot reject the seed; the samples
        // widened by the bound of their grid missing it means it must. Seeds
        // still undecided at the finest level are left to the CPU.
        bool overlap = false, reject = false;
        for (uint lv = 0; lv < T.nlevels && !overlap && !reject; lv++)
        {
            const int step = T.step[lv];
            float vmin = INFINITY, vmax = -INFINITY;
            for (int j = 0; !overlap; j += step)
            {
                if (j > T.h - 1) j = T.h - 1;
                for (int i = 0; ; i += step)
                {
                    if (i > T.w - 1) i = T.w - 1;
                    float x = float(T.x + i), z = float(T.z + j);
                    float v = 0;
                    for (uint o = 0; o < n; o++)
                        v += oct[o].amp * sampleP(pn[o], perm[o], x * oct[o].lac, z * oct[o].lac);
                    float xb = x * f, zb = z * f;
                    for (uint o = 0; o < n; o++)
                        v += oct[o].amp * sampleP(pn[n+o], perm[n+o], xb * oct[o].lac, zb * oct[o].lac);
                    v *= 10000.0f * T.dpamp;
                    vmin = min(vmin, v);
                    vmax = max(vmax, v);
                    if (vmax + T.margin >= T.lmin && vmin - T.margin <= T.lmax)
                    {
                        overlap = true;
                        break;
                    }
                    if (i == T.w - 1) break;
                }
                if (j == T.h - 1) break;
            }
            if (!overlap)
                reject = vmax + T.bound[lv] < T.lmin || vmin - T.bound[lv] > T.lmax;
        }
        if (reject)
            return; // the seed fails this test on the CPU as well
    }
    uint k = atomic_fetch_add_explicit(npass, 1u, memory_order_relaxed);
    passed[k] = seed;
}
)METAL"
