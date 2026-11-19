// Host protocol test with real public EndFrame/app/S/NR GPU queues and a
// source-built NR_Test facade. Identity copy replaces model inference; this
// fixture makes no NVIDIA/vendor image-quality claim.
#define VDXR_FAILED_END_SOURCE_HELPERS_ONLY
#include "fg_failed_end_source_regression.cpp"
#undef VDXR_FAILED_END_SOURCE_HELPERS_ONLY

namespace {
    struct IdentityNrBoundary {
        using Evaluate = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
        inline static Evaluate original{};
        inline static IdentityNrBoundary* active{};
        HMODULE module{};
        std::array<unsigned, 2> evaluations{};
        unsigned invalid{};

        static NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D12GraphicsCommandList* commands,
                                                    const NVSDK_NGX_Handle* feature,
                                                    const NVSDK_NGX_Parameter* parameters,
                                                    PFN_NVSDK_NGX_ProgressCallback callback) {
            const auto result = original(commands, feature, parameters, callback);
            if (NVSDK_NGX_FAILED(result))
                return result;
            // Keep the facade's real live-feature validation, then record the
            // same bounded identity-copy contract used by the existing
            // nr_spatial_runtime_regression.cpp source-facade observer.
            ID3D12Resource *source{}, *destination{};
            int x{}, y{}, width{}, height{}, outputX{}, outputY{};
            const bool valid =
                commands && parameters && NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.Color", &source)) && source &&
                NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.Output", &destination)) && destination &&
                source != destination && NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.ColorSubrectBaseX", &x)) &&
                NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.ColorSubrectBaseY", &y)) &&
                NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.ColorSubrectWidth", &width)) &&
                NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.ColorSubrectHeight", &height)) &&
                NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.OutputSubrectBaseX", &outputX)) &&
                NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.OutputSubrectBaseY", &outputY)) && x >= 0 && y >= 0 &&
                width == 512 && height == 512 && (outputX == 0 || outputX == 512) && outputY == 0;
            if (!valid) {
                ++active->invalid;
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
            }
            ++active->evaluations[outputX ? 1 : 0];
            D3D12_RESOURCE_BARRIER barriers[]{
                CD3DX12_RESOURCE_BARRIER::Transition(
                    source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(
                    destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
            commands->ResourceBarrier(2, barriers);
            D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                to{destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            const D3D12_BOX box{UINT(x), UINT(y), 0, UINT(x + width), UINT(y + height), 1};
            commands->CopyTextureRegion(&to, outputX, outputY, 0, &from, &box);
            for (auto& barrier : barriers)
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands->ResourceBarrier(2, barriers);
            return result;
        }
        explicit IdentityNrBoundary(const char* backend) {
            const auto path = std::filesystem::absolute(backend) / L"nvngx_dlssnr.dll";
            module =
                LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            const auto variant =
                module ? reinterpret_cast<uint32_t(__cdecl*)()>(GetProcAddress(module, "NR_Test_GetVariant")) : nullptr;
            require(variant && variant() == 2,
                    "private source-built NR_Test variant 2 is a prerequisite, not vendor NR");
            original = reinterpret_cast<Evaluate>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
            require(original && !active, "source facade exposes one observed evaluation boundary");
            active = this;
            checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
            checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&original), evaluate)));
            checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        }
        ~IdentityNrBoundary() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), evaluate) || DetourTransactionCommit())
                std::_Exit(2);
            active = nullptr;
            FreeLibrary(module);
        }
    };

    struct AcceptedSubmitBoundary {
        using Submit = decltype(VDXRFG_ApiV3::Submit);
        inline static Submit original{};
        inline static AcceptedSubmitBoundary* active{};
        HMODULE module{};
        unsigned accepted{};
        uint64_t lastToken{}, lastSerial{};
        static HRESULT WINAPI submit(void* context,
                                     const VDXRFG_SubmitInfoV3* input,
                                     VDXRFG_TicketV1* ticket) noexcept {
            const auto result = original(context, input, ticket);
            if (result == S_OK && ticket && ticket->token) {
                ++active->accepted;
                active->lastToken = ticket->token;
                active->lastSerial = ticket->current_serial;
            }
            return result;
        }
        explicit AcceptedSubmitBoundary(const char* backend) {
            const auto path = std::filesystem::absolute(backend) / L"vdxr-frame-generation.dll";
            module =
                LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            const auto getApi =
                module ? reinterpret_cast<VDXRFG_GetApiV3Fn>(GetProcAddress(module, "VDXRFG_GetApiV3")) : nullptr;
            VDXRFG_ApiV3 api{sizeof(api), VDXRFG_API_VERSION_3};
            require(getApi && getApi(&api) == S_OK && api.Submit, "working private FG V3 module is a prerequisite");
            original = api.Submit; // V3/V4 host dispatch uses this exact submitV3 entrypoint.
            require(!active, "one accepted FG submit observer");
            active = this;
            checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
            checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&original), submit)));
            checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        }
        ~AcceptedSubmitBoundary() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), submit) || DetourTransactionCommit())
                std::_Exit(2);
            active = nullptr;
            FreeLibrary(module);
        }
    };
} // namespace

int main(int argc, char** argv) {
    XrInstance instance{};
    try {
        require(argc == 4, "usage: private-facade-and-FG-directory state-directory d3d11|d3d12");
        const bool d3d12 = !strcmp(argv[3], "d3d12");
        require(d3d12 || !strcmp(argv[3], "d3d11"), "known public graphics API");
        std::cout << std::unitbuf;
        enabled = true;
        requestedFrameGeneration = 2;
        asyncSubmissionDisabledOverride = 0;
        nrSharpenSetting = 0;
        nrUpscalingSetting = nrProcessingResolution = coverage = 100;
        preserveNrAppearance = true;
        SettingsReadHook settings;
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        const auto getProc = virtualdesktop_openxr::RuntimeInputRegression::initialize(argv[1], argv[2]);
        IdentityNrBoundary nr(argv[1]);
        AcceptedSubmitBoundary fg(argv[1]);
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[]{XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR successful source return protocol regression");
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
        // Preserve the SDK's normal Oculus identity: no delivery transport is selected.
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
        std::vector<uint32_t> pixels(512 * 512);
        const auto fill = [&](unsigned index, bool overwrite) {
            if (d3d12) {
                app12.fill(index, overwrite);
            } else {
                std::fill(pixels.begin(), pixels.end(), overwrite ? 0xff0000ffu : 0xff604020u);
                for (unsigned eye = 0; eye < 2; ++eye)
                    context->UpdateSubresource(
                        colour.images[index].texture, eye, nullptr, pixels.data(), 512 * 4, 512 * 512 * 4);
            }
        };
        XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (unsigned eye = 0; eye < 2; ++eye) {
            views[eye].pose.orientation.w = 1;
            views[eye].pose.position.x = eye ? .032f : -.032f;
            views[eye].fov = {-.8f, .8f, .8f, -.8f};
            views[eye].subImage = {colour.chain, {{0, 0}, {512, 512}}, 1u - eye};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = space;
        projection.viewCount = 2;
        projection.views = views;
        const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = 1;
        end.layers = &layer;
        const auto beginFrame = [&] {
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState state{XR_TYPE_FRAME_STATE};
            XR_CHECK(xrWaitFrame(session, &wait, &state));
            XrFrameBeginInfo frameBegin{XR_TYPE_FRAME_BEGIN_INFO};
            XR_CHECK(xrBeginFrame(session, &frameBegin));
            end.displayTime = state.predictedDisplayTime;
        };
        // Prove dependencies first. A missing/unsupported FG backend must be a
        // prerequisite failure, never reported as a source-return RED.
        for (unsigned warm = 0; warm < 6 && !fg.accepted; ++warm) {
            beginFrame();
            const auto index = acquireImage(colour);
            fill(index, false);
            releaseImage(colour);
            XR_CHECK(xrEndFrame(session, &end));
            Regression::drain();
        }
        require(fg.accepted && fg.lastToken && nr.evaluations[0] && nr.evaluations[1] && !nr.invalid,
                "prerequisite: successful facade eye copies and a real accepted FG ticket");
        Regression::verifySuccessfulNativeRoute();
        const auto beforeNr = nr.evaluations;
        const auto beforeCaptures = fg.accepted;
        const auto beforeToken = fg.lastToken;
        beginFrame();
        const auto sourceIndex = acquireImage(colour);
        fill(sourceIndex, false);
        releaseImage(colour);
        bool blocked{}, resumed{}, original{};
        {
            SourceGate gate(app12.device.Get());
            XR_CHECK(xrEndFrame(session, &end));
            require(fg.accepted == beforeCaptures + 1 && fg.lastToken && fg.lastToken != beforeToken &&
                        nr.evaluations[0] == beforeNr[0] + 1 && nr.evaluations[1] == beforeNr[1] + 1 && !nr.invalid,
                    "observed EndFrame successfully records both identity NR eyes and accepts a distinct FG token");
            Regression::verifySuccessfulNativeRoute();
            // Capture already Flushes S. Read only immutable source metadata;
            // do not acquire S while the worker settles this gated group.
            const auto resolved = Regression::queuedResolve(colour.chain, sourceIndex, 1, false);
            require(gate.fence->GetCompletedValue() == 0, "captured source read remains behind the exact GPU gate");
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
            require(reacquired, "legally reacquire and wait on the captured source before overwriting");
            fill(reusedIndex, true);
            wil::unique_handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            require(bool(event), "app overwrite completion event");
            ComPtr<ID3D11Fence> marker11;
            ComPtr<ID3D12Fence> marker12;
            if (d3d12) {
                checkHr(app12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker12)));
                checkHr(app12.commands->GetCommandQueue()->Signal(marker12.Get(), 1));
                checkHr(marker12->SetEventOnCompletion(1, event.get()));
            } else {
                ComPtr<ID3D11Device5> device5;
                ComPtr<ID3D11DeviceContext4> context4;
                checkHr(device.As(&device5));
                checkHr(context.As(&context4));
                checkHr(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker11)));
                checkHr(context4->Signal(marker11.Get(), 1));
                context4->Flush();
                checkHr(marker11->SetEventOnCompletion(1, event.get()));
            }
            const auto completed = [&] {
                return marker12 ? marker12->GetCompletedValue() : marker11->GetCompletedValue();
            };
            blocked = WaitForSingleObject(event.get(), 100) == WAIT_TIMEOUT && completed() == 0;
            gate.release();
            resumed = completed() == 1 || (WaitForSingleObject(event.get(), 5000) == WAIT_OBJECT_0 && completed() == 1);
            require(!gate.watchdogReleased, "successful EndFrame does not CPU-drain its source read");
            releaseImage(colour);
            Regression::drain();
            original = Regression::originalPixels(resolved.Get());
        }
        Regression::verifyHealthyDevices();
        XR_CHECK(xrDestroyInstance(instance));
        instance = XR_NULL_HANDLE;
        std::cout << "SUCCESS_SOURCE_RETURN api=" << argv[3] << " model=source-facade-identity-copy"
                  << " eye0=" << nr.evaluations[0] << " eye1=" << nr.evaluations[1] << " token=" << fg.lastToken
                  << " blocked=" << blocked << " resumed=" << resumed << " original=" << original << '\n';
        require(blocked && resumed && original,
                "successful captured native NR route must order application reuse after queued source reads");
        std::cout << "PASS: successful source-return GPU protocol (model stubbed)\n";
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
