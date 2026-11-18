#ifndef DPDENSITY_H
#define DPDENSITY_H

// Density functions: the worldgen JSON compiled into a node graph (shared by
// all samplers of a world, immutable) and the per-seed state that evaluates
// it (noises, registers).
//
// Internal to src/datapack.

#include "dppack.h"

extern "C"
{
#include "noise.h"
#include "biomenoise.h"
}

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <atomic>
#include <cstdint>
#include <vector>

namespace dp
{

//==============================================================================
// noises
//==============================================================================

// A noise of the registry worldgen/noise, with what its seeding needs
struct NoiseDef
{
    QString id;
    int firstOctave = 0;
    std::vector<double> amplitudes;     // 26.3: the amplitude modifiers (1 where none are given)
    uint64_t hash[2] = {};      // md5 of the id, as two big-endian longs
    // 26.3 format: base_amplitude, normalize (0 off, 1 on, 2 legacy)
    bool v26 = false;
    double baseAmplitude = 1.0;
    int normalize = 1;
};

// One level of the game's PerlinNoise. The engine's PerlinNoise holds the
// permutation; its fields amplitude and lacunarity hold the game's amplitude
// and input factor of the level.
struct NoiseLevel
{
    PerlinNoise perlin;
    double valueFactor;
};

// The game's NormalNoise: two PerlinNoise with a common value factor
struct NormalNoiseState
{
    std::vector<NoiseLevel> first, second;
    double valueFactor = 0;

    double sample(double x, double y, double z) const;

    // 26.3: one stack of layers in float (first and second of each octave)
    struct Layer
    {
        PerlinNoise perlin;     // permutation and offsets
        double frequency;
        float amplitude;
    };
    std::vector<Layer> layers;
    float sample26(double x, double y, double z) const;
    // the same as the game samples a chunk's volume: block coordinate times
    // (scale times layer frequency)
    float sample26Volume(int x, int y, int z, double xzScale, double yScale) const;
};

// 26.3 old_blended_noise: three stacks of smeared Perlin noises in float
struct BlendedNoise26State
{
    struct Layer
    {
        PerlinNoise perlin;
        double frequency;
        float amplitude;
        double smear;
    };
    std::vector<Layer> minLimit, maxLimit, main;
    double xzMul = 0, yMul = 0, xzMain = 0, yMain = 0;
};

// The game's BlendedNoise ("old_blended_noise")
struct BlendedNoiseState
{
    PerlinNoise minLimit[16], maxLimit[16], main[8];
};

// md5 of a name as the two longs the game seeds a random with
void hashOf(const QByteArray& name, uint64_t out[2]);

// Java's String.hashCode() of the UTF-16 form of @s
int32_t javaStringHash(const QString& s);

// The game's Mth.floor(): before 26.1 (int) d minus one below it (d2i
// saturates, NaN gives 0, -1e20 gives INT_MIN - 1, which wraps), since 26.1
// (int) Math.floor(d)
int floorOld(double d);
int floorSat(double d);

//==============================================================================
// graph
//==============================================================================

enum DensityOp : uint8_t
{
    OP_CONSTANT,
    OP_Y_GRADIENT,      // y_clamped_gradient
    OP_NOISE,
    OP_SHIFTED_NOISE,   // a, b, c: shift x, y, z
    OP_SHIFT_A,
    OP_SHIFT_B,
    OP_SHIFT,
    OP_ADD, OP_MUL, OP_MIN, OP_MAX,
    OP_ABS, OP_SQUARE, OP_CUBE, OP_HALF_NEGATIVE, OP_QUARTER_NEGATIVE,
    OP_SQUEEZE, OP_INVERT,
    OP_CLAMP,
    OP_RANGE_CHOICE,    // a: input, b: in range, c: out of range
    OP_SPLINE,
    OP_BLENDED_NOISE,   // old_blended_noise
    OP_WEIRD_SCALED,    // weird_scaled_sampler
    OP_END_ISLANDS,
    OP_FIND_TOP_SURFACE,
    // "interpolated" (up to 26.2): a: the argument. Its value is the
    // argument's, except where the terrain fill interpolates it between the
    // corners of the noise cells (dpterrain.cpp).
    OP_INTERPOLATED,
    // 1.18.2: bound to the noise settings that use the function
    OP_PEAKS_VALLEYS,   // TerrainShaper.peaksAndValleys((float) a), in float
    OP_SHAPER_SPLINE,   // terrain_shaper_spline: p[0] min, p[1] max, p[2] added in float
    OP_SLIDE,           // slide: a input, extra: the slide parameters
    OP_BLENDED_NOISE_118,   // old_blended_noise of 1.18.2, extra: its parameters
    // 26.3 (dpformats.cpp); every op of a 26.3 graph is evaluated in float
    OP_GRADIENT,        // p[0] from coordinate, p[1] to coordinate, p[2] from value, p[3] slope, p[4] axis * 4 + tiling
    OP_SQRT, OP_NEGATE, OP_LOG, OP_SIGN,
    OP_ROUND,           // a input, b multiple (-1: 1), p[0] kind (floor, round, ceil, truncate)
    OP_SUB, OP_DIV,
    OP_POW,             // a base, b exponent
    OP_LERP,            // a alpha, b first, c second
    OP_DISTANCE,        // p[0..2] point, p[3] metric
    OP_RELOCATE,        // slice: a input at another coordinate, p[0] axis mask (1 x, 2 y, 4 z), p[1] coordinate
    OP_INTERPOLATE,     // interpolated: a input, p[0] cell size xz, p[1] cell size y
    OP_END_OUTER,       // end_outer_islands
    OP_BLENDED_NOISE_26,    // old_blended_noise of 26.3, extra: its parameters
    // cache_2d where compileSurface() keeps it: a: the argument, whose value
    // it has (the preliminary surface scan takes the argument's value at the
    // scan's first height, as the game's Cache2D keeps it per column)
    OP_CACHE_2D,

    // internal ops after the public ones: add and mul with a literal
    // constant (the game's MulOrAdd), p[0] is the constant
    OP_ADD_CONST = 64,
    OP_MUL_CONST,
};

enum
{
    // The node is inside a flat_cache: it is evaluated at y 0.
    DF_FLAT     = 1,
    // The value changes with y (set by the compiler).
    DF_USES_Y   = 2,
    // Inside a flat_cache: the game samples it at the quart position, so
    // x and z are rounded down to multiples of 4 (DF_FLAT is not enough to
    // tell: it is also set for noises with a y scale of 0, and not for the
    // shifts).
    DF_QUART    = 4,
};

struct DensityNode
{
    DensityOp op = OP_CONSTANT;
    uint8_t flags = 0;
    int32_t a = -1, b = -1, c = -1;     // argument nodes
    int32_t noise = -1;                 // index into DensityGraph::noises
    int32_t spline = -1;                // index into DensityGraph::splines
    double p[5] = {};                   // parameters, by op
    int32_t extra = -1;                 // first value in DensityGraph::extras, by op
};

struct SplinePoint
{
    float location;
    float derivative;
    float value;        // if spline < 0
    int32_t spline;     // nested spline, or -1
};

struct SplineDef
{
    int32_t coordinate;     // node whose value is the coordinate
    int32_t first, count;   // points in DensityGraph::points
};

class DensityGraph
{
public:
    DensityGraph(const Resources& res, Report *report, int mc);

    // Compiles the density function @v (a reference, a number or an object)
    // and returns its node, or -1 after reporting an error. @pack and @id
    // name the file the value is from, for messages. @settings: the noise
    // settings it is part of (an index from legacySettings(), or -1): in
    // 1.18.2 terrain_shaper_spline, slide and old_blended_noise take their
    // parameters from the noise settings that use them.
    int compile(const QJsonValue& v, int pack, const QString& id, int settings = -1);
    // The same as compile(), for the game's plain climate sampler
    // (RandomState.sampler(): structure biome checks, the stronghold search)
    // instead of the climate sampler of a chunk: there flat_cache does not
    // cache, so its argument is evaluated at the sampled y too.
    int compilePlain(const QJsonValue& v, int pack, const QString& id, int settings = -1);
    // compilePlain() that keeps cache_2d as OP_CACHE_2D nodes (the
    // preliminary surface of the aquifers, dpaquifer.cpp)
    int compileSurface(const QJsonValue& v, int pack, const QString& id, int settings = -1);
    // Compiles the registry entry @id.
    int compileReference(const QString& id, int pack, const QString& from);
    // Parses the noise @id of the registry, reporting an error if it is broken.
    void checkNoise(const QString& id);
    // 1.18.2: registers the noise settings @id (the JSON object) for
    // compile(), returns its index. Other versions return -1.
    int legacySettings(const QString& id, const QJsonObject& settings);

    std::vector<DensityNode> nodes;
    std::vector<SplineDef> splines;
    std::vector<SplinePoint> points;
    std::vector<NoiseDef> noises;
    std::vector<double> extras;         // parameter blocks of nodes (DensityNode::extra)
    // per node: the length of the longest path from it to a leaf (1 for a
    // leaf); per spline: the same through its coordinates and nested splines
    std::vector<int32_t> heights;
    std::vector<int32_t> splineHeights;
    // per node: how many OP_INTERPOLATE (26.3) are nested on a path from it
    // (spline coordinates are not followed: interpolated is not allowed there)
    std::vector<int32_t> interpNesting;

    // The deepest nesting compile() accepts: the height of a compiled node
    // (the evaluators recurse along it) and the recursion of the compiler
    // (JSON values and splines inside each other; a function in another file
    // counts twice, the reference and the file's value). Vanilla's highest
    // function is 40 nodes high (the compiler 30 levels deep), the highest of
    // the packs tried 63 (46). At the limit the compiler needs about 300 KiB
    // of stack; the loader's thread has 512 KiB on macOS.
    static constexpr int MAX_DEPTH = 256;
    static constexpr int MAX_COMPILE_DEPTH = 256;
    // the deepest recursion of the compiler so far (for tests)
    int deepest = 0;

    // All argument nodes of a node (including spline coordinates)
    void arguments(int node, std::vector<int>& out) const;

private:
    const Resources& res;
    Report *report;
    int mc;

    struct Context
    {
        int pack;
        QString id;
        bool flat;
        bool plain = false;     // see compilePlain()
        bool keepCache2d = false;   // see compileSurface()
        int settings = -1;      // index into legacy, or -1
    };
    // 1.18.2 noise settings: what terrain_shaper_spline, slide and
    // old_blended_noise take from them
    struct Legacy
    {
        QString id;
        QJsonObject shaper;     // noise.terrain_shaper
        double slide[9];        // cell height, min cell y, cell count y, top target, size, offset, bottom ...
        double blended[6];      // xz scale, y scale, xz main scale, y main scale, cell width, cell height
    };
    std::vector<Legacy> legacy;
    int compileValue(const QJsonValue& v, const Context& ctx);
    int compileValueAt(const QJsonValue& v, const Context& ctx);
    int compileObject(const QJsonObject& o, const Context& ctx);
    int compileObjectOld(const QString& type, const QJsonObject& o, const Context& ctx);
    int compileRef(const QString& id, const Context& ctx);
    // @names: coordinates given by name (the terrain shaper of 1.18.2)
    int compileSpline(const QJsonValue& v, const Context& ctx, bool *ok,
        const QHash<QString, int> *names = nullptr);
    int compileSplineAt(const QJsonValue& v, const Context& ctx, bool *ok,
        const QHash<QString, int> *names);
    // interval_select over fn[lo .. hi] with the thresholds th between them
    int intervalSelect(int input, const std::vector<int>& fn, const std::vector<double>& th, int lo, int hi);
    QString tooDeep() const;
    int depth = 0;                      // compileValue() / compileSpline() recursion
    bool depthReported = false;
    int compileLegacy(const QString& type, const QJsonObject& o, const Context& ctx, bool *handled);
    int compile26(const QString& type, const QJsonObject& o, const Context& ctx);
    bool isConstant26(const QJsonValue& v, int depth = 0) const;
    bool parseNoise26(const QJsonObject& o, NoiseDef *def, QString *err) const;
    int addExtras(const double *v, int n);
    int noiseIndex(const QJsonValue& v, const Context& ctx);
    int addNode(const DensityNode& n);
    int constant(double v);
    void error(const Context& ctx, const QString& text);
    void splineArguments(int spline, std::vector<int>& out) const;

    QHash<QString, int> refs;           // "<id>|<flat>" -> node, -1 failed, -2 in progress
    QHash<QByteArray, int> unique;      // node content -> node
    QHash<QString, int> noiseIds;       // noise id -> index, -1 failed
    QHash<QByteArray, int> splineUnique; // spline content -> index
};

//==============================================================================
// evaluation
//==============================================================================

// The nodes a set of outputs needs, in evaluation order, split into the part
// that only depends on the column (x, z) and the part that changes with y.
struct DensityProgram
{
    std::vector<int32_t> column;    // node indices
    std::vector<int32_t> vertical;
    std::vector<int32_t> usedNoises;
    // find_top_surface nodes: the nodes of their density that change with y
    struct Scan
    {
        int32_t node;
        std::vector<int32_t> nodes;
    };
    std::vector<Scan> scans;
    // slice and interpolated (26.3): all nodes of the argument, evaluated
    // again at another position
    std::vector<Scan> relocs;
    bool usesBlended = false;
    bool usesEndIslands = false;

    void build(const DensityGraph& graph, const std::vector<int>& outputs);
};

// Per-seed state of a graph. Not thread safe: one per sampler.
class DensityState
{
public:
    // @legacy: the noise settings have legacy_random_source (the game's
    // LegacyRandomSource and the noises special to it)
    DensityState(const DensityGraph& graph, const DensityProgram& program, uint64_t seed, int mc,
        bool legacy = false);

    // Evaluates the column part at block (x, z), then the vertical part at y.
    void evalColumn(int x, int z);
    void evalVertical(int y);
    double value(int node) const { return reg[node]; }

    // The value of the node @idx at (x, y, z) from the values @r of its
    // arguments (the registers of the caller). Not for OP_FIND_TOP_SURFACE
    // (see findTopSurface()) and OP_CONSTANT (0).
    inline double compute(int32_t idx, const double *r, int x, int y, int z) const;
    // OP_FIND_TOP_SURFACE @idx in the column (x, z) below the upper bound
    // @upper: evaluates the column part of the program on its own registers.
    double findTopSurface(int32_t idx, int x, int z, double upper);

    // Checked in long loops (find_top_surface): when it is set the loop
    // stops early and the values are wrong; the caller then reports that it
    // was aborted. The caller sets it for the duration of a call.
    const std::atomic_bool *abort = nullptr;

    // The values of the column part can be kept and put back, to return to a
    // column without sampling its noises again.
    size_t columnSize() const { return program.column.size(); }
    void saveColumn(double *dst) const;
    void loadColumn(const double *src, int x, int z);

private:
    const DensityGraph& graph;
    const DensityProgram& program;
    std::vector<double> reg;                // one value per graph node
    std::vector<NormalNoiseState> noises;   // by DensityGraph::noises index
    BlendedNoiseState blended;
    std::vector<BlendedNoise26State> blended26;     // by DensityNode::extra of the node
    EndNoise endNoise;
    int cx = 0, cz = 0;
    bool f32 = false;       // 26.3: float evaluation (run26)
    bool floorSaturates = false;    // Mth.floor() of 26.1 and later

    friend class TerrainState;
    friend class AquiferState;

    void run(const int32_t *list, size_t count, int y);
    void run26(const int32_t *list, size_t count, int y);
    float evalAt(const DensityProgram::Scan& s, int32_t node, int x, int y, int z);
    void init26(uint64_t lo, uint64_t hi, uint64_t seed);
    float spline(int index, const double *r) const;
    double blendedNoise(const DensityNode& n, int x, int y, int z) const;
    double blendedNoise118(const DensityNode& n, int x, int y, int z) const;
};

// Sets DensityState::abort for a scope (and puts the previous one back). A
// null flag keeps the one of an outer scope: StructureFinder::starts() sets
// it, its height checks call Sampler::genHeights() without one.
class AbortScope
{
public:
    AbortScope(DensityState *state, const std::atomic_bool *a)
        : s(a ? state : nullptr), old(s ? s->abort : nullptr)
    {
        if (s)
            s->abort = a;
    }
    ~AbortScope()
    {
        if (s)
            s->abort = old;
    }
private:
    AbortScope(const AbortScope&) = delete;
    AbortScope& operator=(const AbortScope&) = delete;
    DensityState *s;
    const std::atomic_bool *old;
};

} // namespace dp

#endif // DPDENSITY_H
