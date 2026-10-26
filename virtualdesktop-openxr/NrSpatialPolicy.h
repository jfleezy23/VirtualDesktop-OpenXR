#pragma once

#include <cstdint>
#include <dxgiformat.h>

namespace virtualdesktop_openxr::nr_spatial {
    struct Extent {
        uint32_t width{}, height{};
    };
    struct Rect {
        uint32_t x{}, y{}, width{}, height{};
    };

    inline bool IsSupportedColorFormat(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }
    inline bool IsSrgb(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }
    inline DXGI_FORMAT RawColorFormat(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB   ? DXGI_FORMAT_R8G8B8A8_UNORM
               : format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                           : format;
    }
    inline bool TryWorkingExtent(Extent native, uint32_t percent, Extent& work) noexcept {
        work = {};
        if (percent < 50 || percent >= 100 || !native.width || !native.height)
            return false;
        const auto dimension = [percent](uint32_t size) {
            const uint64_t aligned = (((uint64_t(size) * percent + 99) / 100 + 3) / 4) * 4;
            return static_cast<uint32_t>(aligned < size ? aligned : size);
        };
        const Extent candidate{dimension(native.width), dimension(native.height)};
        // Conservative qualification threshold, not a proprietary model limit.
        if (candidate.width < 64 || candidate.height < 64 ||
            (candidate.width == native.width && candidate.height == native.height))
            return false;
        work = candidate;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_spatial
