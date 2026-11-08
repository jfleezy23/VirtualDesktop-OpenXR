#pragma once
#include <cstdint>
#include "../nr-frame-generation/include/pair_capture_nr_control.h"
namespace virtualdesktop_openxr::nr_capture {
    constexpr uint64_t captureByteCap = 134217728;
    inline bool addBytes(uint64_t& a, uint64_t b) noexcept {
        if (b > UINT64_MAX - a)
            return false;
        a += b;
        return true;
    }
    inline bool multiplyBytes(uint64_t a, uint64_t b, uint64_t& out) noexcept {
        out = 0;
        if (a && b > UINT64_MAX / a)
            return false;
        out = a * b;
        return true;
    }
    inline bool alignBytes(uint64_t value, uint64_t alignment, uint64_t& out) noexcept {
        out = 0;
        if (!alignment || (alignment & (alignment - 1)) || value > UINT64_MAX - (alignment - 1))
            return false;
        out = (value + alignment - 1) & ~(alignment - 1);
        return true;
    }
    inline uint64_t pixelBytes(uint64_t format) noexcept {
        return nr_capture_pixel_bytes(format);
    }
    // D11 allocationCharge is a conservative diagnostic charge, not observed driver
    // residency. GetDesc/Map do not expose physical allocation size. D12 plans use
    // actual GetResourceAllocationInfo separately.
    struct SnapshotLayout {
        uint64_t rowBytes{}, rowPitchBound{}, payloadBound{}, allocationCharge{};
    };
    inline bool layout11(uint64_t width,
                         uint64_t height,
                         uint64_t array,
                         uint64_t mips,
                         uint64_t samples,
                         uint64_t format,
                         SnapshotLayout& out) noexcept {
        out = {};
        const auto bpp = pixelBytes(format);
        if (!width || width > UINT32_MAX || !height || height > UINT32_MAX || !array || array > 2 || mips != 1 ||
            samples != 1 || !bpp)
            return false;
        SnapshotLayout result{};
        uint64_t rows{};
        if (!multiplyBytes(width, bpp, result.rowBytes) || !alignBytes(result.rowBytes, 4096, result.rowPitchBound) ||
            !multiplyBytes(height, array, rows) || !multiplyBytes(result.rowPitchBound, rows, result.payloadBound) ||
            !result.payloadBound || result.payloadBound > captureByteCap ||
            !alignBytes(result.payloadBound, 65536, result.allocationCharge))
            return false;
        out = result;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
