#include "searchthread.h"
#include "appactivity.h"

#include "aboutdialog.h"
#include "formsearchcontrol.h"
#include "message.h"
#include "seedtables.h"

#include "datapackworld.h"
#include "seedatlas-engine/quadbase.h"
#include "seedatlas-engine/util.h"

#include <QApplication>
#include <climits>
#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QDirIterator>
#include <QFile>
#include <QVector>


void Session::writeHeader(QTextStream& stream)
{
    stream << "#Version:  " << VERS_MAJOR << "." << VERS_MINOR << "." << VERS_PATCH << "\n";
    stream << "#Time:     " << QDateTime::currentDateTime().toString() << "\n";
    // MC version of the session should take priority over the one in the settings
    wi.write(stream);

    sc.write(stream);
    gen48.write(stream);

    for (Condition &c : cv)
        stream << "#Cond: " << c.toHex() << "\n";
    // only sessions with datapack conditions or of a datapack world have
    // these lines, so that the others stay readable for older versions
    if (datapack)
    {
        stream << "#Datapacks: " << dppacks.join(" | ") << "\n";
        if (!dpdimension.isEmpty())
            stream << "#DpDimension: " << dpdimension << "\n";
    }
    for (const DpCondition& c : dpcv)
        stream << c.toLine() << "\n";
    stream.flush();
}

bool Session::save(QWidget *widget, QTextStream& stream)
{
    (void) widget;
    writeHeader(stream);
    for (uint64_t s : slist)
        stream << QString::asprintf("%" PRId64 "\n", (int64_t)s);
    stream.flush();
    // a full disk or a lost device shows as a failed write
    if (stream.status() != QTextStream::Ok)
        return false;
    if (QFileDevice *f = qobject_cast<QFileDevice*>(stream.device()))
    {
        if (f->error() != QFileDevice::NoError)
            return false;
    }
    return true;
}

bool Session::load(QWidget *widget, QTextStream& stream, bool quiet)
{
    int major = 0, minor = 0, patch = 0;
    QString line;
    line = stream.readLine();
    int lno = 1;

    if (sscanf(line.toLocal8Bit().data(), "#Version: %d.%d.%d", &major, &minor, &patch) != 3)
    {
        if (quiet)
            return false;
        int button = warn(widget, QApplication::tr("Warning"),
            QApplication::tr("File does not look like a session file.\nProgress may be incomplete or broken."),
            QApplication::tr("Continue anyway?"), QMessageBox::Abort | QMessageBox::Yes);
        if (button != QMessageBox::Yes)
            return false;
    }
    // 1.3.0 follows 4.3.dev0 in release order and reads its session format.
    else if (cmpVers(major, minor, patch) > 0 &&
             !(major == 4 && (minor < 3 || (minor == 3 && patch == -1))))
    {
        if (quiet)
            return false;
        int button = warn(widget, QApplication::tr("Warning"),
            QApplication::tr("Session file was created with a newer version.\nProgress may be incomplete or broken."),
            QApplication::tr("Continue loading progress anyway?"), QMessageBox::Abort | QMessageBox::Yes);
        if (button != QMessageBox::Yes)
            return false;
    }

    // "#Large:" resets the world type (files of older versions have no
    // "#Amplified" line), so an "#Amplified" line before it is applied at the end
    int amplifiedLine = -1;
    while (stream.status() == QTextStream::Ok && !stream.atEnd())
    {
        lno++;
        line = stream.readLine();

        if (line.isEmpty()) continue;
        if (line.startsWith("#Time:")) continue;
        if (line.startsWith("#Title:")) continue;
        if (line.startsWith("#Desc:")) continue;
        if (sc.read(line)) continue;
        if (gen48.read(line)) continue;
        if (wi.read(line))
        {
            if (line.startsWith("#Amplified:"))
                amplifiedLine = wi.amplified ? 1 : 0;
            continue;
        }

        if (line.startsWith("#Datapacks:"))
        {
            datapack = true;
            dppacks.clear();
            for (const QString& pack : line.mid(11).split(" | ", Qt::SkipEmptyParts))
                if (!pack.trimmed().isEmpty())
                    dppacks.append(pack.trimmed());
            continue;
        }
        if (line.startsWith("#DpDimension:"))
        {
            dpdimension = line.mid(13).trimmed();
            continue;
        }
        if (line.startsWith("#DpCond:"))
        {
            DpCondition c;
            if (DpCondition::fromLine(line, &c))
            {
                dpcv.push_back(c);
                continue;
            }
            if (quiet)
                return false;
            int button = warn(widget, QApplication::tr("Warning"),
                QApplication::tr("The datapack condition at line %1 cannot be read.").arg(lno),
                QApplication::tr("Continue anyway?"), QMessageBox::Abort | QMessageBox::Yes);
            if (button != QMessageBox::Yes)
                return false;
            continue;
        }
        if (line.startsWith("#Cond:"))
        {   // Conditions
            Condition c;
            if (c.readHex(line.mid(6).trimmed()))
            {
                cv.push_back(c);
            }
            else
            {
                if (quiet)
                    return false;
                int button = warn(widget, QApplication::tr("Warning"),
                    QApplication::tr("Condition [%1] at line %2 is not supported.").arg(c.save).arg(lno),
                    QApplication::tr("Continue anyway?"), QMessageBox::Abort | QMessageBox::Yes);
                if (button != QMessageBox::Yes)
                    return false;
            }
        }
        else
        {   // Seeds
            QByteArray ba = line.toLocal8Bit();
            const char *p = ba.data();
            uint64_t s;
            if (sscanf(p, "%" PRId64, (int64_t*)&s) == 1)
            {
                slist.push_back(s);
            }
            else
            {
                if (quiet)
                    return false;
                int button = warn(widget, QApplication::tr("Warning"),
                    QApplication::tr("Failed to parse line %1 of file:\n%2").arg(lno).arg(line),
                    QApplication::tr("Continue anyway?"), QMessageBox::Abort | QMessageBox::Yes);
                if (button != QMessageBox::Yes)
                    return false;
            }
        }
    }
    if (amplifiedLine >= 0)
    {
        wi.amplified = amplifiedLine != 0;
        if (wi.amplified)
            wi.large = false;
    }
    // the lines can come in any order, so the world type is checked against
    // the version only now (Amplified needs 1.18+)
    wi.normalize();
    return true;
}


SearchMaster::SearchMaster(QWidget *parent)
    : QObject(parent)
    , mutex()
    , stop()
    , resultSlots(1024)
    , proghist()
    , progtimer()
    , itemtimer()
    , count()
    , env()
    , searchtype()
    , mc()
    , genflags()
    , condtree()
    , itemsize()
    , threadcnt()
    , gen48()
    , slist()
    , idx()
    , scnt()
    , prog()
    , seed()
    , smin()
    , smax()
    , isdone()
    , stopping()
    , finishQueued()
    , stoponres()
    , bcand()
    , bscan()
    , bfirst()
    , bhigh0()
    , bscansize(64)
    , binflight()
{
    fast48pure = false;
    env.stop = &stop;
}

SearchMaster::~SearchMaster()
{
    stopSearch();
}

bool SearchMaster::set(QWidget *widget, const Session& s)
{
    dpsearch.reset();
    if (s.wi.dp)
    {   // a datapack world: its own conditions, whole seeds only
        if (s.sc.searchtype != SEARCH_INC && s.sc.searchtype != SEARCH_LIST)
        {
            warn(widget, tr("Datapack worlds are searched seed by seed: choose \"All seeds in "
                "order\" or \"Seeds from a file\" as the search type."));
            return false;
        }
        if (s.sc.searchtype == SEARCH_INC && !s.slist.empty())
        {
            warn(widget, tr("The seed generator (structure seeds) does not apply to datapack "
                "worlds. Set it to \"Auto\" to search them."));
            return false;
        }
        QString err, warning;
        std::shared_ptr<const DpSearch> compiled = DpSearch::compile(s.wi.dp, s.dpcv, &err, &warning);
        if (!compiled)
        {
            warn(widget, err);
            return false;
        }
        condtree.set(std::vector<Condition>(), s.wi.mc);
        dpsearch = compiled;
    }
    else
    {
        if (!setVanilla(widget, s))
            return false;
    }

    this->searchtype = s.sc.searchtype;
    this->mc = s.wi.mc;
    this->genflags = s.wi.genFlags();
    this->itemsize = 1;
    this->threadcnt = std::min(configuredThreadCount(), std::max(1, s.sc.threads));
    this->slist = s.slist;
    this->gen48 = s.gen48;
    if (dpsearch)
        this->gen48.mode = GEN48_NONE;
    this->idx = 0;
    this->scnt = ~(uint64_t)0;
    this->prog = 0;
    this->checked = 0;
    this->lastchecked = 0;
    this->seedunits = s.sc.searchtype == SEARCH_INC && s.slist.empty();
    this->seed = s.sc.startseed;
    this->smin = s.sc.smin;
    this->smax = s.sc.smax;
    this->isdone = false;
    this->stop = false;
    this->stoponres = s.sc.stoponres;
    return true;
}

bool SearchMaster::setVanilla(QWidget *widget, const Session& s)
{
    char refbuf[100] = {};
    char disabled[100] = {};

    for (const Condition& c : s.cv)
        if ((c.meta & Condition::DISABLED) && c.save >= 1 && c.save <= 99)
            disabled[c.save] = 1;

    for (const Condition& c : s.cv)
    {
        char cid[8];
        snprintf(cid, sizeof(cid), "[%02d]", c.save);
        if (c.save < 1 || c.save > 99)
        {
            warn(widget, tr("Condition with invalid ID %1.").arg(cid));
            return false;
        }
        if (c.type < 0 || c.type >= FILTER_MAX)
        {
            warn(widget, tr("Encountered invalid filter type %1 in condition ID %2.").arg(c.type).arg(cid));
            return false;
        }
        if (disabled[c.save])
            continue;

        const FilterInfo& finfo = g_filterinfo.list[c.type];

        if (c.relative && (c.relative < 1 || c.relative > 99 ||
                           refbuf[c.relative] == 0 || disabled[c.relative]))
        {
            warn(widget, tr("Condition with ID %1 has a broken reference position:\n"
                            "condition missing or out of order.").arg(cid));
            return false;
        }
        if (++refbuf[c.save] > 1)
        {
            warn(widget, tr("More than one condition with ID %1.").arg(cid));
            return false;
        }
        if (s.wi.mc < finfo.mcmin)
        {
            const char *mcs = mc2str(finfo.mcmin);
            warn(widget, tr("Condition %1 requires a minimum Minecraft version of %2.").arg(cid, mcs));
            return false;
        }
        if (s.wi.mc > finfo.mcmax)
        {
            const char *mcs = mc2str(finfo.mcmax);
            warn(widget, tr("Condition %1 not available for Minecraft versions above %2.").arg(cid, mcs));
            return false;
        }
        if (c.type >= F_QH_IDEAL && c.type <= F_QM_90)
        {   // quad conditions rely on a dependent structure check to rule out
            // seeds where the huts/monuments do not actually generate
            bool active = false;
            for (const Condition& d : s.cv)
            {
                if (d.relative == c.save && !(d.meta & Condition::DISABLED)
                        && d.type == quadStructureType(c.type))
                {
                    active = true;
                    break;
                }
            }
            if (!active)
            {
                qWarning("Warning: quad condition %s has no active structure check;"
                         " the search matches structure positions alone and may"
                         " include seeds where the structures do not generate.",
                         cid);
            }
        }
        if (c.type == F_BIOME ||
            c.type == F_BIOME_4_RIVER ||
            c.type == F_BIOME_256_OTEMP ||
            c.type == F_BIOME_SAMPLE
        ){
            uint64_t b = c.biomeToFind;
            uint64_t m = c.biomeToFindM;
            if ((c.biomeToExcl & b) || (c.biomeToExclM & m))
            {
                warn(widget, tr("Biome condition with ID %1 has contradicting flags for include and exclude.").arg(cid));
                return false;
            }
            if ((b | m | c.biomeToExcl | c.biomeToExclM) == 0)
            {
                warn(widget, tr("Biome condition with ID %1 specifies no biomes.").arg(cid));
                return false;
            }

            int layerId = 0;
            int scale = c.step;
            if (c.type == F_BIOME_4_RIVER)
            {
                layerId = L_RIVER_4;
                scale = 4;
            }
            else if (c.type == F_BIOME_256_OTEMP)
            {
                layerId = L_OCEAN_TEMP_256;
                scale = 256;
            }
            if (layerId == 0 && s.wi.mc <= MC_1_17)
            {
                Generator tmp;
                setupGenerator(&tmp, s.wi.mc, 0);
                const Layer *l = getLayerForScale(&tmp, scale ? scale : 4);
                if (l)
                    layerId = l - tmp.ls.layers;
            }
            uint64_t ab, am;
            uint32_t flags = 0;
            getAvailableBiomes(&ab, &am, layerId, s.wi.mc, flags);
            b ^= (ab & b);
            m ^= (am & m);
            if (b || m)
            {
                int cnt = __builtin_popcountll(b) + __builtin_popcountll(m);
                QString msg = tr("Biome condition with ID %1 includes %n biome(s) "
                                 "that do not generate in MC %2.", "", cnt);
                warn(widget, msg.arg(cid, mc2str(s.wi.mc)));
                return false;
            }
        }
        if (c.type == F_TEMPS)
        {
            int64_t w = (int64_t)(c.x2 >> 10) - (c.x1 >> 10) + 1;
            int64_t h = (int64_t)(c.z2 >> 10) - (c.z1 >> 10) + 1;
            if (c.count > w * h)
            {
                QString msg = tr("Temperature category condition with ID %1 has too "
                                 "many restrictions (%2) for the area (%3 x %4 @ scale 1:1024).");
                warn(widget, msg.arg(cid).arg(c.count).arg(w).arg(h));
                return false;
            }
        }
        if (finfo.cat == CAT_STRUCT)
        {
            if (c.count >= 128)
            {
                warn(widget, tr("Structure condition %1 checks for too many instances (>= 128).").arg(cid));
                return false;
            }
        }
        if (c.skipref && c.rmax == 0 && c.x1 == 0 && c.x2 == 0 && c.z1 == 0 && c.z2 == 0)
        {
            warn(widget, tr("Condition %1 ignores its only location of size 1.").arg(cid));
            return false;
        }
    }

    QString err = condtree.set(s.cv, s.wi.mc);
    if (err.isEmpty())
    {
        err = env.init(s.wi.mc, s.wi.genFlags(), condtree);
    }
    if (!err.isEmpty())
    {
        warn(widget, tr("Failed to setup search environment:\n%1").arg(err));
        return false;
    }
    return true;
}

static void genQHBases(int qual, uint64_t salt, std::vector<uint64_t>& list48)
{
    int cst_type = 0;
    switch (qual)
    {
    case IDEAL_SALTED:
    case IDEAL:   cst_type = CST_IDEAL; break;
    case CLASSIC: cst_type = CST_CLASSIC; break;
    case NORMAL:  cst_type = CST_NORMAL; break;
    case BARELY:  cst_type = CST_BARELY; break;
    }

    QDirIterator it(":/qh");
    while (it.hasNext())
    {
        QString fnam = it.next();
        QFile file(fnam);
        uint64_t low = it.fileInfo().baseName().toUInt(nullptr, 16);
        uint64_t mid = 0;
        if (getQuadHutCst(low) > cst_type)
            continue;

        if (!file.open(QIODevice::ReadOnly))
            continue;
        QTextStream stream(&file);

        while (!stream.atEnd())
        {
            QString line = stream.readLine();
            uint64_t diff = line.toULongLong(nullptr, 16);
            if (diff == 0)
                break;
            mid += diff;
            uint64_t s48 = (mid << 20) + low;
            list48.push_back(s48 - salt);
        }
    }
}

static bool applyTranspose(std::vector<uint64_t>& slist,
        const Gen48Config& gen48, uint64_t bufmax)
{
    std::vector<uint64_t> list48;

    // accept the corners of the manual area in either order
    int x = std::min(gen48.x1, gen48.x2);
    int z = std::min(gen48.z1, gen48.z2);
    int w = std::max(gen48.x1, gen48.x2) - x + 1;
    int h = std::max(gen48.z1, gen48.z2) - z + 1;

    // does the set of candidates for this condition fit in memory?
    if ((uint64_t)slist.size() * sizeof(int64_t) * (uint64_t)w * (uint64_t)h >= bufmax)
    {
        slist.clear();
        return false;
    }

    try {
        list48.resize(slist.size() * (uint64_t)w * (uint64_t)h);
    } catch (...) {
        slist.clear();
        return false;
    }

    uint64_t *p = list48.data();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            for (uint64_t b : slist)
                *p++ = moveStructure(b, x+i, z+j);

    std::sort(list48.begin(), list48.end());
    auto last = std::unique(list48.begin(), list48.end());
    list48.erase(last, list48.end());
    slist.swap(list48);
    return !slist.empty();
}

void SearchMaster::preSearch()
{
    uint64_t sstart = seed;

    if (gen48.mode == GEN48_AUTO)
    {   // resolve automatic mode
        for (const Condition& c : condtree.condvec)
        {
            if (c.type >= F_QH_IDEAL && c.type <= F_QH_BARELY)
            {
                gen48.mode = GEN48_QH;
                break;
            }
            else if (c.type >= F_QM_95 && c.type <= F_QM_90)
            {
                gen48.mode = GEN48_QM;
                break;
            }
        }
    }

    if (searchtype != SEARCH_LIST)
    {
        if (gen48.mode == GEN48_QH)
        {
            uint64_t salt = 0;
            if (gen48.qual == IDEAL_SALTED)
                salt = gen48.salt;
            else
            {
                StructureConfig sconf;
                getStructureConfig_override(Swamp_Hut, mc, &sconf);
                salt = sconf.salt;
            }
            slist.clear();
            genQHBases(gen48.qual, salt, slist);
        }
        else if (gen48.mode == GEN48_QM)
        {
            StructureConfig sconf;
            getStructureConfig_override(Monument, mc, &sconf);
            slist.clear();
            for (const uint64_t *s = g_qm_90; *s; s++)
                if (qmonumentQual(*s) >= gen48.qmarea)
                    slist.push_back((*s - sconf.salt) & MASK48);
        }
        else if (gen48.mode == GEN48_LIST)
        {
            if (gen48.listsalt)
            {
                for (uint64_t& rs : slist)
                    rs += gen48.listsalt;
            }
        }

        if (!slist.empty())
            applyTranspose(slist, gen48, PRECOMPUTE48_BUFSIZ);
    }

    if (searchtype == SEARCH_LIST)
    {
        if (!slist.empty())
        {   // 64-bit seed list
            scnt = slist.size();
            for (idx = 0; idx < scnt; idx++)
                if (slist[idx] == sstart)
                    break;
            if (idx == scnt)
                idx = 0;
            seed = slist[idx];
            smax = slist.back();
            prog = idx;
        }
        else
        {   // slist should not be empty for a meaningful list search
            scnt = smax = ~(uint64_t)0;
            prog = seed = sstart;
            idx = 0;
        }
    }

    if (searchtype == SEARCH_48ONLY)
    {
        if (!slist.empty())
        {   // 48-bit seed list
            scnt = slist.size();
            for (idx = 0; idx < scnt; idx++)
                if (slist[idx] == sstart)
                    break;
            if (idx == scnt)
                idx = 0;
            seed = slist[idx];
            smax = slist.back();
            prog = idx;
        }
        else
        {
            prog = seed = sstart;
            smax = MASK48;
            scnt = MASK48 + 1;
            if (seed > smax)
                isdone = true;
        }
    }

    if (searchtype == SEARCH_INC)
    {   // smin & smax are given by user
        if (!slist.empty())
        {   // incremental search with a 48-bit list (incl. quad-searches)
            seed = sstart;
            if (seed < smin)
                seed = smin;
            scnt = 0x10000 * slist.size();
            uint64_t high = (seed >> 48) & 0xffff;
            for (idx = 0; idx < slist.size(); idx++)
                if (slist[idx] >= (seed & MASK48))
                    break;
            if (idx == slist.size())
            {
                if (high++ >= (smax >> 48))
                    isdone = true;
                idx = 0;
            }
            seed = (high << 48) | slist[idx];
            // trim the search space to the range [smin, smax]
            uint64_t idxmin, idxmax;
            for (idxmin = 0; idxmin < slist.size(); idxmin++)
                if (slist[idxmin] >= (smin & MASK48))
                    break;
            for (idxmax = 0; idxmax < slist.size(); idxmax++)
                if (slist[idxmax] > (smax & MASK48))
                    break;
            high = (high - (smin >> 48)) & 0xffff;
            prog = high * slist.size() + idx - idxmin;
            high = ((smax >> 48) - (smin >> 48)) & 0xffff;
            scnt = high * slist.size() + idxmax - idxmin;
        }
        else
        {   // simple incremental search
            seed = sstart;
            if (seed < smin)
                seed = smin;
            prog = seed - smin;
            uint64_t range = smax - smin;
            scnt = range == UINT64_MAX ? UINT64_MAX : range + 1;
        }
        if (seed > smax)
            isdone = true;
    }

    if (searchtype == SEARCH_BLOCKS)
    {
        if (!slist.empty())
        {
            scnt = 0x10000 * slist.size();
            for (idx = 0; idx < slist.size(); idx++)
                if (slist[idx] >= (sstart & MASK48))
                    break;
            if (idx == slist.size())
                isdone = true;
            else
            {
                seed = (sstart & ~MASK48) | slist[idx];
                prog = 0x10000 * idx + (seed >> 48);
            }
            smax = slist.back() | (0xffffULL << 48);
        }
        else
        {
            scnt = smax = ~(uint64_t)0;
            seed = sstart;
            prog = (seed << 16) | (seed >> 48);
            bcand.clear();
            bscan = bfirst = sstart & MASK48;
            bhigh0 = (uint32_t)(sstart >> 48);
            binflight = 0;
        }
    }
}

// The area of a condition as in testCondAt() at the origin, in quart
// coordinates, and the squared radius in blocks as testCondAt() derives it.
static void conditionArea(const Condition& c, int& x, int& z, int& w, int& h, int64_t& rmaxsq)
{
    int x1 = c.x1, z1 = c.z1, x2 = c.x2, z2 = c.z2;
    rmaxsq = 0;
    if (c.rmax > 0)
    {
        int64_t r = c.rmax - 1;
        x1 = z1 = -(int)r;
        x2 = z2 = (int)r;
        rmaxsq = r * r + 1;
    }
    x = x1 >> 2;
    z = z1 >> 2;
    w = (x2 >> 2) - x + 1;
    h = (z2 >> 2) - z + 1;
}

// The tests that can serve as a GPU prefilter for a condition tree.
static GpuClimateFilter::Tests gpuTestsFor(const ConditionTree& tree, int mc,
                                           const SearchThreadEnv& env)
{
    GpuClimateFilter::Tests tests;
    if (mc < MC_1_18 || tree.condvec.empty() || tree.references.empty())
        return tests;
    // The direct children of the root condition are combined with AND and
    // are tested at the origin, so each of them has to pass on its own.
    for (char b : tree.references[0])
    {
        const Condition& c = tree.condvec[(int)b];
        int x, z, w, h;
        int64_t rmaxsq;
        conditionArea(c, x, z, w, h, rmaxsq);
        if (w <= 0 || h <= 0)
            continue;
        if (c.type == F_CLIMATE_NOISE)
        {
            // the order of the CPU test, which puts the cheap noises first
            const int order[] = {
                NP_TEMPERATURE, NP_HUMIDITY, NP_WEIRDNESS, NP_EROSION, NP_CONTINENTALNESS,
            };
            for (int i : order)
            {
                if (c.limok[i][0] == INT_MIN && c.limok[i][1] == INT_MAX)
                    continue;
                GpuClimateFilter::Test t;
                t.para = i;
                t.x = x;
                t.z = z;
                t.w = w;
                t.h = h;
                t.lmin = c.limok[i][0];
                t.lmax = c.limok[i][1];
                tests.climate.push_back(t);
            }
        }
        else if (c.type == F_BIOME_SAMPLE && c.ncbpara > 0 &&
                 c.confidence > 0 && c.confidence < 1 &&
                 c.converage > 0 && c.converage <= 1)
        {   // the climate bounds of the included biomes (see testCondAt())
            GpuClimateFilter::SampleTest t;
            t.x = x;
            t.z = z;
            t.w = w;
            t.h = h;
            t.rmaxsq = rmaxsq;
            t.coverage = c.converage;
            t.confidence = c.confidence;
            for (int i = 0; i < c.ncbpara; i++)
            {
                int p = c.cbpara[i];
                t.paras.push_back(p);
                t.lo.push_back(c.cb.lo[p]);
                t.hi.push_back(c.cb.hi[p]);
            }
            tests.samples.push_back(t);
        }
        else if (c.type == F_BIOME && (int)b < (int)env.areachecks.size() &&
                 !env.areachecks[(int)b].biomes.empty())
        {   // the required biomes of a 1:4 area, as areaMayHaveBiomes(): the
            // climate range over the area grown by the shift has to reach
            // the (widened) bounds of each of them
            const SearchThreadEnv::AreaCheck& ac = env.areachecks[(int)b];
            for (const auto& bounds : ac.biomes)
            {
                for (const SearchThreadEnv::AreaBound& ab : bounds)
                {
                    GpuClimateFilter::Test t;
                    t.para = ab.para;
                    t.x = x - ac.shift;
                    t.z = z - ac.shift;
                    t.w = w + 2 * ac.shift;
                    t.h = h + 2 * ac.shift;
                    t.lmin = ab.lo;
                    t.lmax = ab.hi;
                    tests.climate.push_back(t);
                }
            }
        }
    }
    return tests;
}

void SearchMaster::startSearch()
{
    if (stopping)
    {   // called while a stop waits for the workers (from the event loop)
        emit searchFinish(false);
        return;
    }
    stopSearch();
    stop = false;
    preSearch();

    // The GPU works on large items, so a search needs a few of them for
    // every worker; shorter searches are fast on the CPU anyway.
    gputests = GpuClimateFilter::Tests();
    uint64_t gpumin = (uint64_t) 2 * 0x10000 * std::max(threadcnt, 1);
    if (!dpsearch && searchtype == SEARCH_INC && slist.empty() && scnt >= gpumin &&
        GpuClimateFilter::enabled())
        gputests = gpuTestsFor(condtree, mc, env);

    fast48pure = !dpsearch && treeFast48IsPure(condtree);
    lowstate.reset();
    if (searchtype == SEARCH_INC && !slist.empty() && fast48pure)
    {
        lowstate.reset(new (std::nothrow) std::atomic<uint8_t>[slist.size()]);
        if (lowstate)
            for (size_t i = 0; i < slist.size(); i++)
                lowstate[i].store(0, std::memory_order_relaxed);
    }

    if (stop)
    {
        emit searchFinish(false);
        return;
    }

    for (int i = 0; i < threadcnt; i++)
    {
        SearchWorker *worker = new SearchWorker(this);
        QObject::connect(
            worker, &SearchWorker::results,
            this, &SearchMaster::onWorkerResults,
            Qt::QueuedConnection);
        QObject::connect(
            worker, &SearchWorker::finished,
            this, &SearchMaster::onWorkerFinished,
            Qt::QueuedConnection);

        workers.push_back(worker);
    }

    QMutexLocker locker(&mutex);

    proghist.clear();
    progtimer.start();
    itemtimer.start();
    count = 0;

    // The workers yield to interactive work (the map, the analysis tabs and
    // the UI). On macOS this is the utility QoS class, which still runs on
    // the performance cores when nothing else needs them.
    for (SearchWorker *worker : workers)
    {
        worker->start(QThread::LowPriority);
    }
}

void SearchMaster::stopSearch()
{
    stop = true;
    // A stop can be requested again from the events processed below (a
    // result that reaches the limit, stop on result); the first one finishes
    // the job.
    if (workers.empty() || stopping)
        return;

    stopping = true;
    // the events below can change the member, so this works on a copy
    const std::vector<SearchWorker*> list = workers;
    for (SearchWorker *worker : list)
        disconnect(worker, &SearchWorker::finished,
                   this, &SearchMaster::onWorkerFinished);
    for (SearchWorker *worker : list)
    {
        while (!worker->wait(10))
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    {
        QMutexLocker locker(&mutex);
        workers.clear();
    }
    deleteWorkers(list);
    stopping = false;
    finishQueued = false;

    emit searchFinish(false);
}

void SearchMaster::deleteWorkers(const std::vector<SearchWorker*>& list)
{
    for (SearchWorker *worker : list)
    {
        worker->disconnect(this);
        delete worker;
    }
}


static QString getAbbrNum(double x)
{
    if (x >= 10e9)
        return QString::asprintf("%.1fG", x * 1e-9);
    if (x >= 10e6)
        return QString::asprintf("%.1fM", x * 1e-6);
    if (x >= 10e3)
        return QString::asprintf("%.1fK", x * 1e-3);
    return QString::asprintf("%.2f", x);
}

bool SearchMaster::getProgress(QString *status, uint64_t *prog, uint64_t *end, uint64_t *seed, qreal *min, qreal *avg, qreal *max)
{
    if (!mutex.tryLock(10))
        return false;
    *prog = this->prog;
    *end  = this->scnt;
    *seed = this->seed;
    *min = *avg = *max = nan("");

    bool valid = false;
    uint64_t total = checked;
    for (SearchWorker *worker: workers)
    {
        if (worker->prog < *prog)
        {
            *prog = worker->prog;
            *seed = worker->seed.load(std::memory_order_acquire);
            valid = true;
        }
        total += worker->itemdone.load(std::memory_order_relaxed);
    }
    lastchecked = total;

    if (isdone)
    {
        *prog = this->scnt;
    }

    mutex.unlock();

    // track the progress over a few seconds so we can estimate the search speed
    enum { SAMPLE_SEC = 20 };
    if (valid)
    {
        // The position is that of the worker that is furthest behind, which
        // moves at the speed of one worker while it is inside a large item.
        // Where the progress counts seeds, the speed is that of all workers.
        TProg tp = { (uint64_t) progtimer.nsecsElapsed(), seedunits ? total : *prog };
        proghist.push_front(tp);
        while (proghist.size() > 1 && proghist.back().ns < tp.ns - SAMPLE_SEC*1e9)
            proghist.pop_back();
    }

    if (proghist.size() > 1 && proghist.front().ns > proghist.back().ns)
    {
        std::vector<qreal> samples;
        samples.reserve(proghist.size());
        auto it_prev = proghist.begin();
        auto it = it_prev;
        while (++it != proghist.end())
        {
            qreal dp = it_prev->prog - it->prog;
            qreal dt = 1e-9 * (it_prev->ns - it->ns);
            if (dt > 0)
                samples.push_back(dp / dt);
            it_prev = it;
        }
        std::sort(samples.begin(), samples.end());
        qreal speedtot = 0;
        qreal weightot = 1e-6;
        int n = (int) samples.size();
        int r = (int) (n / SAMPLE_SEC);
        int has_zeros = 0;
        for (int i = -r; i <= r; i++)
        {
            int j = n/2 + i;
            if (j < 0 || j >= n)
                continue;
            has_zeros += samples[j] == 0;
            speedtot += samples[j];
            weightot += 1.0;
        }
        *min = samples[n*1/4]; // lower quartile
        *avg = speedtot / weightot; // median
        *max = samples[n*3/4]; // upper quartile
        if (has_zeros)
        {   // probably a slow sampling regime, use whole range for estimate
            // (also where the median is zero: less than one seed per update)
            speedtot = 0;
            for (qreal s : samples)
                speedtot += s;
            *avg = speedtot / n;
        }
    }

    if (!std::isfinite(*avg) || !std::isfinite(*min) || !std::isfinite(*max))
    {
        *status = tr("Measuring search speed...");
        return valid;
    }
    qreal remain = ((qreal)*end - *prog) / (*avg + 1e-6);
    QString eta;
    if (remain >= 3600*24*1000)
        eta = "years";
    else if (remain > 0)
    {
        int s = (int) remain;
        if (s > 86400)
            eta = QString("%1d:%2").arg(s / 86400).arg((s % 86400) / 3600, 2, 10, QLatin1Char('0'));
        else if (s > 3600)
            eta = QString("%1h:%2").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QLatin1Char('0'));
        else
            eta = QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    }
    *status = QString("seeds/sec: %1 min: %2 max: %3 isize: %4 eta: %5")
        // padded with figure spaces, which are as wide as the digits of the
        // proportional UI font, so the values do not shift while they change
        .arg(getAbbrNum(*avg), -8, QChar(0x2007))
        .arg(getAbbrNum(*min), -8, QChar(0x2007))
        .arg(getAbbrNum(*max), -8, QChar(0x2007))
        .arg(itemsize, -3, 10, QChar(0x2007))
        .arg(eta);

    return valid;
}

bool SearchMaster::requestItem(SearchWorker *item)
{
    if (isdone)
        return false;

    // QMutexLocker locker(&mutex);

    // check if we should adjust the item size
    uint64_t nsec = itemtimer.nsecsElapsed();
    count++;
    if (nsec > 0.1e9)
    {
        if (count < 1e2 && itemsize > 1)
            itemsize /= 2;
        if (count > 1e3 && itemsize < 0x10000)
            itemsize *= 2;
        itemtimer.start();
        count = 0;
    }

    if (searchtype == SEARCH_BLOCKS && slist.empty())
        return requestBlockItem(item);

    // GPU batches need large items (of a size the worker chooses), the CPU
    // keeps the adaptive size, which keeps the progress fine-grained
    int itemsize = item->wantgpu && !gputests.empty() ? item->gpuitemsize : this->itemsize;

    // the worker has finished the seeds of its previous item
    checked += item->scnt;
    item->itemdone.store(0, std::memory_order_relaxed);

    item->prog      = prog;
    item->idx       = idx;
    item->sstart    = seed;
    item->scnt      = itemsize;
    item->seed.store(seed, std::memory_order_release);

    prog += itemsize;

    if (searchtype == SEARCH_LIST)
    {
        if (idx + itemsize > scnt)
            item->scnt = scnt - idx;
        idx += itemsize;
        if (idx >= scnt)
            isdone = true;
    }

    if (searchtype == SEARCH_48ONLY)
    {
        if (!slist.empty())
        {
            if (idx + itemsize > scnt)
                item->scnt = scnt - idx;
            idx += itemsize;
            if (idx >= scnt)
                isdone = true;
        }
        else
        {
            seed += itemsize;
            if (seed > MASK48)
                isdone = true;
        }
    }

    if (searchtype == SEARCH_INC)
    {
        const bool fullRange = slist.empty() && smin == 0 && smax == UINT64_MAX;
        if (!fullRange && item->prog < scnt && scnt - item->prog < uint64_t(itemsize))
        {
            item->scnt = int(scnt - item->prog);
            prog = scnt;
        }
        if (!slist.empty())
        {
            uint64_t high = (seed >> 48) & 0xffff;
            idx += itemsize;
            high += idx / slist.size();
            idx %= slist.size();
            seed = (high << 48) | slist[idx];
            if (high > (smax >> 48))
                isdone = true;
        }
        else
        {
            // seed += itemsize; with overflow detection
            uint64_t s = seed + itemsize;
            if (s < seed)
                isdone = true; // overflow
            seed = s;
        }
        if (seed > smax)
            isdone = true;
    }

    if (searchtype == SEARCH_BLOCKS)
    {
        if (!slist.empty())
        {
            uint64_t high = (seed >> 48) & 0xffff;
            high += itemsize;
            if (high >= 0x10000)
            {
                high = 0;
                idx++;
            }
            if (idx >= slist.size())
                isdone = true;
            else
                seed = (high << 48) | slist[idx];
        }
    }

    return true;
}

// Called with the mutex held. Collects the candidates of the scan item the
// worker has just finished and assigns it a new item. A candidate is moved
// into bcand in the same locked step that ends the worker's scan item, so
// it is covered by the progress at all times.
bool SearchMaster::requestBlockItem(SearchWorker *item)
{
    if (item->itemkind == SearchWorker::ITEM_SCAN48)
    {
        for (uint64_t low : item->found)
            bcand.emplace(low, low == bfirst ? bhigh0 : 0);
        item->found.clear();
        binflight--;
        // aim for scan items of a few milliseconds
        if (item->scanns < 2e6 && bscansize < (1 << 20))
            bscansize *= 2;
        else if (item->scanns > 20e6 && bscansize > 1)
            bscansize /= 2;
    }

    if (!bcand.empty())
    {   // upper 16-bit items of the lowest candidate
        auto it = bcand.begin();
        uint64_t low = it->first;
        uint32_t high = it->second;
        int cnt = itemsize;
        if (high + cnt > 0x10000)
            cnt = 0x10000 - high;
        item->itemkind  = SearchWorker::ITEM_SEEDS;
        item->sstart    = ((uint64_t)high << 48) | low;
        item->seed.store(item->sstart, std::memory_order_release);
        item->prog      = (low << 16) | high;
        item->scnt      = cnt;
        high += cnt;
        if (high >= 0x10000)
            bcand.erase(it);
        else
            it->second = high;
    }
    else if (bscan <= MASK48)
    {   // a range of lower 48-bit seeds to scan
        uint64_t cnt = bscansize;
        if (cnt > MASK48 - bscan + 1)
            cnt = MASK48 - bscan + 1;
        uint32_t high = bscan == bfirst ? bhigh0 : 0;
        item->itemkind  = SearchWorker::ITEM_SCAN48;
        item->sstart    = bscan;
        item->seed.store(((uint64_t)high << 48) | bscan, std::memory_order_release);
        item->prog      = (bscan << 16) | high;
        item->scnt      = (int) cnt;
        item->found.clear();
        bscan += cnt;
        binflight++;
    }
    else if (binflight > 0)
    {   // other workers may still find candidates
        item->itemkind  = SearchWorker::ITEM_WAIT;
        item->prog      = ~(uint64_t)0;
        item->scnt      = 0;
    }
    else
    {
        isdone = true;
    }

    updateBlockProgress();
    return !isdone;
}

// The progress of the master is the first position that has not been
// assigned to a worker yet: the lowest open candidate or the scan position.
void SearchMaster::updateBlockProgress()
{
    uint64_t p = ~(uint64_t)0, s = ~(uint64_t)0;
    if (bscan <= MASK48)
    {
        uint32_t high = bscan == bfirst ? bhigh0 : 0;
        p = (bscan << 16) | high;
        s = ((uint64_t)high << 48) | bscan;
    }
    if (!bcand.empty())
    {
        auto it = bcand.begin();
        uint64_t pc = (it->first << 16) | it->second;
        if (pc < p)
        {
            p = pc;
            s = ((uint64_t)it->second << 48) | it->first;
        }
    }
    if (p != ~(uint64_t)0)
    {
        prog = p;
        seed = s;
    }
}

void SearchMaster::onWorkerResults(QVector<uint64_t> seeds)
{
    emit searchResults(seeds);
    resultSlots.release();
}

void SearchMaster::onWorkerFinished()
{
    if (stopping || finishQueued)
        return;
    QMutexLocker locker(&mutex);
    if (workers.empty())
        return;
    for (SearchWorker *worker : workers)
        if (!worker->isFinished())
            return;
    finishQueued = true;
    QMetaObject::invokeMethod(this, [this]() {
        QMutexLocker locker(&mutex);
        if (stopping || workers.empty())
        {
            finishQueued = false;
            return;
        }
        for (SearchWorker *worker : workers)
            if (!worker->isFinished())
            {
                finishQueued = false;
                return;
            }
        const bool done = isdone && !stop;
        const std::vector<SearchWorker*> list = workers;
        workers.clear();
        deleteWorkers(list);
        finishQueued = false;
        locker.unlock();
        emit searchFinish(done);
    }, Qt::QueuedConnection);
}


SearchWorker::SearchWorker(SearchMaster *master)
    : QThread(nullptr)
    , master(master)
{
    this->slist         = master->slist.empty() ? NULL : master->slist.data();
    this->len           = master->slist.size();

    this->prog          = master->prog;
    this->idx           = master->idx;
    this->sstart        = master->seed;
    this->scnt          = 0;
    this->seed.store(master->seed);
    this->itemdone.store(0);
    this->itemkind      = ITEM_SEEDS;
    this->scanns        = 0;
    this->wantgpu       = false;
    this->gpuitemsize   = 0x4000;

    this->env.stop      = &master->stop;
}

SearchWorker::~SearchWorker()
{
}

bool SearchWorker::getNextItem()
{
    QMutexLocker locker(&master->mutex);
    return master->requestItem(this);
}

void SearchWorker::report(uint64_t s)
{
    if (!master->stoponres)
    {
        resbuf.push_back(s);
        return;
    }
    // only the first result of the search counts, it goes out at once
    bool acquired = false;
    while (!*env.stop && !(acquired = master->resultSlots.tryAcquire(1, 10))) {}
    if (*env.stop)
    {   // the stop can arrive right after the slot was acquired
        if (acquired)
            master->resultSlots.release();
        return;
    }
    if (master->stop.exchange(true))
    {
        master->resultSlots.release();
        return;
    }
    emit results(QVector<uint64_t>{ s });
}

bool SearchWorker::flushResults()
{
    if (resbuf.empty())
        return !*env.stop;
    bool acquired = false;
    while (!*env.stop && !(acquired = master->resultSlots.tryAcquire(1, 10))) {}
    if (*env.stop)
    {   // dropped: the progress still points at the start of this item
        if (acquired)
            master->resultSlots.release();
        resbuf.clear();
        return false;
    }
    emit results(resbuf);
    resbuf.clear();
    return true;
}

// A search in a datapack world: every seed is generated with the datapack
// generator (no 48-bit passes, no GPU prefilter). A seed whose test was cut
// short by a stop is not reported; the stop also keeps the progress of the
// item from moving on, so a resumed search tests it again.
void SearchWorker::runDatapack()
{
    const DpSearch *dp = master->dpsearch.get();
    const std::atomic_bool *abort = &master->stop;
    while (!*env.stop && getNextItem())
    {
        if (master->searchtype == SEARCH_LIST)
        {
            uint64_t ie = idx+scnt < len ? idx+scnt : len;
            for (uint64_t i = idx; i < ie && !*env.stop; i++)
            {
                if (dp->test(slist[i], abort) == DpSearch::OK)
                    report(slist[i]);
                itemdone.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            uint64_t s = sstart;
            for (int i = 0; i < scnt && !*env.stop; i++)
            {
                if (dp->test(s, abort) == DpSearch::OK)
                    report(s);
                itemdone.fetch_add(1, std::memory_order_relaxed);
                if (s == ~(uint64_t)0)
                    break; // done
                s++;
            }
        }
        flushResults();
    }
    releaseDatapackSampler();
}

int SearchWorker::testSeed(uint64_t s)
{
    env.setSeed(s);
    return testTreeAt({0, 0}, &env, PASS_FULL_64, nullptr);
}

bool SearchWorker::clockDue()
{
    if (++cpcount < cpstride)
        return false;
    cpcount = 0;
    const qint64 ns = cptimer.nsecsElapsed();
    const qint64 dt = ns - cpchecked;
    cpchecked = ns;
    // seeds that are fast to test share a look at the clock, while a slow
    // stretch brings it back to every seed at once
    if (dt > 2000000)
        cpstride = 1;
    else if (dt < 100000 && cpstride < 64)
        cpstride *= 2;
    return true;
}

void SearchWorker::itemProgress(uint64_t next)
{
    if (cptimer.nsecsElapsed() < 250000000)
        return;
    // the results below the new position have to be out first: a search that
    // stops resumes from the position and would not find them again
    if (next > seed.load(std::memory_order_relaxed) && flushResults())
    {
        QMutexLocker locker(&master->mutex);
        prog += next - seed.load(std::memory_order_relaxed);
        seed.store(next, std::memory_order_release);
    }
    cptimer.start();
    cpchecked = 0;
}

// Incremental search with the GPU prefilter: the GPU removes the seeds that
// fail for sure, the CPU tests the rest. Whether that is faster than the
// CPU alone depends on the tests, the GPU and how many workers share it, so
// the worker keeps measuring both: a GPU item starts with a CPU sample from
// time to time, and a worker that runs on the CPU tries the GPU again after
// a while. The workers thereby settle on a share of the GPU that pays off.
void SearchWorker::runIncGpu(GpuClimateFilter *gpu)
{
    std::unique_ptr<GpuClimateFilter> owned(gpu);
    std::vector<uint64_t> passed;
    const bool forced = GpuClimateFilter::forced();
    QElapsedTimer sincegpu; // CPU time since the GPU was last measured
    double retry = 2e9;     // when to try the GPU again (ns)
    sincegpu.start();
    cptimer.start();
    wantgpu = true;

    while (!*env.stop && getNextItem())
    {
        uint64_t rem = ~(uint64_t)0 - sstart;
        uint32_t cnt = rem < (uint64_t)(scnt - 1) ? (uint32_t)(rem + 1) : (uint32_t)scnt;
        uint64_t start = sstart;
        QElapsedTimer timer;

        if (owned && wantgpu)
        {
            if (!forced && cnt > 4096 && (gpuitems % 16 == 0 || cpunsps == 0))
            {   // a CPU sample to compare with: up to 2048 seeds, but cut
                // short after 100 ms, since seeds that are slow to test
                // would otherwise keep the GPU waiting for minutes
                const uint32_t ncpu = 2048;
                uint32_t n = 0;
                timer.start();
                while (n < ncpu && !*env.stop)
                {
                    if (testSeed(start + n) == COND_OK)
                        report(start + n);
                    n++;
                    itemdone.store(n, std::memory_order_relaxed);
                    itemProgress(start + n);
                    if (timer.nsecsElapsed() > 100000000)
                        break;
                }
                if (n)
                {
                    double ns = (double) timer.nsecsElapsed() / n;
                    cpunsps = cpunsps ? 0.5 * (cpunsps + ns) : ns;
                }
                start += n;
                cnt -= n;
            }
            gpuitems++;
            timer.start();
            passed.clear();
            const uint32_t sampled = (uint32_t) itemdone.load(std::memory_order_relaxed);
            if (cnt && !*env.stop && !owned->filter(start, cnt, passed))
            {
                owned.reset(); // the GPU failed: the CPU takes over
            }
            else
            {
                // (the seeds that passed are in ascending order)
                const uint64_t rejected = sampled + cnt - passed.size();
                itemdone.store(rejected, std::memory_order_relaxed);
                for (size_t j = 0; j < passed.size() && !*env.stop; j++)
                {
                    if (testSeed(passed[j]) == COND_OK)
                        report(passed[j]);
                    itemdone.store(rejected + j + 1, std::memory_order_relaxed);
                    // all seeds up to the next one that passed are done
                    if (clockDue())
                        itemProgress(j + 1 < passed.size() ? passed[j + 1] : start + cnt);
                }
                if (cnt)
                {
                    double ns = (double) timer.nsecsElapsed() / cnt;
                    gpunsps = gpunsps ? 0.5 * (gpunsps + ns) : ns;
                    // items of about half a second: large enough for the GPU,
                    // small enough for a fine-grained progress (the CPU tests
                    // the seeds that pass, which can take most of the time)
                    double want = 0.5e9 / gpunsps;
                    gpuitemsize = (int) std::max(8192.0, std::min(65536.0, want));
                }
                cnt = 0;
            }
        }
        if (cnt)
        {   // on the CPU
            timer.start();
            const uint64_t before = itemdone.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < cnt && !*env.stop; i++)
            {
                if (testSeed(start + i) == COND_OK)
                    report(start + i);
                itemdone.store(before + i + 1, std::memory_order_relaxed);
                if (clockDue())
                    itemProgress(start + i + 1);
            }
            if (owned)
            {
                double ns = (double) timer.nsecsElapsed() / cnt;
                cpunsps = cpunsps ? 0.5 * (cpunsps + ns) : ns;
            }
        }
        flushResults();

        // the kind of the next item
        if (!owned)
            wantgpu = false;
        else if (forced || cpunsps == 0 || gpunsps == 0 || gpunsps < cpunsps)
        {
            wantgpu = true;
            retry = 2e9;
            sincegpu.start();
        }
        else if (wantgpu)
        {   // the CPU is faster for now
            wantgpu = false;
            sincegpu.start();
        }
        else if (sincegpu.nsecsElapsed() > retry)
        {   // try the GPU again, less often while it keeps losing
            wantgpu = true;
            retry = std::min(retry * 2, 64e9);
        }
    }
}

void SearchWorker::run()
{
    BusyActivity busy; // not to be throttled while out of view
    Pos origin = {0,0};
    resbuf.clear();
    if (master->dpsearch)
    {
        runDatapack();
        flushResults();
        return;
    }
    if (!env.init(master->mc, master->genflags, master->condtree).isEmpty())
    {
        *env.stop = true;
        return;
    }
    // the fast 48-bit pass can be skipped for seeds whose lower 48 bits
    // passed it already (block searches, known list entries)
    const bool skipfast = master->fast48pure;

    switch (master->searchtype)
    {
    case SEARCH_LIST:
        while (!*env.stop && getNextItem())
        {   // seed = slist[..]
            uint64_t ie = idx+scnt < len ? idx+scnt : len;
            for (uint64_t i = idx; i < ie; i++)
            {
                if (testSeed(slist[i]) == COND_OK)
                    report(slist[i]);
            }
            flushResults();
        }
        break;

    case SEARCH_48ONLY:
        while (!*env.stop && getNextItem())
        {
            if (slist)
            {
                uint64_t ie = idx+scnt < len ? idx+scnt : len;
                for (uint64_t i = idx; i < ie; i++)
                {
                    env.setSeed(slist[i]);
                    if (testTreeAt(origin, &env, PASS_FULL_48, nullptr) != COND_FAILED)
                        report(slist[i]);
                }
            }
            else
            {
                uint64_t s = sstart;
                for (int i = 0; i < scnt; i++)
                {
                    env.setSeed(s);
                    if (testTreeAt(origin, &env, PASS_FULL_48, nullptr) != COND_FAILED)
                        report(s);
                    if (s >= MASK48)
                        break; // done
                    s++;
                }
            }
            flushResults();
        }
        break;

    case SEARCH_INC:
        if (!slist && !master->gputests.empty())
        {
            GpuClimateFilter *gpu = new GpuClimateFilter;
            if (gpu->init(master->gputests, (master->genflags & LARGE_BIOMES) != 0))
            {
                runIncGpu(gpu);
                break;
            }
            delete gpu;
        }
        wantgpu = false;
        while (!*env.stop && getNextItem())
        {
            if (slist)
            {   // seed = (high << 48) | slist[..]
                uint64_t high = (sstart >> 48) & 0xffff;
                uint64_t lowidx = idx;
                std::atomic<uint8_t> *ls = master->lowstate.get();

                for (int i = 0; i < scnt; i++)
                {
                    uint64_t s = (high << 48) | slist[lowidx];
                    // the fast 48-bit pass of an entry is the same for all
                    // upper bits, so it runs once for each entry
                    uint8_t known = ls ? ls[lowidx].load(std::memory_order_relaxed) : 0;
                    if (known != 2)
                    {
                        env.setSeed(s);
                        int st;
                        if (known == 1)
                            st = testTreeAt64Only(origin, &env, nullptr);
                        else if (ls)
                        {
                            st = testTreeAt(origin, &env, PASS_FAST_48, nullptr);
                            ls[lowidx].store(st == COND_FAILED ? 2 : 1, std::memory_order_relaxed);
                            if (st != COND_FAILED)
                                st = testTreeAt64Only(origin, &env, nullptr);
                        }
                        else
                            st = testTreeAt(origin, &env, PASS_FULL_64, nullptr);
                        if (st == COND_OK)
                            report(s);
                    }

                    if (++lowidx >= len)
                    {
                        lowidx = 0;
                        if (++high >= 0x10000)
                            break; // done
                    }
                }
            }
            else
            {   // seed++
                uint64_t s = sstart;
                for (int i = 0; i < scnt; i++)
                {
                    if (testSeed(s) == COND_OK)
                        report(s);
                    if (s == ~(uint64_t)0)
                        break; // done
                    s++;
                }
            }
            flushResults();
        }
        break;

    case SEARCH_BLOCKS:
        if (!slist)
        {
            QElapsedTimer scantimer;
            while (!*env.stop && getNextItem())
            {
                if (itemkind == ITEM_WAIT)
                {
                    QThread::usleep(500);
                    continue;
                }
                if (itemkind == ITEM_SCAN48)
                {   // find the lower 48-bit seeds that can have results;
                    // the full 48-bit pass starts with the fast one
                    scantimer.start();
                    uint64_t end = sstart + (uint64_t) scnt;
                    for (uint64_t low = sstart; low < end; low++)
                    {
                        env.setSeed(low);
                        if (testTreeAt(origin, &env, PASS_FULL_48, nullptr) != COND_FAILED)
                            found.push_back(low);
                        if (*env.stop)
                            break;
                    }
                    scanns = scantimer.nsecsElapsed();
                    continue;
                }
                // seed = (high << 48) | low, for the assigned highs of a low
                // that passed the 48-bit pass
                uint64_t high = (sstart >> 48) & 0xffff;
                uint64_t low = sstart & MASK48;
                for (int i = 0; i < scnt; i++)
                {
                    uint64_t s = (high << 48) | low;
                    env.setSeed(s);
                    int st = skipfast ? testTreeAt64Only(origin, &env, nullptr)
                                      : testTreeAt(origin, &env, PASS_FULL_64, nullptr);
                    if (st == COND_OK)
                        report(s);
                    if (++high >= 0x10000)
                        break; // done
                }
                flushResults();
            }
            break;
        }
        while (!*env.stop && getNextItem())
        {   // seed = ([..] << 48) | low
            if (slist && idx >= len)
            {
                continue;
            }

            uint64_t high = (sstart >> 48) & 0xffff;
            uint64_t low;
            if (slist)
                low = slist[idx];
            else
                low = sstart & MASK48;

            env.setSeed(low);
            if (testTreeAt(origin, &env, PASS_FULL_48, nullptr) == COND_FAILED)
            {
                continue;
            }

            for (int i = 0; i < scnt; i++)
            {
                uint64_t s = (high << 48) | low;
                env.setSeed(s);
                int st = skipfast ? testTreeAt64Only(origin, &env, nullptr)
                                  : testTreeAt(origin, &env, PASS_FULL_64, nullptr);
                if (st == COND_OK)
                    report(s);
                if (++high >= 0x10000)
                    break; // done
            }
            flushResults();
        }
        break;
    }
    flushResults();
}
