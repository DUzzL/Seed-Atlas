#ifndef CONDITIONDIALOG_H
#define CONDITIONDIALOG_H

#include "config.h"
#include "formconditions.h"
#include "search.h"
#include "util.h"
#include "widgets.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QDialog>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QSpacerItem>

class MainWindow;
class MapView;

namespace Ui {
class ConditionDialog;
}

class NoiseBiomeIndicator : public QCheckBox
{
    Q_OBJECT
public:
    NoiseBiomeIndicator(QString title, QWidget *parent)
        : QCheckBox(title, parent)
    {
    }
    virtual ~NoiseBiomeIndicator() {}
    void mousePressEvent(QMouseEvent *event)
    {   // make read only
        if (event->button() == 0)
            QCheckBox::mousePressEvent(event);
    }
};

class VariantCheckBox : public QCheckBox
{
    Q_OBJECT
public:
    VariantCheckBox(const StartPiece *sp) : QCheckBox(displayName(sp->name)),sp(sp)
    {
        setToolTip(sp->name);
    }
    // "plains_meeting_point_1" -> "Plains meeting point 1"
    static QString displayName(const char *name)
    {
        QString s = QString(name).replace('_', ' ');
        if (!s.isEmpty())
            s[0] = s[0].toUpper();
        return s;
    }
    virtual ~VariantCheckBox() {}
    const StartPiece *sp;
};


class QLineEdit;
class QTabBar;
class QTreeWidget;
class QTreeWidgetItem;

// The type picker of the condition editor: a search field above a list of
// the condition types, grouped under headings. Expert types are listed only
// when "Show expert types" is on (and the current type is always listed).
class ConditionTypePicker : public QWidget
{
    Q_OBJECT
public:
    ConditionTypePicker(int mc, bool showExpert, QWidget *parent = nullptr);

    void setCurrentType(int type);
    void setShowExpert(bool show);
    void setSearchText(const QString& text);
    void focusSearch();

signals:
    void typeChosen(int type);
    void expertToggled(bool show);
    void cancelled();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void fill();
    void applyFilter();
    void chooseFirstMatch();

public:
    QLineEdit *search;
    QTreeWidget *list;
    QCheckBox *checkExpert;
    QLabel *labelEmpty;
    QPushButton *buttonKeep;

private:
    int mc;
    int current = 0;
    bool showExpert;
};

class ConditionDialog : public QDialog
{
    Q_OBJECT

public:

    explicit ConditionDialog(FormConditions *parent, MapView *mapview, Config *config, WorldInfo wi, QListWidgetItem *item = 0, Condition *initcond = 0);
    virtual ~ConditionDialog();

    void addTempCat(int temp, QString name);
    void initComboY(QComboBox *cb, int y);
    void updateMode();
    // the condition as the editor shows it, as it would be accepted
    Condition currentCondition();
    // opens the editor in a given state for screenshots (see main.cpp):
    // "<type id or name>[:option...]", options: picker, search=<text>,
    // circle, square, custom, count=<n>, include=<biome ids>, expert
    void applyTestPreset(const QString& spec);
    void updateBiomeSelection();
    bool warnIfBad(Condition cond);

    void onReject();
    void onAccept();

    void getClimateLimits(int limok[6][2], int limex[6][2]);
    void getClimateLimits(LabeledRange *ranges[6], int limits[6][2]);
    void setClimateLimits(LabeledRange *ranges[6], int limits[6][2], bool complete);

signals:
    void setCond(QListWidgetItem *item, Condition cond, int modified);

private:
    bool detailsGrow() const; // the details are a long list
    // the height of the content where it is short, the full height otherwise
    void fitHeight();
    bool sized = false; // the constructor has set the first size

private slots:
    void setType(int type);
    void setPickerOpen(bool open);
    void updateSummary();
    void updateChosen();
    void updateDetailsSpace();
    void on_comboScale_activated(int);

    void on_comboRelative_activated(int);

    void on_buttonUncheck_clicked();
    void on_buttonInclude_clicked();
    void on_buttonExclude_clicked();

    void on_buttonAreaInfo_clicked();
    void on_buttonFromVisible_clicked();

    void on_radioRadius_toggled(bool checked);
    void on_radioSquare_toggled(bool checked);
    void on_radioCustom_toggled(bool checked);

    void on_lineSquare_editingFinished();
    //void on_lineRadius_editingFinished();

    void on_ConditionDialog_finished(int result);

    void onCheckStartChanged(int state);
    void onClimateLimitChanged();

    void on_lineBiomeSize_textChanged(const QString &text);

    void onLuaSaveAs(const QString& fileName);
    void on_comboLua_currentIndexChanged(int index);
    void on_pushLuaSaveAs_clicked();
    void on_pushLuaSave_clicked();
    void on_pushLuaOpen_clicked();
    void on_pushLuaExample_clicked();

    void on_comboHeightRange_currentIndexChanged(int index);

    void on_pushInfoLua_clicked();

    void on_comboClimatePara_currentIndexChanged(int index);
    void on_comboOctaves_currentIndexChanged(int index);

    void on_comboY1_currentTextChanged(const QString &text);
    void on_comboY2_currentTextChanged(const QString &text);

private:
    bool resolveLuaChanges();
    QString typeInfoText(int filterIndex) const; // help of a filter type

    Ui::ConditionDialog *ui;

    QFrame *separator;
    class QPushButton *typeInfoButton;
    ConditionTypePicker *picker;
    QLabel *labelHint;
    QFrame *frameChosen;
    QLabel *chosenIcon, *chosenName, *chosenBrief, *chosenWarning;
    QPushButton *buttonChange;
    QTabBar *shapeBar;
    QSpacerItem *detailsSpacer = nullptr;
    QTimer *summaryTimer = nullptr;
    std::vector<Condition> others; // the other conditions of the list
    int filterType = F_SELECT;
    bool editing = false;
    std::map<int, QCheckBox*> biomecboxes;
    SpinExclude *tempsboxes[9];
    LabeledRange *climaterange[2][6];
    QCheckBox *climatecomplete[6];
    std::map<int, NoiseBiomeIndicator*> noisebiomes;

    QVector<VariantCheckBox*> variantboxes;
    uint64_t luahash;

public:
    MapView *mapview;
    Config *config;
    QListWidgetItem *item;
    Condition cond;
    WorldInfo wi;
};

#endif // CONDITIONDIALOG_H
