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
        struct Combined {
            float coverage;
            uint32_t percent;
            Rect crop;
            Extent work;
            uint32_t transition;
        };
        // Native 2017x1999, independently calculated crop then ceil/align4 work.
        for (const auto test : {Combined{.95f, 80, {50, 49, 1916, 1900}, {1536, 1520}, 20},
                                Combined{.90f, 80, {100, 99, 1816, 1800}, {1456, 1440}, 40},
                                Combined{.90f, 85, {100, 99, 1816, 1800}, {1544, 1532}, 40},
                                Combined{1.f, 80, {0, 0, 2017, 1999}, {1616, 1600}, 0}}) {
            const auto crop = CoverageRect({2017, 1999}, test.coverage);
            require(crop.x == test.crop.x && crop.y == test.crop.y && crop.width == test.crop.width &&
                        crop.height == test.crop.height,
                    "coverage must center each native eye and retain the aligned legacy crop");
            require(TryWorkingExtent({crop.width, crop.height}, test.percent, work) && work.width == test.work.width &&
                        work.height == test.work.height,
                    "combined work must derive from crop rather than full eye");
            require(TransitionWidth({2017, 1999}, crop, test.coverage) == test.transition,
                    "closest-edge width follows coverage and native shorter dimension");
        }
        require(CoverageRect({2017, 1999}, 0.f).width == 204 && CoverageRect({2017, 1999}, 2.f).width == 2017,
                "existing coverage clamp10-100 retained");
        require(TransitionWidth({80, 80}, {36, 36, 8, 8}, .1f) == 2,
                "feather capped at quarter of smaller crop dimension");
        require(TransitionWidth({20, 20}, {2, 2, 16, 16}, .95f) == 1, "partial crop minimum one native pixel");
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
        const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM,
                                    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                    DXGI_FORMAT_B8G8R8A8_UNORM,
                                    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                    DXGI_FORMAT_UNKNOWN,
                                    DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                    DXGI_FORMAT_B8G8R8A8_TYPELESS,
                                    DXGI_FORMAT_R16G16B16A16_FLOAT};
        for (unsigned source = 0; source < std::size(formats); ++source) {
            require(OutputColorFormat(formats[source], false) == formats[source], "FG-off retains source format");
            const auto canonical = source == 2 ? formats[0] : source == 3 ? formats[1] : formats[source];
            require(OutputColorFormat(formats[source], true) == canonical, "FG canonical RGBA preserves encoding");
            for (unsigned output = 0; output < std::size(formats); ++output) {
                const bool allowed = source < 4 && output < 4 &&
                                     (source == output || (source == 2 && output == 0) || (source == 3 && output == 1));
                require(IsCompatibleColorOutput(formats[source], formats[output]) == allowed,
                        "only identical qualified pairs or encoding-matched BGRA to RGBA");
            }
        }
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
