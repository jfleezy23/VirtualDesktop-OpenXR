#pragma once

#include "NrBeforeSrNgx.h"
#include "NrBeforeSrPipeline.h"
#include "NrBeforeSrStereo.h"
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <optional>
#include <wrl/client.h>

namespace virtualdesktop_openxr::nr_before_sr {
    using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                ID3D12CommandAllocator*,
                                                ID3D12PipelineState*);
    using AllocatorResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);

    struct EvaluationReceipt {
        std::optional<std::uint64_t> epoch;
        StereoViewKey view{};
        unsigned handleId{};
        bool qualifiedColorCaller{};
        NgxColorSubstitution color;
    };

    // The sink outlives every hook invocation, including during process shutdown. It owns
    // per-session admission and draining. All three COM callbacks must forward exactly once, even
    // for foreign objects and when the session is disabled or draining. featureReleased
    // receives the old pointer as an identity only; its storage has already been released.
    class HookSink {
      public:
        virtual void featureCreated(ID3D12GraphicsCommandList*,
                                    NVSDK_NGX_Feature,
                                    NVSDK_NGX_Parameter*,
                                    const NVSDK_NGX_Handle*) noexcept = 0;
        virtual void beforeEvaluate(ID3D12GraphicsCommandList*,
                                    const NVSDK_NGX_Handle*,
                                    const NVSDK_NGX_Parameter*,
                                    EvaluationReceipt&) noexcept = 0;
        virtual void afterEvaluate(const NVSDK_NGX_Handle*, const EvaluationReceipt&, bool succeeded) noexcept = 0;
        virtual void featureReleased(const NVSDK_NGX_Handle*, unsigned handleId) noexcept = 0;
        // Close admission and synchronize outstanding vendor calls before returning.
        // A null device covers all devices. A failed native shutdown still closes admission.
        virtual void beforeNgxShutdown(ID3D12Device* device) noexcept = 0;
        virtual void execute(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*, ExecuteFn) noexcept = 0;
        virtual HRESULT
        reset(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*, ResetFn) noexcept = 0;
        virtual HRESULT resetAllocator(ID3D12CommandAllocator*, AllocatorResetFn) noexcept = 0;

      protected:
        ~HookSink() = default;
    };

    // Call outside DllMain at session startup, before permitting any SR recording.
    // The caller verifies the Hubris executable build and opt-in. A successful install
    // is process-local and permanent. Reuse requires the same sink and method targets.
    bool InstallHooks(ID3D12CommandQueue* queue,
                      ID3D12GraphicsCommandList* probeList,
                      ID3D12CommandAllocator* probeAllocator,
                      HookSink& sink,
                      bool trackPipelineState = false) noexcept;
    void TrackPipelineReset(ID3D12GraphicsCommandList* list,
                            std::uint64_t generation,
                            ID3D12PipelineState* state) noexcept;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CapturePipelineState(ID3D12GraphicsCommandList* list,
                                                                     std::uint64_t generation) noexcept;
    bool ObservesListReset(ID3D12GraphicsCommandList* list) noexcept;
    bool ObservesAllocatorReset(ID3D12CommandAllocator* allocator) noexcept;
    unsigned HubrisHandleId(const NVSDK_NGX_Handle* handle) noexcept;
} // namespace virtualdesktop_openxr::nr_before_sr
