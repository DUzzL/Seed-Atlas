#include "formconditions.h"
#include "config.h"
#include "ui_formconditions.h"

#include "aboutdialog.h"
#include "conditiondialog.h"
#include "mainwindow.h"
#include "message.h"
#include "util.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextStream>

#include <cstring>
#include <iterator>


QDataStream& operator<<(QDataStream& out, const Condition& v)
{
    out.writeRawData((const char*)&v, sizeof(Condition));
    return out;
}

QDataStream& operator>>(QDataStream& in, Condition& v)
{
    in.readRawData((char*)&v, sizeof(Condition));
    return in;
}


ItemDelegate::ItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

ItemDelegate::~ItemDelegate()
{
}

void ItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();

    // the tab separated columns of Condition::summary(true): name, count,
    // reference and area, with widths in units of a digit-wide character
    static const int widths[] = { 26, 4, 6 };
    const int unit = txtWidth(option.fontMetrics);
    const QRect rect = opt.rect;

    const QStringList txts = index.data(Qt::DisplayRole).toString().split('\t');
    int x = rect.left();
    for (int i = 0, n = txts.size(); i < n && x <= rect.right(); i++)
    {
        int w = rect.right() + 1 - x;
        if (i < n-1 && i < (int) std::size(widths))
            w = std::min(w, widths[i] * unit);
        opt.rect = QRect(x, rect.top(), w, rect.height());
        opt.text = txts[i];
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        x += w;
    }
}

QSize ItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{   // fonts with a large line gap, such as Calibri, need their full line height
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(size.height(), option.fontMetrics.lineSpacing() + 2));
    return size;
}

// Reads the condition through a const reference: the rvalue overload of
// qvariant_cast trips a false -Warray-bounds positive in GCC for large types.
static Condition itemCondition(const QListWidgetItem *item)
{
    const QVariant data = item->data(Qt::UserRole);
    return qvariant_cast<Condition>(data);
}

FormConditions::FormConditions(QWidget *parent)
    : QWidget(parent)
    , parent(dynamic_cast<MainWindow*>(parent))
    , ui(new Ui::FormConditions)
{
    ui->setupUi(this);

    ui->listConditions->setItemDelegate(new ItemDelegate(this));
    // room for a few filters below the filter controls
    ui->listConditions->setMinimumHeight(ui->listConditions->fontMetrics().height() * 6);

    if (!this->parent)
    {
        QLayoutItem *item;
        while ((item = ui->layoutButtons->takeAt(0)))
        {
            delete item->widget();
            delete item;
        }
    }
    else
    {   // the dropdown row adds common filters; "Advanced filter..." opens
        // the full editor and "Saved filters" loads or stores whole lists
        setupQuickFilters();
        ui->layoutButtons->insertStretch(0);
    }

#if WASM
    ui->listConditions->setDragDropMode(QAbstractItemView::NoDragDrop);
#endif

    qRegisterMetaType< Condition >("Condition");
}

FormConditions::~FormConditions()
{
    delete ui;
}

std::vector<Condition> FormConditions::getConditions() const
{
    std::vector<Condition> conds;

    for (int i = 0, ie = ui->listConditions->count(); i < ie; i++)
    {
        Condition c = itemCondition(ui->listConditions->item(i));
        conds.push_back(c);
    }
    return conds;
}

void FormConditions::updateSensitivity()
{
    if (!parent)
        return;
    QList<QListWidgetItem*> selected = ui->listConditions->selectedItems();

    if (selected.size() == 0)
    {
        ui->buttonRemove->setEnabled(false);
        ui->buttonEdit->setEnabled(false);
    }
    else if (selected.size() == 1)
    {
        ui->buttonRemove->setEnabled(true);
        ui->buttonEdit->setEnabled(true);
    }
    else
    {
        ui->buttonRemove->setEnabled(true);
        ui->buttonEdit->setEnabled(false);
    }

    std::vector<Condition> selcond;
    int disabled = 0;
    for (int i = 0; i < selected.size(); i++)
    {
        Condition c = itemCondition(selected[i]);
        selcond.push_back(c);
        if (c.meta & Condition::DISABLED)
            disabled++;
    }
    if (disabled == 0)
        ui->buttonDisable->setText(tr("Disable"));
    else if (disabled == selected.size())
        ui->buttonDisable->setText(tr("Enable"));
    else
        ui->buttonDisable->setText(tr("Toggle"));
    ui->buttonDisable->setEnabled(!selected.empty());

    emit selectionUpdate(selcond);
}


int FormConditions::getIndex(int idx) const
{
    const std::vector<Condition> condvec = getConditions();
    int cnt[100] = {};
    for (const Condition& c : condvec)
        if (c.save >= 0 && c.save < 100)
            cnt[c.save]++;
        else return 0;
    if (idx <= 0 || idx >= 100)
        idx = 1;
    if (cnt[idx] == 0)
        return idx;
    for (int i = 1; i < 100; i++)
        if (cnt[i] == 0)
            return i;
    return 0;
}

void FormConditions::clearSelection()
{
    ui->listConditions->clearSelection();
}

QListWidgetItem *FormConditions::lockItem(QListWidgetItem *item)
{
    QListWidgetItem *edit = item->clone();
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    item->setSelected(false);
    item->setBackground(QColor(128, 128, 128, 192));
    edit->setData(Qt::UserRole+1, (qulonglong)item);
    return edit;
}

bool list_contains(QListWidget *list, QListWidgetItem *item)
{
    if (!item)
        return false;
    int n = list->count();
    for (int i = 0; i < n; i++)
        if (list->item(i) == item)
            return true;
    return false;
}

void FormConditions::setItemCondition(QListWidget *list, QListWidgetItem *item, Condition *cond)
{
    QListWidgetItem *target = (QListWidgetItem*) item->data(Qt::UserRole+1).toULongLong();
    if (list_contains(list, target) && !(target->flags() & Qt::ItemIsSelectable))
    {
        item->setData(Qt::UserRole+1, (qulonglong)0);
        *target = *item;
        delete item;
        item = target;
    }
    else
    {
        list->addItem(item);
        cond->save = getIndex(cond->save);
    }

    QString s = cond->summary(true);

    const FilterInfo& ft = g_filterinfo.list[cond->type];
    if (ft.cat == CAT_QUAD)
        item->setBackground(QColor(255, 255, 0, 80));

    item->setText(s);
    item->setData(Qt::UserRole, QVariant::fromValue(*cond));
}


void FormConditions::editCondition(QListWidgetItem *item)
{
    if (!(item->flags() & Qt::ItemIsSelectable) || parent == NULL)
        return;
    WorldInfo wi;
    parent->getSeed(&wi);
    ConditionDialog *dialog = new ConditionDialog(this, parent->getMapView(), &parent->config, wi, item, (Condition*)item->data(Qt::UserRole).data());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &ConditionDialog::setCond, this, &FormConditions::addItemCondition, Qt::QueuedConnection);
    dialog->show();
}


static void remove_selected(QListWidget *list)
{
    const QList<QListWidgetItem*> selected = list->selectedItems();
    for (QListWidgetItem *item : selected)
    {
        delete list->takeItem(list->row(item));
    }
}

void FormConditions::on_buttonRemoveAll_clicked()
{
    if (ui->listConditions->count() == 0)
        return;
    if (QMessageBox::question(this, tr("Remove All"), tr("Remove all conditions?"))
            != QMessageBox::Yes)
        return;
    clearConditions();
}

void FormConditions::clearConditions()
{
    ui->listConditions->clear();
    emit changed();
}

void FormConditions::on_buttonRemove_clicked()
{
    remove_selected(ui->listConditions);
    emit changed();
}

void FormConditions::on_buttonDisable_clicked()
{
    const QList<QListWidgetItem*> selected = ui->listConditions->selectedItems();
    for (QListWidgetItem *item : selected)
    {
        Condition c = itemCondition(item);
        c.meta ^= Condition::DISABLED;
        item->setText(c.summary(true));
        item->setData(Qt::UserRole, QVariant::fromValue(c));
    }
    updateSensitivity();
    // listeners read the conditions, so notify after the toggle
    emit changed();
}

void FormConditions::on_buttonEdit_clicked()
{
    QListWidgetItem* edit = 0;
    QList<QListWidgetItem*> selected = ui->listConditions->selectedItems();

    if (!selected.empty())
        edit = lockItem(selected.first());

    if (edit)
        editCondition(edit);
}

void FormConditions::openConditionEditor()
{
    if (!parent)
        return;
    WorldInfo wi;
    parent->getSeed(&wi);
    ConditionDialog *dialog = new ConditionDialog(this, parent->getMapView(), &parent->config, wi);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &ConditionDialog::setCond, this,
        [this](QListWidgetItem *item, Condition cond, int) { addItemCondition(item, cond); },
        Qt::QueuedConnection);
    dialog->show();
}

void FormConditions::on_listConditions_customContextMenuRequested(const QPoint &pos)
{
    QMenu *menu = new QMenu(this);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    // this is a contextual temporary menu so shortcuts are only indicated here,
    // but will not function - see keyReleaseEvent() for shortcut implementation

    int n = ui->listConditions->selectedItems().size();

    if (parent)
    {
        QAction *actadd = menu->addAction(QIcon::fromTheme("list-add"),
            tr("Advanced filter..."), QKeySequence::New, this,
            &FormConditions::openConditionEditor);
        actadd->setEnabled(true);

        QAction *actedit = menu->addAction(QIcon(),
            tr("Edit condition"), QKeySequence::Open, this,
            &FormConditions::conditionsEdit);
        actedit->setEnabled(n == 1);

        QAction *actcut = menu->addAction(QIcon::fromTheme("edit-cut"),
            tr("Cut %n condition(s)", "", n), QKeySequence::Cut, this,
            &FormConditions::conditionsCut);
        actcut->setEnabled(n > 0);

        QAction *actcopy = menu->addAction(QIcon::fromTheme("edit-copy"),
            tr("Copy %n condition(s)", "", n), QKeySequence::Copy, this,
            &FormConditions::conditionsCopy);
        actcopy->setEnabled(n > 0);

        int pn = conditionsPaste(true);
        QAction *actpaste = menu->addAction(QIcon::fromTheme("edit-paste"),
            tr("Paste %n condition(s)", "", pn), QKeySequence::Paste, this,
            &FormConditions::conditionsPaste);
        actpaste->setEnabled(pn > 0);

        QAction *actdel = menu->addAction(QIcon::fromTheme("edit-delete"),
            tr("Remove %n condition(s)", "", n), QKeySequence::Delete, this,
            &FormConditions::conditionsDelete);
        actdel->setEnabled(n > 0);
    }
    else
    {
        QAction *actcopy = menu->addAction(QIcon::fromTheme("edit-copy"),
            tr("Copy %n condition(s)", "", n), QKeySequence::Copy, this,
            &FormConditions::conditionsCopy);
        actcopy->setEnabled(n > 0);
    }

    menu->popup(ui->listConditions->mapToGlobal(pos));
}

void FormConditions::on_listConditions_itemDoubleClicked(QListWidgetItem *item)
{
    if (!parent)
        return;
    editCondition(lockItem(item));
}

void FormConditions::on_listConditions_itemSelectionChanged()
{
    updateSensitivity();
}

// ---------------------------------------------------------------------------
// Quick filters: common structures and biomes near the world center, added
// from the dropdown row above the list.

namespace {

struct QuickGoal
{
    int type;           // filter type (F_VILLAGE, F_BIOME, ...)
    int biome;          // biome id for F_BIOME, otherwise -1
    const char *label;
    int radius;         // default distance in blocks
};

const QuickGoal g_quickGoals[] = {
    { F_VILLAGE,       -1, QT_TRANSLATE_NOOP("FormConditions", "Village"), 500 },
    { F_PORTAL,        -1, QT_TRANSLATE_NOOP("FormConditions", "Ruined portal"), 250 },
    { F_SHIPWRECK,     -1, QT_TRANSLATE_NOOP("FormConditions", "Shipwreck"), 500 },
    { F_TREASURE,      -1, QT_TRANSLATE_NOOP("FormConditions", "Buried treasure"), 500 },
    { F_OUTPOST,       -1, QT_TRANSLATE_NOOP("FormConditions", "Pillager outpost"), 1000 },
    { F_DESERT,        -1, QT_TRANSLATE_NOOP("FormConditions", "Desert pyramid"), 1000 },
    { F_JUNGLE,        -1, QT_TRANSLATE_NOOP("FormConditions", "Jungle temple"), 1000 },
    { F_HUT,           -1, QT_TRANSLATE_NOOP("FormConditions", "Swamp hut"), 1000 },
    { F_IGLOO,         -1, QT_TRANSLATE_NOOP("FormConditions", "Igloo"), 1000 },
    { F_MONUMENT,      -1, QT_TRANSLATE_NOOP("FormConditions", "Ocean monument"), 1000 },
    { F_TRAILS,        -1, QT_TRANSLATE_NOOP("FormConditions", "Trail ruins"), 1000 },
    { F_CHAMBERS,      -1, QT_TRANSLATE_NOOP("FormConditions", "Trial chambers"), 1000 },
    { F_ANCIENT_CITY,  -1, QT_TRANSLATE_NOOP("FormConditions", "Ancient city"), 1500 },
    { F_STRONGHOLD,    -1, QT_TRANSLATE_NOOP("FormConditions", "Stronghold"), 1500 },
    { F_MANSION,       -1, QT_TRANSLATE_NOOP("FormConditions", "Woodland mansion"), 5000 },
    { F_BIOME, mushroom_fields, QT_TRANSLATE_NOOP("FormConditions", "Mushroom fields"), 1500 },
    { F_BIOME, cherry_grove,    QT_TRANSLATE_NOOP("FormConditions", "Cherry grove"), 1000 },
    { F_BIOME, pale_garden,     QT_TRANSLATE_NOOP("FormConditions", "Pale garden"), 1000 },
    { F_BIOME, flower_forest,   QT_TRANSLATE_NOOP("FormConditions", "Flower forest"), 1000 },
    { F_BIOME, jungle,          QT_TRANSLATE_NOOP("FormConditions", "Jungle"), 1000 },
    { F_BIOME, badlands,        QT_TRANSLATE_NOOP("FormConditions", "Badlands"), 1500 },
    { F_BIOME, ice_spikes,      QT_TRANSLATE_NOOP("FormConditions", "Ice spikes"), 1500 },
};

const int g_quickDistances[] = { 100, 250, 500, 1000, 1500, 2500, 5000, 10000 };

bool quickGoalAvailable(const QuickGoal& g, int mc)
{
    const FilterInfo& ft = g_filterinfo.list[g.type];
    if (mc < ft.mcmin || mc > ft.mcmax)
        return false;
    return g.biome < 0 || isOverworld(mc, g.biome);
}

Condition quickCondition(const QuickGoal& g, int radius)
{
    Condition c;
    memset(&c, 0, sizeof(c));
    c.version = Condition::VER_CURRENT;
    c.type = g.type;
    c.relative = 0; // world origin
    c.count = 1;
    c.save = 0;     // the list assigns the next free id
    QByteArray text = QApplication::translate("FormConditions", g.label)
        .toUtf8().left(sizeof(c.text) - 1);
    memcpy(c.text, text.constData(), text.size());
    if (g.biome < 0)
    {   // structures are located within a radius
        c.rmax = radius + 1;
    }
    else
    {   // biomes are checked in a square, sampled at 1:16 for speed
        c.x1 = c.z1 = -radius;
        c.x2 = c.z2 = radius - 1;
        c.y = 256;
        c.step = 16;
        if (g.biome < 128)
            c.biomeToFind = 1ULL << g.biome;
        else
            c.biomeToFindM = 1ULL << (g.biome - 128);
        c.converage = 0.5f;
        c.confidence = 0.95f;
    }
    return c;
}

// Saved filters are files with the conditions plus a title and description:
// the user's own in the settings folder, the examples in the resources.
struct SavedFilter
{
    std::vector<Condition> condvec;
    QString title;
    QString desc;
};

bool readSavedFilter(SavedFilter& filter, const QString& rc)
{
    QFile file(rc);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QTextStream stream(&file);
    while (stream.status() == QTextStream::Ok && !stream.atEnd())
    {
        QString line = stream.readLine();
        if (line.startsWith("#Cond:"))
        {
            Condition c;
            if (c.readHex(line.mid(6).trimmed()))
                filter.condvec.push_back(c);
        }
        else if (line.startsWith("#Title:"))
            filter.title = line.mid(7).trimmed().replace("\\n", "\n");
        else if (line.startsWith("#Desc:"))
            filter.desc = line.mid(6).trimmed().replace("\\n", "\n");
    }
    return true;
}

bool writeSavedFilter(const SavedFilter& filter, const QString& rc)
{
    QFile file(rc);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QTextStream stream(&file);
    stream << "#Version:  " << VERS_MAJOR << "." << VERS_MINOR << "." << VERS_PATCH << "\n";
    stream << "#Title:    " << QString(filter.title).replace("\n", "\\n") << "\n";
    stream << "#Desc:     " << QString(filter.desc).replace("\n", "\\n") << "\n";
    for (const Condition &c : filter.condvec)
        stream << "#Cond: " << c.toHex() << "\n";
    return true;
}

struct Example
{
    const char *rc;
    const char *title;
    const char *desc;
    int mcmin;
};

const Example g_examples[] = {
    { ":/examples/quadhut_stronghold_mushroom.txt",
      QT_TRANSLATE_NOOP("FormConditions", "Technical Base (1.4+)"),
      QT_TRANSLATE_NOOP("FormConditions", "An ideal Quad-hut next to a Stronghold with a Mushroom Island close by."),
      MC_1_4 },
    { ":/examples/two_zombie_villages.txt",
      QT_TRANSLATE_NOOP("FormConditions", "Abandoned Villages in separate biomes (1.10+)"),
      QT_TRANSLATE_NOOP("FormConditions", "Two abandoned Villages close together, one in a Plains, the other in a Desert. "
          "Works best with the 48-bit family search."),
      MC_1_10 },
    { ":/examples/all_fish.txt",
      QT_TRANSLATE_NOOP("FormConditions", "All the Fish (1.13+)"),
      QT_TRANSLATE_NOOP("FormConditions", "A River bordering a Lukewarm Ocean somewhere within 2000 blocks, "
          "a combination where all fish variants can spawn."),
      MC_1_13 },
    { ":/examples/portal_village_or_treasure.txt",
      QT_TRANSLATE_NOOP("FormConditions", "Village or Treasure with Portal (1.16+)"),
      QT_TRANSLATE_NOOP("FormConditions", "Spawn at a Ruined Portal right beside a Village or Buried Treasure."),
      MC_1_16_1 },
    { ":/examples/village_portal_stronghold.txt",
      QT_TRANSLATE_NOOP("FormConditions", "Speedrunner Village (1.16+)"),
      QT_TRANSLATE_NOOP("FormConditions", "Spawn in a Village with a Ruined Portal leading to a Stronghold. "
          "Works best with the 48-bit family search."),
      MC_1_16_1 },
    { ":/examples/biome_diversity_1_18.txt",
      QT_TRANSLATE_NOOP("FormConditions", "Biome Diversity (1.18+)"),
      QT_TRANSLATE_NOOP("FormConditions", "A wide range of climates near the origin (does not look for any particular biomes)."),
      MC_1_18 },
    { ":/examples/huge_jungle_1_18.txt",
      QT_TRANSLATE_NOOP("FormConditions", "Large Jungle (1.18+)"),
      QT_TRANSLATE_NOOP("FormConditions", "A large Jungle biome at the origin: a climate that primarily supports Jungle variants."),
      MC_1_18 },
    { ":/examples/sinkhole.txt",
      QT_TRANSLATE_NOOP("FormConditions", "Likely sinkhole (1.18+)"),
      QT_TRANSLATE_NOOP("FormConditions", "Extreme climate weirdness that can generate holes to the world floor."),
      MC_1_18 },
};

// the style sheet draws menu sections as plain lines, so headings are
// disabled entries with a bold font
void addMenuHeading(QMenu *menu, const QString& text)
{
    menu->addSeparator();
    QAction *heading = menu->addAction(text);
    QFont font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    heading->setEnabled(false);
}

QString savedFilterFolder()
{
    return appConfigDir();
}

} // namespace

void FormConditions::setupQuickFilters()
{
    quickKind = new QComboBox(this);
    quickKind->addItem(tr("Structure"), false);
    quickKind->addItem(tr("Biome"), true);
    quickWhat = new QComboBox(this);
    // the goal names give way first; the distances are always shown in full
    quickWhat->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    quickWhat->setMinimumContentsLength(10);
    quickDistance = new QComboBox(this);
    quickDistance->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    QPushButton *add = new QPushButton(QIcon::fromTheme("list-add"), tr("Add"), this);
    add->setToolTip(tr("Add this filter to the list"));
    quickKind->setToolTip(tr("Look for a structure or a biome"));
    quickDistance->setToolTip(tr("How far from the world center (0, 0), the spawn point is usually close to it.\n"
        "Structures: the distance to the center. Biomes: ± blocks along x and z, a square area."));

    QPushButton *advanced = new QPushButton(tr("Advanced filter..."), this);
    advanced->setToolTip(tr("All filter types and options, e.g. positions relative to other filters"));
    QPushButton *saved = new QPushButton(tr("Saved filters"), this);
    saved->setToolTip(tr("Load a saved or example filter, or save the current one"));
    savedMenu = new QMenu(saved);
    savedMenu->setToolTipsVisible(true);
    savedMenu->installEventFilter(this); // right-click renames or deletes
    saved->setMenu(savedMenu);
    savedButton = saved;
    connect(savedMenu, &QMenu::aboutToShow, this, &FormConditions::fillSavedMenu);

    QHBoxLayout *row = new QHBoxLayout;
    for (QWidget *w : std::initializer_list<QWidget *>{quickKind, quickWhat, quickDistance, add})
    {   // one height for the whole row, so the texts share one baseline even
        // when the icons of quickWhat are taller than the font
        w->setSizePolicy(w == quickWhat ? QSizePolicy::Expanding : QSizePolicy::Minimum,
            QSizePolicy::Preferred);
        row->addWidget(w, w == quickWhat);
    }
    QHBoxLayout *row2 = new QHBoxLayout;
    row2->addWidget(advanced);
    row2->addWidget(saved);
    row2->addStretch(1);
    ui->layoutQuick->addLayout(row);
    ui->layoutQuick->addLayout(row2);

    connect(quickKind, &QComboBox::currentIndexChanged, this, &FormConditions::fillQuickWhat);
    connect(quickWhat, &QComboBox::currentIndexChanged, this, [this]() {
        const int goal = quickWhat->currentData().toInt();
        if (goal < 0)
            return;
        const int radius = g_quickGoals[goal].radius;
        const QString unit = g_quickGoals[goal].biome < 0 ? tr("within %1 blocks") : tr("within ±%1 blocks");
        const QSignalBlocker blocker(quickDistance);
        const int keep = quickDistance->currentData().toInt();
        quickDistance->clear();
        for (int d : g_quickDistances)
            quickDistance->addItem(unit.arg(d), d);
        // keep a distance the user picked, otherwise use the usual one
        int index = quickDistance->findData(keep > 0 && quickKeepDistance ? keep : radius);
        quickDistance->setCurrentIndex(index >= 0 ? index : 0);
    });
    connect(quickDistance, &QComboBox::activated, this, [this]() { quickKeepDistance = true; });
    connect(add, &QPushButton::clicked, this, [this]() {
        const int goal = quickWhat->currentData().toInt();
        if (goal >= 0)
            addItemCondition(nullptr, quickCondition(g_quickGoals[goal], quickDistance->currentData().toInt()), 1);
    });
    connect(advanced, &QPushButton::clicked, this, &FormConditions::openConditionEditor);
    fillQuickWhat();
}

void FormConditions::updateQuickFilters(int mc)
{
    if (!quickWhat || mc == quickMc)
        return;
    quickMc = mc;
    fillQuickWhat();
}

void FormConditions::fillQuickWhat()
{
    const bool biomes = quickKind->currentData().toBool();
    const int keep = quickWhat->currentData().toInt();
    QSignalBlocker blocker(quickWhat);
    quickWhat->clear();
    for (int i = 0, n = std::size(g_quickGoals); i < n; i++)
    {
        const QuickGoal& g = g_quickGoals[i];
        if ((g.biome >= 0) != biomes || !quickGoalAvailable(g, quickMc))
            continue; // not generated in this Minecraft version
        QIcon icon;
        if (g.biome >= 0)
        {
            QPixmap pix(14, 14);
            pix.fill(QColor(g_biomeColors[g.biome][0], g_biomeColors[g.biome][1], g_biomeColors[g.biome][2]));
            icon = QIcon(pix);
        }
        else if (const char *name = g_filterinfo.list[g.type].icon)
        {
            icon = QIcon(getPix(name, 20));
        }
        quickWhat->addItem(icon, QApplication::translate("FormConditions", g.label), i);
    }
    int index = quickWhat->findData(keep);
    quickWhat->setCurrentIndex(index >= 0 ? index : 0);
    blocker.unblock(); // update the distances for the selection once
    emit quickWhat->currentIndexChanged(quickWhat->currentIndex());
}

void FormConditions::popupSavedFilters()
{
    if (!savedButton)
        return;
    if (savedButton->isVisible())
        savedButton->showMenu();
    else // e.g. from the menu while another tab is shown
        savedMenu->popup(QCursor::pos());
}

void FormConditions::fillSavedMenu()
{
    savedMenu->clear();
    QAction *save = savedMenu->addAction(QIcon::fromTheme("document-save"),
        tr("Save current filters..."), this, &FormConditions::saveCurrentFilters);
    save->setEnabled(ui->listConditions->count() > 0);

    const auto addEntry = [this](const QString& rc, QString title, QString desc, bool enabled) {
        SavedFilter filter;
        readSavedFilter(filter, rc);
        if (title.isEmpty())
            title = filter.title.isEmpty() ? QFileInfo(rc).baseName() : filter.title;
        if (desc.isEmpty())
            desc = filter.desc;
        QString rows;
        for (const Condition& c : filter.condvec)
        {   // a table keeps the columns aligned in the proportional UI font
            rows += "<tr>";
            for (const QString& cell : c.summary(true).split('\t'))
                rows += "<td style='padding-right:8px'>" + cell.trimmed().toHtmlEscaped() + "</td>";
            rows += "</tr>";
        }
        QAction *action = savedMenu->addAction(title);
        action->setData(rc);
        action->setEnabled(enabled);
        action->setToolTip("<p>" + desc.toHtmlEscaped().replace("\n", "<br>") + "</p><table>"
            + rows + "</table>");
        connect(action, &QAction::triggered, this, [this, rc]() { parent->loadSession(rc, true); });
    };

#if !WASM
    QStringList own;
    QDirIterator it(savedFilterFolder(), {"*.preset"}, QDir::Files);
    while (it.hasNext())
        own.append(it.next());
    own.sort();
    addMenuHeading(savedMenu, tr("My filters"));
    if (own.isEmpty())
        savedMenu->addAction(tr("(none yet)"))->setEnabled(false);
    for (const QString& rc : std::as_const(own))
        addEntry(rc, QString(), QString(), true);
#endif

    addMenuHeading(savedMenu, tr("Examples"));
    for (const Example& e : g_examples)
        addEntry(e.rc, tr(e.title), tr(e.desc), quickMc >= e.mcmin);
}

void FormConditions::saveCurrentFilters()
{
    QString rc;
    int n;
    for (n = 1; n < 1000; n++)
    {
        rc = savedFilterFolder() + "/" + QString::number(n) + ".preset";
        if (!QFile::exists(rc))
            break;
    }
    bool ok;
    SavedFilter filter;
    filter.title = QInputDialog::getText(this, tr("Save Filters"), tr("Name:"),
        QLineEdit::Normal, tr("My filter %1").arg(n), &ok).trimmed();
    if (!ok || filter.title.isEmpty())
        return;
    filter.condvec = getConditions();
    QDir().mkpath(savedFilterFolder());
    if (!writeSavedFilter(filter, rc))
        warn(this, tr("Failed to save the filters to:\n\"%1\"").arg(rc));
}

bool FormConditions::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == savedMenu && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == Qt::RightButton)
    {   // own filters can be renamed or deleted from a context menu
        QAction *action = savedMenu->actionAt(static_cast<QMouseEvent*>(event)->position().toPoint());
        const QString rc = action ? action->data().toString() : QString();
        if (rc.isEmpty() || rc.startsWith(':'))
            return true;
        QMenu menu;
        QAction *rename = menu.addAction(tr("Rename..."));
        QAction *remove = menu.addAction(QIcon::fromTheme("edit-delete"), tr("Delete"));
        QAction *chosen = menu.exec(QCursor::pos());
        savedMenu->close();
        SavedFilter filter;
        readSavedFilter(filter, rc);
        if (chosen == rename)
        {
            bool ok;
            QString title = QInputDialog::getText(this, tr("Rename Filter"), tr("Name:"),
                QLineEdit::Normal, action->text(), &ok).trimmed();
            if (ok && !title.isEmpty())
            {
                filter.title = title;
                writeSavedFilter(filter, rc);
            }
        }
        else if (chosen == remove)
        {
            if (QMessageBox::question(this, tr("Delete Filter"),
                    tr("Delete the saved filter \"%1\"?").arg(action->text())) == QMessageBox::Yes)
                QFile::remove(rc);
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void FormConditions::conditionsEdit()
{
    if (!parent)
        return;
    on_buttonEdit_clicked();
}

void FormConditions::conditionsCut()
{
    if (!parent)
        return;
    conditionsCopy();
    on_buttonRemove_clicked();
}

void FormConditions::conditionsCopy()
{
    QString text;
    const QList<QListWidgetItem*> selected = ui->listConditions->selectedItems();
    for (QListWidgetItem *item : selected)
    {
        Condition c = itemCondition(item);
        text += (text.isEmpty() ? "" : "\n") + c.toHex();
    }
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(text);
}

int FormConditions::conditionsPaste(bool countonly)
{
    if (!parent)
        return 0;
    QClipboard *clipboard = QGuiApplication::clipboard();
    const QStringList slist = clipboard->text().split('\n');
    std::vector<Condition> pasted;
    for (const QString& s : slist)
    {
        Condition c;
        if (c.readHex(s))
            pasted.push_back(c);
    }
    if (countonly)
        return pasted.size();
    for (const Condition& c : pasted)
    {
        // a pasted quad condition only gets a new structure check if the
        // clipboard does not bring its own
        bool hasCheck = false;
        if (g_filterinfo.list[c.type].cat == CAT_QUAD)
        {
            for (const Condition& d : pasted)
                if (d.relative == c.save && d.type == quadStructureType(c.type))
                    hasCheck = true;
        }
        addItemCondition(hasCheck ? new QListWidgetItem() : nullptr, c, 1);
    }
    return pasted.size();
}

void FormConditions::conditionsDelete()
{
    if (!parent)
        return;
    on_buttonRemove_clicked();
}

int FormConditions::ensureQuadDependencies(bool reenable)
{
    std::vector<Condition> conds = getConditions();
    int fixes = 0;

    for (const Condition& c : conds)
    {
        if (c.type < F_QH_IDEAL || c.type > F_QM_90)
            continue; // not a quad condition
        if (c.meta & Condition::DISABLED)
            continue; // a disabled quad does not take part in the search

        bool found = false;
        for (const Condition& d : conds)
        {
            if (d.relative != c.save || d.type != quadStructureType(c.type))
                continue;
            found = true;
            if (reenable && (d.meta & Condition::DISABLED))
            {   // enable the existing check
                for (int i = 0, ie = ui->listConditions->count(); i < ie; i++)
                {
                    QListWidgetItem *it = ui->listConditions->item(i);
                    Condition cd = itemCondition(it);
                    if (cd.save == d.save && (cd.meta & Condition::DISABLED))
                    {
                        cd.meta &= ~Condition::DISABLED;
                        it->setText(cd.summary(true));
                        it->setData(Qt::UserRole, QVariant::fromValue(cd));
                        fixes++;
                        break;
                    }
                }
            }
            break;
        }
        if (found)
            continue;

        // same dependency as created for a new quad condition
        Condition cq;
        memset(&cq, 0, sizeof(cq));
        cq.version = Condition::VER_CURRENT;
        cq.type = quadStructureType(c.type);
        // use 256 to avoid confusion when this restriction is removed
        cq.rmax = 256;
        cq.relative = c.save;
        cq.save = c.save + 1;
        cq.count = 4;
        QListWidgetItem *item = new QListWidgetItem(ui->listConditions, QListWidgetItem::UserType);
        setItemCondition(ui->listConditions, item, &cq);
        fixes++;
    }

    if (fixes)
    {
        updateSensitivity();
        emit changed();
    }
    return fixes;
}

void FormConditions::addItemCondition(QListWidgetItem *item, Condition cond, int modified)
{
    const FilterInfo& ft = g_filterinfo.list[cond.type];

    if (ft.cat != CAT_QUAD)
    {
        if (!item) {
            item = new QListWidgetItem();
            modified = 1;
        }
        setItemCondition(ui->listConditions, item, &cond);
    }
    else if (item)
    {
        // items opened for editing are clones that point back to the entry
        // in the list (see lockItem()).
        bool edited = item->data(Qt::UserRole+1).toULongLong() != 0;
        setItemCondition(ui->listConditions, item, &cond);
        if (edited)
        {   // an existing condition may have been changed into a quad filter,
            // which needs the dependent structure check as well
            ensureQuadDependencies();
        }
    }
    else
    {
        modified = 1;
        item = new QListWidgetItem();
        setItemCondition(ui->listConditions, item, &cond);

        if (cond.type >= F_QH_IDEAL && cond.type <= F_QH_BARELY)
        {
            Condition cq;
            memset(&cq, 0, sizeof(cq));
            cq.version = Condition::VER_CURRENT;
            cq.type = F_HUT;
            //cq.x1 = -128; cq.z1 = -128;
            //cq.x2 = +128; cq.z2 = +128;
            // use 256 to avoid confusion when this restriction is removed
            cq.rmax = 256;
            cq.relative = cond.save;
            cq.save = cond.save+1;
            cq.count = 4;
            QListWidgetItem *item = new QListWidgetItem(ui->listConditions, QListWidgetItem::UserType);
            setItemCondition(ui->listConditions, item, &cq);
        }
        else if (cond.type == F_QM_90 || cond.type == F_QM_95)
        {
            Condition cq;
            memset(&cq, 0, sizeof(cq));
            cq.version = Condition::VER_CURRENT;
            cq.type = F_MONUMENT;
            //cq.x1 = -160; cq.z1 = -160;
            //cq.x2 = +160; cq.z2 = +160;
            // use 256 to avoid confusion when this restriction is removed
            cq.rmax = 256;
            cq.relative = cond.save;
            cq.save = cond.save+1;
            cq.count = 4;
            QListWidgetItem *item = new QListWidgetItem(ui->listConditions, QListWidgetItem::UserType);
            setItemCondition(ui->listConditions, item, &cq);
        }
    }

    if (modified)
        emit changed();
    updateSensitivity();
}

void FormConditions::on_listConditions_indexesMoved(const QModelIndexList &)
{
    emit changed();
}

void FormConditions::keyReleaseEvent(QKeyEvent *event)
{
    if (ui->listConditions->hasFocus())
    {
        if (event->matches(QKeySequence::New))
            openConditionEditor();
        else if (event->matches(QKeySequence::Open))
            conditionsEdit();
        else if (event->matches(QKeySequence::Cut))
            conditionsCut();
        else if (event->matches(QKeySequence::Copy))
            conditionsCopy();
        else if (event->matches(QKeySequence::Paste))
            conditionsPaste();
        else if (event->matches(QKeySequence::Delete))
            conditionsDelete();
    }
    QWidget::keyReleaseEvent(event);
}
