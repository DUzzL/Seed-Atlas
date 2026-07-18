#include "motioncontroller.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QDialog>
#include <QEasingCurve>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QMenu>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

bool systemAllowsMotion()
{
    if (qEnvironmentVariableIsSet("SEED_ATLAS_REDUCED_MOTION"))
        return false;

#ifdef Q_OS_WIN
    BOOL animations = TRUE;
    if (SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0))
        return animations;
#endif

    // Common Linux desktop accessibility setting. An unset variable means the
    // desktop has not requested reduced motion.
    const QByteArray gtkAnimations = qgetenv("GTK_ENABLE_ANIMATIONS").trimmed().toLower();
    return gtkAnimations != "0" && gtkAnimations != "false";
}

} // namespace

MotionController::MotionController(QApplication *application)
    : QObject(application)
    , enabled(systemAllowsMotion())
{
    application->installEventFilter(this);
}

bool MotionController::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::Polish && type != QEvent::Show)
        return QObject::eventFilter(watched, event);

    QWidget *widget = qobject_cast<QWidget*>(watched);
    if (!widget)
        return QObject::eventFilter(watched, event);

    registerWidgetTree(widget);

    if (enabled && type == QEvent::Show && shouldAnimatePopup(widget))
        animatePopup(widget);

    return QObject::eventFilter(watched, event);
}

void MotionController::registerWidgetTree(QWidget *root)
{
    // Polish/Show reaches every widget through the application-wide event
    // filter, so inspecting the widget itself is sufficient. Avoid repeatedly
    // walking the whole object tree while a complex dialog is constructed.
    if (QTabWidget *tabs = qobject_cast<QTabWidget*>(root))
        registerTabs(tabs);
    if (QStackedWidget *stack = qobject_cast<QStackedWidget*>(root))
        registerStack(stack);
}

void MotionController::registerTabs(QTabWidget *tabs)
{
    if (!tabs || registeredContainers.contains(tabs))
        return;
    registeredContainers.insert(tabs);
    tabs->setProperty("seedAtlasPreviousTab", tabs->currentIndex());
    connect(tabs, &QObject::destroyed, this, [this, tabs]() {
        registeredContainers.remove(tabs);
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
        const int previous = tabs->property("seedAtlasPreviousTab").toInt();
        tabs->setProperty("seedAtlasPreviousTab", index);
        if (enabled && index >= 0)
            animatePage(tabs->widget(index), index > previous ? 1 : -1);
    });
}

void MotionController::registerStack(QStackedWidget *stack)
{
    // QTabWidget owns an internal stack and already emits the same change.
    // Registering both would restart the page transition twice.
    if (!stack || qobject_cast<QTabWidget*>(stack->parentWidget()) ||
            registeredContainers.contains(stack))
        return;
    registeredContainers.insert(stack);
    stack->setProperty("seedAtlasPreviousPage", stack->currentIndex());
    connect(stack, &QObject::destroyed, this, [this, stack]() {
        registeredContainers.remove(stack);
    });
    connect(stack, &QStackedWidget::currentChanged, this, [this, stack](int index) {
        const int previous = stack->property("seedAtlasPreviousPage").toInt();
        stack->setProperty("seedAtlasPreviousPage", index);
        if (enabled && index >= 0)
            animatePage(stack->widget(index), index > previous ? 1 : -1);
    });
}

void MotionController::animatePage(QWidget *page, int direction)
{
    if (!page || !page->isVisible())
        return;

    // A short fade is enough to make a content change legible without slowing
    // navigation. The graphics effect is removed afterwards so map rendering
    // and large result views do not pay a permanent off-screen-rendering cost.
    if (QAbstractAnimation *running = page->findChild<QAbstractAnimation*>(
            "seedAtlasPageMotion", Qt::FindDirectChildrenOnly))
        running->stop();
    page->setGraphicsEffect(nullptr);

    const QVariant savedRestPosition = page->property("seedAtlasMotionRestPosition");
    if (savedRestPosition.isValid())
        page->move(savedRestPosition.toPoint());

    const QPoint restPosition = page->pos();
    page->setProperty("seedAtlasMotionRestPosition", restPosition);
    const int distance = 28;
    const QPoint startPosition = restPosition + QPoint(direction * distance, 0);
    page->move(startPosition);

    QGraphicsOpacityEffect *effect = new QGraphicsOpacityEffect(page);
    effect->setOpacity(0.76);
    page->setGraphicsEffect(effect);

    QParallelAnimationGroup *group = new QParallelAnimationGroup(page);
    group->setObjectName("seedAtlasPageMotion");

    QPropertyAnimation *fade = new QPropertyAnimation(effect, "opacity", group);
    fade->setDuration(190);
    fade->setStartValue(0.76);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(fade);

    QPropertyAnimation *slide = new QPropertyAnimation(page, "pos", group);
    slide->setDuration(210);
    slide->setStartValue(startPosition);
    slide->setEndValue(restPosition);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(slide);

    QPointer<QWidget> guardedPage(page);
    connect(group, &QParallelAnimationGroup::finished, this, [guardedPage, restPosition]() {
        if (guardedPage)
        {
            guardedPage->move(restPosition);
            guardedPage->setProperty("seedAtlasMotionRestPosition", QVariant());
            guardedPage->setGraphicsEffect(nullptr);
        }
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

bool MotionController::shouldAnimatePopup(const QWidget *widget) const
{
    if (!widget || !widget->isWindow() || openingPopups.contains(const_cast<QWidget*>(widget)))
        return false;
    if (widget->windowFlags().testFlag(Qt::ToolTip))
        return false;
    return qobject_cast<const QDialog*>(widget) || qobject_cast<const QMenu*>(widget) ||
        widget->windowFlags().testFlag(Qt::Popup);
}

void MotionController::animatePopup(QWidget *popup)
{
    openingPopups.insert(popup);
    connect(popup, &QObject::destroyed, this, [this, popup]() {
        openingPopups.remove(popup);
    });

    const bool isMenu = qobject_cast<QMenu*>(popup);
    const int duration = isMenu ? 125 : 190;
    const QPoint finalPosition = popup->pos();
    const QPoint startPosition = finalPosition + QPoint(0, isMenu ? -3 : 7);

    popup->setWindowOpacity(0.0);
    popup->move(startPosition);

    QParallelAnimationGroup *group = new QParallelAnimationGroup(popup);
    QPropertyAnimation *fade = new QPropertyAnimation(popup, "windowOpacity", group);
    fade->setDuration(duration);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(fade);

    QPropertyAnimation *slide = new QPropertyAnimation(popup, "pos", group);
    slide->setDuration(duration);
    slide->setStartValue(startPosition);
    slide->setEndValue(finalPosition);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(slide);

    QPointer<QWidget> guardedPopup(popup);
    connect(group, &QParallelAnimationGroup::finished, this, [this, guardedPopup, popup]() {
        openingPopups.remove(popup);
        if (guardedPopup)
            guardedPopup->setWindowOpacity(1.0);
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}
