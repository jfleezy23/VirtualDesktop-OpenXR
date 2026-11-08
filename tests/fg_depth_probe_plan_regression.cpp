// Compile-only CPU contract: no Windows device or GPU execution.
#include "fg_depth_probe_plan.hpp"

constexpr bool validPlan(uint32_t width, uint32_t height) {
    const auto plan = vdxrfg::depth_probe::makePlan(width, height);
    if (!plan.rowCount || plan.rowCount != (height < 32 ? height : 32) || plan.rowPitch % 256 ||
        plan.rowPitch < uint64_t(width) * 4 || plan.rowStride % 512 || plan.rowStride < plan.rowPitch ||
        plan.rows[0] != 0 || plan.rows[plan.rowCount - 1] != height - 1 || plan.rows[plan.rowCount / 2] != height / 2)
        return false;
    for (uint32_t i = 0; i < plan.rowCount; ++i) {
        if (plan.rows[i] >= height || (i && plan.rows[i] <= plan.rows[i - 1]))
            return false;
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
            const auto offset = plan.offset(endpoint, i);
            if (offset % 512 || offset + plan.rowPitch > plan.bufferBytes)
                return false;
        }
    }
    return plan.offset(1, plan.rowCount - 1) + plan.rowStride == plan.bufferBytes;
}
constexpr bool smallHeights() {
    for (uint32_t height = 1; height < 65; ++height)
        if (!validPlan(2, height) || !validPlan(134, height))
            return false;
    return true;
}
static_assert(smallHeights());
static_assert(validPlan(3840, 2160));
static_assert(validPlan(16384, 16384));
static_assert(vdxrfg::depth_probe::makePlan(16384, 16384).bufferBytes <= 4 * 1024 * 1024);
static_assert(!vdxrfg::depth_probe::makePlan(0, 32).rowCount);
static_assert(!vdxrfg::depth_probe::makePlan(3, 32).rowCount);
static_assert(!vdxrfg::depth_probe::makePlan(2, 0).rowCount);
static_assert(!vdxrfg::depth_probe::makePlan(16386, 32).rowCount);
static_assert(!vdxrfg::depth_probe::makePlan(2, 16385).rowCount);
static_assert(!vdxrfg::depth_probe::makePlan(UINT64_MAX, UINT32_MAX).rowCount);
