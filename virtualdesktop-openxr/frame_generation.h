#pragma once

#include "../nr-frame-generation/include/vdxr_fg_api.h"
#include "../nr-frame-generation/include/pair_capture_private.hpp"
#include "frame_generation_pacing.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>

namespace virtualdesktop_openxr {
    // Original submitted image, never a compositor-aligned/NR depth image.
    // All metadata is copied by value; source owns the submission-device view
    // of the application's released allocation, including its array slice.
    struct FrameGenerationDepthSource {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
        D3D11_TEXTURE2D_DESC description{};
        VDXRFG_RectV1 colourRect{}, depthRect{}, colourCrop{};
        VDXRFG_ViewV1 captureView{};
        uint32_t releasedIndex{}, arraySlice{};
        float minDepth{}, maxDepth{}, nearZ{}, farZ{};
        bool affineVerified{};
    };

    inline DXGI_FORMAT frameGenerationDepthSrvFormat(DXGI_FORMAT format) noexcept {
        switch (format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    inline bool frameGenerationDepthMetadataValid(const FrameGenerationDepthSource& source) noexcept {
        const auto validRect = [](const VDXRFG_RectV1& rect) {
            // The normalization kernel's exact uint point map relies on
            // these D3D11 limits to keep every intermediate below 2^30.
            return rect.width && rect.height && uint64_t(rect.x) + rect.width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
                   uint64_t(rect.y) + rect.height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        };
        const auto& desc = source.description;
        const auto& crop = source.colourCrop;
        const auto& colour = source.colourRect;
        const auto& depth = source.depthRect;
        return source.affineVerified && desc.Width && desc.Height &&
               desc.Width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
               desc.Height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION && desc.MipLevels && desc.ArraySize &&
               desc.SampleDesc.Count == 1 && source.arraySlice < desc.ArraySize &&
               frameGenerationDepthSrvFormat(desc.Format) != DXGI_FORMAT_UNKNOWN && validRect(colour) &&
               validRect(depth) && validRect(crop) && uint64_t(depth.x) + depth.width <= desc.Width &&
               uint64_t(depth.y) + depth.height <= desc.Height && crop.x >= colour.x && crop.y >= colour.y &&
               uint64_t(crop.x) + crop.width <= uint64_t(colour.x) + colour.width &&
               uint64_t(crop.y) + crop.height <= uint64_t(colour.y) + colour.height && std::isfinite(source.minDepth) &&
               std::isfinite(source.maxDepth) && source.minDepth >= 0 && source.minDepth < source.maxDepth &&
               source.maxDepth <= 1 && source.nearZ > 0 && source.farZ > 0 && source.nearZ != source.farZ &&
               std::isfinite(1.f / source.nearZ) && std::isfinite(1.f / source.farZ);
    }

    // The same allocation-free admission is used before requesting guide
    // memory and again before preparing views/copies on the submission device.
    inline bool frameGenerationDepthSourceValid(ID3D11Device* device,
                                                const FrameGenerationDepthSource& source) noexcept {
        if (!device || !source.source || !frameGenerationDepthMetadataValid(source))
            return false;
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        Microsoft::WRL::ComPtr<IUnknown> sourceIdentity, deviceIdentity;
        source.source->GetDevice(&owner);
        if (owner.Get() != device &&
            (FAILED(owner.As(&sourceIdentity)) || FAILED(device->QueryInterface(IID_PPV_ARGS(&deviceIdentity))) ||
             sourceIdentity.Get() != deviceIdentity.Get()))
            return false;
        D3D11_TEXTURE2D_DESC actual{};
        source.source->GetDesc(&actual);
        return actual.Width == source.description.Width && actual.Height == source.description.Height &&
               actual.MipLevels == source.description.MipLevels && actual.ArraySize == source.description.ArraySize &&
               actual.Format == source.description.Format && actual.SampleDesc.Count == 1 &&
               actual.BindFlags == source.description.BindFlags;
    }

    struct FinalProjectionInput {
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> colour;
        std::array<VDXRFG_ViewV1, 2> views{};
        std::array<FrameGenerationDepthSource, 2> depth;
        uint64_t serial{};
        bool nrEnabled{}; // Private observer qualification; never changes NR configuration.
        vdxrfg::pair_capture::Entry entry{};
        int64_t time{}, period{};
        FrameGenerationSlotPlan slots;
    };

    struct OwnedFrameTransaction {
        VDXRFG_TicketV1 ticket{};
        FrameGenerationSlotPlan slots;
        uint64_t consumerValue{};
        uint64_t ownerGeneration{};
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4> destinations;
    };

    // The caller serializes methods and owns the submission-context lease.
    // Captured source pixels stay unchanged until the real copy is queued;
    // runtime preprocessing admission enforces this before producer overlap.
    // Transactions must be retired before replacement/shutdown can succeed.
    class FrameGeneration {
      public:
        struct InitInfo {
            ID3D11Device5* device11{};
            ID3D11DeviceContext4* context11{};
            ID3D12Device* device12{};
            std::filesystem::path modulePath;
            uint32_t eyeWidth{}, height{}, multiplier{};
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
            bool enableGpuTiming{false};
            bool useFidelityFxFull{false};
            bool enableDepthGuidance{false};
            // Initial finalized-projection opacity. Changes require a history
            // reset so both endpoints of every pair use one interpretation.
            bool opaqueInput{false};
            // Session startup snapshot: 0=color-only, 1=current depth assist,
            // 2=optional NVIDIA camera reconstruction. Keep at the end so
            // existing aggregate initialization retains its defaults.
            uint32_t depthMode{1};
            // Prototype: a distinct same-adapter immediate delivery domain.
            // Metadata methods remain caller-serialized. Prepare real B before
            // the next capture; its canonical atlas is reused only after the
            // delivery consumer fence. Null preserves the established path.
            ID3D11Device5* deliveryDevice11{};
            ID3D11DeviceContext4* deliveryContext11{};
        };

        // Optional CPU-only receipt of the actual module Submit call. This is
        // not evidence of GPU completion or a visually effective depth guide.
        struct CaptureDiagnostics {
            uint32_t apiVersion{}, guideMask{}, guideEncoding{};
            uint64_t guideSerial{}, guideEpoch{}, previousSerial{}, currentSerial{}, ticketToken{};
            HRESULT submitResult{E_PENDING};
            bool submitCalled{};
        };

        FrameGeneration();
        ~FrameGeneration();
        FrameGeneration(const FrameGeneration&) = delete;
        FrameGeneration& operator=(const FrameGeneration&) = delete;

        HRESULT initialize(const InitInfo& info) noexcept;
        HRESULT capture(const FinalProjectionInput& input, OwnedFrameTransaction& transaction) noexcept;
        HRESULT capture(const FinalProjectionInput& input,
                        OwnedFrameTransaction& transaction,
                        CaptureDiagnostics* diagnostics) noexcept;
        HRESULT copyOutput(OwnedFrameTransaction& transaction,
                           FrameGenerationRole role,
                           ID3D11Texture2D* destination) noexcept;
        HRESULT retire(OwnedFrameTransaction& transaction) noexcept;
        // BUSY borrows a State-owned GPU completion event. Wait outside the
        // context lease and retry retirement; a wake does not release ownership.
        HRESULT prepareRetirementWait(const OwnedFrameTransaction& transaction, HANDLE& completionEvent) noexcept;
        HRESULT reset(uint32_t reason) noexcept;
        HRESULT shutdown() noexcept;
        HRESULT getStatistics(VDXRFG_StatisticsV1& statistics) const noexcept;
        bool ready(const OwnedFrameTransaction& transaction) const noexcept;
        // Call under the context lease. BUSY borrows a State-owned event;
        // wait outside the lease, then recheck here. A wake alone proves nothing.
        // registerPending=false observes readiness without registering an event.
        HRESULT prepareOutputReadyWait(const OwnedFrameTransaction& transaction,
                                       HANDLE& completionEvent,
                                       FrameGenerationRole role = FrameGenerationRole::real,
                                       bool registerPending = true) noexcept;
        // An active ticket's fully staged current-bank copies may be observed
        // concurrently with opposite-bank capture. Its owner must not copy or
        // retire that bank concurrently; all other methods remain serialized.
        bool copyReady(const OwnedFrameTransaction& transaction, FrameGenerationRole role) const noexcept;
        HRESULT waitForCopy(const OwnedFrameTransaction& transaction,
                            FrameGenerationRole role,
                            DWORD timeoutMs) noexcept;
        struct PrivateNrCanonicalBorrow {
            void* context{};
            ID3D11Device5* device{};
            ID3D12Device* device12{};
            ID3D11DeviceContext4* producer{};
            ID3D11Texture2D* texture{};
            uint64_t resourceLifetime{};
        };
        // Original State/context owner lease must span this scalar borrow and
        // actual owner acquisition. No COM call, AddRef, initialization or worker use.
        bool borrowPrivateNrCanonical(PrivateNrCanonicalBorrow&) const noexcept;
        bool initialized() const noexcept;
        // True only for a successfully created V5 canonical consumer. Also
        // reports retained poisoned work so source protection stays enabled.
        bool depthGuidanceEnabled() const noexcept;
        bool depthGuidanceRequested() const noexcept;
        uint32_t negotiatedDepthMode() const noexcept;
        bool opaqueInputEnabled() const noexcept;
        void diagnostic(const char*, uint64_t, void*, void*, uint64_t, HRESULT = S_OK) const noexcept;
        bool diagnosticEntryEnabled() const noexcept;
        // S_OK changes mode and clears history; S_FALSE is unchanged/unsupported.
        // BUSY and failures preserve the current mode and host capture state.
        HRESULT updateOpaqueInput(bool opaqueInput) noexcept;

      private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace virtualdesktop_openxr
