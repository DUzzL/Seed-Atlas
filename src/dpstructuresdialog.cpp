#include "dpstructuresdialog.h"

#include "datapackworld.h"
#include "dpmap.h"
#include "mainwindow.h"
#include "mapview.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace {

enum { IndexRole = Qt::UserRole + 1, NamespaceRole };
enum { C_NAME, C_PACK, C_STATE };

} // namespace

DpStructuresDialog::DpStructuresDialog(MainWindow *parent)
    : QDialog(parent)
    , parent(parent)
{
    setWindowTitle(tr("Pack structures"));
    setObjectName("dpStructuresDialog");
    QVBoxLayout *layout = new QVBoxLayout(this);

    labelInfo = new QLabel(this);
    labelInfo->setObjectName("conditionHint");
    labelInfo->setWordWrap(true);
    layout->addWidget(labelInfo);

    // the two looks of a start on the map
    QWidget *legend = new QWidget(this);
    QHBoxLayout *legendLayout = new QHBoxLayout(legend);
    legendLayout->setContentsMargins(0, 0, 0, 0);
    DpStructureEntry sample;
    sample.id = QStringLiteral("example:sample");
    sample.color = QColor(86, 140, 220).rgb();
    sample.letter = QChar('S');
    const auto chip = [&](bool unknown, const QString& text) {
        QLabel *icon = new QLabel(legend);
        icon->setPixmap(dpStructureIcon(sample, unknown));
        QLabel *label = new QLabel(text, legend);
        label->setObjectName("conditionHint");
        legendLayout->addWidget(icon);
        legendLayout->addWidget(label);
    };
    chip(false, tr("starts here"));
    legendLayout->addSpacing(12);
    chip(true, tr("not decided (e.g. it depends on the terrain)"));
    legendLayout->addStretch(1);
    layout->addWidget(legend);

    lineSearch = new QLineEdit(this);
    lineSearch->setPlaceholderText(tr("Search structures"));
    lineSearch->setClearButtonEnabled(true);
    layout->addWidget(lineSearch);

    treeStructures = new QTreeWidget(this);
    treeStructures->setObjectName("treeDpStructures");
    treeStructures->setColumnCount(3);
    treeStructures->setHeaderLabels({tr("Structure"), tr("Pack"), tr("Status")});
    treeStructures->setRootIsDecorated(true);
    treeStructures->setUniformRowHeights(true);
    treeStructures->header()->setStretchLastSection(true);
    treeStructures->setMinimumHeight(fontMetrics().height() * 18);
    layout->addWidget(treeStructures, 1);

    labelCount = new QLabel(this);
    labelCount->setObjectName("conditionHint");
    layout->addWidget(labelCount);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *all = box->addButton(tr("Show all"), QDialogButtonBox::ActionRole);
    QPushButton *none = box->addButton(tr("Hide all"), QDialogButtonBox::ActionRole);
    // changes apply at once: no button is the default one
    for (QPushButton *button : {all, none, box->button(QDialogButtonBox::Close)})
    {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(all, &QPushButton::clicked, this, [this]() { showAll(true); });
    connect(none, &QPushButton::clicked, this, [this]() { showAll(false); });
    connect(lineSearch, &QLineEdit::textChanged, this, &DpStructuresDialog::applyFilter);
    connect(treeStructures, &QTreeWidget::itemChanged, this, &DpStructuresDialog::onItemChanged);

    resize(720, 580);
    refresh();
}

void DpStructuresDialog::refresh()
{
    const std::shared_ptr<const DatapackWorld> world =
        parent->datapacks ? parent->datapacks->current() : nullptr;
    if (world != shownWorld)
    {
        shownWorld = world;
        catalog = dpStructureCatalog(world);
        rebuild();
    }
    syncChecks();
}

void DpStructuresDialog::rebuild()
{
    const QSignalBlocker blocker(treeStructures);
    treeStructures->clear();
    if (!catalog)
    {
        labelInfo->setText(tr("The datapack world is not loaded."));
        labelCount->clear();
        return;
    }
    labelInfo->setText(tr("The structures of the loaded datapacks, by namespace. Vanilla "
        "structures follow their entries in the sidebar (Village, Fortress, ...). The map "
        "shows structures from a zoom of 1:%1 in. Structures of mods (types the app does not "
        "know) are listed but cannot be shown.").arg(int(DP_STRUCT_MAX_SCALE)));
    QFont groupFont = treeStructures->font();
    groupFont.setBold(true);
    const QColor muted = QApplication::palette().color(QPalette::PlaceholderText);
    int total = 0, usable = 0;
    for (const QString& ns : catalog->namespaces)
    {
        QTreeWidgetItem *group = new QTreeWidgetItem(treeStructures);
        group->setData(C_NAME, NamespaceRole, ns);
        group->setFont(C_NAME, groupFont);
        group->setIcon(C_NAME, QIcon(dpNamespaceBadge(ns, 16)));
        QSet<QString> packs;
        int count = 0, supported = 0;
        for (int i = 0; i < (int) catalog->entries.size(); i++)
        {
            const DpStructureEntry& e = catalog->entries[i];
            if (e.ns != ns)
                continue;
            QTreeWidgetItem *item = new QTreeWidgetItem(group);
            item->setData(C_NAME, IndexRole, i);
            item->setText(C_NAME, e.name);
            item->setIcon(C_NAME, QIcon(dpStructureIcon(e, false)));
            item->setText(C_PACK, e.pack);
            QStringList tip = {e.id, tr("type %1").arg(e.type)};
            if (!e.set.isEmpty())
                tip << tr("placed by %1").arg(e.set);
            if (e.supported)
            {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
                if (e.vanillaOpt != D_NONE)
                {
                    item->setText(C_STATE, tr("sidebar: %1").arg(mapopt2display(
                        e.vanillaOpt == D_PORTALN ? D_PORTAL : e.vanillaOpt)));
                    tip << tr("Shown together with the sidebar entry %1.").arg(
                        mapopt2display(e.vanillaOpt == D_PORTALN ? D_PORTAL : e.vanillaOpt));
                }
                supported++;
            }
            else
            {
                item->setFlags(Qt::ItemIsSelectable);
                item->setText(C_STATE, tr("not supported"));
                item->setForeground(C_STATE, muted);
                if (!e.reason.isEmpty())
                    tip << e.reason;
            }
            item->setToolTip(C_NAME, tip.join("\n"));
            item->setToolTip(C_STATE, tip.join("\n"));
            packs.insert(e.pack);
            count++;
        }
        QStringList packList(packs.begin(), packs.end());
        packList.sort();
        group->setText(C_NAME, QStringLiteral("%1 (%2)").arg(ns).arg(count));
        group->setText(C_PACK, packList.join(", "));
        group->setToolTip(C_PACK, packList.join("\n"));
        if (supported < count)
        {
            group->setText(C_STATE, tr("%1 of %2 supported").arg(supported).arg(count));
            group->setForeground(C_STATE, muted);
        }
        // the group's tick is kept by syncChecks() (no automatic tristate:
        // that would put ticks on the structures that cannot be shown)
        group->setFlags(supported ? Qt::ItemIsEnabled | Qt::ItemIsUserCheckable : Qt::ItemIsEnabled);
        // the namespaces of the packs open, Minecraft's own closed
        group->setExpanded(ns != QLatin1String("minecraft") || catalog->namespaces.size() == 1);
        total += count;
        usable += supported;
    }
    labelCount->setText(tr("%1 structures, %2 of them supported.").arg(total).arg(usable));
    // as wide as the longest name (also of the closed groups), within limits
    const QFontMetrics fm(groupFont);
    int nameWidth = 0, packWidth = 0;
    for (QTreeWidgetItemIterator it(treeStructures); *it; ++it)
    {
        const int depth = (*it)->parent() ? 2 : 1;
        nameWidth = std::max(nameWidth, fm.horizontalAdvance((*it)->text(C_NAME)) + depth * 20 + 44);
        packWidth = std::max(packWidth, fm.horizontalAdvance((*it)->text(C_PACK)) + 16);
    }
    treeStructures->setColumnWidth(C_NAME, std::clamp(nameWidth, 200, 280));
    treeStructures->setColumnWidth(C_PACK, std::clamp(packWidth, 100, 190));
    applyFilter();
}

void DpStructuresDialog::syncChecks()
{
    if (!catalog)
        return;
    const std::vector<char> mask = parent->getMapView()->getDatapackStructureMask();
    const QSet<QString> ids = [this]() {
        const QStringList list = parent->getMapView()->getDatapackStructures();
        return QSet<QString>(list.begin(), list.end());
    }();
    updating = true;
    const QSignalBlocker blocker(treeStructures);
    for (int g = 0; g < treeStructures->topLevelItemCount(); g++)
    {
        QTreeWidgetItem *group = treeStructures->topLevelItem(g);
        for (int c = 0; c < group->childCount(); c++)
        {
            QTreeWidgetItem *item = group->child(c);
            const int i = item->data(C_NAME, IndexRole).toInt();
            const DpStructureEntry& e = catalog->entries[(size_t) i];
            if (!e.supported)
                continue;
            bool shown;
            if (i < (int) mask.size())
                shown = mask[(size_t) i];
            else if (e.vanillaOpt != D_NONE)
                shown = parent->getMapView()->getShow(e.vanillaOpt);
            else
                shown = ids.contains(e.id);
            item->setCheckState(C_NAME, shown ? Qt::Checked : Qt::Unchecked);
        }
    }
    // the groups follow their structures
    for (int g = 0; g < treeStructures->topLevelItemCount(); g++)
    {
        QTreeWidgetItem *group = treeStructures->topLevelItem(g);
        if (!(group->flags() & Qt::ItemIsUserCheckable))
            continue;
        int on = 0, n = 0;
        for (int c = 0; c < group->childCount(); c++)
            if (group->child(c)->flags() & Qt::ItemIsUserCheckable)
            {
                n++;
                on += group->child(c)->checkState(C_NAME) == Qt::Checked;
            }
        group->setCheckState(C_NAME, on == 0 ? Qt::Unchecked : on == n ? Qt::Checked
            : Qt::PartiallyChecked);
    }
    updating = false;
}

void DpStructuresDialog::applyFilter()
{
    const QString needle = lineSearch->text().trimmed();
    for (int g = 0; g < treeStructures->topLevelItemCount(); g++)
    {
        QTreeWidgetItem *group = treeStructures->topLevelItem(g);
        const bool groupMatch = needle.isEmpty()
            || group->data(C_NAME, NamespaceRole).toString().contains(needle, Qt::CaseInsensitive)
            || group->text(C_PACK).contains(needle, Qt::CaseInsensitive);
        int visible = 0;
        for (int c = 0; c < group->childCount(); c++)
        {
            QTreeWidgetItem *item = group->child(c);
            const int i = item->data(C_NAME, IndexRole).toInt();
            const DpStructureEntry& e = catalog->entries[(size_t) i];
            const bool show = groupMatch || e.name.contains(needle, Qt::CaseInsensitive)
                || e.id.contains(needle, Qt::CaseInsensitive);
            item->setHidden(!show);
            visible += show;
        }
        group->setHidden(visible == 0);
        if (!needle.isEmpty() && visible)
            group->setExpanded(true);
    }
}

void DpStructuresDialog::apply(const QStringList& add, const QStringList& remove,
    const QList<int>& optsOn, const QList<int>& optsOff)
{
    MapView *map = parent->getMapView();
    QStringList ids = map->getDatapackStructures();
    for (const QString& id : remove)
        ids.removeAll(id);
    for (const QString& id : add)
        if (!ids.contains(id))
            ids.append(id);
    map->setDatapackStructures(ids);
    for (int opt : optsOn)
        if (opt >= 0 && opt < parent->saction.size() && parent->saction[opt])
            parent->saction[opt]->setChecked(true);
    for (int opt : optsOff)
        if (opt >= 0 && opt < parent->saction.size() && parent->saction[opt])
            parent->saction[opt]->setChecked(false);
    syncChecks();
}

void DpStructuresDialog::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (updating || column != C_NAME || !catalog)
        return;
    QStringList add, remove;
    QList<int> on, off;
    const auto take = [&](QTreeWidgetItem *child, bool show) {
        const int i = child->data(C_NAME, IndexRole).toInt();
        const DpStructureEntry& e = catalog->entries[(size_t) i];
        if (!e.supported)
            return;
        if (e.vanillaOpt != D_NONE)
            (show ? on : off).append(e.vanillaOpt == D_PORTALN ? D_PORTAL : e.vanillaOpt);
        else
            (show ? add : remove).append(e.id);
    };
    if (!item->parent())
    {   // a namespace: all of its structures
        const bool show = item->checkState(C_NAME) != Qt::Unchecked;
        for (int c = 0; c < item->childCount(); c++)
            take(item->child(c), show);
    }
    else
        take(item, item->checkState(C_NAME) == Qt::Checked);
    apply(add, remove, on, off);
}

void DpStructuresDialog::showAll(bool show)
{
    if (!catalog)
        return;
    QStringList add, remove;
    QList<int> on, off;
    for (const DpStructureEntry& e : catalog->entries)
    {
        if (!e.supported)
            continue;
        if (e.vanillaOpt != D_NONE)
            (show ? on : off).append(e.vanillaOpt == D_PORTALN ? D_PORTAL : e.vanillaOpt);
        else
            (show ? add : remove).append(e.id);
    }
    apply(add, remove, on, off);
}

void DpStructuresDialog::setShown(const QStringList& ids, bool show)
{
    if (!catalog)
        return;
    QStringList add, remove;
    QList<int> on, off;
    for (const DpStructureEntry& e : catalog->entries)
    {
        if (!e.supported || !ids.contains(e.id))
            continue;
        if (e.vanillaOpt != D_NONE)
            (show ? on : off).append(e.vanillaOpt == D_PORTALN ? D_PORTAL : e.vanillaOpt);
        else
            (show ? add : remove).append(e.id);
    }
    apply(add, remove, on, off);
}
