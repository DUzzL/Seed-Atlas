#ifndef FORMDPCONDITIONS_H
#define FORMDPCONDITIONS_H

// The conditions of the Seed Finder for datapack worlds: a list like the one
// of the vanilla conditions (FormConditions), with a row of quick choices and
// an editor, for the biomes of the loaded packs.

#include "dpsearch.h"

#include <QDialog>
#include <QIcon>
#include <QRgb>
#include <QSet>
#include <QWidget>

#include <memory>
#include <vector>

class MainWindow;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QStackedWidget;
struct DatapackWorld;

// A small rounded colour swatch for a biome. @plainSelected: a highlighted
// row keeps the plain swatch (Qt tints it otherwise).
QIcon dpSwatchIcon(QRgb color, const QColor& border, bool plainSelected = false);
// The biomes of the shown dimension in the order of the pickers: those of
// the packs first, Minecraft's own last, by name within a namespace.
std::vector<int> dpBiomeOrder(const DatapackWorld& world);
// Hides the rows of a list grouped under headings (rows with an empty
// @idRole) whose text and id do not contain @needle, and the headings left
// without rows.
void dpFilterGroupedList(QListWidget *list, int idRole, const QString& needle);

class FormDpConditions : public QWidget
{
    Q_OBJECT
public:
    explicit FormDpConditions(MainWindow *parent);

    std::vector<DpCondition> getConditions() const;
    void setConditions(const std::vector<DpCondition>& conditions);
    void clearConditions();
    // adds a condition (with the next free id if it has none) or replaces the
    // one with its id
    void addCondition(DpCondition c);
    // the world changed: names, colours and the biomes of the quick choice
    void refreshWorld();
    QListWidget *list() const { return listConditions; }
    // opens the editor for a new condition
    void openEditor(const DpCondition *edit = nullptr);

signals:
    void changed();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    // the wrapped note keeps the height of its lines (a word-wrapped label
    // does not tell the layout how tall it gets)
    void fitNote();
    void fillQuickBiomes();
    void updateQuickRow();
    void updateItem(QListWidgetItem *item, const DpCondition& c);
    void updateButtons();
    int nextId() const;
    std::shared_ptr<const DatapackWorld> world() const;

    MainWindow *parent;
    QComboBox *quickKind;
    QComboBox *quickBiome;
    QComboBox *quickDistance;
    QPushButton *quickAdd;
    QLabel *note;
    QListWidget *listConditions;
    QPushButton *buttonRemoveAll, *buttonRemove, *buttonDisable, *buttonEdit;
    std::shared_ptr<const DatapackWorld> shownWorld;
};

class DpConditionDialog : public QDialog
{
    Q_OBJECT
public:
    DpConditionDialog(QWidget *parent, const std::shared_ptr<const DatapackWorld>& world,
        const DpCondition& condition);

    DpCondition condition() const;

private:
    void addHeading(const QString& text);
    void addMissing(const QStringList& checked, const QSet<QString>& listed, const QString& tip);
    void fillBiomes(const QStringList& checked);
    void fillStructures(const QStringList& checked);
    void filterBiomes();
    void updateKind();
    void updateSummary();
    QStringList checkedBiomes() const;

    std::shared_ptr<const DatapackWorld> world;
    DpCondition base;
    QFormLayout *form;
    QComboBox *comboKind;
    QLabel *labelKind;
    QLineEdit *lineSearch;
    QListWidget *listBiomes;
    QLabel *labelChecked;
    QComboBox *comboMatch;
    QSpinBox *spinPercent;
    QWidget *rowPercent;
    QWidget *listBox;           // the list of biomes or structures
    QWidget *rowCount;
    QSpinBox *spinCount;
    QComboBox *comboStates;
    QStringList keptBiomes, keptStructures;
    bool listsStructures = false;
    QComboBox *comboShape;
    QStackedWidget *stackWhere;
    QLineEdit *lineX, *lineZ, *lineR;
    QLineEdit *lineX1, *lineZ1, *lineX2, *lineZ2;
    QLineEdit *linePosX, *linePosZ;
    QComboBox *comboY;
    QComboBox *comboScale;
    QWidget *rowShape, *rowScale;
    QLabel *labelSummary;
    QPushButton *buttonOk;
};

#endif // FORMDPCONDITIONS_H
