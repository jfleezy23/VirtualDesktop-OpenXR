// SPDX-License-Identifier: MIT
#pragma once

#include <OVR_CAPI.h>
#include <openxr/openxr.h>
#include <algorithm>
#include <cstdint>

namespace virtualdesktop_openxr {
    // In right-handed view space, distance d maps to stored depth -A + B/d.
    // Reciprocals preserve either infinite endpoint without inf/inf arithmetic.
    inline ovrTimewarpProjectionDesc projectionFromDepthInfo(const XrCompositionLayerDepthInfoKHR& depth) {
        const double inverseNear = 1.0 / static_cast<double>(depth.nearZ);
        const double inverseFar = 1.0 / static_cast<double>(depth.farZ);
        const double projection23 = (static_cast<double>(depth.minDepth) - depth.maxDepth) / (inverseNear - inverseFar);
        return {
            static_cast<float>(projection23 * inverseNear - depth.minDepth), static_cast<float>(projection23), -1.f};
    }

    // Oculus has one depth encoding for both eyes. Cover the union of their
    // physical clipping ranges when OpenXR supplies different encodings.
    inline ovrTimewarpProjectionDesc commonDepthProjection(const XrCompositionLayerDepthInfoKHR& left,
                                                           const XrCompositionLayerDepthInfoKHR& right) {
        XrCompositionLayerDepthInfoKHR depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
        // Reversed-Z retains distant finite detail beside an infinite plane;
        // forward-Z could round that detail to the exact infinity endpoint.
        depth.nearZ = std::max({left.nearZ, left.farZ, right.nearZ, right.farZ});
        depth.farZ = std::min({left.nearZ, left.farZ, right.nearZ, right.farZ});
        depth.maxDepth = 1.f;
        return projectionFromDepthInfo(depth);
    }

    struct DepthEncodingTransform {
        float sourceMin{}, sourceMax{1.f}, targetNear{}, targetFar{1.f};
        uint32_t enabled{};
    };

    inline bool sameDepthEncoding(const XrCompositionLayerDepthInfoKHR& left,
                                  const XrCompositionLayerDepthInfoKHR& right) noexcept {
        return left.minDepth == right.minDepth && left.maxDepth == right.maxDepth && left.nearZ == right.nearZ &&
               left.farZ == right.farZ;
    }

    inline DepthEncodingTransform depthEncodingTransform(const XrCompositionLayerDepthInfoKHR& source,
                                                         const ovrTimewarpProjectionDesc& destination) {
        // Do not recover source distances from its rounded float matrix. A
        // finite forward far plane can otherwise become indistinguishable from
        // infinity. Map the original endpoints in double, then interpolate in
        // destination space. This also avoids overflowing float reciprocals.
        const auto mapped = [&](float distance) {
            return static_cast<float>(static_cast<double>(destination.Projection23) / distance -
                                      destination.Projection22);
        };
        return {source.minDepth, source.maxDepth, mapped(source.nearZ), mapped(source.farZ), 1};
    }
} // namespace virtualdesktop_openxr
