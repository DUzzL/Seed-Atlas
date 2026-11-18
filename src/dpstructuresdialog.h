#ifndef DPSTRUCTURESDIALOG_H
#define DPSTRUCTURESDIALOG_H

// The structures of a datapack world, to pick which ones the map shows:
// grouped by namespace, searchable, with the structures the app cannot place
// (types of mods) listed but switched off, with the reason. Vanilla
// structures that have an entry in the sidebar (villages, ...) follow that
// entry; ticking one of them ticks the entry.

#include <QDialog>

#include <memory>

class MainWindow;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
struct DatapackWorld;
struct DpStructureCatalog;

class DpStructuresDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DpStructuresDialog(MainWindow *parent);

    // the world or the shown structures changed
    void refresh();
    // for tests: ticks the structures with these ids (others unchanged)
    void setShown(const QStringList& ids, bool show);
    // shows or hides every structure that can be shown
    void showAll(bool show);
    QTreeWidget *tree() const { return treeStructures; }

private:
    void rebuild();
    void syncChecks();
    void applyFilter();
    void onItemChanged(QTreeWidgetItem *item, int column);
    void apply(const QStringList& add, const QStringList& remove, const QList<int>& optsOn,
        const QList<int>& optsOff);

    MainWindow *parent;
    std::shared_ptr<const DatapackWorld> shownWorld;
    std::shared_ptr<const DpStructureCatalog> catalog;
    QLabel *labelInfo;
    QLineEdit *lineSearch;
    QTreeWidget *treeStructures;
    QLabel *labelCount;
    bool updating = false;
};

#endif // DPSTRUCTURESDIALOG_H
