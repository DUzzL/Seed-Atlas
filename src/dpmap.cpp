#include "dpmap.h"

#include "config.h"
#include "terrainmap.h"
#include "world.h"
#include "seedatlas-engine/util.h"

#include <QCoreApplication>
#include <QFont>
#include <QHash>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <memory>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("DpMap", text);
}

// FNV-1a over the UTF-8 bytes: the same colour in every run
uint32_t stableHash(const QString& text)
{
    uint32_t h = 2166136261u;
    for (char c : text.toUtf8())
    {
        h ^= (uint8_t) c;
        h *= 16777619u;
    }
    return h;
}

QRgb badgeColor(const QString& id)
{
    const uint32_t h = stableHash(id);
    // saturated, mid-bright colours that read on light and dark maps
    return QColor::fromHsv(int(h % 360), 150 + int((h >> 9) % 70), 175 + int((h >> 17) % 50)).rgb();
}

QChar badgeLetter(const QString& name, const QString& id)
{
    for (const QString& text : {name, id.section(':', 1)})
        for (QChar c : text)
            if (c.isLetterOrNumber())
                return c.toUpper();
    return QChar('?');
}

} // namespace

// ------------------------------------------------------------ catalogue

int dpVanillaStructureOpt(const QString& id)
{
    if (!id.startsWith(QLatin1String("minecraft:")))
        return D_NONE;
    const QString p = id.mid(10);
    if (p.startsWith(QLatin1String("village_")))       return D_VILLAGE;
    if (p == QLatin1String("desert_pyramid"))          return D_DESERT;
    if (p == QLatin1String("jungle_pyramid"))          return D_JUNGLE;
    if (p == QLatin1String("swamp_hut"))               return D_HUT;
    if (p == QLatin1String("igloo"))                   return D_IGLOO;
    if (p == QLatin1String("monument"))                return D_MONUMENT;
    if (p == QLatin1String("mansion"))                 return D_MANSION;
    if (p.startsWith(QLatin1String("ocean_ruin")))     return D_OCEANRUIN;
    if (p.startsWith(QLatin1String("shipwreck")))      return D_SHIPWRECK;
    if (p == QLatin1String("buried_treasure"))         return D_TREASURE;
    if (p.startsWith(QLatin1String("mineshaft")))      return D_MINESHAFT;
    if (p == QLatin1String("pillager_outpost"))        return D_OUTPOST;
    if (p == QLatin1String("ancient_city"))            return D_ANCIENTCITY;
    if (p == QLatin1String("trail_ruins"))             return D_TRAILRUINS;
    if (p == QLatin1String("trial_chambers"))          return D_CHAMBERS;
    if (p == QLatin1String("ruined_portal_nether"))    return D_PORTALN;
    if (p.startsWith(QLatin1String("ruined_portal")))  return D_PORTAL;
    if (p == QLatin1String("fortress"))                return D_FORTESS;
    if (p == QLatin1String("bastion_remnant"))         return D_BASTION;
    if (p == QLatin1String("end_city"))                return D_ENDCITY;
    if (p == QLatin1String("stronghold"))              return D_STRONGHOLD;
    return D_NONE;
}

int dpEngineDim(const DatapackWorld& world)
{
    const dp::DimensionInfo *info = world.dimensionInfo();
    if (world.dimensionId == QLatin1String("minecraft:the_nether"))
        return DIM_NETHER;
    if (world.dimensionId == QLatin1String("minecraft:the_end"))
        return DIM_END;
    if (info && info->hasCeiling)
        return DIM_NETHER;
    return DIM_OVERWORLD;
}

std::shared_ptr<const DpStructureCatalog> dpStructureCatalog(
    const std::shared_ptr<const DatapackWorld>& world)
{
    if (!world || !world->usable())
        return nullptr;
    static QMutex mutex;
    static std::weak_ptr<const DatapackWorld> lastWorld;
    static std::shared_ptr<const DpStructureCatalog> last;
    QMutexLocker locker(&mutex);
    if (last && lastWorld.lock() == world)
        return last;

    std::shared_ptr<DpStructureCatalog> cat = std::make_shared<DpStructureCatalog>();
    QSet<QString> spaces;
    for (const dp::StructureInfo& info : world->world->structures())
    {
        DpStructureEntry e;
        e.id = info.id;
        e.name = info.name.isEmpty() ? info.id : info.name;
        e.ns = info.id.section(':', 0, 0);
        e.type = info.type;
        e.set = info.set;
        e.supported = info.supported;
        e.reason = info.reason;
        e.pack = info.pack.isEmpty() ? tr("Minecraft") : info.pack;
        e.vanillaOpt = dpVanillaStructureOpt(info.id);
        e.color = badgeColor(info.id);
        e.letter = badgeLetter(e.name, info.id);
        cat->entries.push_back(e);
        spaces.insert(e.ns);
    }
    QHash<QString, int> names0;
    for (const DpStructureEntry& e : cat->entries)
        names0[e.name]++;
    for (DpStructureEntry& e : cat->entries)
        e.label = names0.value(e.name) > 1 ? QStringLiteral("%1 (%2)").arg(e.name, e.id) : e.name;
    QStringList names(spaces.begin(), spaces.end());
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
        const bool va = a == QLatin1String("minecraft"), vb = b == QLatin1String("minecraft");
        if (va != vb)
            return va;
        return a < b;
    });
    cat->namespaces = names;
    lastWorld = world;
    last = cat;
    return cat;
}

static QPixmap drawBadge(QRgb color, QChar letter, int size)
{
    const qreal dpr = 2.0;
    QPixmap pix(int(size * dpr), int(size * dpr));
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(1.5, 1.5, size - 3, size - 3);
    p.setPen(QPen(QColor(255, 255, 255, 235), 1.5));
    p.setBrush(QColor(color));
    p.drawRoundedRect(r, size * 0.28, size * 0.28);
    QFont font;
    font.setBold(true);
    font.setPixelSize(std::max(7, int(size * 0.55)));
    p.setFont(font);
    const QColor c(color);
    const int luma = (54 * c.red() + 183 * c.green() + 19 * c.blue()) >> 8;
    p.setPen(luma > 165 ? QColor(20, 22, 24) : QColor(255, 255, 255));
    p.drawText(r, Qt::AlignCenter, QString(letter));
    p.end();
    return pix;
}

// a faded copy with a small "?" chip in the lower right corner
static QPixmap drawUnknown(const QPixmap& base)
{
    const qreal dpr = base.devicePixelRatio();
    const QSizeF size = QSizeF(base.size()) / dpr;
    QPixmap pix(base.size());
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(0.45);
    p.drawPixmap(QPointF(0, 0), base);
    p.setOpacity(1.0);
    const qreal d = std::max(7.0, size.width() * 0.5);
    const QRectF chip(size.width() - d, size.height() - d, d, d);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1));
    p.setBrush(QColor(40, 44, 52, 225));
    p.drawEllipse(chip);
    QFont font;
    font.setBold(true);
    font.setPixelSize(std::max(6, int(d * 0.8)));
    p.setFont(font);
    p.setPen(QColor(255, 255, 255));
    p.drawText(chip, Qt::AlignCenter, QStringLiteral("?"));
    p.end();
    return pix;
}

const QPixmap& dpStructureIcon(const DpStructureEntry& entry, bool unknown)
{
    // The pixmaps live on the heap: callers keep the returned reference (the
    // map draws by &icon) while more icons are made, and QHash moves its
    // values when it grows.
    static QHash<QString, std::shared_ptr<const QPixmap>> cache;
    const int size = (int) round(g_iconscale * 20);
    const QString key = QStringLiteral("%1|%2|%3").arg(entry.id).arg(unknown).arg(size);
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it.value();
    QPixmap base;
    if (entry.vanillaOpt != D_NONE)
        base = getMapIcon(entry.vanillaOpt);
    if (base.isNull())
        base = drawBadge(entry.color, entry.letter, size);
    return *cache.insert(key, std::make_shared<const QPixmap>(unknown ? drawUnknown(base) : base)).value();
}

QPixmap dpNamespaceBadge(const QString& ns, int size)
{
    return drawBadge(badgeColor(ns + ':'), badgeLetter(ns, ns), size);
}

// one short phrase for an unknown start's detail
static QString dpStartDetailText(int detail)
{
    switch (detail)
    {
    case dp::StructureStart::NeedsHeight:
        return tr("its start height depends on the terrain");
    case dp::StructureStart::TerrainCheck:
        return tr("a check of the terrain is left");
    case dp::StructureStart::EarlierUnknown:
        return tr("a structure that the game tries first may start here instead");
    case dp::StructureStart::NotSupported:
        return tr("the structure type is not supported");
    case dp::StructureStart::TiedBiome:
        return tr("the biome at the start is a tie between several biomes");
    default:
        return tr("not decided");
    }
}

QString dpStartStateText(const dp::StructureStart& start)
{
    switch (start.state)
    {
    case dp::StructureStart::Valid:
        return tr("valid");
    case dp::StructureStart::Invalid:
        return tr("not generated");
    default:
        return tr("unknown (%1)").arg(dpStartDetailText(start.detail));
    }
}

bool dpStartsInArea(dp::StructureFinder *finder, int x0, int z0, int x1, int z1,
    const std::vector<char> *structures, bool invalid,
    std::vector<dp::StructureStart> *out, const std::atomic_bool *abort)
{
    if (!finder || !finder->valid())
        return false;
    // The start position of a structure (e.g. the centre of a jigsaw start
    // piece) can lie a few blocks outside of its chunk, so the query covers
    // a margin and the starts are taken by their position.
    const int pad = 4;
    std::vector<dp::StructureStart> all;
    // the starts that are not generated are not kept: they need no height
    finder->setHeightsOfInvalid(invalid);
    const bool ok = finder->starts((x0 >> 4) - pad, (z0 >> 4) - pad, (x1 >> 4) + pad, (z1 >> 4) + pad, &all, abort,
        structures);
    finder->setHeightsOfInvalid(true);
    if (!ok)
        return false;
    for (const dp::StructureStart& s : all)
    {
        if (s.x < x0 || s.x > x1 || s.z < z0 || s.z > z1)
            continue;
        if (!invalid && s.state == dp::StructureStart::Invalid)
            continue;
        if (structures && (s.structure < 0 || s.structure >= (int) structures->size()
                || !(*structures)[(size_t) s.structure]))
            continue;
        out->push_back(s);
    }
    return true;
}

// ------------------------------------------------------------ finder pool

namespace {

struct FinderKey
{
    const dp::World *world = nullptr;
    int dim = -1;
    uint64_t seed = 0;
    bool operator==(const FinderKey& o) const
    {
        return world == o.world && dim == o.dim && seed == o.seed;
    }
};

struct FinderSlot
{
    FinderKey key;
    std::shared_ptr<const dp::World> world; // keeps the world of warming finders
    std::vector<std::unique_ptr<dp::StructureFinder>> idle;
    int alive = 0;      // idle, lent out or warming up
    qint64 used = 0;
};

class FinderPool
{
public:
    FinderPool()
    {
        warmers.setMaxThreadCount(std::max(1, QThread::idealThreadCount()));
        warmers.setObjectName("datapack structure finders");
    }
    ~FinderPool()
    {
        warmers.clear(); // warm-ups that have not started
        warmers.waitForDone();
    }

    FinderSlot *find(const FinderKey& key)
    {
        for (FinderSlot& s : slotList)
            if (s.key == key)
                return &s;
        return nullptr;
    }
    FinderSlot *slot(const FinderKey& key, const std::shared_ptr<const dp::World>& world)
    {
        if (FinderSlot *s = find(key))
            return s;
        // a few worlds or seeds at a time (the map and an analysis); the
        // least recently used one goes
        const size_t limit = 3;
        while (slotList.size() >= limit)
        {
            auto oldest = std::min_element(slotList.begin(), slotList.end(),
                [](const FinderSlot& a, const FinderSlot& b) { return a.used < b.used; });
            for (auto& f : oldest->idle)
                dropped.push_back(std::move(f));
            slotList.erase(oldest);
        }
        slotList.emplace_back();
        slotList.back().key = key;
        slotList.back().world = world;
        return &slotList.back();
    }

    QMutex mutex;
    QWaitCondition cond;
    std::vector<FinderSlot> slotList;
    // finders to destroy outside of the lock
    std::vector<std::unique_ptr<dp::StructureFinder>> dropped;
    qint64 tick = 0;
    // last member: waits for the warm-ups before the rest goes
    QThreadPool warmers;
};

FinderPool& finderPool()
{
    static FinderPool pool;
    return pool;
}

void warmUp(const FinderKey& key, const std::shared_ptr<const dp::World>& world)
{
    {   // nobody waits for a finder of another world or seed any more
        FinderPool& pool = finderPool();
        QMutexLocker locker(&pool.mutex);
        if (!pool.find(key))
            return;
    }
    std::unique_ptr<dp::StructureFinder> finder(new dp::StructureFinder(world, key.dim, key.seed));
    if (finder->valid())
    {   // the first query searches the stronghold rings
        std::vector<dp::StructureStart> none;
        finder->starts(0, 0, 0, 0, &none);
    }
    FinderPool& pool = finderPool();
    QMutexLocker locker(&pool.mutex);
    if (FinderSlot *s = pool.find(key))
        s->idle.push_back(std::move(finder));
    pool.cond.wakeAll();
    locker.unlock();
    finder.reset();
}

} // namespace

std::unique_ptr<dp::StructureFinder> dpTakeFinder(const std::shared_ptr<const DatapackWorld>& world,
    uint64_t seed, const std::atomic_bool *abort)
{
    if (!world || !world->usable())
        return nullptr;
    FinderPool& pool = finderPool();
    const FinderKey key{world->world.get(), world->dimension, seed};
    const int limit = std::max(1, QThread::idealThreadCount());
    bool launched = false;
    std::unique_ptr<dp::StructureFinder> out;
    QMutexLocker locker(&pool.mutex);
    while (!out)
    {
        FinderSlot *s = pool.slot(key, world->world);
        s->used = ++pool.tick;
        if (!s->idle.empty())
        {
            out = std::move(s->idle.back());
            s->idle.pop_back();
            break;
        }
        if (!launched && s->alive < limit)
        {
            s->alive++;
            launched = true;
            std::shared_ptr<const dp::World> w = world->world;
            pool.warmers.start([key, w]() { warmUp(key, w); });
        }
        if (abort && abort->load(std::memory_order_relaxed))
            break;
        pool.cond.wait(&pool.mutex, 20);
    }
    std::vector<std::unique_ptr<dp::StructureFinder>> drop;
    drop.swap(pool.dropped);
    locker.unlock();
    drop.clear();
    return out;
}

void dpReturnFinder(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed,
    std::unique_ptr<dp::StructureFinder> finder)
{
    if (!finder || !world)
        return;
    FinderPool& pool = finderPool();
    const FinderKey key{world->world.get(), world->dimension, seed};
    QMutexLocker locker(&pool.mutex);
    if (FinderSlot *s = pool.find(key))
    {
        s->idle.push_back(std::move(finder));
        pool.cond.wakeAll();
    }
    locker.unlock();
    finder.reset();
}

void dpClearFinderPool()
{
    FinderPool& pool = finderPool();
    pool.warmers.waitForDone();
    std::vector<FinderSlot> slotList;
    {
        QMutexLocker locker(&pool.mutex);
        slotList.swap(pool.slotList);
    }
}

// ------------------------------------------------------------ heights

int dpHeightTop(const DatapackWorld& world, int y)
{
    const dp::DimensionInfo *info = world.dimensionInfo();
    if (info && info->hasCeiling)
        return y;
    return 0x7fffffff;
}

dp::HeightDetail dpMapHeightDetail(int scale)
{
    return scale >= 16 ? dp::HEIGHTS_FAST : dp::HEIGHTS_SURFACE;
}

bool dpTileHeights(dp::Sampler *sampler, int scale, int x, int z, int w, int h,
    dp::HeightKind kind, int top, std::vector<int> *out, const std::atomic_bool *abort)
{
    if (!sampler || !sampler->hasTerrain())
        return false;
    out->assign(size_t(w + 2) * size_t(h + 2), dp::NO_HEIGHT);
    return sampler->genHeights(out->data(), scale, x - 1, z - 1, w + 2, h + 2, kind, top, abort,
        dpMapHeightDetail(scale));
}

bool dpTileBothHeights(dp::Sampler *sampler, int scale, int x, int z, int w, int h, int top,
    std::vector<int> *floor, std::vector<int> *surface, const std::atomic_bool *abort)
{
    if (!sampler || !sampler->hasTerrain())
        return false;
    floor->assign(size_t(w + 2) * size_t(h + 2), dp::NO_HEIGHT);
    surface->assign(floor->size(), dp::NO_HEIGHT);
    return sampler->genBothHeights(floor->data(), surface->data(), scale, x - 1, z - 1, w + 2, h + 2, top,
        abort, dpMapHeightDetail(scale));
}

void dpShadeHeights(unsigned char *rgb, int w, int h, int scale, const std::vector<int>& heights,
    int mode, int minY, int maxY, int level)
{
    if (heights.size() != size_t(w + 2) * size_t(h + 2))
        return;
    const int tw = w + 2;
    const float mul = 0.25f / scale;
    const float lout = 0.65f, lmin = 0.5f, lmax = 1.5f;
    // one grey level per block, unless the dimension is taller than that
    const float graytop = std::max(255, maxY + 1);
    (void) minY;
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            unsigned char *col = rgb + 3 * (size_t(j) * w + i);
            const int c11 = heights[size_t(j + 1) * tw + i + 1];
            if (c11 == dp::NO_HEIGHT)
            {   // no block in this column: the void
                col[0] = 16; col[1] = 18; col[2] = 17;
                continue;
            }
            const auto at = [&](int jj, int ii) {
                const int v = heights[size_t(jj) * tw + ii];
                return v == dp::NO_HEIGHT ? float(c11) : float(v);
            };
            const float t11 = c11;
            if (mode == HV_GRAYSCALE)
            {
                const float v = graytop > 255 ? t11 * (255.0f / graytop) : t11;
                const unsigned char c = (v <= 0) ? 0 : (v > 255) ? 255 : (unsigned char) v;
                col[0] = col[1] = col[2] = c;
                continue;
            }
            const float t01 = at(j, i + 1), t10 = at(j + 1, i);
            const float t12 = at(j + 1, i + 2), t21 = at(j + 2, i + 1);
            float light = 1.0f;
            if (mode == HV_SHADING || mode == HV_CONTOURS_SHADING)
                light += ((t12 + t21) - (t01 + t10)) * mul;
            if (t11 > level)
                light = lout;
            light = std::clamp(light, lmin, lmax);
            if (mode == HV_CONTOURS || mode == HV_CONTOURS_SHADING)
            {
                const float spacing = 16.0f;
                const float tmin = std::min({t01, t10, t12, t21});
                if (std::floor(tmin / spacing) != std::floor(t11 / spacing))
                    light *= 0.5f;
            }
            for (int k = 0; k < 3; k++)
            {
                const float c = col[k] * light;
                col[k] = c <= 0 ? 0 : c > 255 ? 255 : (unsigned char) c;
            }
        }
    }
}

// engine ids of the vanilla biomes of a version, by "minecraft:" id
static const QHash<QString, int>& engineBiomeIds(int mc)
{
    static QMutex mutex;
    static std::map<int, QHash<QString, int>> tables;
    QMutexLocker locker(&mutex);
    auto it = tables.find(mc);
    if (it != tables.end())
        return it->second;
    QHash<QString, int> ids;
    for (int id = 0; id < 256; id++)
    {
        const char *name = biome2str(mc, id);
        if (name && *name)
            ids.insert(QStringLiteral("minecraft:") + QString::fromLatin1(name), id);
    }
    return tables.emplace(mc, ids).first->second;
}

void dpTerrainStyle(unsigned char *rgb, const DatapackWorld& world, const int *biomes,
    int x, int z, int w, int h, int scale, uint64_t seed,
    const std::vector<int>& floor, const std::vector<int>& surface)
{
    const size_t grid = size_t(w + 2) * size_t(h + 2);
    if (floor.size() != grid)
        return;
    const int dim = dpEngineDim(world);
    const std::vector<dp::BiomeInfo>& infos = world.world->biomes();
    const QHash<QString, int>& engine = engineBiomeIds(world.mc);
    std::vector<int> mapped(infos.size(), -1);
    for (size_t i = 0; i < infos.size(); i++)
        if (infos[i].vanilla)
            mapped[i] = engine.value(infos[i].id, -1);

    // the vanilla biomes: the built-in terrain style on the floor heights
    std::vector<int> ids(size_t(w) * size_t(h));
    for (size_t i = 0; i < ids.size(); i++)
    {
        const int b = biomes[i];
        ids[i] = b >= 0 && b < (int) mapped.size() ? mapped[(size_t) b] : -1;
    }
    std::vector<float> heights(grid);
    for (size_t i = 0; i < grid; i++)
        heights[i] = floor[i] == dp::NO_HEIGHT ? (dim == DIM_END ? 0.0f : float(world.minY()))
            : float(floor[i]);
    Range r = {scale, x, z, w, h, 0, 1};
    applyTerrainStyle(rgb, ids.data(), r, world.mc, dim, seed, heights);
    const int tw = w + 2;
    if (dim == DIM_NETHER)
    {   // the built-in style has no relief for the Nether (it has no heights
        // there); here the floor below the chosen level is known
        for (int j = 0; j < h; j++)
        {
            for (int i = 0; i < w; i++)
            {
                const size_t p = size_t(j) * w + i;
                if (ids[p] < 0)
                    continue;
                const int nw = floor[size_t(j) * tw + i], se = floor[size_t(j + 2) * tw + i + 2];
                const int f = floor[size_t(j + 1) * tw + i + 1];
                unsigned char *col = rgb + 3 * p;
                float light = 0;
                if (nw != dp::NO_HEIGHT && se != dp::NO_HEIGHT)
                    light = std::clamp((se - nw) / (2.0f * scale) * 0.65f, -0.3f, 0.3f);
                // lower floors darker
                if (f != dp::NO_HEIGHT)
                    light -= std::clamp((96 - f) / 160.0f, 0.0f, 0.35f);
                for (int q = 0; q < 3; q++)
                {
                    const float v = col[q] * (1.0f + light);
                    col[q] = v <= 0 ? 0 : v > 255 ? 255 : (unsigned char) v;
                }
            }
        }
    }

    // the biomes of the packs: their colour, shaded by the slope, and water
    // where the surface (with fluids) is above the floor
    const bool water = surface.size() == grid;
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            const size_t p = size_t(j) * w + i;
            if (ids[p] >= 0 || biomes[p] < 0)
                continue;
            unsigned char *col = rgb + 3 * p;
            const int f = floor[size_t(j + 1) * tw + i + 1];
            if (f == dp::NO_HEIGHT)
            {
                col[0] = 7; col[1] = 8; col[2] = 15;
                continue;
            }
            float light = 0;
            const int nw = floor[size_t(j) * tw + i], se = floor[size_t(j + 2) * tw + i + 2];
            if (nw != dp::NO_HEIGHT && se != dp::NO_HEIGHT)
                light = std::clamp((se - nw) / (2.0f * scale) * 0.65f, -0.28f, 0.28f);
            float c[3] = {float(col[0]), float(col[1]), float(col[2])};
            if (water)
            {
                const int s = surface[size_t(j + 1) * tw + i + 1];
                if (s != dp::NO_HEIGHT && s > f)
                {   // water: blue, darker with depth
                    const float depth = std::min(1.0f, (s - f) / 40.0f);
                    const float wr = 40, wg = 82, wb = 160;
                    const float k = 0.55f + 0.35f * depth;
                    for (int q = 0; q < 3; q++)
                        c[q] = c[q] * (1 - k) + (q == 0 ? wr : q == 1 ? wg : wb) * k;
                    for (int q = 0; q < 3; q++)
                        c[q] *= 1.0f - 0.35f * depth;
                    light = 0;
                }
            }
            for (int q = 0; q < 3; q++)
            {
                const float v = c[q] * (1.0f + light);
                col[q] = v <= 0 ? 0 : v > 255 ? 255 : (unsigned char) v;
            }
        }
    }
}

int dpBelowRoofLevel(const std::shared_ptr<const DatapackWorld>& world, uint64_t seed)
{
    const dp::DimensionInfo *info = world ? world->dimensionInfo() : nullptr;
    if (!info || !info->hasCeiling || !info->terrain)
        return INT_MIN;
    // The cache holds the worlds weakly: a world loaded later at the address
    // of a freed one never matches an old entry.
    struct Entry
    {
        std::weak_ptr<const dp::World> world;
        int dimension;
        uint64_t seed;
        int level;
    };
    static QMutex mutex;
    static std::vector<Entry> cache;
    {
        QMutexLocker locker(&mutex);
        for (const Entry& e : cache)
            if (e.dimension == world->dimension && e.seed == seed && e.world.lock() == world->world)
                return e.level;
    }
    dp::Sampler sampler(world->world, world->dimension, seed);
    int level = INT_MIN;
    if (sampler.valid() && sampler.hasTerrain())
    {
        // From the top of the roof down to the highest level where a quarter
        // of a few columns spread over the world are air: there the caves
        // under the roof begin.
        const int top = info->minY + info->height - 1;
        std::vector<std::pair<int, int>> columns;
        int start = INT_MIN;
        for (int k = 0; k < 16; k++)
        {
            const int x = (k % 4 - 2) * 613 + 101, z = (k / 4 - 2) * 587 + 59;
            const int roof = sampler.surfaceHeight(x, z, dp::HEIGHT_SURFACE, top);
            if (roof == dp::NO_HEIGHT)
                continue;
            columns.emplace_back(x, z);
            start = std::max(start, roof);
        }
        const int need = std::max(1, int(columns.size()) / 4);
        for (int y = start; start != INT_MIN && y > info->minY && level == INT_MIN; y--)
        {
            int air = 0;
            for (const auto& c : columns)
                air += !sampler.solidAt(c.first, y, c.second);
            if (air >= need)
                level = y;
        }
    }
    QMutexLocker locker(&mutex);
    if (cache.size() > 64)
        cache.clear();
    cache.push_back(Entry{world->world, world->dimension, seed, level});
    return level;
}
