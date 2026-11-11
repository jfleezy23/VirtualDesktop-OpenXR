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

    bool identical(const FrameGenerationSlotPlan& a, const FrameGenerationSlotPlan& b) {
        return a.count == b.count && a.roles == b.roles && a.targets == b.targets && a.fractions == b.fractions &&
               a.synthesisTimes == b.synthesisTimes;
    }

    FrameGenerationSlotPlan planned(int64_t previous, int64_t interval, int64_t period, unsigned multiplier = 4) {
        FrameGenerationCadence cadence;
        const auto reservation =
            cadence.reserve(previous, previous + interval - (multiplier - 1) * period, period, multiplier, 40);
        require(reservation.valid && reservation.nativeId == 40, "source pair retains its native ID reservation");
        return cadence.plan(reservation, previous, true);
    }

    void unchanged(FrameGenerationSlotPlan plan, int64_t previous, int64_t current, const char* reason) {
        const auto before = plan;
        require(!applyUniformStretchedFourSamples(plan, previous, current), reason);
        require(identical(plan, before), "ineligible pair preserves all native and synthesis fields");
    }

    void stretched(int64_t previous,
                   int64_t interval,
                   const std::array<int64_t, 3>& expectedSamples,
                   const std::array<int64_t, 4>& expectedNative) {
        constexpr int64_t period = 8333333;
        auto plan = planned(previous, interval, period);
        const auto before = plan;
        require(plan.count == 4, "original admission accepts the complete stretched group");
        require(applyUniformStretchedFourSamples(plan, previous, previous + interval),
                "admitted stretched 4X group redistributes its content samples");
        require(plan.targets == before.targets && plan.roles == before.roles && plan.count == before.count,
                "content correction cannot move native deadlines, roles or count");
        require(plan.roles == std::array<FrameGenerationRole, 4>{FrameGenerationRole::synthetic1,
                                                                 FrameGenerationRole::synthetic2,
                                                                 FrameGenerationRole::synthetic3,
                                                                 FrameGenerationRole::real},
                "corrected content keeps S1/S2/S3/B role order");
        for (unsigned slot = 0; slot < 4; ++slot) {
            require(plan.targets[slot] - previous == expectedNative[slot], "native targets retain reserved spacing");
            if (slot == 3) {
                require(plan.synthesisTime(slot) == previous + interval, "real B retains the actual source endpoint");
                continue;
            }
            const auto sample = plan.synthesisTime(slot);
            require(sample - previous == expectedSamples[slot], "synthetic sample matches hand-derived quarter time");
            require(sample > previous && sample < previous + interval, "sample stays inside both real endpoints");
            require(slot == 0 || sample > plan.synthesisTime(slot - 1), "synthetic content time strictly increases");
            require(std::abs(static_cast<double>(plan.fractions[slot]) -
                             static_cast<double>(expectedSamples[slot]) / static_cast<double>(interval)) < 1e-7,
                    "module fraction describes the supplied integer sample time");
        }
        require(plan.synthesisTime(1) != plan.targets[1] && plan.synthesisTime(2) != plan.targets[2],
                "later S2/S3 recovery content retains its own sample clock instead of the native deadline");
        const auto once = plan;
        require(!applyUniformStretchedFourSamples(plan, previous, previous + interval),
                "applying the correction twice is idempotent");
        require(identical(plan, once), "idempotent correction cannot accumulate timing or fraction error");
    }
} // namespace

int main() {
    constexpr int64_t previous = 1000000000, period = 8333333;
    // Values are hand-derived floor(interval * k / 4), independent of the production helper.
    stretched(previous, 54698246, {13674561, 27349123, 41023684}, {29698247, 38031580, 46364913, 54698246});
    stretched(previous, 37581600, {9395400, 18790800, 28186200}, {12581601, 20914934, 29248267, 37581600});
    stretched(previous, 36071912, {9017978, 18035956, 27053934}, {11071913, 19405246, 27738579, 36071912});
    stretched(std::numeric_limits<int64_t>::max() - 100000000,
              54698247,
              {13674561, 27349123, 41023685},
              {29698248, 38031581, 46364914, 54698247});

    for (const unsigned multiplier : {2u, 3u}) {
        const auto interval = multiplier * period + 100000;
        const auto plan = planned(previous, interval, period, multiplier);
        require(plan.count == multiplier, "2X/3X control pair remains admitted");
        unchanged(plan, previous, previous + interval, "2X/3X retain existing interpolation behavior");
    }
    const auto nominal = planned(previous, 4 * period, period);
    unchanged(nominal, previous, previous + 4 * period, "aligned nominal 4X needs no redistribution");
    for (unsigned slot = 0; slot < 4; ++slot) {
        require(nominal.synthesisTime(slot) == nominal.targets[slot],
                "unmodified plans use the original native sample times");
    }
    unchanged(planned(previous, 4 * period - 1000000, period),
              previous,
              previous + 4 * period - 1000000,
              "compressed 4X group retains its original samples");

    auto tiny = planned(previous, 5, 1);
    const auto tinyNative = tiny.targets;
    require(tiny.count == 4 && applyUniformStretchedFourSamples(tiny, previous, previous + 5),
            "tiny admitted interval uses representable sample timestamps");
    require(tiny.targets == tinyNative &&
                tiny.synthesisTimes == std::array<int64_t, 3>{previous + 1, previous + 2, previous + 3},
            "five-tick source interval has three ordered integer samples without moving deadlines");
    require(tiny.fractions == std::array<float, 3>{.2f, .4f, .6f},
            "tiny samples use actual timestamp fractions rather than incompatible quarter fractions");

    FrameGenerationCadence cadence;
    const auto reservation = cadence.reserve(previous, previous + period, period, 4, 40);
    const auto prime = cadence.plan(reservation, 0, false);
    require(prime.count == 1 && prime.roles[0] == FrameGenerationRole::real, "prime is real-only");
    unchanged(prime, previous, reservation.currentTarget, "phase correction cannot turn a prime into FG");
    const auto rejected = planned(previous, 50 * period, period);
    require(rejected.count == 1 && rejected.roles[0] == FrameGenerationRole::real,
            "large source gap remains rejected by the original admission rule");
    unchanged(rejected, previous, previous + 50 * period, "correction cannot widen pair admission");

    const auto admitted = planned(previous, 54698246, period);
    unchanged(admitted, 0, previous + 54698246, "invalid prior timestamp cannot form new samples");
    unchanged(admitted, previous, previous, "duplicate source endpoint cannot form new samples");
    unchanged(admitted, previous, previous + 54698247, "B must match the actual current source endpoint");
    auto invalid = admitted;
    invalid.roles[1] = FrameGenerationRole::real;
    unchanged(invalid, previous, previous + 54698246, "unexpected role order cannot be corrected");
    invalid = admitted;
    ++invalid.targets[2];
    unchanged(invalid, previous, previous + 54698246, "nonuniform native slots cannot be corrected");
    require(admitted.synthesisTime(4) == 0, "out-of-range sample lookup remains bounded");
    require(!cadence.reserve(previous, std::numeric_limits<int64_t>::max(), period, 4, 40).valid,
            "native-target addition overflow remains rejected");
    std::printf(
        "PASS: 4X content phases, unchanged native deadlines, endpoint fractions and admission guards (%u checks)\n",
        checks);
}
