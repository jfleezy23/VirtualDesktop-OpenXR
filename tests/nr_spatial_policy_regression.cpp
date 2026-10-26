#include <cstdio>
#include <stdexcept>
#if __has_include("../virtualdesktop-openxr/NrSpatialPolicy.h")
#include "../virtualdesktop-openxr/NrSpatialPolicy.h"
using namespace virtualdesktop_openxr::nr_spatial;
void require(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
int main() {
    try {
        Extent work{};
        require(!TryWorkingExtent({2017, 1999}, 100, work), "native100 must bypass");
        require(TryWorkingExtent({2017, 1999}, 90, work) && work.width == 1816 && work.height == 1800,
                "90 percent must ceil, align4, and preserve unequal dimensions");
        require(TryWorkingExtent({2017, 1999}, 80, work) && work.width == 1616 && work.height == 1600,
                "80 percent first target must ceil and align unequal dimensions");
        require(TryWorkingExtent({129, 127}, 50, work) && work.width == 68 && work.height == 64,
                "minimum qualified 64 grid");
        require(!TryWorkingExtent({120, 127}, 50, work), "work widths under64 must fall back");
        require(!TryWorkingExtent({65, 65}, 99, work), "rounded grid without reduction must fall back");
        require(!TryWorkingExtent({0, 100}, 90, work), "zero extent must fall back");
        for (auto percent : {0u, 49u, 101u, 0xffffffffu})
            require(!TryWorkingExtent({2000, 2000}, percent, work), "invalid percentage must fall back");
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
            require(IsSupportedColorFormat(format), "qualified candidate SDR format");
        for (auto format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_TYPELESS})
            require(!IsSupportedColorFormat(format), "HDR/unknown/untyped view must fall back");
        std::puts("PASS spatial policy");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
#else
int main() {
    std::fprintf(stderr, "FAIL: spatial working-resolution policy is unavailable\n");
    return 1;
}
#endif
