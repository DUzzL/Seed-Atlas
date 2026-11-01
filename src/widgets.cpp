#include "widgets.h"

#include "util.h"

#include <QApplication>
#include <QBoxLayout>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
#include <QToolButton>
#include <QVBoxLayout>

#include <QSet>

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

} // namespace


ScrollableToolBar::ScrollableToolBar(QWidget *parent)
    : QToolBar(parent)
    , scroll(new QScrollArea(this))
    , strip(new QWidget)
    , stripLayout(new QBoxLayout(QBoxLayout::TopToBottom, strip))
    , buttons()
    , separators()
    , toggleButton(new QToolButton(parent ? parent : this))
    , widthAnimation(new QPropertyAnimation(this, "sidebarWidth", this))
    , currentWidth(200)
    , collapsed(false)
{
    stripLayout->setContentsMargins(4, 4, 4, 4);
    stripLayout->setSpacing(3);
    stripLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    scroll->setWidget(strip);
    scroll->setWidgetResizable(false);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QToolBar::addWidget(scroll);
    toggleButton->setObjectName("rightSidebarToggle");
    toggleButton->setFixedSize(18, 72);
    toggleButton->setAutoRaise(false);
    toggleButton->setCursor(Qt::PointingHandCursor);
    toggleButton->setStyleSheet(
        "QToolButton { background: palette(window); border: 1px solid palette(mid); "
        "border-radius: 4px; padding: 0; margin: 0; }"
        "QToolButton:hover { background: palette(button); }");
    connect(toggleButton, &QToolButton::clicked, this, [this]() {
        setCollapsed(!collapsed);
    });
    widthAnimation->setDuration(220);
    widthAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(this, &QToolBar::orientationChanged, this, &ScrollableToolBar::updateDirection);
    connect(this, &QToolBar::iconSizeChanged, this, &ScrollableToolBar::updateButtons);
    updateDirection(orientation());
    updateToggleButton();
}

QAction *ScrollableToolBar::addAction(QAction *action)
{
    QToolButton *button = new QToolButton(strip);
    button->setToolButtonStyle(collapsed ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    button->setIconSize(iconSize());
    button->setMinimumWidth(collapsed ? 0 : 164);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setStyleSheet("QToolButton { text-align: left; padding: 5px 8px; }");
    const auto update = [button, action]() {
        button->setIcon(action->icon());
        button->setText(compactToolbarLabel(action));
        button->setToolTip(action->toolTip().isEmpty() ? action->text() : action->toolTip());
        button->setEnabled(action->isEnabled());
        button->setVisible(action->isVisible());
        button->setCheckable(action->isCheckable());
        button->setChecked(action->isChecked());
    };
    update();
    connect(action, &QAction::changed, button, update);
    connect(button, &QToolButton::clicked, action, [action]() { action->trigger(); });
    stripLayout->addWidget(button);
    buttons.append(button);
    strip->adjustSize();
    scroll->updateGeometry();
    updateGeometry();
    if (collapsed)
        setSidebarWidth(collapsedWidth());
    return action;
}

QAction *ScrollableToolBar::addSeparator()
{
    QFrame *separator = new QFrame(strip);
    if (orientation() == Qt::Horizontal)
    {
        separator->setFrameShape(QFrame::VLine);
        separator->setFixedWidth(6);
    }
    else
    {
        separator->setFrameShape(QFrame::HLine);
        separator->setFixedHeight(6);
    }
    stripLayout->addWidget(separator);
    separators.append(separator);
    return nullptr;
}

void ScrollableToolBar::resizeEvent(QResizeEvent *event)
{
    QToolBar::resizeEvent(event);
    if (QToolButton *extension = findChild<QToolButton *>("qt_toolbar_ext_button"))
        extension->hide();
    QWidget *overlayParent = toggleButton->parentWidget();
    const QPoint origin = mapTo(overlayParent, QPoint(0, 0));
    toggleButton->move(origin.x() - toggleButton->width()/2,
        origin.y() + qMax(0, (height() - toggleButton->height()) / 2));
    toggleButton->raise();
}

void ScrollableToolBar::setCollapsed(bool value, bool animated)
{
    if (collapsed == value &&
        (widthAnimation->state() == QAbstractAnimation::Stopped || !animated))
        return;

    const int startWidth = width();
    widthAnimation->stop();
    collapsed = value;
    for (QToolButton *button : buttons)
    {
        button->setToolButtonStyle(collapsed ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
        button->setMinimumWidth(collapsed ? 0 : 164);
    }
    scroll->setMinimumWidth(orientation() == Qt::Horizontal ? 0 : (collapsed ? 0 : 190));
    strip->adjustSize();
    const int targetWidth = collapsed ? collapsedWidth() : expandedWidth();
    updateToggleButton();
    emit collapsedChanged(collapsed);

    if (!animated || startWidth == targetWidth)
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

int ScrollableToolBar::expandedWidth() const
{
    return qMax(200, qMax(width(), scroll->minimumSizeHint().width()));
}

int ScrollableToolBar::collapsedWidth() const
{
    int buttonWidth = 0;
    for (const QToolButton *button : buttons)
        buttonWidth = qMax(buttonWidth, button->sizeHint().width());

    const QMargins margins = stripLayout->contentsMargins();
    const int scrollBarWidth = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll);
    return qMax(toggleButton->width(), buttonWidth + margins.left() + margins.right() + scrollBarWidth);
}

void ScrollableToolBar::updateToggleButton()
{
    toggleButton->setArrowType(collapsed ? Qt::LeftArrow : Qt::RightArrow);
    toggleButton->setToolTip(collapsed ? tr("Expand side panel") : tr("Collapse side panel"));
    toggleButton->raise();
}

void ScrollableToolBar::updateDirection(Qt::Orientation orientation)
{
    const QBoxLayout::Direction direction = orientation == Qt::Horizontal
        ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
    stripLayout->setDirection(direction);
    const bool horizontal = orientation == Qt::Horizontal;
    scroll->setHorizontalScrollBarPolicy(horizontal ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(horizontal ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    strip->setSizePolicy(horizontal ? QSizePolicy::Minimum : QSizePolicy::Maximum,
        horizontal ? QSizePolicy::Maximum : QSizePolicy::Minimum);
    scroll->setMinimumWidth(horizontal ? 0 : (collapsed ? 0 : 190));
    if (!collapsed)
        setMinimumWidth(horizontal ? 0 : 200);
    setMinimumHeight(horizontal ? iconSize().height() + 16 : 0);
    for (QFrame *separator : separators)
    {
        separator->setMinimumSize(0, 0);
        separator->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        if (horizontal)
        {
            separator->setFrameShape(QFrame::VLine);
            separator->setFixedWidth(6);
        }
        else
        {
            separator->setFrameShape(QFrame::HLine);
            separator->setFixedHeight(6);
        }
    }
}

void ScrollableToolBar::updateButtons(const QSize& size)
{
    for (QToolButton *button : buttons)
    {
        button->setIconSize(size);
        button->setMinimumHeight(size.height() + 10);
    }
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

    QFont font = toggleButton->font();
    font.setBold(true);
    toggleButton->setFont(font);
    toggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggleButton->setArrowType(Qt::ArrowType::RightArrow);
    toggleButton->setCheckable(true);


    frameBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    frameBar->setFrameShape(QFrame::HLine);

    QVBoxLayout *vbox = new QVBoxLayout();
    layoutBar = new QHBoxLayout();
    layoutBar->addWidget(toggleButton, Qt::AlignBottom);
    layoutBar->addWidget(frameBar, Qt::AlignCenter);
    layoutBar->setSpacing(5);
    vbox->addLayout(layoutBar);

    layoutContent = new QHBoxLayout();
    layoutContent->setContentsMargins(20, 0, 0, 0);
    vbox->addLayout(layoutContent);

    vbox->setSizeConstraint(QLayout::SetMaximumSize);
    vbox->setContentsMargins(0, 0, 0, 8);
    vbox->setSpacing(0);
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
    toggleButton->setText(title);
    layoutContent->addWidget(widget);
    contentHeight = widget->sizeHint().height();
    content = widget;
    animgroup->addAnimation(new QPropertyAnimation(content, "minimumHeight"));
    animgroup->addAnimation(new QPropertyAnimation(content, "maximumHeight"));
    setCollapsed(collapsed);
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
    QString txt = QSpinBox::textFromValue(value);
    if (value == 0)
        txt += " " + tr("(exclude)");
    if (value > 1)
        txt += " " + tr("(cluster)");
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
void StyledComboBox::paintEvent(QPaintEvent *e)
{
    if (isEditable() || !currentText().contains('\t'))
        return QComboBox::paintEvent(e);
    QStylePainter painter(this);
    painter.setPen(palette().color(QPalette::Text));
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    opt.currentText = opt.currentText.split('\t').join("   ");
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
