#include "fg_depth_trace.hpp"

TRACELOGGING_DEFINE_PROVIDER(g_vdxrFgDepthProvider,
                             "VDXRFGDepth",
                             (0x68e561f1, 0x5a15, 0x49a1, 0x8e, 0x79, 0x95, 0xeb, 0x20, 0xb5, 0x4b, 0x1e));

namespace {
    struct ProviderLifetime {
        ProviderLifetime() noexcept {
            TraceLoggingRegister(g_vdxrFgDepthProvider);
        }
        ~ProviderLifetime() {
            TraceLoggingUnregister(g_vdxrFgDepthProvider);
        }
    } providerLifetime;
} // namespace
