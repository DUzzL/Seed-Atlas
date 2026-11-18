#include "formdpconditions.h"
#include "dpmap.h"

#include "datapackworld.h"
#include "formconditions.h"     // ItemDelegate
#include "mainwindow.h"
#include "message.h"
#include "widgets.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum { ConditionRole = Qt::UserRole + 10, BiomeIdRole = Qt::UserRole + 11 };

const int quickDistances[] = { 100, 250, 500, 1000, 2000, 5000 };

QString nameSpace(const QString& id)
{
    return id.section(':', 0, 0);
}

DpCondition itemCondition(const QListWidgetItem *item)
{
    DpCondition c;
    DpCondition::fromLine(item->data(ConditionRole).toString(), &c);
    return c;
}

} // namespace

QIcon dpSwatchIcon(QRgb color, const QColor& border, bool plainSelected)
{
    QIcon icon;
    for (int scale : {1, 2, 3})
    {
        QPixmap pixmap(14 * scale, 14 * scale);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        p.setPen(QPen(border, 1));
        p.setBrush(QColor(color));
        p.drawRoundedRect(QRectF(0.5, 0.5, 13, 13), 3, 3);
        p.end();
        pixmap.setDevicePixelRatio(scale);
        icon.addPixmap(pixmap);
        if (plainSelected)
            icon.addPixmap(pixmap, QIcon::Selected);
    }
    return icon;
}

std::vector<int> dpBiomeOrder(const DatapackWorld& world)
{
    if (!world.usable())
        return {};
    const std::vector<dp::BiomeInfo>& biomes = world.world->biomes();
    std::vector<int> order = world.dimensionInfo()->biomes;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const QString na = nameSpace(biomes[a].id), nb = nameSpace(biomes[b].id);
        const bool va = na == QLatin1String("minecraft"), vb = nb == QLatin1String("minecraft");
        if (va != vb)
            return vb;
        if (na != nb)
            return na < nb;
        const int byName = QString::localeAwareCompare(world.labels[a], world.labels[b]);
        return byName != 0 ? byName < 0 : biomes[a].id < biomes[b].id;
    });
    return order;
}

void dpFilterGroupedList(QListWidget *list, int idRole, const QString& needle)
{
    QListWidgetItem *heading = nullptr;
    bool any = false;
    for (int i = 0; i < list->count(); i++)
    {
        QListWidgetItem *item = list->item(i);
        const QString id = item->data(idRole).toString();
        if (id.isEmpty())
        {
            if (heading)
                heading->setHidden(!any);
            heading = item;
            any = false;
            continue;
        }
        const bool show = needle.isEmpty() || item->text().contains(needle, Qt::CaseInsensitive)
            || id.contains(needle, Qt::CaseInsensitive);
        item->setHidden(!show);
        any = any || show;
    }
    if (heading)
        heading->setHidden(!any);
}

// ------------------------------------------------------------ the list

FormDpConditions::FormDpConditions(MainWindow *parent)
    : QWidget(parent)
    , parent(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    quickKind = new QComboBox(this);
    quickKind->addItem(tr("Biome nearby"), int(DpCondition::BIOME_IN_AREA));
    quickKind->addItem(tr("Biome at the origin"), int(DpCondition::BIOME_AT));
    quickKind->addItem(tr("Structure nearby"), int(DpCondition::STRUCTURE_IN_AREA));
    quickKind->setToolTip(tr("A biome somewhere near the world origin, the biome right at it, or "
        "a structure near it"));
    quickBiome = new StyledComboBox(this);
    quickBiome->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    quickBiome->setMinimumContentsLength(10);
    quickBiome->setToolTip(tr("A biome of the loaded datapacks"));
    quickDistance = new QComboBox(this);
    quickDistance->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (int d : quickDistances)
        quickDistance->addItem(tr("within %1 blocks").arg(d), d);
    quickDistance->setCurrentIndex(2);
    quickDistance->setToolTip(tr("How far from the world origin (0, 0); players spawn close to it"));
    quickAdd = new QPushButton(tr("+ Add"), this);
    quickAdd->setObjectName("quickAdd");
    quickAdd->setToolTip(tr("Add this condition to the list"));

    QHBoxLayout *row = new QHBoxLayout;
    for (QWidget *w : std::initializer_list<QWidget *>{quickKind, quickBiome, quickDistance, quickAdd})
    {
        w->setSizePolicy(w == quickBiome ? QSizePolicy::Expanding : QSizePolicy::Minimum,
            QSizePolicy::Preferred);
        row->addWidget(w, w == quickBiome);
    }
    layout->addLayout(row);

    QPushButton *more = new QPushButton(tr("More options..."), this);
    more->setObjectName("buttonMoreOptions");
    more->setToolTip(tr("Open the condition editor: several biomes, the exact area, the share "
        "of an area, the height and the sampling"));
    note = new QLabel(this);
    note->setObjectName("dpSearchNote");
    note->setWordWrap(true);
    layout->addWidget(note);

    listConditions = new QListWidget(this);
    listConditions->setObjectName("listDpConditions");
    listConditions->setItemDelegate(new ItemDelegate(this));
    listConditions->setMinimumHeight(listConditions->fontMetrics().height() * 4);
    listConditions->setWordWrap(true);
    listConditions->setResizeMode(QListView::Adjust);
    listConditions->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listConditions->setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEmptyHint(listConditions, tr("No datapack conditions yet. Pick a biome above and press "
        "\"+ Add\", or use \"More options...\"."));
    layout->addWidget(listConditions, 1);

    buttonRemoveAll = new QPushButton(tr("Remove all"), this);
    buttonRemove = new QPushButton(tr("Remove"), this);
    buttonDisable = new QPushButton(tr("Turn off"), this);
    buttonEdit = new QPushButton(tr("Edit..."), this);
    buttonRemoveAll->setToolTip(tr("Remove every condition from the list"));
    buttonRemove->setToolTip(tr("Remove the selected conditions"));
    buttonDisable->setToolTip(tr("Turn the selected conditions off or on without removing them; "
        "the search ignores conditions that are off"));
    buttonEdit->setToolTip(tr("Change the selected condition (or double-click it)"));
    // the editor opens from the start of the row (the vanilla list has a row
    // of its own for it, next to the saved filters)
    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addWidget(more);
    buttons->addStretch(1);
    for (QPushButton *b : {buttonRemoveAll, buttonRemove, buttonDisable, buttonEdit})
        buttons->addWidget(b);
    layout->addLayout(buttons);

    connect(quickKind, &QComboBox::currentIndexChanged, this, &FormDpConditions::fillQuickBiomes);
    connect(quickAdd, &QPushButton::clicked, this, [this]() {
        const QString id = quickBiome->currentData().toString();
        if (id.isEmpty())
            return;
        DpCondition c;
        c.kind = DpCondition::Kind(quickKind->currentData().toInt());
        if (c.isStructure())
            c.structures = QStringList{id};
        else
            c.biomes = QStringList{id};
        c.shape = DpCondition::CIRCLE;
        c.x = c.z = 0;
        c.r = quickDistance->currentData().toInt();
        addCondition(c);
    });
    connect(more, &QPushButton::clicked, this, [this]() { openEditor(); });
    connect(listConditions, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const DpCondition c = itemCondition(item);
        openEditor(&c);
    });
    connect(listConditions, &QListWidget::itemSelectionChanged, this, &FormDpConditions::updateButtons);
    connect(buttonRemoveAll, &QPushButton::clicked, this, [this]() {
        if (!listConditions->count())
            return;
        clearConditions();
        emit changed();
    });
    connect(buttonRemove, &QPushButton::clicked, this, [this]() {
        const QList<QListWidgetItem*> items = listConditions->selectedItems();
        for (QListWidgetItem *item : items)
            delete item;
        if (!items.isEmpty())
            emit changed();
        updateButtons();
    });
    connect(buttonDisable, &QPushButton::clicked, this, [this]() {
        const QList<QListWidgetItem*> items = listConditions->selectedItems();
        bool anyOn = false;
        for (QListWidgetItem *item : items)
            anyOn = anyOn || itemCondition(item).enabled;
        for (QListWidgetItem *item : items)
        {
            DpCondition c = itemCondition(item);
            if (c.kind == DpCondition::UNKNOWN)
                continue; // cannot be tested
            c.enabled = !anyOn;
            updateItem(item, c);
        }
        if (!items.isEmpty())
            emit changed();
        updateButtons();
    });
    connect(buttonEdit, &QPushButton::clicked, this, [this]() {
        const QList<QListWidgetItem*> items = listConditions->selectedItems();
        if (items.size() == 1)
        {
            const DpCondition c = itemCondition(items.front());
            openEditor(&c);
        }
    });

    refreshWorld();
    updateButtons();
}

void FormDpConditions::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fitNote();
}

void FormDpConditions::fitNote()
{
    // the note spans the form (the layout has no margins); its own width
    // follows only after this resize
    const int width = contentsRect().width();
    if (width < 100)
        return;
    // (measured from the text: the label's own height for a width counts
    // the minimum height that is set here)
    const QMargins margins = note->contentsMargins();
    const int height = note->text().isEmpty() ? 0
        : note->fontMetrics().boundingRect(QRect(0, 0, width - margins.left() - margins.right(),
            100000), Qt::TextWordWrap, note->text()).height() + margins.top() + margins.bottom() + 8;
    if (height > 0 && height != note->minimumHeight())
        note->setMinimumHeight(height);
}

std::shared_ptr<const DatapackWorld> FormDpConditions::world() const
{
    return parent && parent->datapacks ? parent->datapacks->current() : nullptr;
}

std::vector<DpCondition> FormDpConditions::getConditions() const
{
    std::vector<DpCondition> v;
    for (int i = 0; i < listConditions->count(); i++)
        v.push_back(itemCondition(listConditions->item(i)));
    return v;
}

void FormDpConditions::setConditions(const std::vector<DpCondition>& conditions)
{
    listConditions->clear();
    for (const DpCondition& c : conditions)
    {
        QListWidgetItem *item = new QListWidgetItem(listConditions);
        updateItem(item, c);
    }
    updateButtons();
}

void FormDpConditions::clearConditions()
{
    listConditions->clear();
    updateButtons();
}

int FormDpConditions::nextId() const
{
    QSet<int> used;
    for (const DpCondition& c : getConditions())
        used.insert(c.id);
    for (int id = 1; id <= 99; id++)
        if (!used.contains(id))
            return id;
    return 0;
}

void FormDpConditions::addCondition(DpCondition c)
{
    for (int i = 0; i < listConditions->count() && c.id; i++)
    {
        QListWidgetItem *item = listConditions->item(i);
        if (itemCondition(item).id == c.id)
        {
            updateItem(item, c);
            emit changed();
            return;
        }
    }
    c.id = nextId();
    if (!c.id)
    {
        warn(this, tr("The list is full (99 conditions)."));
        return;
    }
    QListWidgetItem *item = new QListWidgetItem(listConditions);
    updateItem(item, c);
    updateButtons();
    emit changed();
}

void FormDpConditions::updateItem(QListWidgetItem *item, const DpCondition& c)
{
    const DpBiomeNames names{world()};
    const QString sentence = dpConditionText(c, names);
    item->setData(ConditionRole, c.toLine());
    item->setText(sentence);
    // the colour of the first biome, as the vanilla list shows a biome
    QIcon icon;
    const std::shared_ptr<const DatapackWorld> w = names.world;
    if (c.isStructure() && !c.structures.isEmpty())
    {   // the marker of the first structure, as on the map
        const std::shared_ptr<const DpStructureCatalog> catalog = dpStructureCatalog(w);
        const int i = names.structureIndex(c.structures.front());
        if (catalog && i >= 0 && i < (int) catalog->entries.size())
            icon = QIcon(dpStructureIcon(catalog->entries[(size_t) i], false));
    }
    else if (!c.biomes.isEmpty() && w && w->usable())
    {
        const int i = names.index(c.biomes.front());
        if (i >= 0)
            icon = dpSwatchIcon(w->world->biomes()[i].color,
                QApplication::palette().color(QPalette::Mid));
    }
    item->setIcon(icon);
    item->setData(ItemDelegate::TagRole, c.enabled ? QString("#%1").arg(c.id)
        : tr("#%1 off").arg(c.id));
    item->setData(ItemDelegate::DisabledRole, !c.enabled);
    QStringList missing;
    for (const QString& id : c.isStructure() ? c.structures : c.biomes)
        if (c.isStructure() ? names.structureIndex(id) < 0 : !names.has(id))
            missing.append(id);
    QString tip = sentence;
    if (!c.enabled)
        tip += "\n" + tr("Turned off: the search ignores this condition.");
    if (!missing.isEmpty() && w && w->usable())
        tip += "\n" + tr("Not in the loaded datapacks: %1").arg(missing.join(", "));
    tip += "\n" + tr("Datapack condition #%1 (%2). Double-click to edit.")
        .arg(c.id).arg(dpKindName(c.kind));
    item->setToolTip(tip);
}

void FormDpConditions::updateButtons()
{
    const int selected = (int) listConditions->selectedItems().size();
    buttonRemoveAll->setEnabled(listConditions->count() > 0);
    buttonRemove->setEnabled(selected > 0);
    buttonDisable->setEnabled(selected > 0);
    buttonEdit->setEnabled(selected == 1);
    bool anyOn = false;
    for (QListWidgetItem *item : listConditions->selectedItems())
        anyOn = anyOn || itemCondition(item).enabled;
    buttonDisable->setText(selected > 0 && !anyOn ? tr("Turn on") : tr("Turn off"));
}

void FormDpConditions::refreshWorld()
{
    const std::shared_ptr<const DatapackWorld> w = world();
    if (w && w->usable())
    {
        note->setText(tr("Datapack searches generate every seed in full, without the GPU "
            "prefilter and the shortcuts of vanilla searches: tens to hundreds of seeds per "
            "second, not thousands. Small areas are fastest; structure conditions are "
            "slower than biome conditions."));
    }
    else if (w)
    {
        note->setText(tr("The datapack world is not ready (%1). The conditions are kept; "
            "searching needs a loaded world.").arg(w->shortText()));
    }
    else
    {
        note->setText(QString());
    }
    note->setVisible(!note->text().isEmpty());
    fitNote();
    if (w == shownWorld && quickBiome->count())
        return;
    shownWorld = w;
    fillQuickBiomes();
    for (int i = 0; i < listConditions->count(); i++)
        updateItem(listConditions->item(i), itemCondition(listConditions->item(i)));
}

void FormDpConditions::fillQuickBiomes()
{
    const QString keep = quickBiome->currentData().toString();
    const QSignalBlocker blocker(quickBiome);
    quickBiome->clear();
    const std::shared_ptr<const DatapackWorld> w = world();
    const bool structures = quickKind->currentData().toInt() == DpCondition::STRUCTURE_IN_AREA;
    if (structures)
    {   // the structures that can be searched: those of the packs first
        quickBiome->setToolTip(tr("A structure of the loaded datapacks"));
        const std::shared_ptr<const DpStructureCatalog> catalog = dpStructureCatalog(w);
        if (catalog)
        {
            std::vector<int> order;
            for (int i = 0; i < (int) catalog->entries.size(); i++)
                if (catalog->entries[(size_t) i].supported)
                    order.push_back(i);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const DpStructureEntry& ea = catalog->entries[(size_t) a];
                const DpStructureEntry& eb = catalog->entries[(size_t) b];
                const bool va = ea.ns == QLatin1String("minecraft");
                const bool vb = eb.ns == QLatin1String("minecraft");
                if (va != vb)
                    return vb;
                return ea.label.localeAwareCompare(eb.label) < 0;
            });
            for (int i : order)
            {
                const DpStructureEntry& e = catalog->entries[(size_t) i];
                quickBiome->addItem(QIcon(dpStructureIcon(e, false)), e.label, e.id);
                quickBiome->setItemData(quickBiome->count() - 1, e.id, Qt::ToolTipRole);
            }
        }
    }
    else if (w && w->usable())
    {
        quickBiome->setToolTip(tr("A biome of the loaded datapacks"));
        const QColor border = QApplication::palette().color(QPalette::Mid);
        for (int i : dpBiomeOrder(*w))
        {
            const dp::BiomeInfo& b = w->world->biomes()[i];
            quickBiome->addItem(dpSwatchIcon(b.color, border), w->labels[i], b.id);
            quickBiome->setItemData(quickBiome->count() - 1, b.id, Qt::ToolTipRole);
        }
    }
    const int index = quickBiome->findData(keep);
    quickBiome->setCurrentIndex(index >= 0 ? index : 0);
    updateQuickRow();
}

void FormDpConditions::updateQuickRow()
{
    const bool at = quickKind->currentData().toInt() == DpCondition::BIOME_AT;
    quickDistance->setVisible(!at);
    quickAdd->setEnabled(quickBiome->count() > 0);
}

void FormDpConditions::openEditor(const DpCondition *edit)
{
    DpCondition c;
    if (edit)
        c = *edit;
    else if (!quickBiome->currentData().toString().isEmpty())
    {
        if (quickKind->currentData().toInt() == DpCondition::STRUCTURE_IN_AREA)
        {
            c.kind = DpCondition::STRUCTURE_IN_AREA;
            c.structures = QStringList{quickBiome->currentData().toString()};
        }
        else
            c.biomes = QStringList{quickBiome->currentData().toString()};
    }
    if (c.kind == DpCondition::UNKNOWN)
    {
        warn(this, tr("This condition comes from a newer version of Seed Atlas and cannot be "
            "edited here."));
        return;
    }
    DpConditionDialog *dialog = new DpConditionDialog(this, world(), c);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog]() { addCondition(dialog->condition()); });
    dialog->show();
}

// ------------------------------------------------------------ the editor

DpConditionDialog::DpConditionDialog(QWidget *parent,
        const std::shared_ptr<const DatapackWorld>& world, const DpCondition& condition)
    : QDialog(parent)
    , world(world)
    , base(condition)
{
    setWindowTitle(condition.id ? tr("Datapack condition #%1").arg(condition.id)
        : tr("New datapack condition"));
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *question = new QLabel(tr("What must a seed have?"), this);
    question->setObjectName("conditionQuestion");
    layout->addWidget(question);

    form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);

    comboKind = new QComboBox(this);
    for (DpCondition::Kind k : {DpCondition::BIOME_IN_AREA, DpCondition::BIOME_AT,
            DpCondition::BIOME_SHARE, DpCondition::STRUCTURE_IN_AREA})
        comboKind->addItem(dpKindName(k), int(k));
    comboKind->setObjectName("comboDpKind");
    form->addRow(tr("Type:"), comboKind);
    labelKind = new QLabel(this);
    labelKind->setObjectName("conditionHint");
    labelKind->setWordWrap(true);
    form->addRow(QString(), labelKind);

    // the biomes, searchable, with their colours
    QWidget *biomeBox = new QWidget(this);
    QVBoxLayout *biomeLayout = new QVBoxLayout(biomeBox);
    biomeLayout->setContentsMargins(0, 0, 0, 0);
    lineSearch = new QLineEdit(biomeBox);
    lineSearch->setPlaceholderText(tr("Search biomes"));
    lineSearch->setClearButtonEnabled(true);
    biomeLayout->addWidget(lineSearch);
    listBiomes = new QListWidget(biomeBox);
    listBiomes->setObjectName("listDpBiomes");
    listBiomes->setMinimumHeight(fontMetrics().height() * 11);
    listBiomes->setUniformItemSizes(true);
    biomeLayout->addWidget(listBiomes);
    labelChecked = new QLabel(biomeBox);
    labelChecked->setObjectName("conditionHint");
    labelChecked->setWordWrap(true);
    biomeLayout->addWidget(labelChecked);
    form->addRow(tr("Biomes:"), biomeBox);
    listBox = biomeBox;

    comboMatch = new QComboBox(this);
    form->addRow(tr("Match:"), comboMatch);

    // structures: how many starts, and which ones count
    rowCount = new QWidget(this);
    QHBoxLayout *countLayout = new QHBoxLayout(rowCount);
    countLayout->setContentsMargins(0, 0, 0, 0);
    spinCount = new QSpinBox(rowCount);
    spinCount->setRange(1, 999);
    spinCount->setValue(condition.count);
    spinCount->setPrefix(tr("at least "));
    countLayout->addWidget(spinCount);
    countLayout->addStretch(1);
    form->addRow(tr("Starts:"), rowCount);
    comboStates = new QComboBox(this);
    comboStates->addItem(tr("Only starts that are certain"), false);
    comboStates->addItem(tr("Also starts that are not decided"), true);
    comboStates->setToolTip(tr("Some starts cannot be decided by the app, e.g. where the start "
        "height depends on the terrain or two biomes tie at the start. They show faded with a "
        "\"?\" on the map. Counting them finds more seeds, some of which do not have the "
        "structure in the game."));
    comboStates->setCurrentIndex(condition.unknownCounts ? 1 : 0);
    form->addRow(tr("Count:"), comboStates);

    rowPercent = new QWidget(this);
    QHBoxLayout *percentLayout = new QHBoxLayout(rowPercent);
    percentLayout->setContentsMargins(0, 0, 0, 0);
    spinPercent = new QSpinBox(rowPercent);
    spinPercent->setRange(0, 100);
    spinPercent->setSuffix(" %");
    spinPercent->setValue(condition.percent);
    percentLayout->addWidget(spinPercent);
    percentLayout->addStretch(1);
    form->addRow(tr("Share:"), rowPercent);

    rowShape = new QWidget(this);
    QHBoxLayout *shapeLayout = new QHBoxLayout(rowShape);
    shapeLayout->setContentsMargins(0, 0, 0, 0);
    comboShape = new QComboBox(rowShape);
    comboShape->addItem(tr("Circle around a point"), int(DpCondition::CIRCLE));
    comboShape->addItem(tr("Square around a point"), int(DpCondition::SQUARE));
    comboShape->addItem(tr("Rectangle"), int(DpCondition::RECT));
    shapeLayout->addWidget(comboShape);
    shapeLayout->addStretch(1);
    form->addRow(tr("Area:"), rowShape);

    QIntValidator *coords = new QIntValidator(-30000000, 30000000, this);
    const auto edit = [&](QWidget *owner, int value) {
        QLineEdit *e = new QLineEdit(QString::number(value), owner);
        e->setValidator(coords);
        e->setMaximumWidth(110);
        return e;
    };
    const auto label = [](const QString& text, QWidget *owner) {
        QLabel *l = new QLabel(text, owner);
        l->setObjectName("conditionUnit");
        return l;
    };
    stackWhere = new QStackedWidget(this);
    {   // circle and square: a centre and a radius
        QWidget *page = new QWidget(stackWhere);
        QHBoxLayout *l = new QHBoxLayout(page);
        l->setContentsMargins(0, 0, 0, 0);
        lineX = edit(page, condition.x);
        lineZ = edit(page, condition.z);
        lineR = edit(page, condition.r);
        lineR->setValidator(new QIntValidator(0, 30000000, lineR));
        l->addWidget(label(tr("X"), page));
        l->addWidget(lineX);
        l->addWidget(label(tr("Z"), page));
        l->addWidget(lineZ);
        l->addWidget(label(tr("radius"), page));
        l->addWidget(lineR);
        l->addStretch(1);
        stackWhere->addWidget(page);
    }
    {   // rectangle: two corners
        QWidget *page = new QWidget(stackWhere);
        QGridLayout *l = new QGridLayout(page);
        l->setContentsMargins(0, 0, 0, 0);
        lineX1 = edit(page, condition.x1);
        lineZ1 = edit(page, condition.z1);
        lineX2 = edit(page, condition.x2);
        lineZ2 = edit(page, condition.z2);
        l->addWidget(label(tr("from X"), page), 0, 0);
        l->addWidget(lineX1, 0, 1);
        l->addWidget(label(tr("Z"), page), 0, 2);
        l->addWidget(lineZ1, 0, 3);
        l->addWidget(label(tr("to X"), page), 1, 0);
        l->addWidget(lineX2, 1, 1);
        l->addWidget(label(tr("Z"), page), 1, 2);
        l->addWidget(lineZ2, 1, 3);
        l->setColumnStretch(4, 1);
        stackWhere->addWidget(page);
    }
    {   // one position
        QWidget *page = new QWidget(stackWhere);
        QHBoxLayout *l = new QHBoxLayout(page);
        l->setContentsMargins(0, 0, 0, 0);
        linePosX = edit(page, condition.kind == DpCondition::BIOME_AT ? condition.x : 0);
        linePosZ = edit(page, condition.kind == DpCondition::BIOME_AT ? condition.z : 0);
        l->addWidget(label(tr("X"), page));
        l->addWidget(linePosX);
        l->addWidget(label(tr("Z"), page));
        l->addWidget(linePosZ);
        l->addStretch(1);
        stackWhere->addWidget(page);
    }
    form->addRow(tr("Where:"), stackWhere);

    comboY = new QComboBox(this);
    comboY->setEditable(true);
    comboY->setInsertPolicy(QComboBox::NoInsert);
    {
        const int top = world && world->usable() ? world->maxY() : 319;
        const int low = world && world->usable() ? world->minY() : -64;
        const int sea = world && world->usable() ? world->dimensionInfo()->seaLevel - 1 : 62;
        comboY->addItem(tr("Top (surface biomes)"), DpCondition::Y_TOP);
        comboY->addItem(tr("%1 (sea level)").arg(sea), sea);
        for (int y : {128, 64, 0, -32})
            if (y > low && y < top && y != sea)
                comboY->addItem(QString::number(y), y);
        if (condition.y == DpCondition::Y_TOP)
            comboY->setCurrentIndex(0);
        else
        {
            const int i = comboY->findData(condition.y);
            if (i >= 0)
                comboY->setCurrentIndex(i);
            else
                comboY->setEditText(QString::number(condition.y));
        }
        comboY->setToolTip(tr("The height at which the biomes are taken. Biomes are 3D: "
            "below the surface there can be cave biomes. \"Top\" gives the surface biomes, "
            "as the map does at its top level. This dimension reaches from Y %1 to %2.")
            .arg(low).arg(top));
    }
    form->addRow(tr("Height (Y):"), comboY);

    rowScale = new QWidget(this);
    QHBoxLayout *scaleLayout = new QHBoxLayout(rowScale);
    scaleLayout->setContentsMargins(0, 0, 0, 0);
    comboScale = new QComboBox(rowScale);
    comboScale->addItem(tr("Every biome cell (1:4, exact)"), 4);
    comboScale->addItem(tr("Every 16 blocks (1:16)"), 16);
    comboScale->addItem(tr("Every 64 blocks (1:64)"), 64);
    comboScale->setToolTip(tr("How densely the area is sampled. Coarser sampling tests fewer "
        "points per seed, which is faster for large areas, but it can miss small patches "
        "of a biome."));
    comboScale->setCurrentIndex(std::max(0, comboScale->findData(condition.scale)));
    scaleLayout->addWidget(comboScale);
    scaleLayout->addStretch(1);
    form->addRow(tr("Sampling:"), rowScale);

    labelSummary = new QLabel(this);
    labelSummary->setObjectName("conditionSummary");
    labelSummary->setWordWrap(true);
    layout->addWidget(labelSummary);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonOk = box->button(QDialogButtonBox::Ok);
    buttonOk->setProperty("primary", true);
    buttonOk->setText(condition.id ? tr("Apply") : tr("Add"));
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    comboKind->setCurrentIndex(std::max(0, comboKind->findData(int(condition.kind))));
    comboShape->setCurrentIndex(std::max(0, comboShape->findData(int(condition.shape))));
    keptBiomes = condition.biomes;
    keptStructures = condition.structures;
    listsStructures = condition.isStructure();
    if (listsStructures)
        fillStructures(keptStructures);
    else
        fillBiomes(keptBiomes);
    updateKind();
    comboMatch->setCurrentIndex(std::max(0, comboMatch->findData(int(condition.match))));

    connect(comboKind, &QComboBox::currentIndexChanged, this, &DpConditionDialog::updateKind);
    connect(comboShape, &QComboBox::currentIndexChanged, this, &DpConditionDialog::updateKind);
    connect(lineSearch, &QLineEdit::textChanged, this, &DpConditionDialog::filterBiomes);
    connect(listBiomes, &QListWidget::itemChanged, this, &DpConditionDialog::updateSummary);
    connect(comboMatch, &QComboBox::currentIndexChanged, this, &DpConditionDialog::updateSummary);
    connect(spinPercent, &QSpinBox::valueChanged, this, &DpConditionDialog::updateSummary);
    connect(spinCount, &QSpinBox::valueChanged, this, &DpConditionDialog::updateSummary);
    connect(comboStates, &QComboBox::currentIndexChanged, this, &DpConditionDialog::updateSummary);
    connect(comboY, &QComboBox::currentTextChanged, this, &DpConditionDialog::updateSummary);
    connect(comboScale, &QComboBox::currentIndexChanged, this, &DpConditionDialog::updateSummary);
    for (QLineEdit *e : {lineX, lineZ, lineR, lineX1, lineZ1, lineX2, lineZ2, linePosX, linePosZ})
        connect(e, &QLineEdit::textChanged, this, &DpConditionDialog::updateSummary);
    updateSummary();
    resize(560, sizeHint().height());
}

void DpConditionDialog::addHeading(const QString& text)
{
    QListWidgetItem *item = new QListWidgetItem(text, listBiomes);
    item->setFlags(Qt::NoItemFlags);
    QFont font = listBiomes->font();
    font.setBold(true);
    item->setFont(font);
    item->setForeground(QApplication::palette().color(QPalette::PlaceholderText));
}

// the ids of the condition that the loaded packs do not have, still checked
void DpConditionDialog::addMissing(const QStringList& checked, const QSet<QString>& listed,
    const QString& tip)
{
    bool first = true;
    for (const QString& id : checked)
    {
        if (listed.contains(id))
            continue;
        if (first)
            addHeading(tr("Not in the loaded datapacks"));
        first = false;
        QListWidgetItem *item = new QListWidgetItem(id, listBiomes);
        item->setData(BiomeIdRole, id);
        item->setToolTip(tip.arg(id));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
}

void DpConditionDialog::fillBiomes(const QStringList& checked)
{
    const QSignalBlocker blocker(listBiomes);
    listBiomes->clear();
    const QColor border = QApplication::palette().color(QPalette::Mid);
    QSet<QString> listed;
    if (world && world->usable())
    {
        QString group;
        for (int i : dpBiomeOrder(*world))
        {
            const dp::BiomeInfo& b = world->world->biomes()[i];
            if (nameSpace(b.id) != group || listBiomes->count() == 0)
            {
                group = nameSpace(b.id);
                addHeading(group);
            }
            QListWidgetItem *item = new QListWidgetItem(dpSwatchIcon(b.color, border),
                world->labels[i], listBiomes);
            item->setData(BiomeIdRole, b.id);
            item->setToolTip(b.id);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setCheckState(checked.contains(b.id) ? Qt::Checked : Qt::Unchecked);
            listed.insert(b.id);
        }
    }
    addMissing(checked, listed, tr("%1 is not a biome of the loaded datapacks; the search leaves "
        "it out."));
}

void DpConditionDialog::fillStructures(const QStringList& checked)
{
    const QSignalBlocker blocker(listBiomes);
    listBiomes->clear();
    QSet<QString> listed;
    const std::shared_ptr<const DpStructureCatalog> catalog = dpStructureCatalog(world);
    if (catalog)
    {
        // the namespaces of the packs first, Minecraft's own last
        QStringList spaces = catalog->namespaces;
        if (spaces.removeAll(QStringLiteral("minecraft")))
            spaces.append(QStringLiteral("minecraft"));
        for (const QString& ns : spaces)
        {
            std::vector<int> order;
            for (int i = 0; i < (int) catalog->entries.size(); i++)
                if (catalog->entries[(size_t) i].ns == ns)
                    order.push_back(i);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                return catalog->entries[(size_t) a].label.localeAwareCompare(
                    catalog->entries[(size_t) b].label) < 0;
            });
            addHeading(ns);
            for (int i : order)
            {
                const DpStructureEntry& e = catalog->entries[(size_t) i];
                QListWidgetItem *item = new QListWidgetItem(QIcon(dpStructureIcon(e, false)),
                    e.label, listBiomes);
                item->setData(BiomeIdRole, e.id);
                if (e.supported)
                {
                    item->setToolTip(QStringLiteral("%1\n%2").arg(e.id, e.pack));
                    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                    item->setCheckState(checked.contains(e.id) ? Qt::Checked : Qt::Unchecked);
                }
                else
                {   // listed, so that it can be found, but not searchable
                    item->setText(tr("%1 (not supported)").arg(e.label));
                    item->setToolTip(QStringLiteral("%1\n%2").arg(e.id, e.reason));
                    item->setFlags(Qt::NoItemFlags);
                }
                listed.insert(e.id);
            }
        }
    }
    addMissing(checked, listed, tr("%1 is not a structure of the loaded datapacks; the search "
        "leaves it out."));
}

void DpConditionDialog::filterBiomes()
{
    dpFilterGroupedList(listBiomes, BiomeIdRole, lineSearch->text().trimmed());
}

QStringList DpConditionDialog::checkedBiomes() const
{
    QStringList ids;
    for (int i = 0; i < listBiomes->count(); i++)
    {
        const QListWidgetItem *item = listBiomes->item(i);
        if (item->checkState() == Qt::Checked && !item->data(BiomeIdRole).toString().isEmpty())
            ids.append(item->data(BiomeIdRole).toString());
    }
    return ids;
}

void DpConditionDialog::updateKind()
{
    const DpCondition::Kind kind = DpCondition::Kind(comboKind->currentData().toInt());
    const int keep = comboMatch->currentData().isValid() ? comboMatch->currentData().toInt() : 0;
    // the list holds biomes or structures; what is ticked is kept for each
    const bool structures = kind == DpCondition::STRUCTURE_IN_AREA;
    if (structures != listsStructures)
    {
        if (listsStructures)
            keptStructures = checkedBiomes();
        else
            keptBiomes = checkedBiomes();
        listsStructures = structures;
        if (structures)
            fillStructures(keptStructures);
        else
            fillBiomes(keptBiomes);
        filterBiomes();
    }
    lineSearch->setPlaceholderText(structures ? tr("Search structures") : tr("Search biomes"));
    if (QLabel *label = qobject_cast<QLabel*>(form->labelForField(listBox)))
        label->setText(structures ? tr("Structures:") : tr("Biomes:"));
    {
        const QSignalBlocker blocker(comboMatch);
        comboMatch->clear();
        switch (kind)
        {
        case DpCondition::BIOME_AT:
            comboMatch->addItem(tr("The biome there is one of them"), int(DpCondition::ANY));
            comboMatch->addItem(tr("The biome there is none of them"), int(DpCondition::NONE));
            labelKind->setText(tr("The biome at one block position, as the game shows it there "
                "(F3 screen)."));
            break;
        case DpCondition::BIOME_SHARE:
            comboMatch->addItem(tr("Together they cover at least the share"), int(DpCondition::ANY));
            comboMatch->addItem(tr("Each of them covers at least the share"), int(DpCondition::ALL));
            comboMatch->addItem(tr("Together they cover at most the share"), int(DpCondition::NONE));
            labelKind->setText(tr("How much of an area the biomes cover, counted in biome cells "
                "(4 × 4 blocks)."));
            break;
        case DpCondition::STRUCTURE_IN_AREA:
            comboMatch->addItem(tr("Together they start often enough"), int(DpCondition::ANY));
            comboMatch->addItem(tr("Each of them starts often enough"), int(DpCondition::ALL));
            comboMatch->addItem(tr("None of them starts (not even undecided)"), int(DpCondition::NONE));
            labelKind->setText(tr("Structures start in an area: their start position (where the "
                "game checks the biome) lies inside it. Structures that a set places in the same "
                "chunk are alternatives and count once."));
            break;
        default:
            comboMatch->addItem(tr("At least one of them occurs"), int(DpCondition::ANY));
            comboMatch->addItem(tr("Each of them occurs"), int(DpCondition::ALL));
            comboMatch->addItem(tr("None of them occurs"), int(DpCondition::NONE));
            labelKind->setText(tr("The biomes occur somewhere in an area, in at least one biome "
                "cell (4 × 4 blocks)."));
            break;
        }
        const int i = comboMatch->findData(keep);
        comboMatch->setCurrentIndex(i >= 0 ? i : 0);
    }
    const bool area = kind != DpCondition::BIOME_AT;
    form->setRowVisible(rowPercent, kind == DpCondition::BIOME_SHARE);
    form->setRowVisible(rowShape, area);
    form->setRowVisible(rowScale, area && !structures);
    form->setRowVisible(comboY, !structures);
    form->setRowVisible(rowCount, structures && comboMatch->currentData().toInt() != DpCondition::NONE);
    form->setRowVisible(comboStates, structures && comboMatch->currentData().toInt() != DpCondition::NONE);
    const int shape = comboShape->currentData().toInt();
    stackWhere->setCurrentIndex(!area ? 2 : shape == DpCondition::RECT ? 1 : 0);
    // the stack is as tall as the page it shows
    for (int i = 0; i < stackWhere->count(); i++)
        stackWhere->widget(i)->setSizePolicy(QSizePolicy::Preferred,
            i == stackWhere->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    stackWhere->adjustSize();
    updateSummary();
}

DpCondition DpConditionDialog::condition() const
{
    DpCondition c = base;
    c.kind = DpCondition::Kind(comboKind->currentData().toInt());
    c.match = DpCondition::Match(comboMatch->currentData().toInt());
    if (listsStructures)
    {
        c.structures = checkedBiomes();
        c.biomes = keptBiomes;
    }
    else
    {
        c.biomes = checkedBiomes();
        c.structures = keptStructures;
    }
    c.count = spinCount->value();
    c.unknownCounts = comboStates->currentData().toBool();
    c.percent = spinPercent->value();
    c.scale = comboScale->currentData().toInt();
    if (c.kind == DpCondition::BIOME_AT)
    {
        c.x = linePosX->text().toInt();
        c.z = linePosZ->text().toInt();
    }
    else
    {
        c.shape = DpCondition::Shape(comboShape->currentData().toInt());
        c.x = lineX->text().toInt();
        c.z = lineZ->text().toInt();
        c.r = std::max(0, lineR->text().toInt());
        c.x1 = lineX1->text().toInt();
        c.z1 = lineZ1->text().toInt();
        c.x2 = lineX2->text().toInt();
        c.z2 = lineZ2->text().toInt();
    }
    const int index = comboY->findText(comboY->currentText());
    if (index >= 0)
        c.y = comboY->itemData(index).toInt();
    else
    {
        bool ok = false;
        const int y = comboY->currentText().section(' ', 0, 0).toInt(&ok);
        c.y = ok ? y : DpCondition::Y_TOP;
    }
    return c;
}

void DpConditionDialog::updateSummary()
{
    const DpCondition c = condition();
    const DpBiomeNames names{world};
    const bool structures = c.isStructure();
    const QStringList& ids = structures ? c.structures : c.biomes;
    QStringList ticked;
    for (const QString& id : ids)
        ticked.append(structures ? names.structureName(id) : names.name(id));
    labelChecked->setText(ids.isEmpty() ? (structures
            ? tr("Tick the structures the condition is about.")
            : tr("Tick the biomes the condition is about."))
        : tr("Ticked: %1").arg(ticked.join(", ")));
    if (structures)
    {   // "none" also rules out the starts that are not decided
        form->setRowVisible(rowCount, c.match != DpCondition::NONE);
        form->setRowVisible(comboStates, c.match != DpCondition::NONE);
    }
    labelSummary->setText(dpConditionText(c, names));
    buttonOk->setEnabled(!ids.isEmpty());
}
