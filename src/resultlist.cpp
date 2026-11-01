#include "resultlist.h"

#include "config.h"
#include "message.h"
#include "util.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaMethod>
#include <QPainter>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <numeric>

CsvWriter::CsvWriter(QTextStream& stream, const Config& config)
    : stream(stream)
    , quote(config.quote)
    , separator(config.separator)
{
    stream << "Sep=" << separator << "\n";
}

void CsvWriter::row(QStringList fields)
{
    if (quote.isEmpty())
    {   // without a quote character, only fields containing the separator are quoted
        for (QString& s : fields)
            if (s.contains(separator))
                s = "\"" + s + "\"";
    }
    stream << quote << fields.join(quote + separator + quote) << quote << "\n";
}

bool saveTextFile(QWidget *parent, QString& prevdir, const QString& title,
        const QString& fileName, const QString& filter,
        const std::function<void(QTextStream&)>& write)
{
#if WASM
    (void) prevdir; (void) title; (void) filter;
    QByteArray content;
    QTextStream stream(&content);
    write(stream);
    stream.flush();
    QFileDialog::saveFileContent(content, fileName);
    return true;
#else
    QString path = QFileDialog::getSaveFileName(parent, title,
        QDir(prevdir).filePath(fileName), filter);
    if (path.isEmpty())
        return false;
    prevdir = QFileInfo(path).absolutePath();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        warn(parent, QApplication::translate("ResultList",
            "Failed to open file for export:\n\"%1\"").arg(path));
        return false;
    }
    QTextStream stream(&file);
    write(stream);
    stream.flush();
    return true;
#endif
}

bool exportCsv(QWidget *parent, QString& prevdir, const QString& title,
        const QString& fileName, const Config& config,
        const std::function<void(CsvWriter&)>& write)
{
    return saveTextFile(parent, prevdir, title, fileName,
        QApplication::translate("ResultList", "Text files (*.txt *csv);;Any files (*)"),
        [&](QTextStream& stream) {
            CsvWriter csv(stream, config);
            write(csv);
        });
}

void addResultMenu(QAbstractItemView *view, const std::function<void()>& exportRows)
{
    // a row with its header (e.g. the seed of a statistics row) first
    const auto rowText = [view](int row) {
        const QAbstractItemModel *model = view->model();
        QStringList fields;
        const QString rowHeader = model->headerData(row, Qt::Vertical).toString();
        if (!rowHeader.isEmpty())
            fields.append(rowHeader);
        for (int c = 0, n = model->columnCount(); c < n; c++)
            fields.append(model->data(model->index(row, c)).toString());
        return fields.join('\t');
    };
    const auto copyRows = [view, rowText](bool all) {
        QList<int> rows;
        if (all)
            for (int r = 0, n = view->model()->rowCount(); r < n; r++)
                rows.append(r);
        else
            for (const QModelIndex& index : view->selectionModel()->selectedRows())
                rows.append(index.row());
        std::sort(rows.begin(), rows.end());
        QStringList lines;
        for (int row : std::as_const(rows))
            lines.append(rowText(row));
        if (!lines.isEmpty())
            QGuiApplication::clipboard()->setText(lines.join('\n') + '\n');
    };
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(view, &QWidget::customContextMenuRequested, view,
        [view, copyRows, exportRows](const QPoint& pos) {
        QMenu menu(view);
        QAction *copy = menu.addAction(QIcon::fromTheme("edit-copy"),
            QApplication::translate("ResultTree", "Copy selected rows"), view, [copyRows]() { copyRows(false); });
        copy->setEnabled(view->selectionModel() && view->selectionModel()->hasSelection());
        QAction *copyAll = menu.addAction(QApplication::translate("ResultTree", "Copy all rows"),
            view, [copyRows]() { copyRows(true); });
        const bool rows = view->model() && view->model()->rowCount() > 0;
        copyAll->setEnabled(rows);
        menu.addSeparator();
        QAction *save = menu.addAction(QIcon::fromTheme("document-save"),
            QApplication::translate("ResultTree", "Export..."), view, exportRows);
        save->setEnabled(rows);
        menu.exec(view->viewport()->mapToGlobal(pos));
    });
}


ResultTree::ResultTree(QWidget *parent)
    : QTreeWidget(parent)
    , posted(0)
{
    configureResultTree(this);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(header(), &QHeaderView::sectionClicked, this,
        [this](int section) { cycleResultTreeSort(this, section); });
}

ResultTree::~ResultTree()
{
    qDeleteAll(pending);
}

int ResultTree::post(QTreeWidgetItem *item)
{
    return post(QList<QTreeWidgetItem*>{item});
}

int ResultTree::post(const QList<QTreeWidgetItem*>& items)
{
    bool schedule = false;
    {
        QMutexLocker locker(&mutex);
        pending.append(items);
        schedule = !flushQueued;
        flushQueued = true;
    }
    if (schedule)
    {   // collect what arrives in the meantime and add it in one batch
        QMetaObject::invokeMethod(this, [this]() {
            QTimer::singleShot(150, this, &ResultTree::flush);
        }, Qt::QueuedConnection);
    }
    return posted.fetch_add(items.size(), std::memory_order_relaxed) + items.size();
}

void ResultTree::flush()
{
    QList<QTreeWidgetItem*> items;
    {
        QMutexLocker locker(&mutex);
        items.swap(pending);
        flushQueued = false;
    }
    if (items.isEmpty())
        return;

    // Adding many items at once is fast, a batch at a time is not, and
    // measuring the columns for every batch made long lists crawl.
    setUpdatesEnabled(false);
    addTopLevelItems(items);
    if (expandDepth > 0)
        for (QTreeWidgetItem *item : std::as_const(items))
            expand(item, expandDepth);
    if (!sized)
    {   // size the free columns once from the first rows
        sized = true;
        for (int c = 0, n = columnCount(); c < n; c++)
            if (header()->sectionResizeMode(c) == QHeaderView::Interactive)
                resizeColumnToContents(c);
    }
    setUpdatesEnabled(true);
}

void ResultTree::finish()
{
    flush();
    resortResultTree(this);
}

void ResultTree::clearResults()
{
    {
        QMutexLocker locker(&mutex);
        qDeleteAll(pending);
        pending.clear();
        posted.store(0, std::memory_order_relaxed);
    }
    clear();
    sized = false;
}

void ResultTree::expand(QTreeWidgetItem *item, int depth)
{
    item->setExpanded(true);
    if (depth > 1)
        for (int i = 0, n = item->childCount(); i < n; i++)
            expand(item->child(i), depth - 1);
}

QString ResultTree::rowText(const QTreeWidgetItem *item) const
{
    QStringList fields;
    for (int c = 0, n = columnCount(); c < n; c++)
    {
        const QString text = item->text(c);
        fields.append(text == "-" ? QString() : text);
    }
    return fields.join('\t');
}

void ResultTree::writeCsv(CsvWriter& csv, const QStringList& header) const
{
    QStringList fields = header;
    if (fields.isEmpty())
        for (int c = 0, n = columnCount(); c < n; c++)
            fields.append(headerItem()->text(c));
    csv.row(fields);
    for (QTreeWidgetItemIterator it(const_cast<ResultTree*>(this)); *it; ++it)
        csv.row(rowText(*it).split('\t'));
}

void ResultTree::copyRows(bool all)
{
    QStringList lines;
    QTreeWidgetItemIterator::IteratorFlags flags = all
        ? QTreeWidgetItemIterator::All : QTreeWidgetItemIterator::Selected;
    for (QTreeWidgetItemIterator it(this, flags); *it; ++it)
        lines.append(rowText(*it));
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join('\n') + '\n');
}

void ResultTree::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *copy = menu.addAction(QIcon::fromTheme("edit-copy"), tr("Copy selected rows"),
        QKeySequence::Copy, this, [this]() { copyRows(false); });
    copy->setEnabled(!selectedItems().isEmpty());
    QAction *copyAll = menu.addAction(tr("Copy all rows"), this, [this]() { copyRows(true); });
    copyAll->setEnabled(topLevelItemCount() > 0);
    if (isSignalConnected(QMetaMethod::fromSignal(&ResultTree::exportRequested)))
    {
        menu.addSeparator();
        QAction *save = menu.addAction(QIcon::fromTheme("document-save"), tr("Export..."),
            this, &ResultTree::exportRequested);
        save->setEnabled(topLevelItemCount() > 0);
    }
    menu.exec(event->globalPos());
}

void ResultTree::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy))
        copyRows(false);
    else
        QTreeWidget::keyPressEvent(event);
}


// The view never asks for more than the rows it shows: their position is
// plain arithmetic (first row = scroll bar value), so nothing scales with the
// number of rows except handing them over and sorting.
ResultTable::ResultTable(const QStringList& headers, QWidget *parent)
    : QAbstractScrollArea(parent)
    , headers(headers)
    , head(new QHeaderView(Qt::Horizontal, this))
{
    QStandardItemModel *labels = new QStandardItemModel(0, headers.size(), this);
    labels->setHorizontalHeaderLabels(headers);
    head->setModel(labels);
    head->setSectionsClickable(true);
    head->setHighlightSections(false);
    head->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    head->setSectionResizeMode(QHeaderView::Interactive);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);

    // the same sort cycle as ResultTree: descending, ascending, none
    connect(head, &QHeaderView::sectionClicked, this, [this](int section) {
        if (sortColumn != section)
            setSort(section, Qt::DescendingOrder);
        else if (sortOrder == Qt::DescendingOrder)
            setSort(section, Qt::AscendingOrder);
        else
            setSort(-1, Qt::DescendingOrder);
    });
    connect(head, &QHeaderView::sectionResized, this, [this]() { updateLayout(); });
    updateLayout();
}

void ResultTable::updateLayout()
{
    const QFontMetrics fm(font());
    rowHeight = fm.height() + 8;
    if (head->property("sized").isNull())
    {   // wide enough for the bold header with its sort arrow, and for a
        // block coordinate
        head->setProperty("sized", true);
        QFont bold = head->font();
        bold.setBold(true);
        const QFontMetrics fmh(bold);
        const int number = fm.horizontalAdvance("-30000000") + 16;
        for (int c = 0, n = headers.size(); c < n; c++)
            head->resizeSection(c, qMax(number, fmh.horizontalAdvance(headers[c]) + 44));
    }
    const int headHeight = head->sizeHint().height();
    setViewportMargins(0, headHeight, 0, 0);
    const QRect area = viewport()->geometry();
    head->setGeometry(area.left(), area.top() - headHeight, area.width(), headHeight);

    const int visible = qMax(1, viewport()->height() / rowHeight);
    verticalScrollBar()->setRange(0, qMax(0, rowCount() - visible));
    verticalScrollBar()->setPageStep(visible);
    verticalScrollBar()->setSingleStep(1);
    horizontalScrollBar()->setRange(0, qMax(0, head->length() - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(20);
    head->setOffset(horizontalScrollBar()->value());
    viewport()->update();
}

void ResultTable::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateLayout();
}

void ResultTable::changeEvent(QEvent *event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        updateLayout();
}

void ResultTable::scrollContentsBy(int, int)
{
    head->setOffset(horizontalScrollBar()->value());
    viewport()->update();
}

void ResultTable::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    const QPalette pal = QApplication::palette();
    const QRect area = viewport()->rect();
    painter.fillRect(area, pal.color(QPalette::Base));
    const int n = headers.size();
    const int first = verticalScrollBar()->value();
    const int last = qMin(rowCount(), first + area.height() / rowHeight + 2);
    const int offset = head->offset();
    for (int row = first, y = 0; row < last; row++, y += rowHeight)
    {
        const QRect line(0, y, area.width(), rowHeight);
        const bool isSelected = selected[order[row]];
        if (isSelected)
            painter.fillRect(line, pal.color(QPalette::Highlight));
        else if (row & 1)
            painter.fillRect(line, pal.color(QPalette::AlternateBase));
        painter.setPen(pal.color(isSelected ? QPalette::HighlightedText : QPalette::Text));
        const int *cells = &values[size_t(order[row]) * n];
        for (int c = 0; c < n; c++)
        {
            const int x = head->sectionPosition(c) - offset;
            const int w = head->sectionSize(c);
            if (x + w < 0 || x > area.width())
                continue;
            painter.drawText(QRect(x + 8, y, w - 12, rowHeight),
                Qt::AlignLeft | Qt::AlignVCenter, QString::number(cells[c]));
        }
        if (row == current && hasFocus())
        {
            QColor focus = pal.color(QPalette::Highlight);
            painter.setPen(focus);
            painter.drawRect(line.adjusted(0, 0, -1, -1));
        }
    }
}

int ResultTable::value(int row, int column) const
{
    return values[size_t(order[row]) * headers.size() + column];
}

void ResultTable::setRows(std::vector<int>&& rows)
{
    values = std::move(rows);
    order.resize(values.size() / std::max<qsizetype>(1, headers.size()));
    selected.assign(order.size(), 0);
    current = anchor = -1;
    verticalScrollBar()->setValue(0);
    setSort(sortColumn, sortOrder);
}

void ResultTable::clearRows()
{
    setRows({});
}

void ResultTable::setSort(int column, Qt::SortOrder dir)
{
    sortColumn = column;
    sortOrder = dir;
    head->setSortIndicatorShown(column >= 0);
    if (column >= 0)
        head->setSortIndicator(column, dir);

    std::iota(order.begin(), order.end(), 0);
    const int n = headers.size();
    const auto key = [&](int row) {
        const int v = values[size_t(row) * n + column];
        return dir == Qt::AscendingOrder ? int64_t(v) : -int64_t(v);
    };
    // rows usually arrive in the order shown (the analyses sort them),
    // and a linear check is much cheaper than sorting them again
    const bool sorted = column < 0 || std::is_sorted(order.begin(), order.end(),
        [&](int a, int b) { return key(a) < key(b); });
    if (!sorted)
    {   // sort (key, row) pairs: contiguous and stable through the row tie-break
        std::vector<std::pair<int64_t, int>> keyed(order.size());
        for (size_t i = 0; i < keyed.size(); i++)
            keyed[i] = { key(int(i)), int(i) };
        std::sort(keyed.begin(), keyed.end());
        for (size_t i = 0; i < keyed.size(); i++)
            order[i] = keyed[i].second;
    }
    current = anchor = -1;
    updateLayout();
}

void ResultTable::select(int row, Qt::KeyboardModifiers modifiers)
{
    if (row < 0 || row >= rowCount())
        return;
    if (modifiers & Qt::ShiftModifier && anchor >= 0)
    {
        std::fill(selected.begin(), selected.end(), 0);
        for (int r = qMin(anchor, row); r <= qMax(anchor, row); r++)
            selected[order[r]] = 1;
    }
    else if (modifiers & Qt::ControlModifier)
    {
        selected[order[row]] ^= 1;
        anchor = row;
    }
    else
    {
        std::fill(selected.begin(), selected.end(), 0);
        selected[order[row]] = 1;
        anchor = row;
    }
    current = row;
    // keep the current row in view
    QScrollBar *bar = verticalScrollBar();
    if (row < bar->value())
        bar->setValue(row);
    else if (row >= bar->value() + bar->pageStep())
        bar->setValue(row - bar->pageStep() + 1);
    viewport()->update();
}

void ResultTable::mousePressEvent(QMouseEvent *event)
{
    const int row = verticalScrollBar()->value() + int(event->position().y()) / rowHeight;
    if (row >= rowCount())
        return;
    if (event->button() == Qt::LeftButton || !selected[order[row]])
        select(row, event->button() == Qt::LeftButton ? event->modifiers() : Qt::NoModifier);
    if (event->button() == Qt::LeftButton)
        emit rowClicked(row);
}

void ResultTable::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy))
        return copyRows(false);
    if (event->matches(QKeySequence::SelectAll))
    {
        std::fill(selected.begin(), selected.end(), 1);
        return viewport()->update();
    }
    const int page = verticalScrollBar()->pageStep();
    int row = current;
    switch (event->key())
    {
    case Qt::Key_Up:        row = current - 1; break;
    case Qt::Key_Down:      row = current + 1; break;
    case Qt::Key_PageUp:    row = current - page; break;
    case Qt::Key_PageDown:  row = current + page; break;
    case Qt::Key_Home:      row = 0; break;
    case Qt::Key_End:       row = rowCount() - 1; break;
    default:
        return QAbstractScrollArea::keyPressEvent(event);
    }
    row = qBound(0, row, rowCount() - 1);
    select(row, event->modifiers() & Qt::ShiftModifier);
    emit rowClicked(row);
}

QString ResultTable::rowText(int row) const
{
    QStringList fields;
    for (int c = 0, n = headers.size(); c < n; c++)
        fields.append(QString::number(value(row, c)));
    return fields.join('\t');
}

void ResultTable::copyRows(bool all) const
{
    QStringList lines;
    for (int r = 0, n = rowCount(); r < n; r++)
        if (all || selected[order[r]])
            lines.append(rowText(r));
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join('\n') + '\n');
}

void ResultTable::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    const bool any = std::find(selected.begin(), selected.end(), 1) != selected.end();
    menu.addAction(QIcon::fromTheme("edit-copy"), QApplication::translate("ResultTree",
        "Copy selected rows"), QKeySequence::Copy, this, [this]() { copyRows(false); })->setEnabled(any);
    menu.addAction(QApplication::translate("ResultTree", "Copy all rows"),
        this, [this]() { copyRows(true); })->setEnabled(rowCount() > 0);
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme("document-save"), QApplication::translate("ResultTree", "Export..."),
        this, &ResultTable::exportRequested)->setEnabled(rowCount() > 0);
    menu.exec(event->globalPos());
}

void ResultTable::writeCsv(CsvWriter& csv) const
{
    csv.row(headers);
    QStringList fields;
    for (int r = 0, rows = rowCount(); r < rows; r++)
    {
        fields.clear();
        for (int c = 0, n = headers.size(); c < n; c++)
            fields.append(QString::number(value(r, c)));
        csv.row(fields);
    }
}
