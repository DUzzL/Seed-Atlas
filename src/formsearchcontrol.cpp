#include "formsearchcontrol.h"

#include <QFileInfo>
#include "widgets.h"
#include "ui_formsearchcontrol.h"

#include "mainwindow.h"
#include "message.h"
#include "datapackworld.h"
#include "formdpconditions.h"
#include "rangedialog.h"
#include "resultlist.h"
#include "search.h"
#include "util.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
#include <QFontMetrics>
#include <QMenu>


QVariant SeedTableModel::data(const QModelIndex& index, int role) const
{
    if (role == Qt::DisplayRole)
    {
        if (index.column() == COL_SEED)
            return seeds[index.row()].txtSeed;
        if (index.column() == COL_HEX48)
            return seeds[index.row()].txtHex48;
        if (index.column() == COL_TOP16)
            return seeds[index.row()].txtTop16;
    }
    else if (role == Qt::UserRole)
    {
        if (index.column() == COL_HEX48)
            return seeds[index.row()].varHex48;
        if (index.column() == COL_TOP16)
            return seeds[index.row()].varTop16;
        return seeds[index.row()].varSeed;
    }
    else if (role == Qt::TextAlignmentRole)
    {
        static QVariant align = QVariant::fromValue((int)Qt::AlignRight | Qt::AlignVCenter);
        if (index.column() != COL_HEX48)
            return align;
    }
    return QVariant();
}

QVariant SeedTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (section < 0)
        return QVariant();
    if (role == Qt::InitialSortOrderRole)
        return QVariant::fromValue(Qt::DescendingOrder);
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
    {
        if (section == COL_SEED)
            return QVariant::fromValue(tr("seed"));
        if (section == COL_TOP16)
            return QVariant::fromValue(tr("top 16"));
        if (section == COL_HEX48)
            return QVariant::fromValue(tr("lower 48 bit"));
    }
    if (role == Qt::ToolTipRole && orientation == Qt::Horizontal)
    {   // the header texts are kept short (and stable for exports)
        if (section == COL_SEED)
            return QVariant::fromValue(tr("The world seed"));
        if (section == COL_TOP16)
            return QVariant::fromValue(tr("The upper 16 bits of the seed. Seeds that differ only "
                "here share their structure positions but not their biomes."));
        if (section == COL_HEX48)
            return QVariant::fromValue(tr("The lower 48 bits of the seed (the structure seed) in "
                "hexadecimal. They decide where most structures can generate."));
    }
    if (role == Qt::DisplayRole && orientation == Qt::Vertical)
        return QVariant::fromValue(section + 1);
    return QVariant();
}

int SeedTableModel::insertSeeds(QVector<uint64_t> newseeds)
{
    int row = seeds.size();
    beginInsertRows(QModelIndex(), row, row + newseeds.size()-1);
    for (uint64_t seed : std::as_const(newseeds))
    {
        Seed s;
        s.seed = seed;
        s.varSeed = QVariant::fromValue(seed);
        s.varTop16 = QVariant::fromValue((quint64)(seed>>48) & 0xFFFF);
        s.varHex48 = QVariant::fromValue((quint64)(seed & MASK48));
        s.txtSeed = QVariant::fromValue(QString::asprintf("%" PRId64, seed));
        s.txtTop16 = QVariant::fromValue(QString::asprintf("%04llx", (quint64)(seed>>48) & 0xFFFF));
        s.txtHex48 = QVariant::fromValue(QString::asprintf("%012llx", (quint64)(seed & MASK48)));
        seeds.append(s);
    }
    endInsertRows();
    return row;
}

void SeedTableModel::removeRow(int row)
{
    beginRemoveRows(QModelIndex(), row, row);
    seeds.removeAt(row);
    endRemoveRows();
}

void SeedTableModel::reset()
{
    if (seeds.empty())
        return;
    beginRemoveRows(QModelIndex(), 0, seeds.size() - 1);
    seeds.clear();
    endRemoveRows();
}

FormSearchControl::FormSearchControl(MainWindow *parent)
    : QWidget(parent)
    , parent(parent)
    , ui(new Ui::FormSearchControl)
    , model()
    , proxy()
    , sthread(this)
    , elapsed()
    , stimer()
    , resultfile()
    , slist64path()
    , slist64fnam()
    , slist64()
    , smin(0)
    , smax(~(uint64_t)0)
    , qbuf()
    , resultSeeds()
    , nextupdate()
    , updt(20)
{
    ui->setupUi(this);
    ui->labelStatus->setWordWrap(true);
    ui->labelStatus->setMinimumWidth(0);
    ui->labelStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    styleInfoButton(ui->buttonSearchHelp);

    // Plain names; the technical name of each search type is in its tooltip.
    // The item order is not stored (the data is), so it can follow use.
    const struct { int type; QString name, tip; } searchTypes[] = {
        { SEARCH_INC, tr("All seeds in order"),
          tr("Checks every seed, one after another, from the start seed on. "
             "The right choice for most searches, especially for biomes. "
             "(Technical name: incremental)") },
        { SEARCH_BLOCKS, tr("Structure seeds first"),
          tr("First finds structure seeds (the lower 48 bits of a seed, which decide where "
             "most structures can be) that can match, then checks all 65,536 world seeds "
             "that share each of them. Faster when the conditions ask for rare structure "
             "positions. (Technical name: 48-bit family blocks)") },
        { SEARCH_48ONLY, tr("Structure seeds only"),
          tr("Lists structure seeds (lower 48 bits) that may match, without checking full "
             "world seeds, so biomes and other full-seed conditions are not confirmed. Useful "
             "as input for the seed generator later. (Technical name: 48-bit only)") },
        { SEARCH_LIST, tr("Seeds from a file..."),
          tr("Checks only the seeds listed in a text file, one seed per line. "
             "(Technical name: seed list from file)") },
    };
    for (const auto& t : searchTypes)
    {
        ui->comboSearchType->addItem(t.name, t.type);
        ui->comboSearchType->setItemData(ui->comboSearchType->count() - 1, t.tip, Qt::ToolTipRole);
    }
    ui->label->setToolTip(tr("How the search goes through the seeds. Hover over an entry "
        "for when to pick it."));
    ui->buttonMore->setToolTip(tr("All seeds in order: limit the range of seeds. "
        "Seeds from a file: choose the file."));
    ui->label_2->setToolTip(tr("The seed the search continues from"));

    checkAdvanced = new QCheckBox(tr("Advanced options"), this);
    checkAdvanced->setToolTip(tr("Show the expert options: search type, start seed, "
        "structure seed generator and result columns, and the Locations sampling pattern"));
    ui->layoutOptions->insertWidget(0, checkAdvanced);
    ui->labelProgress->setMinimumWidth(0);
    setEmptyHint(ui->results, tr("No matching seeds yet. Add conditions, then start the search."));
    connect(checkAdvanced, &QCheckBox::toggled, this, &FormSearchControl::advancedChanged);

    model = new SeedTableModel(ui->results);
    proxy = new SeedSortProxy(ui->results);

    proxy->setSourceModel(model);
    proxy->setDynamicSortFilter(false);
    ui->results->setModel(proxy);

    ui->results->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    ui->results->setShowGrid(false);
    ui->results->horizontalHeader()->setMinimumSectionSize(24);
    ui->results->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(ui->results->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this, &FormSearchControl::onSort);
    ui->results->sortByColumn(-1, Qt::AscendingOrder);

    connect(&sthread, &SearchMaster::searchResults, this, &FormSearchControl::searchResults);
    connect(&sthread, &SearchMaster::searchFinish, this, &FormSearchControl::searchFinish, Qt::QueuedConnection);

    connect(&stimer, &QTimer::timeout, this, QOverload<>::of(&FormSearchControl::progressTimeout));

    connect(ui->results->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, &FormSearchControl::onSeedSelectionChanged);

    searchProgressReset();

    searchLockUi(false);
    updateExportButton();
}

FormSearchControl::~FormSearchControl()
{
    stimer.stop();
    sthread.stopSearch();
    delete ui;
}

bool FormSearchControl::event(QEvent *e)
{
    if (e->type() == QEvent::LayoutRequest)
    {
        QFontMetrics fm = QFontMetrics(ui->results->font());
        ui->results->verticalHeader()->setDefaultSectionSize(fm.height());
    }
    return QWidget::event(e);
}

std::vector<uint64_t> FormSearchControl::getResults()
{
    int n = proxy->rowCount();
    std::vector<uint64_t> results (n);
    for (int i = 0; i < n; i++)
    {
        results[i] = proxy->data(proxy->index(i, SeedTableModel::COL_SEED), Qt::UserRole).toULongLong();
    }
    return results;
}

SearchConfig FormSearchControl::getSearchConfig()
{
    SearchConfig s;
    s.searchtype = ui->comboSearchType->currentData().toInt();
    s.threads = configuredThreadCount();
    s.slist64path = slist64path;
    s.startseed = ui->lineStart->text().toLongLong();
    s.stoponres = ui->checkStop->isChecked();
    s.smin = smin;
    s.smax = smax;
    return s;
}

bool FormSearchControl::setSearchConfig(SearchConfig s, bool quiet)
{
    bool ok = true;
    if (s.searchtype >= SEARCH_INC && s.searchtype <= SEARCH_48ONLY)
    {
        ui->comboSearchType->setCurrentIndex(ui->comboSearchType->findData(s.searchtype));
        on_comboSearchType_currentIndexChanged(s.searchtype);
    }
    else
    {
        ok = false;
    }

    ui->checkStop->setChecked(s.stoponres);
    smin = s.smin;
    smax = s.smax;

#if WASM
    (void) quiet;
#else
    if (ok)
        ok &= setList64(s.slist64path, quiet);
#endif

    ui->lineStart->setText(QString::asprintf("%" PRId64, (int64_t)s.startseed));

    return ok;
}

void FormSearchControl::stopSearch()
{
    sthread.stopSearch();
    onBufferTimeout();
}

bool FormSearchControl::setList64(QString path, bool quiet)
{
    if (!path.isEmpty())
    {
        QFileInfo finfo(path);
        parent->prevdir = finfo.absolutePath();
        slist64fnam = finfo.fileName();
        slist64path = path;
        uint64_t *l = NULL;
        uint64_t len;
        QByteArray ba = path.toLocal8Bit();
        l = loadSavedSeeds(ba.data(), &len);
        if (l != NULL)
        {
            slist64.assign(l, l+len);
            updateSearchProgress(0, len, l[0]);
            free(l);
            return true;
        }
        else if (!quiet)
        {
            int button = warn(this, tr("Warning"),
                tr("Failed to load the seed list from file:\n\"%1\"").arg(path),
                tr("Reset list path?"), QMessageBox::Reset | QMessageBox::Ignore);
            if (button != QMessageBox::Ignore)
            {
                slist64fnam.clear();
                slist64path.clear();
                slist64.clear();
            }
        }
    }
    return false;
}

bool FormSearchControl::setList64(QTextStream& stream)
{
    slist64fnam.clear();
    slist64path.clear();
    slist64.clear();

    while (!stream.atEnd())
    {
        QByteArray line = stream.readLine().toLocal8Bit();
        uint64_t s = 0;
        if (sscanf(line.data(), "%" PRId64, (int64_t*)&s) == 1)
            slist64.push_back(s);
    }

    if (!slist64.empty())
        updateSearchProgress(0, slist64.size(), slist64[0]);

    return true;
}

void FormSearchControl::setResultsPath(QString path)
{
    resultfile.close();
    resultfile.setFileName(path);
}

void FormSearchControl::searchLockUi(bool lock)
{
    searchRunning = lock;
    if (lock)
    {
        ui->comboSearchType->setEnabled(false);
        ui->buttonMore->setEnabled(false);
    }
    else
    {
        ui->buttonStart->setText(tr("Start search"));
        ui->buttonStart->setChecked(false);
        ui->buttonStart->setEnabled(true);
        ui->comboSearchType->setEnabled(true);
        int type = ui->comboSearchType->currentData().toInt();
        ui->buttonMore->setEnabled(type == SEARCH_INC || type == SEARCH_LIST);
    }
    emit searchStatusChanged(lock);
}

void FormSearchControl::setAdvanced(bool advanced)
{
    {
        const QSignalBlocker blocker(checkAdvanced);
        checkAdvanced->setChecked(advanced);
    }
    // keep an expert choice visible while it is in effect
    bool special = ui->comboSearchType->currentData().toInt() != SEARCH_INC;
    for (QWidget *w : {(QWidget*)ui->label, (QWidget*)ui->buttonSearchHelp,
            (QWidget*)ui->comboSearchType, (QWidget*)ui->buttonMore})
        w->setVisible(advanced || special);
    ui->label_2->setVisible(advanced);
    ui->lineStart->setVisible(advanced);
    ui->results->setColumnHidden(1, !advanced);
    ui->results->setColumnHidden(2, !advanced);
    this->advanced = advanced;
    if (!isSearching())
        searchProgressReset();
}

bool FormSearchControl::isSearching() const
{
    return ui->buttonStart->isChecked();
}

void FormSearchControl::setSearchMode(int mode)
{
    ui->comboSearchType->setCurrentIndex(ui->comboSearchType->findData(mode));
    if (mode == SEARCH_LIST)
    {
        on_buttonMore_clicked();
    }
    else
    {
        slist64.clear();
        slist64path.clear();
        slist64fnam.clear();
    }
}

void FormSearchControl::setSearchRange(uint64_t smin, uint64_t smax)
{
    this->smin = smin;
    this->smax = smax;
    searchProgressReset();
}

bool FormSearchControl::getSeed(int row, uint64_t *seed)
{
    QAbstractItemModel *model = ui->results->model();
    if (row < 0 || row >= model->rowCount())
        return false;
    QModelIndex idx = model->index(row, SeedTableModel::COL_SEED);
    *seed = model->data(idx, Qt::UserRole).toULongLong();
    return true;
}


void FormSearchControl::invalidateResults()
{
    if (searchRunning && !discardSearch)
    {   // the running search still uses the old filters: stop it and ignore
        // what it reports until it has finished (see searchFinish())
        discardSearch = true;
        ui->buttonStart->setText(tr("Start search"));
        ui->buttonStart->setChecked(false);
        ui->buttonStart->setEnabled(false);
        sthread.stopSearch();
    }
    // the progress belongs to the discarded results, so start over as well
    model->reset();
    resultSeeds.clear();
    qbuf.clear();
    updateExportButton();
    searchProgressReset();
    ui->lineStart->setText("0");
}

void FormSearchControl::on_buttonClear_clicked()
{
    invalidateResults();
}

void FormSearchControl::on_buttonStart_clicked()
{
    if (ui->buttonStart->isChecked() && parent->datapackMode())
    {   // a datapack world: its own conditions, tested with the datapack generator
        startDatapackSearch();
        update();
        return;
    }
    if (ui->buttonStart->isChecked())
    {
        Session session;
        parent->getSeed(&session.wi);
        session.wi.dp = nullptr; // a vanilla search
        session.cv = parent->formCond->getConditions();
        session.dpcv = parent->formDpCond->getConditions();
        session.sc = getSearchConfig();
        int ok = true;

        if (session.cv.empty())
        {
            warn(this, tr("Add at least one condition first: pick it in the row above the list "
                "and press \"+ Add\", or use \"More options...\"."));
            ok = false;
        }
        if (session.sc.searchtype == SEARCH_LIST && slist64.empty())
        {
            warn(this, tr("No seed list file selected."));
            ok = false;
        }
        if (ok)
        {   // quad conditions require their structure check, otherwise the
            // search matches the 48-bit constellation pattern alone and returns
            // seeds where the huts/monuments do not actually generate
            bool missing = false;
            for (const Condition& c : session.cv)
            {
                if (c.type < F_QH_IDEAL || c.type > F_QM_90)
                    continue;
                if (c.meta & Condition::DISABLED)
                    continue;
                bool active = false;
                for (const Condition& d : session.cv)
                {
                    if (d.relative == c.save && !(d.meta & Condition::DISABLED)
                            && d.type == quadStructureType(c.type))
                    {
                        active = true;
                        break;
                    }
                }
                if (!active)
                {
                    missing = true;
                    break;
                }
            }
            if (missing)
            {
                int button = warn(this, tr("Quad condition without structure check"),
                    tr("A quad condition has no active structure check. Without it, "
                       "the search matches the constellation of structure positions "
                       "alone and can also return seeds where the huts/monuments do "
                       "not actually generate."),
                    tr("Add the missing structure check for a reliable search?"),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
                if (button == QMessageBox::Yes)
                {
                    parent->formCond->ensureQuadDependencies(true);
                    session.cv = parent->formCond->getConditions();
                }
                else if (button != QMessageBox::No)
                {
                    ok = false;
                }
            }
        }
        if (ok)
        {
            session.gen48 = parent->formGen48->getConfig(true);
            // the search can either use a full list or a 48-bit list
            if (session.sc.searchtype == SEARCH_LIST)
                session.slist = slist64;
            else if (session.gen48.mode == GEN48_LIST)
                session.slist = parent->formGen48->getList48();
            else
                session.slist.clear();

            // results found for another version or world type do not
            // belong to this search, nor does the position to resume from
            // (also without results: the position would skip seeds of the other world)
            session.sc.startseed = prepareResultsFor(session.wi, session.sc.startseed);

            ok = sthread.set(parent, session);
        }

        if (ok)
            beginSearch(session);
        else
        {
            ui->buttonStart->setChecked(false);
        }
    }
    else
    {
        ui->buttonStart->setText(tr("Start search"));
        ui->buttonStart->setChecked(false);

        // disable until finish
        ui->buttonStart->setEnabled(false);
        stopSearch();
    }

    update();
}

ResultsWorld ResultsWorld::of(const WorldInfo& wi)
{
    ResultsWorld w;
    w.set = true;
    w.mc = wi.mc;
    w.worldType = wi.worldType();
    if (wi.dp)
    {
        w.datapack = true;
        w.preset = wi.dp->preset;
        w.dimension = wi.dp->dimensionId;
        for (const PackEntry& pack : wi.dp->packs)
            if (pack.enabled)
                w.packs.append(QFileInfo(pack.path).fileName());
    }
    return w;
}

bool FormSearchControl::hasResultsOrProgress()
{
    return !model->seeds.empty() || getSearchConfig().startseed != 0;
}

uint64_t FormSearchControl::prepareResultsFor(const WorldInfo& world, uint64_t startseed)
{
    const ResultsWorld next = ResultsWorld::of(world);
    if ((!model->seeds.empty() || resultsOf.set) && next != resultsOf)
    {
        invalidateResults();
        startseed = 0;
    }
    resultsOf = next;
    return startseed;
}

void FormSearchControl::startDatapackSearch()
{
    Session session;
    parent->getSeed(&session.wi);
    session.cv = parent->formCond->getConditions();
    session.dpcv = parent->formDpCond->getConditions();
    session.sc = getSearchConfig();
    session.gen48 = parent->formGen48->getConfig(false);
    session.datapack = true;
    if (session.wi.dp)
    {
        const ResultsWorld world = ResultsWorld::of(session.wi);
        session.dppacks = world.packs;
        session.dpdimension = world.dimension;
    }
    bool ok = true;
    if (!session.wi.dp || !session.wi.dp->usable())
    {
        warn(this, tr("The datapack world is not ready: %1")
            .arg(session.wi.dp ? session.wi.dp->stateText().replace('\n', ' ')
                : tr("no datapack world")));
        ok = false;
    }
    else if (session.dpcv.empty())
    {
        warn(this, tr("Add at least one datapack condition first: pick a biome in the row "
            "above the list and press \"+ Add\", or use \"More options...\"."));
        ok = false;
    }
    else if (session.sc.searchtype == SEARCH_LIST && slist64.empty())
    {
        warn(this, tr("No seed list file selected."));
        ok = false;
    }
    if (ok)
    {   // whole seeds only: the seed generator (48-bit lists) does not apply
        session.slist = session.sc.searchtype == SEARCH_LIST ? slist64 : std::vector<uint64_t>();
        session.sc.startseed = prepareResultsFor(session.wi, session.sc.startseed);
        ok = sthread.set(parent, session);
    }
    if (ok)
        beginSearch(session);
    else
        ui->buttonStart->setChecked(false);
}

void FormSearchControl::beginSearch(Session& session)
{
    // keep the results: a changed filter has already discarded them,
    // so the list only holds seeds found up to the resume position
    if (!resultfile.fileName().isEmpty())
    {   // the file holds the search and its results so far: the ones
        // kept for a resumed search are written again after the header
        resultfile.close();
        if (resultfile.open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Text))
        {
            QTextStream stream(&resultfile);
            session.writeHeader(stream);
            for (const SeedTableModel::Seed& s : std::as_const(model->seeds))
                stream << QString::asprintf("%" PRId64 "\n", (int64_t)s.seed);
            stream.flush();
        }
    }
    ui->lineStart->setText(QString::asprintf("%" PRId64, (int64_t)session.sc.startseed));
    ui->buttonStart->setText(tr("Abort search"));
    searchLockUi(true);
    nextupdate = 0;
    updt = 20;
    sthread.startSearch();
    elapsed.start();
    stimer.start(250);
}

void FormSearchControl::on_buttonMore_clicked()
{
    int type = ui->comboSearchType->currentData().toInt();
    if (type == SEARCH_LIST)
    {
        QString filter = tr("Text files (*.txt);;Any files (*)");
#if WASM
        auto fileOpenCompleted = [=](const QString &fnam, const QByteArray &content) {
            if (!fnam.isEmpty()) {
                QTextStream stream(content);
                setList64(stream);
            }
        };
        QFileDialog::getOpenFileContent(filter, fileOpenCompleted);
#else
        QString fnam = QFileDialog::getOpenFileName(this, tr("Load seed list"), parent->prevdir, filter);
        setList64(fnam, false);
#endif
    }
    else if (type == SEARCH_INC)
    {
        RangeDialog *dialog = new RangeDialog(this, smin, smax);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &RangeDialog::applyBounds, this, &FormSearchControl::setSearchRange);
        dialog->show();
    }
}

void FormSearchControl::onSeedSelectionChanged()
{
    uint64_t s;
    if (getSeed(ui->results->currentIndex().row(), &s))
        emit selectedSeedChanged(s);
}

void FormSearchControl::on_results_clicked(const QModelIndex &)
{
    onSeedSelectionChanged();
}

void FormSearchControl::on_results_customContextMenuRequested(const QPoint &pos)
{
    QMenu *menu = new QMenu(this);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);

    // this is a contextual temporary menu so shortcuts are only indicated here,
    // but will not function - see keyReleaseEvent() for shortcut implementation

    QAction *actremove = menu->addAction(QIcon::fromTheme("list-remove"),
        tr("Remove selected seed"), QKeySequence::Delete, this,
        &FormSearchControl::removeCurrent);
    actremove->setEnabled(ui->results->selectionModel()->hasSelection());

    QAction *actcopyseed = menu->addAction(QIcon::fromTheme("edit-copy"),
        tr("Copy selected seed"), QKeySequence::Copy, this,
        &FormSearchControl::copySeed);
    actcopyseed->setEnabled(ui->results->selectionModel()->hasSelection());

    QAction *actcopylist = menu->addAction(QIcon::fromTheme("edit-copy"),
        tr("Copy seed list"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this,
        &FormSearchControl::copyResults);
    actcopylist->setEnabled(ui->results->model()->rowCount() > 0);

    int n = pasteList(true);
    QAction *actpaste = menu->addAction(QIcon::fromTheme("edit-paste"),
        tr("Paste %n seed(s) from clipboard", "", n), QKeySequence::Paste, this,
        &FormSearchControl::pasteResults);
    actpaste->setEnabled(n > 0);
    menu->addSeparator();
    QAction *actexport = menu->addAction(QIcon::fromTheme("document-save"), tr("Export..."),
        this, &FormSearchControl::on_buttonExport_clicked);
    actexport->setEnabled(ui->results->model()->rowCount() > 0);
    menu->popup(ui->results->mapToGlobal(pos));
}

void FormSearchControl::on_buttonSearchHelp_clicked()
{
    InfoPopup::toggle(ui->buttonSearchHelp, tr("Help: search types"),
        tr(
        "<html><head/><body><p>"
        "A world seed has 64 bits. The lower 48 bits, the <i>structure seed</i>, "
        "decide where most structures can generate; the upper 16 bits change "
        "the biomes and terrain."
        "</p><p>"
        "<b>All seeds in order</b> (incremental) checks seeds in numerical "
        "order. This is the recommended option for most searches. You can "
        "limit it to a range of seeds with the &quot;...&quot; button."
        "</p><p>"
        "<b>Structure seeds first</b> (48-bit family blocks) looks for "
        "structure seeds that can match first, and then checks all 65,536 world "
        "seeds that share each of them. It suits searches with rare structure "
        "requirements and searches through all seeds."
        "</p><p>"
        "<b>Structure seeds only</b> (48-bit only) checks only structure seeds "
        "and does not test the full conditions. It lists structure seeds that "
        "may match, whatever the upper 16 bits are. A session saved from this "
        "search can later be used with the seed generator to look for "
        "matching world seeds."
        "</p><p>"
        "<b>Seeds from a file</b> searches through an existing list of seeds: "
        "a text file with one decimal seed per line. Choose the file with the "
        "&quot;...&quot; button. (The seed generator is ignored with this "
        "option.)"
        "</p></body></html>"
        ));
}

void FormSearchControl::on_comboSearchType_currentIndexChanged(int)
{
    int type = ui->comboSearchType->currentData().toInt();
    ui->buttonMore->setEnabled(type == SEARCH_INC || type == SEARCH_LIST);
    if (checkAdvanced)
        setAdvanced(advanced); // shows the type while it differs from the default
    else
        searchProgressReset();
}

void FormSearchControl::pasteResults()
{
    pasteList(false);
}

int FormSearchControl::pasteList(bool dummy)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QStringList slist = clipboard->text().split('\n');
    std::vector<uint64_t> seeds;

    int skipped = 0;
    for (QString s : std::as_const(slist))
    {
        s = s.trimmed();
        if (s.isEmpty())
            continue;
        bool ok = true;
        uint64_t seed = (uint64_t) s.toLongLong(&ok);
        if (!ok) // unsigned notation of a negative seed
            seed = s.toULongLong(&ok);
        if (!ok)
        {   // lines that are no seeds are left out, and reported below
            skipped++;
            continue;
        }
        seeds.push_back(seed);
    }

    int added = 0;
    if (!seeds.empty())
        added = searchResultsAdd(seeds, dummy);
    if (!dummy && skipped)
    {
        ui->labelStatus->setText(tr("Pasted %n seed(s); ", "", added) +
            tr("%n line(s) of the clipboard are no seeds and were skipped.", "", skipped));
    }
    return added;
}

void FormSearchControl::onSort(int, Qt::SortOrder)
{
    // We want to achieve: none -> descending -> ascending -> none
    // The headerview flips the indicator with the logic:
    //  if (same_section)
    //      new_order = old_order == descending ? ascending : descending
    //  else
    //      new_order = InitialSortOrder (else ascending)
    QHeaderView *header = ui->results->horizontalHeader();

    if (proxy->order == Qt::AscendingOrder && proxy->column != -1)
    {
        header->setSortIndicatorShown(false);
        header->setSortIndicator(-1, Qt::DescendingOrder);
        proxy->column = -1;
    }
    else
    {
        header->setSortIndicatorShown(true);
    }
}

void FormSearchControl::searchResults(QVector<uint64_t> seeds)
{
    if (discardSearch || seeds.empty())
        return;
    if (resultfile.isOpen())
    {
        QByteArray buf;
        for (uint64_t seed : seeds)
        {
            char s[32];
            snprintf(s, sizeof(s), "%" PRId64 "\n", (int64_t)seed);
            buf += s;
        }
        resultfile.write(buf);
        resultfile.flush(); // once per batch
    }

    qbuf.insert(qbuf.end(), seeds.begin(), seeds.end());
    if (ui->checkStop->isChecked())
    {
        searchResultsAdd(qbuf, false);
        qbuf.clear();
        return;
    }

    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &FormSearchControl::onBufferTimeout);
    }
}

void FormSearchControl::onBufferTimeout()
{
    uint64_t t = -elapsed.elapsed();

    searchResultsAdd(qbuf, false);
    qbuf.clear();

    QApplication::processEvents(); // force processing of events so we can time correctly

    t += elapsed.elapsed();
    if (8*t > updt)
        updt = 4*t;
    nextupdate = elapsed.nsecsElapsed() + 1e6 * updt;
}

int FormSearchControl::searchResultsAdd(std::vector<uint64_t> seeds, bool countonly)
{
    if (seeds.empty())
        return 0;
    const Config& config = parent->config;
    int n = model->seeds.size();
    int nold = n;
    bool discarded = false;

    // a count-only query (e.g. for the context menu) must not affect the search
    if (n >= config.maxMatching)
    {
        if (!countonly)
            sthread.stopSearch();
        discarded = true;
    }
    if (n + (ssize_t)seeds.size() > config.maxMatching)
    {
        if (!countonly)
            sthread.stopSearch();
        discarded = true;
        // the limit can be lowered below the current result count in the settings
        seeds.resize(std::max(0, config.maxMatching - n));
    }

    resultSeeds.reserve(n + seeds.size());

    QVector<uint64_t> newseeds;
    for (uint64_t s : seeds)
    {
        if (resultSeeds.contains(s))
            continue;
        if (!countonly)
        {
            resultSeeds.insert(s);
            newseeds.append(s);
        }
        n++;
    }
    if (!newseeds.empty())
        model->insertSeeds(newseeds);
    updateExportButton();

    int addcnt = n - nold;
    if (countonly)
        return addcnt;

    if (nold == 0 && addcnt > 0 && !(searchRunning && !discardSearch))
    {   // pasted or loaded seeds belong to the world that is shown
        WorldInfo wi;
        parent->getSeed(&wi, false);
        resultsOf = ResultsWorld::of(wi);
    }

    if (ui->checkStop->isChecked() && addcnt)
        sthread.stopSearch();

    if (discarded)
    {
        // guard against recursive calls, which can occur when the
        // eventloop is continued in the warning message dialog
        thread_local bool warn_active = false;
        if (!warn_active)
        {
            warn_active = true;
            warn(this, tr("Maximum number of results reached (%1).").arg(config.maxMatching));
            warn_active = false;
        }
    }

    if (addcnt)
        emit resultsAdded(addcnt);

    return addcnt;
}

void FormSearchControl::searchProgressReset()
{
    uint64_t cnt = parent->formGen48->estimateSeedCnt();
    if (cnt > MASK48)
        cnt = ~(uint64_t)0;
    else
        cnt <<= 16;

    QString fmt;
    int searchtype = ui->comboSearchType->currentData().toInt();
    if (searchtype == SEARCH_LIST)
    {
        if (!slist64fnam.isEmpty())
        {
            fmt = slist64fnam + ": ";
            cnt = slist64.size();
        }
    }
    if (searchtype == SEARCH_INC)
    {
        if (smin != 0 || smax != ~(uint64_t)0)
        {
            fmt = QString::asprintf(" [%" PRIu64 " - %" PRIu64 "]: ", smin, smax);
            cnt = 0;
        }
    }

    if (!advanced && searchtype == SEARCH_INC && fmt.isEmpty())
        fmt = QString(); // the full 64-bit count means nothing to most users
    else if (cnt)
        fmt += QString::asprintf("0 / %" PRIu64 " (0.00%%)", cnt);
    else
        fmt += "0 / ? (0.00%%)";

    ui->progressBar->setValue(0);
    setProgressText(fmt);

    if (parent)
        parent->setProgressIndication();
}

void FormSearchControl::updateSearchProgress(uint64_t prog, uint64_t end, int64_t seed)
{
    ui->lineStart->setText(QString::asprintf("%" PRId64, seed));

    if (!end)
        return;

    double value = (double)prog / end;
    int v = (int) floor(10000 * value);
    ui->progressBar->setValue(v);
    QString fmt = QString::asprintf(
                "%" PRIu64 " / %" PRIu64 " (%d.%02d%%)",
                prog, end, v / 100, v % 100
                );
    int searchtype = ui->comboSearchType->currentData().toInt();
    if (searchtype == SEARCH_LIST)
    {
        if (!slist64fnam.isEmpty())
        {
            fmt = slist64fnam + ": " + fmt;
        }
    }
    if (searchtype == SEARCH_INC)
    {
        if (smin != 0 || smax != ~(uint64_t)0)
        {
            fmt = QString::asprintf("[%" PRIu64 " - %" PRIu64 "]: ", smin, smax) + fmt;
        }
        else if (!advanced)
        {
            // all workers together, the position alone lags behind
            fmt = tr("%L1 seeds checked", "Progressbar").arg(
                std::max(prog, sthread.seedsChecked()));
        }
    }
    setProgressText(fmt);

    if (parent)
        parent->setProgressIndication(value);
}

void FormSearchControl::setProgressText(const QString& format)
{
    ui->progressBar->setFormat(format);
    ui->labelProgress->setText(format.isEmpty() ? QString() : ui->progressBar->text());
    ui->labelProgress->setToolTip(ui->labelProgress->text());
}

void FormSearchControl::searchFinish(bool done)
{
    if (discardSearch)
    {   // stopped because the filters changed: keep the reset list and progress
        discardSearch = false;
        stimer.stop();
        qbuf.clear();
        ui->labelStatus->setText(tr("Search stopped because the conditions changed."));
        searchLockUi(false);
        if (parent)
            parent->setProgressIndication();
        return;
    }
    stimer.stop();
    onBufferTimeout();
    if (proxy->column >= 0)
        proxy->sort(proxy->column, proxy->order);
    progressTimeout();
    if (done)
    {
        ui->lineStart->setText(QString::asprintf("%" PRId64, sthread.smax));
        ui->progressBar->setValue(10000);
        setProgressText(tr("Done", "Progressbar"));
    }
    ui->labelStatus->setText(done
        ? tr("Search complete. %1 results.").arg(model->seeds.size())
        : tr("Search stopped. %1 results.").arg(model->seeds.size()));
    searchLockUi(false);

    if (parent)
        parent->setProgressIndication();
}

void FormSearchControl::progressTimeout()
{
    uint64_t prog, end, seed;
    qreal min, avg, max;
    QString status = tr("Running...", "Progressbar");
    if (discardSearch)
        return; // the progress was reset with the results
    if (!sthread.getProgress(&status, &prog, &end, &seed, &min, &avg, &max))
        return;

    updateSearchProgress(prog, end, seed);

    if (!advanced && isSearching())
    {   // instead of seeds/sec, isize and an eta for the whole seed space
        int found = ui->results->model()->rowCount();
        status = tr("Searching... %1 found so far").arg(found);
        // datapack searches are slow: their speed tells what to expect
        if (sthread.dpsearch && std::isfinite(avg))
            status += QString::fromUtf8(" \u00b7 ") + tr("%L1 seeds per second")
                .arg(avg, 0, 'f', avg < 10 ? 1 : 0);
    }
    ui->labelStatus->setText(status);

    update();
}

void FormSearchControl::removeCurrent()
{
    QModelIndex index = ui->results->currentIndex();
    int row = proxy->mapToSource(index).row();
    if (row >= 0)
    {
        resultSeeds.remove(model->seeds.at(row).seed);
        model->removeRow(row);
        updateExportButton();
    }
}

void FormSearchControl::copySeed()
{
    uint64_t seed;
    if (getSeed(ui->results->currentIndex().row(), &seed))
    {
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QString::asprintf("%" PRId64, seed));
    }
}

void FormSearchControl::copyResults()
{
    QString text;
    int n = ui->results->model()->rowCount();
    for (int i = 0; i < n; i++)
    {
        uint64_t seed;
        if (getSeed(i, &seed))
            text += QString::asprintf("%" PRId64 "\n", seed);
    }
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(text);
}

void FormSearchControl::updateExportButton()
{
    ui->buttonExport->setEnabled(ui->results->model()->rowCount() > 0);
}

void FormSearchControl::on_buttonExport_clicked()
{
    const std::vector<uint64_t> results = getResults();
    if (results.empty())
        return;
    // one signed seed per line, as the paste and the seed list search read it
    saveTextFile(this, parent->prevdir, tr("Export matching seeds"), "seeds.txt",
        tr("Text files (*.txt);;Any files (*)"), [&](QTextStream& stream) {
        for (uint64_t seed : results)
            stream << QString::number(qint64(seed)) << '\n';
    });
}

void FormSearchControl::keyReleaseEvent(QKeyEvent *event)
{
    if (ui->results->hasFocus())
    {
        if (event->matches(QKeySequence::Delete))
            removeCurrent();
        else if (event->matches(QKeySequence::Copy))
            copySeed();
        else if ((event->modifiers() & Qt::CTRL) && (event->modifiers() & Qt::SHIFT) && event->key() == Qt::Key_C)
            copyResults();
        else if (event->matches(QKeySequence::Paste))
            pasteResults();
    }
    QWidget::keyReleaseEvent(event);
}
