// Runtime-linked non-NR regressions. No settings writes or headset required.
#include "pch.h"
#include "runtime.h"
#include <iostream>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
    unsigned destroyCalls;
    void OVR_CDECL countDestroy(ovrSession, ovrTextureSwapChain) {
        ++destroyCalls;
    }
    decltype(&ovr_DestroyTextureSwapChain) originalDestroy = ovr_DestroyTextureSwapChain;
    decltype(&LoadLibraryExW) originalLoad = LoadLibraryExW;
    unsigned loadCalls;
    HMODULE WINAPI rejectLibrary(LPCWSTR, HANDLE, DWORD) {
        ++loadCalls;
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    void checkHr(HRESULT result) {
        require(SUCCEEDED(result), "D3D operation failed");
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void lifetime(const wchar_t* backendDirectory) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ovrInitParams init{};
            init.Flags = ovrInit_RequestVersion;
            init.RequestedMinorVersion = OVR_MINOR_VERSION;
            const auto directory = std::filesystem::absolute(backendDirectory).wstring() + L"\\";
            require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, directory.c_str())),
                    "OVRNull initialization failed");
            originalDestroy = reinterpret_cast<decltype(originalDestroy)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_DestroyTextureSwapChain"));
            require(originalDestroy != nullptr, "OVRNull destroy export missing");
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Detour thread failed");
            require(DetourAttach(reinterpret_cast<PVOID*>(&originalDestroy), countDestroy) == NO_ERROR,
                    "Detour attach failed");
            require(DetourTransactionCommit() == NO_ERROR, "Detour commit failed");
            unsigned failures = 0;
            for (unsigned mode = 0; mode < 3; ++mode) {
                auto* chain = new OpenXrRuntime::Swapchain{};
                chain->appSwapchain.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(0x100);
                if (mode) {
                    chain->resolvedSlices.resize(1);
                    chain->resolvedSlices[0].ovrSwapchain =
                        reinterpret_cast<ovrTextureSwapChain>(uintptr_t(mode == 1 ? 0x100 : 0x200));
                }
                const auto handle = reinterpret_cast<XrSwapchain>(chain);
                runtime.m_swapchains.insert(handle);
                destroyCalls = 0;
                require(runtime.xrDestroySwapchain(handle) == XR_SUCCESS, "DestroySwapchain failed");
                const unsigned expected = mode == 2 ? 2 : 1;
                if (destroyCalls != expected) {
                    ++failures;
                    std::cerr << "FAIL: destroy mode=" << mode << " calls=" << destroyCalls << " expected=" << expected
                              << '\n';
                }
            }
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Detour thread failed");
            require(DetourDetach(reinterpret_cast<PVOID*>(&originalDestroy), countDestroy) == NO_ERROR,
                    "Detour detach failed");
            require(DetourTransactionCommit() == NO_ERROR, "Detour commit failed");
            require(!failures, "An unenumerated swapchain was leaked or an aliased swapchain destroyed twice");
            std::cout << "PASS: empty, aliased and distinct swapchain ownership\n";
        }

        static void missingBackend() {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ovrInitParams init{};
            init.Flags = ovrInit_RequestVersion;
            init.RequestedMinorVersion = OVR_MINOR_VERSION;
            // Guaranteed missing child under this executable's directory, not a machine path.
            const auto directory = std::filesystem::current_path().wstring() + L"\\vdxr-regression-missing-backend\\";
            require(!std::filesystem::exists(directory), "Missing-backend fixture directory exists");
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Detour thread failed");
            require(DetourAttach(reinterpret_cast<PVOID*>(&originalLoad), rejectLibrary) == NO_ERROR,
                    "Detour attach failed");
            require(DetourTransactionCommit() == NO_ERROR, "Detour commit failed");
            const auto result = ovr_InitializeWithPathOverride(&init, directory.c_str());
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Detour thread failed");
            require(DetourDetach(reinterpret_cast<PVOID*>(&originalLoad), rejectLibrary) == NO_ERROR,
                    "Detour detach failed");
            require(DetourTransactionCommit() == NO_ERROR, "Detour commit failed");
            std::cout << "backend search attempts=" << loadCalls << '\n';
            require(OVR_FAILURE(result) && loadCalls == 1,
                    "Missing override must try only its initialized path and report failure");
            std::cout << "PASS: missing overridden backend fails without reading uninitialized paths\n";
        }

        static void ntRetry() {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            checkHr(D3D11CreateDevice(nullptr,
                                      D3D_DRIVER_TYPE_HARDWARE,
                                      nullptr,
                                      0,
                                      nullptr,
                                      0,
                                      D3D11_SDK_VERSION,
                                      &device,
                                      nullptr,
                                      nullptr));
            checkHr(device.As(&runtime.m_ovrSubmissionDevice));
            // Select existing ARC sharing policy without creating a native Vulkan device.
            runtime.m_gpuVendor = 0x8086;
            runtime.m_vkDevice = reinterpret_cast<VkDevice>(uintptr_t(1));
            auto owner = std::make_unique<OpenXrRuntime::Swapchain>();
            auto& chain = *owner;
            chain.ovrSwapchainLength = 1;
            chain.appSwapchain.images.resize(1);
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 32;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            checkHr(device->CreateTexture2D(&desc, nullptr, &chain.appSwapchain.images[0]));
            const auto first = runtime.getSwapchainImages(chain);
            const auto again = runtime.getSwapchainImages(chain);
            require(first.size() == 1 && again == first, "Repeated export must reuse owned NT handles");
            DWORD flags = 0;
            require(GetHandleInformation(first[0], &flags), "NT handle closed before imports completed");
            // Swapchain lifetime owns the handle; never manually close a borrowed import handle.
            owner.reset();
            require(!GetHandleInformation(first[0], &flags) && GetLastError() == ERROR_INVALID_HANDLE,
                    "NT export outlived its swapchain owner");
            runtime.m_vkDevice = VK_NULL_HANDLE;
            std::cout << "PASS: repeated NT-handle export reuses its handle and closes it with its owner\n";
        }

        static void fenceCleanup() {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            checkHr(D3D11CreateDevice(nullptr,
                                      D3D_DRIVER_TYPE_HARDWARE,
                                      nullptr,
                                      0,
                                      nullptr,
                                      0,
                                      D3D11_SDK_VERSION,
                                      &device,
                                      nullptr,
                                      &context));
            checkHr(device.As(&runtime.m_d3d11Device));
            checkHr(context.As(&runtime.m_d3d11Context));
            checkHr(runtime.m_d3d11Device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&runtime.m_d3d11Fence)));
            runtime.cleanupD3D11();
            require(!runtime.m_d3d11Device && !runtime.m_d3d11Context && !runtime.m_d3d11Fence,
                    "D3D11 cleanup retained an application fence and its device");
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "PASS: D3D11 cleanup releases its application fence after flushing\n";
        }

        static void alpha(bool gamma, bool array = false) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL level;
            checkHr(D3D11CreateDevice(nullptr,
                                      D3D_DRIVER_TYPE_HARDWARE,
                                      nullptr,
                                      0,
                                      nullptr,
                                      0,
                                      D3D11_SDK_VERSION,
                                      &device,
                                      &level,
                                      &context));
            checkHr(device.As(&runtime.m_ovrSubmissionDevice));
            checkHr(context.As(&runtime.m_ovrSubmissionContext));
            unsigned failures = 0;
            // Vulkan/GL entries inject declared metadata only, not native API execution.
            for (int64_t declared : {int64_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), int64_t(43), int64_t(0x8c43)}) {
                OpenXrRuntime::Swapchain chain{};
                chain.dirty = true;
                chain.xrDesc.format = declared;
                chain.dxgiFormatForSubmission = gamma ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
                chain.resolvedSlices.resize(1);
                auto& slice = chain.resolvedSlices[0];
                slice.lastCommittedIndex = 0;
                slice.images.resize(1);
                std::vector<uint8_t> pixels(64 * 64 * 4);
                for (size_t p = 0; p < pixels.size(); p += 4) {
                    pixels[p] = 153;
                    pixels[p + 1] = 102;
                    pixels[p + 2] = 51;
                    pixels[p + 3] = 128;
                }
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = desc.Height = 64;
                desc.MipLevels = desc.SampleDesc.Count = 1;
                desc.ArraySize = array ? 2 : 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
                desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                D3D11_SUBRESOURCE_DATA initial[2]{{pixels.data(), 64 * 4, 0}, {pixels.data(), 64 * 4, 0}};
                checkHr(device->CreateTexture2D(&desc, initial, &slice.images[0]));
                const XrRect2Di rect{{5, 7}, {33, 35}};
                const XrCompositionLayerFlags flags = gamma ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                                                  XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
                                                            : 0;
                runtime.preprocessSwapchainImage(chain, 1, 0, flags, rect);
                desc.BindFlags = 0;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
                context->CopyResource(staging.Get(), slice.images[0].Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                unsigned wrong = 0;
                for (unsigned y = 0; y < 64; ++y)
                    for (unsigned x = 0; x < 64; ++x) {
                        const bool inside = x >= 5 && x < 38 && y >= 7 && y < 42;
                        const auto* actual = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                        for (unsigned c = 0; c < 4; ++c) {
                            const auto original = pixels[(y * 64 + x) * 4 + c];
                            int expected = original;
                            if (inside && !gamma && c == 3)
                                expected = 255;
                            if (inside && gamma && c < 3) {
                                const double v = original / 255.;
                                const double linear = v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
                                const double multiplied = linear * (128. / 255.);
                                const double encoded = multiplied <= .0031308
                                                           ? 12.92 * multiplied
                                                           : 1.055 * std::pow(multiplied, 1. / 2.4) - .055;
                                expected = static_cast<int>(std::lround(encoded * 255));
                            }
                            if (std::abs(int(actual[c]) - expected) > (inside && gamma ? 3 : 0))
                                ++wrong;
                        }
                    }
                context->Unmap(staging.Get(), 0);
                if (array) {
                    checkHr(context->Map(staging.Get(), 1, D3D11_MAP_READ, 0, &mapped));
                    for (unsigned y = 0; y < 64; ++y)
                        for (unsigned x = 0; x < 64; ++x) {
                            const auto* actual =
                                static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                            for (unsigned c = 0; c < 4; ++c)
                                if (actual[c] != pixels[(y * 64 + x) * 4 + c])
                                    ++wrong;
                        }
                    context->Unmap(staging.Get(), 1);
                }
                checkHr(device->GetDeviceRemovedReason());
                if (wrong) {
                    ++failures;
                    std::cerr << "FAIL: " << (gamma ? "alpha gamma" : "alpha bounds") << " declared=" << declared
                              << " wrongChannels=" << wrong << '\n';
                }
            }
            require(!failures, "Alpha processing changed pixels outside viewport or used the wrong encoding");
            std::cout << "PASS: " << (gamma ? "mapped alpha encoding and crop isolation" : "alpha crop isolation")
                      << '\n';
        }
    };
} // namespace virtualdesktop_openxr
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 2, "usage: nonnr_compositor_regression lifetime OVRNull-directory | bounds | gamma");
        const std::wstring mode = argv[1];
        if (mode == L"lifetime") {
            require(argc == 3, "lifetime requires OVRNull directory");
            virtualdesktop_openxr::RuntimeInputRegression::lifetime(argv[2]);
        } else if (mode == L"bounds" || mode == L"gamma")
            virtualdesktop_openxr::RuntimeInputRegression::alpha(mode == L"gamma");
        else if (mode == L"array-alpha")
            virtualdesktop_openxr::RuntimeInputRegression::alpha(false, true);
        else if (mode == L"missing-backend")
            virtualdesktop_openxr::RuntimeInputRegression::missingBackend();
        else if (mode == L"nt-retry")
            virtualdesktop_openxr::RuntimeInputRegression::ntRetry();
        else if (mode == L"fence-cleanup")
            virtualdesktop_openxr::RuntimeInputRegression::fenceCleanup();
        else
            throw std::runtime_error("Unknown mode");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
