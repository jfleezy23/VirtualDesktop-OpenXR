#include "frame_generation_pacing.h"

#include <limits>

namespace virtualdesktop_openxr {
    PredictionReservation FrameGenerationCadence::reserve(int64_t now,
                                                          int64_t nextNativeTarget,
                                                          int64_t displayPeriod,
                                                          uint32_t multiplier,
                                                          uint64_t nextNativeId) const noexcept {
        PredictionReservation result{};
        if (now < 0 || nextNativeTarget <= 0 || displayPeriod <= 0 || displayPeriod > 1000000000 ||
            (multiplier != 1 && multiplier != 2 && multiplier != 3) || nextNativeTarget < now ||
            nextNativeTarget > std::numeric_limits<int64_t>::max() - (multiplier - 1) * displayPeriod ||
            nextNativeId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - multiplier) {
            return result;
        }
        result.currentTarget = nextNativeTarget + (multiplier - 1) * displayPeriod;
        result.firstTarget = nextNativeTarget;
        result.period = displayPeriod;
        result.nativeId = nextNativeId;
        result.multiplier = multiplier;
        result.valid = true;
        return result;
    }

    FrameGenerationSlotPlan FrameGenerationCadence::plan(const PredictionReservation& reservation,
                                                         int64_t previousTime,
                                                         bool pairReady) noexcept {
        FrameGenerationSlotPlan result{};
        result.roles[0] = FrameGenerationRole::real;
        result.targets[0] = reservation.currentTarget;
        const auto previousPeriod = m_period;
        m_period = reservation.valid ? reservation.period : 0;
        if (!reservation.valid || !pairReady || previousTime <= 0 || previousTime >= reservation.currentTarget ||
            (previousPeriod && previousPeriod != reservation.period)) {
            return result;
        }
        const int64_t interval = reservation.currentTarget - previousTime;
        // Pinned GPU core clamps fractions outside (0.05,0.95). A discontinuity
        // is a real-frame bypass; never silently accept its clamped fraction.
        // OVR seconds and the physical period are independently quantized to
        // nanoseconds. At 120Hz, 3P truncates to 24,999,999ns while the real
        // endpoint interval is 25,000,000ns. Accept only that rounding budget;
        // fractions below still come from the actual reserved endpoints.
        const auto difference = interval - reservation.multiplier * reservation.period;
        const auto roundingBudget = static_cast<int64_t>(reservation.multiplier) + 1;
        if (difference < -roundingBudget || difference > roundingBudget) {
            return result;
        }
        result.count = reservation.multiplier;
        for (uint32_t slot = 0; slot < result.count; ++slot) {
            result.targets[slot] = reservation.firstTarget + slot * reservation.period;
            result.roles[slot] = slot == result.count - 1
                                     ? FrameGenerationRole::real
                                     : (slot == 0 ? FrameGenerationRole::synthetic1 : FrameGenerationRole::synthetic2);
            if (slot < result.count - 1) {
                result.fractions[slot] = static_cast<float>(result.targets[slot] - previousTime) / interval;
            }
        }
        return result;
    }

    bool FrameGenerationCadence::onTime(const FrameGenerationSlotPlan& plan, int64_t now) const noexcept {
        return plan.count && now >= 0 && now <= plan.targets[0];
    }

    void FrameGenerationCadence::reset() noexcept {
        m_period = 0;
    }
} // namespace virtualdesktop_openxr
