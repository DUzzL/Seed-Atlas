#include "conditiontext.h"

#include "scripts.h"
#include "util.h"

#include <QApplication>
#include <QFileInfo>
#include <QMap>
#include <QStringList>

namespace {

#define TXT(s) QT_TRANSLATE_NOOP("FilterText", s)

struct TypeText
{
    int type;
    int group;
    const char *name;   // display name
    const char *noun;   // counted noun, singular (structure-like types)
    const char *plural; // counted noun, plural
    const char *brief;  // one plain sentence
};

// The names say what the user gets; each brief is derived from the technical
// description in g_filterinfo and from the code in search.cpp.
const TypeText g_typeText[] = {
    // Overworld structures
    { F_VILLAGE,         TG_STRUCT, TXT("Village"), TXT("village"), TXT("villages"), nullptr },
    { F_PORTAL,          TG_STRUCT, TXT("Ruined portal (Overworld)"), TXT("ruined portal"), TXT("ruined portals"), nullptr },
    { F_SHIPWRECK,       TG_STRUCT, TXT("Shipwreck"), TXT("shipwreck"), TXT("shipwrecks"), nullptr },
    { F_TREASURE,        TG_STRUCT, TXT("Buried treasure"), TXT("buried treasure"), TXT("buried treasures"),
      TXT("Buried treasure lies near the middle of a chunk, so the area should include chunk centers.") },
    { F_OUTPOST,         TG_STRUCT, TXT("Pillager outpost"), TXT("pillager outpost"), TXT("pillager outposts"), nullptr },
    { F_DESERT,          TG_STRUCT, TXT("Desert pyramid"), TXT("desert pyramid"), TXT("desert pyramids"),
      TXT("In 1.18 and later, it can fail to generate near caves, rivers and oceans.") },
    { F_JUNGLE,          TG_STRUCT, TXT("Jungle temple"), TXT("jungle temple"), TXT("jungle temples"),
      TXT("In 1.18 and later, it can fail to generate near caves, rivers and oceans.") },
    { F_HUT,             TG_STRUCT, TXT("Swamp hut"), TXT("swamp hut"), TXT("swamp huts"), nullptr },
    { F_IGLOO,           TG_STRUCT, TXT("Igloo"), TXT("igloo"), TXT("igloos"), nullptr },
    { F_MONUMENT,        TG_STRUCT, TXT("Ocean monument"), TXT("ocean monument"), TXT("ocean monuments"), nullptr },
    { F_MANSION,         TG_STRUCT, TXT("Woodland mansion"), TXT("woodland mansion"), TXT("woodland mansions"),
      TXT("In 1.18 and later, it can fail to generate near caves, rivers and oceans.") },
    { F_RUINS,           TG_STRUCT, TXT("Ocean ruin"), TXT("ocean ruin"), TXT("ocean ruins"), nullptr },
    { F_WELL,            TG_STRUCT, TXT("Desert well"), TXT("desert well"), TXT("desert wells"), nullptr },
    { F_MINESHAFT,       TG_STRUCT, TXT("Abandoned mineshaft"), TXT("abandoned mineshaft"), TXT("abandoned mineshafts"), nullptr },
    { F_ANCIENT_CITY,    TG_STRUCT, TXT("Ancient city"), TXT("ancient city"), TXT("ancient cities"), nullptr },
    { F_TRAILS,          TG_STRUCT, TXT("Trail ruins"), TXT("trail ruins"), TXT("trail ruins"), nullptr },
    { F_CHAMBERS,        TG_STRUCT, TXT("Trial chambers"), TXT("trial chambers"), TXT("trial chambers"), nullptr },
    { F_CAMP,            TG_STRUCT, TXT("Abandoned camp"), TXT("abandoned camp"), TXT("abandoned camps"), nullptr },
    { F_STRONGHOLD,      TG_STRUCT, TXT("Stronghold"), TXT("stronghold"), TXT("strongholds"), nullptr },
    { F_FIRST_STRONGHOLD,TG_STRUCT, TXT("First stronghold (rough position)"), nullptr, nullptr,
      TXT("The position of the first stronghold to within about 112 blocks. Much faster to check than \"Stronghold\".") },
    // Nether structures
    { F_FORTRESS,        TG_STRUCT_NETHER, TXT("Nether fortress"), TXT("Nether fortress"), TXT("Nether fortresses"), nullptr },
    { F_BASTION,         TG_STRUCT_NETHER, TXT("Bastion remnant"), TXT("bastion remnant"), TXT("bastion remnants"), nullptr },
    { F_PORTALN,         TG_STRUCT_NETHER, TXT("Ruined portal (Nether)"), TXT("Nether ruined portal"), TXT("Nether ruined portals"), nullptr },
    // End structures
    { F_ENDCITY,         TG_STRUCT_END, TXT("End city"), TXT("End city"), TXT("End cities"), nullptr },
    { F_ENDCITYSHIP,     TG_STRUCT_END, TXT("End city with ship"), TXT("End city with a ship"), TXT("End cities with a ship"), nullptr },
    { F_GATEWAY,         TG_STRUCT_END, TXT("End gateway"), TXT("End gateway"), TXT("End gateways"),
      TXT("Only the gateways scattered in the outer End, not those that appear when the dragon is defeated.") },
    // biomes
    { F_BIOME,           TG_BIOMES, TXT("Biomes in an area (Overworld)"), nullptr, nullptr,
      TXT("The area must contain the biomes you require and none of the biomes you exclude.") },
    { F_BIOME_SAMPLE,    TG_BIOMES, TXT("Biome coverage of an area"), nullptr, nullptr,
      TXT("A share of the area, e.g. 50%, must be covered by the biomes you choose. Checked by sampling.") },
    { F_BIOME_CENTER,    TG_BIOMES, TXT("Biome patch of a minimum size"), nullptr, nullptr,
      TXT("Patches of one biome that are at least a given size; the position is their center.") },
    { F_BIOME_NETHER,    TG_BIOMES, TXT("Biomes in an area (Nether)"), nullptr, nullptr,
      TXT("The area must contain the Nether biomes you require and none of those you exclude.") },
    { F_BIOME_END,       TG_BIOMES, TXT("Biomes in an area (End)"), nullptr, nullptr,
      TXT("The area must contain the End biomes you require and none of those you exclude.") },
    // world
    { F_SPAWN,           TG_WORLD, TXT("Spawn point"), nullptr, nullptr,
      TXT("Where players first appear in the world.") },
    { F_SLIME,           TG_WORLD, TXT("Slime chunk"), TXT("slime chunk"), TXT("slime chunks"),
      TXT("Chunks in which slimes can spawn underground.") },
    { F_HEIGHT,          TG_WORLD, TXT("Surface height"), nullptr, nullptr,
      TXT("The approximate height of the ground at one position.") },
    // quad structures
    { F_QH_IDEAL,        TG_QUAD, TXT("Quad swamp hut (ideal)"), nullptr, nullptr,
      TXT("Four swamp huts that one AFK spot can cover, in one of the best layouts.") },
    { F_QH_CLASSIC,      TG_QUAD, TXT("Quad swamp hut (classic)"), nullptr, nullptr,
      TXT("Four swamp huts that one AFK spot can cover, in the corners where four regions meet.") },
    { F_QH_NORMAL,       TG_QUAD, TXT("Quad swamp hut (normal)"), nullptr, nullptr,
      TXT("Four swamp huts within 128 blocks of one AFK spot, with room for a fall chute.") },
    { F_QH_BARELY,       TG_QUAD, TXT("Quad swamp hut (barely)"), nullptr, nullptr,
      TXT("Four swamp huts in any layout whose bounding boxes just fit within 128 blocks of one AFK spot.") },
    { F_QM_95,           TG_QUAD, TXT("Quad ocean monument (95%+)"), nullptr, nullptr,
      TXT("Four ocean monuments with at least 95% of their area within 128 blocks of one AFK spot.") },
    { F_QM_90,           TG_QUAD, TXT("Quad ocean monument (90%+)"), nullptr, nullptr,
      TXT("Four ocean monuments with at least 90% of their area within 128 blocks of one AFK spot.") },
    // expert: helpers
    { F_LOGIC_OR,        TG_EXPERT, TXT("Either-or group (OR)"), nullptr, nullptr,
      TXT("Passes when at least one of the conditions measured from it passes.") },
    { F_LOGIC_NOT,       TG_EXPERT, TXT("Exclusion group (NOT)"), nullptr, nullptr,
      TXT("Passes when none of the conditions measured from it pass.") },
    { F_LUA,             TG_EXPERT, TXT("Lua script"), nullptr, nullptr,
      TXT("Your own check, written as a Lua script.") },
    { F_SCALE_TO_NETHER, TG_EXPERT, TXT("Convert to Nether coordinates (÷8)"), nullptr, nullptr,
      TXT("Divides the position by 8 for the conditions measured from it, from Overworld to Nether.") },
    { F_SCALE_TO_OVERWORLD, TG_EXPERT, TXT("Convert to Overworld coordinates (×8)"), nullptr, nullptr,
      TXT("Multiplies the position by 8 for the conditions measured from it, from Nether to Overworld.") },
    { F_SPIRAL,          TG_EXPERT, TXT("Scan an area in steps (spiral)"), nullptr, nullptr,
      TXT("Moves a test position through the area, nearest first; the conditions measured from it are checked at each step.") },
    // expert: raw climate types (1.18+)
    { F_CLIMATE_NOISE,   TG_EXPERT, TXT("Climate ranges in an area"), nullptr, nullptr,
      TXT("Limits for the climate values (temperature, humidity, ...) that the area must reach or stay within.") },
    { F_NOISE_SAMPLE,    TG_EXPERT, TXT("Climate value coverage of an area"), nullptr, nullptr,
      TXT("A share of the area must have a climate value in a given range. Checked by sampling.") },
    { F_CLIMATE_MINMAX,  TG_EXPERT, TXT("Lowest or highest climate value"), nullptr, nullptr,
      TXT("Finds where a climate value is lowest or highest in the area, optionally within a range.") },
    // expert: biome layers of the old generator (up to 1.17)
    { F_BIOME_4_RIVER,   TG_EXPERT, TXT("Biomes in an area, plain oceans"), nullptr, nullptr,
      TXT("Like \"Biomes in an area\", but checked before ocean temperatures apply (scale 1:4), so oceans are not yet warm, cold or frozen.") },
    { F_BIOME_256_OTEMP, TG_EXPERT, TXT("Ocean temperatures in an area"), nullptr, nullptr,
      TXT("Checks the ocean temperature map (warm, lukewarm, cold, frozen) at the coarse scale 1:256. Depends on the structure seed only.") },
    { F_BIOME_CENTER_256,TG_EXPERT, TXT("Biome patch, coarse (1:256)"), nullptr, nullptr,
      TXT("Like \"Biome patch of a minimum size\", but on the coarse 1:256 biome map.") },
    { F_TEMPS,           TG_EXPERT, TXT("Temperature zones in an area"), nullptr, nullptr,
      TXT("Requires a number of cells of each temperature zone (warm, cold, ...) at the scale 1:1024.") },
};

const TypeText *typeText(int type)
{
    for (const TypeText& t : g_typeText)
        if (t.type == type)
            return &t;
    return nullptr;
}

QString ft(const char *s)
{
    return s ? QApplication::translate("FilterText", s) : QString();
}

// "A, B and C", "A, B or C", "A, B, C and 2 more"
QString joinNames(const QStringList& names, bool any)
{
    const int show = 3;
    QStringList list = names.mid(0, names.size() > show + 1 ? show : names.size());
    QString last;
    if (names.size() > list.size())
        last = QApplication::translate("FilterText", "%n more", "", names.size() - list.size());
    else if (list.size() > 1)
        last = list.takeLast();
    if (last.isEmpty())
        return list.join(", ");
    return any
        ? QApplication::translate("FilterText", "%1 or %2").arg(list.join(", "), last)
        : QApplication::translate("FilterText", "%1 and %2").arg(list.join(", "), last);
}

QStringList biomeNames(uint64_t lo, uint64_t hi, int mc)
{
    // ids below 128 are in the lower mask, the others offset by 128
    QStringList names;
    for (int i = 0; i < 64; i++)
        if (lo & (1ULL << i))
            names.append(getBiomeDisplay(mc, i));
    for (int i = 0; i < 64; i++)
        if (hi & (1ULL << i))
            names.append(getBiomeDisplay(mc, 128 + i));
    return names;
}

// "the world origin" or "#2 (Village)"
QString referenceText(const Condition& c, const std::vector<Condition> *all)
{
    if (c.relative <= 0)
        return QApplication::translate("FilterText", "the world origin");
    QString tag = QString("#%1").arg(c.relative);
    if (all)
    {
        for (const Condition& o : *all)
        {
            if (o.save == c.relative && o.type > 0)
                return QApplication::translate("FilterText", "%1 (%2)")
                    .arg(tag, filterDisplayName(o.type));
        }
    }
    return tag;
}

QString number(int64_t v)
{
    return QString::number(v);
}

// Where the condition looks, e.g. "within 500 blocks of the world origin".
QString wherePhrase(const Condition& c, const std::vector<Condition> *all)
{
    const FilterInfo& fi = g_filterinfo.list[c.type];
    const QString ref = referenceText(c, all);
    const bool fromOrigin = c.relative <= 0;
    if (fi.loc == FilterInfo::LOC_NIL)
        return QString();
    if ((fi.loc & FilterInfo::LOC_R) && c.rmax > 0)
        return QApplication::translate("FilterText", "within %1 blocks of %2")
            .arg(number(c.rmax - 1), ref);
    if (!(fi.loc & FilterInfo::LOC_2) || (c.x1 == c.x2 && c.z1 == c.z2))
    {   // a single position
        QString pos = QString("(%1, %2)").arg(c.x1).arg(c.z1);
        if (fromOrigin)
            return QApplication::translate("FilterText", "at %1").arg(pos);
        return QApplication::translate("FilterText", "at %1 from %2").arg(pos, ref);
    }
    if (c.x1 == c.z1 && c.x2 == c.z2 && c.x1 < 0 && (c.x1 == -c.x2 || c.x1 + 1 == -c.x2))
    {   // a centered square, as the editor and the quick row make it
        return QApplication::translate("FilterText", "within ±%1 blocks (square) of %2")
            .arg(number(-(int64_t)c.x1), ref);
    }
    QString area = QApplication::translate("FilterText", "between (%1, %2) and (%3, %4)")
        .arg(c.x1).arg(c.z1).arg(c.x2).arg(c.z2);
    if (fromOrigin)
        return area;
    return QApplication::translate("FilterText", "%1 from %2").arg(area, ref);
}

// "Village" style subject + where
QString withWhere(const QString& subject, const QString& where)
{
    if (where.isEmpty())
        return subject;
    return QApplication::translate("FilterText", "%1 %2").arg(subject, where);
}

QString countedSubject(int count, const QString& singular, const QString& plural)
{
    if (count <= 0)
        return QApplication::translate("FilterText", "No %1").arg(singular);
    if (count == 1)
        return QApplication::translate("FilterText", "At least 1 %1").arg(singular);
    return QApplication::translate("FilterText", "At least %1 %2").arg(count).arg(plural);
}

QString variantNote(const Condition& c)
{
    QStringList notes;
    const auto tri = [&](uint16_t flag, const char *yes, const char *no) {
        if (c.varflags & flag)
            notes.append(QApplication::translate("FilterText",
                (c.varflags & Condition::VAR_NOT) ? no : yes));
    };
    switch (c.type)
    {
    case F_VILLAGE:
        tri(Condition::VAR_ABANODONED, TXT("abandoned"), TXT("not abandoned"));
        break;
    case F_IGLOO:
        tri(Condition::VAR_BASEMENT, TXT("with basement"), TXT("without basement"));
        break;
    case F_FORTRESS:
        if (c.varflags & Condition::VAR_DENSE_BB)
            notes.append(QApplication::translate("FilterText", "with 2×2 bridge crossings"));
        if (c.varflags & Condition::VAR_FORTRESS_3X1)
            notes.append(QApplication::translate("FilterText", "with 3 bridge crossings in a row"));
        break;
    }
    if ((c.varflags & Condition::VAR_WITH_START) &&
        (c.type == F_VILLAGE || c.type == F_BASTION || c.type == F_PORTAL || c.type == F_PORTALN))
        notes.append(QApplication::translate("FilterText", "selected types only"));
    if (notes.isEmpty())
        return QString();
    return " (" + notes.join(", ") + ")";
}

QString rangeText(double lo, double hi, bool haslo, bool hashi, bool invert)
{
    if (haslo && hashi)
        return invert
            ? QApplication::translate("FilterText", "outside %1 to %2").arg(lo).arg(hi)
            : QApplication::translate("FilterText", "between %1 and %2").arg(lo).arg(hi);
    if (haslo)
        return invert
            ? QApplication::translate("FilterText", "below %1").arg(lo)
            : QApplication::translate("FilterText", "above %1").arg(lo);
    if (hashi)
        return invert
            ? QApplication::translate("FilterText", "above %1").arg(hi)
            : QApplication::translate("FilterText", "below %1").arg(hi);
    return QString();
}

} // namespace


std::vector<int> filterTypeOrder()
{
    std::vector<int> types;
    for (const TypeText& t : g_typeText)
        types.push_back(t.type);
    return types;
}

QString filterDisplayName(int type)
{
    if (type <= 0 || type >= FILTER_MAX)
        return QString();
    if (const TypeText *t = typeText(type))
        return ft(t->name);
    const char *name = g_filterinfo.list[type].name;
    return name ? QApplication::translate("Filter", name) : QString();
}

QString filterIconName(int type)
{
    if (type <= 0 || type >= FILTER_MAX)
        return QString();
    switch (type)
    {
    case F_LOGIC_OR:    return "logic_or";
    case F_LOGIC_NOT:   return "logic_not";
    case F_LUA:         return "lua";
    case F_BIOME_CENTER:
    case F_BIOME_CENTER_256:
        return "biome_center";
    case F_CLIMATE_NOISE:
    case F_NOISE_SAMPLE:
    case F_CLIMATE_MINMAX:
        return "climate";
    default: break;
    }
    const char *icon = g_filterinfo.list[type].icon;
    return icon ? QString::fromLatin1(icon) : QString();
}

QString filterBrief(int type)
{
    if (type <= 0 || type >= FILTER_MAX)
        return QString();
    const TypeText *t = typeText(type);
    if (t && t->brief)
        return ft(t->brief);
    const FilterInfo& fi = g_filterinfo.list[type];
    if (fi.cat == CAT_STRUCT)
        return QApplication::translate("FilterText",
            "How many of them must generate in the area. Set 0 to require that none does.");
    return QString();
}

int filterGroup(int type)
{
    if (const TypeText *t = typeText(type))
        return t->group;
    return TG_EXPERT;
}

bool filterIsExpert(int type)
{
    return filterGroup(type) == TG_EXPERT;
}

QString filterGroupName(int group)
{
    switch (group)
    {
    case TG_STRUCT:         return QApplication::translate("FilterText", "Structures");
    case TG_STRUCT_NETHER:  return QApplication::translate("FilterText", "Nether structures");
    case TG_STRUCT_END:     return QApplication::translate("FilterText", "End structures");
    case TG_BIOMES:         return QApplication::translate("FilterText", "Biomes");
    case TG_WORLD:          return QApplication::translate("FilterText", "World");
    case TG_QUAD:           return QApplication::translate("FilterText", "Quad structures (for farms)");
    case TG_EXPERT:         return QApplication::translate("FilterText", "Expert");
    }
    return QString();
}

QString filterVersionNote(int type)
{
    if (type <= 0 || type >= FILTER_MAX)
        return QString();
    const FilterInfo& fi = g_filterinfo.list[type];
    const bool lower = fi.mcmin > MC_B1_8;
    const bool upper = fi.mcmax < MC_NEWEST;
    if (lower && upper)
        return QApplication::translate("FilterText", "%1 to %2").arg(mc2str(fi.mcmin), mc2str(fi.mcmax));
    if (lower)
        return QString("%1+").arg(mc2str(fi.mcmin));
    if (upper)
        return QApplication::translate("FilterText", "up to %1").arg(mc2str(fi.mcmax));
    return QString();
}

bool filterAvailable(int type, int mc)
{
    if (type <= 0 || type >= FILTER_MAX)
        return false;
    const FilterInfo& fi = g_filterinfo.list[type];
    return mc >= fi.mcmin && mc <= fi.mcmax;
}

QString climateParaName(int para)
{
    switch (para)
    {
    case NP_TEMPERATURE:    return QApplication::translate("FilterText", "temperature");
    case NP_HUMIDITY:       return QApplication::translate("FilterText", "humidity");
    case NP_CONTINENTALNESS:return QApplication::translate("FilterText", "continentalness");
    case NP_EROSION:        return QApplication::translate("FilterText", "erosion");
    case NP_DEPTH:          return QApplication::translate("FilterText", "depth");
    case NP_WEIRDNESS:      return QApplication::translate("FilterText", "weirdness");
    }
    return QString();
}

QString conditionTag(const Condition& c)
{
    if (c.save <= 0) // a new condition gets its id when it is added
        return QApplication::translate("FilterText", "this condition");
    return QString("#%1").arg(c.save);
}

QString conditionSentence(const Condition& c, int mc, const std::vector<Condition> *all)
{
    if (c.type <= 0 || c.type >= FILTER_MAX)
        return QApplication::translate("FilterText", "Choose what to look for");

    const FilterInfo& fi = g_filterinfo.list[c.type];
    const TypeText *t = typeText(c.type);
    const QString name = filterDisplayName(c.type);
    const QString where = wherePhrase(c, all);
    QString s;

    switch (c.type)
    {
    case F_BIOME:
    case F_BIOME_NETHER:
    case F_BIOME_END:
    case F_BIOME_4_RIVER:
    case F_BIOME_256_OTEMP:
    {
        const bool any = c.flags & Condition::FLG_MATCH_ANY;
        QStringList inc = biomeNames(c.biomeToFind, c.biomeToFindM, mc);
        QStringList exc = biomeNames(c.biomeToExcl, c.biomeToExclM, mc);
        QString subject;
        if (!inc.isEmpty() && !exc.isEmpty())
            subject = QApplication::translate("FilterText", "%1, but no %2")
                .arg(joinNames(inc, any), joinNames(exc, true));
        else if (!inc.isEmpty())
            subject = joinNames(inc, any);
        else if (!exc.isEmpty())
            subject = QApplication::translate("FilterText", "No %1").arg(joinNames(exc, true));
        else
            subject = QApplication::translate("FilterText", "%1 (no biomes chosen yet)").arg(name);
        s = withWhere(subject, where);
        break;
    }
    case F_BIOME_SAMPLE:
    {
        QStringList inc = biomeNames(c.biomeToFind, c.biomeToFindM, mc);
        QString biomes = inc.isEmpty()
            ? QApplication::translate("FilterText", "chosen biomes")
            : joinNames(inc, true);
        s = withWhere(QApplication::translate("FilterText", "At least %1% %2")
            .arg(QString::number(c.converage * 100, 'g', 4), biomes), where);
        break;
    }
    case F_BIOME_CENTER:
    case F_BIOME_CENTER_256:
    {
        QString biome = getBiomeDisplay(mc, c.biomeId);
        QString one = QApplication::translate("FilterText", "%1 patch").arg(biome);
        QString more = QApplication::translate("FilterText", "%1 patches").arg(biome);
        s = withWhere(countedSubject(c.count, one, more), where);
        break;
    }
    case F_NOISE_SAMPLE:
    {
        QString range = rangeText(c.vmin, c.vmax, c.minmax & Condition::E_TEST_LOWER,
            c.minmax & Condition::E_TEST_UPPER, c.flags & Condition::FLG_INVERT);
        s = withWhere(QApplication::translate("FilterText", "At least %1% with %2 %3")
            .arg(QString::number(c.converage * 100, 'g', 4), climateParaName(c.para), range)
            .simplified(), where);
        break;
    }
    case F_CLIMATE_MINMAX:
    {
        QString subject = (c.minmax & Condition::E_LOCATE_MAX)
            ? QApplication::translate("FilterText", "Highest %1").arg(climateParaName(c.para))
            : QApplication::translate("FilterText", "Lowest %1").arg(climateParaName(c.para));
        s = withWhere(subject, where);
        QString range = rangeText(c.vmin, c.vmax, c.minmax & Condition::E_TEST_LOWER,
            c.minmax & Condition::E_TEST_UPPER, c.flags & Condition::FLG_INVERT);
        if (!range.isEmpty())
            s += ", " + range;
        break;
    }
    case F_HEIGHT:
    {
        int lo = c.limok[NP_DEPTH][0], hi = c.limok[NP_DEPTH][1];
        if (lo > hi)
            std::swap(lo, hi);
        const bool inside = c.flags & Condition::FLG_IN_RANGE;
        QString range = rangeText(lo, hi, lo != INT_MIN, hi != INT_MAX, !inside);
        QString subject = range.isEmpty()
            ? name
            : QApplication::translate("FilterText", "Surface height %1").arg(range);
        s = withWhere(subject, where);
        break;
    }
    case F_LOGIC_OR:
        s = c.save > 0
            ? QApplication::translate("FilterText", "Any condition measured from %1 passes").arg(conditionTag(c))
            : QApplication::translate("FilterText", "Any condition measured from it passes");
        break;
    case F_LOGIC_NOT:
        s = c.save > 0
            ? QApplication::translate("FilterText", "No condition measured from %1 passes").arg(conditionTag(c))
            : QApplication::translate("FilterText", "No condition measured from it passes");
        break;
    case F_LUA:
    {
        QMap<uint64_t, QString> scripts;
        getScripts(scripts);
        if (scripts.contains(c.hash))
            s = QApplication::translate("FilterText", "Lua script \"%1\"")
                .arg(QFileInfo(scripts.value(c.hash)).baseName());
        else
            s = QApplication::translate("FilterText", "Lua script (script not found)");
        break;
    }
    case F_SCALE_TO_NETHER:
        s = QApplication::translate("FilterText", "Position of %1 in Nether coordinates (÷8)")
            .arg(referenceText(c, all));
        break;
    case F_SCALE_TO_OVERWORLD:
        s = QApplication::translate("FilterText", "Position of %1 in Overworld coordinates (×8)")
            .arg(referenceText(c, all));
        break;
    case F_SPIRAL:
        s = withWhere(QApplication::translate("FilterText", "Scan in steps of %1 blocks")
            .arg(c.step), where);
        break;
    default:
        if (fi.branch == FilterInfo::BR_CLUST && t && t->noun)
            s = withWhere(countedSubject(c.count, ft(t->noun), ft(t->plural))
                + variantNote(c), where);
        else
            s = withWhere(name + variantNote(c), where);
        break;
    }

    if (c.skipref && fi.branch == FilterInfo::BR_CLUST && c.relative > 0)
        s += QApplication::translate("FilterText", ", not counting one exactly at %1")
            .arg(QString("#%1").arg(c.relative));
    return s;
}
