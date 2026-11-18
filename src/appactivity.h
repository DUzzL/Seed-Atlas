#ifndef APPACTIVITY_H
#define APPACTIVITY_H

#include <QtGlobal>

// Marks a span of computation the user asked for (a search, an analysis, an
// export). macOS puts an app whose windows are hidden or covered into App
// Nap: its timers are throttled and its threads drop to the lowest scheduling
// tier, so a long job all but stopped as soon as the app was no longer in
// view. While at least one scope is alive, the process reports itself as busy
// and is left alone. The system may still go to sleep when idle. Scopes can
// be created on any thread and overlap freely; no effect on other platforms.
class BusyActivity
{
public:
#if defined(Q_OS_MACOS)
    BusyActivity();
    ~BusyActivity();
#else
    BusyActivity() {}
    ~BusyActivity() {}
#endif
    BusyActivity(const BusyActivity&) = delete;
    BusyActivity& operator=(const BusyActivity&) = delete;
};

#endif // APPACTIVITY_H
