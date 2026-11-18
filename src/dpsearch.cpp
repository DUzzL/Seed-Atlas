#include "dpsearch.h"

#include "datapackworld.h"
#include "dpmap.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace {

const char *const kindNames[] = { "", "biome_in_area", "biome_at", "biome_share",
    "structure_in_area" };
const int KIND_COUNT = 5;
const char *const matchNames[] = { "any", "all", "none" };
const char *const shapeNames[] = { "circle", "square", "rect" };

// the largest area a condition may sample, in cells
constexpr int64_t MAX_CELLS = int64_t(4096) * 4096;
// the largest side of the area of a structure condition, in blocks
constexpr int64_t MAX_STRUCTURE_SIDE = 65536;
// tiles of the area, in cells: the unit of early exits
constexpr int TILE = 16;

int floorDiv(int a, int b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

QString tr(const char *text, const char *disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("DpSearch", text, disambiguation, n);
}

int findName(const char *const *names, int n, const QString& s)
{
    for (int i = 0; i < n; i++)
        if (s == QLatin1String(names[i]))
            return i;
    return -1;
}

} // namespace

// ---------------------------------------------------------------- condition

void DpCondition::bounds(int *bx1, int *bz1, int *bx2, int *bz2) const
{
    if (kind == BIOME_AT)
    {
        *bx1 = *bx2 = x;
        *bz1 = *bz2 = z;
    }
    else if (shape == RECT)
    {
        *bx1 = std::min(x1, x2);
        *bx2 = std::max(x1, x2);
        *bz1 = std::min(z1, z2);
        *bz2 = std::max(z1, z2);
    }
    else
    {
        const int64_t rr = std::max(0, r);
        *bx1 = int(std::max<int64_t>(INT_MIN, int64_t(x) - rr));
        *bx2 = int(std::min<int64_t>(INT_MAX, int64_t(x) + rr));
        *bz1 = int(std::max<int64_t>(INT_MIN, int64_t(z) - rr));
        *bz2 = int(std::min<int64_t>(INT_MAX, int64_t(z) + rr));
    }
}

QString DpCondition::toLine() const
{
    if (kind == UNKNOWN && !raw.isEmpty())
    {   // as it came, with the switch and the id of this list
        QJsonObject o = raw;
        o["id"] = id;
        o["on"] = enabled;
        return "#DpCond: " + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    QJsonObject o;
    o["id"] = id;
    o["on"] = enabled;
    o["kind"] = kindNames[kind];
    o["match"] = matchNames[match];
    if (kind == STRUCTURE_IN_AREA)
    {
        o["structures"] = QJsonArray::fromStringList(structures);
        o["count"] = count;
        o["states"] = unknownCounts ? "valid_or_unknown" : "valid";
    }
    else
        o["biomes"] = QJsonArray::fromStringList(biomes);
    if (kind == BIOME_AT)
    {
        o["x"] = x;
        o["z"] = z;
    }
    else
    {
        o["area"] = shapeNames[shape];
        if (shape == RECT)
        {
            o["x1"] = x1;
            o["z1"] = z1;
            o["x2"] = x2;
            o["z2"] = z2;
        }
        else
        {
            o["x"] = x;
            o["z"] = z;
            o["r"] = r;
        }
        if (kind != STRUCTURE_IN_AREA)
            o["scale"] = scale;
    }
    if (kind == BIOME_SHARE)
        o["percent"] = percent;
    if (kind == STRUCTURE_IN_AREA)
        ; // structures start where they start, at no level
    else if (y == Y_TOP)
        o["y"] = "top";
    else
        o["y"] = y;
    return "#DpCond: " + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

bool DpCondition::fromLine(const QString& line, DpCondition *out)
{
    QString text = line.trimmed();
    if (text.startsWith(QLatin1String("#DpCond:")))
        text = text.mid(8).trimmed();
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    DpCondition c;
    c.id = o.value("id").toInt(0);
    if (c.id < 1 || c.id > 99)
        return false;
    c.enabled = o.value("on").toBool(true);
    const int kind = findName(kindNames, KIND_COUNT, o.value("kind").toString());
    if (kind <= 0)
    {   // a kind of a newer version: kept as it is, never tested
        c.kind = UNKNOWN;
        c.enabled = false;
        c.raw = o;
        *out = c;
        return true;
    }
    c.kind = Kind(kind);
    c.match = Match(std::max(0, findName(matchNames, 3, o.value("match").toString("any"))));
    for (const QJsonValue& v : o.value("biomes").toArray())
        if (!v.toString().isEmpty())
            c.biomes.append(v.toString());
    for (const QJsonValue& v : o.value("structures").toArray())
        if (!v.toString().isEmpty())
            c.structures.append(v.toString());
    c.count = std::clamp(o.value("count").toInt(1), 1, 9999);
    c.unknownCounts = o.value("states").toString() == QLatin1String("valid_or_unknown");
    c.shape = Shape(std::max(0, findName(shapeNames, 3, o.value("area").toString("circle"))));
    c.x = o.value("x").toInt(0);
    c.z = o.value("z").toInt(0);
    c.r = std::max(0, o.value("r").toInt(500));
    c.x1 = o.value("x1").toInt(-500);
    c.z1 = o.value("z1").toInt(-500);
    c.x2 = o.value("x2").toInt(500);
    c.z2 = o.value("z2").toInt(500);
    const QJsonValue y = o.value("y");
    c.y = y.isDouble() ? y.toInt() : Y_TOP;
    c.scale = o.value("scale").toInt(4);
    if (c.scale != 4 && c.scale != 16 && c.scale != 64)
        c.scale = 4;
    c.percent = std::clamp(o.value("percent").toInt(50), 0, 100);
    if (c.kind == BIOME_AT && c.match == ALL)
        c.match = ANY;
    *out = c;
    return true;
}

bool DpCondition::operator==(const DpCondition& o) const
{
    return toLine() == o.toLine();
}

// ---------------------------------------------------------------- texts

int DpBiomeNames::index(const QString& id) const
{
    if (!world || !world->usable())
        return -1;
    const std::vector<dp::BiomeInfo>& list = world->world->biomes();
    for (int i = 0; i < (int) list.size(); i++)
        if (list[i].id == id)
            return i;
    return -1;
}

bool DpBiomeNames::has(const QString& id) const
{
    return index(id) >= 0;
}

int DpBiomeNames::structureIndex(const QString& id) const
{
    if (!world || !world->usable())
        return -1;
    const std::vector<dp::StructureInfo>& list = world->world->structures();
    for (int i = 0; i < (int) list.size(); i++)
        if (list[i].id == id)
            return i;
    return -1;
}

QString DpBiomeNames::structureName(const QString& id) const
{
    const int i = structureIndex(id);
    if (i < 0)
        return id;
    const std::shared_ptr<const DpStructureCatalog> catalog = dpStructureCatalog(world);
    if (catalog && i < (int) catalog->entries.size())
        return catalog->entries[(size_t) i].label;
    return world->world->structures()[(size_t) i].name;
}

QString DpBiomeNames::name(const QString& id) const
{
    const int i = index(id);
    if (i >= 0 && i < (int) world->labels.size())
        return world->labels[i];
    return id;
}

QString dpKindName(DpCondition::Kind kind)
{
    switch (kind)
    {
    case DpCondition::BIOME_IN_AREA:    return tr("Biome in an area");
    case DpCondition::BIOME_AT:         return tr("Biome at a position");
    case DpCondition::BIOME_SHARE:      return tr("Biome share of an area");
    case DpCondition::STRUCTURE_IN_AREA: return tr("Structure in an area");
    default:                            return tr("Unknown condition");
    }
}

QString dpConditionText(const DpCondition& c, const DpBiomeNames& names)
{
    if (c.kind == DpCondition::UNKNOWN)
        return tr("A condition of a newer version of Seed Atlas (%1); it is not tested")
            .arg(c.raw.value("kind").toString());

    // the biome (or structure) names, at most three of them
    const QStringList& ids = c.isStructure() ? c.structures : c.biomes;
    QStringList shown;
    for (int i = 0; i < ids.size() && i < 3; i++)
        shown.append(c.isStructure() ? names.structureName(ids[i]) : names.name(ids[i]));
    const QString joiner = c.match == DpCondition::ALL ? tr(" and ") : tr(" or ");
    QString list;
    if (shown.isEmpty())
        list = c.isStructure() ? tr("(no structure)") : tr("(no biome)");
    else if (ids.size() > 3)
        list = shown.join(", ") + tr(" + %n more", nullptr, int(ids.size() - 3));
    else if (shown.size() == 1)
        list = shown.front();
    else
        list = QStringList(shown.mid(0, shown.size() - 1)).join(", ") + joiner + shown.back();

    QString area;
    if (c.shape == DpCondition::RECT)
        area = tr("%1, %2 to %3, %4").arg(c.x1).arg(c.z1).arg(c.x2).arg(c.z2);
    else if (c.shape == DpCondition::SQUARE)
        area = tr("±%1 blocks around %2, %3").arg(c.r).arg(c.x).arg(c.z);
    else
        area = tr("%1 blocks of %2, %3").arg(c.r).arg(c.x).arg(c.z);

    QString text;
    switch (c.kind)
    {
    case DpCondition::BIOME_IN_AREA:
    {
        const QString where = c.shape == DpCondition::CIRCLE ? tr("within %1").arg(area)
            : c.shape == DpCondition::SQUARE ? tr("within %1").arg(area) : tr("in %1").arg(area);
        text = c.match == DpCondition::NONE ? tr("No %1 %2").arg(list, where)
            : tr("%1 %2").arg(list, where);
        break;
    }
    case DpCondition::BIOME_AT:
        text = c.match == DpCondition::NONE ? tr("Not %1 at %2, %3").arg(list).arg(c.x).arg(c.z)
            : tr("%1 at %2, %3").arg(list).arg(c.x).arg(c.z);
        break;
    case DpCondition::BIOME_SHARE:
    {
        const QString of = c.shape == DpCondition::RECT ? tr("the area %1").arg(area)
            : c.shape == DpCondition::SQUARE ? area
            : tr("the area within %1").arg(area);
        if (c.match == DpCondition::ALL)
            text = tr("%1 each cover at least %2% of %3").arg(list).arg(c.percent).arg(of);
        else if (c.match == DpCondition::NONE)
            text = tr("%1 cover at most %2% of %3").arg(list).arg(c.percent).arg(of);
        else
            text = tr("%1 cover at least %2% of %3").arg(list).arg(c.percent).arg(of);
        break;
    }
    case DpCondition::STRUCTURE_IN_AREA:
    {
        const QString where = c.shape == DpCondition::RECT ? tr("in %1").arg(area)
            : tr("within %1").arg(area);
        if (c.match == DpCondition::NONE)
            text = tr("No %1 %2").arg(list, where);
        else if (c.count > 1 && c.match == DpCondition::ALL)
            text = tr("%1 at least %2 times each %3").arg(list).arg(c.count).arg(where);
        else if (c.count > 1)
            text = tr("%1 at least %2 times %3").arg(list).arg(c.count).arg(where);
        else
            text = tr("%1 %2").arg(list, where);
        break;
    }
    default:
        break;
    }
    QStringList details;
    if (c.isStructure())
    {
        if (c.match == DpCondition::NONE)
            details << tr("not even undecided starts");
        else if (c.unknownCounts)
            details << tr("also starts not decided");
    }
    else if (c.y != DpCondition::Y_TOP)
        details << tr("Y %1").arg(c.y);
    if (c.hasArea() && !c.isStructure() && c.scale != 4)
        details << tr("sampled 1:%1").arg(c.scale);
    if (!details.isEmpty())
        text += QString::fromUtf8(" · ") + details.join(QString::fromUtf8(" · "));
    return text;
}

// ---------------------------------------------------------------- search

std::shared_ptr<const DpSearch> DpSearch::compile(
    const std::shared_ptr<const DatapackWorld>& world,
    const std::vector<DpCondition>& conditions, QString *error, QString *warning)
{
    error->clear();
    if (warning)
        warning->clear();
    if (!world || !world->usable())
    {
        *error = world ? tr("The datapack world cannot be searched: %1")
                .arg(world->shortText())
            : tr("No datapack world is loaded.");
        return nullptr;
    }
    std::shared_ptr<DpSearch> s(new DpSearch);
    s->dpworld = world;
    s->nbiomes = (int) world->world->biomes().size();
    const std::vector<dp::BiomeInfo>& biomes = world->world->biomes();
    const DpBiomeNames names{world};
    QStringList missing;
    QSet<int> ids;
    for (const DpCondition& c : conditions)
    {
        const QString cid = QString("#%1").arg(c.id);
        if (c.id < 1 || c.id > 99 || ids.contains(c.id))
        {
            *error = tr("Condition %1 has an id that is not valid or used twice.").arg(cid);
            return nullptr;
        }
        ids.insert(c.id);
        if (!c.enabled || c.kind == DpCondition::UNKNOWN)
            continue;
        Compiled k;
        k.c = c;
        if (c.isStructure())
        {   // the structures, by index of the world
            const std::vector<dp::StructureInfo>& structures = world->world->structures();
            k.mask.assign(structures.size(), 0);
            k.slot.assign(structures.size(), -1);
            for (const QString& id : c.structures)
            {
                const int i = names.structureIndex(id);
                if (i < 0)
                {
                    if (!missing.contains(id))
                        missing.append(id);
                    continue;
                }
                if (!structures[(size_t) i].supported)
                {
                    *error = tr("Condition %1: %2 cannot be searched. %3").arg(cid,
                        names.structureName(id), structures[(size_t) i].reason);
                    return nullptr;
                }
                if (!k.mask[(size_t) i])
                {
                    k.slot[(size_t) i] = (int) k.listed.size();
                    k.listed.push_back(i);
                }
                k.mask[(size_t) i] = 1;
            }
            if (c.structures.isEmpty())
            {
                *error = tr("Condition %1 lists no structure.").arg(cid);
                return nullptr;
            }
            if (k.listed.empty())
            {
                *error = tr("Condition %1: none of its structures is in the loaded datapacks (%2).")
                    .arg(cid, c.structures.join(", "));
                return nullptr;
            }
            int bx1, bz1, bx2, bz2;
            c.bounds(&bx1, &bz1, &bx2, &bz2);
            if (int64_t(bx2) - bx1 >= MAX_STRUCTURE_SIDE || int64_t(bz2) - bz1 >= MAX_STRUCTURE_SIDE)
            {
                *error = tr("Condition %1 covers too large an area for structures (at most %2 "
                    "blocks on a side).").arg(cid).arg(MAX_STRUCTURE_SIDE);
                return nullptr;
            }
            // Structures are tested last: they cost more than any biome
            // condition (a seed's first query of a concentric_rings set
            // searches its stronghold rings).
            k.cost = 1e18 + double(int64_t(bx2) - bx1 + 1) * double(int64_t(bz2) - bz1 + 1);
            s->conds.push_back(k);
            continue;
        }
        k.mask.assign(biomes.size(), 0);
        for (const QString& id : c.biomes)
        {
            const int i = names.index(id);
            if (i < 0)
            {
                if (!missing.contains(id))
                    missing.append(id);
                continue;
            }
            if (!k.mask[i])
                k.listed.push_back(i);
            k.mask[i] = 1;
        }
        if (c.biomes.isEmpty())
        {
            *error = tr("Condition %1 lists no biome.").arg(cid);
            return nullptr;
        }
        if (k.listed.empty())
        {
            *error = tr("Condition %1: none of its biomes is in the loaded datapacks (%2).")
                .arg(cid, c.biomes.join(", "));
            return nullptr;
        }
        const int low = world->minY(), high = world->maxY();
        k.y = c.y == DpCondition::Y_TOP ? high : std::clamp(c.y, low, high);
        if (c.kind == DpCondition::BIOME_AT)
        {
            k.cost = 1;
        }
        else
        {
            int bx1, bz1, bx2, bz2;
            c.bounds(&bx1, &bz1, &bx2, &bz2);
            const int sc = c.scale;
            k.cx1 = floorDiv(bx1, sc);
            k.cz1 = floorDiv(bz1, sc);
            k.cx2 = floorDiv(bx2, sc);
            k.cz2 = floorDiv(bz2, sc);
            const int64_t w = int64_t(k.cx2) - k.cx1 + 1, h = int64_t(k.cz2) - k.cz1 + 1;
            if (w * h > MAX_CELLS)
            {
                *error = tr("Condition %1 covers too large an area for 1:%2. Use a smaller area "
                    "or a coarser sampling.").arg(cid).arg(sc);
                return nullptr;
            }
            for (int j = k.cz1; j <= k.cz2; j++)
                for (int i = k.cx1; i <= k.cx2; i++)
                    k.cells += s->cellInside(k, i, j);
            if (k.cells == 0)
            {
                *error = tr("Condition %1 has an empty area.").arg(cid);
                return nullptr;
            }
            k.cost = double(k.cells);
            // the tiles of the area, nearest to its centre first
            const double mx = (k.cx1 + int64_t(k.cx2) + 1) * 0.5;
            const double mz = (k.cz1 + int64_t(k.cz2) + 1) * 0.5;
            std::vector<std::pair<double, Tile>> order;
            for (int64_t tz = k.cz1; tz <= k.cz2; tz += TILE)
                for (int64_t tx = k.cx1; tx <= k.cx2; tx += TILE)
                {
                    Tile t;
                    t.x = int(tx);
                    t.z = int(tz);
                    t.w = int(std::min<int64_t>(TILE, int64_t(k.cx2) - tx + 1));
                    t.h = int(std::min<int64_t>(TILE, int64_t(k.cz2) - tz + 1));
                    bool any = false;
                    for (int j = 0; j < t.h && !any; j++)
                        for (int i = 0; i < t.w && !any; i++)
                            any = s->cellInside(k, t.x + i, t.z + j);
                    if (!any)
                        continue; // outside of the circle
                    const double dx = tx + t.w * 0.5 - mx, dz = tz + t.h * 0.5 - mz;
                    order.push_back({dx * dx + dz * dz, t});
                }
            std::stable_sort(order.begin(), order.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& o : order)
                k.tiles.push_back(o.second);
            k.slot.assign(biomes.size(), -1);
            for (int i = 0; i < (int) k.listed.size(); i++)
                k.slot[k.listed[i]] = i;
        }
        s->conds.push_back(k);
    }
    if (s->conds.empty())
    {
        *error = tr("Add at least one datapack condition (or switch one on) first.");
        return nullptr;
    }
    // AND: the cheap tests first, so that most seeds fail early
    std::stable_sort(s->conds.begin(), s->conds.end(),
        [](const Compiled& a, const Compiled& b) { return a.cost < b.cost; });
    if (warning && !missing.isEmpty())
        *warning = tr("Not in the loaded datapacks, left out: %1").arg(missing.join(", "));
    return s;
}

bool DpSearch::cellInside(const Compiled& k, int i, int j) const
{
    if (k.c.shape != DpCondition::CIRCLE)
        return true;
    // the centre of the cell
    const double s = k.c.scale;
    const double dx = (i + 0.5) * s - (k.c.x + 0.5);
    const double dz = (j + 0.5) * s - (k.c.z + 0.5);
    const double r = k.c.r;
    return dx * dx + dz * dz <= r * r;
}

DpSearch::Result DpSearch::test(uint64_t seed, const std::atomic_bool *abort) const
{
    dp::Sampler *sampler = datapackSampler(dpworld, seed);
    if (!sampler)
        return FAILED;
    // one finder per seed, made when the first structure condition is reached
    std::unique_ptr<dp::StructureFinder> finder;
    for (const Compiled& k : conds)
    {
        if (abort && abort->load(std::memory_order_relaxed))
            return ABORTED;
        Result r;
        if (k.c.isStructure())
        {
            if (!finder)
                finder.reset(new dp::StructureFinder(dpworld->world, dpworld->dimension, seed));
            r = testStructures(k, finder.get(), abort);
        }
        else if (k.c.kind == DpCondition::BIOME_AT)
        {
            const int id = sampler->biomeAt(1, k.c.x, k.y, k.c.z);
            const bool listed = id >= 0 && id < nbiomes && k.mask[id];
            r = listed == (k.c.match != DpCondition::NONE) ? OK : FAILED;
        }
        else
        {
            r = testArea(k, sampler, abort);
        }
        if (r != OK)
            return r;
    }
    return OK;
}

// Whether a structure start position (in blocks) lies in the area of a
// structure condition.
static bool dpStartInShape(const DpCondition& c, int x, int z)
{
    int bx1, bz1, bx2, bz2;
    c.bounds(&bx1, &bz1, &bx2, &bz2);
    if (x < bx1 || x > bx2 || z < bz1 || z > bz2)
        return false;
    if (c.shape != DpCondition::CIRCLE)
        return true;
    const int64_t dx = int64_t(x) - c.x, dz = int64_t(z) - c.z;
    return dx * dx + dz * dz <= int64_t(c.r) * c.r;
}

DpSearch::Result DpSearch::testStructures(const Compiled& k, void *finderPtr,
    const std::atomic_bool *abort) const
{
    dp::StructureFinder *finder = static_cast<dp::StructureFinder*>(finderPtr);
    const DpCondition& c = k.c;
    int bx1, bz1, bx2, bz2;
    c.bounds(&bx1, &bz1, &bx2, &bz2);
    std::vector<dp::StructureStart> starts;
    if (!dpStartsInArea(finder, bx1, bz1, bx2, bz2, &k.mask, false, &starts, abort))
        return abort && abort->load(std::memory_order_relaxed) ? ABORTED : FAILED;
    // start positions (a set's chunk): several structures that a set may try
    // in one chunk are alternatives, not separate starts
    const auto key = [](const dp::StructureStart& s) {
        return std::make_tuple(s.set, s.chunkX, s.chunkZ);
    };
    std::set<std::tuple<int, int, int>> together;
    std::vector<std::set<std::tuple<int, int, int>>> each(k.listed.size());
    for (const dp::StructureStart& s : starts)
    {
        // "none" is only sure where no start at all may be there: a start
        // that is not decided counts for it, whatever the option says
        const bool counts = s.state == dp::StructureStart::Valid
            || ((c.unknownCounts || c.match == DpCondition::NONE)
                && s.state == dp::StructureStart::Unknown);
        if (!counts || !dpStartInShape(c, s.x, s.z))
            continue;
        together.insert(key(s));
        const int n = k.slot[(size_t) s.structure];
        if (n >= 0)
            each[(size_t) n].insert(key(s));
    }
    switch (c.match)
    {
    case DpCondition::NONE:
        return together.empty() ? OK : FAILED;
    case DpCondition::ALL:
        for (const auto& e : each)
            if ((int) e.size() < c.count)
                return FAILED;
        return OK;
    default:
        return (int) together.size() >= c.count ? OK : FAILED;
    }
}

DpSearch::Result DpSearch::testArea(const Compiled& k, void *samplerPtr,
    const std::atomic_bool *abort) const
{
    dp::Sampler *sampler = static_cast<dp::Sampler*>(samplerPtr);
    const DpCondition& c = k.c;
    const bool share = c.kind == DpCondition::BIOME_SHARE;
    const int64_t total = k.cells;
    // share: the cells needed (at least) or allowed (at most)
    const int64_t need = (int64_t(c.percent) * total + 99) / 100;
    const int64_t allowed = int64_t(c.percent) * total / 100;
    std::vector<int64_t> counts(k.listed.size(), 0);
    const std::vector<int>& slot = k.slot;
    int64_t together = 0, seen = 0;
    int found = 0;

    std::vector<int> ids(TILE * TILE);
    for (const Tile& t : k.tiles)
    {
        if (!sampler->genBiomes(ids.data(), c.scale, t.x, t.z, t.w, t.h, k.y, abort))
            return abort && abort->load(std::memory_order_relaxed) ? ABORTED : FAILED;
        for (int j = 0; j < t.h; j++)
        {
            for (int i = 0; i < t.w; i++)
            {
                if (!cellInside(k, t.x + i, t.z + j))
                    continue;
                seen++;
                const int id = ids[j * t.w + i];
                if (id < 0 || id >= nbiomes || slot[id] < 0)
                    continue;
                const int n = slot[id];
                together++;
                if (counts[n]++ == 0)
                    found++;
                if (!share)
                {
                    if (c.match == DpCondition::ANY)
                        return OK;
                    if (c.match == DpCondition::NONE)
                        return FAILED;
                    if (found == (int) k.listed.size())
                        return OK;
                }
            }
        }
        if (share)
        {
            const int64_t remaining = total - seen;
            if (c.match == DpCondition::ANY)
            {
                if (together >= need)
                    return OK;
                if (together + remaining < need)
                    return FAILED;
            }
            else if (c.match == DpCondition::ALL)
            {
                bool all = true;
                for (int64_t n : counts)
                {
                    if (n + remaining < need)
                        return FAILED;
                    all = all && n >= need;
                }
                if (all)
                    return OK;
            }
            else
            {
                if (together > allowed)
                    return FAILED;
                if (together + remaining <= allowed)
                    return OK;
            }
        }
    }
    if (!share)
        return c.match == DpCondition::NONE ? OK : FAILED;
    if (c.match == DpCondition::ANY)
        return together >= need ? OK : FAILED;
    if (c.match == DpCondition::NONE)
        return together <= allowed ? OK : FAILED;
    for (int64_t n : counts)
        if (n < need)
            return FAILED;
    return OK;
}

// ---------------------------------------------------------------- analysis

bool dpGenBiomes(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed,
    int *out, int scale, int x, int z, int w, int h, int y, const std::atomic_bool *abort)
{
    dp::Sampler *sampler = datapackSampler(world, seed);
    return sampler && sampler->genBiomes(out, scale, x, z, w, h, y, abort);
}

int dpBiomeAt(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed,
    int scale, int x, int y, int z)
{
    dp::Sampler *sampler = datapackSampler(world, seed);
    return sampler ? sampler->biomeAt(scale, x, y, z) : -1;
}

int dpBiomeCount(const std::shared_ptr<const DatapackWorld>& world)
{
    return world && world->usable() ? (int) world->world->biomes().size() : 0;
}
