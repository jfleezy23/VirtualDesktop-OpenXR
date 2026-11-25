#pragma once

#include "NrBeforeSrPolicy.h"

#include <d3d12.h>
#include <optional>
#include <wrl/client.h>

namespace virtualdesktop_openxr::nr_before_sr {
    struct ColorViewPlan {
        DXGI_FORMAT resolvedFormat{};
        // UNKNOWN means the native R32_FLOAT depth already has the layout expected by the model.
        DXGI_FORMAT depthPlaneSrv{};
    };
    inline std::optional<ColorViewPlan> PlanColorViews(const D3D12_RESOURCE_DESC& color,
                                                       Rect colorRect,
                                                       const D3D12_RESOURCE_DESC& depth,
                                                       Rect depthRect) noexcept {
        const auto fits = [](const D3D12_RESOURCE_DESC& desc, Rect r) {
            return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
                   desc.MipLevels == 1 && desc.SampleDesc.Count == 1 && !desc.SampleDesc.Quality &&
                   desc.Width <= detail::MaxTextureDimension && desc.Height <= detail::MaxTextureDimension && r.width &&
                   r.height && r.x <= desc.Width && r.width <= desc.Width - r.x && r.y <= desc.Height &&
                   r.height <= desc.Height - r.y;
        };
        if (!fits(color, colorRect) || !fits(depth, depthRect) || colorRect.width != depthRect.width ||
            colorRect.height != depthRect.height ||
            (color.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && color.Format != DXGI_FORMAT_R11G11B10_FLOAT) ||
            (color.Flags & ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                             D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) ||
            (depth.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            return {};
        if (depth.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
            return ColorViewPlan{color.Format, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
        if (depth.Format == DXGI_FORMAT_R32_FLOAT)
            return ColorViewPlan{color.Format, DXGI_FORMAT_UNKNOWN};
        return {};
    }

    // Immutable descriptors. The host retains each use and its pipeline through possible list replay and its
    // final fence. Reuse also requires proof that the allocator has retired the previous recording.
    class ColorScratch {
      public:
        ColorScratch() = default;
        ColorScratch(const ColorScratch&) = delete;
        ColorScratch& operator=(const ColorScratch&) = delete;

        bool initialize(
            ID3D12Device* device, ID3D12Resource* source, Rect active, ID3D12Resource* depth, Rect depthRect) noexcept;
        bool reuseRetired(
            ID3D12Device* device, ID3D12Resource* source, Rect active, ID3D12Resource* depth, Rect depthRect) noexcept;
        ID3D12Resource* modelInput() const noexcept {
            return m_input.Get();
        }
        ID3D12Resource* modelOutput() const noexcept {
            return m_answer.Get();
        }
        ID3D12Resource* extractedDepth() const noexcept {
            return m_extractedDepth.Get();
        }
        ID3D12Resource* resolvedColor() const noexcept {
            return m_phase == Phase::Finished ? m_resolved.Get() : nullptr;
        }

      private:
        friend class ColorPipeline;
        enum class Phase { Fresh, Encoded, Finished, Abandoned };
        Microsoft::WRL::ComPtr<IUnknown> m_device;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_source, m_depthSource, m_input, m_answer, m_resolved, m_extractedDepth;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_views;
        Rect m_active{};
        Rect m_depthRect{};
        DXGI_FORMAT m_resolvedFormat{};
        UINT m_stride{};
        Phase m_phase{Phase::Fresh};
        ID3D12GraphicsCommandList* m_recordingList{}; // Borrowed only between consecutive encode and resolve/recover.
        float m_preExposure{};
    };

    // Recording requires compute-readable game color and depth plane states at entry.
    // Only private textures receive barriers. Model inputs enter NGX in COMMON as in NrSpatialPipeline.
    class ColorPipeline {
      public:
        ColorPipeline() = default;
        ColorPipeline(const ColorPipeline&) = delete;
        ColorPipeline& operator=(const ColorPipeline&) = delete;

        bool initialize(ID3D12Device* device) noexcept;
        bool encode(ID3D12GraphicsCommandList* list, ColorScratch& scratch, float preExposure) const noexcept;
        bool resolve(ID3D12GraphicsCommandList* list, ColorScratch& scratch, float preExposure) const noexcept;
        // After failed model recording, seal this private batch. Game color has not been written or transitioned.
        bool recover(ID3D12GraphicsCommandList* list, ColorScratch& scratch) const noexcept;

      private:
        void dispatch(ID3D12GraphicsCommandList* list,
                      const ColorScratch& scratch,
                      ID3D12PipelineState* program,
                      UINT firstDescriptor,
                      float preExposure) const noexcept;
        Microsoft::WRL::ComPtr<IUnknown> m_device;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_encode, m_resolve;
    };
} // namespace virtualdesktop_openxr::nr_before_sr
