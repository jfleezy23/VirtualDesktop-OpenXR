// CPU-only public prediction/frontier regressions. The real FG worker consumes
// disabled, ticketless layers; every native SDK boundary is intercepted.
#include "pch.h"
#include "runtime.h"

// Include headers before defining the entry-point alias: SDK headers may alter
// entry-point macros on the first inclusion.
#define wmain retainedPredictionFrontierAsyncMain
#include "nonnr_async_regression.cpp"
#undef wmain

namespace {
    struct PredictionProbe {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned statusCalls{};
        std::vector<long long> predictionIds;
        std::atomic<DWORD> applicationThread{GetCurrentThreadId()};
        std::atomic<double> now{100.};
        std::atomic<double> first{100.01};
        static constexpr double period = 1. / 90.;
    };

    PredictionProbe* predictionProbe{};
    decltype(&ovr_GetTimeInSeconds) originalFrontierTime{};
    decltype(&ovr_GetPredictedDisplayTime) originalFrontierPrediction{};

    double OVR_CDECL frontierTime() {
        return predictionProbe->now.load();
    }
    double OVR_CDECL frontierPrediction(ovrSession, long long id) {
        auto& probe = *predictionProbe;
        std::lock_guard lock(probe.mutex);
        if (GetCurrentThreadId() == probe.applicationThread.load())
            probe.predictionIds.push_back(id);
        probe.changed.notify_all();
        return probe.first.load() + id * PredictionProbe::period;
    }
    ovrResult OVR_CDECL frontierStatus(ovrSession session, ovrSessionStatus* status) {
        const auto result = interceptedStatus(session, status);
        auto& probe = *predictionProbe;
        std::lock_guard lock(probe.mutex);
        ++probe.statusCalls;
        probe.changed.notify_all();
        return result;
    }

    // One hook owner, rather than layering another detour on BackendHooks.
    struct FrontierHooks {
        FrontierHooks(const wchar_t* directory, BackendState& state, PredictionProbe& probe) {
            ovrInitParams init{};
            init.Flags = ovrInit_RequestVersion;
            init.RequestedMinorVersion = OVR_MINOR_VERSION;
            auto path = std::filesystem::absolute(directory).wstring() + L"\\";
            require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, path.c_str())),
                    "private CPU backend initialization");
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            require(module != nullptr, "private CPU backend module");
#define FRONTIER_EXPORT(variable, name)                                                                                \
    variable = reinterpret_cast<decltype(variable)>(GetProcAddress(module, name));                                     \
    require(variable != nullptr, "private CPU backend export " name)
            FRONTIER_EXPORT(originalWait, "ovr_WaitToBeginFrame");
            FRONTIER_EXPORT(originalBegin, "ovr_BeginFrame");
            FRONTIER_EXPORT(originalEnd, "ovr_EndFrame");
            FRONTIER_EXPORT(originalStatus, "ovr_GetSessionStatus");
            FRONTIER_EXPORT(originalHmdDesc, "ovr_GetHmdDesc");
            FRONTIER_EXPORT(originalIndex, "ovr_GetTextureSwapChainCurrentIndex");
            FRONTIER_EXPORT(originalCreateChain, "ovr_CreateTextureSwapChainDX");
            FRONTIER_EXPORT(originalDestroyChain, "ovr_DestroyTextureSwapChain");
            FRONTIER_EXPORT(originalFrontierTime, "ovr_GetTimeInSeconds");
            FRONTIER_EXPORT(originalFrontierPrediction, "ovr_GetPredictedDisplayTime");
#undef FRONTIER_EXPORT
            backendState = &state;
            predictionProbe = &probe;
            attach(true);
        }
        void attach(bool enable) {
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            const auto action = enable ? DetourAttach : DetourDetach;
            checkDetour(action(reinterpret_cast<PVOID*>(&originalWait), interceptedWait));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalBegin), interceptedBegin));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalEnd), interceptedEnd));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalStatus), frontierStatus));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalHmdDesc), interceptedHmdDesc));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalIndex), interceptedIndex));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalCreateChain), interceptedCreateChain));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalDestroyChain), interceptedDestroyChain));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalFrontierTime), frontierTime));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalFrontierPrediction), frontierPrediction));
            checkDetour(DetourTransactionCommit());
        }
        ~FrontierHooks() {
            try {
                attach(false);
            } catch (...) {
                std::_Exit(2);
            }
            predictionProbe = nullptr;
            backendState = nullptr;
        }
    };

    bool sameReservation(const virtualdesktop_openxr::PredictionReservation& a,
                         const virtualdesktop_openxr::PredictionReservation& b) {
        return a.valid == b.valid && a.firstTarget == b.firstTarget && a.currentTarget == b.currentTarget &&
               a.period == b.period && a.nativeId == b.nativeId && a.multiplier == b.multiplier &&
               a.historyEpoch == b.historyEpoch && a.shouldRender == b.shouldRender && a.reason == b.reason &&
               a.returnedDisplayTime == b.returnedDisplayTime;
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void seed(OpenXrRuntime& runtime, unsigned multiplier, bool deferred) {
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameCompleted = runtime.m_frameBegun;
            runtime.m_useMirrorWindow = false;
            runtime.m_useDeferredFrameWait = deferred;
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_fgDesiredMultiplier = multiplier;
            runtime.m_fgObservedVisible = true;
            runtime.m_fgHistoryEpoch = runtime.m_fgPreviousHistoryEpoch = 7;
            runtime.m_fgPreviousCapturedTime = 98000000000;
            runtime.m_asyncSubmissionReady = true;
        }
        static XrFrameState wait(OpenXrRuntime& runtime) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(runtime.xrWaitFrame(reinterpret_cast<XrSession>(1), nullptr, &frame) == XR_SUCCESS,
                    "public Wait succeeds");
            return frame;
        }
        static void noGraphics(const OpenXrRuntime& runtime, const BackendState& state) {
            require(!runtime.m_frameGeneration && !runtime.m_deliveryTransport && !runtime.m_ovrSubmissionDevice &&
                        !runtime.m_d3d11Device && !runtime.m_d3d12CommandQueue && state.createdChains.empty(),
                    "CPU fixture creates no graphics device, texture, module or delivery transport");
        }
        static void prediction(PredictionProbe& probe, uint64_t expected, const PredictionReservation& reservation) {
            std::lock_guard lock(probe.mutex);
            require(!probe.predictionIds.empty(), "public Wait samples a private SDK prediction");
            const auto sampled = probe.predictionIds.back();
            std::cout << "FRONTIER queried=" << sampled << " reserved=" << reservation.nativeId
                      << " expected=" << expected << " multiplier=" << reservation.multiplier << '\n';
            require(sampled >= 0 && static_cast<uint64_t>(sampled) == expected && reservation.nativeId == expected,
                    "prediction query and new reservation start at the unissued native frontier");
        }
        static void published(const wchar_t* backend, unsigned multiplier, bool capacity, bool valid) {
            OpenXrRuntime runtime;
            BackendState state;
            PredictionProbe probe;
            FrontierHooks hooks(backend, state, probe);
            seed(runtime, multiplier, true);
            // The initial group is genuinely accepted/rejected by public Wait.
            // An expired 3X/4X candidate owns no span; its ticketless End still
            // irrevocably publishes one ordinary native cycle.
            if (!valid)
                probe.first = probe.now.load() - .001;
            const auto firstFrame = wait(runtime);
            const auto firstReservation = runtime.m_fgWaitedReservation;
            require(valid ? firstReservation.valid && firstReservation.multiplier == multiplier
                          : !firstReservation.valid && !firstReservation.multiplier,
                    "initial public candidate has the intended retained admission outcome");
            require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS,
                    "initial public Begin succeeds");
            runtime.m_asyncSubmissionThread = std::thread([&] { runtime.asyncFrameGenerationThread(); });
            std::thread waiter;
            std::exception_ptr waiterError;
            std::atomic<bool> waiterDone{};
            XrFrameState next{XR_TYPE_FRAME_STATE};
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state, &waiter); });
            require(state.waitFor([&] { return state.waitCalls == 1; }),
                    "real FG worker reaches its initial controlled native Wait");
            auto end = emptyFrame();
            end.displayTime = firstFrame.predictedDisplayTime;
            require(runtime.xrEndFrame(reinterpret_cast<XrSession>(1), &end) == XR_SUCCESS,
                    "public End publishes a disabled ticketless group");
            const auto published = runtime.m_fgPublishedEndNativeId.load();
            require(published == (valid ? multiplier : 1u) && !runtime.m_fgTimelineReanchor.load(),
                    "publication owns exactly its old span before the worker can request recovery");
            // A fallback intentionally changes the desired next cadence to 1.
            // It does not give that new cadence another budget before prediction.
            require(runtime.m_fgDesiredMultiplier.load() == 1,
                    "complete-layer fallback retains the ordinary next-cadence policy");
            runtime.m_fgPreviousCapturedTime = 98000000000;
            runtime.m_fgPreviousHistoryEpoch = 7;
            const auto history = runtime.m_fgPreviousCapturedTime;
            probe.first = probe.now.load() + .01;
            runtime.m_useDeferredFrameWait = !capacity;
            if (capacity) {
                waiter = std::thread([&] {
                    probe.applicationThread = GetCurrentThreadId();
                    try {
                        next = wait(runtime);
                    } catch (...) {
                        waiterError = std::current_exception();
                    }
                    waiterDone = true;
                });
                {
                    std::unique_lock lock(probe.mutex);
                    require(probe.changed.wait_for(lock, 2000ms, [&] { return probe.statusCalls == 2; }),
                            "next public Wait enters its frame critical section");
                }
                // Status is called under frameMutex. With matched Wait/Begin
                // counts and no startup resources, capacity is this call's first
                // frameMutex release. This is an entry handshake, not a sleep.
                {
                    std::unique_lock lock(runtime.m_frameMutex);
                    std::lock_guard probeLock(probe.mutex);
                    require(!waiterDone.load() && probe.predictionIds.size() == 1,
                            "early public Wait blocks in capacity before its prediction sample");
                }
                state.release();
                waiter.join();
                if (waiterError)
                    std::rethrow_exception(waiterError);
                require(state.waitFor([&] { return state.frames.size() == published; }),
                        "real worker closes every published original cycle");
                require(!runtime.m_fgTimelineReanchor.load(),
                        "public prediction consumes the worker reanchor after capacity returns");
            } else {
                next = wait(runtime);
            }
            require(runtime.m_fgWaitedReservation.valid && runtime.m_fgWaitedReservation.multiplier == 1,
                    "new ordinary reservation remains eligible after fallback");
            prediction(probe, published, runtime.m_fgWaitedReservation);
            require(runtime.m_fgWaitedReservation.nativeId >= runtime.m_fgPublishedEndNativeId.load(),
                    "new reservation never reuses an irrevocably published native position");
            require(runtime.m_fgPreviousCapturedTime == history && runtime.m_fgPreviousHistoryEpoch == 7,
                    "public prediction recovery does not rewrite captured source history");
            const auto waited = runtime.m_fgWaitedReservation;
            require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS &&
                        sameReservation(waited, runtime.m_fgBegunReservation) &&
                        runtime.m_fgBegunReservation.returnedDisplayTime == next.predictedDisplayTime,
                    "public Begin preserves the complete returned reservation and timestamp");
            noGraphics(runtime, state);
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
        static void controls(const wchar_t* backend, unsigned multiplier, bool unpublished, bool monotonic) {
            OpenXrRuntime runtime;
            BackendState state;
            PredictionProbe probe;
            FrontierHooks hooks(backend, state, probe);
            seed(runtime, multiplier, true);
            runtime.m_fgNextReservedNativeId = 27;
            runtime.m_fgWorkerGroupNativeId = runtime.m_fgPublishedEndNativeId = 27;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            const auto previousHistory = runtime.m_fgPreviousCapturedTime;
            if (monotonic)
                runtime.m_lastPredictedDisplayTime = 200000000000;
            const auto frame = wait(runtime);
            require(runtime.m_fgNextReservedNativeId == 27 + multiplier,
                    "accepted original positions advance once before a possible public monotonic clamp");
            prediction(probe, 27, runtime.m_fgWaitedReservation);
            if (monotonic) {
                require(!runtime.m_fgWaitedReservation.valid &&
                            runtime.m_fgWaitedReservation.reason == PredictionReservationReason::monotonic &&
                            runtime.m_fgWaitedReservation.multiplier == multiplier &&
                            frame.predictedDisplayTime == 200000000001,
                        "public monotonic clamp preserves the retained original span without another preadvance");
            } else {
                require(runtime.m_fgWaitedReservation.valid && runtime.m_fgWaitedReservation.multiplier == multiplier,
                        "future valid 2X/3X/4X candidate preserves its original span");
            }
            require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS,
                    "control public Begin succeeds");
            const auto begun = runtime.m_fgBegunReservation;
            if (unpublished) {
                runtime.m_fgTimelineReanchor = true;
                const auto next = wait(runtime);
                prediction(probe, 27 + multiplier, runtime.m_fgWaitedReservation);
                require(sameReservation(begun, runtime.m_fgBegunReservation) &&
                            begun.returnedDisplayTime == frame.predictedDisplayTime &&
                            next.predictedDisplayTime > frame.predictedDisplayTime,
                        "pipelined Wait reserves beyond an unpublished begun span without rewriting it");
            }
            require(runtime.m_fgPreviousCapturedTime == previousHistory && runtime.m_fgPreviousHistoryEpoch == 7,
                    "prediction controls preserve captured history");
            noGraphics(runtime, state);
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
        static void retainedTwo(const wchar_t* backend) {
            OpenXrRuntime runtime;
            BackendState state;
            PredictionProbe probe;
            FrontierHooks hooks(backend, state, probe);
            seed(runtime, 2, true);
            runtime.m_isHeadless = false;
            runtime.m_fgNextReservedNativeId = 27;
            probe.first = probe.now.load() - 28 * PredictionProbe::period;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            wait(runtime);
            require(!runtime.m_fgWaitedReservation.valid &&
                        runtime.m_fgWaitedReservation.reason == PredictionReservationReason::expired2 &&
                        runtime.m_fgWaitedReservation.multiplier == 2 && runtime.m_fgWaitedReservation.nativeId == 27 &&
                        runtime.m_fgNextReservedNativeId == 29,
                    "fully expired renderable 2X retains exactly its two ordinary native positions");
            prediction(probe, 27, runtime.m_fgWaitedReservation);
            noGraphics(runtime, state);
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
        static void unpublishedFallback(const wchar_t* backend, unsigned multiplier) {
            OpenXrRuntime runtime;
            BackendState state;
            PredictionProbe probe;
            FrontierHooks hooks(backend, state, probe);
            seed(runtime, multiplier, true);
            probe.first = probe.now.load() - .001;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            const auto frame = wait(runtime);
            require(!runtime.m_fgWaitedReservation.valid && !runtime.m_fgWaitedReservation.multiplier &&
                        runtime.m_fgNextReservedNativeId == 0,
                    "rejected 3X/4X admission has not preadvanced native positions");
            require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS,
                    "rejected candidate can become the current application frame");
            const auto begun = runtime.m_fgBegunReservation;
            require(!runtime.m_fgTimelineReanchor.load() && !runtime.m_fgPublishedEndNativeId.load() &&
                        !runtime.m_fgWorkerGroupNativeId.load(),
                    "unpublished rejected group has neither publication nor a forced reanchor");
            probe.first = probe.now.load() + .01;
            wait(runtime);
            prediction(probe, 1, runtime.m_fgWaitedReservation);
            require(sameReservation(begun, runtime.m_fgBegunReservation) &&
                        begun.returnedDisplayTime == frame.predictedDisplayTime,
                    "pipelined prediction accounts for one unpublished fallback without rewriting its timestamp");
            noGraphics(runtime, state);
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
        static void discard(const wchar_t* backend, unsigned multiplier) {
            OpenXrRuntime runtime;
            BackendState state;
            PredictionProbe probe;
            FrontierHooks hooks(backend, state, probe);
            seed(runtime, multiplier, true);
            runtime.m_fgNextReservedNativeId = 27;
            runtime.m_fgWorkerGroupNativeId = runtime.m_fgPublishedEndNativeId = 27;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            wait(runtime);
            require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS,
                    "first discard control Begin succeeds");
            const auto second = wait(runtime);
            require(runtime.m_fgNextReservedNativeId == 27 + 2 * multiplier,
                    "two issued public candidates initially reserve both original spans");
            require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_FRAME_DISCARDED,
                    "second Begin without End genuinely discards the earlier frame");
            const auto begun = runtime.m_fgBegunReservation;
            require(begun.reason == PredictionReservationReason::discarded && !begun.valid &&
                        runtime.m_fgTimelineReanchor.load(),
                    "actual discard requests retained timeline recovery");
            wait(runtime);
            prediction(probe, 27 + multiplier, runtime.m_fgWaitedReservation);
            require(sameReservation(begun, runtime.m_fgBegunReservation) &&
                        begun.returnedDisplayTime == second.predictedDisplayTime,
                    "discard can lower the unissued frontier while preserving the current public timestamp");
            noGraphics(runtime, state);
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2 && argc != 3) {
        std::cerr << "FAIL: <private-backend-directory> [case] required\n";
        return 2;
    }
    using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
    const std::wstring selected = argc == 3 ? argv[2] : L"all";
    unsigned ran{}, failures{};
    const auto run = [&](const std::wstring& name, const auto& test) {
        if (selected != L"all" && selected != name)
            return;
        ++ran;
        try {
            test();
            std::wcout << L"PASS: " << name << L"\n";
        } catch (const std::exception& error) {
            ++failures;
            std::wcerr << L"FAIL: " << name << L": ";
            std::cerr << error.what() << '\n';
        }
    };
    for (const auto multiplier : {2u, 3u, 4u}) {
        const auto suffix = std::to_wstring(multiplier);
        run(L"valid-" + suffix, [&] { Regression::controls(argv[1], multiplier, false, false); });
        run(L"published-valid-" + suffix, [&] { Regression::published(argv[1], multiplier, false, true); });
        if (multiplier > 2) {
            run(L"published-" + suffix, [&] { Regression::published(argv[1], multiplier, false, false); });
            run(L"capacity-" + suffix, [&] { Regression::published(argv[1], multiplier, true, false); });
            run(L"unpublished-" + suffix, [&] { Regression::controls(argv[1], multiplier, true, false); });
            run(L"unpublished-fallback-" + suffix, [&] { Regression::unpublishedFallback(argv[1], multiplier); });
            run(L"discard-" + suffix, [&] { Regression::discard(argv[1], multiplier); });
            run(L"monotonic-" + suffix, [&] { Regression::controls(argv[1], multiplier, false, true); });
        }
    }
    run(L"retained-2", [&] { Regression::retainedTwo(argv[1]); });
    if (!ran) {
        std::cerr << "FAIL: unknown prediction-frontier case\n";
        return 2;
    }
    return failures ? 1 : 0;
}
