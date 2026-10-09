// MIT License
//
// Copyright(c) 2022-2024 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "log.h"
#include "runtime.h"
#include "utils.h"

#include "FullScreenQuadVS.h"
#include "ResolveMultisampledDepthPS.h"

// Implements native support to submit swapchains to OVR.
// Implements the necessary support for the XR_KHR_D3D11_enable extension:
// https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#XR_KHR_D3D11_enable

namespace virtualdesktop_openxr {

    using namespace virtualdesktop_openxr::log;
    using namespace virtualdesktop_openxr::utils;

    struct ResolveMultisampledDepthPSConstants {
        alignas(4) uint32_t slice;
    };

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetD3D11GraphicsRequirementsKHR
    XrResult OpenXrRuntime::xrGetD3D11GraphicsRequirementsKHR(XrInstance instance,
                                                              XrSystemId systemId,
                                                              XrGraphicsRequirementsD3D11KHR* graphicsRequirements) {
        if (graphicsRequirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetD3D11GraphicsRequirementsKHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"));

        if (!has_XR_KHR_D3D11_enable) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        CHECK_MSG(ensureOVRSession(), "Failed to re-create OVR session\n");

        memcpy(&graphicsRequirements->adapterLuid, &m_adapterLuid, sizeof(LUID));
        graphicsRequirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

        TraceLoggingWrite(g_traceProvider,
                          "xrGetD3D11GraphicsRequirementsKHR",
                          TraceLoggingCharArray((char*)&graphicsRequirements->adapterLuid, sizeof(LUID), "AdapterLuid"),
                          TLArg((int)graphicsRequirements->minFeatureLevel, "MinFeatureLevel"));

        m_graphicsRequirementQueried = true;

        return XR_SUCCESS;
    }

    // Initialize all the resources needed for D3D11 support, both on the API frontend and also the runtime/OVR backend.
    XrResult OpenXrRuntime::initializeD3D11(const XrGraphicsBindingD3D11KHR& d3dBindings) {
        if (!d3dBindings.device) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }

        // Check that this is the correct adapter for the HMD.
        ComPtr<IDXGIDevice> dxgiDevice;
        CHECK_HRCMD(d3dBindings.device->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));

        ComPtr<IDXGIAdapter> dxgiAdapter;
        CHECK_HRCMD(dxgiDevice->GetAdapter(dxgiAdapter.ReleaseAndGetAddressOf()));

        DXGI_ADAPTER_DESC desc;
        CHECK_HRCMD(dxgiAdapter->GetDesc(&desc));

        if (memcmp(&desc.AdapterLuid, &m_adapterLuid, sizeof(LUID))) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }

        // Query the necessary flavors of device & device context which will let use use fences.
        CHECK_HRCMD(d3dBindings.device->QueryInterface(m_d3d11Device.ReleaseAndGetAddressOf()));
        ComPtr<ID3D11DeviceContext> deviceContext;
        d3dBindings.device->GetImmediateContext(deviceContext.ReleaseAndGetAddressOf());
        CHECK_HRCMD(deviceContext->QueryInterface(m_d3d11Context.ReleaseAndGetAddressOf()));

        if (m_useApplicationDeviceForSubmission) {
            // Try reusing the application device to avoid fence synchronization every frame.
            const std::string deviceName = xr::wide_to_utf8(desc.Description);
            TraceLoggingWrite(
                g_traceProvider, "xrCreateSession", TLArg("D3D11", "Api"), TLArg(deviceName.c_str(), "AdapterName"));
            Log("Using D3D11 on adapter: %s\n", deviceName.c_str());

            m_ovrSubmissionDevice = m_d3d11Device;
            m_ovrSubmissionContext = m_d3d11Context;

            UINT creationFlags = 0;
            if (m_ovrSubmissionDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) {
                creationFlags |= D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED;
            }
            const D3D_FEATURE_LEVEL featureLevel = m_ovrSubmissionDevice->GetFeatureLevel();

            CHECK_HRCMD(
                m_ovrSubmissionDevice->CreateDeviceContextState(creationFlags,
                                                                &featureLevel,
                                                                1,
                                                                D3D11_SDK_VERSION,
                                                                __uuidof(ID3D11Device),
                                                                nullptr,
                                                                m_ovrSubmissionContextState.ReleaseAndGetAddressOf()));

            initializeSubmissionResources();
        } else {
            // Create the resources that OVR will be using.
            initializeSubmissionDevice("D3D11");
        }

        // We will use a shared fence to synchronize between the application context and the OVR (submission) context
        // context.
        wil::unique_handle fenceHandle;
        CHECK_HRCMD(m_ovrSubmissionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, fenceHandle.put()));
        CHECK_HRCMD(
            m_d3d11Device->OpenSharedFence(fenceHandle.get(), IID_PPV_ARGS(m_d3d11Fence.ReleaseAndGetAddressOf())));
        *m_eventForSubmissionFence.put() = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        CHECK_MSG(m_eventForSubmissionFence.get(), "Failed to create submission event");

        // Frame timers.
        for (uint32_t i = 0; i < k_numGpuTimers; i++) {
            m_gpuTimerApp[i] = std::make_unique<D3D11GpuTimer>(m_d3d11Device.Get(), m_d3d11Context.Get());
        }

        return XR_SUCCESS;
    }

    // Initialize all the resources for the OVR backend.
    void OpenXrRuntime::initializeSubmissionDevice(const std::string& appGraphicsApi) {
        ComPtr<IDXGIFactory1> dxgiFactory;
        CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));

        ComPtr<IDXGIAdapter1> dxgiAdapter;
        for (UINT adapterIndex = 0;; adapterIndex++) {
            // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to
            // enumerate.
            CHECK_HRCMD(dxgiFactory->EnumAdapters1(adapterIndex, dxgiAdapter.ReleaseAndGetAddressOf()));

            DXGI_ADAPTER_DESC1 desc;
            CHECK_HRCMD(dxgiAdapter->GetDesc1(&desc));
            if (!memcmp(&desc.AdapterLuid, &m_adapterLuid, sizeof(LUID))) {
                const std::string deviceName = xr::wide_to_utf8(desc.Description);

                TraceLoggingWrite(g_traceProvider,
                                  "xrCreateSession",
                                  TLArg(appGraphicsApi.c_str(), "Api"),
                                  TLArg(deviceName.c_str(), "AdapterName"));
                Log("Using %s on adapter: %s\n", appGraphicsApi.c_str(), deviceName.c_str());

                m_gpuVendor = desc.VendorId;

                break;
            }
        }

        // Create the submission device that OVR will be using.
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> deviceContext;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        CHECK_HRCMD(D3D11CreateDevice(dxgiAdapter.Get(),
                                      D3D_DRIVER_TYPE_UNKNOWN,
                                      0,
                                      flags,
                                      &featureLevel,
                                      1,
                                      D3D11_SDK_VERSION,
                                      device.ReleaseAndGetAddressOf(),
                                      nullptr,
                                      deviceContext.ReleaseAndGetAddressOf()));

        // Query the necessary flavors of device & device context, which will let use use fences.
        CHECK_HRCMD(device->QueryInterface(m_ovrSubmissionDevice.ReleaseAndGetAddressOf()));
        CHECK_HRCMD(deviceContext->QueryInterface(m_ovrSubmissionContext.ReleaseAndGetAddressOf()));

        initializeSubmissionResources();
    }

    void OpenXrRuntime::initializeSubmissionResources() {
        // Create the synchronization fence to serialize work between the application device and submission device.
        CHECK_HRCMD(m_ovrSubmissionDevice->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(m_ovrSubmissionFence.ReleaseAndGetAddressOf())));
        m_fenceValue = 0;
        CHECK_HRCMD(m_ovrSubmissionDevice->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(m_ovrSubmissionCompletionFence.ReleaseAndGetAddressOf())));
        m_submissionFenceValue = 0;

        // Create the resources for pre-processing.
        CHECK_HRCMD(m_ovrSubmissionDevice->CreateVertexShader(
            g_FullScreenQuadVS, sizeof(g_FullScreenQuadVS), nullptr, m_fullQuadVS.ReleaseAndGetAddressOf()));
        setDebugName(m_fullQuadVS.Get(), "FullQuad VS");
        CHECK_HRCMD(m_ovrSubmissionDevice->CreatePixelShader(g_ResolveMultisampledDepthPS,
                                                             sizeof(g_ResolveMultisampledDepthPS),
                                                             nullptr,
                                                             m_resolveMultisampledDepthPS.ReleaseAndGetAddressOf()));
        setDebugName(m_resolveMultisampledDepthPS.Get(), "Resolve MSAA Depth PS");

        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.MaxAnisotropy = 1;
            desc.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
            desc.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
            CHECK_HRCMD(
                m_ovrSubmissionDevice->CreateSamplerState(&desc, m_linearClampSampler.ReleaseAndGetAddressOf()));
            setDebugName(m_linearClampSampler.Get(), "Linear Sampler");
        }
        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.MaxAnisotropy = 1;
            desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            desc.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
            desc.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
            CHECK_HRCMD(m_ovrSubmissionDevice->CreateSamplerState(&desc, m_pointClampSampler.ReleaseAndGetAddressOf()));
            setDebugName(m_pointClampSampler.Get(), "Point Sampler");
        }
        {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = TRUE;
            desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            CHECK_HRCMD(
                m_ovrSubmissionDevice->CreateDepthStencilState(&desc, m_noDepthReadState.ReleaseAndGetAddressOf()));
            setDebugName(m_noDepthReadState.Get(), "No Depth Test State");
        }
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = ((sizeof(ResolveMultisampledDepthPSConstants) + 15) / 16) * 16;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            CHECK_HRCMD(m_ovrSubmissionDevice->CreateBuffer(
                &desc, nullptr, m_resolveMultisampledDepthConstants.ReleaseAndGetAddressOf()));
            setDebugName(m_resolveMultisampledDepthConstants.Get(), "Resolve MSAA Depth Constants");
        }
        for (uint32_t i = 0; i < k_numGpuTimers; i++) {
            m_gpuTimerPrecomposition[i] =
                std::make_unique<D3D11GpuTimer>(m_ovrSubmissionDevice.Get(), m_ovrSubmissionContext.Get());
        }

        // If RenderDoc is loaded, then create a DXGI swapchain to signal events. Otherwise RenderDoc will
        // not see our OpenXR frames.
        HMODULE renderdocModule;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, "renderdoc.dll", &renderdocModule) &&
            renderdocModule) {
            TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLArg("True", "RenderDoc"));
            Log("Detected RenderDoc\n");

            DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
            swapchainDesc.Width = 8;
            swapchainDesc.Height = 8;
            swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapchainDesc.SampleDesc.Count = 1;
            swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapchainDesc.BufferCount = 3;
            swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

            ComPtr<IDXGIDevice> dxgiDevice;
            CHECK_HRCMD(m_ovrSubmissionDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));

            ComPtr<IDXGIAdapter> dxgiAdapter;
            CHECK_HRCMD(dxgiDevice->GetAdapter(dxgiAdapter.ReleaseAndGetAddressOf()));

            ComPtr<IDXGIFactory2> dxgiFactory;
            CHECK_HRCMD(dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));
            CHECK_HRCMD(dxgiFactory->CreateSwapChainForComposition(
                dxgiDevice.Get(), &swapchainDesc, nullptr, m_dxgiSwapchain.ReleaseAndGetAddressOf()));
        }
    }

    void OpenXrRuntime::cleanupD3D11() {
        flushD3D11Context();

        for (uint32_t i = 0; i < k_numGpuTimers; i++) {
            m_gpuTimerApp[i].reset();
        }

        m_d3d11ContextState.Reset();
        m_d3d11Context.Reset();
        m_d3d11Fence.Reset();
        m_d3d11Device.Reset();
    }

    void OpenXrRuntime::cleanupSubmissionDevice() {
        flushSubmissionContext();

        for (uint32_t i = 0; i < k_numGpuTimers; i++) {
            m_gpuTimerPrecomposition[i].reset();
        }

        m_dxgiSwapchain.Reset();
        m_fullQuadVS.Reset();
        m_resolveMultisampledDepthPS.Reset();
        m_resolveMultisampledDepthConstants.Reset();
        m_alphaCorrectShader.Reset();
        m_alphaCorrectConstants.Reset();
        m_sharpenShader.Reset();
        m_upscaleShader.Reset();
        m_alignDepthShader.Reset();
        m_alignDepthConstants.Reset();
        m_upscalerConstants.Reset();
        m_linearClampSampler.Reset();
        m_pointClampSampler.Reset();
        m_noDepthReadState.Reset();

        m_ovrSubmissionFence.Reset();
        m_ovrSubmissionCompletionFence.Reset();
        m_submissionFenceValue = 0;
        m_ovrSubmissionContextState.Reset();
        m_ovrSubmissionContext.Reset();
        m_ovrSubmissionDevice.Reset();
        m_eventForSubmissionFence.reset();
    }

    // Submission can use released images before the application ever exports them.
    void OpenXrRuntime::ensureAppSwapchainImages(Swapchain& xrSwapchain) {
        if (!xrSwapchain.appSwapchain.images.empty()) {
            return;
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        if (!xrSwapchain.appSwapchain.ovrSwapchain) {
            textureDesc.Format = getTypelessFormat(xrSwapchain.dxgiFormatForSubmission);
            textureDesc.Width = xrSwapchain.ovrDesc.Width;
            textureDesc.Height = xrSwapchain.ovrDesc.Height;
            textureDesc.ArraySize = (xrSwapchain.ovrDesc.Type != ovrTexture_Cube) ? xrSwapchain.ovrDesc.ArraySize : 6;
            textureDesc.MipLevels = xrSwapchain.ovrDesc.MipLevels;
            textureDesc.SampleDesc.Count = xrSwapchain.ovrDesc.SampleCount;

            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (xrSwapchain.ovrDesc.BindFlags & ovrTextureBind_DX_RenderTarget) {
                textureDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
            }
            if (xrSwapchain.ovrDesc.BindFlags & ovrTextureBind_DX_UnorderedAccess) {
                textureDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            }
            if (xrSwapchain.ovrDesc.BindFlags & ovrTextureBind_DX_DepthStencil) {
                textureDesc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
            }

            textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            if (requireNTHandleSharing()) {
                textureDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            }
            if (xrSwapchain.ovrDesc.Type == ovrTexture_Cube) {
                textureDesc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
            }
            if (xrSwapchain.ovrDesc.MiscFlags & ovrTextureMisc_AllowGenerateMips) {
                textureDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
            }
        }

        // Query the textures for the swapchain.
        std::vector<ComPtr<ID3D11Texture2D>> images;
        for (int i = 0; i < xrSwapchain.ovrSwapchainLength; i++) {
            {
                ComPtr<ID3D11Texture2D> swapchainTexture;
                if (xrSwapchain.appSwapchain.ovrSwapchain) {
                    CHECK_OVRCMD(
                        ovr_GetTextureSwapChainBufferDX(m_ovrSession,
                                                        xrSwapchain.appSwapchain.ovrSwapchain,
                                                        i,
                                                        IID_PPV_ARGS(swapchainTexture.ReleaseAndGetAddressOf())));
                } else {
                    CHECK_HRCMD(m_ovrSubmissionDevice->CreateTexture2D(
                        &textureDesc, nullptr, swapchainTexture.ReleaseAndGetAddressOf()));
                }
                setDebugName(swapchainTexture.Get(),
                             fmt::format("OVR Swapchain Texture[{}, {}]", i, (void*)&xrSwapchain));

                images.push_back(swapchainTexture);
                if (i == 0) {
                    D3D11_TEXTURE2D_DESC desc;
                    swapchainTexture->GetDesc(&desc);
                    TraceLoggingWrite(g_traceProvider,
                                      "xrEnumerateSwapchainImages",
                                      TLArg("D3D11", "Api"),
                                      TLArg("OVR", "Type"),
                                      TLArg(desc.Width, "Width"),
                                      TLArg(desc.Height, "Height"),
                                      TLArg(desc.ArraySize, "ArraySize"),
                                      TLArg(desc.MipLevels, "MipCount"),
                                      TLArg(desc.SampleDesc.Count, "SampleCount"),
                                      TLArg((int)desc.Format, "Format"),
                                      TLArg((int)desc.Usage, "Usage"),
                                      TLArg(desc.BindFlags, "BindFlags"),
                                      TLArg(desc.CPUAccessFlags, "CPUAccessFlags"),
                                      TLArg(desc.MiscFlags, "MiscFlags"));
                }
            }
        }
        xrSwapchain.appSwapchain.images = std::move(images);
    }

    // Retrieve generic handles to the swapchain images to import into the application device.
    std::vector<HANDLE> OpenXrRuntime::getSwapchainImages(Swapchain& xrSwapchain) {
        ensureAppSwapchainImages(xrSwapchain);
        std::vector<HANDLE> handles;
        if (requireNTHandleSharing()) {
            xrSwapchain.ntTextureHandles.resize(xrSwapchain.ovrSwapchainLength);
        }
        for (int i = 0; i < xrSwapchain.ovrSwapchainLength; i++) {
            // Export the HANDLE.
            const auto texture = xrSwapchain.appSwapchain.images[i];

            ComPtr<IDXGIResource1> dxgiResource;
            CHECK_HRCMD(texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf())));

            HANDLE textureHandle;
            if (!requireNTHandleSharing()) {
                CHECK_HRCMD(dxgiResource->GetSharedHandle(&textureHandle));
            } else {
                auto& ownedHandle = xrSwapchain.ntTextureHandles[i];
                if (!ownedHandle) {
                    CHECK_HRCMD(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ownedHandle.put()));
                }
                textureHandle = ownedHandle.get();
            }

            handles.push_back(textureHandle);
        }

        return handles;
    }

    // Retrieve the swapchain images (ID3D11Texture2D) for the application to use.
    XrResult OpenXrRuntime::getSwapchainImagesD3D11(Swapchain& xrSwapchain,
                                                    XrSwapchainImageD3D11KHR* d3d11Images,
                                                    uint32_t count) {
        // Detect whether this is the first call for this swapchain.
        const bool initialized = !xrSwapchain.d3d11Images.empty();
        bool imported = initialized;
        auto rollback = MakeScopeGuard([&] {
            if (!imported) {
                xrSwapchain.d3d11Images.clear();
            }
        });
        for (uint32_t i = 0; i < count; i++) {
            if (d3d11Images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
        }
        xrSwapchain.d3d11Images.reserve(count);
        const bool skipSharing = m_ovrSubmissionDevice == m_d3d11Device;

        std::vector<HANDLE> textureHandles;
        if (!initialized) {
            // Query the swapchain textures.
            textureHandles = getSwapchainImages(xrSwapchain);
        }

        // Export each D3D11 texture from the submission device into the application device.
        for (uint32_t i = 0; i < count; i++) {
            if (d3d11Images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            if (!initialized) {
                ComPtr<ID3D11Texture2D> d3d11Texture;
                if (!skipSharing) {
                    // Create an imported texture on the application device.
                    if (!requireNTHandleSharing()) {
                        CHECK_HRCMD(m_d3d11Device->OpenSharedResource(
                            textureHandles[i], IID_PPV_ARGS(d3d11Texture.ReleaseAndGetAddressOf())));
                    } else {
                        CHECK_HRCMD(m_d3d11Device->OpenSharedResource1(
                            textureHandles[i], IID_PPV_ARGS(d3d11Texture.ReleaseAndGetAddressOf())));
                    }
                } else {
                    d3d11Texture = xrSwapchain.appSwapchain.images[i];
                }

                setDebugName(d3d11Texture.Get(), fmt::format("App Swapchain Texture[{}, {}]", i, (void*)&xrSwapchain));

                xrSwapchain.d3d11Images.push_back(std::move(d3d11Texture));
            }

            d3d11Images[i].texture = xrSwapchain.d3d11Images[i].Get();

            if (i == 0) {
                D3D11_TEXTURE2D_DESC desc;
                d3d11Images[0].texture->GetDesc(&desc);
                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateSwapchainImages",
                                  TLArg("D3D11", "Api"),
                                  TLArg("Runtime", "Type"),
                                  TLArg(desc.Width, "Width"),
                                  TLArg(desc.Height, "Height"),
                                  TLArg(desc.ArraySize, "ArraySize"),
                                  TLArg(desc.MipLevels, "MipCount"),
                                  TLArg(desc.SampleDesc.Count, "SampleCount"),
                                  TLArg((int)desc.Format, "Format"),
                                  TLArg((int)desc.Usage, "Usage"),
                                  TLArg(desc.BindFlags, "BindFlags"),
                                  TLArg(desc.CPUAccessFlags, "CPUAccessFlags"),
                                  TLArg(desc.MiscFlags, "MiscFlags"));
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateSwapchainImages",
                              TLArg("D3D11", "Api"),
                              TLPArg(d3d11Images[i].texture, "Texture"));
        }

        imported = true;
        return XR_SUCCESS;
    }

    // Prepare a swapchain to be used by OVR.
    void OpenXrRuntime::resolveSwapchainImage(Swapchain& xrSwapchain,
                                              uint32_t slice,
                                              std::set<std::pair<Swapchain*, uint32_t>>& resolved,
                                              bool skipCommit) {
        ensureAppSwapchainImages(xrSwapchain);
        ensureSwapchainSliceResources(xrSwapchain, slice);

        const auto tuple = std::make_pair(&xrSwapchain, slice);
        const bool needCopy = (slice > 0 || !xrSwapchain.appSwapchain.ovrSwapchain);
        if (resolved.count(tuple)) {
            if (skipCommit || !m_precompositor.pendingSwapchainCommits.count(tuple)) {
                return;
            }
            if (needCopy) {
                // A previous processed layer wrote this image but did not submit it.
                // Commit that same destination without advancing and recopying it.
                CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
                m_precompositor.pendingSwapchainCommits.erase(tuple);
                return;
            }
            // Direct swapchains still need the existing released-index synchronization.
        }

        const int lastReleasedIndex = xrSwapchain.lastReleasedIndex;

        TraceLoggingWrite(g_traceProvider,
                          "ResolveSwapchainImage",
                          TLArg(lastReleasedIndex, "LastReleasedIndex"),
                          TLArg(slice, "Slice"),
                          TLArg(needCopy, "NeedCopy"),
                          TLArg(skipCommit, "SkipCommit"));

        int ovrDestIndex = -1;
        while (true) {
            CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(
                m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain, &ovrDestIndex));

            // If we can use the swapchain with LibOVR directly (without a copy), then let's commit to the swapchain
            // until the last committed image matches the last released image index.
            int ovrCommittedIndex = ovrDestIndex - 1;
            if (ovrCommittedIndex < 0) {
                ovrCommittedIndex = xrSwapchain.ovrSwapchainLength - 1;
            }
            if (needCopy || skipCommit) {
                TraceLoggingWrite(g_traceProvider, "ResolveSwapchainImage", TLArg(ovrDestIndex, "DestIndex"));
                // lastCommittedIndex must be set below.
                break;
            }
            TraceLoggingWrite(
                g_traceProvider, "ResolveSwapchainImage_SyncImage", TLArg(ovrCommittedIndex, "CommittedIndex"));
            if (ovrCommittedIndex == lastReleasedIndex) {
                // We still need to commit a static swapchain once!
                if (xrSwapchain.ovrSwapchainLength == 1 && xrSwapchain.dirty) {
                    CHECK_OVRCMD(
                        ovr_CommitTextureSwapChain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
                }
                xrSwapchain.resolvedSlices[slice].lastCommittedIndex = ovrCommittedIndex;
                break;
            }
            CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
        }

        if (needCopy) {
            xrSwapchain.resolvedSlices[slice].hasCopyForPreprocessing = true;
            xrSwapchain.resolvedSlices[slice].alphaCorrections.clear();
            const bool isDepthBuffer =
                (xrSwapchain.xrDesc.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            TraceLoggingWrite(g_traceProvider,
                              "ResolveSwapchainImage_Copy",
                              TLArg(xrSwapchain.ovrDesc.SampleCount == 1 ? "None"
                                    : !isDepthBuffer                     ? "Color"
                                                                         : "Depth",
                                    "Resolve"));

            // Circumvent some of OVR's limitations:
            // - For texture arrays, we must do a copy to slice 0 into another swapchain.
            // - For MSAA, we must resolve into a non-MSAA swapchain.
            if (xrSwapchain.ovrDesc.SampleCount == 1) {
                m_ovrSubmissionContext->CopySubresourceRegion(
                    xrSwapchain.resolvedSlices[slice].images[ovrDestIndex].Get(),
                    0,
                    0,
                    0,
                    0,
                    xrSwapchain.appSwapchain.images[lastReleasedIndex].Get(),
                    D3D11CalcSubresource(0, slice, xrSwapchain.xrDesc.mipCount),
                    nullptr);
            } else {
                // Resolve MSAA. For depth buffers, this requires a shader.
                if (!isDepthBuffer) {
                    m_ovrSubmissionContext->ResolveSubresource(
                        xrSwapchain.resolvedSlices[slice].images[ovrDestIndex].Get(),
                        0,
                        xrSwapchain.appSwapchain.images[lastReleasedIndex].Get(),
                        D3D11CalcSubresource(0, slice, xrSwapchain.xrDesc.mipCount),
                        xrSwapchain.dxgiFormatForSubmission);
                } else {
                    // We are about to do something destructive to the application context. Save the context. It will be
                    // restored at the end of xrEndFrame().
                    if (m_d3d11Device == m_ovrSubmissionDevice && !m_d3d11ContextState) {
                        m_ovrSubmissionContext->SwapDeviceContextState(m_ovrSubmissionContextState.Get(),
                                                                       m_d3d11ContextState.ReleaseAndGetAddressOf());
                    }

                    if ((int)xrSwapchain.appSwapchain.srvs.size() <= lastReleasedIndex) {
                        xrSwapchain.appSwapchain.srvs.resize(lastReleasedIndex + 1);
                    }
                    if (!xrSwapchain.appSwapchain.srvs[lastReleasedIndex]) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                        desc.Format = getShaderResourceViewFormat(xrSwapchain.dxgiFormatForSubmission);
                        desc.Texture2DMSArray.ArraySize = xrSwapchain.ovrDesc.ArraySize;
                        CHECK_HRCMD(m_ovrSubmissionDevice->CreateShaderResourceView(
                            xrSwapchain.appSwapchain.images[lastReleasedIndex].Get(),
                            &desc,
                            xrSwapchain.appSwapchain.srvs[lastReleasedIndex].ReleaseAndGetAddressOf()));
                        setDebugName(
                            xrSwapchain.appSwapchain.srvs[lastReleasedIndex].Get(),
                            fmt::format(
                                "Runtime Slice SRV[{}, {}, {}]", slice, lastReleasedIndex, (void*)&xrSwapchain));
                    }
                    if ((int)xrSwapchain.resolvedSlices[slice].dsvs.size() <= ovrDestIndex) {
                        xrSwapchain.resolvedSlices[slice].dsvs.resize(ovrDestIndex + 1);
                    }
                    if (!xrSwapchain.resolvedSlices[slice].dsvs[ovrDestIndex]) {
                        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                        desc.Format = xrSwapchain.dxgiFormatForSubmission;
                        CHECK_HRCMD(m_ovrSubmissionDevice->CreateDepthStencilView(
                            xrSwapchain.resolvedSlices[slice].images[ovrDestIndex].Get(),
                            &desc,
                            xrSwapchain.resolvedSlices[slice].dsvs[ovrDestIndex].ReleaseAndGetAddressOf()));
                        setDebugName(
                            xrSwapchain.resolvedSlices[slice].dsvs[ovrDestIndex].Get(),
                            fmt::format("Runtime Slice DSV[{}, {}, {}]", slice, ovrDestIndex, (void*)&xrSwapchain));
                    }

                    m_ovrSubmissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

                    m_ovrSubmissionContext->VSSetShader(m_fullQuadVS.Get(), nullptr, 0);
                    m_ovrSubmissionContext->PSSetShader(m_resolveMultisampledDepthPS.Get(), nullptr, 0);

                    m_ovrSubmissionContext->OMSetRenderTargets(
                        0, nullptr, xrSwapchain.resolvedSlices[slice].dsvs[ovrDestIndex].Get());
                    D3D11_VIEWPORT viewport{};
                    viewport.Width = (float)xrSwapchain.ovrDesc.Width;
                    viewport.Height = (float)xrSwapchain.ovrDesc.Height;
                    viewport.MaxDepth = 1.f;
                    m_ovrSubmissionContext->RSSetViewports(1, &viewport);
                    m_ovrSubmissionContext->OMSetDepthStencilState(m_noDepthReadState.Get(), 0xff);
                    {
                        ResolveMultisampledDepthPSConstants constants{};
                        constants.slice = slice;

                        D3D11_MAPPED_SUBRESOURCE mappedResources;
                        CHECK_HRCMD(m_ovrSubmissionContext->Map(m_resolveMultisampledDepthConstants.Get(),
                                                                0,
                                                                D3D11_MAP_WRITE_DISCARD,
                                                                0,
                                                                &mappedResources));
                        memcpy(mappedResources.pData, &constants, sizeof(constants));
                        m_ovrSubmissionContext->Unmap(m_resolveMultisampledDepthConstants.Get(), 0);
                        m_ovrSubmissionContext->PSSetConstantBuffers(
                            0, 1, m_resolveMultisampledDepthConstants.GetAddressOf());
                    }
                    ID3D11SamplerState* sampler[] = {m_pointClampSampler.Get()};
                    m_ovrSubmissionContext->PSSetSamplers(0, 1, sampler);
                    ID3D11ShaderResourceView* SRV[] = {xrSwapchain.appSwapchain.srvs[lastReleasedIndex].Get()};
                    m_ovrSubmissionContext->PSSetShaderResources(0, 1, SRV);

                    m_ovrSubmissionContext->Draw(3, 0);

                    // Unbind all resources to avoid D3D validation errors.
                    {
                        m_ovrSubmissionContext->OMSetRenderTargets(0, nullptr, nullptr);
                        m_ovrSubmissionContext->VSSetShader(nullptr, nullptr, 0);
                        m_ovrSubmissionContext->PSSetShader(nullptr, nullptr, 0);
                        ID3D11Buffer* nullCBV[] = {nullptr};
                        m_ovrSubmissionContext->PSSetConstantBuffers(0, 1, nullCBV);
                        ID3D11SamplerState* nullSampler[] = {nullptr};
                        m_ovrSubmissionContext->PSSetSamplers(0, 1, nullSampler);
                        ID3D11ShaderResourceView* nullSRV[] = {nullptr};
                        m_ovrSubmissionContext->PSSetShaderResources(0, 1, nullSRV);
                    }
                }
            }

            if (!skipCommit) {
                CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
            }
            xrSwapchain.resolvedSlices[slice].lastCommittedIndex = ovrDestIndex;
        } else if (skipCommit) {
            xrSwapchain.resolvedSlices[slice].lastCommittedIndex = lastReleasedIndex;
        }

        resolved.insert(tuple);
        if (skipCommit) {
            m_precompositor.pendingSwapchainCommits.insert(tuple);
        } else {
            m_precompositor.pendingSwapchainCommits.erase(tuple);
        }
    }

    // Ensure necessary resources for submission: lazily create a second swapchain for this slice of the array or
    // when resolving MSAA.
    void OpenXrRuntime::ensureSwapchainSliceResources(Swapchain& xrSwapchain, uint32_t slice) const {
        if (xrSwapchain.resolvedSlices.size() <= slice) {
            xrSwapchain.resolvedSlices.resize(slice + 1);
        }
        if (!xrSwapchain.resolvedSlices[slice].ovrSwapchain) {
            if (slice == 0 && xrSwapchain.appSwapchain.ovrSwapchain) {
                SwapchainSlice candidate = xrSwapchain.appSwapchain;
                xrSwapchain.resolvedSlices[slice] = std::move(candidate);
                return;
            }
            auto desc = xrSwapchain.ovrDesc;
            // Resolve multisampling.
            desc.SampleCount = 1;
            // No need for arrays.
            desc.ArraySize = 1;
            populateSwapchainSlice(xrSwapchain, desc, xrSwapchain.resolvedSlices[slice], slice, "Runtime Slice");
        }
    }

    void OpenXrRuntime::ensureSwapchainPrecompositorResources(Swapchain& xrSwapchain, const ovrSizei& resolution) {
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            ovrSizei currentResolution{};
            if (xrSwapchain.stereoProjection[eye].ovrSwapchain) {
                ovrTextureSwapChainDesc desc{};
                CHECK_OVRCMD(
                    ovr_GetTextureSwapChainDesc(m_ovrSession, xrSwapchain.stereoProjection[eye].ovrSwapchain, &desc));
                currentResolution.w = desc.Width;
                currentResolution.h = desc.Height;
            }

            if (!xrSwapchain.stereoProjection[eye].ovrSwapchain || currentResolution.w != resolution.w ||
                currentResolution.h != resolution.h) {
                SwapchainSlice candidate;
                auto releaseCandidate = MakeScopeGuard([&] {
                    if (candidate.ovrSwapchain) {
                        ovr_DestroyTextureSwapChain(m_ovrSession, candidate.ovrSwapchain);
                    }
                });

                DXGI_FORMAT format;
                {
                    ovrTextureSwapChainDesc desc{};
                    desc.Type = ovrTexture_2D;
                    desc.ArraySize = 1;
                    desc.Width = resolution.w;
                    desc.Height = resolution.h;
                    desc.MipLevels = 1;
                    desc.SampleCount = 1;
                    if (isSRGBFormat(xrSwapchain.dxgiFormatForSubmission)) {
                        desc.Format = OVR_FORMAT_B8G8R8A8_UNORM_SRGB;
                        format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                    } else {
                        desc.Format = OVR_FORMAT_B8G8R8A8_UNORM;
                        format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    }
                    desc.BindFlags = ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess;
                    desc.MiscFlags = ovrTextureMisc_DX_Typeless;
                    populateSwapchainSlice(xrSwapchain, desc, candidate, eye, "Precompositor");
                }

                for (uint32_t i = 0; i < candidate.images.size(); i++) {
                    {
                        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                        desc.Format = getUnorderedAccessViewFormat(format);
                        ComPtr<ID3D11UnorderedAccessView> uav;
                        CHECK_HRCMD(m_ovrSubmissionDevice->CreateUnorderedAccessView(
                            candidate.images[i].Get(), &desc, uav.ReleaseAndGetAddressOf()));
                        setDebugName(uav.Get(),
                                     fmt::format("Precompositor UAV [{}, {}, {}]", eye, i, (void*)&xrSwapchain));
                        candidate.uavs.push_back(std::move(uav));
                    }
                }
                if (xrSwapchain.stereoProjection[eye].ovrSwapchain) {
                    // Retire queued reads before replacing this generation of output resources.
                    flushSubmissionContext();
                    ovr_DestroyTextureSwapChain(m_ovrSession, xrSwapchain.stereoProjection[eye].ovrSwapchain);
                }
                xrSwapchain.stereoProjection[eye] = std::move(candidate);
                releaseCandidate.Deactivate();
            }
        }
    }

    void OpenXrRuntime::populateSwapchainSlice(const Swapchain& xrSwapchain,
                                               const ovrTextureSwapChainDesc& desc,
                                               SwapchainSlice& slice,
                                               uint32_t sliceIndex,
                                               const char* debugName) const {
        SwapchainSlice candidate;
        auto releaseCandidate = MakeScopeGuard([&] {
            if (candidate.ovrSwapchain) {
                ovr_DestroyTextureSwapChain(m_ovrSession, candidate.ovrSwapchain);
            }
        });
        CHECK_OVRCMD(
            ovr_CreateTextureSwapChainDX(m_ovrSession, m_ovrSubmissionDevice.Get(), &desc, &candidate.ovrSwapchain));

        int count = -1;
        CHECK_OVRCMD(ovr_GetTextureSwapChainLength(m_ovrSession, candidate.ovrSwapchain, &count));
        if (count != xrSwapchain.ovrSwapchainLength) {
            throw std::runtime_error("Swapchain image count mismatch");
        }

        // Query the textures for the swapchain.
        candidate.images.reserve(count);
        for (int i = 0; i < count; i++) {
            ComPtr<ID3D11Texture2D> texture;
            CHECK_OVRCMD(ovr_GetTextureSwapChainBufferDX(
                m_ovrSession, candidate.ovrSwapchain, i, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
            setDebugName(
                texture.Get(),
                fmt::format(std::string(debugName) + " Texture[{}, {}, {}]", sliceIndex, i, (void*)&xrSwapchain));

            candidate.images.push_back(std::move(texture));
        }
        slice = std::move(candidate);
        releaseCandidate.Deactivate();
    }

    // Flush any pending work in the app context.
    void OpenXrRuntime::flushD3D11Context() {
        if (m_d3d11Context && m_d3d11Fence) {
            wil::unique_handle eventHandle;
            m_fenceValue++;
            TraceLoggingWrite(
                g_traceProvider, "FlushContext_Wait", TLArg("D3D11", "Api"), TLArg(m_fenceValue, "FenceValue"));
            CHECK_HRCMD(m_d3d11Context->Signal(m_d3d11Fence.Get(), m_fenceValue));
            *eventHandle.put() = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            CHECK_MSG(eventHandle.get(), "Failed to create flush event");
            CHECK_HRCMD(m_d3d11Fence->SetEventOnCompletion(m_fenceValue, eventHandle.get()));
            WaitForSingleObject(eventHandle.get(), INFINITE);
            ResetEvent(eventHandle.get());
        }
    }

    // Flush any pending work in the submission context.
    void OpenXrRuntime::flushSubmissionContext() {
        if (m_ovrSubmissionContext && m_ovrSubmissionCompletionFence) {
            wil::unique_handle eventHandle;
            m_submissionFenceValue++;
            TraceLoggingWrite(g_traceProvider,
                              "FlushContext_Wait",
                              TLArg("D3D11 Submission", "Api"),
                              TLArg(m_submissionFenceValue, "FenceValue"));
            CHECK_HRCMD(m_ovrSubmissionContext->Signal(m_ovrSubmissionCompletionFence.Get(), m_submissionFenceValue));
            *eventHandle.put() = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            CHECK_MSG(eventHandle.get(), "Failed to create flush event");
            CHECK_HRCMD(
                m_ovrSubmissionCompletionFence->SetEventOnCompletion(m_submissionFenceValue, eventHandle.get()));
            WaitForSingleObject(eventHandle.get(), INFINITE);
            ResetEvent(eventHandle.get());
        }
    }

    // Serialize commands from the D3D12 queue to the D3D11 context used by OVR.
    void OpenXrRuntime::serializeD3D11Frame() {
        if (m_ovrSubmissionDevice != m_d3d11Device) {
            m_fenceValue++;
            TraceLoggingWrite(
                g_traceProvider, "xrEndFrame_Sync", TLArg("D3D11", "Api"), TLArg(m_fenceValue, "FenceValue"));
            CHECK_HRCMD(m_d3d11Context->Signal(m_d3d11Fence.Get(), m_fenceValue));

            waitOnSubmissionDevice();
        }
    }

    void OpenXrRuntime::waitOnSubmissionDevice() {
        if (!m_syncGpuWorkInEndFrame.load(std::memory_order_relaxed)) {
            CHECK_HRCMD(m_ovrSubmissionContext->Wait(m_ovrSubmissionFence.Get(), m_fenceValue));
        } else {
            CHECK_HRCMD(m_ovrSubmissionFence->SetEventOnCompletion(m_fenceValue, m_eventForSubmissionFence.get()));
            WaitForSingleObject(m_eventForSubmissionFence.get(), INFINITE);
            ResetEvent(m_eventForSubmissionFence.get());
        }
    }

    bool OpenXrRuntime::requireNTHandleSharing() const {
        // Intel ARC driver does not support sharing KMT HANDLE to Vulkan/OpenGL.
        return m_gpuVendor == 0x8086 && (m_vkDevice || m_glContext.valid);
    }

} // namespace virtualdesktop_openxr
