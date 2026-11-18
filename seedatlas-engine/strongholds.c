#include "strongholds.h"

#include <stdlib.h>
#include <string.h>


//==============================================================================
// Layout generation
//==============================================================================

static const char *sh_names[SH_PIECE_TYPE_NUM] = {
    "SHStart", "SHS", "SHPH", "SHLT", "SHRT", "SHRC", "SHSSD", "SHSD",
    "SH5C", "SHCC", "SHLi", "SHPR", "SHFC",
};

const char *getStrongholdPieceName(int type)
{
    if (type < 0 || type >= SH_PIECE_TYPE_NUM)
        return "?";
    return sh_names[type];
}

/* The weighted piece list (vanilla STRONGHOLD_PIECE_WEIGHTS), in order. */
enum { SH_WEIGHTS = 11 };
static const struct { uint8_t type, weight, max, mindepth; } sh_weights[SH_WEIGHTS] = {
    { SH_STRAIGHT,              40, 0, 0 },
    { SH_PRISON_HALL,            5, 5, 0 },
    { SH_LEFT_TURN,             20, 0, 0 },
    { SH_RIGHT_TURN,            20, 0, 0 },
    { SH_ROOM_CROSSING,         10, 6, 0 },
    { SH_STRAIGHT_STAIRS_DOWN,   5, 5, 0 },
    { SH_STAIRS_DOWN,            5, 5, 0 },
    { SH_FIVE_CROSSING,          5, 4, 0 },
    { SH_CHEST_CORRIDOR,         5, 4, 0 },
    { SH_LIBRARY,               10, 2, 5 }, // placed only at depth > 4
    { SH_PORTAL_ROOM,           20, 1, 6 }, // placed only at depth > 5
};

enum { SH_GRID = 33, SH_CELL_BITS = 3 };

typedef struct
{
    uint64_t rng;               // Java LCG (48 bit)
    StrongholdPiece *list;
    int n;
    int overflow;
    int startMinX, startMinZ;
    int portal;
    int untilPortal;            // stop as soon as the portal room exists
    int stop;
    int imposed;                // piece type imposed on the next door, or -1
    int prevType;               // last placed weighted type, or -1
    int nw;                     // entries in the current weight list
    uint8_t widx[SH_WEIGHTS];   // current weight list (indices into sh_weights)
    uint8_t wcount[SH_WEIGHTS]; // place counts (by sh_weights index)
    int totalWeight;
    int npend;
    uint16_t pend[STRONGHOLD_MAX_PIECES];
    // Spatial index for collision tests: 8x8 block columns around the
    // start. Every piece foot is within 112 blocks of the start piece and no
    // piece reaches more than 15 blocks past its foot, so 33x33 cells cover
    // all pieces, and a piece (at most 16 blocks wide) occupies at most 3x3.
    int gridX0, gridZ0;
    int16_t head[SH_GRID * SH_GRID];
    int16_t nodeNext[9 * STRONGHOLD_MAX_PIECES];
    uint16_t nodePiece[9 * STRONGHOLD_MAX_PIECES];
    int nnodes;
} ShEnv;

typedef struct { int x0, y0, z0, x1, y1, z1; } Box;

/* vanilla BoundingBox.orientBox() */
static inline Box orientBox(int x, int y, int z, int ox, int oy, int oz,
    int w, int h, int d, int facing)
{
    Box b;
    b.y0 = y + oy;
    b.y1 = y + h - 1 + oy;
    switch (facing)
    {
    case SH_NORTH:
        b.x0 = x + ox;          b.x1 = x + w - 1 + ox;
        b.z0 = z - d + 1 + oz;  b.z1 = z + oz;
        break;
    case SH_WEST:
        b.x0 = x - d + 1 + oz;  b.x1 = x + oz;
        b.z0 = z + ox;          b.z1 = z + w - 1 + ox;
        break;
    case SH_EAST:
        b.x0 = x + oz;          b.x1 = x + d - 1 + oz;
        b.z0 = z + ox;          b.z1 = z + w - 1 + ox;
        break;
    default: // SH_SOUTH
        b.x0 = x + ox;          b.x1 = x + w - 1 + ox;
        b.z0 = z + oz;          b.z1 = z + d - 1 + oz;
        break;
    }
    return b;
}

static inline int boxIntersects(const Box *a, const StrongholdPiece *p)
{
    return p->x1 >= a->x0 && p->x0 <= a->x1 &&
           p->z1 >= a->z0 && p->z0 <= a->z1 &&
           p->y1 >= a->y0 && p->y0 <= a->y1;
}

static inline int gridCell(int v, int v0)
{
    int c = (v - v0) >> SH_CELL_BITS;
    return c < 0 ? 0 : (c >= SH_GRID ? SH_GRID - 1 : c);
}

/* Register piece 'idx' in the spatial index. */
static void gridAdd(ShEnv *env, int idx)
{
    const StrongholdPiece *p = &env->list[idx];
    int cx0 = gridCell(p->x0, env->gridX0), cx1 = gridCell(p->x1, env->gridX0);
    int cz0 = gridCell(p->z0, env->gridZ0), cz1 = gridCell(p->z1, env->gridZ0);
    for (int cx = cx0; cx <= cx1; cx++)
    {
        for (int cz = cz0; cz <= cz1; cz++)
        {
            int c = cx * SH_GRID + cz;
            int k = env->nnodes++;
            env->nodePiece[k] = (uint16_t) idx;
            env->nodeNext[k] = env->head[c];
            env->head[c] = (int16_t) k;
        }
    }
}

/* Whether any piece intersects the box. Cells clamp at the grid border, so
 * a box outside the grid (which vanilla never tests) is still handled. */
static int anyCollision(const ShEnv *env, const Box *b)
{
    int cx0 = gridCell(b->x0, env->gridX0), cx1 = gridCell(b->x1, env->gridX0);
    int cz0 = gridCell(b->z0, env->gridZ0), cz1 = gridCell(b->z1, env->gridZ0);
    for (int cx = cx0; cx <= cx1; cx++)
    {
        for (int cz = cz0; cz <= cz1; cz++)
        {
            for (int k = env->head[cx * SH_GRID + cz]; k >= 0; k = env->nodeNext[k])
            {
                if (boxIntersects(b, &env->list[env->nodePiece[k]]))
                    return 1;
            }
        }
    }
    return 0;
}

/* vanilla StructurePiece.findCollisionPiece(): the first intersecting piece
 * in list order, or -1. */
static int findCollision(const ShEnv *env, const Box *b)
{
    int best = -1;
    int cx0 = gridCell(b->x0, env->gridX0), cx1 = gridCell(b->x1, env->gridX0);
    int cz0 = gridCell(b->z0, env->gridZ0), cz1 = gridCell(b->z1, env->gridZ0);
    for (int cx = cx0; cx <= cx1; cx++)
    {
        for (int cz = cz0; cz <= cz1; cz++)
        {
            for (int k = env->head[cx * SH_GRID + cz]; k >= 0; k = env->nodeNext[k])
            {
                int i = env->nodePiece[k];
                if ((best < 0 || i < best) && boxIntersects(b, &env->list[i]))
                    best = i;
            }
        }
    }
    return best;
}

static inline int okBox(const ShEnv *env, const Box *b)
{
    return b->y0 > 10 && !anyCollision(env, b);
}

static inline int randomSmallDoor(uint64_t *rng)
{
    switch (nextInt(rng, 5))
    {
    case 2: return SH_DOOR_WOOD;
    case 3: return SH_DOOR_GRATES;
    case 4: return SH_DOOR_IRON;
    default: return SH_DOOR_OPENING;
    }
}

/* Writes a new piece into the next free list slot without adding it (vanilla
 * adds it in generateAndAddPiece). Returns 0 if the slot does not exist. */
static inline StrongholdPiece *newPiece(ShEnv *env, int type, const Box *b,
    int facing, int depth)
{
    if (env->n >= STRONGHOLD_MAX_PIECES)
    {
        env->overflow = 1;
        return NULL;
    }
    StrongholdPiece *p = &env->list[env->n];
    p->x0 = b->x0; p->y0 = b->y0; p->z0 = b->z0;
    p->x1 = b->x1; p->y1 = b->y1; p->z1 = b->z1;
    p->type = (uint8_t) type;
    p->facing = (uint8_t) facing;
    p->depth = (uint8_t) depth;
    p->door = SH_DOOR_OPENING;
    p->data = 0;
    return p;
}

/* vanilla StrongholdPieces.findAndCreatePieceFactory() / *.createPiece() */
static StrongholdPiece *createPiece(ShEnv *env, int type,
    int x, int y, int z, int facing, int depth)
{
    StrongholdPiece *p;
    Box b;
    switch (type)
    {
    case SH_STRAIGHT:
        b = orientBox(x, y, z, -1, -1, 0, 5, 5, 7, facing);
        if (!okBox(env, &b) || !(p = newPiece(env, type, &b, facing, depth)))
            return NULL;
        p->door = randomSmallDoor(&env->rng);
        if (nextInt(&env->rng, 2) == 0) p->data |= SH_STRAIGHT_LEFT;
        if (nextInt(&env->rng, 2) == 0) p->data |= SH_STRAIGHT_RIGHT;
        return p;
    case SH_PRISON_HALL:
        b = orientBox(x, y, z, -1, -1, 0, 9, 5, 11, facing);
        break;
    case SH_LEFT_TURN:
    case SH_RIGHT_TURN:
        b = orientBox(x, y, z, -1, -1, 0, 5, 5, 5, facing);
        break;
    case SH_ROOM_CROSSING:
        b = orientBox(x, y, z, -4, -1, 0, 11, 7, 11, facing);
        if (!okBox(env, &b) || !(p = newPiece(env, type, &b, facing, depth)))
            return NULL;
        p->door = randomSmallDoor(&env->rng);
        p->data = (uint8_t) nextInt(&env->rng, 5);
        return p;
    case SH_STRAIGHT_STAIRS_DOWN:
        b = orientBox(x, y, z, -1, -7, 0, 5, 11, 8, facing);
        break;
    case SH_STAIRS_DOWN:
        b = orientBox(x, y, z, -1, -7, 0, 5, 11, 5, facing);
        break;
    case SH_FIVE_CROSSING:
        b = orientBox(x, y, z, -4, -3, 0, 10, 9, 11, facing);
        if (!okBox(env, &b) || !(p = newPiece(env, type, &b, facing, depth)))
            return NULL;
        p->door = randomSmallDoor(&env->rng);
        if (nextInt(&env->rng, 2)) p->data |= SH_5C_LEFT_LOW;   // nextBoolean
        if (nextInt(&env->rng, 2)) p->data |= SH_5C_LEFT_HIGH;
        if (nextInt(&env->rng, 2)) p->data |= SH_5C_RIGHT_LOW;
        if (nextInt(&env->rng, 3) > 0) p->data |= SH_5C_RIGHT_HIGH;
        return p;
    case SH_CHEST_CORRIDOR:
        b = orientBox(x, y, z, -1, -1, 0, 5, 5, 7, facing);
        break;
    case SH_LIBRARY:
        b = orientBox(x, y, z, -4, -1, 0, 14, 11, 15, facing);
        if (!okBox(env, &b))
        {
            b = orientBox(x, y, z, -4, -1, 0, 14, 6, 15, facing);
            if (!okBox(env, &b))
                return NULL;
        }
        if (!(p = newPiece(env, type, &b, facing, depth)))
            return NULL;
        p->door = randomSmallDoor(&env->rng);
        p->data = (b.y1 - b.y0 + 1 > 6) ? SH_LIBRARY_TALL : 0;
        return p;
    case SH_PORTAL_ROOM:
        b = orientBox(x, y, z, -4, -1, 0, 11, 8, 16, facing);
        if (!okBox(env, &b))
            return NULL;
        return newPiece(env, type, &b, facing, depth); // no RNG, opening door
    default:
        return NULL;
    }
    // simple pieces: box check, then only the entry door
    if (!okBox(env, &b) || !(p = newPiece(env, type, &b, facing, depth)))
        return NULL;
    p->door = randomSmallDoor(&env->rng);
    return p;
}

static void updatePieceWeight(ShEnv *env, int *hasAny)
{
    int any = 0, tot = 0;
    for (int i = 0; i < env->nw; i++)
    {
        int w = env->widx[i];
        int max = sh_weights[w].max;
        if (max > 0 && env->wcount[w] < max)
            any = 1;
        tot += sh_weights[w].weight;
    }
    env->totalWeight = tot;
    *hasAny = any;
}

/* vanilla FillerCorridor.findPieceBox() + the minY check of the caller */
static StrongholdPiece *createFiller(ShEnv *env, int x, int y, int z,
    int facing, int depth)
{
    Box b = orientBox(x, y, z, -1, -1, 0, 5, 5, 4, facing);
    int c = findCollision(env, &b);
    if (c < 0)
        return NULL;
    const StrongholdPiece *q = &env->list[c];
    if (q->y0 != b.y0)
        return NULL;
    for (int d = 2; d >= 1; d--)
    {
        b = orientBox(x, y, z, -1, -1, 0, 5, 5, d, facing);
        if (!boxIntersects(&b, q))
        {
            b = orientBox(x, y, z, -1, -1, 0, 5, 5, d + 1, facing);
            if (b.y0 <= 1)
                return NULL;
            return newPiece(env, SH_FILLER_CORRIDOR, &b, facing, depth);
        }
    }
    return NULL;
}

/* vanilla StrongholdPieces.generatePieceFromSmallDoor() */
static StrongholdPiece *pieceFromSmallDoor(ShEnv *env, int x, int y, int z,
    int facing, int depth)
{
    int hasAny;
    updatePieceWeight(env, &hasAny);
    if (!hasAny)
        return NULL;

    if (env->imposed >= 0)
    {
        StrongholdPiece *p = createPiece(env, env->imposed, x, y, z, facing, depth);
        env->imposed = -1;
        if (p)
            return p;
    }

    for (int attempt = 0; attempt < 5; attempt++)
    {
        int sel = nextInt(&env->rng, env->totalWeight);
        for (int i = 0; i < env->nw; i++)
        {
            int w = env->widx[i];
            sel -= sh_weights[w].weight;
            if (sel >= 0)
                continue;
            int max = sh_weights[w].max;
            int doPlace = (max == 0 || env->wcount[w] < max) &&
                depth >= sh_weights[w].mindepth;
            if (!doPlace || sh_weights[w].type == env->prevType)
                break;
            StrongholdPiece *p = createPiece(env, sh_weights[w].type,
                x, y, z, facing, depth);
            if (p)
            {
                env->wcount[w]++;
                env->prevType = sh_weights[w].type;
                if (max != 0 && env->wcount[w] >= max)
                {   // remove the exhausted entry, keep the order
                    memmove(env->widx + i, env->widx + i + 1, env->nw - i - 1);
                    env->nw--;
                }
                return p;
            }
            if (env->overflow)
                return NULL;
        }
    }

    return createFiller(env, x, y, z, facing, depth);
}

/* vanilla StrongholdPieces.generateAndAddPiece() */
static void generateAndAddPiece(ShEnv *env, int x, int y, int z, int facing,
    int depth)
{
    if (depth > 50 || env->overflow || env->stop)
        return;
    if (abs(x - env->startMinX) > 112 || abs(z - env->startMinZ) > 112)
        return;
    StrongholdPiece *p = pieceFromSmallDoor(env, x, y, z, facing, depth + 1);
    if (!p)
        return;
    env->pend[env->npend++] = (uint16_t) env->n;
    gridAdd(env, env->n);
    if (p->type == SH_PORTAL_ROOM && env->untilPortal)
    {   // everything before it in the list is known now (see header)
        env->portal = env->n;
        env->stop = 1;
    }
    env->n++;
}

/* vanilla generateSmallDoorChildForward/Left/Right() */
static void childForward(ShEnv *env, const StrongholdPiece *p, int xoff, int yoff)
{
    int y = p->y0 + yoff;
    switch (p->facing)
    {
    case SH_NORTH: generateAndAddPiece(env, p->x0 + xoff, y, p->z0 - 1, SH_NORTH, p->depth); break;
    case SH_SOUTH: generateAndAddPiece(env, p->x0 + xoff, y, p->z1 + 1, SH_SOUTH, p->depth); break;
    case SH_WEST:  generateAndAddPiece(env, p->x0 - 1, y, p->z0 + xoff, SH_WEST, p->depth); break;
    case SH_EAST:  generateAndAddPiece(env, p->x1 + 1, y, p->z0 + xoff, SH_EAST, p->depth); break;
    }
}

static void childLeft(ShEnv *env, const StrongholdPiece *p, int yoff, int zoff)
{
    int y = p->y0 + yoff;
    switch (p->facing)
    {
    case SH_NORTH:
    case SH_SOUTH: generateAndAddPiece(env, p->x0 - 1, y, p->z0 + zoff, SH_WEST, p->depth); break;
    case SH_WEST:
    case SH_EAST:  generateAndAddPiece(env, p->x0 + zoff, y, p->z0 - 1, SH_NORTH, p->depth); break;
    }
}

static void childRight(ShEnv *env, const StrongholdPiece *p, int yoff, int zoff)
{
    int y = p->y0 + yoff;
    switch (p->facing)
    {
    case SH_NORTH:
    case SH_SOUTH: generateAndAddPiece(env, p->x1 + 1, y, p->z0 + zoff, SH_EAST, p->depth); break;
    case SH_WEST:
    case SH_EAST:  generateAndAddPiece(env, p->x0 + zoff, y, p->z1 + 1, SH_SOUTH, p->depth); break;
    }
}

/* vanilla *.addChildren(). The piece is copied, because the list may not
 * move, but the children are written behind it. */
static void addChildren(ShEnv *env, int idx)
{
    StrongholdPiece p = env->list[idx];
    switch (p.type)
    {
    case SH_START:
        env->imposed = SH_FIVE_CROSSING;
        childForward(env, &p, 1, 1);
        break;
    case SH_STRAIGHT:
        childForward(env, &p, 1, 1);
        if (p.data & SH_STRAIGHT_LEFT)
            childLeft(env, &p, 1, 2);
        if (p.data & SH_STRAIGHT_RIGHT)
            childRight(env, &p, 1, 2);
        break;
    case SH_PRISON_HALL:
    case SH_STRAIGHT_STAIRS_DOWN:
    case SH_STAIRS_DOWN:
    case SH_CHEST_CORRIDOR:
        childForward(env, &p, 1, 1);
        break;
    case SH_LEFT_TURN:
        if (p.facing == SH_NORTH || p.facing == SH_EAST)
            childLeft(env, &p, 1, 1);
        else
            childRight(env, &p, 1, 1);
        break;
    case SH_RIGHT_TURN:
        if (p.facing == SH_NORTH || p.facing == SH_EAST)
            childRight(env, &p, 1, 1);
        else
            childLeft(env, &p, 1, 1);
        break;
    case SH_ROOM_CROSSING:
        childForward(env, &p, 4, 1);
        childLeft(env, &p, 1, 4);
        childRight(env, &p, 1, 4);
        break;
    case SH_FIVE_CROSSING: {
        int za = 3, zb = 5;
        if (p.facing == SH_WEST || p.facing == SH_NORTH)
        {
            za = 8 - za;
            zb = 8 - zb;
        }
        // vanilla passes these as (yOff, zOff) to the side doors
        childForward(env, &p, 5, 1);
        if (p.data & SH_5C_LEFT_LOW)   childLeft(env, &p, za, 1);
        if (p.data & SH_5C_LEFT_HIGH)  childLeft(env, &p, zb, 7);
        if (p.data & SH_5C_RIGHT_LOW)  childRight(env, &p, za, 1);
        if (p.data & SH_5C_RIGHT_HIGH) childRight(env, &p, zb, 7);
        break; }
    case SH_PORTAL_ROOM:
        env->portal = idx;
        break;
    default: // library, filler corridor: no children
        break;
    }
}

static int generateLayout(Stronghold *sh, int mc, uint64_t seed,
    int chunkX, int chunkZ, int untilPortal)
{
    ShEnv env;
    int tries = 0;

    if (mc < MC_1_18 || mc > MC_NEWEST)
        return 0;

    sh->seed = seed;
    sh->mc = mc;
    sh->chunkX = chunkX;
    sh->chunkZ = chunkZ;
    sh->n = 0;
    sh->portal = -1;
    env.list = sh->pieces;

    do
    {
        // WorldgenRandom.setLargeFeatureSeed(seed + tries, chunkX, chunkZ)
        uint64_t s = seed + (uint64_t) tries;
        setSeed(&env.rng, s);
        uint64_t la = nextLong(&env.rng);
        uint64_t lb = nextLong(&env.rng);
        setSeed(&env.rng, ((uint64_t)(int64_t)chunkX * la) ^ ((uint64_t)(int64_t)chunkZ * lb) ^ s);
        tries++;

        env.n = 0;
        env.overflow = 0;
        env.portal = -1;
        env.untilPortal = untilPortal;
        env.stop = 0;
        env.imposed = -1;
        env.prevType = -1;
        env.nw = SH_WEIGHTS;
        for (int i = 0; i < SH_WEIGHTS; i++)
        {
            env.widx[i] = (uint8_t) i;
            env.wcount[i] = 0;
        }
        env.npend = 0;
        env.nnodes = 0;
        memset(env.head, 0xff, sizeof(env.head));

        // StartPiece: random facing, 5x11x5 box at y=64
        int facing = nextInt(&env.rng, 4);
        int x = chunkX * 16 + 2, z = chunkZ * 16 + 2;
        Box b = { x, 64, z, x + 4, 64 + 10, z + 4 };
        StrongholdPiece *start = newPiece(&env, SH_START, &b, facing, 0);
        env.startMinX = start->x0;
        env.startMinZ = start->z0;
        env.gridX0 = start->x0 - 128;
        env.gridZ0 = start->z0 - 128;
        gridAdd(&env, 0);
        env.n = 1;

        addChildren(&env, 0);
        while (env.npend > 0 && !env.overflow && !env.stop)
        {
            int j = nextInt(&env.rng, env.npend);
            int idx = env.pend[j];
            memmove(env.pend + j, env.pend + j + 1,
                (env.npend - j - 1) * sizeof(env.pend[0]));
            env.npend--;
            addChildren(&env, idx);
        }
        if (env.overflow)
            return 0;
        if (env.stop)
        {   // The portal room is the last piece kept. The final vertical
            // offset is unknown yet; shift the pieces so that they start at
            // y = -62, the lowest start the final offset can give. Clipping
            // at the writable chunk height (getStrongholdPieceRngCalls) then
            // affects these pieces no more than the final layout.
            env.n = env.portal + 1;
            int ymin = env.list[0].y0;
            for (int i = 1; i < env.n; i++)
                if (env.list[i].y0 < ymin) ymin = env.list[i].y0;
            for (int i = 0; i < env.n; i++)
            {
                env.list[i].y0 += -62 - ymin;
                env.list[i].y1 += -62 - ymin;
            }
            break;
        }

        // StructurePiecesBuilder.moveBelowSeaLevel(63, -64, random, 10)
        int y0 = env.list[0].y0, y1 = env.list[0].y1;
        for (int i = 1; i < env.n; i++)
        {
            if (env.list[i].y0 < y0) y0 = env.list[i].y0;
            if (env.list[i].y1 > y1) y1 = env.list[i].y1;
        }
        const int seaLevel = 63, minY = -64, offset = 10;
        int maxY = seaLevel - offset;
        int top = (y1 - y0 + 1) + minY + 1;
        if (top < maxY)
            top += nextInt(&env.rng, maxY - top);
        int dy = top - y1;
        for (int i = 0; i < env.n; i++)
        {
            env.list[i].y0 += dy;
            env.list[i].y1 += dy;
        }
    }
    while (env.portal < 0);

    sh->tries = tries - 1;
    sh->n = env.n;
    sh->portal = env.portal;
    return env.n;
}

int getStrongholdLayout(Stronghold *sh, int mc, uint64_t seed, int chunkX, int chunkZ)
{
    return generateLayout(sh, mc, seed, chunkX, chunkZ, 0);
}

int getStrongholdLayoutToPortal(Stronghold *sh, int mc, uint64_t seed,
    int chunkX, int chunkZ)
{
    return generateLayout(sh, mc, seed, chunkX, chunkZ, 1);
}


//==============================================================================
// Decoration RNG
//==============================================================================

int getStrongholdFeatureParams(int mc, int *structIndex, int *step)
{
    // GenerationStep.Decoration ordinals
    enum { SURFACE_STRUCTURES = 4, STRONGHOLDS = 5 };
    int idx, stp;

    if (mc < MC_1_18 || mc > MC_NEWEST)
        return 0;
    if (mc <= MC_1_18_2)
    {   // code registered structure features; strongholds have their own step
        stp = STRONGHOLDS;
        idx = 0;
    }
    else
    {   // "step": "surface_structures"; the index counts all structures of
        // that step that precede the stronghold in the structure registry
        stp = SURFACE_STRUCTURES;
        if (mc <= MC_1_19_2)
            idx = 8;    // builtin registration order
        else if (mc <= MC_26_2)
            idx = 19;   // data pack registry, sorted by id
        else
            idx = 37;   // 26.3 adds 18 abandoned_camp_* structures before it
    }
    if (structIndex) *structIndex = idx;
    if (step) *step = stp;
    return 1;
}

static inline uint64_t decoSeedFromAB(uint64_t seed, uint64_t a, uint64_t b,
    int chunkX, int chunkZ)
{
    int64_t x = (int64_t) chunkX * 16, z = (int64_t) chunkZ * 16;
    return ((uint64_t) x * a + (uint64_t) z * b) ^ seed;
}

static inline void decoAB(uint64_t seed, uint64_t *a, uint64_t *b)
{
    Xoroshiro xr;
    xSetSeed(&xr, seed);
    *a = xNextLongJ(&xr) | 1;
    *b = xNextLongJ(&xr) | 1;
}

uint64_t getStrongholdDecoSeed(int mc, uint64_t seed, int chunkX, int chunkZ)
{
    int idx = 0, step = 0;
    uint64_t a, b;
    getStrongholdFeatureParams(mc, &idx, &step);
    decoAB(seed, &a, &b);
    return decoSeedFromAB(seed, a, b, chunkX, chunkZ) + (uint64_t)(idx + 10000 * step);
}


//==============================================================================
// Portal room
//==============================================================================

/* vanilla StructurePiece.getWorldX/Y/Z() */
static inline Pos3 worldPos(const StrongholdPiece *p, int x, int y, int z)
{
    Pos3 w;
    w.y = p->y0 + y;
    switch (p->facing)
    {
    case SH_NORTH: w.x = p->x0 + x; w.z = p->z1 - z; break;
    case SH_SOUTH: w.x = p->x0 + x; w.z = p->z0 + z; break;
    case SH_WEST:  w.x = p->x1 - z; w.z = p->z0 + x; break;
    default:       w.x = p->x0 + z; w.z = p->z0 + x; break; // east
    }
    return w;
}

/* Frame positions in portal room coordinates, in eye draw order. */
static const int8_t portal_frames[12][2] = {
    {4, 8}, {5, 8}, {6, 8},     // facing north
    {4,12}, {5,12}, {6,12},     // facing south
    {3, 9}, {3,10}, {3,11},     // facing east
    {7, 9}, {7,10}, {7,11},     // facing west
};

static inline int floorDiv16(int v) { return v >> 4; }

static inline int64_t span(int a0, int a1, int b0, int b1)
{
    int lo = a0 > b0 ? a0 : b0;
    int hi = a1 < b1 ? a1 : b1;
    return hi >= lo ? (int64_t)(hi - lo + 1) : 0;
}

/* Number of cells on the surface of the piece box that lie in the chunk
 * column. These are the cells where the skip-air wall box of a piece draws
 * one nextFloat(), provided the existing block there is not air. */
static int64_t edgeCellsInChunk(const StrongholdPiece *p, int cx, int cz)
{
    const int minY = -63, maxY = 319; // writable area of the chunk
    int bx0 = cx * 16, bz0 = cz * 16;
    int64_t all = span(p->x0, p->x1, bx0, bx0 + 15) *
        span(p->z0, p->z1, bz0, bz0 + 15) * span(p->y0, p->y1, minY, maxY);
    int64_t in = 0;
    if (p->x1 - p->x0 >= 2 && p->y1 - p->y0 >= 2 && p->z1 - p->z0 >= 2)
    {
        in = span(p->x0 + 1, p->x1 - 1, bx0, bx0 + 15) *
            span(p->z0 + 1, p->z1 - 1, bz0, bz0 + 15) *
            span(p->y0 + 1, p->y1 - 1, minY, maxY);
    }
    return all - in;
}

static inline int inChunk(Pos3 w, int cx, int cz)
{
    return floorDiv16(w.x) == cx && floorDiv16(w.z) == cz;
}

int getStrongholdPieceRngCalls(const StrongholdPiece *p, int chunkX, int chunkZ,
    int *wallCells)
{
    int n = 0, cells = 0;
    int cx = chunkX, cz = chunkZ;
    switch (p->type)
    {
    case SH_FILLER_CORRIDOR:
        break;
    case SH_PORTAL_ROOM:
        // fixed walls/floors, then the 12 eyes; the spawner (after the eyes,
        // in one chunk only) is not counted
        n = PORTAL_ROOM_CALLS_BEFORE_EYES + 12;
        break;
    default:
        // generateBox(0,0,0, w-1,h-1,d-1, skipAir=true, SMOOTH_STONE_SELECTOR)
        cells = (int) edgeCellsInChunk(p, cx, cz);
        break;
    }

    switch (p->type)
    {
    case SH_STRAIGHT:
        n += 4; // four maybeGenerateBlock() torches
        break;
    case SH_PRISON_HALL:
        n += 12; // four 1x3x1 cell walls with skipAir=false
        break;
    case SH_FIVE_CROSSING:
        n += 109; // six fixed selector boxes with skipAir=false
        break;
    case SH_LIBRARY:
        n += 520; // cobweb generateMaybeBox() over 10x4x13 cells
        if (inChunk(worldPos(p, 3, 3, 5), cx, cz))
            n += 2; // chest loot seed (nextLong)
        if ((p->data & SH_LIBRARY_TALL) && inChunk(worldPos(p, 12, 8, 1), cx, cz))
            n += 2;
        break;
    case SH_CHEST_CORRIDOR:
        if (inChunk(worldPos(p, 3, 2, 3), cx, cz))
            n += 2;
        break;
    case SH_ROOM_CROSSING:
        if (p->data == 2 && inChunk(worldPos(p, 3, 4, 8), cx, cz))
            n += 2;
        break;
    default:
        break;
    }
    if (wallCells)
        *wallCells = cells;
    return n;
}

static inline int pieceInChunk(const StrongholdPiece *p, int cx, int cz)
{
    const int minY = -63, maxY = 319;
    return p->x1 >= cx * 16 && p->x0 <= cx * 16 + 15 &&
           p->z1 >= cz * 16 && p->z0 <= cz * 16 + 15 &&
           p->y1 >= minY && p->y0 <= maxY;
}

int getPortalPlan(PortalPlan *pp, const Stronghold *sh)
{
    if (sh->portal < 0 || sh->portal >= sh->n)
        return 0;
    if (!getStrongholdFeatureParams(sh->mc, &pp->structIndex, &pp->step))
        return 0;

    const StrongholdPiece *pr = &sh->pieces[sh->portal];
    pp->mc = sh->mc;
    pp->facing = pr->facing;
    pp->center = worldPos(pr, 5, 3, 10);
    pp->exact = 0;
    pp->nchunks = 0;

    for (int i = 0; i < 12; i++)
    {
        Pos3 w = worldPos(pr, portal_frames[i][0], 3, portal_frames[i][1]);
        pp->frame[i] = w;
        int cx = floorDiv16(w.x), cz = floorDiv16(w.z), j;
        for (j = 0; j < pp->nchunks; j++)
            if (pp->chunks[j].chunkX == cx && pp->chunks[j].chunkZ == cz)
                break;
        if (j == pp->nchunks)
        {
            PortalChunk *pc = &pp->chunks[pp->nchunks++];
            memset(pc, 0, sizeof(*pc));
            pc->chunkX = cx;
            pc->chunkZ = cz;
        }
        pp->chunks[j].frames |= (uint16_t)(1u << i);
    }
    // the 3x3 opening, local x 4..6, z 9..11; every chunk that holds an
    // opening block also holds frames (the opening lies inside the frame ring)
    for (int dz = 0; dz < 3; dz++)
    {
        for (int dx = 0; dx < 3; dx++)
        {
            Pos3 w = worldPos(pr, 4 + dx, 3, 9 + dz);
            for (int j = 0; j < pp->nchunks; j++)
            {
                if (inChunk(w, pp->chunks[j].chunkX, pp->chunks[j].chunkZ))
                    pp->chunks[j].opening |= (uint16_t)(1u << (3 * dz + dx));
            }
        }
    }

    for (int j = 0; j < pp->nchunks; j++)
    {
        PortalChunk *pc = &pp->chunks[j];
        int fixed = 0, cells = 0, npre = 0;
        for (int i = 0; i < sh->portal; i++)
        {
            const StrongholdPiece *p = &sh->pieces[i];
            if (!pieceInChunk(p, pc->chunkX, pc->chunkZ))
                continue;
            int c;
            fixed += getStrongholdPieceRngCalls(p, pc->chunkX, pc->chunkZ, &c);
            cells += c;
            npre++;
        }
        pc->npre = (uint8_t)(npre > 255 ? 255 : npre);
        pc->exact = (cells == 0);
        pc->wallCells = cells;
        pc->skip = fixed + cells + PORTAL_ROOM_CALLS_BEFORE_EYES;
        if (pc->exact)
            pp->exact |= pc->frames;
    }
    return 1;
}


//==============================================================================
// Eyes
//==============================================================================

/* Columns of the GF(2) matrix that advances Xoroshiro128++ by 760 outputs:
 * column j = image of the unit state with only bit j set (j < 64: lo word,
 * else hi word), as {lo, hi}. The state transition of Xoroshiro128++ is
 * linear over GF(2) (only the output uses an addition), so the columns were
 * generated by stepping each unit state 760 times. */
static const uint64_t xjump760[128][2] = {
    { 0x9729b1f34956cf87ULL, 0xbea1cb0e235828b3ULL },
    { 0x69438152e6e4137bULL, 0x30d9b3a637b8cc30ULL },
    { 0x54d7aa11544d7f0dULL, 0xef9ba2a91df9826aULL },
    { 0x184dd2690f183868ULL, 0x6b77a2a23f77b567ULL },
    { 0x66cb946dcd298822ULL, 0x8ee1a0478680083aULL },
    { 0x715a6f94782b3af2ULL, 0x7396c0e3bb0fca9aULL },
    { 0xa57b30c71d13dd35ULL, 0x9386458f6709c14aULL },
    { 0x860757166f353f18ULL, 0x77b6ca6dbf1381b0ULL },
    { 0x2eb84f434773c0f0ULL, 0xe98f1e3a0e68ee6bULL },
    { 0x6ec4a811d3f5e13aULL, 0x48a8feb481412058ULL },
    { 0x310cb9f1bb763692ULL, 0x2acb074fd1528435ULL },
    { 0x54445aa20b269e02ULL, 0x07cfecc893741e1eULL },
    { 0xe9066c93af9dfdb0ULL, 0x175f9549f77a39adULL },
    { 0x62c2b5e8e0d69c9bULL, 0x4426c48954682e38ULL },
    { 0x5e5ef7e00ed35ea9ULL, 0x06eeb208c90ce28cULL },
    { 0xb1b3fcfb522ae01bULL, 0xb20ad12841018041ULL },
    { 0xfc7cf254b6da0494ULL, 0xf94466f60cc9cfd9ULL },
    { 0xe6ee807dd4f693c4ULL, 0x7420dd36650b4e03ULL },
    { 0x7ae8e208f7190436ULL, 0xa4d5e84842496c56ULL },
    { 0xd1ea97de107b32c9ULL, 0x0a1651d2513089caULL },
    { 0x5f5291a6283d5e3bULL, 0xfa7bfb681850a9a3ULL },
    { 0xe075e04ebb0cd1a4ULL, 0xef063377f32c1b27ULL },
    { 0x4a94290c864c26abULL, 0x8ee415201f9ac0d7ULL },
    { 0x5e5ddf9a3ab76eccULL, 0x59d29bb6ecd74a35ULL },
    { 0x80b8f5bd4f4296e2ULL, 0x70d37d8062ede900ULL },
    { 0x14babf1d5d7b1bf1ULL, 0xc325d770d103a272ULL },
    { 0xfe7068c56f7362b3ULL, 0x1386ef197be907e6ULL },
    { 0xd22c04459a7949c0ULL, 0x378500ecc90a9cf1ULL },
    { 0x45f33f678cfa46dfULL, 0xfe49609deaa81ae4ULL },
    { 0x0cbf3a8b7a964ff5ULL, 0xc8df216f835c4cdcULL },
    { 0xb0c0a793d68846d9ULL, 0xadf96f98af5a1ec9ULL },
    { 0x148a61faa04a5b72ULL, 0x814f25287c709c9bULL },
    { 0xa500b82a5dd0442fULL, 0xe0165b6fcb8e09f0ULL },
    { 0x4ab3bd9ea43a43fcULL, 0xa559a881ba91e661ULL },
    { 0x10a26e2069b47539ULL, 0x828686ae4bf3b1e7ULL },
    { 0x6617a2470d9126f2ULL, 0x354a4970c3798857ULL },
    { 0xc8cb1f7ff71d7799ULL, 0x627399b8aeff223bULL },
    { 0xf5da697d4650b5b4ULL, 0xf5dc48949583aab8ULL },
    { 0xaed50dfa8e1f2c0aULL, 0x20b68ce2799464a4ULL },
    { 0x00621d3359224c5dULL, 0x6ce16fa557508398ULL },
    { 0xdd0d3cab187ad8cdULL, 0xf5437e0d460fb080ULL },
    { 0x0a7dff3ceca8340aULL, 0x5a6784625179eb67ULL },
    { 0x66c0ea498ae30744ULL, 0x5bada798d37bf1daULL },
    { 0x9649d0792a7c0f4dULL, 0xf49f3202af8b828dULL },
    { 0xdc21a06b0df2c4d7ULL, 0x06a6f88edfd5f696ULL },
    { 0x5c36949458eb5ff4ULL, 0xa958de14f8755f8eULL },
    { 0x67840ae2d8980411ULL, 0xf41b985d37852acaULL },
    { 0x1002322c32d607c3ULL, 0x0889eabc540cb5fcULL },
    { 0xd08621bb396c6752ULL, 0x2df8ffea481d069aULL },
    { 0xd677a3a5fbcf0701ULL, 0x323e4d1e502fcc2eULL },
    { 0x8fbc61e11fb583e8ULL, 0x2a1b5a014f1e28d6ULL },
    { 0x1eb45493f04b9382ULL, 0x79920c49fc2295feULL },
    { 0xd5706cc1e0b7274eULL, 0xa6f2980b77654919ULL },
    { 0xbaae62ca0da195eeULL, 0x49242973f0d38bc5ULL },
    { 0x9a1086f162376218ULL, 0x5bb41874775c1cf5ULL },
    { 0x5182ac806d5cecefULL, 0xa1c27475a00c0d56ULL },
    { 0xfb2ef04531097121ULL, 0xfa8abd84ca3bf283ULL },
    { 0xac19179c3d689655ULL, 0xca58f43d9cb60c51ULL },
    { 0xed43af5dd7af7accULL, 0x4a258cc0cdea0d50ULL },
    { 0x468128efb2d5f96cULL, 0xcc1823d552f2c042ULL },
    { 0xb25a4a5386305762ULL, 0xedc3c15e60d7a055ULL },
    { 0x29041dcb27c38d24ULL, 0x9b3edfebd022f75fULL },
    { 0xc64435382757e8c5ULL, 0xb4a161f08d70845aULL },
    { 0x66d8209886988ee9ULL, 0x1476f5e55753c1e6ULL },
    { 0x19c7c3b6a2248bf0ULL, 0x32b37ba8ebcebd21ULL },
    { 0x04585f12f71a7a7eULL, 0xac20e08487f30521ULL },
    { 0x4361d754ffc3ef3aULL, 0x3a04800b2f57d6c2ULL },
    { 0x94781ed65ce4f05cULL, 0x9116ff8703a2fde4ULL },
    { 0x2dada86c4c4116a0ULL, 0xc8bf201295aa0187ULL },
    { 0x6190775f0a84b841ULL, 0x169d177d72e1cf6eULL },
    { 0x5bfa7ddac8c92335ULL, 0x6dc7ddf8aef47810ULL },
    { 0xe0879f073b8390aaULL, 0xb2892a43430ef64eULL },
    { 0x9f0c3ea0adb5a286ULL, 0x935baac54121cfc4ULL },
    { 0xf664d22a0895a881ULL, 0x0ea8518bf879a5efULL },
    { 0xbf4ed50a596ac088ULL, 0x8ba56f98f2cc4f7aULL },
    { 0xb003a9c42a64947fULL, 0xeef21af06e0a0185ULL },
    { 0x63c0cd54965137beULL, 0x9fb641777d5b0d1eULL },
    { 0xfbeaf3c81b304e89ULL, 0x0a06c702e598e322ULL },
    { 0x37d7f24be68d314fULL, 0x1a5ba9c2663dd33fULL },
    { 0x8d1ca37e6d9802f0ULL, 0xbb27059694afd248ULL },
    { 0x84e7a1db22563dc3ULL, 0xfa53f67a95deccedULL },
    { 0xbb13ff967e38bc8cULL, 0xf6458473367f4bdaULL },
    { 0x4f459c09fe3c94c6ULL, 0x6603c03ffc16d685ULL },
    { 0xbba841c7d6560f22ULL, 0x085404637907b059ULL },
    { 0xaa8bd2b5b259a49bULL, 0xbf5e11dcbb585d38ULL },
    { 0xf4077d9579ed71c4ULL, 0x8be8ba0509a9e3b1ULL },
    { 0x5a56ce904aadad43ULL, 0x6f1ee9bba817ede4ULL },
    { 0x85bd0a5b2b1f613eULL, 0x64bca8c9658ea667ULL },
    { 0x7e8ced804a7bf3c1ULL, 0x9af56039ed5b676bULL },
    { 0xa2ec7b2e7c4cbcc9ULL, 0x605b50115c8471b7ULL },
    { 0xec3d9bb94636cd36ULL, 0x368afd852e5264eaULL },
    { 0x76982087ceeeab85ULL, 0xe492beffe8642a2bULL },
    { 0x4801e76ae6fd6cbbULL, 0xb5395b14c2c37815ULL },
    { 0x8d9d02574584a4bbULL, 0xd9333173d2a5871eULL },
    { 0x4661b6902fc7d7beULL, 0x4b407a8076b89d2aULL },
    { 0x570906b495b01afbULL, 0x0e50bff9ddccb272ULL },
    { 0xb5f8c537fd811c76ULL, 0xa05ac64529d282cfULL },
    { 0x8ceaa8fd64633a67ULL, 0x4796788837961b7eULL },
    { 0x034ce4ff7e425e42ULL, 0x826090d1fd6087b9ULL },
    { 0x18200d9679cc6824ULL, 0xce777c9e109b7000ULL },
    { 0xe7e04d380470b92cULL, 0x7f997981adf1dbf1ULL },
    { 0x03f57d6bfc05bba6ULL, 0xfa5f51fac197f6abULL },
    { 0xc22968d5e4ce50c7ULL, 0x813e4a988e6ca307ULL },
    { 0xe1c2bc69fe501b1eULL, 0xa39f5ef8fd150cddULL },
    { 0xb4c2fbe82e04a84eULL, 0x8cb69e62422cb70eULL },
    { 0x87c0cdc2253a7269ULL, 0x694ac8fda0855c93ULL },
    { 0x066b779d29655731ULL, 0xabaff55f08adaf63ULL },
    { 0x97bd79af0dfc9c0aULL, 0x5fea359b6c9d1814ULL },
    { 0xf8a33459217404a2ULL, 0x17e0a1707ea1201bULL },
    { 0x00dd88bb0742b4e9ULL, 0x00122ea1784a4d19ULL },
    { 0xfc8078cedc62f651ULL, 0xa81623c9278d8a0dULL },
    { 0x15cb73b6cbbff9d3ULL, 0xb5a03428ac00fd10ULL },
    { 0x64ecd8db5c9ff721ULL, 0x308c609078741ef2ULL },
    { 0xe8dd2d0bf421b74fULL, 0xb3afddc877a91684ULL },
    { 0x566f3dbbc74afc1aULL, 0xc4508328c345c028ULL },
    { 0xf349042186423b7cULL, 0x7b9d7f4dad12a68aULL },
    { 0x6841f59bfec9c94bULL, 0x43f9aade34df2e55ULL },
    { 0xaffec201fcb72a07ULL, 0xc532c3fa0aadfa0bULL },
    { 0x6925e83b393340c1ULL, 0xdd894f38e24ec479ULL },
    { 0x08ae87e6f15032e1ULL, 0x9815571c6837338cULL },
    { 0x380e377d081e988aULL, 0x2e34ccc955545d0dULL },
    { 0xfa489750820c64b1ULL, 0xabb6695499088738ULL },
    { 0x8e229c3a401cc040ULL, 0xd1fc977813aa97f6ULL },
    { 0x84fc15f114c8ffd5ULL, 0xf33a77ab9e804e16ULL },
    { 0x5cbb3810d96db47aULL, 0x84774bf7aa07a202ULL },
    { 0x61ba8c6be722c83bULL, 0xabd2e2e4517a3793ULL },
    { 0xf958d44c7154bca9ULL, 0xcb76df4453ed7bf3ULL },
    { 0xba2b39a94a63bc6fULL, 0x4955857644270dc4ULL },
};

void xSkipPortalRoom(Xoroshiro *xr)
{
    uint64_t lo = 0, hi = 0;
    uint64_t s = xr->lo;
    for (int j = 0; j < 64; j++)
    {
        uint64_t m = -(s >> j & 1);
        lo ^= xjump760[j][0] & m;
        hi ^= xjump760[j][1] & m;
    }
    s = xr->hi;
    for (int j = 0; j < 64; j++)
    {
        uint64_t m = -(s >> j & 1);
        lo ^= xjump760[64 + j][0] & m;
        hi ^= xjump760[64 + j][1] & m;
    }
    xr->lo = lo;
    xr->hi = hi;
}

void initPortalJumpTable(PortalJumpTable *t)
{
    // t->v[k][n] = matrix applied to nibble n placed at bits 4k..4k+3
    for (int k = 0; k < 32; k++)
    {
        for (int n = 0; n < 16; n++)
        {
            uint64_t lo = 0, hi = 0;
            for (int b = 0; b < 4; b++)
            {
                if (n >> b & 1)
                {
                    lo ^= xjump760[4 * k + b][0];
                    hi ^= xjump760[4 * k + b][1];
                }
            }
            t->v[k][n][0] = lo;
            t->v[k][n][1] = hi;
        }
    }
}

uint16_t evalPortalEyes(const PortalPlan *pp, uint64_t seed, int exactOnly)
{
    uint64_t a, b;
    uint16_t eyes = 0;
    uint64_t salt = (uint64_t)(pp->structIndex + 10000 * pp->step);

    decoAB(seed, &a, &b);
    for (int j = 0; j < pp->nchunks; j++)
    {
        const PortalChunk *pc = &pp->chunks[j];
        if (exactOnly && !pc->exact)
            continue;
        Xoroshiro xr;
        xSetSeed(&xr, decoSeedFromAB(seed, a, b, pc->chunkX, pc->chunkZ) + salt);
        int pre = pc->skip - PORTAL_ROOM_CALLS_BEFORE_EYES;
        xSkipN(&xr, pre);
        xSkipPortalRoom(&xr);
        eyes |= drawPortalEyes(&xr) & pc->frames;
    }
    return eyes;
}

static inline int popcount16(unsigned v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

static int portalFromPlan(EndPortal *ep, const PortalPlan *pp, uint64_t seed)
{
    uint64_t a, b;
    uint64_t salt = (uint64_t)(pp->structIndex + 10000 * pp->step);

    memcpy(ep->frame, pp->frame, sizeof(ep->frame));
    ep->center = pp->center;
    ep->facing = pp->facing;
    ep->exact = pp->exact;
    ep->eyes = 0;
    ep->portalBlocks = 0;
    decoAB(seed, &a, &b);
    for (int j = 0; j < pp->nchunks; j++)
    {
        const PortalChunk *pc = &pp->chunks[j];
        Xoroshiro xr;
        xSetSeed(&xr, decoSeedFromAB(seed, a, b, pc->chunkX, pc->chunkZ) + salt);
        xSkipN(&xr, pc->skip - PORTAL_ROOM_CALLS_BEFORE_EYES);
        xSkipPortalRoom(&xr);
        uint16_t draws = drawPortalEyes(&xr);
        ep->eyes |= draws & pc->frames;
        if (draws == 0xfff)
            ep->portalBlocks |= pc->opening;
    }
    ep->lit = (ep->portalBlocks == 0x1ff);
    ep->eyeCount = popcount16(ep->eyes);
    ep->exactEyeCount = popcount16(ep->eyes & ep->exact);
    return 1;
}

int getEndPortal(EndPortal *ep, Stronghold *sh, int mc, uint64_t seed,
    int chunkX, int chunkZ)
{
    Stronghold tmp;
    PortalPlan pp;
    if (!sh)
        sh = &tmp;
    if (!getStrongholdLayout(sh, mc, seed, chunkX, chunkZ))
        return 0;
    if (!getPortalPlan(&pp, sh))
        return 0;
    return portalFromPlan(ep, &pp, seed);
}

int getEndPortalOfStronghold(EndPortal *ep, const Generator *g, int index,
    Pos *startChunk)
{
    StrongholdIter it;
    // a world has 128 strongholds (indices 0 .. 127)
    if (g->mc < MC_1_18 || g->mc > MC_NEWEST || index < 0 || index >= 128)
        return 0;
    initFirstStronghold(&it, g->mc, g->seed);
    for (int i = 0; i <= index; i++)
    {
        if (nextStronghold(&it, g) <= 0 && i < index)
            return 0;
    }
    int cx = it.pos.x >> 4, cz = it.pos.z >> 4;
    if (startChunk)
    {
        startChunk->x = cx;
        startChunk->z = cz;
    }
    return getEndPortal(ep, NULL, g->mc, g->seed, cx, cz);
}
