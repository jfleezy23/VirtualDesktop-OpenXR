// Public failed-EndFrame source ownership with real D3D11/D3D12 queues.
// Rejection follows a queued app-image resolve, before NR evaluation/FG
// capture. Only registry reads/private SDK are isolated.
#define VDXR_NR_HELPERS_ONLY
#include "nr_input_toggle_regression.cpp"
#undef VDXR_NR_HELPERS_ONLY

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static OpenXrRuntime& runtime() {
            return *static_cast<OpenXrRuntime*>(GetInstance());
        }
        static void selectDelivery() {
            // The private SDK identifies itself as Oculus. Exercise the actual
            // VD delivery routing without loading or contacting a headset.
            runtime().m_useOculusRuntime = false;
        }
        static void verifyRoute(bool nativeNr) {
            const auto& value = runtime();
            if (nativeNr) {
                require(!value.m_deliveryTransport && value.m_useOculusRuntime && value.m_frameGenerationMode == 2 &&
                            value.m_useAsyncSubmission && value.m_precompositor.dlssnrSettings.enabled &&
                            value.m_dlssnrResourcesInitialized && value.m_dlssnrContext && !value.m_dlssnrFeature[0] &&
                            !value.m_dlssnrFeature[1] && !value.m_frameGeneration,
                        "public NR-enabled native FG route rejects before feature evaluation or FG capture");
                return;
            }
            require(value.m_deliveryTransport && value.m_frameGenerationMode == 2 && value.m_useAsyncSubmission &&
                        !value.m_precompositor.dlssnrSettings.enabled && !value.m_dlssnrContext,
                    "fresh NR-off independent FG/delivery route required");
        }
        static ComPtr<ID3D11Fence> sourceReturnFence() {
            return runtime().m_ovrSubmissionCompletionFence;
        }
        static bool sourceFailureLatched() {
            auto& value = runtime();
            std::unique_lock state(value.m_asyncSubmissionMutex);
            return bool(value.m_asyncSubmissionError);
        }
        static void verifyHealthyDevices() {
            checkHr(runtime().m_ovrSubmissionDevice->GetDeviceRemovedReason());
            if (runtime().m_d3d11Device)
                checkHr(runtime().m_d3d11Device->GetDeviceRemovedReason());
            if (runtime().m_d3d12Device)
                checkHr(runtime().m_d3d12Device->GetDeviceRemovedReason());
        }
        static void verifySuccessfulNativeRoute() {
            const auto& value = runtime();
            require(!value.m_deliveryTransport && value.m_useOculusRuntime && value.m_frameGenerationMode == 2 &&
                        value.m_useAsyncSubmission && value.m_precompositor.dlssnrSettings.enabled &&
                        value.m_dlssnrContext && value.m_frameGeneration && value.m_frameGeneration->initialized() &&
                        !value.m_precompositor.finalDepth[0] && !value.m_precompositor.finalDepth[1],
                    "successful color-only NR/FG uses the normal native route without delivery transport");
        }
        static void drain() {
            runtime().waitForAsyncSubmissionIdle();
        }
        static void blockSource(ID3D12Device* device, ID3D12Fence* gate) {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock lease(value.m_fgContextMutex);
            wil::unique_handle handle;
            checkHr(device->CreateSharedHandle(gate, nullptr, GENERIC_ALL, nullptr, handle.put()));
            ComPtr<ID3D11Fence> imported;
            checkHr(value.m_ovrSubmissionDevice->OpenSharedFence(handle.get(), IID_PPV_ARGS(&imported)));
            checkHr(value.m_ovrSubmissionContext->Wait(imported.Get(), 1));
        }
        static ComPtr<ID3D11Texture2D>
        queuedResolve(XrSwapchain handle, unsigned sourceIndex, unsigned sourceSlice, bool dispatch = true) {
            auto& value = runtime();
            std::unique_lock lease(value.m_fgContextMutex, std::defer_lock);
            if (dispatch)
                lease.lock();
            const auto& chain = *reinterpret_cast<const OpenXrRuntime::Swapchain*>(handle);
            require(chain.lastReleasedIndex == static_cast<int>(sourceIndex) &&
                        sourceSlice < chain.resolvedSlices.size(),
                    "failed EndFrame retains the exact released source index");
            const auto& slice = chain.resolvedSlices[sourceSlice];
            require(slice.hasCopyForPreprocessing && slice.lastCommittedIndex >= 0 &&
                        static_cast<size_t>(slice.lastCommittedIndex) < slice.images.size() &&
                        slice.images[slice.lastCommittedIndex].Get() != chain.appSwapchain.images[sourceIndex].Get(),
                    "rejection follows a real queued app-to-private resolve");
            // Dispatch existing runtime commands. This creates no extra source
            // read or return wait and cannot repair the behavior under test.
            if (dispatch)
                value.m_ovrSubmissionContext->Flush();
            return slice.images[slice.lastCommittedIndex];
        }
        static bool originalPixels(ID3D11Texture2D* image) {
            auto& value = runtime();
            std::unique_lock lease(value.m_fgContextMutex);
            D3D11_TEXTURE2D_DESC desc{};
            image->GetDesc(&desc);
            desc.BindFlags = desc.MiscFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            checkHr(value.m_ovrSubmissionDevice->CreateTexture2D(&desc, nullptr, &staging));
            value.m_ovrSubmissionContext->CopyResource(staging.Get(), image);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checkHr(value.m_ovrSubmissionContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            const auto* pixel = static_cast<const uint8_t*>(mapped.pData);
            const bool original = pixel[0] == 32 && pixel[1] == 64 && pixel[2] == 96 && pixel[3] == 255;
            std::cout << "RESOLVE_PIXEL " << unsigned(pixel[0]) << ',' << unsigned(pixel[1]) << ','
                      << unsigned(pixel[2]) << ',' << unsigned(pixel[3]) << '\n';
            value.m_ovrSubmissionContext->Unmap(staging.Get(), 0);
            return original;
        }
    };
} // namespace virtualdesktop_openxr

namespace {
    struct SourceExportFailure {
        using Export = HRESULT(STDMETHODCALLTYPE*)(ID3D11Fence*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*);
        inline static Export original{};
        inline static thread_local SourceExportFailure* active{};
        ComPtr<ID3D11Fence> fence;
        unsigned injections{};

        static HRESULT STDMETHODCALLTYPE
        fail(ID3D11Fence* fence, const SECURITY_ATTRIBUTES* attributes, DWORD access, LPCWSTR name, HANDLE* output) {
            if (active && active->fence.Get() == fence) {
                ++active->injections;
                *output = nullptr;
                return E_OUTOFMEMORY;
            }
            return original(fence, attributes, access, name, output);
        }
        SourceExportFailure() : fence(virtualdesktop_openxr::RuntimeFrameGenerationRegression::sourceReturnFence()) {
            require(!active && fence, "one exact source-return export fault");
            // ID3D11Fence inherits IUnknown (3) and ID3D11DeviceChild (4)
            // methods; CreateSharedHandle is its first added vtable method.
            original = reinterpret_cast<Export>((*reinterpret_cast<void***>(fence.Get()))[7]);
            checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
            checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&original), fail)));
            checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
            active = this;
        }
        ~SourceExportFailure() {
            active = nullptr;
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), fail) || DetourTransactionCommit())
                std::_Exit(2);
        }
    };

    struct SourceGate {
        ComPtr<ID3D12Fence> fence;
        std::mutex mutex;
        std::condition_variable ready;
        bool releaseRequested{}, watchdogReleased{};
        std::thread worker;

        explicit SourceGate(ID3D12Device* device) {
            checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::blockSource(device, fence.Get());
            worker = std::thread([this] {
                std::unique_lock lock(mutex);
                watchdogReleased = !ready.wait_for(lock, 5s, [&] { return releaseRequested; });
                lock.unlock();
                if (FAILED(fence->Signal(1)))
                    std::_Exit(3);
            });
        }
        void release() {
            {
                std::lock_guard lock(mutex);
                releaseRequested = true;
                ready.notify_all();
            }
            if (worker.joinable())
                worker.join();
        }
        ~SourceGate() {
            release();
        }
    };

    struct App12 {
        ComPtr<ID3D12Device> device;
        std::unique_ptr<D3D12Utils::CommandContext> commands;
        std::vector<XrSwapchainImageD3D12KHR> images;
        ComPtr<ID3D12DescriptorHeap> targets;
        UINT stride{};

        explicit App12(IDXGIAdapter* adapter) {
            checkHr(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
            commands = std::make_unique<D3D12Utils::CommandContext>(device.Get(), L"failed EndFrame app");
        }
        void attach(XrSwapchain chain) {
            uint32_t count{};
            XR_CHECK(xrEnumerateSwapchainImages(chain, 0, &count, nullptr));
            images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            XR_CHECK(xrEnumerateSwapchainImages(
                chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = count;
            checkHr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&targets)));
            stride = device->GetDescriptorHandleIncrementSize(desc.Type);
            auto handle = targets->GetCPUDescriptorHandleForHeapStart();
            for (const auto& image : images) {
                D3D12_RENDER_TARGET_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.ArraySize = 2;
                device->CreateRenderTargetView(image.texture, &view, handle);
                handle.ptr += stride;
            }
        }
        void fill(unsigned index, bool overwrite) {
            auto list = commands->GetCommandList();
            // XR_KHR_D3D12_enable guarantees a color image state match with
            // RENDER_TARGET after wait and requires that state on release.
            auto target = targets->GetCPUDescriptorHandleForHeapStart();
            target.ptr += index * stride;
            const float original[]{32.f / 255, 64.f / 255, 96.f / 255, 1.f};
            const float red[]{1.f, 0.f, 0.f, 1.f};
            list.Commands->ClearRenderTargetView(target, overwrite ? red : original, 0, nullptr);
            commands->SubmitCommandList(std::move(list));
        }
    };
} // namespace

#ifndef VDXR_FAILED_END_SOURCE_HELPERS_ONLY
int main(int argc, char** argv) {
    XrInstance instance{};
    try {
        require(argc == 5,
                "usage: private-SDK-directory state-directory d3d11|d3d12 "
                "first-rect|second-rect|trailing-layer|native-nr-first-rect|source-export-failure");
        const bool d3d12 = !strcmp(argv[3], "d3d12");
        require(d3d12 || !strcmp(argv[3], "d3d11"), "known public graphics API");
        const std::string scenario = argv[4];
        const bool nativeNr = scenario == "native-nr-first-rect";
        const bool exportFailure = scenario == "source-export-failure";
        require(nativeNr || exportFailure || scenario == "first-rect" || scenario == "second-rect" ||
                    scenario == "trailing-layer",
                "known recoverable EndFrame rejection");
        std::cout << std::unitbuf;
        enabled = nativeNr;
        requestedFrameGeneration = 2;
        asyncSubmissionDisabledOverride = 0;
        nrSharpenSetting = 0;
        nrUpscalingSetting = 100;
        preserveNrAppearance = true;
        SettingsReadHook settings;
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        const auto getProc = virtualdesktop_openxr::RuntimeInputRegression::initialize(argv[1], argv[2]);
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[]{XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR failed EndFrame source regression");
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
        if (!nativeNr)
            Regression::selectDelivery();
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(instance, system, &requirements));
        ComPtr<IDXGIFactory1> factory;
        checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            checkHr(factory->EnumAdapters1(i, &adapter));
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
                                  0,
                                  &requirements.minFeatureLevel,
                                  1,
                                  D3D11_SDK_VERSION,
                                  &device,
                                  nullptr,
                                  &context));
        // Also supplies a CPU-signallable shared gate for the D3D11 scenario.
        App12 app12(adapter.Get());
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.Get();
        XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        if (d3d12) {
            PFN_xrGetD3D12GraphicsRequirementsKHR requirementsProc{};
            XR_CHECK(getProc(instance,
                             "xrGetD3D12GraphicsRequirementsKHR",
                             reinterpret_cast<PFN_xrVoidFunction*>(&requirementsProc)));
            XrGraphicsRequirementsD3D12KHR requirements12{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
            XR_CHECK(requirementsProc(instance, system, &requirements12));
            binding12.device = app12.device.Get();
            binding12.queue = app12.commands->GetCommandQueue();
        }
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
        create.next = d3d12 ? static_cast<const void*>(&binding12) : static_cast<const void*>(&binding);
        create.systemId = system;
        XrSession session{};
        XR_CHECK(xrCreateSession(instance, &create, &session));
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XR_CHECK(xrBeginSession(session, &begin));
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        reference.poseInReferenceSpace.orientation.w = 1;
        XrSpace space{};
        XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
        auto colour = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, false, !d3d12);
        if (d3d12)
            app12.attach(colour.chain);
        const unsigned count = static_cast<unsigned>(d3d12 ? app12.images.size() : colour.images.size());
        require(count > 1, "rotating application swapchain required");
        const auto sourceIndex = acquireImage(colour);
        std::vector<uint32_t> pixels(512 * 512, 0xff604020u);
        const auto fill11 = [&](unsigned index, bool overwrite) {
            std::fill(pixels.begin(), pixels.end(), overwrite ? 0xff0000ffu : 0xff604020u);
            for (unsigned eye = 0; eye < 2; ++eye)
                context->UpdateSubresource(
                    colour.images[index].texture, eye, nullptr, pixels.data(), 512 * 4, 512 * 512 * 4);
        };
        if (d3d12)
            app12.fill(sourceIndex, false);
        else
            fill11(sourceIndex, false);
        releaseImage(colour);
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session, &wait, &state));
        XrFrameBeginInfo frameBegin{XR_TYPE_FRAME_BEGIN_INFO};
        XR_CHECK(xrBeginFrame(session, &frameBegin));
        XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (unsigned eye = 0; eye < 2; ++eye) {
            views[eye].pose.orientation.w = 1;
            views[eye].pose.position.x = eye ? .032f : -.032f;
            views[eye].fov = {-.8f, .8f, .8f, -.8f};
            // Slice one forces a real resolve on the native route too; slice
            // zero may otherwise alias the application's direct SDK chain.
            views[eye].subImage = {colour.chain, {{0, 0}, {512, 512}}, nativeNr ? 1u - eye : eye};
        }
        if (scenario != "trailing-layer")
            views[scenario == "first-rect" || nativeNr || exportFailure ? 0 : 1].subImage.imageRect.extent.width = 513;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = space;
        projection.viewCount = 2;
        projection.views = views;
        const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
                                                     nullptr};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = scenario == "trailing-layer" ? 2 : 1;
        end.layers = layers;
        bool protectedReuse{}, resumed{}, original{};
        {
            SourceGate gate(app12.device.Get());
            std::unique_ptr<SourceExportFailure> exportFault;
            if (exportFailure)
                exportFault = std::make_unique<SourceExportFailure>();
            const auto result = xrEndFrame(session, &end);
            require(result == (scenario == "trailing-layer" ? XR_ERROR_LAYER_INVALID : XR_ERROR_SWAPCHAIN_RECT_INVALID),
                    "recoverable EndFrame rejection result preserved");
            Regression::verifyRoute(nativeNr);
            const auto resolved =
                Regression::queuedResolve(colour.chain, sourceIndex, views[0].subImage.imageArrayIndex);
            require(gate.fence->GetCompletedValue() == 0, "source read remains behind the exact unreleased GPU gate");
            if (exportFailure) {
                require(exportFault->injections == 1 && Regression::sourceFailureLatched(),
                        "exact source-return export OOM reaches the existing asynchronous error latch");
                Regression::verifyHealthyDevices();
                uint32_t nextIndex{};
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XR_CHECK(xrAcquireSwapchainImage(colour.chain, &acquire, &nextIndex));
                XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                imageWait.timeout = 0;
                const auto waited = xrWaitSwapchainImage(colour.chain, &imageWait);
                // No overwrite is issued after a failed return handoff. This
                // checks whether the runtime falsely grants image ownership.
                const bool refused = XR_FAILED(waited);
                gate.release();
                require(!gate.watchdogReleased, "source export fault never CPU-drains the pending read");
                exportFault.reset();
                if (XR_SUCCEEDED(waited))
                    releaseImage(colour);
                XR_CHECK(xrDestroyInstance(instance));
                instance = XR_NULL_HANDLE;
                std::cout << "SOURCE_EXPORT_FAILURE api=" << argv[3] << " injected=1 healthy=1 wait=" << waited
                          << " refused=" << refused << '\n';
                require(refused, "failed source return must refuse subsequent swapchain image wait until teardown");
                std::cout << "PASS: source-return export failure refuses image ownership\n";
                return 0;
            }
            // Reacquire and wait before touching the source. Cycling unused
            // indices is legal and makes the ownership trigger independent of
            // the implementation's number of swapchain images.
            unsigned reusedIndex{};
            bool reacquired{};
            for (unsigned attempt = 0; attempt < count + 1; ++attempt) {
                reusedIndex = acquireImage(colour);
                if (reusedIndex == sourceIndex) {
                    reacquired = true;
                    break;
                }
                releaseImage(colour);
            }
            require(reacquired, "successfully acquire and wait on the same released source image");
            wil::unique_handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            require(bool(event), "application overwrite marker event");
            ComPtr<ID3D11Fence> marker11;
            ComPtr<ID3D12Fence> marker12;
            if (d3d12) {
                app12.fill(reusedIndex, true);
                checkHr(app12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker12)));
                checkHr(app12.commands->GetCommandQueue()->Signal(marker12.Get(), 1));
                checkHr(marker12->SetEventOnCompletion(1, event.get()));
            } else {
                fill11(reusedIndex, true);
                ComPtr<ID3D11Device5> device5;
                ComPtr<ID3D11DeviceContext4> context4;
                checkHr(device.As(&device5));
                checkHr(context.As(&context4));
                checkHr(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker11)));
                checkHr(context4->Signal(marker11.Get(), 1));
                context4->Flush();
                checkHr(marker11->SetEventOnCompletion(1, event.get()));
            }
            protectedReuse = WaitForSingleObject(event.get(), 100) == WAIT_TIMEOUT &&
                             (marker12 ? marker12->GetCompletedValue() : marker11->GetCompletedValue()) == 0;
            gate.release();
            const auto markerCompleted = [&] {
                return marker12 ? marker12->GetCompletedValue() : marker11->GetCompletedValue();
            };
            // A missing return dependency can let the initial observation
            // consume this auto-reset event. Fence completion remains authority.
            resumed = markerCompleted() == 1 ||
                      (WaitForSingleObject(event.get(), 5000) == WAIT_OBJECT_0 && markerCompleted() == 1);
            require(!gate.watchdogReleased, "failed EndFrame returns without waiting for source completion on the CPU");
            original = Regression::originalPixels(resolved.Get());
            releaseImage(colour);
        }
        // Settle the still-begun frame without resubmitting rejected layers.
        end.layerCount = 0;
        end.layers = nullptr;
        XR_CHECK(xrEndFrame(session, &end));
        XR_CHECK(xrDestroyInstance(instance));
        instance = XR_NULL_HANDLE;
        std::cout << "FAILED_END_SOURCE api=" << argv[3] << " case=" << scenario << " blocked=" << protectedReuse
                  << " resumed=" << resumed << " original=" << original << '\n';
        require(protectedReuse, "failed EndFrame must GPU-order legal application image reuse after its queued reads");
        require(resumed && original, "source resolve keeps original pixels and the same application marker resumes");
        std::cout << "PASS: failed EndFrame preserves source ownership without a CPU drain\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (instance && xrDestroyInstance) {
            try {
                XR_CHECK(xrDestroyInstance(instance));
            } catch (...) {
                std::_Exit(2);
            }
        }
        return 1;
    }
}
#endif
