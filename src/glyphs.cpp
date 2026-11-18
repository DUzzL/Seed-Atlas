#include "glyphs.h"

#include <QApplication>
#include <QFont>
#include <QHash>
#include <QIconEngine>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

#include <cmath>
#include <functional>
#include <initializer_list>

namespace {

// one stroke weight for the whole set, on the 20x20 unit grid
const qreal W = 1.6;

// Small drawing vocabulary shared by the glyphs.
struct G
{
    QPainter& p;
    QColor c;

    QPen pen(qreal w = W) const
    {
        return QPen(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    }
    void stroke(qreal w = W)
    {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(pen(w));
        p.setBrush(Qt::NoBrush);
    }
    void filled()
    {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
    }
    void line(qreal x1, qreal y1, qreal x2, qreal y2, qreal w = W)
    {
        stroke(w);
        p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
    }
    void poly(std::initializer_list<QPointF> pts, bool closed = false, qreal w = W)
    {
        stroke(w);
        QPolygonF pg(pts);
        if (closed)
            p.drawPolygon(pg);
        else
            p.drawPolyline(pg);
    }
    void fpoly(std::initializer_list<QPointF> pts)
    {
        filled();
        p.drawPolygon(QPolygonF(pts));
    }
    void rect(qreal x, qreal y, qreal w, qreal h, qreal r = 0, qreal pw = W)
    {
        stroke(pw);
        p.drawRoundedRect(QRectF(x, y, w, h), r, r);
    }
    void frect(qreal x, qreal y, qreal w, qreal h, qreal r = 0)
    {
        filled();
        p.drawRoundedRect(QRectF(x, y, w, h), r, r);
    }
    void circle(qreal x, qreal y, qreal r, qreal w = W)
    {
        stroke(w);
        p.drawEllipse(QPointF(x, y), r, r);
    }
    void dot(qreal x, qreal y, qreal r)
    {
        filled();
        p.drawEllipse(QPointF(x, y), r, r);
    }
    void path(const QPainterPath& path, qreal w = W)
    {
        stroke(w);
        p.drawPath(path);
    }
    void fpath(const QPainterPath& path)
    {
        filled();
        p.drawPath(path);
    }
    // cut a shape out of what is drawn so far
    void cut(const QPainterPath& path)
    {
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.drawPath(path);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    void cutStroke(const QPainterPath& path, qreal w)
    {
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(QPen(Qt::black, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
};

QPainterPath roundedRectPath(qreal x, qreal y, qreal w, qreal h, qreal r)
{
    QPainterPath path;
    path.addRoundedRect(QRectF(x, y, w, h), r, r);
    return path;
}

QPainterPath ellipsePath(qreal x, qreal y, qreal rx, qreal ry)
{
    QPainterPath path;
    path.addEllipse(QPointF(x, y), rx, ry);
    return path;
}

// ---------------------------------------------------------------- glyphs

using GlyphFn = std::function<void(G&)>;

struct GlyphDef
{
    const char *name;
    GlyphFn draw;
};

const std::vector<GlyphDef>& glyphTable()
{
    static const std::vector<GlyphDef> table = {
    {"quad", [](G& g) {
        // four huts around one spot
        for (QPointF c : {QPointF(5.2, 5.4), QPointF(14.8, 5.4), QPointF(5.2, 15.0),
                          QPointF(14.8, 15.0)})
        {
            g.fpoly({QPointF(c.x() - 2.4, c.y() + 2.2), QPointF(c.x() - 2.4, c.y() - 0.6),
                     QPointF(c.x(), c.y() - 2.8), QPointF(c.x() + 2.4, c.y() - 0.6),
                     QPointF(c.x() + 2.4, c.y() + 2.2)});
        }
        g.circle(10, 10.2, 1.7, 1.3);
    }},
    {"overworld", [](G& g) {
        // a grass block
        const QPointF L(2.8, 6.2), T(10, 2.4), R(17.2, 6.2), B(10, 10.0);
        g.fpoly({L, T, R, QPointF(17.2, 8.4), QPointF(15.4, 9.8), QPointF(13.4, 9.5),
                 QPointF(11.6, 11.4), QPointF(10, 12.2), QPointF(8.4, 11.4),
                 QPointF(6.6, 9.5), QPointF(4.6, 9.8), QPointF(2.8, 8.4)});
        g.poly({L, QPointF(2.8, 13.8), QPointF(10, 17.6), QPointF(17.2, 13.8), R});
        g.poly({L, T, R, B}, true);
        g.line(10, 10.0, 10, 17.6);
    }},
    {"nether", [](G& g) {
        // a flame
        QPainterPath f;
        f.moveTo(10, 2.2);
        f.cubicTo(11.2, 5.6, 15.8, 7.6, 15.8, 12.2);
        f.cubicTo(15.8, 15.6, 13.2, 17.8, 10, 17.8);
        f.cubicTo(6.8, 17.8, 4.2, 15.6, 4.2, 12.4);
        f.cubicTo(4.2, 9.6, 5.6, 8.0, 6.8, 6.6);
        f.cubicTo(7.0, 8.4, 7.8, 9.4, 8.8, 9.8);
        f.cubicTo(8.4, 7.0, 8.8, 4.4, 10, 2.2);
        g.path(f);
        QPainterPath i;
        i.moveTo(10, 10.8);
        i.cubicTo(11.2, 12.6, 12.7, 13.6, 12.7, 15.0);
        i.cubicTo(12.7, 16.4, 11.5, 17.2, 10, 17.2);
        i.cubicTo(8.5, 17.2, 7.3, 16.4, 7.3, 15.0);
        i.cubicTo(7.3, 13.6, 8.8, 12.6, 10, 10.8);
        g.fpath(i);
    }},
    {"the_end", [](G& g) {
        // the dragon egg
        QPainterPath e;
        e.moveTo(10, 2.2);
        e.cubicTo(14.2, 2.2, 16.4, 8.8, 16.4, 12.0);
        e.cubicTo(16.4, 15.6, 13.6, 17.8, 10, 17.8);
        e.cubicTo(6.4, 17.8, 3.6, 15.6, 3.6, 12.0);
        e.cubicTo(3.6, 8.8, 5.8, 2.2, 10, 2.2);
        g.path(e);
        g.dot(8.4, 8.4, 1.1);
        g.dot(12.0, 11.4, 1.2);
        g.dot(8.0, 14.0, 1.0);
    }},
    {"grid", [](G& g) {
        g.rect(3, 3, 14, 14, 2);
        g.line(7.67, 3, 7.67, 17);
        g.line(12.33, 3, 12.33, 17);
        g.line(3, 7.67, 17, 7.67);
        g.line(3, 12.33, 17, 12.33);
    }},
    {"terrain", [](G& g) {
        // mountains with a snow cap
        g.poly({QPointF(1.8, 16.4), QPointF(7.4, 5.4), QPointF(13.0, 16.4)});
        g.poly({QPointF(10.6, 11.6), QPointF(13.8, 7.6), QPointF(18.2, 16.4)});
        g.line(1.8, 16.4, 18.2, 16.4);
        g.poly({QPointF(5.4, 9.4), QPointF(6.6, 10.4), QPointF(7.6, 9.2), QPointF(9.2, 9.6)});
    }},
    {"height", [](G& g) {
        // a hill and a vertical measure
        g.poly({QPointF(2, 17), QPointF(6.6, 8.6), QPointF(11.2, 17)});
        g.line(1.8, 17, 18.2, 17);
        g.line(15, 3.2, 15, 14.4);
        g.poly({QPointF(12.8, 5.4), QPointF(15, 3.2), QPointF(17.2, 5.4)});
        g.poly({QPointF(12.8, 12.2), QPointF(15, 14.4), QPointF(17.2, 12.2)});
    }},
    {"map", [](G& g) {
        // a folded map
        g.poly({QPointF(2.6, 5.0), QPointF(7.4, 3.0), QPointF(12.6, 5.0), QPointF(17.4, 3.0),
                QPointF(17.4, 15.0), QPointF(12.6, 17.0), QPointF(7.4, 15.0),
                QPointF(2.6, 17.0)}, true);
        g.line(7.4, 3.0, 7.4, 15.0);
        g.line(12.6, 5.0, 12.6, 17.0);
    }},
    {"biome_center", [](G& g) {
        // a location pin over a patch of land
        QPainterPath patch;
        patch.addEllipse(QPointF(10, 15.4), 7.4, 2.6);
        g.path(patch);
        QPainterPath pin;
        pin.moveTo(10, 15.2);
        pin.cubicTo(8.2, 12.4, 5.4, 9.8, 5.4, 7.0);
        pin.cubicTo(5.4, 4.2, 7.4, 2.2, 10, 2.2);
        pin.cubicTo(12.6, 2.2, 14.6, 4.2, 14.6, 7.0);
        pin.cubicTo(14.6, 9.8, 11.8, 12.4, 10, 15.2);
        pin.closeSubpath();
        g.cutStroke(pin, 4.0);
        g.cut(pin);
        g.path(pin);
        g.circle(10, 7.0, 1.7, 1.4);
    }},
    {"climate", [](G& g) {
        // climate parameters as three sliders
        g.line(2.8, 5, 17.2, 5);
        g.line(2.8, 10, 17.2, 10);
        g.line(2.8, 15, 17.2, 15);
        for (QPointF k : {QPointF(12.6, 5), QPointF(6.6, 10), QPointF(10.6, 15)})
        {
            g.cut(ellipsePath(k.x(), k.y(), 3.0, 3.0));
            g.circle(k.x(), k.y(), 2.0);
        }
    }},
    {"tempcat", [](G& g) {
        // a thermometer
        QPainterPath t = roundedRectPath(8.2, 2.4, 3.6, 11, 1.8)
                .united(ellipsePath(10, 14.6, 3.2, 3.2));
        g.path(t);
        g.dot(10, 14.6, 1.6);
        g.frect(9.25, 7.0, 1.5, 7.0, 0.6);
        g.line(14.0, 5.0, 15.6, 5.0, 1.2);
        g.line(14.0, 7.6, 15.6, 7.6, 1.2);
        g.line(14.0, 10.2, 15.6, 10.2, 1.2);
    }},
    {"logic_or", [](G& g) {
        QPainterPath o;
        o.moveTo(4.2, 4.6);
        o.quadTo(12.6, 4.6, 16.6, 10);
        o.quadTo(12.6, 15.4, 4.2, 15.4);
        o.quadTo(7.0, 10, 4.2, 4.6);
        g.path(o);
        g.line(1.8, 7.6, 5.6, 7.6);
        g.line(1.8, 12.4, 5.6, 12.4);
    }},
    {"logic_not", [](G& g) {
        g.poly({QPointF(4.4, 4.4), QPointF(4.4, 15.6), QPointF(13.4, 10)}, true);
        g.circle(15.6, 10, 1.8);
        g.line(1.6, 10, 4.4, 10);
    }},
    {"lua", [](G& g) {
        // braces of a script
        QPainterPath b;
        b.moveTo(7.8, 3.4);
        b.quadTo(5.4, 3.4, 5.4, 5.8);
        b.lineTo(5.4, 8.0);
        b.quadTo(5.4, 10, 3.4, 10);
        b.quadTo(5.4, 10, 5.4, 12);
        b.lineTo(5.4, 14.2);
        b.quadTo(5.4, 16.6, 7.8, 16.6);
        g.path(b);
        QTransform mirror(-1, 0, 0, 1, 20, 0);
        g.path(mirror.map(b));
        g.dot(10, 10, 1.1);
    }},
    {"reference", [](G& g) {
        // a square spiral, as the spiral iterator walks
        g.poly({QPointF(10, 10), QPointF(13, 10), QPointF(13, 13), QPointF(7, 13),
                QPointF(7, 7), QPointF(16, 7), QPointF(16, 16), QPointF(4, 16),
                QPointF(4, 4), QPointF(16.6, 4)});
    }},
    };
    return table;
}

const GlyphDef *findGlyph(const QString& name)
{
    static QHash<QString, const GlyphDef*> index;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    if (index.isEmpty())
        for (const GlyphDef& def : glyphTable())
            index.insert(QString::fromLatin1(def.name), &def);
    return index.value(name, nullptr);
}

// what an unknown name draws: a dashed square
void drawMissing(G& g)
{
    QPen dash = g.pen(1.2);
    dash.setDashPattern({2.0, 2.0});
    g.p.setPen(dash);
    g.p.setBrush(Qt::NoBrush);
    g.p.drawRoundedRect(QRectF(3, 3, 14, 14), 2.5, 2.5);
}

QString colorKey(const QColor& c)
{
    return QString::number(c.rgba(), 16);
}

// ---------------------------------------------------------------- icon engine

class GlyphIconEngine : public QIconEngine
{
public:
    GlyphIconEngine(const QString& name, bool toggle) : name(name), toggle(toggle) {}

    QColor colorFor(QIcon::Mode mode, QIcon::State state) const
    {
        const QPalette pal = QApplication::palette();
        QColor muted = pal.color(QPalette::PlaceholderText);
        if (mode == QIcon::Disabled)
        {
            muted.setAlphaF(0.55);
            return muted;
        }
        // Item views report expanded rows as "On", so only a toggle button
        // shows the state; lists and menus show the selection.
        if (mode == QIcon::Selected || (toggle && state == QIcon::On))
            return pal.color(QPalette::Text);
        return muted;
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        const int side = qMin(size.width(), size.height());
        if (side <= 0)
            return QPixmap();
        // Qt passes the logical size and the scale; render at device pixels
        const QPixmap glyph = glyphPixmap(name, colorFor(mode, state), side,
                                          qMax<qreal>(1.0, scale));
        if (size.width() == size.height())
            return glyph;
        QPixmap canvas(QSize(size.width(), size.height()) * glyph.devicePixelRatio());
        canvas.setDevicePixelRatio(glyph.devicePixelRatio());
        canvas.fill(Qt::transparent);
        QPainter p(&canvas);
        p.drawPixmap(QPointF((size.width() - side) / 2.0, (size.height() - side) / 2.0), glyph);
        return canvas;
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    void paint(QPainter *painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const int side = qMin(rect.width(), rect.height());
        const QPixmap pm = glyphPixmap(name, colorFor(mode, state), side, dpr);
        painter->drawPixmap(QPointF(rect.x() + (rect.width() - side) / 2.0,
                                    rect.y() + (rect.height() - side) / 2.0), pm);
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override
    {
        return size;
    }

    QIconEngine *clone() const override
    {
        return new GlyphIconEngine(name, toggle);
    }

    QString key() const override
    {
        return QStringLiteral("SeedAtlasGlyph");
    }

    bool isNull() override
    {
        return false;
    }

private:
    QString name;
    bool toggle;
};

} // namespace

bool hasGlyph(const QString& name)
{
    return findGlyph(name) != nullptr;
}

QStringList glyphNames()
{
    QStringList names;
    for (const GlyphDef& def : glyphTable())
        names.append(QString::fromLatin1(def.name));
    return names;
}

void drawGlyph(QPainter& p, const QString& name, const QColor& color)
{
    const GlyphDef *def = findGlyph(name);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    G g{p, color};
    if (def)
        def->draw(g);
    else
        drawMissing(g);
    p.restore();
}

QPixmap glyphPixmap(const QString& name, const QColor& color, int side, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    static QMutex mutex;
    if (side <= 0)
        return QPixmap();
    const int px = qMax(1, int(std::lround(side * dpr)));
    const QString key = name + '|' + colorKey(color) + '|' + QString::number(side) + '|'
            + QString::number(px);
    {
        QMutexLocker lock(&mutex);
        auto it = cache.constFind(key);
        if (it != cache.constEnd())
            return *it;
    }
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.scale(px / 20.0, px / 20.0);
        drawGlyph(p, name, color);
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(qreal(px) / side);
    QMutexLocker lock(&mutex);
    cache.insert(key, pixmap);
    return pixmap;
}

QIcon glyphIcon(const QString& name, bool toggle)
{
    return QIcon(new GlyphIconEngine(name, toggle));
}

bool renderGlyphSheet(const QString& path, qreal dpr)
{
    struct Theme { const char *name; QColor bg, text, muted; };
    const Theme themes[2] = {
        { "dark",  QColor("#161a17"), QColor("#e9eee9"), QColor("#8f9b92") },
        { "light", QColor("#ffffff"), QColor("#1b221d"), QColor("#5f6b62") },
    };
    const QStringList names = glyphNames();
    const int rowH = 46, nameW = 110, headerH = 30;
    const int cellW[4] = {28, 28, 46, 46};
    const int themeW = 8 + cellW[0] + cellW[1] + cellW[2] + cellW[3];
    const int width = nameW + 2 * themeW;
    const int height = headerH + int(names.size()) * rowH;
    QImage image(QSize(width, height) * dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(QColor("#d8d8d8"));
    QPainter p(&image);
    QFont font = QApplication::font();
    font.setPixelSize(12);
    p.setFont(font);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::black);
    for (int t = 0; t < 2; t++)
        p.drawText(QRect(nameW + t * themeW, 0, themeW, headerH), Qt::AlignCenter,
                   QString("%1: 18 off/on, 36 off/on").arg(themes[t].name));
    for (int i = 0; i < names.size(); i++)
    {
        const int y = headerH + i * rowH;
        p.fillRect(QRect(0, y, nameW, rowH), QColor("#eeeeee"));
        p.setPen(Qt::black);
        p.drawText(QRect(6, y, nameW - 6, rowH), Qt::AlignVCenter, names[i]);
        for (int t = 0; t < 2; t++)
        {
            int x = nameW + t * themeW;
            p.fillRect(QRect(x, y, themeW, rowH), themes[t].bg);
            x += 4;
            for (int k = 0; k < 4; k++)
            {
                const int side = k < 2 ? 18 : 36;
                const QColor c = k % 2 ? themes[t].text : themes[t].muted;
                p.drawPixmap(QPointF(x + (cellW[k] - side) / 2, y + (rowH - side) / 2),
                             glyphPixmap(names[i], c, side, dpr));
                x += cellW[k];
            }
        }
    }
    p.end();
    return image.save(path);
}
