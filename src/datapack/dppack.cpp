#include "dppack.h"
#include "dpzip.h"

extern "C"
{
#include "biomes.h"
}

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <memory>

// The loading rules follow the game (net.minecraft.server.packs:
// CompositePackResources, OverlayMetadataSection, TagLoader). The data pack
// loader of the map tool by Jochen Jacobs (mc-datapack-loader, MIT License)
// was read as a reference for the same rules.

namespace dp
{

const char *const REG_DENSITY_FUNCTION = "worldgen/density_function";
const char *const REG_NOISE            = "worldgen/noise";
const char *const REG_NOISE_SETTINGS   = "worldgen/noise_settings";
const char *const REG_WORLD_PRESET     = "worldgen/world_preset";
const char *const REG_PARAMETER_LIST   = "worldgen/multi_noise_biome_source_parameter_list";
const char *const REG_BIOME            = "worldgen/biome";
const char *const REG_DIMENSION        = "dimension";
const char *const REG_DIMENSION_TYPE   = "dimension_type";
const char *const REG_BIOME_TAG        = "tags/worldgen/biome";
const char *const REG_BIOME_COLORS     = "c:worldgen/biome_colors";
const char *const REG_STRUCTURE        = "worldgen/structure";
const char *const REG_STRUCTURE_SET    = "worldgen/structure_set";
const char *const REG_TEMPLATE_POOL    = "worldgen/template_pool";

static const char *const g_registries[] = {
    REG_DENSITY_FUNCTION, REG_NOISE, REG_NOISE_SETTINGS, REG_WORLD_PRESET,
    REG_PARAMETER_LIST, REG_BIOME, REG_DIMENSION, REG_DIMENSION_TYPE,
    REG_STRUCTURE, REG_STRUCTURE_SET, REG_TEMPLATE_POOL,
};

bool packFormatRange(int mc, int *minFormat, int *maxFormat)
{
    // Data pack format per game version, from the version metadata of
    // misode/mcmeta (branch "summary", versions/data.json, field
    // data_pack_version), which is extracted from the version.json inside
    // each game jar. An engine version id stands for several game versions
    // where the engine does not tell them apart:
    //   1.18 8, 1.18.1 8, 1.18.2 9, 1.19 - 1.19.3 10, 1.19.4 12,
    //   1.20 15, 1.20.1 15, 1.20.2 18, 1.20.3 26, 1.20.4 26, 1.20.5 41,
    //   1.20.6 41, 1.21 48, 1.21.1 48, 1.21.2 57, 1.21.3 57, 1.21.4 61,
    //   1.21.5 71, 1.21.6 80, 1.21.7 81, 1.21.8 81, 1.21.9 88, 1.21.10 88,
    //   1.21.11 94 (94.1), 26.1 - 26.1.2 101 (101.1), 26.2 107 (107.1),
    //   26.3 121
    int lo, hi;
    switch (mc)
    {
    case MC_1_18_2:  lo = 9;   hi = 9;   break;   // 1.18.2 only (see versionSupported)
    case MC_1_19_2:  lo = 10;  hi = 10;  break;
    case MC_1_19_4:  lo = 12;  hi = 12;  break;
    case MC_1_20_6:  lo = 15;  hi = 41;  break;
    case MC_1_21_1:  lo = 48;  hi = 48;  break;
    case MC_1_21_2:  lo = 57;  hi = 57;  break;
    case MC_1_21_3:  lo = 57;  hi = 57;  break;
    case MC_1_21_4:  lo = 61;  hi = 61;  break;
    case MC_1_21_5:  lo = 71;  hi = 71;  break;
    case MC_1_21_6:  lo = 80;  hi = 80;  break;
    case MC_1_21_7:  lo = 81;  hi = 81;  break;
    case MC_1_21_8:  lo = 81;  hi = 81;  break;
    case MC_1_21_9:  lo = 88;  hi = 88;  break;
    case MC_1_21_10: lo = 88;  hi = 88;  break;
    case MC_1_21_11: lo = 94;  hi = 94;  break;
    case MC_26_1:    lo = 101; hi = 101; break;
    case MC_26_1_1:  lo = 101; hi = 101; break;
    case MC_26_1_2:  lo = 101; hi = 101; break;
    case MC_26_2:    lo = 107; hi = 107; break;
    case MC_26_3:    lo = 121; hi = 121; break;
    default:
        return false;
    }
    *minFormat = lo;
    *maxFormat = hi;
    return true;
}

//==============================================================================
// JSON
//==============================================================================

// Removes // and # line comments and /* */ comments outside of strings.
static QByteArray stripJsonComments(const QByteArray& in)
{
    QByteArray out;
    out.reserve(in.size());
    const char *p = in.constData();
    const char *end = p + in.size();
    bool str = false;
    while (p < end)
    {
        char c = *p;
        if (str)
        {
            out.append(c);
            if (c == '\\' && p + 1 < end)
                out.append(*++p);
            else if (c == '"')
                str = false;
            p++;
            continue;
        }
        if (c == '"')
        {
            str = true;
            out.append(c);
            p++;
        }
        else if ((c == '/' && p + 1 < end && p[1] == '/') || c == '#')
        {
            while (p < end && *p != '\n')
                p++;
        }
        else if (c == '/' && p + 1 < end && p[1] == '*')
        {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                p++;
            p = p + 1 < end ? p + 2 : end;
            out.append(' ');
        }
        else
        {
            out.append(c);
            p++;
        }
    }
    return out;
}

static bool parseJsonStrict(const QByteArray& data, QJsonValue *value, QString *error)
{
    // QJsonDocument only takes objects and arrays at the top level, a
    // density function file may be a bare number or string: wrap it
    QByteArray wrapped;
    wrapped.reserve(data.size() + 2);
    wrapped.append('[');
    if (data.startsWith("\xEF\xBB\xBF"))
        wrapped.append(data.constData() + 3, data.size() - 3);
    else
        wrapped.append(data);
    wrapped.append(']');
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(wrapped, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() != 1)
    {
        if (err.error != QJsonParseError::NoError)
        {
            // line of the error, for the message
            int line = 1 + (int) wrapped.left(err.offset).count('\n');
            *error = QStringLiteral("%1 (line %2)").arg(err.errorString()).arg(line);
        }
        else
            *error = QStringLiteral("more than one value");
        return false;
    }
    *value = doc.array().at(0);
    return true;
}

bool parseJson(const QByteArray& data, QJsonValue *value, QString *error)
{
    if (parseJsonStrict(data, value, error))
        return true;
    // the game reads its JSON leniently, which lets comments through
    QString e2;
    if (parseJsonStrict(stripJsonComments(data), value, &e2))
        return true;
    return false;
}

QString normalizeId(const QString& id)
{
    if (id.contains(QLatin1Char(':')))
        return id;
    return QStringLiteral("minecraft:") + id;
}

//==============================================================================
// pack sources
//==============================================================================

namespace
{

// Limits against packs that unpack to huge amounts of data (a few hundred
// bytes of deflate stream can stand for a gigabyte of zeros). The vanilla
// worldgen files are below 1 MiB each and about 20 MiB together.
const uint64_t MAX_JSON_FILE = 64ull << 20;     // one JSON file of a pack
const uint64_t MAX_PACK_DATA = 512ull << 20;    // all files a pack hands to the loader
const uint64_t MAX_INNER_JAR = 512ull << 20;    // the game's jar inside the server bundle

class Source
{
public:
    virtual ~Source() {}
    // all files, as paths relative to the pack root with '/'
    virtual QStringList files() = 0;
    // Reads a file of at most @maxSize bytes.
    virtual bool read(const QString& path, QByteArray *data, QString *error, uint64_t maxSize) = 0;
};

QString tooLarge(uint64_t maxSize)
{
    return QStringLiteral("The file is too large (more than %1 MiB).").arg((qulonglong) (maxSize >> 20));
}

class FolderSource : public Source
{
public:
    explicit FolderSource(const QString& root) : root(root)
    {
        // The worldgen report of the data generator of 1.18.2
        // (reports/worldgen) is a data folder without the level "data":
        // <namespace>/worldgen/...
        QDir dir(root);
        bare = !dir.exists(QStringLiteral("data")) && !QFileInfo(dir.filePath(QStringLiteral("pack.mcmeta"))).isFile() &&
            QFileInfo(dir.filePath(QStringLiteral("minecraft/worldgen"))).isDir();
    }
    QStringList files() override
    {
        QStringList out;
        QDir dir(root);
        if (bare)
        {
            addFiles(&out, root, { "worldgen", "dimension", "dimension_type", "tags/worldgen" },
                QStringLiteral("data/"));
            return out;
        }
        // Only what the loader looks at: pack.mcmeta, and below "data" and
        // below "<overlay>/data" the worldgen, dimension and tag folders of
        // every namespace. (A pack or an unpacked game jar holds thousands
        // of other files.)
        if (QFileInfo(dir.filePath(QStringLiteral("pack.mcmeta"))).isFile())
            out.push_back(QStringLiteral("pack.mcmeta"));
        QStringList layers;
        layers.push_back(QString());
        for (const QString& d : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            if (d != QLatin1String("data"))
            {
                layers.push_back(d + QLatin1Char('/'));
                // lets the loader notice a pack that is one folder too deep
                if (QFileInfo(dir.filePath(d + QStringLiteral("/pack.mcmeta"))).isFile())
                    out.push_back(d + QStringLiteral("/pack.mcmeta"));
            }
        }
        for (const QString& layer : layers)
            addFiles(&out, dir.filePath(layer + QStringLiteral("data")), { "worldgen", "dimension",
                "dimension_type", "tags/worldgen", "structure", "structures" }, QString());
        return out;
    }
    // The files below <base>/<namespace>/<sub> of every namespace, as paths
    // relative to the root with @prefix in front
    void addFiles(QStringList *out, const QString& base, std::initializer_list<const char *> subs,
        const QString& prefix) const
    {
        const QDir dir(root), top(base);
        for (const QString& ns : top.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            for (const char *sub : subs)
            {
                QDirIterator it(top.filePath(ns + QLatin1Char('/') + QLatin1String(sub)), QDir::Files,
                    QDirIterator::Subdirectories);
                while (it.hasNext())
                    out->push_back(prefix + dir.relativeFilePath(it.next()));
            }
        }
    }
    bool read(const QString& path, QByteArray *data, QString *error, uint64_t maxSize) override
    {
        data->clear();
        QFile f(QDir(root).filePath(bare && path.startsWith(QLatin1String("data/")) ? path.mid(5) : path));
        if (!f.open(QIODevice::ReadOnly))
        {
            *error = QStringLiteral("The file cannot be read.");
            return false;
        }
        if (f.size() < 0 || (uint64_t) f.size() > maxSize)
        {
            *error = tooLarge(maxSize);
            return false;
        }
        // one byte more than the size, in case the file grows while it is read
        *data = f.read(f.size() + 1);
        if ((uint64_t) data->size() > maxSize)
        {
            data->clear();
            *error = tooLarge(maxSize);
            return false;
        }
        return true;
    }
private:
    QString root;
    bool bare = false;
};

class ZipSource : public Source
{
public:
    bool open(const QString& path, QString *error)
    {
        if (!zip.open(path, error))
            return false;
        buildIndex();
        // The server jar of the game is a bundle: the jar with the data is
        // an entry of it (META-INF/versions/<version>/server-<version>.jar).
        bool anyData = false;
        QString inner;
        for (auto it = index.constBegin(); it != index.constEnd(); ++it)
        {
            if (it.key().startsWith(QLatin1String("data/")))
                anyData = true;
            else if (it.key().startsWith(QLatin1String("META-INF/versions/")) &&
                it.key().endsWith(QLatin1String(".jar")))
                inner = it.key();
        }
        if (!anyData && !inner.isEmpty())
        {
            QByteArray data;
            QString err;
            if (zip.read(index.value(inner), &data, &err, MAX_INNER_JAR) && zip.openData(data, &err))
                buildIndex();
            else
                zip.open(path, &err);   // stay with the outer archive
        }
        return true;
    }
    void buildIndex()
    {
        index.clear();
        const std::vector<ZipReader::Entry>& es = zip.entries();
        for (size_t i = 0; i < es.size(); i++)
            if (!es[i].name.endsWith(QLatin1Char('/')))
                index.insert(es[i].name, i);
    }
    QStringList files() override
    {
        return index.keys();
    }
    bool read(const QString& path, QByteArray *data, QString *error, uint64_t maxSize) override
    {
        auto it = index.constFind(path);
        if (it == index.constEnd())
        {
            *error = QStringLiteral("The file is missing.");
            return false;
        }
        return zip.read(it.value(), data, error, maxSize);
    }
private:
    ZipReader zip;
    QHash<QString, size_t> index;
};

typedef PackFormat Format;
inline Format format(int64_t major, int64_t minor) { return packFormat(major, minor); }
const int64_t MINOR_TOP = 0x7fffffff;

// The minor data pack version of the newest game version of an engine
// version (version.json: pack_version.data_minor); 0 before 1.21.9.
int formatMinor(int mc)
{
    switch (mc)
    {
    case MC_1_21_11: return 1;  // 94.1
    case MC_26_1:               // 101.1
    case MC_26_1_1:
    case MC_26_1_2: return 1;
    case MC_26_2: return 1;     // 107.1
    default: return 0;          // 26.3: 121.0
    }
}

// A number of pack.mcmeta as the game's integer codec reads it: the integer
// part (Number.intValue(); "101.7" is 101, servers 1.19.4 to 26.3 agree).
// False if it is not a number.
bool formatNumber(const QJsonValue& v, int *out)
{
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    *out = d >= 2147483647.0 ? 0x7fffffff : d <= -2147483648.0 ? -0x7fffffff - 1 : (int) d;
    return true;
}

// An InclusiveRange of integers ("supported_formats", "formats"): a number,
// a list of exactly two numbers or {min_inclusive, max_inclusive}, with
// min <= max. False otherwise.
bool readIntRange(const QJsonValue& v, int *lo, int *hi)
{
    bool ok = false;
    if (v.isDouble())
        ok = formatNumber(v, lo) && formatNumber(v, hi);
    else if (v.isArray())
    {
        const QJsonArray a = v.toArray();
        ok = a.size() == 2 && formatNumber(a.at(0), lo) && formatNumber(a.at(1), hi);
    }
    else if (v.isObject())
    {
        const QJsonObject o = v.toObject();
        ok = formatNumber(o.value(QLatin1String("min_inclusive")), lo)
            && formatNumber(o.value(QLatin1String("max_inclusive")), hi);
    }
    return ok && *lo <= *hi;
}

// "min_format" / "max_format" (PackFormat codec): a non-negative number, or
// a list of 1 to 256 of them (major, minor, rest ignored). A single number
// stands for major.0 as the minimum and for every minor of it as the maximum.
bool readFormatVersion(const QJsonValue& v, bool top, Format *f)
{
    int major = 0, minor = top ? (int) MINOR_TOP : 0;
    if (v.isArray())
    {
        const QJsonArray a = v.toArray();
        if (a.isEmpty() || a.size() > 256)
            return false;
        for (const QJsonValue& e : a)
        {
            int n;
            if (!formatNumber(e, &n) || n < 0)
                return false;
        }
        formatNumber(a.at(0), &major);
        if (a.size() > 1)
            formatNumber(a.at(1), &minor);
    }
    else if (!formatNumber(v, &major) || major < 0)
        return false;
    *f = format(major, minor);
    return true;
}

// The format fields of a pack section or an overlay entry as the codec of
// 1.21.9 and later reads them (PackFormat.IntermediaryFormat)
struct FormatFields
{
    bool hasMin = false, hasMax = false, hasFormat = false, hasSupported = false;
    Format min = 0, max = 0;
    int pf = 0;     // "pack_format"
    int supLo = 0, supHi = 0;

    // Reads the fields; @rangeKey is "supported_formats" or "formats", @withFormat
    // whether "pack_format" is one of them. False (with @error) if one that
    // is there cannot be read: the codec fails.
    bool read(const QJsonObject& o, const char *rangeKey, bool withFormat, QString *error)
    {
        QJsonValue v = o.value(QLatin1String("min_format"));
        if ((hasMin = !v.isUndefined()) && !readFormatVersion(v, false, &min))
            return *error = QStringLiteral("\"min_format\" is not a format version"), false;
        v = o.value(QLatin1String("max_format"));
        if ((hasMax = !v.isUndefined()) && !readFormatVersion(v, true, &max))
            return *error = QStringLiteral("\"max_format\" is not a format version"), false;
        v = o.value(QLatin1String("pack_format"));
        if (withFormat && (hasFormat = !v.isUndefined()) && !formatNumber(v, &pf))
            return *error = QStringLiteral("\"pack_format\" is not a number"), false;
        v = o.value(QLatin1String(rangeKey));
        if ((hasSupported = !v.isUndefined()) && !readIntRange(v, &supLo, &supHi))
            return *error = QStringLiteral("\"%1\" is not a range of formats").arg(QLatin1String(rangeKey)), false;
        return true;
    }
    bool any() const { return hasMin || hasMax || hasSupported; }
    int effectiveMinMajor() const
    {
        if (hasMin)
            return hasSupported ? std::min((int) (min >> 32), supLo) : (int) (min >> 32);
        return hasSupported ? supLo : 0x7fffffff;
    }
    // "pack_format" against a range (validatePackFormatForRange)
    bool formatInRange(int lo, int hi, QString *error) const
    {
        if (pf < lo || pf > hi)
            return *error = QStringLiteral("\"pack_format\" %1 is outside the declared formats %2 to %3")
                .arg(pf).arg(lo).arg(hi), false;
        if (pf < 15)
            return *error = QStringLiteral("a pack for several versions cannot start before format 15"), false;
        return true;
    }
    // PackFormat.IntermediaryFormat.validate() with the last format before
    // minor versions (81 for data packs)
    bool validate(bool hasPackFormatField, bool requireOld, const char *oldKey, Format *lo, Format *hi,
                  QString *error) const
    {
        const int last = 81;
        const QString key = QLatin1String(oldKey);
        if (hasMin != hasMax)
            return *error = QStringLiteral("it must declare both \"min_format\" and \"max_format\""), false;
        if (requireOld && !hasSupported)
            return *error = QStringLiteral("\"%1\" is missing, which every overlay needs when one supports a "
                "format up to %2").arg(key).arg(last), false;
        if (hasMin)
        {
            const int majorMin = (int) (min >> 32), majorMax = (int) (max >> 32);
            if (min > max)
                return *error = QStringLiteral("\"min_format\" is greater than \"max_format\""), false;
            if (majorMin > last && !requireOld)
            {
                if (hasSupported)
                    return *error = QStringLiteral("\"%1\" is not allowed from format %2 on").arg(key).arg(last + 1),
                        false;
                if (hasPackFormatField && hasFormat && !formatInRange(majorMin, majorMax, error))
                    return false;
            }
            else
            {
                if (!hasSupported)
                    return *error = QStringLiteral("it supports format %1, which needs \"%2\"").arg(majorMin).arg(key),
                        false;
                if (supLo != majorMin)
                    return *error = QStringLiteral("\"%1\" starts at %2, \"min_format\" at %3").arg(key).arg(supLo)
                        .arg(majorMin), false;
                if (supHi != majorMax && supHi != last)
                    return *error = QStringLiteral("\"%1\" ends at %2, \"max_format\" at %3").arg(key).arg(supHi)
                        .arg(majorMax), false;
                if (hasPackFormatField)
                {
                    if (!hasFormat)
                        return *error = QStringLiteral("it supports a format up to %1, which needs \"pack_format\"")
                            .arg(last), false;
                    if (!formatInRange(majorMin, majorMax, error))
                        return false;
                }
            }
            *lo = min;
            *hi = max;
            return true;
        }
        if (hasSupported)
        {
            if (supHi > last)
                return *error = QStringLiteral("it supports formats after %1 without \"min_format\" and "
                    "\"max_format\"").arg(last), false;
            if (hasPackFormatField)
            {
                if (!hasFormat)
                    return *error = QStringLiteral("it supports a format up to %1, which needs \"pack_format\"")
                        .arg(last), false;
                if (!formatInRange(supLo, supHi, error))
                    return false;
            }
            *lo = format(supLo, 0);
            *hi = format(supHi, 0);
            return true;
        }
        if (hasPackFormatField && hasFormat)
        {
            if (pf > last)
                return *error = QStringLiteral("it supports a format after %1 without \"min_format\" and "
                    "\"max_format\"").arg(last), false;
            *lo = *hi = format(pf, 0);
            return true;
        }
        return *error = QStringLiteral("it declares no formats"), false;
    }
};

// The directory of an overlay entry (OverlayMetadataSection.DIR_VALIDATOR)
bool validOverlayDirectory(const QJsonValue& v)
{
    static const QRegularExpression re(QStringLiteral("^[-_a-zA-Z0-9.]+$"));
    return v.isString() && re.match(v.toString()).hasMatch();
}

// The overlays a version before 1.21.9 (format 18 to 81) applies: every
// entry needs a directory and "formats"; one broken entry and the game
// ignores all of them.
bool overlaysBefore1_21_9(const QJsonValue& section, int major, QStringList *applied, QString *error)
{
    const QJsonValue entries = section.toObject().value(QLatin1String("entries"));
    if (!section.isObject() || !entries.isArray())
        return *error = QStringLiteral("the \"overlays\" section has no list of entries"), false;
    QStringList dirs;
    for (const QJsonValue& ev : entries.toArray())
    {
        const QJsonObject eo = ev.toObject();
        int lo, hi;
        if (!ev.isObject() || !validOverlayDirectory(eo.value(QLatin1String("directory"))))
            return *error = QStringLiteral("an overlay entry has no valid directory"), false;
        if (!readIntRange(eo.value(QLatin1String("formats")), &lo, &hi))
            return *error = QStringLiteral("the overlay \"%1\" has no \"formats\" range, which this version "
                "needs").arg(eo.value(QLatin1String("directory")).toString()), false;
        if (lo <= major && major <= hi)
            dirs.push_back(eo.value(QLatin1String("directory")).toString());
    }
    *applied = dirs;
    return true;
}

// The overlays of 1.21.9 and later (PackFormat.validateHolderList): entries
// without any format are skipped; if one entry supports a format up to 81,
// every entry needs "formats" next to "min_format" and "max_format".
bool overlaysSince1_21_9(const QJsonValue& section, Format version, QStringList *applied, QString *error)
{
    const QJsonValue entries = section.toObject().value(QLatin1String("entries"));
    if (!section.isObject() || !entries.isArray())
        return *error = QStringLiteral("the \"overlays\" section has no list of entries"), false;
    std::vector<std::pair<QString, FormatFields>> list;
    int minVersion = 0x7fffffff;
    for (const QJsonValue& ev : entries.toArray())
    {
        const QJsonObject eo = ev.toObject();
        if (!ev.isObject() || !validOverlayDirectory(eo.value(QLatin1String("directory"))))
            return *error = QStringLiteral("an overlay entry has no valid directory"), false;
        FormatFields f;
        QString err;
        const QString dir = eo.value(QLatin1String("directory")).toString();
        if (!f.read(eo, "formats", false, &err))
            return *error = QStringLiteral("overlay \"%1\": %2").arg(dir, err), false;
        list.emplace_back(dir, f);
        minVersion = std::min(minVersion, f.effectiveMinMajor());
    }
    QStringList dirs;
    for (const auto& e : list)
    {
        if (!e.second.any())
            continue;   // "Unknown or broken overlay entry", skipped
        Format lo, hi;
        QString err;
        if (!e.second.validate(false, minVersion <= 81, "formats", &lo, &hi, &err))
            return *error = QStringLiteral("overlay \"%1\": %2").arg(e.first, err), false;
        if (lo <= version && version <= hi)
            dirs.push_back(e.first);
    }
    *applied = dirs;
    return true;
}

// The pack's description as the component codec takes it: required; from
// 1.20.3 (format 26) a string, a list or an object, before that also a
// number or boolean; an empty list or object is no component in any version.
bool validDescription(const QJsonValue& v, int major)
{
    if (v.isString())
        return true;
    if (v.isArray())
        return !v.toArray().isEmpty();
    if (v.isObject())
        return !v.toObject().isEmpty();
    return (v.isDouble() || v.isBool()) && major < 26;
}

// A text component as plain text
QString componentText(const QJsonValue& v)
{
    QString s;
    if (v.isString())
        s = v.toString();
    else if (v.isArray())
    {
        for (const QJsonValue& e : v.toArray())
            s += componentText(e);
    }
    else if (v.isObject())
    {
        QJsonObject o = v.toObject();
        if (o.contains(QLatin1String("text")))
            s = o.value(QLatin1String("text")).toString();
        else if (o.contains(QLatin1String("fallback")))
            s = o.value(QLatin1String("fallback")).toString();
        else if (o.contains(QLatin1String("translate")))
            s = o.value(QLatin1String("translate")).toString();
        s += componentText(o.value(QLatin1String("extra")));
    }
    // drop legacy formatting codes
    QString out;
    for (int i = 0; i < s.size(); i++)
    {
        if (s[i] == QChar(0x00a7) && i + 1 < s.size())
        {
            i++;
            continue;
        }
        out += s[i];
    }
    return out;
}

void addIssue(Report *report, Issue::Severity sev, const QString& pack, const QString& id, const QString& text)
{
    Issue i;
    i.severity = sev;
    i.pack = pack;
    i.id = id;
    i.text = text;
    report->issues.push_back(i);
}

} // namespace

PackMeta readPackMeta(const QByteArray& text, PackFormat version)
{
    PackMeta m;
    const int major = (int) (version >> 32);
    QJsonValue root;
    QString err;
    // the game parses pack.mcmeta strictly: no comments, no trailing commas
    if (!parseJsonStrict(text, &root, &err) || !root.isObject())
    {
        m.reason = QStringLiteral("pack.mcmeta is not valid JSON%1.").arg(err.isEmpty() ? QString() : ": " + err);
        return m;
    }
    const QJsonObject meta = root.toObject();
    const QJsonValue pv = meta.value(QLatin1String("pack"));
    if (!pv.isObject())
    {
        m.reason = pv.isUndefined() ? QStringLiteral("pack.mcmeta has no \"pack\" section.")
                                    : QStringLiteral("the \"pack\" section of pack.mcmeta is not an object.");
        return m;
    }
    const QJsonObject pack = pv.toObject();
    const QJsonValue desc = pack.value(QLatin1String("description"));
    if (!validDescription(desc, major))
    {
        m.reason = desc.isUndefined() ? QStringLiteral("pack.mcmeta has no \"description\".")
                                      : QStringLiteral("the \"description\" of pack.mcmeta is no text component.");
        return m;
    }
    m.description = componentText(desc);

    if (major < 82)
    {
        // before 1.21.9: "pack_format" is required; "supported_formats"
        // (from 1.20.2) is the range when the game can read it
        if (!formatNumber(pack.value(QLatin1String("pack_format")), &m.packFormat))
        {
            m.reason = QStringLiteral("pack.mcmeta has no number \"pack_format\", which this version needs.");
            return m;
        }
        m.lo = m.hi = format(m.packFormat, 0);
        int a, b;
        if (major >= 18 && readIntRange(pack.value(QLatin1String("supported_formats")), &a, &b))
        {
            m.lo = format(a, 0);
            m.hi = format(b, 0);
        }
        m.rangeKnown = true;
    }
    else
    {
        // 1.21.9 and later: a pack section the game cannot validate still
        // loads, with formats unknown
        FormatFields f;
        QString e;
        if (f.read(pack, "supported_formats", true, &e) && f.validate(true, false, "supported_formats", &m.lo, &m.hi, &e))
            m.rangeKnown = true;
        else
            m.formatProblem = e;
        m.packFormat = f.hasFormat ? f.pf : m.rangeKnown ? (int) (m.lo >> 32) : 0;
    }
    m.loaded = true;

    // From 26.1 on a "features" section the game cannot read refuses the
    // pack (before, the section was dropped): a list of the feature flags of
    // the version (FeatureFlags, the same in 26.1 to 26.3)
    const QJsonValue features = meta.value(QLatin1String("features"));
    if (major >= 101 && !features.isUndefined())
    {
        static const QStringList flags = { "minecraft:vanilla", "minecraft:trade_rebalance",
            "minecraft:redstone_experiments", "minecraft:minecart_improvements" };
        const QJsonValue enabled = features.toObject().value(QLatin1String("enabled"));
        bool ok = features.isObject() && enabled.isArray();
        for (const QJsonValue& f : enabled.toArray())
            ok = ok && f.isString() && flags.contains(normalizeId(f.toString()));
        if (!ok)
        {
            m.loaded = false;
            m.reason = QStringLiteral("the \"features\" of pack.mcmeta are not a list of feature flags of this "
                "version.");
            return m;
        }
    }

    // overlays: since 1.20.2 (format 18)
    const QJsonValue ov = meta.value(QLatin1String("overlays"));
    if (major < 18 || ov.isUndefined())
        return m;
    QStringList applied;
    QString oe;
    const bool ok = major < 82 ? overlaysBefore1_21_9(ov, major, &applied, &oe)
                               : overlaysSince1_21_9(ov, version, &applied, &oe);
    if (!ok)
    {
        // 26.1 and later refuse the whole pack, older versions only its overlays
        if (major >= 101)
        {
            m.loaded = false;
            m.reason = QStringLiteral("the overlays of pack.mcmeta are invalid: %1.").arg(oe);
        }
        else
        {
            m.overlaysIgnored = true;
            m.reason = QStringLiteral("the game ignores the overlays of the pack: %1.").arg(oe);
        }
        return m;
    }
    m.overlays = applied;
    return m;
}

namespace
{

// Loads one pack on top of @res. @index is the pack's index in res->packs.
void loadPack(const QString& path, int index, int mc, bool vanilla, Resources *res, Report *report)
{
    PackInfo& info = res->packs[index];
    info.path = path;
    info.name = QFileInfo(path).fileName();
    if (info.name.isEmpty())
        info.name = QDir(path).dirName();

    std::unique_ptr<Source> src;
    QFileInfo fi(path);
    if (!fi.exists())
    {
        addIssue(report, Issue::Error, info.name, QString(),
            vanilla ? QStringLiteral("The vanilla data was not found at \"%1\".").arg(path)
                    : QStringLiteral("The pack was not found at \"%1\".").arg(path));
        return;
    }
    if (fi.isDir())
    {
        src.reset(new FolderSource(path));
    }
    else
    {
        std::unique_ptr<ZipSource> zs(new ZipSource);
        QString err;
        if (!zs->open(path, &err))
        {
            addIssue(report, Issue::Error, info.name, QString(),
                QStringLiteral("The archive cannot be read: %1").arg(err));
            return;
        }
        src.reset(zs.release());
    }

    QStringList all = src->files();
    QSet<QString> present(all.begin(), all.end());

    // reads a file within the pack's budget (MAX_PACK_DATA); after the
    // budget is used up nothing more is read (one error)
    uint64_t used = 0;
    bool overBudget = false;
    auto readFile = [&](const QString& file, QByteArray *data, QString *err) -> bool {
        if (overBudget)
        {
            *err = QStringLiteral("not read: the pack's files unpack to too much data.");
            return false;
        }
        const uint64_t cap = std::min(MAX_JSON_FILE, MAX_PACK_DATA - used);
        if (src->read(file, data, err, cap))
        {
            used += (uint64_t) data->size();
            return true;
        }
        if (cap < MAX_JSON_FILE && used > MAX_PACK_DATA - MAX_JSON_FILE)
        {
            overBudget = true;
            addIssue(report, Issue::Error, info.name, QString(),
                QStringLiteral("The pack's files unpack to more than %1 MiB; the rest of the pack is not read.")
                    .arg((qulonglong) (MAX_PACK_DATA >> 20)));
            *err = QStringLiteral("not read: the pack's files unpack to too much data.");
        }
        return false;
    };

    int vminMajor = 0, vmaxMajor = 0;
    bool haveVersion = packFormatRange(mc, &vminMajor, &vmaxMajor);
    // the formats of the engine version's game versions
    const Format vmin = format(vminMajor, 0), vmax = format(vmaxMajor, formatMinor(mc));

    // pack.mcmeta
    std::vector<QString> layers;    // directory prefixes, lowest priority first
    layers.push_back(QString());
    info.formatMatches = true;
    if (present.contains(QStringLiteral("pack.mcmeta")))
    {
        QByteArray data;
        QString err;
        if (!readFile(QStringLiteral("pack.mcmeta"), &data, &err))
        {
            addIssue(report, Issue::Error, info.name, QStringLiteral("pack.mcmeta"), err);
        }
        else
        {
            // the game version of the engine version: its newest
            const PackMeta pm = readPackMeta(data, vmax);
            info.description = pm.description;
            info.packFormat = pm.packFormat;
            if (vanilla || !haveVersion)
            {
                // the vanilla data, or a version without known rules: no checks
            }
            else if (!pm.loaded)
            {
                // the game leaves the pack out: so does the generator
                info.loaded = false;
                addIssue(report, Issue::Warning, info.name, QStringLiteral("pack.mcmeta"),
                    QStringLiteral("The game does not load this pack in this version of Minecraft: %1 "
                        "Nothing of it is used.").arg(pm.reason));
                return;
            }
            else
            {
                info.formatMatches = pm.rangeKnown && pm.lo <= vmax && pm.hi >= vmin;
                info.formatProblem = pm.formatProblem;
                if (pm.overlaysIgnored)
                    addIssue(report, Issue::Warning, info.name, QStringLiteral("pack.mcmeta"),
                        pm.reason.left(1).toUpper() + pm.reason.mid(1));
            }
            for (const QString& dir : pm.overlays)
            {
                layers.push_back(dir + QLatin1Char('/'));
                info.overlays.push_back(dir);
            }
        }
    }
    else if (!vanilla)
    {
        addIssue(report, Issue::Warning, info.name, QString(),
            QStringLiteral("The pack has no pack.mcmeta; the game would not load it."));
    }

    // The files of the pack: of every path the highest layer that has it
    // (later overlays win over earlier ones and over the pack itself).
    QHash<QString, QString> chosen;     // path below the layer -> full path
    for (const QString& layer : layers)
    {
        const QString prefix = layer + QStringLiteral("data/");
        for (const QString& f : all)
        {
            if (!f.startsWith(prefix))
                continue;
            chosen.insert(f.mid(layer.size()), f);
        }
    }

    bool anyData = false;
    for (auto it = chosen.constBegin(); it != chosen.constEnd(); ++it)
    {
        // data/<namespace>/<registry>/<path>.json
        const QString& rel = it.key();
        int nsEnd = rel.indexOf(QLatin1Char('/'), 5);
        if (nsEnd < 0)
            continue;
        anyData = true;
        if (rel.endsWith(QLatin1String(".nbt")))
        {
            // structure templates: only noted here, read when a structure needs them
            const QLatin1String dir = mc >= MC_1_21_1 ? QLatin1String("structure/") : QLatin1String("structures/");
            QString rest = rel.mid(nsEnd + 1, rel.size() - nsEnd - 1 - 4);
            if (rest.startsWith(dir) && rest.size() > dir.size())
            {
                FileRef ref;
                ref.pack = index;
                ref.path = it.value();
                res->templates.insert(rel.mid(5, nsEnd - 5) + QLatin1Char(':') + rest.mid(dir.size()), ref);
            }
            continue;
        }
        if (!rel.endsWith(QLatin1String(".json")))
            continue;
        QString ns = rel.mid(5, nsEnd - 5);
        QString rest = rel.mid(nsEnd + 1, rel.size() - nsEnd - 1 - 5);

        if (rest.startsWith(QLatin1String("worldgen/")) || rest.startsWith(QLatin1String("dimension/")) ||
            rest.startsWith(QLatin1String("dimension_type/")))
            info.worldgenFiles++;

        if (ns == QLatin1String("c") && rest == QLatin1String("worldgen/biome_colors"))
        {
            Resource r;
            r.pack = index;
            QString err;
            if (readFile(it.value(), &r.data, &err))
                res->colorFiles.push_back(r);
            else
                addIssue(report, Issue::Warning, info.name, QStringLiteral("c:worldgen/biome_colors"), err);
            continue;
        }

        const char *registry = nullptr;
        bool tag = false;
        QString name;
        for (const char *reg : g_registries)
        {
            int n = (int) strlen(reg);
            if (rest.size() > n + 1 && rest[n] == QLatin1Char('/') && rest.startsWith(QLatin1String(reg)))
            {
                registry = reg;
                name = rest.mid(n + 1);
                break;
            }
        }
        if (!registry)
        {
            int n = (int) strlen(REG_BIOME_TAG);
            if (rest.size() > n + 1 && rest[n] == QLatin1Char('/') && rest.startsWith(QLatin1String(REG_BIOME_TAG)))
            {
                registry = REG_BIOME_TAG;
                name = rest.mid(n + 1);
                tag = true;
            }
        }
        if (!registry)
            continue;
        // The vanilla data only defines dimensions through its world
        // presets. Copies of it that were made with the data generator also
        // contain dimension files (those of the preset "normal"), which must
        // not override the requested preset.
        if (vanilla && registry == REG_DIMENSION)
            continue;

        QString id = ns + QLatin1Char(':') + name;
        Resource r;
        r.pack = index;
        QString err;
        if (overBudget)
            break;
        if (!readFile(it.value(), &r.data, &err))
        {
            if (!overBudget)
                addIssue(report, Issue::Error, info.name, id,
                    QStringLiteral("%1 cannot be read: %2").arg(it.value(), err));
            continue;
        }
        if (!tag)
        {
            res->files[QLatin1String(registry)].insert(id, r);
            continue;
        }

        QJsonValue v;
        if (!parseJson(r.data, &v, &err) || !v.isObject())
        {
            addIssue(report, Issue::Error, info.name, QLatin1Char('#') + id,
                QStringLiteral("The tag file %1 is not valid JSON: %2").arg(it.value(), err));
            continue;
        }
        TagPart part;
        part.pack = index;
        part.replace = v.toObject().value(QLatin1String("replace")).toBool(false);
        for (const QJsonValue& e : v.toObject().value(QLatin1String("values")).toArray())
        {
            if (e.isString())
            {
                part.values.push_back(e.toString());
            }
            else if (e.isObject())
            {
                QString s = e.toObject().value(QLatin1String("id")).toString();
                if (s.isEmpty())
                    continue;
                part.values.push_back(s);
                if (!e.toObject().value(QLatin1String("required")).toBool(true))
                    part.optional.push_back(s);
            }
        }
        res->tags[QLatin1String(registry)][id].push_back(part);
    }

    if (!anyData)
    {
        // a common mistake: the pack's files are one folder further down
        QString nested;
        for (const QString& f : all)
        {
            int i = f.indexOf(QLatin1String("/data/"));
            if (i > 0 && !f.left(i).contains(QLatin1Char('/')) &&
                present.contains(f.left(i) + QStringLiteral("/pack.mcmeta")))
            {
                nested = f.left(i);
                break;
            }
        }
        if (!nested.isEmpty())
            addIssue(report, vanilla ? Issue::Error : Issue::Warning, info.name, QString(),
                QStringLiteral("The data folder is not at the top of the pack but inside \"%1\"; "
                    "the game would not find it there either.").arg(nested));
        else
            addIssue(report, vanilla ? Issue::Error : Issue::Warning, info.name, QString(),
                vanilla ? QStringLiteral("No data folder was found in the vanilla data.")
                        : QStringLiteral("The pack has no data folder."));
    }
}

} // namespace

void loadResources(const Request& request, Resources *res, Report *report)
{
    res->packs.clear();
    res->files.clear();
    res->tags.clear();
    res->colorFiles.clear();
    res->templates.clear();

    res->packs.emplace_back();
    loadPack(request.vanillaPath, 0, request.mc, true, res, report);
    for (const PackSource& ps : request.packs)
    {
        if (!ps.enabled)
            continue;
        res->packs.emplace_back();
        loadPack(ps.path, (int) res->packs.size() - 1, request.mc, false, res, report);
    }
}

struct PackFiles::Private
{
    const Resources *res = nullptr;
    std::vector<std::unique_ptr<Source>> sources;
    std::vector<bool> tried;
};

PackFiles::PackFiles(const Resources& res) : d(new Private)
{
    d->res = &res;
    d->sources.resize(res.packs.size());
    d->tried.resize(res.packs.size(), false);
}

PackFiles::~PackFiles()
{
    delete d;
}

bool PackFiles::read(const FileRef& ref, QByteArray *data, QString *error)
{
    if (ref.pack < 0 || (size_t) ref.pack >= d->sources.size())
    {
        *error = QStringLiteral("The file belongs to no pack.");
        return false;
    }
    if (!d->tried[ref.pack])
    {
        d->tried[ref.pack] = true;
        const QString& path = d->res->packs[ref.pack].path;
        if (QFileInfo(path).isDir())
        {
            d->sources[ref.pack].reset(new FolderSource(path));
        }
        else
        {
            std::unique_ptr<ZipSource> zs(new ZipSource);
            QString err;
            if (zs->open(path, &err))
                d->sources[ref.pack].reset(zs.release());
        }
    }
    if (!d->sources[ref.pack])
    {
        *error = QStringLiteral("The pack cannot be opened again.");
        return false;
    }
    // structure templates: readTemplate() refuses more than 64 MiB unpacked
    return d->sources[ref.pack]->read(ref.path, data, error, 64ull << 20);
}

const Resource *Resources::find(const char *registry, const QString& id) const
{
    auto it = files.constFind(QLatin1String(registry));
    if (it == files.constEnd())
        return nullptr;
    auto jt = it->constFind(id);
    if (jt == it->constEnd())
        return nullptr;
    return &jt.value();
}

QStringList Resources::ids(const char *registry) const
{
    auto it = files.constFind(QLatin1String(registry));
    if (it == files.constEnd())
        return QStringList();
    return it->keys();
}

// Tags nested deeper than this are refused (each level is a recursion of
// resolveTag(); the loader runs on a thread with a small stack)
static const int MAX_TAG_DEPTH = 256;

static void resolveTag(const Resources& res, const char *registry, const QString& id,
    QStringList& out, QSet<QString>& seen, QSet<QString>& open, QSet<QString>& done, QString *error)
{
    auto it = res.tags.constFind(QLatin1String(registry));
    if (it == res.tags.constEnd())
        return;
    auto jt = it->constFind(id);
    if (jt == it->constEnd())
        return;
    if (open.contains(id))
        return;     // a tag that contains itself
    // a tag that was resolved before adds nothing new (all its values are in
    // "seen" already, or come from a tag that is still open)
    if (done.contains(id))
        return;
    if (open.size() >= MAX_TAG_DEPTH)
    {
        if (error->isEmpty())
            *error = QStringLiteral("The tag #%1 nests other tags more than %2 levels deep.").arg(id).arg(MAX_TAG_DEPTH);
        return;
    }
    open.insert(id);
    // values of all packs in order; "replace" drops those of earlier packs
    QStringList values;
    for (const TagPart& part : jt.value())
    {
        if (part.replace)
            values.clear();
        values += part.values;
    }
    for (const QString& v : values)
    {
        if (v.startsWith(QLatin1Char('#')))
        {
            resolveTag(res, registry, normalizeId(v.mid(1)), out, seen, open, done, error);
        }
        else
        {
            QString e = normalizeId(v);
            if (!seen.contains(e))
            {
                seen.insert(e);
                out.push_back(e);
            }
        }
    }
    open.remove(id);
    done.insert(id);
}

QStringList Resources::tagValues(const char *registry, const QString& id, QString *error) const
{
    QStringList out;
    QSet<QString> seen, open, done;
    QString err;
    resolveTag(*this, registry, normalizeId(id), out, seen, open, done, &err);
    if (error)
        *error = err;
    return out;
}

QString Resources::packName(int pack) const
{
    if (pack < 0 || (size_t) pack >= packs.size())
        return QString();
    return packs[pack].name;
}

} // namespace dp
