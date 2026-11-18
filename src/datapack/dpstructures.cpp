#include "dpstructures.h"
#include "dpinternal.h"
#include "dpworld_p.h"
#include "dpzip.h"

extern "C"
{
#include "biomes.h"
#include "rng.h"
#include "util.h"
}

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <unordered_map>

// The rules follow the game's code (1.21.1: StructurePlacement,
// RandomSpreadStructurePlacement, ConcentricRingsStructurePlacement,
// ChunkGeneratorStructureState, ChunkGenerator.createStructures, Structure
// and the findGenerationPoint() of each structure type, JigsawPlacement,
// StructureTemplate). The structure code of deepslate by Jochen Jacobs (MIT
// License) was read as a reference.

namespace dp
{

//==============================================================================
// the game's random numbers and integer arithmetic
//==============================================================================

// WorldgenRandom.setLargeFeatureSeed
static inline void setLargeFeatureSeed(uint64_t *r, uint64_t seed, int x, int z)
{
    setSeed(r, seed);
    uint64_t a = nextLong(r);
    uint64_t b = nextLong(r);
    setSeed(r, ((uint64_t) (int64_t) x * a) ^ ((uint64_t) (int64_t) z * b) ^ seed);
}

// WorldgenRandom.setLargeFeatureWithSalt
static inline void setLargeFeatureWithSalt(uint64_t *r, uint64_t seed, int x, int z, int salt)
{
    setSeed(r, (uint64_t) (int64_t) x * 341873128712ULL + (uint64_t) (int64_t) z * 132897987541ULL +
        seed + (uint64_t) (int64_t) salt);
}

// (int) Math.round(v): the nearest integer, halves up
static inline int javaRound(double v)
{
    double f = std::floor(v);
    if (v - f >= 0.5)
        f += 1.0;
    return (int) (int64_t) f;
}

// (int) f in Java: saturates
static inline int javaF2I(float f)
{
    if (f != f)
        return 0;
    if (f >= 2147483647.0F)
        return 2147483647;
    if (f <= -2147483648.0F)
        return (-2147483647 - 1);
    return (int) f;
}

// Java's (a + b) / 2 on int: rounds towards zero
static inline int javaMid(int a, int b)
{
    return (int) (((int64_t) a + b) / 2);
}

// Mth.nextInt(random, a, b)
static inline int mthNextInt(uint64_t *r, int a, int b)
{
    return a >= b ? a : nextInt(r, b - a + 1) + a;
}

// Mth.randomBetweenInclusive(random, a, b)
static inline int between(uint64_t *r, int a, int b)
{
    return nextInt(r, b - a + 1) + a;
}

//==============================================================================
// height providers
//==============================================================================

int VAnchor::resolve(int minY, int height) const
{
    switch (kind)
    {
    case ABOVE_BOTTOM: return minY + value;
    case BELOW_TOP: return height - 1 + minY - value;
    default: return value;
    }
}

int HeightProv::sample(uint64_t *r, int minY, int height) const
{
    const int i = a.resolve(minY, height);
    const int j = b.resolve(minY, height);
    switch (type)
    {
    case CONSTANT:
        return i;
    case UNIFORM:
        if (i > j)
            return i;
        return between(r, i, j);
    case BIASED_TO_BOTTOM:
    {
        if (j - i - inner + 1 <= 0)
            return i;
        int k = nextInt(r, j - i - inner + 1);
        return nextInt(r, k + inner) + i;
    }
    case VERY_BIASED_TO_BOTTOM:
    {
        if (j - i - inner + 1 <= 0)
            return i;
        int k = mthNextInt(r, i + inner, j);
        int l = mthNextInt(r, i, k - 1);
        return mthNextInt(r, i, l - 1 + inner);
    }
    case TRAPEZOID:
    {
        if (i > j)
            return i;
        int k = j - i;
        if (inner >= k)
            return between(r, i, j);
        int l = (k - inner) / 2;
        int m = k - l;
        return i + between(r, 0, m) + between(r, 0, l);
    }
    case WEIGHTED_LIST:
    {
        int total = 0;
        for (const auto& e : list)
            total += e.second;
        if (total <= 0)
            return minY;
        int n = nextInt(r, total);
        for (const auto& e : list)
        {
            n -= e.second;
            if (n < 0)
                return e.first.sample(r, minY, height);
        }
        return minY;
    }
    }
    return i;
}

//==============================================================================
// NBT (structure templates)
//==============================================================================

namespace
{

// A forward-only NBT reader (big endian, the game's tag types 1 .. 12) that
// skips what it is not asked for, without building a tree.
class Nbt
{
public:
    Nbt(const unsigned char *p, const unsigned char *end) : p(p), end(end) {}
    const unsigned char *p, *end;
    bool ok = true;

    bool need(size_t n)
    {
        if ((size_t) (end - p) < n)
            ok = false;
        return ok;
    }
    int u8() { if (!need(1)) return 0; return *p++; }
    int32_t s32()
    {
        if (!need(4))
            return 0;
        uint32_t v = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
        p += 4;
        return (int32_t) v;
    }
    // a string as raw bytes (modified UTF-8; the keys and ids are ASCII)
    QByteArray str()
    {
        if (!need(2))
            return QByteArray();
        size_t n = ((size_t) p[0] << 8) | p[1];
        p += 2;
        if (!need(n))
            return QByteArray();
        QByteArray s((const char *) p, (int) n);
        p += n;
        return s;
    }
    bool skipStr()
    {
        if (!need(2))
            return false;
        size_t n = ((size_t) p[0] << 8) | p[1];
        p += 2;
        if (!need(n))
            return false;
        p += n;
        return true;
    }
    // the next key of a compound: its type (0 at the end) and name
    int key(QByteArray *name)
    {
        int t = u8();
        if (!ok || t == 0)
            return 0;
        if (t > 12)
        {
            ok = false;
            return 0;
        }
        *name = str();
        return ok ? t : 0;
    }
    // an integer of type 1 .. 4
    int64_t num(int t)
    {
        switch (t)
        {
        case 1: return (int8_t) u8();
        case 2: { if (!need(2)) return 0; int v = (int16_t) ((p[0] << 8) | p[1]); p += 2; return v; }
        case 3: return s32();
        case 4: { int64_t hi = (uint32_t) s32(); int64_t lo = (uint32_t) s32(); return (hi << 32) | lo; }
        default: skip(t); return 0;
        }
    }
    bool skip(int t, int depth = 0)
    {
        if (depth > 512)
            return ok = false;
        static const int fixed[13] = { 0, 1, 2, 4, 8, 4, 8, 0, 0, 0, 0, 0, 0 };
        if (t >= 1 && t <= 6)
            return need((size_t) fixed[t]) && (p += fixed[t], true);
        switch (t)
        {
        case 7: case 11: case 12:
        {
            int32_t n = s32();
            size_t w = t == 7 ? 1 : t == 11 ? 4 : 8;
            if (n < 0 || !need((size_t) n * w))
                return ok = false;
            p += (size_t) n * w;
            return true;
        }
        case 8:
            return skipStr();
        case 9:
        {
            int et = u8();
            int32_t n = s32();
            if (n < 0 || (n > 0 && (et < 1 || et > 12)))
                return ok = false;
            if (et >= 1 && et <= 6)
                return need((size_t) n * fixed[et]) && (p += (size_t) n * fixed[et], true);
            for (int32_t i = 0; i < n && ok; i++)
                skip(et, depth + 1);
            return ok;
        }
        case 10:
            for (;;)
            {
                int ct = u8();
                if (!ok || ct == 0)
                    return ok;
                if (ct > 12 || !skipStr() || !skip(ct, depth + 1))
                    return ok = false;
            }
        default:
            return ok = false;
        }
    }
    // the header of a list: element type and count
    int32_t list(int *et)
    {
        *et = u8();
        int32_t n = s32();
        if (n < 0)
            ok = false;
        return ok ? n : 0;
    }
};

// One palette (a list of block state compounds): which entries are jigsaws
std::vector<char> readPalette(Nbt& r, int32_t n)
{
    std::vector<char> jig;
    for (int32_t i = 0; i < n && r.ok; i++)
    {
        bool isJigsaw = false;
        QByteArray k;
        while (int t = r.key(&k))
        {
            if (t == 8 && k == "Name")
            {
                QByteArray v = r.str();
                isJigsaw = v == "minecraft:jigsaw" || v == "jigsaw";
            }
            else
                r.skip(t);
        }
        jig.push_back(isJigsaw);
    }
    return jig;
}

// gzip (RFC 1952) around a deflate stream
bool gunzip(const QByteArray& in, QByteArray *out, QString *error)
{
    const unsigned char *d = (const unsigned char *) in.constData();
    const size_t n = (size_t) in.size();
    if (n < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8)
    {
        *error = QStringLiteral("not gzip compressed");
        return false;
    }
    const int flags = d[3];
    size_t pos = 10;
    if (flags & 4)
    {
        if (pos + 2 > n)
            goto broken;
        pos += 2 + (d[pos] | (d[pos + 1] << 8));
    }
    if (flags & 8)
        while (pos < n && d[pos++]) {}
    if (flags & 16)
        while (pos < n && d[pos++]) {}
    if (flags & 2)
        pos += 2;
    if (pos + 8 > n)
        goto broken;
    {
        const uint32_t size = d[n - 4] | (d[n - 3] << 8) | (d[n - 2] << 16) | ((uint32_t) d[n - 1] << 24);
        if (size > (64u << 20))
        {
            *error = QStringLiteral("too large");
            return false;
        }
        out->resize((int) size);
        if (!inflateRaw(d + pos, n - 8 - pos, (unsigned char *) out->data(), size))
            goto broken;
        const uint32_t crc = d[n - 8] | (d[n - 7] << 8) | (d[n - 6] << 16) | ((uint32_t) d[n - 5] << 24);
        if (crc32((const unsigned char *) out->constData(), size) != crc)
            goto broken;
        return true;
    }
broken:
    *error = QStringLiteral("the compressed data is damaged");
    return false;
}

} // namespace

bool readTemplate(const QByteArray& file, Template *t, QString *error)
{
    *t = Template();
    QByteArray raw;
    if (!file.isEmpty() && (unsigned char) file[0] == 10)
        raw = file;     // not compressed
    else if (!gunzip(file, &raw, error))
        return false;
    const unsigned char *b = (const unsigned char *) raw.constData();
    Nbt r(b, b + raw.size());
    QByteArray k;
    // the root compound: size and palettes first, the blocks need them
    std::vector<std::vector<char>> palettes;
    const unsigned char *blocks = nullptr;
    if (r.u8() != 10 || !r.skipStr())
        r.ok = false;
    while (r.ok)
    {
        int type = r.key(&k);
        if (!type)
            break;
        if (type == 9 && k == "size")
        {
            int et;
            int32_t n = r.list(&et);
            int v[3] = { 0, 0, 0 };
            for (int32_t i = 0; i < n && r.ok; i++)
            {
                int64_t x = r.num(et);
                if (i < 3)
                    v[i] = (int) x;
            }
            t->sx = v[0]; t->sy = v[1]; t->sz = v[2];
        }
        else if (type == 9 && k == "palette")
        {
            int et;
            int32_t n = r.list(&et);
            if (et == 10 || n == 0)
                palettes.push_back(readPalette(r, n));
            else
                r.ok = false;
        }
        else if (type == 9 && k == "palettes")
        {
            int et;
            int32_t n = r.list(&et);
            for (int32_t i = 0; i < n && r.ok; i++)
            {
                int et2;
                int32_t m = r.list(&et2);
                if (et2 == 10 || m == 0)
                    palettes.push_back(readPalette(r, m));
                else
                    r.ok = false;
            }
        }
        else if (type == 9 && k == "blocks")
        {
            blocks = r.p;
            r.skip(9);
        }
        else
            r.skip(type);
    }
    // the blocks: position, state, and the jigsaw's settings
    t->palettes.resize(palettes.size());
    if (blocks && r.ok)
    {
        Nbt rb(blocks, b + raw.size());
        int et;
        int32_t n = rb.list(&et);
        if (n > 0 && et != 10)
            rb.ok = false;
        for (int32_t i = 0; i < n && rb.ok; i++)
        {
            Template::Jigsaw j;
            int state = -1;
            bool havePos = false;
            const unsigned char *nbt = nullptr;
            while (int type = rb.key(&k))
            {
                if (type == 9 && k == "pos")
                {
                    int et2;
                    int32_t m = rb.list(&et2);
                    int v[3] = { 0, 0, 0 };
                    for (int32_t q = 0; q < m && rb.ok; q++)
                    {
                        int64_t x = rb.num(et2);
                        if (q < 3)
                            v[q] = (int) x;
                    }
                    j.x = v[0]; j.y = v[1]; j.z = v[2];
                    havePos = m == 3;
                }
                else if (type >= 1 && type <= 4 && k == "state")
                    state = (int) rb.num(type);
                else if (type == 10 && k == "nbt")
                {
                    nbt = rb.p;
                    rb.skip(10);
                }
                else
                    rb.skip(type);
            }
            bool anyJigsaw = false;
            for (const std::vector<char>& pal : palettes)
                if (state >= 0 && (size_t) state < pal.size() && pal[(size_t) state])
                    anyJigsaw = true;
            if (!anyJigsaw || !havePos)
                continue;
            if (nbt)
            {
                Nbt rn(nbt, b + raw.size());
                while (int type = rn.key(&k))
                {
                    // "attachement_type" before 1.16
                    if (type == 8 && (k == "name" || (k == "attachement_type" && j.name.isEmpty())))
                        j.name = normalizeId(QString::fromUtf8(rn.str()));
                    else if (type >= 1 && type <= 4 && k == "selection_priority")
                        j.priority = (int) rn.num(type);
                    else
                        rn.skip(type);
                }
            }
            for (size_t q = 0; q < palettes.size(); q++)
                if ((size_t) state < palettes[q].size() && palettes[q][(size_t) state])
                    t->palettes[q].push_back(j);
        }
        r.ok = rb.ok;
    }
    if (!r.ok)
    {
        *error = QStringLiteral("the NBT data is damaged");
        return false;
    }
    // StructureTemplate.buildInfoList: sorted by y, then x, then z
    for (std::vector<Template::Jigsaw>& js : t->palettes)
        std::stable_sort(js.begin(), js.end(), [](const Template::Jigsaw& a, const Template::Jigsaw& b) {
            if (a.y != b.y) return a.y < b.y;
            if (a.x != b.x) return a.x < b.x;
            return a.z < b.z;
        });
    t->found = true;
    return true;
}

//==============================================================================
// loading
//==============================================================================

namespace
{

class Loader
{
public:
    explicit Loader(WorldPrivate& w) : w(w), sd(w.structures), files(w.res) {}
    void run();

private:
    WorldPrivate& w;
    StructureData& sd;
    PackFiles files;
    QHash<QString, int> structIndex;
    QHash<QString, int> templateIndex;
    QHash<int, QString> exclusionTarget;    // set -> other_set of its exclusion zone

    void warn(int pack, const QString& id, const QString& text)
    {
        w.issue(Issue::Warning, pack, id, text);
    }
    std::vector<char> biomeSet(const QJsonValue& v, QString *error = nullptr);
    bool readAnchor(const QJsonValue& v, VAnchor *a);
    bool readHeight(const QJsonValue& v, HeightProv *h, int depth = 0);
    void loadStructure(const QString& id);
    void loadJigsaw(const QString& id, int pack, const QJsonObject& o, StructureDef& def, StructureInfo& info);
    bool readElement(const QJsonValue& v, PoolElement *e, int pack, const QString& poolId, QString *why);
    int templ(const QString& id, int pack, const QString& poolId);
    void loadSet(const QString& id);
};

// A holder set of biomes: "#tag", "id" or a list of ids. A tag nested too
// deeply sets @error.
std::vector<char> Loader::biomeSet(const QJsonValue& v, QString *error)
{
    std::vector<char> mask(w.biomes.size(), 0);
    QStringList ids;
    QString err;
    if (v.isString())
    {
        QString s = v.toString();
        if (s.startsWith(QLatin1Char('#')))
            ids = w.res.tagValues(REG_BIOME_TAG, s.mid(1), &err);
        else
            ids.push_back(normalizeId(s));
    }
    else if (v.isArray())
    {
        for (const QJsonValue& e : v.toArray())
        {
            QString s = e.toString();
            if (s.startsWith(QLatin1Char('#')))
            {
                QString e2;
                ids += w.res.tagValues(REG_BIOME_TAG, s.mid(1), &e2);
                if (err.isEmpty())
                    err = e2;
            }
            else
                ids.push_back(normalizeId(s));
        }
    }
    if (error)
        *error = err;
    for (const QString& id : ids)
    {
        // biomes that no dimension can generate have no index: no cell has them
        auto it = w.biomeIndex.constFind(id);
        if (it != w.biomeIndex.constEnd() && it.value() >= 0 && (size_t) it.value() < mask.size())
            mask[(size_t) it.value()] = 1;
    }
    return mask;
}

bool Loader::readAnchor(const QJsonValue& v, VAnchor *a)
{
    QJsonObject o = v.toObject();
    if (o.contains(QLatin1String("absolute")))
        a->kind = VAnchor::ABSOLUTE, a->value = o.value(QLatin1String("absolute")).toInt();
    else if (o.contains(QLatin1String("above_bottom")))
        a->kind = VAnchor::ABOVE_BOTTOM, a->value = o.value(QLatin1String("above_bottom")).toInt();
    else if (o.contains(QLatin1String("below_top")))
        a->kind = VAnchor::BELOW_TOP, a->value = o.value(QLatin1String("below_top")).toInt();
    else
        return false;
    return true;
}

bool Loader::readHeight(const QJsonValue& v, HeightProv *h, int depth)
{
    if (depth > 16 || !v.isObject())
        return false;
    QJsonObject o = v.toObject();
    if (!o.contains(QLatin1String("type")))
    {
        // a vertical anchor alone: constant
        h->type = HeightProv::CONSTANT;
        return readAnchor(o, &h->a);
    }
    QString type = normalizeId(o.value(QLatin1String("type")).toString());
    if (type == QLatin1String("minecraft:constant"))
    {
        h->type = HeightProv::CONSTANT;
        return readAnchor(o.value(QLatin1String("value")), &h->a);
    }
    if (type == QLatin1String("minecraft:weighted_list"))
    {
        h->type = HeightProv::WEIGHTED_LIST;
        for (const QJsonValue& e : o.value(QLatin1String("distribution")).toArray())
        {
            HeightProv inner;
            if (!readHeight(e.toObject().value(QLatin1String("data")), &inner, depth + 1))
                return false;
            h->list.emplace_back(inner, e.toObject().value(QLatin1String("weight")).toInt(1));
        }
        return !h->list.empty();
    }
    if (type == QLatin1String("minecraft:uniform"))
        h->type = HeightProv::UNIFORM;
    else if (type == QLatin1String("minecraft:biased_to_bottom"))
        h->type = HeightProv::BIASED_TO_BOTTOM, h->inner = o.value(QLatin1String("inner")).toInt(1);
    else if (type == QLatin1String("minecraft:very_biased_to_bottom"))
        h->type = HeightProv::VERY_BIASED_TO_BOTTOM, h->inner = o.value(QLatin1String("inner")).toInt(1);
    else if (type == QLatin1String("minecraft:trapezoid"))
        h->type = HeightProv::TRAPEZOID, h->inner = o.value(QLatin1String("plateau")).toInt(0);
    else
        return false;
    return readAnchor(o.value(QLatin1String("min_inclusive")), &h->a) &&
        readAnchor(o.value(QLatin1String("max_inclusive")), &h->b);
}

static bool heightmapByName(const QString& s, Heightmap *hm)
{
    static const char *const names[] = { "WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG",
        "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES" };
    for (int i = 0; i < 6; i++)
        if (s == QLatin1String(names[i]))
        {
            *hm = (Heightmap) i;
            return true;
        }
    return false;
}

int Loader::templ(const QString& id, int pack, const QString& poolId)
{
    auto it = templateIndex.constFind(id);
    if (it != templateIndex.constEnd())
        return it.value();
    Template t;
    auto ref = w.res.templates.constFind(id);
    if (ref == w.res.templates.constEnd())
    {
        // the game uses an empty template then
        warn(pack, poolId, QStringLiteral("The structure template %1 does not exist.").arg(id));
        t.found = true;
    }
    else
    {
        QByteArray data;
        QString err;
        if (!files.read(ref.value(), &data, &err) || !readTemplate(data, &t, &err))
        {
            warn(ref.value().pack, id, QStringLiteral("The structure template cannot be read: %1.").arg(err));
            t = Template();
        }
    }
    sd.templates.push_back(t);
    int idx = (int) sd.templates.size() - 1;
    templateIndex.insert(id, idx);
    return idx;
}

bool Loader::readElement(const QJsonValue& v, PoolElement *e, int pack, const QString& poolId, QString *why)
{
    QJsonObject o = v.toObject();
    QString type = normalizeId(o.value(QLatin1String("element_type")).toString());
    if (type == QLatin1String("minecraft:single_pool_element") ||
        type == QLatin1String("minecraft:legacy_single_pool_element"))
    {
        e->kind = PoolElement::SINGLE;
        e->tmpl = templ(normalizeId(o.value(QLatin1String("location")).toString()), pack, poolId);
        if (!sd.templates[(size_t) e->tmpl].found)
        {
            *why = QStringLiteral("A template of its start pool cannot be read.");
            return false;
        }
        return true;
    }
    if (type == QLatin1String("minecraft:list_pool_element"))
    {
        e->kind = PoolElement::LIST;
        for (const QJsonValue& c : o.value(QLatin1String("elements")).toArray())
        {
            PoolElement ce;
            if (!readElement(c, &ce, pack, poolId, why))
                return false;
            e->elements.push_back(ce);
        }
        if (e->elements.empty())
        {
            *why = QStringLiteral("Its start pool has an empty list element.");
            return false;
        }
        return true;
    }
    if (type == QLatin1String("minecraft:feature_pool_element"))
    {
        e->kind = PoolElement::FEATURE;
        return true;
    }
    if (type == QLatin1String("minecraft:empty_pool_element"))
    {
        e->kind = PoolElement::EMPTY;
        return true;
    }
    *why = QStringLiteral("Its start pool has an element of the unknown type \"%1\".").arg(type);
    return false;
}

void Loader::loadJigsaw(const QString& id, int pack, const QJsonObject& o, StructureDef& def, StructureInfo& info)
{
    QString poolId = normalizeId(o.value(QLatin1String("start_pool")).toString());
    // JigsawPlacement looks the start pool up through the pool aliases
    for (const QJsonValue& av : o.value(QLatin1String("pool_aliases")).toArray())
    {
        QJsonObject ao = av.toObject();
        QString atype = normalizeId(ao.value(QLatin1String("type")).toString());
        if (atype == QLatin1String("minecraft:direct"))
        {
            if (normalizeId(ao.value(QLatin1String("alias")).toString()) == poolId)
                poolId = normalizeId(ao.value(QLatin1String("target")).toString());
            continue;
        }
        // random and random_group choose with their own random numbers;
        // only a problem if they bind the start pool
        if (QString::fromUtf8(QJsonDocument(ao).toJson(QJsonDocument::Compact))
                .contains(QLatin1Char('"') + poolId + QLatin1Char('"')))
        {
            info.reason = QStringLiteral("Its start pool is chosen at random (pool aliases).");
            return;
        }
    }
    if (!readHeight(o.value(QLatin1String("start_height")), &def.startHeight))
    {
        warn(pack, id, QStringLiteral("The structure's start_height is not a known height provider."));
        info.reason = QStringLiteral("Its start height is not understood.");
        return;
    }
    if (o.contains(QLatin1String("project_start_to_heightmap")))
    {
        def.projected = true;
        if (!heightmapByName(o.value(QLatin1String("project_start_to_heightmap")).toString(), &def.heightmap))
        {
            warn(pack, id, QStringLiteral("Unknown heightmap type in project_start_to_heightmap."));
            info.reason = QStringLiteral("Its heightmap type is not known.");
            return;
        }
    }
    if (o.contains(QLatin1String("start_jigsaw_name")))
        def.startJigsawName = normalizeId(o.value(QLatin1String("start_jigsaw_name")).toString());
    // dimension_padding (since 1.21): a non-negative int, or {bottom, top}
    // whose fields fall back to 0 when they are missing or invalid (the
    // codec reads them leniently). Without the field the game skips the check.
    if (o.contains(QLatin1String("dimension_padding")))
    {
        const QJsonValue pv = o.value(QLatin1String("dimension_padding"));
        auto nonNegative = [](const QJsonValue& v, int *out) {
            const double d = v.toDouble(-1.0);
            if (!v.isDouble() || d != std::floor(d) || d < 0 || d > INT_MAX)
                return false;
            *out = (int) d;
            return true;
        };
        int bottom = 0, top = 0;
        if (pv.isObject())
        {
            if (!nonNegative(pv.toObject().value(QLatin1String("bottom")), &bottom))
                bottom = 0;
            if (!nonNegative(pv.toObject().value(QLatin1String("top")), &top))
                top = 0;
        }
        else if (nonNegative(pv, &bottom))
        {
            top = bottom;
        }
        else
        {
            warn(pack, id, QStringLiteral("dimension_padding must be a non-negative number or an object "
                "with bottom and top."));
            info.reason = QStringLiteral("Its dimension_padding is invalid.");
            return;
        }
        // JigsawPlacement checks the start piece against it since 1.21.4
        def.padding = w.request.mc >= MC_1_21_4;
        def.padBottom = bottom;
        def.padTop = top;
    }

    QJsonObject pool;
    int poolPack = -1;
    const Resource *r = w.res.find(REG_TEMPLATE_POOL, poolId);
    if (!r)
    {
        warn(pack, id, QStringLiteral("The start pool %1 does not exist.").arg(poolId));
        info.reason = QStringLiteral("Its start pool does not exist.");
        return;
    }
    QJsonValue pv;
    QString err;
    if (!parseJson(r->data, &pv, &err) || !pv.isObject())
    {
        warn(r->pack, poolId, QStringLiteral("The template pool is not valid JSON: %1.").arg(err));
        info.reason = QStringLiteral("Its start pool has errors.");
        return;
    }
    pool = pv.toObject();
    poolPack = r->pack;
    for (const QJsonValue& ev : pool.value(QLatin1String("elements")).toArray())
    {
        QJsonObject eo = ev.toObject();
        PoolElement e;
        QString why;
        if (!readElement(eo.value(QLatin1String("element")), &e, poolPack, poolId, &why))
        {
            info.reason = why;
            return;
        }
        // the codec takes weights from 1 to 150
        int weight = 1;
        if (eo.contains(QLatin1String("weight")))
        {
            const double wd = eo.value(QLatin1String("weight")).toDouble(0.0);
            if (wd != std::floor(wd) || wd < 1 || wd > 150)
            {
                warn(poolPack, poolId, QStringLiteral("The weight of an element must be a whole number from 1 to 150."));
                info.reason = QStringLiteral("Its start pool has an invalid weight.");
                return;
            }
            weight = (int) wd;
        }
        def.pool.push_back(e);
        // StructureTemplatePool lists each element as often as its weight
        // and draws one entry of that list: the cumulative weights
        def.poolTotal += weight;
        def.poolCumulative.push_back(def.poolTotal);
    }
    if (def.poolTotal == 0)
    {
        warn(poolPack, poolId, QStringLiteral("The start pool of %1 has no elements.").arg(id));
        info.reason = QStringLiteral("Its start pool is empty.");
        return;
    }
    def.kind = StructureDef::JIGSAW;
    info.supported = true;
}

void Loader::loadStructure(const QString& id)
{
    sd.info.emplace_back();
    sd.defs.emplace_back();
    StructureInfo& info = sd.info.back();
    StructureDef& def = sd.defs.back();
    structIndex.insert(id, (int) sd.info.size() - 1);
    info.id = id;
    info.name = biomeDisplayName(id);

    const Resource *r = w.res.find(REG_STRUCTURE, id);
    if (r->pack > 0)    // packs[0] is the vanilla data
        info.pack = w.res.packName(r->pack);
    QJsonValue v;
    QString err;
    if (!parseJson(r->data, &v, &err) || !v.isObject())
    {
        warn(r->pack, id, QStringLiteral("The structure is not valid JSON: %1.").arg(err));
        info.reason = QStringLiteral("The structure's file has errors.");
        return;
    }
    const QJsonObject o = v.toObject();
    info.type = normalizeId(o.value(QLatin1String("type")).toString());
    QString tagError;
    def.biomes = biomeSet(o.value(QLatin1String("biomes")), &tagError);
    if (!tagError.isEmpty())
    {
        warn(r->pack, id, tagError);
        info.reason = QStringLiteral("Its biome tag is nested too deeply.");
        return;
    }

    const QString t = info.type;
    auto is = [&t](const char *name) { return t == QLatin1String(name); };
    StructureDef::Kind k = StructureDef::UNSUPPORTED;
    if (is("minecraft:jigsaw"))
    {
        loadJigsaw(id, r->pack, o, def, info);
        return;
    }
    else if (is("minecraft:stronghold"))
        k = StructureDef::STRONGHOLD;
    else if (is("minecraft:fortress"))
        k = StructureDef::FORTRESS;
    else if (is("minecraft:ruined_portal"))
        k = StructureDef::RUINED_PORTAL;
    else if (is("minecraft:mineshaft"))
        k = StructureDef::MINESHAFT;
    else if (is("minecraft:woodland_mansion"))
        k = StructureDef::MANSION;
    else if (is("minecraft:end_city"))
        k = StructureDef::END_CITY;
    else if (is("minecraft:ocean_monument"))
        k = StructureDef::MONUMENT, def.heightmap = HM_OCEAN_FLOOR_WG;
    else if (is("minecraft:igloo") || is("minecraft:swamp_hut"))
        k = StructureDef::CHUNK_CENTER, def.heightmap = HM_WORLD_SURFACE_WG;
    else if (is("minecraft:buried_treasure") || is("minecraft:ocean_ruin"))
        k = StructureDef::CHUNK_CENTER, def.heightmap = HM_OCEAN_FLOOR_WG;
    else if (is("minecraft:shipwreck"))
    {
        k = StructureDef::CHUNK_CENTER;
        def.heightmap = o.value(QLatin1String("is_beached")).toBool(false) ? HM_WORLD_SURFACE_WG : HM_OCEAN_FLOOR_WG;
    }
    else if (is("minecraft:desert_pyramid"))
        k = StructureDef::SINGLE_PIECE, def.width = 21, def.depth = 21;
    else if (is("minecraft:jungle_temple"))
        k = StructureDef::SINGLE_PIECE, def.width = 12, def.depth = 15;
    else if (is("minecraft:nether_fossil"))
    {
        if (!readHeight(o.value(QLatin1String("height")), &def.startHeight))
        {
            warn(r->pack, id, QStringLiteral("The structure's height is not a known height provider."));
            info.reason = QStringLiteral("Its height is not understood.");
            return;
        }
        k = StructureDef::NETHER_FOSSIL;
    }
    else
    {
        warn(r->pack, id, QStringLiteral("The structure type \"%1\" is not known (it may come from a mod); "
            "the structure is not shown.").arg(t));
        info.reason = QStringLiteral("The structure type \"%1\" is not known.").arg(t);
        return;
    }
    def.kind = k;
    info.supported = true;
}

void Loader::loadSet(const QString& id)
{
    sd.setInfo.emplace_back();
    sd.sets.emplace_back();
    StructureSetInfo& info = sd.setInfo.back();
    StructureSetDef& set = sd.sets.back();
    info.id = id;

    const Resource *r = w.res.find(REG_STRUCTURE_SET, id);
    QJsonValue v;
    QString err;
    if (!parseJson(r->data, &v, &err) || !v.isObject())
    {
        warn(r->pack, id, QStringLiteral("The structure set is not valid JSON: %1.").arg(err));
        info.reason = QStringLiteral("The structure set's file has errors.");
        return;
    }
    const QJsonObject o = v.toObject();
    bool ok = true;
    for (const QJsonValue& ev : o.value(QLatin1String("structures")).toArray())
    {
        QJsonObject eo = ev.toObject();
        QString sid = normalizeId(eo.value(QLatin1String("structure")).toString());
        auto it = structIndex.constFind(sid);
        if (it == structIndex.constEnd())
        {
            warn(r->pack, id, QStringLiteral("The structure %1 does not exist.").arg(sid));
            ok = false;
            continue;
        }
        int weight = eo.value(QLatin1String("weight")).toInt(1);
        if (weight <= 0)
        {
            warn(r->pack, id, QStringLiteral("The weight of %1 is not positive.").arg(sid));
            ok = false;
        }
        set.entries.emplace_back(it.value(), weight);
        info.structures.push_back(it.value());
    }
    {
        int64_t sum = 0;
        for (const auto& e : set.entries)
            sum += e.second;
        if (sum > INT_MAX)
        {
            // the game sums the weights in an int and cannot draw from the
            // overflowed total
            warn(r->pack, id, QStringLiteral("The weights of the structures add up to more than 2147483647."));
            ok = false;
        }
    }
    if (set.entries.empty())
    {
        if (ok)
            warn(r->pack, id, QStringLiteral("The structure set has no structures."));
        ok = false;
    }

    const QJsonObject po = o.value(QLatin1String("placement")).toObject();
    PlacementDef& pl = set.placement;
    info.placement = normalizeId(po.value(QLatin1String("type")).toString());
    pl.salt = po.value(QLatin1String("salt")).toInt();
    pl.frequency = (float) po.value(QLatin1String("frequency")).toDouble(1.0);
    if (!(pl.frequency >= 0.0F && pl.frequency <= 1.0F))
    {
        warn(r->pack, id, QStringLiteral("The frequency must be from 0 to 1."));
        ok = false;
    }
    QString red = po.value(QLatin1String("frequency_reduction_method")).toString(QStringLiteral("default"));
    if (red == QLatin1String("default"))
        pl.reduction = 0;
    else if (red == QLatin1String("legacy_type_1"))
        pl.reduction = 1;
    else if (red == QLatin1String("legacy_type_2"))
        pl.reduction = 2;
    else if (red == QLatin1String("legacy_type_3"))
        pl.reduction = 3;
    else
    {
        warn(r->pack, id, QStringLiteral("Unknown frequency_reduction_method \"%1\".").arg(red));
        ok = false;
    }
    // a whole number within the range of the game's codec
    auto ranged = [&](const char *key, int lo, int hi, int *out) {
        const QJsonValue fv = po.value(QLatin1String(key));
        const double d = fv.toDouble(-1e300);
        if (!fv.isDouble() || d != std::floor(d) || d < lo || d > hi)
        {
            warn(r->pack, id, QStringLiteral("\"%1\" must be a whole number from %2 to %3.")
                .arg(QLatin1String(key)).arg(lo).arg(hi));
            ok = false;
            *out = lo;
            return;
        }
        *out = (int) d;
    };
    if (info.placement == QLatin1String("minecraft:random_spread"))
    {
        pl.kind = PlacementDef::RANDOM_SPREAD;
        ranged("spacing", 0, 4096, &pl.spacing);
        ranged("separation", 0, 4096, &pl.separation);
        pl.triangular = po.value(QLatin1String("spread_type")).toString() == QLatin1String("triangular");
        if (ok && pl.spacing <= pl.separation)
        {
            warn(r->pack, id, QStringLiteral("The spacing must be larger than the separation."));
            ok = false;
        }
        if (!ok)
            pl.spacing = 1, pl.separation = 0;     // never used: the set is not supported
    }
    else if (info.placement == QLatin1String("minecraft:concentric_rings"))
    {
        pl.kind = PlacementDef::CONCENTRIC_RINGS;
        ranged("distance", 0, 1023, &pl.distance);
        ranged("spread", 0, 1023, &pl.spread);
        ranged("count", 1, 4095, &pl.count);
        QString tagError;
        pl.preferred = biomeSet(po.value(QLatin1String("preferred_biomes")), &tagError);
        if (!tagError.isEmpty())
        {
            warn(r->pack, id, tagError);
            ok = false;
        }
        if (pl.count > 0 && pl.spread <= 0)
        {
            warn(r->pack, id, QStringLiteral("The spread of the rings must be positive."));
            ok = false;
        }
    }
    else
    {
        warn(r->pack, id, QStringLiteral("The placement type \"%1\" is not known (it may come from a mod); "
            "the structure set is not shown.").arg(info.placement));
        info.reason = QStringLiteral("The placement type \"%1\" is not known.").arg(info.placement);
        return;
    }
    if (po.contains(QLatin1String("exclusion_zone")))
    {
        QJsonObject ez = po.value(QLatin1String("exclusion_zone")).toObject();
        pl.excludeChunks = ez.value(QLatin1String("chunk_count")).toInt();
        // resolved when all sets are known
        exclusionTarget.insert((int) sd.sets.size() - 1, normalizeId(ez.value(QLatin1String("other_set")).toString()));
    }
    info.supported = ok;
    if (!ok && info.reason.isEmpty())
        info.reason = QStringLiteral("The structure set has errors.");
}

// The versions whose structure formats and placement were compared with real
// servers or checked against the game's code (1.19.2 to 1.21.11; 26.1 and
// 26.2 against their servers, 26.1.1 and 26.1.2 have the data of 26.1). 1.18.2
// keeps its structures as configured_structure_feature; 26.3 has not been
// checked.
static bool structureVersionVerified(int mc)
{
    return mc >= MC_1_19_2 && mc <= MC_26_2;
}

void Loader::run()
{
    QStringList structures = w.res.ids(REG_STRUCTURE);
    QStringList sets = w.res.ids(REG_STRUCTURE_SET);
    if (!structureVersionVerified(w.request.mc))
    {
        // listed, not read
        const QString why = QStringLiteral("Structures of Minecraft %1 are not supported yet.")
            .arg(versionName(w.request.mc));
        for (const QString& id : sets)
        {
            sd.setInfo.emplace_back();
            sd.sets.emplace_back();
            sd.setInfo.back().id = id;
            sd.setInfo.back().reason = why;
        }
        return;
    }
    for (const QString& id : structures)
        loadStructure(id);
    for (const QString& id : sets)
        loadSet(id);

    // exclusion zones: the other set, no cycles (the game would recurse
    // without end)
    QHash<QString, int> setIndex;
    for (size_t i = 0; i < sd.setInfo.size(); i++)
        setIndex.insert(sd.setInfo[i].id, (int) i);
    for (size_t i = 0; i < sd.sets.size(); i++)
    {
        PlacementDef& pl = sd.sets[i].placement;
        if (!exclusionTarget.contains((int) i))
            continue;
        QString other = exclusionTarget.value((int) i);
        auto it = setIndex.constFind(other);
        if (it == setIndex.constEnd())
        {
            const Resource *r = w.res.find(REG_STRUCTURE_SET, sd.setInfo[i].id);
            warn(r ? r->pack : -1, sd.setInfo[i].id,
                QStringLiteral("The structure set %1 of the exclusion zone does not exist.").arg(other));
            pl.excludeSet = -1;
            sd.setInfo[i].supported = false;
            sd.setInfo[i].reason = QStringLiteral("Its exclusion zone names a set that does not exist.");
            continue;
        }
        pl.excludeSet = it.value();
        if (pl.excludeChunks < 1 || pl.excludeChunks > 16)
        {
            const Resource *r = w.res.find(REG_STRUCTURE_SET, sd.setInfo[i].id);
            warn(r ? r->pack : -1, sd.setInfo[i].id,
                QStringLiteral("The chunk_count of the exclusion zone is outside of 1 .. 16."));
            sd.setInfo[i].supported = false;
            sd.setInfo[i].reason = QStringLiteral("The structure set has errors.");
        }
    }
    for (size_t i = 0; i < sd.sets.size(); i++)
    {
        int s = (int) i, steps = 0;
        while (s >= 0 && steps <= (int) sd.sets.size())
        {
            s = sd.sets[(size_t) s].placement.excludeSet;
            steps++;
        }
        if (s >= 0)
        {
            const Resource *r = w.res.find(REG_STRUCTURE_SET, sd.setInfo[i].id);
            warn(r ? r->pack : -1, sd.setInfo[i].id,
                QStringLiteral("The exclusion zones of the structure sets form a cycle; the game would crash."));
            sd.setInfo[i].supported = false;
            sd.setInfo[i].reason = QStringLiteral("Its exclusion zones form a cycle.");
        }
    }
    // a set that excludes an unusable set cannot be decided
    for (int pass = 0; pass < (int) sd.sets.size(); pass++)
    {
        bool changed = false;
        for (size_t i = 0; i < sd.sets.size(); i++)
        {
            int o = sd.sets[i].placement.excludeSet;
            if (sd.setInfo[i].supported && o >= 0 && !sd.setInfo[(size_t) o].supported)
            {
                sd.setInfo[i].supported = false;
                sd.setInfo[i].reason = QStringLiteral("Its exclusion zone names a set that is not supported.");
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    for (size_t i = 0; i < sd.setInfo.size(); i++)
        for (int s : sd.setInfo[i].structures)
            if (sd.info[(size_t) s].set.isEmpty())
                sd.info[(size_t) s].set = sd.setInfo[i].id;

    // BiomeTags.REQUIRED_OCEAN_MONUMENT_SURROUNDING
    sd.monumentSurrounding = biomeSet(QStringLiteral("#minecraft:required_ocean_monument_surrounding"));
}

} // namespace

void loadStructures(WorldPrivate& w)
{
    w.structures = StructureData();
    Loader(w).run();
}

//==============================================================================
// finding starts
//==============================================================================

class StructureFinderPrivate
{
public:
    std::shared_ptr<const World> world;
    const WorldPrivate *w = nullptr;
    const StructureData *sd = nullptr;
    int dim = -1;
    uint64_t seed = 0;
    std::unique_ptr<Sampler> sampler;
    // the game's plain climate sampler (RandomState.sampler()), which the
    // structure checks and the stronghold search use
    const DimData *dd = nullptr;
    std::unique_ptr<DensityState> plain;
    int plainHint = -1;
    // plainCell() of recent cells: the structures of a set, the heights of a
    // range and neighbouring areas check the same cells again. The result
    // only depends on the cell (with a tie the biome is not used, and
    // whether there is one does not depend on the search's hint).
    struct PlainCell { int qx, qy, qz, biome; bool tied, used; };
    std::vector<PlainCell> plainCache;
    TerrainHeights *heights = nullptr;
    // the dimension's terrain (the sampler's), exact where aquifers is false
    bool terrain = false, aquifers = false, solidDefault = false;
    bool aquiferBlocks = false;     // the aquifers are modelled: blocks from blockAt()
    std::unordered_map<uint64_t, int> heightCache[2];   // (x, z) -> fillHeight(), surface / floor
    int minY = -64, height = 384, seaLevel = 63;
    int genMinY = -64, genHeight = 384;     // WorldGenerationContext
    int minCellY = -16, maxCellY = 79;
    std::vector<char> possible;             // per set: in ChunkGeneratorStructureState.possibleStructureSets
    // concentric_rings: per set, the positions before the biome search (the
    // random numbers of the rings) and the searched chunk positions
    struct Ring
    {
        int x = 0, z = 0;           // chunk before the search
        uint64_t fork = 0;          // the search's random (since 1.19.3)
        bool searched = false;
        int cx = 0, cz = 0;         // the chunk after the search
    };
    std::vector<int> ringsDone;             // per set: rings laid out (and searched, up to 1.19.2)
    std::vector<std::vector<Ring>> rings;

    int biomeCell(int qx, int qy, int qz)
    {
        return sampler->biomeAt(4, qx, qy * 4, qz);
    }
    int plainQX = INT_MIN, plainQZ = INT_MIN;   // the column the plain state holds
    void plainClimate(int qx, int qy, int qz, int64_t np[6])
    {
        // the column part only depends on x and z: a column scan (a range
        // of heights) evaluates it once
        if (qx != plainQX || qz != plainQZ)
        {
            plain->evalColumn((int) ((unsigned) qx << 2), (int) ((unsigned) qz << 2));
            plainQX = qx;
            plainQZ = qz;
        }
        plain->evalVertical((int) ((unsigned) qy << 2));
        for (int k = 0; k < 6; k++)
            np[k] = quantizeClimate((float) plain->value(dd->plainOutputs[k]));
    }
    int plainCell(int qx, int qy, int qz, bool *tied);
    static bool allowed(const std::vector<char>& mask, int b)
    {
        return b >= 0 && (size_t) b < mask.size() && mask[(size_t) b];
    }
    // The game's getFirstOccupiedHeight() (getBaseHeight() - 1) of a column
    // for a heightmap type as the range lo .. hi it is known to be in; est
    // is the terrain fill's value (if hasEst).
    struct Occupied
    {
        int lo, hi, est;
        bool hasEst;
    };
    Occupied occupied(int x, int z, Heightmap hm);
    Occupied occupiedAt(int x, int z, Heightmap hm);
    int fillHeight(int x, int z, bool floor);
    // both kinds of fillHeight() from one search of the column
    void fillHeights(int x, int z, int *surface, int *floor);

    bool isPlacementChunk(int set, int cx, int cz);
    bool isStructureChunk(int set, int cx, int cz);
    bool frequencyOk(const PlacementDef& pl, int cx, int cz) const;
    std::vector<Ring>& ringLayout(int set);
    void ringSearch(const PlacementDef& pl, Ring& ring, uint64_t *rs);
    void ringPositionsIn(int set, int cx0, int cz0, int cx1, int cz1, std::vector<std::pair<int, int>> *out);
    void evalStart(int structure, int cx, int cz, StructureStart *s);
    void chunk(int set, int cx, int cz, std::vector<StructureStart> *out);
    int checkCell(const std::vector<char>& mask, int qx, int qy, int qz);
    int checkColumn(const std::vector<char>& mask, int x, int z, int y0, int y1);
    void checkAt(const std::vector<char>& mask, int x, int y, int z, StructureStart *s);
    void checkRange(const std::vector<char>& mask, int x, int z, int y0, int y1, bool terrainLeft,
        StructureStart *s);
    void checkHeights(const std::vector<char>& mask, int x, int z, int lo, int hi, int est, bool hasEst,
        bool terrainLeft, StructureStart *s);
    void lowestCorner(const int *xs, const int *zs, Occupied *out);
    void netherFossil(const StructureDef& def, int cx, int cz, StructureStart *s);
    // setHeightsOfInvalid(false): a start whose biome check fails at every
    // height it can have comes without its height
    bool invalidHeights = true;
    bool failsAtAnyHeight(const StructureDef& def, int x, int z, int64_t lo, int64_t hi, StructureStart *s);
    // The biome checks over every height cost more than the terrain where
    // they rarely fail: per structure, the checks made, failed and their
    // time, against the time of the terrain's heights (nanoseconds; only
    // the speed depends on them, not a result the map shows)
    // (moving averages: the first heights of a finder fill its caches)
    struct RangeStats { int64_t seen = 0, tries = 0; double ns = 0, failed = 0; };
    std::vector<RangeStats> rangeStats;
    int64_t heightCalls = 0;
    double heightNs = 0;
    bool jigsaw(const StructureDef& def, int cx, int cz, StructureStart *s);
};

bool StructureFinderPrivate::frequencyOk(const PlacementDef& pl, int cx, int cz) const
{
    if (!(pl.frequency < 1.0F))
        return true;
    uint64_t r;
    switch (pl.reduction)
    {
    case 0:
        // StructurePlacement.probabilityReducer passes (salt, x, z) as (x, z, salt)
        setLargeFeatureWithSalt(&r, seed, pl.salt, cx, cz);
        return nextFloat(&r) < pl.frequency;
    case 1:
    {
        // legacyPillagerOutpostReducer
        int i = cx >> 4, j = cz >> 4;
        setSeed(&r, (uint64_t) (int64_t) (int32_t) ((uint32_t) i ^ ((uint32_t) j << 4)) ^ seed);
        next(&r, 32);
        int bound = javaF2I(1.0F / pl.frequency);
        if (bound <= 0)
            return false;
        return nextInt(&r, bound) == 0;
    }
    case 2:
        // legacyArbitrarySaltProbabilityReducer
        setLargeFeatureWithSalt(&r, seed, cx, cz, 10387320);
        return nextFloat(&r) < pl.frequency;
    default:
        // legacyProbabilityReducerWithDouble
        setLargeFeatureSeed(&r, seed, cx, cz);
        return nextDouble(&r) < (double) pl.frequency;
    }
}

// The biome search of one ring position:
// BiomeSource.findBiomeHorizontal(x*16+8, 0, z*16+8, 112, ...), every cell
// of the square of radius 112 >> 2 around it, z outside, x inside,
// reservoir sampling of the preferred ones with the random @rs.
void StructureFinderPrivate::ringSearch(const PlacementDef& pl, Ring& ring, uint64_t *rs)
{
    const int R = 112 >> 2;
    const int n = 2 * R + 1;
    std::vector<int> cells((size_t) n * n), qxs((size_t) n * n), qzs((size_t) n * n);
    // The game makes these lookups one after the other: each starts its
    // parameter search with the result of the one before (as the engine
    // does here).
    const int qx = (ring.x * 16 + 8) >> 2, qz = (ring.z * 16 + 8) >> 2;
    for (int dz = -R, k = 0; dz <= R; dz++)
        for (int dx = -R; dx <= R; dx++, k++)
        {
            qxs[(size_t) k] = qx + dx;
            qzs[(size_t) k] = qz + dz;
        }
    int foundX = 0, foundZ = 0, matches = 0;
    bool found = false;
    bool ok;
    if (plain && dd->source == DimData::MULTI_NOISE && !dd->tree.empty())
    {
        // a pack's list: the plain sampler's climate, the game's search
        // carrying its last result
        int cursor = -1;
        for (int k = 0; k < n * n; k++)
        {
            int64_t np[6];
            plainClimate(qxs[(size_t) k], 0, qzs[(size_t) k], np);
            int e = dd->tree.find(np, &cursor);
            cells[(size_t) k] = e >= 0 ? dd->entryBiome[(size_t) e] : -1;
        }
        ok = true;
    }
    else
    {
        // the vanilla list: the engine's search (the plain sampler's
        // climate is the chunk's there)
        ok = sampleCellsCarry(*sampler, qxs.data(), qzs.data(), n * n, 0, cells.data());
    }
    if (ok)
    {
        for (int dz = -R; dz <= R; dz++)
        {
            for (int dx = -R; dx <= R; dx++)
            {
                int b = cells[(size_t) (dz + R) * n + (dx + R)];
                if (!allowed(pl.preferred, b))
                    continue;
                if (!found || nextInt(rs, matches + 1) == 0)
                {
                    foundX = (qx + dx) * 4;
                    foundZ = (qz + dz) * 4;
                    found = true;
                }
                matches++;
            }
        }
    }
    ring.searched = true;
    ring.cx = found ? foundX >> 4 : ring.x;
    ring.cz = found ? foundZ >> 4 : ring.z;
}

// ChunkGeneratorStructureState.generateRingPositions: the positions of the
// rings. Since 1.19.3 each biome search has a random of its own, forked from
// the rings' random, so the searches are made when a position is asked for
// (ringPositionsIn()); before, the search used the rings' random itself and
// all of them are made here.
std::vector<StructureFinderPrivate::Ring>& StructureFinderPrivate::ringLayout(int set)
{
    std::vector<Ring>& out = rings[(size_t) set];
    if (ringsDone[(size_t) set])
        return out;
    ringsDone[(size_t) set] = 1;
    const PlacementDef& pl = sd->sets[(size_t) set].placement;
    if (pl.count <= 0 || pl.spread <= 0)
        return out;
    // the seed of the rings is the world seed (createForNormal)
    uint64_t r;
    setSeed(&r, seed);
    double angle = nextDouble(&r) * 3.141592653589793 * 2.0;
    int inRing = 0, ring = 0, spread = pl.spread;
    const bool forked = w->request.mc > MC_1_19_2;
    for (int i = 0; i < pl.count; i++)
    {
        double dist = (double) (4 * pl.distance + pl.distance * ring * 6) +
            (nextDouble(&r) - 0.5) * ((double) pl.distance * 2.5);
        Ring rp;
        rp.x = javaRound(std::cos(angle) * dist);
        rp.z = javaRound(std::sin(angle) * dist);
        if (forked)
            rp.fork = (uint64_t) nextLong(&r);
        else
            ringSearch(pl, rp, &r);
        out.push_back(rp);

        angle += 6.283185307179586 / (double) spread;
        if (++inRing == spread)
        {
            ring++;
            inRing = 0;
            spread += 2 * spread / (ring + 1);
            spread = std::min(spread, pl.count - i);
            angle += nextDouble(&r) * 3.141592653589793 * 2.0;
        }
    }
    return out;
}

// The ring positions (chunks) of a set in the chunks cx0 .. cx1, cz0 .. cz1
// (in the rings' order; a chunk can come more than once)
void StructureFinderPrivate::ringPositionsIn(int set, int cx0, int cz0, int cx1, int cz1,
    std::vector<std::pair<int, int>> *out)
{
    const PlacementDef& pl = sd->sets[(size_t) set].placement;
    // the search moves a position by at most 112 + 15 blocks
    const int M = 8;
    for (Ring& rp : ringLayout(set))
    {
        if (!rp.searched)
        {
            if (rp.x + M < cx0 || rp.x - M > cx1 || rp.z + M < cz0 || rp.z - M > cz1)
                continue;
            uint64_t fork;
            setSeed(&fork, rp.fork);
            ringSearch(pl, rp, &fork);
        }
        if (rp.cx >= cx0 && rp.cx <= cx1 && rp.cz >= cz0 && rp.cz <= cz1)
            out->emplace_back(rp.cx, rp.cz);
    }
}

// RandomSpreadStructurePlacement.getPotentialStructureChunk of the region
// (rx, rz)
static void spreadChunk(const PlacementDef& pl, uint64_t seed, int rx, int rz, int *cx, int *cz)
{
    uint64_t r;
    setLargeFeatureWithSalt(&r, seed, rx, rz, pl.salt);
    const int range = pl.spacing - pl.separation;
    int ox, oz;
    if (pl.triangular)
    {
        ox = (nextInt(&r, range) + nextInt(&r, range)) / 2;
        oz = (nextInt(&r, range) + nextInt(&r, range)) / 2;
    }
    else
    {
        ox = nextInt(&r, range);
        oz = nextInt(&r, range);
    }
    *cx = rx * pl.spacing + ox;
    *cz = rz * pl.spacing + oz;
}

bool StructureFinderPrivate::isPlacementChunk(int set, int cx, int cz)
{
    const PlacementDef& pl = sd->sets[(size_t) set].placement;
    if (pl.kind == PlacementDef::RANDOM_SPREAD)
    {
        int x, z;
        spreadChunk(pl, seed, floorDiv(cx, pl.spacing), floorDiv(cz, pl.spacing), &x, &z);
        return x == cx && z == cz;
    }
    if (pl.kind == PlacementDef::CONCENTRIC_RINGS)
    {
        // no positions for a set that is not possible in the dimension
        if (!possible[(size_t) set])
            return false;
        std::vector<std::pair<int, int>> in;
        ringPositionsIn(set, cx, cz, cx, cz, &in);
        return !in.empty();
    }
    return false;
}

// StructurePlacement.isStructureChunk
bool StructureFinderPrivate::isStructureChunk(int set, int cx, int cz)
{
    const PlacementDef& pl = sd->sets[(size_t) set].placement;
    if (!isPlacementChunk(set, cx, cz) || !frequencyOk(pl, cx, cz))
        return false;
    if (pl.excludeSet >= 0)
    {
        // ExclusionZone: any placement of the other set within the square
        const int n = pl.excludeChunks;
        for (int x = cx - n; x <= cx + n; x++)
            for (int z = cz - n; z <= cz + n; z++)
                if (isStructureChunk(pl.excludeSet, x, z))
                    return false;
    }
    return true;
}

// The biome of a cell as a lookup of the game's plain sampler without a
// known history sees it (Structure.isValidBiome calls the biome source
// directly, on a worker thread). @tied: entries of several biomes are
// nearest to the climate (they overlap or tie); the game's result then
// depends on the lookup that thread made before, so it may be either.
int StructureFinderPrivate::plainCell(int qx, int qy, int qz, bool *tied)
{
    *tied = false;
    if (!plain)
        return biomeCell(qx, qy, qz);
    const bool multi = dd->source == DimData::MULTI_NOISE;
    const ClimateTree& tree = multi ? dd->tree : dd->nativeTree;
    if (tree.empty())
        return biomeCell(qx, qy, qz);
    if (plainCache.empty())
        plainCache.resize(1u << 14);
    const uint64_t key = (uint64_t) (uint32_t) qx * 0x9E3779B97F4A7C15ull ^ (uint64_t) (uint32_t) qz * 0xC2B2AE3D27D4EB4Full
        ^ (uint64_t) (uint32_t) qy * 0x165667B19E3779F9ull;
    PlainCell& e = plainCache[(size_t) (key >> 50) & (plainCache.size() - 1)];
    if (e.used && e.qx == qx && e.qy == qy && e.qz == qz)
    {
        *tied = e.tied;
        return e.biome;
    }
    int64_t np[6];
    plainClimate(qx, qy, qz, np);
    ClimateTree::Nearest n = tree.findNearest(np, &plainHint);
    if (n.entry < 0)
        return -1;
    *tied = n.rival;
    int b;
    if (multi)
        b = dd->entryBiome[(size_t) n.entry];
    else
    {
        // the engine's list; where the engine decides the biome later (and
        // in the vanilla list the plain sampler sees what the chunk does)
        int id = dd->nativeGroups[(size_t) n.entry];
        b = id >= 0 && id < 256 ? dd->nativeBiome[id] : biomeCell(qx, qy, qz);
    }
    e = { qx, qy, qz, b, *tied, true };
    return b;
}

// The biome check of one cell: 1 allowed, 0 not, -1 either (tied).
int StructureFinderPrivate::checkCell(const std::vector<char>& mask, int qx, int qy, int qz)
{
    bool tied;
    int b = plainCell(qx, qy, qz, &tied);
    if (tied)
        return -1;
    return allowed(mask, b) ? 1 : 0;
}

// Whether the biome at x, z is allowed at all heights y0 .. y1 (blocks):
// 1 all, 0 none, -1 some, -2 undecided because of ties.
int StructureFinderPrivate::checkColumn(const std::vector<char>& mask, int x, int z, int y0, int y1)
{
    int q0 = std::max(minCellY, y0 >> 2), q1 = std::min(maxCellY, y1 >> 2);
    if (q0 > q1)
        q0 = q1 = std::max(minCellY, std::min(maxCellY, y0 >> 2));
    bool yes = false, no = false, tie = false;
    for (int q = q0; q <= q1; q++)
    {
        int c = checkCell(mask, x >> 2, q, z >> 2);
        if (c > 0)
            yes = true;
        else if (c == 0)
            no = true;
        else
            tie = true;
        if (yes && no)
            return -1;
    }
    return tie ? -2 : yes ? 1 : 0;
}

void StructureFinderPrivate::checkAt(const std::vector<char>& mask, int x, int y, int z, StructureStart *s)
{
    s->x = x;
    s->y = y;
    s->z = z;
    s->hasY = true;
    s->estimatedY = false;
    int c = checkCell(mask, x >> 2, y >> 2, z >> 2);
    s->detail = c < 0 ? StructureStart::TiedBiome : StructureStart::Checked;
    s->state = c < 0 ? StructureStart::Unknown : c ? StructureStart::Valid : StructureStart::Invalid;
}

// The start's height is one of y0 .. y1. @terrainLeft: a terrain check the
// game makes before the biome check could not be made.
void StructureFinderPrivate::checkRange(const std::vector<char>& mask, int x, int z, int y0, int y1,
    bool terrainLeft, StructureStart *s)
{
    s->x = x;
    s->z = z;
    s->hasY = false;
    s->estimatedY = false;
    int c = checkColumn(mask, x, z, y0, y1);
    if (c == 0)
    {
        s->state = StructureStart::Invalid;
        s->detail = StructureStart::Checked;
    }
    else if (c < 0)
    {
        s->state = StructureStart::Unknown;
        s->detail = c == -1 ? StructureStart::NeedsHeight : StructureStart::TiedBiome;
    }
    else if (terrainLeft)
    {
        s->state = StructureStart::Unknown;
        s->detail = StructureStart::TerrainCheck;
    }
    else
    {
        s->state = StructureStart::Valid;
        s->detail = StructureStart::Checked;
    }
}

// The terrain fill's height of a column (Sampler::surfaceHeight(); the
// dimension's bottom - 1 for a column without blocks, as the game's
// getBaseHeight() falls back to the bottom)
int StructureFinderPrivate::fillHeight(int x, int z, bool floor)
{
    std::unordered_map<uint64_t, int>& cache = heightCache[floor ? 1 : 0];
    const uint64_t key = (uint64_t) (uint32_t) x << 32 | (uint32_t) z;
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    int h = sampler->surfaceHeight(x, z, floor ? HEIGHT_OCEAN_FLOOR : HEIGHT_SURFACE);
    if (h == NO_HEIGHT)
        h = minY - 1;
    if (cache.size() >= (1u << 18))
        cache.clear();
    cache.emplace(key, h);
    return h;
}

void StructureFinderPrivate::fillHeights(int x, int z, int *surface, int *floor)
{
    const uint64_t key = (uint64_t) (uint32_t) x << 32 | (uint32_t) z;
    auto s = heightCache[0].find(key), f = heightCache[1].find(key);
    if (s != heightCache[0].end() && f != heightCache[1].end())
    {
        *surface = s->second;
        *floor = f->second;
        return;
    }
    int h[2] = { NO_HEIGHT, NO_HEIGHT };
    if (!sampler->genBothHeights(&h[1], &h[0], 1, x, z, 1, 1))
        h[0] = h[1] = NO_HEIGHT;
    for (int k = 0; k < 2; k++)
    {
        if (h[k] == NO_HEIGHT)
            h[k] = minY - 1;
        if (heightCache[k].size() >= (1u << 18))
            heightCache[k].clear();
        heightCache[k].emplace(key, h[k]);
    }
    *surface = h[0];
    *floor = h[1];
}

StructureFinderPrivate::Occupied StructureFinderPrivate::occupied(int x, int z, Heightmap hm)
{
    if (invalidHeights)
        return occupiedAt(x, z, hm);
    const auto t0 = std::chrono::steady_clock::now();
    const Occupied o = occupiedAt(x, z, hm);
    const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count();
    heightNs += (ns - heightNs) * (heightCalls < 64 ? 1.0 / (double) (heightCalls + 1) : 1.0 / 64);
    heightCalls++;
    return o;
}

StructureFinderPrivate::Occupied StructureFinderPrivate::occupiedAt(int x, int z, Heightmap hm)
{
    Occupied o { minY - 1, minY + height - 1, minY - 1, false };
    if (heights)
    {
        int h = heights->baseHeight(x, z, hm);
        if (h != INT_MIN)
            o.lo = o.hi = o.est = h - 1, o.hasEst = true;
        return o;
    }
    if (!terrain)
        return o;
    // Heightmap.Types.isOpaque() on the blocks of a noise column (the
    // default block, fluids, air): WORLD_SURFACE(_WG) and MOTION_BLOCKING
    // (_NO_LEAVES) count every block that is not air, OCEAN_FLOOR(_WG) the
    // ones that block motion (not fluids)
    const bool floor = hm == HM_OCEAN_FLOOR_WG || hm == HM_OCEAN_FLOOR;
    o.hasEst = true;
    if (!aquifers)
    {
        o.est = fillHeight(x, z, floor);
        o.lo = o.hi = o.est;
        return o;
    }
    // With aquifers the blocks where the density is not positive may be
    // fluid, air or a barrier (stone) whatever the global levels say. The
    // highest block of positive density stays: it is the lower bound.
    if (floor)
        o.lo = o.est = fillHeight(x, z, true);
    else
        fillHeights(x, z, &o.est, &o.lo);
    return o;
}

// The biome check of a start at x, z whose y is one of lo .. hi (exactly
// known if lo == hi); @terrainLeft: a terrain check the game makes before
// the biome check could not be decided.
void StructureFinderPrivate::checkHeights(const std::vector<char>& mask, int x, int z, int lo, int hi, int est,
    bool hasEst, bool terrainLeft, StructureStart *s)
{
    if (lo == hi && !terrainLeft)
    {
        checkAt(mask, x, lo, z, s);
        return;
    }
    checkRange(mask, x, z, lo, hi, terrainLeft, s);
    if (hasEst)
    {
        s->y = est;
        s->estimatedY = true;
    }
}

// Structure.getLowestY() over the four corners: the lowest
// getFirstOccupiedHeight(WORLD_SURFACE_WG)
void StructureFinderPrivate::lowestCorner(const int *xs, const int *zs, Occupied *out)
{
    *out = Occupied { INT_MAX, INT_MAX, INT_MAX, true };
    for (int i = 0; i < 4; i++)
    {
        Occupied o = occupied(xs[i], zs[i], HM_WORLD_SURFACE_WG);
        out->lo = std::min(out->lo, o.lo);
        out->hi = std::min(out->hi, o.hi);
        out->est = std::min(out->est, o.est);
        out->hasEst = out->hasEst && o.hasEst;
    }
}

namespace
{

struct Box
{
    int x0, y0, z0, x1, y1, z1;
};

// StructureTemplate.transform with no mirror and the pivot 0, 0
inline void rotateXZ(int rot, int x, int z, int *ox, int *oz)
{
    switch (rot)
    {
    case 1: *ox = -z; *oz = x; break;       // CLOCKWISE_90
    case 2: *ox = -x; *oz = -z; break;      // CLOCKWISE_180
    case 3: *ox = z; *oz = -x; break;       // COUNTERCLOCKWISE_90
    default: *ox = x; *oz = z; break;
    }
}

// StructureTemplate.getBoundingBox(pos, rotation, pivot 0, no mirror)
Box templateBox(const Template& t, int rot, int px, int py, int pz)
{
    int bx, bz;
    rotateXZ(rot, t.sx - 1, t.sz - 1, &bx, &bz);
    const int by = t.sy - 1;
    Box b;
    b.x0 = std::min(0, bx) + px; b.x1 = std::max(0, bx) + px;
    b.y0 = std::min(0, by) + py; b.y1 = std::max(0, by) + py;
    b.z0 = std::min(0, bz) + pz; b.z1 = std::max(0, bz) + pz;
    return b;
}

bool elementBox(const StructureData& sd, const PoolElement& e, int rot, int px, int py, int pz, Box *out)
{
    switch (e.kind)
    {
    case PoolElement::SINGLE:
        *out = templateBox(sd.templates[(size_t) e.tmpl], rot, px, py, pz);
        return true;
    case PoolElement::FEATURE:
        *out = { px, py, pz, px, py, pz };
        return true;
    case PoolElement::LIST:
    {
        // BoundingBox.encapsulatingBoxes of the non-empty elements
        bool any = false;
        for (const PoolElement& c : e.elements)
        {
            Box b;
            if (c.kind == PoolElement::EMPTY || !elementBox(sd, c, rot, px, py, pz, &b))
                continue;
            if (!any)
                *out = b;
            else
            {
                out->x0 = std::min(out->x0, b.x0); out->y0 = std::min(out->y0, b.y0); out->z0 = std::min(out->z0, b.z0);
                out->x1 = std::max(out->x1, b.x1); out->y1 = std::max(out->y1, b.y1); out->z1 = std::max(out->z1, b.z1);
            }
            any = true;
        }
        return any;
    }
    default:
        return false;
    }
}

// The jigsaw blocks of an element in the order getShuffledJigsawBlocks()
// gives them (positions relative to the element's position).
void elementJigsaws(const StructureData& sd, const PoolElement& e, int rot, int px, int py, int pz,
    uint64_t *rng, std::vector<Template::Jigsaw> *out)
{
    out->clear();
    if (e.kind == PoolElement::LIST)
    {
        if (!e.elements.empty())
            elementJigsaws(sd, e.elements[0], rot, px, py, pz, rng, out);
        return;
    }
    if (e.kind == PoolElement::FEATURE)
    {
        // FeaturePoolElement: one jigsaw "minecraft:bottom" at the position
        Template::Jigsaw j;
        j.name = QStringLiteral("minecraft:bottom");
        out->push_back(j);
        return;
    }
    if (e.kind != PoolElement::SINGLE)
        return;
    const Template& t = sd.templates[(size_t) e.tmpl];
    if (t.palettes.empty())
        return;
    size_t pal = 0;
    if (t.palettes.size() > 1)
    {
        // StructurePlaceSettings.getRandomPalette: a random seeded with
        // Mth.getSeed(pos)
        int64_t l = (int64_t) ((uint64_t) (int64_t) (int32_t) ((uint32_t) px * 3129871u) ^
            ((uint64_t) (int64_t) pz * 116129781ULL) ^ (uint64_t) (int64_t) py);
        l = (int64_t) ((uint64_t) l * (uint64_t) l * 42317861ULL + (uint64_t) l * 11ULL);
        uint64_t r;
        setSeed(&r, (uint64_t) (l >> 16));
        pal = (size_t) nextInt(&r, (int) t.palettes.size());
    }
    for (const Template::Jigsaw& j : t.palettes[pal])
    {
        Template::Jigsaw k = j;
        rotateXZ(rot, j.x, j.z, &k.x, &k.z);
        out->push_back(k);
    }
    // Util.shuffle, then sortBySelectionPriority (stable, highest first)
    for (int i = (int) out->size(); i > 1; i--)
    {
        int k = nextInt(rng, i);
        std::swap((*out)[(size_t) i - 1], (*out)[(size_t) k]);
    }
    std::stable_sort(out->begin(), out->end(), [](const Template::Jigsaw& a, const Template::Jigsaw& b) {
        return a.priority > b.priority;
    });
}

} // namespace

// JigsawStructure.findGenerationPoint and JigsawPlacement.addPieces up to
// the start position. Returns false if the game finds no start.
// For setHeightsOfInvalid(false): if the biome check of a start at x, z
// fails in every cell of the heights lo .. hi (the cells the game would
// check, not clamped to the dimension), the start is not generated whatever
// the terrain's height is: sets @s to that without asking the terrain and
// returns true. Only where the terrain would be asked.
bool StructureFinderPrivate::failsAtAnyHeight(const StructureDef& def, int x, int z, int64_t lo, int64_t hi,
    StructureStart *s)
{
    if (invalidHeights || !terrain || heights)
        return false;
    const int64_t q0 = lo >> 2, q1 = hi >> 2;
    if (q1 - q0 > 1024 || q0 < INT_MIN / 2 || q1 > INT_MAX / 2)
        return false;
    const size_t si = (size_t) (&def - sd->defs.data());
    if (rangeStats.size() != sd->defs.size())
        rangeStats.assign(sd->defs.size(), RangeStats());
    RangeStats& st = rangeStats[si];
    // after a few tries (and then for every 64th start): only where the
    // expected saving, failed / tries times the height's time, is more than
    // the time of the checks
    st.seen++;
    if (st.tries >= 16 && st.seen % 64 != 0 && heightCalls > 0 && st.ns >= st.failed * heightNs)
        return false;
    const auto t0 = std::chrono::steady_clock::now();
    // from the top: above the terrain a column usually has one biome, so a
    // start that passes is found after a few cells
    bool fails = true;
    for (int64_t q = q1; q >= q0 && fails; q--)
        fails = checkCell(def.biomes, x >> 2, (int) q, z >> 2) == 0;
    const double ns = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count();
    const double w = st.tries < 16 ? 1.0 / (double) (st.tries + 1) : 1.0 / 16;
    st.ns += (ns - st.ns) * w;
    st.failed += ((fails ? 1.0 : 0.0) - st.failed) * w;
    st.tries++;
    if (!fails)
        return false;
    s->x = x;
    s->z = z;
    s->y = 0;
    s->hasY = false;
    s->estimatedY = false;
    s->state = StructureStart::Invalid;
    s->detail = StructureStart::Checked;
    return true;
}

bool StructureFinderPrivate::jigsaw(const StructureDef& def, int cx, int cz, StructureStart *s)
{
    uint64_t r;
    setLargeFeatureSeed(&r, seed, cx, cz);
    const int startY = def.startHeight.sample(&r, genMinY, genHeight);
    const int px = cx * 16, pz = cz * 16;
    const int rot = nextInt(&r, 4);
    const int pick = nextInt(&r, def.poolTotal);
    const size_t ei = (size_t) (std::upper_bound(def.poolCumulative.begin(), def.poolCumulative.end(), pick) -
        def.poolCumulative.begin());
    const PoolElement& e = def.pool[ei];
    if (e.kind == PoolElement::EMPTY)
        return false;
    int vx = 0, vy = 0, vz = 0;
    if (!def.startJigsawName.isEmpty())
    {
        std::vector<Template::Jigsaw> js;
        elementJigsaws(*sd, e, rot, px, startY, pz, &r, &js);
        bool found = false;
        for (const Template::Jigsaw& j : js)
        {
            if (j.name == def.startJigsawName)
            {
                vx = j.x; vy = j.y; vz = j.z;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    Box b;
    if (!elementBox(*sd, e, rot, px - vx, startY - vy, pz - vz, &b))
        return false;
    const int x = javaMid(b.x0, b.x1), z = javaMid(b.z0, b.z1);
    // JigsawPlacement.addPieces moves the start piece so that its box starts
    // at k - groundLevelDelta (1 for every element type), k the height of
    // the piece's position; with dimension_padding (1.21.4 and later) the
    // moved box must keep the padding to the bottom and the top of the world
    // (isStartTooCloseToWorldHeightLimits), else there is no start. The
    // heights of k that pass: kMin .. kMax.
    const int64_t span = (int64_t) b.y1 - b.y0;
    const int64_t kMin = (int64_t) minY + def.padBottom + 1;
    const int64_t kMax = (int64_t) minY + height - 1 - def.padTop + 1 - span;
    if (!def.projected)
    {
        // k is the position's height, the start height minus the start
        // jigsaw's y
        const int64_t k = (int64_t) startY - vy;
        if (def.padding && (k < kMin || k > kMax))
            return false;
        checkAt(def.biomes, x, startY, z, s);
        return true;
    }
    // JigsawPlacement.addPieces: y = start height + getFirstFreeHeight() +
    // the start jigsaw's y (getFirstFreeHeight() = getBaseHeight()), k = y
    // without the jigsaw's y
    const int add = startY + 1 + vy;
    // (with padding the height decides whether there is a start at all)
    if (!def.padding && failsAtAnyHeight(def, x, z, (int64_t) minY - 1 + add, (int64_t) minY + height - 1 + add, s))
        return true;
    const Occupied o = occupied(x, z, def.heightmap);
    int lo = o.lo, hi = o.hi;
    bool left = false;
    if (def.padding)
    {
        // the occupied heights whose k = startY + 1 + occupied passes
        const int64_t oMin = kMin - startY - 1, oMax = kMax - startY - 1;
        if (hi < oMin || lo > oMax)
            return false;
        if (lo < oMin || hi > oMax)
        {
            // only some of the possible heights pass: unknown
            lo = (int) std::max<int64_t>(lo, oMin);
            hi = (int) std::min<int64_t>(hi, oMax);
            left = true;
        }
    }
    checkHeights(def.biomes, x, z, lo + add, hi + add, o.est + add, o.hasEst, left, s);
    return true;
}

void StructureFinderPrivate::evalStart(int structure, int cx, int cz, StructureStart *s)
{
    const StructureDef& def = sd->defs[(size_t) structure];
    s->structure = structure;
    s->chunkX = cx;
    s->chunkZ = cz;
    const int x0 = cx * 16, z0 = cz * 16;
    const int top = minY + height - 1;
    switch (def.kind)
    {
    case StructureDef::JIGSAW:
        if (!jigsaw(def, cx, cz, s))
        {
            s->x = x0;
            s->z = z0;
            s->hasY = false;
            s->state = StructureStart::Invalid;
            s->detail = StructureStart::Checked;
        }
        return;
    case StructureDef::STRONGHOLD:
        checkAt(def.biomes, x0, 0, z0, s);
        return;
    case StructureDef::FORTRESS:
        checkAt(def.biomes, x0, 64, z0, s);
        return;
    case StructureDef::RUINED_PORTAL:
        // the height comes from the terrain and the placement of the setup
        checkRange(def.biomes, x0, z0, minY, top, false, s);
        return;
    case StructureDef::MINESHAFT:
        // y 50, moved by the layout of its pieces
        checkRange(def.biomes, x0 + 8, z0, minY, top, false, s);
        return;
    case StructureDef::MANSION:
    case StructureDef::END_CITY:
    {
        // Structure.getLowestYIn5by5BoxOffset7Blocks: the lowest corner of
        // the 5 x 5 box the rotation points to; below 60 no start
        uint64_t r;
        setLargeFeatureSeed(&r, seed, cx, cz);
        int rot = nextInt(&r, 4);
        int w = 5, d = 5;
        if (rot == 1)
            w = -5;
        else if (rot == 2)
            w = -5, d = -5;
        else if (rot == 3)
            d = -5;
        const int x = x0 + 7, z = z0 + 7;
        const int xs[4] = { x, x, x + w, x + w }, zs[4] = { z, z + d, z, z + d };
        Occupied low;
        lowestCorner(xs, zs, &low);
        if (low.hi < 60)
        {
            s->x = x; s->z = z;
            s->hasY = low.lo == low.hi;
            s->estimatedY = !s->hasY && low.hasEst;
            s->y = s->hasY ? low.hi : s->estimatedY ? low.est : 0;
            s->state = StructureStart::Invalid;
            s->detail = StructureStart::Checked;
            return;
        }
        if (low.lo >= 60)
            checkHeights(def.biomes, x, z, low.lo, low.hi, low.est, low.hasEst, false, s);
        else
            checkHeights(def.biomes, x, z, 60, low.hi, low.est, low.hasEst, true, s);
        return;
    }
    case StructureDef::SINGLE_PIECE:
    {
        // SinglePieceStructure: no start if getLowestY() over the
        // footprint's corners is below the sea level, then on top of the
        // chunk's centre (WORLD_SURFACE_WG). The biome check first: it
        // needs one height instead of four.
        if (failsAtAnyHeight(def, x0 + 8, z0 + 8, minY - 1, top, s))
            return;
        const Occupied c = occupied(x0 + 8, z0 + 8, def.heightmap);
        checkHeights(def.biomes, x0 + 8, z0 + 8, c.lo, c.hi, c.est, c.hasEst, false, s);
        if (s->state == StructureStart::Invalid)
            return;
        const int xs[4] = { x0, x0, x0 + def.width, x0 + def.width };
        const int zs[4] = { z0, z0 + def.depth, z0, z0 + def.depth };
        Occupied low;
        lowestCorner(xs, zs, &low);
        if (low.hi < seaLevel)
        {
            s->state = StructureStart::Invalid;
            s->detail = StructureStart::Checked;
        }
        else if (low.lo < seaLevel && s->state == StructureStart::Valid)
        {
            s->state = StructureStart::Unknown;
            s->detail = StructureStart::TerrainCheck;
        }
        return;
    }
    case StructureDef::MONUMENT:
    {
        // OceanMonumentStructure: every biome around the centre at sea
        // level must be in the tag (BiomeSource.getBiomesWithin), then the
        // biome check on the ocean floor at the chunk's centre. Both have to
        // pass, so the cheap one first: if it fails, the box does not matter.
        if (failsAtAnyHeight(def, x0 + 8, z0 + 8, minY - 1, top, s))
            return;
        const Occupied c = occupied(x0 + 8, z0 + 8, def.heightmap);
        checkHeights(def.biomes, x0 + 8, z0 + 8, c.lo, c.hi, c.est, c.hasEst, false, s);
        if (s->state == StructureStart::Invalid)
            return;
        const int bx = x0 + 9, bz = z0 + 9, R = 29;
        const int qx0 = (bx - R) >> 2, qx1 = (bx + R) >> 2;
        const int qy0 = (seaLevel - R) >> 2, qy1 = (seaLevel + R) >> 2;
        const int qz0 = (bz - R) >> 2, qz1 = (bz + R) >> 2;
        const int nx = qx1 - qx0 + 1, nz = qz1 - qz0 + 1;
        bool ok = true, tie = false;
        for (int qy = qy0; qy <= qy1 && ok; qy++)
            for (int j = 0; j < nz && ok; j++)
                for (int i = 0; i < nx && ok; i++)
                {
                    int k = checkCell(sd->monumentSurrounding, qx0 + i, qy, qz0 + j);
                    if (k < 0)
                        tie = true;     // may still be a biome of the tag
                    else if (k == 0)
                        ok = false;
                }
        if (!ok)
        {
            s->state = StructureStart::Invalid;
            s->detail = StructureStart::Checked;
        }
        else if (tie)
        {
            s->state = StructureStart::Unknown;
            s->detail = StructureStart::TiedBiome;
        }
        return;
    }
    case StructureDef::CHUNK_CENTER:
    {
        // Structure.onTopOfChunkCenter: getFirstOccupiedHeight() at (8, 8)
        if (failsAtAnyHeight(def, x0 + 8, z0 + 8, minY - 1, top, s))
            return;
        const Occupied c = occupied(x0 + 8, z0 + 8, def.heightmap);
        checkHeights(def.biomes, x0 + 8, z0 + 8, c.lo, c.hi, c.est, c.hasEst, false, s);
        return;
    }
    case StructureDef::NETHER_FOSSIL:
        netherFossil(def, cx, cz, s);
        return;
    default:
        s->x = x0; s->z = z0; s->hasY = false;
        s->state = StructureStart::Unknown;
        s->detail = StructureStart::NotSupported;
        return;
    }
}

// NetherFossilStructure.findGenerationPoint: a random column of the chunk;
// from the height the provider gives, down to the first air block above a
// solid one (the noise column: getBaseColumn()), which must be above the
// sea level. The biome check is at that solid block.
void StructureFinderPrivate::netherFossil(const StructureDef& def, int cx, int cz, StructureStart *s)
{
    uint64_t r;
    setLargeFeatureSeed(&r, seed, cx, cz);
    const int x = cx * 16 + nextInt(&r, 16);
    const int z = cz * 16 + nextInt(&r, 16);
    int y = def.startHeight.sample(&r, genMinY, genHeight);
    s->x = x;
    s->z = z;
    s->hasY = false;
    if (y <= seaLevel)
    {
        s->state = StructureStart::Invalid;
        s->detail = StructureStart::Checked;
        return;
    }
    // The column is exact where there are no aquifers: above the sea level
    // a block is the default block (solid, its top face sturdy) or air.
    // Soul sand only comes with the surface rules, later. Without it the
    // start is one of the heights above the sea level up to y, or there is
    // none (the search may find no solid block above the sea level).
    if (!terrain || aquifers || !solidDefault || heights)
    {
        checkRange(def.biomes, x, z, seaLevel + 1, y, true, s);
        return;
    }
    checkRange(def.biomes, x, z, seaLevel + 1, y, false, s);
    if (s->state == StructureStart::Invalid)
        return;
    bool found = false;
    while (y > seaLevel)
    {
        // with aquifers: air (not a fluid) above a solid block
        const bool air = aquiferBlocks ? blockAt(*sampler, x, y, z) == 0 : !sampler->solidAt(x, y, z);
        y--;
        if (air && (aquiferBlocks ? blockAt(*sampler, x, y, z) == 2 : sampler->solidAt(x, y, z)))
        {
            found = true;
            break;
        }
    }
    if (!found || y <= seaLevel)
    {
        s->state = StructureStart::Invalid;
        s->detail = StructureStart::Checked;
        return;
    }
    checkAt(def.biomes, x, y, z, s);
}

// ChunkGenerator.createStructures for one set and one placement chunk
void StructureFinderPrivate::chunk(int set, int cx, int cz, std::vector<StructureStart> *out)
{
    const StructureSetDef& sdef = sd->sets[(size_t) set];
    // the order in which the game tries the structures (the draws do not
    // depend on which of them fail)
    std::vector<int> order;
    if (sdef.entries.size() == 1)
    {
        order.push_back(sdef.entries[0].first);
    }
    else
    {
        std::vector<std::pair<int, int>> list = sdef.entries;
        uint64_t r;
        setLargeFeatureSeed(&r, seed, cx, cz);
        int total = 0;
        for (const auto& e : list)
            total += e.second;
        while (!list.empty() && total > 0)
        {
            int j = nextInt(&r, total);
            size_t k = 0;
            for (; k < list.size(); k++)
            {
                j -= list[k].second;
                if (j < 0)
                    break;
            }
            if (k >= list.size())
                k = list.size() - 1;
            order.push_back(list[k].first);
            total -= list[k].second;
            list.erase(list.begin() + (long) k);
        }
    }
    bool earlierUnknown = false;
    for (int st : order)
    {
        if (!sd->info[(size_t) st].supported)
        {
            earlierUnknown = true;
            continue;
        }
        StructureStart s;
        s.set = set;
        evalStart(st, cx, cz, &s);
        const bool valid = s.state == StructureStart::Valid;
        if (earlierUnknown && valid)
        {
            s.state = StructureStart::Unknown;
            s.detail = StructureStart::EarlierUnknown;
        }
        if (s.state == StructureStart::Unknown)
            earlierUnknown = true;
        out->push_back(s);
        if (valid)
            break;
    }
}

StructureFinder::StructureFinder(std::shared_ptr<const World> world, int dimension, uint64_t seed)
    : p(new StructureFinderPrivate)
{
    p->world = world;
    if (!world)
        return;
    const WorldPrivate *w = world->d();
    if (dimension < 0 || (size_t) dimension >= w->dims.size() || !w->dims[(size_t) dimension].supported)
        return;
    p->sampler.reset(new Sampler(world, dimension, seed));
    if (!p->sampler->valid())
    {
        p->sampler.reset();
        return;
    }
    p->w = w;
    p->sd = &w->structures;
    p->dim = dimension;
    p->seed = seed;
    const DimensionInfo& info = w->dims[(size_t) dimension];
    const DimData& dd = w->data[(size_t) dimension];
    p->minY = info.minY;
    p->height = info.height;
    p->seaLevel = info.seaLevel;
    p->genMinY = std::max(info.minY, dd.noiseMinY);
    p->genHeight = std::min(info.height, dd.noiseHeight);
    p->minCellY = dd.minCellY;
    p->maxCellY = dd.maxCellY;
    p->dd = &dd;
    p->terrain = p->sampler->hasTerrain();
    // the aquifers: exact where they are modelled (the heights of the
    // game's getBaseHeight(), which fills a single noise cell)
    setBaseColumns(*p->sampler, true);
    p->aquifers = dd.terrain.aquifers && !hasAquifers(*p->sampler);
    p->aquiferBlocks = hasAquifers(*p->sampler);
    p->solidDefault = dd.terrain.defaultBlock == TerrainDef::SOLID;
    if ((dd.source == DimData::MULTI_NOISE || dd.source == DimData::NATIVE_OVERWORLD) && w->graph &&
        dd.plainOutputs[0] >= 0)
        p->plain.reset(new DensityState(*w->graph, dd.plainProgram, seed, w->request.mc, dd.legacy));

    // ChunkGeneratorStructureState.possibleStructureSets: the sets with a
    // structure that can be in one of the dimension's biomes
    const size_t n = p->sd->sets.size();
    p->possible.assign(n, 0);
    p->ringsDone.assign(n, 0);
    p->rings.resize(n);
    for (size_t i = 0; i < n; i++)
    {
        for (const auto& e : p->sd->sets[i].entries)
        {
            const std::vector<char>& mask = p->sd->defs[(size_t) e.first].biomes;
            for (int b : info.biomes)
                if (StructureFinderPrivate::allowed(mask, b))
                    p->possible[i] = 1;
        }
    }
}

StructureFinder::~StructureFinder()
{
    delete p;
}

void StructureFinder::setHeightsOfInvalid(bool exact)
{
    p->invalidHeights = exact;
}

bool StructureFinder::valid() const
{
    return p->sd != nullptr;
}

bool StructureFinder::starts(int cx0, int cz0, int cx1, int cz1, std::vector<StructureStart> *out,
    const std::atomic_bool *abort, const std::vector<char> *structures)
{
    if (!p->sd)
        return false;
    // long scans inside the density functions (climate, terrain) check the
    // flag too
    DensityState *climate = nullptr, *terrain = nullptr;
    if (p->sampler)
        samplerStates(*p->sampler, &climate, &terrain);
    AbortScope scopePlain(p->plain.get(), abort), scopeClimate(climate, abort), scopeTerrain(terrain, abort);
    for (size_t set = 0; set < p->sd->sets.size(); set++)
    {
        if (abort && abort->load(std::memory_order_relaxed))
            return false;
        if (!p->possible[set] || !p->sd->setInfo[set].supported)
            continue;
        if (structures && std::none_of(p->sd->setInfo[set].structures.begin(),
                p->sd->setInfo[set].structures.end(), [structures](int s) {
                    return s >= 0 && s < (int) structures->size() && (*structures)[(size_t) s];
                }))
            continue;
        const PlacementDef& pl = p->sd->sets[set].placement;
        std::vector<std::pair<int, int>> chunks;
        if (pl.kind == PlacementDef::RANDOM_SPREAD)
        {
            const int rx0 = floorDiv(cx0, pl.spacing), rx1 = floorDiv(cx1, pl.spacing);
            const int rz0 = floorDiv(cz0, pl.spacing), rz1 = floorDiv(cz1, pl.spacing);
            for (int rz = rz0; rz <= rz1; rz++)
            {
                if (abort && abort->load(std::memory_order_relaxed))
                    return false;
                for (int rx = rx0; rx <= rx1; rx++)
                {
                    int x, z;
                    spreadChunk(pl, p->seed, rx, rz, &x, &z);
                    if (x >= cx0 && x <= cx1 && z >= cz0 && z <= cz1)
                        chunks.emplace_back(x, z);
                }
            }
        }
        else if (pl.kind == PlacementDef::CONCENTRIC_RINGS)
        {
            p->ringPositionsIn((int) set, cx0, cz0, cx1, cz1, &chunks);
            std::sort(chunks.begin(), chunks.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                return a.second != b.second ? a.second < b.second : a.first < b.first;
            });
            chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
        }
        for (const auto& c : chunks)
        {
            if (!p->frequencyOk(pl, c.first, c.second))
                continue;
            if (pl.excludeSet >= 0)
            {
                bool excluded = false;
                const int n = pl.excludeChunks;
                for (int x = c.first - n; x <= c.first + n && !excluded; x++)
                    for (int z = c.second - n; z <= c.second + n && !excluded; z++)
                        excluded = p->isStructureChunk(pl.excludeSet, x, z);
                if (excluded)
                    continue;
            }
            p->chunk((int) set, c.first, c.second, out);
        }
    }
    return !(abort && abort->load(std::memory_order_relaxed));
}

std::vector<std::pair<int, int>> ringPositions(StructureFinder& finder, int set)
{
    StructureFinderPrivate *p = finder.d();
    if (!p->sd || set < 0 || (size_t) set >= p->sd->sets.size() || !p->possible[(size_t) set] ||
        p->sd->sets[(size_t) set].placement.kind != PlacementDef::CONCENTRIC_RINGS)
        return {};
    std::vector<std::pair<int, int>> out;
    p->ringPositionsIn(set, INT_MIN / 2, INT_MIN / 2, INT_MAX / 2, INT_MAX / 2, &out);
    return out;
}

void setTerrainHeights(StructureFinder& finder, TerrainHeights *heights)
{
    finder.d()->heights = heights;
}

} // namespace dp
