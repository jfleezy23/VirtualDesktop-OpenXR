// Actual synthesis and interop through the production native submission worker.
// The private backend boundary is controlled: this proves handoff ordering and
// deadline policy, not physical headset scanout or a measured VD render margin.
#define wmain retainedAsyncFixtureMain
#include "nonnr_async_regression.cpp"
#undef wmain
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main

#include "fg_early_ready_test_gate.h"

namespace {
    std::atomic<double> handoffClock{};
    decltype(&ovr_GetTimeInSeconds) originalHandoffTime{};
    decltype(&ovr_CommitTextureSwapChain) originalHandoffCommit{};
    decltype(&ovr_GetPredictedDisplayTime) originalHandoffPrediction{};
    std::atomic<long long> handoffWaitedId{-1};
    std::atomic<unsigned> handoffPredictionCalls{};
    double handoffPredictionBase{}, handoffPredictionPeriod{};
    std::vector<std::pair<long long, double>> handoffPredictions;
    double OVR_CDECL fixedHandoffTime() {
        return handoffClock.load();
    }
    ovrResult OVR_CDECL handoffCommit(ovrSession, ovrTextureSwapChain) {
        return ovrSuccess;
    }
    double OVR_CDECL handoffPrediction(ovrSession, long long id) {
        ++handoffPredictionCalls;
        const double result =
            handoffPredictionBase + id * handoffPredictionPeriod + (handoffWaitedId.load() >= id ? .001 : 0.);
        handoffPredictions.emplace_back(id, result);
        return result;
    }
    struct HandoffHooks {
        const double origin = ovr_GetTimeInSeconds();
        HandoffHooks() {
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalHandoffTime =
                reinterpret_cast<decltype(originalHandoffTime)>(GetProcAddress(module, "ovr_GetTimeInSeconds"));
            originalHandoffCommit =
                reinterpret_cast<decltype(originalHandoffCommit)>(GetProcAddress(module, "ovr_CommitTextureSwapChain"));
            originalHandoffPrediction = reinterpret_cast<decltype(originalHandoffPrediction)>(
                GetProcAddress(module, "ovr_GetPredictedDisplayTime"));
            require(originalHandoffTime && originalHandoffCommit && originalHandoffPrediction,
                    "handoff backend exports");
            handoffClock = origin;
            handoffWaitedId = -1;
            handoffPredictionCalls = 0;
            handoffPredictions.clear();
            attach(true);
        }
        void attach(bool enable) {
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            const auto action = enable ? DetourAttach : DetourDetach;
            checkDetour(action(reinterpret_cast<PVOID*>(&originalHandoffTime), fixedHandoffTime));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalHandoffCommit), handoffCommit));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalHandoffPrediction), handoffPrediction));
            checkDetour(DetourTransactionCommit());
        }
        ~HandoffHooks() {
            try {
                attach(false);
            } catch (...) {
                std::_Exit(2);
            }
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void handoff(const wchar_t* backend,
                            const wchar_t* module,
                            unsigned multiplier,
                            unsigned rate,
                            bool fidelityFx,
                            const std::wstring& scenario) {
            const bool split = multiplier == 4 || (fidelityFx && multiplier == 3);
            const bool late = scenario == L"late-tail";
            const bool blockedMarker = scenario == L"blocked-marker-control";
            const bool burst = scenario == L"burst-control";
            const bool futureInput = scenario == L"future-input";
            std::atomic<bool> futureQueued{};
            std::thread producer;
            std::exception_ptr producerFailure;
            OwnedFrameTransaction copyCheck;
            require(!late || split, "late-tail requires a separately composed tail");
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks backendHooks(backend, state);
            HandoffHooks clock;
            Interop hw;
            EarlyOutputGate tail(hw.gpu.device.Get());
            RuntimeInputRegression::seed(runtime);
            runtime.m_ovrSubmissionDevice = hw.device;
            runtime.m_ovrSubmissionContext = hw.context;
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            runtime.m_fgEyeResolution = {eyeWidth, height};
            runtime.m_idealFrameDuration = 1. / rate;
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           multiplier,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            init.useFidelityFxFull = fidelityFx;
            success(runtime.m_frameGeneration->initialize(init), "initialize native early handoff");
            for (unsigned bank = 0; bank < 2; ++bank)
                for (unsigned role = 0; role < 4; ++role) {
                    auto& output = runtime.m_fgPresentationBanks[bank][role];
                    output.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x8000 + bank * 4 + role));
                    output.images.push_back(hw.texture());
                }
            ComPtr<ID3D11Fence> presented, markerGate;
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&presented)), "handoff marker");
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&markerGate)), "marker control");
            HANDLE shared{};
            success(markerGate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared), "export marker gate");
            ComPtr<ID3D12Fence> markerRelease;
            const auto opened = hw.gpu.device->OpenSharedHandle(shared, IID_PPV_ARGS(&markerRelease));
            CloseHandle(shared);
            success(opened, "import marker gate");
            std::mutex phaseMutex;
            std::condition_variable phaseChanged;
            bool continueAfterFirst{};
            const int64_t period = static_cast<int64_t>(1e9 / rate);
            const double nativeFirst = clock.origin + .3;
            // A known test transport lead, not a production timing constant.
            const int64_t first = runtime.ovrTimeToXrTime(nativeFirst + .065);
            handoffPredictionBase = nativeFirst + .065;
            handoffPredictionPeriod = period / 1e9;
            const bool tracing = TraceLoggingProviderEnabled(log::g_traceProvider, 0, 0) != FALSE;
            if (GetEnvironmentVariableW(L"VDXR_FG_TEST_REQUIRE_TRACE", nullptr, 0))
                require(tracing, "requested trace capture must enable the registered provider");
            std::vector<unsigned> roles;
            std::vector<double> ends;
            std::vector<uint64_t> completedBeforeSlot;
            state.observeWait = [&](long long id) {
                if (id == 1) {
                    std::unique_lock lock(phaseMutex);
                    require(phaseChanged.wait_for(lock, 3s, [&] { return continueAfterFirst; }),
                            "bounded first native handoff observation");
                }
                if (futureInput && id == 1) {
                    const auto until = std::chrono::steady_clock::now() + 1s;
                    while (!futureQueued && std::chrono::steady_clock::now() < until)
                        std::this_thread::yield();
                    require(futureQueued, "next producer is admitted after current copies are staged");
                    std::unique_lock lease(runtime.m_fgContextMutex);
                    for (unsigned slot = 0; slot < multiplier; ++slot) {
                        const auto deadline = std::chrono::steady_clock::now() + 500ms;
                        while (!runtime.m_frameGeneration->copyReady(copyCheck, copyCheck.slots.roles[slot]) &&
                               std::chrono::steady_clock::now() < deadline)
                            std::this_thread::sleep_for(1ms);
                        require(runtime.m_frameGeneration->copyReady(copyCheck, copyCheck.slots.roles[slot]),
                                "future producer dependency cannot precede any current role copy");
                    }
                }
                const double now = late && id >= 1    ? runtime.xrTimeToOvrTime(first + period + 1000000)
                                   : burst && id == 1 ? nativeFirst + 4 * (period / 1e9) - .002
                                                      : nativeFirst + id * (period / 1e9) - .002;
                handoffClock = (std::max)(handoffClock.load(), now);
                handoffWaitedId = id;
                if (id > 0 && !futureInput) {
                    const auto until = std::chrono::steady_clock::now() + 500ms;
                    while (presented->GetCompletedValue() < static_cast<uint64_t>(id) &&
                           std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for(1ms);
                    completedBeforeSlot.push_back(presented->GetCompletedValue());
                }
            };
            state.observeLayers = [&](const ovrLayerHeader* const* layers, unsigned count) {
                require(count == 1 && layers[0] && layers[0]->Type == ovrLayerType_EyeFov, "native stereo handoff");
                const auto& projection = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                const auto role =
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(projection.ColorTexture[0]) - 0x8000) % 4;
                roles.push_back(role);
                ends.push_back(ovr_GetTimeInSeconds());
                if (blockedMarker && roles.size() == 1)
                    success(hw.context->Wait(markerGate.Get(), 1), "negative control blocks native GPU handoff");
                success(hw.context->Signal(presented.Get(), roles.size()), "native GPU handoff marker");
                hw.context->Flush();
            };
            auto cleanup = MakeScopeGuard([&] {
                tail.release();
                (void)markerRelease->Signal(1);
                {
                    std::lock_guard lock(phaseMutex);
                    continueAfterFirst = true;
                }
                phaseChanged.notify_all();
                RuntimeInputRegression::stop(runtime, state, &producer);
            });
            FrameGenerationCadence cadence;
            const auto reservation =
                cadence.reserve(runtime.ovrTimeToXrTime(clock.origin), first, period, multiplier, 0);
            FinalProjectionInput input;
            input.serial = 1;
            input.time = first - period;
            input.period = period;
            input.slots =
                cadence.plan(cadence.reserve(0, first - multiplier * period, period, multiplier, 0), 0, false);
            const auto a = scene(0), b = scene(24);
            input.colour[0] = input.colour[1] = hw.texture(&a);
            for (unsigned eye = 0; eye < 2; ++eye) {
                auto& view = input.views[eye];
                view.rect = {eye * eyeWidth, 0, eyeWidth, height};
                view.orientation[3] = 1;
                view.position[0] = eye ? .032f : -.032f;
                view.fov[0] = view.fov[3] = -.8f;
                view.fov[1] = view.fov[2] = .8f;
            }
            OwnedFrameTransaction prime;
            success(runtime.m_frameGeneration->capture(input, prime), "native prime");
            hw.await(*runtime.m_frameGeneration, prime);
            tail.arm(prime.ticket);
            hw.retire(*runtime.m_frameGeneration, prime);
            input.serial = 2;
            input.time = reservation.currentTarget;
            input.slots = cadence.plan(reservation, first - period, true);
            input.colour[0] = input.colour[1] = hw.texture(&b);
            OwnedFrameTransaction pair;
            success(runtime.m_frameGeneration->capture(input, pair), "native split pair");
            require(bool(tail.first) == split, "native host negotiates the intended first/tail split");
            const auto full = static_cast<ID3D12Fence*>(pair.ticket.output_ready_fence);
            const auto fullValue = pair.ticket.output_ready_value;
            copyCheck = pair;
            RuntimeInputRegression::start(runtime);
            state.release();
            runtime.waitForAsyncSubmissionIdle();
            {
                std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                runtime.m_fgReservationForSubmission = reservation;
                runtime.m_fgTransactionForSubmission = std::move(pair);
                runtime.m_layersForAsyncSubmission.resize(1);
                runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                runtime.m_asyncSubmissionReady = false;
                runtime.m_asyncSubmissionCondVar.notify_all();
            }
            const bool firstCpu = state.waitFor([&] { return state.frames.size() >= 1; }, 500ms);
            const auto deadline = std::chrono::steady_clock::now() + 500ms;
            while (presented->GetCompletedValue() < 1 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            const bool firstGpu = presented->GetCompletedValue() >= 1 && presented->GetCompletedValue() != UINT64_MAX;
            const bool fullPending = full->GetCompletedValue() < fullValue;
            const bool earlyRole = firstCpu && roles[0] == 0;
            if (futureInput) {
                if (split) {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    require(!runtime.m_asyncSubmissionReady, "pending tail keeps next-producer admission closed");
                }
                producer = std::thread([&] {
                    try {
                        runtime.waitForFrameGenerationCapacity();
                        std::unique_lock lease(runtime.m_fgContextMutex);
                        success(hw.context->Wait(markerGate.Get(), 1), "queue future producer dependency");
                        hw.context->Flush();
                        futureQueued = true;
                    } catch (...) {
                        producerFailure = std::current_exception();
                    }
                });
            }
            if (!late)
                tail.release();
            else
                handoffClock = runtime.xrTimeToOvrTime(first + period + 1000000);
            {
                std::lock_guard lock(phaseMutex);
                continueAfterFirst = true;
            }
            phaseChanged.notify_all();
            require(firstCpu && firstGpu && earlyRole, "S1 native CPU and GPU handoffs complete before tail release");
            require(!split || fullPending, "native early handoff does not imply whole-group completion");
            require(state.waitFor([&] { return state.frames.size() == multiplier; }), "all native slots complete");
            if (futureInput) {
                require(futureQueued && presented->GetCompletedValue() == 1,
                        "later native GPU markers expose the separate shared-context dependency limit");
                success(markerRelease->Signal(1), "release future producer before completing native markers");
                producer.join();
                if (producerFailure)
                    std::rethrow_exception(producerFailure);
            }
            const auto markerDeadline = std::chrono::steady_clock::now() + 500ms;
            while (presented->GetCompletedValue() < multiplier && std::chrono::steady_clock::now() < markerDeadline)
                std::this_thread::sleep_for(1ms);
            require(presented->GetCompletedValue() >= multiplier && presented->GetCompletedValue() != UINT64_MAX,
                    "every native slot has a completed GPU handoff");
            for (unsigned slot = 0; slot < multiplier; ++slot) {
                const unsigned expected = slot == multiplier - 1 || (late && slot > 0) ? 2
                                          : slot == 0                                  ? 0
                                          : slot == 1                                  ? 1
                                                                                       : 3;
                require(roles[slot] == expected, "every native slot uses the exact intended role");
                if (slot && !late)
                    require(std::abs((ends[slot] - ends[slot - 1]) * 1e9 - period) < 100,
                            "strict native slots cannot bunch or skip a period");
            }
            for (unsigned i = 0; i < completedBeforeSlot.size(); ++i)
                require(completedBeforeSlot[i] >= i + 1 && completedBeforeSlot[i] != UINT64_MAX,
                        "previous GPU handoff completes before advancing to the next native slot");
            tail.release();
            const auto settled = [&] {
                std::unique_lock lock(runtime.m_asyncSubmissionMutex);
                require(runtime.m_asyncSubmissionCondVar.wait_for(
                            lock,
                            2s,
                            [&] {
                                return runtime.m_asyncSubmissionError ||
                                       (!runtime.m_fgActiveSubmission && runtime.m_layersForAsyncSubmission.empty());
                            }),
                        "bounded native group retirement");
                if (runtime.m_asyncSubmissionError)
                    std::rethrow_exception(runtime.m_asyncSubmissionError);
            };
            settled();
            const unsigned groups = scenario == L"early-tail" ? 17 : 1;
            for (unsigned group = 1; group < groups; ++group) {
                const auto previousTime = input.time;
                input.serial = 2 + group;
                input.time += multiplier * period;
                const auto next = cadence.reserve(runtime.ovrTimeToXrTime(handoffClock.load()),
                                                  first + group * multiplier * period,
                                                  period,
                                                  multiplier,
                                                  group * multiplier);
                input.slots = cadence.plan(next, previousTime, true);
                const auto pixels = scene(24 + group * 2);
                input.colour[0] = input.colour[1] = hw.texture(&pixels);
                OwnedFrameTransaction frame;
                {
                    std::unique_lock lease(runtime.m_fgContextMutex);
                    success(runtime.m_frameGeneration->capture(input, frame), "steady native source capture");
                }
                {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    runtime.m_fgReservationForSubmission = next;
                    runtime.m_fgTransactionForSubmission = std::move(frame);
                    runtime.m_layersForAsyncSubmission.resize(1);
                    runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                    runtime.m_asyncSubmissionReady = false;
                    runtime.m_asyncSubmissionCondVar.notify_all();
                }
                require(state.waitFor([&] { return state.frames.size() == (group + 1) * multiplier; }),
                        "every steady native group completes all its slots");
                settled();
            }
            const auto totalSlots = groups * multiplier;
            require(roles.size() == totalSlots && ends.size() == totalSlots,
                    "strict steady window cannot pass with only one complete group");
            for (unsigned i = 0; i < totalSlots; ++i) {
                const auto slot = i % multiplier;
                const unsigned expected = slot == multiplier - 1 || (late && slot > 0) ? 2
                                          : slot == 0                                  ? 0
                                          : slot == 1                                  ? 1
                                                                                       : 3;
                require(roles[i] == expected && state.frames[i].id == i,
                        "entire native window has contiguous IDs and the intended temporal roles");
                if (i && !late)
                    require(std::abs((ends[i] - ends[i - 1]) * 1e9 - period) < 100,
                            "entire steady window excludes long gaps and catch-up bursts");
            }
            const auto lastMarker = std::chrono::steady_clock::now() + 500ms;
            while (presented->GetCompletedValue() < totalSlots && std::chrono::steady_clock::now() < lastMarker)
                std::this_thread::sleep_for(1ms);
            require(presented->GetCompletedValue() >= totalSlots && presented->GetCompletedValue() != UINT64_MAX,
                    "all steady-window GPU handoff markers complete");
            for (unsigned i = 0; i < completedBeforeSlot.size(); ++i)
                require(completedBeforeSlot[i] >= i + 1 && completedBeforeSlot[i] != UINT64_MAX,
                        "steady window never advances past an incomplete prior GPU handoff");
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
            require(handoffPredictionCalls == (tracing ? 2 * totalSlots : 0),
                    "trace-off adds no SDK prediction calls; trace-on samples exactly the same native ID twice");
            if (tracing)
                for (unsigned slot = 0; slot < totalSlots; ++slot) {
                    const auto before = handoffPredictions[2 * slot];
                    const auto after = handoffPredictions[2 * slot + 1];
                    require(before.first == slot && after.first == slot &&
                                std::abs(after.second - before.second - .001) < 1e-8,
                            "native trace captures same-ID phase drift without retiming source slots");
                }
            hw.gpu.noErrors();
            std::cout << "PASS: strict " << multiplier << "x " << rate << "Hz "
                      << (fidelityFx ? "FidelityFX" : "NVIDIA") << " native CPU/GPU handoffs; groups=" << groups
                      << " late=" << late << '\n';
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(TraceLoggingRegister(virtualdesktop_openxr::log::g_traceProvider) == ERROR_SUCCESS,
                "register the standalone fixture's ETW provider");
        const auto traceCleanup =
            MakeScopeGuard([] { TraceLoggingUnregister(virtualdesktop_openxr::log::g_traceProvider); });
        require(argc == 7, "backend, module, multiplier, refresh rate, optical-flow backend and scenario required");
        const auto multiplier = static_cast<unsigned>(std::stoul(argv[3]));
        const auto rate = static_cast<unsigned>(std::stoul(argv[4]));
        require(multiplier >= 2 && multiplier <= 4 && (rate == 120 || rate == 144), "strict native matrix");
        const std::wstring flow = argv[5], scenario = argv[6];
        require(flow == L"nvidia" || flow == L"fidelityfx-full", "known flow backend");
        require(scenario == L"early-tail" || scenario == L"late-tail" || scenario == L"blocked-marker-control" ||
                    scenario == L"burst-control" || scenario == L"future-input",
                "known handoff case");
        virtualdesktop_openxr::RuntimeFrameGenerationRegression::handoff(
            argv[1], argv[2], multiplier, rate, flow == L"fidelityfx-full", scenario);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
