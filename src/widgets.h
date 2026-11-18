#ifndef WIDGETS_H
#define WIDGETS_H

#include <QIcon>
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
class QScrollBar;
class QResizeEvent;
class QPropertyAnimation;

class QAbstractItemView;

// Chrome icons (tool rail, map controls, chevrons) drawn as vector shapes in
// the theme's colors, so they stay crisp at any scale and follow the theme.
// kind: search, biomes, structures, slime, eye, datapacks, menu, settings,
// origin, plus, minus, go, chevron-left, chevron-right, arrow-up, arrow-down,
// close, layers. A second color is used for
// the checked ("On") state, e.g. the active tool in the rail.
QIcon chromeIcon(const QString& kind, const QColor& color, int size = 20,
                 const QColor& checkedColor = QColor());

// Map layer icons: the small pixel-art sources scaled with nearest-neighbour
// at integer factors for each device pixel ratio, and a calmer but legible
// grey "off" version that suits the background of the given theme.
QIcon layerIcon(const QString& rcbase, int size, bool darkTheme);

// Shows a short muted hint centered in a list or table while it has no rows.
void setEmptyHint(QAbstractItemView *view, const QString& text);

// The layer sidebar at the right edge of the map. Collapsed it shows only the
// layer icons stacked vertically (names as tooltips); expanded it shows icon
// and name with small group headings. The action strip scrolls instead of
// hiding overflow actions in Qt's extension menu.
class ScrollableToolBar : public QToolBar
{
    Q_OBJECT
    Q_PROPERTY(int sidebarWidth READ sidebarWidth WRITE setSidebarWidth)
public:
    explicit ScrollableToolBar(QWidget *parent = nullptr);

    QAction *addAction(QAction *action);
    QAction *addSeparator();
    // a separator followed by a group heading that shows while expanded
    void addSection(const QString& title);
    bool isCollapsed() const { return collapsed; }
    int sidebarWidth() const { return currentWidth; }
    void setCollapsed(bool collapsed, bool animated = true);
    void setSidebarWidth(int width);
    // color of the expand/collapse chevron, follows the theme
    void setChromeColor(const QColor& color);

protected:
    void resizeEvent(QResizeEvent *event) override;

signals:
    void collapsedChanged(bool collapsed);

private slots:
    void updateButtons(const QSize& size);

private:
    int expandedWidth() const;
    int collapsedWidth() const;
    void updateToggleButton();
    void applyCollapsedLayout();

    QWidget *panel;
    QLabel *title;
    QHBoxLayout *headerLayout;
    QScrollArea *scroll;
    // The visible scroll bar lives in a gutter column of its own next to the
    // scroll area, whose own bar stays hidden: showing or hiding it never
    // narrows the strip, so it cannot cover an icon or its outline.
    QWidget *gutter;
    QScrollBar *scrollBar;
    QWidget *strip;
    QBoxLayout *stripLayout;
    QVector<QToolButton*> buttons;
    QVector<QLabel*> sectionLabels;
    QVector<QFrame*> separators;
    QToolButton *toggleButton;
    QPropertyAnimation *widthAnimation;
    QColor chromeColor;
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
    // shows "TITLE · count" in the heading; a negative count hides it
    void setCount(int count);
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
    QString title;
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

    // the width of the box around contents of this width (as in its size hint)
    int widthForContents(int width) const;

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
