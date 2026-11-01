#ifndef RESULTLIST_H
#define RESULTLIST_H

#include <QMutex>
#include <QStringList>
#include <QAbstractScrollArea>
#include <QTreeWidget>

#include <atomic>
#include <functional>

class QHeaderView;
class QTextStream;
struct Config;

// Writes the rows of an export in the separator and quote style of the
// settings, starting with the "Sep=" line that spreadsheets understand.
class CsvWriter
{
public:
    CsvWriter(QTextStream& stream, const Config& config);
    void row(QStringList fields);

private:
    QTextStream& stream;
    QString quote, separator;
};

// Asks for a file and writes the export into it (offers a download on WASM).
// prevdir is the remembered folder of the file dialogs.
bool saveTextFile(QWidget *parent, QString& prevdir, const QString& title,
        const QString& fileName, const QString& filter,
        const std::function<void(QTextStream&)>& write);
bool exportCsv(QWidget *parent, QString& prevdir, const QString& title,
        const QString& fileName, const Config& config,
        const std::function<void(CsvWriter&)>& write);

// The context menu of the result lists (copy rows, export) for a table view.
void addResultMenu(class QAbstractItemView *view, const std::function<void()>& exportRows);

// The result list of every analysis tab. Analysis threads hand over their
// items with post() from any thread without waiting for the GUI; the items are
// added in batches a few times per second, the columns are sized once from the
// first rows, and the sort is applied once at the end (finish()). Clicking a
// header cycles the sort, and a context menu copies or exports the rows.
class ResultTree : public QTreeWidget
{
    Q_OBJECT
public:
    explicit ResultTree(QWidget *parent = nullptr);
    ~ResultTree() override;

    // Thread-safe. Returns the number of items posted since the last clearResults().
    int post(QTreeWidgetItem *item);
    int post(const QList<QTreeWidgetItem*>& items);

    void flush();   // adds the posted items now
    void finish();  // flush and apply the sort
    void clearResults(); // removes all rows, including posted ones

    void setExpandDepth(int depth) { expandDepth = depth; }

    // header and all rows, depth first; "-" placeholders become empty fields
    void writeCsv(CsvWriter& csv, const QStringList& header = {}) const;
    QString rowText(const QTreeWidgetItem *item) const;

signals:
    void exportRequested();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void copyRows(bool all);
    void expand(QTreeWidgetItem *item, int depth);

    QMutex mutex;
    QList<QTreeWidgetItem*> pending;
    bool flushQueued = false;
    std::atomic_int posted;
    bool sized = false;
    int expandDepth = 0;
};

// A flat result list of whole numbers, for lists that can grow to millions of
// rows. The rows live in one compact array and only the rows in view are
// painted, straight from that array: there is nothing kept per row, so
// handing over, sorting and scrolling cost the same for ten or ten million
// rows. It scrolls by whole rows and has the header sorting, context menu and
// export of the other result lists.
class ResultTable : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit ResultTable(const QStringList& headers, QWidget *parent = nullptr);

    // values row by row, one per column; takes them over without copying
    void setRows(std::vector<int>&& values);
    void clearRows();
    int rowCount() const { return int(order.size()); }
    int value(int row, int column) const; // row in display order
    void setSort(int column, Qt::SortOrder order);
    QHeaderView *header() const { return head; }

    void writeCsv(CsvWriter& csv) const;

signals:
    void rowClicked(int row);
    void exportRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateLayout();
    void select(int row, Qt::KeyboardModifiers modifiers);
    void copyRows(bool all) const;
    QString rowText(int row) const;

    QStringList headers;
    std::vector<int> values;            // row by row, in the order handed over
    std::vector<int> order;             // display row -> handed-over row
    std::vector<uint8_t> selected;      // per handed-over row
    int current = -1, anchor = -1;      // display rows
    int rowHeight = 20;
    QHeaderView *head;
    int sortColumn = -1;
    Qt::SortOrder sortOrder = Qt::DescendingOrder;
};

#endif // RESULTLIST_H
