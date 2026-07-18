#ifndef FORTRESSLAYOUT_H
#define FORTRESSLAYOUT_H

#include "seedatlas-engine/finders.h"

#include <vector>

enum FortressLayoutType
{
    FORTRESS_LAYOUT_2X2,
    FORTRESS_LAYOUT_3X1,
};

enum FortressLineOrientation
{
    FORTRESS_LINE_NONE,
    FORTRESS_LINE_X,
    FORTRESS_LINE_Z,
};

struct FortressLayoutMatch
{
    FortressLayoutMatch()
        : type(FORTRESS_LAYOUT_2X2), orientation(FORTRESS_LINE_NONE),
          position(), pieceCount(0) {}
    FortressLayoutMatch(FortressLayoutType type,
            FortressLineOrientation orientation, Pos position, int pieceCount)
        : type(type), orientation(orientation), position(position),
          pieceCount(pieceCount) {}

    FortressLayoutType type;
    FortressLineOrientation orientation;
    Pos position;
    int pieceCount;
};

struct FortressLayoutResult
{
    std::vector<FortressLayoutMatch> matches;
    bool has2x2 = false;
    bool has3x1 = false;
    // This is deliberately kept separately from the sorted matches. It is the
    // first position yielded by the historical 2x2 scan and therefore keeps
    // saved relative conditions stable.
    Pos legacy2x2Position = {};
};

bool isFortressCrossingPiece(const Piece& piece);

// Detects the requested farm layouts. The returned matches are sorted by type,
// representative position and orientation. A straight run of more than three
// pieces is returned once as one maximal 3x1-compatible line.
FortressLayoutResult findFortressLayouts(const Piece *pieces, int count,
    bool find2x2, bool find3x1);

#endif // FORTRESSLAYOUT_H
