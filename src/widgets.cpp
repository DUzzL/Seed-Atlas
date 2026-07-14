#include "widgets.h"

#include "util.h"

#include <QApplication>
#include <QBoxLayout>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QListView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyleOptionSlider>
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
    connect(this, &QToolBar::orientationChanged, this, &ScrollableToolBar::updateDirection);
    connect(this, &QToolBar::iconSizeChanged, this, &ScrollableToolBar::updateButtons);
    updateDirection(orientation());
}

QAction *ScrollableToolBar::addAction(QAction *action)
{
    QToolButton *button = new QToolButton(strip);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(iconSize());
    button->setMinimumWidth(164);
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
    scroll->setMinimumWidth(horizontal ? 0 : 190);
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

    int height = content->size().height();
    if (height)
        contentHeight = height;

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
        anim->setDuration(200);
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

void Collapsible::setInfo(const QString& title, const QString& text)
{
    QPushButton *button = new QPushButton("", this);
    button->setStyleSheet(
      "QPushButton {"
      "    background-color: rgba(255, 255, 255, 0);"
      "    border: none;"
      "    image: url(:/icons/info.png);"
      "    image-position: right;"
      "}"
      "QPushButton:hover {"
      "    image: url(:/icons/info_h.png);"
      "}");
    int fmh = fontMetrics().height();
    button->setIconSize(QSize(fmh, fmh));
    button->setToolTip(tr("Show help"));

    connect(button, SIGNAL(clicked()), this, SLOT(showInfo()));

    layoutBar->addWidget(button, Qt::AlignLeft);
    infotitle = title;
    infomsg = text;
}

void Collapsible::showInfo()
{
    // windows plays an annoying sound when you use QMessageBox::information()
    QMessageBox *mb = new QMessageBox(this);
    mb->setIcon(QMessageBox::Information);
    mb->setWindowTitle(infotitle);
    mb->setText(infomsg);
    mb->show();
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
    int x;
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
    x = e->x() - groove.x();
#else
    x = (int)e->position().x() - groove.x();
#endif
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

    int x = e->x() - groove.x();
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

    connect(slider, SIGNAL(valueChanged(int)), this, SLOT(rangeChanged(void)));
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
    QObject::connect(this, SIGNAL(valueChanged(int)), this, SLOT(change(int)), Qt::QueuedConnection);
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
    else
    {
        QStyledItemDelegate::paint(painter, option, index);
    }
}

QSize ComboBoxDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (isSeparator(index))
    {
        int pm = combo->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, combo) + 4;
        return QSize(pm, pm);
    }
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    int h = size.height();
    int hicon = combo->iconSize().height() + 1;
    int hfont = combo->fontMetrics().height() + 4;
    if (h < hicon) h = hicon;
    if (h < hfont) h = hfont;
    if (size.height() < h)
        size.setHeight(h);
    return size;
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
