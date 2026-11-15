#include "frame_generation_pacing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace virtualdesktop_openxr;

namespace {
    unsigned checks{};

    void require(bool condition, const char* message) {
        ++checks;
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }

    FrameGenerationSlotPlan planned(int64_t previous, int64_t interval, int64_t period, unsigned multiplier = 4) {
        FrameGenerationCadence cadence;
        const auto reservation =
            cadence.reserve(previous, previous + interval - (multiplier - 1) * period, period, multiplier, 40);
        require(reservation.valid && reservation.nativeId == 40, "source pair retains its native ID reservation");
        return cadence.plan(reservation, previous, true);
    }

    void checkTargets(int64_t previous, int64_t interval, int64_t period, unsigned multiplier) {
        const auto plan = planned(previous, interval, period, multiplier);
        require(plan.count == multiplier, "a complete in-bracket pair is admitted");
        const int64_t endpoint = previous + interval;
        const int64_t first = endpoint - (multiplier - 1) * period;
        for (unsigned slot = 0; slot < multiplier; ++slot) {
            const int64_t target = first + slot * period;
            require(plan.targets[slot] == target && plan.synthesisTime(slot) == target,
                    "content and native timing use the original reserved slot");
            if (slot + 1 == multiplier) {
                require(plan.roles[slot] == FrameGenerationRole::real && target == endpoint,
                        "B stays at the real endpoint");
            } else {
                constexpr FrameGenerationRole roles[] = {
                    FrameGenerationRole::synthetic1, FrameGenerationRole::synthetic2, FrameGenerationRole::synthetic3};
                require(plan.roles[slot] == roles[slot], "synthetic role order is preserved");
                require(std::abs(plan.fractions[slot] - static_cast<float>(target - previous) / interval) < 1e-6f,
                        "motion fraction derives from the native target and the actual source pair");
            }
        }
        require(plan.synthesisTime(4) == 0, "out-of-range lookup stays bounded");
    }
} // namespace

int main() {
    constexpr int64_t previous = 1000000000, period = 8333333;
    for (unsigned hz : {72u, 90u, 96u, 120u, 144u}) {
        for (unsigned multiplier : {2u, 3u, 4u}) {
            const auto step = 1000000000LL / hz;
            checkTargets(previous, multiplier * step, step, multiplier);
            checkTargets(previous, (multiplier + 3) * step, step, multiplier);
            checkTargets(previous + (multiplier + 3) * step, multiplier * step, step, multiplier);
        }
    }
    checkTargets(previous, 64800000, period, 4);
    const auto stretched = planned(previous, 64800000, period);
    require(stretched.synthesisTime(0) == previous + 39800001, "stretched S1 retains original native timing");
    checkTargets(previous, 4 * period - 1000000, period, 4);
    checkTargets(previous, 5, 1, 4);
    FrameGenerationCadence cadence;
    const auto reservation = cadence.reserve(previous, previous + period, period, 4, 40);
    require(cadence.plan(reservation, 0, false).count == 1, "an unpaired prime is real-only");
    require(planned(previous, 50 * period, period).count == 1, "an out-of-bracket pair stays real-only");
    require(!cadence.reserve(previous, std::numeric_limits<int64_t>::max(), period, 4, 40).valid,
            "native-target overflow remains ineligible");
    std::printf("PASS: target-timed motion samples and unchanged native roles (%u checks)\n", checks);
}
