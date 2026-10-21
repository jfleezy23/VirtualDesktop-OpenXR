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
        // SDK predictions and real game frames need not be an exact multiple
        // of the native period. Preserve the reserved slots and interpolate
        // from their actual source endpoints, within the pinned core's range.
        // Validate a separate candidate so rejection preserves only real B.
        auto candidate = result;
        for (uint32_t slot = 0; slot < reservation.multiplier; ++slot) {
            candidate.targets[slot] = reservation.firstTarget + slot * reservation.period;
            candidate.roles[slot] = slot == reservation.multiplier - 1 ? FrameGenerationRole::real
                                                                       : (slot == 0 ? FrameGenerationRole::synthetic1
                                                                                    : FrameGenerationRole::synthetic2);
            if (slot < reservation.multiplier - 1) {
                const auto target = candidate.targets[slot];
                if (target <= previousTime || target >= reservation.currentTarget ||
                    (slot && target <= candidate.targets[slot - 1])) {
                    return result;
                }
                const float fraction = static_cast<float>(target - previousTime) / interval;
                if (fraction <= .05f || fraction >= .95f) {
                    return result;
                }
                candidate.fractions[slot] = fraction;
            }
        }
        candidate.count = reservation.multiplier;
        return candidate;
    }

    bool FrameGenerationCadence::onTime(const FrameGenerationSlotPlan& plan, int64_t now) const noexcept {
        return plan.count && now >= 0 && now <= plan.targets[0];
    }

    void FrameGenerationCadence::reset() noexcept {
        m_period = 0;
    }
} // namespace virtualdesktop_openxr
