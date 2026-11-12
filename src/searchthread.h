#ifndef SEARCHTHREAD_H
#define SEARCHTHREAD_H

#include "search.h"
#include "config.h"
#include "gpufilter.h"

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
};

struct SearchWorker;

struct SearchMaster : QObject
{
    Q_OBJECT
public:
    SearchMaster(QWidget *parent);
    virtual ~SearchMaster();

    bool set(QWidget *widget, const Session& s);

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

    int                         searchtype;
    int                         mc;
    int                         large;
    ConditionTree               condtree;
    int                         itemsize;   // number of seeds per search item
    int                         threadcnt;  // numbr of worker threads
    Gen48Config                 gen48;      // 48-bit generator settings
    std::vector<uint64_t>       slist;      // candidate list
    uint64_t                    idx;        // index within candidate list
    uint64_t                    scnt;       // search space size
    uint64_t                    prog;       // search space progress tracker
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
    // the end seed is the highest unsigned seed value in the search space
    // (or the last entry in the seed list)

private:
    SearchThreadEnv     env;
    QVector<uint64_t>   resbuf;     // results not reported yet
    void runIncGpu(GpuClimateFilter *gpu);
    int testSeed(uint64_t s);
    // GPU against CPU: the measured time per seed of each (0: not measured)
    double              cpunsps = 0, gpunsps = 0;
    uint64_t            gpuitems = 0;
};





#endif // SEARCHTHREAD_H
