// CPU-only production metadata admission. Does not initialize D3D or a module.
#include "frame_generation.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace virtualdesktop_openxr;
static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int main() {
    require(!FrameGeneration::InitInfo{}.enableDepthGuidance, "depth guidance allocation is explicit opt-in");
    FrameGenerationDepthSource source;
    source.affineVerified = true;
    source.description.Width = 128;
    source.description.Height = 64;
    source.description.MipLevels = 2;
    source.description.ArraySize = 2;
    source.description.SampleDesc.Count = 1;
    source.description.Format = DXGI_FORMAT_D32_FLOAT;
    source.arraySlice = 1;
    source.colourRect = {20, 10, 80, 40};
    source.colourCrop = {30, 15, 60, 30};
    source.depthRect = {8, 4, 112, 56};
    source.minDepth = .2f;
    source.maxDepth = .8f;
    source.nearZ = .1f;
    source.farZ = 100.f;
    const auto valid = source;
    for (auto format : {DXGI_FORMAT_D16_UNORM,
                        DXGI_FORMAT_D24_UNORM_S8_UINT,
                        DXGI_FORMAT_D32_FLOAT,
                        DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                        DXGI_FORMAT_R16_TYPELESS,
                        DXGI_FORMAT_R24G8_TYPELESS,
                        DXGI_FORMAT_R32_TYPELESS,
                        DXGI_FORMAT_R32G8X24_TYPELESS}) {
        source.description.Format = format;
        require(frameGenerationDepthMetadataValid(source), "supported single-sample depth metadata");
    }
    source = valid;
    source.nearZ = 100.f;
    source.farZ = .1f;
    require(frameGenerationDepthMetadataValid(source), "reversed Z retained");
    source.nearZ = std::numeric_limits<float>::infinity();
    require(frameGenerationDepthMetadataValid(source), "infinite near retained");
    source = valid;
    source.farZ = std::numeric_limits<float>::infinity();
    require(frameGenerationDepthMetadataValid(source), "infinite far retained");
    for (unsigned mode = 0; mode < 16; ++mode) {
        source = valid;
        switch (mode) {
        case 0:
            source.affineVerified = false;
            break;
        case 1:
            source.description.SampleDesc.Count = 4;
            break;
        case 2:
            source.description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case 3:
            source.arraySlice = 2;
            break;
        case 4:
            source.depthRect.width = 121;
            break;
        case 5:
            source.colourCrop.x = 19;
            break;
        case 6:
            source.colourCrop.width = 71;
            break;
        case 7:
            source.minDepth = source.maxDepth;
            break;
        case 8:
            source.minDepth = -.1f;
            break;
        case 9:
            source.nearZ = 0;
            break;
        case 10:
            source.farZ = std::numeric_limits<float>::quiet_NaN();
            break;
        case 11:
            source.nearZ = source.farZ;
            break;
        case 12:
            source.colourRect.x = UINT32_MAX;
            break;
        case 13:
            source.colourRect.x = 16384;
            source.colourCrop.x = 16384;
            break;
        case 14:
            source.description.Width = 16385;
            break;
        case 15:
            source.description.Height = 16385;
            break;
        }
        require(!frameGenerationDepthMetadataValid(source), "unsupported metadata declines guidance");
    }
    std::puts("PASS: genuine host depth metadata, supported formats, affine bounds, reversed/infinite Z and rejection "
              "guards (CPU only)");
}
