#ifndef DPCHECK_H
#define DPCHECK_H

// The pack check: a plain-text report about the loaded data packs, for
// owners who test packs that cannot be shared. Per pack: the worldgen files
// per registry, the dimensions with their generator and biome source types,
// the types the app does not support (with ids), the time to load it (with
// the vanilla data) and a short speed sample; then the same for all packs
// together. Only pack file names appear, no paths (see dpScrubPaths).

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

struct DpCheckInput
{
    int mc = 0;
    QString vanillaPath;
    QString preset;                 // "minecraft:normal", ...
    std::vector<QString> packs;     // the switched on packs, in load order
    QString dimension;              // the dimension the map shows
    uint64_t seed = 0;
};

// Builds the report in the calling thread (seconds per pack). @progress is
// called with a short text before each step. Returns an empty string if
// aborted.
QString dpCheckReport(const DpCheckInput& input, const std::atomic_bool *abort,
    const std::function<void(const QString&)>& progress = nullptr);

// Removes folders from a text meant to be shared: every form of the @paths
// (as given, cleaned, with native separators, canonical) becomes its file
// name, their folders and the home folder are dropped, and any other quoted
// absolute path is cut down to its last part.
QString dpScrubPaths(const QString& text, const QStringList& paths);

#endif // DPCHECK_H
