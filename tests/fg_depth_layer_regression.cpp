// CPU-only test of the same role metadata builder used by the native worker.
#include "frame_generation_layers.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace virtualdesktop_openxr;
static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int main() {
    ovrLayer_Union source{};
    source.Header.Type = ovrLayerType_EyeFovDepth;
    source.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;
    source.EyeFovDepth.ProjectionDesc = {-.25f, -.5f, -1.f};
    source.EyeFov.SensorSampleTime = 17.5;
    for (unsigned eye = 0; eye < 2; ++eye) {
        source.EyeFov.Fov[eye] = {.7f, .8f, .9f, 1.f};
        source.EyeFov.RenderPose[eye].Orientation.w = 1.f;
        source.EyeFov.RenderPose[eye].Position.x = eye ? .032f : -.032f;
        source.EyeFovDepth.DepthTexture[eye] = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(10 + eye));
    }
    auto colour = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(20));
    auto depth = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(30));
    for (unsigned multiplier : {2u, 3u, 4u}) {
        for (unsigned repeat = 0; repeat < 2000 * multiplier; ++repeat) {
            const auto real =
                frameGenerationPresentationLayer(source, FrameGenerationRole::real, colour, depth, {8, 6});
            require(real.Header.Type == ovrLayerType_EyeFovDepth,
                    "real B, prime and deadline repeats retain EyeFovDepth");
            require(real.EyeFovDepth.DepthTexture[0] == depth && real.EyeFovDepth.DepthTexture[1] == depth,
                    "real B references its private packed depth bank");
            require(!std::memcmp(&real.EyeFovDepth.ProjectionDesc,
                                 &source.EyeFovDepth.ProjectionDesc,
                                 sizeof(real.EyeFovDepth.ProjectionDesc)),
                    "real B preserves depth encoding");
            for (unsigned eye = 0; eye < 2; ++eye) {
                require(real.EyeFov.ColorTexture[eye] == colour && real.EyeFov.Viewport[eye].Pos.x == int(eye) * 8 &&
                            real.EyeFov.Viewport[eye].Pos.y == 0 && real.EyeFov.Viewport[eye].Size.w == 8 &&
                            real.EyeFov.Viewport[eye].Size.h == 6,
                        "real color and depth share packed viewports");
                require(!std::memcmp(&real.EyeFov.RenderPose[eye], &source.EyeFov.RenderPose[eye], sizeof(ovrPosef)) &&
                            !std::memcmp(&real.EyeFov.Fov[eye], &source.EyeFov.Fov[eye], sizeof(ovrFovPort)) &&
                            real.EyeFov.SensorSampleTime == 17.5 && real.Header.Flags == source.Header.Flags,
                        "real B preserves camera, timestamp and layer flags");
            }
            for (auto role :
                 {FrameGenerationRole::synthetic1, FrameGenerationRole::synthetic2, FrameGenerationRole::synthetic3}) {
                const auto synthetic = frameGenerationPresentationLayer(source, role, colour, depth, {8, 6});
                require(synthetic.Header.Type == ovrLayerType_EyeFov && !synthetic.EyeFovDepth.DepthTexture[0] &&
                            !synthetic.EyeFovDepth.DepthTexture[1],
                        "synthetic pixels never carry real B depth");
            }
        }
    }
    source.Header.Type = ovrLayerType_EyeFov;
    require(frameGenerationPresentationLayer(source, FrameGenerationRole::real, colour, nullptr, {8, 6}).Header.Type ==
                ovrLayerType_EyeFov,
            "old color-only real path remains EyeFov");
    std::puts("PASS: role-specific depth, packed viewports, B metadata and sustained repeats (CPU only)");
}
