#include "../virtualdesktop-openxr/NrBeforeSrStereo.h"

#include <cstdio>
#include <cstdlib>

using namespace virtualdesktop_openxr::nr_before_sr;

namespace {
    constexpr StereoViewKey a{91, {16, 32, 1600, 1200}};
    constexpr StereoViewKey b{7, {1648, 32, 1600, 1200}};

    void require(bool condition, const char* explanation) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", explanation);
            std::exit(1);
        }
    }

    void observe(StereoGate& gate, StereoViewKey view) {
        const auto frame = gate.enterEvaluation();
        gate.leaveEvaluation(frame, view, false);
    }

    void epoch(StereoGate& gate, StereoViewKey first, StereoViewKey second) {
        gate.begin();
        observe(gate, first);
        observe(gate, second);
        gate.complete(true);
    }

    void calibrationAndCallOrder() {
        StereoGate gate;
        observe(gate, a);
        observe(gate, b);
        gate.complete(true);
        epoch(gate, a, b);
        gate.begin();
        require(!gate.expected(), "one stereo frame cannot authorize model work");
        observe(gate, b);
        observe(gate, a);
        gate.complete(true);
        require(!gate.expected(), "model work is scoped to an open frame");
        gate.begin();
        require(gate.expected() && gate.canApply(b.generation, b.output) && gate.canApply(a.generation, a.output),
                "two accepted stereo frames qualify the same pair across reversed evaluation order");
        require(!gate.canApply(a.generation, b.output) && !gate.canApply(100, a.output),
                "both output region and live generation must match");
        observe(gate, b);
        require(!gate.canApply(b.generation, b.output) && gate.canApply(a.generation, a.output),
                "each qualified view can be treated once per frame");
        observe(gate, a);
        require(!gate.canApply(a.generation, a.output), "completed pair admits no more calls");
        gate.complete(true);
        // Separate eye outputs may use the same display rectangle.
        StereoGate separate;
        const StereoViewKey other{b.generation, a.output};
        epoch(separate, a, other);
        epoch(separate, other, a);
        separate.begin();
        require(separate.canApply(other.generation, other.output), "distinct features may use identical rectangles");
    }

    void invalidWindows() {
        for (int fault = 0; fault != 9; ++fault) {
            StereoGate gate;
            epoch(gate, a, b);
            epoch(gate, b, a);
            gate.begin();
            observe(gate, a);
            switch (fault) {
            case 0:
                break; // Missing second view.
            case 1:
                observe(gate, a);
                break;
            case 2:
                observe(gate, {a.generation, b.output});
                break;
            case 3:
                observe(gate, b);
                observe(gate, {45, b.output});
                break;
            case 4:
                observe(gate, b);
                break; // xrEndFrame rejects the projection below.
            case 5:
                observe(gate, {0, b.output});
                break;
            case 6:
                observe(gate, {b.generation, {0, 0, 0, 1200}});
                break;
            case 7:
                gate.begin();
                break; // Missing end of the previous epoch.
            case 8:
                observe(gate, b);
                observe(gate, b);
                break;
            }
            if (fault != 0 && fault != 4)
                require(!gate.expected(), "observed ambiguity immediately blocks further treatment");
            gate.complete(fault != 4);
            gate.begin();
            require(!gate.expected(), "an invalid window clears readiness");
            observe(gate, a);
            observe(gate, b);
            gate.complete(true);
            gate.begin();
            require(!gate.expected(), "invalid window also clears the first calibration sample");
            observe(gate, b);
            observe(gate, a);
            gate.complete(true);
            gate.begin();
            require(gate.expected() != nullptr, "two fresh consecutive accepted windows restore readiness");
        }
    }

    void changedPair() {
        for (const StereoViewKey replacement : {
                 StereoViewKey{100, b.output},
                 StereoViewKey{b.generation, {1652, 32, 1600, 1200}},
                 StereoViewKey{b.generation, {1648, 36, 1600, 1200}},
                 StereoViewKey{b.generation, {1648, 32, 1500, 1200}},
                 StereoViewKey{b.generation, {1648, 32, 1600, 1100}},
             }) {
            StereoGate gate;
            epoch(gate, a, b);
            epoch(gate, b, a);
            gate.begin();
            require(!gate.canApply(replacement.generation, replacement.output), "changed key must pass through");
            observe(gate, replacement);
            require(!gate.canApply(a.generation, a.output), "do not treat mate after observing a changed key");
            observe(gate, a);
            gate.complete(true);
            gate.begin();
            require(!gate.expected(), "one complete changed pair only seeds recalibration");
            observe(gate, a);
            observe(gate, replacement);
            gate.complete(true);
            gate.begin();
            require(gate.canApply(replacement.generation, replacement.output) && !gate.canApply(b.generation, b.output),
                    "replacement generation or display region must qualify afresh");
        }
    }

    void callsAcrossFrameBoundaries() {
        StereoGate gate;
        epoch(gate, a, b);
        epoch(gate, b, a);
        gate.begin();
        const auto first = gate.enterEvaluation();
        require(gate.canApply(a.generation, a.output), "a single in-flight call may record");
        gate.leaveEvaluation(first, a, true);
        const auto old = gate.enterEvaluation();
        require(!gate.complete(true) && gate.modelsRecorded() == 1, "outstanding second eye leaves partial coverage");
        gate.begin();
        require(!gate.expected() && gate.modelsRecorded() == 0, "new frame starts without coverage");
        require(!gate.leaveEvaluation(old, b, true) && gate.modelsRecorded() == 0,
                "late return cannot credit a model to a different frame");
        observe(gate, a);
        observe(gate, b);
        require(!gate.complete(true), "a frame entered with a pending call cannot seed calibration");
        epoch(gate, a, b);
        gate.begin();
        require(!gate.expected(), "the crossed frame cannot count as the first calibration sample");
        observe(gate, a);
        observe(gate, b);
        gate.complete(true);

        const auto outside = gate.enterEvaluation();
        gate.begin();
        require(!gate.expected() && !gate.leaveEvaluation(outside, b, false),
                "a call begun between frames cannot complete in an open frame");
        observe(gate, a);
        observe(gate, b);
        require(!gate.complete(true), "an outstanding between-frame call invalidates this frame");
    }

    void overlappingEvaluations() {
        for (const bool innerFirst : {true, false}) {
            StereoGate gate;
            epoch(gate, a, b);
            epoch(gate, b, a);
            gate.begin();
            require(!gate.evaluationInFlight(), "a known call may retire previous SDK objects before native entry");
            const auto outer = gate.enterEvaluation();
            require(gate.evaluationInFlight(), "an overlapping entry cannot retire SDK objects");
            require(gate.canApply(a.generation, a.output), "admitted outer call may record an eye");
            const auto inner = gate.enterEvaluation();
            require(!gate.canApply(a.generation, a.output) && !gate.canApply(b.generation, b.output),
                    "overlap blocks both duplicate and mate before either can be recorded");
            if (innerFirst) {
                gate.leaveEvaluation(inner, a, false);
                gate.leaveEvaluation(outer, a, true);
            } else {
                gate.leaveEvaluation(outer, a, true);
                require(gate.evaluationInFlight(), "an out-of-order return still leaves a native call outstanding");
                gate.leaveEvaluation(inner, a, false);
            }
            require(!gate.evaluationInFlight(), "retirement can resume after both calls return");
            require(gate.modelsRecorded() == 1 && !gate.complete(true),
                    "nested or concurrent returns preserve actual partial coverage");
            epoch(gate, a, b);
            epoch(gate, b, a);
            gate.begin();
            require(gate.expected(), "all overlapping calls retired before fresh calibration");
            gate.leaveEvaluation(gate.enterEvaluation(), b, true);
            gate.leaveEvaluation(gate.enterEvaluation(), a, true);
            require(gate.complete(true) && gate.modelsRecorded() == 2,
                    "ordinary reversed-order stereo resumes with two recorded models");
        }
    }

    void calibrationFollowsTheSdkThread() {
        StereoGate gate;
        const StereoViewKey left{a.generation, a.output, 10};
        const StereoViewKey right{b.generation, b.output, 10};
        const StereoViewKey newLeft{a.generation, a.output, 20};
        const StereoViewKey newRight{b.generation, b.output, 20};
        epoch(gate, left, newRight);
        epoch(gate, left, newRight);
        gate.begin();
        require(!gate.expected(), "a pair split over SDK threads never calibrates");
        observe(gate, left);
        gate.complete(true);
        epoch(gate, left, right);
        epoch(gate, newRight, newLeft);
        gate.begin();
        require(!gate.expected(), "a startup thread does not qualify work on a different SDK thread");
        observe(gate, newLeft);
        observe(gate, newRight);
        gate.complete(true);
        gate.begin();
        require(gate.canApply(newLeft.generation, newLeft.output, newLeft.sdkThread) &&
                    !gate.canApply(left.generation, left.output, left.sdkThread),
                "recalibration permits the stable SDK thread and excludes the earlier caller");
    }

    void failureAfterEntry() {
        StereoGate gate;
        epoch(gate, a, b);
        epoch(gate, b, a);
        gate.begin();
        const auto success = gate.enterEvaluation();
        gate.leaveEvaluation(success, a, true);
        const auto failed = gate.enterEvaluation();
        gate.leaveEvaluation(failed, {b.generation, {}}, false);
        require(!gate.expected() && !gate.complete(true) && gate.modelsRecorded() == 1,
                "a preparation exception with a known generation invalidates partial treatment");
        epoch(gate, a, b);
        epoch(gate, b, a);
        gate.begin();
        gate.leaveEvaluation(gate.enterEvaluation(), a, true);
        gate.leaveEvaluation(gate.enterEvaluation(), b, true);
        const auto extra = gate.enterEvaluation();
        require(!gate.complete(true) && gate.modelsRecorded() == 2,
                "even two recorded eyes cannot qualify a frame with a third outstanding call");
        require(!gate.leaveEvaluation(extra, {}, false), "return after end cannot alter a closed frame");
    }

    void partialStereoCannotAuthorizeTheNextPair() {
        StereoGate gate;
        epoch(gate, a, b);
        epoch(gate, a, b);
        gate.begin();
        require(gate.expected() != nullptr, "valid input pair is calibrated");
        gate.leaveEvaluation(gate.enterEvaluation(), a, true);
        gate.leaveEvaluation(gate.enterEvaluation(), b, false);
        require(!gate.complete(true) && gate.modelsRecorded() == 1,
                "one recorded eye invalidates calibration even when both native eyes succeed");
        gate.begin();
        require(!gate.expected(), "an incomplete NR pair cannot recur on the next frame");
        observe(gate, a);
        observe(gate, b);
        gate.complete(true);
        gate.begin();
        require(!gate.expected(), "one stable frame starts calibration afresh");
        observe(gate, a);
        observe(gate, b);
        gate.complete(true);
        gate.begin();
        require(gate.expected() != nullptr, "two stable frames restore calibration");
        observe(gate, a);
        observe(gate, b);
        require(gate.complete(true), "a busy bank skipping the whole pair keeps calibration");
        gate.begin();
        require(gate.expected() != nullptr, "whole-pair passthrough remains ready");
    }
} // namespace

int main() {
    calibrationAndCallOrder();
    invalidWindows();
    changedPair();
    callsAcrossFrameBoundaries();
    overlappingEvaluations();
    calibrationFollowsTheSdkThread();
    failureAfterEntry();
    partialStereoCannotAuthorizeTheNextPair();
    std::puts("PASS: before-SR stereo calibration");
}
