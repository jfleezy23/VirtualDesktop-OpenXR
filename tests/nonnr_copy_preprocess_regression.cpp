// Hardware component regressions through real runtime copy/preprocess/EndFrame methods.
// Backend indexing/commit/submission are intercepted; no native compositor or settings writes.
#include "pch.h"
#include "runtime.h"
#include <array>

OVR_PUBLIC_FUNCTION(ovrResult) ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }
    std::atomic<bool> watching = false;
    DWORD watchedThreadId{};
    unsigned copies = 0;
    unsigned commits = 0;
    unsigned submitted = 0;
    unsigned sourceCurrent = 0, destinationCurrent = 0, chainLength = 1;
    using Copy = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
    Copy originalCopy{};
    ID3D11DeviceContext* watchedContext{};
    using Map = HRESULT(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
    Map originalMap{};
    bool injectCorrectionFailure = false;
    unsigned correctionMaps = 0;
    HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* context,
                                  ID3D11Resource* resource,
                                  UINT subresource,
                                  D3D11_MAP type,
                                  UINT flags,
                                  D3D11_MAPPED_SUBRESOURCE* output) {
        if (GetCurrentThreadId() == watchedThreadId && watching && context == watchedContext &&
            type == D3D11_MAP_WRITE_DISCARD) {
            ++correctionMaps;
            if (injectCorrectionFailure && correctionMaps == 3) {
                injectCorrectionFailure = false;
                return E_FAIL;
            }
        }
        return originalMap(context, resource, subresource, type, flags, output);
    }
    decltype(&ovr_GetTextureSwapChainCurrentIndex) originalIndex{};
    decltype(&ovr_CommitTextureSwapChain) originalCommit{};
    decltype(&ovr_EndFrame) originalEnd{};
    void STDMETHODCALLTYPE copy(ID3D11DeviceContext* context,
                                ID3D11Resource* dst,
                                UINT index,
                                UINT x,
                                UINT y,
                                UINT z,
                                ID3D11Resource* src,
                                UINT sourceIndex,
                                const D3D11_BOX* box) {
        if (GetCurrentThreadId() == watchedThreadId && watching && context == watchedContext)
            ++copies;
        originalCopy(context, dst, index, x, y, z, src, sourceIndex, box);
    }
    ovrResult OVR_CDECL index(ovrSession, ovrTextureSwapChain chain, int* output) {
        *output = chain == reinterpret_cast<ovrTextureSwapChain>(0x1000) ? sourceCurrent : destinationCurrent;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL commit(ovrSession, ovrTextureSwapChain chain) {
        if (GetCurrentThreadId() == watchedThreadId && watching)
            ++commits;
        auto& current = chain == reinterpret_cast<ovrTextureSwapChain>(0x1000) ? sourceCurrent : destinationCurrent;
        current = (current + 1) % chainLength;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL
    end(ovrSession, long long, const ovrViewScaleDesc*, const ovrLayerHeader* const* layers, unsigned count) {
        require(layers && count > 0 && count <= ovrMaxLayerCount, "Backend layer boundary invalid");
        ++submitted;
        return ovrSuccess;
    }
    struct Hooks {
        Hooks(const wchar_t* directory, ID3D11DeviceContext* context) {
            watchedThreadId = GetCurrentThreadId();
            ovrInitParams params{};
            params.Flags = ovrInit_RequestVersion;
            params.RequestedMinorVersion = OVR_MINOR_VERSION;
            auto path = std::filesystem::absolute(directory).wstring() + L"\\";
            require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&params, path.c_str())), "Explicit OVRNull init failed");
            auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            require(module != nullptr, "Backend missing");
            originalIndex = reinterpret_cast<decltype(originalIndex)>(
                GetProcAddress(module, "ovr_GetTextureSwapChainCurrentIndex"));
            originalCommit =
                reinterpret_cast<decltype(originalCommit)>(GetProcAddress(module, "ovr_CommitTextureSwapChain"));
            originalEnd = reinterpret_cast<decltype(originalEnd)>(GetProcAddress(module, "ovr_EndFrame"));
            watchedContext = context;
            originalCopy = reinterpret_cast<Copy>((*reinterpret_cast<void***>(context))[46]);
            originalMap = reinterpret_cast<Map>((*reinterpret_cast<void***>(context))[14]);
            require(originalIndex && originalCommit && originalEnd && originalCopy && originalMap,
                    "Probe exports missing");
            require(DetourTransactionBegin() == NO_ERROR, "Hook transaction failed");
            LONG error = DetourUpdateThread(GetCurrentThread());
            error |= DetourAttach(reinterpret_cast<PVOID*>(&originalIndex), index);
            error |= DetourAttach(reinterpret_cast<PVOID*>(&originalCommit), commit);
            error |= DetourAttach(reinterpret_cast<PVOID*>(&originalEnd), end);
            error |= DetourAttach(reinterpret_cast<PVOID*>(&originalCopy), copy);
            error |= DetourAttach(reinterpret_cast<PVOID*>(&originalMap), map);
            if (error) {
                DetourTransactionAbort();
                throw std::runtime_error("Hook attach failed");
            }
            require(DetourTransactionCommit() == NO_ERROR, "Hook commit failed");
        }
        ~Hooks() {
            watching = false;
            LONG error = DetourTransactionBegin();
            error |= DetourUpdateThread(GetCurrentThread());
            error |= DetourDetach(reinterpret_cast<PVOID*>(&originalIndex), index);
            error |= DetourDetach(reinterpret_cast<PVOID*>(&originalCommit), commit);
            error |= DetourDetach(reinterpret_cast<PVOID*>(&originalEnd), end);
            error |= DetourDetach(reinterpret_cast<PVOID*>(&originalCopy), copy);
            error |= DetourDetach(reinterpret_cast<PVOID*>(&originalMap), map);
            error |= DetourTransactionCommit();
            if (error)
                std::terminate();
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void run(const wchar_t* directory, const std::wstring& mode) {
            const bool gamma = mode == L"gamma" || mode == L"gamma-stress";
            const bool direct = mode == L"direct";
            const bool rotating = mode == L"rotating" || mode == L"fresh-release";
            const bool pending = mode == L"pending";
            const bool premultiply = !(mode == L"clear" || direct || mode == L"blend");
            chainLength = rotating ? 3 : 1;
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            CHECK_HRCMD(D3D11CreateDevice(nullptr,
                                          D3D_DRIVER_TYPE_HARDWARE,
                                          nullptr,
                                          0,
                                          nullptr,
                                          0,
                                          D3D11_SDK_VERSION,
                                          device.GetAddressOf(),
                                          nullptr,
                                          context.GetAddressOf()));
            CHECK_HRCMD(device.As(&runtime.m_ovrSubmissionDevice));
            CHECK_HRCMD(context.As(&runtime.m_ovrSubmissionContext));
            runtime.m_d3d11Device = runtime.m_ovrSubmissionDevice;
            runtime.m_d3d11Context = runtime.m_ovrSubmissionContext;
            auto feature = device->GetFeatureLevel();
            CHECK_HRCMD(runtime.m_ovrSubmissionDevice->CreateDeviceContextState(
                0,
                &feature,
                1,
                D3D11_SDK_VERSION,
                __uuidof(ID3D11Device),
                nullptr,
                runtime.m_ovrSubmissionContextState.GetAddressOf()));
            Hooks hooks(directory, runtime.m_ovrSubmissionContext.Get());
            runtime.m_sessionCreated = runtime.m_sessionBegun = true;
            runtime.m_sessionState = XR_SESSION_STATE_FOCUSED;
            runtime.m_hmdStatus.HmdPresent = runtime.m_hmdStatus.HmdMounted = runtime.m_hmdStatus.IsVisible = true;
            runtime.m_useAsyncSubmission = false;
            runtime.m_isHeadless = false;
            runtime.m_useOculusRuntime = true;
            runtime.m_useMirrorWindow = false;
            for (auto& eye : runtime.m_cachedEyeInfo)
                eye.HmdToEyePose.Orientation.w = 1.f;
            OpenXrRuntime::Space space{};
            space.referenceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            space.poseInSpace = xr::math::Pose::Identity();
            auto spaceHandle = reinterpret_cast<XrSpace>(&space);
            runtime.m_spaces.insert(spaceHandle);
            OpenXrRuntime::Swapchain chain{};
            chain.xrDesc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
            chain.xrDesc.width = chain.xrDesc.height = 64;
            chain.xrDesc.arraySize = 2;
            chain.xrDesc.faceCount = chain.xrDesc.mipCount = chain.xrDesc.sampleCount = 1;
            chain.xrDesc.createFlags = rotating ? 0 : XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
            chain.xrDesc.format = gamma ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            chain.xrDesc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            chain.ovrDesc.Type = ovrTexture_2D;
            chain.ovrDesc.Format = gamma ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM;
            chain.ovrDesc.BindFlags = ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess;
            chain.ovrDesc.MiscFlags = ovrTextureMisc_DX_Typeless;
            chain.ovrDesc.ArraySize = 2;
            chain.ovrDesc.Width = chain.ovrDesc.Height = 64;
            chain.ovrDesc.MipLevels = chain.ovrDesc.SampleCount = 1;
            chain.ovrDesc.StaticImage = !rotating;
            chain.dxgiFormatForSubmission = gamma ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            chain.ovrSwapchainLength = chainLength;
            chain.appSwapchain.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(0x1000);
            chain.appSwapchain.images.resize(chainLength);
            chain.resolvedSlices.resize(2);
            chain.resolvedSlices[1].ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(0x2000);
            chain.resolvedSlices[1].images.resize(chainLength);
            std::vector<uint8_t> pixels(64 * 64 * 4);
            for (size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = 153;
                pixels[i + 1] = 102;
                pixels[i + 2] = 51;
                pixels[i + 3] = 128;
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 64;
            desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.ArraySize = 2;
            desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
            D3D11_SUBRESOURCE_DATA initial[2]{{pixels.data(), 256, 0}, {pixels.data(), 256, 0}};
            for (auto& texture : chain.appSwapchain.images)
                CHECK_HRCMD(device->CreateTexture2D(&desc, initial, texture.GetAddressOf()));
            desc.ArraySize = 1;
            for (auto& texture : chain.resolvedSlices[1].images)
                CHECK_HRCMD(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()));
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            CHECK_HRCMD(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()));
            auto chainHandle = reinterpret_cast<XrSwapchain>(&chain);
            runtime.m_swapchains.insert(chainHandle);
            auto cleanup = MakeScopeGuard([&] {
                runtime.m_spaces.erase(spaceHandle);
                runtime.m_swapchains.erase(chainHandle);
            });
            uint32_t image = 99;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            require(runtime.xrAcquireSwapchainImage(chainHandle, &acquire, &image) == XR_SUCCESS && image == 0,
                    "Acquire failed");
            require(runtime.xrWaitSwapchainImage(chainHandle, &wait) == XR_SUCCESS, "Wait failed");
            context->UpdateSubresource(chain.appSwapchain.images[0].Get(), 0, nullptr, pixels.data(), 256, 0);
            context->UpdateSubresource(chain.appSwapchain.images[0].Get(), 1, nullptr, pixels.data(), 256, 0);
            require(runtime.xrReleaseSwapchainImage(chainHandle, &release) == XR_SUCCESS && chain.dirty,
                    "Release failed");
            std::array<XrCompositionLayerQuad, ovrMaxLayerCount> quads{};
            std::array<const XrCompositionLayerBaseHeader*, ovrMaxLayerCount> layers{};
            for (unsigned i = 0; i < quads.size(); ++i) {
                auto& q = quads[i];
                q.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                q.space = spaceHandle;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage = {
                    chainHandle, {{0, 0}, {64, 64}}, direct ? 0u : (i == 0 && mode != L"primary-noop" ? 0u : 1u)};
                q.pose = xr::math::Pose::Identity();
                q.size = {1.f, 1.f};
                if (premultiply)
                    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                   XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                if (mode == L"blend")
                    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
            }
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = 1000000000;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = 2;
            endInfo.layers = layers.data();
            if (mode == L"duplicate" || mode == L"disjoint" || mode == L"overlap" || mode == L"correction-retry")
                endInfo.layerCount = 3;
            if (mode == L"disjoint") {
                quads[1].subImage.imageRect = {{5, 7}, {20, 35}};
                quads[2].subImage.imageRect = {{30, 7}, {25, 35}};
            }
            if (mode == L"overlap" || mode == L"correction-retry") {
                quads[1].subImage.imageRect = {{5, 7}, {33, 35}};
                quads[2].subImage.imageRect = {{15, 17}, {33, 35}};
            }
            if (mode == L"viewport-changing")
                quads[1].subImage.imageRect = {{5, 7}, {20, 35}};
            if (mode == L"mixed-disjoint") {
                endInfo.layerCount = 4;
                quads[1].subImage.imageRect = quads[3].subImage.imageRect = {{0, 0}, {32, 64}};
                quads[2].subImage.imageRect = {{32, 0}, {32, 64}};
                quads[2].layerFlags = 0;
            }
            if (mode == L"mixed-overlap-coverage") {
                endInfo.layerCount = 4;
                quads[1].subImage.imageRect = {{5, 7}, {50, 45}};
                quads[2].subImage.imageRect = {{20, 17}, {20, 20}};
                quads[3].subImage.imageRect = {{5, 7}, {15, 10}};
                quads[2].layerFlags = 0;
            }
            auto read = [&](bool source) {
                auto& slice = chain.resolvedSlices[direct ? 0 : 1];
                auto& texture = source ? chain.appSwapchain.images[chain.lastReleasedIndex]
                                       : slice.images[slice.lastCommittedIndex];
                context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), source ? 1u : 0u, nullptr);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                CHECK_HRCMD(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                std::vector<uint8_t> data(64 * 64 * 4);
                for (unsigned y = 0; y < 64; ++y)
                    memcpy(data.data() + y * 256, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, 256);
                context->Unmap(staging.Get(), 0);
                return data;
            };
            unsigned frames = 0;
            std::array<uint8_t, 4> raw{153, 102, 51, 128};
            for (unsigned frame = 0; frame < 3; ++frame) {
                if (mode == L"regions-stress" || mode == L"gamma-stress") {
                    endInfo.layerCount = ovrMaxLayerCount;
                    uint32_t random = 42 + frame;
                    const auto next = [&] {
                        random = random * 1664525u + 1013904223u;
                        return random;
                    };
                    for (unsigned i = 1; i < endInfo.layerCount; ++i) {
                        const int32_t x = next() % 63, y = next() % 63;
                        quads[i].subImage.imageRect = {
                            {x, y}, {int32_t(1 + next() % (64 - x)), int32_t(1 + next() % (64 - y))}};
                    }
                }
                if (frame > 0 && mode == L"fresh-release") {
                    require(runtime.xrAcquireSwapchainImage(chainHandle, &acquire, &image) == XR_SUCCESS,
                            "Fresh acquire failed");
                    require(runtime.xrWaitSwapchainImage(chainHandle, &wait) == XR_SUCCESS, "Fresh wait failed");
                    raw[0] = uint8_t(153 - frame * 30);
                    for (size_t i = 0; i < pixels.size(); i += 4)
                        pixels[i] = raw[0];
                    context->UpdateSubresource(
                        chain.appSwapchain.images[image].Get(), 0, nullptr, pixels.data(), 256, 0);
                    context->UpdateSubresource(
                        chain.appSwapchain.images[image].Get(), 1, nullptr, pixels.data(), 256, 0);
                    require(runtime.xrReleaseSwapchainImage(chainHandle, &release) == XR_SUCCESS,
                            "Fresh release failed");
                }
                if (mode == L"flags-changing")
                    quads[1].layerFlags = frame == 0   ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                                             XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
                                          : frame == 1 ? 0
                                                       : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                if (mode == L"viewport-changing")
                    quads[1].subImage.imageRect = {{int32_t(5 + frame * 15), 7}, {20, 35}};
                copies = commits = correctionMaps = 0;
                watching = true;
                if (pending) {
                    runtime.m_precompositor.resolvedSwapchainImages.clear();
                    runtime.m_precompositor.pendingSwapchainCommits.clear();
                    runtime.resolveSwapchainImage(chain, 1, runtime.m_precompositor.resolvedSwapchainImages, true);
                    runtime.preprocessSwapchainImage(chain, 1, 1, quads[1].layerFlags, quads[1].subImage.imageRect);
                    runtime.resolveSwapchainImage(chain, 1, runtime.m_precompositor.resolvedSwapchainImages);
                    require(runtime.m_precompositor.pendingSwapchainCommits.empty(), "Pending commit not consumed");
                    runtime.preprocessSwapchainImage(chain, 1, 1, quads[1].layerFlags, quads[1].subImage.imageRect);
                    chain.dirty = false;
                } else {
                    runtime.m_frameWaited = runtime.m_frameBegun = ++frames;
                    runtime.m_renderTimerApp.start();
                    if (mode == L"correction-retry" && frame == 0) {
                        injectCorrectionFailure = true;
                        bool failed = false;
                        try {
                            runtime.xrEndFrame(reinterpret_cast<XrSession>(1), &endInfo);
                        } catch (const std::exception&) {
                            failed = true;
                        }
                        require(failed && !injectCorrectionFailure && submitted == 0,
                                "Injected correction failure must propagate before backend submission");
                    }
                    require(runtime.xrEndFrame(reinterpret_cast<XrSession>(1), &endInfo) == XR_SUCCESS,
                            "Actual EndFrame failed");
                }
                watching = false;
                const auto result = read(false), source = read(true);
                require(copies == (direct                                      ? 0u
                                   : mode == L"correction-retry" && frame == 0 ? 2u
                                                                               : 1u),
                        "Wrong runtime copy count");
                if (pending)
                    require(commits == 1, "Pending path must commit the existing corrected destination once");
                if (mode == L"mixed-overlap-coverage")
                    require(correctionMaps == 2, "Unchanged pixels outside a different transform were corrected again");
                unsigned wrong = 0;
                for (unsigned y = 0; y < 64; ++y)
                    for (unsigned x = 0; x < 64; ++x) {
                        auto expected = raw;
                        bool covered = false, clearAlpha = false, multiply = false;
                        for (unsigned layer = 1; layer < endInfo.layerCount; ++layer) {
                            auto r = quads[layer].subImage.imageRect;
                            if (x >= unsigned(r.offset.x) && x < unsigned(r.offset.x + r.extent.width) &&
                                y >= unsigned(r.offset.y) && y < unsigned(r.offset.y + r.extent.height)) {
                                covered = true;
                                clearAlpha =
                                    !(quads[layer].layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                                multiply = !!(quads[layer].layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
                            }
                        }
                        if (covered) {
                            // This checks retained one-output ordering, not compositing of contradictory layer flags.
                            if (mode == L"mixed-overlap-coverage" && x >= 20 && x < 40 && y >= 17 && y < 37)
                                multiply = true;
                            if (clearAlpha)
                                expected[3] = 255;
                            if (multiply)
                                for (unsigned c = 0; c < 3; ++c) {
                                    double v = raw[c] / 255.;
                                    if (gamma)
                                        v = v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
                                    v *= mode == L"mixed-overlap-coverage" ? 128. / 255. : expected[3] / 255.;
                                    if (gamma)
                                        v = v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1. / 2.4) - .055;
                                    expected[c] = uint8_t(std::lround(v * 255.));
                                }
                        }
                        for (unsigned c = 0; c < 4; ++c) {
                            const auto offset = (y * 64 + x) * 4 + c;
                            if (source[offset] != raw[c] ||
                                std::abs(int(result[offset]) - int(expected[c])) > (gamma ? 2 : 0))
                                ++wrong;
                        }
                    }
                std::cout << "FRAME " << frame + 1 << " wrong_components=" << wrong << " copies=" << copies
                          << " commits=" << commits << "\n";
                require(wrong == 0, "Copied image correction/region/source preservation failed");
            }
            std::cout << "PASS: repeated copy/preprocess pixels and region ownership\n";
            require(submitted == frames, "Backend submission count mismatch");
            watching = false;
        }
    };
} // namespace virtualdesktop_openxr
int wmain(int argc, wchar_t** argv) {
    try {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        require(argc == 3, "Mode and OVRNull directory required");
        const std::wstring mode = argv[1];
        require(mode == L"clear" || mode == L"premultiply" || mode == L"gamma" || mode == L"direct" ||
                    mode == L"blend" || mode == L"duplicate" || mode == L"disjoint" || mode == L"overlap" ||
                    mode == L"primary-noop" || mode == L"viewport-changing" || mode == L"flags-changing" ||
                    mode == L"rotating" || mode == L"fresh-release" || mode == L"pending" ||
                    mode == L"regions-stress" || mode == L"gamma-stress" || mode == L"correction-retry" ||
                    mode == L"mixed-disjoint" || mode == L"mixed-overlap-coverage",
                "Unknown regression mode");
        virtualdesktop_openxr::RuntimeInputRegression::run(argv[2], argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
