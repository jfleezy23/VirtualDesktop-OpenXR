// Public OpenXR path, real vendor NR, real NVOF and private OVRNull.
// Settings are read-only synthetic registry responses, never real writes.
#define VDXR_NR_HELPERS_ONLY
#include "nr_input_toggle_regression.cpp"
#undef VDXR_NR_HELPERS_ONLY

namespace {
    decltype(&ovr_GetPredictedDisplayTime) originalPrediction;
    decltype(&ovr_GetHmdDesc) originalDisplay;
    decltype(&ovr_WaitToBeginFrame) originalNativeWait;
    std::atomic<double> clockBase{};
    std::mutex clockMutex;
    std::condition_variable clockChanged;
    bool clockWarmed{};
    uint64_t nextWaitId{};
    double OVR_CDECL stablePrediction(ovrSession, long long id) {
        return clockBase.load() + id / 120.;
    }
    ovrHmdDesc OVR_CDECL display120(ovrSession session) {
        auto info = originalDisplay(session);
        info.DisplayRefreshRate = 120.f;
        return info;
    }
    ovrResult OVR_CDECL scheduledNativeWait(ovrSession session, long long id) {
        {
            std::unique_lock lock(clockMutex);
            require(id >= 0 && static_cast<uint64_t>(id) == nextWaitId++, "scheduled native Wait IDs advance once");
            if (id >= 2) {
                require(clockChanged.wait_for(lock, 5s, [] { return clockWarmed; }),
                        "warmup epoch released within bound");
            }
        }
        const auto target = stablePrediction(session, id);
        // OVRNull internally paces at72Hz. A120Hz prediction paired with that
        // Wait is an invalid test clock. Use one real-time120Hz epoch with a
        // 2ms render lead; GPU/Begin/Commit/End remain their real implementations.
        while (target - ovr_GetTimeInSeconds() > .002) {
            std::this_thread::sleep_for(std::chrono::duration<double>(target - ovr_GetTimeInSeconds() - .002));
        }
        std::cout << "scheduled native wait=" << id << " lead=" << (target - ovr_GetTimeInSeconds()) * 1000 << "ms\n";
        return ovrSuccess;
    }
    struct NativeClock {
        NativeClock() {
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalPrediction =
                reinterpret_cast<decltype(originalPrediction)>(GetProcAddress(module, "ovr_GetPredictedDisplayTime"));
            originalDisplay = reinterpret_cast<decltype(originalDisplay)>(GetProcAddress(module, "ovr_GetHmdDesc"));
            originalNativeWait =
                reinterpret_cast<decltype(originalNativeWait)>(GetProcAddress(module, "ovr_WaitToBeginFrame"));
            require(originalPrediction && originalDisplay, "native prediction exports required");
            clockBase = ovr_GetTimeInSeconds() + 1. / 120.;
            checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
            checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalPrediction), stablePrediction)));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalDisplay), display120)));
            checkHr(
                HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalNativeWait), scheduledNativeWait)));
            checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        }
        ~NativeClock() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalPrediction), stablePrediction) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDisplay), display120) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalNativeWait), scheduledNativeWait) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
        }
        void reanchorAfterWarmup() {
            std::unique_lock lock(clockMutex);
            clockBase = ovr_GetTimeInSeconds(); // native2 is now two periods ahead
            clockWarmed = true;
            clockChanged.notify_all();
        }
        void releaseOnFailure() {
            std::unique_lock lock(clockMutex);
            clockWarmed = true;
            clockChanged.notify_all();
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static OpenXrRuntime& runtime() {
            return *static_cast<OpenXrRuntime*>(GetInstance());
        }
        static unsigned nrEvaluations[2];
        static std::atomic<unsigned> syntheticPresentations;
        static std::map<ovrTextureSwapChain, unsigned> nrCommits;
        static std::map<ovrTextureSwapChain, unsigned> commitsBeforeInput;
        static std::vector<long long> presentedIds;
        static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) originalEvaluate[2];
        template <unsigned eye>
        static NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D12GraphicsCommandList* commands,
                                                    const NVSDK_NGX_Handle* feature,
                                                    const NVSDK_NGX_Parameter* parameters,
                                                    PFN_NVSDK_NGX_ProgressCallback callback) {
            ++nrEvaluations[eye];
            return originalEvaluate[eye](commands, feature, parameters, callback);
        }
        static void observe() {
            auto& value = runtime();
            for (unsigned eye = 0; eye < 2; ++eye) {
                originalEvaluate[eye] = value.m_dlssnrFeatureEvaluate[eye];
            }
            value.m_dlssnrFeatureEvaluate[0] = evaluate<0>;
            value.m_dlssnrFeatureEvaluate[1] = evaluate<1>;
        }
        static VDXRFG_StatisticsV1 statistics() {
            VDXRFG_StatisticsV1 result{};
            std::unique_lock contextLease(runtime().m_fgContextMutex);
            checkHr(runtime().m_frameGeneration->getStatistics(result));
            return result;
        }
        static void verifyInternalChains() {
            auto& value = runtime();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            if (value.m_fgPreviousCapturedTime != value.m_precompositor.displayTime) {
                return; // A complete NR-preserving fallback must commit its final source.
            }
            require(nrCommits[value.m_dlssnrOutputSwapchain] == commitsBeforeInput[value.m_dlssnrOutputSwapchain],
                    "internal NR capture source is not committed to a compositor that never consumes it");
            for (const auto handle : value.m_swapchains) {
                const auto& chain = *reinterpret_cast<OpenXrRuntime::Swapchain*>(handle);
                for (const auto& eye : chain.stereoProjection) {
                    require(!eye.ovrSwapchain || nrCommits[eye.ovrSwapchain] == commitsBeforeInput[eye.ovrSwapchain],
                            "internal sharpen/upscale source does not fill an unused compositor chain");
                }
            }
        }
        static void beforeInputEnd() {
            std::unique_lock lock(captureMutex);
            commitsBeforeInput = nrCommits;
        }
        static void observeCommit(ovrTextureSwapChain chain) {
            std::unique_lock lock(captureMutex);
            ++nrCommits[chain];
        }
        static void observePresentation(const ovrLayerEyeFov& projection) {
            // Called within production End's context lease, before the backend
            // consumes this actual layer. A synthesized GPU pair alone is not
            // evidence that any synthetic pixels were submitted.
            for (const auto& bank : runtime().m_fgPresentationBanks) {
                for (unsigned role = 0; role < 2; ++role) {
                    if (bank[role].ovrSwapchain && bank[role].ovrSwapchain == projection.ColorTexture[0]) {
                        ++syntheticPresentations;
                    }
                }
            }
        }
        static void observeId(long long id) {
            require(id == static_cast<long long>(presentedIds.size()),
                    "public Wait/Begin/End retains consecutive independent native IDs");
            presentedIds.push_back(id);
        }
        static void verifyFinal(bool nr, int size, bool drain) {
            auto& value = runtime();
            if (drain) {
                value.waitForAsyncSubmissionIdle();
            }
            std::unique_lock contextLease(value.m_fgContextMutex);
            VDXRFG_StatisticsV1 statistics{};
            if (value.m_frameGeneration) {
                checkHr(value.m_frameGeneration->getStatistics(statistics));
            }
            std::cout << "public FG frame=" << value.m_frameBegun << " native=" << value.m_fgBegunReservation.nativeId
                      << " target=" << value.m_fgBegunReservation.currentTarget
                      << " period=" << value.m_fgBegunReservation.period
                      << " previous=" << value.m_fgPreviousCapturedTime
                      << " multiplier=" << value.m_fgBegunReservation.multiplier << " pairs=" << statistics.pairs
                      << " captures=" << statistics.captures << " reprime=" << value.m_fgNeedReprime.load()
                      << " now=" << value.ovrTimeToXrTime(ovr_GetTimeInSeconds()) << '\n';
            require(value.m_precompositor.dlssnrSettings.foveationSize == 1.f, "NR coverage stays100%");
            if (!nr) {
                require(!value.m_precompositor.finalNrColour[0] && !value.m_precompositor.finalNrColour[1],
                        "NR off never submits original game pixels as post-NR");
                require(!value.m_frameGeneration, "NR-off fresh session never loads the FG host");
                return;
            }
            for (unsigned eye = 0; eye < 2; ++eye) {
                require(value.m_precompositor.finalNrColour[eye] != nullptr,
                        "both finalized NR eye resources captured");
                D3D11_TEXTURE2D_DESC desc{};
                value.m_precompositor.finalNrColour[eye]->GetDesc(&desc);
                require(desc.Height == size, "FG capture uses post-upscaler dimensions");
            }
            require(value.m_frameGeneration && value.m_frameGeneration->initialized(),
                    "eligible public NR frame reaches real FG module");
            if (drain) {
                require(!value.m_fgTransactionForSubmission.ticket.token, "presenter retired the full transaction");
            }
        }
    };
    unsigned RuntimeFrameGenerationRegression::nrEvaluations[2]{};
    std::atomic<unsigned> RuntimeFrameGenerationRegression::syntheticPresentations{};
    std::map<ovrTextureSwapChain, unsigned> RuntimeFrameGenerationRegression::nrCommits;
    std::map<ovrTextureSwapChain, unsigned> RuntimeFrameGenerationRegression::commitsBeforeInput;
    std::vector<long long> RuntimeFrameGenerationRegression::presentedIds;
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) RuntimeFrameGenerationRegression::originalEvaluate[2]{};
} // namespace virtualdesktop_openxr

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    XrInstance instance{};
    try {
        require(argc == 4, "private backend, state directory and native/upscaled/discard required");
        const bool discard = !strcmp(argv[3], "discard");
        require(discard || !strcmp(argv[3], "native") || !strcmp(argv[3], "upscaled"), "known public FG scenario");
        requestedFrameGeneration = 3;
        coverage = 100;
        nrUpscalingSetting = !strcmp(argv[3], "upscaled") ? 150 : 100;
        nrSharpenSetting = !strcmp(argv[3], "upscaled") ? 25 : 0;
        SettingsReadHook settings;
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        const auto getProc = virtualdesktop_openxr::RuntimeInputRegression::initialize(argv[1], argv[2]);
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR post NR frame generation regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = extensions;
        XR_CHECK(xrCreateInstance(&info, &instance));
#define LOAD(name) XR_CHECK(getProc(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)));
        XR_FUNCTIONS(LOAD)
#undef LOAD
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system{};
        XR_CHECK(xrGetSystem(instance, &systemInfo, &system));
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(instance, system, &requirements));
        ComPtr<IDXGIFactory1> factory;
        checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            checkHr(factory->EnumAdapters1(i, &adapter));
            DXGI_ADAPTER_DESC1 desc{};
            checkHr(adapter->GetDesc1(&desc));
            if (!memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID))) {
                std::wcout << L"public runtime adapter=" << desc.Description << L" vendor=" << desc.VendorId << L'\n';
                break;
            }
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checkHr(D3D11CreateDevice(adapter.Get(),
                                  D3D_DRIVER_TYPE_UNKNOWN,
                                  nullptr,
                                  0,
                                  &requirements.minFeatureLevel,
                                  1,
                                  D3D11_SDK_VERSION,
                                  &device,
                                  nullptr,
                                  &context));
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.Get();
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
        create.next = &binding;
        create.systemId = system;
        XrSession session{};
        XR_CHECK(xrCreateSession(instance, &create, &session));
        PFN_xrDestroySession destroySession{};
        XR_CHECK(getProc(instance, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&destroySession)));
        {
            CompositorHooks compositor;
            NativeClock nativeClock;
            auto releaseClock = MakeScopeGuard([&] {
                nativeClock.releaseOnFailure();
                if (session) {
                    try {
                        checkXr(destroySession(session), "failure session cleanup");
                    } catch (...) {
                        std::_Exit(2);
                    }
                }
            });
            observeProjection = Regression::observePresentation;
            observeNativeEnd = Regression::observeId;
            expectDepth[0] = expectDepth[1] = false;
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XR_CHECK(xrBeginSession(session, &begin));
            XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            reference.poseInReferenceSpace.orientation.w = 1.f;
            XrSpace space{};
            XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
            auto colour = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, false);
            const std::vector<uint32_t> rgba(512 * 512, 0xff604020);
            constexpr unsigned inputs = 12;
            unsigned beforeDiscard{};
            for (unsigned frame = 0; frame < inputs; ++frame) {
                const bool nr = frame != 0;
                virtualdesktop_openxr::RuntimeInputRegression::setEnabled(nr);
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState state{XR_TYPE_FRAME_STATE};
                XR_CHECK(xrWaitFrame(session, &wait, &state));
                XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
                XR_CHECK(xrBeginFrame(session, &fb));
                if (discard && frame == 4) {
                    beforeDiscard = Regression::syntheticPresentations.load();
                    const auto previousTime = state.predictedDisplayTime;
                    XR_CHECK(xrWaitFrame(session, &wait, &state));
                    require(state.predictedDisplayTime > previousTime, "pipelined Wait timestamps stay monotonic");
                    require(xrBeginFrame(session, &fb) == XR_FRAME_DISCARDED,
                            "second public Begin discards its unsubmitted predecessor");
                }
                const auto image = acquireImage(colour);
                for (unsigned eye = 0; eye < 2; ++eye) {
                    context->UpdateSubresource(colour.images[image].texture,
                                               D3D11CalcSubresource(0, eye, 1),
                                               nullptr,
                                               rgba.data(),
                                               512 * 4,
                                               512 * 512 * 4);
                }
                releaseImage(colour);
                XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
                for (unsigned eye = 0; eye < 2; ++eye) {
                    views[eye].pose.orientation.w = 1.f;
                    views[eye].pose.position.x = eye ? .032f : -.032f;
                    views[eye].fov = {-.8f, .8f, .8f, -.8f};
                    views[eye].subImage = {colour.chain, {{0, 0}, {512, 512}}, eye};
                }
                XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                layer.space = space;
                layer.viewCount = 2;
                layer.views = views;
                const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
                end.displayTime = state.predictedDisplayTime;
                end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                end.layerCount = 1;
                end.layers = &header;
                Regression::beforeInputEnd();
                XR_CHECK(xrEndFrame(session, &end));
                Regression::verifyFinal(
                    nr, ((51200 / nrUpscalingSetting + 3) / 4) * 4, frame <= 1 || frame == inputs - 1);
                Regression::verifyInternalChains();
                if (!frame) {
                    observeNativeCommit = Regression::observeCommit;
                }
                if (frame == 1) {
                    Regression::observe();
                    nativeClock.reanchorAfterWarmup();
                }
                if (!captureFailure.empty()) {
                    throw std::runtime_error(captureFailure);
                }
            }
            require(Regression::nrEvaluations[0] == inputs - 2 && Regression::nrEvaluations[1] == inputs - 2,
                    "exactly one real NR evaluation per eye per input, none on synthesized output");
            const auto statistics = Regression::statistics();
            require(statistics.pairs >= 4 && statistics.synthetic_frames >= 8,
                    "public120Hz prediction path must produce a real pair after warmup");
            require(Regression::syntheticPresentations >= 8,
                    "production native presenter must submit both generated roles, not only accept GPU synthesis");
            if (discard) {
                require(Regression::syntheticPresentations >= beforeDiscard + 4,
                        "public discard recovers real synthetic presentation after reanchoring");
            }
            observeProjection = {};
            observeNativeEnd = {};
            observeNativeCommit = {};
            XR_CHECK(destroySession(session)); // join worker before removing its pacing/End hooks
            session = XR_NULL_HANDLE;
        }
        context->ClearState();
        context->Flush();
        XR_CHECK(xrDestroyInstance(instance));
        instance = XR_NULL_HANDLE;
        std::cout << "PASS: public post-NR capture, optional FG, final upscaling and100% coverage with real vendor\n";
        return 0;
    } catch (const std::exception& e) {
        if (instance && xrDestroyInstance) {
            try {
                checkXr(xrDestroyInstance(instance), "failure cleanup");
            } catch (...) {
            }
        }
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
