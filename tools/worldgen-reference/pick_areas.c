// Pick the per-seed areas "ocean", "peaks", "inland" and "plains" of worlds.txt.
//
// Uses the engine's climate noise (Default world type), so this is only a
// convenience to find interesting places; the reference data itself comes from
// vanilla. Build out of tree against an engine checkout, e.g.
//   E=<repo>/seedatlas-engine; cc -fwrapv -ffp-contract=off -O3 -I$E -o pick_areas pick_areas.c \
//     $E/finders.c $E/generator.c $E/layers.c $E/biomenoise.c $E/biomes.c \
//     $E/noise.c $E/util.c $E/quadbase.c -lm
// Usage: pick_areas <mc> <seed>...   prints one worlds.txt line per seed.
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STEP = 64, RANGE = 16000, HALF = 8 }; // HALF: half area size in chunks

typedef struct { int found, x, z; long long d2; int64_t cont, ero, weird; } Pick;

static int biomeAt(Generator *g, int x, int z, int64_t *np)
{
    // biome cell of block (x, 64, z)
    return sampleBiomeNoise(&g->bn, np, x >> 2, 64 >> 2, z >> 2, NULL, 0);
}

// The centre and the four points 96 blocks diagonally away all satisfy pred.
static int allAround(Generator *g, int x, int z, int (*pred)(int))
{
    static const int dx[] = { 0, -96, 96, -96, 96 }, dz[] = { 0, -96, -96, 96, 96 };
    int64_t np[6];
    for (int i = 0; i < 5; i++)
        if (!pred(biomeAt(g, x + dx[i], z + dz[i], np)))
            return 0;
    return 1;
}

static int isDeepSea(int id)
{
    return id == deep_ocean || id == deep_cold_ocean || id == deep_lukewarm_ocean ||
        id == deep_frozen_ocean;
}
static int isPeakBiome(int id) { return id == jagged_peaks || id == frozen_peaks || id == stony_peaks; }
static int isPlainsBiome(int id) { return id == plains; }

static void take(Pick *p, int x, int z, const int64_t *np)
{
    long long d2 = (long long) x * x + (long long) z * z;
    if (p->found && p->d2 <= d2)
        return;
    p->found = 1; p->x = x; p->z = z; p->d2 = d2;
    p->cont = np[NP_CONTINENTALNESS]; p->ero = np[NP_EROSION]; p->weird = np[NP_WEIRDNESS];
}

static void printArea(const char *name, const Pick *p)
{
    int cx = (p->x >> 4) - HALF, cz = (p->z >> 4) - HALF;
    printf(" %s:%d,%d,%d,%d", name, cx, cz, 2 * HALF, 2 * HALF);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: pick_areas <mc> <seed>...\n"); return 1; }
    int mc = str2mc(argv[1]);
    if (mc < MC_1_18) { fprintf(stderr, "bad mc\n"); return 1; }
    for (int a = 2; a < argc; a++)
    {
        uint64_t seed = (uint64_t) strtoll(argv[a], NULL, 10);
        Generator g;
        setupGenerator(&g, mc, 0);
        applySeed(&g, DIM_OVERWORLD, seed);
        Pick ocean = {0}, peaks = {0}, inland = {0}, plain = {0}, best = {0};
        int64_t bestCont = -100000;
        for (int z = -RANGE; z <= RANGE; z += STEP)
        for (int x = -RANGE; x <= RANGE; x += STEP)
        {
            // keep clear of the origin area (blocks -256..255) and its surroundings
            if (abs(x) < 640 && abs(z) < 640)
                continue;
            int64_t np[6];
            int id = biomeAt(&g, x, z, np);
            if (np[NP_CONTINENTALNESS] > bestCont)
            {
                bestCont = np[NP_CONTINENTALNESS];
                best.found = 0;
                take(&best, x, z, np);
            }
            // continentalness >= 0.55: deep inside "far inland" (>= 0.3)
            if (np[NP_CONTINENTALNESS] >= 5500)
                take(&inland, x, z, np);
            if (isDeepSea(id) && allAround(&g, x, z, isDeepSea))
                take(&ocean, x, z, np);
            if (isPeakBiome(id)) // peaks are narrow ridges: only the centre is required
                take(&peaks, x, z, np);
            if (isPlainsBiome(id) && allAround(&g, x, z, isPlainsBiome))
                take(&plain, x, z, np);
        }
        if (!inland.found)
            inland = best;
        printf("# %lld: continentalness/erosion/weirdness x10000 at the centres (engine, %s, Default):"
            " ocean %lld/%lld/%lld (%d,%d) peaks %lld/%lld/%lld (%d,%d) inland %lld/%lld/%lld (%d,%d)"
            " plains %lld/%lld/%lld (%d,%d)\n", (long long) seed, argv[1],
            (long long) ocean.cont, (long long) ocean.ero, (long long) ocean.weird, ocean.x, ocean.z,
            (long long) peaks.cont, (long long) peaks.ero, (long long) peaks.weird, peaks.x, peaks.z,
            (long long) inland.cont, (long long) inland.ero, (long long) inland.weird, inland.x, inland.z,
            (long long) plain.cont, (long long) plain.ero, (long long) plain.weird, plain.x, plain.z);
        printf("%lld origin:-16,-16,32,32 far1:304,-196,16,16 far2:-1258,742,16,16", (long long) seed);
        if (ocean.found) printArea("ocean", &ocean);
        if (peaks.found) printArea("peaks", &peaks);
        if (inland.found) printArea("inland", &inland);
        if (plain.found) printArea("plains", &plain);
        printf("\n");
    }
    return 0;
}
