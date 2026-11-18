#ifndef DPWORLD_P_H
#define DPWORLD_P_H

// The private side of dp::World: what load() builds and samplers read.
// Internal to src/datapack and its tests.

#include "dpworld.h"
#include "dpclimate.h"
#include "dpdensity.h"
#include "dpinternal.h"
#include "dppack.h"
#include "dpstructures.h"
#include "dpsurface.h"
#include "dpterrain.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace dp
{

// What a sampler needs of one dimension
struct DimData
{
    enum Source { NONE, MULTI_NOISE, NATIVE_OVERWORLD, FIXED, CHECKERBOARD, THE_END };
    Source source = NONE;
    // the noise settings have legacy_random_source
    bool legacy = false;

    // multi_noise: the six climate functions (CLIMATE_* order) and the nodes
    // evaluating them needs
    int outputs[6] = { -1, -1, -1, -1, -1, -1 };
    DensityProgram program;
    // the same functions for the game's plain climate sampler (see
    // DensityGraph::compilePlain()), for the structures
    int plainOutputs[6] = { -1, -1, -1, -1, -1, -1 };
    DensityProgram plainProgram;
    // the parameter list of a pack
    ClimateTree tree;
    std::vector<int32_t> entryBiome;    // entry -> index into World::biomes()
    // the vanilla overworld list: the engine's tree of the version
    int32_t nativeBiome[256];           // engine biome id -> index into World::biomes()
    // The entries of that list without their order (see dpnative.cpp), to
    // tell which entries are nearest to a climate; empty if not available.
    ClimateTree nativeTree;
    std::vector<int32_t> nativeGroups;  // entry -> engine biome id, or negative
    std::vector<int32_t> nativeNode;    // entry -> node of the engine's tree
    std::vector<int32_t> nativeEntry;   // node of the engine's tree -> entry, or -1

    // the lowest and highest biome cell (y) of the dimension's chunks
    int minCellY = -16;
    int maxCellY = 79;

    // the generator's height range (noise settings), for the game's
    // WorldGenerationContext together with the dimension type's
    int noiseMinY = -64;
    int noiseHeight = 384;

    int32_t fixedBiome = -1;
    std::vector<int32_t> checker;
    int checkerShift = 0;

    // the_end: the biomes the_end, end_highlands, end_midlands,
    // small_end_islands, end_barrens; the router's erosion is outputs[3]
    int32_t endBiomes[5] = { -1, -1, -1, -1, -1 };

    // terrain (final_density)
    TerrainDef terrain;
    // the surface rules (the heights of HEIGHTS_SURFACE)
    SurfaceRuleDef surface;
};

class WorldPrivate
{
public:
    Request request;
    std::vector<PackInfo> packs;
    std::vector<BiomeInfo> biomes;
    std::vector<DimensionInfo> dims;
    std::vector<DimData> data;
    QStringList presets;
    Resources res;
    std::unique_ptr<DensityGraph> graph;
    StructureData structures;

    Report *report = nullptr;           // only while loading
    QHash<QString, int> biomeIndex;
    QHash<QString, QRgb> packColors;
    QHash<QString, QString> packNames;
    QHash<QString, QRgb> vanillaColors;
    QHash<int, QHash<QString, QRgb>> packBiomeColors; // pack -> its biomes' colours

    void load();
    void issue(Issue::Severity sev, int pack, const QString& id, const QString& text);
    int biome(const QString& id);
    QRgb fallbackColor(const QString& id);
    void loadColors();
    void loadDimension(const QString& id, const QJsonValue& json, int pack);
    bool loadBiomeSource(const QJsonObject& src, int pack, const QString& dimId,
        DimensionInfo& info, DimData& d, bool *needsClimate);
    bool parseJsonFile(const char *registry, const QString& id, int fromPack,
        const QString& fromId, QJsonObject *obj, int *pack);
};

bool nativeOverworldEntries(int mc, std::vector<ClimateEntry> *entries, std::vector<int32_t> *groups,
    std::vector<int32_t> *nodeOfEntry, std::vector<int32_t> *entryOfNode);
bool nativeOverworldList(int mc, std::vector<ClimateEntry> *entries, std::vector<int32_t> *ids);
// children per node of the game's climate search tree: 10 before 1.19.4, 6, 19 from 26.3
int climateArity(int mc);

// The terrain of a sampler as the game's getBaseHeight() and getBaseColumn()
// make it (the structures; see TerrainState::setBaseColumns())
void setBaseColumns(Sampler& sampler, bool base);
// TerrainState::block() of the sampler's terrain (0 without terrain)
int blockAt(Sampler& sampler, int x, int y, int z);
// Are the aquifers of the sampler's dimension modelled?
bool hasAquifers(Sampler& sampler);

// The density states of a sampler: of the climate and of the terrain
// (created here if the dimension has terrain), or null
void samplerStates(Sampler& sampler, DensityState **climate, DensityState **terrain);

} // namespace dp

#endif // DPWORLD_P_H
