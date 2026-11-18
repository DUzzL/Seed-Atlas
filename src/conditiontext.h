#ifndef CONDITIONTEXT_H
#define CONDITIONTEXT_H

#include "search.h"

#include <QString>
#include <vector>

// Plain-language texts for the condition types and conditions, as shown in
// the condition editor and the condition list. They are display texts only:
// the technical names in g_filterinfo (used by Condition::summary(), the
// command line output and the Locations export) stay unchanged.

enum
{
    TG_STRUCT,          // Overworld structures
    TG_STRUCT_NETHER,   // Nether structures
    TG_STRUCT_END,      // End structures
    TG_BIOMES,          // biomes
    TG_WORLD,           // spawn, slime chunks, surface height
    TG_QUAD,            // quad structures
    TG_EXPERT,          // helpers, scripts, raw climate and legacy layer types
    TG_MAX,
};

// The condition types in the order of the picker (without the legacy ids).
std::vector<int> filterTypeOrder();

// Display name of a condition type, e.g. "Biomes in an area (Overworld)".
QString filterDisplayName(int type);
// One plain sentence about what the type checks.
QString filterBrief(int type);
// The icon name of a condition type for getIcon(), empty for none. Most come
// from the filter table; the logic groups and Lua (all "helper" there), the
// biome patches and the climate types get glyphs of their own.
QString filterIconName(int type);
// The picker group of a type (TG_*); expert types are in TG_EXPERT.
int filterGroup(int type);
bool filterIsExpert(int type);
QString filterGroupName(int group);
// The Minecraft versions of a type in short, e.g. "1.18+", "up to 1.17" or
// "1.13 to 1.17"; empty when it exists in all versions.
QString filterVersionNote(int type);
bool filterAvailable(int type, int mc);
// Name of a climate parameter (NP_*).
QString climateParaName(int para);

// The "#3" tag that identifies a condition, so that others can be measured
// from it.
QString conditionTag(const Condition& c);

// One plain sentence that describes the condition, e.g. "At least 1 village
// within 500 blocks of the world origin". The other conditions, when given,
// name the condition that this one is measured from; mc selects the biome
// names.
QString conditionSentence(const Condition& c, int mc,
                          const std::vector<Condition> *all = nullptr);

#endif // CONDITIONTEXT_H
