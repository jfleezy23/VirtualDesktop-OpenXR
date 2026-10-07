// Run the production depth alignment shader on WARP. Only external OVR image allocation is intercepted.
// No registry writes, neural rendering module, headset, or installed runtime changes.
#include "pch.h"
#include "runtime.h"
#include "FullScreenQuadVS.h"
#include <iostream>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    void checkHr(HRESULT result) {
        require(SUCCEEDED(result), "Native D3D operation failed");
    }
    struct OutputChain {
        ovrTextureSwapChainDesc desc{};
        ComPtr<ID3D11Texture2D> image;
    };
    std::map<ovrTextureSwapChain, std::unique_ptr<OutputChain>> chains;
    unsigned commits;
    decltype(&ovr_CreateTextureSwapChainDX) originalCreate;
    decltype(&ovr_GetTextureSwapChainLength) originalLength;
    decltype(&ovr_GetTextureSwapChainBufferDX) originalBuffer;
    decltype(&ovr_GetTextureSwapChainCurrentIndex) originalIndex;
    decltype(&ovr_GetTextureSwapChainDesc) originalDesc;
    decltype(&ovr_CommitTextureSwapChain) originalCommit;
    decltype(&ovr_DestroyTextureSwapChain) originalDestroy;

    ovrResult OVR_CDECL createChain(ovrSession,
                                    IUnknown* unknown,
                                    const ovrTextureSwapChainDesc* desc,
                                    ovrTextureSwapChain* output) {
        ComPtr<ID3D11Device> device;
        if (!unknown || !desc || !output || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&device))))
            return ovrError_InvalidParameter;
        auto chain = std::make_unique<OutputChain>();
        chain->desc = *desc;
        D3D11_TEXTURE2D_DESC native{};
        native.Width = desc->Width;
        native.Height = desc->Height;
        native.ArraySize = native.MipLevels = native.SampleDesc.Count = 1;
        if (desc->Format == OVR_FORMAT_D32_FLOAT) {
            native.Format = DXGI_FORMAT_R32_TYPELESS;
            native.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            std::vector<float> sentinel(native.Width * native.Height, .99f);
            D3D11_SUBRESOURCE_DATA data{sentinel.data(), native.Width * sizeof(float), 0};
            if (FAILED(device->CreateTexture2D(&native, &data, &chain->image)))
                return ovrError_MemoryAllocationFailure;
        } else {
            native.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            native.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device->CreateTexture2D(&native, nullptr, &chain->image)))
                return ovrError_MemoryAllocationFailure;
        }
        const auto handle = reinterpret_cast<ovrTextureSwapChain>(chain.get());
        chains.emplace(handle, std::move(chain));
        *output = handle;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainLength(ovrSession, ovrTextureSwapChain handle, int* output) {
        if (!chains.count(handle) || !output)
            return ovrError_InvalidParameter;
        *output = 1;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainBuffer(ovrSession, ovrTextureSwapChain handle, int index, IID iid, void** output) {
        const auto found = chains.find(handle);
        if (found == chains.end() || index || !output || FAILED(found->second->image->QueryInterface(iid, output)))
            return ovrError_InvalidParameter;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainIndex(ovrSession, ovrTextureSwapChain handle, int* output) {
        if (!chains.count(handle) || !output)
            return ovrError_InvalidParameter;
        *output = 0;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainDesc(ovrSession, ovrTextureSwapChain handle, ovrTextureSwapChainDesc* output) {
        const auto found = chains.find(handle);
        if (found == chains.end() || !output)
            return ovrError_InvalidParameter;
        *output = found->second->desc;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL commitChain(ovrSession, ovrTextureSwapChain handle) {
        if (!chains.count(handle))
            return ovrError_InvalidParameter;
        ++commits;
        return ovrSuccess;
    }
    void OVR_CDECL destroyChain(ovrSession, ovrTextureSwapChain handle) {
        require(chains.erase(handle) == 1, "Unknown output chain destroyed");
    }
    struct Hooks {
        explicit Hooks(const wchar_t* directory) {
            ovrInitParams init{};
            init.Flags = ovrInit_RequestVersion;
            init.RequestedMinorVersion = OVR_MINOR_VERSION;
            const auto path = std::filesystem::absolute(directory).wstring() + L"\\";
            require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, path.c_str())), "OVRNull initialization failed");
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
#define LOAD(variable, name)                                                                                           \
    variable = reinterpret_cast<decltype(variable)>(GetProcAddress(module, #name));                                    \
    require(variable, "Missing OVR export")
            LOAD(originalCreate, ovr_CreateTextureSwapChainDX);
            LOAD(originalLength, ovr_GetTextureSwapChainLength);
            LOAD(originalBuffer, ovr_GetTextureSwapChainBufferDX);
            LOAD(originalIndex, ovr_GetTextureSwapChainCurrentIndex);
            LOAD(originalDesc, ovr_GetTextureSwapChainDesc);
            LOAD(originalCommit, ovr_CommitTextureSwapChain);
            LOAD(originalDestroy, ovr_DestroyTextureSwapChain);
#undef LOAD
            commits = 0;
            transact(true);
        }
        ~Hooks() {
            transact(false);
            chains.clear();
        }
        static void transact(bool attach) {
            LONG result = DetourTransactionBegin();
            const bool active = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
#define CHANGE(variable, replacement)                                                                                  \
    if (result == NO_ERROR)                                                                                            \
    result = attach ? DetourAttach(reinterpret_cast<PVOID*>(&variable), replacement)                                   \
                    : DetourDetach(reinterpret_cast<PVOID*>(&variable), replacement)
            CHANGE(originalCreate, createChain);
            CHANGE(originalLength, chainLength);
            CHANGE(originalBuffer, chainBuffer);
            CHANGE(originalIndex, chainIndex);
            CHANGE(originalDesc, chainDesc);
            CHANGE(originalCommit, commitChain);
            CHANGE(originalDestroy, destroyChain);
#undef CHANGE
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (active)
                DetourTransactionAbort();
            require(result == NO_ERROR, "OVR hook transaction failed");
        }
    };
    float value(unsigned x, unsigned y, unsigned eye) {
        return (1.f + x + 16.f * y + 32.f * eye) / 512.f;
    }
    float quantized(float input, DXGI_FORMAT format) {
        if (format == DXGI_FORMAT_D16_UNORM)
            return std::round(input * 65535.f) / 65535.f;
        if (format == DXGI_FORMAT_D24_UNORM_S8_UINT)
            return std::round(input * 16777215.f) / 16777215.f;
        return input;
    }
    void checkPixels(ID3D11Device* device,
                     ID3D11DeviceContext* context,
                     ovrTextureSwapChain handle,
                     XrRect2Di source,
                     ovrRecti destination,
                     DXGI_FORMAT format,
                     unsigned eye) {
        require(chains.count(handle), "Alignment did not publish an output image");
        auto image = chains.at(handle)->image;
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), image.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        unsigned failures = 0;
        for (unsigned y = 0; y < desc.Height; ++y) {
            const auto row =
                reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0; x < desc.Width; ++x) {
                float expected = .99f;
                if (int(x) >= destination.Pos.x && int(x) < destination.Pos.x + destination.Size.w &&
                    int(y) >= destination.Pos.y && int(y) < destination.Pos.y + destination.Size.h) {
                    const auto sx =
                        source.offset.x + int((x - destination.Pos.x + .5f) * source.extent.width / destination.Size.w);
                    const auto sy = source.offset.y +
                                    int((y - destination.Pos.y + .5f) * source.extent.height / destination.Size.h);
                    expected = quantized(value(sx, sy, eye), format);
                }
                if (std::abs(row[x] - expected) > .000002f)
                    ++failures;
            }
        }
        context->Unmap(staging.Get(), 0);
        std::cout << "eye=" << eye << " format=" << format << " pixel failures=" << failures << '\n';
        require(failures == 0, "Aligned depth pixels or untouched viewport border are incorrect");
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void run(const wchar_t* directory, DXGI_FORMAT format) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            checkHr(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
            checkHr(device.As(&runtime.m_ovrSubmissionDevice));
            checkHr(context.As(&runtime.m_ovrSubmissionContext));
            checkHr(runtime.m_ovrSubmissionDevice->CreateFence(
                0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&runtime.m_ovrSubmissionCompletionFence)));
            runtime.initializePrecompositorResources();
            checkHr(device->CreateVertexShader(
                g_FullScreenQuadVS, sizeof(g_FullScreenQuadVS), nullptr, &runtime.m_fullQuadVS));
            D3D11_DEPTH_STENCIL_DESC state{};
            state.DepthEnable = TRUE;
            state.DepthFunc = D3D11_COMPARISON_ALWAYS;
            state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            checkHr(device->CreateDepthStencilState(&state, &runtime.m_noDepthReadState));
            Hooks hooks(directory);
            auto owner = std::make_unique<OpenXrRuntime::Swapchain>();
            std::array<OpenXrRuntime::Swapchain, 2> input;
            XrSwapchainSubImage color[2]{};
            XrSwapchainSubImage depth[2]{};
            const XrSwapchainSubImage* colorPointers[]{&color[0], &color[1]};
            const XrSwapchainSubImage* depthPointers[]{&depth[0], &depth[1]};
            ovrLayerEyeFovDepth layer{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                color[eye].swapchain = reinterpret_cast<XrSwapchain>(owner.get());
                depth[eye].swapchain = reinterpret_cast<XrSwapchain>(&input[eye]);
                depth[eye].imageRect = eye ? XrRect2Di{{5, 1}, {8, 9}} : XrRect2Di{{3, 2}, {7, 6}};
                layer.Viewport[eye] = eye ? ovrRecti{{1, 2}, {9, 7}} : ovrRecti{{2, 3}, {24, 15}};
                ovrTextureSwapChainDesc output{};
                output.Type = ovrTexture_2D;
                output.Format = OVR_FORMAT_R8G8B8A8_UNORM;
                output.Width = eye ? 22 : 32;
                output.Height = eye ? 18 : 24;
                require(OVR_SUCCESS(createChain(nullptr, device.Get(), &output, &layer.ColorTexture[eye])),
                        "Color allocation failed");
                auto& chain = input[eye];
                chain.ovrDesc.Width = 16;
                chain.ovrDesc.Height = 12;
                chain.dxgiFormatForSubmission = format;
                chain.resolvedSlices.resize(1);
                chain.resolvedSlices[0].lastCommittedIndex = 0;
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = 16;
                desc.Height = 12;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
                desc.Format = format == DXGI_FORMAT_D16_UNORM              ? DXGI_FORMAT_R16_TYPELESS
                              : format == DXGI_FORMAT_D24_UNORM_S8_UINT    ? DXGI_FORMAT_R24G8_TYPELESS
                              : format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_R32G8X24_TYPELESS
                                                                           : DXGI_FORMAT_R32_TYPELESS;
                const unsigned pixelSize = format == DXGI_FORMAT_D16_UNORM              ? 2
                                           : format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8
                                                                                        : 4;
                std::vector<uint8_t> pixels(16 * 12 * pixelSize);
                for (unsigned y = 0; y < 12; ++y)
                    for (unsigned x = 0; x < 16; ++x) {
                        auto* target = pixels.data() + (y * 16 + x) * pixelSize;
                        const float sample = value(x, y, eye);
                        if (format == DXGI_FORMAT_D16_UNORM) {
                            const uint16_t native = uint16_t(std::round(sample * 65535.f));
                            memcpy(target, &native, sizeof(native));
                        } else if (format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                            const uint32_t native = uint32_t(std::round(sample * 16777215.f)) | 0x5a000000u;
                            memcpy(target, &native, sizeof(native));
                        } else
                            memcpy(target, &sample, sizeof(sample));
                    }
                D3D11_SUBRESOURCE_DATA data{pixels.data(), 16 * pixelSize, 0};
                ComPtr<ID3D11Texture2D> image;
                checkHr(device->CreateTexture2D(&desc, &data, &image));
                chain.resolvedSlices[0].images.push_back(image);
            }
            runtime.alignDepthLayer(colorPointers, depthPointers, layer);
            const std::array<ovrTextureSwapChain, 2> first{layer.DepthTexture[0], layer.DepthTexture[1]};
            const std::array<XrRect2Di, 2> originalRects{depth[0].imageRect, depth[1].imageRect};
            for (unsigned eye = 0; eye < 2; ++eye)
                checkPixels(
                    device.Get(), context.Get(), first[eye], depth[eye].imageRect, layer.Viewport[eye], format, eye);
            runtime.alignDepthLayer(colorPointers, depthPointers, layer);
            require(layer.DepthTexture[0] == first[0] && layer.DepthTexture[1] == first[1],
                    "Matching geometry did not reuse cached depth images");
            runtime.m_precompositor.layerIndex = 1;
            depth[0].imageRect.offset.x++;
            depth[1].imageRect.offset.y++;
            runtime.alignDepthLayer(colorPointers, depthPointers, layer);
            require(layer.DepthTexture[0] != first[0] && layer.DepthTexture[1] != first[1],
                    "Later layer overwrote an earlier layer's depth image");
            for (unsigned eye = 0; eye < 2; ++eye) {
                checkPixels(
                    device.Get(), context.Get(), first[eye], originalRects[eye], layer.Viewport[eye], format, eye);
                checkPixels(device.Get(),
                            context.Get(),
                            layer.DepthTexture[eye],
                            depth[eye].imageRect,
                            layer.Viewport[eye],
                            format,
                            eye);
            }
            require(commits == 6, "Every aligned eye image must be committed exactly once per call");
            runtime.m_sessionCreated = true;
            const auto handle = reinterpret_cast<XrSwapchain>(owner.release());
            runtime.m_swapchains.insert(handle);
            require(runtime.xrDestroySwapchain(handle) == XR_SUCCESS, "Depth cache swapchain destruction failed");
            runtime.m_sessionCreated = false;
            require(chains.size() == 2, "Depth projection outputs survived real swapchain destruction");
            destroyChain(nullptr, layer.ColorTexture[0]);
            destroyChain(nullptr, layer.ColorTexture[1]);
            require(chains.empty(), "Output images survived fixture cleanup");
            std::cout << "PASS: depth format " << format
                      << ", scaled offset pixels, unequal eyes, layer cache and cleanup\n";
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 3, "usage: nonnr_depth_alignment_regression d16|d24s8|d32|d32s8|all OVRNull-directory");
        const std::wstring mode = argv[1];
        const std::pair<const wchar_t*, DXGI_FORMAT> formats[]{{L"d16", DXGI_FORMAT_D16_UNORM},
                                                               {L"d24s8", DXGI_FORMAT_D24_UNORM_S8_UINT},
                                                               {L"d32", DXGI_FORMAT_D32_FLOAT},
                                                               {L"d32s8", DXGI_FORMAT_D32_FLOAT_S8X24_UINT}};
        bool matched = false;
        for (auto [name, format] : formats)
            if (mode == name || mode == L"all") {
                matched = true;
                virtualdesktop_openxr::RuntimeInputRegression::run(argv[2], format);
            }
        require(matched, "Unknown depth format mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
