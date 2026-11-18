/* Compares the stronghold layouts and End portal eyes of strongholds.c with
 * reference data from real vanilla chunk generation
 * (tools/portal-reference/data/portals_<mc>.txt and pieces_<mc>.txt).
 * Files portals_<mc>_amplified.txt and pieces_<mc>_amplified.txt hold the same
 * for worlds of the type Amplified: the layouts are checked in the same way,
 * the stronghold positions with the AMPLIFIED generator flag (the positions
 * are snapped to biomes, which depend on the amplified depth).
 *
 * Usage: test-portal-reference [data dir]
 *        (default: ../tools/portal-reference/data)
 *
 * Exit status 0 iff every start chunk, piece list and frame position matches
 * and every frame that strongholds.c flags as exact has the reference eye
 * state; 1 if a layout, frame position or exact eye differs; 3 if only start
 * chunks (stronghold positions from finders.c) differ. Eye states of
 * non-exact frames are reported, not enforced.
 */
#include "strongholds.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    long strongholds, shChecked, shOk;
    long layoutMissing, startOk, startBad, startUnknown;
    long piecesOk, piecesBad;
    long framesOk, framesBad;
    long exactFrames, exactMatch, nonexactFrames, nonexactMatch, nonexactAirMatch;
    long portalsFullyExact, portalsMatchAll;
    long startFound, startNotFound; // Amplified, lines without an index
} Stats;

static int pieceTypeFromId(const char *id)
{
    const char *c = strchr(id, ':');
    if (c)
        id = c + 1;
    for (int t = 0; t < SH_PIECE_TYPE_NUM; t++)
    {
        const char *n = getStrongholdPieceName(t);
        size_t i;
        for (i = 0; n[i] && id[i]; i++)
        {
            char a = n[i], b = id[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b)
                break;
        }
        if (!n[i] && !id[i])
            return t;
    }
    return -1;
}

/* Direction.get2DDataValue(): SOUTH 0, WEST 1, NORTH 2, EAST 3 */
static int facingTo2D(int f)
{
    static const int v[4] = { 2, 3, 0, 1 }; // N, E, S, W
    return v[f & 3];
}

static int startChunkOf(int mc, int amplified, uint64_t seed, int index, Pos *out)
{
    static Generator g;
    static int gmc = -1, gamp = -1;
    static uint64_t gseed;
    if (gmc != mc || gamp != amplified)
    {
        setupGenerator(&g, mc, amplified ? AMPLIFIED : 0);
        gmc = mc;
        gamp = amplified;
        gseed = ~seed;
    }
    if (gseed != seed)
    {
        applySeed(&g, DIM_OVERWORLD, seed);
        gseed = seed;
    }
    StrongholdIter it;
    initFirstStronghold(&it, mc, seed);
    for (int i = 0; i <= index; i++)
        if (nextStronghold(&it, &g) <= 0 && i < index)
            return 0;
    out->x = it.pos.x >> 4;
    out->z = it.pos.z >> 4;
    return 1;
}

/* pieces_<mc>.txt: compare every listed layout */
static void checkPieces(const char *path, int mc, Stats *st, int verbose)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    static Stronghold sh;
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp))
    {
        char mcs[32];
        long long seed;
        int cx, cz, n;
        if (line[0] != 'S' || sscanf(line, "S %31s %lld %d %d %d", mcs, &seed, &cx, &cz, &n) != 5)
            continue;
        int got = getStrongholdLayout(&sh, mc, (uint64_t) seed, cx, cz);
        int ok = (got == n);
        for (int i = 0; i < n; i++)
        {
            char id[64];
            int b[6], o, d;
            if (!fgets(line, sizeof(line), fp) ||
                sscanf(line, "%63s %d %d %d %d %d %d %d %d", id, b, b+1, b+2, b+3, b+4, b+5, &o, &d) != 9)
            {
                ok = 0;
                break;
            }
            if (i >= got)
                continue;
            const StrongholdPiece *p = &sh.pieces[i];
            if (pieceTypeFromId(id) != p->type ||
                b[0] != p->x0 || b[1] != p->y0 || b[2] != p->z0 ||
                b[3] != p->x1 || b[4] != p->y1 || b[5] != p->z1 ||
                o != facingTo2D(p->facing) || d != p->depth)
            {
                if (ok && verbose)
                    printf("  piece %d differs: ref '%s' vs %s %d %d %d %d %d %d o=%d d=%d\n",
                        i, line, getStrongholdPieceName(p->type), p->x0, p->y0, p->z0,
                        p->x1, p->y1, p->z1, facingTo2D(p->facing), p->depth);
                ok = 0;
            }
        }
        if (ok)
            st->piecesOk++;
        else
        {
            st->piecesBad++;
            printf("  LAYOUT MISMATCH %s seed %lld start %d %d (ref %d pieces, got %d)\n",
                mcs, seed, cx, cz, n, got);
        }
    }
    fclose(fp);
}

/* portals_<mc>.txt: frames and eyes */
static void checkPortals(const char *path, int mc, int amplified, Stats *st, int verbose)
{
    FILE *fp = fopen(path, "r");
    char line[2048];
    static Stronghold sh;
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        char mcs[32];
        long long seed;
        int idx, cx, cz, off;
        if (sscanf(line, "%31s %lld %d %d %d%n", mcs, &seed, &idx, &cx, &cz, &off) != 5)
            continue;
        int rf[12][4];
        const char *s = line + off;
        int nf = 0;
        for (; nf < 12; nf++)
        {
            int k;
            if (sscanf(s, "%d %d %d %d%n", &rf[nf][0], &rf[nf][1], &rf[nf][2], &rf[nf][3], &k) != 4)
                break;
            s += k;
        }
        st->strongholds++;
        if (nf != 12)
        {
            printf("  bad line: %s", line);
            continue;
        }

        if (idx >= 0)
        {
            Pos sc;
            if (startChunkOf(mc, amplified, (uint64_t) seed, idx, &sc) && sc.x == cx && sc.z == cz)
                st->startOk++;
            else
            {
                st->startBad++;
                printf("  START MISMATCH %s seed %lld sh %d: ref %d %d, engine %d %d\n",
                    mcs, seed, idx, cx, cz, sc.x, sc.z);
            }
        }
        else
        {
            st->startUnknown++;
            if (amplified)
            {   // No vanilla index (the position comes from /locate): it has
                // to be one of the engine's strongholds of the first rings.
                int i, found = 0;
                for (i = 0; i < 19 && !found; i++)
                {
                    Pos sc;
                    found = startChunkOf(mc, amplified, (uint64_t) seed, i, &sc) &&
                        sc.x == cx && sc.z == cz;
                }
                if (found)
                    st->startFound++;
                else
                {
                    st->startNotFound++;
                    printf("  START NOT FOUND %s seed %lld: ref %d %d is none of the engine's"
                        " first 19 Amplified strongholds\n", mcs, seed, cx, cz);
                }
            }
        }

        EndPortal ep;
        PortalPlan pp;
        if (!getEndPortal(&ep, &sh, mc, (uint64_t) seed, cx, cz) || !getPortalPlan(&pp, &sh))
        {
            st->layoutMissing++;
            printf("  NO LAYOUT %s seed %lld start %d %d\n", mcs, seed, cx, cz);
            continue;
        }
        // alternative best effort: assume all wall cells of earlier pieces are air
        for (int j = 0; j < pp.nchunks; j++)
            pp.chunks[j].skip -= pp.chunks[j].wallCells;
        uint16_t airEyes = evalPortalEyes(&pp, (uint64_t) seed, 0);
        st->shChecked++;
        int allExact = (ep.exact == 0xfff), allMatch = 1, framesOk = 1;
        for (int f = 0; f < 12; f++)
        {
            int i;
            for (i = 0; i < 12; i++)
                if (ep.frame[i].x == rf[f][0] && ep.frame[i].y == rf[f][1] && ep.frame[i].z == rf[f][2])
                    break;
            if (i == 12)
            {
                framesOk = 0;
                continue;
            }
            int eye = ep.eyes >> i & 1;
            int ex = ep.exact >> i & 1;
            int match = (eye == rf[f][3]);
            allMatch &= match;
            if (ex)
            {
                st->exactFrames++;
                st->exactMatch += match;
                if (!match)
                    printf("  EXACT EYE MISMATCH %s seed %lld start %d %d frame %d %d %d: ref %d engine %d\n",
                        mcs, seed, cx, cz, rf[f][0], rf[f][1], rf[f][2], rf[f][3], eye);
            }
            else
            {
                st->nonexactFrames++;
                st->nonexactMatch += match;
                st->nonexactAirMatch += ((airEyes >> i & 1) == rf[f][3]);
                if (!match && verbose)
                    printf("  non-exact eye differs %s seed %lld start %d %d frame %d %d %d: ref %d engine %d\n",
                        mcs, seed, cx, cz, rf[f][0], rf[f][1], rf[f][2], rf[f][3], eye);
            }
        }
        if (framesOk)
            st->framesOk++;
        else
        {
            st->framesBad++;
            printf("  FRAME POSITION MISMATCH %s seed %lld start %d %d\n", mcs, seed, cx, cz);
        }
        st->portalsFullyExact += allExact;
        st->portalsMatchAll += allMatch;
        st->shOk += framesOk && allMatch;
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../tools/portal-reference/data";
    int verbose = getenv("VERBOSE") != NULL;
    DIR *d = opendir(dir);
    struct dirent *e;
    int files = 0, fail = 0, startWarn = 0;

    if (!d)
    {
        printf("reference data directory '%s' not found\n", dir);
        return 2;
    }
    while ((e = readdir(d)) != NULL)
    {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (strncmp(n, "portals_", 8) != 0 || len < 13 || strcmp(n + len - 4, ".txt") != 0)
            continue;
        char ver[64], name[64], path[1024];
        snprintf(name, sizeof(name), "%.*s", (int)(len - 12), n + 8);
        snprintf(ver, sizeof(ver), "%s", name);
        // portals_<mc>_amplified.txt: a world of the type Amplified
        int amplified = 0;
        size_t vlen = strlen(ver);
        if (vlen > 10 && strcmp(ver + vlen - 10, "_amplified") == 0)
        {
            amplified = 1;
            ver[vlen - 10] = 0;
        }
        int mc = str2mc(ver);
        if (mc < MC_1_18 || mc > MC_26_3)
        {
            printf("%s: version '%s' not supported, skipped\n", n, ver);
            continue;
        }
        Stats st;
        memset(&st, 0, sizeof(st));
        snprintf(path, sizeof(path), "%s/pieces_%s.txt", dir, name);
        checkPieces(path, mc, &st, verbose);
        snprintf(path, sizeof(path), "%s/%s", dir, n);
        checkPortals(path, mc, amplified, &st, verbose);
        files++;

        printf("%s (%s): %ld strongholds\n", name, mc2str(mc), st.strongholds);
        printf("  start chunk  : %ld match, %ld differ, %ld without index\n",
            st.startOk, st.startBad, st.startUnknown);
        if (amplified && st.startUnknown)
        {
            printf("  without index: %ld are among the engine's Amplified strongholds, %ld are not\n",
                st.startFound, st.startNotFound);
            st.startBad += st.startNotFound;
        }
        printf("  piece lists  : %ld match, %ld differ\n", st.piecesOk, st.piecesBad);
        printf("  frame coords : %ld match, %ld differ, %ld no layout\n",
            st.framesOk, st.framesBad, st.layoutMissing);
        printf("  exact eyes   : %ld / %ld match\n", st.exactMatch, st.exactFrames);
        printf("  non-exact    : %ld / %ld match (best effort, no-air assumption);"
            " %ld with an all-air assumption\n",
            st.nonexactMatch, st.nonexactFrames, st.nonexactAirMatch);
        printf("  portals      : %ld fully exact, %ld with all 12 eyes matching\n",
            st.portalsFullyExact, st.portalsMatchAll);
        if (st.piecesBad || st.framesBad || st.layoutMissing ||
            st.exactMatch != st.exactFrames)
            fail = 1;
        if (st.startBad)
        {
            startWarn = 1;
            printf("  WARNING: start chunks come from initFirstStronghold()/nextStronghold()"
                " in finders.c, not from strongholds.c\n");
        }
    }
    closedir(d);
    if (!files)
    {
        printf("no portals_<mc>.txt files in '%s'\n", dir);
        return 2;
    }
    printf(fail ? "FAILED\n" : startWarn ?
        "OK (layouts and exact eyes), but some stronghold positions differ\n" : "OK\n");
    return fail ? 1 : startWarn ? 3 : 0;
}
