#ifndef DPEVAL_H
#define DPEVAL_H

// The arithmetic of one density function node, shared by the climate
// evaluator (dpdensity.cpp) and the terrain evaluator (dpterrain.cpp), for
// every version up to 26.2 (26.3 graphs are evaluated in float by
// DensityState::run26() in dpformats.cpp).
// Internal to src/datapack. Like the rest of the generator this must be
// compiled without fused multiply-adds (-ffp-contract=off).

#include "dpdensity.h"

extern "C"
{
// defined in the engine's biomenoise.c, not declared in its headers
float getEndHeightNoise(const EndNoise *en, int x, int z, int range);
}

#include <cmath>

namespace dp
{

inline double javaMin(double a, double b)
{
    if (a != a)
        return a;
    if (a == 0.0 && b == 0.0 && std::signbit(b))
        return b;
    return a <= b ? a : b;
}

inline double javaMax(double a, double b)
{
    if (a != a)
        return a;
    if (a == 0.0 && b == 0.0 && std::signbit(a))
        return b;
    return a >= b ? a : b;
}

// Mth.clampedLerp(start, end, delta)
inline double clampedLerpD(double start, double end, double delta)
{
    if (delta < 0.0)
        return start;
    if (delta > 1.0)
        return end;
    return start + delta * (end - start);
}

// Mth.clampedMap(value, fromMin, fromMax, toMin, toMax)
inline double clampedMap(double v, double e, double f, double g, double h)
{
    return clampedLerpD(g, h, (v - e) / (f - e));
}

// DensityState::compute(): see dpdensity.h
inline double DensityState::compute(int32_t idx, const double *r, int x, int y, int z) const
{
    const DensityNode& n = graph.nodes[idx];
    const int yy = (n.flags & DF_FLAT) ? 0 : y;
    // x and z only matter to the leaves that sample a position
#define DP_QUART() do { if (n.flags & DF_QUART) { x &= ~3; z &= ~3; } } while (0)
    switch ((int) n.op)
    {
    case OP_Y_GRADIENT:
        return clampedMap((double) yy, n.p[0], n.p[1], n.p[2], n.p[3]);
    case OP_NOISE:
        DP_QUART();
        return noises[n.noise].sample(x * n.p[0], yy * n.p[1], z * n.p[0]);
    case OP_SHIFTED_NOISE:
        DP_QUART();
        return noises[n.noise].sample(x * n.p[0] + r[n.a], yy * n.p[1] + r[n.b], z * n.p[0] + r[n.c]);
    case OP_SHIFT_A:
        DP_QUART();
        return noises[n.noise].sample(x * 0.25, 0.0, z * 0.25) * 4.0;
    case OP_SHIFT_B:
        DP_QUART();
        return noises[n.noise].sample(z * 0.25, x * 0.25, 0.0) * 4.0;
    case OP_SHIFT:
        DP_QUART();
        return noises[n.noise].sample(x * 0.25, yy * 0.25, z * 0.25) * 4.0;
    case OP_ADD:
        return r[n.a] + r[n.b];
    case OP_ADD_CONST:
        return r[n.a] + n.p[0];
    case OP_MUL:
        return r[n.a] == 0.0 ? 0.0 : r[n.a] * r[n.b];
    case OP_MUL_CONST:
        return r[n.a] * n.p[0];
    case OP_MIN:
        return javaMin(r[n.a], r[n.b]);
    case OP_MAX:
        return javaMax(r[n.a], r[n.b]);
    case OP_ABS:
        return std::fabs(r[n.a]);
    case OP_SQUARE:
        return r[n.a] * r[n.a];
    case OP_CUBE:
        return r[n.a] * r[n.a] * r[n.a];
    case OP_HALF_NEGATIVE:
        return r[n.a] > 0.0 ? r[n.a] : r[n.a] * 0.5;
    case OP_QUARTER_NEGATIVE:
        return r[n.a] > 0.0 ? r[n.a] : r[n.a] * 0.25;
    case OP_SQUEEZE:
    {
        double a = r[n.a];
        double e = a < -1.0 ? -1.0 : javaMin(a, 1.0);   // Mth.clamp
        return e / 2.0 - e * e * e / 24.0;
    }
    case OP_INVERT:
        return 1.0 / r[n.a];
    case OP_CLAMP:
        return r[n.a] < n.p[0] ? n.p[0] : javaMin(r[n.a], n.p[1]);
    case OP_RANGE_CHOICE:
        return (r[n.a] >= n.p[0] && r[n.a] < n.p[1]) ? r[n.b] : r[n.c];
    case OP_SPLINE:
        return (double) spline(n.spline, r);
    case OP_BLENDED_NOISE:
        DP_QUART();
        return blendedNoise(n, x, yy, z);
    case OP_BLENDED_NOISE_118:
        DP_QUART();
        return blendedNoise118(n, x, yy, z);
    case OP_PEAKS_VALLEYS:
    {
        // TerrainShaper.peaksAndValleys(), in float
        const float w = (float) r[n.a];
        return (double) (-(std::fabs(std::fabs(w) - 0.6666667F) - 0.33333334F) * 3.0F);
    }
    case OP_SHAPER_SPLINE:
    {
        // the shaper's spline (plus the global offset for "offset"),
        // Mth.clamp()
        const float f = spline(n.spline, r) + (float) n.p[2];
        const double d = (double) f;
        return d < n.p[0] ? n.p[0] : d > n.p[1] ? n.p[1] : d;
    }
    case OP_SLIDE:
    {
        // NoiseRouterData.applySlide() of 1.18.2
        const double *e = &graph.extras[n.extra];
        const int cellHeight = (int) e[0];
        const double d = (double) (yy / cellHeight - (int) e[1]);
        double v = r[n.a];
        if (e[4] > 0)
            v = clampedLerpD(e[3], v, (e[2] - d - e[5]) / e[4]);
        if (e[7] > 0)
            v = clampedLerpD(e[6], v, (d - e[8]) / e[7]);
        return v;
    }
    case OP_WEIRD_SCALED:
    {
        DP_QUART();
        double a = r[n.a];
        double e;
        if (n.p[0] == 1)    // RarityValueMapper.TYPE1 (spaghetti 3D)
            e = a < -0.5 ? 0.75 : a < 0.0 ? 1.0 : a < 0.5 ? 1.5 : 2.0;
        else                // TYPE2 (spaghetti 2D)
            e = a < -0.75 ? 0.5 : a < -0.5 ? 0.75 : a < 0.5 ? 1.0 : a < 0.75 ? 2.0 : 3.0;
        return e * std::fabs(noises[n.noise].sample(x / e, yy / e, z / e));
    }
    case OP_END_ISLANDS:
        DP_QUART();
        // the island height of the 8 block cell (Java's truncating
        // division), minus 8, over 128
        return ((double) getEndHeightNoise(&endNoise, x / 8, z / 8, 0) - 8.0) / 128.0;
    case OP_INTERPOLATED:
    case OP_CACHE_2D:
        return r[n.a];
    default:
        return 0.0;
    }
#undef DP_QUART
}

} // namespace dp

#endif // DPEVAL_H
