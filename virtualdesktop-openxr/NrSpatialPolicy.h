#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <dxgiformat.h>

namespace virtualdesktop_openxr::nr_spatial {
    struct Extent {
        uint32_t width{}, height{};
    };
    struct Rect {
        uint32_t x{}, y{}, width{}, height{};
    };

    inline Rect CoverageRect(Extent native, float coverage) noexcept {
        coverage = std::isfinite(coverage) ? std::clamp(coverage, 0.1f, 1.f) : 1.f;
        const auto dimension = [coverage](uint32_t size) {
            return std::min(size, (static_cast<uint32_t>(size * coverage) + 3u) & ~3u);
        };
        const auto width = dimension(native.width), height = dimension(native.height);
        return {(native.width - width) / 2, (native.height - height) / 2, width, height};
    }
    inline uint32_t TransitionWidth(Extent native, Rect crop, float coverage) noexcept {
        if (crop.x == 0 && crop.y == 0 && crop.width == native.width && crop.height == native.height)
            return 0;
        coverage = std::isfinite(coverage) ? std::clamp(coverage, 0.1f, 1.f) : 1.f;
        const auto width = static_cast<uint32_t>(
            std::ceil(std::min(native.width, native.height) * std::min(0.02, 0.2 * (1.0 - double(coverage)))));
        return std::min(std::max(1u, width), std::min(crop.width, crop.height) / 4);
    }
    inline Rect ProcessingRect(Rect full, Rect local) noexcept {
        return !local.x && !local.y && !local.width && !local.height ? Rect{0, 0, full.width, full.height} : local;
    }

    inline bool IsSupportedColorFormat(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }
    inline bool IsSrgb(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }
    inline DXGI_FORMAT OutputColorFormat(DXGI_FORMAT source, bool frameGeneration) noexcept {
        if (frameGeneration) {
            if (source == DXGI_FORMAT_B8G8R8A8_UNORM)
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            if (source == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        }
        return source;
    }
    inline bool IsCompatibleColorOutput(DXGI_FORMAT source, DXGI_FORMAT output) noexcept {
        return IsSupportedColorFormat(source) && IsSupportedColorFormat(output) &&
               (source == output || OutputColorFormat(source, true) == output);
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
