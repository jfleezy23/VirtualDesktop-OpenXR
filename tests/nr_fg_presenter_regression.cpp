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
    for (const auto interval : {28775268LL, 29979406LL, 32767774LL, 39723500LL, 41952600LL}) {
        FrameGenerationCadence live;
        constexpr int64_t nativePeriod = 8333333, previous = 1000000000;
        const auto first = previous + interval - 2 * nativePeriod;
        const auto reservation = live.reserve(previous, first, nativePeriod, 3, 3);
        const auto group = live.plan(reservation, previous, true);
        require(group.count == 3, "captured nonuniform game intervals retain both valid synthetic slots");
        require(group.targets[0] == first && group.targets[1] == first + nativePeriod &&
                    group.targets[2] == previous + interval,
                "nonuniform interpolation never retimes reserved native targets");
        for (unsigned slot = 0; slot < 2; ++slot) {
            require(group.fractions[slot] == static_cast<float>(group.targets[slot] - previous) / interval,
                    "nonuniform fractions use actual source endpoints");
        }
        require(!live.onTime(group, first + 1), "nonuniform inputs retain the original deadline guard");
    }
    for (const unsigned multiplier : {2u, 3u}) {
        FrameGenerationCadence jittered;
        constexpr int64_t nativePeriod = 8333333, previous = 1000000000;
        const auto first = previous + nativePeriod + 15000;
        const auto reservation = jittered.reserve(previous, first, nativePeriod, multiplier, 3);
        const auto group = jittered.plan(reservation, previous, true);
        require(group.count == multiplier, "prediction jitter does not force otherwise valid pairs to re-prime");
    }
    for (const auto firstOffset : {0LL, 1LL, 19LL}) {
        FrameGenerationCadence bounds;
        constexpr int64_t previous = 1000000000;
        const auto nativePeriod = firstOffset == 19 ? 1 : 19;
        const auto reservation = bounds.reserve(previous, previous + firstOffset, nativePeriod, 2, 3);
        const auto fallback = bounds.plan(reservation, previous, true);
        require(fallback.count == 1 && fallback.roles[0] == FrameGenerationRole::real &&
                    fallback.targets[0] == reservation.currentTarget,
                "out-of-bracket and exact module fraction boundaries preserve a pristine real-only plan");
    }
    FrameGenerationCadence partial;
    const auto upper = partial.reserve(firstTarget, firstTarget + 180, 10, 3, 3);
    const auto rejected = partial.plan(upper, firstTarget, true);
    require(rejected.count == 1 && rejected.roles[0] == FrameGenerationRole::real &&
                rejected.targets[0] == upper.currentTarget,
            "invalid second fraction cannot expose a partially initialized synthetic plan");
    std::puts("PASS: FG pure reservation, 2X/3X roles, fractions, late/overflow/reset guards");
}
