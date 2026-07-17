#ifndef MAPPINGWORKERS_H
#define MAPPINGWORKERS_H

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

// Mapping is explicitly a foreground, CPU-bound operation. On Windows the
// normal priority class can leave logical processors to competing background
// tools even though every mapping worker is ready. Raise the whole process
// while a mapping group is active so the GUI remains at the same relative
// priority, and restore the previous class after the last concurrent group.
class MappingPriorityScope
{
public:
    MappingPriorityScope()
    {
#ifdef Q_OS_WIN
        std::lock_guard<std::mutex> lock(mutex());
        if (users()++ == 0)
        {
            original() = GetPriorityClass(GetCurrentProcess());
            changed() = original() && original() != HIGH_PRIORITY_CLASS &&
                original() != REALTIME_PRIORITY_CLASS &&
                SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        }
#endif
    }

    ~MappingPriorityScope()
    {
#ifdef Q_OS_WIN
        std::lock_guard<std::mutex> lock(mutex());
        if (--users() == 0 && changed())
        {
            SetPriorityClass(GetCurrentProcess(), original());
            changed() = false;
        }
#endif
    }

private:
#ifdef Q_OS_WIN
    static std::mutex& mutex() { static std::mutex value; return value; }
    static size_t& users() { static size_t value = 0; return value; }
    static DWORD& original() { static DWORD value = 0; return value; }
    static bool& changed() { static bool value = false; return value; }
#endif
};

inline size_t mappingThreadCount()
{
    return size_t(std::max(1, QThread::idealThreadCount()));
}

inline void waitForMappingThread(QThread& thread)
{
    while (!thread.wait(10))
    {
        QCoreApplication *app = QCoreApplication::instance();
        if (app && QThread::currentThread() == app->thread())
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
}

// Runtime counters shared by all Seed Mapping analyses. Besides making the
// current worker use visible to the UI, these counters give the headless
// regression test a way to verify that a heavy mapping job really starts one
// worker per logical CPU.
struct MappingWorkerState
{
    std::atomic_size_t planned{0};
    std::atomic_size_t started{0};
    std::atomic_size_t active{0};
    std::atomic_size_t peak{0};

    void reset()
    {
        planned.store(0, std::memory_order_relaxed);
        started.store(0, std::memory_order_relaxed);
        active.store(0, std::memory_order_relaxed);
        peak.store(0, std::memory_order_relaxed);
    }

    void enter()
    {
        started.fetch_add(1, std::memory_order_relaxed);
        const size_t now = active.fetch_add(1, std::memory_order_relaxed) + 1;
        size_t oldPeak = peak.load(std::memory_order_relaxed);
        while (oldPeak < now && !peak.compare_exchange_weak(oldPeak, now,
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    void leave()
    {
        active.fetch_sub(1, std::memory_order_relaxed);
    }
};

template <class Worker>
size_t runMappingWorkers(uint64_t jobCount, MappingWorkerState& state, Worker&& worker)
{
    const size_t workerCount = size_t(std::min<uint64_t>(jobCount, mappingThreadCount()));
    state.planned.store(workerCount, std::memory_order_relaxed);
    if (!workerCount)
        return 0;
    MappingPriorityScope priorityScope;

    // Release all workers together. This prevents a short first job from
    // finishing while the remaining OS threads are still being created.
    std::atomic_size_t ready{0};
    std::atomic_bool go{false};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; i++)
    {
        workers.emplace_back([&, i]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            state.enter();
            worker(i);
            state.leave();
        });
    }
    while (ready.load(std::memory_order_acquire) != workerCount)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (std::thread& thread : workers)
        thread.join();
    return workerCount;
}

#endif // MAPPINGWORKERS_H
