#pragma once
#include "PairCaptureNrSdk.h"
namespace virtualdesktop_openxr::nr_capture {
    struct SdkDispatchBinding {
        uint64_t dispatchModule{NR_ABSENT}, evaluateModule{NR_ABSENT}, releaseModule{NR_ABSENT};
    };
    // Original observations only, no SDK/COM callbacks or queries. The caller binds
    // each actual dispatch address to its observed loaded module, including paired
    // Evaluate/Release entries. No fixed SDK module index is assumed.
    // Only wire placement (module index, paramFirst/callIndex, observation mask)
    // changes; actual results, lifetimes, parameter bits and write stamps remain.
    // All failure paths leave the complete packet unchanged.
    bool appendSdkObservation(NrPacketV1&, const SdkObservation&, const SdkDispatchBinding&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
