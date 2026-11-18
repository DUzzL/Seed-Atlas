#ifndef SEARCHTHREAD_H
#define SEARCHTHREAD_H

#include "search.h"
#include "config.h"
#include "gpufilter.h"
#include "dpsearch.h"

#include <QThread>
#include <QMutex>
#include <QVector>
#include <QElapsedTimer>
#include <QTimer>
#include <QMessageBox>
#include <QSemaphore>

#include <atomic>
#include <deque>
#include <map>
#include <memory>

struct Session
{
    void writeHeader(QTextStream& stream);
    bool save(QWidget *widget, QTextStream& stream);
    bool load(QWidget *widget, QTextStream& stream, bool quiet);

    WorldInfo wi;
    SearchConfig sc;
    Gen48Config gen48;
    std::vector<Condition> cv;
    std::vector<uint64_t> slist;
    // The conditions for datapack worlds. They are kept in every session
    // that has some ("#DpCond:" lines), so that they survive while the packs
    // are switched off.
    std::vector<DpCondition> dpcv;
    // The session belongs to a datapack world: it was saved while packs were
    // loaded, and its search uses dpcv ("#Datapacks:" line, with the file
    // names of the packs for messages). wi.dp is set for a search, not by load().
    bool datapack = false;
    QStringList dppacks;
    // the dimension of the datapack world ("#DpDimension:"); empty in files
    // written before the line existed
    QString dpdimension;
};

struct SearchWorker;

struct SearchMaster : QObject
{
    Q_OBJECT
public:
    SearchMaster(QWidget *parent);
    virtual ~SearchMaster();

    bool set(QWidget *widget, const Session& s);
    // the checks and the setup of a search in a world of the built-in generator
    bool setVanilla(QWidget *widget, const Session& s);

    void preSearch();

    void startSearch();
    void stopSearch();

    // Get search progress:
    //  status  : progress status summary
    //  prog    : scheduled progress in search space
    //  end     : size of search space
    //  seed    : current seed to be processed
    // Get the search speed (provided it is called at regular intervals):
    //  min,max : lower and upper search speed quartiles
    //  avg     : search speed average
    bool getProgress(QString *status, uint64_t *prog, uint64_t *end, uint64_t *seed, qreal *min, qreal *avg, qreal *max);
    // The number of seeds that all workers together have tested since the
    // search started, as of the last getProgress(). The progress position
    // is lower: it is where the worker that is furthest behind stands.
    uint64_t seedsChecked() const { return lastchecked; }

    bool requestItem(SearchWorker *item);

public slots:
    void onWorkerResults(QVector<uint64_t> seeds);
    void onWorkerFinished();

signals:
    // a batch of matching seeds, in the order of one worker
    void searchResults(QVector<uint64_t> seeds);
    void searchFinish(bool done);

public:
    struct TProg { uint64_t ns, prog; };

public:
    std::vector<SearchWorker*>  workers;

    QMutex                      mutex;
    std::atomic_bool            stop;
    QSemaphore                  resultSlots;    // result batches in flight

    std::deque<TProg>           proghist;
    QElapsedTimer               progtimer;
    QElapsedTimer               itemtimer;
    uint64_t                    count;

    SearchThreadEnv             env;

    // a search in a datapack world: the compiled datapack conditions (the
    // vanilla conditions are not used then)
    std::shared_ptr<const DpSearch> dpsearch;

    int                         searchtype;
    int                         mc;
    uint32_t                    genflags;       // world type, see WorldInfo::genFlags()
    ConditionTree               condtree;
    int                         itemsize;   // number of seeds per search item
    int                         threadcnt;  // numbr of worker threads
    Gen48Config                 gen48;      // 48-bit generator settings
    std::vector<uint64_t>       slist;      // candidate list
    uint64_t                    idx;        // index within candidate list
    uint64_t                    scnt;       // search space size
    uint64_t                    prog;       // search space progress tracker
    uint64_t                    checked;    // seeds of the finished items
    uint64_t                    lastchecked;// ... with those of the items in work
    bool                        seedunits;  // the progress counts seeds
    uint64_t                    seed;       // current seed (next to be processed)
    uint64_t                    smin;
    uint64_t                    smax;
    bool                        isdone;
    bool                        stopping;
    bool                        finishQueued;
    bool                        stoponres;

    // 48-bit family block search without a seed list: the workers scan
    // ranges of lower 48-bit seeds in parallel and hand their candidates
    // back; the master then splits each candidate into upper 16-bit items.
    // tests for the GPU prefilter (incremental searches only)
    GpuClimateFilter::Tests gputests;

    // Incremental search with a 48-bit list: the fast 48-bit pass of each
    // entry, which is the same for all upper 16 bits (0: not known yet,
    // 1: passes, 2: fails). Only used when that pass depends on the lower
    // 48 bits alone (see treeFast48IsPure()).
    std::unique_ptr<std::atomic<uint8_t>[]> lowstate;
    // Can a worker skip the fast 48-bit pass for a seed whose lower bits
    // passed the 48-bit pass?
    bool                        fast48pure;

    std::map<uint64_t, uint32_t> bcand;     // candidate low -> next high
    uint64_t                    bscan;      // next low to scan
    uint64_t                    bfirst;     // low that starts at bhigh0
    uint32_t                    bhigh0;     // first high of the resumed low
    int                         bscansize;  // number of lows per scan item
    int                         binflight;  // scan items being processed

private:
    bool requestBlockItem(SearchWorker *item);
    void updateBlockProgress();
    void deleteWorkers(const std::vector<SearchWorker*>& list);
};


struct SearchWorker : QThread
{
    Q_OBJECT
public:
    SearchWorker(SearchMaster *master);
    ~SearchWorker();

    bool getNextItem();
    // Collects a matching seed; the results go to the master in batches
    // (immediately when the search stops on the first result).
    void report(uint64_t seed);
    // Hands the collected results to the master, at the end of each item
    // (before the next item moves the progress on). False if the search
    // stops: the batch is then dropped, and since the progress still points
    // at the start of the item, found again on resume.
    bool flushResults();
    virtual void run() override;

signals:
    void results(QVector<uint64_t> seeds);

public:
    SearchMaster      * master;

    const uint64_t    * slist;      // candidate list
    uint64_t            len;        // number of candidates

    /// current work item
    enum { ITEM_SEEDS, ITEM_SCAN48, ITEM_WAIT };
    int                 itemkind;   // kind of work item
    std::vector<uint64_t> found;    // (out) candidates of a scan item
    uint64_t            scanns;     // (out) duration of the last scan item
    uint64_t            prog;       // search space progress (under the mutex)
    uint64_t            idx;        // current index in candidate buffer
    uint64_t            sstart;     // starting seed
    int                 scnt;       // number of seeds to process in this item
    bool                wantgpu;    // (in) the item is for the GPU prefilter
    int                 gpuitemsize;// (in) seeds of such an item
    // (out) the first seed of the item whose results have not been reported
    // yet, read by getProgress() as the position to resume from
    std::atomic<uint64_t> seed;
    // (out) seeds of the current item that are done
    std::atomic<uint64_t> itemdone;
    // the end seed is the highest unsigned seed value in the search space
    // (or the last entry in the seed list)

private:
    SearchThreadEnv     env;
    QVector<uint64_t>   resbuf;     // results not reported yet
    void runIncGpu(GpuClimateFilter *gpu);
    void runDatapack();
    int testSeed(uint64_t s);
    // Progress inside an item. A GPU item has thousands of seeds, and with
    // tests that take long for each seed it runs for minutes, during which
    // the search would report neither progress nor results. itemProgress()
    // moves the position of the worker to `next`, the first seed of the item
    // that is not done yet, at most a few times a second. It reads the clock,
    // which seeds that are fast to test should not pay for each time:
    // clockDue() is called after each seed and tells when to call it.
    bool clockDue();
    void itemProgress(uint64_t next);
    QElapsedTimer       cptimer;        // time since the last report
    qint64              cpchecked = 0;  // when the clock was read last
    int                 cpstride = 1;   // seeds between two looks at the clock
    int                 cpcount = 0;
    // GPU against CPU: the measured time per seed of each (0: not measured)
    double              cpunsps = 0, gpunsps = 0;
    uint64_t            gpuitems = 0;
};





#endif // SEARCHTHREAD_H
