#pragma once
#include "PairCaptureNrPrivateApi.h"
#pragma push_macro("max")
#undef max
#include "PairCaptureNrOwner.h"
#pragma pop_macro("max")
#include "PairCaptureNrSdk.h"
#include <d3d11_4.h>

namespace virtualdesktop_openxr::nr_capture {
    using ReceiverCall = uint32_t(NR_PRIVATE_CALL*)(const NrPacketV1*, NrAckV1*) noexcept;
    using ReceiverReady = uint32_t(NR_PRIVATE_CALL*)(NrReadyV1*) noexcept;

    struct HostRequest {
        bool valid{};
        uint64_t attempt{}, nonce[2]{}, pid{}, creation{}, mode{}, minimumSerial{}, minimumTime{};
        wchar_t armFile[4096]{}, output[4096]{};
        uint8_t hostHash[32]{}, fgHash[32]{}, modelHash[32]{}, sourceHash[32]{}, authorHash[32]{};
        wchar_t sourceBridgePath[4096]{};
    };
    const HostRequest& hostRequest() noexcept;
    struct HostLifetimeFacts {
        uint64_t instance{}, instanceLifetime{}, sessionHandle{}, sessionLifetime{};
    };
    bool hostLifetimeFacts(HostLifetimeFacts&) noexcept;
    void hostInstanceCreated(const void*) noexcept;
    void hostSessionCreated(const void*, uint64_t handle) noexcept;
    void hostSessionDestroyed(const void*) noexcept;
    // All pointer parameters below are known original owners/observations, never worker callbacks.
    bool hostBindReceiver(const NrReadyV1&,
                          ReceiverCall,
                          HMODULE actualModule,
                          ID3D12Device* actualDevice,
                          ID3D12CommandQueue* actualQueue,
                          void* actualContext) noexcept;
    void hostInvalidateWarm() noexcept;
    void hostWarmLayoutComplete(uint64_t serial, bool complete) noexcept;
    void hostWarmFrame(uint64_t serial,
                       uint64_t contextLifetime,
                       const SdkWarmFacts&,
                       uint64_t originalSignalValue,
                       int32_t originalSignalResult,
                       int32_t outputWaitResult,
                       bool graphInventoryComplete) noexcept;
    void hostModuleAttested(uint64_t serial, uint64_t history, bool complete) noexcept;
    void hostOriginalAdmission(
        uint64_t serial, uint64_t ticket, uint64_t previous, uint64_t history, int32_t result) noexcept;
    void hostOriginalRetirement(uint64_t serial, uint64_t ticket, uint64_t history, int32_t result) noexcept;
    struct OwnedGroupBoundary {
        vdxrfg::pair_capture::nr::NrGroupId id{};
        vdxrfg::pair_capture::nr::NrSubmissionKey actual{};
        uint64_t boundary{};
        ID3D11Fence* fence11{};
        ID3D12Fence* fence12{};
    };
    vdxrfg::pair_capture::nr::NrWorkOwner& hostWorkOwner() noexcept;
    bool hostRegisterGroup(uint32_t frame, uint32_t group, const OwnedGroupBoundary&) noexcept;
    bool hostFinishFrameTransfer(uint32_t frame) noexcept;
    bool hostReceiverBinding(NrReadyV1&, ReceiverCall&, HMODULE&) noexcept;
    bool hostLatestSource(NrSourceWitnessV1&) noexcept;
    bool hostBeginSelectedRaw(uint32_t frame, uint64_t serial, const NrReadyV1&, uint64_t sourceWitness) noexcept;
    bool hostPinnedModules(NrModuleV1 (&modules)[2], NrCallV1 (&calls)[2]) noexcept;
    void hostCaptureTerminal(uint64_t reason, bool uncertain) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
