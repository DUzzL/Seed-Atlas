/* Regression tests for the stronghold positions of initFirstStronghold() and
 * nextStronghold() (finders.c) against vanilla Minecraft.
 *
 * Every expected start chunk below is the vanilla concentric-ring position
 * (ChunkGeneratorStructureState / ChunkGenerator ringPositions) printed by a
 * real dedicated server of that version for that seed and ring index. The
 * cases were sampled from 13,824 strongholds of 26.3 (108 seeds) and 3,072 to
 * 5,120 strongholds (24 to 40 seeds) of each of 1.18.2, 1.19.2, 1.19.4,
 * 1.20.1, 1.20.6, 1.21.1, 1.21.2, 1.21.4, 1.21.5, 1.21.8, 1.21.9, 1.21.11,
 * 26.1 and 26.2.
 *
 * Comments: "fixed" = wrong before the stronghold_biased_to corrections
 * (cherry_grove only from 1.21.9, never dappled_forest), "cherry" = 1.21.9+
 * cases that need cherry_grove to be valid, "ref" = the cases of
 * tools/portal-reference/data.
 *
 * Only ring indices up to 40 are used, to keep the test fast. g_known lists
 * the remaining known differences in that range (vanilla, then engine). They
 * are reported, not enforced. Causes found for all remaining differences:
 *  - 26.3 rebuilt the climate R-tree with 19 children per node (6 before),
 *    so exact distance ties between parameter points resolve differently;
 *    the engine still searches the 1.21.4 tree. Fixing it needs a 26.3 tree.
 *  - 26.2 builds its R-tree with the Sulfur Caves point (7594 leaves); the
 *    engine searches the 1.21.4 tree and compares that point afterwards,
 *    so a few ties (e.g. savanna/beach) resolve differently.
 *  - seed 2844090353585370607 #103 (1.19.4 .. 26.1, index > 40, so not
 *    listed): the depth parameter of one cell differs by 1 (engine 4204,
 *    vanilla 4203) where river and dripstone_caves tie.
 *
 * Usage: test-stronghold-positions
 * Exit status 0 iff every case in g_cases matches.
 */
#include "finders.h"
#include "util.h"

#include <stdint.h>
#include <stdio.h>

typedef struct { int mc; int64_t seed; int index, cx, cz; } ShPosCase;
typedef struct { int mc; int64_t seed; int index, cx, cz, ecx, ecz; } ShPosKnown;

static const ShPosCase g_cases[] = {
    { MC_1_18_2,              INT64_MIN,  33,  -664,  -180 },
    { MC_1_18_2, -2585411958573129678LL,  14,  -463,    70 },
    { MC_1_18_2,                  -42LL,  13,   -20,  -497 },
    { MC_1_18_2,                    1LL,  38,  -244,  -871 },
    { MC_1_18_2,  6027786629280759191LL,   0,   109,  -107 },
    { MC_1_18_2,  8471536028894645041LL,  36,   314,   855 },
    { MC_1_19_2, -8383263537365765080LL,  16,   239,  -470 },
    { MC_1_19_2, -6254896864395726207LL,  26,   267,  -626 },
    { MC_1_19_2, -5637252045523159586LL,  10,  -227,   479 },
    { MC_1_19_2, -1276250787475158968LL,  21,  -462,   483 },
    { MC_1_19_2, -1217653759622919644LL,  15,   487,   135 },
    { MC_1_19_2,                12345LL,  10,   447,   324 },
    { MC_1_19_4,              INT64_MIN,  32,  -692,   271 },
    { MC_1_19_4, -5758315005108273411LL,  38,  -231,   837 },
    { MC_1_19_4, -2585411958573129678LL,  24,  -647,  -279 },
    { MC_1_19_4,                    0LL,  34,  -524,  -678 },
    { MC_1_19_4,  4960603779658265448LL,  20,   656,   336 },
    { MC_1_19_4,  6027786629280759191LL,  26,   383,   578 },
    { MC_1_20_6, -8711875192544430397LL,   2,  -134,    48 },  // fixed
    { MC_1_20_6, -8711875192544430397LL,  17,  -473,    39 },  // fixed
    { MC_1_20_6, -8711875192544430397LL,  25,  -271,   650 },  // fixed
    { MC_1_20_6, -5913132262601449588LL,   7,  -203,  -250 },  // fixed
    { MC_1_20_6, -5325319943810965777LL,   7,   -14,  -299 },  // fixed
    { MC_1_20_6, -5325319943810965777LL,  29,  -661,  -348 },  // fixed
    { MC_1_20_6, -4294938300906460663LL,   9,   462,   174 },  // fixed
    { MC_1_20_6, -1276250787475158968LL,  25,   315,  -645 },
    { MC_1_20_6, -1217653759622919644LL,   5,  -100,   306 },
    { MC_1_20_6,   765922469937924989LL,   7,   178,   249 },  // fixed
    { MC_1_20_6,  1521491129459179100LL,   7,   190,   256 },
    { MC_1_20_6,  1521491129459179100LL,  32,   616,  -346 },
    { MC_1_20_6,  2844090353585370607LL,  16,   465,   115 },  // fixed
    { MC_1_20_6,  3210390158486947509LL,   3,  -106,   281 },  // fixed
    { MC_1_20_6,  5134011547551940572LL,  13,   459,  -284 },  // fixed
    { MC_1_20_6,  8515313184317428878LL,  18,  -239,   463 },  // fixed
    { MC_1_21_1,              INT64_MIN,  37,   200,  -853 },
    { MC_1_21_1, -8711875192544430397LL,   2,  -134,    48 },  // fixed
    { MC_1_21_1, -8711875192544430397LL,  16,  -364,   317 },  // fixed
    { MC_1_21_1, -8383263537365765080LL,   6,  -298,  -167 },  // fixed
    { MC_1_21_1, -8383263537365765080LL,  29,   489,  -447 },  // fixed
    { MC_1_21_1, -5768582347884958694LL,   1,   147,    67 },  // fixed
    { MC_1_21_1, -5768582347884958694LL,   3,  -314,   -48 },  // fixed
    { MC_1_21_1, -5758315005108273411LL,  32,   556,   406 },  // fixed
    { MC_1_21_1, -2163032611597832988LL,   0,     4,   136 },  // fixed
    { MC_1_21_1, -1217653759622919644LL,  11,    39,   503 },
    { MC_1_21_1,  -946252964296086649LL,  20,   534,   456 },  // fixed
    { MC_1_21_1,                  -42LL,  27,   127,   694 },
    { MC_1_21_1,                   -1LL,  16,   -68,   500 },
    { MC_1_21_1,                12345LL,   1,   -34,  -103 },  // fixed
    { MC_1_21_1,                12345LL,  16,   160,  -483 },  // fixed
    { MC_1_21_1,  2844090353585370607LL,  16,   465,   115 },  // fixed
    { MC_1_21_1,  2844090353585370607LL,  36,  -753,   470 },  // fixed
    { MC_1_21_1,  3210390158486947509LL,   2,   116,    -4 },
    { MC_1_21_1,  3210390158486947509LL,   3,  -106,   281 },  // fixed
    { MC_1_21_1,  4712153430222316363LL,  33,  -442,   566 },  // fixed
    { MC_1_21_1,  5134011547551940572LL,  13,   459,  -284 },  // fixed
    { MC_1_21_1,  6027786629280759191LL,   5,   115,  -344 },  // fixed
    { MC_1_21_1,  8515313184317428878LL,   5,    79,  -342 },  // fixed
    { MC_1_21_1,  8515313184317428878LL,  13,   211,  -419 },
    { MC_1_21_1,  8515313184317428878LL,  18,  -239,   463 },  // fixed
    { MC_1_21_1,  9223372036854775807LL,  29,  -635,   233 },  // fixed
    { MC_1_21_11, -7152204650183302433LL,  24,   206,   637 },
    { MC_1_21_11, -4096410119449981659LL,  31,   644,   351 },
    { MC_1_21_11, -2163032611597832988LL,  12,  -154,   491 },
    { MC_1_21_11, -2163032611597832988LL,  24,  -464,   581 },
    { MC_1_21_11, -2163032611597832988LL,  33,   694,  -189 },
    { MC_1_21_11,  2844090353585370607LL,   9,  -288,   440 },
    { MC_1_21_11,  6027786629280759191LL,  10,   396,   281 },
    { MC_1_21_11,  6971765479610566834LL,   3,   260,   205 },
    { MC_1_21_8, -8711875192544430397LL,  17,  -473,    39 },  // fixed
    { MC_1_21_8, -5913132262601449588LL,   3,   289,   -47 },  // fixed
    { MC_1_21_8, -5768582347884958694LL,  26,  -528,  -409 },
    { MC_1_21_8, -5758315005108273411LL,  32,   556,   406 },  // fixed
    { MC_1_21_8, -1881332783362805210LL,  22,   -72,  -724 },  // fixed
    { MC_1_21_8, -1276250787475158968LL,  16,   101,   492 },
    { MC_1_21_8, -1217653759622919644LL,  21,   571,  -359 },
    { MC_1_21_8,  -946252964296086649LL,   5,  -282,   145 },  // fixed
    { MC_1_21_8,   765922469937924989LL,   7,   178,   249 },  // fixed
    { MC_1_21_8,  4712153430222316363LL,  33,  -442,   566 },  // fixed
    { MC_1_21_8,  4960603779658265448LL,   7,  -225,  -271 },
    { MC_1_21_8,  5134011547551940572LL,  37,  -883,   291 },  // fixed
    { MC_1_21_8,  5134011547551940572LL,  39,  -875,  -253 },  // fixed
    { MC_1_21_8,  7302286388936974606LL,   1,    -8,  -148 },  // fixed
    { MC_1_21_8,  8515313184317428878LL,   5,    79,  -342 },  // fixed
    { MC_1_21_8,  9223372036854775807LL,  29,  -635,   233 },  // fixed
    { MC_1_21_9, -5913132262601449588LL,   3,   282,   -52 },  // cherry
    { MC_1_21_9, -5768582347884958694LL,   1,   151,    67 },  // cherry
    { MC_1_21_9, -5758315005108273411LL,  32,   559,   406 },  // cherry
    { MC_1_21_9, -5325319943810965777LL,   7,   -18,  -300 },  // cherry
    { MC_1_21_9, -5325319943810965777LL,  36,   160,   887 },  // cherry
    { MC_1_21_9,  3210390158486947509LL,   3,  -104,   280 },  // cherry
    { MC_1_21_9,  4712153430222316363LL,  33,  -446,   566 },  // cherry
    { MC_1_21_9,  8515313184317428878LL,  18,  -238,   462 },  // cherry
    { MC_26_1,  -7893058343860395115LL,  37,   310,   800 },
    { MC_26_1,  -5768582347884958694LL,  17,   370,  -408 },
    { MC_26_1,  -5758315005108273411LL,  35,   551,   760 },
    { MC_26_1,  -2163032611597832988LL,  18,   406,  -305 },
    { MC_26_1,   4960603779658265448LL,  12,  -522,     8 },
    { MC_26_1,   6027786629280759191LL,  17,   145,  -466 },
    { MC_26_1,   7302286388936974606LL,   0,   -93,    49 },
    { MC_26_1,   9223372036854775807LL,  36,  -892,  -119 },
    { MC_26_2,  -8383263537365765080LL,  17,  -526,   145 },
    { MC_26_2,  -7152204650183302433LL,   1,    -2,   -91 },
    { MC_26_2,  -5758315005108273411LL,   2,   -65,    73 },
    { MC_26_2,                   -42LL,  12,   285,  -416 },
    { MC_26_2,                 12345LL,   7,  -179,  -308 },
    { MC_26_2,   1521491129459179100LL,  27,  -588,  -346 },
    { MC_26_2,   4712153430222316363LL,   1,   128,   -39 },
    { MC_26_2,   6971765479610566834LL,  40,   756,  -491 },
    { MC_26_3,               INT64_MIN,   0,   -16,  -111 },  // fixed
    { MC_26_3,  -8365776648368433233LL,  18,  -484,  -107 },  // fixed
    { MC_26_3,  -7893058343860395115LL,  15,  -224,  -498 },
    { MC_26_3,  -7442104260772907672LL,  29,   614,   276 },  // fixed
    { MC_26_3,  -7152204650183302433LL,  31,  -330,  -581 },  // fixed
    { MC_26_3,  -5913132262601449588LL,   7,  -213,  -248 },  // fixed
    { MC_26_3,  -5758315005108273411LL,  13,   390,  -336 },  // fixed
    { MC_26_3,  -5325319943810965777LL,  11,   219,   453 },
    { MC_26_3,  -4272783757762851690LL,   6,  -166,  -293 },  // fixed
    { MC_26_3,  -4272783757762851690LL,  24,   628,  -369 },  // fixed
    { MC_26_3,  -4030668432897588236LL,  20,   589,   314 },  // fixed
    { MC_26_3,  -4030668432897588236LL,  21,   438,   567 },  // fixed
    { MC_26_3,  -2865522108614902933LL,  13,   469,  -276 },  // fixed
    { MC_26_3,  -1881332783362805210LL,   3,  -282,  -204 },  // fixed
    { MC_26_3,  -1217653759622919644LL,  35,   292,   856 },
    { MC_26_3,   -946252964296086649LL,   5,  -283,   146 },  // fixed
    { MC_26_3,   -946252964296086649LL,  18,   407,  -258 },  // fixed
    { MC_26_3,   -644251687506663611LL,   3,    53,   297 },
    { MC_26_3,   -644251687506663611LL,   7,   209,  -184 },
    { MC_26_3,   -644251687506663611LL,   8,   285,   104 },  // fixed
    { MC_26_3,   -185936925820507990LL,  13,   -18,   524 },  // fixed
    { MC_26_3,      -214992467332240LL,  12,   494,   221 },
    { MC_26_3,      -210852920683299LL,   9,   -23,  -493 },  // fixed
    { MC_26_3,      -138877386356727LL,  16,  -482,   122 },
    { MC_26_3,      -138877386356727LL,  28,  -555,   377 },  // fixed
    { MC_26_3,      -138877386356727LL,  36,  -307,   813 },
    { MC_26_3,       -78324223464332LL,   6,   214,   275 },
    { MC_26_3,            -757793488LL,  10,   507,   103 },  // fixed
    { MC_26_3,            -130154378LL,   6,   123,   299 },
    { MC_26_3,            -130154378LL,   9,   228,  -484 },  // fixed
    { MC_26_3,            -130154378LL,  27,  -617,  -309 },  // fixed
    { MC_26_3,            -130154378LL,  29,  -177,  -666 },  // fixed
    { MC_26_3,                   -42LL,  34,   383,  -787 },  // fixed
    { MC_26_3,                     0LL,  14,   141,   515 },  // fixed
    { MC_26_3,                     1LL,  32,  -690,   156 },  // fixed
    { MC_26_3,                 12345LL,  16,   165,  -484 },
    { MC_26_3,             184252217LL,   8,   -61,  -307 },  // fixed
    { MC_26_3,             760748523LL,  14,   517,    85 },
    { MC_26_3,             767975338LL,  27,   725,    -6 },  // fixed
    { MC_26_3,             767975338LL,  31,   -70,   698 },  // fixed
    { MC_26_3,             790300812LL,  23,  -322,  -591 },  // fixed
    { MC_26_3,             864052806LL,  17,   451,  -202 },
    { MC_26_3,            1707868881LL,  37,  -475,   724 },  // fixed
    { MC_26_3,        45107820682373LL,  12,    35,  -475 },  // fixed
    { MC_26_3,       138983759820774LL,   2,   -49,   -96 },
    { MC_26_3,       200357556440134LL,  21,  -516,   538 },
    { MC_26_3,   1045808438710157515LL,  24,  -434,  -536 },  // fixed
    { MC_26_3,   1521491129459179100LL,   5,   136,  -295 },  // fixed
    { MC_26_3,   2752529985321314232LL,  37,    57,  -861 },
    { MC_26_3,   4712153430222316363LL,  33,  -443,   553 },  // fixed
    { MC_26_3,   4979489264426476242LL,   4,     4,   285 },  // fixed
    { MC_26_3,   6027786629280759191LL,   5,   118,  -344 },  // fixed
    { MC_26_3,   6599734236935463037LL,  34,  -256,  -898 },  // fixed
    { MC_26_3,   7302286388936974606LL,  14,  -495,   138 },  // fixed
    { MC_26_3,   8239240105686726951LL,  11,    74,   512 },  // fixed
    { MC_26_3,   8239240105686726951LL,  15,  -379,  -378 },  // fixed
};

static const ShPosKnown g_known[] = {
    { MC_26_2,  -5325319943810965777LL,  32,   117,  -682,   119,  -682 },
    { MC_26_3,  -5913132262601449588LL,  18,  -426,  -282,  -427,  -282 },
    { MC_26_3,  -5325319943810965777LL,  29,  -658,  -348,  -657,  -348 },
    { MC_26_3,  -5325319943810965777LL,  32,   117,  -682,   119,  -682 },
    { MC_26_3,  -2865522108614902933LL,  26,  -549,   432,  -548,   432 },
    { MC_26_3,  -2585411958573129678LL,  24,  -648,  -279,  -647,  -279 },
    { MC_26_3,   -946252964296086649LL,  21,   295,   641,   302,   641 },
    { MC_26_3,   -682390310338198715LL,  19,   259,  -686,   260,  -686 },
    { MC_26_3,      -132159036920835LL,  28,  -716,  -101,  -715,  -101 },
    { MC_26_3,       -78324223464332LL,   2,    61,  -139,    60,  -139 },
    { MC_26_3,           -1673027849LL,   7,  -101,  -283,  -112,  -282 },
    { MC_26_3,            -757793488LL,   4,  -358,   -35,  -357,   -35 },
    { MC_26_3,            -549422536LL,  38,  -655,   617,  -656,   617 },
    { MC_26_3,        45107820682373LL,  20,   584,   353,   585,   353 },
    { MC_26_3,    765922469937924989LL,  12,  -390,   257,  -389,   257 },
    { MC_26_3,   4712153430222316363LL,   8,   284,  -138,   279,  -138 },
    { MC_26_3,   7793571252795489780LL,  14,  -318,   392,  -317,   392 },
    { MC_26_3,   8087610117231461417LL,  30,  -130,  -693,  -131,  -693 },
};

/* Start chunk of stronghold <index> as the engine computes it. Keeps the
 * iterator between calls, so increasing indices of one seed are cheap. */
static Pos engineStart(int mc, uint64_t seed, int index)
{
    static Generator g;
    static StrongholdIter it;
    static int gmc = -1, next = 0;
    static uint64_t gseed;
    static Pos last;
    if (gmc != mc)
    {
        setupGenerator(&g, mc, 0);
        gmc = mc;
        gseed = ~seed;
    }
    if (gseed != seed || index < next - 1)
    {
        if (gseed != seed)
            applySeed(&g, DIM_OVERWORLD, seed);
        gseed = seed;
        initFirstStronghold(&it, mc, seed);
        next = 0;
    }
    while (next <= index)
    {
        nextStronghold(&it, &g);
        last.x = it.pos.x >> 4;
        last.z = it.pos.z >> 4;
        next++;
    }
    return last;
}

int main(void)
{
    int n = sizeof(g_cases) / sizeof(g_cases[0]);
    int nk = sizeof(g_known) / sizeof(g_known[0]);
    int bad = 0, kstill = 0;

    for (int i = 0; i < n; i++)
    {
        const ShPosCase *c = &g_cases[i];
        Pos p = engineStart(c->mc, (uint64_t) c->seed, c->index);
        if (p.x != c->cx || p.z != c->cz)
        {
            printf("MISMATCH %s seed %lld #%d: vanilla %d %d, engine %d %d\n",
                mc2str(c->mc), (long long) c->seed, c->index, c->cx, c->cz, p.x, p.z);
            bad++;
        }
    }
    for (int i = 0; i < nk; i++)
    {
        const ShPosKnown *k = &g_known[i];
        Pos p = engineStart(k->mc, (uint64_t) k->seed, k->index);
        if (p.x == k->cx && p.z == k->cz)
            printf("known difference now matches vanilla: %s seed %lld #%d\n",
                mc2str(k->mc), (long long) k->seed, k->index);
        else
        {
            kstill++;
            if (p.x != k->ecx || p.z != k->ecz)
                printf("known difference changed: %s seed %lld #%d: vanilla %d %d, engine %d %d (was %d %d)\n",
                    mc2str(k->mc), (long long) k->seed, k->index, k->cx, k->cz, p.x, p.z, k->ecx, k->ecz);
        }
    }
    printf("stronghold positions: %d / %d match vanilla; %d / %d known differences remain\n",
        n - bad, n, kstill, nk);
    printf(bad ? "FAILED\n" : "OK\n");
    return bad ? 1 : 0;
}
