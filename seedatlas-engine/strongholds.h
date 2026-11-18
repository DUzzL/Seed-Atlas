#ifndef STRONGHOLDS_H_
#define STRONGHOLDS_H_

/* Stronghold piece layouts and End portal eyes (Minecraft 1.18 .. 26.3).
 *
 * The rules implemented here, and how each of them was verified, are written
 * down in docs/portal-eyes.md. In short:
 *
 *  - The layout depends on the lower 48 bits of the world seed and on the
 *    structure start chunk only (Java LCG, large feature seed).
 *  - Each portal frame's eye is drawn from the decoration RNG of the chunk that
 *    contains the frame (Xoroshiro128++, full 64-bit world seed). The portal
 *    room draws all 12 eyes in every chunk it intersects, after a fixed number
 *    (PORTAL_ROOM_CALLS_BEFORE_EYES) of RNG calls of its own.
 *  - Stronghold pieces that come before the portal room in the piece list and
 *    intersect the same chunk advance that chunk's RNG first; most of them by
 *    an amount that depends on the terrain (air vs. non-air blocks). Frames in
 *    such chunks are reported as not exact.
 *
 * Everything here is reentrant: no globals, no static mutable state, no heap.
 */

#include "finders.h"

#ifdef __cplusplus
extern "C"
{
#endif

enum StrongholdPieceType
{
    SH_START,                   // "SHStart" (stairs down, generation origin)
    SH_STRAIGHT,                // "SHS"
    SH_PRISON_HALL,             // "SHPH"
    SH_LEFT_TURN,               // "SHLT"
    SH_RIGHT_TURN,              // "SHRT"
    SH_ROOM_CROSSING,           // "SHRC"
    SH_STRAIGHT_STAIRS_DOWN,    // "SHSSD"
    SH_STAIRS_DOWN,             // "SHSD"
    SH_FIVE_CROSSING,           // "SH5C"
    SH_CHEST_CORRIDOR,          // "SHCC"
    SH_LIBRARY,                 // "SHLi"
    SH_PORTAL_ROOM,             // "SHPR"
    SH_FILLER_CORRIDOR,         // "SHFC"
    SH_PIECE_TYPE_NUM
};

/* Horizontal facing of a piece, in the order of vanilla's
 * Direction.Plane.HORIZONTAL (NORTH, EAST, SOUTH, WEST). */
enum { SH_NORTH, SH_EAST, SH_SOUTH, SH_WEST };

/* Entry door of a piece (vanilla SmallDoorType). */
enum { SH_DOOR_OPENING, SH_DOOR_WOOD, SH_DOOR_GRATES, SH_DOOR_IRON };

/* Per-type flags in StrongholdPiece.data */
enum
{
    SH_STRAIGHT_LEFT        = 1,    // Straight: corridor branches left
    SH_STRAIGHT_RIGHT       = 2,    // Straight: corridor branches right
    SH_5C_LEFT_LOW          = 1,    // FiveCrossing exits
    SH_5C_LEFT_HIGH         = 2,
    SH_5C_RIGHT_LOW         = 4,
    SH_5C_RIGHT_HIGH        = 8,
    SH_LIBRARY_TALL         = 1,    // Library: two-storey variant
    // RoomCrossing: data holds the room variant 0..4 (2 has a chest)
};

STRUCT(StrongholdPiece)
{
    int x0, y0, z0;     // bounding box, inclusive block coordinates
    int x1, y1, z1;
    uint8_t type;       // StrongholdPieceType
    uint8_t facing;     // SH_NORTH .. SH_WEST
    uint8_t depth;      // generation depth
    uint8_t door;       // entry door
    uint8_t data;       // type specific flags (see above)
};

/* Upper bound for the number of pieces. Vanilla has no hard limit; over 10^6
 * random starts the mean was 153 and the largest layout had 665 pieces (see
 * docs/portal-eyes.md). Generation fails (returns 0) instead of overflowing. */
#define STRONGHOLD_MAX_PIECES 1024

STRUCT(Stronghold)
{
    uint64_t seed;      // world seed (the layout uses the lower 48 bits)
    int mc;
    int chunkX, chunkZ; // structure start chunk
    int tries;          // extra layout attempts (seed + tries) until a portal room existed
    int n;              // number of pieces
    int portal;         // index of the portal room in pieces[]
    StrongholdPiece pieces[STRONGHOLD_MAX_PIECES]; // vanilla piece order
};

/* Generate the stronghold layout for a structure start chunk, exactly as
 * vanilla does it, including the final vertical offset (moveBelowSeaLevel).
 * Versions MC_1_18 .. MC_NEWEST are supported (see PORTAL_EYES_VERIFIED).
 *
 * @sh      : output (about 32 KB; may live on the stack). The function
 *            itself uses about 40 KB of stack for its work state.
 * @mc      : Minecraft version
 * @seed    : world seed (only the lower 48 bits matter for the layout)
 * @chunkX, chunkZ : structure start chunk (the stronghold position >> 4)
 *
 * Returns the number of pieces, or 0 if the version is unsupported or the
 * piece buffer would overflow.
 */
int getStrongholdLayout(Stronghold *sh, int mc, uint64_t seed, int chunkX, int chunkZ);

/* Faster partial layout for searches: stops as soon as the portal room has
 * been placed. The result holds the pieces up to and including the portal
 * room (sh->portal == sh->n - 1), which are exactly the pieces that
 * getStrongholdLayout() puts before it, with the same x/z boxes. The vertical
 * offset (moveBelowSeaLevel) is not known yet; the pieces are shifted to
 * start at y = -62 instead, so y coordinates differ from the final layout.
 * getPortalPlan() on this result yields the same chunks, frame masks, skips
 * and exactness as on the full layout; only the y of frame/center differ. Returns the number of pieces, or 0 if the piece buffer would
 * overflow first. If the complete generation would overflow later on, this
 * function may still succeed (getStrongholdLayout() then returns 0). */
int getStrongholdLayoutToPortal(Stronghold *sh, int mc, uint64_t seed,
    int chunkX, int chunkZ);

/* Short vanilla piece id ("SHPR", ...) of a piece type. */
const char *getStrongholdPieceName(int type);


//==============================================================================
// End portal
//==============================================================================

/* RNG calls the portal room makes in each chunk before its 12 eye draws. */
/* The newest version whose rules were checked against vanilla (see
 * docs/portal-eyes.md). Newer versions up to MC_NEWEST are computed with the
 * rules of this one; their results are unverified, and user interfaces
 * should say so. */
#define PORTAL_EYES_VERIFIED MC_26_3

#define PORTAL_ROOM_CALLS_BEFORE_EYES 760

/* An eye is placed iff nextFloat() > 0.9F. nextFloat() is the top 24 bits of a
 * Xoroshiro128++ output times 2^-24 and 0.9F == 15099494 * 2^-24, so this is
 * equivalent to (xNextLong(xr) >> 40) > PORTAL_EYE_THRESHOLD. */
#define PORTAL_EYE_THRESHOLD 15099494

STRUCT(PortalChunk)
{
    int chunkX, chunkZ;
    uint16_t frames;    // frames (bit i = draw i) that lie in this chunk
    uint16_t opening;   // opening blocks in this chunk (bit 3*dz+dx, see EndPortal)
    uint8_t exact;      // eye states of these frames do not depend on terrain
    uint8_t npre;       // stronghold pieces before the portal room in this chunk
    int skip;           // RNG calls before the 12 eye draws: preceding pieces
                        // (assuming no air in their walls) + 760
    int wallCells;      // part of 'skip' that depends on terrain: one call
                        // per wall cell of a preceding piece unless it is air
};

/* Everything about a portal that depends only on the layout (i.e. on the
 * lower 48 seed bits and the start chunk). Evaluating the eyes for a full
 * 64-bit seed then only needs evalPortalEyes(). */
STRUCT(PortalPlan)
{
    int mc;
    int structIndex, step;  // setFeatureSeed() parameters for the stronghold
    int facing;             // portal room orientation (SH_NORTH ..)
    Pos3 frame[12];         // frame block positions, in eye draw order
    Pos3 center;            // middle block of the 3x3 portal opening
    uint16_t exact;         // bit i: frame i is exact
    int nchunks;            // chunks containing frames (1 to 4)
    PortalChunk chunks[4];
};

STRUCT(EndPortal)
{
    Pos3 frame[12];     // frame block positions, in eye draw order
    Pos3 center;        // middle block of the 3x3 portal opening
    int facing;         // portal room orientation
    uint16_t eyes;      // bit i: frame i has an eye (best effort if not exact)
    uint16_t exact;     // bit i: the state of frame i follows from the seed alone
    int eyeCount;       // number of eyes (all frames)
    int exactEyeCount;  // number of eyes among the exact frames
    /* End portal blocks that worldgen places in the 3x3 opening: bit 3*dz+dx
     * is the block at portal room coordinates x = 4+dx, z = 9+dz. Vanilla
     * places them per chunk, iff all 12 draws of that chunk are eyes, so a
     * portal whose frames span several chunks can show 12 eyes and still be
     * unlit (or partially lit). Same exactness as the frames of that chunk. */
    uint16_t portalBlocks;
    int lit;            // portalBlocks == 0x1ff
};

/* The decoration step and the index of the stronghold within that step, as
 * passed to WorldgenRandom.setFeatureSeed(). Returns 0 for unsupported
 * versions. */
int getStrongholdFeatureParams(int mc, int *structIndex, int *step);

/* The seed that vanilla gives to the Xoroshiro128++ decoration RNG when it
 * places stronghold pieces in chunk (chunkX, chunkZ):
 *   getPopulationSeed(mc, seed, chunkX*16, chunkZ*16) + structIndex + 10000*step
 * Initialise the RNG with xSetSeed(&xr, result). */
uint64_t getStrongholdDecoSeed(int mc, uint64_t seed, int chunkX, int chunkZ);

/* RNG calls that the postProcess() of piece 'p' makes in chunk (chunkX,
 * chunkZ), up to and including the portal room's eye draws. The return value
 * counts the calls that do not depend on terrain. *wallCells (nullable)
 * receives the number of wall cells of the piece in that chunk: each of them
 * draws one more call unless the block there is air when the piece is placed.
 * A frame is exact iff no piece before the portal room has wall cells in the
 * frame's chunk. */
int getStrongholdPieceRngCalls(const StrongholdPiece *p, int chunkX, int chunkZ,
    int *wallCells);

/* Derive the portal plan from a layout. Returns 0 on failure. */
int getPortalPlan(PortalPlan *pp, const Stronghold *sh);

/* Draw the 12 eyes from a decoration RNG that has already been advanced to
 * the first eye draw. Bit i of the result is set if draw i places an eye. */
static inline uint16_t drawPortalEyes(Xoroshiro *xr)
{
    uint16_t m = 0;
    for (int i = 0; i < 12; i++)
        m |= (uint16_t)((xNextLong(xr) >> 40) > PORTAL_EYE_THRESHOLD) << i;
    return m;
}

/* Advance a Xoroshiro128++ state by exactly PORTAL_ROOM_CALLS_BEFORE_EYES
 * outputs, using a precomputed GF(2) jump matrix (about 10x faster than
 * stepping). */
void xSkipPortalRoom(Xoroshiro *xr);

/* Faster variant of xSkipPortalRoom() with a 8 KB nibble table that the
 * caller owns (e.g. one per search thread, or a shared read-only one).
 * initPortalJumpTable() fills it once. */
STRUCT(PortalJumpTable)
{
    uint64_t v[32][16][2];
};
void initPortalJumpTable(PortalJumpTable *t);

static inline void xSkipPortalRoomT(const PortalJumpTable *t, Xoroshiro *xr)
{
    uint64_t lo = 0, hi = 0;
    uint64_t s = xr->lo;
    for (int k = 0; k < 16; k++, s >>= 4)
    {
        lo ^= t->v[k][s & 15][0];
        hi ^= t->v[k][s & 15][1];
    }
    s = xr->hi;
    for (int k = 16; k < 32; k++, s >>= 4)
    {
        lo ^= t->v[k][s & 15][0];
        hi ^= t->v[k][s & 15][1];
    }
    xr->lo = lo;
    xr->hi = hi;
}

/* Evaluate the eyes of a planned portal for a full 64-bit world seed.
 * Returns the eye mask (bit i = frame i). Frames that are not exact use the
 * no-air assumption of PortalChunk.skip. If 'exactOnly' is non-zero, chunks
 * without exact frames are skipped (their bits are 0). */
uint16_t evalPortalEyes(const PortalPlan *pp, uint64_t seed, int exactOnly);

/* Layout + plan + eyes for one stronghold start. Returns 0 on failure.
 * @sh may be NULL, then a temporary on the stack is used. */
int getEndPortal(EndPortal *ep, Stronghold *sh, int mc, uint64_t seed,
    int chunkX, int chunkZ);

/* The portal of the i-th stronghold (0-based, in StrongholdIter order) of the
 * seed the generator was set up with (overworld, MC_1_18 .. MC_NEWEST).
 * Optionally returns the start chunk. Returns 0 on failure. */
int getEndPortalOfStronghold(EndPortal *ep, const Generator *g, int index,
    Pos *startChunk);


#ifdef __cplusplus
}
#endif

#endif /* STRONGHOLDS_H_ */
