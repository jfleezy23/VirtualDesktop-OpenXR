// Real VD SDK / public OpenXR lifecycle. No controlled clock, Wait, Begin,
// prediction, Commit, or End replacement. The observer forwards every End.
#define VDXR_NATIVE_DELIVERY_PROBE 1
#include "nr_fg_runtime_regression.cpp"
#include <iomanip>
#include <set>

namespace {
    struct NativeEndObserver {
        inline static decltype(&ovr_EndFrame) original{};
        inline static decltype(&ovr_WaitToBeginFrame) originalWait{};
        inline static decltype(&ovr_BeginFrame) originalBegin{};
        inline static decltype(&ovr_CommitTextureSwapChain) originalCommit{};
        struct Call {
            char phase;
            long long id;
            ovrTextureSwapChain chain;
            int result;
            double enter, returned;
        };
        inline static std::vector<Call> calls;
        inline static bool wantsDepth{};
        inline static std::map<ovrTextureSwapChain, unsigned> commitCounts;
        inline static std::map<ovrTextureSwapChain, std::pair<double, unsigned>> submittedCounts;
        HMODULE module{};
        bool attached{};
        static ovrResult OVR_CDECL wait(ovrSession session, long long id) {
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::prebeginBeforeWait(id);
#endif
            const auto enter = ovr_GetTimeInSeconds();
            const auto result = originalWait(session, id);
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::prebeginAfterWait(id);
#endif
            std::lock_guard lock(mutex);
            calls.push_back({'W', id, {}, static_cast<int>(result), enter, ovr_GetTimeInSeconds()});
            return result;
        }
        static ovrResult OVR_CDECL begin(ovrSession session, long long id) {
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
            Regression::prebeginRenderEnter();
            auto leave = MakeScopeGuard([] { Regression::prebeginRenderExit(); });
            Regression::prebeginBeforeBegin(id);
#endif
            const auto enter = ovr_GetTimeInSeconds();
#ifdef VDXR_PREBEGIN_FAILURE_REGRESSION
            const auto result = Regression::prebeginBeginResult(id, originalBegin(session, id));
#else
            const auto result = originalBegin(session, id);
#endif
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            Regression::prebeginAfterBegin(id);
#endif
            std::lock_guard lock(mutex);
            calls.push_back({'B', id, {}, static_cast<int>(result), enter, ovr_GetTimeInSeconds()});
            return result;
        }
        static ovrResult OVR_CDECL commit(ovrSession session, ovrTextureSwapChain chain) {
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
            Regression::prebeginRenderEnter();
            auto leave = MakeScopeGuard([] { Regression::prebeginRenderExit(); });
#endif
            const auto enter = ovr_GetTimeInSeconds();
            const auto result = originalCommit(session, chain);
            std::lock_guard lock(mutex);
            calls.push_back({'C', -1, chain, static_cast<int>(result), enter, ovr_GetTimeInSeconds()});
            if (OVR_SUCCESS(result))
                ++commitCounts[chain];
            return result;
        }
        inline static std::mutex mutex;
        inline static std::condition_variable deliveryChanged;
        inline static std::shared_ptr<virtualdesktop_openxr::RuntimeFrameGenerationRegression::LiveDeliveryGate> gate;
        struct Entry {
            long long id;
            double returned, sample;
            unsigned role;
            int result;
        };
        inline static std::vector<Entry> ends;
        inline static std::atomic<unsigned> failures{};
        static ovrResult OVR_CDECL end(ovrSession session,
                                       long long id,
                                       const ovrViewScaleDesc* scale,
                                       const ovrLayerHeader* const* layers,
                                       unsigned count) {
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
            Regression::prebeginRenderEnter();
            auto leave = MakeScopeGuard([] { Regression::prebeginRenderExit(); });
            Regression::prebeginObserveEnd(id, layers, count);
#endif
            std::shared_ptr<virtualdesktop_openxr::RuntimeFrameGenerationRegression::LiveDeliveryGate> currentGate;
            {
                std::lock_guard lock(mutex);
                if (gate && gate->pendingNativeId == id)
                    currentGate = gate;
            }
            if (currentGate)
                virtualdesktop_openxr::RuntimeFrameGenerationRegression::observeFutureDelivery(
                    *currentGate, layers, count);
            const auto enter = ovr_GetTimeInSeconds();
            const auto result = original(session, id, scale, layers, count);
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            Regression::prebeginAfterNativeEnd(id, result);
#endif
            std::lock_guard lock(mutex);
            calls.push_back({'E', id, {}, static_cast<int>(result), enter, ovr_GetTimeInSeconds()});
            if (!virtualdesktop_openxr::RuntimeFrameGenerationRegression::nativeLayerValid(layers, count, wantsDepth))
                ++failures;
            if (OVR_FAILURE(result))
                ++failures;
            unsigned role = 4;
            double sample{};
            if (count == 1 && layers && layers[0] &&
                (layers[0]->Type == ovrLayerType_EyeFov || layers[0]->Type == ovrLayerType_EyeFovDepth)) {
                const auto& layer = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                role = virtualdesktop_openxr::RuntimeFrameGenerationRegression::nativeRole(layer.ColorTexture[0]);
                sample = layer.SensorSampleTime;
                std::vector<ovrTextureSwapChain> selected{layer.ColorTexture[0], layer.ColorTexture[1]};
                if (layer.Header.Type == ovrLayerType_EyeFovDepth) {
                    const auto& depth = *reinterpret_cast<const ovrLayerEyeFovDepth*>(layers[0]);
                    selected.push_back(depth.DepthTexture[0]);
                    selected.push_back(depth.DepthTexture[1]);
                }
                std::sort(selected.begin(), selected.end(), std::less<ovrTextureSwapChain>{});
                selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
                for (const auto chain : selected) {
                    auto& previous = submittedCounts[chain];
                    const auto expected = previous.second + (previous.first == sample ? 0u : 1u);
                    if (!chain || commitCounts[chain] != expected)
                        ++failures;
                    previous = {sample, commitCounts[chain]};
                }
            }
            ends.push_back({id, ovr_GetTimeInSeconds(), sample, role, static_cast<int>(result)});
            if (gate && gate->pendingNativeId == id && gate->presentationMarker) {
                if (FAILED(virtualdesktop_openxr::RuntimeFrameGenerationRegression::markNativeDelivery(*gate)))
                    ++failures;
                gate->endObserved = true;
                deliveryChanged.notify_all();
            }
            return result;
        }
        explicit NativeEndObserver(bool depth) {
            wantsDepth = depth;
#ifdef VDXR_NATIVE_CONTROLLED_SDK
            constexpr auto moduleName = L"LibOVRRT64_1.dll";
#else
            constexpr auto moduleName = L"VirtualDesktop.LibOVRRT64_1.dll";
#endif
            require(GetModuleHandleExW(0, moduleName, &module), "real VD native SDK must be loaded and pinned");
            auto releaseOnFailure = MakeScopeGuard([&] {
                FreeLibrary(module);
                module = nullptr;
            });
            original = reinterpret_cast<decltype(original)>(GetProcAddress(module, "ovr_EndFrame"));
            originalWait = reinterpret_cast<decltype(originalWait)>(GetProcAddress(module, "ovr_WaitToBeginFrame"));
            originalBegin = reinterpret_cast<decltype(originalBegin)>(GetProcAddress(module, "ovr_BeginFrame"));
            originalCommit =
                reinterpret_cast<decltype(originalCommit)>(GetProcAddress(module, "ovr_CommitTextureSwapChain"));
            require(original && originalWait && originalBegin && originalCommit, "real native lifecycle exports");
            require(DetourTransactionBegin() == NO_ERROR, "begin native lifecycle observation");
            auto abort = MakeScopeGuard([&] { DetourTransactionAbort(); });
            require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                        DetourAttach(reinterpret_cast<PVOID*>(&original), end) == NO_ERROR &&
                        DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait) == NO_ERROR &&
                        DetourAttach(reinterpret_cast<PVOID*>(&originalBegin), begin) == NO_ERROR &&
                        DetourAttach(reinterpret_cast<PVOID*>(&originalCommit), commit) == NO_ERROR,
                    "attach forwarding-only lifecycle observers");
            require(DetourTransactionCommit() == NO_ERROR, "commit native lifecycle observation");
            abort.Deactivate();
            attached = true;
            releaseOnFailure.Deactivate();
        }
        void detach() {
            if (!attached)
                return;
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), end) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalBegin), begin) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalCommit), commit) || DetourTransactionCommit())
                std::_Exit(3);
            attached = false;
        }
        ~NativeEndObserver() {
            detach();
            if (module)
                FreeLibrary(module);
        }
    };
} // namespace

#ifdef VDXR_PREBEGIN_ENTRY_NAME
int VDXR_PREBEGIN_ENTRY_NAME(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    std::cout << std::unitbuf;
    XrInstance instance{};
    XrSession session{};
    PFN_xrDestroySession destroy{};
    try {
        require(argc == 7, "assets state multiplier nr-on|nr-off depth|colour d3d11|d3d12");
        requestedFrameGeneration = static_cast<unsigned>(std::stoul(argv[3]));
        require(requestedFrameGeneration >= 2 && requestedFrameGeneration <= 4, "known multiplier");
        const bool nr = std::string(argv[4]) == "nr-on";
        require(nr || std::string(argv[4]) == "nr-off", "known NR switch");
        const bool depth = std::string(argv[5]) == "depth";
        require(depth || std::string(argv[5]) == "colour", "known composition switch");
        const bool api12 = std::string(argv[6]) == "d3d12";
        require(api12 || std::string(argv[6]) == "d3d11", "known graphics binding");
        requestedFrameGenerationBackend = 0;
        requestedFrameGenerationDepthMode = 1;
        preserveNrAppearance = true;
        enabled = nr;
        coverage = 90;
        nrProcessingResolution = 85;
        nrDepthSubmissionSetting = depth ? 1 : 0;
        SettingsReadHook settings;
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        const auto getProc = virtualdesktop_openxr::RuntimeInputRegression::initialize(argv[1], argv[2]);
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[]{XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
                                 XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
                                 XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR native delivery qualification");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 3;
        info.enabledExtensionNames = extensions;
        XR_CHECK(xrCreateInstance(&info, &instance));
#define LOAD_NATIVE(name) XR_CHECK(getProc(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)));
        XR_FUNCTIONS(LOAD_NATIVE)
#undef LOAD_NATIVE
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system{};
        XR_CHECK(xrGetSystem(instance, &systemInfo, &system));
#ifdef VDXR_NATIVE_CONTROLLED_SDK
        std::cout << "CONTROLLED_SDK=1; scheduled clock; no native headset qualification\n";
        Regression::selectControlledDeliveryBackend();
        auto controlledClock = std::make_unique<NativeClock>();
#endif
        PFN_xrLocateViews xrLocateViews{};
        XR_CHECK(getProc(instance, "xrLocateViews", reinterpret_cast<PFN_xrVoidFunction*>(&xrLocateViews)));
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(instance, system, &requirements));
        ComPtr<IDXGIFactory1> factory;
        checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;; ++index) {
            checkHr(factory->EnumAdapters1(index, &adapter));
            DXGI_ADAPTER_DESC1 desc{};
            checkHr(adapter->GetDesc1(&desc));
            if (!memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)))
                break;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checkHr(D3D11CreateDevice(adapter.Get(),
                                  D3D_DRIVER_TYPE_UNKNOWN,
                                  nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT,
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
        if (api12) {
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
        create.systemId = system;
        create.next = api12 ? static_cast<const void*>(&binding12) : static_cast<const void*>(&binding);
        XR_CHECK(xrCreateSession(instance, &create, &session));
        XR_CHECK(getProc(instance, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&destroy)));
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XR_CHECK(xrBeginSession(session, &begin));
        NativeEndObserver observer(depth);
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
        require(!depth, "pre-Begin overlap fixture uses the real V4 colour FG path");
        Regression::PrebeginFixture prebeginFixture(argv[1], nr);
#endif
        auto stopBeforeObserver = MakeScopeGuard([&] {
#ifdef VDXR_NATIVE_CONTROLLED_SDK
            if (controlledClock)
                controlledClock->releaseOnFailure();
#endif
            try {
                Regression::observeSpatial(false);
            } catch (...) {
                std::_Exit(3);
            }
            if (session && destroy) {
#ifdef VDXR_LAST_COPY_GATE_REGRESSION
                Regression::lastCopyReportWorkerError();
#endif
                try {
                    checkXr(destroy(session), "stop native worker before detaching observer");
                } catch (...) {
                    std::_Exit(2);
                }
                session = XR_NULL_HANDLE;
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
                Regression::prebeginVerifyCleanup();
#endif
            }
            observer.detach();
#ifdef VDXR_NATIVE_CONTROLLED_SDK
            controlledClock.reset();
#endif
        });

        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        reference.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace space{};
        XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
        {
            auto colour = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, false, !api12);
            if (input12)
                input12->attach(colour.chain);
            std::unique_ptr<DepthPattern> pattern;
            if (depth)
                pattern = std::make_unique<DepthPattern>(session, device.Get(), input12.get(), true);
            std::vector<ComPtr<ID3D11RenderTargetView>> views;
            if (!api12) {
                for (const auto& image : colour.images) {
                    for (UINT eye = 0; eye < 2; ++eye) {
                        D3D11_RENDER_TARGET_VIEW_DESC desc{};
                        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                        desc.Texture2DArray.FirstArraySlice = eye;
                        desc.Texture2DArray.ArraySize = 1;
                        ComPtr<ID3D11RenderTargetView> view;
                        checkHr(device->CreateRenderTargetView(image.texture, &desc, &view));
                        views.push_back(view);
                    }
                }
            }
            bool overlapVerified{};
            unsigned overlapAttempts{}, overlapArmed{};
#ifndef VDXR_NATIVE_CONTROLLED_SDK
            const auto readinessDeadline = std::chrono::steady_clock::now() + 10s;
            bool nativeRenderableSeen{};
            unsigned startupFrames{};
#endif
            for (unsigned frame = 0; frame < 40;) {
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
                Regression::prebeginBeforeFrame(frame);
#endif
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState state{XR_TYPE_FRAME_STATE};
                XR_CHECK(xrWaitFrame(session, &wait, &state));
                bool qualifyingFrame = state.shouldRender != XR_FALSE;
#ifndef VDXR_NATIVE_CONTROLLED_SDK
                qualifyingFrame = nativeRenderableSeen && state.shouldRender;
                if (!qualifyingFrame) {
                    require(std::chrono::steady_clock::now() < readinessDeadline,
                            "native qualification did not obtain40 visible frames within bounded readiness window");
                    ++startupFrames;
                }
#else
                require(qualifyingFrame, "controlled VD must remain renderable");
#endif
                XrFrameBeginInfo start{XR_TYPE_FRAME_BEGIN_INFO};
                XR_CHECK(xrBeginFrame(session, &start));
                const auto image = acquireImage(colour);
                if (input12) {
                    input12->fill(image);
                } else {
                    for (unsigned eye = 0; eye < 2; ++eye) {
                        const float tint[]{eye ? .65f : .1f, .1f + frame * .005f, eye ? .1f : .65f, 1.f};
                        context->ClearRenderTargetView(views[image * 2 + eye].Get(), tint);
                    }
                }
                releaseImage(colour);
                uint32_t depthImage{};
                if (pattern) {
                    depthImage = acquireImage(pattern->images);
                    pattern->fill(context.Get(), depthImage, frame);
                    releaseImage(pattern->images);
                }
                XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
                locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locate.displayTime = state.predictedDisplayTime;
                locate.space = space;
                XrViewState tracking{XR_TYPE_VIEW_STATE};
                XrView poses[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                uint32_t count{};
                XR_CHECK(xrLocateViews(session, &locate, &tracking, 2, &count, poses));
                require(count == 2, "native headset supplies two views");
                XrCompositionLayerProjectionView eyes[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
                XrCompositionLayerDepthInfoKHR depths[2]{{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                         {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
                for (unsigned eye = 0; eye < 2; ++eye) {
                    eyes[eye].pose = poses[eye].pose;
                    eyes[eye].fov = poses[eye].fov;
                    eyes[eye].subImage = {colour.chain, {{0, 0}, {512, 512}}, eye};
                    if (pattern) {
                        depths[eye].subImage = {pattern->images.chain, pattern->rect, eye};
                        depths[eye].minDepth = 0.f;
                        depths[eye].maxDepth = 1.f;
                        depths[eye].nearZ = .1f;
                        depths[eye].farZ = 100.f;
                        eyes[eye].next = &depths[eye];
                    }
                }
                XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                layer.space = space;
                layer.viewCount = 2;
                layer.views = eyes;
                const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                XrFrameEndInfo finish{XR_TYPE_FRAME_END_INFO};
                finish.displayTime = state.predictedDisplayTime;
                finish.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                finish.layerCount = 1;
                finish.layers = &header;
                std::shared_ptr<Regression::LiveDeliveryGate> liveGate;
                std::thread completion;
                auto joinCompletion = MakeScopeGuard([&] {
                    if (liveGate && liveGate->release)
                        liveGate->releaseOrExit();
                    if (completion.joinable())
                        completion.join();
                    if (liveGate && liveGate->sourceMarker) {
                        wil::unique_handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
                        if (!event || FAILED(liveGate->sourceMarker->SetEventOnCompletion(1, event.get())) ||
                            WaitForSingleObject(event.get(), 5000) != WAIT_OBJECT_0 ||
                            liveGate->sourceMarker->GetCompletedValue() != 1)
                            std::_Exit(4);
                    }
                });
                if (qualifyingFrame && frame >= 8 && !overlapVerified
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
                    && false // This executable exercises the earlier held-Begin gate instead.
#endif
                ) {
                    ++overlapAttempts;
                    liveGate = std::make_shared<Regression::LiveDeliveryGate>();
                    const auto registered = [&] {
                        std::lock_guard lock(NativeEndObserver::mutex);
                        const auto last = NativeEndObserver::ends.empty() ? -1LL : NativeEndObserver::ends.back().id;
                        if (last >= liveGate->pendingNativeId)
                            return false;
                        NativeEndObserver::gate = liveGate;
                        return true;
                    };
                    const bool armed = Regression::armLiveDeliveryGate(*liveGate, registered);
                    std::cout << "NATIVE_OVERLAP_ARM frame=" << frame << " active=" << liveGate->activeAtArm
                              << " owned=" << liveGate->ownedAtArm << " ready=" << liveGate->readyAtArm
                              << " admission_wake=" << liveGate->admissionWake
                              << " ready_before_lease=" << liveGate->readyBeforeLease
                              << " mailbox_empty=" << liveGate->mailboxEmptyAtArm
                              << " stopping=" << liveGate->stoppingAtArm
                              << " admission_expired=" << liveGate->admissionExpired
                              << " registered=" << liveGate->gateRegistered << " armed=" << armed << '\n';
                    if (armed) {
                        ++overlapArmed;
                        completion = std::thread([liveGate] {
                            auto alwaysRelease = MakeScopeGuard([&] { liveGate->releaseOrExit(); });
                            try {
                                std::unique_lock lock(NativeEndObserver::mutex);
                                const bool observed = NativeEndObserver::deliveryChanged.wait_for(
                                    lock, 250ms, [&] { return liveGate->endObserved; });
                                liveGate->endNotified = observed;
                                lock.unlock();
                                wil::unique_handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
                                liveGate->eventCreated = event.get() != nullptr;
                                if (observed && event) {
                                    liveGate->eventRegistered =
                                        SUCCEEDED(liveGate->presentationMarker->SetEventOnCompletion(1, event.get()));
                                    if (liveGate->eventRegistered)
                                        liveGate->eventCompleted =
                                            WaitForSingleObject(event.get(), 100) == WAIT_OBJECT_0;
                                }
                                if (observed) {
                                    // End's condition-variable notification publishes these worker-written fields.
                                    liveGate->futureSampled = true;
                                    liveGate->acceptedAtObservation = liveGate->futureAccepted;
                                    liveGate->pendingAtObservation = liveGate->futurePendingAtEnd;
                                }
                                // Preserve the original short-circuited fence reads; unknown observations stay
                                // UINT64_MAX.
                                if (liveGate->eventCompleted) {
                                    liveGate->presentationValueAtObservation =
                                        liveGate->presentationMarker->GetCompletedValue();
                                    if (liveGate->presentationValueAtObservation == 1) {
                                        liveGate->sourceValueAtObservation =
                                            liveGate->sourceMarker->GetCompletedValue();
                                        if (liveGate->sourceValueAtObservation == 0 && liveGate->futureAccepted &&
                                            liveGate->futurePendingAtEnd && liveGate->futureOutput) {
                                            liveGate->futureDeviceValueAtObservation =
                                                liveGate->futureOutput->GetCompletedValue();
                                            if (liveGate->futureDeviceValueAtObservation != UINT64_MAX)
                                                liveGate->futureValueAtObservation =
                                                    liveGate->futureOutput->GetCompletedValue();
                                        }
                                    }
                                }
                                liveGate->independentCompletion =
                                    observed && liveGate->eventCreated && liveGate->eventRegistered &&
                                    liveGate->eventCompleted && liveGate->presentationValueAtObservation == 1 &&
                                    liveGate->sourceValueAtObservation == 0 && liveGate->acceptedAtObservation &&
                                    liveGate->pendingAtObservation && liveGate->futureOutput &&
                                    liveGate->futureDeviceValueAtObservation != UINT64_MAX &&
                                    liveGate->futureValueAtObservation < liveGate->futureOutputValue;
                                liveGate->completed = ovr_GetTimeInSeconds();
                            } catch (...) {
                                liveGate->completionError = true;
                            }
                        });
                    } else {
                        liveGate->releaseOrExit();
                        std::lock_guard lock(NativeEndObserver::mutex);
                        NativeEndObserver::gate.reset();
                    }
                }
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
                Regression::prebeginBeforeEnd(frame);
#endif
                XR_CHECK(xrEndFrame(session, &finish));
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
                Regression::prebeginAfterEnd(frame);
#endif
                if (completion.joinable()) {
                    completion.join();
                    overlapVerified = liveGate->independentCompletion;
                    std::cout << "NATIVE_OVERLAP nativeId=" << liveGate->pendingNativeId
                              << " active_at_arm=" << liveGate->activeAtArm << " owned_at_arm=" << liveGate->ownedAtArm
                              << " admission_ready=" << liveGate->readyAtArm
                              << " mailbox_empty_at_arm=" << liveGate->mailboxEmptyAtArm
                              << " end_notified=" << liveGate->endNotified
                              << " P_event_created=" << liveGate->eventCreated
                              << " P_event_registered=" << liveGate->eventRegistered
                              << " P_event_completed=" << liveGate->eventCompleted
                              << " P_value=" << liveGate->presentationValueAtObservation
                              << " S_value=" << liveGate->sourceValueAtObservation
                              << " future_sampled=" << liveGate->futureSampled
                              << " future_accepted=" << liveGate->acceptedAtObservation
                              << " future_pending=" << liveGate->pendingAtObservation
                              << " P_completed_while_S_held=" << overlapVerified
                              << " future_device_value=" << liveGate->futureDeviceValueAtObservation
                              << " future_value=" << liveGate->futureValueAtObservation
                              << " completion_error=" << liveGate->completionError
                              << " elapsed_ms=" << (liveGate->completed - liveGate->armed) * 1000 << '\n';
                    std::lock_guard lock(NativeEndObserver::mutex);
                    NativeEndObserver::gate.reset();
                }
                if (nr && qualifyingFrame && frame == 1)
                    Regression::observeSpatial(true);
#ifdef VDXR_NATIVE_CONTROLLED_SDK
                if (frame <= 1) {
                    Regression::drain();
                    controlledClock->reanchorAfterWarmup();
                }
#endif
#ifndef VDXR_NATIVE_CONTROLLED_SDK
                if (!nativeRenderableSeen && state.shouldRender) {
                    std::lock_guard lock(NativeEndObserver::mutex);
                    const bool validTexturedEnd = !NativeEndObserver::failures &&
                                                  std::any_of(NativeEndObserver::ends.begin(),
                                                              NativeEndObserver::ends.end(),
                                                              [](const auto& entry) { return entry.result >= 0; });
                    if (validTexturedEnd) {
                        nativeRenderableSeen = true;
                        std::cout << "NATIVE_TEXTURED_STARTUP startup_frames=" << startupFrames
                                  << " visible=1 textured_End=1\n";
                    }
                }
#endif
                if (qualifyingFrame)
                    ++frame;
            }
#ifndef VDXR_NATIVE_CONTROLLED_SDK
            require(nativeRenderableSeen, "native qualification requires actual SDK visibility");
            std::cout << "NATIVE_VISIBILITY_QUALIFICATION visible_frames=40 nonqualifying_frames=" << startupFrames
                      << '\n';
#endif
            std::cout << "NATIVE_OVERLAP_SUMMARY attempts=" << overlapAttempts << " armed=" << overlapArmed
                      << " verified=" << overlapVerified << '\n';
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
            Regression::drain();
            Regression::prebeginVerify();
#else
            require(overlapArmed > 0, "native trial must arm the overlapping producer gate at least once");
            require(overlapVerified, "native trial must exercise a real overlapping blocked producer");
#endif
            Regression::verifyNativeDelivery(nr);
            if (nr)
                require(Regression::successfulNrEvaluations[0] >= 30 && Regression::successfulNrEvaluations[1] >= 30 &&
                            !Regression::failedNrEvaluations[0] && !Regression::failedNrEvaluations[1] &&
                            !Regression::invalidNrParameters,
                        "both actual Ada B evaluations run with the intended90/85 and appearance parameters");
            else
                require(!Regression::nrEvaluations[0] && !Regression::nrEvaluations[1], "NR off performs no inference");
            Regression::observeSpatial(false);
        }
        XR_CHECK(destroy(session));
        session = XR_NULL_HANDLE;
        observer.detach();
#ifdef VDXR_NATIVE_CONTROLLED_SDK
        controlledClock.reset();
#endif
        XR_CHECK(xrDestroyInstance(instance));
        instance = XR_NULL_HANDLE;
        require(!observer.failures && observer.ends.size() >= 40,
                "native End results, selected-chain commits and layer metadata pass");
        std::ofstream lifecycle(std::filesystem::path(argv[2]) / "native-lifecycle.csv");
        lifecycle << "phase,id,chain,result,enter_seconds,return_seconds\n";
        std::map<long long, unsigned> stages;
        unsigned commits{};
        for (const auto& call : observer.calls) {
            lifecycle << call.phase << ',' << call.id << ',' << call.chain << ',' << call.result << ','
                      << std::setprecision(17) << call.enter << ',' << call.returned << '\n';
            if (call.phase == 'W') {
                if (call.result == ovrError_NotInitialized)
                    continue;
                require(call.result >= 0 || call.result == ovrError_Timeout,
                        "native Wait succeeds or explicitly times out");
                require(stages[call.id] == 0, "native Wait precedes a unique Begin");
                stages[call.id] = 1;
            } else if (call.phase == 'B') {
                require(call.result >= 0 && stages[call.id] == 1, "native Begin follows the same completed Wait");
                stages[call.id] = 2;
            } else if (call.phase == 'E') {
                require(call.result >= 0 && stages[call.id] == 2, "native End follows the same Begin exactly once");
                stages[call.id] = 3;
            } else {
                require(call.result >= 0, "every actual native Commit succeeds");
                ++commits;
            }
        }
        require(lifecycle.good() && commits >= 40, "real native lifecycle and Commit evidence is complete");
        std::ofstream csv(std::filesystem::path(argv[2]) / "native-ends.csv");
        csv << "id,return_seconds,sensor_seconds,role,result\n";
        std::array<unsigned, 5> roles{};
        unsigned complete{};
        std::set<double> distinctGroups;
        const std::vector<unsigned> expected = requestedFrameGeneration == 2   ? std::vector<unsigned>{0, 2}
                                               : requestedFrameGeneration == 3 ? std::vector<unsigned>{0, 1, 2}
                                                                               : std::vector<unsigned>{0, 1, 3, 2};
        for (size_t i = 0; i < observer.ends.size(); ++i) {
            const auto& entry = observer.ends[i];
            ++roles.at(entry.role);
            csv << entry.id << ',' << std::setprecision(17) << entry.returned << ',' << entry.sample << ','
                << entry.role << ',' << entry.result << '\n';
            if (i)
                require(entry.id == observer.ends[i - 1].id + 1, "native frame IDs are consecutive");
            if (i + 1 >= expected.size()) {
                const auto start = i + 1 - expected.size();
                bool matches = true;
                for (size_t j = 0; j < expected.size(); ++j)
                    matches &=
                        observer.ends[start + j].role == expected[j] && observer.ends[start + j].sample == entry.sample;
                matches &= entry.sample > 0 && std::isfinite(entry.sample);
                complete += matches;
                if (matches)
                    distinctGroups.insert(entry.sample);
            }
        }
        require(csv.good() && complete >= 4 && distinctGroups.size() >= 4,
                "real native End receives complete consecutive requested S/B groups");
        std::cout << "NATIVE_ROLES S1=" << roles[0] << " S2=" << roles[1] << " S3=" << roles[3] << " B=" << roles[2]
                  << " complete=" << complete << '\n';
#ifdef VDXR_NATIVE_CONTROLLED_SDK
        std::cout << "PASS: controlled SDK full frame lifecycle, presentation, ownership and teardown; Ends="
#else
        std::cout << "PASS: real VD SDK full frame lifecycle, native presentation, ownership and teardown; Ends="
#endif
                  << observer.ends.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (session && destroy)
            try {
                checkXr(destroy(session), "failure native session cleanup");
            } catch (...) {
                std::_Exit(2);
            }
        if (instance && xrDestroyInstance)
            try {
                checkXr(xrDestroyInstance(instance), "failure native instance cleanup");
            } catch (...) {
                std::_Exit(2);
            }
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
