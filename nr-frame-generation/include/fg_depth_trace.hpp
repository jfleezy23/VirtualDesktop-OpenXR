#pragma once

#include <windows.h>
#include <TraceLoggingProvider.h>

TRACELOGGING_DECLARE_PROVIDER(g_vdxrFgDepthProvider);

// No event arguments are evaluated unless an ETW consumer enables this provider.
// Binding/queue events describe CPU work. DepthPixelsCompleted separately reports
// fence-verified sampled depth rows; it does not establish visual effectiveness.
#define VDXR_FG_DEPTH_TRACE(eventName, ...)                                                                            \
    do {                                                                                                               \
        if (TraceLoggingProviderEnabled(g_vdxrFgDepthProvider, 0, 0)) {                                                \
            TraceLoggingWrite(g_vdxrFgDepthProvider, eventName, __VA_ARGS__);                                          \
        }                                                                                                              \
    } while (false)
