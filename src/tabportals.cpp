#include "tabportals.h"
#include "appactivity.h"

#include "mappingworkers.h"
#include "message.h"
#include "resultlist.h"
#include "widgets.h"
#include "config.h"
#include "seedatlas-engine/util.h"

#include <QApplication>
#include <QCheckBox>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QEvent>
#include <functional>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

namespace {

constexpr uint64_t LOW48 = (uint64_t(1) << 48) - 1;
constexpr uint64_t SEEDS_PER_FAMILY = 65536;
// hits listed during a search (all of them are also kept between sessions)
constexpr uint64_t LIST_LIMIT = 100000;
// the hit list of earlier versions in the settings: raw records
constexpr quint32 RESULTS_MAGIC = 0x50455931; // "PEY1"
// The search state file: magic and format version. Version 2 adds the
// Amplified world type and is written only for such a search, so that the
// state of every other search stays readable by earlier versions (which
// would otherwise continue an Amplified search as a default one).
constexpr quint32 STATE_MAGIC = 0x50455953;   // "PEYS"
constexpr quint32 STATE_VERSION = 1;
constexpr quint32 STATE_VERSION_AMPLIFIED = 2;
// a running search writes its state at least this often (milliseconds)
constexpr qint64 AUTOSAVE_MS = 60000;

// The search state (settings, position, counters, hits) lives in a file of
// its own: it is written atomically while a search runs, and the hit list
// can be larger than the settings store allows.
QString searchStatePath()
{
    // appConfigDir() already separates test settings from the real ones
    return appConfigDir() + "/portal-search.dat";
}

void writeRecord(QDataStream& out, const EyePortalRecord& r)
{
    out << quint64(r.seed) << qint32(r.x) << qint32(r.y) << qint32(r.z)
        << quint16(r.eyes) << quint16(r.exact) << quint16(r.opening)
        << quint8(r.index) << quint8(r.eyeCount) << quint8(r.exactEyeCount)
        << quint8(r.exactFrames) << quint8(r.lit) << quint8(r.litExact);
    for (uint8_t c : r.cell)
        out << quint8(c);
}

EyePortalRecord readRecord(QDataStream& in)
{
    EyePortalRecord r;
    std::memset(&r, 0, sizeof(r));
    quint64 seed;
    qint32 x, y, z;
    quint16 eyes, exact, opening;
    quint8 index, eyeCount, exactEyeCount, exactFrames, lit, litExact;
    in >> seed >> x >> y >> z >> eyes >> exact >> opening >> index >> eyeCount
        >> exactEyeCount >> exactFrames >> lit >> litExact;
    r.seed = seed;
    r.x = x; r.y = y; r.z = z;
    r.eyes = eyes; r.exact = exact; r.opening = opening;
    r.index = index; r.eyeCount = eyeCount; r.exactEyeCount = exactEyeCount;
    r.exactFrames = exactFrames; r.lit = lit; r.litExact = litExact;
    for (uint8_t& c : r.cell)
    {
        quint8 v;
        in >> v;
        c = v;
    }
    return r;
}

// P(eye) per frame: nextFloat() > 0.9F, i.e. 1677721 of 2^24 values
constexpr double EYE_P = 1677721.0 / 16777216.0;

// Share of portals with m exact frames (m = 0..12), measured with
// getEndPortal() over 20000 random stronghold starts (26.3; layouts and
// exactness are the same in all supported versions).
constexpr double EXACT_FRAMES_SHARE[13] = {
    0.28355, 0.00110, 0.00130, 0.06830, 0.00190, 0.07095, 0.00045,
    0.06285, 0.0, 0.06330, 0.0, 0.0, 0.44630 };

double binomialTail(int n, int k, double p)
{
    if (k <= 0) return 1.0;
    if (k > n) return 0.0;
    double sum = 0.0;
    for (int j = k; j <= n; j++)
    {
        double c = 1.0;
        for (int i = 0; i < j; i++)
            c = c * (n - i) / (i + 1);
        sum += c * std::pow(p, j) * std::pow(1.0 - p, n - j);
    }
    return sum;
}

// probability that one portal counts as a hit
double portalHitProbability(int minEyes, bool exactOnly)
{
    if (!exactOnly)
        return binomialTail(12, minEyes, EYE_P);
    double sum = 0.0;
    for (int m = 0; m <= 12; m++)
        sum += EXACT_FRAMES_SHARE[m] * binomialTail(m, minEyes, EYE_P);
    return sum;
}

QString superscript(int v)
{
    static const QChar digits[10] = { u'⁰', u'¹', u'²', u'³', u'⁴', u'⁵', u'⁶', u'⁷', u'⁸', u'⁹' };
    QString s = QString::number(v), out;
    for (QChar c : s)
        out += c == '-' ? QChar(u'⁻') : digits[c.digitValue()];
    return out;
}

// 1234567 -> "1.2 × 10⁶" for large values, plain digits otherwise
QString bigNumber(double v)
{
    if (v < 1e6)
        return QLocale().toString(qint64(std::llround(v)));
    const int e = int(std::floor(std::log10(v)));
    return QString("%1 × 10%2").arg(v / std::pow(10.0, e), 0, 'f', 1).arg(superscript(e));
}

QString rateText(double perSecond)
{
    if (perSecond >= 1e9) return QString("%1 G").arg(perSecond / 1e9, 0, 'f', 2);
    if (perSecond >= 1e6) return QString("%1 M").arg(perSecond / 1e6, 0, 'f', 1);
    if (perSecond >= 1e3) return QString("%1 k").arg(perSecond / 1e3, 0, 'f', 1);
    return QString::number(perSecond, 'f', 0);
}

QString clockText(double seconds)
{
    const qint64 s = qint64(seconds);
    return QString::asprintf("%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
}

QString durationText(double s)
{
    if (s < 1) return TabPortals::tr("less than a second");
    if (s < 90) return TabPortals::tr("%n second(s)", "", int(std::ceil(s)));
    if (s < 90 * 60) return TabPortals::tr("%n minute(s)", "", int(std::lround(s / 60)));
    if (s < 48 * 3600) return TabPortals::tr("%1 hours").arg(s / 3600, 0, 'f', 1);
    const double days = s / 86400;
    if (days < 730) return TabPortals::tr("%1 days").arg(days, 0, 'f', days < 10 ? 1 : 0);
    const double years = days / 365.25;
    if (years < 1e4) return TabPortals::tr("%1 years").arg(QLocale().toString(qint64(std::llround(years))));
    return TabPortals::tr("%1 years").arg(bigNumber(years));
}

QString seedText(uint64_t seed)
{
    return QString::number(qint64(seed));
}

QString litText(const EyePortalRecord& r)
{
    QString s = r.lit ? TabPortals::tr("yes") : r.opening ? TabPortals::tr("partly")
        : TabPortals::tr("no");
    return r.litExact ? s : s + "?";
}

QColor uncertainColor(const QPalette& pal)
{
    return pal.color(QPalette::Base).lightness() < 128 ? QColor(240, 170, 60) : QColor(176, 96, 0);
}

// Word-wrapped labels report a small minimum height; give them exactly the
// height of their current text so that lines are neither cut off nor padded.
void fitLabelHeight(QLabel *label)
{
    const QMargins m = label->contentsMargins();
    const int w = label->width() - m.left() - m.right();
    if (w <= 0)
        return;
    const QRect text = label->fontMetrics().boundingRect(QRect(0, 0, w, 1 << 20),
        int(Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap), label->text());
    label->setFixedHeight(text.height() + m.top() + m.bottom());
}

// Refits a label whenever its width changes (QTabWidget pages do not pass
// height-for-width on to their layouts).
class LabelFitter : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Resize)
            if (QLabel *label = qobject_cast<QLabel*>(watched))
                fitLabelHeight(label);
        return QObject::eventFilter(watched, event);
    }
};

QString framesTip()
{
    return TabPortals::tr("The 12 frames seen from above, north up. A filled frame has an eye, "
        "a hollow one has none. Frames with an orange edge depend on caves and other "
        "terrain: their state is Seed Atlas's best estimate (about 95% right per "
        "frame). A purple center means the portal is lit.");
}

} // namespace


//==============================================================================
// Analysis thread
//==============================================================================

void AnalysisPortals::run()
{
    BusyActivity busy; // not to be throttled while out of view
    rows.clear();
    if (wi.mc < MC_1_18 || wi.mc > MC_NEWEST)
        return;
    Generator g;
    setupGenerator(&g, wi.mc, wi.genFlags());
    applySeed(&g, DIM_OVERWORLD, wi.seed);
    StrongholdIter it;
    initFirstStronghold(&it, wi.mc, wi.seed);
    // the layout work state is large; keep it off the thread's stack
    std::unique_ptr<Stronghold> sh(new Stronghold);
    for (int i = 0; i < count && !stop; i++)
    {
        const int more = nextStronghold(&it, &g);
        EndPortal portal;
        if (getEndPortal(&portal, sh.get(), wi.mc, wi.seed, it.pos.x >> 4, it.pos.z >> 4))
            rows.push_back(EyePortalRecord::fromPortal(wi.seed, i, portal));
        if (more <= 0)
            break;
    }
}


//==============================================================================
// Table model and frame painter
//==============================================================================

PortalTableModel::PortalTableModel(bool withSeed, QObject *parent)
    : QAbstractTableModel(parent)
{
    if (withSeed)
        columns.push_back(SEED);
    for (Column c : { INDEX, EYES, CERTAIN, EXACT, LIT, FRAMES, X, Y, Z })
        columns.push_back(c);
}

int PortalTableModel::column(Column kind) const
{
    auto it = std::find(columns.begin(), columns.end(), kind);
    return it == columns.end() ? -1 : int(it - columns.begin());
}

int PortalTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(rows.size());
}

int PortalTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(columns.size());
}

QVariant PortalTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(rows.size()))
        return QVariant();
    const EyePortalRecord& r = rows[size_t(index.row())];
    const Column c = columns[size_t(index.column())];
    if (role == Qt::DisplayRole)
    {
        switch (c)
        {
        case SEED:      return seedText(r.seed);
        case INDEX:     return int(r.index) + 1;
        case X:         return r.x;
        case Y:         return r.y;
        case Z:         return r.z;
        case EYES:      return r.hasUncertainEyes() ? QString("%1*").arg(r.eyeCount)
                            : QString::number(r.eyeCount);
        case CERTAIN:   return int(r.exactEyeCount);
        case EXACT:     return QString("%1/12").arg(r.exactFrames);
        case LIT:       return litText(r);
        case FRAMES:    return r.frameText();
        }
    }
    else if (role == Qt::TextAlignmentRole)
    {
        if (c == SEED || c == FRAMES || c == LIT)
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        return int(Qt::AlignRight | Qt::AlignVCenter);
    }
    else if (role == Qt::ForegroundRole)
    {
        if ((c == EYES && r.hasUncertainEyes()) || (c == LIT && !r.litExact))
            return uncertainColor(QPalette());
    }
    else if (role == Qt::ToolTipRole)
    {
        if (c == EYES && r.hasUncertainEyes())
            return TabPortals::tr("%1 of these eyes are on frames that depend on caves "
                "and other terrain; only %2 are certain.")
                .arg(r.eyeCount - r.exactEyeCount).arg(r.exactEyeCount);
        if (c == LIT && !r.litExact)
            return TabPortals::tr("Depends on frames whose state is not certain.");
        if (c == FRAMES)
            return framesTip();
    }
    return QVariant();
}

QVariant PortalTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= int(columns.size()))
        return QVariant();
    const Column c = columns[size_t(section)];
    if (role == Qt::DisplayRole)
    {
        switch (c)
        {
        case SEED:      return tr("seed");
        case INDEX:     return tr("#");
        case X:         return tr("x");
        case Y:         return tr("y");
        case Z:         return tr("z");
        case EYES:      return tr("eyes");
        case CERTAIN:   return tr("certain");
        case EXACT:     return tr("known");
        case LIT:       return tr("lit");
        case FRAMES:    return tr("frames");
        }
    }
    else if (role == Qt::ToolTipRole)
    {
        switch (c)
        {
        case INDEX:     return tr("Stronghold number, in the order the game places them.");
        case X: case Y: case Z:
            return tr("Center of the portal opening.");
        case EYES:      return tr("Eyes in all 12 frames. * marks counts that include eyes on frames "
                            "whose state depends on terrain (best estimate).");
        case CERTAIN:   return tr("Eyes that follow from the seed alone.");
        case EXACT:     return tr("Known frames: frames whose state follows from the seed alone. "
                            "The others depend on caves and other terrain.");
        case LIT:       return tr("Whether world generation activates the portal. "
                            "? marks an estimate that depends on terrain.");
        case FRAMES:    return framesTip();
        default:        break;
        }
    }
    return QVariant();
}

void PortalTableModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= int(columns.size()))
        return;
    const Column c = columns[size_t(column)];
    const auto key = [c](const EyePortalRecord& r) -> int64_t {
        switch (c)
        {
        case SEED:      return int64_t(r.seed);
        case INDEX:     return r.index;
        case X:         return r.x;
        case Y:         return r.y;
        case Z:         return r.z;
        case EYES:      return r.eyeCount * 16 + r.exactEyeCount;
        case CERTAIN:   return r.exactEyeCount * 16 + r.eyeCount;
        case EXACT:     return r.exactFrames;
        case LIT:       return int(r.lit) * 2 + r.litExact;
        case FRAMES:    return int64_t(r.eyes) << 16 | r.exact;
        }
        return 0;
    };
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    std::vector<int> perm(rows.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::stable_sort(perm.begin(), perm.end(), [&](int a, int b) {
        const int64_t ka = key(rows[size_t(a)]), kb = key(rows[size_t(b)]);
        return order == Qt::AscendingOrder ? ka < kb : ka > kb;
    });
    std::vector<EyePortalRecord> sorted;
    sorted.reserve(rows.size());
    std::vector<int> newRow(rows.size());
    for (size_t i = 0; i < perm.size(); i++)
    {
        sorted.push_back(rows[size_t(perm[i])]);
        newRow[size_t(perm[i])] = int(i);
    }
    rows.swap(sorted);
    const QModelIndexList old = persistentIndexList();
    QModelIndexList updated;
    for (const QModelIndex& idx : old)
        updated.append(index(newRow[size_t(idx.row())], idx.column()));
    changePersistentIndexList(old, updated);
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void PortalTableModel::setRows(std::vector<EyePortalRecord>&& newRows)
{
    beginResetModel();
    rows = std::move(newRows);
    endResetModel();
}

void PortalTableModel::appendRows(const std::vector<EyePortalRecord>& more)
{
    if (more.empty())
        return;
    beginInsertRows(QModelIndex(), int(rows.size()), int(rows.size() + more.size()) - 1);
    rows.insert(rows.end(), more.begin(), more.end());
    endInsertRows();
}

void PortalTableModel::clear()
{
    beginResetModel();
    rows.clear();
    rows.shrink_to_fit();
    endResetModel();
}

void PortalTableModel::writeCsv(CsvWriter& csv) const
{
    QStringList head;
    if (column(SEED) >= 0)
        head << "seed";
    head << "stronghold" << "x" << "y" << "z" << "eyes" << "certain_eyes"
         << "certain_frames" << "uncertain_eyes" << "lit" << "lit_certain"
         << "frames_clockwise_from_nw";
    csv.row(head);
    for (const EyePortalRecord& r : rows)
    {
        QStringList f;
        if (column(SEED) >= 0)
            f << seedText(r.seed);
        f << QString::number(int(r.index) + 1) << QString::number(r.x)
          << QString::number(r.y) << QString::number(r.z)
          << QString::number(r.eyeCount) << QString::number(r.exactEyeCount)
          << QString::number(r.exactFrames)
          << QString::number(r.eyeCount - r.exactEyeCount)
          << (r.lit ? "1" : "0") << (r.litExact ? "1" : "0") << r.frameText();
        csv.row(f);
    }
}

namespace {

struct GlyphColors
{
    QColor eye, frame, frameFill, uncertain, portal;
    explicit GlyphColors(bool dark)
        : eye(dark ? QColor(70, 210, 165) : QColor(16, 128, 96))
        // neutral grey, so that "no eye" never reads like the orange "uncertain"
        , frame(dark ? QColor(168, 174, 186) : QColor(112, 116, 126))
        , frameFill(dark ? QColor(168, 174, 186, 30) : QColor(112, 116, 126, 26))
        , uncertain(dark ? QColor(245, 170, 55) : QColor(204, 112, 0))
        , portal(dark ? QColor(126, 86, 214) : QColor(52, 26, 104))
    {}
};

bool isDark(const QPalette& pal)
{
    return pal.color(QPalette::Base).lightness() < 128;
}

// One frame: filled = eye, hollow = no eye; an orange edge marks a frame whose
// state depends on terrain (an uncertain eye is also drawn paler).
void paintFrameCell(QPainter *p, const QRectF& box, bool eye, bool sure, const GlyphColors& c)
{
    const QRectF in = box.adjusted(0.5, 0.5, -0.5, -0.5);
    if (eye)
    {
        QColor fill = c.eye;
        if (!sure)
            fill.setAlphaF(0.55f);
        p->fillRect(box, fill);
    }
    else
        p->fillRect(box, c.frameFill);
    p->setPen(QPen(sure ? (eye ? c.eye.darker(130) : c.frame) : c.uncertain,
        sure ? 1.0 : 1.6));
    p->setBrush(Qt::NoBrush);
    p->drawRect(sure ? in : box.adjusted(0.8, 0.8, -0.8, -0.8));
}

// The legend below the tables, drawn with the same cells as the glyph.
class PortalLegend : public QWidget
{
public:
    explicit PortalLegend(QWidget *parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setToolTip(framesTip());
    }
    QSize sizeHint() const override
    {
        return QSize(200, fontMetrics().height() + 6);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        // the application palette follows the light/dark setting of the app
        const GlyphColors c(isDark(QGuiApplication::palette()));
        const int box = std::min(10, fontMetrics().ascent());
        const int y = (height() - box) / 2;
        int x = 0;
        const auto item = [&](const QString& text, const std::function<void(const QRectF&)>& draw) {
            draw(QRectF(x, y, box, box));
            x += box + 5;
            p.setPen(QGuiApplication::palette().color(QPalette::WindowText));
            p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignVCenter | Qt::AlignLeft, text);
            x += fontMetrics().horizontalAdvance(text) + 14;
        };
        item(TabPortals::tr("eye"), [&](const QRectF& r) { paintFrameCell(&p, r, true, true, c); });
        item(TabPortals::tr("no eye"), [&](const QRectF& r) { paintFrameCell(&p, r, false, true, c); });
        item(TabPortals::tr("uncertain"), [&](const QRectF& r) {
            paintFrameCell(&p, QRectF(r.x(), r.y(), r.width(), r.height()), true, false, c);
            x += box + 2;
            paintFrameCell(&p, QRectF(r.x() + box + 2, r.y(), r.width(), r.height()), false, false, c);
        });
        item(TabPortals::tr("lit"), [&](const QRectF& r) { p.fillRect(r, c.portal); });
    }
};

} // namespace

void PortalFramesDelegate::paintPortal(QPainter *p, const QRectF& rect, const EyePortalRecord& r,
    bool dark)
{
    const qreal cell = std::floor(std::min(rect.width(), rect.height()) / 5.0);
    if (cell < 3)
        return;
    const QPointF o(rect.left() + std::floor((rect.width() - 5 * cell) / 2),
        rect.top() + std::floor((rect.height() - 5 * cell) / 2));
    const qreal gap = cell >= 6 ? 1 : 0;
    const GlyphColors c(dark);

    p->save();
    p->setRenderHint(QPainter::Antialiasing, false);
    for (int b = 0; b < 9; b++)
    {
        if (!((r.opening >> b) & 1))
            continue;
        const int dx = b % 3 - 1, dz = b / 3 - 1;
        p->fillRect(QRectF(o.x() + (dx + 2) * cell, o.y() + (dz + 2) * cell, cell, cell), c.portal);
    }
    for (int i = 0; i < 12; i++)
    {
        const int k = r.cell[i];
        const QRectF box(o.x() + (k % 5) * cell + gap / 2, o.y() + (k / 5) * cell + gap / 2,
            cell - gap, cell - gap);
        paintFrameCell(p, box, (r.eyes >> i) & 1, (r.exact >> i) & 1, c);
    }
    p->restore();
}

void PortalFramesDelegate::paint(QPainter *painter, const QStyleOptionViewItem& option,
    const QModelIndex& index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    opt.text.clear();
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const PortalTableModel *model = qobject_cast<const PortalTableModel*>(index.model());
    if (!model || index.row() >= model->rowCount())
        return;
    const QRectF rect = QRectF(option.rect).adjusted(3, 1, -3, -1);
    paintPortal(painter, QRectF(rect.left(), rect.top(), rect.height(), rect.height()),
        model->record(index.row()), isDark(option.palette));
}

QSize PortalFramesDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    (void) index;
    const int h = std::max(option.fontMetrics.height() + 8, 42);
    return QSize(h + 8, h);
}


//==============================================================================
// Tab
//==============================================================================

TabPortals::TabPortals(MainWindow *parent)
    : QWidget(parent), parent(parent), analysis(this), driver(this)
{
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    labelNotice = new QLabel(this);
    labelNotice->setWordWrap(true);
    labelNotice->setContentsMargins(0, 0, 0, 6);
    labelNotice->setObjectName("labelNotice");
    labelNotice->hide();
    outer->addWidget(labelNotice);
    pages = new QTabWidget(this);
    pages->setObjectName("portalPages");
    pages->setDocumentMode(true);
    outer->addWidget(pages, 1);

    const auto withInfo = [this](QWidget *w, const QString& info) {
        QWidget *box = new QWidget(this);
        QHBoxLayout *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(w, 1);
        row->addWidget(createInfoButton(box, tr("Help: End portals"), info));
        return box;
    };

    // --- analysis of the shown seed -----------------------------------------
    analysisPage = new QWidget(this);
    QVBoxLayout *al = new QVBoxLayout(analysisPage);
    QLabel *intro = new QLabel(tr("Eyes of Ender in the End portals of the seed shown on the map."),
        analysisPage);
    intro->setWordWrap(true);
    intro->setObjectName("labelDescription");
    al->addWidget(withInfo(intro, tr("Each frame of an End portal gets an eye with a chance "
        "of 10%. Frames whose state follows from the seed alone are certain; the others "
        "depend on caves in the same chunk and show Seed Atlas's best estimate (about 95% "
        "right per frame). Frames marked certain follow the game's rules exactly. In 26.3, "
        "however, about 1 in 300 strongholds is placed slightly differently by the game "
        "than computed here (a known biome tie-break difference), so confirm a result in "
        "the game.")));
    QGridLayout *ag = new QGridLayout;
    ag->addWidget(new QLabel(tr("Strongholds:"), analysisPage), 0, 0);
    spinCount = new QSpinBox(analysisPage);
    spinCount->setObjectName("spinPortalCount");
    spinCount->setRange(1, 128);
    spinCount->setValue(3);
    ag->addWidget(withInfo(spinCount, tr("Number of strongholds to list, in the order the game "
        "places them (the first ring has 3). Every 1.18+ world has 128.")), 0, 1);
    ag->setColumnStretch(1, 1);
    al->addLayout(ag);
    analysisModel = new PortalTableModel(false, this);
    analysisTable = createTable(analysisModel);
    analysisTable->setObjectName("tablePortals");
    setEmptyHint(analysisTable, tr("No portals listed yet. Press Analyze."));
    al->addWidget(analysisTable, 1);
    al->addWidget(new PortalLegend(analysisPage));
    labelAnalysis = new QLabel(analysisPage);
    labelAnalysis->setObjectName("labelStatus");
    labelAnalysis->setWordWrap(true);
    al->addWidget(labelAnalysis);
    QHBoxLayout *aa = new QHBoxLayout;
    pushExportAnalysis = new QPushButton(tr("Export..."), analysisPage);
    pushExportAnalysis->setEnabled(false);
    pushAnalyze = new QPushButton(tr("Analyze"), analysisPage);
    pushAnalyze->setObjectName("pushPortalAnalyze");
    pushAnalyze->setProperty("primary", true);
    aa->addWidget(pushExportAnalysis);
    aa->addWidget(pushAnalyze);
    al->addLayout(aa);
    pages->addTab(analysisPage, tr("This seed"));

    // --- seed search ---------------------------------------------------------
    QWidget *searchPage = new QWidget(this);
    QVBoxLayout *sl = new QVBoxLayout(searchPage);
    QGridLayout *sg = new QGridLayout;
    sg->addWidget(new QLabel(tr("Minimum eyes:"), searchPage), 0, 0);
    spinMinEyes = new QSpinBox(searchPage);
    spinMinEyes->setObjectName("spinMinEyes");
    spinMinEyes->setRange(1, 12);
    spinMinEyes->setValue(12);
    sg->addWidget(withInfo(spinMinEyes, tr("Report portals with at least this many eyes. "
        "Each frame has an eye with a chance of 10%, so 12 eyes happen once in "
        "about 10¹² portals.")), 0, 1);
    sg->addWidget(new QLabel(tr("Strongholds per seed:"), searchPage), 1, 0);
    spinStrongholds = new QSpinBox(searchPage);
    spinStrongholds->setObjectName("spinSearchStrongholds");
    spinStrongholds->setRange(1, 128);
    spinStrongholds->setValue(3);
    sg->addWidget(withInfo(spinStrongholds, tr("Examine the first strongholds of every seed. "
        "More strongholds find more portals per seed but each one is farther away.")), 1, 1);
    checkExactOnly = new QCheckBox(tr("Certain eyes only"), searchPage);
    checkExactOnly->setObjectName("checkExactOnly");
    checkExactOnly->setChecked(true);
    sg->addWidget(withInfo(checkExactOnly, tr("Count only eyes that follow from the seed alone. "
        "Other frames lie in chunks where earlier stronghold rooms consume random "
        "numbers depending on caves; their state is a best estimate that is right "
        "for about 95% of frames. About 45% of portals have only certain frames. "
        "Frames marked certain follow the game's rules exactly. In 26.3, however, about "
        "1 in 300 strongholds is placed slightly differently by the game than computed "
        "here (a known biome tie-break difference), so confirm a hit in the game.")), 2, 0, 1, 2);
    sg->addWidget(new QLabel(tr("Start family:"), searchPage), 3, 0);
    lineStart = new QLineEdit("0", searchPage);
    lineStart->setObjectName("lineStartFamily");
    sg->addWidget(withInfo(lineStart, tr("The search goes through the 2⁴⁸ seed families in order, starting "
        "at this one. A family is the 65536 seeds that share their lower 48 bits (the "
        "structure seed). Enter a decimal number, or a hexadecimal one starting with 0x.")), 3, 1);
    checkRandom = new QCheckBox(tr("Random start"), searchPage);
    checkRandom->setObjectName("checkRandomStart");
    checkRandom->setChecked(true);
    sg->addWidget(withInfo(checkRandom, tr("A new search starts at a random family, so separate "
        "searches cover different seeds. A stopped search continues where it stopped "
        "as long as its settings and the version stay the same.")), 4, 0, 1, 2);
    sg->setColumnStretch(1, 1);
    sl->addLayout(sg);
    labelExpectation = new QLabel(searchPage);
    labelExpectation->setObjectName("labelExpectation");
    labelExpectation->setWordWrap(true);
    sl->addWidget(labelExpectation);
    searchModel = new PortalTableModel(true, this);
    searchTable = createTable(searchModel);
    searchTable->setObjectName("tablePortalSeeds");
    setEmptyHint(searchTable, tr("No matching portals yet. Start the search."));
    sl->addWidget(searchTable, 1);
    sl->addWidget(new PortalLegend(searchPage));
    labelSearch = new QLabel(searchPage);
    labelSearch->setObjectName("labelSearchStatus");
    labelSearch->setWordWrap(true);
    labelSearch->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sl->addWidget(labelSearch);
    QHBoxLayout *sa = new QHBoxLayout;
    pushReset = new QPushButton(tr("Reset"), searchPage);
    pushReset->setObjectName("pushPortalReset");
    pushReset->setToolTip(tr("Forget where this search stopped and clear its results."));
    pushExportSearch = new QPushButton(tr("Export..."), searchPage);
    pushExportSearch->setObjectName("pushPortalExport");
    pushSearch = new QPushButton(tr("Start"), searchPage);
    pushSearch->setObjectName("pushPortalSearch");
    pushSearch->setProperty("primary", true);
    sa->addWidget(pushReset);
    sa->addWidget(pushExportSearch);
    sa->addWidget(pushSearch);
    sl->addLayout(sa);
    pages->addTab(searchPage, tr("Seed search"));

    LabelFitter *fitter = new LabelFitter(this);
    for (QLabel *label : { labelExpectation, labelSearch, labelAnalysis })
    {
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        label->installEventFilter(fitter);
    }

    searchTimer = new QTimer(this);
    searchTimer->setInterval(250);

    connect(spinCount, &QSpinBox::valueChanged, this, [this]() { refresh(); });
    connect(pushAnalyze, &QPushButton::clicked, this, &TabPortals::onAnalyzeClicked);
    connect(pushExportAnalysis, &QPushButton::clicked, this, &TabPortals::onExportAnalysis);
    connect(&analysis, &QThread::finished, this, &TabPortals::onAnalysisFinished, Qt::QueuedConnection);
    connect(analysisTable->selectionModel(), &QItemSelectionModel::currentRowChanged,
        this, &TabPortals::onAnalysisRowChanged);
    connect(analysisTable, &QTableView::clicked, this, &TabPortals::onAnalysisRowChanged);
    addResultMenu(analysisTable, [this]() { onExportAnalysis(); });

    connect(pushSearch, &QPushButton::clicked, this, &TabPortals::onSearchClicked);
    connect(pushReset, &QPushButton::clicked, this, &TabPortals::onResetClicked);
    connect(pushExportSearch, &QPushButton::clicked, this, &TabPortals::onExportSearch);
    connect(&driver, &EyeSearchDriver::finished, this, &TabPortals::onSearchFinished);
    connect(searchTimer, &QTimer::timeout, this, &TabPortals::onSearchTimer);
    connect(searchTable, &QTableView::clicked, this, &TabPortals::onSearchRowActivated);
    connect(searchTable, &QTableView::activated, this, &TabPortals::onSearchRowActivated);
    addResultMenu(searchTable, [this]() { onExportSearch(); });
    connect(checkRandom, &QCheckBox::toggled, this, [this]() { updateSearchControls(); });
    for (QSpinBox *spin : { spinMinEyes, spinStrongholds })
        connect(spin, &QSpinBox::valueChanged, this, [this]() {
            updateSearchControls();
            updateExpectation();
        });
    connect(checkExactOnly, &QCheckBox::toggled, this, [this]() {
        updateSearchControls();
        updateExpectation();
    });
    connect(lineStart, &QLineEdit::textChanged, this, [this]() { updateSearchControls(); });

    updateSearchControls();
    updateSearchStatus();
    updateExpectation();
}

TabPortals::~TabPortals()
{
    searchTimer->stop();
    // waiting can deliver the queued 'finished'; it must not start over
    analysisAgain = false;
    analysis.stop = true;
    waitForMappingThread(analysis);
    // the driver stops and joins its workers when it is destroyed
}

QTableView *TabPortals::createTable(PortalTableModel *model)
{
    QTableView *view = new QTableView(this);
    view->setModel(model);
    view->setItemDelegateForColumn(model->column(PortalTableModel::FRAMES),
        new PortalFramesDelegate(view));
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setWordWrap(false);
    view->setSortingEnabled(true);
    view->horizontalHeader()->setSortIndicatorShown(false);
    view->horizontalHeader()->setSectionsClickable(true);
    connect(view->horizontalHeader(), &QHeaderView::sectionClicked, view, [view]() {
        view->horizontalHeader()->setSortIndicatorShown(true);
    });
    view->horizontalHeader()->setMinimumSectionSize(24);
    view->horizontalHeader()->setStretchLastSection(true);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->verticalHeader()->setDefaultSectionSize(std::max(fontMetrics().height() + 8, 42));
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    // compact default widths: the panel is narrow
    const QFontMetrics fm = fontMetrics();
    for (int c = 0; c < model->columnCount(); c++)
    {
        const QString h = model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        // room for the header text, its margins and the sort indicator
        int w = fm.horizontalAdvance(h) + 30;
        switch (model->columnKind(c))
        {
        case PortalTableModel::SEED:    w = fm.horizontalAdvance("-9223372036854775808") + 12; break;
        case PortalTableModel::FRAMES:  w = std::max(w, view->verticalHeader()->defaultSectionSize() + 10); break;
        case PortalTableModel::X: case PortalTableModel::Z:
            w = std::max(w, fm.horizontalAdvance("-299999") + 14); break;
        case PortalTableModel::Y:       w = std::max(w, fm.horizontalAdvance("-64") + 14); break;
        case PortalTableModel::EXACT:   w = std::max(w, fm.horizontalAdvance("12/12") + 14); break;
        case PortalTableModel::LIT:     w = std::max(w, fm.horizontalAdvance("partly?") + 14); break;
        default: break;
        }
        view->setColumnWidth(c, w);
    }
    return view;
}

void TabPortals::save(QSettings& settings)
{
    // Before load() ran, the widgets hold defaults and the search state was
    // never read: nothing to save, and nothing may be overwritten.
    if (!loaded)
        return;
    settings.setValue("portals/page", pages->currentIndex());
    settings.setValue("portals/analysisCount", spinCount->value());
    settings.setValue("portals/minEyes", spinMinEyes->value());
    settings.setValue("portals/strongholds", spinStrongholds->value());
    settings.setValue("portals/exactOnly", checkExactOnly->isChecked());
    settings.setValue("portals/start", lineStart->text());
    settings.setValue("portals/randomStart", checkRandom->isChecked());
    if (saveSearchState())
        settings.remove("portals/search");  // the format of earlier versions
}

/* Writes the search (settings, position, counters and all listed hits) to
 * the state file, atomically. A running search hands over its queued hits
 * together with the position, so that every family saved as done has its
 * hits in the file. Returns false if the file could not be written. */
bool TabPortals::saveSearchState()
{
    EyeSearchProgress p = progress;
    if (driver.isRunning())
    {
        std::vector<EyePortalRecord> pending;
        p = driver.takeProgressAndHits(pending);
        searchModel->appendRows(pending);
    }
    autosaveClock.start();
    const QString path = searchStatePath();
    if (!searchValid && searchModel->rowCount() == 0)
    {
        QFile::remove(path);
        return true;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_5_15);
    const quint32 version = searchSettings.amplified ? STATE_VERSION_AMPLIFIED : STATE_VERSION;
    out << STATE_MAGIC << version;
    // the version as its name, as everywhere else (enum values move when
    // versions are added)
    out << searchValid << QString::fromLatin1(mc2str(searchSettings.mc)) << searchSettings.large;
    if (version >= STATE_VERSION_AMPLIFIED)
        out << searchSettings.amplified;
    out << qint32(searchSettings.numStrongholds) << qint32(searchSettings.minEyes)
        << searchSettings.exactOnly << quint64(searchSettings.start);
    out << quint64(p.next) << quint32(p.redo.size());
    for (uint64_t off : p.redo)
        out << quint64(off);
    out << quint64(p.familiesDone) << quint64(p.hits) << quint64(p.dropped);
    for (uint64_t n : p.hitsByEyes)
        out << quint64(n);
    out << p.seconds << measuredRate;
    const std::vector<EyePortalRecord>& rows = searchModel->records();
    out << quint32(rows.size());
    for (const EyePortalRecord& r : rows)
        writeRecord(out, r);
    if (out.status() != QDataStream::Ok)
    {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

/* Reads the state file. Returns false if there is none or it cannot be read
 * (unknown format, unknown version name), so that load() falls back to the
 * settings of earlier versions. */
bool TabPortals::loadSearchState()
{
    QFile file(searchStatePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_5_15);
    quint32 magic = 0, version = 0;
    in >> magic >> version;
    if (magic != STATE_MAGIC || (version != STATE_VERSION && version != STATE_VERSION_AMPLIFIED))
        return false;
    bool valid, large, amplified = false, exactOnly;
    QString mcName;
    qint32 strongholds, minEyes;
    quint64 start, next, familiesDone, hits, dropped;
    quint32 nredo;
    in >> valid >> mcName >> large;
    if (version >= STATE_VERSION_AMPLIFIED)
        in >> amplified;
    in >> strongholds >> minEyes >> exactOnly >> start >> next >> nredo;
    if (in.status() != QDataStream::Ok || nredo > (1u << 20))
        return false;
    const int mc = str2mc(mcName.toLatin1().constData());
    if (mc == MC_UNDEF)
        return false;
    EyeSearchProgress p;
    p.next = next;
    for (quint32 i = 0; i < nredo; i++)
    {
        quint64 off;
        in >> off;
        p.redo.push_back(off);
    }
    in >> familiesDone >> hits >> dropped;
    p.familiesDone = familiesDone;
    p.hits = hits;
    p.dropped = dropped;
    for (uint64_t& n : p.hitsByEyes)
    {
        quint64 v;
        in >> v;
        n = v;
    }
    double rate = 0;
    in >> p.seconds >> rate;
    quint32 nrows = 0;
    in >> nrows;
    if (in.status() != QDataStream::Ok || nrows > LIST_LIMIT)
        return false;
    std::vector<EyePortalRecord> rows;
    rows.reserve(nrows);
    for (quint32 i = 0; i < nrows; i++)
        rows.push_back(readRecord(in));
    if (in.status() != QDataStream::Ok)
        return false;

    searchValid = valid;
    searchSettings = EyeSearchSettings();
    searchSettings.mc = mc;
    searchSettings.large = large;
    searchSettings.amplified = amplified && !large && mc >= MC_1_18;
    searchSettings.numStrongholds = strongholds;
    searchSettings.minEyes = minEyes;
    searchSettings.exactOnly = exactOnly;
    searchSettings.start = start & LOW48;
    progress = p;
    measuredRate = rate;
    searchModel->setRows(std::move(rows));
    return true;
}

/* The search as earlier versions stored it in the settings: the version as
 * an enum value and the hits as raw records. */
void TabPortals::loadLegacySearch(QSettings& settings)
{
    searchValid = settings.value("portals/search/valid", false).toBool();
    if (!searchValid)
        return;
    searchSettings.mc = settings.value("portals/search/mc").toInt();
    searchSettings.large = settings.value("portals/search/large").toBool();
    searchSettings.amplified = false; // these versions had no Amplified
    searchSettings.numStrongholds = settings.value("portals/search/strongholds").toInt();
    searchSettings.minEyes = settings.value("portals/search/minEyes").toInt();
    searchSettings.exactOnly = settings.value("portals/search/exactOnly").toBool();
    searchSettings.start = settings.value("portals/search/start").toString().toULongLong() & LOW48;
    progress = EyeSearchProgress();
    progress.next = settings.value("portals/search/next").toString().toULongLong();
    for (const QString& s : settings.value("portals/search/redo").toStringList())
        progress.redo.push_back(s.toULongLong());
    progress.familiesDone = settings.value("portals/search/familiesDone").toString().toULongLong();
    progress.hits = settings.value("portals/search/hits").toString().toULongLong();
    progress.dropped = settings.value("portals/search/dropped").toString().toULongLong();
    const QStringList byEyes = settings.value("portals/search/hitsByEyes").toStringList();
    for (int i = 0; i < 13 && i < byEyes.size(); i++)
        progress.hitsByEyes[i] = byEyes[i].toULongLong();
    progress.seconds = settings.value("portals/search/seconds", 0.0).toDouble();
    measuredRate = settings.value("portals/search/rate", 0.0).toDouble();

    const QByteArray blob = settings.value("portals/search/results").toByteArray();
    quint32 header[2] = {};
    std::vector<EyePortalRecord> rows;
    if (blob.size() >= qsizetype(sizeof(header)))
    {
        std::memcpy(header, blob.constData(), sizeof(header));
        const size_t n = (size_t(blob.size()) - sizeof(header)) / sizeof(EyePortalRecord);
        if (header[0] == RESULTS_MAGIC && header[1] == sizeof(EyePortalRecord))
        {
            rows.resize(n);
            std::memcpy(rows.data(), blob.constData() + sizeof(header), n * sizeof(EyePortalRecord));
        }
    }
    // earlier versions kept only the best 2000 hits: the others are counted
    // as not listed, so the counters and the list agree
    if (progress.hits > rows.size() + progress.dropped)
        progress.dropped = progress.hits - rows.size();
    searchModel->setRows(std::move(rows));
}

void TabPortals::load(QSettings& settings)
{
    // Tabs are reloaded whenever they are shown; this one keeps its own
    // state while the program runs and reads the settings only once.
    if (loaded)
    {
        refresh();
        return;
    }
    loaded = true;
    pages->setCurrentIndex(settings.value("portals/page", pages->currentIndex()).toInt());
    spinMinEyes->setValue(settings.value("portals/minEyes", spinMinEyes->value()).toInt());
    spinStrongholds->setValue(settings.value("portals/strongholds", spinStrongholds->value()).toInt());
    checkExactOnly->setChecked(settings.value("portals/exactOnly", checkExactOnly->isChecked()).toBool());
    lineStart->setText(settings.value("portals/start", lineStart->text()).toString());
    checkRandom->setChecked(settings.value("portals/randomStart", checkRandom->isChecked()).toBool());

    if (!driver.isRunning() && !loadSearchState())
        loadLegacySearch(settings);
    spinCount->setValue(settings.value("portals/analysisCount", spinCount->value()).toInt());
    updateSearchControls();
    updateSearchStatus();
    updateExpectation();
    refresh();
}

bool TabPortals::contextSupported(QString *reason) const
{
    WorldInfo wi;
    parent->getSeed(&wi, false);
    if (wi.datapack())
    {
        // Strongholds are placed by the built-in generator, which does not
        // know the world of the data packs. The window says so above the
        // tool (see MainWindow::updateDatapackNotice), hence no text here.
        if (reason)
            reason->clear();
        return false;
    }
    if (wi.mc < MC_1_18 || wi.mc > MC_NEWEST)
    {
        if (reason)
            *reason = tr("End portal eyes are only known for Minecraft 1.18 to %1. "
                "Choose one of these versions to use this tool.").arg(mc2str(MC_NEWEST));
        return false;
    }
    if (parent->getDim() != DIM_OVERWORLD)
    {
        if (reason)
            *reason = tr("Strongholds and their End portals are in the Overworld. "
                "Switch the map to the Overworld to see the portals of this seed.");
        return false;
    }
    return true;
}

void TabPortals::refresh()
{
    QString reason;
    const bool ok = contextSupported(&reason);
    WorldInfo wi;
    parent->getSeed(&wi, false);
    if (ok && wi.mc > PORTAL_EYES_VERIFIED)
        reason = tr("The rules for End portal eyes in %1 have not been checked against the game yet; "
            "they are computed as in %2. Check results in the game.")
            .arg(mc2str(wi.mc)).arg(mc2str(PORTAL_EYES_VERIFIED));
    labelNotice->setText(reason);
    labelNotice->setVisible(!reason.isEmpty());
    analysisPage->setEnabled(ok);
    updateSearchControls();
    if (!ok)
    {
        analysis.stop = true;
        analysisAgain = false;
        analysisModel->clear();
        analysisValid = false;
        pushExportAnalysis->setEnabled(false);
        labelAnalysis->clear();
        return;
    }
    const bool same = analysisValid && wi.seed == analysedWorld.seed
        && wi.mc == analysedWorld.mc && wi.worldType() == analysedWorld.worldType()
        && spinCount->value() == analysedCount;
    if (!same && isVisible())
        startAnalysis();
}

void TabPortals::onAnalyzeClicked()
{
    startAnalysis();
}

void TabPortals::startAnalysis()
{
    if (!contextSupported(nullptr))
        return;
    if (analysis.isRunning())
    {   // finish the running one first, then start over with the new input
        analysis.stop = true;
        analysisAgain = true;
        return;
    }
    analysisAgain = false;
    parent->getSeed(&analysis.wi, false);
    analysis.count = spinCount->value();
    analysis.stop = false;
    analysisValid = false;
    labelAnalysis->setText(tr("Analyzing..."));
    pushAnalyze->setEnabled(false);
    analysis.start();
}

void TabPortals::onAnalysisFinished()
{
    pushAnalyze->setEnabled(true);
    if (analysisAgain || analysis.stop)
    {
        if (analysisAgain)
            startAnalysis();
        return;
    }
    analysedWorld = analysis.wi;
    analysedCount = analysis.count;
    analysisValid = true;
    std::vector<EyePortalRecord> rows = std::move(analysis.rows);
    analysis.rows.clear();
    int eyes = 0, exactEyes = 0, certainPortals = 0;
    for (const EyePortalRecord& r : rows)
    {
        eyes += r.eyeCount;
        exactEyes += r.exactEyeCount;
        certainPortals += r.exactFrames == 12;
    }
    const int n = int(rows.size());
    analysisModel->setRows(std::move(rows));
    pushExportAnalysis->setEnabled(n > 0);
    labelAnalysis->setText(tr("Seed %1 (%2): %n portal(s), %3 eyes, %4 of them certain. "
        "%5 portal(s) have only certain frames.", "", n)
        .arg(seedText(analysedWorld.seed)).arg(worldText(analysedWorld.mc, analysedWorld.worldType()))
        .arg(eyes).arg(exactEyes).arg(certainPortals));
    fitLabelHeight(labelAnalysis);
}

void TabPortals::onAnalysisRowChanged(const QModelIndex& current)
{
    if (!current.isValid() || current.row() >= analysisModel->rowCount())
        return;
    showPortal(analysisModel->record(current.row()));
}

void TabPortals::showPortal(const EyePortalRecord& r)
{
    parent->getMapView()->setView(r.x + 0.5, r.z + 0.5);
    Shape circle = { Shape::CIRCLE, DIM_OVERWORLD, 24, Pos{r.x, r.z}, Pos{r.x, r.z} };
    parent->getMapView()->setShapes({circle});
}

void TabPortals::onSearchRowActivated(const QModelIndex& index)
{
    if (!index.isValid() || index.row() >= searchModel->rowCount())
        return;
    const EyePortalRecord r = searchModel->record(index.row());
    WorldInfo wi;
    parent->getSeed(&wi, false);
    const int mc = searchValid ? searchSettings.mc : wi.mc;
    // the hits belong to the world type of the search, not to the one shown
    const bool large = searchValid ? searchSettings.large : wi.large;
    const bool amplified = searchValid ? searchSettings.amplified : wi.amplified;
    if (wi.seed != r.seed || wi.mc != mc || wi.large != large || wi.amplified != amplified
        || parent->getDim() != DIM_OVERWORLD)
    {
        wi.seed = r.seed;
        wi.mc = mc;
        wi.large = large;
        wi.amplified = amplified;
        parent->getMapView()->deleteWorld();
    }
    parent->getMapView()->setView(r.x + 0.5, r.z + 0.5);
    parent->setSeed(wi, DIM_OVERWORLD);
    Shape circle = { Shape::CIRCLE, DIM_OVERWORLD, 24, Pos{r.x, r.z}, Pos{r.x, r.z} };
    parent->getMapView()->setShapes({circle});
}

EyeSearchSettings TabPortals::currentSettings(bool *ok) const
{
    EyeSearchSettings s;
    WorldInfo wi;
    parent->getSeed(&wi, false);
    s.mc = wi.mc;
    s.large = wi.large;
    s.amplified = wi.amplified;
    s.numStrongholds = spinStrongholds->value();
    s.minEyes = spinMinEyes->value();
    s.exactOnly = checkExactOnly->isChecked();
    // decimal, or hexadecimal with 0x (base 0 would read a leading 0 as octal)
    bool parsed = false;
    const QString text = lineStart->text().trimmed();
    const bool hex = text.startsWith("0x", Qt::CaseInsensitive);
    const qulonglong v = (hex ? text.mid(2) : text).toULongLong(&parsed, hex ? 16 : 10);
    s.start = uint64_t(v) & LOW48;
    if (ok)
        *ok = parsed && uint64_t(v) <= LOW48;
    return s;
}

bool TabPortals::hasProgress() const
{
    return searchValid && (progress.next > 0 || !progress.redo.empty());
}

void TabPortals::updateSearchControls()
{
    const bool running = driver.isRunning();
    WorldInfo wi;
    parent->getSeed(&wi, false);
    const bool versionOk = wi.mc >= MC_1_18 && wi.mc <= MC_NEWEST;
    for (QWidget *w : { (QWidget*)spinMinEyes, (QWidget*)spinStrongholds,
            (QWidget*)checkExactOnly, (QWidget*)checkRandom })
        w->setEnabled(!running && versionOk);
    lineStart->setEnabled(!running && versionOk);
    pushReset->setEnabled(!running && (searchValid || searchModel->rowCount() > 0));
    pushExportSearch->setEnabled(searchModel->rowCount() > 0);
    if (running)
    {
        pushSearch->setEnabled(true);
        pushSearch->setText(driver.isStopping() ? tr("Stopping...") : tr("Stop"));
        return;
    }
    pushSearch->setEnabled(versionOk);
    bool ok = false;
    const EyeSearchSettings s = currentSettings(&ok);
    const bool resume = hasProgress() && s == searchSettings && ok;
    pushSearch->setText(resume ? tr("Continue") : tr("Start"));
}

void TabPortals::onSearchClicked()
{
    if (driver.isRunning())
    {
        driver.requestStop();
        updateSearchControls();
        return;
    }
    bool ok = false;
    EyeSearchSettings s = currentSettings(&ok);
    const bool resume = hasProgress() && ok && s == searchSettings;
    if (!resume)
    {
        if (checkRandom->isChecked())
        {
            s.start = QRandomGenerator::global()->generate64() & LOW48;
            const QSignalBlocker block(lineStart);
            lineStart->setText(QString::number(s.start));
        }
        else if (!ok)
        {
            warn(this, tr("The start family must be a number from 0 to %1.")
                .arg(QString::number(LOW48)));
            return;
        }
        searchSettings = s;
        progress = EyeSearchProgress();
        searchModel->clear();
        measuredRate = 0;
    }
    QString error;
    const int threads = std::max(1, configuredThreadCount());
    // stage (b) on the GPU where that is faster and the GPU setting allows it
    if (!driver.start(GpuEyeSearchBackend::choose(threads, searchSettings.config()), searchSettings, progress,
            uint64_t(searchModel->rowCount()), LIST_LIMIT, &error))
    {
        warn(this, error);
        updateSearchControls();
        return;
    }
    searchValid = true;
    runStartFamilies = progress.familiesDone;
    runClock.start();
    rateSamples.clear();
    rateSamples.emplace_back(0, progress.familiesDone);
    searchTimer->start();
    autosaveClock.start();
    updateSearchControls();
    updateSearchStatus();
    updateExpectation();
}

void TabPortals::onResetClicked()
{
    if (driver.isRunning())
        return;
    searchValid = false;
    progress = EyeSearchProgress();
    searchSettings = EyeSearchSettings();
    measuredRate = 0;
    searchModel->clear();
    saveSearchState();
    updateSearchControls();
    updateSearchStatus();
    updateExpectation();
}

void TabPortals::onSearchTimer()
{
    // hits arrive in batches; the list takes what has been queued since the last tick
    std::vector<EyePortalRecord> hits = driver.takeHits();
    searchModel->appendRows(hits);

    const EyeSearchProgress p = driver.progress();
    const qint64 now = runClock.elapsed();
    rateSamples.emplace_back(now, p.familiesDone);
    while (rateSamples.size() > 2 && now - rateSamples.front().first > 10000)
        rateSamples.pop_front();
    // rate over the last seconds once enough families completed, else over the run
    const auto& first = rateSamples.front();
    const uint64_t windowFamilies = p.familiesDone - first.second;
    const qint64 windowMs = now - first.first;
    if (windowMs >= 2000 && windowFamilies >= uint64_t(std::max(4, driver.workerCount())))
        measuredRate = windowFamilies * double(SEEDS_PER_FAMILY) * 1000.0 / windowMs;
    else if (now >= 1000 && p.familiesDone > runStartFamilies)
        measuredRate = (p.familiesDone - runStartFamilies) * double(SEEDS_PER_FAMILY) * 1000.0 / now;
    // the state on disk is at most a minute old, also if the program crashes
    // or is ended without saving
    if (autosaveClock.elapsed() >= AUTOSAVE_MS)
        saveSearchState();
    updateSearchStatus();
    updateExpectation();
    updateSearchControls();
}

void TabPortals::onSearchFinished()
{
    searchTimer->stop();
    searchModel->appendRows(driver.takeHits());
    progress = driver.progress();
    saveSearchState();
    const QString error = driver.errorMessage();
    updateSearchControls();
    updateSearchStatus();
    updateExpectation();
    if (!error.isEmpty())
        warn(this, error);
}

void TabPortals::updateSearchStatus()
{
    const bool running = driver.isRunning();
    if (!searchValid && !running)
    {
        labelSearch->setText(tr("Ready. Press Start to search all seeds for portals with many eyes."));
        fitLabelHeight(labelSearch);
        return;
    }
    const EyeSearchProgress p = running ? driver.progress() : progress;
    const EyeSearchSettings& s = searchSettings;
    QStringList lines;
    QString head = running ? (driver.isStopping() ? tr("Stopping") : tr("Searching"))
        : tr("Stopped");
    lines << tr("%1 %2, %3, at least %4 %5 eyes.")
        .arg(head).arg(worldText(s.mc, worldTypeOf(s.large, s.amplified)))
        .arg(tr("%n stronghold(s) per seed", "", s.numStrongholds)).arg(s.minEyes)
        .arg(s.exactOnly ? tr("certain") : tr("(certain or estimated)"));
    QString speed;
    if (measuredRate > 0)
        speed = tr("%1 seeds/s").arg(rateText(measuredRate));
    else if (running)
        speed = tr("measuring speed");
    QString line = tr("Families: %1 (%2 seeds)").arg(QLocale().toString(qulonglong(p.familiesDone)))
        .arg(bigNumber(double(p.familiesDone) * SEEDS_PER_FAMILY));
    if (!speed.isEmpty())
        line += " · " + speed;
    line += " · " + clockText(p.seconds);
    if (running && driver.gpuUnavailable())
        line += " · " + tr("CPU, %n thread(s) (the GPU failed and is tried again)", "",
            driver.workerCount());
    else if (running)
        line += " · " + tr("%1, %n thread(s)", "", driver.workerCount()).arg(driver.backendName());
    lines << line;
    if (running && p.familiesDone == runStartFamilies)
        lines << tr("Scanning the first families (65536 seeds each)...");
    // the first family not known to be complete; later ones may be done already
    uint64_t resumeAt = p.next;
    for (uint64_t off : p.redo)
        resumeAt = std::min(resumeAt, off);
    lines << tr("Position: family %1 (started at %2).")
        .arg(QString::number((s.start + resumeAt) & LOW48)).arg(QString::number(s.start));

    QString hits = tr("Hits: %1").arg(QLocale().toString(qulonglong(p.hits)));
    QStringList byEyes;
    for (int e = 12; e >= s.minEyes; e--)
        if (p.hitsByEyes[e])
            byEyes << tr("%1× %2 eyes").arg(QLocale().toString(qulonglong(p.hitsByEyes[e]))).arg(e);
    if (!byEyes.isEmpty() && byEyes.size() <= 4)
        hits += " (" + byEyes.join(", ") + ")";
    const uint64_t listed = uint64_t(searchModel->rowCount());
    if (p.hits > listed)
        hits += tr(", %1 listed").arg(QLocale().toString(qulonglong(listed)));
    lines << hits + ".";
    if (p.dropped > 0)
        lines << tr("Not every hit is listed: the list keeps at most %1 hits, and at most %2 "
            "from each family (searches saved by earlier versions of the program kept only "
            "the best 2000). The others were counted only. Raise the minimum eyes "
            "to list all of them.").arg(QLocale().toString(qulonglong(LIST_LIMIT)))
            .arg(QLocale().toString(qulonglong(EyeSearchDriver::FAMILY_KEEP_LIMIT)));

    labelSearch->setText(lines.join('\n'));
    fitLabelHeight(labelSearch);
}

void TabPortals::updateExpectation()
{
    const int minEyes = spinMinEyes->value();
    const bool exactOnly = checkExactOnly->isChecked();
    const int strongholds = spinStrongholds->value();
    const double p = portalHitProbability(minEyes, exactOnly);
    QString text = tr("Chance of a hit: 1 in %1 portals").arg(bigNumber(1.0 / p));
    // only meaningful for the speed of the settings it was measured with
    bool ok = false;
    const bool sameSearch = searchValid && currentSettings(&ok) == searchSettings;
    if (measuredRate > 0 && sameSearch)
    {
        const double perSecond = measuredRate * strongholds * p;
        text += tr(". At %1 seeds/s with %n stronghold(s) per seed, expect one hit every %2 "
            "on average.", "", strongholds).arg(rateText(measuredRate)).arg(durationText(1.0 / perSecond));
    }
    else
        text += tr(" (%1 per seed). The expected time to a hit is shown once the search speed "
            "has been measured.").arg(tr("%n stronghold(s)", "", strongholds));
    if (!exactOnly)
        text += " " + tr("Counts include estimated eyes: check hits marked * in the game.");
    labelExpectation->setText(text);
    fitLabelHeight(labelExpectation);
}

void TabPortals::onExportSearch()
{
    if (!searchModel->rowCount())
        return;
    exportCsv(this, parent->prevdir, tr("Export End portal search"), "end-portals.csv",
        parent->config, [this](CsvWriter& csv) {
        csv.row({"#mc", mc2str(searchSettings.mc)});
        csv.row({"#large_biomes", searchSettings.large ? "1" : "0"});
        csv.row({"#amplified", searchSettings.amplified ? "1" : "0"});
        csv.row({"#min_eyes", QString::number(searchSettings.minEyes)});
        csv.row({"#certain_eyes_only", searchSettings.exactOnly ? "1" : "0"});
        csv.row({"#strongholds_per_seed", QString::number(searchSettings.numStrongholds)});
        searchModel->writeCsv(csv);
    });
}

void TabPortals::onExportAnalysis()
{
    if (!analysisModel->rowCount())
        return;
    exportCsv(this, parent->prevdir, tr("Export End portals"), "end-portals-seed.csv",
        parent->config, [this](CsvWriter& csv) {
        csv.row({"#seed", seedText(analysedWorld.seed)});
        csv.row({"#mc", mc2str(analysedWorld.mc)});
        csv.row({"#large_biomes", analysedWorld.large ? "1" : "0"});
        csv.row({"#amplified", analysedWorld.amplified ? "1" : "0"});
        analysisModel->writeCsv(csv);
    });
}

void TabPortals::stopWork()
{
    if (driver.isRunning())
        driver.requestStop();
    analysis.stop = true;
}
