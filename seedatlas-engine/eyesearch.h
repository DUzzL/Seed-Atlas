#ifndef EYESEARCH_H_
#define EYESEARCH_H_

/* Seed search for End portals with many eyes (Minecraft 1.18 .. 26.3).
 *
 * The search space is organised in 48-bit families: the 65536 world seeds
 * that share their lower 48 bits. The stronghold layouts of a family differ
 * only by the structure start chunk, while the eyes depend on all 64 bits
 * (see docs/portal-eyes.md), so a family is the natural unit of work.
 *
 * A search context belongs to one thread. Contexts are independent of each
 * other, so every worker thread creates its own.
 *
 * How scanEyeFamily() works (eyesearch.c)
 * ---------------------------------------
 * (a) Per family, on the CPU: for every stronghold index, a superset of the
 *     start chunks that nextStronghold() can return ("candidates"):
 *     - 1.19.3+: the approximate position follows from the lower 48 bits
 *       (the biome search has its own RNG), and the biome snap stays within
 *       +/-112 blocks, so the candidates are the 15x15 chunks around it.
 *     - 1.18 .. 1.19.2: the ring iteration shares its RNG with the biome
 *       search. Index 0 is a 15x15 window as above; indices 1 and 2 lie on a
 *       known ray at an unknown distance (a strip of about 1400 chunks);
 *       later rings start at a random angle (a whole annulus, about 190k
 *       chunks for ring 1).
 *     Each candidate gets a layout (stopped at the portal room) and a plan:
 *     the eye "tests" whose outcome decides whether the portal reaches
 *     minEyes. Plans that cannot reach it (e.g. too few exact frames with
 *     exactOnly) are dropped.
 * (b) Bulk eye evaluation (the hot loop; the split scan below lets a GPU
 *     stage replace it): for every plan, its tests run as a cascade over
 *     the 65536 seeds, each test
 *     only over the survivors of the previous one, until more than
 *     (tests - minEyes) misses reject a seed. Survivors mark their seed for
 *     the plan's stronghold indices.
 * (c) Exact confirmation of marked seeds with nextStronghold() (the biome
 *     search, the authority for the start chunk) and getEndPortal(); only
 *     these results are reported.
 * If survivors would be so frequent that (c) costs about as much as running
 * nextStronghold() for every seed (low minEyes), a group of candidates skips
 * (a)/(b) and marks all seeds ("direct" mode), so the scan is never
 * noticeably slower than scanEyeFamilyReference().
 *
 * Data consumed by stage (b)
 * --------------------------
 * Per family (low48), for each seed high h (seed = h << 48 | low48):
 *     decoA[h], decoB[h] = the two setDecorationSeed multipliers
 *     (xSetSeed(seed); a = xNextLongJ() | 1; b = xNextLongJ() | 1).
 * Per test: mx = 16*chunkX, mz = 16*chunkZ of the frame chunk, the constant
 *     salt = structIndex + 10000*step, and n = the number of RNG outputs
 *     before the tested draw (chunk skip + draw index), represented by an
 *     EyeTable for M^n (M = one Xoroshiro128++ step over GF(2)).
 * One test of one seed:
 *     v    = ((mx*decoA[h] + mz*decoB[h]) ^ seed) + salt      (mod 2^64)
 *     l    = mix(v ^ XH), r = mix((v ^ XH) + XL)              (= xSetSeed)
 *     p    = XOR over k < 8 of t.v[k][byte k of l]
 *          ^ XOR over k < 8 of t.v[8+k][byte k of r]
 *     lo40 = p >> 23, x = (lo40 + (p & 0x7fffff)) & 0x7fffff
 *     base = (2*x + (lo40 >> 16)) & 0xffffff
 *     base <= T-4               : no eye  (T = PORTAL_EYE_THRESHOLD)
 *     T < base <= 0xffffff - 4  : eye
 *     otherwise (about 6e-7)    : ambiguous, decide exactly (full state).
 * Every table entry holds the 63 state bits lo[63:24] (bits 62..23) and
 * hi[46:24] (bits 22..0) of M^n applied to the unit vectors of that byte,
 * which is all the top 24 output bits need up to three unknown carries.
 */

#include "strongholds.h"

#ifdef __cplusplus
extern "C"
{
#endif

STRUCT(EyeSearchConfig)
{
    int mc;             // Minecraft version, MC_1_18 .. MC_NEWEST
    int large;          // large biomes world
    int numStrongholds; // examine the first n strongholds of a seed (1 .. 128)
    int minEyes;        // report portals with at least this many eyes (1 .. 12)
    /* Non-zero: count only the eyes of exact frames (EndPortal.exactEyeCount),
     * i.e. eyes that follow from the seed alone. Zero: count all eyes
     * (EndPortal.eyeCount), including the best-effort state of frames whose
     * eye depends on the terrain. */
    int exactOnly;
    int amplified;      // Amplified world (not together with large)
};

STRUCT(EyeSearchHit)
{
    uint64_t seed;      // world seed
    int index;          // stronghold index (0-based, StrongholdIter order)
    Pos startChunk;     // structure start chunk of that stronghold
    EndPortal portal;   // the complete, exactly evaluated portal
};

/* Called for every hit. A non-zero return value aborts the scan. */
typedef int (*EyeSearchCallback)(void *user, const EyeSearchHit *hit);

typedef struct EyeSearch EyeSearch;

/* Creates a search context for one thread, or returns NULL if the
 * configuration is not supported. All memory (about 4.5 MB) is allocated
 * here; scans do not allocate. */
EyeSearch *createEyeSearch(const EyeSearchConfig *cfg);
void freeEyeSearch(EyeSearch *es);

/* Examines all 65536 world seeds with the given lower 48 bits and reports
 * every (seed, stronghold) whose portal satisfies the configuration. Each
 * reported hit is exact with respect to the engine: it equals what
 * getEndPortalOfStronghold() yields for that seed and index.
 *
 * Hits are reported in no particular order. '*stop' (nullable) is polled
 * regularly; once it is non-zero the scan returns early.
 *
 * Returns the number of reported hits, or -1 if the scan was aborted by
 * '*stop' or by the callback (the family is then incomplete).
 */
int scanEyeFamily(EyeSearch *es, uint64_t low48, EyeSearchCallback cb,
    void *user, const volatile int *stop);

/* Straightforward reference implementation of scanEyeFamily(): evaluates
 * every seed on its own. Slow; it defines the expected results in tests. */
int scanEyeFamilyReference(EyeSearch *es, uint64_t low48, EyeSearchCallback cb,
    void *user, const volatile int *stop);

/* The same, restricted to the seeds (high << 48) | low48 with
 * highBegin <= high < highEnd (0 <= highBegin < highEnd <= 65536). Mainly for
 * tests; a full family is cheaper per seed, because stage (a) is per family. */
int scanEyeFamilyRange(EyeSearch *es, uint64_t low48, int highBegin, int highEnd,
    EyeSearchCallback cb, void *user, const volatile int *stop);
int scanEyeFamilyReferenceRange(EyeSearch *es, uint64_t low48, int highBegin,
    int highEnd, EyeSearchCallback cb, void *user, const volatile int *stop);

/* Strategy of scanEyeFamily (tests and benchmarks; results never depend on
 * it): automatic, always filter, or always confirm every seed directly. */
enum { EYESEARCH_AUTO, EYESEARCH_FILTER, EYESEARCH_DIRECT };
void setEyeSearchMode(EyeSearch *es, int mode);

/* Counters, accumulated over scans until reset. */
STRUCT(EyeSearchStats)
{
    uint64_t families;      // scan calls
    uint64_t seeds;         // seeds covered
    uint64_t candidates;    // candidate start chunks (layouts generated)
    uint64_t plans;         // plans that can reach minEyes (swept)
    uint64_t tables;        // eye tables built for non-standard skips
    uint64_t tests;         // single-seed eye tests in stage (b)
    uint64_t survivors;     // (seed, plan) pairs with enough eyes
    uint64_t confirmSeeds;  // seeds examined in stage (c)
    uint64_t biomeSearches; // nextStronghold() calls with biomes in stage (c)
    uint64_t hits;
    /* Wall-clock seconds this context spent in each stage (monotonic clock,
     * per context, so valid with one context per thread; includes time the
     * thread was preempted). */
    double timePrep;        // stage (a)
    double timeSweep;       // ... stage (b)
    double timeConfirm;     // ... stage (c)
};
void getEyeSearchStats(const EyeSearch *es, EyeSearchStats *st);
void resetEyeSearchStats(EyeSearch *es);

/* Test hooks (used by tests_eyesearch.c).
 * eyeSearchIsCandidate: whether start chunk (cx, cz) belongs to the candidate
 * set of stronghold 'index' in family low48, i.e. whether stage (a) would
 * examine it.
 * eyeSearchSelfTest: compares the stage (b) eye test against plain Xoroshiro
 * stepping for 'count' random seeds and skips; returns the number of errors
 * and, optionally, how many ambiguous estimates were resolved. */
int eyeSearchIsCandidate(EyeSearch *es, uint64_t low48, int index, int cx, int cz);
long eyeSearchSelfTest(EyeSearch *es, uint64_t rng, long count, long *ambiguous);
/* eyeSearchFilterCheck: runs stages (a) and (b) on a seed range and checks
 * them against evalPortalEyes() on complete layouts: every (seed, candidate)
 * whose portal reaches minEyes must be marked. Returns the number of
 * unmarked ones (0 = correct); annulus groups are skipped (too large). */
long eyeSearchFilterCheck(EyeSearch *es, uint64_t low48, int h0, int h1,
    long *checked);

/* Stage (b) eye table for one skip (see above): 16 byte positions x 256. */
STRUCT(EyeTable)
{
    uint64_t v[16][256];
};

/* Split scan: the same as scanEyeFamily(), with stage (b) done by the caller
 * (e.g. on a GPU). For one family:
 *
 *     eyeSearchBegin(es, low48);
 *     while ((n = eyeSearchNextPlans(es, plans, max, stop)) > 0)
 *         for every (plan, seed high h) that the caller's stage (b) keeps:
 *             eyeSearchKeep(es, &plans[i], h);
 *         (or eyeSearchSweep(es, plans, n, stop): stage (b) on the CPU)
 *     hits = eyeSearchFinish(es, cb, user, stop);
 *
 * The caller may keep more pairs than necessary, but it has to keep every
 * (plan, h) for which at least plan->ntests - plan->k tests give an eye;
 * eyeSearchKeep() tests the pair exactly, so the hits are then exactly those
 * of scanEyeFamily(). The plans of a call stay valid until the family is
 * finished (they are copies), so several batches may be in flight.
 *
 * One test of a plan for the seed (high << 48) | low48: see "Data consumed by
 * stage (b)" above, with mx, mz and n from EyePlanTest, the decoration
 * multipliers from eyeSearchGetDecoration() and the salt from
 * eyeSearchSalt(). The test gives an eye if the top 24 bits of the next
 * output after n outputs exceed PORTAL_EYE_THRESHOLD. */
STRUCT(EyePlanTest)
{
    int32_t mx, mz;     // 16 * chunkX, 16 * chunkZ of the frame chunk
    uint32_t n;         // RNG outputs before the tested draw
};

STRUCT(EyePlan)
{
    int group;          // candidate group (engine internal)
    int ntests;         // tests (counted frames), minEyes .. 12
    int k;              // allowed misses: ntests - minEyes
    EyePlanTest test[12];
};

/* eyeSearchNextPlans() needs room for at least this many plans. */
enum { EYESEARCH_MIN_PLANS = 256 };

/* Starts the split scan of the 65536 seeds of a family (stage (a) setup). */
void eyeSearchBegin(EyeSearch *es, uint64_t low48);
/* Stage (a): the next up to 'max' (>= EYESEARCH_MIN_PLANS) plans. Groups
 * that are cheaper to confirm directly are marked here and give no plans.
 * Returns the number of plans, 0 once the family has no more (then
 * eyeSearchPlansDone() is non-zero), or -1 if stopped. */
int eyeSearchNextPlans(EyeSearch *es, EyePlan *plans, int max,
    const volatile int *stop);
/* Non-zero once eyeSearchNextPlans() has handled every candidate group of the
 * family. Callers may check it when they get 0 plans. */
int eyeSearchPlansDone(const EyeSearch *es);
/* The decoration multipliers of the current family: ab[2h] = a and
 * ab[2h+1] = b of seed high h (65536 pairs). */
void eyeSearchGetDecoration(const EyeSearch *es, uint64_t *ab);
uint64_t eyeSearchSalt(const EyeSearch *es);
uint64_t eyeSearchFamily(const EyeSearch *es);  // low48 of the current family
/* Tests (plan, high) exactly and marks the seed for confirmation if the plan
 * reaches minEyes. Returns 1 if marked, 0 if not, -1 for invalid input. */
int eyeSearchKeep(EyeSearch *es, const EyePlan *plan, int high);
/* Stage (b) on the CPU for the given plans (as scanEyeFamily() does).
 * Returns 0, or -1 if stopped. */
int eyeSearchSweep(EyeSearch *es, const EyePlan *plans, int n,
    const volatile int *stop);
/* Stage (c): confirms the marked seeds and reports the hits. Returns the
 * number of hits, or -1 if aborted (as scanEyeFamily()). */
int eyeSearchFinish(EyeSearch *es, EyeSearchCallback cb, void *user,
    const volatile int *stop);
/* The columns of M^n (n Xoroshiro128++ steps over GF(2)): col[j] = (lo, hi)
 * of the state that unit vector j (bit j of lo for j < 64, else bit j-64 of
 * hi) becomes. For building other forms of eye tables. */
void eyeSearchJumpColumns(const EyeSearch *es, uint32_t n, uint64_t col[128][2]);

#ifdef __cplusplus
}
#endif

#endif /* EYESEARCH_H_ */
