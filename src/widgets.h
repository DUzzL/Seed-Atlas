#ifndef WIDGETS_H
#define WIDGETS_H

#include <QWidget>
#include <QSlider>
#include <QComboBox>
#include <QStyledItemDelegate>
#include <QSpinBox>
#include <QLineEdit>
#include <QBoxLayout>
#include <QToolBar>
#include <QVector>
#include <QFrame>
#include <QPointer>

class QParallelAnimationGroup;
class QToolButton;
class QFrame;
class QHBoxLayout;
class QLabel;
class QScrollArea;
class QResizeEvent;
class QPropertyAnimation;

// A dockable toolbar whose action strip remains accessible through a scroll
// area instead of hiding overflow actions in Qt's extension menu.
class ScrollableToolBar : public QToolBar
{
    Q_OBJECT
    Q_PROPERTY(int sidebarWidth READ sidebarWidth WRITE setSidebarWidth)
public:
    explicit ScrollableToolBar(QWidget *parent = nullptr);

    QAction *addAction(QAction *action);
    QAction *addSeparator();
    bool isCollapsed() const { return collapsed; }
    int sidebarWidth() const { return currentWidth; }
    void setCollapsed(bool collapsed, bool animated = true);
    void setSidebarWidth(int width);

protected:
    void resizeEvent(QResizeEvent *event) override;

signals:
    void collapsedChanged(bool collapsed);

private slots:
    void updateDirection(Qt::Orientation orientation);
    void updateButtons(const QSize& size);

private:
    int expandedWidth() const;
    int collapsedWidth() const;
    void updateToggleButton();

    QScrollArea *scroll;
    QWidget *strip;
    QBoxLayout *stripLayout;
    QVector<QToolButton*> buttons;
    QVector<QFrame*> separators;
    QToolButton *toggleButton;
    QPropertyAnimation *widthAnimation;
    int currentWidth;
    bool collapsed;
};

class QAbstractButton;
class QPushButton;

// Every help ("i") button in the app looks and behaves the same: an "i" in a
// ring of the theme's accent color, filled on hover or while its help is
// open, and a help card below the button that a second click, a click
// anywhere else or Esc closes again.
void styleInfoButton(QAbstractButton *button);
QPushButton *createInfoButton(QWidget *parent, const QString& title, const QString& text);

class InfoPopup : public QFrame
{
public:
    // Opens the help below anchor (or below area within it), or closes it if
    // it is already open there. Only one help card is open at a time.
    static void toggle(QWidget *anchor, const QString& title, const QString& text,
            const QRect& area = QRect());

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    InfoPopup(QWidget *anchor, const QRect& area, const QString& title, const QString& text);
    ~InfoPopup();
    void place();

    QPointer<QWidget> anchor;
    QRect area;
    static QPointer<InfoPopup> current;
};

class Collapsible : public QWidget
{
    Q_OBJECT
public:
    explicit Collapsible(QWidget *parent = nullptr);
    void init(const QString& title, QWidget *widget, bool collapsed);
    void setInfo(const QString& title, const QString& text);
    void setCollapsed(bool collapsed); // without animation

public slots:
    void toggle(bool collapsed);
    void finishAnimation();
    void showInfo();

public:
    QParallelAnimationGroup* animgroup;
    QToolButton* toggleButton;
    QFrame* frameBar;
    QWidget *content;
    QHBoxLayout *layoutBar;
    QHBoxLayout *layoutContent;

    int contentHeight;
    QString infotitle;
    QString infomsg;
    QWidget *infoButton = nullptr;
};

class RangeSlider : public QSlider
{
    Q_OBJECT
public:
    RangeSlider(QWidget *parent = nullptr, int vmin = 0, int vmax = 100);
    virtual ~RangeSlider();

    virtual void paintEvent(QPaintEvent *e) override;
    virtual void wheelEvent(QWheelEvent *e) override;
    virtual void mousePressEvent(QMouseEvent *e) override;
    virtual void mouseReleaseEvent(QMouseEvent *e) override;
    virtual void mouseMoveEvent(QMouseEvent *e) override;

    int vmin, vmax;
    int pos0, pos1;
    int holding;
    QColor colinner, colouter;
};

class LabeledRange : public QWidget
{
    Q_OBJECT
public:
    LabeledRange(QWidget *parent = nullptr, int vmin = 0, int vmax = 10);
    virtual ~LabeledRange();

    void setValues(int p0, int p1);
    void setLimitText(QString min, QString max);
    void setHighlight(QColor inner, QColor outer);

public slots:
    void rangeChanged();

signals:
    void onRangeChange();

public:
    RangeSlider *slider;
    QLabel *minlabel, *maxlabel;
    QString mintxt, maxtxt;
};


// QLineEdit defaults to a style hint width 17 characters, which is too long for coordinates
class CoordEdit : public QLineEdit
{
    Q_OBJECT
public:
    CoordEdit(QWidget *parent = nullptr) : QLineEdit(parent) {}
    virtual QSize sizeHint() const override;
};

class SpinExclude : public QSpinBox
{
    Q_OBJECT
public:
    SpinExclude(QWidget *parent = nullptr);
    virtual ~SpinExclude();
    virtual int valueFromText(const QString &text) const override;
    virtual QString textFromValue(int value) const override;

public slots:
    void change(int v);
};

class SpinInstances : public QSpinBox
{
    Q_OBJECT
public:
    SpinInstances(QWidget *parent = nullptr);
    virtual ~SpinInstances();
    virtual int valueFromText(const QString &text) const override;
    virtual QString textFromValue(int value) const override;
};

// QComboBox uses QItemDelegate, which would not support styles
class ComboBoxDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    // item texts may hold tab separated columns, which are aligned over all
    // items, and a shortcut in this role, which is shown right-aligned
    enum { ShortcutRole = Qt::UserRole + 64 };

    ComboBoxDelegate(QObject *parent, QComboBox *cmb);
    virtual ~ComboBoxDelegate();
    virtual void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    virtual QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    static QList<int> columnStarts(const QAbstractItemModel *model, const QFontMetrics& fm);

    QComboBox *combo;
};

class StyledComboBox : public QComboBox
{
    Q_OBJECT
public:
    StyledComboBox(QWidget *parent) : QComboBox(parent)
    {
        setItemDelegate(new ComboBoxDelegate(parent, this));
    }

protected:
    // shows the tab separated columns of an item with spaces between them
    void paintEvent(QPaintEvent *e) override;
};

class CheckableComboBox : public StyledComboBox
{
    Q_OBJECT
public:
    explicit CheckableComboBox(QWidget *parent = nullptr);

    QVector<int> checkedData() const;
    void setCheckedData(const QVector<int>& values);
    void showPopup() override;
    void hidePopup() override;

signals:
    void selectionChanged();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void updateSummary();
    bool ignoreNextPopupHide = false;
};

#endif // WIDGETS_H
