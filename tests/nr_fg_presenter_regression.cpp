#include "frame_generation_pacing.h"
#include "LastRoleWaitGuard.h"

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

namespace {
    void checkTwoPositionRecovery() {
        constexpr int64_t first = 100000000, period = 8000000, last = first + period;
        FrameGenerationCadence cadence;
        for (const auto now : {first, first + 1, last - 1}) {
            const auto reservation = cadence.reserve(now, first, period, 2, 200);
            require(reservation.valid && reservation.reason == PredictionReservationReason::usable,
                    "a renderable pair stays eligible up to its original B");
            const auto plan = cadence.plan(reservation, first - period, true);
            require(reservation.nativeId == 200 && plan.count == 2 && plan.targets[0] == first &&
                        plan.targets[1] == last && plan.fractions[0] == .5f,
                    "partial lateness preserves original native positions and interpolation");
            require(cadence.onTime(plan, now) == (now == first), "late S1 retains its original deadline");
        }
        for (const auto now : {last, last + 1}) {
            const auto expired = cadence.reserve(now, first, period, 2, 200);
            require(!expired.valid && expired.reason == PredictionReservationReason::expired2 &&
                        expired.nativeId == 200 && expired.multiplier == 2 && expired.period == period &&
                        expired.firstTarget == first && expired.currentTarget == last,
                    "a fully expired renderable pair retains both original positions");
            const auto plan = cadence.plan(expired, first - period, true);
            require(plan.count == 1 && plan.roles[0] == FrameGenerationRole::real,
                    "expired positions cannot earn generated-frame credit");
        }
        const auto noRender = cadence.reserve(first + 1, first, period, 2, 200, false);
        require(noRender.reason == PredictionReservationReason::none && !noRender.multiplier,
                "a late no-render input cannot claim future recovery");
        for (const auto multiplier : {1u, 3u, 4u}) {
            const auto late = cadence.reserve(first + 1, first, period, multiplier, 200);
            require(!late.valid && !late.multiplier, "other multipliers keep late rejection");
        }
        require(!cadence.reserve(first, first, period, 2, uint64_t(INT64_MAX) - 1).multiplier,
                "unrepresentable next range cannot claim a native position");
    }

    void checkLastRoleWait() {
        using namespace fg_live;
        for (const auto multiplier : {2u, 3u, 4u}) {
            const auto role = multiplier == 2   ? FrameGenerationRole::synthetic1
                              : multiplier == 3 ? FrameGenerationRole::synthetic2
                                                : FrameGenerationRole::synthetic3;
            const VdxrLastRoleWaitFactsV1 sample{sizeof(VdxrLastRoleWaitFactsV1),
                                                 1,
                                                 multiplier,
                                                 static_cast<uint32_t>(role),
                                                 1,
                                                 2,
                                                 3,
                                                 4,
                                                 11,
                                                 10,
                                                 10,
                                                 1,
                                                 requiredGuardFlags};
            require(authorizeCpp(sample) == 1, "completed B and pending last synthetic permit independent delivery");
            for (unsigned bit = 1; bit <= requiredGuardFlags; bit <<= 1) {
                auto missing = sample;
                missing.flags &= ~bit;
                require(!authorizeCpp(missing), "every handoff certification is required");
            }
            auto rejected = sample;
            rejected.selectedRole = static_cast<uint32_t>(FrameGenerationRole::real);
            require(!authorizeCpp(rejected), "a real frame cannot take the selected synthetic wait");
            rejected = sample;
            rejected.completedValue = sample.realValue - 1;
            require(!authorizeCpp(rejected), "B must have completed");
            rejected.completedValue = sample.selectedValue;
            require(!authorizeCpp(rejected), "an already-complete synthetic does not require a wait");
            rejected.completedValue = UINT64_MAX;
            require(!authorizeCpp(rejected), "device removal cannot certify a handoff");
            rejected = sample;
            rejected.ticketToken = 0;
            require(!authorizeCpp(rejected), "a missing owner ticket cannot qualify");
        }
    }
} // namespace

int main() {
    checkTwoPositionRecovery();
    checkLastRoleWait();
    FrameGenerationCadence four;
    const auto fourReservation = four.reserve(990000000, 1000000000, 10000000, 4, 0);
    require(fourReservation.valid && fourReservation.currentTarget == 1030000000,
            "4X reserves three synthetic native slots before B");
    const auto fourPlan = four.plan(fourReservation, 990000000, true);
    require(fourPlan.count == 4 && fourPlan.roles[0] == FrameGenerationRole::synthetic1 &&
                fourPlan.roles[1] == FrameGenerationRole::synthetic2 &&
                fourPlan.roles[2] == FrameGenerationRole::synthetic3 && fourPlan.roles[3] == FrameGenerationRole::real,
            "4X publishes distinct S1/S2/S3/B in temporal order");
    require(fourPlan.targets[2] == 1020000000 && fourPlan.targets[3] == 1030000000,
            "4X S3 and B keep distinct native targets");
    require(fourPlan.fractions[0] == .25f && fourPlan.fractions[1] == .5f && fourPlan.fractions[2] == .75f,
            "4X fractions are quarter/half/three-quarter");
    require(!four.onTime(fourPlan, 1000000001), "4X late completion cannot retime generated pixels");
    for (const unsigned multiplier : {2u, 3u, 4u}) {
        FrameGenerationCadence sustained;
        int64_t previous = 1000000000;
        for (unsigned iteration = 0; iteration < 2000; ++iteration) {
            const bool gap = iteration % 37 == 0;
            const int64_t first = previous + (gap ? 1000000000 : 10000000);
            const auto reservation = sustained.reserve(previous, first, 10000000, multiplier, iteration * multiplier);
            const auto group = sustained.plan(reservation, previous, true);
            require(group.count == (gap ? 1u : multiplier),
                    "sustained cadence rejects long gaps and immediately recovers");
            require(group.targets[group.count - 1] == reservation.currentTarget, "fallback retains original B target");
            require(!sustained.onTime(group, group.targets[0] + 1), "late outputs never become on-time by recovery");
            previous = reservation.currentTarget;
        }
    }
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
    require(!cadence.reserve(0, firstTarget, period, 5, 0).valid, "unsupported multiplier rejected");
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
    std::puts("PASS: FG pure reservation, 2X/3X/4X roles, sustained cadence, fractions, late/overflow/reset guards");
}
