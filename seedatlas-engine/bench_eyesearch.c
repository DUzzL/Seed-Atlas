/* Throughput of scanEyeFamily() for representative configurations.
 *
 * Usage: bench-eyesearch [FAMILIES] [MC]
 *        bench-eyesearch -c MC LARGE N MINEYES EXACT FAMILIES
 * One thread; prints seeds per second, the time split between the stages
 * (a: candidate plans, b: eye tests, c: confirmation) and, for minEyes 12,
 * the expected time per hit on this one thread. The rate does not scale
 * linearly with threads (an Apple M6 with 12 cores reaches about 6x the one
 * thread rate, not 12x); tools/eyesearch-gpu-check bench measures the whole
 * machine.
 */
#include "eyesearch.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Processor time on POSIX, wall time on Windows; both fine for one thread. */
static double now(void)
{
    return (double) clock() / CLOCKS_PER_SEC;
}

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void run(int mc, int large, int nsh, int minEyes, int exactOnly, int fams)
{
    EyeSearchConfig cfg = { mc, large, nsh, minEyes, exactOnly };
    EyeSearch *es = createEyeSearch(&cfg);
    if (!es)
        return;
    uint64_t rng = 2024;
    double t0 = now();
    long hits = 0;
    for (int f = 0; f < fams; f++)
        hits += scanEyeFamily(es, splitmix64(&rng) & MASK48, NULL, NULL, NULL);
    double t = now() - t0;
    EyeSearchStats st;
    getEyeSearchStats(es, &st);
    double rate = st.seeds / t;
    double cpu = st.timePrep + st.timeSweep + st.timeConfirm;
    printf("%-6s large=%d n=%-3d min=%-2d exact=%d | %9.0f seeds/s | "
        "a %4.1f%% b %4.1f%% c %4.1f%% | %5.1f plans/idx, %.2f tests/plan-seed, "
        "%ld hits",
        mc2str(mc), large, nsh, minEyes, exactOnly, rate,
        100 * st.timePrep / cpu, 100 * st.timeSweep / cpu,
        100 * st.timeConfirm / cpu,
        (double) st.plans / st.families / nsh,
        st.plans ? (double) st.tests / st.plans / 65536 : 0, hits);
    if (st.biomeSearches)
        printf(" | %.1f biome searches/family", (double) st.biomeSearches / st.families);
    if (minEyes == 12)
    {
        // qualifying portals: candidates with 12 counted frames
        double q = st.candidates ? (double) st.plans / st.candidates : 0;
        double p = pow(1677721.0 / 16777216.0, 12);
        double perSeed = nsh * q * p;
        double secs = 1 / (perSeed * rate);
        printf(" | expected %.2g s (%.0f h) per hit on one thread", secs, secs / 3600);
    }
    printf("\n");
    fflush(stdout);
    freeEyeSearch(es);
}

int main(int argc, char **argv)
{
    if (argc == 8 && !strcmp(argv[1], "-c"))
    {   // -c MC LARGE N MINEYES EXACT FAMILIES
        run(str2mc(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
            atoi(argv[6]), atoi(argv[7]));
        return 0;
    }
    int fams = argc > 1 ? atoi(argv[1]) : 4;
    int only = argc > 2 ? str2mc(argv[2]) : -1;
    const int mcs[] = { MC_26_3, MC_1_21_1, MC_1_19_2, MC_1_18 };
    for (int v = 0; v < 4; v++)
    {
        int mc = mcs[v];
        if (only >= 0 && mc != only)
            continue;
        run(mc, 0, 3, 12, 1, fams);
        run(mc, 0, 3, 12, 0, fams);
        if (v == 0)
        {
            run(mc, 1, 3, 12, 1, fams);
            run(mc, 0, 1, 12, 1, fams);
            run(mc, 0, 8, 12, 1, fams);
            run(mc, 0, 3, 11, 1, fams);
            run(mc, 0, 3, 10, 0, fams);
            run(mc, 0, 3, 9, 1, fams);
            run(mc, 0, 3, 8, 1, 1);
        }
    }
    return 0;
}
