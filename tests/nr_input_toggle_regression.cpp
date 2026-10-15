// Runtime-linked live settings/import guard regression, with native OVRNull and real NR.
// Reuse the retained depth fixture's geometry, native commit tracking, and depth readback.
#include "pch.h"
#include "runtime.h"
#define main retainedDepthFixtureMain
#include "nr_depth_regression.cpp"
#undef main
#pragma comment(lib, "d3dcompiler.lib")

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    std::atomic<DWORD> enabled{};
    DWORD coverage = 100;
    DWORD requestedFrameGeneration{}, nrSharpenSetting{}, nrUpscalingSetting{100};
    bool expectDepth[2] = {true, true};
    bool readSubmittedColour = true;
    bool checkOffColourEdges = false;
    std::array<std::array<unsigned, 4>, 2> expectedOffColour{{{32, 64, 96, 255}, {32, 64, 96, 255}}};
    std::function<void(const ovrLayerEyeFov&)> observeProjection;
    std::function<void(long long)> observeNativeEnd;
    std::function<void(ovrTextureSwapChain)> observeNativeCommit;
    ovrResult OVR_CDECL captureCommit(ovrSession session, ovrTextureSwapChain chain) {
        const auto result = hookCommit(session, chain);
        if (OVR_SUCCESS(result) && observeNativeCommit) {
            observeNativeCommit(chain);
        }
        return result;
    }
    std::wstring settingsKey;
    decltype(&RegGetValueW) originalRead = RegGetValueW;

    LSTATUS WINAPI
    readSettings(HKEY key, LPCWSTR subkey, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
        if (key != HKEY_LOCAL_MACHINE || !subkey || settingsKey != subkey)
            return originalRead(key, subkey, name, flags, type, data, size);
        if (!name)
            return ERROR_FILE_NOT_FOUND;
        DWORD value{};
        const std::wstring_view setting(name);
        if (setting == L"DLSSNR_Enabled")
            value = enabled.load();
        else if (setting == L"FoveationSize")
            value = coverage;
        else if (setting == L"upscaling")
            value = nrUpscalingSetting;
        else if (setting == L"FrameGenerationMode")
            value = requestedFrameGeneration;
        else if (setting == L"sharpen")
            value = nrSharpenSetting;
        else if (setting == L"quirk_disable_async_submission")
            value = requestedFrameGeneration ? 0 : 1;
        else if (setting == L"quirk_use_depth")
            value = 1;
        else
            return ERROR_FILE_NOT_FOUND;
        if (type)
            *type = REG_DWORD;
        const auto requestedTypes = flags & RRF_RT_ANY;
        if (requestedTypes && !(requestedTypes & RRF_RT_REG_DWORD))
            return ERROR_UNSUPPORTED_TYPE;
        if (!size)
            return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
        const DWORD capacity = *size;
        *size = sizeof(value);
        if (!data)
            return ERROR_SUCCESS;
        if (capacity < sizeof(value)) {
            if (flags & RRF_ZEROONFAILURE)
                memset(data, 0, capacity);
            return ERROR_MORE_DATA;
        }
        memcpy(data, &value, sizeof(value));
        return ERROR_SUCCESS;
    }

    class SettingsReadHook {
      public:
        SettingsReadHook() {
            settingsKey = xr::utf8_to_wide(virtualdesktop_openxr::RegPrefix);
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalRead), readSettings);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            require(result == NO_ERROR, "Read-only synthetic settings hook installation failed");
        }
        ~SettingsReadHook() {
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalRead), readSettings);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (result != NO_ERROR) {
                std::cerr << "FAIL: Settings hook removal failed: " << result << '\n';
                std::terminate();
            }
        }
        SettingsReadHook(const SettingsReadHook&) = delete;
        SettingsReadHook& operator=(const SettingsReadHook&) = delete;
    };

    void readColor(ovrSession session, const ovrLayerEyeFov& layer, unsigned eye) {
        auto texture = getCommitted(session, layer.ColorTexture[eye]);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        require(desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                "Submitted color readback requires RGBA8");
        ComPtr<ID3D11Device> device;
        texture->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        desc.BindFlags = desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto& viewport = layer.Viewport[eye];
        const auto* pixel = static_cast<const uint8_t*>(mapped.pData) +
                            (viewport.Pos.y + viewport.Size.h / 2) * mapped.RowPitch +
                            (viewport.Pos.x + viewport.Size.w / 2) * 4;
        const std::array<unsigned, 4> actual{pixel[0], pixel[1], pixel[2], pixel[3]};
        bool edgesCorrect = true;
        if (checkOffColourEdges && !enabled.load()) {
            for (const auto& corner : std::array<std::array<int, 2>, 4>{{{0, 0},
                                                                         {viewport.Size.w - 1, 0},
                                                                         {0, viewport.Size.h - 1},
                                                                         {viewport.Size.w - 1, viewport.Size.h - 1}}}) {
                const auto* edge = static_cast<const uint8_t*>(mapped.pData) +
                                   (viewport.Pos.y + corner[1]) * mapped.RowPitch + (viewport.Pos.x + corner[0]) * 4;
                edgesCorrect &= std::array<unsigned, 4>{edge[0], edge[1], edge[2], edge[3]} == expectedOffColour[eye];
            }
        }
        context->Unmap(staging.Get(), 0);
        require(edgesCorrect, "NR-off cropped eye edges preserve the submitted subrectangle");
        std::cout << "native color eye=" << eye << " NR=" << enabled.load() << " rgba=" << actual[0] << ',' << actual[1]
                  << ',' << actual[2] << ',' << actual[3] << '\n';
        require(actual[3] == 255, "Submitted color alpha is incorrect");
        if (!enabled.load())
            require(actual == expectedOffColour[eye], "NR-off color pixels changed");
        checkHr(device->GetDeviceRemovedReason());
    }
    ovrResult OVR_CDECL captureEnd(ovrSession session,
                                   long long frame,
                                   const ovrViewScaleDesc* scale,
                                   const ovrLayerHeader* const* layers,
                                   unsigned count) {
        {
            std::lock_guard lock(captureMutex);
            try {
                if (observeNativeEnd) {
                    observeNativeEnd(frame);
                }
                bool found{};
                for (unsigned i = 0; i < count; ++i) {
                    if (!layers[i] ||
                        (layers[i]->Type != ovrLayerType_EyeFovDepth && layers[i]->Type != ovrLayerType_EyeFov))
                        continue;
                    require((layers[i]->Type == ovrLayerType_EyeFovDepth) == (expectDepth[0] || expectDepth[1]),
                            "Unexpected native projection depth type");
                    for (unsigned eye = 0; eye < 2; ++eye) {
                        if (expectDepth[eye])
                            checkDepth(session, *reinterpret_cast<const ovrLayerEyeFovDepth*>(layers[i]), eye);
                        if (readSubmittedColour) {
                            readColor(session, *reinterpret_cast<const ovrLayerEyeFov*>(layers[i]), eye);
                        }
                    }
                    if (observeProjection) {
                        observeProjection(*reinterpret_cast<const ovrLayerEyeFov*>(layers[i]));
                    }
                    found = true;
                    break;
                }
                require(found, "No native color/depth projection was submitted");
                ++captured;
            } catch (const std::exception& error) {
                captureFailure = error.what();
            }
        }
        const auto result = originalEnd(session, frame, scale, layers, count);
        ready.notify_all();
        return result;
    }
    class CompositorHooks {
      public:
        CompositorHooks() {
            require(GetModuleHandleExW(0, L"LibOVRRT64_1.dll", &m_module), "Private OVRNull was not loaded");
            originalEnd = reinterpret_cast<decltype(originalEnd)>(GetProcAddress(m_module, "ovr_EndFrame"));
            originalCommit =
                reinterpret_cast<decltype(originalCommit)>(GetProcAddress(m_module, "ovr_CommitTextureSwapChain"));
            getBuffer =
                reinterpret_cast<decltype(getBuffer)>(GetProcAddress(m_module, "ovr_GetTextureSwapChainBufferDX"));
            getIndex =
                reinterpret_cast<decltype(getIndex)>(GetProcAddress(m_module, "ovr_GetTextureSwapChainCurrentIndex"));
            if (!originalEnd || !originalCommit || !getBuffer || !getIndex) {
                FreeLibrary(m_module);
                throw std::runtime_error("Private OVRNull observation export missing");
            }
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalCommit), captureCommit);
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalEnd), captureEnd);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (result != NO_ERROR) {
                FreeLibrary(m_module);
                throw std::runtime_error("Native compositor observation installation failed");
            }
        }
        ~CompositorHooks() {
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalEnd), captureEnd);
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalCommit), captureCommit);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (result != NO_ERROR) {
                std::cerr << "FAIL: Native compositor observation removal failed: " << result << '\n';
                std::terminate();
            }
            FreeLibrary(m_module);
        }
        CompositorHooks(const CompositorHooks&) = delete;
        CompositorHooks& operator=(const CompositorHooks&) = delete;

      private:
        HMODULE m_module{};
    };

    XrResult XRAPI_CALL linkedDispatch(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
        return virtualdesktop_openxr::GetInstance()->xrGetInstanceProcAddr(instance, name, function);
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        struct ParameterSpy final : NVSDK_NGX_Parameter {
            NVSDK_NGX_Parameter* original;
            unsigned writes{};
            explicit ParameterSpy(NVSDK_NGX_Parameter* parameters) : original(parameters) {
            }
            void count(const char* name) {
                if (!strcmp(name, "DLSSNR.Depth") || !strcmp(name, "DLSSNR.DepthSubrectBaseX") ||
                    !strcmp(name, "DLSSNR.DepthSubrectBaseY") || !strcmp(name, "DLSSNR.DepthSubrectWidth") ||
                    !strcmp(name, "DLSSNR.DepthSubrectHeight"))
                    ++writes;
            }
#define FORWARD_PARAMETER(type)                                                                                        \
    void Set(const char* name, type value) override {                                                                  \
        count(name);                                                                                                   \
        original->Set(name, value);                                                                                    \
    }                                                                                                                  \
    NVSDK_NGX_Result Get(const char* name, type* value) const override {                                               \
        return original->Get(name, value);                                                                             \
    }
            FORWARD_PARAMETER(unsigned long long)
            FORWARD_PARAMETER(float)
            FORWARD_PARAMETER(double)
            FORWARD_PARAMETER(unsigned int)
            FORWARD_PARAMETER(int)
            FORWARD_PARAMETER(ID3D11Resource*)
            FORWARD_PARAMETER(ID3D12Resource*)
            FORWARD_PARAMETER(void*)
#undef FORWARD_PARAMETER
            void Reset() override {
                original->Reset();
            }
        };
        struct Observation {
            OpenXrRuntime& runtime;
            ParameterSpy parameters;
            decltype(&::NVSDK_NGX_D3D12_EvaluateFeature) evaluate[2];
            NVSDK_NGX_Handle* features[2];
            XrSwapchain depth{};
            bool inverted[2]{};
            bool metadata{};
            bool writes{};
            unsigned failures{}, evaluations{};
            inline static Observation* active{};
            Observation(bool checkMetadata, bool checkWrites)
                : runtime(*static_cast<OpenXrRuntime*>(GetInstance())), parameters(runtime.m_ngxParameters),
                  metadata(checkMetadata), writes(checkWrites) {
                require(parameters.original && runtime.m_dlssnrFeature[0] && runtime.m_dlssnrFeature[1],
                        "Real NR must be warm before observing parameters");
                for (unsigned eye = 0; eye < 2; ++eye) {
                    evaluate[eye] = runtime.m_dlssnrFeatureEvaluate[eye];
                    features[eye] = runtime.m_dlssnrFeature[eye];
                }
                require(features[0] != features[1], "NR eyes must have separate features");
                active = this;
                runtime.m_ngxParameters = &parameters;
                runtime.m_dlssnrFeatureEvaluate[0] = observedEvaluate<0>;
                runtime.m_dlssnrFeatureEvaluate[1] = observedEvaluate<1>;
            }
            ~Observation() {
                runtime.m_ngxParameters = parameters.original;
                for (unsigned eye = 0; eye < 2; ++eye)
                    runtime.m_dlssnrFeatureEvaluate[eye] = evaluate[eye];
                active = nullptr;
            }
            Observation(const Observation&) = delete;
            Observation& operator=(const Observation&) = delete;
            void check(bool value, const char* message, unsigned eye) {
                if (!value) {
                    ++failures;
                    std::cerr << "ASSERTION FAILED: " << message << " eye=" << eye << '\n';
                }
            }
            template <unsigned eye>
            static NVSDK_NGX_Result NVSDK_CONV observedEvaluate(ID3D12GraphicsCommandList* commands,
                                                                const NVSDK_NGX_Handle* feature,
                                                                const NVSDK_NGX_Parameter* input,
                                                                PFN_NVSDK_NGX_ProgressCallback callback) {
                auto& observation = *active;
                int inversion{}, rect[4]{};
                ID3D12Resource* resource{};
                const auto* parameters = observation.parameters.original;
                observation.check(input == &observation.parameters && feature == observation.features[eye],
                                  "Feature/parameter observation lost the real warmed identity",
                                  eye);
                observation.check(NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.DepthInverted", &inversion)),
                                  "Depth inversion parameter is missing",
                                  eye);
                observation.check(NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.Depth", &resource)),
                                  "Depth resource parameter is missing",
                                  eye);
                const char* names[] = {"DLSSNR.DepthSubrectBaseX",
                                       "DLSSNR.DepthSubrectBaseY",
                                       "DLSSNR.DepthSubrectWidth",
                                       "DLSSNR.DepthSubrectHeight"};
                for (unsigned i = 0; i < 4; ++i)
                    observation.check(NVSDK_NGX_SUCCEED(parameters->Get(names[i], &rect[i])),
                                      "Depth rectangle parameter is missing",
                                      eye);
                const bool hasDepth = expectDepth[0] && expectDepth[1];
                if (hasDepth) {
                    auto& chain = *reinterpret_cast<OpenXrRuntime::Swapchain*>(observation.depth);
                    auto& slice = chain.resolvedSlices[eye];
                    observation.check(resource == slice.dlssnrImages[slice.lastCommittedIndex].Get(),
                                      "NR bound the wrong eye/depth generation",
                                      eye);
                    const int extent = (int)xr::math::AlignTo<4>((uint32_t)(512 * (coverage * .01f)));
                    const int offset = (512 - extent) / 2;
                    observation.check(rect[0] == offset && rect[1] == offset && rect[2] == extent && rect[3] == extent,
                                      "NR depth crop differs from actual high-coverage color crop",
                                      eye);
                    if (observation.metadata)
                        observation.check(bool(inversion) == observation.inverted[eye],
                                          "NR used the other eye's depth convention",
                                          eye);
                } else {
                    observation.check(!resource && !rect[0] && !rect[1] && !rect[2] && !rect[3],
                                      "Optional depth bindings were not fully cleared",
                                      eye);
                }
                if (observation.writes)
                    observation.check(observation.parameters.writes == 5, "Redundant depth parameter writes", eye);
                std::cout << "NR parameters eye=" << eye << " inverted=" << inversion << " depth=" << hasDepth
                          << " writes=" << observation.parameters.writes << " crop=" << rect[0] << ',' << rect[1] << ','
                          << rect[2] << ',' << rect[3] << '\n';
                observation.parameters.writes = 0;
                ++observation.evaluations;
                // Private vendor implementations must receive their own allocation, never our forwarding proxy.
                return observation.evaluate[eye](commands, feature, parameters, callback);
            }
        };
        static PFN_xrGetInstanceProcAddr initialize(const char* privateFolder, const char* stateFolder) {
            dllHome = std::filesystem::absolute(privateFolder);
            programData = std::filesystem::absolute(stateFolder);
            std::filesystem::create_directories(programData);
            static_cast<OpenXrRuntime*>(GetInstance())->stopRegistryWatcher();
            return linkedDispatch;
        }
        static void setEnabled(bool value) {
            enabled = value;
            auto& runtime = *static_cast<OpenXrRuntime*>(GetInstance());
            runtime.refreshSettings();
            require(runtime.m_dlssnrSettings.enabled == value &&
                        runtime.m_dlssnrSettings.foveationSize == coverage * .01f,
                    "Synthetic settings were not published through real refreshSettings");
        }
        static unsigned checkCaches(XrSwapchain handle, const char* kind, const char* phase, bool freshOff) {
            const auto& chain = *reinterpret_cast<OpenXrRuntime::Swapchain*>(handle);
            require(chain.resolvedSlices.size() == 2, "Color/depth fixture must resolve both native eyes");
            unsigned failures{};
            for (size_t eye = 0; eye < chain.resolvedSlices.size(); ++eye) {
                const auto& slice = chain.resolvedSlices[eye];
                const auto expected = freshOff ? 0 : slice.images.size();
                std::cout << "cache phase=" << phase << " kind=" << kind << " eye=" << eye
                          << " imports=" << slice.dlssnrImages.size() << " sourceImages=" << slice.images.size()
                          << " expected=" << expected << '\n';
                if (slice.images.empty() || slice.dlssnrImages.size() != expected) {
                    ++failures;
                    std::cerr << "ASSERTION FAILED: " << kind << " NR input imports phase=" << phase << " eye=" << eye
                              << '\n';
                }
            }
            auto& runtime = *static_cast<OpenXrRuntime*>(GetInstance());
            require(runtime.m_precompositor.dlssnrSettings.enabled == bool(enabled.load()),
                    "xrEndFrame did not capture the current live NR settings");
            if (runtime.m_dlssnrDevice) {
                checkHr(runtime.m_dlssnrDevice->GetDeviceRemovedReason());
            } else {
                require(freshOff && !runtime.m_dlssnrResourcesInitialized,
                        "only a fresh NR-off path may lack the lazy NR device");
            }
            return failures;
        }
        static std::vector<ComPtr<ID3D12Resource>> imports(XrSwapchain handle) {
            std::vector<ComPtr<ID3D12Resource>> result;
            for (const auto& slice : reinterpret_cast<OpenXrRuntime::Swapchain*>(handle)->resolvedSlices)
                result.insert(result.end(), slice.dlssnrImages.begin(), slice.dlssnrImages.end());
            return result;
        }
    };
} // namespace virtualdesktop_openxr

#ifndef VDXR_NR_HELPERS_ONLY
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    XrInstance instance{};
    try {
        require(argc >= 3 && argc <= 5,
                "Usage: nr_input_toggle_regression <backend> <state> [metadata|writes|controls] [90|100]");
        const std::string mode = argc >= 4 ? argv[3] : "toggle";
        require(mode == "toggle" || mode == "metadata" || mode == "writes" || mode == "controls", "Unknown mode");
        if (argc == 5) {
            require(!strcmp(argv[4], "90") || !strcmp(argv[4], "100"), "Coverage must be 90 or 100");
            coverage = !strcmp(argv[4], "90") ? 90 : 100;
        }
        SettingsReadHook settings;
        const auto getProc = virtualdesktop_openxr::RuntimeInputRegression::initialize(argv[1], argv[2]);
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR native NR input toggle regression");
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
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.Get();
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
        create.next = &binding;
        create.systemId = system;
        XrSession session{};
        XR_CHECK(xrCreateSession(instance, &create, &session));
        unsigned failures{};
        {
            CompositorHooks compositor;
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XR_CHECK(xrBeginSession(session, &begin));
            XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            reference.poseInReferenceSpace.orientation.w = 1.f;
            XrSpace space{};
            XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
            const char* vsSource = "float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2(id==1?2:0,id==2?2:0);"
                                   "return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
            const char* psSource = "cbuffer c{int2 origin;int2 extent;float base;float3 pad;};"
                                   "float main(float4 p:SV_Position):SV_Depth{return base+"
                                   "(p.x>=origin.x+extent.x/2?0.1:0)+(p.y>=origin.y+extent.y/2?0.2:0);}";
            ComPtr<ID3DBlob> vsCode, psCode, errors;
            checkHr(D3DCompile(
                vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, &errors));
            checkHr(D3DCompile(
                psSource, strlen(psSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psCode, &errors));
            ComPtr<ID3D11VertexShader> vs;
            ComPtr<ID3D11PixelShader> ps;
            checkHr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
            checkHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
            D3D11_DEPTH_STENCIL_DESC dsDesc{};
            dsDesc.DepthEnable = TRUE;
            dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            ComPtr<ID3D11DepthStencilState> ds;
            checkHr(device->CreateDepthStencilState(&dsDesc, &ds));
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = 32;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ComPtr<ID3D11Buffer> cb;
            checkHr(device->CreateBuffer(&cbDesc, nullptr, &cb));
            const std::vector<uint32_t> rgba(512 * 512, 0xff604020);
            auto colorA = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, false);
            auto depthA = createImages(session, DXGI_FORMAT_D32_FLOAT, 512, 512, true);
            Images colorB, depthB;
            using Regression = virtualdesktop_openxr::RuntimeInputRegression;
            std::vector<ComPtr<ID3D12Resource>> colorImportsB, depthImportsB;
            std::unique_ptr<Regression::Observation> observation;
            const char* phases[] = {"A-OFF-fresh",
                                    "A-ON",
                                    "A-OFF-cached",
                                    "B-OFF-fresh",
                                    "B-ON",
                                    "B-OFF-cached",
                                    "B-ON-cached",
                                    "depth-asymmetric",
                                    "depth-absent",
                                    "depth-right-only",
                                    "depth-left-only",
                                    "depth-restored"};
            const int frameCount = mode == "toggle" ? 7 : 12;
            for (int frame = 0; frame < frameCount; ++frame) {
                const bool nr = frame == 1 || frame == 4 || frame >= 6;
                if (frame == 7)
                    observation = std::make_unique<Regression::Observation>(mode == "metadata", mode == "writes");
                Regression::setEnabled(nr);
                if (frame == 3) {
                    colorB = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, false);
                    depthB = createImages(session, DXGI_FORMAT_D32_FLOAT, 512, 512, true);
                }
                auto& color = frame < 3 ? colorA : colorB;
                auto& depth = frame < 3 ? depthA : depthB;
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState state{XR_TYPE_FRAME_STATE};
                XR_CHECK(xrWaitFrame(session, &wait, &state));
                XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
                XR_CHECK(xrBeginFrame(session, &fb));
                const auto ci = acquireImage(color), di = acquireImage(depth);
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    context->UpdateSubresource(color.images[ci].texture,
                                               D3D11CalcSubresource(0, eye, 1),
                                               nullptr,
                                               rgba.data(),
                                               512 * 4,
                                               512 * 512 * 4);
                    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                    desc.Format = DXGI_FORMAT_D32_FLOAT;
                    desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray.FirstArraySlice = eye;
                    desc.Texture2DArray.ArraySize = 1;
                    ComPtr<ID3D11DepthStencilView> dsv;
                    checkHr(device->CreateDepthStencilView(depth.images[di].texture, &desc, &dsv));
                    struct Constants {
                        int origin[2], extent[2];
                        float base, padding[3];
                    } constants{{0, 0}, {512, 512}, eye ? .55f : .15f, {}};
                    context->UpdateSubresource(cb.Get(), 0, nullptr, &constants, 0, 0);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    context->VSSetShader(vs.Get(), nullptr, 0);
                    context->PSSetShader(ps.Get(), nullptr, 0);
                    context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
                    context->OMSetDepthStencilState(ds.Get(), 0);
                    context->OMSetRenderTargets(0, nullptr, dsv.Get());
                    D3D11_VIEWPORT vp{0, 0, 512, 512, 0, 1};
                    context->RSSetViewports(1, &vp);
                    context->Draw(3, 0);
                }
                context->OMSetRenderTargets(0, nullptr, nullptr);
                releaseImage(color);
                releaseImage(depth);
                XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
                XrCompositionLayerDepthInfoKHR depths[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                            {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    views[eye].pose.orientation.w = 1;
                    views[eye].pose.position = {eye ? .032f : -.032f, 1.6f, 0};
                    views[eye].fov = {-.8f, .8f, .8f, -.8f};
                    views[eye].subImage = {color.chain, {{0, 0}, {512, 512}}, eye};
                    views[eye].next = &depths[eye];
                    depths[eye].subImage = {depth.chain, {{0, 0}, {512, 512}}, eye};
                    depths[eye].minDepth = 0;
                    depths[eye].maxDepth = 1;
                    depths[eye].nearZ = .1f;
                    depths[eye].farZ = 100;
                    if (observation) {
                        expectDepth[eye] = frame != 8 && !(frame == 9 && eye == 0) && !(frame == 10 && eye == 1);
                        if (!expectDepth[eye])
                            views[eye].next = nullptr;
                        const bool inverted = (frame == 7 && eye == 1) || (frame == 11 && eye == 0);
                        if (inverted) {
                            depths[eye].nearZ = 100;
                            depths[eye].farZ = .1f;
                        }
                        observation->inverted[eye] = inverted;
                        observation->depth = depth.chain;
                    }
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
                XR_CHECK(xrEndFrame(session, &end));
                {
                    std::unique_lock lock(captureMutex);
                    require(ready.wait_for(lock,
                                           std::chrono::seconds(30),
                                           [&] { return captured > frame || !captureFailure.empty(); }),
                            "Native compositor readback timed out");
                    if (!captureFailure.empty())
                        throw std::runtime_error(captureFailure);
                }
                failures += Regression::checkCaches(color.chain, "color", phases[frame], frame == 0 || frame == 3);
                failures += Regression::checkCaches(depth.chain, "depth", phases[frame], frame == 0 || frame == 3);
                if (nr)
                    require(GetModuleHandleW(L"nvngx_dlssnr.dll"), "Real NR vendor module was not loaded");
                if (frame == 4) {
                    colorImportsB = Regression::imports(colorB.chain);
                    depthImportsB = Regression::imports(depthB.chain);
                }
                if (frame >= 5) {
                    require(colorImportsB == Regression::imports(colorB.chain) &&
                                depthImportsB == Regression::imports(depthB.chain),
                            "Live toggling replaced a completed color/depth input generation");
                }
                checkHr(device->GetDeviceRemovedReason());
            }
            if (observation) {
                failures += observation->failures;
                require(observation->evaluations == 10, "Not all real vendor evaluations were observed");
            }
        } // Detach native observations before normal instance teardown unloads OVRNull.
        context->ClearState();
        context->Flush();
        XR_CHECK(xrDestroyInstance(instance));
        instance = XR_NULL_HANDLE;
        std::cout << "normal instance/session teardown completed; frames=" << captured
                  << " color/depth cache assertion failures=" << failures << '\n';
        require(!failures, "NR native cache/parameter assertions failed");
        std::cout << "PASS: native synchronous live OFF/ON/OFF with fresh pair B, complete cached imports, both-eye "
                     "color/depth readback and real NR; mode="
                  << mode << " FoveationSize=" << coverage << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (instance && xrDestroyInstance) {
            try {
                checkXr(xrDestroyInstance(instance), "failure-path xrDestroyInstance");
            } catch (const std::exception& cleanup) {
                std::cerr << "FAIL: cleanup: " << cleanup.what() << '\n';
            }
        }
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
#endif
