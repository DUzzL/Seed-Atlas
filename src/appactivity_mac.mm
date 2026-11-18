#include "appactivity.h"

#import <Foundation/Foundation.h>

#include <cstddef>
#include <mutex>

namespace
{

std::mutex activityMutex;
size_t activityUsers = 0;
id<NSObject> activityToken = nil;

} // namespace

BusyActivity::BusyActivity()
{
    std::lock_guard<std::mutex> lock(activityMutex);
    if (activityUsers++ == 0)
    {
        @autoreleasepool
        {
            // The user-initiated part keeps App Nap away. On its own the
            // threads of a hidden app are still held at the utility tier
            // (measured on macOS 27: a fraction of the speed on a busy
            // machine); only the latency-critical flag leaves them at their
            // normal priority.
            activityToken = [[NSProcessInfo processInfo]
                beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                    | NSActivityLatencyCritical
                reason:@"Search or analysis in progress"];
        }
    }
}

BusyActivity::~BusyActivity()
{
    std::lock_guard<std::mutex> lock(activityMutex);
    if (--activityUsers == 0 && activityToken)
    {
        @autoreleasepool
        {
            [[NSProcessInfo processInfo] endActivity:activityToken];
            activityToken = nil;
        }
    }
}
