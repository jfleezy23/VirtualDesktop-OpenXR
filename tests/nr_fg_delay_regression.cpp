// Real GPU deadline regressions. Gates delay synthesis or a subsequent NR-style
// dependency without a blocking Map/readback in the presentation observer.
#define wmain retainedAsyncFixtureMain
#include "nonnr_async_regression.cpp"
#undef wmain
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main

namespace {
    decltype(VDXRFG_ApiV1::Submit) originalDelayedSubmit;
    ID3D12Fence* synthesisGate;
    ID3D12Fence* compositeGate;
    ID3D12CommandQueue* dependencyQueue;
    bool delaySubmission{};
    HRESULT WINAPI delayedSubmit(void* context, const VDXRFG_SubmitInfoV1* input, VDXRFG_TicketV1* ticket) noexcept {
        auto gated = *input;
        if (delaySubmission) {
            // Preserve the real input-copy dependency while adding a separate
            // synthesis delay. A wall-clock sleep cannot prove input readiness.
            HRESULT hr = S_OK;
            if (input->producer_fence) {
                hr = dependencyQueue->Wait(static_cast<ID3D12Fence*>(input->producer_fence), input->producer_value);
            }
            if (SUCCEEDED(hr)) {
                hr = dependencyQueue->Wait(synthesisGate, 1);
            }
            if (SUCCEEDED(hr)) {
                hr = dependencyQueue->Signal(compositeGate, 1);
            }
            if (FAILED(hr)) {
                *ticket = {};
                return hr;
            }
            gated.producer_fence = compositeGate;
            gated.producer_value = 1;
        }
        return originalDelayedSubmit(context, &gated, ticket);
    }
    struct SubmitHook {
        ComPtr<ID3D12Fence> composite;
        SubmitHook(Gpu& gpu, ID3D12Fence* gate) {
            success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&composite)), "composite gate");
            const auto module = GetModuleHandleW(L"vdxr-frame-generation.dll");
            auto getApi = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
            require(getApi != nullptr, "FG API export required");
            VDXRFG_ApiV1 api{};
            api.struct_size = sizeof(api);
            api.api_version = VDXRFG_API_VERSION;
            success(getApi(&api), "get real module API");
            originalDelayedSubmit = api.Submit;
            synthesisGate = gate;
            compositeGate = composite.Get();
            dependencyQueue = gpu.queue.Get();
            delaySubmission = false;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalDelayedSubmit), delayedSubmit));
            checkDetour(DetourTransactionCommit());
        }
        ~SubmitHook() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDelayedSubmit), delayedSubmit) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
            synthesisGate = nullptr;
            compositeGate = nullptr;
            dependencyQueue = nullptr;
        }
    };
    decltype(&ovr_CommitTextureSwapChain) originalDelayCommit;
    std::map<ovrTextureSwapChain, unsigned> currentImages, commitCounts;
    ovrResult OVR_CDECL advanceCommit(ovrSession, ovrTextureSwapChain chain) {
        ++commitCounts[chain];
        currentImages[chain] = (currentImages[chain] + 1) % 2;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL advancingIndex(ovrSession, ovrTextureSwapChain chain, int* index) {
        *index = static_cast<int>(currentImages.at(chain));
        return ovrSuccess;
    }
    struct AdvancingChains {
        AdvancingChains() {
            currentImages.clear();
            commitCounts.clear();
            originalDelayCommit = reinterpret_cast<decltype(originalDelayCommit)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_CommitTextureSwapChain"));
            require(originalDelayCommit != nullptr, "commit export required");
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourDetach(reinterpret_cast<PVOID*>(&originalIndex), interceptedIndex));
            checkDetour(DetourTransactionCommit());
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalIndex), advancingIndex));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalDelayCommit), advanceCommit));
            checkDetour(DetourTransactionCommit());
        }
        ~AdvancingChains() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDelayCommit), advanceCommit) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalIndex), advancingIndex) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR || DetourTransactionBegin() != NO_ERROR ||
                DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalIndex), interceptedIndex) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void delay(const wchar_t* backend, const wchar_t* module, const std::wstring& scenario) {
            const bool futureInput = scenario == L"future-input";
            const bool control = scenario == L"control";
            const bool lateNr = scenario == L"late-nr";
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            AdvancingChains chains;
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = 3;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            runtime.m_fgEyeResolution = {eyeWidth, height};
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           3,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize delayed real GPU module");
            ComPtr<ID3D12Fence> fgGate, futureRelease;
            success(hw.gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fgGate)), "FG gate");
            // Keep the detoured module loaded through SubmitHook destruction.
            const auto heldModule = LoadLibraryW(std::filesystem::absolute(module).c_str());
            require(heldModule != nullptr, "hold detoured module");
            auto heldModuleGuard = MakeScopeGuard([&] { FreeLibrary(heldModule); });
            SubmitHook submitHook(hw.gpu, fgGate.Get());
            ComPtr<ID3D11Fence> futureGate, presented;
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&futureGate)), "future NR gate");
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&presented)), "presentation marker");
            HANDLE shared{};
            success(futureGate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared), "export future gate");
            const auto opened = hw.gpu.device->OpenSharedHandle(shared, IID_PPV_ARGS(&futureRelease));
            CloseHandle(shared);
            success(opened, "import future gate");
            std::array<std::array<ComPtr<ID3D11Texture2D>, 2>, 6> textures;
            for (unsigned bank = 0; bank < 2; ++bank) {
                for (unsigned role = 0; role < 3; ++role) {
                    auto& slice = runtime.m_fgPresentationBanks[bank][role];
                    slice.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6000 + bank * 3 + role));
                    currentImages[slice.ovrSwapchain] = 0;
                    for (unsigned image = 0; image < 2; ++image) {
                        textures[bank * 3 + role][image] = hw.texture();
                        slice.images.push_back(textures[bank * 3 + role][image]);
                    }
                }
            }
            std::vector<ovrTextureSwapChain> submitted;
            std::vector<unsigned> submittedImages;
            OwnedFrameTransaction observedPair;
            bool laterCopiesReady{};
            state.observeLayers = [&](const ovrLayerHeader* const* layers, unsigned count) {
                require(count == 1 && layers[0] && layers[0]->Type == ovrLayerType_EyeFov, "stereo projection");
                const auto& eye = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                require(eye.ColorTexture[0] == eye.ColorTexture[1] && eye.Viewport[1].Pos.x == eyeWidth,
                        "distinct packed-eye rectangles");
                const auto chain = eye.ColorTexture[0];
                if (futureInput && submitted.size() == 1) {
                    laterCopiesReady =
                        runtime.m_frameGeneration->copyReady(observedPair, FrameGenerationRole::synthetic2) &&
                        runtime.m_frameGeneration->copyReady(observedPair, FrameGenerationRole::real);
                }
                submitted.push_back(chain);
                submittedImages.push_back((currentImages.at(chain) + 1) % 2);
                success(hw.context->Signal(presented.Get(), submitted.size()), "signal actual queued presentation");
                hw.context->Flush();
            };
            std::thread producer;
            std::exception_ptr producerFailure;
            std::atomic<bool> queuedFuture{};
            if (futureInput) {
                state.observeWait = [&](long long id) {
                    if (id != 1) {
                        return;
                    }
                    // Force the next NR wait between the first and second
                    // native Ends, after the worker releases its context lease.
                    const auto limit = std::chrono::steady_clock::now() + 2s;
                    while (!queuedFuture && std::chrono::steady_clock::now() < limit) {
                        std::this_thread::sleep_for(1ms);
                    }
                    require(queuedFuture, "next input gate is queued before S2 native Begin");
                };
            }
            auto cleanup = MakeScopeGuard([&] {
                // Release every artificial dependency BEFORE joins or readback,
                // including on assertion failure. A driver stall is not hidden.
                (void)fgGate->Signal(1);
                (void)futureRelease->Signal(1);
                RuntimeInputRegression::stop(runtime, state, &producer);
            });
            FrameGenerationCadence cadence;
            constexpr int64_t period = 20000000;
            const auto first = runtime.ovrTimeToXrTime(ovr_GetTimeInSeconds()) + 500000000;
            const auto reservation = cadence.reserve(first - period, first, period, 3, 0);
            FinalProjectionInput input;
            input.serial = 1;
            input.time = first - period;
            input.period = period;
            input.slots = cadence.plan(cadence.reserve(first - 4 * period, first - 3 * period, period, 3, 0), 0, false);
            const auto a = scene(0), b = scene(24);
            input.colour[0] = input.colour[1] = hw.texture(&a);
            for (unsigned eye = 0; eye < 2; ++eye) {
                input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
                input.views[eye].orientation[3] = 1;
                input.views[eye].position[0] = eye ? .032f : -.032f;
                input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
                input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
            }
            OwnedFrameTransaction prime;
            success(runtime.m_frameGeneration->capture(input, prime), "prime actual synthesis history");
            hw.await(*runtime.m_frameGeneration, prime);
            hw.retire(*runtime.m_frameGeneration, prime);
            input.serial = 2;
            input.time = reservation.currentTarget;
            input.slots = cadence.plan(reservation, first - period, true);
            input.colour[0] = input.colour[1] = hw.texture(&b);
            require(input.slots.count == 3, "valid three-slot moving pair");
            delaySubmission = !control && !lateNr;
            if (lateNr) {
                success(hw.context->Wait(futureGate.Get(), 1), "model unfinished current NR source");
            }
            OwnedFrameTransaction pair;
            success(runtime.m_frameGeneration->capture(input, pair), "capture moving pair without waiting for FG");
            observedPair = pair;
            const auto readyValue = pair.ticket.output_ready_value;
            auto readyFence = static_cast<ID3D12Fence*>(pair.ticket.output_ready_fence);
            // A submission boundary trace must not Map/read the output before
            // it is handed to the actual worker.
            if (control) {
                hw.await(*runtime.m_frameGeneration, pair);
            }
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
            if (futureInput) {
                producer = std::thread([&] {
                    try {
                        runtime.waitForFrameGenerationCapacity();
                        std::unique_lock lease(runtime.m_fgContextMutex);
                        success(hw.context->Wait(futureGate.Get(), 1), "queue next frame NR-style wait");
                        hw.context->Flush();
                        queuedFuture = true;
                    } catch (...) {
                        producerFailure = std::current_exception();
                    }
                });
                std::this_thread::sleep_for(30ms);
                success(fgGate->Signal(1), "release current synthesis independently of next input");
            }
            const auto deadline = runtime.xrTimeToOvrTime(first) + .025;
            while (ovr_GetTimeInSeconds() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            const auto atDeadline = presented->GetCompletedValue();
            const bool synthesisReady = readyFence->GetCompletedValue() >= readyValue;
            unsigned endsAtDeadline{};
            bool admissionOpen{};
            {
                std::lock_guard lock(state.mutex);
                endsAtDeadline = static_cast<unsigned>(submitted.size());
            }
            {
                std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                admissionOpen = runtime.m_asyncSubmissionReady;
            }
            std::cout << "DEADLINE: scenario=" << std::string(scenario.begin(), scenario.end())
                      << " module_ready=" << synthesisReady << " ends=" << endsAtDeadline
                      << " gpu_handoffs=" << atDeadline << " next_wait=" << queuedFuture << '\n';
            success(fgGate->Signal(1), "release late synthesis before cleanup");
            success(futureRelease->Signal(1), "release future input before cleanup");
            if (producer.joinable()) {
                producer.join();
            }
            if (producerFailure) {
                std::rethrow_exception(producerFailure);
            }
            runtime.waitForAsyncSubmissionIdle();
            require(submitted.size() == 3, "exactly three native End calls");
            const auto chain = submitted.back();
            const auto textureIndex = static_cast<unsigned>(reinterpret_cast<uintptr_t>(chain) - 0x6000);
            require(hw.read(textures.at(textureIndex).at(submittedImages.back()).Get()) == b,
                    "real B retains exact finalized NR pixels with advancing SDK image indices");
            if (!futureInput && !control) {
                require(commitCounts[chain] == 1, "late padding repeats commit real B only once");
            }
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
            hw.gpu.noErrors();
            require(endsAtDeadline == 3, "all native End attempts occur before the observation deadline");
            require(atDeadline != UINT64_MAX &&
                        (lateNr ? atDeadline == 0 : (futureInput ? atDeadline >= 1 : atDeadline == 3)),
                    "current presentation GPU copy must complete without future input or late FG");
            require(!futureInput || laterCopiesReady, "S2 and B copies finish despite next NR wait before later Ends");
            require(control || futureInput || !admissionOpen,
                    "unready module does not admit a next capture while independent real B is presented");
            require(control || (futureInput ? synthesisReady : !synthesisReady), "gate control reached intended phase");
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(argc == 4, "backend, module and scenario required");
        const std::wstring scenario = argv[3];
        require(scenario == L"late-fg" || scenario == L"future-input" || scenario == L"control" ||
                    scenario == L"late-nr",
                "unknown delay case");
        virtualdesktop_openxr::RuntimeFrameGenerationRegression::delay(argv[1], argv[2], scenario);
        std::cout << "PASS: real GPU deadline, independent B and queue-order proof\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
