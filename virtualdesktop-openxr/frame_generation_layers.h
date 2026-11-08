#pragma once

#include "frame_generation_pacing.h"
#include <OVR_CAPI.h>

namespace virtualdesktop_openxr {
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
