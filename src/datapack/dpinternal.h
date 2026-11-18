#ifndef DPINTERNAL_H
#define DPINTERNAL_H

// Internals shared between the files of src/datapack and its tests. Not part
// of the public interface (dpworld.h).

#include "dpworld.h"

#include <QJsonObject>
#include <QRgb>
#include <QString>

namespace dp
{

// "somepack:alpine_grove" -> "Alpine Grove"
QString biomeDisplayName(const QString& id);

// The game version @mc as players name it ("1.18.2", "26.1"), for messages
QString versionName(int mc);

// Java's Math.floorDiv and Math.floorMod on int
inline int floorDiv(int a, int b)
{
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
inline int floorMod(int a, int b)
{
    const int m = a % b;
    return (m != 0 && ((m < 0) != (b < 0))) ? m + b : m;
}

// A stable colour for a biome without a colour of its own: derived from the
// id, and from the colours of the biome's own JSON (effects) if it has any.
QRgb fallbackBiomeColor(const QString& id, const QJsonObject& biome, int variant = 0);
// The distance of two colours as the eye sees it, roughly (redmean); 0 .. 765
double colorDistance(QRgb a, QRgb b);
// Biomes of one pack whose colours are this close take another variant
// (WorldPrivate::packBiomeColors)
const double MIN_BIOME_COLOR_DISTANCE = 45.0;
const int BIOME_COLOR_VARIANTS = 12;

// How the game's "last result" of the biome parameter search is treated.
// The game starts every search from the result of the search before it (per
// thread) and only replaces that by a strictly closer entry. Where entries
// overlap or tie, the biome of a cell therefore depends on the cell that was
// searched before it.
enum CursorPolicy
{
    // Like a chunk of the game: the cells are searched in the order in which
    // the game fills a chunk section (x, then y, then z), in runs of the four
    // z cells of a chunk. A run starts from the result of the cell the game
    // searches before it, which is looked up without history. The biome of a
    // cell then depends on its position alone: not on the requested area,
    // not on the scale (1:1 zooms these cells, larger scales show the cell
    // in the middle of each pixel with the biome it has at 1:4), not on
    // earlier calls.
    CURSOR_CHUNK,
    // One cursor through all samples of a genBiomes() call, reset at the
    // start of each call (the result depends on the requested area).
    CURSOR_CARRY,
    // Every sample is searched without history.
    CURSOR_FRESH,
};

// The biomes of the cells (qx[i], qy, qz[i]) at 1:4, searched in this order
// with one "last result" carried from each search to the next, starting
// without history: like the game's lookups in BiomeSource.
// findBiomeHorizontal() (the stronghold rings), which the game makes one
// after the other on one thread. Writes n indices into World::biomes() to
// @out. Returns false if the sampler is not valid.
bool sampleCellsCarry(Sampler& sampler, const int *qx, const int *qz, int n, int qy, int *out);

// Test hooks: process wide settings that samplers and worlds created
// afterwards use. The app does not call these.
void setCursorPolicy(CursorPolicy policy);
void setAllowUnverifiedVersions(bool allow);
// false: terrain heights scan every block (no bounds), for comparisons
void setTerrainPruning(bool prune);
bool terrainPruning();

} // namespace dp

#endif // DPINTERNAL_H
