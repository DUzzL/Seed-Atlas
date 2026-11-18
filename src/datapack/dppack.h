#ifndef DPPACK_H
#define DPPACK_H

// Reading data packs (folders, zip and jar files) and merging them into one
// set of resources, the way the game layers packs.
//
// Internal to src/datapack.

#include "dpworld.h"

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

namespace dp
{

// The data pack format number of an engine version (MC_1_19_2, ...): the
// range of the game versions the engine id stands for. Returns false for
// versions without one in the table.
bool packFormatRange(int mc, int *minFormat, int *maxFormat);

// A data pack format as the game compares it (PackFormat: major, then minor)
typedef int64_t PackFormat;
inline PackFormat packFormat(int64_t major, int64_t minor = 0)
{
    return (major << 32) | (minor & 0x7fffffff);
}

// What the game makes of a pack.mcmeta (Pack.readPackMetadata with the
// codecs of its "pack" and "overlays" sections) in a game version whose data
// pack format is @version. The rules are those real servers 1.19.4 to 26.3
// showed with probe packs (tools/worldgen-reference/data/mcmeta_*.txt):
// whether the game loads the pack at all, and which overlays it applies.
struct PackMeta
{
    bool loaded = false;        // the game loads the pack
    QString reason;             // why not, or why its overlays are ignored
    bool overlaysIgnored = false;   // the pack loads without any overlay
    bool rangeKnown = false;    // the pack section declares formats the game reads
    QString formatProblem;      // why not (1.21.9 and later; the pack loads all the same)
    PackFormat lo = 0, hi = 0;  // ... these
    int packFormat = 0;         // "pack_format" (or the major of the range)
    QString description;        // plain text
    QStringList overlays;       // applied overlay directories, lowest priority first
};
PackMeta readPackMeta(const QByteArray& text, PackFormat version);

// Parses JSON the way the game tolerates it: any top level value, and (if
// the strict parse fails) comments. Returns false with a message otherwise.
bool parseJson(const QByteArray& data, QJsonValue *value, QString *error);

// "plains" -> "minecraft:plains"; ids with a namespace stay as they are
QString normalizeId(const QString& id);

// Registries the generator reads, as paths below data/<namespace>/
extern const char *const REG_DENSITY_FUNCTION;  // worldgen/density_function
extern const char *const REG_NOISE;             // worldgen/noise
extern const char *const REG_NOISE_SETTINGS;    // worldgen/noise_settings
extern const char *const REG_WORLD_PRESET;      // worldgen/world_preset
extern const char *const REG_PARAMETER_LIST;    // worldgen/multi_noise_biome_source_parameter_list
extern const char *const REG_BIOME;             // worldgen/biome
extern const char *const REG_DIMENSION;         // dimension
extern const char *const REG_DIMENSION_TYPE;    // dimension_type
extern const char *const REG_BIOME_TAG;         // tags/worldgen/biome
extern const char *const REG_BIOME_COLORS;      // the file data/c/worldgen/biome_colors.json
extern const char *const REG_STRUCTURE;         // worldgen/structure
extern const char *const REG_STRUCTURE_SET;     // worldgen/structure_set
extern const char *const REG_TEMPLATE_POOL;     // worldgen/template_pool

struct Resource
{
    QByteArray data;
    int pack = -1;      // index into Resources::packs
};

// One pack's version of a tag file
struct TagPart
{
    int pack = -1;
    bool replace = false;
    QStringList values;     // ids and "#tag" references, in file order
    QStringList optional;   // those of the values that need not exist
};

// A file that is only read when it is needed (structure templates, .nbt)
struct FileRef
{
    int pack = -1;      // index into Resources::packs
    QString path;       // path inside the pack (with the overlay directory)
};

struct Resources
{
    // [0] is the vanilla baseline, then the enabled packs of the request
    std::vector<PackInfo> packs;
    // registry -> id -> file of the last pack that has it
    QHash<QString, QMap<QString, Resource>> files;
    // tag registry -> tag id -> parts in pack order
    QHash<QString, QMap<QString, std::vector<TagPart>>> tags;
    // biome colour files of all packs, in pack order
    std::vector<Resource> colorFiles;
    // structure templates: id -> file of the last pack that has it
    // (data/<namespace>/structure/<path>.nbt, "structures" before 1.21)
    QHash<QString, FileRef> templates;

    const Resource *find(const char *registry, const QString& id) const;
    QStringList ids(const char *registry) const;
    // The ids of a tag after merging the packs ("replace" drops what earlier
    // packs listed) and resolving nested tags, in order, without duplicates.
    // Tags nested too deeply set @error (and are left out).
    QStringList tagValues(const char *registry, const QString& id, QString *error = nullptr) const;
    QString packName(int pack) const;
};

// Loads the vanilla baseline and the enabled packs of @request, in order.
// Problems are added to @report.
void loadResources(const Request& request, Resources *res, Report *report);

// Reads files of the packs after loadResources(), e.g. the structure
// templates. Keeps each pack open that it has read from until it is
// destroyed.
class PackFiles
{
public:
    explicit PackFiles(const Resources& res);
    ~PackFiles();
    bool read(const FileRef& ref, QByteArray *data, QString *error);

private:
    PackFiles(const PackFiles&) = delete;
    PackFiles& operator=(const PackFiles&) = delete;
    struct Private;
    Private *d;
};

} // namespace dp

#endif // DPPACK_H
