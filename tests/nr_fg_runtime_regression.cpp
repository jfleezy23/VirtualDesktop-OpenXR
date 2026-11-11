// Public OpenXR path, real vendor NR, selected real flow backend and private OVRNull.
// Globals use synthetic registry responses. The profile case owns one temporary
// executable-specific key and never rewrites global or game settings.
#define VDXR_NR_HELPERS_ONLY
#include "nr_input_toggle_regression.cpp"
#undef VDXR_NR_HELPERS_ONLY
#include "nr_fg_lookup_probe.h"
#include "../virtualdesktop-openxr/game_profiles.h"
#include "../virtualdesktop-openxr/log.h"
#include <fstream>
#include <limits>
#include "runtime_delivery_layers.h"

namespace {
    // Unit-simulated provider state only: no ETW session or registered provider.
    // The fixture is pinned to the SDK's provider layout. Level 0 / keyword 0
    // is the exact predicate used by IsTraceEnabled(); normal event levels stay
    // disabled. Keep this alive until the runtime worker and instance are gone.
    struct ScopedSyntheticRuntimeTrace {
        _tlgProvider_t* provider{};
        UINT32 previousLevel{};

        explicit ScopedSyntheticRuntimeTrace(bool active) noexcept {
            if (active) {
                provider = const_cast<_tlgProvider_t*>(virtualdesktop_openxr::log::g_traceProvider);
                previousLevel = provider->LevelPlus1;
                provider->LevelPlus1 = 1;
            }
        }
        ~ScopedSyntheticRuntimeTrace() {
            if (provider)
                provider->LevelPlus1 = previousLevel;
        }
        static bool enabled() noexcept {
            using virtualdesktop_openxr::log::g_traceProvider;
            return IsTraceEnabled();
        }
        void verify() const {
            require(provider && !provider->RegHandle && enabled(),
                    "unit-simulated tracing must enable the real runtime predicate without ETW registration");
        }
        ScopedSyntheticRuntimeTrace(const ScopedSyntheticRuntimeTrace&) = delete;
        ScopedSyntheticRuntimeTrace& operator=(const ScopedSyntheticRuntimeTrace&) = delete;
    };

    // Only the caller's public EndFrame scope observes these SDK calls. Worker,
    // session setup and native timing calls retain the real SDK behavior.
    struct PublicFrameQuerySpy {
        struct Call;
        inline static thread_local Call* current{};
        inline static decltype(&ovr_GetFloat) originalGetFloat{};
        inline static decltype(&ovr_SetFloat) originalSetFloat{};
        inline static decltype(&ovr_GetTrackingOriginType) originalGetOrigin{};
        const bool poseMode;
        bool attached{};
        unsigned observedProjections{};
        struct ProjectionExpectation {
            float height;
            std::array<XrPosef, 2> views;
        };
        std::mutex mutex;
        std::map<double, ProjectionExpectation> projections;

        struct Call {
            const bool poseMode;
            const float height;
            unsigned eyeHeightCalls{}, appGpuCalls{};
            float appGpuValue{};
            explicit Call(const PublicFrameQuerySpy& spy, float liveHeight = 0)
                : poseMode(spy.poseMode), height(liveHeight) {
                require(!current, "public EndFrame query scopes must not nest");
                current = this;
            }
            ~Call() {
                finish();
            }
            void finish() noexcept {
                if (current == this)
                    current = nullptr;
            }
            void verifyPose(unsigned expected, const char* phase) const {
                std::cout << "PROJECTION_POSE_QUERY phase=" << phase << " height_calls=" << eyeHeightCalls
                          << " expected=" << expected << '\n';
                require(eyeHeightCalls == expected,
                        "public projection path must locate once per layer after the first valid eye, preserving "
                        "invalid-eye order");
            }
            void verifyAppGpu(float expected) const {
                std::cout << "IDLE_FPS_QUERY app_gpu_calls=" << appGpuCalls << " value=" << appGpuValue
                          << " expected=" << expected << '\n';
                require(appGpuCalls == 1 && std::isfinite(appGpuValue) && appGpuValue == expected,
                        "idle FPS optimization must preserve the real AppGpuTime dispatch and value");
            }
            Call(const Call&) = delete;
            Call& operator=(const Call&) = delete;
        };
        static float OVR_CDECL getFloat(ovrSession session, const char* name, float fallback) {
            if (current && current->poseMode && name && !strcmp(name, OVR_KEY_EYE_HEIGHT)) {
                // A duplicate lookup sees genuinely different live input. The
                // one layer transform must still be shared by both eye poses.
                return current->height + .25f * current->eyeHeightCalls++;
            }
            return originalGetFloat(session, name, fallback);
        }
        static ovrBool OVR_CDECL setFloat(ovrSession session, const char* name, float value) {
            if (current && !current->poseMode && name && !strcmp(name, "AppGpuTime")) {
                ++current->appGpuCalls;
                current->appGpuValue = value;
            }
            return originalSetFloat(session, name, value);
        }
        static ovrTrackingOrigin OVR_CDECL getOrigin(ovrSession session) {
            if (current && current->poseMode)
                return ovrTrackingOrigin_FloorLevel;
            return originalGetOrigin(session);
        }
        PublicFrameQuerySpy(bool active, bool pose) : poseMode(pose) {
            if (!active)
                return;
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalGetFloat = reinterpret_cast<decltype(originalGetFloat)>(GetProcAddress(module, "ovr_GetFloat"));
            originalSetFloat = reinterpret_cast<decltype(originalSetFloat)>(GetProcAddress(module, "ovr_SetFloat"));
            originalGetOrigin =
                reinterpret_cast<decltype(originalGetOrigin)>(GetProcAddress(module, "ovr_GetTrackingOriginType"));
            require(originalGetFloat && originalSetFloat && originalGetOrigin,
                    "private SDK query observation exports required");
            LONG status = DetourTransactionBegin();
            const bool transaction = status == NO_ERROR;
            if (status == NO_ERROR)
                status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalGetFloat), getFloat);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalSetFloat), setFloat);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalGetOrigin), getOrigin);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            checkHr(HRESULT_FROM_WIN32(status));
            attached = true;
        }
        ~PublicFrameQuerySpy() {
            if (attached &&
                (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                 DetourDetach(reinterpret_cast<PVOID*>(&originalGetFloat), getFloat) != NO_ERROR ||
                 DetourDetach(reinterpret_cast<PVOID*>(&originalSetFloat), setFloat) != NO_ERROR ||
                 DetourDetach(reinterpret_cast<PVOID*>(&originalGetOrigin), getOrigin) != NO_ERROR ||
                 DetourTransactionCommit() != NO_ERROR))
                std::_Exit(2);
        }
        void expectProjection(double sampleTime, float height, const XrCompositionLayerProjectionView* views) {
            std::lock_guard lock(mutex);
            projections.emplace(sampleTime, ProjectionExpectation{height, {views[0].pose, views[1].pose}});
        }
        void observeProjectionPose(const ovrLayerEyeFov& layer) {
            std::lock_guard lock(mutex);
            const auto found = projections.find(layer.SensorSampleTime);
            require(found != projections.end(), "native projection retains its supplied sample time");
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& expected = found->second.views[eye];
                const auto& actual = layer.RenderPose[eye];
                require(std::abs(actual.Position.x - expected.position.x) < .0001f &&
                            std::abs(actual.Position.y - expected.position.y - found->second.height) < .0001f &&
                            std::abs(actual.Position.z - expected.position.z) < .0001f &&
                            std::abs(actual.Orientation.x - expected.orientation.x) < .0001f &&
                            std::abs(actual.Orientation.y - expected.orientation.y) < .0001f &&
                            std::abs(actual.Orientation.z - expected.orientation.z) < .0001f &&
                            std::abs(actual.Orientation.w - expected.orientation.w) < .0001f,
                        "native stereo projection must share its live origin transform and preserve each eye pose");
            }
            ++observedProjections;
        }
        void verifyProjectionObservations() {
            std::lock_guard lock(mutex);
            std::cout << "PROJECTION_POSE_NATIVE observed=" << observedProjections
                      << " distinct_inputs=" << projections.size() << '\n';
            require(observedProjections && projections.size() == 8,
                    "bounded public pose-query inputs must reach the actual native presenter");
        }
        PublicFrameQuerySpy(const PublicFrameQuerySpy&) = delete;
        PublicFrameQuerySpy& operator=(const PublicFrameQuerySpy&) = delete;
    };

    // Observe real module calls, including their original results/tickets. No
    // capability bit, runtime boolean or B-depth readback proves V5 admission.
    struct GuideApiSpy {
        inline static VDXRFG_GetApiV5Fn originalGetApi{};
        inline static VDXRFG_GetApiV4Fn originalGetApi4{};
        inline static decltype(VDXRFG_ApiV5::Create) originalCreate{};
        inline static decltype(VDXRFG_ApiV4::Create) originalCreate4{};
        inline static decltype(VDXRFG_ApiV5::Submit) originalSubmit{};
        inline static decltype(VDXRFG_ApiV5::Reset) originalReset{};
        inline static std::atomic<unsigned> negotiations{}, guideCreates{}, submits{}, validGuides{}, compatiblePairs{},
            invalidGuides{};
        inline static std::atomic<void*> guideContext{};
        inline static std::atomic<uint64_t> previousSerial{}, previousEpoch{};
        inline static bool checkOpacity{};
        inline static std::atomic<bool> expectedOpaque{}, contextOpaque{};
        inline static std::atomic<unsigned> opaqueCreates{}, blendedCreates{}, opacityMismatches{}, opacityPrimes{};
        inline static std::atomic<unsigned> opaqueResets{}, alphaResets{};
        inline static bool checkRuntimeTiming{};
        inline static std::atomic<unsigned> runtimeCreates{}, successfulRuntimeCreates{}, timingCapableNegotiations{},
            tracedRuntimeCreates{}, gpuTimingCreates{};
        HMODULE module{};
        bool attached{};

        static void observeRuntimeCreate(uint32_t flags) noexcept {
            if (checkRuntimeTiming) {
                ++runtimeCreates;
                if (ScopedSyntheticRuntimeTrace::enabled())
                    ++tracedRuntimeCreates;
                if (flags & VDXRFG_CREATE_GPU_TIMING)
                    ++gpuTimingCreates;
            }
        }
        static HRESULT WINAPI create4(const VDXRFG_CreateInfoV3* info, void** context) noexcept {
            observeRuntimeCreate(info ? info->flags : 0);
            const auto result = originalCreate4(info, context);
            if (SUCCEEDED(result) && context && *context)
                ++successfulRuntimeCreates;
            return result;
        }
        static HRESULT WINAPI create(const VDXRFG_CreateInfoV5* info, void** context) noexcept {
            observeRuntimeCreate(info ? info->base.flags : 0);
            const auto result = originalCreate(info, context);
            if (SUCCEEDED(result) && context && *context) {
                if (checkRuntimeTiming)
                    ++successfulRuntimeCreates;
                guideContext = info && info->guide_input ? *context : nullptr;
                if (guideContext.load())
                    ++guideCreates;
                contextOpaque = info && (info->base.flags & VDXRFG_CREATE_COLOR_OPAQUE);
                if (guideContext.load()) {
                    if (contextOpaque)
                        ++opaqueCreates;
                    else
                        ++blendedCreates;
                }
                previousSerial = previousEpoch = 0;
            }
            return result;
        }
        static HRESULT WINAPI submit(void* context, const VDXRFG_SubmitInfoV5* info, VDXRFG_TicketV1* ticket) noexcept {
            ++submits;
            const auto result = originalSubmit(context, info, ticket);
            if (FAILED(result)) {
                previousSerial = previousEpoch = 0;
                return result;
            }
            const bool valid =
                info && info->struct_size == sizeof(*info) && info->api_version == VDXRFG_API_VERSION_5 &&
                info->base.struct_size == sizeof(info->base) && info->base.api_version == VDXRFG_API_VERSION_3 &&
                context == guideContext.load() && context && info->guide_mask == 3 && info->guide_serial &&
                info->guide_serial == info->base.serial && info->guide_epoch &&
                info->guide_encoding == VDXRFG_GUIDE_ENCODING_INVERSE_Z && ticket && ticket->token &&
                ticket->current_serial == info->base.serial;
            if (valid) {
                if (checkOpacity && contextOpaque.load() != expectedOpaque.load())
                    ++opacityMismatches;
                if (checkOpacity && !previousSerial.load()) {
                    if (ticket->prime && !ticket->previous_serial)
                        ++opacityPrimes;
                    else
                        ++opacityMismatches;
                }
                ++validGuides;
                const auto serial = previousSerial.load();
                if (serial && serial != UINT64_MAX && info->base.serial == serial + 1 &&
                    info->guide_epoch == previousEpoch.load() && ticket->previous_serial == serial && !ticket->prime)
                    ++compatiblePairs;
                previousSerial = info->base.serial;
                previousEpoch = info->guide_epoch;
            } else {
                if (info && (info->guide_mask || info->guide_serial || info->guide_epoch || info->guide_encoding))
                    ++invalidGuides;
                previousSerial = previousEpoch = 0;
            }
            return result;
        }
        static HRESULT WINAPI reset(void* context, uint32_t flags) noexcept {
            const auto result = originalReset(context, flags);
            if (SUCCEEDED(result)) {
                previousSerial = previousEpoch = 0;
                if (flags & VDXRFG_RESET_COLOR_OPAQUE) {
                    contextOpaque = true;
                    ++opaqueResets;
                } else if (flags & VDXRFG_RESET_COLOR_ALPHA) {
                    contextOpaque = false;
                    ++alphaResets;
                }
            }
            return result;
        }
        static HRESULT WINAPI getApi(VDXRFG_ApiV5* api) noexcept {
            const auto result = originalGetApi(api);
            if (SUCCEEDED(result) && api && api->Create && api->Submit && api->Reset) {
                ++negotiations;
                if (checkRuntimeTiming && (api->supported_create_flags & VDXRFG_CREATE_GPU_TIMING))
                    ++timingCapableNegotiations;
                originalCreate = api->Create;
                originalSubmit = api->Submit;
                originalReset = api->Reset;
                api->Create = create;
                api->Submit = submit;
                api->Reset = reset;
            }
            return result;
        }
        static HRESULT WINAPI getApi4(VDXRFG_ApiV4* api) noexcept {
            const auto result = originalGetApi4(api);
            if (SUCCEEDED(result) && api && api->Create) {
                if (api->supported_create_flags & VDXRFG_CREATE_GPU_TIMING)
                    ++timingCapableNegotiations;
                originalCreate4 = api->Create;
                api->Create = create4;
            }
            return result;
        }
        explicit GuideApiSpy(const char* backend) {
            const auto path = std::filesystem::absolute(backend) / L"vdxr-frame-generation.dll";
            module = LoadLibraryExW(
                path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            require(module != nullptr, "load selected private FG module for real V5 observation");
            originalGetApi = reinterpret_cast<VDXRFG_GetApiV5Fn>(GetProcAddress(module, "VDXRFG_GetApiV5"));
            // Color-only runtime FG selects V4; V5 is negotiated only when
            // optional depth guidance is requested. Observe its real Create
            // without enabling guidance or changing the selected module.
            if (checkRuntimeTiming) {
                originalGetApi4 = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
                require(originalGetApi4 != nullptr, "timing regression requires the real module's V4 color API");
            }
            // An older real module remains usable, but depth assertions below
            // must turn RED because it cannot receive V5 guides.
            if (!originalGetApi && !originalGetApi4)
                return;
            LONG status = DetourTransactionBegin();
            const bool transaction = status == NO_ERROR;
            if (status == NO_ERROR)
                status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR && originalGetApi)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalGetApi), getApi);
            if (status == NO_ERROR && originalGetApi4)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalGetApi4), getApi4);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (status != NO_ERROR) {
                FreeLibrary(module);
                module = nullptr;
                checkHr(HRESULT_FROM_WIN32(status));
            }
            attached = true;
        }
        ~GuideApiSpy() {
            if (attached &&
                (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                 (originalGetApi && DetourDetach(reinterpret_cast<PVOID*>(&originalGetApi), getApi) != NO_ERROR) ||
                 (originalGetApi4 && DetourDetach(reinterpret_cast<PVOID*>(&originalGetApi4), getApi4) != NO_ERROR) ||
                 DetourTransactionCommit() != NO_ERROR))
                std::_Exit(2);
            if (module)
                FreeLibrary(module);
        }
        void verify(bool depthScenario) const {
            std::cout << "V5_GUIDE_CALLS negotiated=" << negotiations << " creates=" << guideCreates
                      << " submits=" << submits << " valid=" << validGuides << " compatible=" << compatiblePairs
                      << " invalid=" << invalidGuides << '\n';
            if (depthScenario) {
                require(negotiations && guideCreates,
                        "public depth path must actually create the real V5 module with nonnull guide input");
                require(validGuides >= 64 && compatiblePairs >= 64 && !invalidGuides,
                        "public depth path must forward at least64 valid guides and consecutive compatible V5 pairs");
                if (checkOpacity)
                    require(
                        guideCreates == 1 && opaqueCreates == 1 && !blendedCreates && !opaqueResets && !alphaResets &&
                            opacityPrimes >= 1 && !opacityMismatches,
                        "source alpha flags preserve the final opaque projection without mode resets or recreation");
            } else {
                require(!guideCreates && !submits,
                        "ordinary color-only path must not allocate or submit optional V5 guides");
            }
        }
        void verifyRuntimeTiming(bool complete) const {
            if (!checkRuntimeTiming)
                return;
            if (complete || gpuTimingCreates)
                std::cout << "UNIT_SIMULATED_TRACE_FG creates=" << runtimeCreates
                          << " successful=" << successfulRuntimeCreates << " traced=" << tracedRuntimeCreates
                          << " timing_capable=" << timingCapableNegotiations << " gpu_timing=" << gpuTimingCreates
                          << " etw_session=0\n";
            require(!gpuTimingCreates,
                    "public runtime FG creation must disable GPU timing even when IsTraceEnabled is true");
            require(tracedRuntimeCreates == runtimeCreates,
                    "every observed runtime Create must occur with the real trace predicate enabled");
            if (complete) {
                require(runtimeCreates && successfulRuntimeCreates && timingCapableNegotiations,
                        "public EndFrame must create the real timing-capable module under simulated trace state");
            }
        }
        GuideApiSpy(const GuideApiSpy&) = delete;
        GuideApiSpy& operator=(const GuideApiSpy&) = delete;
    };

    struct FixtureGameProfile {
        std::wstring key;
        FixtureGameProfile() {
            const auto& executable = vdxr_profiles::currentExecutable();
            key = xr::utf8_to_wide(virtualdesktop_openxr::RegPrefix) + L"\\Profiles\\" +
                  vdxr_profiles::profileId(executable);
            HKEY opened{};
            DWORD disposition{};
            require(RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                                    key.c_str(),
                                    0,
                                    nullptr,
                                    0,
                                    KEY_SET_VALUE | KEY_WOW64_64KEY,
                                    nullptr,
                                    &opened,
                                    &disposition) == ERROR_SUCCESS,
                    "create isolated executable profile");
            if (disposition != REG_CREATED_NEW_KEY) {
                RegCloseKey(opened);
                throw std::runtime_error("Refusing to overwrite an existing fixture executable profile");
            }
            try {
                require(RegSetValueExW(opened,
                                       L"ExecutablePath",
                                       0,
                                       REG_SZ,
                                       reinterpret_cast<const BYTE*>(executable.c_str()),
                                       static_cast<DWORD>((executable.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
                        "write fixture identity");
                const DWORD multiplier = 4;
                require(RegSetValueExW(opened,
                                       L"FrameGenerationMode",
                                       0,
                                       REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&multiplier),
                                       sizeof(multiplier)) == ERROR_SUCCESS,
                        "write fixture frame-generation override");
            } catch (...) {
                RegCloseKey(opened);
                RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), KEY_WOW64_64KEY, 0);
                throw;
            }
            RegCloseKey(opened);
        }
        void remove() {
            const auto status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), KEY_WOW64_64KEY, 0);
            require(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND, "remove fixture executable profile");
            key.clear();
        }
        ~FixtureGameProfile() {
            if (!key.empty())
                RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), KEY_WOW64_64KEY, 0);
        }
    };

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
        void attach(XrSwapchain chain, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
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
                    view.Format = format;
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.FirstArraySlice = eye;
                    view.Texture2DArray.ArraySize = 1;
                    device->CreateRenderTargetView(image.texture, &view, handle);
                    handle.ptr += stride;
                }
            }
        }
        void fill(unsigned index, bool overwrite = false, bool cropped = false) {
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
                if (cropped && !overwrite) {
                    const float outside[]{0.f, 1.f, 0.f, 1.f};
                    const D3D12_RECT rect{16, 12, 528, 524};
                    commands.Commands->ClearRenderTargetView(handle, outside, 0, nullptr);
                    commands.Commands->ClearRenderTargetView(handle, value, 1, &rect);
                } else {
                    commands.Commands->ClearRenderTargetView(handle, value, 0, nullptr);
                }
                handle.ptr += stride;
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands.Commands->ResourceBarrier(1, &barrier);
            context->SubmitCommandList(std::move(commands));
        }
    };

    // Deliberately offset, half-resolution depth. Alignment must preserve the
    // discontinuities when it maps this rectangle into the final colour rect.
    struct DepthPattern {
        Images images;
        std::vector<std::array<ComPtr<ID3D11DepthStencilView>, 2>> views;
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11DepthStencilState> state;
        ComPtr<ID3D11Buffer> constants;
        D3D12Input* input12{};
        std::vector<XrSwapchainImageD3D12KHR> images12;
        ComPtr<ID3D12DescriptorHeap> views12;
        UINT stride12{};
        const XrRect2Di rect;
        const bool matchingGeometry;
        const bool finiteEndpoint;

        DepthPattern(XrSession session,
                     ID3D11Device* device,
                     D3D12Input* app12 = nullptr,
                     bool sameGeometry = false,
                     bool farEndpoint = false)
            : images(createImages(
                  session, DXGI_FORMAT_D32_FLOAT, sameGeometry ? 544 : 384, sameGeometry ? 536 : 320, true, !app12)),
              input12(app12), rect(sameGeometry ? XrRect2Di{{16, 12}, {512, 512}} : XrRect2Di{{64, 32}, {256, 256}}),
              matchingGeometry(sameGeometry), finiteEndpoint(farEndpoint) {
            if (input12) {
                uint32_t count{};
                XR_CHECK(xrEnumerateSwapchainImages(images.chain, 0, &count, nullptr));
                require(count != 0, "public D3D12 depth swapchain exports a nonempty ring");
                images12.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                XR_CHECK(xrEnumerateSwapchainImages(
                    images.chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images12.data())));
                D3D12_DESCRIPTOR_HEAP_DESC heap{};
                heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
                heap.NumDescriptors = count * 2;
                checkHr(input12->device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views12)));
                stride12 = input12->device->GetDescriptorHandleIncrementSize(heap.Type);
                auto handle = views12->GetCPUDescriptorHandleForHeapStart();
                for (const auto& image : images12) {
                    require(image.texture != nullptr, "public D3D12 depth swapchain exports real images");
                    for (unsigned eye = 0; eye < 2; ++eye) {
                        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
                        view.Format = DXGI_FORMAT_D32_FLOAT;
                        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                        view.Texture2DArray.FirstArraySlice = eye;
                        view.Texture2DArray.ArraySize = 1;
                        input12->device->CreateDepthStencilView(image.texture, &view, handle);
                        handle.ptr += stride12;
                    }
                }
                return;
            }
            const char* vs = "float4 main(uint id:SV_VertexID):SV_Position{float2 "
                             "p=float2(id==1?2:0,id==2?2:0);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
            const char* ps = "cbuffer c{float base;float3 pad;};float main(float4 p:SV_Position):SV_Depth{"
                             "return base+(p.x>=160?0.1:0)+(p.y>=208?0.2:0);}";
            ComPtr<ID3DBlob> vsCode, psCode, errors;
            checkHr(D3DCompile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, &errors));
            checkHr(D3DCompile(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psCode, &errors));
            checkHr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vertex));
            checkHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &pixel));
            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable = TRUE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
            checkHr(device->CreateDepthStencilState(&depth, &state));
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 16;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            checkHr(device->CreateBuffer(&buffer, nullptr, &constants));
            views.resize(images.images.size());
            for (size_t index = 0; index < views.size(); ++index)
                for (unsigned eye = 0; eye < 2; ++eye) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                    view.Format = DXGI_FORMAT_D32_FLOAT;
                    view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.FirstArraySlice = eye;
                    view.Texture2DArray.ArraySize = 1;
                    checkHr(device->CreateDepthStencilView(images.images[index].texture, &view, &views[index][eye]));
                }
        }
        void fill12(unsigned index, unsigned frame, bool overwrite) {
            // The public binding owns this context: depth and color writes,
            // including immediate app reuse, follow the runtime's queue waits.
            auto commands = input12->context->GetCommandList();
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                images12[index].texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            commands.Commands->ResourceBarrier(1, &barrier);
            auto handle = views12->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += index * 2 * stride12;
            for (unsigned eye = 0; eye < 2; ++eye) {
                const float base = (eye ? .55f : .15f) + frame * .001f;
                commands.Commands->ClearDepthStencilView(
                    handle, D3D12_CLEAR_FLAG_DEPTH, overwrite ? .97f : base, 0, 0, nullptr);
                if (!overwrite) {
                    const D3D12_RECT right{160, 0, 384, 320};
                    const D3D12_RECT bottom{0, 208, 384, 320};
                    const D3D12_RECT corner{160, 208, 384, 320};
                    commands.Commands->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, base + .1f, 0, 1, &right);
                    commands.Commands->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, base + .2f, 0, 1, &bottom);
                    commands.Commands->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, base + .3f, 0, 1, &corner);
                }
                handle.ptr += stride12;
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands.Commands->ResourceBarrier(1, &barrier);
            input12->context->SubmitCommandList(std::move(commands));
        }
        void fill(ID3D11DeviceContext* context, unsigned index, unsigned frame) {
            if (input12) {
                fill12(index, frame, false);
                return;
            }
            for (unsigned eye = 0; eye < 2; ++eye) {
                if (finiteEndpoint && !eye) {
                    context->ClearDepthStencilView(views[index][eye].Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
                    continue;
                }
                const float data[]{(eye ? .55f : .15f) + frame * .001f, 0, 0, 0};
                context->UpdateSubresource(constants.Get(), 0, nullptr, data, 0, 0);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vertex.Get(), nullptr, 0);
                context->PSSetShader(pixel.Get(), nullptr, 0);
                context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
                context->OMSetDepthStencilState(state.Get(), 0);
                context->OMSetRenderTargets(0, nullptr, views[index][eye].Get());
                const D3D11_VIEWPORT viewport{
                    0, 0, matchingGeometry ? 544.f : 384.f, matchingGeometry ? 536.f : 320.f, 0, 1};
                context->RSSetViewports(1, &viewport);
                context->Draw(3, 0);
            }
            context->ClearState();
        }
        void overwrite(ID3D11DeviceContext* context, unsigned index) {
            if (input12) {
                fill12(index, 0, true);
                return;
            }
            for (const auto& view : views[index])
                context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, .97f, 0);
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
    bool predictionJitter{};
    uint64_t nextWaitId{};
#ifdef VDXR_4X_CAPTURE_REGRESSION
    std::atomic<long long> phaseStretchFirstId{std::numeric_limits<long long>::max()};
#endif
    double OVR_CDECL stablePrediction(ovrSession, long long id) {
        return clockBase.load() + id / static_cast<double>(nativeRate) + (predictionJitter ? id * .000005 : 0.)
#ifdef VDXR_4X_CAPTURE_REGRESSION
               + (id >= phaseStretchFirstId.load() ? .012 : 0.)
#endif
            ;
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

#ifdef VDXR_LAST_COPY_GATE_REGRESSION
#include "nr_fg_last_copy_gate.h"
#endif

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
#ifdef VDXR_PREBEGIN_OVERLAP_REGRESSION
#include "nr_fg_prebegin_overlap_access.inc"
#endif
#ifdef VDXR_DELIVERY_REGRESSION
        static void selectControlledDeliveryBackend() {
            // LOAD_LOCAL_LIBOVR labels the controlled SDK Oculus. This fixture
            // explicitly emulates the VD routing policy; actual SDK calls remain
            // hooked to the same controlled backend, never a real headset.
            runtime().m_useOculusRuntime = false;
        }
#endif
        static OpenXrRuntime& runtime() {
            return *static_cast<OpenXrRuntime*>(GetInstance());
        }
        struct FrameQueryState {
            OpenXrRuntime& value;
            const bool previousOculus;
            const bool previousMirror;
            const OpenXrRuntime::Space previousOrigin;
            const std::optional<float> previousFloorHeight;
            bool restored{};
            explicit FrameQueryState(bool pose)
                : value(runtime()), previousOculus(value.m_useOculusRuntime),
                  previousMirror(value.m_useMirrorWindow.load(std::memory_order_relaxed)),
                  previousOrigin(*value.m_originSpace), previousFloorHeight(value.m_lastKnownFloorHeight) {
                require(!ScopedSyntheticRuntimeTrace::enabled(), "frame-query probe requires tracing disabled");
                require(pose || value.m_frameTimes.empty(), "idle FPS probe starts with no collected samples");
                value.m_useMirrorWindow = false;
                if (pose) {
                    value.m_originSpace->referenceType = XR_REFERENCE_SPACE_TYPE_STAGE;
                    value.m_originSpace->poseInSpace = xr::math::Pose::Identity();
                } else {
                    // The controlled SDK labels itself Oculus. Exercise the
                    // existing VD AppGpuTime branch without replacing its call.
                    value.m_useOculusRuntime = false;
                }
            }
            void restore() noexcept {
                if (restored)
                    return;
                value.m_useOculusRuntime = previousOculus;
                value.m_useMirrorWindow = previousMirror;
                if (value.m_originSpace)
                    *value.m_originSpace = previousOrigin;
                value.m_lastKnownFloorHeight = previousFloorHeight;
                restored = true;
            }
            ~FrameQueryState() {
                restore();
            }
            FrameQueryState(const FrameQueryState&) = delete;
            FrameQueryState& operator=(const FrameQueryState&) = delete;
        };
        static void verifyIdleFps() {
            const auto& value = runtime();
            std::cout << "IDLE_FPS_QUERY frame_times=" << value.m_frameTimes.size()
                      << " trace=" << ScopedSyntheticRuntimeTrace::enabled()
                      << " mirror=" << value.m_useMirrorWindow.load(std::memory_order_relaxed) << '\n';
            require(!ScopedSyntheticRuntimeTrace::enabled() && !value.m_useMirrorWindow && value.m_frameTimes.empty(),
                    "public EndFrame must leave FPS samples empty when trace and mirror consumers are disabled");
        }
        static float expectedAppGpuTime() {
            return runtime().m_lastGpuFrameTimeUs / 1e6f;
        }
        static double nativeSampleTime(XrTime time) {
            return runtime().xrTimeToOvrTime(time);
        }
        static void verifyAppQueue(ID3D12CommandQueue* queue) {
            require(queue && runtime().m_d3d12CommandQueue.Get() == queue,
                    "D3D12 app writes use the queue supplied in the public OpenXR binding");
        }
#ifdef VDXR_NATIVE_DELIVERY_PROBE
        struct LiveDeliveryGate {
            ComPtr<ID3D12Fence> release;
            ComPtr<ID3D12Fence> futureOutput;
            uint64_t futureOutputValue{};
            ComPtr<ID3D11Fence> sourceMarker, presentationMarker;
            long long pendingNativeId{-1};
            uint64_t expectedSerial{};
            bool endObserved{}, independentCompletion{}, futureAccepted{}, futurePendingAtEnd{};
            bool activeAtArm{}, ownedAtArm{}, readyAtArm{}, mailboxEmptyAtArm{}, gateRegistered{};
            bool admissionWake{}, readyBeforeLease{}, stoppingAtArm{}, admissionExpired{};
            bool endNotified{}, eventCreated{}, eventRegistered{}, eventCompleted{}, futureSampled{};
            bool acceptedAtObservation{}, pendingAtObservation{}, completionError{};
            uint64_t sourceValueAtObservation{UINT64_MAX}, presentationValueAtObservation{UINT64_MAX};
            uint64_t futureDeviceValueAtObservation{UINT64_MAX};
            uint64_t futureValueAtObservation{UINT64_MAX};
            double armed{}, completed{};
            void releaseOrExit() noexcept {
                if (release && FAILED(release->Signal(1)))
                    std::_Exit(4); // Never unwind resources behind an unreleasable GPU wait.
            }
        };
        static bool armLiveDeliveryGate(LiveDeliveryGate& gate, const std::function<bool()>& registerGate) {
            auto& value = runtime();
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            {
                // Admission can require this worker to snapshot/copy on S.
                // Do not lease its context or insert our S wait before it opens.
                std::unique_lock state(value.m_asyncSubmissionMutex);
                gate.admissionWake = value.m_asyncSubmissionCondVar.wait_until(state, deadline, [&] {
                    return value.m_terminateAsyncThread || value.m_asyncSubmissionError ||
                           value.m_asyncSubmissionReady || !value.m_fgActiveSubmission;
                });
                gate.activeAtArm = value.m_fgActiveSubmission;
                gate.ownedAtArm = value.m_fgActiveOwnedPixels;
                gate.readyAtArm = gate.readyBeforeLease = value.m_asyncSubmissionReady;
                gate.mailboxEmptyAtArm = value.m_layersForAsyncSubmission.empty();
                gate.stoppingAtArm = value.m_terminateAsyncThread;
                gate.admissionExpired = std::chrono::steady_clock::now() >= deadline;
                if (value.m_asyncSubmissionError)
                    std::rethrow_exception(value.m_asyncSubmissionError);
                if (!gate.admissionWake || gate.admissionExpired || gate.stoppingAtArm || !gate.activeAtArm ||
                    !gate.ownedAtArm || !gate.readyAtArm || !gate.mailboxEmptyAtArm)
                    return false;
            }
            std::unique_lock lease(value.m_fgContextMutex, std::defer_lock);
            value.lockFrameGenerationProducerContext(lease);
            std::unique_lock state(value.m_asyncSubmissionMutex);
            gate.activeAtArm = value.m_fgActiveSubmission;
            gate.ownedAtArm = value.m_fgActiveOwnedPixels;
            gate.readyAtArm = value.m_asyncSubmissionReady;
            gate.mailboxEmptyAtArm = value.m_layersForAsyncSubmission.empty();
            gate.stoppingAtArm = value.m_terminateAsyncThread;
            gate.admissionExpired = std::chrono::steady_clock::now() >= deadline;
            if (value.m_asyncSubmissionError)
                std::rethrow_exception(value.m_asyncSubmissionError);
            if (gate.admissionExpired || gate.stoppingAtArm || !gate.activeAtArm || !gate.ownedAtArm ||
                !gate.readyAtArm || !gate.mailboxEmptyAtArm)
                return false;
            gate.pendingNativeId = static_cast<long long>(value.m_fgWorkerGroupNativeId.load()) - 1;
            gate.expectedSerial = value.m_frameBegun;
            require(value.m_fgDevice && value.m_deliveryTransport, "live gate requires real isolated FG domains");
            checkHr(value.m_fgDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate.release)));
            wil::unique_handle handle;
            checkHr(
                value.m_fgDevice->CreateSharedHandle(gate.release.Get(), nullptr, GENERIC_ALL, nullptr, handle.put()));
            ComPtr<ID3D11Fence> held;
            checkHr(value.m_ovrSubmissionDevice->OpenSharedFence(handle.get(), IID_PPV_ARGS(&held)));
            checkHr(
                value.m_ovrSubmissionDevice->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.sourceMarker)));
            checkHr(value.presentationDevice()->CreateFence(
                0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.presentationMarker)));
            auto releaseOnFailure = MakeScopeGuard([&] { gate.releaseOrExit(); });
            checkHr(value.m_ovrSubmissionContext->Wait(held.Get(), 1));
            checkHr(value.m_ovrSubmissionContext->Signal(gate.sourceMarker.Get(), 1));
            value.m_ovrSubmissionContext->Flush();
            gate.armed = ovr_GetTimeInSeconds();
            if (!registerGate())
                return false;
            gate.gateRegistered = true;
            releaseOnFailure.Deactivate();
            return true;
        }
        // Called under the worker's FG lease, before forwarding the older End.
        static void observeFutureDelivery(LiveDeliveryGate& gate, const ovrLayerHeader* const* layers, unsigned count) {
            auto& value = runtime();
            std::unique_lock state(value.m_asyncSubmissionMutex);
            const auto& next = value.m_fgTransactionForSubmission;
            if (!count || !layers || !layers[0] ||
                (layers[0]->Type != ovrLayerType_EyeFov && layers[0]->Type != ovrLayerType_EyeFovDepth))
                return;
            const auto& old = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
            unsigned oldBank = UINT_MAX;
            for (unsigned bank = 0; bank < value.m_fgPresentationBanks.size(); ++bank)
                if (value.m_fgPresentationBanks[bank][2].ovrSwapchain == old.ColorTexture[0])
                    oldBank = bank;
            gate.futureAccepted = next.ticket.token && !next.ticket.prime &&
                                  next.ticket.current_serial == gate.expectedSerial && gate.expectedSerial &&
                                  next.ticket.previous_serial == gate.expectedSerial - 1 && oldBank != UINT_MAX &&
                                  next.ticket.bank != oldBank && !value.m_layersForAsyncSubmission.empty();
            if (gate.futureAccepted && next.ticket.output_ready_fence && next.ticket.output_ready_value) {
                gate.futureOutput = static_cast<ID3D12Fence*>(next.ticket.output_ready_fence);
                gate.futureOutputValue = next.ticket.output_ready_value;
                const auto completed = gate.futureOutput->GetCompletedValue();
                gate.futurePendingAtEnd = completed != UINT64_MAX && completed < gate.futureOutputValue;
            }
        }
        static HRESULT markNativeDelivery(LiveDeliveryGate& gate) {
            const auto result = runtime().presentationContext()->Signal(gate.presentationMarker.Get(), 1);
            if (SUCCEEDED(result))
                runtime().presentationContext()->Flush();
            return result;
        }
        static bool nativeLayerValid(const ovrLayerHeader* const* layers, unsigned count, bool depth) {
            if (count != 1 || !layers || !layers[0] ||
                (layers[0]->Type != ovrLayerType_EyeFov && layers[0]->Type != ovrLayerType_EyeFovDepth))
                return false;
            const auto& layer = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
            const auto role = nativeRole(layer.ColorTexture[0]);
            if (!std::isfinite(layer.SensorSampleTime) || layer.SensorSampleTime <= 0 || !layer.ColorTexture[0] ||
                !layer.ColorTexture[1])
                return false;
            if (role != 4 && layer.ColorTexture[0] != layer.ColorTexture[1])
                return false;
            if (role != 2 && role != 4)
                return layer.Header.Type == ovrLayerType_EyeFov;
            if (!depth)
                return layer.Header.Type == ovrLayerType_EyeFov;
            if (layer.Header.Type != ovrLayerType_EyeFovDepth)
                return false;
            const auto& withDepth = *reinterpret_cast<const ovrLayerEyeFovDepth*>(layers[0]);
            if (!withDepth.DepthTexture[0] || !withDepth.DepthTexture[1] ||
                !std::isfinite(withDepth.ProjectionDesc.Projection22) ||
                !std::isfinite(withDepth.ProjectionDesc.Projection23) ||
                !std::isfinite(withDepth.ProjectionDesc.Projection32) || withDepth.ProjectionDesc.Projection23 == 0 ||
                withDepth.ProjectionDesc.Projection32 != -1.f)
                return false;
            if (role == 2) {
                for (size_t bank = 0; bank < runtime().m_fgPresentationBanks.size(); ++bank) {
                    if (runtime().m_fgPresentationBanks[bank][2].ovrSwapchain == layer.ColorTexture[0]) {
                        const auto& pair = runtime().m_fgDepthPresentationBanks[bank];
                        return pair.preparedImage >= 0 && pair.output.lastCommittedIndex == pair.preparedImage &&
                               withDepth.DepthTexture[0] == pair.output.ovrSwapchain &&
                               withDepth.DepthTexture[1] == pair.output.ovrSwapchain;
                    }
                }
                return false;
            }
            return true;
        }
        static unsigned nativeRole(ovrTextureSwapChain chain) {
            for (const auto& bank : runtime().m_fgPresentationBanks)
                for (unsigned role = 0; role < bank.size(); ++role)
                    if (bank[role].ovrSwapchain && bank[role].ovrSwapchain == chain)
                        return role;
            return 4;
        }
        static void verifyNativeDelivery(bool nr) {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock contextLease(value.m_fgContextMutex);
            require(!value.m_useOculusRuntime && value.m_deliveryTransport,
                    "real VD backend uses independent delivery transport");
            require(value.m_frameGeneration && value.m_frameGeneration->initialized(), "native FG backend initialized");
            require(value.m_frameGenerationMode == requestedFrameGeneration, "native FG multiplier retained");
            VDXRFG_StatisticsV1 stats{};
            checkHr(value.m_frameGeneration->getStatistics(stats));
            require(stats.captures >= 20 && stats.pairs >= 10 && stats.synthetic_frames >= 10,
                    "real FG core produces repeated complete native groups");
            require(value.m_precompositor.dlssnrSettings.enabled == nr &&
                        std::abs(value.m_precompositor.dlssnrSettings.foveationSize - .9f) < .0001f &&
                        value.m_precompositor.dlssnrSettings.processingResolution == 85,
                    "native qualification uses the intended NR state and90/85 processing settings");
            if (nr)
                require(value.m_dlssnrFeature[0] && value.m_dlssnrFeature[1] &&
                            value.m_dlssnrFeature[0] != value.m_dlssnrFeature[1],
                        "both distinct native NR eyes initialized");
            else
                verifyNoNrGraph();
            checkHr(value.presentationDevice()->GetDeviceRemovedReason());
            checkHr(value.m_ovrSubmissionDevice->GetDeviceRemovedReason());
            std::cout << "NATIVE_GPU captures=" << stats.captures << " pairs=" << stats.pairs
                      << " generated=" << stats.synthetic_frames
                      << " finalCopies=" << value.m_deliveryTransport->statistics().nativeCopies << '\n';
        }
#endif
        static void captureExtra(DeliveryLayerFixture& fixture, const ovrLayerHeader* const* layers, unsigned count) {
            fixture.capture(runtime().m_ovrSession, layers, count);
        }
        static void verifyExtra(DeliveryLayerFixture& fixture) {
            runtime().waitForAsyncSubmissionIdle();
            std::unique_lock lease(runtime().m_fgContextMutex);
            fixture.verify();
            require(runtime().m_deliveryTransport && runtime().m_deliveryTransport->statistics().nativeCopies == 3 &&
                        runtime().m_deliveryTransport->statistics().nativeCommits == 3,
                    "final selection deduplicates three native chains and copies no unused FG intermediates");
        }
        static unsigned nrEvaluations[2];
        inline static unsigned successfulNrEvaluations[2]{}, failedNrEvaluations[2]{};
        static unsigned invalidNrParameters;
        static std::atomic<unsigned> syntheticPresentations;
        static std::map<ovrTextureSwapChain, unsigned> nrCommits;
        static std::map<ovrTextureSwapChain, unsigned> commitsBeforeInput;
        static std::vector<long long> presentedIds;
        static std::vector<std::pair<long long, unsigned>> presentedRoles;
        static ovrLayerEyeFov lastProjection;
        static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) originalEvaluate[2];
        inline static bool spatialObservation{};
        inline static ovrTextureSwapChain previousSpatialOutput{};
        inline static unsigned spatialSynthetic[3]{};
        inline static bool combinedSpatial{};
        inline static unsigned combinedSynthetic[3][3]{};
        struct DepthInput {
            double sampleTime{};
            unsigned frame{}, cycles{}, nativeCount{}, realCount{};
            bool accepted{}, sampled{}, haveCamera{}, validReservation{};
            ovrLayerEyeFov camera{};
            ovrLayerEyeFovDepth real{};
            int colourImage{-1}, depthImage{-1};
            unsigned colourCommits{}, depthCommits{};
            std::map<ovrTextureSwapChain, unsigned> commitsAtInput;
            std::array<ComPtr<ID3D11Texture2D>, 2> source, colour, depth;
            std::array<ovrRecti, 2> sourceRect{};
        };
        inline static std::vector<DepthInput> depthInputs;
        inline static std::map<ovrTextureSwapChain, unsigned> depthCommitsSeen, colourCommitsSeen;
        inline static std::map<ovrTextureSwapChain, unsigned> ordinaryCommitsSeen;
        inline static std::vector<ovrTextureSwapChain> alignedDepthChains;
        inline static unsigned depthSynthetic[3]{}, depthRepeats{}, depthOverwrites{};
        inline static bool depthInputCropped{};
        inline static unsigned depthEncodingCase{};
        inline static bool depthGuideColorOnly{};
        inline static bool depthOpacityCase{};

        // Queue independent CPU-readable copies while the worker owns its real
        // context lease. Mapping is deferred until all native groups drain.
        static ComPtr<ID3D11Texture2D> stageDepthFixtureImage(ID3D11Texture2D* image) {
            auto& value = runtime();
            D3D11_TEXTURE2D_DESC desc{};
            image->GetDesc(&desc);
            require(desc.ArraySize == 1 && desc.MipLevels == 1, "resolved fixture snapshot is one complete slice");
            desc.BindFlags = desc.MiscFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> copy;
            ComPtr<ID3D11Device> owner;
            ComPtr<ID3D11DeviceContext> context;
            image->GetDevice(&owner);
            owner->GetImmediateContext(&context);
            checkHr(owner->CreateTexture2D(&desc, nullptr, &copy));
            context->CopyResource(copy.Get(), image);
            return copy;
        }
        static void beforeDepthInput(unsigned frame, XrTime time) {
            std::unique_lock lock(captureMutex);
            if (depthGuideColorOnly)
                runtime().m_shouldUseDepth = false;
            DepthInput input;
            input.frame = frame;
            input.commitsAtInput = nrCommits;
            input.sampleTime = runtime().xrTimeToOvrTime(time);
            const auto& reservation = runtime().m_fgBegunReservation;
            input.validReservation = reservation.valid && reservation.multiplier >= 1 && reservation.multiplier <= 4;
            input.cycles = reservation.multiplier >= 1 && reservation.multiplier <= 4 ? reservation.multiplier : 1;
            input.sampled = frame < 4 || frame % 8 == 0 || frame >= 64;
            if (depthOpacityCase)
                for (const auto& prior : depthInputs)
                    if (std::abs(prior.sampleTime - input.sampleTime) < 1.e-7)
                        std::cout << "OPACITY_DUPLICATE_TIME frame=" << frame << " prior=" << prior.frame
                                  << " cycles=" << input.cycles << " sample_time=" << input.sampleTime << '\n';
            depthInputs.push_back(std::move(input));
        }
        static void observeDepthLayers(const ovrLayerHeader* const* layers, unsigned count) {
            require(count == 1 && layers && layers[0], "depth worker presents exactly the submitted projection");
            const auto& layer = *reinterpret_cast<const ovrLayerEyeFovDepth*>(layers[0]);
            auto found = std::find_if(depthInputs.begin(), depthInputs.end(), [&](const auto& input) {
                return input.sampleTime == layer.SensorSampleTime;
            });
            require(found != depthInputs.end(), "native role retains its public input sensor sample time");
            require(std::count_if(
                        depthInputs.begin(),
                        depthInputs.end(),
                        [&](const auto& candidate) { return candidate.sampleTime == layer.SensorSampleTime; }) == 1,
                    "native role has exactly one public timestamp owner");
            auto& input = *found;
            ++input.nativeCount;
            auto& value = runtime();
            unsigned role = 2, realBank = 2;
            for (unsigned bank = 0; bank < value.m_fgPresentationBanks.size(); ++bank)
                for (unsigned candidate = 0; candidate < 4; ++candidate)
                    if (value.m_fgPresentationBanks[bank][candidate].ovrSwapchain &&
                        value.m_fgPresentationBanks[bank][candidate].ovrSwapchain == layer.ColorTexture[0]) {
                        role = candidate;
                        if (role == 2)
                            realBank = bank;
                    }
            const bool synthetic = role != 2;
            require(input.validReservation || !synthetic, "invalid FG reservations present ordinary real B only");
            expectDepth[0] = expectDepth[1] = !synthetic && !depthGuideColorOnly;
            require(layer.Header.Type ==
                        (synthetic || depthGuideColorOnly ? ovrLayerType_EyeFov : ovrLayerType_EyeFovDepth),
                    "native synthetic roles are EyeFov and every real B retains EyeFovDepth");
            require(layer.Header.Flags == 0, "depth FG preserves the source projection flags");
            if (!input.haveCamera) {
                input.camera = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                input.haveCamera = true;
            }
            require(!memcmp(input.camera.RenderPose, layer.RenderPose, sizeof(layer.RenderPose)) &&
                        !memcmp(input.camera.Fov, layer.Fov, sizeof(layer.Fov)),
                    "synthetic, B and repeated B all retain this input's camera metadata");
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& fov = layer.Fov[eye];
                require(
                    std::abs(fov.LeftTan - std::tan(.8f)) < 1.e-6f && std::abs(fov.RightTan - std::tan(.8f)) < 1.e-6f &&
                        std::abs(fov.UpTan - std::tan(.8f)) < 1.e-6f && std::abs(fov.DownTan - std::tan(.8f)) < 1.e-6f,
                    "native depth group retains the independently specified FOV");
                if (synthetic || realBank < 2) {
                    const auto& rect = layer.Viewport[eye];
                    require(rect.Pos.x == int(eye) * 512 && rect.Pos.y == 0 && rect.Size.w == 512 &&
                                rect.Size.h == 512 && layer.ColorTexture[0] == layer.ColorTexture[1],
                            "every generated-group role uses the final packed stereo viewports");
                }
            }
            if (depthGuideColorOnly) {
                // EyeFov carries no depth payload. Do not inspect fields past
                // that native layer's extent; V5 admission is checked separately.
                if (synthetic) {
                    ++depthSynthetic[role == 3 ? 2 : role];
                    return;
                }
                const auto colourImage = written.at(layer.ColorTexture[0]);
                if (input.realCount++) {
                    if (depthOpacityCase &&
                        (memcmp(&input.real, &layer, sizeof(ovrLayerEyeFov)) || input.colourImage != colourImage ||
                         input.colourCommits != nrCommits[layer.ColorTexture[0]]))
                        std::cout << "OPACITY_REPEAT_MISMATCH input=" << input.frame
                                  << " timestamp=" << layer.SensorSampleTime
                                  << " old_chain=" << input.real.ColorTexture[0]
                                  << " new_chain=" << layer.ColorTexture[0] << " old_image=" << input.colourImage
                                  << " new_image=" << colourImage << " old_commits=" << input.colourCommits
                                  << " new_commits=" << nrCommits[layer.ColorTexture[0]] << '\n';
                    require(!memcmp(&input.real, &layer, sizeof(ovrLayerEyeFov)) && input.colourImage == colourImage &&
                                input.colourCommits == nrCommits[layer.ColorTexture[0]],
                            "color-only B repeats preserve the submitted color and camera");
                    ++depthRepeats;
                    return;
                }
                memcpy(&input.real, &layer, sizeof(ovrLayerEyeFov));
                input.colourImage = colourImage;
                input.colourCommits = nrCommits[layer.ColorTexture[0]];
                if (input.sampled)
                    for (unsigned eye = 0; eye < 2; ++eye)
                        input.colour[eye] =
                            stageDepthFixtureImage(getCommitted(value.m_ovrSession, layer.ColorTexture[eye]).Get());
                return;
            }
            if (synthetic) {
                require(!layer.DepthTexture[0] && !layer.DepthTexture[1] && !layer.ProjectionDesc.Projection22 &&
                            !layer.ProjectionDesc.Projection23 && !layer.ProjectionDesc.Projection32,
                        "a native synthetic role cannot inherit B depth handles or projection");
                ++depthSynthetic[role == 3 ? 2 : role];
                return;
            }
            require(layer.DepthTexture[0] && layer.DepthTexture[1] &&
                        (depthEncodingCase ? (std::isfinite(layer.ProjectionDesc.Projection22) &&
                                              std::isfinite(layer.ProjectionDesc.Projection23) &&
                                              layer.ProjectionDesc.Projection23 != 0)
                                           : (std::abs(layer.ProjectionDesc.Projection22 - -1.001001f) < 1.e-6f &&
                                              std::abs(layer.ProjectionDesc.Projection23 - -.1001001f) < 1.e-6f)) &&
                        layer.ProjectionDesc.Projection32 == -1.f,
                    "real native depth retains both eyes and the submitted near/far projection");
            const auto colourImage = written.at(layer.ColorTexture[0]);
            const auto depthImage = written.at(layer.DepthTexture[0]);
            if (input.realCount++) {
                require(!memcmp(&input.real, &layer, sizeof(layer)) && input.colourImage == colourImage &&
                            input.depthImage == depthImage && input.colourCommits == nrCommits[layer.ColorTexture[0]] &&
                            input.depthCommits == nrCommits[layer.DepthTexture[0]],
                        "padding/deadline B repeats the exact private pair without another commit");
                ++depthRepeats;
                return;
            }
            input.real = layer;
            input.colourImage = colourImage;
            input.depthImage = depthImage;
            input.colourCommits = nrCommits[layer.ColorTexture[0]];
            input.depthCommits = nrCommits[layer.DepthTexture[0]];
            if (realBank < 2) {
                const auto& depthBank = value.m_fgDepthPresentationBanks[realBank];
                require(layer.DepthTexture[0] == depthBank.output.ovrSwapchain &&
                            layer.DepthTexture[1] == depthBank.output.ovrSwapchain &&
                            depthImage == depthBank.preparedImage &&
                            colourImage == value.m_fgPresentationBanks[realBank][2].lastCommittedIndex,
                        "real B submits the independently prepared colour and depth images from the same bank");
                require(input.depthCommits == depthCommitsSeen[layer.DepthTexture[0]] + 1 &&
                            input.colourCommits == colourCommitsSeen[layer.ColorTexture[0]] + 1,
                        "each accepted B commits its private colour and depth chain exactly once");
                depthCommitsSeen[layer.DepthTexture[0]] = input.depthCommits;
                colourCommitsSeen[layer.ColorTexture[0]] = input.colourCommits;
            }
            if (realBank >= 2) {
                std::vector<ovrTextureSwapChain> checked;
                for (auto chain :
                     {layer.ColorTexture[0], layer.ColorTexture[1], layer.DepthTexture[0], layer.DepthTexture[1]}) {
                    if (std::find(checked.begin(), checked.end(), chain) != checked.end())
                        continue;
                    checked.push_back(chain);
                    require(nrCommits[chain] == ordinaryCommitsSeen[chain] + 1,
                            "ordinary fallback commits each selected colour/depth chain once before actual native End");
                    ordinaryCommitsSeen[chain] = nrCommits[chain];
                }
            }
            if (input.sampled)
                for (unsigned eye = 0; eye < 2; ++eye) {
                    input.colour[eye] =
                        stageDepthFixtureImage(getCommitted(value.m_ovrSession, layer.ColorTexture[eye]).Get());
                    input.depth[eye] =
                        stageDepthFixtureImage(getCommitted(value.m_ovrSession, layer.DepthTexture[eye]).Get());
                }
        }
        static void afterDepthInput(unsigned frame, XrTime time, bool nr) {
            auto& value = runtime();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock lock(captureMutex);
            auto& input = depthInputs.at(frame);
            input.accepted = value.m_fgPreviousCapturedTime == time;
            if (depthOpacityCase)
                std::cout << "OPACITY_INPUT frame=" << frame << " accepted=" << input.accepted
                          << " context_opaque=" << GuideApiSpy::contextOpaque << '\n';
            if (frame >= 2 && input.accepted != (frame != 66)) {
                const auto& r = value.m_fgBegunReservation;
                std::cerr << "RECOVERY_DIAGNOSTIC input=" << frame << " serial=" << value.m_frameBegun
                          << " supplied=" << time << " now=" << value.ovrTimeToXrTime(ovr_GetTimeInSeconds())
                          << " valid=" << r.valid << " shouldRender=" << r.shouldRender << " nativeId=" << r.nativeId
                          << " firstTarget=" << r.firstTarget << " currentTarget=" << r.currentTarget
                          << " multiplier=" << r.multiplier << " desired=" << value.m_fgDesiredMultiplier
                          << " workerEnd=" << value.m_fgWorkerGroupNativeId
                          << " publishedEnd=" << value.m_fgPublishedEndNativeId
                          << " unavailable=" << value.m_frameGenerationUnavailable << '\n';
            }
            if (frame >= 2)
                require(input.accepted == (frame != 66),
                        "sustained public depth frames are accepted except the explicit ordinary fallback");
            if (depthGuideColorOnly)
                require(!value.m_shouldUseDepth && value.m_precompositor.originalFgDepth[0].source &&
                            value.m_precompositor.originalFgDepth[1].source,
                        "native compositor ignores depth while both submitted original guide sources remain available");
            require(depthGuideColorOnly || (value.m_precompositor.finalDepth[0] && value.m_precompositor.finalDepth[1]),
                    "scaled/offset depth produces both actual aligned final images");
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto chain = value.m_precompositor.pendingDepthCommits[eye];
                require(depthGuideColorOnly ? !chain
                                            : ((depthEncodingCase == 4 || depthEncodingCase == 6) && !chain) || chain,
                        "aligned depth has a produced chain unless geometry or color-only policy avoids alignment");
                if (chain &&
                    std::find(alignedDepthChains.begin(), alignedDepthChains.end(), chain) == alignedDepthChains.end())
                    alignedDepthChains.push_back(chain);
                if (input.sampled) {
                    input.source[eye] = stageDepthFixtureImage(value.m_precompositor.finalColour[eye].Get());
                    input.sourceRect[eye] = {{nr ? int(eye) * 512 : 16, nr ? 0 : 12}, {512, 512}};
                }
            }
        }
        static void verifyDepthResults() {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock lock(captureMutex);
            for (const auto chain : alignedDepthChains)
                require(nrCommits[chain] == ordinaryCommitsSeen[chain],
                        "unused aligned intermediates never commit; each actual ordinary End accounts for its commit");
            unsigned accepted{}, samples{}, ordinary{};
            for (const auto& input : depthInputs) {
                accepted += input.accepted;
                ordinary += !input.accepted;
                require(input.nativeCount == input.cycles && input.realCount,
                        "every public depth input completes all reserved native slots and presents B");
                if (!input.sampled)
                    continue;
                ++samples;
                for (unsigned eye = 0; eye < 2; ++eye) {
                    require(input.source[eye] && input.colour[eye] && (depthGuideColorOnly || input.depth[eye]),
                            "sampled public input retains source and actual native B snapshots");
                    const auto& rect = input.real.Viewport[eye];
                    require(viewportPixels(input.source[eye].Get(), input.sourceRect[eye]) ==
                                viewportPixels(input.colour[eye].Get(), rect),
                            "native real-B pixels exactly preserve this frame's finalized colour after app reuse");
                    if (depthGuideColorOnly) {
                        require(input.real.Header.Type == ovrLayerType_EyeFov,
                                "depth-guided real B remains color-only at the actual native receiver");
                        continue;
                    }
                    D3D11_TEXTURE2D_DESC desc{};
                    input.depth[eye]->GetDesc(&desc);
                    D3D11_TEXTURE2D_DESC colourDesc{};
                    input.colour[eye]->GetDesc(&colourDesc);
                    require(desc.Width == colourDesc.Width && desc.Height == colourDesc.Height,
                            "native depth and colour resources map one to one");
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    ComPtr<ID3D11Device> depthOwner;
                    ComPtr<ID3D11DeviceContext> depthContext;
                    input.depth[eye]->GetDevice(&depthOwner);
                    depthOwner->GetImmediateContext(&depthContext);
                    checkHr(depthContext->Map(input.depth[eye].Get(), 0, D3D11_MAP_READ, 0, &mapped));
                    bool correct = true;
                    // Asymmetric edges distinguish submitted-rect alignment from
                    // incorrectly mapping the entire 384x320 source resource.
                    for (const auto probe : {std::array<int, 2>{64, 64},
                                             {200, 64},
                                             {256, 64},
                                             {64, 344},
                                             {64, 384},
                                             {200, 344},
                                             {256, 384}}) {
                        const int x = rect.Pos.x + probe[0];
                        const int y = rect.Pos.y + probe[1];
                        const float actual = depthAt(
                            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4, desc.Format);
                        const float sourceX = (depthEncodingCase == 4 || depthEncodingCase == 6)
                                                  ? 16.f + probe[0] + .5f
                                                  : 64.f + (probe[0] + .5f) * .5f;
                        const float sourceY = (depthEncodingCase == 4 || depthEncodingCase == 6)
                                                  ? 12.f + probe[1] + .5f
                                                  : 32.f + (probe[1] + .5f) * .5f;
                        const float expected = depthEncodingCase >= 5 && !eye
                                                   ? 1.f
                                                   : (eye ? .55f : .15f) + input.frame * .001f +
                                                         (sourceX >= 160.f ? .1f : 0.f) +
                                                         (sourceY >= 208.f ? .2f : 0.f);
                        if (depthEncodingCase) {
                            // Independent physical-distance oracle: interpolate
                            // inverse endpoint distances in the submitted eye's
                            // raw range, then decode the actual native pixels
                            // using the ONE projection delivered to LibOVR.
                            const double minimum = depthEncodingCase == 2 ? (eye ? .05f : .1f) : 0;
                            const double maximum = depthEncodingCase == 2 ? (eye ? .98f : .95f) : 1;
                            const double u = (expected - minimum) / (maximum - minimum);
                            const double inverseNear = depthEncodingCase == 5   ? (eye ? 0.0 : 1.0)
                                                       : depthEncodingCase == 6 ? 1.0
                                                       : depthEncodingCase == 3 ? (eye ? 0.0 : .1)
                                                                                : (eye ? .5 : 1.0);
                            const double inverseFar = depthEncodingCase == 5   ? (eye ? 2.0 : 1e-8)
                                                      : depthEncodingCase == 6 ? (eye ? 0.0 : 1e-8)
                                                      : depthEncodingCase == 3 ? (eye ? .5 : 1.0)
                                                                               : (eye ? .05 : .1);
                            const double expectedMeters = 1.0 / ((1.0 - u) * inverseNear + u * inverseFar);
                            const auto& projection = input.real.ProjectionDesc;
                            const double actualMeters =
                                double(projection.Projection23) / (double(actual) + projection.Projection22);
                            const bool matches = std::isfinite(actual) && actual >= 0 && actual <= 1 &&
                                                 std::isfinite(actualMeters) && actualMeters > 0 &&
                                                 std::abs(actualMeters - expectedMeters) < expectedMeters * .0002;
                            if (!matches)
                                std::cerr << "DEPTH_METERS frame=" << input.frame << " eye=" << eye
                                          << " accepted=" << input.accepted << " raw=" << actual
                                          << " actual=" << actualMeters << " expected=" << expectedMeters << '\n';
                            correct &= matches;
                            continue;
                        }
                        if (!std::isfinite(actual) || std::abs(actual - expected) >= .0001f)
                            std::cerr << "DEPTH_PIXEL frame=" << input.frame << " eye=" << eye
                                      << " accepted=" << input.accepted << " probe=" << probe[0] << ',' << probe[1]
                                      << " actual=" << actual << " expected=" << expected << '\n';
                        correct &= std::isfinite(actual) && std::abs(actual - expected) < .0001f;
                    }
                    depthContext->Unmap(input.depth[eye].Get(), 0);
                    require(correct,
                            "native B depth preserves each eye's physical distances, geometry and discontinuities "
                            "after reuse");
                }
            }
            require(accepted >= 64 && ordinary && depthOverwrites == depthInputs.size(),
                    "at least 64 accepted depth groups surround ordinary fallback and immediate app reuse");
            require(depthSynthetic[0] >= 4 && (requestedFrameGeneration == 2 || depthSynthetic[1] >= 4) &&
                        (requestedFrameGeneration != 4 || depthSynthetic[2] >= 4) && depthRepeats,
                    "every requested synthetic role and repeated real B reach actual native EndFrame");
            require(!value.m_fgTransactionForSubmission.ticket.token && !value.m_fgTransactionForFrame.ticket.token &&
                        !value.m_fgDepthCompletionUnknown,
                    "all real depth transactions settle before normal shutdown");
            std::cout << "DEPTH_END_FRAME FG=" << requestedFrameGeneration << " accepted=" << accepted
                      << " ordinary=" << ordinary << " samples=" << samples << " overwrites=" << depthOverwrites
                      << " S1=" << depthSynthetic[0] << " S2=" << depthSynthetic[1] << " S3=" << depthSynthetic[2]
                      << " repeats=" << depthRepeats << " pixel_pairs=exact\n";
        }
        // Hand-derived for native512: coverage95->488,90->460; then ceil/align4.
        static int expectedSpatialSize() {
            return coverage == 95 ? 392 : coverage == 90 ? (nrProcessingResolution == 85 ? 392 : 368) : 412;
        }
        template <unsigned eye>
        static NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D12GraphicsCommandList* commands,
                                                    const NVSDK_NGX_Handle* feature,
                                                    const NVSDK_NGX_Parameter* parameters,
                                                    PFN_NVSDK_NGX_ProgressCallback callback) {
            ++nrEvaluations[eye];
            const auto hasInt = [&](const char* name, int expected) {
                int actual{};
                return NVSDK_NGX_SUCCEED(parameters->Get(name, &actual)) && actual == expected;
            };
            const auto hasFloat = [&](const char* name, float expected) {
                float actual{};
                return NVSDK_NGX_SUCCEED(parameters->Get(name, &actual)) && std::abs(actual - expected) < .0001f;
            };
            const bool spatial = spatialObservation && nrProcessingResolution != 100;
            const int size = spatial ? expectedSpatialSize() : 512;
            if (spatial) {
                ID3D12Resource *color{}, *answer{};
                if (!NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.Color", &color)) ||
                    !NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.Output", &answer)) || !color || !answer ||
                    color->GetDesc().Width != unsigned(size) || color->GetDesc().Height != unsigned(size) ||
                    answer->GetDesc().Width != unsigned(size) || answer->GetDesc().Height != unsigned(size))
                    ++invalidNrParameters;
                if (depthInputCropped) {
                    ID3D12Resource* depth{};
                    if (!NVSDK_NGX_SUCCEED(parameters->Get("DLSSNR.Depth", &depth)) || !depth ||
                        depth->GetDesc().Width != unsigned(size) || depth->GetDesc().Height != unsigned(size) ||
                        !hasInt("DLSSNR.DepthSubrectBaseX", 0) || !hasInt("DLSSNR.DepthSubrectBaseY", 0) ||
                        !hasInt("DLSSNR.DepthSubrectWidth", size) || !hasInt("DLSSNR.DepthSubrectHeight", size))
                        ++invalidNrParameters;
                }
            }
            if (!hasInt("DLSSNR.Style", 2) || !hasInt("DLSSNR.Hint.Render.Preset", 2) ||
                !hasFloat("DLSSNR.Intensity", .91f) || !hasFloat("DLSSNR.LocalToneStrength", 1.f) ||
                !hasFloat("DLSSNR.LocalStructureStrength", 1.f) || !hasFloat("DLSSNR.SkinStructureStrength", 1.f) ||
                !hasInt("DLSSNR.ColorSubrectWidth", size) || !hasInt("DLSSNR.ColorSubrectHeight", size) ||
                !hasInt("DLSSNR.OutputSubrectWidth", size) || !hasInt("DLSSNR.OutputSubrectHeight", size) ||
                !hasInt("DLSSNR.ColorSubrectBaseX", depthInputCropped && !spatial ? 16 : 0) ||
                !hasInt("DLSSNR.ColorSubrectBaseY", depthInputCropped && !spatial ? 12 : 0) ||
                !hasInt("DLSSNR.OutputSubrectBaseX", spatial ? 0 : eye * 512) ||
                !hasInt("DLSSNR.OutputSubrectBaseY", 0))
                ++invalidNrParameters;
            // Always forward to actual Ada inference; diagnostics must not
            // substitute success or throw through the callback boundary.
            const auto result = originalEvaluate[eye](commands, feature, parameters, callback);
            if (NVSDK_NGX_SUCCEED(result))
                ++successfulNrEvaluations[eye];
            else
                ++failedNrEvaluations[eye];
            return result;
        }
        static void observe() {
            auto& value = runtime();
            for (unsigned eye = 0; eye < 2; ++eye) {
                originalEvaluate[eye] = value.m_dlssnrFeatureEvaluate[eye];
            }
            value.m_dlssnrFeatureEvaluate[0] = evaluate<0>;
            value.m_dlssnrFeatureEvaluate[1] = evaluate<1>;
        }
        static NVSDK_NGX_Result NVSDK_CONV evaluateSpatial(ID3D12GraphicsCommandList* commands,
                                                           const NVSDK_NGX_Handle* feature,
                                                           const NVSDK_NGX_Parameter* parameters,
                                                           PFN_NVSDK_NGX_ProgressCallback callback) {
            const bool left = feature && feature == runtime().m_dlssnrFeature[0];
            const bool right = feature && feature == runtime().m_dlssnrFeature[1];
            if (left != right)
                return left ? evaluate<0>(commands, feature, parameters, callback)
                            : evaluate<1>(commands, feature, parameters, callback);
            ++invalidNrParameters;
            return originalEvaluate[0](commands, feature, parameters, callback);
        }
        static void observeSpatial(bool enable) {
            if (enable) {
                originalEvaluate[0] = runtime().m_dlssnrFeatureEvaluate[0];
                require(originalEvaluate[0] == runtime().m_dlssnrFeatureEvaluate[1],
                        "one real vendor evaluation export serves both recreated eye features");
            } else if (!spatialObservation) {
                return;
            }
            // Hook the actual export across feature recreation; do not replace
            // production callbacks that the native100/spatial transition clears.
            checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
            checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
            const auto operation = enable ? DetourAttach : DetourDetach;
            checkHr(HRESULT_FROM_WIN32(operation(reinterpret_cast<PVOID*>(&originalEvaluate[0]), evaluateSpatial)));
            checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
            originalEvaluate[1] = originalEvaluate[0];
            spatialObservation = enable;
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
        static void drain() {
            runtime().waitForAsyncSubmissionIdle();
        }
        static void drainAndVerifyPixels(std::optional<long long> finalNativeId = {}) {
            drain();
            {
                std::lock_guard lock(captureMutex);
                if (!captureFailure.empty())
                    throw std::runtime_error(captureFailure);
                if (finalNativeId)
                    require(!presentedIds.empty() && presentedIds.back() == *finalNativeId,
                            "final ordinary input must complete its native End before readback");
            }
            verifyPixels();
        }
        static void forceFallback() {
            runtime().m_fgBegunReservation.shouldRender = false;
        }
        static void retainBusyHistory(std::array<OwnedFrameTransaction, 2>& held, bool bothBanks) {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock lease(value.m_fgContextMutex);
            require(value.m_frameGeneration && value.m_fgPreviousCapturedTime,
                    "busy-reset fixture starts after an actual public capture");
            const auto period = value.m_fgBegunReservation.period;
            FrameGenerationCadence cadence;
            FinalProjectionInput input;
            input.period = period;
            input.colour = value.m_precompositor.finalColour;
            const bool packed = input.colour[0] == input.colour[1];
            for (unsigned eye = 0; eye < 2; ++eye) {
                auto& view = input.views[eye];
                view.rect = {packed ? eye * 512u : 0u, 0, 512, 512};
                view.orientation[3] = 1.f;
                view.position[0] = eye ? .032f : -.032f;
                view.fov[0] = view.fov[3] = -.8f;
                view.fov[1] = view.fov[2] = .8f;
            }
            for (unsigned index = 0; index < (bothBanks ? 2u : 1u); ++index) {
                input.serial = value.m_frameBegun + index + 1;
                const auto previous = value.m_fgPreviousCapturedTime + index * 3 * period;
                input.time = previous + 3 * period;
                input.slots = cadence.plan(cadence.reserve(0, previous + period, period, 3, 0), previous, true);
                checkHr(value.m_frameGeneration->capture(input, held[index]));
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (!value.m_frameGeneration->ready(held[index]) && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(1ms);
                }
                require(value.m_frameGeneration->ready(held[index]), "bounded held-history GPU readiness");
            }
            require(held[0].ticket.token && value.m_frameGeneration->reset(1) == VDXRFG_E_BUSY,
                    "a real retained GPU ticket prevents history reset");
            if (bothBanks) {
                OwnedFrameTransaction refused;
                require(held[1].ticket.token && held[0].ticket.bank != held[1].ticket.bank &&
                            value.m_frameGeneration->capture(input, refused) == VDXRFG_E_BUSY && !refused.ticket.token,
                        "both retained real banks prevent capture without accepting another GPU ticket");
            }
        }
        static void retireBusyHistory(OwnedFrameTransaction& held) {
            if (!held.ticket.token) {
                return;
            }
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            HRESULT result = VDXRFG_E_BUSY;
            do {
                {
                    std::unique_lock lease(value.m_fgContextMutex);
                    result = value.m_frameGeneration->retire(held);
                }
                if (result == VDXRFG_E_BUSY) {
                    std::this_thread::sleep_for(1ms);
                }
            } while (result == VDXRFG_E_BUSY && std::chrono::steady_clock::now() < deadline);
            checkHr(result);
        }
        static void verifyBusyFallback() {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            require(value.m_fgPreviousCapturedTime == 0 && !value.m_fgTransactionForSubmission.ticket.token,
                    "the public End bypasses busy reset without publishing a partial GPU ticket");
        }
        static void verifyRecoveryCadence(const XrFrameState& state, bool native, XrTime previous) {
            const auto& value = runtime();
            const auto& reservation = value.m_fgWaitedReservation;
            const unsigned multiplier = native ? 1u : requestedFrameGeneration;
            std::cout << "public recovery cadence: multiplier=" << reservation.multiplier
                      << " period=" << state.predictedDisplayPeriod << " first=" << reservation.firstTarget
                      << " target=" << state.predictedDisplayTime << '\n';
            require(reservation.valid && reservation.multiplier == multiplier &&
                        state.predictedDisplayPeriod == multiplier * reservation.period &&
                        state.predictedDisplayTime == reservation.firstTarget + (multiplier - 1) * reservation.period &&
                        state.predictedDisplayTime - previous >= multiplier * reservation.period,
                    native ? "unsupported composition must restore the public native application cadence"
                           : "temporary busy reset must not release the public application at native cadence");
        }
        static void verifyRecoveryGroup(size_t begin, uint64_t expectedCaptures, uint64_t expectedPairs, bool pair) {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            const auto current = statistics();
            std::cout << "public recovery presentation: native_slots=" << (presentedRoles.size() - begin)
                      << " captures_delta=" << (current.captures - expectedCaptures + 1)
                      << " pairs_delta=" << (current.pairs - expectedPairs + (pair ? 1u : 0u)) << '\n';
            require(current.captures == expectedCaptures && current.pairs == expectedPairs,
                    pair ? "the next public capture produces exactly one recovered synthesized pair"
                         : "recovery consumes exactly one public capture and primes without a stale pair");
            require(presentedRoles.size() == begin + 3 && presentedRoles[begin].second == (pair ? 0u : 2u) &&
                        presentedRoles[begin + 1].second == (pair ? 1u : 2u) && presentedRoles[begin + 2].second == 2 &&
                        presentedRoles[begin].first + 1 == presentedRoles[begin + 1].first &&
                        presentedRoles[begin + 1].first + 1 == presentedRoles[begin + 2].first,
                    pair ? "the immediately following public group presents consecutive S1/S2/B native slots"
                         : "a recovered public prime preserves all three consecutive native slots");
        }
        static void verifyCaptureRoute(XrTime time, bool accepted) {
            require((runtime().m_fgPreviousCapturedTime == time) == accepted,
                    "source overwrite exercises the intended accepted or bypassed input route");
        }
        inline static std::atomic<unsigned> finalColourDestroyed{};
        inline static unsigned finalColourOwners{};
        struct FinalColourLifetime final : IUnknown {
            std::atomic<ULONG> references{1};
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
                if (!object)
                    return E_POINTER;
                *object = nullptr;
                if (iid != __uuidof(IUnknown))
                    return E_NOINTERFACE;
                *object = this;
                AddRef();
                return S_OK;
            }
            ULONG STDMETHODCALLTYPE AddRef() override {
                return ++references;
            }
            ULONG STDMETHODCALLTYPE Release() override {
                const auto remaining = --references;
                if (!remaining) {
                    ++finalColourDestroyed;
                    delete this;
                }
                return remaining;
            }
        };
        static void watchFinalColourCleanup() {
            const auto& colours = runtime().m_precompositor.finalColour;
            constexpr GUID witness{0x2e4a6b10, 0x0838, 0x4fb1, {0x81, 0x48, 0x12, 0xb1, 0x6b, 0xe5, 0x52, 0x31}};
            finalColourDestroyed = 0;
            finalColourOwners = 0;
            for (unsigned eye = 0; eye < colours.size(); ++eye) {
                if (!colours[eye])
                    continue;
                if (eye && colours[eye] == colours[0])
                    continue;
                ComPtr<IUnknown> lifetime;
                lifetime.Attach(new FinalColourLifetime);
                checkHr(colours[eye]->SetPrivateDataInterface(witness, lifetime.Get()));
                ++finalColourOwners;
            }
            require(finalColourOwners || !requestedFrameGeneration,
                    "enabled FG cleanup probe starts with real finalized colour ownership");
            std::cout << "FINAL_COLOUR_OWNERS before_cleanup=" << finalColourOwners << '\n';
        }
        using CleanupEvent = HRESULT(STDMETHODCALLTYPE*)(ID3D11Fence*, UINT64, HANDLE);
        inline static CleanupEvent originalCleanupEvent{};
        inline static ID3D11Fence* cleanupFaultFence{};
        inline static unsigned cleanupFaults{};
        static HRESULT STDMETHODCALLTYPE failCleanupEvent(ID3D11Fence* fence, UINT64 value, HANDLE event) {
            if (fence == cleanupFaultFence) {
                ++cleanupFaults;
                return E_FAIL;
            }
            return originalCleanupEvent(fence, value, event);
        }
        static void verifyFailedCleanup(PFN_xrDestroySession destroy, XrSession session) {
            auto& value = runtime();
            const auto colours = value.m_precompositor.finalColour;
            cleanupFaultFence = value.m_ovrSubmissionCompletionFence.Get();
            require(cleanupFaultFence != nullptr, "submission drain fault targets the actual completion fence");
            // ID3D11Fence inherits seven IUnknown/DeviceChild entries; completion is entry9.
            originalCleanupEvent = reinterpret_cast<CleanupEvent>((*reinterpret_cast<void***>(cleanupFaultFence))[9]);
            const auto detour = [](bool attach) {
                checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
                checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
                const auto operation = attach ? DetourAttach : DetourDetach;
                checkHr(
                    HRESULT_FROM_WIN32(operation(reinterpret_cast<PVOID*>(&originalCleanupEvent), failCleanupEvent)));
                checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
            };
            detour(true);
            auto detach = MakeScopeGuard([&] { detour(false); });
            const auto result = destroy(session);
            require(XR_FAILED(result) && cleanupFaults == 1,
                    "public session cleanup reports the injected real submission drain failure");
            require(value.m_precompositor.finalColour == colours && finalColourDestroyed == 0,
                    "failed submission drain preserves both final colour owners");
            std::cout << "FINAL_COLOUR_FAILED_DRAIN preserved=" << finalColourOwners << '\n';
        }
        static void verifySessionCleanup() {
            const auto& value = runtime();
            std::cout << "FINAL_COLOUR_OWNERS after_cleanup=" << finalColourDestroyed.load() << '\n';
            require(finalColourDestroyed == finalColourOwners,
                    "successful session cleanup releases the actual final colour COM owners");
            for (const auto& colour : value.m_precompositor.finalColour)
                require(!colour, "successful session cleanup clears every finalized colour reference");
            require(!value.m_frameGeneration && !value.m_fgDevice && !value.m_fgSourceFence11 &&
                        !value.m_fgSourceFence12 && !value.m_dlssnrResourcesInitialized,
                    "session cleanup retires FG devices, imported source fences and lazy NR readiness");
            require(!value.m_dlssnrOutputSwapchain && value.m_dlssnrOutputSwapchainImages.empty() &&
                        value.m_dlssnrOutputSwapchainTextures.empty() && value.m_dlssnrOutputSwapchainSrvs.empty(),
                    "session cleanup releases the NR output swapchain and its resource rings");
            for (const auto& bank : value.m_fgPresentationBanks)
                for (const auto& role : bank)
                    require(!role.ovrSwapchain && role.images.empty(),
                            "session cleanup releases every FG presentation chain and image");
            for (const auto& bank : value.m_fgDepthPresentationBanks)
                require(!bank.output.ovrSwapchain && bank.output.images.empty() && bank.output.dsvs.empty() &&
                            !bank.sources[0] && !bank.sources[1] && !bank.inputCopies[0] && !bank.inputCopies[1] &&
                            !bank.srvs[0] && !bank.srvs[1] && !bank.token,
                        "normal session cleanup releases every private depth bank and retained source");
            require(!value.m_precompositor.finalDepth[0] && !value.m_precompositor.finalDepth[1],
                    "normal session cleanup releases final aligned depth ownership");
        }
        static uint64_t expectedReanchorAfterDiscard() {
            auto& value = runtime();
            std::unique_lock lock(value.m_asyncSubmissionMutex);
            require(value.m_fgTimelineReanchor && value.m_frameBegun > value.m_frameCompleted &&
                        value.m_fgBegunReservation.multiplier == 4,
                    "4x discard probe reaches a pending begun group before its End");
            return std::max(value.m_fgWorkerGroupNativeId.load(), value.m_fgPublishedEndNativeId.load()) + 4;
        }
        static void verifyQueuedReanchor(uint64_t expected) {
            require(runtime().m_fgWaitedReservation.nativeId == expected,
                    "pipelined Wait after 4x discard must reserve past all four pending native slots");
        }
        static void verifyPublishedSpan() {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            if (value.m_fgBegunReservation.multiplier == 4)
                require(value.m_fgPublishedEndNativeId.load() == value.m_fgWorkerGroupNativeId.load(),
                        "4x publication and completed worker must account for the same four native IDs");
        }
        inline static bool priorInputFallback{};
        static void verifyInternalChains() {
            auto& value = runtime();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            if (value.m_frameGeneration) {
                std::set<ovrTextureSwapChain> chains;
                unsigned count{};
                for (const auto& bank : value.m_fgPresentationBanks)
                    for (const auto& role : bank)
                        if (role.ovrSwapchain) {
                            ++count;
                            chains.insert(role.ovrSwapchain);
                            require(!role.images.empty(), "every presentation chain owns its SDK image ring");
                        }
                require(count == value.m_frameGenerationMode + 1 && chains.size() == count,
                        "public FG allocates one chain per synthetic role and two independent real-B chains");
            }
            if (value.m_fgPreviousCapturedTime != value.m_precompositor.displayTime) {
                priorInputFallback = true;
                return; // A complete NR-preserving fallback must commit its final source.
            }
            priorInputFallback = false;
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
            // Ticketless fallback cannot grant capacity until its native batch
            // commits. Snapshot after that boundary so the next accepted input
            // cannot be charged for the previous input's delayed native Commit.
            if (priorInputFallback && runtime().m_frameGenerationMode)
                runtime().waitForFrameGenerationCapacity();
            std::unique_lock lock(captureMutex);
            commitsBeforeInput = nrCommits;
        }
        static void beginLookupProbe(NrLookupProbe& probe) {
            probe.begin(runtime().m_dlssnrOutputSwapchainSrvs);
        }
        static void verifyLookupResult(bool postprocess) {
            const auto& value = runtime();
            require(value.m_dlssnrOutputSwapchainTextures.size() == value.m_dlssnrOutputSwapchainImages.size() &&
                        value.m_dlssnrOutputSwapchainTextures.size() == value.m_dlssnrOutputSwapchainSrvs.size(),
                    "retained NR texture, D3D12 image and SRV rings stay aligned");
            ComPtr<ID3D11Resource> packed;
            value.m_dlssnrOutputSwapchainSrvs[value.m_dlssnrOutputSwapchainLastWrittenIndex]->GetResource(&packed);
            ComPtr<ID3D11Texture2D> texture;
            checkHr(packed.As(&texture));
            require(value.m_dlssnrOutputSwapchainTextures[value.m_dlssnrOutputSwapchainLastWrittenIndex] == texture,
                    "the retained current bank matches its independently resolved SRV texture");
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
#ifdef VDXR_DELIVERY_REGRESSION
            ComPtr<ID3D11Texture2D> sdkImage;
            require(OVR_SUCCESS(ovr_GetTextureSwapChainBufferDX(
                        runtime().m_ovrSession, projection.ColorTexture[0], 0, IID_PPV_ARGS(&sdkImage))),
                    "native SDK image query succeeds at actual End");
            ComPtr<ID3D11Device> sdkDevice;
            sdkImage->GetDevice(&sdkDevice);
            ComPtr<IUnknown> sdkIdentity, producerIdentity;
            checkHr(sdkDevice.As(&sdkIdentity));
            checkHr(runtime().m_ovrSubmissionDevice.As(&producerIdentity));
            require(sdkIdentity != producerIdentity, "native SDK output and private producer domains must differ");
#endif
            lastProjection = projection;
            unsigned presentedRole = 2;
            // Called within production End's context lease, before the backend
            // consumes this actual layer. A synthesized GPU pair alone is not
            // evidence that any synthetic pixels were submitted.
            for (const auto& bank : runtime().m_fgPresentationBanks) {
                for (const unsigned role : {0u, 1u, 3u}) {
                    if (bank[role].ovrSwapchain && bank[role].ovrSwapchain == projection.ColorTexture[0]) {
                        ++syntheticPresentations;
                        const unsigned syntheticIndex = role == 3 ? 2 : role;
                        if (runtime().m_nrSpatialPipeline && runtime().m_nrSpatialFeatures &&
                            runtime().m_precompositor.dlssnrSettings.processingResolution != 100) {
                            ++spatialSynthetic[syntheticIndex];
                            const auto& settings = runtime().m_precompositor.dlssnrSettings;
                            if (combinedSpatial && settings.foveationSize < 1.f) {
                                const unsigned cohort = settings.foveationSize > .925f        ? 0
                                                        : settings.processingResolution == 85 ? 2
                                                                                              : 1;
                                ++combinedSynthetic[cohort][syntheticIndex];
                            }
                        }
                        presentedRole = role;
                    }
                }
            }
            require(!presentedIds.empty(), "projection follows its actual native End ID");
            presentedRoles.emplace_back(presentedIds.back(), presentedRole);
        }
        static void verifyPixels() {
            auto& value = runtime();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            for (unsigned eye = 0; eye < 2; ++eye) {
                readColor(value.m_ovrSession, lastProjection, eye);
            }
        }
        static std::vector<uint8_t> viewportPixels(ID3D11Texture2D* image, const ovrRecti& rect) {
            require(image, "viewport pixel readback requires a submitted projection image");
            D3D11_TEXTURE2D_DESC desc{};
            image->GetDesc(&desc);
            require(rect.Pos.x >= 0 && rect.Pos.y >= 0 && rect.Size.w > 0 && rect.Size.h > 0 &&
                        unsigned(rect.Pos.x + rect.Size.w) <= desc.Width &&
                        unsigned(rect.Pos.y + rect.Size.h) <= desc.Height && desc.ArraySize == 1 && desc.MipLevels == 1,
                    "bounded complete real-frame viewport");
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                              desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            require(bgra || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                    "known real-frame RGBA layout");
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            image->GetDevice(&device);
            device->GetImmediateContext(&context);
            desc.BindFlags = desc.MiscFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
            context->CopyResource(staging.Get(), image);
            std::vector<uint8_t> pixels(size_t(rect.Size.w) * rect.Size.h * 4);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            for (int y = 0; y < rect.Size.h; ++y) {
                const auto* row =
                    static_cast<const uint8_t*>(mapped.pData) + (rect.Pos.y + y) * mapped.RowPitch + rect.Pos.x * 4;
                auto* output = pixels.data() + size_t(y) * rect.Size.w * 4;
                memcpy(output, row, size_t(rect.Size.w) * 4);
                if (bgra)
                    for (int x = 0; x < rect.Size.w; ++x)
                        std::swap(output[x * 4], output[x * 4 + 2]);
            }
            context->Unmap(staging.Get(), 0);
            return pixels;
        }
        static void verifyRealPixels(const std::filesystem::path& directory) {
            std::filesystem::create_directories(directory);
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            bool realBank = false;
            for (const auto& bank : value.m_fgPresentationBanks)
                realBank |= bank[2].ovrSwapchain && bank[2].ovrSwapchain == lastProjection.ColorTexture[0] &&
                            bank[2].ovrSwapchain == lastProjection.ColorTexture[1];
            require(realBank, "final NR presentation is the module real-B role");
            // The worker consumes the source layer list. These fixed NR scenarios
            // retain either one packed NR texture or separate postprocessed eyes.
            const bool packed = value.m_precompositor.finalColour[0] == value.m_precompositor.finalColour[1];
            for (unsigned eye = 0; eye < 2; ++eye) {
                const ovrRecti source{{packed ? static_cast<int>(eye) * value.m_fgEyeResolution.w : 0, 0},
                                      value.m_fgEyeResolution};
                require(lastProjection.Viewport[eye].Size.w == source.Size.w &&
                            lastProjection.Viewport[eye].Size.h == source.Size.h,
                        "real NR presentation retains both eye viewport dimensions");
                auto presented = getCommitted(value.m_ovrSession, lastProjection.ColorTexture[eye]);
                const auto pixels = viewportPixels(value.m_precompositor.finalColour[eye].Get(), source);
                require(pixels == viewportPixels(presented.Get(), lastProjection.Viewport[eye]),
                        "all real NR RGBA pixels survive frame-generation presentation exactly");
                std::ofstream output(directory / ("final-real-eye" + std::to_string(eye) + ".rgba"), std::ios::binary);
                output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
                require(output.good(), "save complete final real NR pixels for cross-backend comparison");
            }
        }
        static void verifyTransitionPixels(const std::filesystem::path& directory,
                                           const XrCompositionLayerProjectionView* views,
                                           bool srgb) {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            bool realBank = false;
            for (const auto& bank : value.m_fgPresentationBanks)
                realBank |= bank[2].ovrSwapchain && bank[2].ovrSwapchain == lastProjection.ColorTexture[0] &&
                            bank[2].ovrSwapchain == lastProjection.ColorTexture[1];
            require(requestedFrameGeneration && value.m_precompositor.isProj0SRGB == srgb &&
                        value.m_precompositor.finalColour[0] == value.m_precompositor.finalColour[1],
                    "transition checkpoint retains one packed NR source with the submitted encoding");
            const bool canonical = nrProcessingResolution < 100;
            const auto sourceFormat = canonical ? (srgb ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM)
                                                : (srgb ? OVR_FORMAT_B8G8R8A8_UNORM_SRGB : OVR_FORMAT_B8G8R8A8_UNORM);
            const auto presentedFormat =
                realBank ? (srgb ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM) : sourceFormat;
            require(value.m_dlssnrOutputSwapchainFormat == sourceFormat,
                    "transition NR source retains its independently expected family and encoding");
            const auto sourceImage =
                value.m_dlssnrOutputSwapchainTextures[value.m_dlssnrOutputSwapchainLastWrittenIndex];
            require(value.m_precompositor.finalColour[0] == sourceImage,
                    "transition checkpoint reads this input's retained NR output bank");
            const int stride =
                std::max(views[0].subImage.imageRect.extent.width, views[1].subImage.imageRect.extent.width);
            require(value.m_dlssnrOutputSwapchainResolution.w == stride,
                    "current view geometry determines packed NR stride independently of stale FG state");
            std::filesystem::create_directories(directory);
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& extent = views[eye].subImage.imageRect.extent;
                const ovrRecti source{{static_cast<int>(eye) * stride, 0}, {extent.width, extent.height}};
                const auto& viewport = lastProjection.Viewport[eye];
                require(viewport.Size.w == extent.width && viewport.Size.h == extent.height &&
                            viewport.Pos.x == source.Pos.x && viewport.Pos.y == source.Pos.y,
                        "transition presentation preserves current native eye dimensions and packing");
                require(realBank || lastProjection.ColorTexture[eye] == value.m_dlssnrOutputSwapchain,
                        "transition presents either module real-B or the complete current native NR source");
                ovrTextureSwapChainDesc description{};
                require(OVR_SUCCESS(ovr_GetTextureSwapChainDesc(
                            value.m_ovrSession, lastProjection.ColorTexture[eye], &description)) &&
                            description.Format == presentedFormat,
                        "transition presentation metadata preserves the matching transfer function and actual family");
                auto presented = getCommitted(value.m_ovrSession, lastProjection.ColorTexture[eye]);
                const auto pixels = viewportPixels(sourceImage.Get(), source);
                require(pixels == viewportPixels(presented.Get(), viewport),
                        "all transition NR pixels survive native fallback or real-B presentation exactly");
                std::ofstream output(directory / ("transition-eye" + std::to_string(eye) + ".rgba"), std::ios::binary);
                output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
                require(output.good(), "save complete transition pixels for independent comparison");
            }
            std::cout << "TRANSITION_PIXELS processing=" << nrProcessingResolution << " FG=" << requestedFrameGeneration
                      << " srgb=" << srgb << " route=" << (realBank ? "real-B" : "native") << " stride=" << stride
                      << " format=" << presentedFormat << " exact\n";
        }
        static void verifyPostprocessTransitionPixels(const std::filesystem::path& directory,
                                                      const XrCompositionLayerProjectionView* views,
                                                      bool srgb) {
            auto& value = runtime();
            value.waitForAsyncSubmissionIdle();
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            require(combinedSpatial && requestedFrameGeneration && coverage == 90 && nrProcessingResolution == 80 &&
                        nrUpscalingSetting == 100 && value.m_upscalingMultiplier == 1.f && nrSharpenSetting == 25 &&
                        value.m_precompositor.isProj0SRGB == srgb,
                    "combined checkpoint uses90/80 with25% sharpening and session-native upscaling");
            bool realBank = false;
            for (const auto& bank : value.m_fgPresentationBanks)
                realBank |= bank[2].ovrSwapchain && bank[2].ovrSwapchain == lastProjection.ColorTexture[0] &&
                            bank[2].ovrSwapchain == lastProjection.ColorTexture[1];
            // Upscaling is session-initialized. This live phase changes sharpening
            // only, so the complete512-square source is independent of stale FG state.
            constexpr int size = 512;
            const auto format = srgb ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM;
            std::filesystem::create_directories(directory);
            auto& input = *reinterpret_cast<OpenXrRuntime::Swapchain*>(views[0].subImage.swapchain);
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& source = input.stereoProjection[eye];
                require(source.ovrSwapchain && source.lastCommittedIndex >= 0 &&
                            size_t(source.lastCommittedIndex) < source.images.size(),
                        "current input owns a complete committed per-eye postprocess bank");
                const auto sourceImage = source.images[source.lastCommittedIndex];
                require(value.m_precompositor.finalColour[eye] == sourceImage,
                        "checkpoint reads this input's current postprocessed source image");
                D3D11_TEXTURE2D_DESC image{};
                sourceImage->GetDesc(&image);
                require(image.Width == size && image.Height == size && image.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS,
                        "current sharpened image retains the complete512-square RGBA allocation");
                ovrTextureSwapChainDesc sourceDescription{}, presentedDescription{};
                require(OVR_SUCCESS(
                            ovr_GetTextureSwapChainDesc(value.m_ovrSession, source.ovrSwapchain, &sourceDescription)) &&
                            sourceDescription.Width == size && sourceDescription.Height == size &&
                            sourceDescription.Format == format,
                        "current postprocess source chain preserves complete eye extent and encoding");
                const auto& viewport = lastProjection.Viewport[eye];
                require(viewport.Size.w == size && viewport.Size.h == size && viewport.Pos.y == 0 &&
                            viewport.Pos.x == (realBank ? static_cast<int>(eye) * size : 0),
                        "postprocess transition presents complete current eyes with route-correct packing");
                require(realBank || lastProjection.ColorTexture[eye] == source.ovrSwapchain,
                        "postprocess transition uses real-B or this input's current postprocessed source chain");
                require(OVR_SUCCESS(ovr_GetTextureSwapChainDesc(
                            value.m_ovrSession, lastProjection.ColorTexture[eye], &presentedDescription)) &&
                            presentedDescription.Width == (realBank ? size * 2 : size) &&
                            presentedDescription.Height == size && presentedDescription.Format == format,
                        "presented chain has the complete expected allocation and transfer function");
                auto presented = getCommitted(value.m_ovrSession, lastProjection.ColorTexture[eye]);
                const auto pixels = viewportPixels(sourceImage.Get(), {{0, 0}, {size, size}});
                require(pixels == viewportPixels(presented.Get(), viewport),
                        "every current postprocessed source pixel survives transition presentation exactly");
                std::ofstream output(directory / ("postprocess-transition-eye" + std::to_string(eye) + ".rgba"),
                                     std::ios::binary);
                output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
                require(output.good(), "save complete postprocess transition pixels for independent comparison");
            }
            std::cout << "POSTPROCESS_TRANSITION_PIXELS coverage=90 processing=80 FG=" << requestedFrameGeneration
                      << " srgb=" << srgb << " route=" << (realBank ? "real-B" : "native")
                      << " extent=512x512 upscaling=100 sharpening=25 format=" << format << " exact\n";
        }
        static void observeId(long long id) {
            require(id == static_cast<long long>(presentedIds.size()),
                    "public Wait/Begin/End retains consecutive independent native IDs");
            presentedIds.push_back(id);
        }
        static void verifyBgraFormats(bool srgb) {
            auto& value = runtime();
            std::unique_lock contextLease(value.m_fgContextMutex);
            require(value.m_fgFormat == (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM),
                    "BGRA NR capture normalizes FG to the matching RGBA transfer function");
            const auto nrFormat = srgb ? OVR_FORMAT_B8G8R8A8_UNORM_SRGB : OVR_FORMAT_B8G8R8A8_UNORM;
            require(value.m_dlssnrOutputSwapchainFormat == nrFormat,
                    "NR output retains its original BGRA transfer-function metadata");
            ovrTextureSwapChainDesc nrDescription{};
            require(OVR_SUCCESS(
                        ovr_GetTextureSwapChainDesc(value.m_ovrSession, value.m_dlssnrOutputSwapchain, &nrDescription)),
                    "read the original BGRA NR output swapchain description");
            require(nrDescription.Format == nrFormat, "original NR output swapchain remains BGRA");
            const auto fgFormat = srgb ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM;
            unsigned allocated{};
            for (const auto& bank : value.m_fgPresentationBanks) {
                for (const auto& slice : bank) {
                    if (slice.ovrSwapchain) {
                        ++allocated;
                        ovrTextureSwapChainDesc description{};
                        require(OVR_SUCCESS(
                                    ovr_GetTextureSwapChainDesc(value.m_ovrSession, slice.ovrSwapchain, &description)),
                                "read each allocated FG presentation swapchain description");
                        require(description.Format == fgFormat,
                                "every FG presentation swapchain uses the matching RGBA transfer function");
                    }
                }
            }
            require(allocated > 0, "BGRA NR allocates FG presentation swapchains");
        }
        static void verifySpatialFormats(bool srgb, bool postprocess, bool bypass) {
            auto& value = runtime();
            std::unique_lock contextLease(value.m_fgContextMutex);
            const bool spatial = nrProcessingResolution != 100;
            const bool fg = requestedFrameGeneration != 0;
            const bool canonical = spatial && fg;
            const auto expected = canonical ? (srgb ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM)
                                            : (srgb ? OVR_FORMAT_B8G8R8A8_UNORM_SRGB : OVR_FORMAT_B8G8R8A8_UNORM);
            const auto typed = canonical ? (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM)
                                         : (srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM);
            const auto backing = canonical ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS;
            require(bool(value.m_nrSpatialPipeline) == spatial,
                    "spatial80 allocates its route; native100 retires the spatial generation");
            if (spatial) {
                require(value.m_nrSpatialFeatures && value.m_nrSpatialStatus == 100 + nrProcessingResolution,
                        "actual reduced processing completes without native fallback");
                const auto size = expectedSpatialSize();
                for (unsigned eye = 0; eye < 2; ++eye) {
                    const auto work = value.m_nrSpatialPipeline->GetWorkingEye(eye);
                    require(work.extent.width == unsigned(size) && work.extent.height == unsigned(size) && work.proxy &&
                                work.answer && value.m_nrSpatialFeatureExtents[eye].w == size &&
                                value.m_nrSpatialFeatureExtents[eye].h == size,
                            "both actual Ada features consume the expected crop-sized working eyes");
                }
            }
            require(value.m_precompositor.dlssnrSettings.processingResolution == nrProcessingResolution &&
                        std::abs(value.m_precompositor.dlssnrSettings.foveationSize - coverage / 100.f) < .0001f &&
                        value.m_frameGenerationMode == requestedFrameGeneration,
                    "independent processing, coverage and session FG request remain explicit");
            require(value.m_dlssnrOutputSwapchainFormat == expected,
                    "spatial FG uses encoding-matched RGBA; FG-off and native100 keep source BGRA");
            if (spatial && previousSpatialOutput)
                require(value.m_dlssnrOutputSwapchain == previousSpatialOutput,
                        "postprocess off/on and bypass do not replace the unchanged spatial output bank");
            previousSpatialOutput = spatial ? value.m_dlssnrOutputSwapchain : nullptr;
            ovrTextureSwapChainDesc description{};
            checkHr(OVR_SUCCESS(
                        ovr_GetTextureSwapChainDesc(value.m_ovrSession, value.m_dlssnrOutputSwapchain, &description))
                        ? S_OK
                        : E_FAIL);
            require(description.Format == expected && description.Width == 1024 && description.Height == 512,
                    "selected output allocation retains native double-wide extent and transfer function");
            require(value.m_dlssnrOutputSwapchainTextures.size() == value.m_dlssnrOutputSwapchainImages.size() &&
                        value.m_dlssnrOutputSwapchainTextures.size() == value.m_dlssnrOutputSwapchainSrvs.size(),
                    "output texture/SRV/import caches stay aligned after format transitions");
            for (size_t i = 0; i < value.m_dlssnrOutputSwapchainTextures.size(); ++i) {
                D3D11_TEXTURE2D_DESC resource{};
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                value.m_dlssnrOutputSwapchainTextures[i]->GetDesc(&resource);
                value.m_dlssnrOutputSwapchainSrvs[i]->GetDesc(&view);
                require(resource.Format == backing && view.Format == typed &&
                            value.m_dlssnrOutputSwapchainImages[i]->GetDesc().Format == backing,
                        "actual D3D11/D3D12 output family and typed upscaler SRV agree with selected output");
            }
            if (fg) {
                const auto finalBacking =
                    spatial || postprocess ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS;
                for (const auto& texture : value.m_precompositor.finalColour) {
                    D3D11_TEXTURE2D_DESC resource{};
                    require(texture != nullptr, "FG has final spatial/postprocessed eye ownership");
                    texture->GetDesc(&resource);
                    require(resource.Format == finalBacking,
                            "final capture uses selected output or canonical postprocessor output");
                }
                if (!postprocess)
                    require(value.m_precompositor.finalColour[0] == value.m_precompositor.finalColour[1],
                            "final spatial NR exposes one packed color texture to the FG copy path");
                if (bypass)
                    require(
                        value.m_fgPreviousCapturedTime != value.m_precompositor.displayTime,
                        "explicit FG bypass preserves requested canonical spatial output without accepting capture");
            } else {
                require(!value.m_frameGeneration, "separate FG-off session never initializes the FG module");
            }
            std::cout << "SPATIAL_FORMAT processing=" << nrProcessingResolution << " coverage=" << coverage
                      << " FG=" << requestedFrameGeneration << " srgb=" << srgb << " postprocess=" << postprocess
                      << " bypass=" << bypass << " ovr=" << expected << " backing=" << backing << " srv=" << typed
                      << '\n';
        }
        static void verifySpatialOffPixels() {
            auto& value = runtime();
            // The FG-off synthetic settings select synchronous submission; no
            // async worker exists to signal waitForAsyncSubmissionIdle().
            require(requestedFrameGeneration == 0, "FG-off pixel verification follows synchronous EndFrame");
            for (unsigned eye = 0; eye < 2; ++eye) {
                const ovrRecti source{{static_cast<int>(eye) * 512, 0}, {512, 512}};
                auto presented = getCommitted(value.m_ovrSession, lastProjection.ColorTexture[eye]);
                require(viewportPixels(
                            value.m_dlssnrOutputSwapchainTextures[value.m_dlssnrOutputSwapchainLastWrittenIndex].Get(),
                            source) == viewportPixels(presented.Get(), lastProjection.Viewport[eye]),
                        "FG-off presentation retains every final spatial NR pixel");
                std::cout << "SPATIAL_OFF_PIXELS eye=" << eye << " exact\n";
            }
        }
        struct SpatialRecovery {
            VDXRFG_StatisticsV1 statistics{};
            std::array<unsigned, 3> synthetic{};
            std::array<unsigned, 2> evaluations{};
            bool currentCapture{}, currentGeometry{}, realBank{};
            uint64_t nextNative{};
            double nextLeadMs{};
        };
        static SpatialRecovery spatialRecovery() {
            auto& value = runtime();
            // Call after the checkpoint or final-input drain. Never wait for
            // the presenter while holding either of its context leases.
            std::unique_lock contextLease(value.m_fgContextMutex);
            std::unique_lock captureLock(captureMutex);
            SpatialRecovery result{};
            require(value.m_frameGeneration != nullptr, "spatial recovery observes the actual initialized FG module");
            checkHr(value.m_frameGeneration->getStatistics(result.statistics));
            result.synthetic = {spatialSynthetic[0], spatialSynthetic[1], spatialSynthetic[2]};
            result.evaluations = {nrEvaluations[0], nrEvaluations[1]};
            result.currentCapture = value.m_fgPreviousCapturedTime == value.m_precompositor.displayTime;
            const auto format =
                value.m_precompositor.isProj0SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            const auto ovrFormat =
                value.m_precompositor.isProj0SRGB ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM;
            result.currentGeometry = value.m_fgEyeResolution.w == 512 && value.m_fgEyeResolution.h == 512 &&
                                     value.m_fgFormat == format && value.m_nrSpatialPipeline &&
                                     value.m_precompositor.dlssnrSettings.processingResolution != 100 &&
                                     value.m_fgBegunReservation.multiplier == requestedFrameGeneration;
            for (const auto& bank : value.m_fgPresentationBanks)
                result.realBank |= bank[2].ovrSwapchain && bank[2].ovrSwapchain == lastProjection.ColorTexture[0] &&
                                   bank[2].ovrSwapchain == lastProjection.ColorTexture[1];
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& viewport = lastProjection.Viewport[eye];
                ovrTextureSwapChainDesc description{};
                result.currentGeometry &= viewport.Pos.x == static_cast<int>(eye) * 512 && viewport.Pos.y == 0 &&
                                          viewport.Size.w == 512 && viewport.Size.h == 512 &&
                                          OVR_SUCCESS(ovr_GetTextureSwapChainDesc(
                                              value.m_ovrSession, lastProjection.ColorTexture[eye], &description)) &&
                                          description.Format == ovrFormat;
            }
            {
                std::unique_lock clockLock(clockMutex);
                result.nextNative = nextWaitId;
                result.nextLeadMs = (stablePrediction(value.m_ovrSession, static_cast<long long>(nextWaitId)) -
                                     ovr_GetTimeInSeconds()) *
                                    1000;
            }
            return result;
        }
        static bool recoveredSpatial(const SpatialRecovery& baseline, const SpatialRecovery& current) {
            const unsigned roles = combinedSpatial ? 4u : 1u;
            return current.statistics.captures >= baseline.statistics.captures + 2 &&
                   current.statistics.pairs >= baseline.statistics.pairs + 1 &&
                   current.synthetic[0] >= baseline.synthetic[0] + roles &&
                   (requestedFrameGeneration == 2 || current.synthetic[1] >= baseline.synthetic[1] + roles) &&
                   (requestedFrameGeneration != 4 || current.synthetic[2] >= baseline.synthetic[2] + roles) &&
                   current.currentCapture && current.currentGeometry && current.realBank;
        }
        static void printSpatialRecovery(const char* phase, unsigned inputs, const SpatialRecovery& state) {
            std::cout << "SPATIAL_RECOVERY " << phase << " inputs=" << inputs << " FG=" << requestedFrameGeneration
                      << " captures=" << state.statistics.captures << " pairs=" << state.statistics.pairs
                      << " S1=" << state.synthetic[0] << " S2=" << state.synthetic[1] << " S3=" << state.synthetic[2]
                      << " accepted=" << state.currentCapture << " geometry=" << state.currentGeometry
                      << " realB=" << state.realBank << " nextNative=" << state.nextNative
                      << " nextLeadMs=" << state.nextLeadMs << " evaluations=" << state.evaluations[0] << ','
                      << state.evaluations[1] << '\n';
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
            require(std::abs(value.m_precompositor.dlssnrSettings.foveationSize - coverage / 100.f) < .0001f,
                    "NR coverage retains the independently selected value");
            require(value.m_frameGenerationBackend == requestedFrameGenerationBackend,
                    "selected backend reaches the real session configuration");
            const auto& appearance = value.m_precompositor.dlssnrSettings;
            require(appearance.style == 2 && std::abs(appearance.intensity - .91f) < .0001f &&
                        appearance.localToneStrength == 1.f && appearance.localStructureStrength == 1.f &&
                        appearance.skinStructureStrength == 1.f,
                    "NR appearance settings preserve the current hair/face treatment");
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
    unsigned RuntimeFrameGenerationRegression::invalidNrParameters{};
    std::atomic<unsigned> RuntimeFrameGenerationRegression::syntheticPresentations{};
    std::map<ovrTextureSwapChain, unsigned> RuntimeFrameGenerationRegression::nrCommits;
    std::map<ovrTextureSwapChain, unsigned> RuntimeFrameGenerationRegression::commitsBeforeInput;
    std::vector<long long> RuntimeFrameGenerationRegression::presentedIds;
    std::vector<std::pair<long long, unsigned>> RuntimeFrameGenerationRegression::presentedRoles;
    ovrLayerEyeFov RuntimeFrameGenerationRegression::lastProjection{};
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) RuntimeFrameGenerationRegression::originalEvaluate[2]{};
} // namespace virtualdesktop_openxr

#ifndef VDXR_NATIVE_DELIVERY_PROBE
int main(int argc, char** argv) {
    const bool runtimeInstrumentationOff = argc == 4 && !strcmp(argv[3], "runtime-instrumentation-off");
    const bool projectionPoseQuery = argc == 4 && !strcmp(argv[3], "projection-pose-query");
    const bool idleFpsQuery = argc == 4 && !strcmp(argv[3], "idle-fps-query");
    const bool frameQueryScenario = projectionPoseQuery || idleFpsQuery;
    // Linked-runtime fixtures do not execute the DLL's process attach. Register
    // its provider here so an external collector can observe the real methods.
    struct RuntimeTraceRegistration {
        const bool registered;
        explicit RuntimeTraceRegistration(bool active) : registered(active) {
            if (registered)
                TraceLoggingRegister(virtualdesktop_openxr::log::g_traceProvider);
        }
        ~RuntimeTraceRegistration() {
            if (registered)
                TraceLoggingUnregister(virtualdesktop_openxr::log::g_traceProvider);
        }
    } runtimeTraceRegistration(!runtimeInstrumentationOff && !frameQueryScenario);
    ScopedSyntheticRuntimeTrace syntheticTrace(runtimeInstrumentationOff);
    std::cout << std::unitbuf;
    XrInstance instance{};
    try {
        require(argc == 4, "private backend, state directory and FG scenario required");
        const std::string scenario = argv[3];
        if (runtimeInstrumentationOff)
            syntheticTrace.verify();
        const bool nrReuse = scenario.rfind("delivery-nr-reuse-", 0) == 0;
        const bool nrReuseQuad = nrReuse && scenario.find("quad") != std::string::npos;
        allowQuadOnly = nrReuseQuad;
        const bool extraScenario = scenario == "delivery-mixed-layers";
        const bool modeFlip = scenario == "delivery-off-before-begin";
        if (modeFlip)
            asyncSubmissionDisabledOverride = 0;
        const bool depthOpacityCase = scenario == "depth-opacity-nr-on-2" || scenario == "depth-opacity-nr-off-2" ||
                                      scenario == "depth-opacity-nr-on-3" || scenario == "depth-opacity-nr-off-3" ||
                                      scenario == "depth-opacity-nr-on-4" || scenario == "depth-opacity-nr-off-4";
        const bool depthGuideColorOnly = depthOpacityCase || scenario == "depth-guide-color-only-nr-on-4" ||
                                         scenario == "depth-guide-color-only-nr-off-4";
        const unsigned depthEncodingCase = scenario == "depth-encoding-mismatch-3"            ? 1u
                                           : scenario == "depth-encoding-restricted-3"        ? 2u
                                           : scenario == "depth-encoding-reversed-infinite-3" ? 3u
                                           : scenario == "depth-encoding-matching-geometry-3" ? 4u
                                           : scenario == "depth-encoding-forward-far-3"       ? 5u
                                           : scenario == "depth-encoding-rounded-identical-3" ? 6u
                                                                                              : 0u;
        const bool depthSpatial =
            scenario == "depth-aligned-spatial80-nr-on-4" || scenario == "depth-aligned-d3d12-spatial80-nr-on-4";
        const bool depth12 = scenario == "depth-aligned-d3d12-spatial80-nr-on-4" ||
                             scenario == "depth-aligned-d3d12-nr-off-2" || scenario == "depth-aligned-d3d12-nr-off-3" ||
                             scenario == "depth-aligned-d3d12-nr-off-4" || scenario == "depth-aligned-d3d12-nr-on-2" ||
                             scenario == "depth-aligned-d3d12-nr-on-3" || scenario == "depth-aligned-d3d12-nr-on-4";
        const bool depthScenario = depthGuideColorOnly || depthEncodingCase || depthSpatial || depth12 ||
                                   scenario == "depth-aligned-nr-off-2" || scenario == "depth-aligned-nr-off-3" ||
                                   scenario == "depth-aligned-nr-off-4" || scenario == "depth-aligned-nr-on-2" ||
                                   scenario == "depth-aligned-nr-on-3" || scenario == "depth-aligned-nr-on-4";
        const bool depthNr = depthScenario && scenario.find("-nr-on-") != std::string::npos;
        unsigned steadyCoverage{}, steadyProcessing{};
        for (const auto settings : {std::array<unsigned, 2>{95, 80}, {90, 80}, {90, 85}, {100, 80}})
            for (const auto encoding : {"bgra", "bgra-srgb"})
                for (const auto multiplier : {2u, 3u, 4u})
                    if (scenario == "spatial-steady-" + std::to_string(settings[0]) + "-" +
                                        std::to_string(settings[1]) + "-" + encoding + "-" +
                                        std::to_string(multiplier)) {
                        steadyCoverage = settings[0];
                        steadyProcessing = settings[1];
                    }
        const bool steady = steadyCoverage != 0;
        const bool combined = scenario == "spatial-combined-bgra-off" || scenario == "spatial-combined-bgra-srgb-off" ||
                              scenario == "spatial-combined-bgra-2" || scenario == "spatial-combined-bgra-srgb-2" ||
                              scenario == "spatial-combined-bgra-3" || scenario == "spatial-combined-bgra-srgb-3" ||
                              scenario == "spatial-combined-bgra-4" || scenario == "spatial-combined-bgra-srgb-4";
        const bool spatial =
            steady || combined || scenario == "spatial-bgra-off" || scenario == "spatial-bgra-srgb-off" ||
            scenario == "spatial-bgra-2" || scenario == "spatial-bgra-srgb-2" || scenario == "spatial-bgra-3" ||
            scenario == "spatial-bgra-srgb-3" || scenario == "spatial-bgra-4" || scenario == "spatial-bgra-srgb-4";
        const bool spatialOff = spatial && scenario.find("-off") != std::string::npos;
        const bool discard4 = scenario == "bgra-discard-4";
        const bool publication4 = scenario == "bgra-publication-4";
        const bool profile4 = scenario == "bgra-profile-4";
        const bool discard = !strcmp(argv[3], "discard") || discard4;
        const bool toggle = !strcmp(argv[3], "nr-toggle");
        const bool captureBusyRecovery = !strcmp(argv[3], "nr-off-capture-busy");
        const bool busyRecovery = !strcmp(argv[3], "nr-off-reset-busy") || captureBusyRecovery;
        predictionJitter = !strcmp(argv[3], "bgra-srgb-jitter");
        const bool bgra = (scenario.rfind("delivery-d3d12-nr-on-", 0) == 0) || spatial || discard4 || publication4 ||
                          profile4 || scenario == "bgra-4" || scenario == "bgra-srgb-4" || !strcmp(argv[3], "bgra") ||
                          !strcmp(argv[3], "bgra-srgb") || predictionJitter;
        const bool bgraSrgb = !strcmp(argv[3], "bgra-srgb") || scenario == "bgra-srgb-4" || predictionJitter ||
                              (spatial && scenario.find("srgb") != std::string::npos);
        const bool delivery12 = scenario.rfind("delivery-d3d12-nr-", 0) == 0;
        const bool delivery12Nr = delivery12 && scenario.find("-nr-on-") != std::string::npos;
        const bool d3d12 = (nrReuse && scenario.find("d3d12") != std::string::npos) || delivery12 || depth12 ||
                           !strcmp(argv[3], "nr-off-d3d12") || !strcmp(argv[3], "nr-off-d3d12-reuse") ||
                           !strcmp(argv[3], "nr-off-d3d12-reuse-bypass");
        const bool cropped = depthScenario || !strcmp(argv[3], "nr-off-cropped");
        const bool packed = !strcmp(argv[3], "nr-off-packed");
        const bool reuseFallback = nrReuseQuad || (nrReuse && scenario.find("fallback") != std::string::npos) ||
                                   !strcmp(argv[3], "nr-off-reuse-bypass") ||
                                   !strcmp(argv[3], "nr-off-d3d12-reuse-bypass");
        const bool reuse =
            nrReuse || !strcmp(argv[3], "nr-off-reuse") || !strcmp(argv[3], "nr-off-d3d12-reuse") || reuseFallback;
        const bool fallback = !strcmp(argv[3], "nr-off-bypass") || reuseFallback;
        const bool noNr = runtimeInstrumentationOff || frameQueryScenario || modeFlip || extraScenario ||
                          (depthScenario ? !depthNr
                                         : !strcmp(argv[3], "nr-off-2") || !strcmp(argv[3], "nr-off-3") ||
                                               scenario == "nr-off-4" || !strcmp(argv[3], "nr-off-upscaled") ||
                                               cropped || packed || ((reuse || fallback) && !nrReuse) ||
                                               (d3d12 && !delivery12Nr && !nrReuse) || busyRecovery);
        if (const auto rate = std::getenv("VDXR_FG_TEST_REFRESH_RATE")) {
            nativeRate = static_cast<unsigned>(std::stoul(rate));
            require(nativeRate == 30 || nativeRate == 60 || nativeRate == 72 || nativeRate == 80 || nativeRate == 90 ||
                        nativeRate == 96 || nativeRate == 100 || nativeRate == 120 || nativeRate == 144,
                    "explicit supported test clock");
        }
        std::cout << "private backend test clock=" << nativeRate << "Hz\n";
        const bool sharpenOnly = !strcmp(argv[3], "sharpen");
        require(nrReuse || depthScenario || noNr || discard || toggle || sharpenOnly || bgra ||
                    !strcmp(argv[3], "native") || !strcmp(argv[3], "upscaled"),
                "known public FG scenario");
        requestedFrameGeneration =
            runtimeInstrumentationOff || frameQueryScenario || !strcmp(argv[3], "nr-off-2") ? 2 : 3;
        if (scenario == "nr-off-4" || scenario == "bgra-4" || scenario == "bgra-srgb-4" || discard4 || publication4 ||
            profile4)
            requestedFrameGeneration = 4;
        if (delivery12)
            requestedFrameGeneration = static_cast<unsigned>(scenario.back() - '0');
        if (nrReuse)
            requestedFrameGeneration = static_cast<unsigned>(scenario.back() - '0');
        if (spatial)
            requestedFrameGeneration = spatialOff ? 0 : static_cast<unsigned>(scenario.back() - '0');
        if (depthScenario)
            requestedFrameGeneration = static_cast<unsigned>(scenario.back() - '0');
        if (const auto backend = std::getenv("VDXR_FG_TEST_BACKEND")) {
            require(!strcmp(backend, "nvidia") || !strcmp(backend, "fidelityfx-full"), "known test backend");
            requestedFrameGenerationBackend = !strcmp(backend, "fidelityfx-full") ? 1 : 0;
        }
        preserveNrAppearance = true;
        coverage = steady ? steadyCoverage : 100;
        const bool upscaled = !strcmp(argv[3], "upscaled") || !strcmp(argv[3], "nr-off-upscaled");
        nrUpscalingSetting = upscaled ? 150 : 100;
        nrSharpenSetting = upscaled || sharpenOnly ? 25 : 0;
        nrProcessingResolution = steady ? steadyProcessing : (spatial || depthSpatial) ? 80 : 100;
        enabled = nrReuse || toggle || bgra;
        // Blocking Map readbacks inside each native End would themselves miss
        // 120Hz deadlines. Verify final pixels after draining the actual worker.
        readSubmittedColour = false;
        checkOffColourEdges = cropped;
        expectedOffColour[0] = {32, 64, 96, 255};
        expectedOffColour[1] = {144, 80, 48, 255};
        std::unique_ptr<FixtureGameProfile> profile;
        if (profile4) {
            globalFrameGenerationReadOverride = 2;
            profile = std::make_unique<FixtureGameProfile>();
        }
        if (const auto mode = std::getenv("VDXR_FG_TEST_DEPTH_MODE"))
            requestedFrameGenerationDepthMode = std::stoul(mode);
        SettingsReadHook settings;
        GuideApiSpy::checkRuntimeTiming = runtimeInstrumentationOff;
        GuideApiSpy guideApi(argv[1]);
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        Regression::depthInputCropped = depthScenario;
        Regression::depthEncodingCase = depthEncodingCase;
        Regression::depthGuideColorOnly = depthGuideColorOnly;
        Regression::depthOpacityCase = depthOpacityCase;
        GuideApiSpy::checkOpacity = depthOpacityCase;
        Regression::combinedSpatial = combined || steady;
        if (spatial)
            std::cout << "QUALIFICATION_PROFILE kind=" << (steady ? "steady-fixed" : "cold-transition")
                      << " clock=" << nativeRate << "Hz coverage=" << coverage
                      << " processing=" << nrProcessingResolution << " FG=" << requestedFrameGeneration << '\n';
        const auto getProc = virtualdesktop_openxr::RuntimeInputRegression::initialize(argv[1], argv[2]);
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
                                    XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
                                    XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
                                    XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR post NR frame generation regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = extraScenario ? 4 : depthScenario ? 3 : 2;
        info.enabledExtensionNames = extensions;
        XR_CHECK(xrCreateInstance(&info, &instance));
#define LOAD(name) XR_CHECK(getProc(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)));
        XR_FUNCTIONS(LOAD)
#undef LOAD
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system{};
        XR_CHECK(xrGetSystem(instance, &systemInfo, &system));
#ifdef VDXR_DELIVERY_REGRESSION
        Regression::selectControlledDeliveryBackend();
#endif
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
        if (modeFlip)
            requestedFrameGeneration = 0;
        if (input12)
            Regression::verifyAppQueue(binding12.queue);
        PFN_xrDestroySession destroySession{};
        XR_CHECK(getProc(instance, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&destroySession)));
        {
            CompositorHooks compositor;
            NativeClock nativeClock;
            PublicFrameQuerySpy frameQueries(frameQueryScenario, projectionPoseQuery);
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
            std::unique_ptr<Regression::FrameQueryState> frameQueryState;
            if (frameQueryScenario)
                frameQueryState = std::make_unique<Regression::FrameQueryState>(projectionPoseQuery);
            std::array<virtualdesktop_openxr::OwnedFrameTransaction, 2> heldHistory;
            auto releaseHistory = MakeScopeGuard([&] {
                try {
                    for (auto& held : heldHistory) {
                        Regression::retireBusyHistory(held);
                    }
                } catch (const std::exception& error) {
                    std::cerr << "FAIL: bounded busy-history cleanup: " << error.what() << '\n';
                    std::_Exit(2);
                }
            });
            observeProjection = Regression::observePresentation;
            if (projectionPoseQuery)
                observeProjection = [&](const ovrLayerEyeFov& projection) {
                    Regression::observePresentation(projection);
                    frameQueries.observeProjectionPose(projection);
                };
            observeNativeEnd = Regression::observeId;
            expectDepth[0] = expectDepth[1] = false;
            if (depthScenario) {
                observeNativeLayers = Regression::observeDepthLayers;
                observeNativeCommit = Regression::observeCommit;
                // The depth scenario queues readbacks at the native End hook;
                // the inherited synchronous probe must not perturb slot timing.
                depthPixelsDefined = false;
            }
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XR_CHECK(xrBeginSession(session, &begin));
            XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            reference.poseInReferenceSpace.orientation.w = 1.f;
            XrSpace space{};
            XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
            std::unique_ptr<DeliveryLayerFixture> extras;
            if (extraScenario) {
                extras = std::make_unique<DeliveryLayerFixture>(session, space, context.Get());
                observeNativeLayers = [&](const ovrLayerHeader* const* layers, unsigned count) {
                    Regression::captureExtra(*extras, layers, count);
                };
            }
            const unsigned width = packed ? 1024u : (cropped ? 544u : 512u);
            const unsigned height = cropped ? 536u : 512u;
            const auto colourFormat = bgraSrgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                                               : (bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
            auto colour = createImages(session, colourFormat, width, height, false, !d3d12);
            std::unique_ptr<DepthPattern> depth;
            if (depthScenario)
                depth = std::make_unique<DepthPattern>(session,
                                                       device.Get(),
                                                       input12.get(),
                                                       depthEncodingCase == 4 || depthEncodingCase == 6,
                                                       depthEncodingCase >= 5);
            if (input12) {
                input12->attach(colour.chain, colourFormat);
            }
            std::vector<uint32_t> rgba(width * height);
            std::vector<ComPtr<ID3D11RenderTargetView>> reuseViews;
            if ((reuse || depthScenario) && !d3d12) {
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
            const unsigned inputs = runtimeInstrumentationOff || frameQueryScenario ? 8u
                                    : nrReuseQuad                                   ? 64u
                                    : reuse ? 4u
                                            : ((fallback || toggle || busyRecovery) ? 18u : 12u);
            unsigned beforeDiscard{};
            XrTime previousPublicTime{};
            NrLookupProbe lookupProbe;
            auto releaseSpatialObservation = MakeScopeGuard([&] { Regression::observeSpatial(false); });
            const unsigned inputCount = depthOpacityCase ? 96u
                                        : depthScenario  ? 70u
                                        : steady         ? 128u
                                        : spatial        ? 30u
                                                         : inputs;
            const bool recoverSpatial = spatial && !spatialOff;
            const unsigned recoveryExtraInputs = steady ? 0u : combined ? 128u : 32u;
            const unsigned maximumInputs = inputCount + (recoverSpatial ? recoveryExtraInputs : 0u);
            if (recoverSpatial)
                std::cout << "SPATIAL_RECOVERY_BUDGET combined=" << combined << " extraInputs=" << recoveryExtraInputs
                          << '\n';
            unsigned executedInputs{};
            Regression::SpatialRecovery recoveryBaseline{};
            bool haveRecoveryBaseline{}, steadyRecovery{};
            size_t steadyPresentationStart{};
            XrFrameState queuedState{XR_TYPE_FRAME_STATE};
            bool queuedWait{};
            for (unsigned frame = 0; frame < maximumInputs; ++frame) {
                const bool spatialPostprocess = spatial && !steady && frame >= 8 && frame < 16;
                const bool spatialBypass = spatial && !steady && !spatialOff && frame == 22;
                if (spatial && !steady) {
                    nrProcessingResolution = frame >= 16 && frame < 20 ? 100 : 80;
                    if (combined) {
                        coverage = frame < 8 ? 95 : frame < 16 ? 90 : frame < 20 ? 100 : 90;
                        if (frame >= 20)
                            nrProcessingResolution = 85;
                    }
                    nrUpscalingSetting = 100; // Upscaling is session-initialized; this phase changes sharpening only.
                    nrSharpenSetting = spatialPostprocess ? 25 : 0;
                }
                const bool nr = depthScenario ? (depthNr && frame != 0)
                                              : bgra || (toggle ? (frame < 3 || frame >= 6) : (!noNr && frame != 0));
                virtualdesktop_openxr::RuntimeInputRegression::setEnabled(nr);
                if (depthScenario) {
                    expectedOffColour[0] = {32 + frame, 64, 96, 255};
                    expectedOffColour[1] = {144, 80 + frame, 48, 255};
                    if (depthOpacityCase && noNr) {
                        constexpr unsigned alpha[]{0, 128, 255};
                        expectedOffColour[0][3] = alpha[frame % 3];
                        expectedOffColour[1][3] = alpha[(frame + 1) % 3];
                    }
                }
                if (busyRecovery && frame == 4) {
                    Regression::retainBusyHistory(heldHistory, captureBusyRecovery);
                }
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState state{XR_TYPE_FRAME_STATE};
                const auto previousTarget = previousPublicTime;
                if (queuedWait) {
                    state = queuedState;
                    queuedWait = false;
                } else {
                    XR_CHECK(xrWaitFrame(session, &wait, &state));
                }
                if (busyRecovery && (frame == 10 || frame == 11)) {
                    Regression::verifyRecoveryCadence(state, frame == 10, previousPublicTime);
                }
                previousPublicTime = state.predictedDisplayTime;
                XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
                XR_CHECK(xrBeginFrame(session, &fb));
                if (discard && frame == 4) {
                    beforeDiscard = Regression::syntheticPresentations.load();
                    const auto previousTime = state.predictedDisplayTime;
                    XR_CHECK(xrWaitFrame(session, &wait, &state));
                    require(state.predictedDisplayTime > previousTime, "pipelined Wait timestamps stay monotonic");
                    require(xrBeginFrame(session, &fb) == XR_FRAME_DISCARDED,
                            "second public Begin discards its unsubmitted predecessor");
                    if (discard4) {
                        const auto expectedNative = Regression::expectedReanchorAfterDiscard();
                        XR_CHECK(xrWaitFrame(session, &wait, &queuedState));
                        Regression::verifyQueuedReanchor(expectedNative);
                        queuedWait = true;
                    }
                }
                const auto image = acquireImage(colour);
                if (input12) {
                    input12->fill(image, false, cropped);
                }
                for (unsigned eye = 0; !input12 && eye < 2; ++eye) {
                    const auto& tint = expectedOffColour[eye];
                    const auto pixel = (bgra ? tint[2] : tint[0]) | (tint[1] << 8) |
                                       ((bgra ? tint[0] : tint[2]) << 16) | (tint[3] << 24);
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
                unsigned depthImage{};
                if (depth) {
                    depthImage = acquireImage(depth->images);
                    depth->fill(context.Get(), depthImage, frame);
                    releaseImage(depth->images);
                }
                XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
                XrCompositionLayerDepthInfoKHR depths[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                            {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
                for (unsigned eye = 0; eye < 2; ++eye) {
                    views[eye].pose.orientation.w = 1.f;
                    views[eye].pose.position.x = eye ? .032f : -.032f;
                    if (projectionPoseQuery) {
                        views[eye].pose.position.y = eye ? -.2f : .1f;
                        views[eye].pose.position.z = eye ? -.3f : .2f;
                        views[eye].pose.orientation.w = std::cos(eye ? .15f : .1f);
                        if (eye)
                            views[eye].pose.orientation.x = std::sin(.15f);
                        else
                            views[eye].pose.orientation.y = std::sin(.1f);
                    }
                    views[eye].fov = {-.8f, .8f, .8f, -.8f};
                    if (busyRecovery && !captureBusyRecovery && frame >= 4 && !eye) {
                        views[eye].fov.angleLeft = -.7f;
                    }
                    views[eye].subImage = {
                        colour.chain,
                        {{packed ? static_cast<int>(eye * 512) : (cropped ? 16 : 0), cropped ? 12 : 0}, {512, 512}},
                        packed ? 0u : eye};
                    if (depth) {
                        views[eye].next = &depths[eye];
                        depths[eye].subImage = {depth->images.chain, depth->rect, eye};
                        depths[eye].minDepth = 0;
                        depths[eye].maxDepth = 1;
                        depths[eye].nearZ = .1f;
                        depths[eye].farZ = 100.f;
                        if (depthEncodingCase) {
                            depths[eye].nearZ = eye ? 2.f : 1.f;
                            depths[eye].farZ = eye ? 20.f : 10.f;
                            if (depthEncodingCase == 2) {
                                depths[eye].minDepth = eye ? .05f : .1f;
                                depths[eye].maxDepth = eye ? .98f : .95f;
                            } else if (depthEncodingCase == 3) {
                                depths[eye].nearZ = eye ? std::numeric_limits<float>::infinity() : 10.f;
                                depths[eye].farZ = eye ? 2.f : 1.f;
                            } else if (depthEncodingCase >= 5) {
                                depths[eye].nearZ =
                                    eye && depthEncodingCase == 5 ? std::numeric_limits<float>::infinity() : 1.f;
                                depths[eye].farZ = !eye                     ? 1e8f
                                                   : depthEncodingCase == 5 ? .5f
                                                                            : std::numeric_limits<float>::infinity();
                            }
                        }
                    }
                }
                XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                if (depthGuideColorOnly && (!depthOpacityCase || (frame >= 16 && frame < 32)))
                    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                GuideApiSpy::expectedOpaque =
                    depthOpacityCase || !(layer.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                layer.space = space;
                layer.viewCount = 2;
                layer.views = views;
                const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                const XrCompositionLayerBaseHeader* duplicateLayers[]{header, header};
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
                end.displayTime = state.predictedDisplayTime;
                end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                end.layerCount = 1;
                end.layers = &header;
                XrCompositionLayerQuad dynamicQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                const bool quadOnlyInput = nrReuseQuad && frame == 3;
                if (quadOnlyInput) {
                    dynamicQuad.space = space;
                    dynamicQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    dynamicQuad.pose.orientation.w = 1.f;
                    dynamicQuad.pose.position.z = -2.f;
                    dynamicQuad.size = {1.f, 1.f};
                    dynamicQuad.subImage = {colour.chain, {{0, 0}, {512, 512}}, 0};
                    header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dynamicQuad);
                }
                const XrCompositionLayerBaseHeader* extraHeaders[3]{};
                if (extras && frame == 3) {
                    extraHeaders[0] = header;
                    extraHeaders[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&extras->quadLayer);
                    extraHeaders[2] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&extras->cylinder);
                    end.layerCount = 3;
                    end.layers = extraHeaders;
                }
                if (busyRecovery && frame == 9) {
                    // A genuinely unsupported composition must still select
                    // native cadence; it is not temporary history pressure.
                    end.layerCount = 2;
                    end.layers = duplicateLayers;
                }
                Regression::beforeInputEnd();
                if (depthScenario)
                    Regression::beforeDepthInput(frame, end.displayTime);
                if (!quadOnlyInput && ((depthScenario && frame == 66) || spatialBypass ||
                                       (fallback && frame == (reuseFallback ? 3u : 2u)))) {
                    // The third app image wraps to SDK-current minus one while
                    // successful FG has never committed its raw source chain.
                    Regression::forceFallback();
                }
                ComPtr<ID3D12Fence> sourceGate;
                std::atomic<bool> allowSource{}, sourceTimeout{};
                std::thread releaseSource;
                if (((reuse || (depthScenario && !depthNr)) && frame == 3) ||
                    (depthScenario && depthNr && frame == 66)) {
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
                const bool measureLookups = nr && frame >= 2 && !spatialOff && !quadOnlyInput;
                if (measureLookups)
                    Regression::beginLookupProbe(lookupProbe);
                VDXRFG_StatisticsV1 beforeRecovery{};
                size_t beforeRecoveryRoles{};
                if (busyRecovery && (frame == 5 || frame == 6)) {
                    beforeRecovery = Regression::statistics();
                    beforeRecoveryRoles = Regression::presentedRoles.size();
                }
                if (frameQueryScenario) {
                    const float liveHeight = 1.25f + frame * .05f;
                    if (projectionPoseQuery && !frame) {
                        // Failed EndFrame leaves this same begun frame open.
                        // Preserve validation order before the successful retry.
                        for (unsigned invalidEye = 0; invalidEye < 2; ++invalidEye) {
                            const auto orientation = views[invalidEye].pose.orientation;
                            views[invalidEye].pose.orientation = {};
                            auto restorePose =
                                MakeScopeGuard([&] { views[invalidEye].pose.orientation = orientation; });
                            PublicFrameQuerySpy::Call query(frameQueries, liveHeight);
                            const auto result = xrEndFrame(session, &end);
                            query.finish();
                            require(result == XR_ERROR_POSE_INVALID,
                                    "public invalid-eye EndFrame keeps its error result");
                            query.verifyPose(invalidEye, invalidEye ? "invalid-second-eye" : "invalid-first-eye");
                        }
                    }
                    if (projectionPoseQuery)
                        frameQueries.expectProjection(Regression::nativeSampleTime(end.displayTime), liveHeight, views);
                    const auto expectedAppGpu = Regression::expectedAppGpuTime();
                    PublicFrameQuerySpy::Call query(frameQueries, liveHeight);
                    const auto result = xrEndFrame(session, &end);
                    query.finish();
                    XR_CHECK(result);
                    if (projectionPoseQuery)
                        query.verifyPose(1, "valid-stereo");
                    else {
                        query.verifyAppGpu(expectedAppGpu);
                        Regression::verifyIdleFps();
                    }
                } else {
                    XR_CHECK(xrEndFrame(session, &end));
                }
                if (runtimeInstrumentationOff) {
                    syntheticTrace.verify();
                    guideApi.verifyRuntimeTiming(false);
                }
                if (depth) {
                    Regression::afterDepthInput(frame, end.displayTime, nr);
                    // Queue app writes immediately after EndFrame and before
                    // the next Wait/drain. Both eyes and both sources change.
                    const float overwrite[]{1.f, 0.f, 0.f, 1.f};
                    if (input12)
                        input12->fill(image, true);
                    else
                        context->ClearRenderTargetView(reuseViews[image].Get(), overwrite);
                    depth->overwrite(context.Get(), depthImage);
                    if (!input12)
                        context->Flush();
                    ++Regression::depthOverwrites;
                }
                if (busyRecovery && frame == 4) {
                    Regression::verifyBusyFallback();
                    for (auto& held : heldHistory) {
                        Regression::retireBusyHistory(held);
                    }
                } else if (busyRecovery && (frame == 5 || frame == 6)) {
                    Regression::verifyRecoveryGroup(beforeRecoveryRoles,
                                                    beforeRecovery.captures + 1,
                                                    beforeRecovery.pairs + (frame == 6 ? 1u : 0u),
                                                    frame == 6);
                    Regression::verifyRecoveryCadence(state, false, previousTarget);
                }
                if (measureLookups)
                    lookupProbe.finish();
                if (measureLookups)
                    Regression::verifyLookupResult(upscaled || sharpenOnly || spatialPostprocess);
                if (fallback && frame == 2) {
                    Regression::drainAndVerifyPixels();
                }
                if (sourceGate) {
                    Regression::verifyCaptureRoute(end.displayTime, depthScenario ? frame != 66 : !reuseFallback);
                    // No drain/readback before app reuse: independently queued
                    // writes must wait for the runtime's two source copies.
                    const float overwrite[]{1.f, 0.f, 0.f, 1.f};
                    if (input12) {
                        input12->fill(image, true);
                    } else {
                        context->ClearRenderTargetView(reuseViews[image].Get(), overwrite);
                        context->Flush();
                    }
                    bool protectedReuse = true;
                    wil::unique_handle appReuseEvent;
                    ComPtr<ID3D11Fence> appReuseMarker11;
                    ComPtr<ID3D12Fence> appReuseMarker12;
                    if (nrReuse) {
                        // The application marker is behind its immediate overwrite.
                        // A live S gate must prevent this marker from completing.
                        appReuseEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
                        require(bool(appReuseEvent), "application reuse completion event");
                        if (input12) {
                            checkHr(input12->device->CreateFence(
                                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&appReuseMarker12)));
                            checkHr(input12->context->GetCommandQueue()->Signal(appReuseMarker12.Get(), 1));
                            checkHr(appReuseMarker12->SetEventOnCompletion(1, appReuseEvent.get()));
                            protectedReuse = WaitForSingleObject(appReuseEvent.get(), 100) == WAIT_TIMEOUT &&
                                             appReuseMarker12->GetCompletedValue() == 0;
                        } else {
                            ComPtr<ID3D11Device5> appDevice;
                            ComPtr<ID3D11DeviceContext4> appContext;
                            checkHr(device.As(&appDevice));
                            checkHr(context.As(&appContext));
                            checkHr(appDevice->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&appReuseMarker11)));
                            checkHr(appContext->Signal(appReuseMarker11.Get(), 1));
                            appContext->Flush();
                            checkHr(appReuseMarker11->SetEventOnCompletion(1, appReuseEvent.get()));
                            protectedReuse = WaitForSingleObject(appReuseEvent.get(), 100) == WAIT_TIMEOUT &&
                                             appReuseMarker11->GetCompletedValue() == 0;
                        }
                    }
                    allowSource = true;
                    releaseSource.join();
                    require(!sourceTimeout, "source protection never blocks EndFrame on the CPU");
                    require(protectedReuse,
                            "application image reuse must remain GPU-blocked behind actual NR input copies");
                    if (nrReuse)
                        require(WaitForSingleObject(appReuseEvent.get(), 5000) == WAIT_OBJECT_0 &&
                                    (appReuseMarker12 ? appReuseMarker12->GetCompletedValue()
                                                      : appReuseMarker11->GetCompletedValue()) == 1,
                                "the same protected app overwrite resumes after actual source reads complete");
                    if (depthScenario || nrReuse)
                        Regression::drain();
                    else
                        Regression::drainAndVerifyPixels();
                }
                if (noNr) {
                    Regression::verifyNoNrGraph();
                }
                if (!spatialOff && !modeFlip && !quadOnlyInput)
                    Regression::verifyFinal(nr,
                                            ((51200 / nrUpscalingSetting + 3) / 4) * 4,
                                            frame <= 1 || frame == inputCount - 1 ||
                                                (recoverSpatial && frame >= inputCount));
                if (publication4)
                    Regression::verifyPublishedSpan();
                if (depthSpatial && nr)
                    Regression::verifySpatialFormats(false, false, frame == 66);
                if (spatial) {
                    Regression::verifySpatialFormats(bgraSrgb, spatialPostprocess, spatialBypass);
                    if (!spatialOff && combined && frame == 15)
                        Regression::verifyPostprocessTransitionPixels(
                            std::filesystem::path(argv[2]) / ("phase-" + std::to_string(frame)), views, bgraSrgb);
                    if (!steady && !spatialOff && (frame == 7 || (!combined && frame == 15)))
                        Regression::verifyRealPixels(std::filesystem::path(argv[2]) /
                                                     ("phase-" + std::to_string(frame)));
                    if (!steady && !spatialOff && (frame == 19 || spatialBypass))
                        Regression::verifyTransitionPixels(
                            std::filesystem::path(argv[2]) / ("phase-" + std::to_string(frame)), views, bgraSrgb);
                    if (spatialBypass) {
                        recoveryBaseline = Regression::spatialRecovery();
                        haveRecoveryBaseline = true;
                        Regression::printSpatialRecovery("baseline-after-bypass", frame + 1, recoveryBaseline);
                    }
                } else if (bgra) {
                    Regression::verifyBgraFormats(bgraSrgb);
                }
                Regression::verifyInternalChains();
                if (toggle && frame == 4) {
                    Regression::drainAndVerifyPixels();
                }
                if (frame <= 1) {
                    if (frame == 1 && !noNr) {
                        if (spatial || depthSpatial)
                            Regression::observeSpatial(true);
                        else
                            Regression::observe();
                    }
                    nativeClock.reanchorAfterWarmup();
                    if (steady && frame == 1) {
                        recoveryBaseline = Regression::spatialRecovery();
                        haveRecoveryBaseline = true;
                        steadyPresentationStart = Regression::presentedRoles.size();
                        Regression::printSpatialRecovery("baseline-after-startup", frame + 1, recoveryBaseline);
                    }
                }
                if (!frame) {
                    observeNativeCommit = Regression::observeCommit;
                }
                {
                    std::lock_guard lock(captureMutex);
                    if (!captureFailure.empty())
                        throw std::runtime_error(captureFailure);
                }
                ++executedInputs;
                if (recoverSpatial && executedInputs >= inputCount) {
                    require(haveRecoveryBaseline,
                            "qualification requires its explicit startup or after-bypass baseline");
                    const auto current = Regression::spatialRecovery();
                    steadyRecovery = Regression::recoveredSpatial(recoveryBaseline, current);
                    if (steadyRecovery) {
                        Regression::printSpatialRecovery("recovered", executedInputs, current);
                        break;
                    }
                    if (executedInputs == inputCount)
                        Regression::printSpatialRecovery("bounded-tail-begins", executedInputs, current);
                    if (executedInputs == maximumInputs) {
                        Regression::printSpatialRecovery("failure-baseline", steady ? 2 : 23, recoveryBaseline);
                        Regression::printSpatialRecovery("failure-final", executedInputs, current);
                    }
                }
            }
            require(!recoverSpatial || steadyRecovery,
                    "spatial FG must recover new accepted captures, pairs and requested roles within bounded tail");
            if (extras)
                Regression::verifyExtra(*extras);
            if (depthScenario)
                Regression::verifyDepthResults();
            const auto expectedNr = noNr ? 0u : executedInputs - (toggle ? 5u : 2u) - (nrReuseQuad ? 1u : 0u);
            if (modeFlip)
                Regression::drainAndVerifyPixels(executedInputs - 1);
            else
                Regression::verifyPixels();
            if (spatialOff)
                Regression::verifySpatialOffPixels();
            else if (!noNr && !depthScenario)
                Regression::verifyRealPixels(argv[2]);
            unsigned eyeDifference{};
            for (unsigned component = 0; component < 3; ++component)
                eyeDifference += std::abs(int(observedColour[0][component]) - int(observedColour[1][component]));
            require(eyeDifference > 32, "final FG presentation preserves distinct left and right eye pixels");
            require(Regression::nrEvaluations[0] == expectedNr && Regression::nrEvaluations[1] == expectedNr,
                    "exactly one real NR evaluation per eye per input, none on synthesized output");
            require(!Regression::invalidNrParameters,
                    "actual forwarded Ada evaluations preserve full NR rectangles, preset B and hair/face controls");
            const auto statistics = (spatialOff || modeFlip) ? VDXRFG_StatisticsV1{} : Regression::statistics();
            const auto minimumSynthetic = requestedFrameGeneration ? 4u * (requestedFrameGeneration - 1) : 0u;
            if (reuse) {
                // These cases deliberately stall source processing. Ownership
                // and late real fallback are tested independently of cadence;
                // the normal 2X/3X cases require actual synthetic submissions.
                require(statistics.captures >= 2 && !Regression::presentedIds.empty(),
                        "source reuse surrounds real module captures and native presentations");
            } else if (!spatialOff && !modeFlip) {
                require(statistics.pairs >= 4 && statistics.synthetic_frames >= minimumSynthetic,
                        "public prediction path must produce real pairs after warmup");
                require(Regression::syntheticPresentations >= minimumSynthetic,
                        "production native presenter must submit generated roles, not only accept GPU synthesis");
            }
            if (spatial && !spatialOff) {
                require(Regression::spatialSynthetic[0] >= 4 &&
                            (requestedFrameGeneration == 2 || Regression::spatialSynthetic[1] >= 4) &&
                            (requestedFrameGeneration != 4 || Regression::spatialSynthetic[2] >= 4),
                        "all requested synthetic roles reach native submission while reduced NR is active");
                std::cout << "SPATIAL_PRESENT FG=" << requestedFrameGeneration
                          << " S1=" << Regression::spatialSynthetic[0] << " S2=" << Regression::spatialSynthetic[1]
                          << " S3=" << Regression::spatialSynthetic[2] << " evaluated=" << Regression::nrEvaluations[0]
                          << ',' << Regression::nrEvaluations[1] << '\n';
                if (steady)
                    std::cout << "STEADY_PRESENT clock=" << nativeRate << "Hz coverage=" << coverage
                              << " processing=" << nrProcessingResolution << " FG=" << requestedFrameGeneration
                              << " measuredInputs=" << executedInputs - 2
                              << " NEW_S1=" << Regression::spatialSynthetic[0] - recoveryBaseline.synthetic[0]
                              << " NEW_S2=" << Regression::spatialSynthetic[1] - recoveryBaseline.synthetic[1]
                              << " NEW_S3=" << Regression::spatialSynthetic[2] - recoveryBaseline.synthetic[2] << '\n';
                if (combined) {
                    require(Regression::combinedSynthetic[2][0] >= 4 &&
                                (requestedFrameGeneration == 2 || Regression::combinedSynthetic[2][1] >= 4) &&
                                (requestedFrameGeneration != 4 || Regression::combinedSynthetic[2][2] >= 4),
                            "after combined coverage/processing changes, native synthetic roles recover on90/85");
                    for (unsigned cohort = 0; cohort < 3; ++cohort)
                        std::cout << "COMBINED_PRESENT coverage=" << (cohort == 0 ? 95 : 90)
                                  << " processing=" << (cohort == 2 ? 85 : 80) << " FG=" << requestedFrameGeneration
                                  << " S1=" << Regression::combinedSynthetic[cohort][0]
                                  << " S2=" << Regression::combinedSynthetic[cohort][1]
                                  << " S3=" << Regression::combinedSynthetic[cohort][2] << '\n';
                }
            }
            if (discard) {
                std::cout << "DISCARD_PRESENT observed=" << Regression::syntheticPresentations
                          << " before=" << beforeDiscard << " roles=";
                for (const auto& entry : Regression::presentedRoles)
                    std::cout << entry.first << ':' << entry.second << ',';
                std::cout << '\n';
                require(Regression::syntheticPresentations >= beforeDiscard + 4,
                        "public discard recovers real synthetic presentation after reanchoring");
            }
            if (nrReuseQuad) {
                require(quadOnlyEnds == requestedFrameGeneration,
                        "dynamic ordinary quad reaches every reserved native slot with NR preference enabled");
                require(Regression::syntheticPresentations >= 4u * (requestedFrameGeneration - 1),
                        "projection resumes actual synthetic presentation after the held dynamic quad fallback");
            }
            if (bgra && !spatialOff && requestedFrameGeneration == 3) {
                unsigned s1{}, s2{}, complete{};
                const auto& roles = Regression::presentedRoles;
                const size_t start = steady ? steadyPresentationStart : 0;
                for (size_t i = start; i < roles.size(); ++i) {
                    s1 += roles[i].second == 0;
                    s2 += roles[i].second == 1;
                    if (i >= start + 2 && roles[i - 2].second == 0 && roles[i - 1].second == 1 &&
                        roles[i].second == 2 && roles[i - 2].first + 1 == roles[i - 1].first &&
                        roles[i - 1].first + 1 == roles[i].first)
                        ++complete;
                }
                require(s1 >= 4 && s2 >= 4 && complete >= 1,
                        "BGRA NR must present both synthetic roles and a complete consecutive S1/S2/B group");
                std::cout << "BGRA3x proof: S1=" << s1 << " S2=" << s2 << " complete=" << complete << '\n';
            }
            if (bgra && !spatialOff && requestedFrameGeneration == 4) {
                std::array<unsigned, 3> counts{};
                unsigned complete{};
                const auto& roles = Regression::presentedRoles;
                const size_t start = steady ? steadyPresentationStart : 0;
                for (size_t i = start; i < roles.size(); ++i) {
                    const auto role = roles[i].second;
                    if (role == 0 || role == 1 || role == 3)
                        ++counts[role == 3 ? 2 : role];
                    if (i >= start + 3 && roles[i - 3].second == 0 && roles[i - 2].second == 1 &&
                        roles[i - 1].second == 3 && role == 2 && roles[i - 3].first + 1 == roles[i - 2].first &&
                        roles[i - 2].first + 1 == roles[i - 1].first && roles[i - 1].first + 1 == roles[i].first)
                        ++complete;
                }
                require(counts[0] >= 4 && counts[1] >= 4 && counts[2] >= 4 && complete >= 1,
                        "BGRA NR must present S1/S2/S3/B with consecutive native IDs");
                std::cout << "BGRA4x proof: S1=" << counts[0] << " S2=" << counts[1] << " S3=" << counts[2]
                          << " complete=" << complete << '\n';
            }
            if (projectionPoseQuery)
                frameQueries.verifyProjectionObservations();
            observeProjection = {};
            observeNativeEnd = {};
            observeNativeCommit = {};
            observeNativeLayers = {};
            Regression::observeSpatial(false);
            if (input12) {
                input12->context->Flush();
            }
            Regression::watchFinalColourCleanup();
            if (std::getenv("VDXR_FG_TEST_CLEANUP_DRAIN_FAILURE"))
                Regression::verifyFailedCleanup(destroySession, session);
            if (frameQueryState)
                frameQueryState->restore();
            XR_CHECK(destroySession(session)); // join worker before removing its pacing/End hooks
            session = XR_NULL_HANDLE;
            // The witness observes resource destruction, not just Release.
            // D3D11 defers destruction until a Flush after references and
            // bindings are released. Clear this fixture-owned application
            // context before applying that synchronous lifetime assertion.
            // https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush
            std::cout << "FINAL_COLOUR_OWNERS before_fixture_flush=" << Regression::finalColourDestroyed.load() << '\n';
            context->ClearState();
            context->Flush();
            Regression::verifySessionCleanup();
        }
        XR_CHECK(xrDestroyInstance(instance));
        instance = XR_NULL_HANDLE;
        guideApi.verify(depthScenario);
        guideApi.verifyRuntimeTiming(true);
        if (profile4) {
            profile->remove();
            std::cout << "PASS: actual executable profile selects4x over global2x without changing globals\n";
        }
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

#endif // VDXR_NATIVE_DELIVERY_PROBE
