// CPU-only admission regression through production xrWaitFrame/xrBeginFrame.
// The private SDK clock/status exports are controlled; no device, image, FG
// module, GPU work, or native presentation is created by this fixture.
// Default: characterize the retained contract. --expect-partial-recovery is a
// separate, deliberately RED baseline proposal, not a gameplay-defect test.
#define wmain retainedPartialReservationAsyncMain
#include "nonnr_async_regression.cpp"
#undef wmain

namespace {
    decltype(&ovr_GetTimeInSeconds) originalPartialTime{};
    decltype(&ovr_GetPredictedDisplayTime) originalPartialPrediction{};
    double partialNow{}, partialPrediction{};
    uint64_t partialPredictionId{};
    unsigned partialPredictionCalls{};

    double OVR_CDECL partialTime() {
        return partialNow;
    }
    double OVR_CDECL partialDisplayTime(ovrSession, long long id) {
        partialPredictionId = static_cast<uint64_t>(id);
        ++partialPredictionCalls;
        return partialPrediction;
    }
    struct PartialClockHooks {
        PartialClockHooks() {
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalPartialTime =
                reinterpret_cast<decltype(originalPartialTime)>(GetProcAddress(module, "ovr_GetTimeInSeconds"));
            originalPartialPrediction = reinterpret_cast<decltype(originalPartialPrediction)>(
                GetProcAddress(module, "ovr_GetPredictedDisplayTime"));
            require(originalPartialTime && originalPartialPrediction, "private timing exports required");
            partialPredictionCalls = 0;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalPartialTime), partialTime));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalPartialPrediction), partialDisplayTime));
            checkDetour(DetourTransactionCommit());
        }
        ~PartialClockHooks() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalPartialTime), partialTime) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalPartialPrediction), partialDisplayTime) ||
                DetourTransactionCommit())
                std::_Exit(2);
        }
    };

    void cadenceBoundaries(bool expectRecovery) {
        using namespace virtualdesktop_openxr;
        constexpr int64_t first = 1000000000, period = 10000000;
        FrameGenerationCadence cadence;
        for (const auto multiplier : {3u, 4u}) {
            const auto current = first + (multiplier - 1) * period;
            for (const auto now : {first, first + 1, current - 1}) {
                const auto r = cadence.reserve(now, first, period, multiplier, 27, true);
                std::cout << "CADENCE multiplier=" << multiplier << " now=" << now << " first=" << first
                          << " B=" << current << " valid=" << r.valid << '\n';
                if (!expectRecovery && now > first) {
                    require(!r.valid && !r.multiplier && r.reason == PredictionReservationReason::none,
                            "retained 3x/4x contract rejects a candidate after its first native target");
                    continue;
                }
                require(r.valid && r.firstTarget == first && r.currentTarget == current && r.nativeId == 27 &&
                            r.multiplier == multiplier && r.reason == PredictionReservationReason::usable,
                        "renderable partially elapsed 3x/4x group keeps its original reservation");
                const auto plan = cadence.plan(r, first - period, true);
                require(plan.count == multiplier, "partially elapsed admission retains every original plan slot");
                for (unsigned slot = 0; slot < multiplier; ++slot)
                    require(plan.targets[slot] == first + slot * period,
                            "admission never moves original native targets");
                for (unsigned slot = 0; slot + 1 < multiplier; ++slot)
                    require(std::abs(plan.fractions[slot] - float(slot + 1) / multiplier) < 1e-6f,
                            "admission keeps the original source endpoints and interpolation fractions");
            }
        }
    }

    void cadenceControls() {
        using namespace virtualdesktop_openxr;
        constexpr int64_t first = 1000000000, period = 10000000;
        FrameGenerationCadence cadence;
        for (const auto multiplier : {3u, 4u}) {
            const auto current = first + (multiplier - 1) * period;
            for (const auto now : {current, current + 1}) {
                const auto r = cadence.reserve(now, first, period, multiplier, 27, true);
                require(!r.valid && !r.multiplier && r.reason == PredictionReservationReason::none,
                        "fully expired 3x/4x group remains rejected");
            }
            const auto hidden = cadence.reserve(first + 1, first, period, multiplier, 27, false);
            require(!hidden.valid && !hidden.multiplier,
                    "nonrenderable partially elapsed group does not gain recovery admission");
            const auto onTime = cadence.reserve(first - 1, first, period, multiplier, 27, false);
            require(onTime.valid && !onTime.shouldRender && onTime.firstTarget == first,
                    "future nonrenderable reservation preserves the existing timing contract");
        }
        const auto partial2 = cadence.reserve(first + 1, first, period, 2, 27, true);
        const auto expired2 = cadence.reserve(first + period, first, period, 2, 27, true);
        require(partial2.valid && partial2.multiplier == 2 && partial2.currentTarget == first + period &&
                    !expired2.valid && expired2.multiplier == 2 &&
                    expired2.reason == PredictionReservationReason::expired2 &&
                    expired2.currentTarget == first + period && expired2.nativeId == 27,
                "existing partially elapsed and fully expired 2x semantics stay intact");
        require(!cadence.reserve(first, first, 0, 3, 27).valid && !cadence.reserve(first, first, period, 5, 27).valid,
                "invalid timing and multiplier remain rejected");
        std::cout << "PASS: cadence expiration, visibility, invalid-input and retained 2x controls\n";
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void publicReservation(const wchar_t* backend,
                                      unsigned multiplier,
                                      bool partial,
                                      bool expectRecovery,
                                      bool visible = true,
                                      bool monotonic = false,
                                      bool future = false) {
            OpenXrRuntime runtime;
            BackendState state;
            state.visible = visible;
            BackendHooks backendHooks(backend, state);
            PartialClockHooks clock;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameCompleted = runtime.m_frameBegun;
            runtime.m_isHeadless = false;
            runtime.m_useDeferredFrameWait = true;
            runtime.m_fgObservedVisible = visible;
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_fgDesiredMultiplier = multiplier;
            runtime.m_fgNextReservedNativeId = 27;
            runtime.m_fgTimelineReanchor = false;
            runtime.m_fgNeedReprime = false;
            runtime.m_fgHistoryEpoch = 7;
            runtime.m_fgPreviousHistoryEpoch = 7;
            const int64_t period = static_cast<int64_t>((1. / 90.) * 1e9);
            const int64_t first = 100000000000;
            const int64_t current = first + (multiplier - 1) * period;
            // Exercise the production max(prediction, previous B + period).
            // The SDK prediction is older than that original reserved frontier.
            runtime.m_fgLastReservedTime = first - period;
            runtime.m_fgPreviousCapturedTime = first - period;
            runtime.m_lastPredictedDisplayTime = monotonic ? current : first - period;
            partialPrediction = runtime.xrTimeToOvrTime(first - period);
            partialNow = runtime.xrTimeToOvrTime(future    ? first - period / 4
                                                 : partial ? first + period / 4
                                                           : current + period / 4);
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(runtime.xrWaitFrame(reinterpret_cast<XrSession>(1), nullptr, &frame) == XR_SUCCESS,
                    "controlled public Wait succeeds");
            const auto reservation = runtime.m_fgWaitedReservation;
            require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS,
                    "public Begin retains the waited reservation without discarding it");
            const auto& begun = runtime.m_fgBegunReservation;
            std::cout << "PUBLIC multiplier=" << multiplier << " partial=" << partial << " future=" << future
                      << " visible=" << visible << " monotonic=" << monotonic << " valid=" << begun.valid
                      << " reason=" << static_cast<unsigned>(begun.reason) << " first=" << begun.firstTarget
                      << " B=" << begun.currentTarget << " returned=" << frame.predictedDisplayTime
                      << " period=" << frame.predictedDisplayPeriod
                      << " next_native=" << runtime.m_fgNextReservedNativeId << '\n';
            require(partialPredictionCalls == 1 && partialPredictionId == 27,
                    "actual public Wait queries exactly the original native reservation ID");
            require(begun.valid == reservation.valid && begun.reason == reservation.reason &&
                        begun.firstTarget == reservation.firstTarget &&
                        begun.currentTarget == reservation.currentTarget && begun.nativeId == reservation.nativeId &&
                        begun.historyEpoch == 7 && begun.returnedDisplayTime == frame.predictedDisplayTime,
                    "public Begin copies the complete waited snapshot unchanged");
            require(runtime.m_fgPreviousCapturedTime == first - period && runtime.m_fgPreviousHistoryEpoch == 7 &&
                        runtime.m_fgDesiredMultiplier == multiplier && !runtime.m_fgNeedReprime,
                    "prediction admission does not itself rewrite captured history or desired cadence");
            require(state.createdChains.empty() && !runtime.m_frameGeneration && !runtime.m_ovrSubmissionDevice &&
                        !runtime.m_asyncSubmissionThread.joinable(),
                    "CPU fixture creates no graphics resources, module or native worker");
            const bool retainsPositions = future || (partial && visible && expectRecovery) || multiplier == 2;
            require(runtime.m_fgNextReservedNativeId == 27 + (retainsPositions ? multiplier : 0),
                    "public frontier accounts for exactly the accepted original native positions");
            if (partial && visible && retainsPositions) {
                require(begun.firstTarget == first && begun.currentTarget == current && begun.nativeId == 27 &&
                            begun.multiplier == multiplier &&
                            frame.predictedDisplayPeriod == (monotonic ? period : multiplier * period) &&
                            runtime.m_fgLastReservedTime == current,
                        "accepted reservation retains original targets and the existing public period selection");
                require(monotonic ? !begun.valid && begun.reason == PredictionReservationReason::monotonic &&
                                        frame.predictedDisplayTime == current + 1
                                  : begun.valid && begun.reason == PredictionReservationReason::usable &&
                                        frame.predictedDisplayTime == current,
                        "accepted reservation respects the existing public monotonicity guard");
            } else if (multiplier == 2) {
                require(!begun.valid && begun.reason == PredictionReservationReason::expired2 &&
                            begun.currentTarget == current && frame.predictedDisplayTime == current,
                        "public fully expired 2x retains its established positions and fallback reason");
            } else {
                require(!begun.valid && !begun.multiplier && begun.reason == PredictionReservationReason::monotonic &&
                            frame.predictedDisplayTime == (monotonic ? current : first - period) + 1 &&
                            frame.predictedDisplayPeriod == period && runtime.m_fgLastReservedTime == first - period,
                        "retained rejected 3x/4x candidate keeps native cadence, frontier and public monotonic clamp");
            }
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::cout << std::unitbuf;
    if ((argc != 2 && argc != 3) || (argc == 3 && std::wstring(argv[2]) != L"--expect-partial-recovery" &&
                                     std::wstring(argv[2]) != L"--retained-contract")) {
        std::cerr << "FAIL: <private-backend-directory|--cadence-only> "
                     "[--retained-contract|--expect-partial-recovery] required\n";
        return 2;
    }
    const bool expectRecovery = argc == 3 && std::wstring(argv[2]) == L"--expect-partial-recovery";
    std::cout << "CONTRACT="
              << (expectRecovery ? "partial-recovery-proposal; expected RED on retained baseline" : "retained-baseline")
              << "; CPU admission only, not an archived backlog or gameplay-defect reproduction\n";
    unsigned failures{};
    const auto run = [&](const char* name, const auto& test) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    };
    run("cadence controls", cadenceControls);
    run("partially elapsed cadence boundaries", [&] { cadenceBoundaries(expectRecovery); });
    if (std::wstring(argv[1]) != L"--cadence-only") {
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        for (const auto multiplier : {3u, 4u}) {
            run("public partially elapsed reservation",
                [&] { Regression::publicReservation(argv[1], multiplier, true, expectRecovery); });
            run("public fully expired control",
                [&] { Regression::publicReservation(argv[1], multiplier, false, expectRecovery); });
            run("public nonrenderable control",
                [&] { Regression::publicReservation(argv[1], multiplier, true, expectRecovery, false); });
            run("public future monotonicity control",
                [&] { Regression::publicReservation(argv[1], multiplier, true, expectRecovery, true, true, true); });
        }
        run("public partially elapsed 2x control",
            [&] { Regression::publicReservation(argv[1], 2, true, expectRecovery); });
        run("public fully expired 2x control",
            [&] { Regression::publicReservation(argv[1], 2, false, expectRecovery); });
    }
    return failures ? 1 : 0;
}
