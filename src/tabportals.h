#ifndef TABPORTALS_H
#define TABPORTALS_H

#include "mainwindow.h"
#include "eyesearchdriver.h"

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QStyledItemDelegate>
#include <QThread>

#include <atomic>
#include <deque>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableView;
class CsvWriter;

// The End portals of the first strongholds of one seed.
class AnalysisPortals : public QThread
{
    Q_OBJECT
public:
    explicit AnalysisPortals(QObject *parent = nullptr) : QThread(parent), stop(false) {}
    void run() override;

    WorldInfo wi;
    int count = 3;
    std::atomic_bool stop;
    std::vector<EyePortalRecord> rows;  // filled by run()
};

// Rows of End portals, for the analysis and for the search results.
class PortalTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { SEED, INDEX, X, Y, Z, EYES, CERTAIN, EXACT, LIT, FRAMES };

    PortalTableModel(bool withSeed, QObject *parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;

    void setRows(std::vector<EyePortalRecord>&& rows);
    void appendRows(const std::vector<EyePortalRecord>& rows);
    void clear();
    const std::vector<EyePortalRecord>& records() const { return rows; }
    const EyePortalRecord& record(int row) const { return rows[size_t(row)]; }
    Column columnKind(int column) const { return columns[size_t(column)]; }
    int column(Column kind) const;

    void writeCsv(CsvWriter& csv) const;

private:
    std::vector<Column> columns;
    std::vector<EyePortalRecord> rows;
};

// Paints the 12 frames of a portal as seen from above (north up).
class PortalFramesDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    static void paintPortal(QPainter *painter, const QRectF& rect, const EyePortalRecord& r,
        bool dark);
};

class TabPortals : public QWidget, public ISaveTab
{
    Q_OBJECT
public:
    explicit TabPortals(MainWindow *parent = nullptr);
    ~TabPortals();

    void save(QSettings& settings) override;
    void load(QSettings& settings) override;
    void refresh() override;
    void stopWork() override;

private slots:
    void onAnalyzeClicked();
    void onAnalysisFinished();
    void onAnalysisRowChanged(const QModelIndex& current);
    void onSearchClicked();
    void onResetClicked();
    void onSearchFinished();
    void onSearchTimer();
    void onSearchRowActivated(const QModelIndex& index);
    void onExportSearch();
    void onExportAnalysis();

private:
    QTableView *createTable(PortalTableModel *model);
    void startAnalysis();
    bool contextSupported(QString *reason) const;
    EyeSearchSettings currentSettings(bool *ok) const;
    bool hasProgress() const;
    void updateSearchControls();
    void updateSearchStatus();
    void updateExpectation();
    void showPortal(const EyePortalRecord& r);
    bool saveSearchState();
    bool loadSearchState();
    void loadLegacySearch(QSettings& settings);

    MainWindow *parent;
    bool loaded = false;

    QLabel *labelNotice;
    QTabWidget *pages;

    // analysis of the shown seed
    AnalysisPortals analysis;
    bool analysisAgain = false;     // restart once the running one stopped
    WorldInfo analysedWorld;
    int analysedCount = 0;
    bool analysisValid = false;
    QWidget *analysisPage;
    QSpinBox *spinCount;
    QPushButton *pushAnalyze, *pushExportAnalysis;
    PortalTableModel *analysisModel;
    QTableView *analysisTable;
    QLabel *labelAnalysis;

    // seed search
    EyeSearchDriver driver;
    EyeSearchSettings searchSettings;   // what 'progress' belongs to
    bool searchValid = false;           // 'searchSettings' and 'progress' are in use
    EyeSearchProgress progress;         // when not running
    double measuredRate = 0;            // seeds per second, last measurement
    uint64_t runStartFamilies = 0;
    QElapsedTimer runClock;
    QElapsedTimer autosaveClock;        // since the search state was last written
    std::deque<std::pair<qint64, uint64_t>> rateSamples;
    QTimer *searchTimer;
    QSpinBox *spinMinEyes, *spinStrongholds;
    QCheckBox *checkExactOnly, *checkRandom;
    QLineEdit *lineStart;
    QPushButton *pushSearch, *pushReset, *pushExportSearch;
    QLabel *labelExpectation, *labelSearch;
    PortalTableModel *searchModel;
    QTableView *searchTable;
};

#endif // TABPORTALS_H
