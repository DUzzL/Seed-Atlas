#include "tabdatapacks.h"

#include "aboutdialog.h"
#include "dpcheck.h"
#include "formdpconditions.h"
#include "message.h"
#include "util.h"
#include "vanilladata.h"
#include "widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

enum { BiomeIdRole = Qt::UserRole + 1 };

// The Minecraft versions with datapack worlds as the version picker names
// them, with runs of neighbouring versions joined: "1.19, 1.20 and 1.21.1 to
// 1.21.11". (1.19.2 is one of the versions the picker hides by default.)
QString supportedVersionsText()
{
    QStringList parts;
    int first = MC_UNDEF, last = MC_UNDEF;
    const auto flush = [&]() {
        if (first == MC_UNDEF)
            return;
        if (last - first >= 2)
            parts.append(TabDatapacks::tr("%1 to %2").arg(mc2str(first), mc2str(last)));
        else
            for (int mc = first; mc <= last; mc++)
                parts.append(mc2str(mc));
        first = last = MC_UNDEF;
    };
    for (int mc = MC_UNDEF + 1; mc <= MC_NEWEST; mc++)
    {
        const bool hidden = mc == MC_1_19_2 && !g_extgen.experimentalVers;
        const bool sameMinor = first != MC_UNDEF
            && QString(mc2str(mc)).section('.', 0, 1) == QString(mc2str(first)).section('.', 0, 1)
            && QString(mc2str(first)).count('.') == 2 && QString(mc2str(mc)).count('.') == 2;
        if (dp::versionSupported(mc) && !hidden)
        {
            if (first != MC_UNDEF && !sameMinor)
                flush();
            if (first == MC_UNDEF)
                first = mc;
            last = mc;
        }
        else
        {
            flush();
        }
    }
    flush();
    if (parts.size() <= 1)
        return parts.join(QString());
    const QString tail = parts.takeLast();
    return TabDatapacks::tr("%1 and %2").arg(parts.join(", "), tail);
}

// the newest version with datapack worlds
int newestSupportedVersion()
{
    for (int mc = MC_NEWEST; mc > MC_UNDEF; mc--)
        if (dp::versionSupported(mc))
            return mc;
    return MC_UNDEF;
}

// A sentence a non-developer can read. The generator's own text stays in it,
// as it names what a pack author has to look at.
QString readableIssue(const dp::Issue& issue)
{
    QString text = issue.text;
    if (text.startsWith(QLatin1String("Unknown ")))
    {
        text = TabDatapacks::tr("This pack uses a world generation feature the map does not "
            "support yet: %1").arg(text.left(1).toLower() + text.mid(1));
    }
    return text;
}

// errors first, then warnings, then the rest
std::vector<dp::Issue> sortedIssues(const dp::Report& report)
{
    std::vector<dp::Issue> list = report.issues;
    std::stable_sort(list.begin(), list.end(), [](const dp::Issue& a, const dp::Issue& b) {
        return a.severity > b.severity;
    });
    return list;
}

void setState(QLabel *label, const char *state)
{
    if (label->property("state").toString() == QLatin1String(state))
        return;
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

QToolButton *rowButton(QWidget *parent, const QString& tip)
{
    QToolButton *button = new QToolButton(parent);
    button->setObjectName("packRowButton");
    button->setProperty("keepIconSize", true);
    button->setIconSize(QSize(14, 14));
    button->setFixedSize(24, 24);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    return button;
}

} // namespace

TabDatapacks::TabDatapacks(MainWindow *parent, DatapackManager *manager)
    : QWidget(parent)
    , parent(parent)
    , manager(manager)
{
    setProperty("railIcon", "datapacks");

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setObjectName("datapackScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    QWidget *body = new QWidget(scroll);
    body->setObjectName("datapackBody");
    scroll->setWidget(body);
    QVBoxLayout *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(8);

    QLabel *description = new QLabel(tr("A datapack world is a Minecraft world whose terrain and "
        "biomes come from data packs or mods. Add your packs to see the biome map of such a "
        "world for your seed; everything is computed on this computer."), body);
    description->setObjectName("labelDescription");
    description->setWordWrap(true);
    layout->addWidget(description);

    QHBoxLayout *addRow = new QHBoxLayout;
    buttonAdd = new QPushButton(tr("Add datapack..."), body);
    buttonAdd->setProperty("primary", true);
    buttonAdd->setToolTip(tr("Add a data pack (.zip) or a mod (.jar). You can also drop files "
        "and folders onto the window."));
    buttonAddFolder = new QPushButton(tr("Add folder..."), body);
    buttonAddFolder->setToolTip(tr("Add a data pack that is an unpacked folder (with pack.mcmeta)."));
    addRow->addWidget(buttonAdd, 1);
    addRow->addWidget(buttonAddFolder);
    layout->addLayout(addRow);

    // --- packs ---
    QWidget *packs = new QWidget(body);
    QVBoxLayout *packsLayout = new QVBoxLayout(packs);
    packsLayout->setContentsMargins(0, 0, 0, 0);
    packsLayout->setSpacing(6);
    labelNoPacks = new QLabel(tr("No datapacks yet: the map shows the vanilla world. Add a pack "
        "with the button above, or drop pack files and folders onto the window."), packs);
    labelNoPacks->setObjectName("labelDescription");
    labelNoPacks->setWordWrap(true);
    packsLayout->addWidget(labelNoPacks);
    packRows = new QWidget(packs);
    packLayout = new QVBoxLayout(packRows);
    packLayout->setContentsMargins(0, 0, 0, 0);
    packLayout->setSpacing(6);
    packsLayout->addWidget(packRows);
    sectionPacks = new Collapsible(body);
    // as tall as its rows, never more (a Collapsible grows by default)
    sectionPacks->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    sectionPacks->init(tr("Packs"), packs, false);
    sectionPacks->setInfo(tr("Help: Packs"), tr(
        "<html><head/><body><p>"
        "The packs are loaded from top to bottom, as Minecraft does. When two packs "
        "contain the same file, the pack further down wins: <b>later packs override "
        "earlier ones</b>. Use the arrows to change the order."
        "</p><p>"
        "A pack that is switched off stays in the list but is not loaded. With no pack "
        "switched on, the map shows the vanilla world again and all tools work as usual."
        "</p><p>"
        "Zip files, mod jars and unpacked folders work. Nothing is copied: the packs are "
        "read from where they are, so a pack that is moved or deleted shows up as missing."
        "</p></body></html>"));
    layout->addWidget(sectionPacks);

    // --- world: state, game data, dimension ---
    QWidget *world = new QWidget(body);
    QVBoxLayout *worldLayout = new QVBoxLayout(world);
    worldLayout->setContentsMargins(0, 0, 0, 0);
    worldLayout->setSpacing(6);

    labelState = new QLabel(world);
    labelState->setObjectName("datapackState");
    labelState->setWordWrap(true);
    worldLayout->addWidget(labelState);

    buttonVersion = new QPushButton(world);
    buttonVersion->hide();
    worldLayout->addWidget(buttonVersion, 0, Qt::AlignLeft);

    issues = new QTextBrowser(world);
    issues->setObjectName("datapackIssues");
    issues->setOpenLinks(false);
    issues->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    issues->hide();
    worldLayout->addWidget(issues);

    labelVanilla = new QLabel(world);
    labelVanilla->setObjectName("datapackVanilla");
    labelVanilla->setWordWrap(true);
    labelVanilla->setTextFormat(Qt::RichText);
    connect(labelVanilla, &QLabel::linkActivated, this, &TabDatapacks::chooseVanillaFile);
    worldLayout->addWidget(labelVanilla);

    progress = new QProgressBar(world);
    progress->setProperty("thin", true);
    progress->setTextVisible(false);
    progress->hide();
    worldLayout->addWidget(progress);

    QHBoxLayout *vanillaRow = new QHBoxLayout;
    buttonDownload = new QPushButton(tr("Download from Mojang..."), world);
    buttonDownload->setToolTip(tr("Download the game file of this version from Mojang's "
        "servers. You are asked before Mojang's servers are contacted."));
    buttonChooseFile = new QPushButton(tr("Choose file..."), world);
    buttonChooseFile->setToolTip(tr("Pick the game's own file (<version>.jar from the "
        "\"versions\" folder of Minecraft) or the server file."));
    buttonChooseFolder = new QPushButton(tr("Choose folder..."), world);
    buttonChooseFolder->setToolTip(tr("Pick a folder with the unpacked game data "
        "(it holds data/minecraft/worldgen)."));
    buttonCancel = new QPushButton(tr("Cancel"), world);
    buttonCancel->hide();
    vanillaRow->addWidget(buttonDownload);
    vanillaRow->addWidget(buttonChooseFile);
    vanillaRow->addWidget(buttonChooseFolder);
    vanillaRow->addWidget(buttonCancel);
    vanillaRow->addStretch(1);
    worldLayout->addLayout(vanillaRow);

    dimensionRow = new QWidget(world);
    QHBoxLayout *dimensionLayout = new QHBoxLayout(dimensionRow);
    dimensionLayout->setContentsMargins(0, 2, 0, 0);
    QLabel *dimensionLabel = new QLabel(tr("Dimension"), dimensionRow);
    comboDimension = new StyledComboBox(dimensionRow);
    comboDimension->setObjectName("comboDimension");
    comboDimension->setToolTip(tr("The dimension of the datapack world that the map shows"));
    dimensionLayout->addWidget(dimensionLabel);
    dimensionLayout->addWidget(comboDimension, 1);
    worldLayout->addWidget(dimensionRow);

    labelPreset = new QLabel(world);
    labelPreset->setObjectName("labelDescription");
    labelPreset->setWordWrap(true);
    worldLayout->addWidget(labelPreset);

    QHBoxLayout *reportRow = new QHBoxLayout;
    buttonCopy = new QPushButton(tr("Copy details"), world);
    buttonCopy->setToolTip(tr("Copy the packs, the version and all messages as text, "
        "e.g. to report a problem."));
    buttonReload = new QPushButton(tr("Reload"), world);
    buttonReload->setToolTip(tr("Read the packs again, e.g. after changing a pack file."));
    buttonCheck = new QPushButton(tr("Check packs..."), world);
    buttonCheck->setObjectName("buttonCheckPacks");
    buttonCheck->setToolTip(tr("A report on each pack: its worldgen files, dimensions and biome "
        "sources, what the app does not support (with ids), how long it takes to load and a "
        "short speed sample. Plain text to copy; it names the pack files, not their folders."));
    reportRow->addWidget(buttonCopy);
    reportRow->addWidget(buttonReload);
    reportRow->addWidget(buttonCheck);
    reportRow->addStretch(1);
    worldLayout->addLayout(reportRow);

    sectionWorld = new Collapsible(body);
    sectionWorld->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    sectionWorld->init(tr("World"), world, false);
    sectionWorld->setInfo(tr("Help: Datapack world"), tr(
        "<html><head/><body><p>"
        "The map shows the <b>biomes</b> of the datapack world at the height (Y) set on the "
        "map, with the names and colours of the packs. Slime chunks and the grid work as "
        "usual."
        "</p><p>"
        "Structures of the packs show with the sidebar entries (vanilla structures) and "
        "<i>Pack structures</i>; a faded marker with a \"?\" is a start the app cannot decide "
        "(e.g. its height depends on the terrain). The terrain and height views use the terrain "
        "of the packs (without caves, carvers, trees and structures). The Seed Finder, the "
        "biome and the structure analysis work for datapack worlds; the world spawn, the "
        "density and fortress analyses, Locations and End Portals need a vanilla world."
        "</p><p>"
        "<i>Check packs</i> writes a report about each pack (files, dimensions, what is not "
        "supported, load time and speed), e.g. to report a problem."
        "</p><p>"
        "Packs build on Minecraft's own world generation data, which is not part of Seed "
        "Atlas. It is read from your Minecraft installation, from a file you choose, or "
        "downloaded from Mojang if you agree."
        "</p><p>"
        "Supported are Overworld-like dimensions (noise terrain with a biome list, a single "
        "biome or a checkerboard). Other dimensions of a pack are listed but cannot be shown "
        "yet."
        "</p></body></html>"));
    layout->addWidget(sectionWorld);

    // --- biomes ---
    QWidget *biomes = new QWidget(body);
    QVBoxLayout *biomeLayout = new QVBoxLayout(biomes);
    biomeLayout->setContentsMargins(0, 0, 0, 0);
    biomeLayout->setSpacing(6);
    lineSearch = new QLineEdit(biomes);
    lineSearch->setPlaceholderText(tr("Search biomes"));
    lineSearch->setClearButtonEnabled(true);
    biomeLayout->addWidget(lineSearch);
    listBiomes = new QListWidget(biomes);
    listBiomes->setObjectName("datapackBiomes");
    listBiomes->setSelectionMode(QAbstractItemView::MultiSelection);
    listBiomes->setUniformItemSizes(false);
    listBiomes->setIconSize(QSize(14, 14));
    listBiomes->setMinimumHeight(170);
    listBiomes->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listBiomes->setTextElideMode(Qt::ElideRight);
    listBiomes->setToolTip(tr("Click biomes to highlight them on the map"));
    setEmptyHint(listBiomes, tr("The biomes of the datapack world appear here."));
    biomeLayout->addWidget(listBiomes, 1);
    QHBoxLayout *countRow = new QHBoxLayout;
    labelBiomeCount = new QLabel(biomes);
    labelBiomeCount->setObjectName("labelStatus");
    buttonClear = new QPushButton(tr("Clear highlight"), biomes);
    buttonClear->setEnabled(false);
    countRow->addWidget(labelBiomeCount, 1);
    countRow->addWidget(buttonClear);
    biomeLayout->addLayout(countRow);
    sectionBiomes = new Collapsible(body);
    sectionBiomes->init(tr("Biomes"), biomes, false);
    sectionBiomes->setInfo(tr("Help: Biomes"), tr(
        "<html><head/><body><p>"
        "All biomes that the shown dimension can generate, grouped by where they come "
        "from: the biomes of the packs first, Minecraft's own last."
        "</p><p>"
        "Click one or more biomes to <b>highlight</b> them on the map: everything else "
        "turns grey. Click a biome again to take it out, or use <b>Clear highlight</b>. "
        "The search field narrows the list by name or id."
        "</p></body></html>"));
    // the list takes the height that is left, and may give some up before
    // the whole panel starts to scroll
    sectionBiomes->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(sectionBiomes, 1);
    layout->addStretch(0); // takes the space while the sections are hidden

    connect(buttonAdd, &QPushButton::clicked, this, &TabDatapacks::addFiles);
    connect(buttonAddFolder, &QPushButton::clicked, this, &TabDatapacks::addFolder);
    connect(buttonCopy, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(detailsText());
    });
    connect(buttonReload, &QPushButton::clicked, manager, &DatapackManager::reload);
    connect(buttonCheck, &QPushButton::clicked, this, [this]() {
        if (checkThread)
            checkAbort = true;
        else
            checkPacks();
    });
    connect(buttonVersion, &QPushButton::clicked, this, [this]() {
        this->parent->setVersion(newestSupportedVersion());
    });
    connect(buttonDownload, &QPushButton::clicked, this, &TabDatapacks::downloadVanilla);
    connect(buttonChooseFile, &QPushButton::clicked, this, &TabDatapacks::chooseVanillaFile);
    connect(buttonChooseFolder, &QPushButton::clicked, this, &TabDatapacks::chooseVanillaFolder);
    connect(comboDimension, &QComboBox::activated, this, [this](int index) {
        if (!updating)
            this->manager->setDimension(comboDimension->itemData(index).toString());
    });
    connect(lineSearch, &QLineEdit::textChanged, this, &TabDatapacks::applyBiomeFilter);
    connect(listBiomes, &QListWidget::itemSelectionChanged, this,
        &TabDatapacks::onBiomeSelectionChanged);
    connect(buttonClear, &QPushButton::clicked, listBiomes, &QListWidget::clearSelection);

    VanillaData *vanilla = manager->vanillaData();
    connect(buttonCancel, &QPushButton::clicked, vanilla, &VanillaData::cancel);
    connect(vanilla, &VanillaData::infoReady, this, [this, vanilla](int, qint64, const QString&) {
        // the user agreed before the version list was asked for (downloadVanilla)
        vanilla->startDownload();
        updateVanilla();
    });
    connect(vanilla, &VanillaData::progress, this, [this](int mc, qint64 received, qint64 total) {
        progress->setRange(0, 1000);
        progress->setValue(total > 0 ? int(1000.0 * received / total) : 0);
        labelVanilla->setText(tr("Downloading the game file of Minecraft %1... %2 of %3 MB")
            .arg(VanillaData::gameVersion(mc))
            .arg(QLocale().toString(received / 1048576.0, 'f', 1))
            .arg(QLocale().toString(total / 1048576.0, 'f', 1)));
    });
    connect(vanilla, &VanillaData::finished, this, [this](int, bool ok, const QString& error) {
        vanillaError = ok ? QString() : error;
        updateVanilla();
    });

    connect(manager, &DatapackManager::listChanged, this, &TabDatapacks::refresh);
    connect(manager, &DatapackManager::worldChanged, this, &TabDatapacks::refresh,
        Qt::QueuedConnection);
    refresh();
}

TabDatapacks::~TabDatapacks()
{
    if (checkThread)
    {
        checkAbort = true;
        checkThread->disconnect(this);
        checkThread->wait();
        delete checkThread;
    }
}

void TabDatapacks::checkPacks()
{
    if (checkThread)
        return;
    const std::shared_ptr<const DatapackWorld> world = manager->current();
    if (!world || world->vanillaPath.isEmpty())
    {
        info(this, tr("The packs can be checked once the game data of the version is there "
            "(see Game data above)."));
        return;
    }
    if (!QFileInfo::exists(world->vanillaPath))
    {   // moved or deleted since the world was loaded
        manager->reload();
        info(this, tr("The game data file of the loaded world is gone. The packs are read "
            "again; check them once the game data is there (see Game data above)."));
        return;
    }
    WorldInfo wi;
    parent->getSeed(&wi, false);
    DpCheckInput input;
    input.mc = world->mc;
    input.vanillaPath = world->vanillaPath;
    input.preset = world->preset;
    input.dimension = manager->dimension();
    input.seed = wi.seed;
    for (int i = 0; i < (int) manager->entries().size(); i++)
        if (manager->entries()[i].enabled && manager->packExists(i))
            input.packs.push_back(manager->entries()[i].path);
    if (input.packs.empty())
        return;
    checkAbort = false;
    std::shared_ptr<QString> result = std::make_shared<QString>();
    QPointer<QPushButton> button = buttonCheck;
    checkThread = QThread::create([this, input, result, button]() {
        *result = dpCheckReport(input, &checkAbort, [button](const QString& step) {
            QMetaObject::invokeMethod(qApp, [button, step]() {
                if (button)
                    button->setText(TabDatapacks::tr("Checking %1... (stop)").arg(step));
            }, Qt::QueuedConnection);
        });
    });
    checkThread->setObjectName("pack check");
    checkThread->setStackSize(8u << 20);   // it loads the packs like the loader
    connect(checkThread, &QThread::finished, this, [this, result]() {
        checkThread->deleteLater();
        checkThread = nullptr;
        buttonCheck->setText(tr("Check packs..."));
        if (!result->isEmpty())
        {
            checkReport = *result;
            showCheckReport(checkReport);
        }
    });
    buttonCheck->setText(tr("Checking... (stop)"));
    checkThread->start();
}

void TabDatapacks::showCheckReport(const QString& report)
{
    QDialog *dialog = new QDialog(this);
    dialog->setObjectName("dpCheckDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Pack check"));
    QVBoxLayout *layout = new QVBoxLayout(dialog);
    QLabel *hint = new QLabel(tr("What the app makes of the loaded packs. Copy it to report a "
        "problem; it names the pack files, not their folders."), dialog);
    hint->setObjectName("labelDescription");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    QPlainTextEdit *text = new QPlainTextEdit(dialog);
    text->setObjectName("textCheckReport");
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlainText(report);
    layout->addWidget(text, 1);
    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QPushButton *copy = box->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    QPushButton *save = box->addButton(tr("Save..."), QDialogButtonBox::ActionRole);
    copy->setProperty("primary", true);
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(copy, &QPushButton::clicked, dialog, [report]() {
        QGuiApplication::clipboard()->setText(report);
    });
    connect(save, &QPushButton::clicked, dialog, [dialog, report]() {
        const QString path = QFileDialog::getSaveFileName(dialog, tr("Save the pack check"),
            QDir::homePath() + "/pack-check.txt", tr("Text files (*.txt)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(report.toUtf8()) < 0)
            warn(dialog, tr("The report could not be saved."));
    });
    dialog->resize(860, 640);
    dialog->show();
}

void TabDatapacks::save(QSettings&)
{
    // The list, the dimension and the highlight belong to the world on the
    // map: the window saves them with the other world settings, whether this
    // tool was opened or not.
}

void TabDatapacks::load(QSettings&)
{
    shownWorld.reset();
    refresh();
}

int TabDatapacks::selectedVersion() const
{
    WorldInfo wi;
    parent->getSeed(&wi, false);
    return wi.mc;
}

void TabDatapacks::refreshTheme()
{
    packSignature.clear();
    shownWorld.reset();
    refresh();
}

void TabDatapacks::refresh()
{
    if (updating)
        return;
    updating = true;
    rebuildPacks();
    updateState();
    updateVanilla();
    updateDimensions();
    rebuildBiomes();
    // without packs there is no world to describe
    const bool any = !manager->entries().empty();
    sectionWorld->setVisible(any);
    sectionBiomes->setVisible(any && listBiomes->count() > 0);
    updating = false;
}

void TabDatapacks::rebuildPacks()
{
    const std::vector<PackEntry>& entries = manager->entries();
    const std::shared_ptr<const DatapackWorld> world = manager->current();

    // only rebuild the rows when something they show has changed
    QString signature = QString::number(world ? world->mc : -1);
    for (int i = 0; i < (int) entries.size(); i++)
    {
        const dp::PackInfo *info = manager->packInfo(entries[i].path);
        signature += QString("|%1:%2:%3:%4:%5").arg(entries[i].path).arg(entries[i].enabled)
            .arg(manager->packExists(i)).arg(info ? info->description : QString())
            .arg(info ? info->formatMatches : true) + QString(":%1").arg(info ? info->loaded : true);
    }
    if (signature == packSignature)
        return;
    packSignature = signature;

    while (QLayoutItem *item = packLayout->takeAt(0))
    {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    labelNoPacks->setVisible(entries.empty());
    packRows->setVisible(!entries.empty());
    sectionPacks->setCount(entries.empty() ? -1 : (int) entries.size());

    const QColor iconColor = QApplication::palette().color(QPalette::PlaceholderText);
    for (int i = 0; i < (int) entries.size(); i++)
    {
        const PackEntry& entry = entries[i];
        const bool exists = manager->packExists(i);
        const dp::PackInfo *info = manager->packInfo(entry.path);
        const QFileInfo file(entry.path);

        QFrame *row = new QFrame(packRows);
        row->setObjectName("packRow");
        row->setAttribute(Qt::WA_StyledBackground, true);
        row->setProperty("missing", !exists);
        QHBoxLayout *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 6, 6);
        rowLayout->setSpacing(6);

        QCheckBox *check = new QCheckBox(row);
        check->setChecked(entry.enabled);
        check->setToolTip(entry.enabled ? tr("Switch this pack off (it stays in the list)")
            : tr("Switch this pack on"));
        rowLayout->addWidget(check, 0, Qt::AlignTop);

        QVBoxLayout *texts = new QVBoxLayout;
        texts->setSpacing(1);
        QLabel *name = new QLabel(file.fileName(), row);
        name->setObjectName("packName");
        name->setToolTip(QDir::toNativeSeparators(entry.path));
        name->setTextInteractionFlags(Qt::NoTextInteraction);
        name->setMinimumWidth(40);
        name->setEnabled(entry.enabled && exists);
        texts->addWidget(name);

        QString description = info ? info->description.simplified() : QString();
        if (!exists)
            description.clear();
        if (!description.isEmpty())
        {
            QLabel *desc = new QLabel(description, row);
            desc->setObjectName("labelDescription");
            desc->setWordWrap(true);
            desc->setTextFormat(Qt::PlainText);
            desc->setMinimumWidth(40);
            texts->addWidget(desc);
        }
        QString note;
        bool problem = false;
        if (!exists)
        {
            note = tr("Not found: the pack was moved or deleted. Put it back, or remove "
                "this entry.");
            problem = true;
        }
        else if (!entry.enabled)
        {
            note = tr("Switched off");
        }
        else if (info && world && world->mc == selectedVersion() && !info->loaded)
        {
            note = tr("Minecraft %1 does not load this pack (see the load report). Nothing of it is used.")
                .arg(mc2str(world->mc));
            problem = true;
        }
        else if (info && world && world->mc == selectedVersion() && !info->formatMatches)
        {
            note = tr("Does not declare support for Minecraft %1. It is loaded anyway.")
                .arg(mc2str(world->mc));
        }
        if (!note.isEmpty())
        {
            QLabel *noteLabel = new QLabel(note, row);
            noteLabel->setObjectName("packNote");
            noteLabel->setProperty("problem", problem);
            noteLabel->setWordWrap(true);
            noteLabel->setMinimumWidth(40);
            texts->addWidget(noteLabel);
        }
        rowLayout->addLayout(texts, 1);

        QToolButton *up = rowButton(row, tr("Move up (load earlier)"));
        up->setIcon(chromeIcon("arrow-up", iconColor, 14));
        up->setEnabled(i > 0);
        QToolButton *down = rowButton(row, tr("Move down (load later, overrides the packs above)"));
        down->setIcon(chromeIcon("arrow-down", iconColor, 14));
        down->setEnabled(i + 1 < (int) entries.size());
        QToolButton *remove = rowButton(row, tr("Remove from the list (the file is not deleted)"));
        remove->setIcon(chromeIcon("close", iconColor, 14));
        rowLayout->addWidget(up, 0, Qt::AlignTop);
        rowLayout->addWidget(down, 0, Qt::AlignTop);
        rowLayout->addWidget(remove, 0, Qt::AlignTop);

        DatapackManager *m = manager;
        connect(check, &QCheckBox::toggled, m, [m, i](bool on) { m->setPackEnabled(i, on); });
        connect(up, &QToolButton::clicked, m, [m, i]() { m->movePack(i, -1); });
        connect(down, &QToolButton::clicked, m, [m, i]() { m->movePack(i, +1); });
        connect(remove, &QToolButton::clicked, m, [m, i]() { m->removePack(i); });
        packLayout->addWidget(row);
    }
}

QString TabDatapacks::stateLine() const
{
    return labelState->text();
}

QString TabDatapacks::issuesText() const
{
    return issues->isVisibleTo(this) ? issues->toPlainText() : QString();
}

void TabDatapacks::updateState()
{
    const std::vector<PackEntry>& entries = manager->entries();
    const std::shared_ptr<const DatapackWorld> world = manager->current();
    int missing = 0;
    for (int i = 0; i < (int) entries.size(); i++)
        missing += entries[i].enabled && !manager->packExists(i);

    QString html;
    buttonVersion->hide();
    if (!manager->active())
    {
        if (entries.empty())
            labelState->setText(tr("No datapack world: the map shows the vanilla world."));
        else if (missing)
            labelState->setText(tr("No datapack world: the packs that are switched on were "
                "not found. The map shows the vanilla world."));
        else
            labelState->setText(tr("All packs are switched off: the map shows the vanilla world."));
        setState(labelState, missing ? "problem" : "idle");
    }
    else if (!world || world->state == DatapackWorld::Loading)
    {
        labelState->setText(tr("Loading the packs..."));
        setState(labelState, "busy");
    }
    else if (world->state == DatapackWorld::UnsupportedVersion)
    {
        labelState->setText(tr("Datapack worlds are not available for Minecraft %1. They "
            "work with Minecraft %2.").arg(mc2str(world->mc), supportedVersionsText()));
        setState(labelState, "problem");
        buttonVersion->setText(tr("Switch to Minecraft %1").arg(mc2str(newestSupportedVersion())));
        buttonVersion->show();
    }
    else if (world->state == DatapackWorld::NoVanillaData)
    {
        labelState->setText(tr("One step is left: the game data of Minecraft %1 is missing.")
            .arg(mc2str(world->mc)));
        setState(labelState, "problem");
    }
    else
    {
        int errors = 0, warnings = 0;
        for (const dp::Issue& issue : world->report.issues)
        {
            errors += issue.severity == dp::Issue::Error;
            warnings += issue.severity == dp::Issue::Warning;
        }
        if (world->state == DatapackWorld::Ready)
        {
            const dp::DimensionInfo *dim = world->dimensionInfo();
            QString text = tr("Ready: the map shows the datapack world (%1 biomes).")
                .arg(dim ? (int) dim->biomes.size() : 0);
            if (missing)
                text += " " + (missing == 1 ? tr("One pack was not found and is left out.")
                    : tr("%1 packs were not found and are left out.").arg(missing));
            else if (warnings)
                text += " " + (warnings == 1 ? tr("One note below.")
                    : tr("%1 notes below.").arg(warnings));
            labelState->setText(text);
            setState(labelState, "ok"); // the missing pack is marked in the list
        }
        else if (errors)
        {
            labelState->setText(tr("Problem: the packs cannot be loaded (%1). The map stays "
                "empty until this is fixed or the pack is switched off.")
                .arg(errors == 1 ? tr("one error") : tr("%1 errors").arg(errors)));
            setState(labelState, "problem");
        }
        else
        {
            labelState->setText(tr("Problem: the packs have no dimension that the map can "
                "show yet (see Dimension below)."));
            setState(labelState, "problem");
        }

        const QPalette pal = QApplication::palette();
        const QString muted = pal.color(QPalette::PlaceholderText).name();
        const QString danger = parent->themeColor("danger").name();
        const QString strong = pal.color(QPalette::Text).name();
        for (const dp::Issue& issue : sortedIssues(world->report))
        {
            const bool error = issue.severity == dp::Issue::Error;
            QString head = error ? tr("Error") : issue.severity == dp::Issue::Warning
                ? tr("Note") : tr("Info");
            QString where;
            if (!issue.pack.isEmpty())
                where += issue.pack.toHtmlEscaped();
            if (!issue.id.isEmpty())
                where += (where.isEmpty() ? QString() : QString::fromUtf8(" \u00b7 "))
                    + issue.id.toHtmlEscaped();
            html += QString("<p style=\"margin:0 0 7px 0\"><span style=\"color:%1; "
                "font-weight:600\">%2</span>").arg(error ? danger : muted, head);
            if (!where.isEmpty())
                html += QString(" <span style=\"color:%1; font-weight:600\">%2</span>")
                    .arg(strong, where);
            html += "<br>" + readableIssue(issue).toHtmlEscaped() + "</p>";
        }
    }

    issues->setVisible(!html.isEmpty());
    if (!html.isEmpty())
    {
        const int scrollPos = issues->verticalScrollBar()->value();
        issues->setHtml(html);
        // as tall as its text, up to a limit; then it scrolls
        issues->document()->setTextWidth(qMax(200, issues->viewport()->width()));
        const int height = int(issues->document()->size().height()) + 12;
        issues->setFixedHeight(qBound(44, height, 170));
        issues->verticalScrollBar()->setValue(scrollPos);
    }
    buttonCopy->setVisible(!entries.empty());
    buttonReload->setVisible(manager->active());
}

void TabDatapacks::updateVanilla()
{
    VanillaData *vanilla = manager->vanillaData();
    const int mc = selectedVersion();
    const QString version = VanillaData::gameVersion(mc);
    const bool relevant = !manager->entries().empty() && !version.isEmpty()
        && dp::versionSupported(mc);
    const bool busy = vanilla->busy();

    labelVanilla->setVisible(relevant);
    buttonDownload->setVisible(false);
    buttonChooseFile->setVisible(relevant && !busy);
    buttonChooseFolder->setVisible(false);
    buttonCancel->setVisible(busy);
    progress->setVisible(busy);
    if (!relevant)
    {
        if (busy)
            vanilla->cancel();
        return;
    }
    if (busy)
    {   // (the version that downloads, which may not be the one picked now)
        if (progress->maximum() == 0 || progress->value() <= 0)
        {
            progress->setRange(0, 0); // busy indicator until the transfer starts
            labelVanilla->setText(tr("Asking Mojang's server for Minecraft %1...")
                .arg(VanillaData::gameVersion(vanilla->busyVersion())));
        }
        return;
    }
    progress->setRange(0, 1000);
    progress->setValue(0);

    const QString danger = parent->themeColor("danger").name();
    const VanillaSource source = vanilla->find(mc);
    const bool missing = source.kind == VanillaSource::None;
    const bool inFiles = VanillaData::dataInGameFiles(mc);
    // the loaded world could not read the game data (e.g. a damaged file)
    QString damage;
    const std::shared_ptr<const DatapackWorld> world = manager->current();
    if (!missing && world && world->mc == mc && world->vanillaPath == source.path
        && world->state == DatapackWorld::Problem)
    {
        const QString name = QFileInfo(source.path).fileName();
        for (const dp::Issue& issue : world->report.issues)
            if (issue.severity == dp::Issue::Error && issue.pack == name && damage.isEmpty())
                damage = issue.text;
    }
    QString text;
    switch (source.kind)
    {
    case VanillaSource::Chosen:
        text = tr("Game data of Minecraft %1: the file you chose (%2).")
            .arg(version, QFileInfo(source.path).fileName().toHtmlEscaped());
        break;
    case VanillaSource::Installation:
        text = tr("Game data of Minecraft %1: found in your Minecraft installation (%2).")
            .arg(version, source.detail.toHtmlEscaped());
        break;
    case VanillaSource::Downloaded:
        text = tr("Game data of Minecraft %1: downloaded from Mojang.").arg(version);
        break;
    case VanillaSource::None:
        if (!inFiles)
            text = tr("Packs build on Minecraft's own world data. Minecraft %1 keeps it in its "
                "code, so the game's files do not contain it and it cannot be downloaded. Run the "
                "game's data generator with the server file (java -DbundlerMainClass="
                "net.minecraft.data.Main -jar server.jar --reports) and choose the folder "
                "generated/reports/worldgen it writes, or a folder that holds "
                "data/minecraft/worldgen.").arg(version);
        else
            text = tr("Packs build on Minecraft's own world data, which is part of the game and "
                "not of Seed Atlas. It was not found in a Minecraft installation on this "
                "computer: download it from Mojang, or choose the game's file yourself.");
        break;
    }
    if (!damage.isEmpty())
        text += QString("<br><span style=\"color:%1\">%2</span>").arg(danger,
            (source.kind == VanillaSource::Downloaded
                ? tr("The downloaded file cannot be read. Download it again, or choose the "
                     "game's file yourself.")
                : tr("This file cannot be read. Choose another one."))
                .toHtmlEscaped());
    if (!missing && damage.isEmpty()) // (else the buttons below offer it)
        text += QString(" <a href=\"#file\" style=\"color:%1\">%2</a>")
            .arg(parent->themeColor("accentStrong").name(), tr("Use another file..."));
    if (!vanillaError.isEmpty())
        text += QString("<br><span style=\"color:%1\">%2</span>").arg(danger,
            vanillaError.toHtmlEscaped());
    labelVanilla->setText(text);
    labelVanilla->setToolTip(source.path.isEmpty() ? QString()
        : QDir::toNativeSeparators(source.path));
    const bool again = !damage.isEmpty() && source.kind == VanillaSource::Downloaded;
    buttonDownload->setText(again ? tr("Download again...") : tr("Download from Mojang..."));
    buttonDownload->setVisible((missing || again) && inFiles && VanillaData::canDownload());
    buttonChooseFile->setVisible(missing || !damage.isEmpty());
    buttonChooseFolder->setVisible(missing || !damage.isEmpty());
}

void TabDatapacks::updateDimensions()
{
    const std::shared_ptr<const DatapackWorld> world = manager->current();
    const bool loaded = world && world->world && world->report.ok()
        && !world->world->dimensions().empty();
    dimensionRow->setVisible(loaded);
    labelPreset->setVisible(false);
    if (!loaded)
        return;

    const std::vector<dp::DimensionInfo>& dims = world->world->dimensions();
    comboDimension->clear();
    for (int i = 0; i < (int) dims.size(); i++)
    {
        const dp::DimensionInfo& dim = dims[i];
        QString name = dim.id;
        if (name.startsWith(QLatin1String("minecraft:")))
        {   // "Overworld" for "minecraft:overworld", "The nether" ...
            name = name.mid(10).replace('_', ' ');
            name[0] = name[0].toUpper();
        }
        if (!dim.supported)
            name += tr(" (cannot be shown yet)");
        comboDimension->addItem(name, dim.id);
        comboDimension->setItemData(i, dim.supported ? dim.id
            : dim.reason.isEmpty() ? tr("The map cannot show this dimension yet.") : dim.reason,
            Qt::ToolTipRole);
        if (!dim.supported)
        {   // listed, but not selectable
            comboDimension->setItemData(i, 0, Qt::UserRole - 1);
        }
    }
    comboDimension->setCurrentIndex(world->dimension);
    QString tip = tr("The dimension of the datapack world that the map shows");
    if (world->dimension >= 0)
        tip += "\n" + dims[world->dimension].id;
    comboDimension->setToolTip(tip);

    if (world->usable())
    {
        const dp::DimensionInfo& dim = dims[world->dimension];
        QString text = tr("Height: Y %1 to %2, sea level %3.").arg(dim.minY)
            .arg(dim.minY + dim.height - 1).arg(dim.seaLevel);
        if (!world->presetMatters)
            text += " " + tr("A pack brings its own file for this dimension, so the world "
                "type (\"World\" on the map) does not change it.");
        else
            text += " " + tr("World type: %1 (\"World\" on the map).")
                .arg(worldTypeName(world->preset == datapackPreset(WORLD_AMPLIFIED) ? WORLD_AMPLIFIED
                    : world->preset == datapackPreset(WORLD_LARGE_BIOMES) ? WORLD_LARGE_BIOMES
                    : WORLD_DEFAULT));
        labelPreset->setText(text);
        labelPreset->setVisible(true);
    }
}

void TabDatapacks::rebuildBiomes()
{
    const std::shared_ptr<const DatapackWorld> world = manager->current();
    if (world == shownWorld && (world || listBiomes->count() == 0))
        return;
    // while the packs load, the list of the previous world stays
    if (world && world->state == DatapackWorld::Loading && listBiomes->count()
        && shownWorld && shownWorld->usable())
        return;
    shownWorld = world;

    const QStringList highlighted = parent->getMapView()->getDatapackHighlights();
    const QSignalBlocker blocker(listBiomes);
    listBiomes->clear();
    int count = 0;
    if (world && world->usable())
    {
        const std::vector<dp::BiomeInfo>& biomes = world->world->biomes();
        const QColor border = QApplication::palette().color(QPalette::Mid);
        QFont headingFont = listBiomes->font();
        headingFont.setBold(true);
        QString group;
        QListWidgetItem *heading = nullptr;
        int groupCount = 0;
        const auto closeGroup = [&]() {
            if (heading)
                heading->setText(QString::fromUtf8("%1 \u00b7 %2").arg(group).arg(groupCount));
        };
        for (int index : dpBiomeOrder(*world))
        {
            const dp::BiomeInfo& biome = biomes[index];
            const QString space = biome.id.section(':', 0, 0);
            if (space != group || !heading)
            {
                closeGroup();
                group = space;
                groupCount = 0;
                heading = new QListWidgetItem(listBiomes);
                heading->setFlags(Qt::NoItemFlags);
                heading->setFont(headingFont);
                heading->setForeground(QApplication::palette().color(QPalette::PlaceholderText));
                heading->setData(BiomeIdRole, QString());
            }
            QListWidgetItem *item = new QListWidgetItem(dpSwatchIcon(biome.color, border, true),
                world->labels[index], listBiomes);
            item->setData(BiomeIdRole, biome.id);
            item->setToolTip(biome.id);
            if (highlighted.contains(biome.id))
                item->setSelected(true);
            groupCount++;
            count++;
        }
        closeGroup();
    }
    sectionBiomes->setCount(count ? count : -1);
    applyBiomeFilter();
    onBiomeSelectionChanged();
}

void TabDatapacks::applyBiomeFilter()
{
    dpFilterGroupedList(listBiomes, BiomeIdRole, lineSearch->text().trimmed());
    updateBiomeCount();
}

void TabDatapacks::setBiomeFilter(const QString& text)
{
    lineSearch->setText(text);
}

void TabDatapacks::setHighlightedBiomes(const QStringList& ids)
{
    {
        const QSignalBlocker blocker(listBiomes);
        for (int i = 0; i < listBiomes->count(); i++)
        {
            QListWidgetItem *item = listBiomes->item(i);
            const QString id = item->data(BiomeIdRole).toString();
            if (!id.isEmpty())
                item->setSelected(ids.contains(id));
        }
    }
    parent->getMapView()->setDatapackHighlights(ids);
    updateBiomeCount();
}

void TabDatapacks::onBiomeSelectionChanged()
{
    // only a list that shows a world decides: an empty list (no world, or
    // one that failed to load) keeps the highlight for later
    if (shownWorld && shownWorld->usable())
    {
        QStringList ids;
        for (int i = 0; i < listBiomes->count(); i++)
        {
            QListWidgetItem *item = listBiomes->item(i);
            const QString id = item->data(BiomeIdRole).toString();
            if (!id.isEmpty() && item->isSelected())
                ids.append(id);
        }
        if (ids != parent->getMapView()->getDatapackHighlights())
            parent->getMapView()->setDatapackHighlights(ids);
    }
    updateBiomeCount();
}

void TabDatapacks::updateBiomeCount()
{
    int total = 0, shown = 0, selected = 0;
    for (int i = 0; i < listBiomes->count(); i++)
    {
        QListWidgetItem *item = listBiomes->item(i);
        if (item->data(BiomeIdRole).toString().isEmpty())
            continue;
        total++;
        shown += !item->isHidden();
        selected += item->isSelected();
    }
    QString text;
    if (total)
    {
        text = shown == total ? (total == 1 ? tr("1 biome") : tr("%1 biomes").arg(total))
            : tr("%1 of %2 biomes").arg(shown).arg(total);
        if (selected)
            text += QString::fromUtf8(" \u00b7 ") + tr("%1 highlighted").arg(selected);
    }
    labelBiomeCount->setText(text);
    buttonClear->setEnabled(selected > 0);
}

QString TabDatapacks::detailsText() const
{
    const std::shared_ptr<const DatapackWorld> world = manager->current();
    WorldInfo wi;
    parent->getSeed(&wi, false);
    QStringList lines;
    lines << QString("Seed Atlas %1, datapack world").arg(getVersStr());
    lines << QString("Minecraft %1, world type %2, dimension %3").arg(mc2str(wi.mc),
        worldTypeName(wi.worldType()), manager->dimension());
    lines << QString("State: %1").arg(labelState->text());
    const VanillaSource source = manager->vanillaData()->find(wi.mc);
    static const char *const kinds[] = {"missing", "chosen file", "Minecraft installation", "downloaded"};
    lines << QString("Game data: %1").arg(kinds[source.kind]);
    lines << QString();
    lines << "Packs (in load order):";
    const std::vector<PackEntry>& entries = manager->entries();
    for (int i = 0; i < (int) entries.size(); i++)
    {
        const dp::PackInfo *info = manager->packInfo(entries[i].path);
        QString line = QString("%1. %2").arg(i + 1).arg(QFileInfo(entries[i].path).fileName());
        if (!manager->packExists(i))
            line += " [not found]";
        else if (!entries[i].enabled)
            line += " [switched off]";
        if (info)
        {
            line += QString(" (pack format %1%2, %3 worldgen files)").arg(info->packFormat)
                .arg(!info->loaded ? ", not loaded by the game" : info->formatMatches ? "" : ", not declared for this version")
                .arg(info->worldgenFiles);
            if (!info->overlays.isEmpty())
                line += " overlays: " + info->overlays.join(", ");
        }
        lines << line;
    }
    if (world && world->world)
    {
        lines << QString();
        lines << "Dimensions:";
        for (const dp::DimensionInfo& dim : world->world->dimensions())
            lines << QString("- %1: %2").arg(dim.id, dim.supported
                ? QString("%1 biomes, y %2 to %3").arg(dim.biomes.size()).arg(dim.minY)
                    .arg(dim.minY + dim.height - 1)
                : "not shown: " + dim.reason);
        lines << QString();
        lines << QString("Messages (%1):").arg(world->report.issues.size());
        for (const dp::Issue& issue : sortedIssues(world->report))
        {
            lines << QString("- %1 [%2] %3: %4").arg(issue.severity == dp::Issue::Error ? "ERROR"
                    : issue.severity == dp::Issue::Warning ? "note" : "info",
                issue.pack.isEmpty() ? QString("-") : issue.pack,
                issue.id.isEmpty() ? QString("-") : issue.id, issue.text);
        }
    }
    // the messages of the load may name files with their folders
    QStringList known;
    for (const PackEntry& entry : entries)
        known << entry.path;
    known << source.path;
    if (world)
        known << world->vanillaPath;
    return dpScrubPaths(lines.join("\n") + "\n", known);
}

void TabDatapacks::addFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add datapacks"),
        parent->prevdir, tr("Data packs and mods (*.zip *.jar);;Any files (*)"));
    if (files.isEmpty())
        return;
    parent->prevdir = QFileInfo(files.first()).absolutePath();
    parent->addDatapacks(files);
}

void TabDatapacks::addFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Add a datapack folder"),
        parent->prevdir);
    if (dir.isEmpty())
        return;
    parent->prevdir = QFileInfo(dir).absolutePath();
    parent->addDatapacks({dir});
}

void TabDatapacks::chooseVanillaFile()
{
    const int mc = selectedVersion();
    const QString version = VanillaData::gameVersion(mc);
    const QString file = QFileDialog::getOpenFileName(this,
        tr("Choose the game file of Minecraft %1").arg(version), parent->prevdir,
        tr("Minecraft game files (*.jar *.zip);;Any files (*)"));
    if (file.isEmpty())
        return;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = manager->vanillaData()->choose(mc, file, &error);
    QApplication::restoreOverrideCursor();
    vanillaError = ok ? QString() : tr("\"%1\" cannot be used: %2")
        .arg(QFileInfo(file).fileName(), error);
    updateVanilla();
}

void TabDatapacks::chooseVanillaFolder()
{
    const int mc = selectedVersion();
    const QString version = VanillaData::gameVersion(mc);
    const QString dir = QFileDialog::getExistingDirectory(this,
        tr("Choose the folder with the game data of Minecraft %1").arg(version), parent->prevdir);
    if (dir.isEmpty())
        return;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = manager->vanillaData()->choose(mc, dir, &error);
    QApplication::restoreOverrideCursor();
    vanillaError = ok ? QString() : tr("\"%1\" cannot be used: %2")
        .arg(QFileInfo(dir).fileName(), error);
    updateVanilla();
}

void TabDatapacks::downloadVanilla()
{
    VanillaData *vanilla = manager->vanillaData();
    const int mc = selectedVersion();
    if (vanilla->busy())
        return;
    // One clear question per version, before any of Mojang's servers is
    // contacted: what is fetched, from where, about how much. Test runs
    // answer it through a variable of their own.
    bool agreed = qEnvironmentVariableIsSet("SEED_ATLAS_TEST_VANILLA_DOWNLOAD")
        && !qEnvironmentVariableIsEmpty("SEED_ATLAS_TEST_SETTINGS_DIR");
    if (!agreed)
    {
        const QString version = VanillaData::gameVersion(mc);
        QMessageBox box(QMessageBox::Question, tr("Download game data"),
            tr("Datapack worlds build on the world generation data of Minecraft %1, "
               "which is part of the game and not of Seed Atlas.").arg(version),
            QMessageBox::NoButton, this);
        box.setObjectName("vanillaConsent");
        box.setInformativeText(tr("Download the game file of Minecraft %1 from Mojang now?\n\n"
            "From: Mojang's servers (piston-meta.mojang.com for the version list, "
            "piston-data.mojang.com for the file)\n"
            "Size: about 20 to 40 MB, shown exactly once the download starts\n"
            "Saved in: %2\n\n"
            "Nothing is sent to Mojang before you agree. The file is only read on this "
            "computer, to compute the map.")
            .arg(version, QDir::toNativeSeparators(VanillaData::cacheDir())));
        QPushButton *yes = box.addButton(tr("Download"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(yes);
        box.exec();
        agreed = box.clickedButton() == yes;
    }
    if (!agreed)
        return;
    vanillaError.clear();
    progress->setRange(0, 0);
    progress->setValue(0);
    vanilla->fetchInfo(mc);
    updateVanilla();
}
