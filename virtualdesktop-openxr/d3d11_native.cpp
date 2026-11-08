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
#include "PairCaptureNrPostFx.h"
#include "PairCaptureNrSources.h"
#include "PairCaptureNrPrivate.h"

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
        if (vdxrfg::pair_capture::debugRequested())
            flags |= D3D11_CREATE_DEVICE_DEBUG;
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
            nr_capture::postFxSamplerCreated(m_linearClampSampler.Get(), desc);
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

        if (m_dlssnrContext) {
            m_dlssnrContext->Flush();
        }
        // Release the last finalized eye textures only after both submission and NR work have drained.
        m_precompositor.finalColour = {};
        // The drain above also protects all optional scratch and descriptor banks.
        if (m_nrSpatialPipeline) {
            m_nrSpatialPipeline->DiscardRecording();
            m_nrSpatialPipeline.reset();
        }
        if (m_nrNativePipeline) {
            m_nrNativePipeline->DiscardRecording();
            m_nrNativePipeline.reset();
        }
        m_nrNativeNeedsDrain = false;
        m_nrNativeNeedsFeatureReset = false;
        m_nrNativeStatus = 0;
        m_nrSpatialNeedsDrain = false;
        m_nrSpatialNeedsFeatureReset = false;
        m_nrSpatialFeatures = false;
        m_nrSpatialLastFrame = UINT64_MAX;
        m_dlssnrOutputSwapchainImages.clear();
        m_dlssnrOutputSwapchainTextures.clear();
        m_dlssnrOutputSwapchainSrvs.clear();
        m_dlssnrOutputSwapchainLastWrittenIndex = -1;
        if (m_dlssnrOutputSwapchain) {
            destroySubmissionSwapchain(m_ovrSession, m_dlssnrOutputSwapchain);
            m_dlssnrOutputSwapchain = nullptr;
        }
        m_dlssnrOutputSwapchainResolution = {0, 0};
        m_dlssnrOutputSwapchainFormat = OVR_FORMAT_UNKNOWN;
        m_dlssnrFeatureResolution = {0, 0};

        cleanupDlssnrResources();
        m_dlssnrInFence.Reset();
        m_dlssnrOutFence.Reset();
        m_dlssnrContext.reset();
        m_dlssnrDevice.Reset();

        m_ovrSubmissionFence.Reset();
        m_ovrSubmissionCompletionFence.Reset();
        m_submissionFenceValue = 0;
        m_ovrSubmissionContextState.Reset();
        if (m_ovrSubmissionContext && !m_useApplicationDeviceForSubmission) {
            // The earlier drain precedes the final Release calls. Complete
            // deferred destruction after those releases on our own context;
            // never clear a borrowed application's graphics state here.
            m_ovrSubmissionContext->ClearState();
            m_ovrSubmissionContext->Flush();
        }
        m_ovrSubmissionContext.Reset();
        m_ovrSubmissionDevice.Reset();
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
                    CHECK_OVRCMD(submissionSwapchainBuffer(m_ovrSession,
                                                           xrSwapchain.appSwapchain.ovrSwapchain,
                                                           i,
                                                           IID_PPV_ARGS(swapchainTexture.ReleaseAndGetAddressOf())));
                } else {
                    CHECK_HRCMD(m_ovrSubmissionDevice->CreateTexture2D(
                        &textureDesc, nullptr, swapchainTexture.ReleaseAndGetAddressOf()));
                }
                setDebugName(swapchainTexture.Get(),
                             fmt::format("OVR Swapchain Texture[{}, {}]", i, (void*)&xrSwapchain));

                if (nr_capture::hostRequest().valid) {
                    D3D11_TEXTURE2D_DESC nrSourceDescription{};
                    swapchainTexture->GetDesc(&nrSourceDescription);
                    nr_capture::nrBankTextureObserved(swapchainTexture.Get(), m_ovrSubmissionDevice.Get());
                    nr_capture::sourceTextureCreated(&xrSwapchain,
                                                     static_cast<uint32_t>(i),
                                                     swapchainTexture.Get(),
                                                     m_ovrSubmissionDevice.Get(),
                                                     nrSourceDescription);
                }
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
            const auto nrQueryBegin = nr_capture::sourceClock();
            const auto nrQueryResult = texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf()));
            nr_capture::sourceQueryObserved(&xrSwapchain,
                                            static_cast<uint32_t>(i),
                                            texture.Get(),
                                            dxgiResource.Get(),
                                            nrQueryResult,
                                            nrQueryBegin,
                                            nr_capture::sourceClock());
            CHECK_HRESULT(nrQueryResult,
                          "texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf()))");

            HANDLE textureHandle;
            if (!requireNTHandleSharing()) {
                const auto nrExportBegin = nr_capture::sourceClock();
                const auto nrExportResult = dxgiResource->GetSharedHandle(&textureHandle);
                nr_capture::sourceExportObserved(&xrSwapchain,
                                                 static_cast<uint32_t>(i),
                                                 SUCCEEDED(nrExportResult) ? textureHandle : nullptr,
                                                 false,
                                                 0,
                                                 nrExportResult,
                                                 nrExportBegin,
                                                 nr_capture::sourceClock());
                CHECK_HRESULT(nrExportResult, "dxgiResource->GetSharedHandle(&textureHandle)");
            } else {
                auto& ownedHandle = xrSwapchain.ntTextureHandles[i];
                if (!ownedHandle) {
                    const auto nrExportBegin = nr_capture::sourceClock();
                    auto nrExportResult = m_deliveryTransport
                                              ? m_deliveryTransport->duplicateSharedHandle(
                                                    xrSwapchain.appSwapchain.images[i].Get(), ownedHandle.put())
                                              : S_FALSE;
                    if (nrExportResult == S_FALSE)
                        nrExportResult =
                            dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ownedHandle.put());
                    nr_capture::sourceExportObserved(&xrSwapchain,
                                                     static_cast<uint32_t>(i),
                                                     ownedHandle.get(),
                                                     true,
                                                     GENERIC_ALL,
                                                     nrExportResult,
                                                     nrExportBegin,
                                                     nr_capture::sourceClock());
                    CHECK_HRESULT(nrExportResult,
                                  "dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ownedHandle.put())");
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
            if (skipSharing) {
                ensureAppSwapchainImages(xrSwapchain);
            } else {
                textureHandles = getSwapchainImages(xrSwapchain);
            }
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
                        const auto nrImportBegin = nr_capture::sourceClock();
                        const auto nrImportResult = m_d3d11Device->OpenSharedResource(
                            textureHandles[i], IID_PPV_ARGS(d3d11Texture.ReleaseAndGetAddressOf()));
                        nr_capture::sourceImportObserved(&xrSwapchain,
                                                         i,
                                                         m_d3d11Device.Get(),
                                                         m_d3d11Device.Get(),
                                                         d3d11Texture.Get(),
                                                         textureHandles[i],
                                                         false,
                                                         nrImportResult,
                                                         nrImportBegin,
                                                         nr_capture::sourceClock());
                        CHECK_HRESULT(nrImportResult,
                                      "m_d3d11Device->OpenSharedResource(textureHandles[i], "
                                      "IID_PPV_ARGS(d3d11Texture.ReleaseAndGetAddressOf()))");
                    } else {
                        const auto nrImportBegin = nr_capture::sourceClock();
                        const auto nrImportResult = m_d3d11Device->OpenSharedResource1(
                            textureHandles[i], IID_PPV_ARGS(d3d11Texture.ReleaseAndGetAddressOf()));
                        nr_capture::sourceImportObserved(&xrSwapchain,
                                                         i,
                                                         m_d3d11Device.Get(),
                                                         m_d3d11Device.Get(),
                                                         d3d11Texture.Get(),
                                                         textureHandles[i],
                                                         true,
                                                         nrImportResult,
                                                         nrImportBegin,
                                                         nr_capture::sourceClock());
                        CHECK_HRESULT(nrImportResult,
                                      "m_d3d11Device->OpenSharedResource1(textureHandles[i], "
                                      "IID_PPV_ARGS(d3d11Texture.ReleaseAndGetAddressOf()))");
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
                                              SwapchainImageList& resolved,
                                              bool skipCommit) {
        ensureAppSwapchainImages(xrSwapchain);
        ensureSwapchainSliceResources(xrSwapchain, slice);

        const auto tuple = std::make_pair(&xrSwapchain, slice);
        const auto resolvedPosition = std::lower_bound(resolved.begin(), resolved.end(), tuple);
        const bool alreadyResolved = resolvedPosition != resolved.end() && *resolvedPosition == tuple;
        auto& pending = m_precompositor.pendingSwapchainCommits;
        const auto pendingPosition = std::lower_bound(pending.begin(), pending.end(), tuple);
        const bool needsCommit = pendingPosition != pending.end() && *pendingPosition == tuple;
        const bool needCopy = xrSwapchain.resolvedSlices[slice].ovrSwapchain != xrSwapchain.appSwapchain.ovrSwapchain;
        if (alreadyResolved) {
            if (skipCommit || !needsCommit) {
                return;
            }
            if (needCopy) {
                // A previous processed layer wrote this image but did not submit it.
                // Commit that same destination without advancing and recopying it.
                CHECK_OVRCMD(commitSubmissionSwapchain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
                xrSwapchain.resolvedSlices[slice].hasCommittedImage = true;
                pending.erase(pendingPosition);
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
            CHECK_OVRCMD(
                submissionSwapchainIndex(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain, &ovrDestIndex));

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
            if (ovrCommittedIndex == lastReleasedIndex &&
                (xrSwapchain.resolvedSlices[slice].hasCommittedImage || xrSwapchain.ovrSwapchainLength == 1)) {
                // We still need to commit a static swapchain once!
                if (!xrSwapchain.resolvedSlices[slice].hasCommittedImage ||
                    (xrSwapchain.ovrSwapchainLength == 1 && xrSwapchain.dirty)) {
                    CHECK_OVRCMD(
                        commitSubmissionSwapchain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
                    xrSwapchain.resolvedSlices[slice].hasCommittedImage = true;
                }
                xrSwapchain.resolvedSlices[slice].lastCommittedIndex = ovrCommittedIndex;
                break;
            }
            CHECK_OVRCMD(commitSubmissionSwapchain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
            xrSwapchain.resolvedSlices[slice].hasCommittedImage = true;
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
                if (m_deliveryTransport) {
                    D3D11_TEXTURE2D_DESC destination{};
                    xrSwapchain.resolvedSlices[slice].images[ovrDestIndex]->GetDesc(&destination);
                    for (UINT mip = 1; mip < destination.MipLevels; ++mip)
                        m_ovrSubmissionContext->CopySubresourceRegion(
                            xrSwapchain.resolvedSlices[slice].images[ovrDestIndex].Get(),
                            mip,
                            0,
                            0,
                            0,
                            xrSwapchain.appSwapchain.images[lastReleasedIndex].Get(),
                            D3D11CalcSubresource(mip, slice, xrSwapchain.xrDesc.mipCount),
                            nullptr);
                }
                nr_capture::sourceBankWriteObserved(xrSwapchain.resolvedSlices[slice].images[ovrDestIndex].Get(),
                                                    m_ovrSubmissionContext.Get(),
                                                    m_frameBegun,
                                                    NR_COPY,
                                                    xrSwapchain.appSwapchain.images[lastReleasedIndex].Get());
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
                CHECK_OVRCMD(commitSubmissionSwapchain(m_ovrSession, xrSwapchain.resolvedSlices[slice].ovrSwapchain));
                xrSwapchain.resolvedSlices[slice].hasCommittedImage = true;
            }
            xrSwapchain.resolvedSlices[slice].lastCommittedIndex = ovrDestIndex;
        } else if (skipCommit) {
            xrSwapchain.resolvedSlices[slice].lastCommittedIndex = lastReleasedIndex;
        }

        if (!alreadyResolved) {
            resolved.insert(resolvedPosition, tuple);
        }
        if (skipCommit) {
            if (!needsCommit) {
                pending.insert(pendingPosition, tuple);
            }
        } else if (needsCommit) {
            pending.erase(pendingPosition);
        }
    }

    // Ensure necessary resources for submission: lazily create a second swapchain for this slice of the array or
    // when resolving MSAA.
    void OpenXrRuntime::ensureSwapchainSliceResources(Swapchain& xrSwapchain, uint32_t slice) const {
        if (xrSwapchain.resolvedSlices.size() <= slice) {
            xrSwapchain.resolvedSlices.resize(slice + 1);
        }
        // Ordinary FG may hold app images across many index wraps without
        // committing them. A private resolved source keeps fallback commits
        // independent of the application's circular acquisition history.
        const bool privateSource =
            m_frameGenerationMode && !m_precompositor.dlssnrSettings.enabled && (m_d3d11Context || m_d3d12CommandQueue);
        const bool replaceAlias =
            privateSource && xrSwapchain.appSwapchain.ovrSwapchain &&
            xrSwapchain.resolvedSlices[slice].ovrSwapchain == xrSwapchain.appSwapchain.ovrSwapchain;
        if (!xrSwapchain.resolvedSlices[slice].ovrSwapchain || replaceAlias) {
            if (slice == 0 && xrSwapchain.appSwapchain.ovrSwapchain && !privateSource) {
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
            const auto currentResolution = xrSwapchain.stereoProjection[eye].resolution;

            if (!xrSwapchain.stereoProjection[eye].ovrSwapchain || currentResolution.w != resolution.w ||
                currentResolution.h != resolution.h) {
                SwapchainSlice candidate;
                auto releaseCandidate = MakeScopeGuard([&] {
                    if (candidate.ovrSwapchain) {
                        destroySubmissionSwapchain(m_ovrSession, candidate.ovrSwapchain);
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
                        desc.Format =
                            m_frameGenerationMode ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_B8G8R8A8_UNORM_SRGB;
                        format =
                            m_frameGenerationMode ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                    } else {
                        desc.Format = m_frameGenerationMode ? OVR_FORMAT_R8G8B8A8_UNORM : OVR_FORMAT_B8G8R8A8_UNORM;
                        format = m_frameGenerationMode ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
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
                        nr_capture::postFxUavCreated(candidate.images[i].Get(), uav.Get(), desc);
                        setDebugName(uav.Get(),
                                     fmt::format("Precompositor UAV [{}, {}, {}]", eye, i, (void*)&xrSwapchain));
                        candidate.uavs.push_back(std::move(uav));
                    }
                }
                ovrTextureSwapChainDesc actualDesc{};
                CHECK_OVRCMD(submissionSwapchainDescription(m_ovrSession, candidate.ovrSwapchain, &actualDesc));
                candidate.resolution = {actualDesc.Width, actualDesc.Height};

                if (xrSwapchain.stereoProjection[eye].ovrSwapchain) {
                    // Retire queued reads before replacing this generation of output resources.
                    flushSubmissionContext();
                    destroySubmissionSwapchain(m_ovrSession, xrSwapchain.stereoProjection[eye].ovrSwapchain);
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
                destroySubmissionSwapchain(m_ovrSession, candidate.ovrSwapchain);
            }
        });
        CHECK_OVRCMD(
            createSubmissionSwapchain(m_ovrSession, m_ovrSubmissionDevice.Get(), &desc, &candidate.ovrSwapchain));

        int count = -1;
        CHECK_OVRCMD(submissionSwapchainLength(m_ovrSession, candidate.ovrSwapchain, &count));
        if (count <= 0 || (!m_deliveryTransport && count != xrSwapchain.ovrSwapchainLength)) {
            throw std::runtime_error("Swapchain image count mismatch");
        }

        // Query the textures for the swapchain.
        candidate.images.reserve(count);
        for (int i = 0; i < count; i++) {
            ComPtr<ID3D11Texture2D> texture;
            CHECK_OVRCMD(submissionSwapchainBuffer(
                m_ovrSession, candidate.ovrSwapchain, i, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
            nr_capture::nrBankTextureObserved(texture.Get(), m_ovrSubmissionDevice.Get());
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
            CHECK_MSG(WaitForSingleObject(eventHandle.get(), INFINITE) == WAIT_OBJECT_0,
                      "Failed to wait for D3D11 app flush completion");
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
            CHECK_MSG(WaitForSingleObject(eventHandle.get(), INFINITE) == WAIT_OBJECT_0,
                      "Failed to wait for D3D11 submission flush completion");
            ResetEvent(eventHandle.get());
        }
    }

    // Serialize commands from the D3D12 queue to the D3D11 context used by OVR.
    void OpenXrRuntime::serializeD3D11Frame(std::unique_lock<std::mutex>* producerLease, uint64_t earlyAppFence) {
        CHECK_MSG(!earlyAppFence ||
                      (m_frameGenerationMode && m_useAsyncSubmission && !isD3D12Session() && !isVulkanSession() &&
                       !isOpenGLSession() && m_d3d11Device && m_d3d11Context &&
                       m_ovrSubmissionDevice != m_d3d11Device && producerLease && producerLease->owns_lock() &&
                       producerLease->mutex() == &m_fgContextMutex && earlyAppFence == m_fenceValue),
                  "Invalid early application fence ownership");
        if (m_ovrSubmissionDevice != m_d3d11Device) {
            if (!earlyAppFence) {
                m_fenceValue++;
                CHECK_HRCMD(m_d3d11Context->Signal(m_d3d11Fence.Get(), m_fenceValue));
                if (m_frameGenerationMode && producerLease && producerLease->owns_lock())
                    m_d3d11Context->Flush();
            }
            TraceLoggingWrite(
                g_traceProvider, "xrEndFrame_Sync", TLArg("D3D11", "Api"), TLArg(m_fenceValue, "FenceValue"));

            waitOnSubmissionDevice(producerLease);
        }
    }

    void OpenXrRuntime::waitForFrameGenerationProducerFence(ID3D11Fence* fence,
                                                            uint64_t value,
                                                            std::unique_lock<std::mutex>* producerLease,
                                                            const char* dependency,
                                                            bool synchronousWait) {
        if (!m_frameGenerationMode || !m_useAsyncSubmission || m_useApplicationDeviceForSubmission ||
            isVulkanSession() || isOpenGLSession() || !producerLease)
            return;
        CHECK_MSG(producerLease->owns_lock() && producerLease->mutex() == &m_fgContextMutex,
                  "FG producer fence requires its submission context lease");
        {
            std::unique_lock state(m_asyncSubmissionMutex);
            if (m_asyncSubmissionError)
                std::rethrow_exception(m_asyncSubmissionError);
            CHECK_MSG(!m_terminateAsyncThread, "FG producer completion is stopping");
            CHECK_MSG(m_layersForAsyncSubmission.empty(), "FG producer must own the empty submission mailbox");
            // With no earlier group using the SDK queue, keep the original
            // asynchronous dependency and the application's CPU/GPU overlap.
            if (!m_fgActiveSubmission)
                return;
            CHECK_MSG(m_fgActiveOwnedPixels, "FG producer requires privately owned active pixels");
        }
        auto completed = fence->GetCompletedValue();
        CHECK_MSG(completed != UINT64_MAX, "FG producer fence device removed");
        if (completed >= value)
            return;
        if (m_deliveryTransport && !synchronousWait) {
            // P can finish the older group independently of S. Keep the exact
            // GPU Wait in the caller, without deferring this producer on the CPU.
            // NR's input Signal must be dispatched before S waits for NR output.
            if (fence == m_dlssnrOutFence.Get())
                m_ovrSubmissionContext->Flush();
            return;
        }
        {
            // Retirement publishes a wake on this same producer-owned event.
            // Publish its handle under the state lock before yielding context.
            std::unique_lock state(m_asyncSubmissionMutex);
            if (!m_fgProducerFenceEvent) {
                m_fgProducerFenceEvent.reset(CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS));
                CHECK_MSG(m_fgProducerFenceEvent.get(), "FG producer completion event creation failed");
            }
        }
        // Dispatch NR's input Signal before waiting on the NR queue's output.
        // Keep the existing GPU Wait below. It may remain pending when the
        // preceding presentation group finishes before the producer fence.
        m_ovrSubmissionContext->Flush();
        CHECK_HRCMD(fence->SetEventOnCompletion(value, m_fgProducerFenceEvent.get()));
        const bool trace = IsTraceEnabled();
        const double enter = trace ? ovr_GetTimeInSeconds() : 0.;
        producerLease->unlock();
        auto restoreLease = MakeScopeGuard([&] { lockFrameGenerationProducerContext(*producerLease); });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (completed < value) {
            {
                std::unique_lock lock(m_asyncSubmissionMutex);
                if (m_asyncSubmissionError)
                    std::rethrow_exception(m_asyncSubmissionError);
                CHECK_MSG(!m_terminateAsyncThread, "FG producer completion is stopping");
                // The previous group has now queued every SDK handoff and
                // retired its copies. A still-pending Wait is safe *after* it.
                // Retain that original GPU Wait below; do not drain future NR
                // merely to make this helper return with a completed fence.
                if (!m_fgActiveSubmission && m_layersForAsyncSubmission.empty())
                    break;
            }
            const auto waited = WaitForSingleObject(m_fgProducerFenceEvent.get(), 5);
            CHECK_MSG(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT, "FG producer fence event wait failed");
            completed = fence->GetCompletedValue();
            CHECK_MSG(completed != UINT64_MAX, "FG producer fence device removed");
            CHECK_MSG(completed >= value || std::chrono::steady_clock::now() < deadline,
                      "FG producer fence completion timed out");
        }
        if (trace)
            TraceLoggingWrite(g_traceProvider,
                              "FG_ProducerFence",
                              TLArg(dependency, "Dependency"),
                              TLArg(value, "Target"),
                              TLArg(completed, "Completed"),
                              TLArg(enter, "WaitEnter"),
                              TLArg(ovr_GetTimeInSeconds(), "WaitReturn"));
    }

    void OpenXrRuntime::waitOnSubmissionDevice(std::unique_lock<std::mutex>* producerLease) {
        const bool synchronousWait = m_syncGpuWorkInEndFrame.load(std::memory_order_relaxed);
        waitForFrameGenerationProducerFence(
            m_ovrSubmissionFence.Get(), m_fenceValue, producerLease, "application", synchronousWait);
        if (!synchronousWait) {
            CHECK_HRCMD(m_ovrSubmissionContext->Wait(m_ovrSubmissionFence.Get(), m_fenceValue));
        } else {
            // A null event waits synchronously for this exact fence value without a separately reset Win32 event.
            // D3D11 documents D3D12-equivalent null-event blocking despite the SDK's nonoptional HANDLE annotation.
#pragma warning(suppress : 6387)
            CHECK_HRCMD(m_ovrSubmissionFence->SetEventOnCompletion(m_fenceValue, nullptr));
        }
    }

    bool OpenXrRuntime::requireNTHandleSharing() const {
        // Intel ARC driver does not support sharing KMT HANDLE to Vulkan/OpenGL.
        return m_gpuVendor == 0x8086 && (m_vkDevice || m_glContext.valid);
    }

} // namespace virtualdesktop_openxr
