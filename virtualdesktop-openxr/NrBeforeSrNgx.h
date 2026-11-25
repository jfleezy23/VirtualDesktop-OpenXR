#pragma once

#include "NrBeforeSrPolicy.h"

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

#include <cmath>
#include <optional>

namespace virtualdesktop_openxr::nr_before_sr {
    // Only native SR parameters are read here. Model parameters are owned by the recorder.
    struct NgxSrFeature {
        Extent maxRender, output;
        unsigned int createFlags{};
        bool outputSubrects{};
    };

    struct NgxTextureRegion {
        ID3D12Resource* texture{};
        Rect active{};
    };

    struct NgxSrEvaluation {
        NgxTextureRegion color, depth, motion, output;
        float motionScaleX{}, motionScaleY{}, preExposure{1.f};
        bool reset{};
    };

    namespace detail {
        template <typename T>
        bool ReadNgx(const NVSDK_NGX_Parameter& parameters, const char* name, T& value) {
            T read{};
            if (NVSDK_NGX_FAILED(parameters.Get(name, &read)))
                return false;
            value = read;
            return true;
        }

        inline bool ReadNgxTexture(const NVSDK_NGX_Parameter& parameters,
                                   const char* textureName,
                                   const char* originX,
                                   const char* originY,
                                   Extent extent,
                                   NgxTextureRegion& region) {
            if (!ReadNgx(parameters, textureName, region.texture) || !region.texture)
                return false;
            region.active = {0, 0, extent.width, extent.height};
            ReadNgx(parameters, originX, region.active.x);
            ReadNgx(parameters, originY, region.active.y);
            return true;
        }
    } // namespace detail

    // The caller must own exclusive mutable access to the native table for this entire scope.
    // EvaluateFeature itself borrows a const table; its argument alone cannot grant this access.
    class ScopedNgxColor {
      public:
        explicit ScopedNgxColor(bool& restored) noexcept : m_restored(restored) {
            m_restored = true;
        }
        ScopedNgxColor(const ScopedNgxColor&) = delete;
        ScopedNgxColor& operator=(const ScopedNgxColor&) = delete;
        ~ScopedNgxColor() {
            restore();
        }

        bool bind(NVSDK_NGX_Parameter& table, const NgxTextureRegion& original, ID3D12Resource* resolved) noexcept try {
            if (m_table || !original.texture || !resolved || original.texture == resolved || !matches(table, original))
                return false;
            m_original = original;
            m_table = &table;
            // Missing or wrong-type origin keys were declined above. NGX has no per-key erase.
            if (original.active.x)
                table.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
            if (original.active.y)
                table.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
            table.Set(NVSDK_NGX_Parameter_Color, resolved);
            if (matches(table, {resolved, {}}))
                return true;
            restore();
            return false;
        } catch (...) {
            restore();
            return false;
        }

        bool restore() noexcept {
            if (!m_table)
                return m_restored;
            auto& table = *m_table;
            m_table = nullptr;
            // An SDK setter returns void. Attempt every original value even if one setter fails.
            const auto restoreKey = [&](const char* key, auto value) noexcept {
                try {
                    table.Set(key, value);
                } catch (...) {
                }
            };
            restoreKey(NVSDK_NGX_Parameter_Color, m_original.texture);
            if (m_original.active.y)
                restoreKey(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, m_original.active.y);
            if (m_original.active.x)
                restoreKey(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, m_original.active.x);
            try {
                m_restored = matches(table, m_original);
            } catch (...) {
                m_restored = false;
            }
            return m_restored;
        }

        static bool matches(const NVSDK_NGX_Parameter& table, const NgxTextureRegion& expected) {
            ID3D12Resource* color{};
            unsigned int x{}, y{};
            return detail::ReadNgx(table, NVSDK_NGX_Parameter_Color, color) && color == expected.texture &&
                   detail::ReadNgx(table, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, x) &&
                   x == expected.active.x &&
                   detail::ReadNgx(table, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, y) &&
                   y == expected.active.y;
        }

      private:
        NVSDK_NGX_Parameter* m_table{};
        NgxTextureRegion m_original{};
        bool& m_restored;
    };

    struct NgxColorSubstitution {
        NVSDK_NGX_Parameter* exclusiveTable{};
        NgxTextureRegion original{};
        ID3D12Resource* resolved{};
        bool substituted{}, restored{true};
    };

    template <typename NativeEvaluate>
    NVSDK_NGX_Result EvaluateWithResolvedColor(const NVSDK_NGX_Parameter* table,
                                               NgxColorSubstitution& color,
                                               NativeEvaluate nativeEvaluate) {
        if (!color.resolved || !table || color.exclusiveTable != table)
            return nativeEvaluate();
        ScopedNgxColor scope(color.restored);
        color.substituted = scope.bind(*color.exclusiveTable, color.original, color.resolved);
        if (!color.restored)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        return nativeEvaluate();
    }

    inline std::optional<NgxSrFeature> ReadNgxSrFeature(const NVSDK_NGX_Parameter& parameters) {
        using detail::ReadNgx;
        NgxSrFeature feature;
        int flags{};
        if (!ReadNgx(parameters, NVSDK_NGX_Parameter_Width, feature.maxRender.width) ||
            !ReadNgx(parameters, NVSDK_NGX_Parameter_Height, feature.maxRender.height) ||
            !ReadNgx(parameters, NVSDK_NGX_Parameter_OutWidth, feature.output.width) ||
            !ReadNgx(parameters, NVSDK_NGX_Parameter_OutHeight, feature.output.height) ||
            !ReadNgx(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags) || !feature.maxRender.width ||
            !feature.maxRender.height || !feature.output.width || !feature.output.height || flags < 0)
            return {};
        feature.createFlags = static_cast<unsigned int>(flags);
        int outputSubrects{};
        if (ReadNgx(parameters, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, outputSubrects)) {
            feature.outputSubrects = outputSubrects > 0;
        } else {
            unsigned int legacySubrects{};
            ReadNgx(parameters, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, legacySubrects);
            feature.outputSubrects = legacySubrects != 0;
        }
        return feature;
    }

    inline std::optional<NgxSrEvaluation> ReadNgxSrEvaluation(const NVSDK_NGX_Parameter& parameters,
                                                              const NgxSrFeature& feature) {
        using detail::ReadNgx;
        using detail::ReadNgxTexture;

        auto render = feature.maxRender;
        const bool hasWidth =
            ReadNgx(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render.width);
        const bool hasHeight =
            ReadNgx(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render.height);
        if (hasWidth != hasHeight || !render.width || !render.height)
            return {};

        const auto motionExtent = feature.createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes ? render : feature.output;
        NgxSrEvaluation frame;
        if (!ReadNgxTexture(parameters,
                            NVSDK_NGX_Parameter_Color,
                            NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                            render,
                            frame.color) ||
            !ReadNgxTexture(parameters,
                            NVSDK_NGX_Parameter_Depth,
                            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                            render,
                            frame.depth) ||
            !ReadNgxTexture(parameters,
                            NVSDK_NGX_Parameter_MotionVectors,
                            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
                            motionExtent,
                            frame.motion) ||
            !ReadNgxTexture(parameters,
                            NVSDK_NGX_Parameter_Output,
                            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
                            feature.output,
                            frame.output) ||
            !ReadNgx(parameters, NVSDK_NGX_Parameter_MV_Scale_X, frame.motionScaleX) ||
            !ReadNgx(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, frame.motionScaleY))
            return {};

        int reset{};
        if (ReadNgx(parameters, NVSDK_NGX_Parameter_Reset, reset))
            frame.reset = reset != 0;
        ReadNgx(parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, frame.preExposure);
        if (!std::isfinite(frame.preExposure) || frame.preExposure <= 0.f)
            return {};
        return frame;
    }
} // namespace virtualdesktop_openxr::nr_before_sr
