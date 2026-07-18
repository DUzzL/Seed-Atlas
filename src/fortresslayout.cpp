#include "fortresslayout.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>

bool isFortressCrossingPiece(const Piece& piece)
{
    return piece.type == FORTRESS_START || piece.type == BRIDGE_CROSSING;
}

static Pos pieceCentre(const Piece& piece)
{
    return {
        int((int64_t(piece.bb0.x) + piece.bb1.x) / 2),
        int((int64_t(piece.bb0.z) + piece.bb1.z) / 2),
    };
}

static void appendLineMatches(const std::vector<Piece>& eligible,
    FortressLineOrientation orientation,
    std::vector<FortressLayoutMatch>& matches)
{
    // A line keeps both perpendicular bounding-box limits fixed. This rejects
    // turns and offset/L-shaped arrangements even when endpoints happen to
    // touch in one coordinate.
    using Key = std::tuple<int, int, int>;
    std::map<Key, std::vector<const Piece*>> lines;
    for (const Piece& piece : eligible)
    {
        const Key key = orientation == FORTRESS_LINE_X
            ? Key(piece.bb0.y, piece.bb0.z, piece.bb1.z)
            : Key(piece.bb0.y, piece.bb0.x, piece.bb1.x);
        lines[key].push_back(&piece);
    }

    for (auto& entry : lines)
    {
        std::vector<const Piece*>& line = entry.second;
        std::sort(line.begin(), line.end(), [orientation](const Piece *a, const Piece *b) {
            if (orientation == FORTRESS_LINE_X)
                return std::tie(a->bb0.x, a->bb1.x, a->bb0.z, a->bb1.z) <
                    std::tie(b->bb0.x, b->bb1.x, b->bb0.z, b->bb1.z);
            return std::tie(a->bb0.z, a->bb1.z, a->bb0.x, a->bb1.x) <
                std::tie(b->bb0.z, b->bb1.z, b->bb0.x, b->bb1.x);
        });

        size_t begin = 0;
        while (begin < line.size())
        {
            size_t end = begin + 1;
            while (end < line.size())
            {
                const Piece& previous = *line[end-1];
                const Piece& current = *line[end];
                const bool adjacent = orientation == FORTRESS_LINE_X
                    ? int64_t(previous.bb1.x) + 1 == current.bb0.x
                    : int64_t(previous.bb1.z) + 1 == current.bb0.z;
                if (!adjacent)
                    break;
                end++;
            }
            const size_t length = end - begin;
            if (length >= 3)
            {
                // For an even-sized run, choose the lower-coordinate one of
                // the two middle pieces. This is deterministic and retains a
                // representative that lies on an eligible crossing.
                const Piece& middle = *line[begin + (length-1) / 2];
                matches.push_back({FORTRESS_LAYOUT_3X1, orientation,
                    pieceCentre(middle), int(length)});
            }
            begin = end;
        }
    }
}

FortressLayoutResult findFortressLayouts(const Piece *pieces, int count,
    bool find2x2, bool find3x1)
{
    FortressLayoutResult result;
    std::vector<Piece> eligible;
    eligible.reserve(std::max(0, count));
    for (int i = 0; i < count; i++)
        if (isFortressCrossingPiece(pieces[i]))
            eligible.push_back(pieces[i]);

    if (find2x2 && eligible.size() >= 4)
    {
        // Preserve the original algorithm byte-for-byte in its comparisons and
        // iteration order. In particular, its representative is bb1.x/bb1.z
        // of the first qualifying anchor, not the geometric square centre.
        for (size_t i = 0; i < eligible.size(); i++)
        {
            int adjacent = 0;
            for (size_t j = 0; j < eligible.size(); j++)
            {
                if (eligible[i].bb0.y != eligible[j].bb0.y) continue;
                if (eligible[i].bb1.x != eligible[j].bb1.x &&
                        eligible[i].bb1.x+1 != eligible[j].bb0.x) continue;
                if (eligible[i].bb1.z != eligible[j].bb1.z &&
                        eligible[i].bb1.z+1 != eligible[j].bb0.z) continue;
                adjacent++;
            }
            if (adjacent >= 4)
            {
                const Pos position = {eligible[i].bb1.x, eligible[i].bb1.z};
                if (!result.has2x2)
                    result.legacy2x2Position = position;
                result.has2x2 = true;
                result.matches.push_back({FORTRESS_LAYOUT_2X2,
                    FORTRESS_LINE_NONE, position, adjacent});
            }
        }
    }

    if (find3x1 && eligible.size() >= 3)
    {
        appendLineMatches(eligible, FORTRESS_LINE_X, result.matches);
        appendLineMatches(eligible, FORTRESS_LINE_Z, result.matches);
    }

    std::sort(result.matches.begin(), result.matches.end(),
        [](const FortressLayoutMatch& a, const FortressLayoutMatch& b) {
            return std::tie(a.type, a.position.x, a.position.z, a.orientation,
                       a.pieceCount) <
                std::tie(b.type, b.position.x, b.position.z, b.orientation,
                       b.pieceCount);
        });
    result.matches.erase(std::unique(result.matches.begin(), result.matches.end(),
        [](const FortressLayoutMatch& a, const FortressLayoutMatch& b) {
            return a.type == b.type && a.orientation == b.orientation &&
                a.position.x == b.position.x && a.position.z == b.position.z &&
                a.pieceCount == b.pieceCount;
        }), result.matches.end());
    result.has3x1 = std::any_of(result.matches.begin(), result.matches.end(),
        [](const FortressLayoutMatch& match) {
            return match.type == FORTRESS_LAYOUT_3X1;
        });
    return result;
}
