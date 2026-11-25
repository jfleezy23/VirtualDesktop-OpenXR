#pragma once

#include <openxr/openxr.h>

namespace virtualdesktop_openxr::nr_before_sr {
    // Called after xrEndFrame has validated and processed all app layers. OpenXR composes them
    // in submission order, so only quads following the stereo projection are overlays.
    inline bool HasStereoProjectionWithOptionalQuads(const XrFrameEndInfo& frame) noexcept {
        if (!frame.layerCount || !frame.layers || !frame.layers[0] ||
            frame.layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            return false;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(frame.layers[0]);
        if (projection->viewCount != 2)
            return false;
        for (uint32_t i = 1; i < frame.layerCount; ++i) {
            if (!frame.layers[i] || frame.layers[i]->type != XR_TYPE_COMPOSITION_LAYER_QUAD)
                return false;
        }
        return true;
    }
} // namespace virtualdesktop_openxr::nr_before_sr
