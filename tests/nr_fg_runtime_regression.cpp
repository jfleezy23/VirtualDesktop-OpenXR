// Public OpenXR path, real vendor NR, real NVOF and private OVRNull.
// Settings are read-only synthetic registry responses, never real writes.
#define VDXR_NR_HELPERS_ONLY
#include "nr_input_toggle_regression.cpp"
#undef VDXR_NR_HELPERS_ONLY
#include "nr_fg_lookup_probe.h"

namespace {
    struct D3D12Input {
        ComPtr<ID3D12Device> device;
        std::unique_ptr<D3D12Utils::CommandContext> context;
        ComPtr<ID3D12DescriptorHeap> targets;
        std::vector<XrSwapchainImageD3D12KHR> images;
        UINT stride{};

        explicit D3D12Input(IDXGIAdapter* adapter) {
            checkHr(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
            context = std::make_unique<D3D12Utils::CommandContext>(device.Get(), L"FG app fixture");
        }
        void attach(XrSwapchain chain) {
            uint32_t count{};
            XR_CHECK(xrEnumerateSwapchainImages(chain, 0, &count, nullptr));
            images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            XR_CHECK(xrEnumerateSwapchainImages(
                chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = count * 2;
            checkHr(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets)));
            stride = device->GetDescriptorHandleIncrementSize(heap.Type);
            auto handle = targets->GetCPUDescriptorHandleForHeapStart();
            for (const auto& image : images) {
                require(image.texture != nullptr, "public D3D12 swapchain exports real images");
                for (unsigned eye = 0; eye < 2; ++eye) {
                    D3D12_RENDER_TARGET_VIEW_DESC view{};
                    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.FirstArraySlice = eye;
                    view.Texture2DArray.ArraySize = 1;
                    device->CreateRenderTargetView(image.texture, &view, handle);
                    handle.ptr += stride;
                }
            }
        }
        void fill(unsigned index, bool overwrite = false) {
            auto commands = context->GetCommandList();
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                images[index].texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            commands.Commands->ResourceBarrier(1, &barrier);
            auto handle = targets->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += index * 2 * stride;
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& tint = expectedOffColour[eye];
                const float value[]{overwrite ? 1.f : tint[0] / 255.f,
                                    overwrite ? 0.f : tint[1] / 255.f,
                                    overwrite ? 0.f : tint[2] / 255.f,
                                    1.f};
                commands.Commands->ClearRenderTargetView(handle, value, 0, nullptr);
                handle.ptr += stride;
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands.Commands->ResourceBarrier(1, &barrier);
            context->SubmitCommandList(std::move(commands));
        }
    };

    decltype(&ovr_GetPredictedDisplayTime) originalPrediction;
    decltype(&ovr_GetHmdDesc) originalDisplay;
    decltype(&ovr_WaitToBeginFrame) originalNativeWait;
    std::atomic<double> clockBase{};
    std::mutex clockMutex;
    std::condition_variable clockChanged;
    bool clockWarmed{};
    unsigned nativeRate = 120;
    uint64_t nextWaitId{};
    double OVR_CDECL stablePrediction(ovrSession, long long id) {
        return clockBase.load() + id / static_cast<double>(nativeRate);
    }
    ovrHmdDesc OVR_CDECL display120(ovrSession session) {
        auto info = originalDisplay(session);
        info.DisplayRefreshRate = static_cast<float>(nativeRate);
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
            clockBase = ovr_GetTimeInSeconds() + 1. / nativeRate;
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
        static ovrLayerEyeFov lastProjection;
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
        static void verifyNoNrGraph() {
            const auto& value = runtime();
            require(!value.m_dlssnrResourcesInitialized && !value.m_dlssnrDevice && !value.m_dlssnrContext &&
                        !value.m_ngxParameters,
                    "fresh NR-off session never initializes an NR GPU graph or NGX parameters");
            require(value.m_d3d12CommandQueue ? bool(value.m_fgSourceFence12) : bool(value.m_fgSourceFence11),
                    "the active public graphics API imports the source-reuse fence");
        }
        static ComPtr<ID3D12Fence> blockSource() {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock lease(value.m_fgContextMutex);
            ComPtr<ID3D12Fence> fence;
            checkHr(value.m_fgDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
            wil::unique_handle handle;
            checkHr(value.m_fgDevice->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, handle.put()));
            ComPtr<ID3D11Fence> imported;
            checkHr(value.m_ovrSubmissionDevice->OpenSharedFence(handle.get(), IID_PPV_ARGS(&imported)));
            checkHr(value.m_ovrSubmissionContext->Wait(imported.Get(), 1));
            return fence;
        }
        static void drainAndVerifyPixels() {
            runtime().waitForAsyncSubmissionIdle();
            verifyPixels();
        }
        static void forceFallback() {
            runtime().m_fgBegunReservation.shouldRender = false;
        }
        static void verifyCaptureRoute(XrTime time, bool accepted) {
            require((runtime().m_fgPreviousCapturedTime == time) == accepted,
                    "source overwrite exercises the intended accepted or bypassed input route");
        }
        static void verifySessionCleanup() {
            const auto& value = runtime();
            require(!value.m_frameGeneration && !value.m_fgDevice && !value.m_fgSourceFence11 &&
                        !value.m_fgSourceFence12 && !value.m_dlssnrResourcesInitialized,
                    "session cleanup retires FG devices, imported source fences and lazy NR readiness");
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
        static void beginLookupProbe(NrLookupProbe& probe) {
            probe.begin(runtime().m_dlssnrOutputSwapchainSrvs);
        }
        static void verifyLookupResult(bool postprocess) {
            const auto& value = runtime();
            ComPtr<ID3D11Resource> packed;
            value.m_dlssnrOutputSwapchainSrvs[value.m_dlssnrOutputSwapchainLastWrittenIndex]->GetResource(&packed);
            ComPtr<ID3D11Texture2D> texture;
            checkHr(packed.As(&texture));
            for (unsigned eye = 0; eye < 2; ++eye) {
                if (!postprocess) {
                    require(value.m_precompositor.finalColour[eye] == texture,
                            "both final eye references own the current packed NR texture");
                } else {
                    require(value.m_precompositor.finalColour[eye] != texture,
                            "postprocessed FG colour replaces the intermediate NR texture");
                    bool found{};
                    for (const auto handle : value.m_swapchains) {
                        const auto& projection =
                            reinterpret_cast<OpenXrRuntime::Swapchain*>(handle)->stereoProjection[eye];
                        found |=
                            projection.lastCommittedIndex >= 0 &&
                            value.m_precompositor.finalColour[eye] == projection.images[projection.lastCommittedIndex];
                    }
                    require(found, "FG captures each eye's current postprocessed texture");
                }
            }
        }
        static void observeCommit(ovrTextureSwapChain chain) {
            std::unique_lock lock(captureMutex);
            ++nrCommits[chain];
        }
        static void observePresentation(const ovrLayerEyeFov& projection) {
            lastProjection = projection;
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
        static void verifyPixels() {
            auto& value = runtime();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            for (unsigned eye = 0; eye < 2; ++eye) {
                readColor(value.m_ovrSession, lastProjection, eye);
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
                require(value.m_frameGeneration && value.m_frameGeneration->initialized(),
                        "NR-off public frame must initialize the independent FG host");
            }
            for (unsigned eye = 0; eye < 2; ++eye) {
                require(value.m_precompositor.finalColour[eye] != nullptr, "both finalized NR eye resources captured");
                D3D11_TEXTURE2D_DESC desc{};
                value.m_precompositor.finalColour[eye]->GetDesc(&desc);
                require(desc.Height >= static_cast<unsigned>(size), "FG source contains the final eye viewport");
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
    ovrLayerEyeFov RuntimeFrameGenerationRegression::lastProjection{};
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) RuntimeFrameGenerationRegression::originalEvaluate[2]{};
} // namespace virtualdesktop_openxr

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    XrInstance instance{};
    try {
        require(argc == 4, "private backend, state directory and FG scenario required");
        const bool discard = !strcmp(argv[3], "discard");
        const bool toggle = !strcmp(argv[3], "nr-toggle");
        const bool d3d12 = !strcmp(argv[3], "nr-off-d3d12") || !strcmp(argv[3], "nr-off-d3d12-reuse") ||
                           !strcmp(argv[3], "nr-off-d3d12-reuse-bypass");
        const bool cropped = !strcmp(argv[3], "nr-off-cropped");
        const bool packed = !strcmp(argv[3], "nr-off-packed");
        const bool reuseFallback =
            !strcmp(argv[3], "nr-off-reuse-bypass") || !strcmp(argv[3], "nr-off-d3d12-reuse-bypass");
        const bool reuse = !strcmp(argv[3], "nr-off-reuse") || !strcmp(argv[3], "nr-off-d3d12-reuse") || reuseFallback;
        const bool fallback = !strcmp(argv[3], "nr-off-bypass") || reuseFallback;
        const bool noNr = !strcmp(argv[3], "nr-off-2") || !strcmp(argv[3], "nr-off-3") ||
                          !strcmp(argv[3], "nr-off-upscaled") || cropped || packed || reuse || fallback || d3d12;
        if (const auto rate = std::getenv("VDXR_FG_TEST_REFRESH_RATE")) {
            nativeRate = static_cast<unsigned>(std::stoul(rate));
            require(nativeRate == 30 || nativeRate == 60 || nativeRate == 120, "explicit supported test clock");
        }
        std::cout << "private backend test clock=" << nativeRate << "Hz\n";
        const bool sharpenOnly = !strcmp(argv[3], "sharpen");
        require(noNr || discard || toggle || sharpenOnly || !strcmp(argv[3], "native") || !strcmp(argv[3], "upscaled"),
                "known public FG scenario");
        requestedFrameGeneration = !strcmp(argv[3], "nr-off-2") ? 2 : 3;
        coverage = 100;
        const bool upscaled = !strcmp(argv[3], "upscaled") || !strcmp(argv[3], "nr-off-upscaled");
        nrUpscalingSetting = upscaled ? 150 : 100;
        nrSharpenSetting = upscaled || sharpenOnly ? 25 : 0;
        enabled = toggle;
        // Blocking Map readbacks inside each native End would themselves miss
        // 120Hz deadlines. Verify final pixels after draining the actual worker.
        readSubmittedColour = false;
        checkOffColourEdges = cropped;
        expectedOffColour[0] = {32, 64, 96, 255};
        expectedOffColour[1] = {144, 80, 48, 255};
        SettingsReadHook settings;
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        const auto getProc = virtualdesktop_openxr::RuntimeInputRegression::initialize(argv[1], argv[2]);
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR post NR frame generation regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 2;
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
        std::unique_ptr<D3D12Input> input12;
        XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        if (d3d12) {
            PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements{};
            XR_CHECK(getProc(instance,
                             "xrGetD3D12GraphicsRequirementsKHR",
                             reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
            XrGraphicsRequirementsD3D12KHR requirements12{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
            XR_CHECK(getRequirements(instance, system, &requirements12));
            input12 = std::make_unique<D3D12Input>(adapter.Get());
            binding12.device = input12->device.Get();
            binding12.queue = input12->context->GetCommandQueue();
        }
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
        create.next = d3d12 ? static_cast<const void*>(&binding12) : static_cast<const void*>(&binding);
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
            const unsigned width = packed ? 1024u : (cropped ? 544u : 512u);
            const unsigned height = cropped ? 536u : 512u;
            auto colour = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, width, height, false, !d3d12);
            if (input12) {
                input12->attach(colour.chain);
            }
            std::vector<uint32_t> rgba(width * height);
            std::vector<ComPtr<ID3D11RenderTargetView>> reuseViews;
            if (reuse && !d3d12) {
                D3D11_RENDER_TARGET_VIEW_DESC description{};
                description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                description.Texture2DArray.ArraySize = 2;
                for (const auto& item : colour.images) {
                    ComPtr<ID3D11RenderTargetView> view;
                    checkHr(device->CreateRenderTargetView(item.texture, &description, &view));
                    reuseViews.push_back(std::move(view));
                }
            }
            const unsigned inputs = reuse ? 4u : ((fallback || toggle) ? 18u : 12u);
            unsigned beforeDiscard{};
            NrLookupProbe lookupProbe;
            for (unsigned frame = 0; frame < inputs; ++frame) {
                const bool nr = toggle ? (frame < 3 || frame >= 6) : (!noNr && frame != 0);
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
                if (input12) {
                    input12->fill(image);
                }
                for (unsigned eye = 0; !input12 && eye < 2; ++eye) {
                    const auto& tint = expectedOffColour[eye];
                    const auto pixel = tint[0] | (tint[1] << 8) | (tint[2] << 16) | (tint[3] << 24);
                    if (!packed || !eye) {
                        std::fill(rgba.begin(), rgba.end(), pixel);
                        if (cropped) {
                            std::fill(rgba.begin(), rgba.end(), 0xff00ff00u);
                            for (unsigned y = 12; y < 524; ++y) {
                                std::fill(rgba.begin() + y * width + 16, rgba.begin() + y * width + 528, pixel);
                            }
                        }
                        if (packed) {
                            const auto& right = expectedOffColour[1];
                            const auto rightPixel = right[0] | (right[1] << 8) | (right[2] << 16) | (right[3] << 24);
                            for (unsigned y = 0; y < height; ++y) {
                                std::fill(rgba.begin() + y * width + 512, rgba.begin() + (y + 1) * width, rightPixel);
                            }
                        }
                    }
                    context->UpdateSubresource(colour.images[image].texture,
                                               D3D11CalcSubresource(0, eye, 1),
                                               nullptr,
                                               rgba.data(),
                                               width * 4,
                                               width * height * 4);
                }
                releaseImage(colour);
                XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
                for (unsigned eye = 0; eye < 2; ++eye) {
                    views[eye].pose.orientation.w = 1.f;
                    views[eye].pose.position.x = eye ? .032f : -.032f;
                    views[eye].fov = {-.8f, .8f, .8f, -.8f};
                    views[eye].subImage = {
                        colour.chain,
                        {{packed ? static_cast<int>(eye * 512) : (cropped ? 16 : 0), cropped ? 12 : 0}, {512, 512}},
                        packed ? 0u : eye};
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
                if (fallback && frame == (reuseFallback ? 3u : 2u)) {
                    // The third app image wraps to SDK-current minus one while
                    // successful FG has never committed its raw source chain.
                    Regression::forceFallback();
                }
                ComPtr<ID3D12Fence> sourceGate;
                std::atomic<bool> allowSource{}, sourceTimeout{};
                std::thread releaseSource;
                if (reuse && frame == 3) {
                    sourceGate = Regression::blockSource();
                    releaseSource = std::thread([&] {
                        const auto deadline = std::chrono::steady_clock::now() + 5s;
                        while (!allowSource && std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::sleep_for(1ms);
                        }
                        sourceTimeout = !allowSource;
                        std::this_thread::sleep_for(50ms);
                        sourceGate->Signal(1);
                    });
                }
                auto releaseSourceGuard = MakeScopeGuard([&] {
                    allowSource = true;
                    if (releaseSource.joinable()) {
                        releaseSource.join();
                    }
                });
                const bool measureLookups = nr && frame >= 2;
                if (measureLookups)
                    Regression::beginLookupProbe(lookupProbe);
                XR_CHECK(xrEndFrame(session, &end));
                if (measureLookups)
                    lookupProbe.finish(upscaled || sharpenOnly);
                if (measureLookups)
                    Regression::verifyLookupResult(upscaled || sharpenOnly);
                if (fallback && frame == 2) {
                    Regression::drainAndVerifyPixels();
                }
                if (sourceGate) {
                    Regression::verifyCaptureRoute(end.displayTime, !reuseFallback);
                    // No drain/readback before app reuse: independently queued
                    // writes must wait for the runtime's two source copies.
                    const float overwrite[]{1.f, 0.f, 0.f, 1.f};
                    if (input12) {
                        input12->fill(image, true);
                    } else {
                        context->ClearRenderTargetView(reuseViews[image].Get(), overwrite);
                        context->Flush();
                    }
                    allowSource = true;
                    releaseSource.join();
                    require(!sourceTimeout, "source protection never blocks EndFrame on the CPU");
                    Regression::drainAndVerifyPixels();
                }
                if (noNr) {
                    Regression::verifyNoNrGraph();
                }
                Regression::verifyFinal(
                    nr, ((51200 / nrUpscalingSetting + 3) / 4) * 4, frame <= 1 || frame == inputs - 1);
                Regression::verifyInternalChains();
                if (toggle && frame == 4) {
                    Regression::drainAndVerifyPixels();
                }
                if (frame <= 1) {
                    if (frame == 1 && !noNr) {
                        Regression::observe();
                    }
                    nativeClock.reanchorAfterWarmup();
                }
                if (!frame) {
                    observeNativeCommit = Regression::observeCommit;
                }
                if (!captureFailure.empty()) {
                    throw std::runtime_error(captureFailure);
                }
            }
            const auto expectedNr = noNr ? 0u : inputs - (toggle ? 5u : 2u);
            Regression::verifyPixels();
            unsigned eyeDifference{};
            for (unsigned component = 0; component < 3; ++component)
                eyeDifference += std::abs(int(observedColour[0][component]) - int(observedColour[1][component]));
            require(eyeDifference > 32, "final FG presentation preserves distinct left and right eye pixels");
            require(Regression::nrEvaluations[0] == expectedNr && Regression::nrEvaluations[1] == expectedNr,
                    "exactly one real NR evaluation per eye per input, none on synthesized output");
            const auto statistics = Regression::statistics();
            const auto minimumSynthetic = 4u * (requestedFrameGeneration - 1);
            if (reuse) {
                // These cases deliberately stall source processing. Ownership
                // and late real fallback are tested independently of cadence;
                // the normal 2X/3X cases require actual synthetic submissions.
                require(statistics.captures >= 2 && !Regression::presentedIds.empty(),
                        "source reuse surrounds real module captures and native presentations");
            } else {
                require(statistics.pairs >= 4 && statistics.synthetic_frames >= minimumSynthetic,
                        "public prediction path must produce real pairs after warmup");
                require(Regression::syntheticPresentations >= minimumSynthetic,
                        "production native presenter must submit generated roles, not only accept GPU synthesis");
            }
            if (discard) {
                require(Regression::syntheticPresentations >= beforeDiscard + 4,
                        "public discard recovers real synthetic presentation after reanchoring");
            }
            observeProjection = {};
            observeNativeEnd = {};
            observeNativeCommit = {};
            if (input12) {
                input12->context->Flush();
            }
            XR_CHECK(destroySession(session)); // join worker before removing its pacing/End hooks
            session = XR_NULL_HANDLE;
            Regression::verifySessionCleanup();
        }
        context->ClearState();
        context->Flush();
        XR_CHECK(xrDestroyInstance(instance));
        instance = XR_NULL_HANDLE;
        std::cout << "PASS: public FG capture, native presentation, final colour and independent NR state\n";
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
