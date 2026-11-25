#include "../virtualdesktop-openxr/NrBeforeSrLayers.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

int main() {
    XrCompositionLayerProjection projection{};
    projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    projection.viewCount = 2;
    XrCompositionLayerQuad overlay{};
    overlay.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    XrCompositionLayerBaseHeader other{};
    other.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
    const auto* scene = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    const auto* quad = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay);
    const auto check =
        [](std::initializer_list<const XrCompositionLayerBaseHeader*> layers, bool expect, const char* label) {
            XrFrameEndInfo frame{};
            frame.layerCount = static_cast<uint32_t>(layers.size());
            frame.layers = layers.begin();
            if (virtualdesktop_openxr::nr_before_sr::HasStereoProjectionWithOptionalQuads(frame) != expect) {
                std::fprintf(stderr, "FAIL: %s\n", label);
                std::exit(1);
            }
        };
    check({scene}, true, "single stereo projection");
    check({scene, quad}, true, "Hubris stereo projection with quad overlay");
    check({scene, quad, quad}, true, "multiple trailing quads");
    check({quad, scene}, false, "quad underlay");
    check({scene, scene}, false, "multiple projections");
    check({scene, quad, scene}, false, "additional projection after overlay");
    check({scene, &other}, false, "unexpected layer type");
    check({}, false, "empty layer list");
    check({quad}, false, "missing projection");
    check({nullptr}, false, "null projection");
    check({scene, nullptr}, false, "null overlay");
    projection.viewCount = 1;
    check({scene, quad}, false, "mono projection");
    std::puts("PASS: before-SR stereo projection layers");
}
