#pragma once

#include <array>
#include <cstdint>

namespace vdxrfg::depth_probe {
    // R32 stereo atlas, two endpoints, at most 32 unique scanlines each.
    // Separate near-uniform halves include the exact middle and both edges.
    struct Plan {
        uint32_t width{}, height{}, rowCount{}, rowPitch{}, rowStride{};
        uint64_t bufferBytes{};
        std::array<uint32_t, 32> rows{};
        constexpr uint64_t offset(uint32_t endpoint, uint32_t row) const noexcept {
            return (uint64_t(endpoint) * rowCount + row) * rowStride;
        }
    };

    constexpr Plan makePlan(uint64_t width, uint32_t height) noexcept {
        if (!width || width > 16384 || (width & 1) || !height || height > 16384)
            return {};
        Plan plan{};
        plan.width = static_cast<uint32_t>(width);
        plan.height = height;
        plan.rowCount = height < 32 ? height : 32;
        plan.rowPitch = (plan.width * 4 + 255) & ~255u;
        plan.rowStride = (plan.rowPitch + 511) & ~511u;
        plan.bufferBytes = uint64_t(plan.rowStride) * plan.rowCount * 2;
        const uint32_t middle = height / 2, middleRow = plan.rowCount / 2;
        for (uint32_t i = 0; i < plan.rowCount; ++i) {
            plan.rows[i] = i <= middleRow
                               ? (middleRow ? i * middle / middleRow : 0)
                               : middle + (i - middleRow) * (height - 1 - middle) / (plan.rowCount - 1 - middleRow);
        }
        return plan;
    }
} // namespace vdxrfg::depth_probe
