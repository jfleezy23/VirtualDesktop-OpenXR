#include "../virtualdesktop-openxr/frame_generation_layers.h"

#include <cstdio>
#include <cstdlib>

using virtualdesktop_openxr::frameGenerationLayerLayoutSupported;

namespace {
    void require(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }
} // namespace

int main() {
    char xrChains[3]{}, nativeChains[4]{};
    XrCompositionLayerProjectionView views[2]{};
    for (unsigned eye = 0; eye < 2; ++eye)
        views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&xrChains[eye]);
    XrCompositionLayerProjection projection{};
    projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    projection.viewCount = 2;
    projection.views = views;
    XrCompositionLayerQuad quad{};
    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad.subImage.swapchain = reinterpret_cast<XrSwapchain>(&xrChains[2]);
    const auto* xrProjection = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    const auto* xrQuad = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    const XrCompositionLayerBaseHeader* xrLayers[]{xrProjection, xrQuad, xrQuad};
    XrFrameEndInfo frame{};
    frame.layers = xrLayers;
    frame.layerCount = 1;
    std::vector<ovrLayer_Union> layers(1);
    layers[0].Header.Type = ovrLayerType_EyeFov;
    layers[0].EyeFov.ColorTexture[0] = reinterpret_cast<ovrTextureSwapChain>(&nativeChains[0]);
    layers[0].EyeFov.ColorTexture[1] = reinterpret_cast<ovrTextureSwapChain>(&nativeChains[1]);
    require(frameGenerationLayerLayoutSupported(frame, layers, false), "single stereo projection");

    ++frame.layerCount;
    layers.resize(2);
    layers[1].Header.Type = ovrLayerType_Quad;
    layers[1].Quad.ColorTexture = reinterpret_cast<ovrTextureSwapChain>(&nativeChains[2]);
    const auto hudChain = layers[1].Quad.ColorTexture;
    require(frameGenerationLayerLayoutSupported(frame, layers, true), "stereo scene with independent HUD");
    require(!frameGenerationLayerLayoutSupported(frame, layers, false), "private route must permit the HUD");

    quad.subImage = views[1].subImage;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "HUD cannot modify a scene eye source");
    quad.subImage.swapchain = reinterpret_cast<XrSwapchain>(&xrChains[2]);
    layers[1].Quad.ColorTexture = layers[0].EyeFov.ColorTexture[0];
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "HUD cannot alias native scene color");
    layers[1].Quad.ColorTexture = hudChain;

    layers[0].Header.Type = ovrLayerType_EyeFovDepth;
    layers[0].EyeFovDepth.DepthTexture[0] = reinterpret_cast<ovrTextureSwapChain>(&nativeChains[3]);
    require(frameGenerationLayerLayoutSupported(frame, layers, true), "stereo depth with an independent HUD");
    layers[1].Quad.ColorTexture = layers[0].EyeFovDepth.DepthTexture[0];
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "HUD cannot alias native scene depth");
    layers[1].Quad.ColorTexture = nullptr;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "a native HUD swapchain is required");
    layers[1].Quad.ColorTexture = hudChain;
    XrCompositionLayerDepthInfoKHR depth{};
    depth.type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
    depth.subImage = quad.subImage;
    views[0].next = &depth;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "HUD cannot alias the XR scene depth slice");
    ++depth.subImage.imageArrayIndex;
    require(frameGenerationLayerLayoutSupported(frame, layers, true), "independent slices remain eligible");
    views[0].next = nullptr;

    xrLayers[1] = nullptr;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "null HUD layer");
    xrLayers[1] = xrProjection;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "second projection is unsupported");
    xrLayers[0] = xrQuad;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "underlay ordering is unsupported");
    xrLayers[0] = xrProjection;
    xrLayers[1] = xrQuad;
    projection.viewCount = 1;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "mono scene is unsupported");
    projection.viewCount = 2;
    layers[1].Header.Type = ovrLayerType_Cylinder;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "XR and native layer types must agree");
    layers[1].Header.Type = ovrLayerType_Quad;
    ++frame.layerCount;
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "XR and native layer counts must agree");
    layers.push_back(layers[1]);
    require(!frameGenerationLayerLayoutSupported(frame, layers, true), "only one independent HUD is supported");
    std::puts("PASS: FG stereo projection and HUD layout");
}
