#pragma once

#include "vdxr_fg_api.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace vdxrfg {
    // Private GPU derivation of the two owned canonical guide histories. The
    // caller must prove full previous synthesis completion before each record.
    class DepthAvailability final {
      public:
        [[nodiscard]] HRESULT initialize(ID3D12Device* device,
                                         ID3D12Resource* const histories[2],
                                         uint32_t atlasWidth,
                                         uint32_t atlasHeight) noexcept;
        // Rectangles are validated global-atlas published viewports, one per eye.
        // Records on the existing direct guide list; leaves histories in COMMON
        // and flags in combined pixel/nonpixel shader-read state. No CPU wait.
        [[nodiscard]] HRESULT record(ID3D12GraphicsCommandList* list,
                                     uint32_t currentHistory,
                                     const VDXRFG_RectV1 rects[2]) noexcept;
        [[nodiscard]] ID3D12Resource* flagsResource() const noexcept {
            return flags_.Get();
        }

      private:
        Microsoft::WRL::ComPtr<ID3D12Device> device_;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> histories_;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch_, flags_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> tilesPipeline_, reducePipeline_;
        uint32_t width_{}, height_{}, tilesX_{}, tilesY_{}, increment_{};
        D3D12_RESOURCE_STATES flagsState_{D3D12_RESOURCE_STATE_COMMON};
    };
} // namespace vdxrfg
