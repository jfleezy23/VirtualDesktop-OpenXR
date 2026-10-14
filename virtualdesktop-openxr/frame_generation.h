#pragma once

#include "../nr-frame-generation/include/vdxr_fg_api.h"
#include "frame_generation_pacing.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <memory>

namespace virtualdesktop_openxr {
    struct FinalProjectionInput {
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> colour;
        std::array<VDXRFG_ViewV1, 2> views{};
        uint64_t serial{};
        int64_t time{}, period{};
        FrameGenerationSlotPlan slots;
    };

    struct OwnedFrameTransaction {
        VDXRFG_TicketV1 ticket{};
        FrameGenerationSlotPlan slots;
        uint64_t consumerValue{};
        uint64_t ownerGeneration{};
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> destinations;
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
        };

        FrameGeneration();
        ~FrameGeneration();
        FrameGeneration(const FrameGeneration&) = delete;
        FrameGeneration& operator=(const FrameGeneration&) = delete;

        HRESULT initialize(const InitInfo& info) noexcept;
        HRESULT capture(const FinalProjectionInput& input, OwnedFrameTransaction& transaction) noexcept;
        HRESULT copyOutput(OwnedFrameTransaction& transaction,
                           FrameGenerationRole role,
                           ID3D11Texture2D* destination) noexcept;
        HRESULT retire(OwnedFrameTransaction& transaction) noexcept;
        HRESULT reset(uint32_t reason) noexcept;
        HRESULT shutdown() noexcept;
        HRESULT getStatistics(VDXRFG_StatisticsV1& statistics) const noexcept;
        bool ready(const OwnedFrameTransaction& transaction) const noexcept;
        bool copyReady(const OwnedFrameTransaction& transaction, FrameGenerationRole role) const noexcept;
        HRESULT waitForCopy(const OwnedFrameTransaction& transaction,
                            FrameGenerationRole role,
                            DWORD timeoutMs) noexcept;
        bool initialized() const noexcept;

      private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace virtualdesktop_openxr
