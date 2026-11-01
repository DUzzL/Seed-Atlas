#ifndef FORMCONDITIONS_H
#define FORMCONDITIONS_H

#include <QWidget>
#include <QListWidgetItem>
#include <QStyledItemDelegate>

#include "search.h"

namespace Ui {
class FormConditions;
}

class MainWindow;

Q_DECLARE_METATYPE(Condition)

class ItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit ItemDelegate(QObject *parent = nullptr);
    virtual ~ItemDelegate();

    virtual void paint(QPainter *painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    virtual QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

class FormConditions : public QWidget
{
    Q_OBJECT

public:
    explicit FormConditions(QWidget *parent = nullptr);
    ~FormConditions();

    std::vector<Condition> getConditions() const;
    void updateSensitivity();
    int getIndex(int idx) const;

    // make sure that every quad condition has its dependent structure check
    // (F_HUT / F_MONUMENT). Returns the number of conditions added or, if
    // reenable is set, also of previously disabled checks that were enabled.
    int ensureQuadDependencies(bool reenable = false);

    void clearSelection();
    void clearConditions();

    QListWidgetItem *lockItem(QListWidgetItem *item);
    void setItemCondition(QListWidget *list, QListWidgetItem *item, Condition *cond);
    void editCondition(QListWidgetItem *item);

signals:
    void changed();
    void selectionUpdate(const std::vector<Condition>& selected);

public slots:
    void on_buttonRemoveAll_clicked();
    void on_buttonRemove_clicked();
    void on_buttonDisable_clicked();
    void on_buttonEdit_clicked();
    void openConditionEditor(); // the full condition editor

    void on_listConditions_customContextMenuRequested(const QPoint &pos);
    void on_listConditions_itemDoubleClicked(QListWidgetItem *item);
    void on_listConditions_itemSelectionChanged();

    void updateQuickFilters(int mc); // offer what generates in this version
    void popupSavedFilters();

    void conditionsEdit();
    void conditionsCut();
    void conditionsCopy();
    int conditionsPaste(bool countonly = false);
    void conditionsDelete();

    void addItemCondition(QListWidgetItem *item, Condition cond, int modified = 1);

private slots:
    void on_listConditions_indexesMoved(const QModelIndexList &indexes);

protected:
    void keyReleaseEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupQuickFilters();
    void fillQuickWhat();
    void fillSavedMenu();
    void saveCurrentFilters();

    MainWindow *parent;
    Ui::FormConditions *ui;
    class QComboBox *quickKind = nullptr, *quickWhat = nullptr, *quickDistance = nullptr;
    bool quickKeepDistance = false; // the user picked a distance
    int quickMc = MC_NEWEST;
    class QPushButton *savedButton = nullptr;
    class QMenu *savedMenu = nullptr;
};

#endif // FORMCONDITIONS_H
