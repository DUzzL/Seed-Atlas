#ifndef TABDATAPACKS_H
#define TABDATAPACKS_H

#include "mainwindow.h"
#include "datapackworld.h"

#include <atomic>

class Collapsible;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QVBoxLayout;
class StyledComboBox;

// The Datapacks tool: the list of loaded data packs, the state of the world
// they make, the vanilla game data they build on, the dimension to show and
// the biomes of that dimension, which can be highlighted on the map.
class TabDatapacks : public QWidget, public ISaveTab
{
    Q_OBJECT
public:
    explicit TabDatapacks(MainWindow *parent, DatapackManager *manager);
    ~TabDatapacks();

    void save(QSettings& settings) override;
    void load(QSettings& settings) override;
    void refresh() override;

    // icons and colours follow the theme
    void refreshTheme();

    // the highlighted biomes, by id
    void setHighlightedBiomes(const QStringList& ids);
    void setBiomeFilter(const QString& text);
    // what "Copy details" copies
    QString detailsText() const;
    // one line that says what state the world is in (also for tests)
    QString stateLine() const;
    QString issuesText() const;
    // the pack check (see dpcheck.h): runs in the background and shows the
    // report when it is done; the last report is kept
    void checkPacks();
    bool checking() const { return checkThread != nullptr; }
    QString lastCheckReport() const { return checkReport; }
    // the window that shows a report (also for tests)
    void showCheckReport(const QString& report);

public slots:
    void addFiles();
    void addFolder();
    void chooseVanillaFile();
    void chooseVanillaFolder();
    void downloadVanilla();

private:
    void rebuildPacks();
    void updateState();
    void updateVanilla();
    void updateDimensions();
    void rebuildBiomes();
    void applyBiomeFilter();
    void onBiomeSelectionChanged();
    void updateBiomeCount();
    int selectedVersion() const;

    MainWindow *parent;
    DatapackManager *manager;

    Collapsible *sectionPacks;
    Collapsible *sectionWorld;
    Collapsible *sectionBiomes;

    QPushButton *buttonAdd;
    QPushButton *buttonAddFolder;
    QWidget *packRows;
    QVBoxLayout *packLayout;
    QLabel *labelNoPacks;
    QString packSignature;

    QLabel *labelState;
    QTextBrowser *issues;
    QPushButton *buttonCopy;
    QPushButton *buttonReload;
    QPushButton *buttonCheck;
    class QThread *checkThread = nullptr;
    std::atomic_bool checkAbort{false};
    QString checkReport;
    QPushButton *buttonVersion; // switches to a version with datapack worlds

    QLabel *labelVanilla;
    QPushButton *buttonDownload;
    QPushButton *buttonChooseFile;
    QPushButton *buttonChooseFolder;
    QPushButton *buttonCancel;
    QProgressBar *progress;
    QString vanillaError;

    QWidget *dimensionRow;
    StyledComboBox *comboDimension;
    QLabel *labelPreset;

    QLineEdit *lineSearch;
    QListWidget *listBiomes;
    QLabel *labelBiomeCount;
    QPushButton *buttonClear;
    std::shared_ptr<const DatapackWorld> shownWorld;
    bool updating = false;
};

#endif // TABDATAPACKS_H
