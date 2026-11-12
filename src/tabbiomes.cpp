#include "tabbiomes.h"
#include "ui_tabbiomes.h"

#include "message.h"
#include "util.h"
#include "resultlist.h"

#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QScrollBar>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

struct BiomeCenter
{
    Pos pos;
    int size;
    int biome;          // index in the list of searched biomes
    int firstx, firstz; // first cell of the area in row-major order
};

static uint64_t greatestCommonDivisor(uint64_t a, uint64_t b)
{
    while (b)
    {
        uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

// inverse of a modulo n (gcd(a, n) == 1)
static uint64_t inverseModulo(uint64_t a, uint64_t n)
{
    __int128 t = 0, newt = 1, r = n, newr = a % n;
    while (newr != 0)
    {
        __int128 q = r / newr, tmp;
        tmp = t - q * newt; t = newt; newt = tmp;
        tmp = r - q * newr; r = newr; newr = tmp;
    }
    if (t < 0)
        t += n;
    return uint64_t(t);
}

/*
 * Splits a large area into tiles for the workers. The results of the
 * statistics do not depend on the tiling.
 */
static int mappingTileSize(int64_t width, int64_t height, int minimum, int maximum)
{
    const uint64_t area = uint64_t(width) * uint64_t(height);
    const uint64_t targetJobs = std::max<uint64_t>(1, mappingThreadCount() * 8);
    const double ideal = std::sqrt(double(area) / double(targetJobs));
    return std::max(minimum, std::min(int(std::ceil(ideal)), maximum));
}

// The y of a range: in blocks at 1:1, otherwise in 1:4 cells (as the map).
static int rangeY(int scale, int y)
{
    return scale == 1 ? y : y >> 2;
}

/*
 * Finds the connected areas (4-neighbourhood) of the searched biomes, with
 * the same result as getBiomeCenters() over the whole range with a tolerance
 * of one: every area with its exact size and centre, in the order of the
 * searched biomes and then of the first cell of the area in rows. The area
 * is processed in fixed tiles by all workers; areas that reach a tile edge
 * are joined with those of the neighbour tile (union-find) once both tiles
 * are done, so the result does not depend on the tiling or the threads.
 * All searched biomes are located in one pass.
 */
class AreaLocator
{
public:
    enum { TILE = 256, NONE = 255 };
    // Tiles are handed out in blocks of BLOCK x BLOCK, so the areas that
    // cross tile edges are completed soon, also in very wide searches.
    enum { BLOCK = 16 };

    AreaLocator(const WorldInfo& wi, int dim, const QVector<int>& biomes,
            int x1, int z1, int x2, int z2, int y, int minsize,
            std::atomic_bool *stop, std::atomic_uint64_t *done,
            std::atomic_uint64_t *total, std::atomic_uint64_t *resultCount,
            MappingWorkerState *workerState, std::atomic_bool *failed)
        : wi(wi), dim(dim), x1(x1), z1(z1), x2(x2), z2(z2), y(y)
        , minsize(minsize), stop(stop), done(done), total(total)
        , resultCount(resultCount), workerState(workerState), failed(failed)
    {
        std::fill(std::begin(index), std::end(index), uint8_t(NONE));
        uint64_t set[4] = {};
        for (int i = 0; i < biomes.size() && i < NONE; i++)
        {
            const int id = biomes[i];
            if (id >= 0 && id < 256)
            {
                index[id] = uint8_t(i);
                set[id >> 6] |= 1ULL << (id & 63);
            }
        }
        climate = dim == DIM_OVERWORLD && wi.mc >= MC_1_18;
        if (climate && getBiomeClimateBounds(&bounds, wi.mc, set))
            nparas = getClimateBoundParas(&bounds, paras);
        tilesX = (uint64_t(int64_t(x2) - x1) + TILE) / TILE;
        tilesZ = (uint64_t(int64_t(z2) - z1) + TILE) / TILE;
        blocksX = (tilesX + BLOCK - 1) / BLOCK;
        blocksZ = (tilesZ + BLOCK - 1) / BLOCK;
    }

    QVector<BiomeCenter> run()
    {
        const uint64_t tileCount = tilesX * tilesZ;
        const uint64_t slotCount = blocksX * blocksZ * BLOCK * BLOCK;
        total->fetch_add(tileCount, std::memory_order_relaxed);
        std::atomic_uint64_t nextTile(0);
        auto scanTiles = [&](size_t) {
            try
            {
                Generator g;
                setupGenerator(&g, wi.mc, wi.large);
                applySeed(&g, dim, wi.seed);
                Tile tile;
                while (!stopped())
                {
                    const uint64_t slot = nextTile.fetch_add(1, std::memory_order_relaxed);
                    if (slot >= slotCount)
                        break;
                    const uint64_t block = slot / (BLOCK * BLOCK);
                    const uint64_t local = slot % (BLOCK * BLOCK);
                    const uint64_t tx = (block % blocksX) * BLOCK + local % BLOCK;
                    const uint64_t tz = (block / blocksX) * BLOCK + local / BLOCK;
                    if (tx >= tilesX || tz >= tilesZ)
                        continue;
                    if (!scanTile(&g, tz * tilesX + tx, tile))
                        break;
                    merge(tile);
                    done->fetch_add(1, std::memory_order_relaxed);
                }
            }
            catch (const std::bad_alloc&)
            {
                failed->store(true, std::memory_order_relaxed);
            }
        };
        runMappingWorkers(tileCount, *workerState, scanTiles);
        if (stopped())
            return {};
        std::sort(results.begin(), results.end(),
            [](const BiomeCenter& a, const BiomeCenter& b) {
                return std::tie(a.biome, a.firstz, a.firstx) <
                    std::tie(b.biome, b.firstz, b.firstx);
            });
        return results;
    }

private:
    struct Area
    {
        int64_t sumx, sumz;
        int64_t n;
        int firstx, firstz;
        int biome;
        int edges; // bit 0..3: touches the top, bottom, left, right neighbour tile
    };
    struct Tile
    {
        uint64_t index;
        int tx, tz, x, z, w, h;
        std::vector<uint8_t> cls;
        std::vector<int> label;
        std::vector<Area> areas;
        std::vector<int> stack;
    };
    struct Borders
    {
        int waiting = 0; // edges whose neighbour tile is not done yet
        // global area of each cell along such an edge (empty: no area)
        std::vector<int> edge[4];
    };
    enum { TOP, BOTTOM, LEFT, RIGHT };

    bool stopped() const
    {
        return stop->load(std::memory_order_relaxed) || failed->load(std::memory_order_relaxed);
    }

    bool scanTile(Generator *g, uint64_t t, Tile& tile)
    {
        tile.index = t;
        tile.tx = int(t % tilesX);
        tile.tz = int(t / tilesX);
        tile.x = int(int64_t(x1) + int64_t(tile.tx) * TILE);
        tile.z = int(int64_t(z1) + int64_t(tile.tz) * TILE);
        tile.w = int(std::min<int64_t>(TILE, int64_t(x2) - tile.x + 1));
        tile.h = int(std::min<int64_t>(TILE, int64_t(z2) - tile.z + 1));
        const size_t cells = size_t(tile.w) * size_t(tile.h);
        tile.cls.assign(cells, uint8_t(NONE));
        tile.label.assign(cells, -1);
        tile.areas.clear();

        if (climate)
        {   // exact: the biome as getBiomeAt(), after the exact climate bounds
            for (int j = 0; j < tile.h; j++)
            {
                if (stopped())
                    return false;
                for (int i = 0; i < tile.w; i++)
                {
                    const int x = tile.x + i, z = tile.z + j;
                    if (nparas && !climateMayBeWithin(&g->bn, &bounds, paras, nparas, x, z, 0))
                        continue;
                    const int id = sampleBiomeNoise(&g->bn, NULL, x, y, z, NULL, 0);
                    if (id >= 0 && id < 256)
                        tile.cls[size_t(j) * tile.w + i] = index[id];
                }
            }
        }
        else
        {
            Range r = {4, tile.x, tile.z, tile.w, tile.h, y, 1};
            int *ids = allocCache(g, r);
            if (!ids)
                throw std::bad_alloc();
            genBiomes(g, ids, r);
            for (size_t k = 0; k < cells; k++)
                if (ids[k] >= 0 && ids[k] < 256)
                    tile.cls[k] = index[ids[k]];
            free(ids);
            if (stopped())
                return false;
        }

        // label the areas inside the tile
        const bool up = tile.tz > 0, down = uint64_t(tile.tz) + 1 < tilesZ;
        const bool left = tile.tx > 0, right = uint64_t(tile.tx) + 1 < tilesX;
        for (int j = 0; j < tile.h; j++)
        {
            for (int i = 0; i < tile.w; i++)
            {
                const size_t k0 = size_t(j) * tile.w + i;
                if (tile.cls[k0] == NONE || tile.label[k0] >= 0)
                    continue;
                const int id = int(tile.areas.size());
                Area a = {0, 0, 0, tile.x + i, tile.z + j, tile.cls[k0], 0};
                tile.stack.clear();
                tile.stack.push_back(int(k0));
                tile.label[k0] = id;
                while (!tile.stack.empty())
                {
                    const int k = tile.stack.back();
                    tile.stack.pop_back();
                    const int ci = k % tile.w, cj = k / tile.w;
                    a.n++;
                    a.sumx += tile.x + ci;
                    a.sumz += tile.z + cj;
                    if (cj == 0 && up) a.edges |= 1 << TOP;
                    if (cj == tile.h-1 && down) a.edges |= 1 << BOTTOM;
                    if (ci == 0 && left) a.edges |= 1 << LEFT;
                    if (ci == tile.w-1 && right) a.edges |= 1 << RIGHT;
                    const int next[4][2] = {{ci, cj-1}, {ci, cj+1}, {ci-1, cj}, {ci+1, cj}};
                    for (const auto& nb : next)
                    {
                        if (nb[0] < 0 || nb[0] >= tile.w || nb[1] < 0 || nb[1] >= tile.h)
                            continue;
                        const size_t kk = size_t(nb[1]) * tile.w + nb[0];
                        if (tile.label[kk] >= 0 || tile.cls[kk] != a.biome)
                            continue;
                        tile.label[kk] = id;
                        tile.stack.push_back(int(kk));
                    }
                }
                tile.areas.push_back(a);
            }
        }
        return true;
    }

    int find(int a)
    {
        while (parent[a] != a)
        {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    }

    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a == b)
            return;
        if (areas[a].n < areas[b].n)
            std::swap(a, b);
        parent[b] = a;
        Area& ra = areas[a];
        const Area& rb = areas[b];
        ra.n += rb.n;
        ra.sumx += rb.sumx;
        ra.sumz += rb.sumz;
        if (std::tie(rb.firstz, rb.firstx) < std::tie(ra.firstz, ra.firstx))
        {
            ra.firstz = rb.firstz;
            ra.firstx = rb.firstx;
        }
        open[a] += open[b];
    }

    void finish(const Area& a)
    {   // as getBiomeCenters() computes the centre
        if (a.n < minsize)
            return;
        BiomeCenter c;
        c.pos.x = (int) round((a.sumx / (double)a.n + 0.5) * 4);
        c.pos.z = (int) round((a.sumz / (double)a.n + 0.5) * 4);
        c.size = int(std::min<int64_t>(a.n, INT_MAX));
        c.biome = a.biome;
        c.firstx = a.firstx;
        c.firstz = a.firstz;
        results.push_back(c);
        resultCount->fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<int> edgeCells(const Tile& tile, int edge, const std::vector<int>& global) const
    {
        std::vector<int> v;
        const int n = (edge == TOP || edge == BOTTOM) ? tile.w : tile.h;
        v.resize(n);
        for (int k = 0; k < n; k++)
        {
            int i = 0, j = 0;
            switch (edge)
            {
            case TOP:    i = k; j = 0; break;
            case BOTTOM: i = k; j = tile.h-1; break;
            case LEFT:   i = 0; j = k; break;
            case RIGHT:  i = tile.w-1; j = k; break;
            }
            const int l = tile.label[size_t(j) * tile.w + i];
            v[k] = l < 0 ? -1 : global[l];
        }
        return v;
    }

    static std::vector<int> unique(std::vector<int> v)
    {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (!v.empty() && v.front() < 0)
            v.erase(v.begin());
        return v;
    }

    void merge(const Tile& tile)
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<int> global(tile.areas.size(), -1);
        std::vector<int> check;
        for (size_t l = 0; l < tile.areas.size(); l++)
        {
            const Area& a = tile.areas[l];
            if (!a.edges)
            {   // complete within the tile
                finish(a);
                continue;
            }
            global[l] = int(areas.size());
            areas.push_back(a);
            parent.push_back(global[l]);
            open.push_back(0);
            final.push_back(0);
            check.push_back(global[l]);
        }

        const int64_t neighbour[4] = {
            int64_t(tile.index) - int64_t(tilesX), int64_t(tile.index) + int64_t(tilesX),
            int64_t(tile.index) - 1, int64_t(tile.index) + 1 };
        const bool exists[4] = {
            tile.tz > 0, uint64_t(tile.tz) + 1 < tilesZ,
            tile.tx > 0, uint64_t(tile.tx) + 1 < tilesX };
        const int opposite[4] = {BOTTOM, TOP, RIGHT, LEFT};

        // Every finished tile stays registered until all of its neighbours
        // are done as well, so that they know which edges can be joined.
        Borders mine;
        std::vector<int> cells[4];
        bool ready[4] = {};
        for (int e = 0; e < 4; e++)
        {
            if (!exists[e])
                continue;
            if (!check.empty())
                cells[e] = edgeCells(tile, e, global);
            ready[e] = pending.count(uint64_t(neighbour[e])) != 0;
            if (!ready[e])
            {   // the edge waits for the neighbour tile
                mine.waiting |= 1 << e;
                const std::vector<int> touching = unique(cells[e]);
                for (int a : touching)
                    open[a]++;
                if (!touching.empty())
                    mine.edge[e] = cells[e];
            }
        }
        for (int e = 0; e < 4; e++)
        {
            if (!ready[e])
                continue;
            auto it = pending.find(uint64_t(neighbour[e]));
            std::vector<int> other;
            other.swap(it->second.edge[opposite[e]]);
            it->second.waiting &= ~(1 << opposite[e]);
            if (!cells[e].empty())
            {
                for (size_t k = 0; k < cells[e].size() && k < other.size(); k++)
                {
                    const int a = cells[e][k], b = other[k];
                    if (a >= 0 && b >= 0 && areas[a].biome == areas[b].biome)
                        unite(a, b);
                }
            }
            for (int b : unique(other))
            {
                open[find(b)]--;
                check.push_back(b);
            }
            if (!it->second.waiting)
                pending.erase(it);
        }
        if (mine.waiting)
            pending.emplace(tile.index, std::move(mine));
        for (int a : check)
        {
            const int r = find(a);
            if (!final[r] && open[r] == 0)
            {
                final[r] = 1;
                finish(areas[r]);
            }
        }
    }

    const WorldInfo wi;
    const int dim;
    const int x1, z1, x2, z2, y;
    const int64_t minsize;
    std::atomic_bool *stop;
    std::atomic_uint64_t *done, *total, *resultCount;
    MappingWorkerState *workerState;
    std::atomic_bool *failed;

    uint8_t index[256];
    bool climate = false;
    BiomeClimateBounds bounds;
    int paras[NP_MAX];
    int nparas = 0;
    uint64_t tilesX = 0, tilesZ = 0, blocksX = 0, blocksZ = 0;

    std::mutex mutex;
    std::vector<Area> areas;     // areas that reach a tile edge
    std::vector<int> parent, open;
    std::vector<char> final;
    std::unordered_map<uint64_t, Borders> pending;
    QVector<BiomeCenter> results;
};

/*
 * With a border tolerance the areas depend on the search order, so the
 * reference search runs over the whole range at once (one thread).
 */
static QVector<BiomeCenter> locateWithTolerance(const WorldInfo& wi, int dim,
        const QVector<int>& biomes, int x1, int z1, int x2, int z2, int y,
        int minsize, int tolerance, std::atomic_bool *stop,
        std::atomic_uint64_t *done, std::atomic_uint64_t *total,
        std::atomic_uint64_t *resultCount, std::atomic_bool *failed)
{
    QVector<BiomeCenter> results;
    const int64_t width = int64_t(x2) - x1 + 1;
    const int64_t height = int64_t(z2) - z1 + 1;
    if (width > INT_MAX || height > INT_MAX)
    {
        failed->store(true, std::memory_order_relaxed);
        return results;
    }
    Generator g;
    setupGenerator(&g, wi.mc, wi.large);
    applySeed(&g, dim, wi.seed);
    total->fetch_add(biomes.size(), std::memory_order_relaxed);
    const Range r = {4, x1, z1, int(width), int(height), y, 1};
    const size_t maxEntries = size_t(width) * size_t(height);
    std::vector<Pos> pos(4096);
    std::vector<int> siz(4096);
    for (int b = 0; b < biomes.size(); b++)
    {
        int count = 0;
        for (;;)
        {
            int status = 0;
            count = getBiomeCentersEx(pos.data(), siz.data(), int(pos.size()),
                &g, r, biomes[b], minsize, tolerance, (volatile char*)stop, &status);
            if (status < 0)
            {
                failed->store(true, std::memory_order_relaxed);
                return {};
            }
            if (status != 1 || pos.size() >= maxEntries || stop->load())
                break;
            const size_t capacity = std::min(maxEntries, pos.size() * 2);
            pos.resize(capacity);
            siz.resize(capacity);
        }
        if (stop->load())
            return {};
        for (int i = 0; i < count; i++)
            results.append({pos[i], siz[i], b, 0, 0});
        resultCount->fetch_add(count, std::memory_order_relaxed);
        done->fetch_add(1, std::memory_order_relaxed);
    }
    return results;
}

} // namespace

void AnalysisBiomes::run()
{
    workers.reset();
    workDone.store(0, std::memory_order_relaxed);
    workTotal.store(0, std::memory_order_relaxed);
    resultCount.store(0, std::memory_order_relaxed);
    Generator g;
    setupGenerator(&g, wi.mc, wi.large);

    if (dat.locateBiomes.isEmpty() && seeds.size() > 1)
    {   // With little work per seed the tiles of one seed cannot keep all
        // workers busy: analyse several seeds at once instead.
        const uint64_t cells = uint64_t(int64_t(dat.x2) - dat.x1 + 1) *
            uint64_t(int64_t(dat.z2) - dat.z1 + 1);
        const uint64_t perSeed = std::min(cells, dat.samples);
        if (perSeed < uint64_t(mappingThreadCount()) * 256 * 256)
        {
            runSeedsParallel();
            return;
        }
    }

    for (idx = 0; idx < (long)seeds.size(); idx++)
    {
        if (stop) break;
        wi.seed = seeds[idx];
        if (!dat.locateBiomes.isEmpty())
        {
            if (!runLocate(&g))
                break;
        }
        else
            runStatistics(&g);
    }
}

void AnalysisBiomes::runSeedsParallel()
{
    const size_t count = seeds.size();
    std::vector<QVector<uint64_t>> ready(count);
    std::vector<char> isReady(count, 0);
    std::mutex emitMutex;
    size_t nextEmit = 0;
    std::atomic_size_t nextSeed(0);
    workTotal.fetch_add(count, std::memory_order_relaxed);
    idx = 0;
    runMappingWorkers(count, workers, [&](size_t) {
        while (!stop.load(std::memory_order_relaxed))
        {
            const size_t i = nextSeed.fetch_add(1, std::memory_order_relaxed);
            if (i >= count)
                break;
            QVector<uint64_t> idcnt = countBiomes(seeds[i], false);
            if (stop.load(std::memory_order_relaxed))
                break;
            workDone.fetch_add(1, std::memory_order_relaxed);
            // hand the seeds over in their order
            std::lock_guard<std::mutex> lock(emitMutex);
            ready[i] = idcnt;
            isReady[i] = 1;
            while (nextEmit < count && isReady[nextEmit])
            {
                emit seedDone(seeds[nextEmit], ready[nextEmit]);
                ready[nextEmit] = QVector<uint64_t>();
                nextEmit++;
                idx = long(nextEmit);
            }
        }
    });
}

void AnalysisBiomes::runStatistics(Generator *g)
{
    (void)g;
    QVector<uint64_t> idcnt = countBiomes(wi.seed, true);
    if (!stop) // discard partially processed seed
        emit seedDone(wi.seed, idcnt);
}

QVector<uint64_t> AnalysisBiomes::countBiomes(uint64_t seed, bool parallel)
{
    QVector<uint64_t> idcnt(257);
    const int w = dat.x2 - dat.x1 + 1;
    const int h = dat.z2 - dat.z1 + 1;
    const uint64_t n = w * (uint64_t)h;
    const int y = rangeY(dat.scale, wi.y);

    // runs the jobs on all workers, or here for a seed of a parallel batch
    const auto runJobs = [&](uint64_t jobs, size_t workerCount,
            const std::function<void(size_t)>& job) {
        (void)workerCount;
        if (!jobs)
            return;
        if (parallel)
            runMappingWorkers(jobs, workers, job);
        else
            job(0);
    };

    for (int d = 0; d < 3; d++)
    {
        if (dims[d] == DIM_UNDEF)
            continue;
        const int dim = dims[d];
        // At 1:16 and coarser the generator takes a faster, approximate path
        // for 1.18+ (no shift, search hint): sample the exact biome instead.
        const bool exactNoise = dim == DIM_OVERWORLD && wi.mc >= MC_1_18 && dat.scale > 4;
        // layered versions generate whole areas far faster than single points
        const bool layered = dim == DIM_OVERWORLD && wi.mc <= MC_1_17;

        const uint64_t sampleCount = std::min(dat.samples, n);
        const bool sampling = dat.samples < n;
        const bool sampleByArea = sampling && layered && n / 1024 <= sampleCount;
        uint64_t offset = 0, stride = 1, strideInverse = 1;
        if (sampling)
        {   // a random permutation of the area: sample i is cell (i*stride+offset) % n
            offset = n ? getRnd64() % n : 0;
            stride = n ? getRnd64() % n : 1;
            if (!stride)
                stride = 1;
            while (n > 1 && greatestCommonDivisor(stride, n) != 1)
                stride = stride == n-1 ? 1 : stride+1;
            if (sampleByArea && n > 1)
                strideInverse = inverseModulo(stride, n);
        }

        const size_t workerCount = parallel ? mappingThreadCount() : 1;
        if (!sampling || sampleByArea)
        {   // the area in small cancellable tiles
            const int step = mappingTileSize(w, h, 64, 256);
            const uint64_t tilesX = (uint64_t(w) + step - 1) / step;
            const uint64_t tilesZ = (uint64_t(h) + step - 1) / step;
            const uint64_t tileCount = tilesX * tilesZ;
            if (parallel)
                workTotal.fetch_add(sampleByArea ? sampleCount : tileCount,
                    std::memory_order_relaxed);
            std::atomic_uint64_t nextTile(0);
            std::vector<QVector<uint64_t>> counts(std::min<uint64_t>(tileCount, workerCount),
                QVector<uint64_t>(257));
            auto scanTiles = [&](size_t workerIndex) {
                Generator local;
                setupGenerator(&local, wi.mc, wi.large);
                applySeed(&local, dim, seed);
                QVector<uint64_t>& localCounts = counts[workerIndex];
                const int s = dat.scale / 4, mid = s / 2;
                while (!stop.load(std::memory_order_relaxed))
                {
                    const uint64_t index = nextTile.fetch_add(1, std::memory_order_relaxed);
                    if (index >= tileCount)
                        break;
                    const int tx = int(int64_t(dat.x1) + int64_t(index % tilesX) * step);
                    const int tz = int(int64_t(dat.z1) + int64_t(index / tilesX) * step);
                    const int tw = std::min(step, dat.x2-tx+1);
                    const int th = std::min(step, dat.z2-tz+1);
                    uint64_t sampled = 0;
                    if (exactNoise)
                    {
                        for (int j = 0; j < th; j++)
                            for (int i = 0; i < tw; i++)
                            {
                                const int id = sampleBiomeNoise(&local.bn, NULL,
                                    (tx+i)*s + mid, y, (tz+j)*s + mid, NULL, 0);
                                localCounts[id & 0xff]++;
                            }
                    }
                    else
                    {
                        Range r = {dat.scale, tx, tz, tw, th, y, 1};
                        int *ids = allocCache(&local, r);
                        genBiomes(&local, ids, r);
                        if (stop.load(std::memory_order_relaxed))
                        {
                            free(ids);
                            break;
                        }
                        if (!sampleByArea)
                        {
                            for (int i = 0; i < tw*th; i++)
                                localCounts[ids[i] & 0xff]++;
                        }
                        else
                        {   // only the cells of the permutation's first samples
                            for (int j = 0; j < th; j++)
                                for (int i = 0; i < tw; i++)
                                {
                                    const uint64_t cell = uint64_t(tz - dat.z1 + j) * w +
                                        uint64_t(tx - dat.x1 + i);
                                    const uint64_t rank = uint64_t(
                                        (static_cast<unsigned __int128>((cell + n - offset) % n)
                                            * strideInverse) % n);
                                    if (rank < sampleCount)
                                    {
                                        localCounts[ids[j*tw+i] & 0xff]++;
                                        sampled++;
                                    }
                                }
                        }
                        free(ids);
                    }
                    if (parallel)
                        workDone.fetch_add(sampleByArea ? sampled : 1, std::memory_order_relaxed);
                }
            };
            runJobs(tileCount, workerCount, scanTiles);
            for (const QVector<uint64_t>& localCounts : counts)
                for (int id = 0; id < 256; id++)
                    idcnt[id] += localCounts[id];
        }
        else
        {   // a statistic from the first samples of a random permutation of the area
            if (parallel)
                workTotal.fetch_add(sampleCount, std::memory_order_relaxed);
            std::atomic_uint64_t nextSample(0);
            std::vector<QVector<uint64_t>> counts(std::min<uint64_t>(sampleCount, workerCount),
                QVector<uint64_t>(257));
            auto scanSamples = [&](size_t workerIndex) {
                Generator local;
                setupGenerator(&local, wi.mc, wi.large);
                applySeed(&local, dim, seed);
                QVector<uint64_t>& localCounts = counts[workerIndex];
                uint64_t localDone = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    const uint64_t i = nextSample.fetch_add(1, std::memory_order_relaxed);
                    if (i >= sampleCount)
                        break;
                    const uint64_t index = uint64_t(
                        (static_cast<unsigned __int128>(i) * stride + offset) % n);
                    const int x = int(index % w);
                    const int z = int(index / w);
                    const int id = getBiomeAt(&local, dat.scale,
                        dat.x1+x, y, dat.z1+z);
                    localCounts[id & 0xff]++;
                    if (++localDone == 256)
                    {
                        if (parallel)
                            workDone.fetch_add(localDone, std::memory_order_relaxed);
                        localDone = 0;
                    }
                }
                if (parallel)
                    workDone.fetch_add(localDone, std::memory_order_relaxed);
            };
            runJobs(sampleCount, workerCount, scanSamples);
            for (const QVector<uint64_t>& localCounts : counts)
                for (int id = 0; id < 256; id++)
                    idcnt[id] += localCounts[id];
        }
    }

    int bcnt = 0;
    for (int id = 0; id < 256; id++)
        bcnt += !!idcnt[id];
    idcnt[256] = bcnt;
    return idcnt;
}

bool AnalysisBiomes::runLocate(Generator *g)
{
    (void)g;
    if (dat.locateDim == DIM_END)
        return true;
    std::atomic_bool allocationFailed(false);
    QTreeWidgetItem *seeditem = nullptr;
    const int y = wi.y >> 2;
    QVector<BiomeCenter> centers;
    if (tolerance <= 1)
    {
        AreaLocator locator(wi, dat.locateDim, dat.locateBiomes,
            dat.x1, dat.z1, dat.x2, dat.z2, y, minsize, &stop, &workDone,
            &workTotal, &resultCount, &workers, &allocationFailed);
        centers = locator.run();
    }
    else
    {
        centers = locateWithTolerance(wi, dat.locateDim, dat.locateBiomes,
            dat.x1, dat.z1, dat.x2, dat.z2, y, minsize, tolerance, &stop,
            &workDone, &workTotal, &resultCount, &allocationFailed);
    }
    if (allocationFailed.load(std::memory_order_relaxed))
    {
        emit failed(tolerance <= 1
            ? tr("Not enough memory to complete the biome search.")
            : tr("Not enough memory to complete the biome search. "
                 "With a border tolerance the area is searched at once; "
                 "use a smaller area or no tolerance."));
        return false;
    }
    if (stop)
        return false;
    for (const BiomeCenter& center : std::as_const(centers))
    {
        if (!seeditem)
        {
            seeditem = new QTreeWidgetItem();
            seeditem->setData(0, Qt::DisplayRole, QVariant::fromValue((qlonglong)wi.seed));
            seeditem->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
            seeditem->setData(0, Qt::UserRole+1, QVariant::fromValue(dat.locateDim));
        }
        const int biomeId = dat.locateBiomes[center.biome];
        QTreeWidgetItem* item = new QTreeWidgetItem(seeditem);
        item->setText(0, "-");
        item->setData(1, Qt::DisplayRole, getBiomeDisplay(wi.mc, biomeId));
        item->setData(1, Qt::UserRole, biomeId);
        item->setData(2, Qt::DisplayRole, QVariant::fromValue(center.size));
        item->setData(3, Qt::DisplayRole, QVariant::fromValue(center.pos.x));
        item->setData(4, Qt::DisplayRole, QVariant::fromValue(center.pos.z));
        item->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
        item->setData(0, Qt::UserRole+1, QVariant::fromValue(dat.locateDim));
        item->setData(0, Qt::UserRole+2, QVariant::fromValue(center.pos));
    }
    if (seeditem)
        emit seedItem(seeditem);
    return !stop.load(std::memory_order_relaxed);
}


QVariant BiomeTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();
    static QVariant align = QVariant::fromValue((int)Qt::AlignCenter);
    if (role == Qt::TextAlignmentRole)
        return align;
    if (role == Qt::DisplayRole)
    {
        const int id = ids[index.column()];
        const auto biomes = cnt.constFind(id);
        if (biomes == cnt.cend())
            return QVariant();
        return biomes->value(seeds[index.row()]);
    }
    return QVariant();
}

qulonglong BiomeTableModel::valueAt(int row, int column) const
{
    if (row < 0 || row >= seeds.size() || column < 0 || column >= ids.size())
        return 0;
    const auto biomes = cnt.constFind(ids[column]);
    if (biomes == cnt.cend())
        return 0;
    const auto value = biomes->constFind(seeds[row]);
    return value == biomes->cend() ? 0 : value->toULongLong();
}

bool BiomeSortProxy::lessThan(const QModelIndex& a, const QModelIndex& b) const
{
    const BiomeTableModel *table = static_cast<const BiomeTableModel*>(sourceModel());
    const qulonglong av = a.row() < sortValues.size()
        ? sortValues[a.row()] : table->valueAt(a.row(), a.column());
    const qulonglong bv = b.row() < sortValues.size()
        ? sortValues[b.row()] : table->valueAt(b.row(), b.column());
    return bv < av;
}

QVariant BiomeTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (section < 0)
        return QVariant();
    if (role == Qt::InitialSortOrderRole)
        return QVariant::fromValue(Qt::AscendingOrder);
    if (orientation == Qt::Vertical && section < seeds.size())
    {
        int64_t seed = seeds[section];
        if (role == Qt::UserRole || role == Qt::UserRole+1)
            return QVariant::fromValue(seed); // identifier and export
        if (role == Qt::DisplayRole)
            return QVariant::fromValue(QString(" %1 ").arg(seed));
        static QVariant align = QVariant::fromValue((int)Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::TextAlignmentRole)
            return align;
    }
    if (orientation == Qt::Horizontal && section < ids.size())
    {
        int id = ids[section];
        const char *bname = biome2str(cmp.mc, id);
        if (role == Qt::UserRole)
            return id; // identifier
        if (role == Qt::UserRole+1)
            return bname ? bname : "#"; // export role
        if (role == Qt::DisplayRole)
            return id == 256 ? tr("Biomes") : getBiomeDisplay(cmp.mc, id);
        if (role == Qt::ToolTipRole && bname)
            return QVariant::fromValue(QString("%1:%2").arg(id).arg(bname));
    }
    return QVariant();
}

void BiomeTableModel::insertIds(QSet<int>& nids)
{
    for (int id : std::as_const(nids))
    {
        QList<int>::iterator it = std::lower_bound(ids.begin(), ids.end(), id, cmp);
        if (it == ids.end() || *it != id)
        {
            int i = std::distance(ids.begin(), it);
            beginInsertColumns(QModelIndex(), i, i);
            ids.insert(i, id);
            endInsertColumns();
        }
    }
}

void BiomeTableModel::insertSeeds(QList<uint64_t>& nseeds)
{
    int i = seeds.size();
    beginInsertRows(QModelIndex(), i, i+nseeds.size()-1);
    seeds.append(nseeds);
    endInsertRows();
}

void BiomeTableModel::reset(int mc)
{
    beginResetModel();
    seeds.clear();
    ids.clear();
    cnt.clear();
    cmp.mode = IdCmp::SORT_DIM;
    cmp.dim = DIM_UNDEF;
    cmp.mc = mc;
    endResetModel();
}

BiomeHeader::BiomeHeader(QWidget *parent)
    : QHeaderView(Qt::Horizontal, parent)
    , hover(-1)
    , pressed(-1)
{
    setSectionsClickable(true);
    setHighlightSections(true);
    connect(this, &QHeaderView::sectionPressed, this, &BiomeHeader::onSectionPress);
}

void BiomeHeader::onSectionPress(int section)
{
    pressed = section;
}

bool BiomeHeader::event(QEvent *e)
{
    switch (e->type())
    {
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
        hover = logicalIndexAt(static_cast<QHoverEvent*>(e)->position().toPoint());
        break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
        hover = -1;
        break;
    default: break;
    }
    return QHeaderView::event(e);
}

void BiomeHeader::paintSection(QPainter *painter, const QRect& rect, int section) const
{
    QHeaderView::paintSection(painter, rect, section);
}

QSize BiomeHeader::sectionSizeFromContents(int section) const
{
    return QHeaderView::sectionSizeFromContents(section);
}

TabBiomes::TabBiomes(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabBiomes)
    , parent(parent)
    , thread()
    , model(new BiomeTableModel(this))
    , proxy(new BiomeSortProxy(this))
    , selectedBiomes()
    , elapsed()
    , updt(20)
    , nextupdate()
{
    ui->setupUi(this);

    proxy->setSourceModel(model);
    proxy->setDynamicSortFilter(false);
    ui->table->setModel(proxy);

    BiomeHeader *header = new BiomeHeader(ui->table);
    ui->table->setHorizontalHeader(header);
    connect(header, &QHeaderView::sortIndicatorChanged, this, &TabBiomes::onTableSort);
    connect(ui->table->verticalHeader(), &QHeaderView::sectionClicked, this, &TabBiomes::onVHeaderClicked);

    ui->table->setSortingEnabled(true);
    // all rows are single-line text: ResizeToContents would measure every row
    // (and every biome column) again after each batch of up to 65536 seeds
    ui->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    ui->table->verticalHeader()->setDefaultSectionSize(ui->table->fontMetrics().height() + 4);
    ui->table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    ui->treeLocate->header()->setMinimumSectionSize(24);
    ui->treeLocate->header()->setSectionResizeMode(QHeaderView::Stretch);
    ui->treeLocate->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(ui->treeLocate, &ResultTree::exportRequested, this, &TabBiomes::on_pushExport_clicked);
    addResultMenu(ui->table, [this]() { on_pushExport_clicked(); });

    QIntValidator *intval = new QIntValidator(-60e6, 60e6, this);
    ui->lineX1->setValidator(intval);
    ui->lineZ1->setValidator(intval);
    ui->lineX2->setValidator(intval);
    ui->lineZ2->setValidator(intval);

    ui->lineBiomeSize->setValidator(new QIntValidator(1, INT_MAX / 16, this));
    ui->lineTolerance->setValidator(new QIntValidator(0, 255, this));
    ui->lineBiomeSize->setText("1");
    ui->lineTolerance->setText("0");

    // the arguments are copies, so the analysis thread need not wait for the GUI
    connect(&thread, &AnalysisBiomes::seedDone, this, &TabBiomes::onAnalysisSeedDone, Qt::QueuedConnection);
    connect(&thread, &AnalysisBiomes::seedItem, ui->treeLocate,
        [this](QTreeWidgetItem *item) { ui->treeLocate->post(item); }, Qt::DirectConnection);
    connect(&thread, &AnalysisBiomes::failed, this,
        [this](const QString& message) { warn(this, message); }, Qt::QueuedConnection);
    connect(&thread, &AnalysisBiomes::finished, this, &TabBiomes::onAnalysisFinished,
        Qt::QueuedConnection);

    for (int id = 0; id < 256; id++)
    {
        QString s;
        if (!(s = getBiomeDisplay(MC_1_17, id)).isEmpty())
            str2biome[s] = id;
        if (!(s = getBiomeDisplay(MC_NEWEST, id)).isEmpty())
            str2biome[s] = id;
    }

}

TabBiomes::~TabBiomes()
{
    thread.stop = true;
    waitForMappingThread(thread);
    delete ui;
}

bool TabBiomes::event(QEvent *e)
{
    return QWidget::event(e);
}

void TabBiomes::save(QSettings& settings)
{
    const auto saveCoordinate = [&settings](const char *key, QLineEdit *line) {
        if (line->text().trimmed().isEmpty())
            settings.remove(key);
        else
            settings.setValue(key, line->text().toInt());
    };
    saveCoordinate("analysis/x1", ui->lineX1);
    saveCoordinate("analysis/z1", ui->lineZ1);
    saveCoordinate("analysis/x2", ui->lineX2);
    saveCoordinate("analysis/z2", ui->lineZ2);
    settings.setValue("analysis/seedsrc", ui->comboSeedSource->currentIndex());
    settings.setValue("analysis/scaleidx", ui->comboScale->currentIndex());
    settings.setValue("analysis/overworld", ui->checkOverworld->isChecked());
    settings.setValue("analysis/nether", ui->checkNether->isChecked());
    settings.setValue("analysis/end", ui->checkEnd->isChecked());
    settings.setValue("analysis/samples", ui->lineSamples->text().toULongLong());
    settings.setValue("analysis/fullarea", ui->radioFullSample->isChecked());
    QVector<int> selected = selectedBiomeList();
    settings.setValue("analysis/biomeid", selected.isEmpty() ? -1 : selected.constFirst());
    QVariantList biomeIds;
    for (int id : selectedBiomeList())
        biomeIds.append(id);
    settings.setValue("analysis/biomeids", biomeIds);
    settings.setValue("analysis/biomesize_chunks", ui->lineBiomeSize->text().toInt());
    settings.setValue("analysis/tolerance", ui->lineTolerance->text().toInt());
}

static void loadCheck(QSettings *s, QCheckBox *cb, const char *key)
{
    cb->setChecked( s->value(key, cb->isChecked()).toBool() );
}
static void loadCombo(QSettings *s, QComboBox *combo, const char *key)
{
    combo->setCurrentIndex( s->value(key, combo->currentIndex()).toInt() );
}
static void loadLine(QSettings *s, QLineEdit *line, const char *key)
{
    if (s->contains(key))
        line->setText(QString::number(s->value(key).toLongLong()));
}
void TabBiomes::load(QSettings& settings)
{
    loadLine(&settings, ui->lineX1, "analysis/x1");
    loadLine(&settings, ui->lineZ1, "analysis/z1");
    loadLine(&settings, ui->lineX2, "analysis/x2");
    loadLine(&settings, ui->lineZ2, "analysis/z2");
    loadCombo(&settings, ui->comboSeedSource, "analysis/seedsrc");
    loadCombo(&settings, ui->comboScale, "analysis/scaleidx");
    loadCheck(&settings, ui->checkOverworld, "analysis/overworld");
    loadCheck(&settings, ui->checkNether, "analysis/nether");
    loadCheck(&settings, ui->checkEnd, "analysis/end");
    loadLine(&settings, ui->lineSamples, "analysis/samples");
    if (settings.value("analysis/fullarea", true).toBool())
        ui->radioFullSample->setChecked(true);
    else
        ui->radioStochastic->setChecked(true);
    selectedBiomes.clear();
    const QVariantList storedBiomes = settings.value("analysis/biomeids").toList();
    for (const QVariant& value : storedBiomes)
        selectedBiomes.insert(value.toInt());
    refreshBiomes(settings.value("analysis/biomeid", -1).toInt());
    if (settings.contains("analysis/biomesize_chunks"))
    {
        loadLine(&settings, ui->lineBiomeSize, "analysis/biomesize_chunks");
    }
    else
    {   // migrate the former 1:4-cell input to whole chunks without weakening it
        const int oldArea = settings.value("analysis/biomesize", 16).toInt();
        ui->lineBiomeSize->setText(QString::number(std::max(1, (oldArea + 15) / 16)));
    }
    loadLine(&settings, ui->lineTolerance, "analysis/tolerance");
}

void TabBiomes::refreshBiomes(int activeid)
{
    WorldInfo wi;
    parent->getSeed(&wi);
    if (!ui->labelDescription->property("originalText").isValid())
        ui->labelDescription->setProperty("originalText", ui->labelDescription->text());
    // thread.wi.seed is advanced by the worker; the seed list is set at the start
    const std::vector<uint64_t>& analysed = thread.seeds;
    const bool previous = ui->treeLocate->topLevelItemCount() && !analysed.empty() &&
        std::find(analysed.begin(), analysed.end(), wi.seed) == analysed.end();
    ui->labelDescription->setText(!previous
        ? ui->labelDescription->property("originalText").toString()
        : analysed.size() == 1
        ? tr("Results from seed %1. Analyze again for the current map.").arg(qint64(analysed.front()))
        : tr("Results from other seeds. Analyze again for the current map."));
    const int dim = parent->getDim();
    const int locateTab = ui->tabWidget->indexOf(ui->tabLocate);
    const bool canLocate = dim != DIM_END;
    ui->tabWidget->tabBar()->setTabVisible(locateTab, true);
    ui->tabLocate->setEnabled(canLocate);

    const auto isLocateBiome = [=](int id) {
        if (!biomeExists(wi.mc, id))
            return false;
        return dim == DIM_OVERWORLD ? isOverworld(wi.mc, id) != 0
                                    : getDimension(id) == dim;
    };
    QSet<int> supportedBiomes;
    for (int id : std::as_const(selectedBiomes))
        if (isLocateBiome(id))
            supportedBiomes.insert(id);
    selectedBiomes = supportedBiomes;
    if (!canLocate)
    {
        ui->comboBiome->clear();
        updateBiomeSelectionControls();
        return;
    }
    if (activeid >= 0 && !isLocateBiome(activeid))
        activeid = -1;
    if (activeid == -1)
    {
        if (!selectedBiomes.isEmpty())
            activeid = *selectedBiomes.constBegin();
    }
    std::vector<int> ids;
    for (int i = 0; i < 256; i++)
        if (isLocateBiome(i))
            ids.push_back(i);
    IdCmp cmp(IdCmp::SORT_LEX, wi.mc, dim);
    std::sort(ids.begin(), ids.end(), cmp);
    ui->comboBiome->clear();
    for (int i : ids)
        ui->comboBiome->addItem(getBiomeIcon(i), getBiomeDisplay(wi.mc, i), QVariant::fromValue(i));
    if (activeid >= 0)
    {
        int idx = ui->comboBiome->findText(getBiomeDisplay(wi.mc, activeid));
        ui->comboBiome->setCurrentIndex(idx);
        if (selectedBiomes.isEmpty() && idx >= 0)
            selectedBiomes.insert(activeid);
    }
    else
    {
        ui->comboBiome->setCurrentIndex(-1);
    }
    ui->comboBiome->setCheckedData(selectedBiomeList());
    updateBiomeSelectionControls();
}

QVector<int> TabBiomes::selectedBiomeList() const
{
    QVector<int> ids = selectedBiomes.values().toVector();
    std::sort(ids.begin(), ids.end());
    return ids;
}

void TabBiomes::updateBiomeSelectionControls()
{
    QStringList names;
    WorldInfo wi;
    parent->getSeed(&wi);
    for (int id : selectedBiomeList())
        names.append(getBiomeDisplay(wi.mc, id));
    QString summary = tr("Selected biomes: %1").arg(names.join(", "));
    ui->buttonHighlight->setToolTip(summary);
    if (selectedBiomes.isEmpty() && ui->buttonHighlight->isChecked())
        ui->buttonHighlight->setChecked(false);
    ui->buttonHighlight->setEnabled(!selectedBiomes.isEmpty());
    if (!ui->buttonHighlight->isChecked())
    {
        ui->buttonHighlight->setText(selectedBiomes.size() > 1
                ? tr("Highlight %n biomes", "", selectedBiomes.size())
                : tr("Highlight on map"));
    }
}

void TabBiomes::onTableSort(int, Qt::SortOrder)
{
    QHeaderView *header = ui->table->horizontalHeader();

    if (proxy->order == Qt::DescendingOrder && proxy->column != -1)
    {
        header->setSortIndicatorShown(false);
        header->setSortIndicator(-1, Qt::AscendingOrder);
        proxy->column = -1;
    }
    else
    {
        header->setSortIndicatorShown(true);
    }
}

void TabBiomes::onVHeaderClicked(int row)
{
    QVariant dat = proxy->headerData(row, Qt::Vertical, Qt::UserRole);
    if (dat.isValid())
    {
        uint64_t seed = dat.toULongLong();
        WorldInfo wi;
        parent->getSeed(&wi);
        wi.seed = seed;
        parent->setSeed(wi);
    }
}

void TabBiomes::onAnalysisSeedDone(uint64_t seed, QVector<uint64_t> idcnt)
{
    idcnt.push_back(seed);
    qbufs.push_back(idcnt);
    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &TabBiomes::onBufferTimeout);
    }
}

void TabBiomes::onAnalysisFinished()
{
    onBufferTimeout();
    if (!thread.dat.locateBiomes.isEmpty())
        ui->treeLocate->finish();
    else if (proxy->column >= 0)
        proxy->sort(proxy->column, proxy->order);
    on_tabWidget_currentChanged(-1);
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabBiomes::onBufferTimeout()
{
    const auto updateProgress = [&]() {
        if (thread.stop.load(std::memory_order_relaxed))
        {
            ui->pushStart->setText(tr("Stopping..."));
            return;
        }
        QString progress = QString::asprintf(" (%ld/%zu)", thread.idx.load(), thread.seeds.size());
        const uint64_t total = thread.workTotal.load(std::memory_order_relaxed);
        if (total)
            progress += QString::asprintf(" [%llu/%llu, %zu threads]",
                static_cast<unsigned long long>(thread.workDone.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(total),
                thread.workers.peak.load(std::memory_order_relaxed));
        if (!thread.dat.locateBiomes.isEmpty())
            progress += QString::asprintf(" [%llu results]",
                static_cast<unsigned long long>(
                    thread.resultCount.load(std::memory_order_relaxed)));
        ui->pushStart->setText(tr("Stop") + progress);
    };
    if (qbufs.empty())
    {
        if (!thread.isRunning())
            return;
        updateProgress();
        QTimer::singleShot(250, this, &TabBiomes::onBufferTimeout);
        return;
    }

    uint64_t t = -elapsed.elapsed();

    if (!qbufs.empty())
    {
        ui->table->setUpdatesEnabled(false);

        // store column widths to track which columns need to widen
        QMap<int, int> colwidth;
        for (int c = 0, n = model->ids.size(); c < n; c++)
            colwidth[model->ids[c]] = ui->table->columnWidth(c);

        QList<uint64_t> new_seeds;
        QSet<int> new_ids;
        QFontMetrics fm = fontMetrics();

        for (int i = 0, n = qbufs.size(); i < n; i++)
        {
            QVector<uint64_t>& scnt = qbufs[i];
            uint64_t seed = scnt.back();
            scnt.resize(scnt.size()-1);

            new_seeds.push_back(seed);
            for (int id = 0, idn = scnt.size(); id < idn; id++)
            {
                uint64_t cnt = scnt[id];
                if (cnt == 0)
                    continue;
                new_ids.insert(id);
                model->cnt[id][seed] = QVariant::fromValue(cnt);
                int w = txtWidth(fm, QString::number(cnt) + "#");
                if (w > colwidth[id])
                    colwidth[id] = w;
            }
        }
        model->insertIds(new_ids);
        model->insertSeeds(new_seeds);

        ui->table->setUpdatesEnabled(true);

        //ui->table->resizeColumnsToContents();
        for (int i = 0, n = proxy->columnCount(); i < n; i++)
        {
            int id = proxy->headerData(i, Qt::Horizontal, Qt::UserRole).toInt();
            ui->table->setColumnWidth(i, qMax(80, colwidth[id] + 28));
        }
        int rowheight = fm.height() + 4;
        ui->table->verticalHeader()->setDefaultSectionSize(rowheight);
        qbufs.clear();
    }

    if (thread.isRunning())
        updateProgress();

    QApplication::processEvents(); // force processing of events so we can time correctly

    t += elapsed.elapsed();
    if (8*t > updt)
        updt = 4*t;
    nextupdate = elapsed.nsecsElapsed() + 1e6 * updt;
    if (thread.isRunning())
        QTimer::singleShot(250, this, &TabBiomes::onBufferTimeout);
}

void TabBiomes::on_pushStart_clicked()
{
    if (thread.isRunning())
    {
        thread.stop = true;
        ui->pushStart->setText(tr("Stopping..."));
        return;
    }

    updt = 20;
    nextupdate = 0;
    elapsed.start();
    thread.stop = false;

    parent->getSeed(&thread.wi);
    thread.seeds.clear();
    if (ui->comboSeedSource->currentIndex() == 0)
        thread.seeds.push_back(thread.wi.seed);
    else
        thread.seeds = parent->formControl->getResults();

    thread.dims[0] = ui->checkOverworld->isChecked() ? DIM_OVERWORLD : DIM_UNDEF;
    thread.dims[1] = ui->checkNether->isChecked() ? DIM_NETHER : DIM_UNDEF;
    thread.dims[2] = ui->checkEnd->isChecked() ? DIM_END : DIM_UNDEF;

    int x1 = ui->lineX1->text().toInt();
    int z1 = ui->lineZ1->text().toInt();
    int x2 = ui->lineX2->text().toInt();
    int z2 = ui->lineZ2->text().toInt();
    if (x2 < x1) std::swap(x1, x2);
    if (z2 < z1) std::swap(z1, z2);

    int s = ui->comboScale->currentIndex() * 2; // combo index matches a power of 4 scale
    int scale = 1 << s;

    if (ui->radioFullSample->isChecked())
    {
        thread.dat.samples = ~0ULL;
    }
    else
    {
        thread.dat.samples = ui->lineSamples->text().toULongLong();
        scale = 4;
        s = 2;
    }

    if (ui->tabWidget->currentWidget() == ui->tabLocate)
    {
        if (selectedBiomes.isEmpty())
        {
            warn(parent, tr("Select at least one biome."));
            return;
        }
        ui->treeLocate->clearResults();
        thread.dat.locateBiomes = selectedBiomeList();
        thread.dat.locate = thread.dat.locateBiomes.isEmpty()
                ? -1 : thread.dat.locateBiomes.constFirst();
        thread.dat.locateDim = parent->getDim();
        const int chunks = ui->lineBiomeSize->text().toInt();
        thread.minsize = chunks > INT_MAX / 16 ? INT_MAX : chunks * 16;
        thread.tolerance = ui->lineTolerance->text().toInt();
        if (thread.minsize <= 0)
            thread.minsize = 1;
        scale = 4;
        s = 2;
    }
    else
    {
        model->reset(thread.wi.mc);
        thread.dat.locate = -1;
        thread.dat.locateBiomes.clear();
    }

    thread.dat.scale = scale;
    thread.dat.x1 = x1 >> s;
    thread.dat.z1 = z1 >> s;
    thread.dat.x2 = x2 >> s;
    thread.dat.z2 = z2 >> s;

    if (thread.dat.locate < 0)
        dats = thread.dat;
    else
        datl = thread.dat;

    ui->pushExport->setEnabled(false);
    thread.workDone.store(0, std::memory_order_relaxed);
    thread.workTotal.store(0, std::memory_order_relaxed);
    thread.resultCount.store(0, std::memory_order_relaxed);
    thread.workers.reset();
    ui->pushStart->setChecked(true);
    QString progress = QString::asprintf(" (0/%zu)", thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    thread.start();
    QTimer::singleShot(250, this, &TabBiomes::onBufferTimeout);
}


void TabBiomes::exportResults(CsvWriter& csv)
{
    const auto area = [&](const char *key, qint64 v, qint64 scale) {
        csv.row({key, QString::number(v), QString("(%1)").arg(v * scale)});
    };
    if (ui->tabWidget->currentWidget() == ui->tabStats)
    {
        area("#X1", dats.x1, dats.scale);
        area("#Z1", dats.z1, dats.scale);
        area("#X2", dats.x2, dats.scale);
        area("#Z2", dats.z2, dats.scale);
        csv.row({"#scale", QString("1:%1").arg(dats.scale)});
        if (dats.samples != ~0ULL)
            csv.row({"#samples", QString::number(dats.samples)});

        QStringList header = { tr("seed") };
        for (int col = 0, ncol = proxy->columnCount(); col < ncol; col++)
            header.append(proxy->headerData(col, Qt::Horizontal, Qt::UserRole+1).toString());
        csv.row(header);

        for (int row = 0, nrow = proxy->rowCount(); row < nrow; row++)
        {
            QStringList cols;
            cols.append(proxy->headerData(row, Qt::Vertical, Qt::UserRole+1).toString());
            for (int col = 0, ncol = proxy->columnCount(); col < ncol; col++)
            {
                QString cntstr = proxy->data(proxy->index(row, col)).toString();
                cols.append(cntstr == "" ? "0" : cntstr);
            }
            csv.row(cols);
        }
    }
    else if (ui->tabWidget->currentWidget() == ui->tabLocate)
    {
        area("#X1", datl.x1, datl.scale);
        area("#Z1", datl.z1, datl.scale);
        area("#X2", datl.x2, datl.scale);
        area("#Z2", datl.z2, datl.scale);
        csv.row({"#scale", QString("1:%1").arg(datl.scale)});
        QStringList biomeNames;
        for (int id : std::as_const(datl.locateBiomes))
            biomeNames.append(QString::fromLatin1(biome2str(MC_NEWEST, id)));
        csv.row({"#biomes", biomeNames.join(",")});

        csv.row({ tr("seed"), tr("biome"), tr("area"), tr("x"), tr("z") });
        QString seed;
        for (QTreeWidgetItemIterator it(ui->treeLocate); *it; ++it)
        {
            QTreeWidgetItem *item = *it;
            if (item->text(0) != "-")
            {
                seed = item->text(0);
                continue;
            }
            csv.row({ seed, item->text(1), item->text(2), item->text(3), item->text(4) });
        }
    }
}

void TabBiomes::on_pushExport_clicked()
{
    exportCsv(this, parent->prevdir, tr("Export biome analysis"), "biomes.csv",
        parent->config, [this](CsvWriter& csv) { exportResults(csv); });
}

void TabBiomes::on_buttonFromVisible_clicked()
{
    MapView *mapview = parent->getMapView();

    int x1, z1, x2, z2;
    mapview->getVisible(&x1, &z1, &x2, &z2);

    ui->lineX1->setText( QString::number(x1) );
    ui->lineZ1->setText( QString::number(z1) );
    ui->lineX2->setText( QString::number(x2) );
    ui->lineZ2->setText( QString::number(z2) );
}

void TabBiomes::on_radioFullSample_toggled(bool checked)
{
    ui->comboScale->setEnabled(checked);
    ui->lineSamples->setEnabled(!checked);
}

void TabBiomes::on_comboBiome_selectionChanged()
{
    selectedBiomes.clear();
    for (int biomeId : ui->comboBiome->checkedData())
        selectedBiomes.insert(biomeId);
    updateBiomeSelectionControls();
    if (ui->buttonHighlight->isChecked())
        parent->getMapView()->setBiomeHighlights(selectedBiomeList());
}

void TabBiomes::on_buttonHighlight_toggled(bool checked)
{
    parent->getMapView()->setBiomeHighlights(checked ? selectedBiomeList() : QVector<int>());
    ui->buttonHighlight->setText(checked ? tr("Clear highlight") : tr("Highlight on map"));
    updateBiomeSelectionControls();
}

void TabBiomes::on_treeLocate_itemClicked(QTreeWidgetItem *item, int column)
{
    (void) column;
    QVariant dat;
    dat = item->data(0, Qt::UserRole);
    if (dat.isValid())
    {
        uint64_t seed = qvariant_cast<uint64_t>(dat);
        int dim = item->data(0, Qt::UserRole+1).toInt();
        WorldInfo wi;
        parent->getSeed(&wi);
        wi.seed = seed;
        parent->setSeed(wi, dim);
    }

    dat = item->data(0, Qt::UserRole+2);
    if (dat.isValid())
    {
        Pos p = qvariant_cast<Pos>(dat);
        parent->getMapView()->setView(p.x+0.5, p.z+0.5);
    }
}

void TabBiomes::on_tabWidget_currentChanged(int)
{
    bool ok = false;
    if (!thread.isRunning())
    {
        if (ui->tabWidget->currentWidget() == ui->tabStats)
            ok = !model->ids.empty();
        if (ui->tabWidget->currentWidget() == ui->tabLocate)
            ok = ui->treeLocate->topLevelItemCount() > 0;
    }
    ui->pushExport->setEnabled(ok);
}
