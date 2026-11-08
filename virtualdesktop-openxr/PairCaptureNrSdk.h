#pragma once
#include "PairCaptureNrContracts.h"
#include "NrOwnershipViews.h"
#include <nvsdk_ngx_params.h>
#include <type_traits>
#include <utility>

namespace virtualdesktop_openxr::nr_capture {
    struct SdkObservation {
        NrCallV1 call{};
        std::array<NrParamV1, 39> parameters{};
        bool parametersObserved{}, ended{}, threw{};
    };
    struct SdkWarmFacts {
        uint64_t tableAddress{}, tableLifetime{};
        uint64_t featureAddress[2]{}, featureLifetime[2]{};
        uint64_t evaluateAddress[2]{}, releaseAddress[2]{};
        uint64_t evaluateSequence[2]{}, frameSerial{}, totalSequence{};
        bool tableLive{}, featureLive[2]{}, completeFrame{}, invalid{};
    };
    // Scalar-only journaling; the caller supplies actual objects from its existing lease.
    void sdkTableCreated(NVSDK_NGX_Parameter*) noexcept;
    void sdkTableDestroyed(NVSDK_NGX_Parameter*) noexcept;
    void sdkRecording(uint64_t serial,
                      const void* context,
                      const vdxrfg::pair_capture::nr::NrRecordingOwnership&) noexcept;
    // Same original caller lease only; no worker Runtime/SDK dereference.
    bool sdkCurrentRecording(ID3D12GraphicsCommandList* actualList,
                             uint64_t& serial,
                             vdxrfg::pair_capture::nr::NrRecordingOwnership&) noexcept;
    void sdkResourceCreated(ID3D12Resource*) noexcept;
    void sdkResourceWrite(ID3D12Resource*, ID3D12GraphicsCommandList*, uint64_t kind) noexcept;
    uint64_t sdkObservedResourceLifetime(ID3D12Resource*) noexcept;
    void sdkResourceLifetime(ID3D12Resource*, uint64_t lifetime) noexcept;
    void sdkSetBits(NVSDK_NGX_Parameter*, const char*, uint64_t type, uint64_t bits) noexcept;
    uint64_t sdkCallBegin(uint64_t kind,
                          uint32_t eye,
                          uint64_t dispatch,
                          ID3D12GraphicsCommandList*,
                          NVSDK_NGX_Parameter*,
                          uint64_t feature,
                          uint64_t evaluate,
                          uint64_t release,
                          ID3D12Device* device = nullptr) noexcept;
    void sdkCallEnd(uint64_t token, uint32_t result, uint64_t feature) noexcept;
    void sdkCallThrew(uint64_t token) noexcept;
    bool sdkWarmFacts(SdkWarmFacts&) noexcept;
    bool sdkCopyObservations(SdkObservation* rows, size_t capacity, size_t& count) noexcept;
    bool sdkCopyLifecycle(SdkObservation* rows, size_t capacity, size_t& count) noexcept;

    struct SetBits {
        uint64_t type, bits;
    };
    inline SetBits setBits(int v) noexcept {
        return {NR_I32, static_cast<uint32_t>(v)};
    }
    inline SetBits setBits(unsigned v) noexcept {
        return {NR_U32, v};
    }
    inline SetBits setBits(float v) noexcept {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        return {NR_F32, bits};
    }
    inline SetBits setBits(ID3D12Resource* v) noexcept {
        return {NR_RESOURCE_ID, reinterpret_cast<uint64_t>(v)};
    }
    // A typed once-only tee: argument evaluation and original overload are preserved.
    // No SDK Get, extra Set, COM, allocation or observer lock is introduced.
    template <class T>
    inline void ObserveSet(NVSDK_NGX_Parameter* table, const char* key, T value) {
        table->Set(key, value);
        const auto raw = setBits(value);
        sdkSetBits(table, key, raw.type, raw.bits);
    }
    class SdkCallObservation {
      public:
        SdkCallObservation(uint64_t kind,
                           uint32_t eye,
                           uint64_t dispatch,
                           ID3D12GraphicsCommandList* list,
                           NVSDK_NGX_Parameter* table,
                           uint64_t feature,
                           uint64_t evaluate,
                           uint64_t release,
                           ID3D12Device* device = nullptr) noexcept
            : token_(sdkCallBegin(kind, eye, dispatch, list, table, feature, evaluate, release, device)) {
        }
        ~SdkCallObservation() {
            if (!ended_)
                sdkCallThrew(token_);
        }
        template <class Result>
        Result finish(Result result, uint64_t feature) noexcept {
            ended_ = true;
            sdkCallEnd(token_, static_cast<uint32_t>(result), feature);
            return result;
        }

      private:
        uint64_t token_{};
        bool ended_{};
    };
    template <class Call>
    auto ObserveSdkInit(uint64_t dispatch, ID3D12Device* device, Call&& original) -> decltype(original()) {
        SdkCallObservation observation(NR_INIT_MANUAL, 2, dispatch, nullptr, nullptr, 0, 0, 0, device);
        return observation.finish(original(), 0);
    }
    template <class Call, class Feature>
    auto ObserveSdkCall(uint64_t kind,
                        uint32_t eye,
                        uint64_t dispatch,
                        ID3D12GraphicsCommandList* list,
                        NVSDK_NGX_Parameter* table,
                        Feature* feature,
                        uint64_t evaluate,
                        uint64_t release,
                        Call&& original) -> decltype(original()) {
        SdkCallObservation observation(
            kind, eye, dispatch, list, table, feature ? reinterpret_cast<uint64_t>(*feature) : 0, evaluate, release);
        const auto result = original(); // original exactly once, including actual exception propagation
        return observation.finish(result, feature ? reinterpret_cast<uint64_t>(*feature) : 0);
    }
} // namespace virtualdesktop_openxr::nr_capture
