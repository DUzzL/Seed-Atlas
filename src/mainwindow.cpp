#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "aboutdialog.h"
#include "biomecolordialog.h"
#include "configdialog.h"
#include "datapackworld.h"
#include "dpmap.h"
#include "dpstructuresdialog.h"
#include "exportdialog.h"
#include "extgendialog.h"
#include "glyphs.h"
#include "layerdialog.h"
#include "maptoolsdialog.h"
#include "message.h"
#include "tabbiomes.h"
#include "tabdatapacks.h"
#include "formdpconditions.h"
#include "tablocations.h"
#include "tabslime.h"
#include "tabportals.h"
#include "tabstructures.h"
#include "util.h"
#include "vanilladata.h"
#include "world.h"

#if WITH_UPDATER
#include "updater.h"
#endif

#include <QBuffer>
#include <QButtonGroup>
#include <QTabBar>
#include <QVBoxLayout>
#include <QByteArray>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFile>
#include <QSaveFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QDebug>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QIntValidator>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QMenu>
#include <QMetaType>
#include <QPalette>
#include <QPainter>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QPushButton>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QToolButton>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif
#include <QPointer>
#include <QTreeWidget>
#include <QTableView>
#include <QListWidget>
#include <QAbstractItemView>

#if WITH_DBUS
#include <QDBusMessage>
#include <QDBusConnection>
#endif

int getStructureConfig_override(int stype, int mc, StructureConfig *sconf);

namespace {

// Which chrome icon the rail shows for a tool page.
QString railIconKind(const QWidget *page)
{
    if (page->objectName() == "tabSearch")
        return "search";
    if (page->inherits("TabBiomes"))
        return "biomes";
    if (page->inherits("TabStructures"))
        return "structures";
    if (page->inherits("TabSlime"))
        return "slime";
    if (page->inherits("TabPortals"))
        return "eye";
    const QString kind = page->property("railIcon").toString();
    return kind.isEmpty() ? QString("layers") : kind;
}

QFrame *createMapCard(const char *name, QWidget *parent)
{
    QFrame *card = new QFrame(parent);
    card->setObjectName(name);
    card->setAttribute(Qt::WA_StyledBackground, true);
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 90));
    card->setGraphicsEffect(shadow);
    return card;
}

constexpr int SIDE_PANEL_MINIMUM_WIDTH = 400;
constexpr int SIDE_PANEL_DEFAULT_WIDTH = 470;
constexpr int MAP_CARD_MARGIN = 12;

} // namespace


MainWindow::MainWindow(QString sessionpath, QString resultspath, QString opensession,
                       QWidget *parent)
    : QMainWindow(parent)
    , ui()
    , dock()
    , mapView()
    , formCond()
    , formGen48()
    , formControl()
    , lopt()
    , config()
    , mconfig(false)
    , sessionpath(sessionpath)
    , opensession(opensession)
    , prevdir(".")
    , autosaveTimer()
    , tabidx(-1)
    , tabsearch(-1)
    , progval(-1)
    , dimactions{}
    , dimgroup()
    , sidePanelToggle()
    , sidePanelAnimation(new QPropertyAnimation(this, "sidePanelAnimationWidth", this))
    , sidePanelWidth(SIDE_PANEL_DEFAULT_WIDTH)
    , sidePanelCurrentWidth(SIDE_PANEL_DEFAULT_WIDTH)
    , sidePanelCollapsed(false)
    , sidePanelChanging(false)
{
    QSettings settings(appSettingsId(), appSettingsId());
    // Seed Atlas currently ships with one English interface only.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    settings.remove("config/lang");

    ui = new Ui::MainWindow;
    dock = new QDockWidget(tr("Map"), this);
    mapView = new MapView(this);

    ui->setupUi(this);
    resize(1600, 900);

    // Data packs: the list is part of the settings (read in loadSettings()).
    // A change reaches the map through onDatapacksChanged(), queued, so that
    // it never runs in the middle of another update.
    vanillaData = new VanillaData(this);
    datapacks = new DatapackManager(vanillaData, this);
    connect(datapacks, &DatapackManager::worldChanged, this, &MainWindow::onDatapacksChanged,
        Qt::QueuedConnection);
    setAcceptDrops(true);
    for (int i = 0; i < ui->comboY->count(); i++)
        vanillaLevels.append(ui->comboY->itemText(i));

    // Replace the crowded menu strip with one dedicated Settings control.
    // It opens the full Settings dialog instead of a drop-down menu.
    ui->menuBar->clear();
    ui->menuBar->hide();
    // the procedural icons get their color in onStyleChanged()
    settingsAction = new QAction(tr("Settings"), this);
    settingsAction->setProperty("toolbarLabel", tr("Settings"));
    connect(settingsAction, &QAction::triggered, ui->actionPreferences, &QAction::trigger);

    // The former menus (sessions, presets, examples, layers, export, about)
    // stay reachable through one Menu button. Their actions are also added
    // to the window, as shortcuts of actions in a hidden menu do not fire.
    mainMenu = new QMenu(this);
    for (QMenu *menu : {ui->menuFile, ui->menuEdit, ui->menuMap, ui->menuLayer, ui->menuHelp})
    {
        mainMenu->addMenu(menu);
        for (QAction *action : menu->actions())
        {
            if (!action->menu() && !action->isSeparator())
                addAction(action);
        }
    }
    menuAction = new QAction(tr("Menu"), this);
    menuAction->setProperty("toolbarLabel", tr("Menu"));
    connect(menuAction, &QAction::triggered, this, [this]() {
        mainMenu->popup(QCursor::pos());
    });

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) {
        if (config.uistyle == STYLE_SYSTEM)
            onStyleChanged(STYLE_SYSTEM);
    });
#endif

    dock->setWidget(mapView);
    dock->setFeatures(QDockWidget::DockWidgetFloatable);
    connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::onDockFloating);
    QMainWindow *submain = new QMainWindow(this);
    ui->frameMap->layout()->addWidget(submain);
    submain->addDockWidget(Qt::LeftDockWidgetArea, dock);
    mapView->setFocusPolicy(Qt::StrongFocus);
    setDockable(false);

    formCond = new FormConditions(this);
    formDpCond = new FormDpConditions(this);
    {   // the vanilla conditions and those for datapack worlds share the
        // Conditions section; it shows the ones of the world on the map
        QWidget *conditionsBox = new QWidget(this);
        QVBoxLayout *conditionsLayout = new QVBoxLayout(conditionsBox);
        conditionsLayout->setContentsMargins(0, 0, 0, 0);
        conditionsLayout->setSpacing(0);
        conditionsLayout->addWidget(formCond);
        conditionsLayout->addWidget(formDpCond);
        formDpCond->hide();
    }
    formGen48 = new FormGen48(this);
    formControl = new FormSearchControl(this);

    ui->menuHistory->clear();

    tabLocations = new TabLocations(this);
    ui->tabContainerSearch->addTab(tabLocations, tr("Locations"));
    ui->tabContainer->addTab(new TabBiomes(this), tr("Biomes"));
    ui->tabContainer->addTab(new TabStructures(this), tr("Structures"));
    ui->tabContainer->addTab(new TabSlime(this), tr("Slime chunks"));
    ui->tabContainer->addTab(new TabPortals(this), tr("End portals"));
    tabDatapacks = new TabDatapacks(this, datapacks);
    ui->tabContainer->addTab(tabDatapacks, tr("Datapacks"));
    setupToolRail();

    // One notice for all tools that only know vanilla worlds, above the tool.
    datapackNotice = new QLabel(ui->controlFrame);
    datapackNotice->setObjectName("labelNotice");
    datapackNotice->setWordWrap(true);
    datapackNotice->setTextFormat(Qt::RichText);
    datapackNotice->hide();
    connect(datapackNotice, &QLabel::linkActivated, this, &MainWindow::openDatapacks);
    if (QBoxLayout *panel = qobject_cast<QBoxLayout*>(ui->controlFrame->layout()))
        panel->insertWidget(panel->indexOf(ui->tabContainer), datapackNotice);
    connect(ui->tabContainer, &QTabWidget::currentChanged, this, &MainWindow::updateDatapackNotice);

    laction.resize(LOPT_MAX);
    laction[LOPT_BIOMES] = ui->actionBiomes;
    laction[LOPT_NOISE_T_4] = ui->actionParaTemperature;
    laction[LOPT_NOISE_H_4] = ui->actionParaHumidity;
    laction[LOPT_NOISE_C_4] = ui->actionParaContinentalness;
    laction[LOPT_NOISE_E_4] = ui->actionParaErosion;
    laction[LOPT_NOISE_W_4] = ui->actionParaWeirdness;
    laction[LOPT_NOISE_D_4] = ui->actionParaDepth;
    laction[LOPT_RIVER_4] = ui->actionRiver;
    laction[LOPT_OCEAN_256] = ui->actionOceanTemp;
    laction[LOPT_NOOCEAN_1] = ui->actionNoOceans;
    laction[LOPT_BETA_T_1] = ui->actionBetaTemperature;
    laction[LOPT_BETA_H_1] = ui->actionBetaHumidity;
    laction[LOPT_HEIGHT] = ui->actionHeight;
    laction[LOPT_TERRAIN] = ui->actionTerrain;
    laction[LOPT_STRUCTS] = ui->actionStructures;

    QActionGroup *grp = new QActionGroup(this);
    for (int i = 0; i < laction.size(); i++)
    {
        QAction *act = laction[i];
        if (!act) continue;
        connect(act, &QAction::toggled, [=](bool checked) {
            // an exclusive group also emits toggled(false) for the old layer
            if (checked && !layerSync)
                this->onActionBiomeLayerSelect(i);
        });
        act->setActionGroup(grp);
    }
    connect(mapView, &MapView::layerChange, this, &MainWindow::onActionBiomeLayerSelect);
    for (int i = 0; i <= 9; i++)
    {
        QAction *act = new QAction(this);
        act->setShortcut(QKeySequence(QKeyCombination(Qt::ALT, Qt::Key(int(Qt::Key_0) + i))));
        act->setEnabled(true);
        connect(act, &QAction::triggered, [=](){
            this->onActionBiomeLayerSelect(lopt.mode, i);
        });
        addAction(act);
    }

    // the chrome icons get their colors in onStyleChanged()
    acthome = new QAction(tr("Go to world origin (0, 0)"), this);
    actzoom[0] = new QAction(tr("Zoom in"), this);
    actzoom[1] = new QAction(tr("Zoom out"), this);
    acthome->setProperty("toolbarLabel", tr("Origin"));
    actzoom[0]->setProperty("toolbarLabel", tr("Zoom in"));
    actzoom[1]->setProperty("toolbarLabel", tr("Zoom out"));
    connect(acthome, &QAction::triggered, [=](){ this->mapGoto(0,0,16); });
    connect(actzoom[0], &QAction::triggered, [=](){ this->mapZoom(pow(2.0, +0.5)); });
    connect(actzoom[1], &QAction::triggered, [=](){ this->mapZoom(pow(2.0, -0.5)); });

    // Settings, Menu, origin and zoom live in the tool rail and on the map;
    // the sidebar lists only the map layers, in three groups.
    saction.resize(D_STRUCT_NUM);
    ui->toolBar->addSection(tr("View"));
    addMapAction(D_GRID);
    // The layer menu is hidden, so the terrain map has its own toggle.
    terrainAction = addMapAction("terrain",
        tr("Terrain map: Minecraft-like surface colors with hill shading (1.18 and newer)"),
        tr("Terrain"));
    terrainAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_G));
    addAction(terrainAction); // the toolbar alone does not deliver the shortcut
    connect(terrainAction, &QAction::toggled, this, [this](bool checked) {
        if (!layerSync)
            onActionBiomeLayerSelect(checked ? LOPT_TERRAIN : LOPT_BIOMES);
    });
    ui->toolBar->addSection(tr("Dimension"));

    dimactions[0] = addMapAction("overworld", tr("Overworld"));
    dimactions[1] = addMapAction("nether", tr("Nether"));
    dimactions[2] = addMapAction("the_end", tr("End"));
    dimgroup = new QActionGroup(this);
    dimgroup->setExclusive(true);

    for (int i = 0; i < 3; i++)
    {
        dimactions[i]->setObjectName(QString("dimensionAction%1").arg(i));
        dimgroup->addAction(dimactions[i]);
        connect(dimactions[i], &QAction::triggered, this, [this, i](bool checked) {
            // A dimension is a map mode, not an optional overlay. The custom
            // toolbar forwards clicks through QAction::trigger(), so guard the
            // active action explicitly in addition to QActionGroup exclusivity.
            if (!checked)
            {
                dimactions[i]->setChecked(true);
                return;
            }
            updateMapSeed();
        });
    }
    dimactions[0]->setChecked(true);
    ui->toolBar->addSection(tr("Structures"));

    // Only the structures of the shown dimension and version are listed (see
    // updateStructureActions), so they need no separators between groups.
    addMapAction(D_SLIME);
    // The structures of a datapack world: one entry that opens their list (a
    // pack can bring hundreds). Vanilla structures of such a world use the
    // entries below; it is only shown for datapack worlds.
    dpStructAction = new QAction(tr("Structures of the datapacks: pick which ones the map shows"), this);
    dpStructAction->setObjectName("actionDatapackStructures");
    dpStructAction->setProperty("toolbarLabel", tr("Pack structures..."));
    dpStructAction->setVisible(false);
    ui->toolBar->addAction(dpStructAction);
    connect(dpStructAction, &QAction::triggered, this, &MainWindow::openDatapackStructures);
    addMapAction(D_SPAWN);
    addMapAction(D_STRONGHOLD);
    addMapAction(D_VILLAGE);
    addMapAction(D_MINESHAFT);
    addMapAction(D_DESERT);
    addMapAction(D_JUNGLE);
    addMapAction(D_HUT);
    addMapAction(D_MONUMENT);
    addMapAction(D_IGLOO);
    addMapAction(D_MANSION);
    addMapAction(D_OCEANRUIN);
    addMapAction(D_SHIPWRECK);
    addMapAction(D_TREASURE);
    addMapAction(D_WELL);
    addMapAction(D_GEODE);
    addMapAction(D_OUTPOST);
    addMapAction(D_PORTAL);
    addMapAction(D_ANCIENTCITY);
    addMapAction(D_TRAILRUINS);
    addMapAction(D_CHAMBERS);
    addMapAction(D_CAMP);
    addMapAction(D_OREVEIN);
    addMapAction(D_FORTESS);
    addMapAction(D_BASTION);
    addMapAction(D_ENDCITY);
    addMapAction(D_ENDCITYSHIP);
    addMapAction(D_GATEWAY);

    connect(ui->toolBar, &ScrollableToolBar::collapsedChanged, this, [](bool collapsed) {
        QSettings settings(appSettingsId(), appSettingsId());
        settings.setValue("ui/rightSidebarCollapsed", collapsed);
    });

    saction[D_GRID]->setChecked(true);
    saction[D_SPAWN]->setChecked(true);

    ui->splitterMap->setStretchFactor(0, 0);
    ui->splitterMap->setStretchFactor(1, 1);
    ui->splitterMap->setSizes(QList<int>({SIDE_PANEL_DEFAULT_WIDTH, 1040}));
    ui->splitterSearch->setSizes(QList<int>({1000, 3000}));
    ui->splitterSeeds->setSizes(QList<int>({500, 2500}));
    ui->splitterMap->setHandleWidth(1);

    setupMapOverlays();

    sidePanelToggle = new QToolButton(ui->centralWidget);
    sidePanelToggle->setObjectName("sidePanelToggle");
    sidePanelToggle->setFixedSize(14, 56);
    sidePanelToggle->setAutoRaise(false);
    sidePanelToggle->setCursor(Qt::PointingHandCursor);
    sidePanelToggle->setToolTip(tr("Collapse side panel"));
    connect(sidePanelToggle, &QToolButton::clicked, this, &MainWindow::onSidePanelToggle);
    connect(ui->splitterMap, &QSplitter::splitterMoved, this, &MainWindow::onSplitterMapMoved);
    sidePanelAnimation->setDuration(220);
    sidePanelAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(sidePanelAnimation, &QPropertyAnimation::finished,
        this, &MainWindow::onSidePanelAnimationFinished);
    QTimer::singleShot(0, this, &MainWindow::updateSidePanelToggle);

    qRegisterMetaType< int64_t >("int64_t");
    qRegisterMetaType< uint64_t >("uint64_t");
    qRegisterMetaType< QVector<uint64_t> >("QVector<uint64_t>");
    qRegisterMetaType< Config >("Config");

    formCond->updateSensitivity();

    connect(&autosaveTimer, &QTimer::timeout, this, &MainWindow::onAutosaveTimeout);

    loadSettings();

    // the box of both condition lists (see the constructor's start)
    ui->collapseConstraints->init(tr("Conditions"), formCond->parentWidget(), false);
    connect(formCond, &FormConditions::changed, this, &MainWindow::onConditionsChanged);
    connect(formDpCond, &FormDpConditions::changed, this, [this]() {
        if (formControl)
            formControl->invalidateResults();
    });
    connect(formCond, &FormConditions::selectionUpdate, this, &MainWindow::onConditionsSelect);
    ui->collapseConstraints->setInfo(
        tr("Help: Conditions"),
        tr(
        "<html><head/><body><p>"
        "Pick a structure or biome and how far from the world origin it may be, "
        "then press <b>+ Add</b>. <b>More options...</b> opens the condition "
        "editor with every kind of condition and all settings, and "
        "<b>Saved filters</b> loads examples or your own lists of conditions "
        "and saves the current one."
        "</p><p>"
        "A seed matches when it meets all conditions in the list. Conditions "
        "that are turned off are ignored."
        "</p><p>"
        "Each condition has a number (#1, #2, ...) at the right. Another "
        "condition can be <b>measured from</b> it instead of from the world "
        "origin, e.g. a village within 200 blocks of #1. When a condition "
        "passes, it usually gives one position for the conditions measured "
        "from it. A structure condition that asks for at least 1 instead "
        "tries each instance it finds, one after another, and one that asks "
        "for several uses their average position."
        "</p><p>"
        "Biome conditions give the center of their area, as they check the "
        "area as a whole. To find where a biome is, use <b>Biome patch of a "
        "minimum size</b>, or (with the expert types) scan an area in steps "
        "with a condition measured from the scan."
        "</p></body></html>"
    ));

    // 48-bit generator settings are not all that interesting unless we are
    // using them, so start as collapsed if they are on the "Auto" setting.
    Gen48Config gen48 = formGen48->getConfig(false);
    ui->collapseGen48->init(tr("Seed generator"), formGen48, gen48.mode == GEN48_AUTO);
    connect(formGen48, &FormGen48::changed, this, &MainWindow::onGen48Changed);
    ui->collapseGen48->setInfo(
        tr("Help: Seed generator"),
        tr(
        "<html><head/><body><p>"
        "Where most structures can generate depends only on the lower 48 bits "
        "of a seed, the <i>structure seed</i>. For some searches, suitable "
        "structure seeds can be computed directly instead of testing every "
        "seed, which makes the search much faster."
        "</p><p>"
        "<b>Auto</b> is recommended: it picks suitable settings from the "
        "Seed Finder conditions."
        "</p><p>"
        "<b>Quad-feature</b> produces candidates with four structures close "
        "together, for structures that are placed in 32&#8209;chunk regions "
        "with an 8&#8209;chunk gap, such as swamp huts."
        "</p><p>"
        "A perfect <b>Quad-monument</b> does not exist, but some extremely "
        "rare structure seeds come close, with over 90&#37; of the monument "
        "area within 128 blocks of one spot. The generator uses a "
        "precomputed list of these structure seeds."
        "</p><p>"
        "With <b>Seed list</b> you load your own structure seeds from a "
        "file. Optionally, a salt (a fixed number that each structure type "
        "adds to the seed) can be added, and the seeds can be moved by whole "
        "regions of 512 blocks."
        "</p></body></html>"
    ));

    ui->collapseControl->init(tr("Matching seeds"), formControl, false);
    connect(formControl, &FormSearchControl::selectedSeedChanged, this, &MainWindow::onSelectedSeedChanged);
    connect(formControl, &FormSearchControl::searchStatusChanged, this, &MainWindow::onSearchStatusChanged);
    if (!resultspath.isEmpty())
        formControl->setResultsPath(resultspath);
    ui->collapseControl->setInfo(
        tr("Help: Matching seeds"),
        tr(
        "<html><head/><body><p>"
        "The list of seeds acts as a buffer onto which suitable seeds are added "
        "when they are found. You can also copy the seed list, or paste seeds "
        "into the list. Selecting a seed will open it in the map view."
        "</p></body></html>"
    ));

    connect(formControl, &FormSearchControl::advancedChanged, this, [this](bool on) {
        config.advancedSearch = on;
        QSettings settings(appSettingsId(), appSettingsId());
        settings.setValue("config/advancedSearch", on);
        applySearchMode();
    });
    applySearchMode();

    // the section headings count their entries: "CONDITIONS \u00b7 2"
    const auto countRows = [](QAbstractItemView *view, Collapsible *section) {
        if (!view || !view->model())
            return;
        QAbstractItemModel *model = view->model();
        // the list empties itself while the window is torn down, after the
        // heading button is gone
        QPointer<QToolButton> heading = section->toggleButton;
        const auto update = [model, section, heading]() {
            if (heading)
                section->setCount(model->rowCount());
        };
        connect(model, &QAbstractItemModel::rowsInserted, section, update);
        connect(model, &QAbstractItemModel::rowsRemoved, section, update);
        connect(model, &QAbstractItemModel::modelReset, section, update);
        connect(model, &QAbstractItemModel::layoutChanged, section, update);
        update();
    };
    // the heading counts the conditions of the list that is shown
    for (QListWidget *list : {formCond->findChild<QListWidget*>("listConditions"), formDpCond->list()})
    {
        if (!list)
            continue;
        QAbstractItemModel *model = list->model();
        QPointer<QToolButton> heading = ui->collapseConstraints->toggleButton;
        const auto update = [this, heading]() {
            if (heading && activeConditionList())
                ui->collapseConstraints->setCount(activeConditionList()->count());
        };
        connect(model, &QAbstractItemModel::rowsInserted, this, update);
        connect(model, &QAbstractItemModel::rowsRemoved, this, update);
        connect(model, &QAbstractItemModel::modelReset, this, update);
        connect(model, &QAbstractItemModel::layoutChanged, this, update);
    }
    ui->collapseConstraints->setCount(activeConditionList()->count());

    // the condition list grows with its rows, the matching seeds give way
    for (QListWidget *list : {formCond->findChild<QListWidget*>("listConditions"), formDpCond->list()})
    {
        if (!list)
            continue;
        const auto refit = [this]() {
            QTimer::singleShot(0, this, &MainWindow::fitConditionsHeight);
        };
        QAbstractItemModel *model = list->model();
        connect(model, &QAbstractItemModel::rowsInserted, this, refit);
        connect(model, &QAbstractItemModel::rowsRemoved, this, refit);
        connect(model, &QAbstractItemModel::modelReset, this, refit);
        connect(model, &QAbstractItemModel::dataChanged, this, refit);
    }
    connect(ui->collapseConstraints->toggleButton, &QToolButton::toggled, this, [this]() {
        QTimer::singleShot(250, this, &MainWindow::fitConditionsHeight);
    });
    connect(ui->splitterSearch, &QSplitter::splitterMoved, this, [this]() {
        conditionsHeightManual = true;
    });
    ui->splitterSearch->installEventFilter(this);
    countRows(formControl->findChild<QTableView*>("results"), ui->collapseControl);

    // only sync the generator here: onConditionsChanged() would also discard
    // the matching seeds that loadSettings() just restored from the session
    formGen48->updateAutoConditions(formCond->getConditions());
    update();

#if WASM
    ui->menuFile->removeAction(ui->actionQuit);
#endif

#if WITH_UPDATER
    QAction *updateaction = new QAction(tr("Check for updates"), this);
    connect(updateaction, &QAction::triggered,
        [this]() { searchForUpdates(this, &config, false); });
    ui->menuHelp->insertAction(ui->actionAbout, updateaction);
    ui->menuHelp->insertSeparator(ui->actionAbout);

    if (config.checkForUpdates)
        QTimer::singleShot(0, this,
            [this]() { searchForUpdates(this, &config, true); });
#endif
}

MainWindow::~MainWindow()
{
    // The analysis tabs stop and wait for their threads when destroyed, which
    // processes queued events that can call back into this window. Destroy
    // them while the window is still intact instead of with the children.
    // Only the analysis tabs: the Seed Finder page also holds the search forms
    // and the container of the Locations tab.
    tabLocations = nullptr;
    QList<QWidget*> analysisTabs;
    for (QTabWidget *tabs : {ui->tabContainerSearch, ui->tabContainer})
        for (int i = 0; i < tabs->count(); i++)
            if (dynamic_cast<ISaveTab*>(tabs->widget(i)))
                analysisTabs.append(tabs->widget(i));
    qDeleteAll(analysisTabs);
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    formControl->stopSearch();
    QThreadPool::globalInstance()->clear();
    saveSettings();

    if (config.restoreSession)
    {
        QString error;
        if (!saveSession(sessionpath, true, &error))
        {
            int button = warn(this, tr("Session not saved"),
                tr("The current session (conditions, matching seeds and search progress) "
                   "could not be saved:\n%1\n\nThe previous session file was kept.").arg(error),
                tr("Quit anyway?"), QMessageBox::Yes | QMessageBox::No);
            if (button != QMessageBox::Yes)
            {
                event->ignore();
                return;
            }
        }
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, &MainWindow::updateSidePanelToggle);
}

void MainWindow::setRightSidebarCollapsed(bool collapsed, bool animated)
{
    ui->toolBar->setCollapsed(collapsed, animated);
}

void MainWindow::setSidePanelCollapsed(bool collapsed)
{
    if (sidePanelCollapsed == collapsed &&
        sidePanelAnimation->state() == QAbstractAnimation::Stopped)
        return;

    const int startWidth = ui->controlFrame->isVisible() ? ui->controlFrame->width() : 0;
    sidePanelAnimation->stop();
    sidePanelChanging = true;
    if (collapsed)
    {
        if (startWidth >= SIDE_PANEL_MINIMUM_WIDTH)
            sidePanelWidth = startWidth;
    }
    else
    {
        ui->controlFrame->show();
    }
    sidePanelCollapsed = collapsed;
    ui->controlFrame->setMinimumWidth(0);
    const int targetWidth = collapsed ? 0 : qMax(SIDE_PANEL_MINIMUM_WIDTH, sidePanelWidth);
    setSidePanelAnimationWidth(startWidth);
    sidePanelAnimation->setStartValue(startWidth);
    sidePanelAnimation->setEndValue(targetWidth);
    sidePanelAnimation->start();
    updateSidePanelToggle();
}

void MainWindow::onSidePanelToggle()
{
    setSidePanelCollapsed(!sidePanelCollapsed);
}

void MainWindow::onSplitterMapMoved(int, int)
{
    if (sidePanelChanging || sidePanelCollapsed)
        return;
    const int width = ui->controlFrame->width();
    if (width < SIDE_PANEL_MINIMUM_WIDTH)
        setSidePanelCollapsed(true);
    else
    {
        sidePanelWidth = width;
        updateSidePanelToggle();
    }
}

void MainWindow::setSidePanelAnimationWidth(int width)
{
    sidePanelCurrentWidth = qMax(0, width);
    ui->controlFrame->setMaximumWidth(sidePanelCurrentWidth);
    ui->splitterMap->setSizes({sidePanelCurrentWidth,
        qMax(1, ui->splitterMap->width() - sidePanelCurrentWidth)});
}

void MainWindow::onSidePanelAnimationFinished()
{
    if (sidePanelCollapsed)
        ui->controlFrame->hide();
    ui->controlFrame->setMaximumWidth(QWIDGETSIZE_MAX);
    sidePanelChanging = false;
    QTimer::singleShot(0, this, &MainWindow::updateSidePanelToggle);
}

void MainWindow::updateSidePanelToggle()
{
    if (!sidePanelToggle)
        return;

    // QSplitter derives a finite cross-axis maximum from its current child
    // size hints. With the tall scrollable side toolbar this makes the entire
    // analysis/map area sit in a short, vertically centered strip. The main
    // splitter is the only central-layout item, so it must always use the
    // complete central-widget height.
    const int availableHeight = ui->centralWidget->height();
    if (availableHeight > 0 && ui->splitterMap->height() != availableHeight)
    {
        ui->gridLayout_6->setAlignment(ui->splitterMap, Qt::AlignTop);
        ui->splitterMap->setFixedHeight(availableHeight);
        ui->gridLayout_6->invalidate();
        ui->gridLayout_6->activate();
    }

    // The splitter reports a temporary width of zero while the main window is
    // being laid out.  Treating that as a user resize collapses the panel on
    // startup and can leave the nested map dock at its minimum height.  Only
    // onSplitterMapMoved() is allowed to collapse the panel.
    const bool collapsed = sidePanelCollapsed;
    sidePanelToggle->setArrowType(collapsed ? Qt::RightArrow : Qt::LeftArrow);
    sidePanelToggle->setToolTip(collapsed ? tr("Expand side panel") : tr("Collapse side panel"));
    const QRect handleGeometry = ui->splitterMap->handle(1)->geometry();
    const int boundary = collapsed ? 0 : handleGeometry.center().x();
    const QPoint splitterOrigin = ui->splitterMap->mapTo(ui->centralWidget, QPoint(0, 0));
    const int x = qBound(0, splitterOrigin.x() + boundary - sidePanelToggle->width()/2,
        qMax(0, ui->centralWidget->width() - sidePanelToggle->width()));
    const int y = qMax(2, splitterOrigin.y() +
        (ui->splitterMap->height() - sidePanelToggle->height()) / 2);
    sidePanelToggle->move(x, y);
    sidePanelToggle->raise();
}

void MainWindow::setupToolRail()
{
    // One icon per tool at the left edge. The tools stay pages of tabContainer
    // (its own tab bar is hidden), so the rail only drives setCurrentIndex().
    ui->tabContainer->tabBar()->hide();

    toolRail = new QFrame(ui->centralWidget);
    toolRail->setObjectName("toolRail");
    toolRail->setAttribute(Qt::WA_StyledBackground, true);
    toolRail->setFixedWidth(52);
    QVBoxLayout *layout = new QVBoxLayout(toolRail);
    layout->setContentsMargins(8, 12, 8, 12);
    layout->setSpacing(6);

    railGroup = new QButtonGroup(this);
    railGroup->setExclusive(true);
    const auto railButton = [this](const QString& name) {
        QToolButton *button = new QToolButton(toolRail);
        button->setObjectName("railButton");
        button->setProperty("keepIconSize", true);
        button->setFixedSize(36, 36);
        button->setIconSize(QSize(20, 20));
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(name);
        button->setAccessibleName(name);
        return button;
    };
    for (int i = 0; i < ui->tabContainer->count(); i++)
    {
        QToolButton *button = railButton(ui->tabContainer->tabText(i));
        button->setCheckable(true);
        button->setProperty("railIcon", railIconKind(ui->tabContainer->widget(i)));
        railGroup->addButton(button, i);
        layout->addWidget(button, 0, Qt::AlignHCenter);
    }
    layout->addStretch(1);

    railMenuButton = railButton(tr("Menu"));
    railMenuButton->setProperty("railIcon", "menu");
    connect(railMenuButton, &QToolButton::clicked, this, [this]() {
        mainMenu->popup(railMenuButton->mapToGlobal(QPoint(railMenuButton->width() + 8, 0)));
    });
    layout->addWidget(railMenuButton, 0, Qt::AlignHCenter);

    railSettingsButton = railButton(tr("Settings"));
    railSettingsButton->setProperty("railIcon", "settings");
    connect(railSettingsButton, &QToolButton::clicked, settingsAction, &QAction::trigger);
    layout->addWidget(railSettingsButton, 0, Qt::AlignHCenter);

    ui->gridLayout_6->addWidget(toolRail, 0, 0);

    // Second-level pages (Seeds/Locations, Locate/Statistics, ...) line up
    // with the panel edge instead of adding an indentation of their own.
    for (QTabWidget *tabs : ui->controlFrame->findChildren<QTabWidget*>())
    {
        if (tabs == ui->tabContainer)
            continue;
        for (int i = 0; i < tabs->count(); i++)
        {
            if (QLayout *layout = tabs->widget(i)->layout())
                layout->setContentsMargins(0, 0, 0, 0);
        }
    }

    connect(railGroup, &QButtonGroup::idClicked, this, &MainWindow::onRailClicked);
    connect(ui->tabContainer, &QTabWidget::currentChanged, this, &MainWindow::updateToolRail);
    updateToolRail();
}

void MainWindow::onRailClicked(int index)
{
    // a click on the active tool folds the panel away or opens it again
    if (index == ui->tabContainer->currentIndex())
    {
        setSidePanelCollapsed(!sidePanelCollapsed);
        return;
    }
    ui->tabContainer->setCurrentIndex(index);
    if (sidePanelCollapsed)
        setSidePanelCollapsed(false);
}

void MainWindow::updateToolRail()
{
    const int index = ui->tabContainer->currentIndex();
    if (railGroup)
    {
        if (QAbstractButton *button = railGroup->button(index))
            button->setChecked(true);
    }
    ui->panelTitle->setText(ui->tabContainer->tabText(index));
}

void MainWindow::setupMapOverlays()
{
    // World inputs: a compact card at the top left of the map, reachable while
    // the side panel is collapsed. The widgets keep their names from the form.
    QFrame *worldBar = ui->worldBar;
    worldBar->setParent(mapView);
    worldBar->setAttribute(Qt::WA_StyledBackground, true);
    QGraphicsDropShadowEffect *worldShadow = new QGraphicsDropShadowEffect(worldBar);
    worldShadow->setBlurRadius(18);
    worldShadow->setOffset(0, 3);
    worldShadow->setColor(QColor(0, 0, 0, 90));
    worldBar->setGraphicsEffect(worldShadow);
    ui->labelMono->hide(); // only a reference for the monospace font
    ui->seedEdit->setMinimumWidth(120);
    {   // the world types in the order of WorldType, so that index == type
        const QSignalBlocker blocker(ui->comboWorldType);
        for (int type = 0; type < WORLD_TYPE_NUM; type++)
            ui->comboWorldType->addItem(worldTypeName(type));
        ui->comboWorldType->setCurrentIndex(WORLD_DEFAULT);
    }
    updateWorldTypeAvailability(MC_DEFAULT);

    // A datapack world shows at a glance: a small chip with the number of
    // packs at the start of the card, which opens the Datapacks tool.
    packChip = new QToolButton(worldBar);
    packChip->setObjectName("packChip");
    packChip->setProperty("keepIconSize", true);
    packChip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    packChip->setIconSize(QSize(14, 14));
    packChip->setCursor(Qt::PointingHandCursor);
    packChip->hide();
    connect(packChip, &QToolButton::clicked, this, &MainWindow::openDatapacks);
    if (QBoxLayout *card = qobject_cast<QBoxLayout*>(worldBar->layout()))
        card->insertWidget(0, packChip);

    // the box shows only the level, its list the full names ("62 (Sea Level)")
    ui->comboY->view()->setMinimumWidth(ui->comboY->view()->sizeHintForColumn(0) + 36);
    worldBar->show();

    // Status strip at the bottom: biome and position under the cursor, scale,
    // zoom, origin and a compact "Go to".
    mapStatusStrip = createMapCard("mapStatusStrip", mapView);
    QHBoxLayout *strip = new QHBoxLayout(mapStatusStrip);
    strip->setContentsMargins(12, 5, 6, 5);
    strip->setSpacing(6);

    mapBiomeName = new QLabel(mapStatusStrip);
    mapBiomeName->setObjectName("mapBiomeName");
    mapBiomeName->setToolTip(tr("Biome under the cursor"));
    mapCursorPos = new QLabel(mapStatusStrip);
    mapCursorPos->setObjectName("mapCursorPos");
    mapCursorPos->setToolTip(tr("Block coordinates under the cursor"));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setStyleHint(QFont::Monospace);
    mapCursorPos->setFont(mono);
    mapScale = new QLabel(mapStatusStrip);
    mapScale->setObjectName("mapScale");
    mapScale->setToolTip(tr("Map scale: blocks per screen pixel"));

    const auto stripButton = [this](QAction *action) {
        QToolButton *button = new QToolButton(mapStatusStrip);
        button->setObjectName("mapStripButton");
        button->setProperty("keepIconSize", true);
        button->setDefaultAction(action);
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(28, 28);
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoRaise(false);
        return button;
    };
    mapZoomOut = stripButton(actzoom[1]);
    mapZoomIn = stripButton(actzoom[0]);
    mapOrigin = stripButton(acthome);

    coordinateFinder = new QFrame(mapStatusStrip);
    coordinateFinder->setObjectName("mapCoordinateFinder");
    QHBoxLayout *coordinateLayout = new QHBoxLayout(coordinateFinder);
    coordinateLayout->setContentsMargins(0, 0, 0, 0);
    coordinateLayout->setSpacing(5);
    QLabel *coordinateTitle = new QLabel(tr("Go to"), coordinateFinder);
    coordinateTitle->setObjectName("coordinateFinderTitle");
    QLabel *coordinateXLabel = new QLabel(tr("X"), coordinateFinder);
    coordinateXLabel->setObjectName("coordinateFinderAxis");
    QLineEdit *coordinateX = new QLineEdit(coordinateFinder);
    QLabel *coordinateZLabel = new QLabel(tr("Z"), coordinateFinder);
    coordinateZLabel->setObjectName("coordinateFinderAxis");
    QLineEdit *coordinateZ = new QLineEdit(coordinateFinder);
    coordinateGo = new QPushButton(coordinateFinder);
    coordinateGo->setObjectName("coordinateFinderGo");
    coordinateGo->setProperty("keepIconSize", true);
    coordinateGo->setIconSize(QSize(16, 16));
    coordinateGo->setFixedSize(28, 28);
    coordinateGo->setCursor(Qt::PointingHandCursor);
    coordinateGo->setToolTip(tr("Go to coordinates"));
    coordinateGo->setAccessibleName(tr("Go to coordinates"));

    coordinateX->setText("0");
    coordinateZ->setText("0");
    coordinateX->setValidator(new QIntValidator(-30000000, 30000000, coordinateX));
    coordinateZ->setValidator(new QIntValidator(-30000000, 30000000, coordinateZ));
    coordinateX->setToolTip(tr("X coordinate"));
    coordinateZ->setToolTip(tr("Z coordinate"));
    coordinateLayout->addWidget(coordinateTitle);
    coordinateLayout->addSpacing(2);
    coordinateLayout->addWidget(coordinateXLabel);
    coordinateLayout->addWidget(coordinateX);
    coordinateLayout->addWidget(coordinateZLabel);
    coordinateLayout->addWidget(coordinateZ);
    coordinateLayout->addWidget(coordinateGo);

    auto goToCoordinates = [this, coordinateX, coordinateZ]() {
        if (!coordinateX->hasAcceptableInput() || !coordinateZ->hasAcceptableInput())
            return;
        getMapView()->setView(coordinateX->text().toDouble(), coordinateZ->text().toDouble());
        getMapView()->setFocus();
    };
    connect(coordinateGo, &QPushButton::clicked, this, goToCoordinates);
    connect(coordinateX, &QLineEdit::returnPressed, this, goToCoordinates);
    connect(coordinateZ, &QLineEdit::returnPressed, this, goToCoordinates);
    auto updateCoordinateButton = [this, coordinateX, coordinateZ]() {
        coordinateGo->setEnabled(coordinateX->hasAcceptableInput() && coordinateZ->hasAcceptableInput());
    };
    connect(coordinateX, &QLineEdit::textChanged, this, updateCoordinateButton);
    connect(coordinateZ, &QLineEdit::textChanged, this, updateCoordinateButton);
    updateCoordinateButton();

    mapStripDivider = new QFrame(mapStatusStrip);
    mapStripDivider->setObjectName("mapStripDivider");
    mapStripDivider->setFixedSize(1, 20);

    strip->addWidget(mapBiomeName);
    strip->addSpacing(4);
    strip->addWidget(mapCursorPos);
    strip->addStretch(1);
    strip->addWidget(mapScale);
    strip->addSpacing(2);
    strip->addWidget(mapZoomOut);
    strip->addWidget(mapZoomIn);
    strip->addWidget(mapOrigin);
    strip->addSpacing(4);
    strip->addWidget(mapStripDivider);
    strip->addSpacing(4);
    strip->addWidget(coordinateFinder);
    mapStatusStrip->show();

    getMapView()->setCursorTextVisible(false);
    connect(getMapView(), &MapView::cursorInfoChanged, this, &MainWindow::updateMapStatus,
        Qt::QueuedConnection);
    updateMapStatus(QString(), 0, 0, getMapView()->getScale(), false);
    mapView->installEventFilter(this);
    layoutMapOverlays();
}

void MainWindow::updateMapStatus(const QString& biome, int x, int z, qreal scale, bool hasWorld)
{
    if (!mapBiomeName)
        return;
    QString none = tr("No seed");
    if (!hasWorld && datapackMode() && !ui->seedEdit->text().trimmed().isEmpty())
    {   // a datapack world that cannot be shown (yet)
        const std::shared_ptr<const DatapackWorld> world = datapacks->current();
        none = world ? world->shortText() : tr("Datapack world");
    }
    mapBiomeName->setText(hasWorld ? biome : none);
    mapCursorPos->setText(hasWorld
        ? QString("X %1 · Z %2").arg(x).arg(z) : QString());
    mapCursorPos->setVisible(hasWorld && mapCursorPos->property("fits").toBool());
    QString text;
    if (scale >= 1)
        text = QString("1:%1").arg(QLocale().toString(scale, 'g', 3));
    else
        text = QString("%1:1").arg(QLocale().toString(1.0 / scale, 'g', 3));
    mapScale->setText(text);
}

void MainWindow::layoutMapOverlays()
{
    if (!mapStatusStrip)
        return;
    const int margin = MAP_CARD_MARGIN;
    const int available = qMax(0, mapView->width() - 2 * margin);

    // World card: drop the field labels first, then let the seed field shrink.
    QFrame *worldBar = ui->worldBar;
    for (QLabel *label : {ui->labelMC, ui->labelSeed, ui->labelWorldType, ui->labelY})
        label->setVisible(true);
    setWorldTypeNames(false);
    ui->comboY->setMaximumWidth(QWIDGETSIZE_MAX);
    ui->seedEdit->setMinimumWidth(170);
    // A datapack world: its chip first gives up the world type picker, if
    // the packs do not use the world type anyway, later its number.
    const std::shared_ptr<const DatapackWorld> packWorld = datapacks ? datapacks->current() : nullptr;
    const bool typeUnused = datapackMode() && packWorld && packWorld->usable()
        && !packWorld->presetMatters;
    ui->comboWorldType->setVisible(true);
    if (packChip)
        packChip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    if (typeUnused && worldBar->sizeHint().width() > available)
    {
        ui->comboWorldType->setVisible(false);
        ui->labelWorldType->setVisible(false);
    }
    if (worldBar->sizeHint().width() > available)
    {
        for (QLabel *label : {ui->labelMC, ui->labelSeed, ui->labelWorldType})
            label->setVisible(false);
    }
    if (worldBar->sizeHint().width() > available)
        ui->seedEdit->setMinimumWidth(110);
    if (worldBar->sizeHint().width() > available)
        setWorldTypeNames(true);
    if (worldBar->sizeHint().width() > available)
        ui->seedEdit->setMinimumWidth(70);
    // Still too wide with the seed field at its minimum (the size hint above
    // counts the field at its preferred width): the level box keeps only its
    // number, without its label.
    if (worldBar->minimumSizeHint().width() > available)
    {
        ui->labelY->setVisible(false);
        ui->comboY->ensurePolished();
        ui->comboY->setMaximumWidth(ui->comboY->widthForContents(
            ui->comboY->fontMetrics().horizontalAdvance("-64") + 6));
    }
    if (packChip && packChip->isVisibleTo(worldBar) && worldBar->minimumSizeHint().width() > available)
        packChip->setToolButtonStyle(Qt::ToolButtonIconOnly);
    worldBar->adjustSize();
    worldBar->resize(qMin(worldBar->sizeHint().width(), available), worldBar->sizeHint().height());
    worldBar->move(margin, margin);
    worldBar->raise();

    // Status strip: give up the "Go to" fields first, then the scale and the
    // cursor position; biome, zoom and origin always stay.
    coordinateFinder->setVisible(true);
    mapStripDivider->setVisible(true);
    mapScale->setVisible(true);
    mapCursorPos->setProperty("fits", true);
    mapCursorPos->setVisible(!mapCursorPos->text().isEmpty());
    const auto needed = [this]() {
        mapStatusStrip->layout()->activate();
        return mapStatusStrip->layout()->minimumSize().width() + 150;
    };
    if (needed() > available)
    {
        coordinateFinder->setVisible(false);
        mapStripDivider->setVisible(false);
    }
    if (needed() > available)
        mapScale->setVisible(false);
    if (needed() > available)
    {
        mapCursorPos->setProperty("fits", false);
        mapCursorPos->setVisible(false);
    }
    const int height = mapStatusStrip->sizeHint().height();
    mapStatusStrip->setGeometry(margin, mapView->height() - height - margin, available, height);
    mapStatusStrip->raise();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == mapView && event->type() == QEvent::Resize)
    {
        layoutMapOverlays();
        // the panel edge moves with the map, e.g. when the layer sidebar
        // changes its width
        QTimer::singleShot(0, this, &MainWindow::updateSidePanelToggle);
    }
    else if (watched == ui->splitterSearch && event->type() == QEvent::Resize)
    {
        QTimer::singleShot(0, this, &MainWindow::fitConditionsHeight);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::fitConditionsHeight()
{
    QListWidget *list = activeConditionList();
    QList<int> sizes = ui->splitterSearch->sizes();
    if (!list || conditionsHeightManual || sizes.size() != 2 || !list->isVisible()
        || !ui->collapseConstraints->toggleButton->isChecked())
        return;
    const int total = sizes[0] + sizes[1];
    if (total <= 0)
        return;
    // the height of all rows, as the list lays them out at its width
    int rows = 0;
    // (a row that wraps has its height in the layout of the list, which the
    // size hint does not always know yet)
    for (int i = 0, n = list->count(); i < n; i++)
        rows += qMax(list->sizeHintForRow(i), list->visualRect(list->model()->index(i, 0)).height())
            + 2 * list->spacing();
    // an empty list shows its hint, which may wrap
    if (list->count() == 0)
    {
        if (QLabel *hint = list->viewport()->findChild<QLabel*>("emptyHint",
                Qt::FindDirectChildrenOnly))
        {
            const int width = qMax(1, list->viewport()->width() - 24);
            rows = qMax(rows, hint->heightForWidth(width) + 16);
        }
    }
    const int listWant = qMax(list->minimumHeight(), rows + 2 * list->frameWidth() + 4);
    // everything else in the section: heading, quick filter row, buttons
    // (at least what the layout needs besides the list: a section that was
    // squeezed below its minimum would otherwise stay squeezed)
    const int around = qMax(sizes[0] - list->height(),
        ui->collapseConstraints->minimumSizeHint().height() - list->minimumHeight());
    // the former split stays the least; the matching seeds keep at least a
    // third of the height, and never less than a few rows
    const int least = total / 4;
    const int most = total - qMax(220, total / 3);
    const int want = qBound(least, around + listWant, qMax(least, most));
    if (qAbs(want - sizes[0]) <= 1)
        return;
    ui->splitterSearch->setSizes({want, total - want});
}

void MainWindow::refreshChromeIcons()
{
    const QPalette pal = QApplication::palette();
    const QColor muted = pal.color(QPalette::PlaceholderText);
    const QColor text = pal.color(QPalette::Text);
    const QColor accent = pal.color(QPalette::Highlight);

    if (railGroup)
    {
        QList<QAbstractButton*> rail = railGroup->buttons();
        rail << railMenuButton << railSettingsButton;
        for (QAbstractButton *button : rail)
        {
            if (button)
                button->setIcon(chromeIcon(button->property("railIcon").toString(),
                    muted, 20, accent));
        }
    }
    settingsAction->setIcon(chromeIcon("settings", muted));
    menuAction->setIcon(chromeIcon("menu", muted));
    acthome->setIcon(chromeIcon("origin", text, 16));
    actzoom[0]->setIcon(chromeIcon("plus", text, 16));
    actzoom[1]->setIcon(chromeIcon("minus", text, 16));
    if (coordinateGo)
        coordinateGo->setIcon(chromeIcon("go", text, 16));
    ui->toolBar->setChromeColor(muted);
    if (tabDatapacks)
        tabDatapacks->refreshTheme();
    updateDatapackMode(); // the pack chip is drawn in a theme color

    // the layer icons: their grey "off" state depends on the background
    const int size = ui->toolBar->iconSize().height();
    for (QAction *action : layerActions)
        action->setIcon(mapLayerIcon(action->property("layerIcon").toString(), size));
}

// The views and dimensions are glyphs (muted while off, in the text color
// while on), the structures their pixel-art pictures.
QIcon MainWindow::mapLayerIcon(const QString& rcbase, int size) const
{
    if (hasGlyph(rcbase))
        return glyphIcon(rcbase, true);
    return layerIcon(rcbase, size, darkTheme);
}

QAction *MainWindow::addMapAction(int opt)
{
    QString tip = tr("Show %1").arg(mapopt2display(opt));
    QString label = mapopt2display(opt);
    // Keep the fixed vertical toolbar compact. Tooltips and the toolbar editor
    // still use the full Minecraft structure names from mapopt2display().
    switch (opt)
    {
    case D_SLIME:       label = tr("Slime"); break;
    case D_DESERT:      label = tr("Pyramid"); break;
    case D_JUNGLE:      label = tr("Temple"); break;
    case D_MANSION:     label = tr("Mansion"); break;
    case D_MONUMENT:    label = tr("Monument"); break;
    case D_OCEANRUIN:   label = tr("Ruins"); break;
    case D_TREASURE:    label = tr("Treasure"); break;
    case D_GEODE:       label = tr("Geode"); break;
    case D_OUTPOST:     label = tr("Outpost"); break;
    case D_ANCIENTCITY: label = tr("Ancient City"); break;
    case D_TRAILRUINS:  label = tr("Trail Ruins"); break;
    case D_CHAMBERS:    label = tr("Chambers"); break;
    case D_CAMP:        label = tr("Abandoned Camp"); break;
    case D_OREVEIN:     label = tr("Ore Veins"); break;
    case D_PORTAL:      label = tr("Portal"); break;
    case D_FORTESS:     label = tr("Fortress"); break;
    case D_BASTION:     label = tr("Bastion"); break;
    case D_ENDCITY:     label = tr("End City"); break;
    case D_ENDCITYSHIP: label = tr("End City (Ship)"); break;
    case D_GATEWAY:     label = tr("Gateway"); break;
    default: break;
    }
    QAction *action = addMapAction(mapopt2str(opt), tip, label);
    action->connect(action, &QAction::toggled, [=](bool state){ this->onActionMapToggled(opt, state); });
    saction[opt] = action;
    return action;
}

QAction *MainWindow::addMapAction(QString rcbase, QString tip, QString toolbarLabel)
{
    // crisp pixel art in color while shown, a calm grey while hidden; the
    // icons are rebuilt for the theme in refreshChromeIcons()
    QAction *action = new QAction(mapLayerIcon(rcbase, ui->toolBar->iconSize().height()),
        tip, this);
    action->setProperty("layerIcon", rcbase);
    if (!toolbarLabel.isEmpty())
        action->setProperty("toolbarLabel", toolbarLabel);
    action->setCheckable(true);
    ui->toolBar->addAction(action);
    layerActions.append(action);
    return action;
}

MapView* MainWindow::getMapView()
{
    return mapView;
}

bool MainWindow::getSeed(WorldInfo *wi, bool applyrand)
{
    bool ok = true;
    const std::string& mcs = ui->comboBoxMC->currentText().toStdString();
    wi->mc = str2mc(mcs.c_str());
    if (wi->mc == MC_UNDEF)
    {
        if (applyrand)
            qDebug() << "Unknown MC version: " << mcs.c_str();
        wi->mc = MC_DEFAULT;
        ok = false;
    }

    const QString seedText = ui->seedEdit->text().trimmed();
    if (seedText.isEmpty())
    {
        wi->seed = 0;
        ok = false;
    }
    else
    {
        const int v = str2seed(seedText, &wi->seed);
        if (v == S_RANDOM && applyrand)
            ui->seedEdit->setText(QString::asprintf("%" PRId64, (int64_t)wi->seed));
    }

    wi->setWorldType(ui->comboWorldType->currentIndex());
    wi->y = ui->comboY->currentText().section(' ', 0, 0).toInt();
    // with data packs loaded, the world is a datapack world
    wi->dp = datapacks ? datapacks->resolve(wi->mc, wi->worldType()) : nullptr;
    // no Amplified before 1.18, and the surface level of the world type
    wi->normalize();

    return ok;
}

bool MainWindow::setSeed(WorldInfo wi, int dim)
{
    if (dim == DIM_OVERWORLD)
        dimactions[0]->setChecked(true);
    if (dim == DIM_NETHER)
        dimactions[1]->setChecked(true);
    else if (dim == DIM_END)
        dimactions[2]->setChecked(true);
    else
        dim = getDim();

    // The datapack world belongs to the version and world type that are set
    // here, whatever the caller had (it may have changed the version).
    wi.dp = datapacks ? datapacks->resolve(wi.mc, wi.worldType()) : nullptr;
    wi.normalize(); // no Amplified before 1.18, the surface level of the world type

    const QList<QAction*> hist = ui->menuHistory->actions();

    if (hist.empty() || hist.front()->data().toULongLong() != wi.seed)
    {
        QAction *rm = nullptr;
        if (hist.size() >= 16)
            rm = hist.back();
        for (QAction *act : hist)
            if (act->data().toULongLong() == wi.seed)
                rm = act;
        if (rm)
        {
            ui->menuHistory->removeAction(rm);
            rm->deleteLater();
        }
        QString s = QString::asprintf("%" PRId64, wi.seed);
        QAction *act = new QAction(s, this);
        act->setData(QVariant::fromValue(wi.seed));
        act->connect(act, &QAction::triggered, [=](){ this->onActionHistory(act); });
        ui->menuHistory->insertAction(hist.empty() ? 0 : hist.first(), act);
        ui->menuHistory->setEnabled(true);
    }

    // temporarily disable UI to prevent recursive setSeed() updates
    ui->comboBoxMC->setEnabled(false);
    ui->comboWorldType->setEnabled(false);
    ui->seedEdit->setEnabled(false);
    ui->comboY->setEnabled(false);

    updateLevelPicker(&wi);
    showWorldType(wi.mc, wi.worldType());
    int i, n = ui->comboY->count();
    for (i = 0; i < n; i++)
        if (ui->comboY->itemText(i).section(' ', 0, 0).toInt() == wi.y)
            break;
    if (i >= n)
        ui->comboY->addItem(QString::number(wi.y));
    ui->comboY->setCurrentIndex(i);

    // A version that the list hides (e.g. 1.16.1 without experimental
    // versions) has to be listed, otherwise the box keeps the old version and
    // everything that reads it back (search, results, saving) uses that one.
    if (ui->comboBoxMC->findText(mc2str(wi.mc)) < 0)
    {
        setMCList(g_extgen.experimentalVers, wi.mc);
        ui->comboBoxMC->setEnabled(false);
    }
    ui->comboBoxMC->setCurrentText(mc2str(wi.mc));
    ui->seedEdit->setText(QString::asprintf("%" PRId64, (int64_t)wi.seed));
    updateTerrainAvailability(wi.mc);
    formCond->updateQuickFilters(wi.mc);
    // A datapack world has the biome map of its own dimension only: the
    // layer and the dimension picked in the sidebar stay as they are and
    // apply again without packs.
    if (wi.dp)
    {   // the biome map, or the height and terrain layers (with the display
        // mode of the height layer)
        LayerOpt dpopt;
        if ((lopt.mode == LOPT_HEIGHT || lopt.mode == LOPT_TERRAIN) && wi.dp->usable()
            && wi.dp->dimensionInfo()->terrain)
        {
            dpopt.mode = lopt.mode;
            dpopt.disp[lopt.mode] = lopt.disp[lopt.mode];
        }
        getMapView()->setSeed(wi, DIM_OVERWORLD, dpopt);
    }
    else
        getMapView()->setSeed(wi, dim, lopt);

    ui->comboBoxMC->setEnabled(true);
    // a pack with its own dimension file does not use the world type
    ui->comboWorldType->setEnabled(!(wi.dp && wi.dp->usable() && !wi.dp->presetMatters));
    ui->seedEdit->setEnabled(true);
    ui->comboY->setEnabled(true);

    if (ISaveTab *tab = dynamic_cast<ISaveTab*>(ui->tabContainer->currentWidget()))
        tab->refresh();
    if (ISaveTab *tab = dynamic_cast<ISaveTab*>(ui->tabContainerSearch->currentWidget()))
        tab->refresh();
    updateStructureActions();
    return true;
}

// Whether a map option has anything to show in this dimension and version.
static bool mapOptionExists(int opt, int dim, int mc)
{
    switch (opt)
    {
    case D_GRID:
        return true;
    case D_FORTESS:
    case D_BASTION:
        if (dim != DIM_NETHER)
            return false;
        break;
    case D_ENDCITY:
    case D_ENDCITYSHIP:
    case D_GATEWAY:
        if (dim != DIM_END)
            return false;
        break;
    case D_PORTAL: // also controls the ruined portals of the Nether
        if (dim == DIM_END)
            return false;
        if (dim == DIM_NETHER)
            opt = D_PORTALN;
        break;
    default:
        if (dim != DIM_OVERWORLD)
            return false;
        break;
    }
    if (opt == D_OREVEIN)
        return mc >= MC_1_18;
    int stype = mapopt2stype(opt);
    if (stype < 0)
        return true; // slime chunks, spawn and strongholds
    StructureConfig sconf;
    return getStructureConfig_override(stype, mc, &sconf);
}

void MainWindow::updateStructureActions()
{
    WorldInfo wi;
    getSeed(&wi, false);
    if (wi.mc <= MC_UNDEF)
        wi.mc = MC_NEWEST; // no version picked yet (first start)
    int dim = getDim();
    const bool datapack = datapackMode();
    // A datapack world: its vanilla structures use the entries of the
    // built-in ones (villages, fortresses, ...), as far as it has them.
    const std::shared_ptr<const DatapackWorld> packWorld = datapack ? datapacks->current() : nullptr;
    const std::shared_ptr<const DpStructureCatalog> catalog = dpStructureCatalog(packWorld);
    std::vector<int> found(D_STRUCT_NUM, 0), supported(D_STRUCT_NUM, 0);
    if (catalog)
    {
        dim = dpEngineDim(*packWorld);
        for (const DpStructureEntry& e : catalog->entries)
        {
            if (e.vanillaOpt == D_NONE)
                continue;
            const int opt = e.vanillaOpt == D_PORTALN ? D_PORTAL : e.vanillaOpt;
            found[opt]++;
            supported[opt] += e.supported;
        }
    }
    for (int opt = 0; opt < (int) saction.size(); opt++)
    {
        QAction *act = saction[opt];
        if (!act)
            continue;
        const bool exists = mconfig.enabled(opt) && mapOptionExists(opt, dim, wi.mc);
        const QString tip = tr("Show %1").arg(mapopt2display(opt));
        if (!datapack || opt == D_GRID || opt == D_SLIME)
        {
            act->setVisible(exists);
            act->setEnabled(true);
            act->setToolTip(tip);
            continue;
        }
        if (catalog && found[opt])
        {   // a structure of the datapack world
            act->setVisible(exists);
            act->setEnabled(supported[opt] > 0);
            act->setToolTip(supported[opt] > 0 ? tip
                : tr("%1: the datapacks change this structure in a way that is not "
                     "supported (see Pack structures).").arg(mapopt2display(opt)));
            continue;
        }
        // spawn, geodes, ore veins, ... come from the built-in generator, and
        // structures the datapack world does not have
        act->setVisible(exists);
        act->setEnabled(false);
        if (opt == D_SPAWN)
            act->setToolTip(tr("%1: not available for datapack worlds yet (the world spawn "
                "depends on the terrain and the game's spawn search).").arg(mapopt2display(opt)));
        else if (catalog)
            act->setToolTip(tr("%1: not in this datapack world, or not available for "
                "datapack worlds yet.").arg(mapopt2display(opt)));
        else
            act->setToolTip(tr("%1: available once the datapack world is loaded.")
                .arg(mapopt2display(opt)));
    }
    if (dpStructDialog)
        dpStructDialog->refresh();
    if (dpStructAction)
    {
        int pack = 0, packSupported = 0;
        if (catalog)
            for (const DpStructureEntry& e : catalog->entries)
                if (e.vanillaOpt == D_NONE)
                {
                    pack++;
                    packSupported += e.supported;
                }
        dpStructAction->setVisible(datapack && catalog);
        dpStructAction->setEnabled(catalog != nullptr);
        dpStructAction->setToolTip(!catalog ? QString()
            : pack == 0 ? tr("This datapack world has only vanilla structures; they use the "
                "entries below. Click to see the list.")
            : tr("%n structure(s) of the datapacks (%1 supported). Click to pick which ones "
                "the map shows.", nullptr, pack).arg(packSupported));
    }
}

bool MainWindow::datapackMode() const
{
    return datapacks && datapacks->active();
}

int MainWindow::getDim()
{
    // a datapack world has its own dimensions (see the Datapacks tool); for
    // everything else it counts as the Overworld
    return datapackMode() ? 0 : toolbarDim();
}

int MainWindow::toolbarDim()
{
    if (!dimgroup)
        return 0;
    QAction *active = dimgroup->checkedAction();
    if (active == dimactions[1])
        return -1; // nether
    if (active == dimactions[2])
        return +1; // end
    return 0;
}

void MainWindow::saveSettings()
{
    QSettings settings(appSettingsId(), appSettingsId());

    settings.setValue("mainwindow/maximized", isMaximized());
    settings.setValue("mainwindow/size", size());
    settings.setValue("mainwindow/pos", pos());
    settings.setValue("mainwindow/prevdir", prevdir);
    settings.setValue("ui/rightSidebarCollapsed", ui->toolBar->isCollapsed());

    config.save(settings);

    g_extgen.save(settings);

    saveTabs(settings);

    WorldInfo wi;
    getSeed(&wi, false);
    wi.save(settings);
    if (ui->comboBoxMC->currentIndex() < 0)
        settings.remove("map/mc");
    if (ui->seedEdit->text().isEmpty())
        settings.remove("map/seed");
    // The level of a datapack world belongs to its dimension: the level of
    // the built-in worlds is kept next to it.
    settings.remove("datapacks/y");
    if (levelsDatapack)
    {
        settings.setValue("datapacks/y", wi.y);
        if (vanillaLevel != INT_MIN)
            settings.setValue("map/y", vanillaLevel);
    }
    datapacks->save(settings);
    settings.setValue("datapacks/highlight", getMapView()->getDatapackHighlights());
    settings.setValue("datapacks/structures", getMapView()->getDatapackStructures());
    settings.setValue("map/dim", toolbarDim());
    settings.setValue("map/terrain", lopt.mode == LOPT_TERRAIN);
    settings.setValue("map/x", getMapView()->getX());
    settings.setValue("map/z", getMapView()->getZ());
    settings.setValue("map/scale", getMapView()->getScale());
    for (int stype = 0; stype < D_STRUCT_NUM; stype++)
    {
        QString s = QString("map/show_") + mapopt2str(stype);
        settings.setValue(s, getMapView()->getShow(stype));
    }
    // the session is saved by closeEvent(), which can report a failure
}

void MainWindow::saveTabs(QSettings& settings)
{
    for (QTabWidget *tabs : {ui->tabContainer, ui->tabContainerSearch})
    {
        for (int i = 0; i < tabs->count(); i++)
        {
            // a tab that never read its settings would overwrite them with
            // its defaults
            ISaveTab *tab = dynamic_cast<ISaveTab*>(tabs->widget(i));
            if (tab && loadedTabs.contains(tab))
                tab->save(settings);
        }
    }
}


void MainWindow::loadSettings()
{
    QSettings settings(appSettingsId(), appSettingsId());
    const bool hasStoredSeed = settings.contains("map/seed");
    const bool hasStoredVersion = settings.contains("map/mc");

    // the layer sidebar starts as a slim icon column
    setRightSidebarCollapsed(settings.value("ui/rightSidebarCollapsed", true).toBool(), false);

    getMapView()->deleteWorld();

    onUpdateConfig();

#if WASM
    showFullScreen();
#else
    if (settings.value("mainwindow/maximized", isMaximized()).toBool()) {
        showMaximized();
    } else if (config.restoreWindow) {
        resize(settings.value("mainwindow/size", size()).toSize());
        move(settings.value("mainwindow/pos", pos()).toPoint());
    }
#endif
    prevdir = settings.value("mainwindow/prevdir", prevdir).toString();

    qreal x = getMapView()->getX();
    qreal z = getMapView()->getZ();
    qreal scale = getMapView()->getScale();
    x = settings.value("map/x", x).toDouble();
    z = settings.value("map/z", z).toDouble();
    scale = settings.value("map/scale", scale).toDouble();
    mapGoto(x, z, scale);

    for (int sopt = 0; sopt < D_STRUCT_NUM; sopt++)
    {
        bool show = getMapView()->getShow(sopt);
        QString soptstr = QString("map/show_") + mapopt2str(sopt);
        show = settings.value(soptstr, show).toBool();
        if (saction[sopt])
            saction[sopt]->setChecked(show);
        getMapView()->setShow(sopt, show);
    }
    // Ruined portals use one visible toolbar action for both dimensions.
    // Keep the hidden Nether option in sync with that canonical action.
    getMapView()->setShow(D_PORTALN, getMapView()->getShow(D_PORTAL));

    g_extgen.load(settings);
    setMCList(g_extgen.experimentalVers);

    // the data packs, before the first world is set up; they are loaded
    // once the stored version and world type are set (setSeed below)
    std::unique_ptr<DatapackManager::Hold> holdLoads(new DatapackManager::Hold(datapacks));
    datapacks->load(settings);
    datapackLevel = settings.value("datapacks/y", INT_MIN).toInt();
    getMapView()->setDatapackHighlights(settings.value("datapacks/highlight").toStringList());
    getMapView()->setDatapackStructures(settings.value("datapacks/structures").toStringList());

    WorldInfo wi;
    // NOTE: version can be wrong when the mc-enum changes, but the session file should correct it
    getSeed(&wi, false);
    wi.load(settings);
    int dim = settings.value("map/dim", getDim()).toInt();
    if (settings.value("map/terrain", false).toBool())
    {
        lopt.mode = LOPT_TERRAIN;
        syncLayerActions();
    }

    onUpdateMapConfig();
    setSeed(wi, dim);
    holdLoads.reset();

    if (!hasStoredVersion)
    {
        const QSignalBlocker blocker(ui->comboBoxMC);
        ui->comboBoxMC->setCurrentIndex(-1);
    }

    if (!hasStoredSeed)
    {
        ui->seedEdit->clear();
        getMapView()->deleteWorld();
        getMapView()->update(2);
    }

    // The session does not depend on the map seed: a search can run with an
    // empty seed field (as on the first start).
    restoringSession = true;
    if (!opensession.isEmpty())
    {
        if (QFile::exists(opensession))
            loadSession(opensession, false);
        else
            qWarning("Session file not found: %s", qUtf8Printable(opensession));
    }
    else if (config.restoreSession && QFile::exists(sessionpath))
    {
        loadSession(sessionpath, false);
    }
    restoringSession = false;
}

bool MainWindow::saveSession(QString path, bool quiet, QString *error)
{
    // Write to a temporary file and replace the old session only when the new
    // one is complete: a full disk or a crash must not destroy the last good
    // session. The replaced session is kept as a backup ("<file>.bak").
    QString err;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        err = file.errorString();
    }
    else
    {
        QTextStream stream(&file);
        const bool written = saveSession(stream);
        stream.flush();
        if (!written || stream.status() != QTextStream::Ok || file.error() != QFileDevice::NoError)
        {
            err = file.errorString();
            if (err.isEmpty() || file.error() == QFileDevice::NoError)
                err = tr("The file could not be written completely.");
            file.cancelWriting(); // discarded, the old session stays
        }
        else
        {
            const QString backup = path + ".bak";
            if (QFile::exists(path))
            {
                QFile::remove(backup);
                QFile::copy(path, backup);
            }
            if (!file.commit())
                err = file.errorString();
        }
    }

    if (err.isEmpty())
        return true;
    if (error)
        *error = QString("\"%1\": %2").arg(QDir::toNativeSeparators(path), err);
    if (!quiet)
        warn(this, tr("Failed to save the session to:\n\"%1\"\n%2")
            .arg(QDir::toNativeSeparators(path), err));
    return false;
}

bool MainWindow::loadSession(QString fnam, bool keepresults)
{
    QFile file(fnam);
    if (!file.open(QIODevice::ReadOnly))
    {
        warn(this, QApplication::tr("Failed to open session file:\n\"%1\"").arg(fnam));
        return false;
    }

    QTextStream stream(&file);
    return loadSession(stream, keepresults, false);
}

bool MainWindow::saveSession(QTextStream& stream)
{
    Session session;
    session.sc = formControl->getSearchConfig();
    session.gen48 = formGen48->getConfig(false);
    session.cv = formCond->getConditions();
    session.dpcv = formDpCond->getConditions();
    session.slist = formControl->getResults();
    getSeed(&session.wi);
    // "#Datapacks" says which kind of world the results and the progress
    // belong to; without either, the world that is shown
    const ResultsWorld& results = formControl->resultsWorld();
    if (results.set && formControl->hasResultsOrProgress())
    {
        session.datapack = results.datapack;
        session.dppacks = results.packs;
        session.dpdimension = results.dimension;
    }
    else if (datapackMode())
    {
        const ResultsWorld shown = ResultsWorld::of(session.wi);
        session.datapack = true;
        session.dppacks = shown.packs;
        session.dpdimension = shown.dimension;
    }
    return session.save(this, stream);
}

bool MainWindow::loadSession(QTextStream& stream, bool keepresults, bool quiet)
{
    Session session;
    // build current session before loading to keep unspecified values the same
    getSeed(&session.wi);
    session.sc = formControl->getSearchConfig();
    session.gen48 = formGen48->getConfig(false);
    // ...except the progress, which belongs to the filters being replaced:
    // a filter template without "#Progress" starts from the beginning
    session.sc.startseed = 0;

    if (!session.load(this, stream, quiet))
        return false;

    // The world generation settings the session was made with (structure
    // salts, terrain estimate) belong to its filters and results like the
    // version does. Lines the file does not contain keep the current values.
    if (!session.sc.extgen.sameGeneration(g_extgen))
    {
        applyExtGen(session.sc.extgen);
        getMapView()->deleteWorld(); // regenerate with the new settings
        if (!quiet)
        {
            info(this, tr("This session was made with other world generation settings "
                "(structure salt overrides or the terrain estimate). They have been "
                "applied, so that the search continues as before."));
        }
    }

    if (ui->seedEdit->text().trimmed().isEmpty())
    {   // A session has no map seed: keep the field empty instead of showing
        // seed 0, but take over the version and the world type.
        const QString mcs = mc2str(session.wi.mc);
        if (ui->comboBoxMC->findText(mcs) < 0)
            setMCList(g_extgen.experimentalVers, session.wi.mc);
        {
            const QSignalBlocker blockVersion(ui->comboBoxMC);
            const QSignalBlocker blockWorldType(ui->comboWorldType);
            ui->comboBoxMC->setCurrentText(mcs);
            showWorldType(session.wi.mc, session.wi.worldType());
        }
        updateMapSeed();
    }
    else
    {
        setSeed(session.wi);
    }

    formCond->clearConditions();
    for (Condition &c : session.cv)
    {
        QListWidgetItem *item = new QListWidgetItem();
        formCond->addItemCondition(item, c);
    }
    formDpCond->setConditions(session.dpcv);
    // The world the results and the progress of the session belong to: a
    // search in another world (other kind, packs or dimension) starts from
    // the beginning and replaces them (see FormSearchControl::prepareResultsFor).
    const bool sessionResults = !session.slist.empty() || session.sc.startseed != 0;
    WorldInfo shownNow;
    getSeed(&shownNow, false);
    const ResultsWorld shownWorld = ResultsWorld::of(shownNow);
    ResultsWorld sessionWorld;
    sessionWorld.set = true;
    sessionWorld.mc = session.wi.mc;
    sessionWorld.worldType = session.wi.worldType();
    if (session.datapack)
    {
        sessionWorld.datapack = true;
        sessionWorld.preset = datapackPreset(sessionWorld.worldType);
        // files written before "#DpDimension" existed: the dimension shown now
        sessionWorld.dimension = session.dpdimension.isEmpty()
            ? shownWorld.dimension : session.dpdimension;
        if (sessionWorld.dimension.isEmpty())
            sessionWorld.dimension = datapacks->dimension();
        sessionWorld.packs = session.dppacks;
    }
    if (!quiet)
    {   // a session of a datapack world and the world that is shown now
        const QString restart = sessionResults ? tr("Its results and search progress belong to "
            "that world: a search here starts from the beginning and replaces them.") : QString();
        if (session.datapack && !datapackMode())
        {
            QStringList names;
            for (const QString& path : std::as_const(session.dppacks))
                names.append(QFileInfo(path).fileName());
            info(this, (tr("This session was saved with datapacks (%1). Its datapack conditions "
                "are kept, but a search with them needs the packs: add or switch them on in "
                "Datapacks. Without packs the Seed Finder uses the vanilla conditions.")
                .arg(names.isEmpty() ? tr("none listed") : names.join(", ")) + " " + restart).trimmed());
        }
        else if (datapackMode() && !session.datapack
            && (!session.cv.empty() || (sessionResults && !restoringSession)))
        {
            info(this, (tr("This session was saved for a vanilla world. Datapacks are loaded, "
                "so the Seed Finder shows the datapack conditions; switch the packs off to "
                "search with the vanilla conditions of the session.") + " " + restart).trimmed());
        }
        else if (datapackMode() && session.datapack && sessionResults && !restoringSession
            && sessionWorld != shownWorld)
        {
            info(this, tr("This session was saved with other datapacks or another dimension "
                "(%1). Its results stay listed, but a search with the packs loaded now starts "
                "from the beginning and replaces them.")
                .arg((sessionWorld.packs.isEmpty() ? tr("no packs listed")
                    : sessionWorld.packs.join(", ")) + ", " + sessionWorld.dimension));
        }
        else if (datapackMode() && datapacks->current() && datapacks->current()->usable())
        {
            const DpBiomeNames names{datapacks->current()};
            QStringList missing;
            for (const DpCondition& c : session.dpcv)
            {
                for (const QString& id : c.biomes)
                    if (!names.has(id) && !missing.contains(id))
                        missing.append(id);
                for (const QString& id : c.structures)
                    if (names.structureIndex(id) < 0 && !missing.contains(id))
                        missing.append(id);
            }
            if (!missing.isEmpty())
                warn(this, tr("Some biomes or structures of the datapack conditions are not in "
                    "the loaded datapacks: %1. The search leaves them out.").arg(missing.join(", ")));
        }
    }

    formGen48->setConfig(session.gen48, quiet);
    formGen48->updateCount();

    if (!keepresults)
        formControl->on_buttonClear_clicked();
    formControl->setSearchConfig(session.sc, quiet);
    formControl->searchResultsAdd(session.slist, false);
    formControl->searchProgressReset();
    // (after searchResultsAdd(), which takes loaded seeds for the shown world)
    if (!keepresults || !session.slist.empty())
        formControl->setResultsWorld(sessionWorld);

    return true;
}

void MainWindow::updateMapSeed()
{
    WorldInfo wi;
    const bool hasSeed = !ui->seedEdit->text().trimmed().isEmpty();
    if (getSeed(&wi, hasSeed))
        setSeed(wi);
    else if (!hasSeed)
    {
        getMapView()->deleteWorld();
        getMapView()->update(2);
    }

    bool state;
    state = (wi.mc <= MC_B1_7);
    ui->actionNoOceans->setEnabled(state);
    ui->actionBetaTemperature->setEnabled(state);
    ui->actionBetaHumidity->setEnabled(state);
    state = (wi.mc >= MC_1_13 && wi.mc <= MC_1_17);
    ui->actionRiver->setEnabled(state);
    ui->actionOceanTemp->setEnabled(state);
    state = (wi.mc >= MC_1_18);
    ui->actionParaTemperature->setEnabled(state);
    ui->actionParaHumidity->setEnabled(state);
    ui->actionParaContinentalness->setEnabled(state);
    ui->actionParaErosion->setEnabled(state);
    ui->actionParaDepth->setEnabled(state);
    ui->actionParaWeirdness->setEnabled(state);
    updateTerrainAvailability(wi.mc);
    // without a seed, setSeed() did not run: the picker still has to follow
    // the version
    updateWorldTypeAvailability(wi.mc);
    formCond->updateQuickFilters(wi.mc);

    ui->actionAddShadow->setEnabled(wi.mc <= MC_1_17);
    ui->actionOpenShadow->setEnabled(wi.mc <= MC_1_17);

    // with data packs loaded, only the biome map is right
    updateDatapackMode();
    if (!hasSeed)
        updateStructureActions();

    emit mapUpdated();
    update();
}

void MainWindow::setDockable(bool dockable)
{
    if (dockable)
    {   // reset to default
        QWidget *title = dock->titleBarWidget();
        dock->setTitleBarWidget(nullptr);
        delete title;
        //dock->resize(1920, 1080);
    }
    else
    {   // add a dummy widget with a layout to hide the title bar
        // and avoid a warning about negative size widget
        QWidget *title = new QWidget(this);
        QHBoxLayout *l = new QHBoxLayout(title);
        l->setContentsMargins(0, 0, 0, 0);
        title->setLayout(l);
        dock->setTitleBarWidget(title);
        dock->setFloating(false);
    }
}

void MainWindow::setMCList(bool experimental, int extramc)
{
    WorldInfo wi;
    if (ui->comboBoxMC->count())
        getSeed(&wi, false);
    else
        wi.mc = MC_DEFAULT;
    QStringList mclist;
    for (int mc = MC_NEWEST; mc > MC_UNDEF; mc--)
    {
        if (!experimental && mc != wi.mc && mc != extramc)
        {
            if (mc <= MC_1_0 || mc == MC_1_16_1 || mc == MC_1_19_2)
                continue;
        }
        mclist.append(mc2str(mc));
    }
    const QString s = mc2str(wi.mc);
    ui->comboBoxMC->setEnabled(false);
    ui->comboBoxMC->clear();
    ui->comboBoxMC->addItems(mclist);
    ui->comboBoxMC->setCurrentText(s);
    ui->comboBoxMC->setEnabled(true);
}

void MainWindow::mapGoto(qreal x, qreal z, qreal scale)
{
    getMapView()->setView(x, z, scale);
}

void MainWindow::mapZoom(qreal factor)
{
    getMapView()->zoom(factor);
}

void MainWindow::setBiomeColorRc(QString rc)
{
    config.biomeColorPath = rc;
    onBiomeColorChange();
}

void MainWindow::setProgressIndication(double value)
{
    int v = (int) floor(10000 * value);
    if (v == progval)
        return;
    progval = v;

#if WITH_DBUS
    auto message = QDBusMessage::createSignal("/", "com.canonical.Unity.LauncherEntry", "Update");
    QVariantMap properties;
    if (value >= 0 && value <= 1)
    {
        properties.insert("progress-visible", true);
        properties.insert("progress", value);
    }
    else
    {
        properties.insert("progress-visible", false);
        properties.insert("progress", 0);
    }
    message << QString("application://%1.desktop").arg(QGuiApplication::desktopFileName()) << properties;
    QDBusConnection::sessionBus().send(message);
#endif
}

void MainWindow::on_comboBoxMC_currentIndexChanged(int)
{
    if (ui->comboBoxMC->isEnabled() && ui->comboBoxMC->count())
        updateMapSeed();
}
void MainWindow::on_seedEdit_editingFinished()
{
    if (ui->seedEdit->isEnabled())
        updateMapSeed();
}
void MainWindow::on_comboWorldType_currentIndexChanged(int)
{
    if (ui->comboWorldType->isEnabled())
        updateMapSeed();
}
void MainWindow::on_comboY_currentIndexChanged(int)
{
    if (ui->comboY->isEnabled())
        updateMapSeed();
}

void MainWindow::on_actionSave_triggered()
{
#if WASM
    QByteArray content;
    QTextStream stream(&content);
    saveSession(stream);
    QFileDialog::saveFileContent(content, "session.txt");
#else
    QString filter = tr("Session files (*.session *.txt);;Any files (*)");
    QString fnam = QFileDialog::getSaveFileName(this, tr("Save session"), prevdir, filter);
    if (!fnam.isEmpty())
    {
        QFileInfo finfo(fnam);
        prevdir = finfo.absolutePath();
        saveSession(fnam, false);
    }
#endif
}

void MainWindow::on_actionLoad_triggered()
{
    QString filter = tr("Session files (*.session *.txt);;Any files (*)");
#if WASM
    auto fileOpenCompleted = [=](const QString &fnam, const QByteArray &content) {
        if (!fnam.isEmpty()) {
            QTextStream stream(content);
            loadSession(stream, false, false);
        }
    };
    QFileDialog::getOpenFileContent(filter, fileOpenCompleted);
#else
    QString fnam = QFileDialog::getOpenFileName(this, tr("Load session"), prevdir, filter);
    if (!fnam.isEmpty())
    {
        QFileInfo finfo(fnam);
        prevdir = finfo.absolutePath();
        loadSession(fnam, false);
    }
#endif
}

void MainWindow::on_actionQuit_triggered()
{
    close();
}

void MainWindow::on_actionPreferences_triggered()
{
    // one instance only: an older copy would save stale values on Apply
    static QPointer<ConfigDialog> open;
    if (open)
    {
        open->raise();
        open->activateWindow();
        return;
    }
    ConfigDialog *dialog = new ConfigDialog(this, &config);
    open = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &ConfigDialog::updateConfig, this, &MainWindow::onUpdateConfig);
    connect(dialog, &ConfigDialog::updateMapConfig, this, &MainWindow::onUpdateMapConfig);
    connect(dialog, &ConfigDialog::openMapTools, this, &MainWindow::on_actionToolbarConfig_triggered);
    dialog->show();
}

void MainWindow::on_actionGoto_triggered()
{
    getMapView()->onGoto();
}

void MainWindow::on_actionOpenShadow_triggered()
{
    WorldInfo wi;
    if (getSeed(&wi))
    {
        wi.seed = getShadow(wi.seed);
        setSeed(wi);
    }
}

void MainWindow::on_actionRedistribute_triggered()
{
    std::vector<Condition> conds = formCond->getConditions();
    if (!conds.empty())
    {
        QMap<int, int> ids;
        for (int i = 0, n = conds.size(); i < n; i++)
            ids.insert(conds[i].save, ids.size()+1);
        for (int i = 0, n = conds.size(); i < n; i++)
            if (conds[i].relative && !ids.contains(conds[i].relative))
                ids.insert(conds[i].relative, ids.size()+1);
        formCond->clearConditions();
        for (Condition& c : conds)
        {
            c.save = ids[c.save];
            c.relative = ids[c.relative];
            // a fresh item re-adds the condition as is; a null item would
            // create another structure check for each quad condition
            formCond->addItemCondition(new QListWidgetItem(), c, 1);
        }
    }
}

void MainWindow::on_actionToolbarConfig_triggered()
{
    MapToolsDialog *dialog = new MapToolsDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &MapToolsDialog::updateMapConfig, this, &MainWindow::onUpdateMapConfig);
    dialog->show();
}

void MainWindow::on_actionBiomeColors_triggered()
{
    WorldInfo wi;
    getSeed(&wi);
    BiomeColorDialog *dialog = new BiomeColorDialog(this, config.biomeColorPath, wi.mc, getDim());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &BiomeColorDialog::yieldBiomeColorRc, this, &MainWindow::setBiomeColorRc);
    dialog->show();
}

void MainWindow::on_actionPresetLoad_triggered()
{
    formCond->popupSavedFilters();
}

void MainWindow::applySearchMode()
{
    const bool advanced = config.advancedSearch;
    formControl->setAdvanced(advanced);
    if (tabLocations)
        tabLocations->setAdvanced(advanced);
    // the 48-bit generator stays visible while it does more than "Auto"
    const bool gen48 = formGen48->getConfig(false).mode != GEN48_AUTO;
    // datapack worlds are searched seed by seed: no structure seed generator
    ui->collapseGen48->setVisible((advanced || gen48) && !datapackMode());
}

void MainWindow::on_actionAbout_triggered()
{
    AboutDialog *dialog = new AboutDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::on_actionCopy_triggered()
{
    formControl->copyResults();
}

void MainWindow::on_actionPaste_triggered()
{
    formControl->pasteResults();
}

void MainWindow::on_actionAddShadow_triggered()
{
    const std::vector<uint64_t> results = formControl->getResults();
    std::vector<uint64_t> shadows;
    shadows.reserve(results.size());
    for (uint64_t s : results)
        shadows.push_back( getShadow(s) );
    formControl->searchResultsAdd(shadows, false);
}

void MainWindow::on_actionExtGen_triggered()
{
    ExtGenDialog *dialog = new ExtGenDialog(this, &g_extgen);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // the search reads these settings without a snapshot: keep a search from
    // starting while the dialog is open (the action is disabled during one)
    dialog->setWindowModality(Qt::WindowModal);
    connect(dialog, &QDialog::accepted, [=] {
        g_extgen = dialog->getSettings();
        publishExtGen(); // the copy that worker threads read
        // invalidate the map world, forcing an update
        getMapView()->deleteWorld();
        setMCList(g_extgen.experimentalVers);
        updateMapSeed();
    });
    dialog->show();
}

void MainWindow::on_actionExportImg_triggered()
{
    ExportDialog *dialog = new ExportDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::on_actionScreenshot_triggered()
{
#if WASM
    QPixmap pixmap = getMapView()->screenshot();
    QImage img = pixmap.toImage();
    QByteArray content;
    QBuffer buffer(&content);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    QFileDialog::saveFileContent(content, "screenshot.png");
#else
    QString filter = tr("Images (*.png *.jpg *.ppm)");
    QString fnam = QFileDialog::getSaveFileName(this, tr("Save screenshot"), prevdir, filter);
    if (!fnam.isEmpty())
    {
        QPixmap pixmap = getMapView()->screenshot();
        QImage img = pixmap.toImage();
        img.save(fnam);
    }
#endif
}

void MainWindow::on_actionDock_triggered()
{
    if (dock->isFloating())
    {
        setDockable(false);
        ui->actionDock->setText(tr("Undock map"));
    }
    else
    {
        setDockable(true);
        dock->setFloating(true);
        ui->actionDock->setText(tr("Redock map"));
    }
}

void MainWindow::on_actionLayerDisplay_triggered()
{
    WorldInfo wi;
    getSeed(&wi, false);
    LayerDialog *dialog = new LayerDialog(this, wi);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setLayerOptions(lopt);
    connect(dialog, &LayerDialog::apply, [=](){
        lopt = dialog->getLayerOptions();
        syncLayerActions();
        updateMapSeed();
    });
    dialog->show();
}

int MainWindow::switchTab(QTabWidget *tabs, int idxprev, int idxnext)
{
    QSettings settings(appSettingsId(), appSettingsId());
    ISaveTab *tabold = dynamic_cast<ISaveTab*>(tabs->widget(idxprev));
    ISaveTab *tabnew = dynamic_cast<ISaveTab*>(tabs->widget(idxnext));
    if (tabold && loadedTabs.contains(tabold)) tabold->save(settings);
    if (tabnew)
    {
        tabnew->load(settings);
        loadedTabs.insert(tabnew);
    }
    return idxnext;
}

void MainWindow::on_tabContainer_currentChanged(int index)
{
    tabidx = switchTab(ui->tabContainer, tabidx, index);
}

void MainWindow::on_tabContainerSearch_currentChanged(int index)
{
    tabsearch = switchTab(ui->tabContainerSearch, tabsearch, index);
}

void MainWindow::on_actionSearch_seed_list_triggered()
{
    formControl->setSearchMode(SEARCH_LIST);
}

void MainWindow::on_actionSearch_full_seed_space_triggered()
{
    formControl->setSearchMode(SEARCH_BLOCKS);
}

void MainWindow::onAutosaveTimeout()
{
    if (config.autosaveCycle)
    {
        QString error;
        if (saveSession(sessionpath, true, &error))
        {
            autosaveFailed = false;
        }
        else if (!autosaveFailed)
        {   // report once, not every cycle, until a save succeeds again
            autosaveFailed = true;
            warn(this, tr("Autosave failed"),
                tr("The session could not be saved automatically:\n%1\n\n"
                   "The previous session file was kept.").arg(error));
        }
    }
}

void MainWindow::onActionHistory(QAction *act)
{
    uint64_t seed = act->data().toULongLong();
    onSelectedSeedChanged(seed);
    //ui->menuHistory->removeAction(act);
    //act->deleteLater();
}

void MainWindow::onActionMapToggled(int sopt, bool show)
{
    if (sopt == D_PORTAL) // overworld portals should also control nether
        getMapView()->setShow(D_PORTALN, show);
    getMapView()->setShow(sopt, show);
}

void MainWindow::syncLayerActions()
{
    // Signals stay connected so that the toolbar button follows the state.
    layerSync = true;
    if (lopt.mode >= 0 && lopt.mode < laction.size() && laction[lopt.mode])
        laction[lopt.mode]->setChecked(true);
    if (terrainAction)
        terrainAction->setChecked(lopt.mode == LOPT_TERRAIN);
    layerSync = false;
}

void MainWindow::updateTerrainAvailability(int mc)
{
    // The terrain map shades the surface height of the 1.18+ generator. For
    // older versions it is unavailable and falls back to the biome map.
    // A datapack world has no terrain model yet: there the view is only
    // switched off, the choice stays for the worlds without packs.
    const bool datapack = datapackMode();
    const bool available = mc > MC_1_17;
    if (!available && !datapack && lopt.mode == LOPT_TERRAIN)
    {
        lopt.mode = LOPT_BIOMES;
        syncLayerActions();
    }
    // A datapack world has terrain where its dimension's final_density could
    // be compiled; the choice of the layer stays either way.
    const std::shared_ptr<const DatapackWorld> packWorld = datapack ? datapacks->current() : nullptr;
    const bool packLoaded = packWorld && packWorld->usable();
    const bool packTerrain = packLoaded && packWorld->dimensionInfo()->terrain;
    const bool enabled = datapack ? (packTerrain || !packLoaded) : available;
    ui->actionTerrain->setEnabled(enabled);
    if (terrainAction)
    {
        terrainAction->setEnabled(enabled);
        QString tip = tr("Terrain map: Minecraft-like surface colors with hill shading (1.18 and newer)");
        if (datapack && packLoaded && !packTerrain)
        {
            const QString why = packWorld->dimensionInfo()->terrainReason;
            tip = tr("Terrain map: not available for this dimension of the datapack world.");
            if (!why.isEmpty())
                tip += " " + why;
        }
        else if (datapack)
            tip = tr("Terrain map: surface colors with hill shading from the terrain of the "
                "datapack world (without caves, carvers, trees and structures). Zoomed out to 8 blocks "
                "per pixel and more, its heights are the highest solid block with the sea and lava "
                "levels only: no lakes or aquifers, no blocks of the surface rules (such as powder snow).");
        terrainAction->setToolTip(tip);
    }
}

void MainWindow::updateWorldTypeAvailability(int mc)
{
    showWorldType(mc, ui->comboWorldType->currentIndex());
}

void MainWindow::showWorldType(int mc, int type)
{
    // In a datapack world the world types are its world presets (normal,
    // large_biomes, amplified).
    QStringList presetList;
    const QStringList *presets = nullptr;
    const std::shared_ptr<const DatapackWorld> packWorld = datapacks ? datapacks->current() : nullptr;
    if (datapackMode() && packWorld && packWorld->usable() && packWorld->mc == mc)
    {
        presetList = packWorld->world->presets();
        presets = &presetList;
    }
    ui->comboWorldType->setToolTip(packWorld && packWorld->usable() && !packWorld->presetMatters
        ? tr("World type: not used here. A data pack brings its own file for this dimension.")
        : tr("World type, as chosen when the world was created"));
    QStandardItemModel *model = qobject_cast<QStandardItemModel*>(ui->comboWorldType->model());
    for (int i = 0; model && i < WORLD_TYPE_NUM; i++)
    {
        QStandardItem *item = model->item(i);
        if (!item)
            continue;
        bool available = worldTypeAvailable(i, mc);
        QString tip;
        if (presets && !presets->contains(datapackPreset(i)))
        {   // a datapack world: the world types its packs and version offer
            available = false;
            tip = tr("The loaded data packs do not offer this world type.");
        }
        else if (i == WORLD_LARGE_BIOMES)
        {
            tip = available ? tr("Large Biomes: every biome is four times as wide and long.")
                : tr("Large Biomes is available for Minecraft 1.3 and newer.");
        }
        else if (i == WORLD_AMPLIFIED)
        {
            tip = available ? tr("Amplified: much taller terrain, up to the build limit. The "
                    "climate is that of a default world, but biomes at a fixed height, "
                    "strongholds and everything that depends on the terrain can differ.")
                : tr("Amplified is available for Minecraft 1.18 and newer. In older versions "
                    "it has the biomes of a default world and differs only in its terrain, "
                    "which is not known for these versions: use Default.");
        }
        item->setEnabled(available);
        item->setToolTip(tip);
    }
    // Amplified does not exist in this version: back to the default world.
    // (Large Biomes stays selected, as it always has; the generators of the
    // versions without it ignore it.)
    if (type == WORLD_AMPLIFIED && !worldTypeAvailable(WORLD_AMPLIFIED, mc))
        type = WORLD_DEFAULT;
    if (type < 0 || type >= WORLD_TYPE_NUM)
        type = WORLD_DEFAULT;
    {
        const QSignalBlocker blocker(ui->comboWorldType);
        ui->comboWorldType->setCurrentIndex(type);
    }
    // the first entry of the Y picker is the surface level of the world type
    // (the levels of a datapack world are those of its dimension)
    const QString surface = tr("%1 (surface)").arg(surfaceLevel(type == WORLD_AMPLIFIED));
    if (!levelsDatapack && ui->comboY->count() && ui->comboY->itemText(0) != surface)
    {
        const QSignalBlocker blocker(ui->comboY);
        ui->comboY->setItemText(0, surface);
    }
}

void MainWindow::setWorldTypeNames(bool compact)
{
    for (int type = 0; type < WORLD_TYPE_NUM && type < ui->comboWorldType->count(); type++)
    {
        QString name = worldTypeName(type);
        if (compact && type == WORLD_LARGE_BIOMES)
            name = tr("Large");
        if (ui->comboWorldType->itemText(type) != name)
            ui->comboWorldType->setItemText(type, name);
    }
    // The box is as wide as its longest name: its size hint (for a generous
    // contents length) is capped at that width. The contents length itself
    // stays as it is, QComboBox keeps a stale minimum size hint when it
    // changes; and the width is not fixed, so that a card without enough room
    // can still squeeze the box.
    ui->comboWorldType->ensurePolished();
    const QFontMetrics fm = ui->comboWorldType->fontMetrics();
    int widest = 0;
    for (int type = 0; type < ui->comboWorldType->count(); type++)
        widest = qMax(widest, fm.horizontalAdvance(ui->comboWorldType->itemText(type)));
    ui->comboWorldType->setMaximumWidth(ui->comboWorldType->widthForContents(widest + 4));
}

void MainWindow::onActionBiomeLayerSelect(int mode, int disp)
{
    WorldInfo wi;
    const bool hasSeed = getSeed(&wi, false);
    lopt.mode = mode;
    syncLayerActions();
    if (disp >= 0)
    {
        if (!getLayerOptionInfo(nullptr, mode, disp, wi))
            return; // unsupported display mode
        lopt.disp[mode] = disp;
    }
    // without a seed only remember the layer; setSeed() would fill in seed 0
    if (hasSeed)
        setSeed(wi, DIM_UNDEF);
}

void MainWindow::onConditionsChanged()
{
    std::vector<Condition> conds = formCond->getConditions();
    formGen48->updateAutoConditions(conds);
    if (formControl)
        formControl->invalidateResults();
}

void MainWindow::onConditionsSelect(const std::vector<Condition>& selection)
{
    std::vector<Shape> shapes;
    for (const Condition& c : selection)
    {
        if (c.meta & Condition::DISABLED)
            continue;
        if (c.relative)
            continue;
        const FilterInfo& ft = g_filterinfo.list[c.type];

        Shape s;
        s.dim = ft.dim;
        if (c.rmax)
        {
            s.type = Shape::CIRCLE;
            s.p1 = s.p2 = Pos{0,0};
            s.r = c.rmax - 1;
        }
        else
        {
            s.type = Shape::RECT;
            s.p1 = Pos{c.x1, c.z1};
            s.p2 = Pos{c.x2+1, c.z2+1};
            s.r = 0;
        }
        shapes.push_back(s);
    }
    getMapView()->setShapes(shapes);
}

void MainWindow::onGen48Changed()
{
    formGen48->updateCount();
    formControl->searchProgressReset();
    applySearchMode();
}

void MainWindow::onSelectedSeedChanged(uint64_t seed)
{
    WorldInfo wi;
    getSeed(&wi, false);
    wi.seed = seed;
    setSeed(wi);
}

void MainWindow::onSearchStatusChanged(bool running)
{
    // the running search uses a snapshot of the filters; editing them would
    // discard the results while the search keeps adding seeds for the old ones
    formGen48->setEnabled(!running);
    formCond->setEnabled(!running);
    formDpCond->setEnabled(!running);
    ui->actionExtGen->setEnabled(!running);
    // loading replaces the filters; the saved filters are vanilla conditions
    ui->actionPresetLoad->setEnabled(!running && !datapackMode());
}

void MainWindow::onUpdateConfig()
{
    Config old = config;
    config.load();
    QThreadPool::globalInstance()->setMaxThreadCount(config.mapThreads);

    if (formControl && formGen48)
        applySearchMode();

    QFont fnorm_old = ui->labelMC->font();
    QFont fmono_old = ui->labelMono->font();
    QFont fnorm = config.fontNorm;
    QFont fmono = config.fontMono;
    if (fnorm.family() == "Monospace") // avoid identification conflict
        fnorm = QApplication::font();
    fnorm.setStyleHint(QFont::AnyStyle);
    fmono.setStyleHint(QFont::Monospace);
    fmono.setFixedPitch(true);

    bool font_changed = fnorm_old != fnorm || fmono_old != fmono;

    QFontMetrics fm_ref(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    QFontMetrics fm_new(fnorm);

    g_fontscale = fm_new.height() / (qreal) fm_ref.height();
    g_iconscale = config.iconScale;

    ui->toolBar->setMovable(false);
    ui->toolBar->setAllowedAreas(Qt::RightToolBarArea);
    if (toolBarArea(ui->toolBar) != Qt::RightToolBarArea)
    {
        removeToolBar(ui->toolBar);
        addToolBar(Qt::RightToolBarArea, ui->toolBar);
        ui->toolBar->show();
    }

    getMapView()->setConfig(config);

    if ((old.uistyle != config.uistyle) || (old.iconScale != config.iconScale) ||
        font_changed || qApp->styleSheet().isEmpty())
    {
        int s = qBound(20, (int) round(g_iconscale * 24), 40);
        ui->toolBar->setIconSize(QSize(s, s));
        onStyleChanged(config.uistyle);
    }
    if (!old.biomeColorPath.isEmpty() || !config.biomeColorPath.isEmpty())
    {
        onBiomeColorChange();
    }

    if (font_changed)
    {
        QFont fnormb = fnorm;
        QFont fmonob = fmono;
        fnormb.setBold(true);
        fmonob.setBold(true);

        QApplication::setFont(fnorm);

        QWidgetList wlist = QApplication::allWidgets();
        for (QWidget *w : std::as_const(wlist))
        {
            const QFont& f = w->font();
            if (f.styleHint() == QFont::Monospace || f.family() == "Monospace" ||
                f.family() == "Courier New")
                w->setFont(f.bold() ? fmonob : fmono);
            else
                w->setFont(f.bold() ? fnormb : fnorm);
        }

        QSize iconsize = QSize((int)round(14 * g_fontscale), (int)round(14 * g_fontscale));

        // update cascade; chrome buttons keep the icon size of their design
        for (QWidget *w : std::as_const(wlist))
        {
            QAbstractButton *b = qobject_cast<QAbstractButton*>(w);
            if (b && !b->property("keepIconSize").toBool())
                b->setIconSize(iconsize);
            w->update();
        }
        // explicit fonts override the sizes and weights of the style sheet
        // (headings, panel title): polish everything again
        if (!qApp->styleSheet().isEmpty())
            qApp->setStyleSheet(qApp->styleSheet());
    }

    if (config.autosaveCycle)
    {
        autosaveTimer.setInterval(config.autosaveCycle * 60000);
        autosaveTimer.start();
    }
    else
    {
        autosaveTimer.stop();
    }
}

void MainWindow::onUpdateMapConfig()
{
    MapConfig old = mconfig;
    mconfig.load();

    if (!old.sameMapOpts(mconfig))
    {
        updateStructureActions();
        if (getMapView()->world)
        {
            getMapView()->deleteWorld();
            updateMapSeed();
        }
    }
    actzoom[0]->setVisible(mconfig.zoomEnabled);
    actzoom[1]->setVisible(mconfig.zoomEnabled);
    if (mapZoomIn && mapZoomOut)
    {
        mapZoomIn->setVisible(mconfig.zoomEnabled);
        mapZoomOut->setVisible(mconfig.zoomEnabled);
        layoutMapOverlays();
    }
}

void MainWindow::onBiomeColorChange()
{
    QFile file(config.biomeColorPath);
    if (!config.biomeColorPath.isEmpty() && file.open(QIODevice::ReadOnly))
    {
        char buf[32*1024];
        qint64 siz = file.read(buf, sizeof(buf)-1);
        file.close();
        if (siz >= 0)
        {
            buf[siz] = 0;
            initBiomeColors(g_biomeColors);
            parseBiomeColors(g_biomeColors, buf);
        }
    }
    else
    {
        initBiomeColors(g_biomeColors);
    }
    getMapView()->refreshBiomeColors();
    ISaveTab *tab = dynamic_cast<ISaveTab*>(ui->tabContainer->currentWidget());
    if (tab)
        tab->refresh();
}

void MainWindow::onStyleChanged(int style)
{
    static const bool fallbackSystemDark =
            QApplication::palette().color(QPalette::Window).lightness() < 128;

    bool dark = style == STYLE_DARK;
    if (style == STYLE_SYSTEM)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        Qt::ColorScheme scheme = qApp->styleHints()->colorScheme();
        dark = scheme == Qt::ColorScheme::Dark ||
               (scheme == Qt::ColorScheme::Unknown && fallbackSystemDark);
#else
        dark = fallbackSystemDark;
#endif
    }

    // "Atlas" palette: neutral chrome with grass green as the only accent.
    // accentStrong is the accent for text on accentSoft (contrast >= 4.5:1).
    struct ThemeColors {
        const char *window, *surface, *mapSurface, *input, *popup, *subtle;
        const char *text, *muted, *disabledText, *disabled;
        const char *border, *borderHover, *controlBorder, *alternate;
        const char *button, *buttonHover;
        const char *accent, *accentHover, *accentSoft, *accentText, *accentStrong;
        const char *scroll, *scrollHover;
        const char *rail, *card, *danger;
    };

    const ThemeColors colors = dark
            ? ThemeColors{
                "#101311", "#161a17", "#0c0f0d", "#0d100e", "#1b201c", "#1d231f",
                "#e9eee9", "#8f9b92", "#5c665e", "#131714",
                "#2a322c", "#3c473f", "#4d5850", "#121613",
                "#1c221e", "#252c27",
                "#6cc070", "#80cf84", "#1c3323", "#07140a", "#8fd893",
                "#323b34", "#4a564d",
                "#0f1210", "#161a17", "#ff8a7a"
              }
            : ThemeColors{
                "#f4f6f3", "#ffffff", "#e8ece7", "#ffffff", "#ffffff", "#eff2ee",
                "#1b221d", "#5f6b62", "#9aa49c", "#f0f2ef",
                "#d9dfd8", "#b4beb5", "#97a299", "#f7f9f6",
                "#ffffff", "#eef2ed",
                "#2b8139", "#236d2f", "#dff1e1", "#ffffff", "#1d6529",
                "#c3cbc3", "#9ea99f",
                "#eef1ed", "#ffffff", "#c62f1f"
              };
    darkTheme = dark;
    colorDanger = QColor(colors.danger);
    colorAccent = QColor(colors.accent);
    colorAccentStrong = QColor(colors.accentStrong);
    colorMuted = QColor(colors.muted);

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(colors.window));
    palette.setColor(QPalette::WindowText, QColor(colors.text));
    palette.setColor(QPalette::Base, QColor(colors.input));
    palette.setColor(QPalette::AlternateBase, QColor(colors.alternate));
    palette.setColor(QPalette::Text, QColor(colors.text));
    palette.setColor(QPalette::PlaceholderText, QColor(colors.muted));
    palette.setColor(QPalette::Button, QColor(colors.button));
    palette.setColor(QPalette::ButtonText, QColor(colors.text));
    palette.setColor(QPalette::Highlight, QColor(colors.accent));
    palette.setColor(QPalette::HighlightedText, QColor(colors.accentText));
    palette.setColor(QPalette::Link, QColor(colors.accentStrong));
    palette.setColor(QPalette::Mid, QColor(colors.border));
    palette.setColor(QPalette::ToolTipBase, QColor(colors.popup));
    palette.setColor(QPalette::ToolTipText, QColor(colors.text));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(colors.disabledText));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(colors.disabledText));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(colors.disabledText));

    if (QStyle *fusion = QStyleFactory::create("Fusion"))
        qApp->setStyle(fusion);
    qApp->setPalette(palette);
    refreshChromeIcons();

    QFile file(":/modern.qss");
    if (!file.open(QFile::ReadOnly))
    {
        qWarning() << "Failed to load modern UI stylesheet";
        return;
    }
    QString st = QString::fromUtf8(file.readAll());
    const QPair<const char *, const char *> replacements[] = {
        {"@WINDOW@", colors.window}, {"@SURFACE@", colors.surface},
        {"@MAP_SURFACE@", colors.mapSurface}, {"@INPUT@", colors.input},
        {"@POPUP@", colors.popup}, {"@SUBTLE@", colors.subtle},
        {"@TEXT@", colors.text}, {"@MUTED@", colors.muted},
        {"@DISABLED_TEXT@", colors.disabledText}, {"@DISABLED@", colors.disabled},
        {"@BORDER@", colors.border}, {"@BORDER_HOVER@", colors.borderHover},
        {"@CONTROL_BORDER@", colors.controlBorder},
        {"@ALTERNATE@", colors.alternate}, {"@BUTTON@", colors.button},
        {"@BUTTON_HOVER@", colors.buttonHover}, {"@ACCENT@", colors.accent},
        {"@ACCENT_HOVER@", colors.accentHover}, {"@ACCENT_SOFT@", colors.accentSoft},
        {"@ACCENT_TEXT@", colors.accentText}, {"@ACCENT_STRONG@", colors.accentStrong},
        {"@SCROLL@", colors.scroll}, {"@SCROLL_HOVER@", colors.scrollHover},
        {"@RAIL@", colors.rail}, {"@CARD@", colors.card}, {"@DANGER@", colors.danger},
    };
    for (const auto& replacement : replacements)
        st.replace(QLatin1String(replacement.first), QLatin1String(replacement.second));
    if (st.contains('@'))
        qWarning() << "Unreplaced token in the UI stylesheet";

    qApp->setStyleSheet(st);
}

void MainWindow::onDockFloating(bool floating)
{
    if (!floating)
    {
        setDockable(false);
        ui->actionDock->setText(tr("Undock map"));
    }
}

// ---------------------------------------------------------------- data packs

QColor MainWindow::themeColor(const QString& name) const
{
    if (name == "danger")
        return colorDanger;
    if (name == "accent")
        return colorAccent;
    if (name == "accentStrong")
        return colorAccentStrong;
    return colorMuted;
}

void MainWindow::setVersion(int mc)
{
    const QString name = mc2str(mc);
    if (ui->comboBoxMC->findText(name) < 0)
        setMCList(g_extgen.experimentalVers, mc);
    if (ui->comboBoxMC->currentText() != name)
        ui->comboBoxMC->setCurrentText(name);
    else
        updateMapSeed();
}

void MainWindow::openDatapackStructures()
{
    if (!dpStructDialog)
    {
        dpStructDialog = new DpStructuresDialog(this);
        dpStructDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dpStructDialog->refresh();
    dpStructDialog->show();
    dpStructDialog->raise();
    dpStructDialog->activateWindow();
}

void MainWindow::openDatapacks()
{
    ui->tabContainer->setCurrentWidget(tabDatapacks);
    if (sidePanelCollapsed)
        setSidePanelCollapsed(false);
}

void MainWindow::addDatapacks(const QStringList& paths)
{
    QStringList rejected;
    for (const QString& path : paths)
        if (!isDatapackPath(path))
            rejected.append(QFileInfo(path).fileName());
    const int added = datapacks->addPacks(paths);
    openDatapacks();
    if (!rejected.isEmpty())
    {
        warn(this, tr("Not a data pack: %1\n\nData packs are zip files, mod jars or folders.")
            .arg(rejected.join(", ")));
    }
    else if (!added && !paths.isEmpty())
    {
        info(this, tr("These packs are already in the list."));
    }
}

void MainWindow::onDatapacksChanged()
{
    updateMapSeed();
}

static QStringList droppedPaths(const QMimeData *mime)
{
    QStringList paths;
    if (!mime || !mime->hasUrls())
        return paths;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    return paths;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    bool any = false;
    for (const QString& path : droppedPaths(event->mimeData()))
        any = any || isDatapackPath(path);
    if (!any)
        return;
    event->acceptProposedAction();
    if (!dropHighlight)
    {
        dropHighlight = new QFrame(this);
        dropHighlight->setObjectName("dropHighlight");
        dropHighlight->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        dropHighlight->setAttribute(Qt::WA_StyledBackground, true);
        QVBoxLayout *layout = new QVBoxLayout(dropHighlight);
        QLabel *label = new QLabel(tr("Drop to add as datapack"), dropHighlight);
        label->setObjectName("dropHighlightLabel");
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label, 0, Qt::AlignCenter);
    }
    dropHighlight->setGeometry(rect().adjusted(8, 8, -8, -8));
    dropHighlight->raise();
    dropHighlight->show();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    if (dropHighlight)
        dropHighlight->hide();
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (dropHighlight)
        dropHighlight->hide();
    const QStringList paths = droppedPaths(event->mimeData());
    if (paths.isEmpty())
        return;
    event->acceptProposedAction();
    // after the drop has ended: a message box inside the drop would block
    // the application that the files were dragged from
    QTimer::singleShot(0, this, [this, paths]() { addDatapacks(paths); });
}

void MainWindow::updateLevelPicker(WorldInfo *wi)
{
    const bool datapack = wi->dp && wi->dp->usable();
    if (wi->dp && !datapack)
        return; // loading or failed: the picker waits for a world
    const QSignalBlocker blocker(ui->comboY);
    if (datapack)
    {
        const int low = wi->dp->minY(), high = wi->dp->maxY();
        const int sea = wi->dp->dimensionInfo()->seaLevel - 1; // the topmost water block
        // A dimension with a ceiling (the Nether): the level just below the
        // roof, where the map looks down into the caves.
        const int roof = dpBelowRoofLevel(wi->dp, wi->seed);
        const bool otherDimension = levelsDatapack && levelsDimension != wi->dp->dimensionId;
        if (!levelsDatapack)
        {   // coming from a built-in world: remember its level, start at the
            // top (as "surface" does) or where the datapack world was left
            vanillaLevel = wi->y;
            if (datapackLevel != INT_MIN)
                wi->y = datapackLevel;
            else if (wi->y == 255 || wi->y == surfaceLevel(false) || wi->y == surfaceLevel(true))
                wi->y = roof != INT_MIN ? roof : high;
            datapackLevel = INT_MIN;
        }
        else if (otherDimension)
        {   // another dimension: its top, or below its roof
            if (roof != INT_MIN && wi->y > roof)
                wi->y = roof;
            else if (roof == INT_MIN && wi->y == levelsRoof)
                wi->y = high;
        }
        levelsDimension = wi->dp->dimensionId;
        levelsRoof = roof;
        wi->y = qBound(low, wi->y, high);
        QStringList levels;
        levels << tr("%1 (top)").arg(high);
        if (roof != INT_MIN && roof < high && roof > low)
            levels << tr("%1 (below the roof)").arg(roof);
        // every 32 blocks in between (floor division, for negative levels)
        const int step = 32;
        int first = (roof != INT_MIN && roof < high ? roof : high) - 1;
        first -= ((first % step) + step) % step;
        for (int y = first; y > low; y -= step)
            levels << QString::number(y);
        levels << tr("%1 (bottom)").arg(low);
        // the sea level, in its place between the steps
        if (sea > low && sea < high)
        {
            int at = 1;
            while (at < levels.size() - 1 && levels[at].section(' ', 0, 0).toInt() > sea)
                at++;
            if (at < levels.size() && levels[at].section(' ', 0, 0).toInt() == sea)
                levels[at] = tr("%1 (sea level)").arg(sea);
            else
                levels.insert(at, tr("%1 (sea level)").arg(sea));
        }
        bool same = levelsDatapack && ui->comboY->count() >= levels.size();
        for (int i = 0; same && i < levels.size(); i++)
            same = ui->comboY->itemText(i) == levels[i];
        if (!same)
        {
            ui->comboY->clear();
            ui->comboY->addItems(levels);
        }
        levelsDatapack = true;
        QString tip = tr("The height (Y level) at which the map shows the biomes of "
            "the datapack world. This dimension reaches from Y %1 to %2.").arg(low).arg(high);
        if (roof != INT_MIN)
            tip += " " + tr("It has a roof: the height layers look down from the chosen level, "
                "\"below the roof\" shows the caves under it.");
        ui->comboY->setToolTip(tip);
    }
    else if (levelsDatapack)
    {   // back to a built-in world and its level (the picker still holds the
        // level of the datapack world; wi->y went through normalize() already)
        levelsDatapack = false;
        datapackLevel = ui->comboY->currentText().section(' ', 0, 0).toInt();
        ui->comboY->clear();
        ui->comboY->addItems(vanillaLevels);
        if (vanillaLevel != INT_MIN)
            wi->y = vanillaLevel;
        vanillaLevel = INT_MIN;
        wi->normalize();
        ui->comboY->setToolTip(ui->labelY->toolTip());
    }
    ui->comboY->view()->setMinimumWidth(ui->comboY->view()->sizeHintForColumn(0) + 36);
}

QListWidget *MainWindow::activeConditionList() const
{
    if (formDpCond && datapackMode())
        return formDpCond->list();
    return formCond ? formCond->findChild<QListWidget*>("listConditions") : nullptr;
}

void MainWindow::updateConditionLists()
{
    if (!formDpCond || !formCond)
        return;
    const bool datapack = datapackMode();
    formDpCond->refreshWorld();
    if (formDpCond->isVisibleTo(formDpCond->parentWidget()) != datapack)
    {
        formCond->setVisible(!datapack);
        formDpCond->setVisible(datapack);
        if (QListWidget *list = activeConditionList())
            ui->collapseConstraints->setCount(list->count());
        QTimer::singleShot(0, this, &MainWindow::fitConditionsHeight);
        applySearchMode(); // no seed generator for datapack worlds
    }
    // the saved filters and examples are vanilla conditions
    ui->actionPresetLoad->setEnabled(!datapack && !(formControl && formControl->isSearching()));
    // the Locations page evaluates the vanilla conditions
    if (tabLocations)
    {
        const int index = ui->tabContainerSearch->indexOf(tabLocations);
        tabLocations->setEnabled(!datapack);
        ui->tabContainerSearch->setTabEnabled(index, !datapack);
        ui->tabContainerSearch->setTabToolTip(index, datapack
            ? tr("Locations: not available for datapack worlds yet (it evaluates the vanilla "
                 "conditions). Switch off the datapacks to use it.")
            : QString());
        if (datapack && ui->tabContainerSearch->currentWidget() == tabLocations)
            ui->tabContainerSearch->setCurrentIndex(0);
    }
}

void MainWindow::updateDatapackNotice()
{
    if (!datapackNotice)
        return;
    QWidget *page = ui->tabContainer->currentWidget();
    const bool vanillaOnly = page && page != tabDatapacks && !page->inherits("TabSlime")
        && !page->inherits("TabBiomes") && !page->inherits("TabStructures")
        && page != ui->tabSearch;
    const bool show = datapackShown && vanillaOnly;
    if (show)
    {
        datapackNotice->setText(tr("These tools work on vanilla worlds. Remove or switch off "
            "the datapacks to use them. <a href=\"#datapacks\" style=\"color:%1\">Open Datapacks</a>")
            .arg(colorAccentStrong.name()));
    }
    datapackNotice->setVisible(show);
}

void MainWindow::updateDatapackMode()
{
    if (!datapacks || !packChip || laction.isEmpty() || !dimgroup)
        return; // called while the window is still being built
    const bool datapack = datapackMode();
    const std::shared_ptr<const DatapackWorld> world = datapacks->current();

    // the pack chip of the world card
    int count = 0;
    for (int i = 0; i < (int) datapacks->entries().size(); i++)
        count += datapacks->entries()[i].enabled && datapacks->packExists(i);
    const bool problem = datapack && world && world->state != DatapackWorld::Ready
        && world->state != DatapackWorld::Loading;
    const bool chipChanged = packChip->isVisible() != datapack
        || packChip->text() != QString::number(count);
    packChip->setText(QString::number(count));
    packChip->setIcon(chromeIcon("datapacks", problem ? colorDanger : colorAccentStrong, 14));
    packChip->setProperty("problem", problem);
    packChip->style()->unpolish(packChip);
    packChip->style()->polish(packChip);
    packChip->setToolTip(problem
        ? tr("Datapack world with a problem. Click to open Datapacks.")
        : tr("Datapack world: %n data pack(s) loaded. Click to open Datapacks.", nullptr, count));
    packChip->setAccessibleName(tr("Datapacks"));
    packChip->setVisible(datapack);
    if (chipChanged)
        layoutMapOverlays();

    // Layers of the built-in generator. Without packs, updateMapSeed() has
    // just set what the version offers; these two exist in every version.
    // A datapack world has the biome map and, where its dimension has
    // terrain, the height and terrain layers.
    const bool packTerrain = datapack && world && world->usable() && world->dimensionInfo()->terrain;
    for (int i = 0; i < laction.size(); i++)
    {
        if (!laction[i] || i == LOPT_BIOMES || i == LOPT_TERRAIN)
            continue;
        if (i == LOPT_HEIGHT)
            laction[i]->setToolTip(packTerrain
                ? tr("Height map of the datapack world: the ocean floor of its terrain (without caves, "
                    "carvers, trees and structures). Zoomed out to 4 blocks per pixel and more, it shows "
                    "the highest solid block with the sea and lava levels only: no lakes or aquifers, no "
                    "blocks of the surface rules (such as powder snow).")
                : tr("An estimate of the terrain height, computed without generating the full terrain"));
        if (datapack)
            laction[i]->setEnabled(i == LOPT_HEIGHT && (packTerrain || !world || !world->usable()));
        else if (i == LOPT_HEIGHT || i == LOPT_STRUCTS)
            laction[i]->setEnabled(true);
    }
    if (dpStructAction)
        dpStructAction->setIcon(chromeIcon("datapacks", colorAccentStrong, 20));
    ui->actionLayerDisplay->setEnabled(!datapack);
    ui->actionBiomeColors->setEnabled(!datapack);
    for (int i = 0; i < 3; i++)
    {
        static const char *const names[] = {QT_TR_NOOP("Overworld"), QT_TR_NOOP("Nether"),
            QT_TR_NOOP("End")};
        dimactions[i]->setEnabled(!datapack);
        dimactions[i]->setToolTip(datapack
            ? tr("%1: a datapack world has its own dimensions. Pick one in Datapacks.").arg(tr(names[i]))
            : tr(names[i]));
    }

    // The search and the analyses work for one world: what runs for a vanilla
    // world stops when packs come, what runs for a datapack world stops when
    // that world changes (packs, version, world type, dimension) or goes.
    const std::shared_ptr<const DatapackWorld> nowWorld = datapack ? world : nullptr;
    if ((datapack && !datapackShown) || nowWorld != workWorld)
    {
        formControl->stopSearch();
        for (QTabWidget *tabs : {ui->tabContainer, ui->tabContainerSearch})
            for (int i = 0; i < tabs->count(); i++)
                if (ISaveTab *tab = dynamic_cast<ISaveTab*>(tabs->widget(i)))
                    if (tabs->widget(i) != tabDatapacks && !tabs->widget(i)->inherits("TabSlime"))
                        tab->stopWork();
    }
    // the tool in front lists the biomes of the new world
    const bool worldSwitched = nowWorld != workWorld;
    workWorld = nowWorld;
    // the tools that only know vanilla worlds; the Seed Finder, the biome
    // and the structure analysis have a datapack side
    for (int i = 0; i < ui->tabContainer->count(); i++)
    {
        QWidget *page = ui->tabContainer->widget(i);
        if (page == tabDatapacks || page->inherits("TabSlime") || page->inherits("TabBiomes")
            || page->inherits("TabStructures") || page == ui->tabSearch)
            continue;
        page->setEnabled(!datapack);
    }
    updateConditionLists();
    const bool changed = datapackShown != datapack;
    datapackShown = datapack;
    if (worldSwitched && !changed && datapack)
        if (ISaveTab *tab = dynamic_cast<ISaveTab*>(ui->tabContainer->currentWidget()))
            tab->refresh();
    updateDatapackNotice();
    // without a world the status strip says why (the map only reports changes)
    if (!getMapView()->world)
        updateMapStatus(QString(), 0, 0, getMapView()->getScale(), false);
    if (changed)
    {
        WorldInfo wi;
        getSeed(&wi, false);
        updateTerrainAvailability(wi.mc);
        updateStructureActions();
        // the tool in front shows what applies now (e.g. the slime chunks)
        if (ISaveTab *tab = dynamic_cast<ISaveTab*>(ui->tabContainer->currentWidget()))
            tab->refresh();
    }
}
