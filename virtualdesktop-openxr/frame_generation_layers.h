#pragma once

#include "frame_generation_pacing.h"
#include <OVR_CAPI.h>
#include <openxr/openxr.h>
#include <vector>

namespace virtualdesktop_openxr {
    // processLayers has already validated the submitted views, spaces and swapchains.
    inline bool frameGenerationLayerLayoutSupported(const XrFrameEndInfo& info,
                                                    const std::vector<ovrLayer_Union>& layers,
                                                    bool allowQuad) noexcept {
        if ((info.layerCount != 1 && !(allowQuad && info.layerCount == 2)) || layers.size() != info.layerCount ||
            !info.layers || !info.layers[0] || info.layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION ||
            (layers[0].Header.Type != ovrLayerType_EyeFov && layers[0].Header.Type != ovrLayerType_EyeFovDepth))
            return false;

        const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[0]);
        if (projection.viewCount != 2 || !projection.views)
            return false;
        if (info.layerCount == 1)
            return true;
        if (!info.layers[1] || info.layers[1]->type != XR_TYPE_COMPOSITION_LAYER_QUAD ||
            layers[1].Header.Type != ovrLayerType_Quad || !layers[1].Quad.ColorTexture)
            return false;

        const auto& hud = reinterpret_cast<const XrCompositionLayerQuad*>(info.layers[1])->subImage;
        if (!hud.swapchain)
            return false;
        const auto aliasesHud = [&](const XrSwapchainSubImage& source) {
            return hud.swapchain == source.swapchain && hud.imageArrayIndex == source.imageArrayIndex;
        };
        for (unsigned eye = 0; eye < 2; ++eye) {
            // Quad alpha processing can write an already resolved slice before FG packs it.
            const auto& view = projection.views[eye];
            if (aliasesHud(view.subImage) || layers[1].Quad.ColorTexture == layers[0].EyeFov.ColorTexture[eye] ||
                (layers[0].Header.Type == ovrLayerType_EyeFovDepth &&
                 layers[1].Quad.ColorTexture == layers[0].EyeFovDepth.DepthTexture[eye]))
                return false;
            for (auto entry = reinterpret_cast<const XrBaseInStructure*>(view.next); entry; entry = entry->next) {
                if (entry->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR &&
                    aliasesHud(reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(entry)->subImage))
                    return false;
            }
        }
        return true;
    }

    // The source is B's camera/metadata. Synthetic rasters use the same camera.
    inline ovrLayer_Union frameGenerationPresentationLayer(const ovrLayer_Union& source,
                                                           FrameGenerationRole role,
                                                           ovrTextureSwapChain colour,
                                                           ovrTextureSwapChain realDepth,
                                                           ovrSizei eyeSize) {
        auto layer = source;
        const bool hasDepth = role == FrameGenerationRole::real && source.Header.Type == ovrLayerType_EyeFovDepth;
        layer.Header.Type = hasDepth ? ovrLayerType_EyeFovDepth : ovrLayerType_EyeFov;
        // Synthetic depth is not implemented. Clear even the inactive union
        // fields so B's depth can never escape as a synthetic pixel pairing.
        for (auto& texture : layer.EyeFovDepth.DepthTexture)
            texture = hasDepth ? realDepth : nullptr;
        if (!hasDepth)
            layer.EyeFovDepth.ProjectionDesc = {};
        for (unsigned eye = 0; eye < 2; ++eye) {
            layer.EyeFov.ColorTexture[eye] = colour;
            layer.EyeFov.Viewport[eye].Pos = {static_cast<int>(eye) * eyeSize.w, 0};
            layer.EyeFov.Viewport[eye].Size = eyeSize;
        }
        return layer;
    }
} // namespace virtualdesktop_openxr
