#pragma once

#include <array>
#include <cstdint>

namespace virtualdesktop_openxr {
    enum class FrameGenerationRole { synthetic1, synthetic2, real, synthetic3 };

    struct PredictionReservation {
        int64_t currentTarget{};
        int64_t firstTarget{};
        int64_t period{};
        uint64_t nativeId{};
        // Snapshot at xrWaitFrame, retained through pipelined Begin/End calls.
        uint64_t historyEpoch{};
        uint32_t multiplier{};
        bool valid{};
        bool shouldRender{true};
    };

    struct FrameGenerationSlotPlan {
        std::array<FrameGenerationRole, 4> roles{};
        std::array<int64_t, 4> targets{};
        std::array<float, 3> fractions{};
        uint32_t count{1};
        // Optional synthesis sample times; native presentation/deadline targets stay above.
        std::array<int64_t, 3> synthesisTimes{};
        int64_t synthesisTime(unsigned slot) const noexcept {
            if (slot >= targets.size())
                return 0;
            return slot + 1 < count && slot < synthesisTimes.size() && synthesisTimes[slot] > 0 ? synthesisTimes[slot]
                                                                                                : targets[slot];
        }
    };

    // Apply only after the cadence planner admits a complete group.
    bool applyUniformStretchedFourSamples(FrameGenerationSlotPlan& plan,
                                          int64_t previousTime,
                                          int64_t currentTime) noexcept;

    class FrameGenerationCadence {
      public:
        PredictionReservation reserve(int64_t now,
                                      int64_t nextNativeTarget,
                                      int64_t displayPeriod,
                                      uint32_t multiplier,
                                      uint64_t nextNativeId) const noexcept;
        FrameGenerationSlotPlan plan(const PredictionReservation& reservation,
                                     int64_t previousTime,
                                     bool pairReady) noexcept;
        bool onTime(const FrameGenerationSlotPlan& plan, int64_t now) const noexcept;
        void reset() noexcept;

      private:
        int64_t m_period{};
    };
} // namespace virtualdesktop_openxr
