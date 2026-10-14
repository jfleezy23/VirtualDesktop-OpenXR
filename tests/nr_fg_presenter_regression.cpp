#include "frame_generation_pacing.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cmath>

using namespace virtualdesktop_openxr;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    FrameGenerationCadence cadence;
    constexpr int64_t period = 10000000, firstTarget = 1000000000;
    const auto prime = cadence.reserve(firstTarget - period, firstTarget, period, 3, 20);
    require(prime.valid && prime.currentTarget == 1020000000 && prime.nativeId == 20,
            "3X prediction reserves B two native periods beyond S1");
    const auto primePlan = cadence.plan(prime, 0, false);
    require(primePlan.count == 1 && primePlan.roles[0] == FrameGenerationRole::real, "prime has only real B");
    const auto next = cadence.reserve(1020000000, 1030000000, period, 3, 23);
    const auto triplet = cadence.plan(next, 1020000000, true);
    require(triplet.count == 3 && triplet.roles[0] == FrameGenerationRole::synthetic1 &&
                triplet.roles[1] == FrameGenerationRole::synthetic2 && triplet.roles[2] == FrameGenerationRole::real,
            "3X order is S1/S2/B");
    require(triplet.targets[0] == 1030000000 && triplet.targets[1] == 1040000000 && triplet.targets[2] == 1050000000,
            "3X targets are distinct native slots");
    require(triplet.fractions[0] > 0.33332f && triplet.fractions[0] < 0.33334f && triplet.fractions[1] > 0.66665f &&
                triplet.fractions[1] < 0.66668f,
            "3X fractions derive from actual endpoints");
    const auto two = cadence.reserve(1020000000, 1030000000, period, 2, 23);
    const auto pair = cadence.plan(two, 1020000000, true);
    require(pair.count == 2 && pair.roles[1] == FrameGenerationRole::real && pair.fractions[0] == 0.5f,
            "2X midpoint and real order");
    require(cadence.plan(next, next.currentTarget, true).count == 1, "duplicate timestamp bypasses");
    require(cadence.plan(next, -1, true).count == 1, "invalid timestamp bypasses");
    require(cadence.plan(next, 500000000, true).count == 1, "timestamp gap bypasses rather than extrapolates");
    require(!cadence.reserve(0, 1, 0, 3, 0).valid, "zero period rejected");
    require(!cadence.reserve(0, std::numeric_limits<int64_t>::max(), period, 3, 0).valid,
            "prediction addition overflow rejected");
    require(!cadence.reserve(0, firstTarget, period, 4, 0).valid, "unsupported multiplier rejected");
    require(!cadence.onTime(triplet, 1030000001), "late work cannot move synthetic group unchanged");
    require(cadence.onTime(triplet, 1029000000), "ready work has a valid first slot");
    const auto changed = cadence.reserve(1020000000, 1030000000, period / 2, 3, 23);
    require(cadence.plan(changed, 1020000000, true).count == 1, "refresh change re-primes");
    for (const double refresh : {90., 120., 144.}) {
        FrameGenerationCadence rounded;
        const auto nativePeriod = static_cast<int64_t>(1e9 / refresh);
        const auto timestamp = [refresh](int id) { return static_cast<int64_t>((10. + id / refresh) * 1e9); };
        const auto a = rounded.reserve(timestamp(0) - nativePeriod, timestamp(0), nativePeriod, 3, 0);
        rounded.plan(a, 0, false);
        const auto b = rounded.reserve(timestamp(3) - nativePeriod, timestamp(3), nativePeriod, 3, 3);
        const auto group = rounded.plan(b, a.currentTarget, true);
        require(group.count == 3, "90/120/144Hz double-clock quantization still forms a stable 3X pair");
        const auto interval = b.currentTarget - a.currentTarget;
        require(group.fractions[0] == static_cast<float>(group.targets[0] - a.currentTarget) / interval,
                "fractions use reserved endpoints, including nanosecond rounding");
    }
    std::puts("PASS: FG pure reservation, 2X/3X roles, fractions, late/overflow/reset guards");
}
