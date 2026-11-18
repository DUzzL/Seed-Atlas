#include "widgets.h"

#include "util.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidgetItem>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QEasingCurve>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStylePainter>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <QSet>

#include <cmath>

namespace {

QString compactToolbarLabel(const QAction *action)
{
    const QVariant label = action->property("toolbarLabel");
    if (label.isValid())
        return label.toString();
    QString text = action->text();
    if (text.startsWith("Show "))
        text.remove(0, 5);
    return text;
}

// Draws one chrome glyph on a 20x20 unit canvas.
void drawChromeGlyph(QPainter& p, const QString& kind, const QColor& color)
{
    QPen pen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (kind == "search")
    {
        p.drawEllipse(QPointF(8.6, 8.6), 5.1, 5.1);
        p.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12.5, 12.5), QPointF(16.6, 16.6));
    }
    else if (kind == "biomes")
    {   // four patches of land, like a small map
        for (const QRectF& r : {QRectF(3, 3, 6, 6), QRectF(11, 3, 6, 6),
                                QRectF(3, 11, 6, 6), QRectF(11, 11, 6, 6)})
            p.drawRoundedRect(r, 1.6, 1.6);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(11, 3, 6, 6), 1.6, 1.6);
        p.drawRoundedRect(QRectF(3, 11, 6, 6), 1.6, 1.6);
    }
    else if (kind == "structures")
    {   // a house
        p.drawPolyline(QPolygonF({QPointF(2.8, 9.6), QPointF(10, 3.4), QPointF(17.2, 9.6)}));
        p.drawPolyline(QPolygonF({QPointF(4.8, 8.4), QPointF(4.8, 16.6),
                                  QPointF(15.2, 16.6), QPointF(15.2, 8.4)}));
        p.drawPolyline(QPolygonF({QPointF(8.4, 16.6), QPointF(8.4, 12.2),
                                  QPointF(11.6, 12.2), QPointF(11.6, 16.6)}));
    }
    else if (kind == "slime")
    {   // a slime cube with a face
        p.drawRoundedRect(QRectF(3.2, 4.2, 13.6, 12.6), 3.2, 3.2);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRect(QRectF(6.6, 8.2, 2.4, 2.4));
        p.drawRect(QRectF(11.0, 8.2, 2.4, 2.4));
        p.drawRect(QRectF(9.0, 12.6, 2.0, 1.6));
    }
    else if (kind == "eye")
    {   // The End Portals tool: an eye of ender as a small pixel-art orb. A
        // stepped round rim, a dim body, a bright iris and a tall slit pupil
        // cut out of it; three tones of the one rail color. 9x9 cells of 2
        // units on the 20-unit canvas, whole pixels at 1x, 2x and 3x.
        static const char *const rows[9] = {
            "  #####  ",
            " #.ooo.# ",
            "#.oo oo.#",
            "#.oo oo.#",
            "#.oo oo.#",
            "#.oo oo.#",
            "#.oo oo.#",
            " #.ooo.# ",
            "  #####  ",
        };
        QColor rim = color, body = color;
        rim.setAlphaF(color.alphaF() * 0.78);
        body.setAlphaF(color.alphaF() * 0.36);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        for (int y = 0; y < 9; y++)
        {
            for (int x = 0; x < 9; x++)
            {
                const char cell = rows[y][x];
                if (cell == ' ')
                    continue;
                p.fillRect(QRectF(1 + 2*x, 1 + 2*y, 2, 2),
                    cell == 'o' ? color : cell == '#' ? rim : body);
            }
        }
    }
    else if (kind == "menu")
    {
        for (qreal y : {5.0, 10.0, 15.0})
            p.drawLine(QPointF(3.5, y), QPointF(16.5, y));
    }
    else if (kind == "settings")
    {   // a gear: a ring with eight teeth and a hole
        p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::FlatCap));
        for (int i = 0; i < 8; i++)
        {
            const qreal a = i * M_PI / 4;
            p.drawLine(QPointF(10 + 5.2*cos(a), 10 + 5.2*sin(a)),
                       QPointF(10 + 8.0*cos(a), 10 + 8.0*sin(a)));
        }
        p.setPen(pen);
        p.drawEllipse(QPointF(10, 10), 5.2, 5.2);
        p.drawEllipse(QPointF(10, 10), 2.0, 2.0);
    }
    else if (kind == "origin")
    {   // crosshair on the world origin
        p.drawEllipse(QPointF(10, 10), 4.6, 4.6);
        p.drawLine(QPointF(10, 1.8), QPointF(10, 5.2));
        p.drawLine(QPointF(10, 14.8), QPointF(10, 18.2));
        p.drawLine(QPointF(1.8, 10), QPointF(5.2, 10));
        p.drawLine(QPointF(14.8, 10), QPointF(18.2, 10));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(10, 10), 1.5, 1.5);
    }
    else if (kind == "plus")
    {
        p.drawLine(QPointF(10, 4.5), QPointF(10, 15.5));
        p.drawLine(QPointF(4.5, 10), QPointF(15.5, 10));
    }
    else if (kind == "minus")
    {
        p.drawLine(QPointF(4.5, 10), QPointF(15.5, 10));
    }
    else if (kind == "go")
    {
        p.drawLine(QPointF(4, 10), QPointF(15.5, 10));
        p.drawPolyline(QPolygonF({QPointF(10.5, 5), QPointF(15.5, 10), QPointF(10.5, 15)}));
    }
    else if (kind == "chevron-left" || kind == "chevron-right")
    {
        const qreal d = kind == "chevron-left" ? 1 : -1;
        p.drawPolyline(QPolygonF({QPointF(10 + 2.5*d, 5), QPointF(10 - 2.5*d, 10),
                                  QPointF(10 + 2.5*d, 15)}));
    }
    else if (kind == "datapacks")
    {   // the Datapacks tool: a pack, i.e. a box with a lid seen from the
        // front (the outline of an open-topped cube with its centre edges)
        p.drawPolygon(QPolygonF({QPointF(10, 2.6), QPointF(16.8, 6.2), QPointF(16.8, 13.8),
                                 QPointF(10, 17.4), QPointF(3.2, 13.8), QPointF(3.2, 6.2)}));
        p.drawPolyline(QPolygonF({QPointF(3.2, 6.2), QPointF(10, 9.8), QPointF(16.8, 6.2)}));
        p.drawLine(QPointF(10, 9.8), QPointF(10, 17.4));
    }
    else if (kind == "arrow-up" || kind == "arrow-down")
    {
        const qreal d = kind == "arrow-up" ? 1 : -1;
        p.drawLine(QPointF(10, 10 - 5.5*d), QPointF(10, 10 + 5.5*d));
        p.drawPolyline(QPolygonF({QPointF(5, 10 - 0.5*d), QPointF(10, 10 - 5.5*d),
                                  QPointF(15, 10 - 0.5*d)}));
    }
    else if (kind == "close")
    {
        p.drawLine(QPointF(5, 5), QPointF(15, 15));
        p.drawLine(QPointF(15, 5), QPointF(5, 15));
    }
    else if (kind == "layers")
    {
        QPolygonF top({QPointF(10, 3), QPointF(17, 7), QPointF(10, 11), QPointF(3, 7)});
        p.drawPolygon(top);
        p.drawPolyline(QPolygonF({QPointF(3, 10.5), QPointF(10, 14.5), QPointF(17, 10.5)}));
        p.drawPolyline(QPolygonF({QPointF(3, 14), QPointF(10, 18), QPointF(17, 14)}));
    }
}

QPixmap chromePixmap(const QString& kind, const QColor& color, int side)
{
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(side / 20.0, side / 20.0);
    drawChromeGlyph(p, kind, color);
    return pixmap;
}

// The pixel-art source at an integer nearest-neighbour scale that fits side,
// centered on a square canvas. Larger sources are scaled down smoothly.
QPixmap pixelArtPixmap(const QImage& source, int side)
{
    QImage canvas(side, side, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    if (source.isNull())
        return QPixmap::fromImage(canvas);
    const int extent = qMax(source.width(), source.height());
    QImage scaled;
    const int factor = side / extent;
    if (factor >= 1)
        scaled = source.scaled(source.size() * factor, Qt::KeepAspectRatio, Qt::FastTransformation);
    else
        scaled = source.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&canvas);
    p.drawImage((side - scaled.width()) / 2, (side - scaled.height()) / 2, scaled);
    p.end();
    return QPixmap::fromImage(canvas);
}

// A grey version of a layer icon whose tones stay within a range that is
// legible on the sidebar background: the black outlines of the pixel art
// would vanish on the dark theme, the white highlights on the light one.
QImage inactiveLayerImage(const QImage& source, bool darkTheme)
{
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const int low = darkTheme ? 96 : 64;
    const int high = darkTheme ? 198 : 168;
    for (int y = 0; y < image.height(); y++)
    {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++)
        {
            const int gray = low + qGray(line[x]) * (high - low) / 255;
            line[x] = qRgba(gray, gray, gray, qAlpha(line[x]));
        }
    }
    return image;
}

class EmptyHint : public QLabel
{
public:
    EmptyHint(QAbstractItemView *view, const QString& text)
        : QLabel(text, view->viewport())
        , view(view)
    {
        setObjectName("emptyHint");
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        view->viewport()->installEventFilter(this);
        view->installEventFilter(this);
        watchModel();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == view->viewport() && event->type() == QEvent::Resize)
            place();
        else if (watched == view && (event->type() == QEvent::Show ||
                 event->type() == QEvent::Polish))
            watchModel();
        return false;
    }

private:
    void place()
    {
        setGeometry(view->viewport()->rect().adjusted(12, 8, -12, -8));
    }

    void watchModel()
    {
        QAbstractItemModel *current = view->model();
        if (current != model)
        {
            if (model)
                disconnect(model, nullptr, this, nullptr);
            model = current;
            if (model)
            {
                const auto update = [this]() { refresh(); };
                connect(model, &QAbstractItemModel::rowsInserted, this, update);
                connect(model, &QAbstractItemModel::rowsRemoved, this, update);
                connect(model, &QAbstractItemModel::modelReset, this, update);
                connect(model, &QAbstractItemModel::layoutChanged, this, update);
            }
        }
        refresh();
    }

    void refresh()
    {
        place();
        setVisible(!model || model->rowCount() == 0);
    }

    QAbstractItemView *view;
    QPointer<QAbstractItemModel> model;
};

} // namespace

QIcon chromeIcon(const QString& kind, const QColor& color, int size, const QColor& checkedColor)
{
    QIcon icon;
    for (int scale : {1, 2, 3})
    {
        icon.addPixmap(chromePixmap(kind, color, size * scale), QIcon::Normal, QIcon::Off);
        if (checkedColor.isValid())
            icon.addPixmap(chromePixmap(kind, checkedColor, size * scale), QIcon::Normal, QIcon::On);
    }
    return icon;
}

QIcon layerIcon(const QString& rcbase, int size, bool darkTheme)
{
    const QImage active(":/icons/" + rcbase + ".png");
    const QImage inactive = inactiveLayerImage(active, darkTheme);
    QIcon icon;
    for (int scale : {1, 2, 3})
    {
        const int side = size * scale;
        const QPixmap on = pixelArtPixmap(active, side);
        const QPixmap off = pixelArtPixmap(inactive, side);
        icon.addPixmap(on, QIcon::Normal, QIcon::On);
        icon.addPixmap(off, QIcon::Normal, QIcon::Off);
        // disabled layers keep the calm grey look instead of Qt's washed-out one
        icon.addPixmap(off, QIcon::Disabled, QIcon::Off);
        icon.addPixmap(off, QIcon::Disabled, QIcon::On);
    }
    return icon;
}

void setEmptyHint(QAbstractItemView *view, const QString& text)
{
    if (!view)
        return;
    if (QLabel *existing = view->viewport()->findChild<QLabel*>("emptyHint",
            Qt::FindDirectChildrenOnly))
    {
        existing->setText(text);
        return;
    }
    new EmptyHint(view, text);
}


// width of the scroll bar column at the right edge of the layer sidebar
static const int GUTTER_WIDTH = 10;

ScrollableToolBar::ScrollableToolBar(QWidget *parent)
    : QToolBar(parent)
    , panel(new QWidget(this))
    , title(new QLabel(tr("LAYERS"), panel))
    , headerLayout(new QHBoxLayout)
    , scroll(new QScrollArea(panel))
    , gutter(new QWidget(panel))
    , scrollBar(new QScrollBar(Qt::Vertical, gutter))
    , strip(new QWidget)
    , stripLayout(new QBoxLayout(QBoxLayout::TopToBottom, strip))
    , buttons()
    , sectionLabels()
    , separators()
    , toggleButton(new QToolButton(panel))
    , widthAnimation(new QPropertyAnimation(this, "sidebarWidth", this))
    , chromeColor(Qt::gray)
    , currentWidth(200)
    , collapsed(false)
{
    panel->setObjectName("layerSidebar");
    scroll->setObjectName("layerScroll");
    strip->setObjectName("layerStrip");
    title->setObjectName("layerSidebarTitle");

    QVBoxLayout *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 6, 0, 0);
    panelLayout->setSpacing(4);
    headerLayout->setContentsMargins(8, 0, 6, 0);
    headerLayout->setSpacing(4);
    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(toggleButton);
    panelLayout->addLayout(headerLayout);
    QHBoxLayout *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(scroll, 1);
    body->addWidget(gutter);
    panelLayout->addLayout(body, 1);

    // the gutter keeps its width whether or not the list scrolls
    gutter->setObjectName("layerScrollGutter");
    gutter->setFixedWidth(GUTTER_WIDTH);
    QVBoxLayout *gutterLayout = new QVBoxLayout(gutter);
    gutterLayout->setContentsMargins(1, 2, 3, 6);
    gutterLayout->addWidget(scrollBar);
    scrollBar->setObjectName("layerScrollBar");
    scrollBar->setRange(0, 0);
    scrollBar->hide();
    QScrollBar *inner = scroll->verticalScrollBar();
    connect(inner, &QScrollBar::rangeChanged, this, [this, inner](int minimum, int maximum) {
        scrollBar->setRange(minimum, maximum);
        scrollBar->setPageStep(inner->pageStep());
        scrollBar->setSingleStep(inner->singleStep());
        scrollBar->setVisible(maximum > minimum);
    });
    connect(inner, &QScrollBar::valueChanged, scrollBar, &QScrollBar::setValue);
    connect(scrollBar, &QScrollBar::valueChanged, inner, &QScrollBar::setValue);

    stripLayout->setContentsMargins(6, 0, 6, 8);
    stripLayout->setSpacing(3);
    stripLayout->addStretch(1);
    scroll->setWidget(strip);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // still scrolls (wheel, keyboard, ensureVisible), drawn in the gutter
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QToolBar::addWidget(panel);

    toggleButton->setObjectName("rightSidebarToggle");
    toggleButton->setProperty("keepIconSize", true);
    toggleButton->setAutoRaise(true);
    toggleButton->setCursor(Qt::PointingHandCursor);
    toggleButton->setIconSize(QSize(16, 16));
    connect(toggleButton, &QToolButton::clicked, this, [this]() {
        setCollapsed(!collapsed);
    });
    widthAnimation->setDuration(200);
    widthAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(this, &QToolBar::iconSizeChanged, this, &ScrollableToolBar::updateButtons);
    setMinimumWidth(currentWidth);
    setMaximumWidth(currentWidth);
    updateToggleButton();
}

QAction *ScrollableToolBar::addAction(QAction *action)
{
    QToolButton *button = new QToolButton(strip);
    button->setObjectName("layerButton");
    button->setProperty("keepIconSize", true);
    button->setIconSize(iconSize());
    button->setCursor(Qt::PointingHandCursor);
    const auto update = [button, action]() {
        button->setIcon(action->icon());
        button->setText(compactToolbarLabel(action));
        // the tooltip names the layer, which matters most while collapsed
        button->setToolTip(action->toolTip().isEmpty() ? action->text() : action->toolTip());
        button->setEnabled(action->isEnabled());
        button->setVisible(action->isVisible());
        button->setCheckable(action->isCheckable());
        button->setChecked(action->isChecked());
    };
    update();
    connect(action, &QAction::changed, button, update);
    connect(button, &QToolButton::clicked, action, [action]() { action->trigger(); });
    stripLayout->insertWidget(stripLayout->count() - 1, button);
    buttons.append(button);
    applyCollapsedLayout();
    return action;
}

QAction *ScrollableToolBar::addSeparator()
{
    addSection(QString());
    return nullptr;
}

void ScrollableToolBar::addSection(const QString& text)
{
    if (!buttons.isEmpty())
    {
        QFrame *separator = new QFrame(strip);
        separator->setObjectName("layerSeparator");
        separator->setFrameShape(QFrame::NoFrame);
        separator->setFixedHeight(1);
        stripLayout->insertSpacing(stripLayout->count() - 1, 4);
        stripLayout->insertWidget(stripLayout->count() - 1, separator);
        stripLayout->insertSpacing(stripLayout->count() - 1, 2);
        separators.append(separator);
    }
    if (!text.isEmpty())
    {
        QLabel *label = new QLabel(text.toUpper(), strip);
        label->setObjectName("layerSectionLabel");
        label->setContentsMargins(4, 4, 0, 1);
        stripLayout->insertWidget(stripLayout->count() - 1, label);
        sectionLabels.append(label);
    }
    applyCollapsedLayout();
}

void ScrollableToolBar::resizeEvent(QResizeEvent *event)
{
    QToolBar::resizeEvent(event);
    if (QToolButton *extension = findChild<QToolButton *>("qt_toolbar_ext_button"))
        extension->hide();
    // the frame of the tool bar is only known once it is laid out
    if (collapsed && widthAnimation->state() == QAbstractAnimation::Stopped)
    {
        const int target = collapsedWidth();
        if (target != currentWidth)
            QTimer::singleShot(0, this, [this]() {
                if (collapsed && widthAnimation->state() == QAbstractAnimation::Stopped)
                    setSidebarWidth(collapsedWidth());
            });
    }
}

void ScrollableToolBar::applyCollapsedLayout()
{
    const int side = iconSize().height() + 10;
    for (QToolButton *button : buttons)
    {
        button->setToolButtonStyle(collapsed ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
        if (collapsed)
        {
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            button->setFixedSize(side, side);
            stripLayout->setAlignment(button, Qt::AlignHCenter);
        }
        else
        {
            button->setMinimumSize(0, side);
            button->setMaximumSize(QWIDGETSIZE_MAX, side);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            stripLayout->setAlignment(button, Qt::Alignment());
        }
    }
    for (QLabel *label : sectionLabels)
        label->setVisible(!collapsed);
    title->setVisible(!collapsed);
    headerLayout->setContentsMargins(collapsed ? 0 : 10, 0, collapsed ? 0 : 6, 0);
    headerLayout->setAlignment(toggleButton, collapsed ? Qt::AlignHCenter : Qt::AlignRight);
}

void ScrollableToolBar::setCollapsed(bool value, bool animated)
{
    if (collapsed == value &&
        (widthAnimation->state() == QAbstractAnimation::Stopped || !animated))
    {
        setSidebarWidth(collapsed ? collapsedWidth() : expandedWidth());
        return;
    }

    const int startWidth = width();
    widthAnimation->stop();
    collapsed = value;
    applyCollapsedLayout();
    const int targetWidth = collapsed ? collapsedWidth() : expandedWidth();
    updateToggleButton();
    emit collapsedChanged(collapsed);

    if (!animated || startWidth == targetWidth || !isVisible())
    {
        setSidebarWidth(targetWidth);
        return;
    }

    setSidebarWidth(startWidth);
    widthAnimation->setStartValue(startWidth);
    widthAnimation->setEndValue(targetWidth);
    widthAnimation->start();
}

void ScrollableToolBar::setSidebarWidth(int width)
{
    currentWidth = qMax(1, width);
    setMinimumWidth(currentWidth);
    setMaximumWidth(currentWidth);
    updateGeometry();
}

void ScrollableToolBar::setChromeColor(const QColor& color)
{
    chromeColor = color;
    updateToggleButton();
}

int ScrollableToolBar::expandedWidth() const
{
    return 200;
}

int ScrollableToolBar::collapsedWidth() const
{
    // the icon buttons with even strip margins, the scroll bar gutter and the
    // tool bar's own frame (its left border line)
    const int side = iconSize().height() + 10;
    const QMargins margins = stripLayout->contentsMargins();
    int frame = 1;
    if (panel->width() > 0 && width() > panel->width())
        frame = qMin(width() - panel->width(), 8);
    return side + margins.left() + margins.right() + GUTTER_WIDTH + frame;
}

void ScrollableToolBar::updateToggleButton()
{
    toggleButton->setIcon(chromeIcon(collapsed ? "chevron-left" : "chevron-right", chromeColor, 16));
    toggleButton->setToolTip(collapsed ? tr("Show layer names") : tr("Hide layer names"));
    toggleButton->setAccessibleName(toggleButton->toolTip());
}

void ScrollableToolBar::updateButtons(const QSize& size)
{
    for (QToolButton *button : buttons)
        button->setIconSize(size);
    applyCollapsedLayout();
    if (widthAnimation->state() == QAbstractAnimation::Stopped)
        setSidebarWidth(collapsed ? collapsedWidth() : expandedWidth());
}

Collapsible::Collapsible(QWidget *parent)
    : QWidget(parent)
    , animgroup(new QParallelAnimationGroup(this))
    , toggleButton(new QToolButton(this))
    , frameBar(new QFrame(this))
    , content()
    , contentHeight()
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);

    // a quiet uppercase heading; font size and weight come from the style sheet
    toggleButton->setObjectName("sectionToggle");
    toggleButton->setProperty("keepIconSize", true);
    toggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggleButton->setArrowType(Qt::ArrowType::RightArrow);
    toggleButton->setIconSize(QSize(9, 9));
    toggleButton->setCheckable(true);
    toggleButton->setCursor(Qt::PointingHandCursor);

    // only a spacer now that pushes the help button to the right edge
    frameBar->setObjectName("sectionLine");
    frameBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    frameBar->setFrameShape(QFrame::NoFrame);

    QVBoxLayout *vbox = new QVBoxLayout();
    layoutBar = new QHBoxLayout();
    layoutBar->addWidget(toggleButton, Qt::AlignBottom);
    layoutBar->addWidget(frameBar, Qt::AlignCenter);
    layoutBar->setSpacing(5);
    vbox->addLayout(layoutBar);

    layoutContent = new QHBoxLayout();
    layoutContent->setContentsMargins(0, 0, 0, 0);
    vbox->addLayout(layoutContent);

    vbox->setSizeConstraint(QLayout::SetMaximumSize);
    vbox->setContentsMargins(0, 0, 0, 6);
    vbox->setSpacing(4);
    setLayout(vbox);

    connect(toggleButton, &QToolButton::toggled, this, &Collapsible::toggle);
    connect(animgroup, &QAbstractAnimation::finished, this, &Collapsible::finishAnimation);
}

void Collapsible::toggle(bool collapsed)
{
    if (!content)
        return;

    if (collapsed)
    {   // expanding: the content may have changed size while it was collapsed
        contentHeight = qMax(contentHeight, content->sizeHint().height());
    }
    else
    {
        int height = content->size().height();
        if (height)
            contentHeight = height;
    }

    if (collapsed)
    {
        content->setMinimumHeight(0);
        content->setMaximumHeight(0);
        toggleButton->setArrowType(Qt::ArrowType::DownArrow);
        animgroup->setDirection(QAbstractAnimation::Forward);
    }
    else
    {
        toggleButton->setArrowType(Qt::ArrowType::RightArrow);
        animgroup->setDirection(QAbstractAnimation::Backward);
    }

    for (int i = 0, n = animgroup->animationCount(); i < n; i++)
    {
        QPropertyAnimation *anim;
        anim = (QPropertyAnimation*) animgroup->animationAt(i);
        anim->setStartValue(0);
        anim->setEndValue(contentHeight);
        anim->setDuration(190);
        anim->setEasingCurve(QEasingCurve::OutCubic);
    }

    animgroup->start();
}

void Collapsible::finishAnimation()
{
    if (toggleButton->isChecked())
    {
        content->setMinimumHeight(0);
        content->setMaximumHeight(16777215);
    }
}

void Collapsible::init(const QString& title, QWidget *widget, bool collapsed)
{
    this->title = title;
    toggleButton->setText(title.toUpper());
    layoutContent->addWidget(widget);
    contentHeight = widget->sizeHint().height();
    content = widget;
    animgroup->addAnimation(new QPropertyAnimation(content, "minimumHeight"));
    animgroup->addAnimation(new QPropertyAnimation(content, "maximumHeight"));
    setCollapsed(collapsed);
}

void Collapsible::setCount(int count)
{
    QString text = title.toUpper();
    if (count >= 0)
        text += QString::fromUtf8(" \u00b7 ") + QLocale().toString(count);
    toggleButton->setText(text);
}

void Collapsible::setCollapsed(bool collapsed)
{
    if (collapsed)
    {
        toggleButton->setChecked(false);
        content->setMinimumHeight(0);
        content->setMaximumHeight(0);
        toggleButton->setArrowType(Qt::ArrowType::RightArrow);
        animgroup->setDirection(QAbstractAnimation::Backward);
    }
    else
    {
        toggleButton->setChecked(true);
        content->setMinimumHeight(0);
        content->setMaximumHeight(16777215);
        toggleButton->setArrowType(Qt::ArrowType::DownArrow);
        animgroup->setDirection(QAbstractAnimation::Forward);
    }
    animgroup->stop();
}

// The help icon, painted from the theme colors so it suits light and dark
// mode: an "i" in an accent ring, filled while hovered or open.
static QIcon infoIcon(const QAbstractButton *button)
{
    const QPalette pal = QApplication::palette();
    const QColor accent = pal.color(QPalette::Highlight);
    const bool filled = button->underMouse() || button->property("infoOpen").toBool();
    const int size = button->iconSize().width();
    const qreal dpr = button->devicePixelRatioF();
    QPixmap pix(QSize(size, size) * dpr);
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal ring = qMax<qreal>(1.25, size / 11.0);
    const QRectF circle(ring / 2, ring / 2, size - ring, size - ring);
    QColor glyph = accent;
    if (filled)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(circle.adjusted(-ring / 2, -ring / 2, ring / 2, ring / 2));
        glyph = pal.color(QPalette::HighlightedText);
    }
    else
    {
        p.setPen(QPen(accent, ring));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(circle);
    }
    // dot and stem as shapes, so the "i" sits centered with any font
    const qreal cx = size / 2.0, stem = size * 0.13;
    p.setPen(Qt::NoPen);
    p.setBrush(glyph);
    p.drawEllipse(QPointF(cx, size * 0.30), stem * 0.62, stem * 0.62);
    p.drawRoundedRect(QRectF(cx - stem / 2, size * 0.43, stem, size * 0.32), stem / 2, stem / 2);
    return QIcon(pix);
}

namespace {
// Keeps the icon of every help button in step with hover, open state and theme.
class InfoButtonFilter : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type())
        {
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::DynamicPropertyChange:
        case QEvent::Show:
            if (QAbstractButton *button = qobject_cast<QAbstractButton*>(watched))
                button->setIcon(infoIcon(button));
            break;
        default:
            break;
        }
        return false;
    }
};
}

void styleInfoButton(QAbstractButton *button)
{
    static InfoButtonFilter *filter = new InfoButtonFilter(qApp);
    const int size = button->fontMetrics().height() + 2;
    button->setText("");
    button->setStyleSheet("QAbstractButton { background: transparent; border: none; padding: 0; }");
    button->setIconSize(QSize(size, size));
    button->setFixedSize(size + 4, size + 4);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(QApplication::translate("Info", "Show help"));
    button->setIcon(infoIcon(button));
    button->installEventFilter(filter);
}

QPushButton *createInfoButton(QWidget *parent, const QString& title, const QString& text)
{
    QPushButton *button = new QPushButton(parent);
    styleInfoButton(button);
    QObject::connect(button, &QPushButton::clicked, button,
        [button, title, text]() { InfoPopup::toggle(button, title, text); });
    return button;
}

QPointer<InfoPopup> InfoPopup::current;

void InfoPopup::toggle(QWidget *anchor, const QString& title, const QString& text, const QRect& area)
{
    // a closed card lingers until its deferred deletion; it no longer counts
    if (current && current->isVisible())
    {
        const bool same = current->anchor == anchor && current->area == area;
        current->close();
        if (same)
            return;
    }
    current = new InfoPopup(anchor, area, title, text);
    current->place();
    current->show();
    current->raise();
}

// A card inside the anchor's window: no native window to create, so it shows
// at once, and it never outlives or leaves the window it explains.
InfoPopup::InfoPopup(QWidget *anchor, const QRect& area, const QString& title, const QString& text)
    : QFrame(anchor->window())
    , anchor(anchor)
    , area(area)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground);
    setObjectName("infoPopup");
    // explicit colors: palette() in a child's style sheet resolves to the
    // background of the widgets below it, which left the card see-through
    const QPalette pal = QApplication::palette();
    QColor border = pal.color(QPalette::ToolTipText);
    border.setAlpha(55);
    setStyleSheet(QString(
        "#infoPopup { background-color: %1; border: 1px solid %2; border-radius: 10px; }"
        "#infoPopup QLabel { color: %3; background: transparent; border: 0; }"
        "#infoPopup QLabel#infoTitle { color: %4; font-weight: 700; }"
        "#infoPopup QScrollArea, #infoPopup QScrollArea > QWidget > QWidget {"
        " background: transparent; border: 0; }")
        .arg(pal.color(QPalette::ToolTipBase).name(), border.name(QColor::HexArgb),
             pal.color(QPalette::ToolTipText).name(), pal.color(QPalette::Highlight).name()));
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 70));
    setGraphicsEffect(shadow);

    const QWidget *window = anchor->window();
    const int width = qBound(240, fontMetrics().averageCharWidth() * 52,
        qMax(240, window->width() - 24));
    const int inner = width - 28;
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 11, 14, 13);
    layout->setSpacing(6);

    // the card belongs to its i button, so a "Help: " prefix says nothing
    QString caption = title;
    if (caption.startsWith(QStringLiteral("Help: ")))
        caption = caption.mid(6);
    if (!caption.isEmpty())
        caption[0] = caption[0].toUpper();
    QLabel *heading = new QLabel(caption, this);
    heading->setObjectName("infoTitle");
    heading->setWordWrap(true);
    heading->setFixedWidth(inner);
    layout->addWidget(heading);

    QLabel *body = new QLabel(text);
    body->setWordWrap(true);
    body->setOpenExternalLinks(true);
    body->setTextInteractionFlags(Qt::TextBrowserInteraction);
    body->setFixedWidth(inner);
    const int needed = body->heightForWidth(inner);

    // long help texts scroll instead of growing past the window
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const int maxHeight = qMax(120, window->height() * 6 / 10);
    if (needed > maxHeight)
    {
        const int bar = scroll->verticalScrollBar()->sizeHint().width();
        body->setFixedWidth(inner - bar);
        body->setFixedHeight(body->heightForWidth(inner - bar));
        scroll->setFixedSize(inner, maxHeight);
    }
    else
    {
        body->setFixedHeight(needed);
        scroll->setFixedSize(inner, needed);
    }
    scroll->setWidget(body);
    layout->addWidget(scroll);
    setFixedWidth(width);
    adjustSize();

    // a click anywhere else, Esc, hiding the button or leaving the window closes it
    qApp->installEventFilter(this);
    anchor->window()->installEventFilter(this);
    anchor->setProperty("infoOpen", true);
}

InfoPopup::~InfoPopup()
{
    if (anchor)
        anchor->setProperty("infoOpen", false);
}

void InfoPopup::place()
{
    if (!anchor)
        return;
    QWidget *window = parentWidget();
    const QRect target = area.isNull() ? anchor->rect() : area;
    const QPoint topLeft = anchor->mapTo(window, target.topLeft());
    const QPoint bottomRight = anchor->mapTo(window, target.bottomRight());
    // below the button and right-aligned to it, or above if there is no room
    QPoint pos(bottomRight.x() - width() + 1, bottomRight.y() + 6);
    if (pos.y() + height() > window->height() - 8)
        pos.setY(topLeft.y() - height() - 6);
    pos.setX(qBound(8, pos.x(), qMax(8, window->width() - width() - 8)));
    pos.setY(qBound(8, pos.y(), qMax(8, window->height() - height() - 8)));
    move(pos);
}

bool InfoPopup::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type())
    {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    {   // the press itself still reaches its target; the i button toggles on its own
        QWidget *target = qobject_cast<QWidget*>(watched);
        if (target && target != this && !isAncestorOf(target) && target != anchor)
            close();
        break;
    }
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
        {
            close();
            return true;
        }
        break;
    case QEvent::Hide:
        if (watched == anchor || watched == parentWidget())
            close();
        break;
    case QEvent::WindowDeactivate:
        if (watched == parentWidget())
            close();
        break;
    case QEvent::Resize:
        if (watched == parentWidget())
            place();
        break;
    default:
        break;
    }
    return QFrame::eventFilter(watched, event);
}

void Collapsible::setInfo(const QString& title, const QString& text)
{
    QPushButton *button = new QPushButton(this);
    styleInfoButton(button);
    infoButton = button;

    connect(button, &QPushButton::clicked, this, &Collapsible::showInfo);

    layoutBar->addWidget(button, Qt::AlignLeft);
    infotitle = title;
    infomsg = text;
}

void Collapsible::showInfo()
{
    InfoPopup::toggle(infoButton, infotitle, infomsg);
}

RangeSlider::RangeSlider(QWidget *parent, int vmin, int vmax)
    : QSlider(parent)
    , vmin(vmin)
    , vmax(vmax)
    , pos0(vmin)
    , pos1(vmax)
    , holding(0)
{
    setRange(vmin, vmax);
    setOrientation(Qt::Horizontal);

    // drawComplexControl() draws the background on every call if there is a stylesheet
    // to mitigate this, we set the background transparent
    setStyleSheet(
        "QSlider { background-color: rgba(180, 180, 180, 0); }\n"
        "QSlider::sub-page { background-color: rgba(255, 255, 255, 0); }"
    );
    //colinner = palette().color(QPalette::Highlight);
    //colouter = QColor(0, 0, 0, 0);
}

RangeSlider::~RangeSlider()
{
}

void RangeSlider::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    QStyleOptionSlider opt;
    initStyleOption(&opt);

    // draw the back groove of the slider without range highlighting
    opt.subControls = QStyle::SC_SliderGroove;
    opt.sliderValue = opt.sliderPosition = vmin;
    style()->drawComplexControl(QStyle::CC_Slider, &opt, &painter, this);

    // draw the range highlighting between the handles
    QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    opt.sliderValue = opt.sliderPosition = pos0;
    QRect handle0 = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
    opt.sliderValue = opt.sliderPosition = pos1;
    QRect handle1 = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

    if (colinner.isValid())
    {
        painter.setBrush(QBrush(colinner));
        painter.setPen(QPen(colinner, 0));
        int x0 = handle0.center().x();
        int x1 = handle1.center().x();
        int y  = handle0.center().y() - 1;
        QRect span = QRect(x0, y, x1-x0, 3);
        painter.drawRect(groove.intersected(span));
    }
    if (colouter.isValid())
    {
        painter.setBrush(QBrush(colouter));
        painter.setPen(QPen(colouter, 0));
        int x0 = handle0.center().x();
        int x1 = handle1.center().x();
        int y  = handle0.center().y() - 1;
        QRect left = QRect(groove.x()+1, y, x0-groove.x()-2, 3);
        painter.drawRect(groove.intersected(left));
        QRect right = QRect(x1+1, y, groove.right()-x1-2, 3);
        painter.drawRect(groove.intersected(right));
    }

    int pmin = pos0 < pos1 ? pos0 : pos1;
    int pmax = pos0 > pos1 ? pos0 : pos1;
    // draw handles
    opt.subControls = QStyle::SC_SliderHandle;
    opt.sliderValue = opt.sliderPosition = pmax;
    style()->drawComplexControl(QStyle::CC_Slider, &opt, &painter, this);
    opt.sliderValue = opt.sliderPosition = pmin;
    style()->drawComplexControl(QStyle::CC_Slider, &opt, &painter, this);
}

void RangeSlider::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
    {
        // determine which handle we are holding if any
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        QStyle::SubControl hit;
        holding = 0;
        opt.sliderValue = opt.sliderPosition = pos0;
        hit = style()->hitTestComplexControl(QStyle::CC_Slider, &opt, e->pos(), this);
        if (hit == QStyle::SC_SliderHandle)
            holding = -1;
        opt.sliderValue = opt.sliderPosition = pos1;
        hit = style()->hitTestComplexControl(QStyle::CC_Slider, &opt, e->pos(), this);
        if (hit == QStyle::SC_SliderHandle)
            holding = +1;
    }

    QSlider::mousePressEvent(e);
}

void RangeSlider::mouseReleaseEvent(QMouseEvent *e)
{
    holding = 0;
    if (pos0 > pos1)
    {
        int tmp = pos0;
        pos0 = pos1;
        pos1 = tmp;
    }
    emit valueChanged(0);
    QSlider::mouseReleaseEvent(e);
}

void RangeSlider::wheelEvent(QWheelEvent *e)
{
    QStyleOptionSlider opt;
    initStyleOption(&opt);

    QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);

    int delta = e->angleDelta().y() / 8 / 15;
    int x = (int)e->position().x() - groove.x();
    x = style()->sliderValueFromPosition(vmin, vmax, x, groove.width());

    int h = 0;
    if (x < pos0)
        h = -1;
    else if (x > pos1)
        h = +1;
    else if (abs(pos0 - x) < abs(pos1 - x))
        h = -1;
    else if (abs(pos0 - x) > abs(pos1 - x))
        h = +1;

    if (e->modifiers() & Qt::ControlModifier)
        delta *= 50;

    if (h < 0)
    {
        pos0 += delta;
        if (pos0 < vmin) pos0 = vmin;
        if (pos0 > pos1) pos0 = pos1;
        emit valueChanged(pos0);
    }
    else
    {
        pos1 += delta;
        if (pos1 > vmax) pos1 = vmax;
        if (pos1 < pos0) pos1 = pos0;
        emit valueChanged(pos1);
    }

    update();
}

void RangeSlider::mouseMoveEvent(QMouseEvent *e)
{
    if (holding == 0)
        return;

    QStyleOptionSlider opt;
    initStyleOption(&opt);

    opt.sliderValue = opt.sliderPosition = holding < 0 ? pos0 : pos1;

    QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);

    int x = e->position().toPoint().x() - groove.x();
    x = style()->sliderValueFromPosition(vmin, vmax, x, groove.width());

    if (holding < 0)
        pos0 = x;
    else
        pos1 = x;

    emit valueChanged(x);
    update();
}


LabeledRange::LabeledRange(QWidget *parent, int vmin, int vmax)
    : QWidget(parent)
{
    slider = new RangeSlider(this, vmin, vmax);
    minlabel = new QLabel(this);
    maxlabel = new QLabel(this);
    minlabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    maxlabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    int w = fontMetrics().tightBoundingRect("+012345").width();
    minlabel->setFixedWidth(w);
    maxlabel->setFixedWidth(w);

    QBoxLayout *l = new QBoxLayout(QBoxLayout::LeftToRight, this);
    l->addWidget(minlabel);
    l->addWidget(slider);
    l->addWidget(maxlabel);
    l->setContentsMargins(0, 0, 0, 0);
    setLayout(l);

    connect(slider, &QSlider::valueChanged, this, &LabeledRange::rangeChanged);
    rangeChanged();
}

LabeledRange::~LabeledRange()
{
}

void LabeledRange::setLimitText(QString min, QString max)
{
    mintxt = min;
    maxtxt = max;
    rangeChanged();
}

void LabeledRange::setValues(int p0, int p1)
{
    slider->pos0 = p0;
    slider->pos1 = p1;
    slider->update();
    rangeChanged();
}

void LabeledRange::setHighlight(QColor inner, QColor outer)
{
    slider->colinner = inner;
    slider->colouter = outer;
    slider->update();
}

void LabeledRange::rangeChanged()
{
    int pmin = slider->pos0 < slider->pos1 ? slider->pos0 : slider->pos1;
    int pmax = slider->pos0 > slider->pos1 ? slider->pos0 : slider->pos1;
    minlabel->setText(QString::number(pmin));
    maxlabel->setText(QString::number(pmax));
    if (!mintxt.isEmpty() && pmin == slider->vmin)
        minlabel->setText(mintxt);
    if (!maxtxt.isEmpty() && pmax == slider->vmax)
        maxlabel->setText(maxtxt);
    emit onRangeChange();
}


QSize CoordEdit::sizeHint() const
{
    QSize size = QLineEdit::minimumSizeHint();
    QFontMetrics fm(font());
    size.setWidth(size.width() + txtWidth(fm, "-30000000"));
    return size;
}

SpinExclude::SpinExclude(QWidget *parent)
    : QSpinBox(parent)
{
    setMinimum(-1);
    QObject::connect(this, &QSpinBox::valueChanged, this, &SpinExclude::change, Qt::QueuedConnection);
}

SpinExclude::~SpinExclude()
{
}

int SpinExclude::valueFromText(const QString &text) const
{
    return QSpinBox::valueFromText(text.section(" ", 0, 0));
}

QString SpinExclude::textFromValue(int value) const
{
    QString txt = QSpinBox::textFromValue(value);
    if (value == 0)
        txt += " " + tr("(ignore)");
    if (value == -1)
        txt += " " + tr("(exclude)");
    return txt;
}

void SpinExclude::change(int v)
{
    const char *style = "";
    if (v < 0)
        style = "background: #28ff0000";
    if (v > 0)
        style = "background: #2800ff00";
    setStyleSheet(style);
    findChild<QLineEdit*>()->deselect();
}


SpinInstances::SpinInstances(QWidget *parent)
    : QSpinBox(parent)
{
    setRange(0, 99);
}

SpinInstances::~SpinInstances()
{

}

int SpinInstances::valueFromText(const QString &text) const
{
    return QSpinBox::valueFromText(text.section(" ", 0, 0));
}

QString SpinInstances::textFromValue(int value) const
{
    // the number comes first, valueFromText() reads only that
    QString txt = QSpinBox::textFromValue(value);
    if (value == 0)
        txt += " " + tr("(none allowed)");
    else
        txt += " " + tr("or more");
    return txt;
}


ComboBoxDelegate::ComboBoxDelegate(QObject *parent, QComboBox *cmb)
    : QStyledItemDelegate(parent)
    , combo(cmb)
{
}

ComboBoxDelegate::~ComboBoxDelegate()
{
}

static bool isSeparator(const QModelIndex &index)
{
    return index.data(Qt::AccessibleDescriptionRole).toString() == QString::fromLatin1("separator");
}

void ComboBoxDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (isSeparator(index))
    {
        QStyleOptionViewItem opt = option;
        if (const QAbstractItemView *view = qobject_cast<const QAbstractItemView*>(option.widget))
            opt.rect.setWidth(view->viewport()->width());
        combo->style()->drawPrimitive(QStyle::PE_IndicatorToolBarSeparator, &opt, painter, combo);
    }
    else if (index.data().toString().contains('\t') || index.data(ShortcutRole).isValid())
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QStringList cols = opt.text.split('\t');
        const QList<int> starts = columnStarts(index.model(), opt.fontMetrics);
        opt.text = cols[0];
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        const QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const bool selected = opt.state & QStyle::State_Selected;
        painter->save();
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        for (int i = 1; i < cols.size(); i++)
            painter->drawText(r.adjusted(starts[i], 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, cols[i]);
        // the shortcut is right-aligned like in a menu
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->drawText(r.adjusted(0, 0, -6, 0), Qt::AlignRight | Qt::AlignVCenter,
            index.data(ShortcutRole).toString());
        painter->restore();
    }
    else if (!(index.flags() & Qt::ItemIsEnabled))
    {   // an entry that cannot be chosen: the style sheet gives the list one
        // text color for all states, so mute it here
        QStyleOptionViewItem opt = option;
        opt.palette.setColor(QPalette::Text, opt.palette.color(QPalette::PlaceholderText));
        QStyledItemDelegate::paint(painter, opt, index);
    }
    else
    {
        QStyledItemDelegate::paint(painter, option, index);
    }
}

// the x offsets of the tab separated text columns, aligned over all items of
// the model, followed by the end of the last column
QList<int> ComboBoxDelegate::columnStarts(const QAbstractItemModel *model, const QFontMetrics& fm)
{
    QList<int> widths;
    for (int row = 0, n = model->rowCount(); row < n; row++)
    {
        const QStringList cols = model->index(row, 0).data().toString().split('\t');
        for (int i = 0; i < cols.size(); i++)
        {
            if (widths.size() <= i)
                widths.append(0);
            widths[i] = std::max(widths[i], fm.horizontalAdvance(cols[i]));
        }
    }
    QList<int> starts = {0};
    for (int i = 0; i < widths.size(); i++)
        starts.append(starts.last() + widths[i] + (i + 1 < widths.size() ? fm.horizontalAdvance("   ") : 0));
    return starts;
}

QSize ComboBoxDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (isSeparator(index))
    {
        int pm = combo->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, combo) + 4;
        return QSize(pm, pm);
    }
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const QString text = index.data().toString();
    const QString shortcut = index.data(ShortcutRole).toString();
    if (text.contains('\t') || !shortcut.isEmpty())
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text = text.section('\t', 0, 0);
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const QFontMetrics fm(opt.font);
        int width = style->sizeFromContents(QStyle::CT_ItemViewItem, &opt, QSize(), widget).width();
        width += columnStarts(index.model(), fm).last() - fm.horizontalAdvance(opt.text);
        if (!shortcut.isEmpty())
            width += fm.horizontalAdvance(shortcut) + 6 * fm.averageCharWidth();
        size.setWidth(width);
    }
    int h = size.height();
    int hicon = combo->iconSize().height() + 1;
    int hfont = combo->fontMetrics().height() + 4;
    if (h < hicon) h = hicon;
    if (h < hfont) h = hfont;
    if (size.height() < h)
        size.setHeight(h);
    return size;
}
int StyledComboBox::widthForContents(int width) const
{
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    const QSize contents(width, qMax(fontMetrics().height(), 14) + 2);
    return style()->sizeFromContents(QStyle::CT_ComboBox, &opt, contents, this).width();
}

void StyledComboBox::paintEvent(QPaintEvent *e)
{
    if (isEditable())
        return QComboBox::paintEvent(e);
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    QString text = opt.currentText.split('\t').join("   ");
    // An item with a note in brackets, e.g. "256 (surface)", that does not
    // fit the box shows without the note instead of being cut off.
    const int paren = text.indexOf(" (");
    if (paren > 0)
    {
        const QRect field = style()->subControlRect(QStyle::CC_ComboBox, &opt,
            QStyle::SC_ComboBoxEditField, this);
        if (fontMetrics().horizontalAdvance(text) > field.width())
            text.truncate(paren);
    }
    if (text == opt.currentText)
        return QComboBox::paintEvent(e);
    QStylePainter painter(this);
    painter.setPen(palette().color(QPalette::Text));
    opt.currentText = text;
    painter.drawComplexControl(QStyle::CC_ComboBox, opt);
    painter.drawControl(QStyle::CE_ComboBoxLabel, opt);
}

CheckableComboBox::CheckableComboBox(QWidget *parent)
    : StyledComboBox(parent)
{
    setEditable(true);
    lineEdit()->setReadOnly(true);
    lineEdit()->installEventFilter(this);
    view()->viewport()->installEventFilter(this);
    updateSummary();
}

QVector<int> CheckableComboBox::checkedData() const
{
    QVector<int> values;
    for (int i = 0; i < count(); i++)
    {
        if (itemData(i, Qt::CheckStateRole).toInt() == Qt::Checked)
            values.append(itemData(i).toInt());
    }
    return values;
}

void CheckableComboBox::setCheckedData(const QVector<int>& values)
{
    QSet<int> checked(values.begin(), values.end());
    for (int i = 0; i < count(); i++)
        setItemData(i, checked.contains(itemData(i).toInt()) ? Qt::Checked : Qt::Unchecked,
                    Qt::CheckStateRole);
    updateSummary();
}

bool CheckableComboBox::eventFilter(QObject *object, QEvent *event)
{
    if (object == lineEdit() && event->type() == QEvent::MouseButtonPress)
    {
        QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            showPopup();
            return true;
        }
    }
    if (object == view()->viewport() && event->type() == QEvent::MouseButtonRelease)
    {
        QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        QModelIndex index = view()->indexAt(mouse->pos());
        if (index.isValid())
        {
            int row = index.row();
            bool checked = itemData(row, Qt::CheckStateRole).toInt() == Qt::Checked;
            setItemData(row, checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
            updateSummary();
            emit selectionChanged();
        }
        return true;
    }
    return StyledComboBox::eventFilter(object, event);
}

void CheckableComboBox::showPopup()
{
    // An editable QComboBox can send a delayed hide for the same click that
    // opened it. Ignore that close once, independent of platform/timing.
    ignoreNextPopupHide = true;
    StyledComboBox::showPopup();
}

void CheckableComboBox::hidePopup()
{
    if (ignoreNextPopupHide)
    {
        ignoreNextPopupHide = false;
        return;
    }
    StyledComboBox::hidePopup();
}

void CheckableComboBox::updateSummary()
{
    QStringList names;
    for (int i = 0; i < count(); i++)
    {
        if (itemData(i, Qt::CheckStateRole).toInt() == Qt::Checked)
            names.append(itemText(i));
    }
    QString summary = names.isEmpty() ? tr("Select biomes...") : names.join(", ");
    lineEdit()->setText(summary + QString::fromUtf8("  ▾"));
    lineEdit()->setToolTip(names.join("\n"));
}
