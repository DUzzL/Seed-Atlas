#ifndef MOTIONCONTROLLER_H
#define MOTIONCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QSet>

class QApplication;
class QEvent;
class QStackedWidget;
class QTabWidget;
class QWidget;

// Provides one restrained motion language for widgets created anywhere in the
// application. Keeping this in one event filter also covers dialogs and tabs
// that are built dynamically after start-up.
class MotionController : public QObject
{
    Q_OBJECT
public:
    explicit MotionController(QApplication *application);

    bool animationsEnabled() const { return enabled; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void registerWidgetTree(QWidget *root);
    void registerTabs(QTabWidget *tabs);
    void registerStack(QStackedWidget *stack);
    void animatePage(QWidget *page, int direction = 0);
    void animatePopup(QWidget *popup);
    bool shouldAnimatePopup(const QWidget *widget) const;

    bool enabled;
    QSet<QObject*> registeredContainers;
    QSet<QObject*> openingPopups;
};

#endif
