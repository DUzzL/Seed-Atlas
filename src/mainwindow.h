#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAction>
#include <QActionGroup>
#include <QListWidgetItem>
#include <QMainWindow>
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
};

class MapView;
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

    QAction *addMapAction(int opt);
    QAction *addMapAction(QString rcbase, QString tip, QString toolbarLabel = QString());

    bool getSeed(WorldInfo *wi, bool applyrand = true);
    bool setSeed(WorldInfo wi, int dim = DIM_UNDEF);
    int getDim();
    MapView *getMapView();
    bool loadSession(QString path, bool keepresults);

protected:
    void saveSettings();
    void loadSettings();
    bool saveSession(QString path, bool quiet, QString *error = nullptr);
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
    void on_checkLarge_toggled();
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

private:
    void setRightSidebarCollapsed(bool collapsed, bool animated);
    void setSidePanelCollapsed(bool collapsed);
    int sidePanelAnimationWidth() const { return sidePanelCurrentWidth; }
    void setSidePanelAnimationWidth(int width);
    void updateSidePanelToggle();

public:
    Ui::MainWindow *ui;
    QDockWidget *dock;
    MapView *mapView;

    FormConditions *formCond;
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
    QAction *menuAction = nullptr; // opens the former menu bar menus
    QAction *settingsAction = nullptr;
    class QPushButton *coordinateGo = nullptr; // "go to" button of the map
    class TabLocations *tabLocations = nullptr;
    bool layerSync = false; // set while the layer actions follow lopt
    void syncLayerActions();
    void updateTerrainAvailability(int mc); // the terrain map needs 1.18+
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
