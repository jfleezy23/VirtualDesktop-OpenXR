// Public OpenXR path, real vendor NR, selected real flow backend and private OVRNull.
// Globals use synthetic registry responses. The profile case owns one temporary
// executable-specific key and never rewrites global or game settings.
#define VDXR_NR_HELPERS_ONLY
#include "nr_input_toggle_regression.cpp"
#undef VDXR_NR_HELPERS_ONLY
#include "nr_fg_lookup_probe.h"
#include "../virtualdesktop-openxr/game_profiles.h"
#include <fstream>

namespace {
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
    bool predictionJitter{};
    uint64_t nextWaitId{};
    double OVR_CDECL stablePrediction(ovrSession, long long id) {
        return clockBase.load() + id / static_cast<double>(nativeRate) + (predictionJitter ? id * .000005 : 0.);
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
            }
            if (!hasInt("DLSSNR.Style", 2) || !hasInt("DLSSNR.Hint.Render.Preset", 2) ||
                !hasFloat("DLSSNR.Intensity", .91f) || !hasFloat("DLSSNR.LocalToneStrength", 1.f) ||
                !hasFloat("DLSSNR.LocalStructureStrength", 1.f) || !hasFloat("DLSSNR.SkinStructureStrength", 1.f) ||
                !hasInt("DLSSNR.ColorSubrectWidth", size) || !hasInt("DLSSNR.ColorSubrectHeight", size) ||
                !hasInt("DLSSNR.OutputSubrectWidth", size) || !hasInt("DLSSNR.OutputSubrectHeight", size) ||
                !hasInt("DLSSNR.ColorSubrectBaseX", 0) || !hasInt("DLSSNR.ColorSubrectBaseY", 0) ||
                !hasInt("DLSSNR.OutputSubrectBaseX", spatial ? 0 : eye * 512) ||
                !hasInt("DLSSNR.OutputSubrectBaseY", 0))
                ++invalidNrParameters;
            // Always forward to actual Ada inference; diagnostics must not
            // substitute success or throw through the callback boundary.
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
        static NVSDK_NGX_Result NVSDK_CONV evaluateSpatial(ID3D12GraphicsCommandList* commands,
                                                           const NVSDK_NGX_Handle* feature,
                                                           const NVSDK_NGX_Parameter* parameters,
                                                           PFN_NVSDK_NGX_ProgressCallback callback) {
            return nrEvaluations[0] == nrEvaluations[1] ? evaluate<0>(commands, feature, parameters, callback)
                                                        : evaluate<1>(commands, feature, parameters, callback);
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
        static void drainAndVerifyPixels() {
            runtime().waitForAsyncSubmissionIdle();
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
        static void verifySessionCleanup() {
            const auto& value = runtime();
            require(!value.m_frameGeneration && !value.m_fgDevice && !value.m_fgSourceFence11 &&
                        !value.m_fgSourceFence12 && !value.m_dlssnrResourcesInitialized,
                    "session cleanup retires FG devices, imported source fences and lazy NR readiness");
            require(!value.m_dlssnrOutputSwapchain && value.m_dlssnrOutputSwapchainImages.empty() &&
                        value.m_dlssnrOutputSwapchainTextures.empty() && value.m_dlssnrOutputSwapchainSrvs.empty(),
                    "session cleanup releases the NR output swapchain and its resource rings");
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

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    XrInstance instance{};
    try {
        require(argc == 4, "private backend, state directory and FG scenario required");
        const std::string scenario = argv[3];
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
        const bool bgra = spatial || discard4 || publication4 || profile4 || scenario == "bgra-4" ||
                          scenario == "bgra-srgb-4" || !strcmp(argv[3], "bgra") || !strcmp(argv[3], "bgra-srgb") ||
                          predictionJitter;
        const bool bgraSrgb = !strcmp(argv[3], "bgra-srgb") || scenario == "bgra-srgb-4" || predictionJitter ||
                              (spatial && scenario.find("srgb") != std::string::npos);
        const bool d3d12 = !strcmp(argv[3], "nr-off-d3d12") || !strcmp(argv[3], "nr-off-d3d12-reuse") ||
                           !strcmp(argv[3], "nr-off-d3d12-reuse-bypass");
        const bool cropped = !strcmp(argv[3], "nr-off-cropped");
        const bool packed = !strcmp(argv[3], "nr-off-packed");
        const bool reuseFallback =
            !strcmp(argv[3], "nr-off-reuse-bypass") || !strcmp(argv[3], "nr-off-d3d12-reuse-bypass");
        const bool reuse = !strcmp(argv[3], "nr-off-reuse") || !strcmp(argv[3], "nr-off-d3d12-reuse") || reuseFallback;
        const bool fallback = !strcmp(argv[3], "nr-off-bypass") || reuseFallback;
        const bool noNr = !strcmp(argv[3], "nr-off-2") || !strcmp(argv[3], "nr-off-3") || scenario == "nr-off-4" ||
                          !strcmp(argv[3], "nr-off-upscaled") || cropped || packed || reuse || fallback || d3d12 ||
                          busyRecovery;
        if (const auto rate = std::getenv("VDXR_FG_TEST_REFRESH_RATE")) {
            nativeRate = static_cast<unsigned>(std::stoul(rate));
            require(nativeRate == 30 || nativeRate == 60 || nativeRate == 120, "explicit supported test clock");
        }
        std::cout << "private backend test clock=" << nativeRate << "Hz\n";
        const bool sharpenOnly = !strcmp(argv[3], "sharpen");
        require(noNr || discard || toggle || sharpenOnly || bgra || !strcmp(argv[3], "native") ||
                    !strcmp(argv[3], "upscaled"),
                "known public FG scenario");
        requestedFrameGeneration = !strcmp(argv[3], "nr-off-2") ? 2 : 3;
        if (scenario == "nr-off-4" || scenario == "bgra-4" || scenario == "bgra-srgb-4" || discard4 || publication4 ||
            profile4)
            requestedFrameGeneration = 4;
        if (spatial)
            requestedFrameGeneration = spatialOff ? 0 : static_cast<unsigned>(scenario.back() - '0');
        if (const auto backend = std::getenv("VDXR_FG_TEST_BACKEND")) {
            require(!strcmp(backend, "nvidia") || !strcmp(backend, "fidelityfx-full"), "known test backend");
            requestedFrameGenerationBackend = !strcmp(backend, "fidelityfx-full") ? 1 : 0;
        }
        preserveNrAppearance = true;
        coverage = steady ? steadyCoverage : 100;
        const bool upscaled = !strcmp(argv[3], "upscaled") || !strcmp(argv[3], "nr-off-upscaled");
        nrUpscalingSetting = upscaled ? 150 : 100;
        nrSharpenSetting = upscaled || sharpenOnly ? 25 : 0;
        nrProcessingResolution = steady ? steadyProcessing : spatial ? 80 : 100;
        enabled = toggle || bgra;
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
        SettingsReadHook settings;
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        Regression::combinedSpatial = combined || steady;
        if (spatial)
            std::cout << "QUALIFICATION_PROFILE kind=" << (steady ? "steady-fixed" : "cold-transition")
                      << " clock=" << nativeRate << "Hz coverage=" << coverage
                      << " processing=" << nrProcessingResolution << " FG=" << requestedFrameGeneration << '\n';
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
            const auto colourFormat = bgraSrgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                                               : (bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
            auto colour = createImages(session, colourFormat, width, height, false, !d3d12);
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
            const unsigned inputs = reuse ? 4u : ((fallback || toggle || busyRecovery) ? 18u : 12u);
            unsigned beforeDiscard{};
            XrTime previousPublicTime{};
            NrLookupProbe lookupProbe;
            auto releaseSpatialObservation = MakeScopeGuard([&] { Regression::observeSpatial(false); });
            const unsigned inputCount = steady ? 128u : spatial ? 30u : inputs;
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
                const bool nr = bgra || (toggle ? (frame < 3 || frame >= 6) : (!noNr && frame != 0));
                virtualdesktop_openxr::RuntimeInputRegression::setEnabled(nr);
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
                    input12->fill(image);
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
                XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
                for (unsigned eye = 0; eye < 2; ++eye) {
                    views[eye].pose.orientation.w = 1.f;
                    views[eye].pose.position.x = eye ? .032f : -.032f;
                    views[eye].fov = {-.8f, .8f, .8f, -.8f};
                    if (busyRecovery && !captureBusyRecovery && frame >= 4 && !eye) {
                        views[eye].fov.angleLeft = -.7f;
                    }
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
                const XrCompositionLayerBaseHeader* duplicateLayers[]{header, header};
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
                end.displayTime = state.predictedDisplayTime;
                end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                end.layerCount = 1;
                end.layers = &header;
                if (busyRecovery && frame == 9) {
                    // A genuinely unsupported composition must still select
                    // native cadence; it is not temporary history pressure.
                    end.layerCount = 2;
                    end.layers = duplicateLayers;
                }
                Regression::beforeInputEnd();
                if (spatialBypass || (fallback && frame == (reuseFallback ? 3u : 2u))) {
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
                const bool measureLookups = nr && frame >= 2 && !spatialOff;
                if (measureLookups)
                    Regression::beginLookupProbe(lookupProbe);
                VDXRFG_StatisticsV1 beforeRecovery{};
                size_t beforeRecoveryRoles{};
                if (busyRecovery && (frame == 5 || frame == 6)) {
                    beforeRecovery = Regression::statistics();
                    beforeRecoveryRoles = Regression::presentedRoles.size();
                }
                XR_CHECK(xrEndFrame(session, &end));
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
                if (!spatialOff)
                    Regression::verifyFinal(nr,
                                            ((51200 / nrUpscalingSetting + 3) / 4) * 4,
                                            frame <= 1 || frame == inputCount - 1 ||
                                                (recoverSpatial && frame >= inputCount));
                if (publication4)
                    Regression::verifyPublishedSpan();
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
                        if (spatial)
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
                if (!captureFailure.empty()) {
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
            const auto expectedNr = noNr ? 0u : executedInputs - (toggle ? 5u : 2u);
            Regression::verifyPixels();
            if (spatialOff)
                Regression::verifySpatialOffPixels();
            else if (!noNr)
                Regression::verifyRealPixels(argv[2]);
            unsigned eyeDifference{};
            for (unsigned component = 0; component < 3; ++component)
                eyeDifference += std::abs(int(observedColour[0][component]) - int(observedColour[1][component]));
            require(eyeDifference > 32, "final FG presentation preserves distinct left and right eye pixels");
            require(Regression::nrEvaluations[0] == expectedNr && Regression::nrEvaluations[1] == expectedNr,
                    "exactly one real NR evaluation per eye per input, none on synthesized output");
            require(!Regression::invalidNrParameters,
                    "actual forwarded Ada evaluations preserve full NR rectangles, preset B and hair/face controls");
            const auto statistics = spatialOff ? VDXRFG_StatisticsV1{} : Regression::statistics();
            const auto minimumSynthetic = requestedFrameGeneration ? 4u * (requestedFrameGeneration - 1) : 0u;
            if (reuse) {
                // These cases deliberately stall source processing. Ownership
                // and late real fallback are tested independently of cadence;
                // the normal 2X/3X cases require actual synthetic submissions.
                require(statistics.captures >= 2 && !Regression::presentedIds.empty(),
                        "source reuse surrounds real module captures and native presentations");
            } else if (!spatialOff) {
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
                require(Regression::syntheticPresentations >= beforeDiscard + 4,
                        "public discard recovers real synthetic presentation after reanchoring");
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
            observeProjection = {};
            observeNativeEnd = {};
            observeNativeCommit = {};
            Regression::observeSpatial(false);
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
