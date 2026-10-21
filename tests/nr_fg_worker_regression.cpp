// Real GPU output through the production native presenter. OVRNull's timing
// and presentation entrypoints are intercepted; no headset is claimed here.
#define wmain retainedAsyncFixtureMain
#include "nonnr_async_regression.cpp"
#undef wmain
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main

namespace {
    std::wstring allocationFault;
    unsigned allocatedChains{}, destroyedChains{}, allocationAttempts{}, injectedAllocationFailures{};
    decltype(&ovr_CreateTextureSwapChainDX) originalAllocation;
    decltype(&ovr_GetTextureSwapChainLength) originalLength;
    decltype(&ovr_GetTextureSwapChainBufferDX) originalBuffer;
    decltype(&ovr_DestroyTextureSwapChain) originalDestroy;
    ovrResult OVR_CDECL failAllocation(ovrSession session,
                                       IUnknown* device,
                                       const ovrTextureSwapChainDesc* desc,
                                       ovrTextureSwapChain* chain) {
        ++allocationAttempts;
        if (allocationFault == L"allocation-create" && allocationAttempts == 3) {
            ++injectedAllocationFailures;
            return ovrError_MemoryAllocationFailure;
        }
        // BackendHooks replaces allocation with IDs for nongraphics fixtures.
        // Use its original trampoline here so length/buffer inspect real chains.
        const auto result = originalCreateChain(session, device, desc, chain);
        if (OVR_SUCCESS(result)) {
            ++allocatedChains;
        }
        return result;
    }
    ovrResult OVR_CDECL failLength(ovrSession session, ovrTextureSwapChain chain, int* count) {
        if (allocatedChains == 3 && allocationFault == L"allocation-length") {
            ++injectedAllocationFailures;
            return ovrError_RuntimeException;
        }
        if (allocatedChains == 3 && allocationFault == L"allocation-empty") {
            ++injectedAllocationFailures;
            *count = 0;
            return ovrSuccess;
        }
        return originalLength(session, chain, count);
    }
    ovrResult OVR_CDECL failBuffer(ovrSession session, ovrTextureSwapChain chain, int index, IID iid, void** image) {
        if (allocatedChains == 3 && allocationFault == L"allocation-buffer") {
            ++injectedAllocationFailures;
            return ovrError_RuntimeException;
        }
        return originalBuffer(session, chain, index, iid, image);
    }
    void OVR_CDECL countDestroy(ovrSession session, ovrTextureSwapChain chain) {
        ++destroyedChains;
        originalDestroyChain(session, chain);
    }
    struct AllocationHooks {
        AllocationHooks(const std::wstring& scenario) {
            allocationFault = scenario;
            allocatedChains = destroyedChains = allocationAttempts = injectedAllocationFailures = 0;
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalAllocation =
                reinterpret_cast<decltype(originalAllocation)>(GetProcAddress(module, "ovr_CreateTextureSwapChainDX"));
            originalLength =
                reinterpret_cast<decltype(originalLength)>(GetProcAddress(module, "ovr_GetTextureSwapChainLength"));
            originalBuffer =
                reinterpret_cast<decltype(originalBuffer)>(GetProcAddress(module, "ovr_GetTextureSwapChainBufferDX"));
            originalDestroy =
                reinterpret_cast<decltype(originalDestroy)>(GetProcAddress(module, "ovr_DestroyTextureSwapChain"));
            require(originalAllocation && originalLength && originalBuffer && originalDestroy,
                    "allocation exports required");
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalAllocation), failAllocation));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalLength), failLength));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalBuffer), failBuffer));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalDestroy), countDestroy));
            checkDetour(DetourTransactionCommit());
        }
        ~AllocationHooks() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalAllocation), failAllocation) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalLength), failLength) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalBuffer), failBuffer) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDestroy), countDestroy) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
        }
    };
    decltype(&ovr_CommitTextureSwapChain) originalFgCommit;
    ovrResult OVR_CDECL fakeCommit(ovrSession, ovrTextureSwapChain) {
        return ovrSuccess;
    }
    struct CommitHook {
        CommitHook() {
            originalFgCommit = reinterpret_cast<decltype(originalFgCommit)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_CommitTextureSwapChain"));
            require(originalFgCommit != nullptr, "commit export required");
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalFgCommit), fakeCommit));
            checkDetour(DetourTransactionCommit());
        }
        ~CommitHook() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalFgCommit), fakeCommit) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
        }
    };

    decltype(&ovr_GetPredictedDisplayTime) originalHistoryPrediction;
    decltype(&ovr_ClearShouldRecenterFlag) originalHistoryClear;
    double historyClockBase;
    double OVR_CDECL historyPrediction(ovrSession, long long id) {
        return historyClockBase + id / 90.;
    }
    void OVR_CDECL historyClear(ovrSession) {
        std::lock_guard lock(backendState->mutex);
        backendState->recenter = false;
        ++backendState->recenterClears;
    }
    struct HistoryHooks {
        HistoryHooks() {
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalHistoryPrediction = reinterpret_cast<decltype(originalHistoryPrediction)>(
                GetProcAddress(module, "ovr_GetPredictedDisplayTime"));
            originalHistoryClear =
                reinterpret_cast<decltype(originalHistoryClear)>(GetProcAddress(module, "ovr_ClearShouldRecenterFlag"));
            require(originalHistoryPrediction && originalHistoryClear, "history clock/status exports required");
            historyClockBase = ovr_GetTimeInSeconds() + .01;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalHistoryPrediction), historyPrediction));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalHistoryClear), historyClear));
            checkDetour(DetourTransactionCommit());
        }
        ~HistoryHooks() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalHistoryPrediction), historyPrediction) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalHistoryClear), historyClear) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void allocation(const wchar_t* backend, const std::wstring& scenario) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            Interop hw;
            ovrGraphicsLuid luid{};
            require(OVR_SUCCESS(ovr_Create(&runtime.m_ovrSession, &luid)), "allocation fixture owns a valid session");
            RuntimeInputRegression::seed(runtime);
            runtime.m_ovrSubmissionDevice = hw.device;
            runtime.m_ovrSubmissionContext = hw.context;
            runtime.m_frameGenerationMode = 3;
            runtime.m_fgDesiredMultiplier = 3;
            runtime.m_precompositor.dlssnrSettings.enabled = true;
            runtime.m_precompositor.dlssnrSettings.foveationSize = 1.f;
            runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = hw.texture();
            runtime.m_fgBegunReservation.valid = true;
            runtime.m_fgBegunReservation.shouldRender = true;
            runtime.m_fgBegunReservation.currentTarget = 1000000000;
            runtime.m_layersForFrame.resize(1);
            auto& projection = runtime.m_layersForFrame[0].EyeFov;
            projection.Header.Type = ovrLayerType_EyeFov;
            projection.Viewport[0].Size = projection.Viewport[1].Size = {eyeWidth, height};
            XrCompositionLayerProjection xrProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            OpenXrRuntime::Swapchain colourInputs[2]{};
            XrCompositionLayerProjectionView views[2]{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&colourInputs[eye]);
            }
            xrProjection.viewCount = 2;
            xrProjection.views = views;
            const XrCompositionLayerBaseHeader* layers[]{
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&xrProjection)};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = 1000000000;
            end.layerCount = 1;
            end.layers = layers;
            AllocationHooks failures(scenario);
            auto cleanup = MakeScopeGuard([&] { runtime.cleanupSessionResources(); });
            bool accepted{};
            std::exception_ptr failure;
            try {
                accepted = runtime.prepareFrameGeneration(end);
            } catch (...) {
                failure = std::current_exception();
            }
            require(injectedAllocationFailures == 1,
                    "the intended failure fired after earlier real chains were allocated");
            require(!failure && !accepted, "optional allocation failure preserves the original NR submission");
            require(runtime.m_frameGenerationUnavailable && !runtime.m_frameGeneration &&
                        runtime.m_fgDesiredMultiplier.load() == 1 && !runtime.m_fgTransactionForFrame.ticket.token,
                    "failed optional allocation disables only FG without accepting GPU work");
            require(allocatedChains >= 2 && allocatedChains == destroyedChains,
                    "partial presentation roles are all rolled back");
            const auto attempts = allocationAttempts;
            require(!runtime.prepareFrameGeneration(end) && attempts == allocationAttempts,
                    "failed optional allocation is not retried every frame");
            require(runtime.m_precompositor.dlssnrSettings.enabled &&
                        runtime.m_precompositor.dlssnrSettings.foveationSize == 1.f &&
                        runtime.m_precompositor.finalColour[0] && runtime.m_layersForFrame.size() == 1,
                    "fallback retains final NR pixels, coverage and layer");
            runtime.cleanupSessionResources();
            cleanup.Deactivate();
            hw.gpu.noErrors();
        }
        static void startup(const wchar_t* backend, const std::wstring& scenario) {
            OpenXrRuntime runtime;
            BackendState state;
            state.fault = scenario == L"startup-timeout" ? Fault::Timeout : Fault::NotInitialized;
            state.recoverAfterFaults = scenario == L"startup-retry" ? 3 : 0;
            BackendHooks hooks(backend, state);
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = 3;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            RuntimeInputRegression::start(runtime);
            state.release();
            if (scenario == L"startup-stop") {
                require(state.waitFor([&] { return state.faultCalls >= 2; }),
                        "persistent uninitialized backend retries before any Begin");
            } else {
                runtime.waitForFrameGenerationCapacity();
                std::lock_guard lock(state.mutex);
                require(state.beginCalls == 1 && state.faultCalls == (scenario == L"startup-retry" ? 3u : 1u),
                        "recoverable native Wait outcomes preserve baseline startup behavior");
                for (const auto id : state.waitedIds) {
                    require(id == 0, "pre-Begin retries do not advance the compositor ID");
                }
            }
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
            require(!runtime.m_asyncSubmissionError && !runtime.m_asyncSubmissionThread.joinable(),
                    "stopping a retry does not turn it into a permanent submission failure");
        }
        static void statusSnapshots(const wchar_t* backend) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            HistoryHooks history;
            ovrGraphicsLuid luid{};
            require(OVR_SUCCESS(ovr_Create(&runtime.m_ovrSession, &luid)), "status fixture owns a valid Null session");
            historyClockBase = ovr_GetTimeInSeconds() + 1.;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameCompleted = runtime.m_frameBegun;
            runtime.m_isHeadless = false;
            runtime.m_frameGenerationMode = 3;
            runtime.m_idealFrameDuration = runtime.m_predictedFrameDuration = 1. / 90.;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            RuntimeInputRegression::start(runtime);
            state.release();
            runtime.waitForFrameGenerationCapacity();
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            const auto wait = [&] {
                require(runtime.xrWaitFrame(reinterpret_cast<XrSession>(1), nullptr, &frame) == XR_SUCCESS,
                        "public Wait succeeds with controlled clock and status");
                require(runtime.m_fgWaitedReservation.valid, "status snapshot has a valid prediction");
                return runtime.m_fgWaitedReservation;
            };
            const auto begin = [&] {
                // No graphics are submitted in this status-only fixture. Account
                // for completion of the old frame before calling public Begin.
                runtime.m_frameCompleted = runtime.m_frameBegun;
                require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS,
                        "public Begin copies its waited snapshot");
            };
            const auto initial = wait();
            begin();
            {
                std::lock_guard lock(state.mutex);
                state.recenter = true;
            }
            const auto recentered = wait();
            require(recentered.historyEpoch > initial.historyEpoch &&
                        runtime.m_fgBegunReservation.historyEpoch == initial.historyEpoch,
                    "pipelined next Wait preserves the previous begun epoch");
            begin();
            require(runtime.m_fgBegunReservation.historyEpoch == recentered.historyEpoch && runtime.m_shouldRecenter,
                    "recenter epoch belongs to the next begun frame while app events are pending");
            {
                std::lock_guard lock(state.mutex);
                require(state.recenterClears == 1, "first recenter flag acknowledged");
                state.recenter = true;
            }
            const auto secondRecenter = wait();
            require(secondRecenter.historyEpoch > recentered.historyEpoch,
                    "new backend recenter invalidates history even with earlier app events pending");
            begin();
            const auto stillAsserted = wait();
            require(stillAsserted.historyEpoch == secondRecenter.historyEpoch,
                    "one pending recenter signal does not repeatedly invalidate history");
            begin();
            {
                std::lock_guard lock(state.mutex);
                state.visible = false;
            }
            const auto hidden = wait();
            require(!hidden.shouldRender && hidden.historyEpoch > stillAsserted.historyEpoch,
                    "hidden Wait records the visibility discontinuity");
            begin();
            {
                std::lock_guard lock(state.mutex);
                state.visible = true;
            }
            const auto visibleAgain = wait();
            require(visibleAgain.shouldRender && visibleAgain.historyEpoch > hidden.historyEpoch &&
                        !runtime.m_fgBegunReservation.shouldRender,
                    "visible again re-primes even when hidden frame had no capture");
            begin();
            require(runtime.m_fgBegunReservation.historyEpoch == visibleAgain.historyEpoch &&
                        runtime.m_fgBegunReservation.shouldRender,
                    "public Begin retains render eligibility and epoch from its own Wait");
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
        static void history(const wchar_t* backend, const wchar_t* module, const std::wstring& scenario) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_isHeadless = false;
            runtime.m_hmdStatus.IsVisible = true;
            runtime.m_frameGenerationMode = 3;
            runtime.m_fgEyeResolution = {eyeWidth, height};
            runtime.m_fgFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           3,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize finalized-color history control");
            auto cleanup = MakeScopeGuard([&] { runtime.cleanupSessionResources(); });
            runtime.m_precompositor.dlssnrSettings.enabled = true;
            runtime.m_precompositor.dlssnrSettings.foveationSize = 1.f;
            runtime.m_layersForFrame.resize(1);
            auto& projection = runtime.m_layersForFrame[0].EyeFov;
            projection.Header.Type = ovrLayerType_EyeFov;
            for (unsigned eye = 0; eye < 2; ++eye) {
                projection.Viewport[eye] = {{static_cast<int>(eye * eyeWidth), 0}, {eyeWidth, height}};
                projection.RenderPose[eye].Orientation.w = 1.f;
                projection.RenderPose[eye].Position.x = eye ? .032f : -.032f;
                projection.Fov[eye] = {1.f, 1.f, 1.f, 1.f};
            }
            XrCompositionLayerProjection xrProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            OpenXrRuntime::Swapchain colourInputs[2]{};
            XrCompositionLayerProjectionView views[2]{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&colourInputs[eye]);
            }
            xrProjection.viewCount = 2;
            xrProjection.views = views;
            xrProjection.space = reinterpret_cast<XrSpace>(uintptr_t(0x100));
            const XrCompositionLayerBaseHeader* layers[]{
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&xrProjection)};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.layerCount = 1;
            end.layers = layers;
            constexpr int64_t period = 10000000;
            uint64_t epoch = 1;
            OwnedFrameTransaction held;
            const auto capture = [&](unsigned frame, bool expectedPrime, bool hold = false) {
                runtime.m_frameBegun = frame;
                end.displayTime = 1000000000 + frame * 3 * period;
                runtime.m_fgBegunReservation = runtime.m_frameGenerationCadence.reserve(
                    end.displayTime - 3 * period, end.displayTime - 2 * period, period, 3, frame * 3);
                runtime.m_fgBegunReservation.historyEpoch = epoch;
                const auto pixels = scene(frame * 8);
                runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = hw.texture(&pixels);
                require(runtime.prepareFrameGeneration(end), "eligible finalized input remains on FG path");
                auto& transaction = runtime.m_fgTransactionForFrame;
                hw.await(*runtime.m_frameGeneration, transaction);
                // Settle first so a failed assertion cannot leave this fixture owning live GPU work.
                const bool prime = transaction.ticket.prime != 0;
                if (hold) {
                    held = std::move(transaction);
                    transaction = {};
                } else {
                    hw.retire(*runtime.m_frameGeneration, transaction);
                }
                require(prime == expectedPrime,
                        "history discontinuity primes once; unchanged endpoints form a real synthesized pair");
            };
            capture(1, true);
            if (scenario == L"history-pipelined") {
                // The next Wait observes recentering before this frame's End.
                runtime.m_recenterTime = end.displayTime + 1;
                runtime.m_fgHistoryEpoch = epoch + 1;
            }
            capture(2, false, scenario == L"history-busy");
            if (scenario == L"history-busy") {
                projection.Fov[0].LeftTan = .9f;
                runtime.m_frameBegun = 3;
                end.displayTime += 3 * period;
                runtime.m_fgBegunReservation = runtime.m_frameGenerationCadence.reserve(
                    end.displayTime - 3 * period, end.displayTime - 2 * period, period, 3, 9);
                runtime.m_fgBegunReservation.historyEpoch = epoch;
                const auto token = held.ticket.token;
                require(!runtime.prepareFrameGeneration(end) && !runtime.m_fgTransactionForFrame.ticket.token &&
                            held.ticket.token == token && runtime.m_frameGeneration->ready(held),
                        "metadata change bypasses rather than reset or overwrite a live output");
                require(runtime.m_fgDesiredMultiplier.load() == 3,
                        "busy metadata transition preserves the requested recovery cadence");
                hw.retire(*runtime.m_frameGeneration, held);
                capture(4, true);
                capture(5, false);
                runtime.cleanupSessionResources();
                cleanup.Deactivate();
                hw.gpu.noErrors();
                return;
            }
            if (scenario == L"history-recenter" || scenario == L"history-pipelined") {
                runtime.m_recenterTime = end.displayTime + 1;
                ++epoch;
            } else if (scenario == L"history-fov") {
                projection.Fov[0].LeftTan = projection.Fov[1].LeftTan = .9f;
            } else if (scenario == L"history-space") {
                xrProjection.space = reinterpret_cast<XrSpace>(uintptr_t(0x200));
            } else if (scenario == L"history-sharpen") {
                runtime.m_precompositor.sharpenFactor = .5f;
            } else {
                require(scenario == L"history-visibility" || scenario == L"history-visibility-gap",
                        "known history discontinuity required");
                runtime.m_hmdStatus.IsVisible = false;
                runtime.m_fgBegunReservation.shouldRender = false;
                if (scenario == L"history-visibility") {
                    require(!runtime.prepareFrameGeneration(end) && !runtime.m_fgTransactionForFrame.ticket.token &&
                                runtime.m_fgDesiredMultiplier.load() == 1,
                            "hidden session bypasses synthesis without changing finalized color");
                }
                runtime.m_hmdStatus.IsVisible = true;
                epoch += 2;
            }
            capture(3, true);
            capture(4, false);
            runtime.cleanupSessionResources();
            cleanup.Deactivate();
            hw.gpu.noErrors();
        }
        static void cleanupProducer(const wchar_t* backend, const wchar_t* module) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           3,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize pending-producer cleanup host");
            FinalProjectionInput input;
            FrameGenerationCadence cadence;
            input.serial = 1;
            input.period = 10000000;
            input.time = 1000000000;
            input.slots = cadence.plan(
                cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 0), 0, false);
            const auto pixels = scene(0);
            input.colour[0] = input.colour[1] = hw.texture(&pixels);
            for (unsigned eye = 0; eye < 2; ++eye) {
                input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
                input.views[eye].orientation[3] = 1.f;
                input.views[eye].position[0] = eye ? .032f : -.032f;
                input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
                input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
            }
            OwnedFrameTransaction prime;
            success(runtime.m_frameGeneration->capture(input, prime), "seed duplicate-serial rejection control");
            hw.await(*runtime.m_frameGeneration, prime);
            hw.retire(*runtime.m_frameGeneration, prime);
            ComPtr<ID3D11Fence> gate;
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "pending producer gate");
            HANDLE handle{};
            success(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "export producer gate");
            ComPtr<ID3D12Fence> release;
            const auto opened = hw.gpu.device->OpenSharedHandle(handle, IID_PPV_ARGS(&release));
            CloseHandle(handle);
            success(opened, "import producer release fence");
            input.time += 3 * input.period;
            input.slots =
                cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 3),
                             input.time - 30000000,
                             true);
            success(hw.context->Wait(gate.Get(), 1), "park host producer before rejected duplicate");
            OwnedFrameTransaction rejected;
            const auto result = runtime.m_frameGeneration->capture(input, rejected);
            const auto shutdown = runtime.m_frameGeneration->shutdown();
            std::atomic<HRESULT> releaseResult{E_FAIL};
            std::thread releaser([&] {
                std::this_thread::sleep_for(75ms);
                releaseResult = release->Signal(1);
            });
            auto join = MakeScopeGuard([&] { releaser.join(); });
            require(result == VDXRFG_E_REJECTED && !rejected.ticket.token && shutdown == VDXRFG_E_BUSY,
                    "rejected admission leaves actual producer work pending without a module ticket");
            const auto start = std::chrono::steady_clock::now();
            runtime.cleanupFrameGeneration();
            const auto elapsed = std::chrono::steady_clock::now() - start;
            releaser.join();
            join.Deactivate();
            success(releaseResult, "release pending producer");
            require(!runtime.m_frameGeneration && elapsed >= 50ms && elapsed < 2s,
                    "cleanup retries BUSY producer completion before releasing graph");
            runtime.cleanupSessionResources();
            hw.gpu.noErrors();
        }
        static void bypass(const std::wstring& scenario) {
            OpenXrRuntime runtime;
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = 3;
            runtime.m_fgDesiredMultiplier = 3;
            runtime.m_precompositor.dlssnrSettings.enabled = true;
            runtime.m_precompositor.dlssnrSettings.foveationSize = 1.f;
            runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = hw.texture();
            runtime.m_fgBegunReservation.valid = true;
            runtime.m_fgBegunReservation.currentTarget = 1000000000;
            runtime.m_layersForFrame.resize(1);
            runtime.m_layersForFrame[0].Header.Type = ovrLayerType_EyeFov;
            runtime.m_fgBegunReservation.shouldRender = true;
            runtime.m_layersForFrame[0].EyeFov.Viewport[0].Size = {eyeWidth, height};
            runtime.m_layersForFrame[0].EyeFov.Viewport[1].Size = {eyeWidth, height};
            if (scenario == L"unequal-eyes") {
                runtime.m_layersForFrame[0].EyeFov.Viewport[1].Size.w -= 4;
            } else if (scenario == L"bgrx") {
                D3D11_TEXTURE2D_DESC desc{};
                runtime.m_precompositor.finalColour[0]->GetDesc(&desc);
                desc.Format = DXGI_FORMAT_B8G8R8X8_UNORM;
                ComPtr<ID3D11Texture2D> bgra;
                success(hw.device->CreateTexture2D(&desc, nullptr, &bgra), "unsupported final BGRX control");
                runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = bgra;
            }
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = 1000000000;
            end.layerCount = 1;
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            OpenXrRuntime::Swapchain colourInputs[2]{};
            XrCompositionLayerProjectionView views[2]{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&colourInputs[eye]);
            }
            projection.viewCount = 2;
            projection.views = views;
            if (scenario == L"static" || scenario == L"protected") {
                colourInputs[1].xrDesc.createFlags = scenario == L"static" ? XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT
                                                                           : XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
            } else if (scenario == L"nr-off") {
                runtime.m_precompositor.dlssnrSettings.enabled = false;
            } else if (scenario == L"low-coverage") {
                runtime.m_precompositor.dlssnrSettings.foveationSize = .89f;
            } else if (scenario == L"app-submission") {
                runtime.m_useApplicationDeviceForSubmission = true;
            } else if (scenario == L"synchronous") {
                runtime.m_useAsyncSubmission = false;
            } else if (scenario == L"depth") {
                runtime.m_layersForFrame[0].Header.Type = ovrLayerType_EyeFovDepth;
            } else if (scenario == L"overlay" || scenario == L"second-projection") {
                runtime.m_layersForFrame.resize(2);
                runtime.m_layersForFrame[1].Header.Type =
                    scenario == L"overlay" ? ovrLayerType_Quad : ovrLayerType_EyeFov;
                end.layerCount = 2;
            }
            const XrCompositionLayerBaseHeader* layers[]{
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
            end.layers = layers;
            require(!runtime.prepareFrameGeneration(end) && !runtime.m_frameGeneration,
                    "unsupported final composition bypasses before loading a module");
            require(runtime.m_fgDesiredMultiplier.load() == 1,
                    "unsupported format or eye extents restore native application cadence");
            runtime.cleanupSessionResources();
        }
        static void run(const wchar_t* backend,
                        const wchar_t* module,
                        unsigned multiplier,
                        bool late,
                        bool capacity,
                        bool failedPending,
                        const std::wstring& scenario) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            CommitHook commits;
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            runtime.m_fgEyeResolution = {eyeWidth, height};
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           multiplier,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize presenter GPU module");
            std::map<ovrTextureSwapChain, ComPtr<ID3D11Texture2D>> textures;
            for (unsigned bank = 0; bank < 2; ++bank) {
                for (unsigned role = 0; role < 3; ++role) {
                    if (role == 1 && multiplier == 2) {
                        continue;
                    }
                    auto& slice = runtime.m_fgPresentationBanks[bank][role];
                    slice.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6000 + bank * 3 + role));
                    slice.images.push_back(hw.texture());
                    textures[slice.ovrSwapchain] = slice.images[0];
                }
            }
            std::vector<std::vector<uint8_t>> frames;
            std::vector<ovrTextureSwapChain> chains;
            state.observeLayers = [&](const ovrLayerHeader* const* layers, unsigned count) {
                require(count == 1 && layers[0] && layers[0]->Type == ovrLayerType_EyeFov,
                        "each generated slot presents a stereo color projection");
                const auto& eye = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                require(eye.ColorTexture[0] == eye.ColorTexture[1] && eye.Viewport[0].Pos.x == 0 &&
                            eye.Viewport[1].Pos.x == eyeWidth && eye.Viewport[0].Size.w == eyeWidth,
                        "both eyes use distinct rectangles of an owned output");
                frames.push_back(hw.read(textures.at(eye.ColorTexture[0]).Get()));
                chains.push_back(eye.ColorTexture[0]);
            };
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            RuntimeInputRegression::start(runtime);
            state.release();
            runtime.waitForAsyncSubmissionIdle();
            const auto a = scene(0), b = scene(24);
            FrameGenerationCadence cadence;
            constexpr int64_t period = 10000000;
            const auto first = runtime.ovrTimeToXrTime(ovr_GetTimeInSeconds()) + 20000000;
            int64_t previous{};
            for (unsigned group = 0; group < 2; ++group) {
                runtime.waitForAsyncSubmissionIdle();
                const auto reservation = cadence.reserve(
                    first - period, first + group * multiplier * period, period, multiplier, group * multiplier);
                FinalProjectionInput input;
                input.serial = group + 1;
                input.time = reservation.currentTarget;
                input.period = period;
                input.slots = cadence.plan(reservation, previous, group != 0);
                input.colour[0] = input.colour[1] = hw.texture(group ? &b : &a);
                for (unsigned eye = 0; eye < 2; ++eye) {
                    input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
                    input.views[eye].orientation[3] = 1;
                    input.views[eye].position[0] = eye ? .032f : -.032f;
                    input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
                    input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
                }
                OwnedFrameTransaction transaction;
                success(runtime.m_frameGeneration->capture(input, transaction), "capture next presenter group");
                hw.await(*runtime.m_frameGeneration, transaction);
                OwnedFrameTransaction pending;
                PredictionReservation pendingReservation;
                const auto activeToken = transaction.ticket.token;
                if (failedPending) {
                    pendingReservation =
                        cadence.reserve(first - period, first + multiplier * period, period, multiplier, multiplier);
                    auto nextInput = input;
                    nextInput.serial = 2;
                    nextInput.time = pendingReservation.currentTarget;
                    nextInput.slots = cadence.plan(pendingReservation, input.time, true);
                    nextInput.colour[0] = nextInput.colour[1] = hw.texture(&b);
                    success(runtime.m_frameGeneration->capture(nextInput, pending),
                            "retain second bank before failure");
                    hw.await(*runtime.m_frameGeneration, pending);
                    std::lock_guard lock(state.mutex);
                    state.gateEnd = true;
                    state.releaseEnd = false;
                    state.fault = Fault::End;
                }
                if (late && group) {
                    for (auto& target : transaction.slots.targets) {
                        target = 1;
                    }
                }
                const bool injectedFailure = group && scenario.rfind(L"fail-", 0) == 0;
                const unsigned failedSlot = scenario.find(L"s1") != std::wstring::npos   ? 0
                                            : scenario.find(L"s2") != std::wstring::npos ? 1
                                                                                         : 2;
                if (injectedFailure) {
                    std::lock_guard lock(state.mutex);
                    state.faultFrameId = multiplier + failedSlot;
                    state.fault = scenario.find(L"wait") != std::wstring::npos    ? Fault::Wait
                                  : scenario.find(L"begin") != std::wstring::npos ? Fault::Begin
                                                                                  : Fault::End;
                }
                if (capacity && !group) {
                    std::lock_guard lock(state.mutex);
                    state.gateEnd = true;
                    state.releaseEnd = false;
                }
                if (group && scenario == L"stop-before-begin") {
                    state.observeWait = [&](long long id) {
                        if (id == static_cast<long long>(multiplier)) {
                            std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                            runtime.m_terminateAsyncThread = true;
                            runtime.m_asyncSubmissionCondVar.notify_all();
                        }
                    };
                }
                {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    runtime.m_fgReservationForSubmission = reservation;
                    runtime.m_fgTransactionForSubmission = std::move(transaction);
                    runtime.m_layersForAsyncSubmission.resize(1);
                    runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                    runtime.m_asyncSubmissionReady = false;
                    runtime.m_asyncSubmissionCondVar.notify_all();
                }
                if (group && scenario == L"stop-before-begin") {
                    require(state.waitFor([&] { return state.waitCalls > multiplier; }),
                            "termination reaches first native timing wait after current copies are staged");
                    RuntimeInputRegression::stop(runtime, state);
                    cleanup.Deactivate();
                    require(!runtime.m_asyncSubmissionError && !runtime.m_frameGeneration &&
                                !runtime.m_fgFailedTransaction.ticket.token && state.frames.size() == multiplier,
                            "staged ticket is retired without an extra End or submission fault on stop");
                    hw.gpu.noErrors();
                    return;
                }
                if (failedPending) {
                    require(state.waitFor([&] { return state.enteredEnd == 1; }), "failed-active gate reached");
                    const auto pendingToken = pending.ticket.token;
                    {
                        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_asyncSubmissionReady && runtime.m_fgActiveOwnedPixels,
                                "owned active group permits one pending payload");
                        runtime.m_fgReservationForSubmission = pendingReservation;
                        runtime.m_fgTransactionForSubmission = std::move(pending);
                        runtime.m_layersForAsyncSubmission.resize(1);
                        runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                        runtime.m_asyncSubmissionReady = false;
                        runtime.m_asyncSubmissionCondVar.notify_all();
                    }
                    state.release();
                    {
                        std::unique_lock lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_asyncSubmissionCondVar.wait_for(
                                    lock, 2s, [&] { return runtime.m_asyncSubmissionError != nullptr; }),
                                "native End failure wakes producers");
                        require(runtime.m_fgFailedTransaction.ticket.token == activeToken &&
                                    runtime.m_fgTransactionForSubmission.ticket.token == pendingToken,
                                "failed active cannot overwrite pending bank retirement identity");
                    }
                    RuntimeInputRegression::stop(runtime, state);
                    cleanup.Deactivate();
                    require(!runtime.m_frameGeneration && !runtime.m_fgFailedTransaction.ticket.token &&
                                !runtime.m_fgTransactionForSubmission.ticket.token,
                            "joined failure cleanup settles both retained transactions before unloading");
                    hw.gpu.noErrors();
                    return;
                }
                if (capacity && !group) {
                    require(state.waitFor([&] { return state.enteredEnd == 1; }), "active group reached parked End");
                    std::atomic<bool> admitted{};
                    std::thread producer([&] {
                        runtime.waitForFrameGenerationCapacity();
                        admitted = true;
                    });
                    const auto deadline = std::chrono::steady_clock::now() + 200ms;
                    while (!admitted && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(1ms);
                    }
                    const bool overlapAllowed = admitted;
                    state.release();
                    producer.join();
                    require(overlapAllowed, "owned active pixels free one pending mailbox before native End completes");
                }
                if (injectedFailure) {
                    require(state.waitFor([&] { return state.faultCalls == 1; }),
                            "fault reaches its intended native slot");
                    bool failed{};
                    try {
                        runtime.waitForAsyncSubmissionIdle();
                    } catch (const std::exception&) {
                        failed = true;
                    }
                    require(failed, "partial presentation stops the producer with an explicit runtime error");
                    {
                        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_fgFailedTransaction.ticket.token == activeToken,
                                "failed synthetic group retains its ticket until joined cleanup");
                    }
                    {
                        std::lock_guard lock(state.mutex);
                        require(state.frames.size() == multiplier + failedSlot + (state.fault == Fault::End),
                                "failure does not replay earlier synthetic slots or present later slots");
                        for (unsigned i = 0; i < state.frames.size(); ++i) {
                            require(state.frames[i].id == i, "failed cycle does not reuse a compositor ID");
                        }
                        if (failedSlot) {
                            require(frames[multiplier] != a && frames[multiplier] != b,
                                    "first synthetic slot completed before the injected later-slot failure");
                        }
                    }
                    RuntimeInputRegression::stop(runtime, state);
                    cleanup.Deactivate();
                    hw.gpu.noErrors();
                    return;
                }
                require(state.waitFor([&] { return state.frames.size() == (group + 1) * multiplier; }, 10s),
                        "bounded native presentation group");
                previous = input.time;
            }
            runtime.waitForAsyncSubmissionIdle();
            {
                std::lock_guard lock(state.mutex);
                require(frames.size() == 2 * multiplier, "prime and pair use native slot counts");
                for (unsigned i = 0; i < frames.size(); ++i) {
                    require(state.frames[i].id == i && state.waitedIds[i] == i && state.begunIds[i] == i,
                            "native Wait/Begin/End IDs are independent, consecutive and monotonic");
                    if (i < multiplier) {
                        require(frames[i] == a, "prime padding repeats exact real input");
                    }
                }
                require(frames.back() == b, "native B output is bit-exact");
                if (late) {
                    for (unsigned i = multiplier; i < frames.size(); ++i) {
                        require(frames[i] == b, "late group uses real B, never retimed synthetic roles");
                    }
                    require(runtime.m_fgNeedReprime.load(), "late group invalidates temporal pairing");
                } else {
                    require(frames[multiplier] != a && frames[multiplier] != b && chains[multiplier] != chains.back(),
                            "presented S1 is distinct and committed on its own chain");
                    if (multiplier == 3) {
                        require(frames[3] != frames[4] && chains[3] != chains[4] && chains[4] != chains[5],
                                "presented S1/S2/B use distinct pixels and separate chains");
                    }
                }
            }
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
            hw.gpu.noErrors();
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(argc == 5, "backend, module, multiplier and normal/late required");
        const unsigned multiplier = std::wcstoul(argv[3], nullptr, 10);
        require(multiplier == 2 || multiplier == 3, "2X or 3X required");
        const std::wstring scenario = argv[4];
        if (scenario == L"bgrx" || scenario == L"unequal-eyes" || scenario == L"static" || scenario == L"protected" ||
            scenario == L"nr-off" || scenario == L"low-coverage" || scenario == L"app-submission" ||
            scenario == L"synchronous" || scenario == L"depth" || scenario == L"overlay" ||
            scenario == L"second-projection") {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::bypass(scenario);
        } else if (scenario.rfind(L"allocation-", 0) == 0) {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::allocation(argv[1], scenario);
        } else if (scenario == L"cleanup-producer") {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::cleanupProducer(argv[1], argv[2]);
        } else if (scenario == L"status-snapshots") {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::statusSnapshots(argv[1]);
        } else if (scenario.rfind(L"startup-", 0) == 0) {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::startup(argv[1], scenario);
        } else if (scenario.rfind(L"history-", 0) == 0) {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::history(argv[1], argv[2], scenario);
        } else {
            require(scenario == L"normal" || scenario == L"late" || scenario == L"capacity" ||
                        scenario == L"stop-before-begin" || scenario == L"failed-pending" ||
                        scenario == L"fail-wait-s1" || scenario == L"fail-wait-s2" || scenario == L"fail-wait-b" ||
                        scenario == L"fail-begin-s1" || scenario == L"fail-begin-s2" || scenario == L"fail-begin-b" ||
                        scenario == L"fail-end-s1" || scenario == L"fail-end-s2" || scenario == L"fail-end-b",
                    "unknown worker scenario");
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::run(argv[1],
                                                                         argv[2],
                                                                         multiplier,
                                                                         scenario == L"late",
                                                                         scenario == L"capacity",
                                                                         scenario == L"failed-pending",
                                                                         scenario);
        }
        std::cout << "PASS: real GPU native presenter roles, retained pixels, IDs and late bypass\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
