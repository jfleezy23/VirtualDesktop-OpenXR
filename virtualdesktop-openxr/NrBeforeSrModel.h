#pragma once

#include "NrBeforeSr.h"
#include "NrBeforeSrNgx.h"

namespace virtualdesktop_openxr::nr_before_sr {
    struct BeforeSrModelApi {
        decltype(&::NVSDK_NGX_D3D12_CreateFeature) create{};
        decltype(&::NVSDK_NGX_D3D12_EvaluateFeature) evaluate{};
        decltype(&::NVSDK_NGX_D3D12_ReleaseFeature) release{};
        decltype(&::NVSDK_NGX_D3D12_GetCapabilityParameters) getParameters{};
        decltype(&::NVSDK_NGX_D3D12_DestroyParameters) destroyParameters{};
    };

    // The caller has qualified the native game's image/SDK and exact D3D12 device.
    // On success, direct model dispatch remains bound to this device for the rest of the process.
    bool OpenBeforeSrModel(ID3D12Device* device, BeforeSrModelApi& api) noexcept;

    // These are private, SDK-allocated capability maps. Preserve their prepopulated
    // callbacks, and release with the same SDK that allocated them after GPU retirement.
    // The controller can keep one of these in its recording owner or feature history.
    class ModelParameters {
      public:
        ModelParameters() = default;
        ModelParameters(const ModelParameters&) = delete;
        ModelParameters& operator=(const ModelParameters&) = delete;
        ~ModelParameters() {
            if (m_table)
                m_destroy(m_table);
        }

        bool acquire(const BeforeSrModelApi& api) {
            if (m_table || !api.getParameters || !api.destroyParameters)
                return false;
            m_destroy = api.destroyParameters;
            return NVSDK_NGX_SUCCEED(api.getParameters(&m_table)) && m_table;
        }
        NVSDK_NGX_Parameter* get() const noexcept {
            return m_table;
        }

      private:
        NVSDK_NGX_Parameter* m_table{};
        decltype(BeforeSrModelApi::destroyParameters) m_destroy{};
    };

    // On the qualified Hubris model the native output-sized RG16 guide matches the equivalent
    // render-grid control. Preserve its independent subrect and the caller's original signed pixel scales.
    inline bool ModelMotionExtentEligible(const NgxSrEvaluation& frame, DXGI_FORMAT format) noexcept {
        const auto render = frame.color.active;
        const auto motion = frame.motion.active;
        const auto validRect = [](Rect r) {
            return r.width && r.height && r.width <= detail::MaxTextureDimension &&
                   r.height <= detail::MaxTextureDimension && r.x <= detail::MaxTextureDimension - r.width &&
                   r.y <= detail::MaxTextureDimension - r.height;
        };
        if (!validRect(render) || !validRect(motion) || !std::isfinite(frame.motionScaleX) ||
            !std::isfinite(frame.motionScaleY) || frame.motionScaleX == 0.f || frame.motionScaleY == 0.f ||
            (format != DXGI_FORMAT_R16G16_FLOAT && format != DXGI_FORMAT_R32G32_FLOAT))
            return false;
        if (motion.width == render.width && motion.height == render.height)
            return true;
        return format == DXGI_FORMAT_R16G16_FLOAT && !motion.x && !motion.y && validRect(frame.output.active) &&
               motion.width >= render.width && motion.height >= render.height &&
               motion.width == frame.output.active.width && motion.height == frame.output.active.height;
    }

    inline void ConfigureModelCreate(NVSDK_NGX_Parameter& params, Extent render) {
        params.Set("DLSSNR.Width", static_cast<int>(render.width));
        params.Set("DLSSNR.Height", static_cast<int>(render.height));
        params.Set("DLSSNR.Hint.Render.Preset", 2); // Match VDXR's lightweight preset B.
        params.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        params.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    }

    // The caller supplies validated private, zero-origin encoded color and answer buffers.
    // HDR/pre-exposure belongs to the color codec; feature 18 consumes its encoded input.
    inline void ConfigureModelEvaluate(NVSDK_NGX_Parameter& params,
                                       const NgxSrEvaluation& frame,
                                       ID3D12Resource* encoded,
                                       ID3D12Resource* answer,
                                       ID3D12Resource* extractedDepth,
                                       bool depthInverted,
                                       bool reset,
                                       const ModelAppearance& appearance) {
        params.Set("DLSSNR.Enabled", 1);
        params.Set("DLSSNR.DepthInverted", static_cast<int>(depthInverted));
        params.Set("DLSSNR.Reset", static_cast<int>(reset || frame.reset));
        params.Set("DLSSNR.Color", encoded);
        params.Set("DLSSNR.ColorSubrectBaseX", 0u);
        params.Set("DLSSNR.ColorSubrectBaseY", 0u);
        params.Set("DLSSNR.ColorSubrectWidth", frame.color.active.width);
        params.Set("DLSSNR.ColorSubrectHeight", frame.color.active.height);
        params.Set("DLSSNR.Output", answer);
        params.Set("DLSSNR.OutputSubrectBaseX", 0u);
        params.Set("DLSSNR.OutputSubrectBaseY", 0u);
        params.Set("DLSSNR.OutputSubrectWidth", frame.color.active.width);
        params.Set("DLSSNR.OutputSubrectHeight", frame.color.active.height);
        params.Set("DLSSNR.Depth", extractedDepth ? extractedDepth : frame.depth.texture);
        params.Set("DLSSNR.DepthSubrectBaseX", extractedDepth ? 0u : frame.depth.active.x);
        params.Set("DLSSNR.DepthSubrectBaseY", extractedDepth ? 0u : frame.depth.active.y);
        params.Set("DLSSNR.DepthSubrectWidth", frame.depth.active.width);
        params.Set("DLSSNR.DepthSubrectHeight", frame.depth.active.height);
        params.Set("DLSSNR.MVec", frame.motion.texture);
        params.Set("DLSSNR.MVecSubrectBaseX", frame.motion.active.x);
        params.Set("DLSSNR.MVecSubrectBaseY", frame.motion.active.y);
        params.Set("DLSSNR.MVecSubrectWidth", frame.motion.active.width);
        params.Set("DLSSNR.MVecSubrectHeight", frame.motion.active.height);
        params.Set("DLSSNR.MVecScaleX", frame.motionScaleX);
        params.Set("DLSSNR.MVecScaleY", frame.motionScaleY);
        params.Set("DLSSNR.UseAutoMask", 1);
        params.Set("DLSSNR.Style", appearance.style);
        params.Set("DLSSNR.Intensity", appearance.intensity);
        params.Set("DLSSNR.LocalToneStrength", appearance.localToneStrength);
        params.Set("DLSSNR.LocalStructureStrength", appearance.localStructureStrength);
        params.Set("DLSSNR.SkinStructureStrength", appearance.skinStructureStrength);
        params.Set("DLSSNR.UICorrection", 0);
    }
} // namespace virtualdesktop_openxr::nr_before_sr
