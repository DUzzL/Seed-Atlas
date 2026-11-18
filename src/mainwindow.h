#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAction>
#include <QActionGroup>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QPointer>
#include <QMessageBox>
#include <QMutex>
#include <QRunnable>
#include <QSet>
#include <QSettings>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <climits>

#include "mapview.h"
#include "formconditions.h"
#include "formgen48.h"
#include "formsearchcontrol.h"

namespace Ui {
class MainWindow;
}

struct ISaveTab
{
    virtual void save(QSettings& settings) = 0;
    virtual void load(QSettings& settings) = 0;
    virtual void refresh() {}
    // Stops a running analysis or search of the tab, as its Stop button does
    // (used when the tab is switched off as a whole, e.g. for datapack worlds).
    virtual void stopWork() {}
};

class DatapackManager;
class FormDpConditions;
class TabDatapacks;
class VanillaData;

class MapView;
class QButtonGroup;
class QFrame;
class QLabel;
class QMenu;
class QResizeEvent;
class QToolButton;
class QPropertyAnimation;
class QTabWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
    Q_PROPERTY(int sidePanelAnimationWidth READ sidePanelAnimationWidth WRITE setSidePanelAnimationWidth)

public:
    // sessionpath: the automatic session (restored at start, saved on quit
    // and by the autosave); opensession: a session file to open at start
    // instead, which is never overwritten automatically
    explicit MainWindow(QString sessionpath, QString resultspath,
                        QString opensession = QString(), QWidget *parent = 0);
    virtual ~MainWindow();
    virtual void closeEvent(QCloseEvent *event) override;
    virtual void resizeEvent(QResizeEvent *event) override;
    virtual bool eventFilter(QObject *watched, QEvent *event) override;
    // data packs can be dropped onto the window
    virtual void dragEnterEvent(QDragEnterEvent *event) override;
    virtual void dragLeaveEvent(QDragLeaveEvent *event) override;
    virtual void dropEvent(QDropEvent *event) override;

    QAction *addMapAction(int opt);
    QAction *addMapAction(QString rcbase, QString tip, QString toolbarLabel = QString());

    bool getSeed(WorldInfo *wi, bool applyrand = true);
    bool setSeed(WorldInfo wi, int dim = DIM_UNDEF);
    // The dimension the map shows: the one picked in the layer sidebar, or
    // the Overworld while a datapack world is shown (its dimensions are picked
    // in the Datapacks tool).
    int getDim();
    MapView *getMapView();

    // Data packs are loaded: every world is a datapack world, and the tools
    // of the built-in generator are switched off.
    bool datapackMode() const;
    // Adds data packs (zip, jar or folder) and opens the Datapacks tool.
    void addDatapacks(const QStringList& paths);
    void openDatapacks();
    // the list of the datapack world's structures, to pick what the map shows
    void openDatapackStructures();
    // Selects a Minecraft version in the version picker.
    void setVersion(int mc);
    // a color of the current theme: "danger", "accent", "accentStrong", "muted"
    QColor themeColor(const QString& name) const;
    bool loadSession(QString path, bool keepresults);
    bool saveSession(QString path, bool quiet, QString *error = nullptr);

protected:
    void saveSettings();
    void loadSettings();
    bool saveSession(QTextStream& stream);
    bool loadSession(QTextStream& stream, bool keepresults, bool quiet);
    void updateMapSeed();
    void setDockable(bool dockable);
    // also lists the version extramc when it is one of the hidden ones
    void setMCList(bool experimental, int extramc = MC_UNDEF);
    // saves the tabs that have read their settings (all of them, not only the
    // current one: analysis tabs keep working in the background)
    void saveTabs(QSettings& settings);
    int switchTab(QTabWidget *tabs, int idxprev, int idxnext);

signals:
    void mapUpdated();

public slots:
    void mapGoto(qreal x, qreal z, qreal scale);
    void mapZoom(qreal factor);
    void setBiomeColorRc(QString rc);

    void setProgressIndication(double value = -1);

private slots:
    void on_comboBoxMC_currentIndexChanged(int a);
    void on_seedEdit_editingFinished();
    void on_comboWorldType_currentIndexChanged(int index);
    void on_comboY_currentIndexChanged(int index);

    void on_actionSave_triggered();
    void on_actionLoad_triggered();
    void on_actionQuit_triggered();
    void on_actionPreferences_triggered();
    void on_actionGoto_triggered();
    void on_actionOpenShadow_triggered();
    void on_actionToolbarConfig_triggered();
    void on_actionBiomeColors_triggered();
    void on_actionPresetLoad_triggered();
    void applySearchMode();
    void on_actionAbout_triggered();
    void on_actionCopy_triggered();
    void on_actionPaste_triggered();
    void on_actionAddShadow_triggered();
    void on_actionRedistribute_triggered();
    void on_actionExtGen_triggered();
    void on_actionExportImg_triggered();
    void on_actionScreenshot_triggered();
    void on_actionDock_triggered();
    void on_actionLayerDisplay_triggered();

    void on_tabContainer_currentChanged(int index);
    void on_tabContainerSearch_currentChanged(int index);

    void on_actionSearch_seed_list_triggered();
    void on_actionSearch_full_seed_space_triggered();

    // internal events
    void onAutosaveTimeout();
    void onActionMapToggled(int sopt, bool a);
    void onActionHistory(QAction *act);
    void onActionBiomeLayerSelect(int lopt, int disp = -1);
    void onConditionsChanged();
    void onConditionsSelect(const std::vector<Condition>& selection);
    void onGen48Changed();
    void onSelectedSeedChanged(uint64_t seed);
    void onSearchStatusChanged(bool running);
    void onUpdateConfig();
    void onUpdateMapConfig();
    void onBiomeColorChange();
    void onStyleChanged(int style);
    void onDockFloating(bool floating);
    void onSidePanelToggle();
    void onSplitterMapMoved(int position, int index);
    void onSidePanelAnimationFinished();
    void onRailClicked(int index);
    void updateToolRail();
    void updateMapStatus(const QString& biome, int x, int z, qreal scale, bool hasWorld);
    void onDatapacksChanged();

private:
    void setRightSidebarCollapsed(bool collapsed, bool animated);
    void setSidePanelCollapsed(bool collapsed);
    int sidePanelAnimationWidth() const { return sidePanelCurrentWidth; }
    void setSidePanelAnimationWidth(int width);
    void updateSidePanelToggle();
    void setupToolRail();
    void setupMapOverlays();
    void layoutMapOverlays();
    // gives the condition list the height of its rows, within limits
    void fitConditionsHeight();
    bool conditionsHeightManual = false; // the user moved the splitter
    void refreshChromeIcons(); // theme colored chrome and layer icons
    int toolbarDim(); // the dimension picked in the layer sidebar
    // Switches everything that only knows the built-in generator off while a
    // datapack world is shown, and on again afterwards: layers, structures,
    // dimensions, the vanilla-only tools. It only disables, the choices
    // themselves (and with them the saved settings) stay as they are.
    void updateDatapackMode();
    void updateDatapackNotice();
    // the Seed Finder shows the conditions of the world type that is shown:
    // the vanilla ones or those for datapack worlds
    void updateConditionLists();
    // the condition list the Seed Finder shows now
    class QListWidget *activeConditionList() const;
    // the levels of the Y picker: those of the built-in worlds, or those of
    // the dimension of a datapack world
    void updateLevelPicker(WorldInfo *wi);

public:
    Ui::MainWindow *ui;
    QDockWidget *dock;
    MapView *mapView;

    FormConditions *formCond;
    FormDpConditions *formDpCond = nullptr; // the conditions for datapack worlds
    FormGen48 *formGen48;
    FormSearchControl *formControl;
    LayerOpt lopt;
    Config config;
    MapConfig mconfig;
    QString sessionpath;
    QString opensession;
    QString prevdir;
    QSet<ISaveTab*> loadedTabs; // tabs whose load() has run
    bool autosaveFailed = false; // the failure was reported once
    QTimer autosaveTimer;
    int tabidx;
    int tabsearch;
    int progval;

    QVector<QAction*> laction;
    QVector<QAction*> saction;
    QAction *terrainAction = nullptr; // toolbar toggle for LOPT_TERRAIN
    QAction *dpStructAction = nullptr; // opens the structures of a datapack world
    QPointer<class DpStructuresDialog> dpStructDialog;
    QAction *menuAction = nullptr; // opens the former menu bar menus
    QAction *settingsAction = nullptr;
    class QPushButton *coordinateGo = nullptr; // "go to" button of the map
    class TabLocations *tabLocations = nullptr;
    VanillaData *vanillaData = nullptr;
    DatapackManager *datapacks = nullptr;
    TabDatapacks *tabDatapacks = nullptr;
    QToolButton *packChip = nullptr; // marks a datapack world in the world card
    QLabel *datapackNotice = nullptr; // above the tools that need a vanilla world
    QFrame *dropHighlight = nullptr; // shown while packs are dragged over the window
    bool datapackShown = false; // the gating of updateDatapackMode() is on
    // the datapack world that the search and the analyses were left with:
    // work for another one stops when it changes
    std::shared_ptr<const DatapackWorld> workWorld;
    // set while the session of the last run is read at the start: messages
    // that only explain a later search are left out then
    bool restoringSession = false;
    bool levelsDatapack = false; // the Y picker lists the levels of a datapack dimension
    QString levelsDimension; // the datapack dimension whose levels the Y picker lists
    int levelsRoof = INT_MIN; // its level below the roof, if it has one
    QStringList vanillaLevels; // the entries of the Y picker for built-in worlds
    int vanillaLevel = INT_MIN; // the level to return to after a datapack world
    int datapackLevel = INT_MIN; // the level of the datapack world of the last run
    QColor colorDanger, colorAccent, colorAccentStrong, colorMuted;
    QMenu *mainMenu = nullptr; // the former menu bar, opened from the rail
    QFrame *toolRail = nullptr;
    QButtonGroup *railGroup = nullptr; // one button per page of tabContainer
    QToolButton *railMenuButton = nullptr;
    QToolButton *railSettingsButton = nullptr;
    QFrame *mapStatusStrip = nullptr;
    QLabel *mapBiomeName = nullptr;
    QLabel *mapCursorPos = nullptr;
    QLabel *mapScale = nullptr;
    QToolButton *mapZoomIn = nullptr;
    QToolButton *mapZoomOut = nullptr;
    QToolButton *mapOrigin = nullptr;
    QFrame *coordinateFinder = nullptr;
    QFrame *mapStripDivider = nullptr;
    QVector<QAction*> layerActions; // sidebar actions with layer icons
    QIcon mapLayerIcon(const QString& rcbase, int size) const;
    bool darkTheme = true;
    bool layerSync = false; // set while the layer actions follow lopt
    void syncLayerActions();
    void updateTerrainAvailability(int mc); // the terrain map needs 1.18+
    // The world types that exist in a version; a selected Amplified falls
    // back to the default world where there is none (before 1.18).
    void updateWorldTypeAvailability(int mc);
    // Shows a world type in the picker (the default world where the version
    // does not have it), with what follows from it: the world types that
    // exist in the version and the level of the surface entry of the Y picker.
    void showWorldType(int mc, int type);
    void setWorldTypeNames(bool compact); // short names for a narrow card
    void updateStructureActions(); // only what exists in the shown dimension
    QAction *acthome;
    QAction *actzoom[2];
    QAction *dimactions[3];
    QActionGroup *dimgroup;
    QToolButton *sidePanelToggle;
    QPropertyAnimation *sidePanelAnimation;
    int sidePanelWidth;
    int sidePanelCurrentWidth;
    bool sidePanelCollapsed;
    bool sidePanelChanging;
};

#endif // MAINWINDOW_H
