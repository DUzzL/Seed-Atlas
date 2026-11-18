#include "dpcheck.h"

#include "aboutdialog.h"

#include "datapack/dpworld.h"
// Only to read the files of a pack archive.
#include "datapack/dpzip.h"
#include "util.h"
#include "seedatlas-engine/util.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <map>

namespace {

// One file of a pack: its path inside the pack ('/' separated) and a way to
// read it.
struct PackFile
{
    QString name;
    size_t zipIndex = 0;
};

class PackReader
{
public:
    bool open(const QString& path)
    {
        const QFileInfo info(path);
        if (info.isDir())
        {
            folder = path;
            QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                PackFile f;
                f.name = QDir(path).relativeFilePath(it.next());
                list.push_back(f);
            }
            return true;
        }
        QString error;
        if (!zip.open(path, &error))
            return false;
        for (size_t i = 0; i < zip.entries().size(); i++)
        {
            PackFile f;
            f.name = zip.entries()[i].name;
            f.zipIndex = i;
            list.push_back(f);
        }
        return true;
    }
    // The file's contents, or nothing if it is larger than a JSON file can
    // sensibly be or the pack's budget is used up (like the loader's limits).
    QByteArray read(const PackFile& f)
    {
        static const qint64 MAX_FILE = 64ll << 20;
        static const qint64 MAX_PACK = 512ll << 20;
        const qint64 left = qMin(MAX_FILE, MAX_PACK - used);
        if (left <= 0)
            return QByteArray();
        QByteArray data;
        if (!folder.isEmpty())
        {
            QFile file(QDir(folder).filePath(f.name));
            if (!file.open(QIODevice::ReadOnly) || file.size() > left)
                return QByteArray();
            data = file.read(left);
        }
        else
        {
            QString error;
            if (!zip.read(f.zipIndex, &data, &error, (uint64_t) left))
                return QByteArray();
        }
        used += data.size();
        return data;
    }
    std::vector<PackFile> list;

private:
    QString folder;
    dp::ZipReader zip;
    qint64 used = 0;
};

QString ms(double v)
{
    return QLocale::c().toString(v, 'f', v < 10 ? 1 : 0) + " ms";
}

QString us(double v)
{
    return QLocale::c().toString(v, 'f', v < 10 ? 2 : 1) + " us";
}

// at most @n ids, then "and N more"
QString idList(const QStringList& ids, int n = 8)
{
    if (ids.size() <= n)
        return ids.join(", ");
    return QStringList(ids.mid(0, n)).join(", ") + QString(" and %1 more").arg(ids.size() - n);
}

// the values of "type" keys with a namespace other than minecraft
void collectTypes(const QJsonValue& v, QMap<QString, QStringList> *types, const QString& id)
{
    if (v.isObject())
    {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
        {
            if (it.key() == QLatin1String("type") && it.value().isString())
            {
                const QString t = it.value().toString();
                if (t.contains(':') && !t.startsWith(QLatin1String("minecraft:")))
                {
                    QStringList& list = (*types)[t];
                    if (!list.contains(id))
                        list.append(id);
                }
            }
            collectTypes(it.value(), types, id);
        }
    }
    else if (v.isArray())
    {
        for (const QJsonValue& e : v.toArray())
            collectTypes(e, types, id);
    }
}

// a dimension's generator and biome source, in one line
QString describeDimension(const QJsonObject& dim)
{
    const QJsonObject gen = dim.value("generator").toObject();
    QStringList parts;
    parts << QString("generator %1").arg(gen.value("type").toString("?"));
    if (gen.contains("biome_source"))
    {
        const QJsonObject bs = gen.value("biome_source").toObject();
        QString source = bs.value("type").toString("?");
        if (bs.value("preset").isString())
            source += QString(" (preset %1)").arg(bs.value("preset").toString());
        else if (bs.value("biomes").isArray())
            source += QString(" (%1 entries)").arg(bs.value("biomes").toArray().size());
        else if (bs.value("biome").isString())
            source += QString(" (%1)").arg(bs.value("biome").toString());
        parts << QString("biome source %1").arg(source);
    }
    if (gen.value("settings").isString())
        parts << QString("settings %1").arg(gen.value("settings").toString());
    else if (gen.value("settings").isObject())
        parts << QString("settings inline");
    if (dim.value("type").isString())
        parts << QString("type %1").arg(dim.value("type").toString());
    return parts.join(", ");
}

struct FileScan
{
    QMap<QString, int> registries;          // "worldgen/biome" -> files
    QStringList dimensions;                 // "ns:name: generator ..., biome source ..."
    QStringList presets;                    // "ns:name: dimensions ..."
    QMap<QString, QStringList> customTypes; // non-vanilla "type" values -> file ids
    int unreadable = 0;
};

FileScan scanPack(const QString& path, const QStringList& overlays)
{
    FileScan out;
    PackReader reader;
    if (!reader.open(path))
    {
        out.unreadable = -1;
        return out;
    }
    QStringList roots = {QString()};
    for (const QString& o : overlays)
        roots << o + '/';
    for (const PackFile& f : reader.list)
    {
        for (const QString& root : roots)
        {
            if (!f.name.startsWith(root + "data/"))
                continue;
            const QStringList parts = f.name.mid(root.size() + 5).split('/');
            if (parts.size() < 3)
                continue;
            const QString ns = parts[0];
            QString registry;
            int rest = 2;
            if (parts[1] == QLatin1String("worldgen") && parts.size() >= 4)
            {
                registry = "worldgen/" + parts[2];
                rest = 3;
            }
            else if (parts[1] == QLatin1String("tags") && parts.size() >= 5
                && parts[2] == QLatin1String("worldgen"))
            {
                registry = "tags/worldgen/" + parts[3];
                rest = 4;
            }
            else if (parts[1] == QLatin1String("dimension") || parts[1] == QLatin1String("dimension_type"))
                registry = parts[1];
            else if ((parts[1] == QLatin1String("structure") || parts[1] == QLatin1String("structures"))
                && f.name.endsWith(QLatin1String(".nbt")))
                registry = "structure templates (.nbt)";
            else
                continue;
            const bool json = f.name.endsWith(QLatin1String(".json"));
            if (!json && !registry.startsWith(QLatin1String("structure templates")))
                continue;
            out.registries[registry]++;
            if (!json)
                continue;
            QString name = QStringList(parts.mid(rest)).join('/');
            name.chop(5);
            const QString id = ns + ':' + name;
            const bool dims = registry == QLatin1String("dimension");
            const bool preset = registry == QLatin1String("worldgen/world_preset");
            const bool density = registry == QLatin1String("worldgen/density_function")
                || registry == QLatin1String("worldgen/noise_settings");
            if (!dims && !preset && !density)
                continue;
            const QByteArray data = reader.read(f);
            const QJsonDocument doc = QJsonDocument::fromJson(data);
            if (!doc.isObject() && !doc.isArray())
            {   // a density function can be a constant or a reference
                const QByteArray text = data.trimmed();
                bool number = false;
                text.toDouble(&number);
                if (!number && !(text.startsWith('"') && text.endsWith('"')))
                    out.unreadable++;
                continue;
            }
            const QString where = root.isEmpty() ? id : QString("%1 (%2)").arg(id, root.chopped(1));
            if (dims)
                out.dimensions << QString("%1: %2").arg(where, describeDimension(doc.object()));
            if (preset)
            {
                QStringList list;
                const QJsonObject ds = doc.object().value("dimensions").toObject();
                for (auto it = ds.begin(); it != ds.end(); ++it)
                    list << QString("%1 (%2)").arg(it.key(), describeDimension(it.value().toObject()));
                out.presets << QString("%1: %2").arg(where, list.join("; "));
                collectTypes(doc.object(), &out.customTypes, id);
            }
            if (dims || density)
                collectTypes(doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()),
                    &out.customTypes, id);
        }
    }
    return out;
}

struct Timing
{
    double load = 0;
    QStringList lines;
};

// the speed sample of one loaded world
QStringList speedSample(const std::shared_ptr<const dp::World>& world, const QString& wanted,
    uint64_t seed, const std::atomic_bool *abort)
{
    QStringList lines;
    int dim = world->dimension(wanted);
    if (dim < 0 || !world->dimensions()[dim].supported)
    {
        dim = -1;
        for (int i = 0; i < (int) world->dimensions().size() && dim < 0; i++)
            if (world->dimensions()[i].supported)
                dim = i;
    }
    if (dim < 0)
    {
        lines << "speed: no dimension that the map can show";
        return lines;
    }
    const dp::DimensionInfo& info = world->dimensions()[dim];
    lines << QString("speed sample (%1, seed %2, one thread):").arg(info.id).arg((qint64) seed);
    QElapsedTimer t;
    t.start();
    dp::Sampler sampler(world, dim, seed);
    const double make = t.nsecsElapsed() / 1e6;
    const int top = info.minY + info.height - 1;
    {
        const int n = 64;
        std::vector<int> ids(n * n);
        sampler.genBiomes(ids.data(), 4, -n / 2, -n / 2, n, n, top, abort);  // warm up
        t.start();
        sampler.genBiomes(ids.data(), 4, 1000, 1000, n, n, top, abort);
        lines << QString("  biomes at 1:4: %1 per cell (%2x%2 cells; making a sampler %3)")
            .arg(us(t.nsecsElapsed() / 1e3 / (n * n))).arg(n).arg(ms(make));
    }
    if (abort && *abort)
        return lines;
    if (sampler.hasTerrain())
    {
        const int n = 32;
        std::vector<int> h(n * n);
        for (int scale : {4, 1})
        {
            t.start();
            sampler.genHeights(h.data(), scale, 2000 / scale, 2000 / scale, n, n,
                dp::HEIGHT_SURFACE, info.hasCeiling ? top / 2 : 0x7fffffff, abort);
            lines << QString("  heights at 1:%1: %2 per column (%3x%3 columns)").arg(scale)
                .arg(us(t.nsecsElapsed() / 1e3 / (n * n))).arg(n);
        }
    }
    else
        lines << QString("  heights: none (%1)").arg(info.terrainReason.isEmpty()
            ? QString("no terrain") : info.terrainReason);
    if (abort && *abort)
        return lines;
    {
        t.start();
        dp::StructureFinder finder(world, dim, seed);
        std::vector<dp::StructureStart> starts;
        finder.starts(0, 0, 0, 0, &starts, abort);
        const double first = t.nsecsElapsed() / 1e6;
        starts.clear();
        t.start();
        finder.starts(64, 64, 64 + 127, 64 + 127, &starts, abort);
        const double view = t.nsecsElapsed() / 1e6;
        int valid = 0, unknown = 0;
        for (const dp::StructureStart& s : starts)
        {
            valid += s.state == dp::StructureStart::Valid;
            unknown += s.state == dp::StructureStart::Unknown;
        }
        lines << QString("  structures: first query of a seed %1 (it searches the stronghold "
            "rings), then %2 per 2048x2048 blocks (%3 valid, %4 not decided)")
            .arg(ms(first), ms(view)).arg(valid).arg(unknown);
    }
    return lines;
}

// what a loaded world says about the given pack (or all packs)
QStringList worldFindings(const dp::World& world, const dp::Report& report, const QString& pack)
{
    QStringList lines;
    const std::vector<dp::BiomeInfo>& biomes = world.biomes();
    int packBiomes = 0;
    for (const dp::BiomeInfo& b : biomes)
        packBiomes += !b.vanilla;
    lines << QString("biomes: %1 (%2 not vanilla)").arg(biomes.size()).arg(packBiomes);
    lines << "dimensions:";
    for (const dp::DimensionInfo& d : world.dimensions())
    {
        QString line = QString("  %1: ").arg(d.id);
        if (d.supported)
        {
            line += QString("shown, %1 biomes, y %2 to %3").arg(d.biomes.size()).arg(d.minY)
                .arg(d.minY + d.height - 1);
            line += d.terrain ? ", terrain" : QString(", no terrain (%1)").arg(d.terrainReason);
            if (d.terrain && !d.aquiferReason.isEmpty())
                line += QString(", aquifers not modelled (%1)").arg(d.aquiferReason);
            if (d.terrain && !d.surfaceReason.isEmpty())
                line += QString(", surface rules not modelled (%1)").arg(d.surfaceReason);
            if (d.hasCeiling)
                line += ", ceiling";
        }
        else
            line += "not shown: " + d.reason;
        lines << line;
    }
    // structures and sets the app cannot place, by type
    QMap<QString, QStringList> types, placements;
    int total = 0, supported = 0;
    for (const dp::StructureInfo& s : world.structures())
    {
        if (!pack.isEmpty() && s.id.startsWith(QLatin1String("minecraft:")))
            continue;
        total++;
        supported += s.supported;
        if (!s.supported)
            types[s.type.isEmpty() ? QString("?") : s.type] << s.id;
    }
    for (const dp::StructureSetInfo& s : world.structureSets())
        if (!s.supported && (pack.isEmpty() || !s.id.startsWith(QLatin1String("minecraft:"))))
            placements[QString("%1: %2").arg(s.placement.isEmpty() ? QString("?") : s.placement,
                s.reason.isEmpty() ? QString("not supported") : s.reason)] << s.id;
    lines << QString("structures%1: %2, %3 supported").arg(pack.isEmpty() ? "" : " (not minecraft:)")
        .arg(total).arg(supported);
    if (!types.isEmpty())
    {
        lines << "unsupported structure types:";
        for (auto it = types.begin(); it != types.end(); ++it)
            lines << QString("  %1: %2 (%3)").arg(it.key()).arg(it.value().size()).arg(idList(it.value()));
    }
    if (!placements.isEmpty())
    {
        lines << "structure sets that are not placed (placement type: reason):";
        for (auto it = placements.begin(); it != placements.end(); ++it)
            lines << QString("  %1: %2 (%3)").arg(it.key()).arg(it.value().size()).arg(idList(it.value()));
    }
    // the load's messages, grouped by text
    struct Group { int severity; QStringList ids; int count = 0; };
    std::map<QString, Group> groups;
    for (const dp::Issue& issue : report.issues)
    {
        if (!pack.isEmpty() && issue.pack != pack)
            continue;
        Group& g = groups[issue.text];
        g.severity = int(issue.severity);
        g.count++;
        if (!issue.id.isEmpty() && !g.ids.contains(issue.id))
            g.ids << issue.id;
    }
    if (!groups.empty())
    {
        lines << QString("messages of the load (%1):").arg(report.issues.size());
        for (int severity : {int(dp::Issue::Error), int(dp::Issue::Warning), int(dp::Issue::Info)})
        {
            for (const auto& g : groups)
            {
                if (g.second.severity != severity)
                    continue;
                const char *tag = severity == dp::Issue::Error ? "ERROR"
                    : severity == dp::Issue::Warning ? "note" : "info";
                QString line = QString("  %1 %2x: %3").arg(tag).arg(g.second.count).arg(g.first);
                if (!g.second.ids.isEmpty())
                    line += QString(" [%1]").arg(idList(g.second.ids, 6));
                lines << line;
            }
        }
    }
    return lines;
}

} // namespace

QString dpCheckReport(const DpCheckInput& input, const std::atomic_bool *abort,
    const std::function<void(const QString&)>& progress)
{
    const auto aborted = [abort]() { return abort && abort->load(); };
    const auto say = [&](const QString& text) { if (progress) progress(text); };
    QStringList out;
    out << QString("Seed Atlas %1 - pack check").arg(getVersStr());
    out << QString("Minecraft %1, world preset %2, map dimension %3")
        .arg(mc2str(input.mc), input.preset, input.dimension);
    out << QString("Seed of the speed samples (delete this line if the seed is private): %1")
        .arg((qint64) input.seed);
    out << QString("Timings: one thread of %1 (load times include the vanilla data)")
        .arg(QThread::idealThreadCount());
    out << QString();

    const auto request = [&](const std::vector<QString>& packs) {
        dp::Request r;
        r.mc = input.mc;
        r.vanillaPath = input.vanillaPath;
        r.preset = input.preset;
        for (const QString& p : packs)
        {
            dp::PackSource s;
            s.path = p;
            r.packs.push_back(s);
        }
        return r;
    };

    // all packs together, as the map shows them
    say(QStringLiteral("all packs"));
    QElapsedTimer t;
    t.start();
    dp::Report allReport;
    const std::shared_ptr<const dp::World> all = dp::World::load(request(input.packs), &allReport);
    const double allLoad = t.nsecsElapsed() / 1e6;
    out << QString("== All %1 packs together ==").arg(input.packs.size());
    out << "packs in load order:";
    for (const dp::PackInfo& p : all->packs())
    {
        if (p.path == input.vanillaPath)
            continue;
        QString line = QString("  %1 (pack format %2%3, %4 worldgen files)").arg(p.name)
            .arg(p.packFormat).arg(!p.loaded ? ", not loaded by the game" : p.formatMatches ? "" : ", not declared for this version")
            .arg(p.worldgenFiles);
        if (!p.overlays.isEmpty())
            line += " overlays: " + p.overlays.join(", ");
        out << line;
    }
    out << QString("load: %1").arg(ms(allLoad));
    out << worldFindings(*all, allReport, QString());
    if (allReport.ok())
        out << speedSample(all, input.dimension, input.seed, abort);
    out << QString();
    if (aborted())
        return QString();

    // each pack alone (with the vanilla data)
    int n = 0;
    for (const QString& path : input.packs)
    {
        n++;
        const QString name = QFileInfo(path).fileName();
        say(QString("%1 (%2 of %3)").arg(name).arg(n).arg(input.packs.size()));
        out << QString("== Pack %1: %2 ==").arg(n).arg(name);
        t.start();
        dp::Report report;
        const std::shared_ptr<const dp::World> world = dp::World::load(request({path}), &report);
        const double load = t.nsecsElapsed() / 1e6;
        QStringList overlays;
        QString packName = name;
        for (const dp::PackInfo& p : world->packs())
        {
            if (p.path != path)
                continue;
            overlays = p.overlays;
            packName = p.name;
            out << QString("pack.mcmeta: pack format %1%2%3").arg(p.packFormat)
                .arg(!p.loaded ? " (not loaded by the game)" : p.formatMatches ? "" : " (not declared for this version)")
                .arg(p.description.isEmpty() ? QString() : QString(", \"%1\"")
                    .arg(p.description.simplified().left(120)));
            if (!overlays.isEmpty())
                out << "overlays applied: " + overlays.join(", ");
        }
        const FileScan scan = scanPack(path, overlays);
        if (scan.unreadable < 0)
            out << "files: the pack could not be read";
        else
        {
            out << "worldgen files per registry:";
            for (auto it = scan.registries.begin(); it != scan.registries.end(); ++it)
                out << QString("  %1: %2").arg(it.key()).arg(it.value());
            if (scan.registries.isEmpty())
                out << "  none";
            if (scan.unreadable > 0)
                out << QString("  %1 files are not valid JSON").arg(scan.unreadable);
            if (!scan.dimensions.isEmpty())
            {
                out << "dimension files:";
                for (const QString& d : scan.dimensions)
                    out << "  " + d;
            }
            if (!scan.presets.isEmpty())
            {
                out << "world presets:";
                for (const QString& d : scan.presets)
                    out << "  " + d;
            }
            if (!scan.customTypes.isEmpty())
            {
                out << "types not of Minecraft in density functions, noise settings, dimensions "
                    "and presets (density functions, biome sources, generators of mods):";
                for (auto it = scan.customTypes.begin(); it != scan.customTypes.end(); ++it)
                    out << QString("  %1: %2 files (%3)").arg(it.key()).arg(it.value().size())
                        .arg(idList(it.value(), 5));
            }
        }
        out << QString("load alone: %1").arg(ms(load));
        out << worldFindings(*world, report, packName);
        if (report.ok())
            out << speedSample(world, input.dimension, input.seed, abort);
        out << QString();
        if (aborted())
            return QString();
    }
    // the load's messages may name the files with their folders (e.g. a pack
    // that was moved while the check ran)
    QStringList known(input.packs.begin(), input.packs.end());
    known << input.vanillaPath;
    return dpScrubPaths(out.join("\n") + "\n", known);
}

QString dpScrubPaths(const QString& text, const QStringList& paths)
{
    struct Sub
    {
        QString from, to;
    };
    std::vector<Sub> subs;
    const auto add = [&subs](const QString& from, const QString& to) {
        // (an empty or one-character pattern would match everywhere)
        if (from.size() > 1)
            subs.push_back({from, to});
    };
    const auto forms = [](const QString& path) {
        QStringList out;
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
        out << path << clean << QDir::toNativeSeparators(clean);
        const QString canonical = QFileInfo(clean).canonicalFilePath();
        if (!canonical.isEmpty())
            out << canonical << QDir::toNativeSeparators(canonical);
        out.removeDuplicates();
        return out;
    };
    for (const QString& path : paths)
    {
        if (path.trimmed().isEmpty())
            continue;
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
        QString name = QFileInfo(clean).fileName();
        if (name.isEmpty())
            name = QDir(clean).dirName();
        for (const QString& form : forms(path))
            add(form, name);
        // the folder it is in (also when the file itself is gone): other
        // files of that folder lose the folder as well
        const QString folder = QFileInfo(clean).absolutePath();
        if (folder != QLatin1String("/"))
            for (const QString& form : forms(folder))
            {
                add(form + QLatin1Char('/'), QString());
                add(form + QDir::separator(), QString());
            }
    }
    for (const QString& form : forms(QDir::homePath()))
        if (form != QLatin1String("/"))
            add(form, QStringLiteral("~"));
    // longest first, so that a folder does not cut into a longer known path
    std::stable_sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) {
        return a.from.size() > b.from.size();
    });
    QString out = text;
    for (const Sub& sub : subs)
        out.replace(sub.from, sub.to);

    // any other absolute path in quotes: only its last part
    static const QRegularExpression quoted(
        QStringLiteral("\"((?:[A-Za-z]:[\\\\/]|\\\\\\\\|/|~/)[^\"\n]*)\""));
    QString result;
    qsizetype at = 0;
    QRegularExpressionMatchIterator it = quoted.globalMatch(out);
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        const QStringList parts = m.captured(1).split(QRegularExpression(QStringLiteral("[\\\\/]")),
            Qt::SkipEmptyParts);
        result += out.mid(at, m.capturedStart() - at);
        result += QLatin1Char('"') + (parts.isEmpty() ? QString() : parts.last()) + QLatin1Char('"');
        at = m.capturedEnd();
    }
    result += out.mid(at);
    return result;
}
