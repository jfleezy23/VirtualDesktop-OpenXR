// Runtime-linked swapchain regressions; no settings writes, headset, or native Vulkan calls.
#include "pch.h"
#include "runtime.h"
#include <iostream>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    void checkHr(HRESULT result) {
        require(SUCCEEDED(result), "D3D operation failed");
    }
    void initializeBackend(const wchar_t* backendDirectory) {
        ovrInitParams init{};
        init.Flags = ovrInit_RequestVersion;
        init.RequestedMinorVersion = OVR_MINOR_VERSION;
        const auto directory = std::filesystem::absolute(backendDirectory).wstring() + L"\\";
        require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, directory.c_str())), "OVRNull initialization failed");
    }
    template <typename Handle>
    Handle token(uintptr_t value) {
        return reinterpret_cast<Handle>(value);
    }

    ComPtr<ID3D11Texture2D> ovrBuffer;
    unsigned ovrCreateCalls;
    unsigned ovrCommitCalls;
    unsigned ovrDestroyCalls;
    unsigned invalidOvrCalls;
    bool failOvrLength;
    std::vector<ovrTextureSwapChain> destroyedChains;
    ovrTextureSwapChainDesc oldOvrDescription{};
    std::map<ovrTextureSwapChain, ovrTextureSwapChainDesc> chainDescriptions;
    bool hookCleanupFailed;
    decltype(&ovr_CreateTextureSwapChainDX) originalCreate;
    decltype(&ovr_GetTextureSwapChainLength) originalLength;
    decltype(&ovr_GetTextureSwapChainBufferDX) originalBuffer;
    decltype(&ovr_GetTextureSwapChainCurrentIndex) originalIndex;
    decltype(&ovr_CommitTextureSwapChain) originalCommit;
    decltype(&ovr_DestroyTextureSwapChain) originalDestroy;
    decltype(&ovr_GetTextureSwapChainDesc) originalDescription;

    ovrResult OVR_CDECL createChain(ovrSession,
                                    IUnknown*,
                                    const ovrTextureSwapChainDesc* desc,
                                    ovrTextureSwapChain* output) {
        if (!desc || !output || desc->Type != ovrTexture_2D || desc->ArraySize != 1 || desc->SampleCount != 1) {
            ++invalidOvrCalls;
            return ovrError_InvalidParameter;
        }
        *output = token<ovrTextureSwapChain>(0x1000 + (++ovrCreateCalls) * 0x100);
        chainDescriptions[*output] = *desc;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainLength(ovrSession, ovrTextureSwapChain, int* output) {
        if (failOvrLength)
            return ovrError_InvalidParameter;
        *output = 1;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainBuffer(ovrSession, ovrTextureSwapChain, int index, IID iid, void** output) {
        if (index != 0 || !ovrBuffer || FAILED(ovrBuffer->QueryInterface(iid, output))) {
            ++invalidOvrCalls;
            return ovrError_InvalidParameter;
        }
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainIndex(ovrSession, ovrTextureSwapChain, int* output) {
        *output = 0;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL commitChain(ovrSession, ovrTextureSwapChain) {
        ++ovrCommitCalls;
        return ovrSuccess;
    }
    void OVR_CDECL destroyChain(ovrSession, ovrTextureSwapChain chain) {
        if (!chain)
            ++invalidOvrCalls;
        ++ovrDestroyCalls;
        destroyedChains.push_back(chain);
    }
    ovrResult OVR_CDECL chainDescription(ovrSession, ovrTextureSwapChain chain, ovrTextureSwapChainDesc* output) {
        if (output && chainDescriptions.count(chain)) {
            *output = chainDescriptions.at(chain);
            return ovrSuccess;
        }
        if (chain != token<ovrTextureSwapChain>(0x1000) || !output) {
            ++invalidOvrCalls;
            return ovrError_InvalidParameter;
        }
        *output = oldOvrDescription;
        return ovrSuccess;
    }

    class OvrHooks {
      public:
        OvrHooks() {
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            require(module != nullptr, "OVRNull backend was not loaded");
#define LOAD_OVR(variable, name)                                                                                       \
    variable = reinterpret_cast<decltype(variable)>(GetProcAddress(module, #name));                                    \
    require(variable != nullptr, "OVRNull export missing: " #name)
            LOAD_OVR(originalCreate, ovr_CreateTextureSwapChainDX);
            LOAD_OVR(originalLength, ovr_GetTextureSwapChainLength);
            LOAD_OVR(originalBuffer, ovr_GetTextureSwapChainBufferDX);
            LOAD_OVR(originalIndex, ovr_GetTextureSwapChainCurrentIndex);
            LOAD_OVR(originalCommit, ovr_CommitTextureSwapChain);
            LOAD_OVR(originalDestroy, ovr_DestroyTextureSwapChain);
            LOAD_OVR(originalDescription, ovr_GetTextureSwapChainDesc);
#undef LOAD_OVR
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Detour thread failed");
#define ATTACH_OVR(variable, replacement)                                                                              \
    require(DetourAttach(reinterpret_cast<PVOID*>(&variable), replacement) == NO_ERROR, "Detour attach failed")
            ATTACH_OVR(originalCreate, createChain);
            ATTACH_OVR(originalLength, chainLength);
            ATTACH_OVR(originalBuffer, chainBuffer);
            ATTACH_OVR(originalIndex, chainIndex);
            ATTACH_OVR(originalCommit, commitChain);
            ATTACH_OVR(originalDestroy, destroyChain);
            ATTACH_OVR(originalDescription, chainDescription);
#undef ATTACH_OVR
            require(DetourTransactionCommit() == NO_ERROR, "Detour commit failed");
        }
        ~OvrHooks() {
            LONG result = DetourTransactionBegin();
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
#define DETACH_OVR(variable, replacement)                                                                              \
    if (result == NO_ERROR)                                                                                            \
    result = DetourDetach(reinterpret_cast<PVOID*>(&variable), replacement)
            DETACH_OVR(originalCreate, createChain);
            DETACH_OVR(originalLength, chainLength);
            DETACH_OVR(originalBuffer, chainBuffer);
            DETACH_OVR(originalIndex, chainIndex);
            DETACH_OVR(originalCommit, commitChain);
            DETACH_OVR(originalDestroy, destroyChain);
            DETACH_OVR(originalDescription, chainDescription);
#undef DETACH_OVR
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            if (result != NO_ERROR) {
                hookCleanupFailed = true;
                std::cerr << "ERROR: OVR hook cleanup failed: " << result << '\n';
            }
        }
        OvrHooks(const OvrHooks&) = delete;
        OvrHooks& operator=(const OvrHooks&) = delete;
    };

    using CreateRtv = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
                                                  ID3D11Resource*,
                                                  const D3D11_RENDER_TARGET_VIEW_DESC*,
                                                  ID3D11RenderTargetView**);
    CreateRtv originalRtv;
    ID3D11Device* watchedRtvDevice;
    unsigned rtvCalls;

    HRESULT STDMETHODCALLTYPE countRtv(ID3D11Device* device,
                                       ID3D11Resource* resource,
                                       const D3D11_RENDER_TARGET_VIEW_DESC* desc,
                                       ID3D11RenderTargetView** output) {
        if (device == watchedRtvDevice)
            ++rtvCalls;
        return originalRtv(device, resource, desc, output);
    }

    class RtvHooks {
      public:
        explicit RtvHooks(ID3D11Device* device) : m_device(device) {
            auto** table = *reinterpret_cast<void***>(device);
            originalRtv = reinterpret_cast<CreateRtv>(table[9]); // ID3D11Device::CreateRenderTargetView.
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalRtv), countRtv);
            watchedRtvDevice = device;
            rtvCalls = 0;
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (result != NO_ERROR) {
                watchedRtvDevice = nullptr;
                throw std::runtime_error("RTV hook installation failed");
            }
        }
        ~RtvHooks() {
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalRtv), countRtv);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (result != NO_ERROR)
                std::terminate();
            watchedRtvDevice = nullptr;
        }
        RtvHooks(const RtvHooks&) = delete;
        RtvHooks& operator=(const RtvHooks&) = delete;

      private:
        ComPtr<ID3D11Device> m_device;
    };

    struct VulkanSpy {
        bool failImageCreation{};
        unsigned failAllocationAt{};
        unsigned failBindAt{};
        bool recording{};
        unsigned invalidCalls{};
        unsigned createCalls{};
        unsigned destroyCalls{};
        unsigned freeCalls{};
        unsigned allocationCalls{};
        unsigned bindCalls{};
        VkImageCreateFlags createFlags{};
        VkImageUsageFlags imageUsage{};
        VkFormat imageFormat{};
        VkImageAspectFlags aspects{};
        unsigned barriers{};
        std::set<VkImage> images;
        std::set<VkDeviceMemory> memories;
    } vkSpy;
    const auto vkDevice = token<VkDevice>(0x1100);
    const auto vkQueue = token<VkQueue>(0x2200);
    const auto vkFence = token<VkFence>(0x3300);
    const auto vkCommandBuffer = token<VkCommandBuffer>(0x4400);
    const auto vkImage = token<VkImage>(0x5500);

    VKAPI_ATTR VkResult VKAPI_CALL resetFences(VkDevice device, uint32_t count, const VkFence* fences) {
        if (device != vkDevice || count != 1 || !fences || *fences != vkFence)
            ++vkSpy.invalidCalls;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL submitQueue(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence) {
        if (queue != vkQueue || count != 1 || !info)
            ++vkSpy.invalidCalls;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL
    waitFences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32, uint64_t) {
        if (device != vkDevice || count != 1 || !fences || *fences != vkFence)
            ++vkSpy.invalidCalls;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL beginCommands(VkCommandBuffer buffer, const VkCommandBufferBeginInfo*) {
        if (buffer != vkCommandBuffer || vkSpy.recording) {
            ++vkSpy.invalidCalls;
            return VK_ERROR_UNKNOWN;
        }
        vkSpy.recording = true;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL endCommands(VkCommandBuffer buffer) {
        if (buffer != vkCommandBuffer || !vkSpy.recording)
            ++vkSpy.invalidCalls;
        vkSpy.recording = false;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL resetCommands(VkCommandBuffer buffer, VkCommandBufferResetFlags) {
        if (buffer != vkCommandBuffer)
            ++vkSpy.invalidCalls;
        vkSpy.recording = false;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice device,
                                               const VkImageCreateInfo* info,
                                               const VkAllocationCallbacks*,
                                               VkImage* image) {
        if (device != vkDevice || !info || !image) {
            ++vkSpy.invalidCalls;
            return VK_ERROR_UNKNOWN;
        }
        ++vkSpy.createCalls;
        vkSpy.createFlags = info->flags;
        vkSpy.imageUsage = info->usage;
        vkSpy.imageFormat = info->format;
        if (vkSpy.failImageCreation)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *image = token<VkImage>(0x5500 + (vkSpy.createCalls - 1) * 0x10);
        vkSpy.images.insert(*image);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL imageRequirements(VkDevice device,
                                                 const VkImageMemoryRequirementsInfo2* info,
                                                 VkMemoryRequirements2* requirements) {
        if (device != vkDevice || !info || !vkSpy.images.count(info->image))
            ++vkSpy.invalidCalls;
        requirements->memoryRequirements = {4096, 256, 1};
    }
    VKAPI_ATTR VkResult VKAPI_CALL handleProperties(VkDevice device,
                                                    VkExternalMemoryHandleTypeFlagBits type,
                                                    HANDLE handle,
                                                    VkMemoryWin32HandlePropertiesKHR* properties) {
        if (device != vkDevice || type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT || !handle)
            ++vkSpy.invalidCalls;
        properties->memoryTypeBits = 1;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice device,
                                                  const VkMemoryAllocateInfo* info,
                                                  const VkAllocationCallbacks*,
                                                  VkDeviceMemory* memory) {
        if (device != vkDevice || !info || info->allocationSize != 4096 || info->memoryTypeIndex != 0)
            ++vkSpy.invalidCalls;
        if (++vkSpy.allocationCalls == vkSpy.failAllocationAt)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *memory = token<VkDeviceMemory>(0x6600 + (vkSpy.allocationCalls - 1) * 0x10);
        vkSpy.memories.insert(*memory);
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL bindMemory(VkDevice device,
                                              VkImage image,
                                              VkDeviceMemory memory,
                                              VkDeviceSize offset) {
        if (device != vkDevice || !vkSpy.images.count(image) || !vkSpy.memories.count(memory) || offset != 0)
            ++vkSpy.invalidCalls;
        if (++vkSpy.bindCalls == vkSpy.failBindAt)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL pipelineBarrier(VkCommandBuffer buffer,
                                               VkPipelineStageFlags,
                                               VkPipelineStageFlags,
                                               VkDependencyFlags,
                                               uint32_t,
                                               const VkMemoryBarrier*,
                                               uint32_t,
                                               const VkBufferMemoryBarrier*,
                                               uint32_t count,
                                               const VkImageMemoryBarrier* barriers) {
        if (buffer != vkCommandBuffer || !vkSpy.recording || count != 1 || !barriers ||
            !vkSpy.images.count(barriers->image)) {
            ++vkSpy.invalidCalls;
            return;
        }
        ++vkSpy.barriers;
        vkSpy.aspects = barriers->subresourceRange.aspectMask;
    }
    VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*) {
        if (device != vkDevice || !vkSpy.images.erase(image))
            ++vkSpy.invalidCalls;
        ++vkSpy.destroyCalls;
    }
    VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*) {
        if (device != vkDevice || !vkSpy.memories.erase(memory))
            ++vkSpy.invalidCalls;
        ++vkSpy.freeCalls;
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void unusedRtvs(const wchar_t* backendDirectory) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            initializeBackend(backendDirectory);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            checkHr(runtime.m_ovrSubmissionDevice->CreateFence(
                0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&runtime.m_ovrSubmissionCompletionFence)));
            ovrCreateCalls = ovrDestroyCalls = invalidOvrCalls = 0;
            chainDescriptions.clear();
            auto desc = textureDescription(DXGI_FORMAT_B8G8R8A8_TYPELESS);
            desc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
            checkHr(device->CreateTexture2D(&desc, nullptr, &ovrBuffer));
            OpenXrRuntime::Swapchain chain{};
            chain.ovrSwapchainLength = 1;
            chain.dxgiFormatForSubmission = DXGI_FORMAT_B8G8R8A8_UNORM;
            {
                OvrHooks hooks;
                RtvHooks rtvHooks(runtime.m_ovrSubmissionDevice.Get());
                auto cleanup = MakeScopeGuard([&] {
                    for (auto& eye : chain.stereoProjection) {
                        if (eye.ovrSwapchain)
                            ovr_DestroyTextureSwapChain(runtime.m_ovrSession, eye.ovrSwapchain);
                        eye = {};
                    }
                });
                runtime.ensureSwapchainPrecompositorResources(chain, {64, 64});
                require(ovrCreateCalls == 2 && !invalidOvrCalls, "stereo output generation did not complete");
                for (const auto& eye : chain.stereoProjection) {
                    require(eye.images.size() == 1 && eye.uavs.size() == 1 && eye.uavs[0],
                            "output generation lacks a complete UAV");
                    const auto flags = chainDescriptions.at(eye.ovrSwapchain).BindFlags;
                    require((flags & (ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess)) ==
                                (ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess),
                            "output texture bind contract changed");
                    const float black[4]{};
                    context->ClearUnorderedAccessViewFloat(eye.uavs[0].Get(), black);
                }
                const auto firstCalls = rtvCalls;
                runtime.ensureSwapchainPrecompositorResources(chain, {64, 64});
                require(ovrCreateCalls == 2 && rtvCalls == firstCalls, "cached output generation created views again");
                desc.Width = desc.Height = 32;
                checkHr(device->CreateTexture2D(&desc, nullptr, &ovrBuffer));
                runtime.ensureSwapchainPrecompositorResources(chain, {32, 32});
                require(ovrCreateCalls == 4 && ovrDestroyCalls == 2 && !invalidOvrCalls,
                        "output resize did not replace both generations exactly once");
                for (const auto& eye : chain.stereoProjection) {
                    require(eye.uavs.size() == 1 && eye.uavs[0], "resized output generation lacks its UAV");
                }
                std::cout << "native unused RTV calls: initial=" << firstCalls << " total=" << rtvCalls << '\n';
                require(!rtvCalls, "compute precompositor must not create unused render-target views");
            }
            ovrBuffer.Reset();
            checkHr(device->GetDeviceRemovedReason());
            std::cout
                << "PASS: precompositor creation/reuse/resize preserves UAVs and bind flags without RTV creation\n";
        }
        static void installD3D(OpenXrRuntime& runtime,
                               ComPtr<ID3D11Device>& device,
                               ComPtr<ID3D11DeviceContext>& context) {
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
        }
        static D3D11_TEXTURE2D_DESC textureDescription(DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 64;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = format;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            return desc;
        }
        static void reverseSlices(const wchar_t* backendDirectory) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            initializeBackend(backendDirectory);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            const auto desc = textureDescription();
            checkHr(device->CreateTexture2D(&desc, nullptr, &ovrBuffer));
            ovrCreateCalls = ovrCommitCalls = invalidOvrCalls = 0;
            {
                OvrHooks hooks;
                OpenXrRuntime::Swapchain chain{};
                chain.ovrSwapchainLength = 1;
                chain.ovrDesc.Type = ovrTexture_2D;
                chain.ovrDesc.ArraySize = 2;
                chain.ovrDesc.SampleCount = chain.ovrDesc.MipLevels = 1;
                chain.ovrDesc.Width = chain.ovrDesc.Height = 64;
                chain.ovrDesc.Format = OVR_FORMAT_R8G8B8A8_UNORM;
                chain.appSwapchain.ovrSwapchain = token<ovrTextureSwapChain>(0x1000);
                chain.appSwapchain.images.push_back(ovrBuffer);
                runtime.ensureSwapchainSliceResources(chain, 1);
                runtime.ensureSwapchainSliceResources(chain, 0);
                std::cout << "reverse-slices creates=" << ovrCreateCalls << " slice0AliasesApp="
                          << (chain.resolvedSlices[0].ovrSwapchain == chain.appSwapchain.ovrSwapchain) << '\n';
                require(!invalidOvrCalls, "Reverse-slice fixture received invalid OVR arguments");
                require(chain.resolvedSlices[0].ovrSwapchain == chain.appSwapchain.ovrSwapchain &&
                            chain.resolvedSlices[0].images[0].Get() == chain.appSwapchain.images[0].Get(),
                        "Slice 0 selected unwritten images after slice 1 was initialized first");
                require(ovrCreateCalls == 1, "Direct slice 0 created an unnecessary distinct OVR swapchain");
            }
            ovrBuffer.Reset();
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "PASS: reverse array-slice order preserves direct slice 0 images\n";
        }
        static void mipArray(const wchar_t* backendDirectory) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            initializeBackend(backendDirectory);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            auto sourceDesc = textureDescription();
            sourceDesc.ArraySize = sourceDesc.MipLevels = 2;
            ComPtr<ID3D11Texture2D> source, destination;
            checkHr(device->CreateTexture2D(&sourceDesc, nullptr, &source));
            const std::array<std::array<uint8_t, 4>, 4> colors{
                {{{255, 0, 0, 255}}, {{0, 255, 0, 255}}, {{0, 0, 255, 255}}, {{255, 255, 0, 255}}}};
            for (unsigned subresource = 0; subresource < 4; ++subresource) {
                const unsigned size = subresource % 2 ? 32 : 64;
                std::vector<uint8_t> pixels(size * size * 4);
                for (size_t p = 0; p < pixels.size(); p += 4)
                    std::copy(colors[subresource].begin(), colors[subresource].end(), pixels.begin() + p);
                context->UpdateSubresource(
                    source.Get(), subresource, nullptr, pixels.data(), size * 4, size * size * 4);
            }
            auto destDesc = textureDescription();
            std::vector<uint8_t> black(64 * 64 * 4);
            const D3D11_SUBRESOURCE_DATA initial{black.data(), 64 * 4, 0};
            checkHr(device->CreateTexture2D(&destDesc, &initial, &destination));
            ovrCreateCalls = ovrCommitCalls = invalidOvrCalls = 0;
            {
                OvrHooks hooks;
                OpenXrRuntime::Swapchain chain{};
                chain.ovrSwapchainLength = 1;
                chain.lastReleasedIndex = 0;
                chain.dxgiFormatForSubmission = DXGI_FORMAT_R8G8B8A8_UNORM;
                chain.xrDesc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                chain.xrDesc.mipCount = chain.xrDesc.arraySize = 2;
                chain.ovrDesc.Type = ovrTexture_2D;
                chain.ovrDesc.ArraySize = chain.ovrDesc.MipLevels = 2;
                chain.ovrDesc.SampleCount = 1;
                chain.appSwapchain.ovrSwapchain = token<ovrTextureSwapChain>(0x1000);
                chain.appSwapchain.images.push_back(source);
                chain.resolvedSlices.resize(2);
                chain.resolvedSlices[1].ovrSwapchain = token<ovrTextureSwapChain>(0x2000);
                chain.resolvedSlices[1].images.push_back(destination);
                std::set<std::pair<OpenXrRuntime::Swapchain*, uint32_t>> resolved;
                runtime.resolveSwapchainImage(chain, 1, resolved);
            }
            require(ovrCommitCalls == 1 && !invalidOvrCalls,
                    "Mip-array resolve did not commit exactly one valid image");
            destDesc.BindFlags = 0;
            destDesc.Usage = D3D11_USAGE_STAGING;
            destDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            checkHr(device->CreateTexture2D(&destDesc, nullptr, &staging));
            context->CopyResource(staging.Get(), destination.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            unsigned wrongPixels = 0;
            for (unsigned y = 0; y < 64; ++y)
                for (unsigned x = 0; x < 64; ++x) {
                    const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                    if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 255 || pixel[3] != 255)
                        ++wrongPixels;
                }
            context->Unmap(staging.Get(), 0);
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "mip-array wrongPixels=" << wrongPixels << " expected=0 sourceSubresource=2\n";
            require(!wrongPixels, "Array slice 1 copied another slice's mip instead of its full base image");
            std::cout << "PASS: mipmapped array copy selects slice 1 base mip\n";
        }
        static void installVulkanSpy(OpenXrRuntime& runtime) {
            runtime.m_vkDevice = vkDevice;
            runtime.m_vkQueue = vkQueue;
            runtime.m_vkFenceForFlush = vkFence;
            runtime.m_vkCmdBuffer = vkCommandBuffer;
            runtime.m_vkMemoryProperties.memoryTypeCount = 1;
            runtime.m_vkMemoryProperties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            runtime.m_vkDispatch.vkResetFences = resetFences;
            runtime.m_vkDispatch.vkQueueSubmit = submitQueue;
            runtime.m_vkDispatch.vkWaitForFences = waitFences;
            runtime.m_vkDispatch.vkBeginCommandBuffer = beginCommands;
            runtime.m_vkDispatch.vkEndCommandBuffer = endCommands;
            runtime.m_vkDispatch.vkResetCommandBuffer = resetCommands;
            runtime.m_vkDispatch.vkCreateImage = createImage;
            runtime.m_vkDispatch.vkGetImageMemoryRequirements2KHR = imageRequirements;
            runtime.m_vkDispatch.vkGetMemoryWin32HandlePropertiesKHR = handleProperties;
            runtime.m_vkDispatch.vkAllocateMemory = allocateMemory;
            runtime.m_vkDispatch.vkBindImageMemory = bindMemory;
            runtime.m_vkDispatch.vkCmdPipelineBarrier = pipelineBarrier;
            runtime.m_vkDispatch.vkDestroyImage = destroyImage;
            runtime.m_vkDispatch.vkFreeMemory = freeMemory;
        }
        static void importRetry() {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            runtime.m_d3d11Device = runtime.m_ovrSubmissionDevice;
            auto desc = textureDescription();
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            OpenXrRuntime::Swapchain chain{};
            chain.ovrSwapchainLength = 2;
            for (unsigned i = 0; i < 2; ++i) {
                ComPtr<ID3D11Texture2D> texture;
                checkHr(device->CreateTexture2D(&desc, nullptr, &texture));
                chain.appSwapchain.images.push_back(texture);
            }
            XrSwapchainImageD3D11KHR images[2]{{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR}, {XR_TYPE_UNKNOWN}};
            require(runtime.getSwapchainImagesD3D11(chain, images, 2) == XR_ERROR_VALIDATION_FAILURE,
                    "Invalid second image header was accepted");
            std::cout << "import-retry cachedAfterFailure=" << chain.d3d11Images.size() << " expected=0\n";
            // Stop before retry if incomplete imports were published: the old code would index out of bounds.
            require(chain.d3d11Images.empty(), "Invalid second image header published incomplete D3D11 imports");
            images[1].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            require(runtime.getSwapchainImagesD3D11(chain, images, 2) == XR_SUCCESS,
                    "Corrected D3D11 image-header retry failed");
            require(chain.d3d11Images.size() == 2 && images[0].texture == chain.appSwapchain.images[0].Get() &&
                        images[1].texture == chain.appSwapchain.images[1].Get(),
                    "Corrected image-header retry did not return both application images");
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "PASS: invalid image header preserves a complete D3D11 import retry\n";
        }
        static void vulkanImportRetry() {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            installVulkanSpy(runtime);
            unsigned failures = 0;
            for (bool failBind : {false, true}) {
                vkSpy = {};
                if (failBind)
                    vkSpy.failBindAt = 2;
                else
                    vkSpy.failAllocationAt = 2;
                OpenXrRuntime::Swapchain chain{};
                chain.ovrSwapchainLength = 2;
                chain.xrDesc.arraySize = chain.xrDesc.mipCount = chain.xrDesc.faceCount = chain.xrDesc.sampleCount = 1;
                chain.xrDesc.width = chain.xrDesc.height = 64;
                chain.xrDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
                chain.xrDesc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                auto desc = textureDescription(DXGI_FORMAT_R8G8B8A8_TYPELESS);
                desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
                desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                for (unsigned i = 0; i < 2; ++i) {
                    ComPtr<ID3D11Texture2D> texture;
                    checkHr(device->CreateTexture2D(&desc, nullptr, &texture));
                    chain.appSwapchain.images.push_back(texture);
                }
                XrSwapchainImageVulkanKHR output[2]{{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR},
                                                    {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}};
                bool threw = false;
                try {
                    runtime.getSwapchainImagesVulkan(chain, output, 2);
                } catch (const std::exception&) {
                    threw = true;
                }
                require(threw && vkSpy.createCalls == 2 && !vkSpy.invalidCalls &&
                            (failBind ? vkSpy.bindCalls == 2 : vkSpy.allocationCalls == 2),
                        "Vulkan import fixture did not reach the second-image injected failure");
                std::cout << "vulkan-retry failure=" << (failBind ? "bind" : "allocate")
                          << " cachedImages=" << chain.vkImages.size()
                          << " cachedMemory=" << chain.vkDeviceMemory.size() << " liveImages=" << vkSpy.images.size()
                          << " liveMemory=" << vkSpy.memories.size() << " recording=" << vkSpy.recording << '\n';
                if (!chain.vkImages.empty() || !chain.vkDeviceMemory.empty() || !vkSpy.images.empty() ||
                    !vkSpy.memories.empty() || vkSpy.recording) {
                    ++failures;
                    // Clean the baseline's retained test handles, then continue to the other independent failure.
                    runtime.cleanupSwapchainImagesVulkan(chain);
                    continue;
                }
                require(vkSpy.destroyCalls == 2 && vkSpy.freeCalls == (failBind ? 2u : 1u),
                        "Vulkan rollback did not release each successfully allocated resource exactly once");
                vkSpy.failAllocationAt = vkSpy.failBindAt = 0;
                require(runtime.getSwapchainImagesVulkan(chain, output, 2) == XR_SUCCESS,
                        "Vulkan import retry failed after allocation/binding recovered");
                require(chain.vkImages.size() == 2 && chain.vkDeviceMemory.size() == 2 &&
                            output[0].image != VK_NULL_HANDLE && output[1].image != VK_NULL_HANDLE &&
                            output[0].image != output[1].image && !vkSpy.invalidCalls && !vkSpy.recording,
                        "Vulkan import retry did not produce two complete distinct images");
                runtime.cleanupSwapchainImagesVulkan(chain);
                require(vkSpy.images.empty() && vkSpy.memories.empty() && !vkSpy.invalidCalls,
                        "Vulkan import retry cleanup retained or double-destroyed resources");
                std::cout << "PASS: Vulkan " << (failBind ? "bind" : "allocate")
                          << " failure cleans resources and retries\n";
            }
            runtime.m_vkDevice = VK_NULL_HANDLE;
            runtime.m_vkQueue = VK_NULL_HANDLE;
            runtime.m_vkFenceForFlush = VK_NULL_HANDLE;
            runtime.m_vkCmdBuffer = VK_NULL_HANDLE;
            checkHr(device->GetDeviceRemovedReason());
            require(!failures, "Vulkan second-image failure published resources or retained recording commands");
        }
        static void ovrLengthFailure(const wchar_t* backendDirectory) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            initializeBackend(backendDirectory);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            ovrCreateCalls = ovrDestroyCalls = invalidOvrCalls = 0;
            failOvrLength = true;
            {
                OvrHooks hooks;
                OpenXrRuntime::Swapchain chain{};
                chain.ovrSwapchainLength = 1;
                ovrTextureSwapChainDesc desc{};
                desc.Type = ovrTexture_2D;
                desc.ArraySize = desc.SampleCount = desc.MipLevels = 1;
                desc.Width = desc.Height = 64;
                desc.Format = OVR_FORMAT_R8G8B8A8_UNORM;
                OpenXrRuntime::SwapchainSlice output{};
                bool threw = false;
                try {
                    runtime.populateSwapchainSlice(chain, desc, output, 0, "OVR length failure");
                } catch (const std::exception&) {
                    threw = true;
                }
                require(threw && ovrCreateCalls == 1 && !invalidOvrCalls,
                        "OVR slice fixture did not reach its injected length failure");
                const bool published = output.ovrSwapchain != nullptr || !output.images.empty();
                const auto destroyed = ovrDestroyCalls;
                std::cout << "ovr-length published=" << published << " destroyCalls=" << destroyed << " expected=1\n";
                if (output.ovrSwapchain && !destroyed) {
                    ovr_DestroyTextureSwapChain(runtime.m_ovrSession, output.ovrSwapchain);
                    output.ovrSwapchain = nullptr;
                }
                require(!published && destroyed == 1,
                        "OVR length failure published an incomplete slice instead of destroying its candidate");
            }
            failOvrLength = false;
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "PASS: failed OVR length query cleans the uninitialized slice candidate\n";
        }
        static void ovrCreateFailure(const wchar_t* backendDirectory) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            initializeBackend(backendDirectory);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            runtime.m_sessionCreated = true;
            runtime.m_isHeadless = false;
            runtime.m_forceSlowpathSwapchains = false;
            ovrCreateCalls = ovrDestroyCalls = invalidOvrCalls = 0;
            destroyedChains.clear();
            failOvrLength = true;
            {
                OvrHooks hooks;
                XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
                info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                info.width = info.height = 64;
                info.arraySize = info.faceCount = info.sampleCount = info.mipCount = 1;
                XrSwapchain output = XR_NULL_HANDLE;
                bool threw = false;
                try {
                    runtime.xrCreateSwapchain(reinterpret_cast<XrSession>(1), &info, &output);
                } catch (const std::exception&) {
                    threw = true;
                }
                require(threw && ovrCreateCalls == 1 && !invalidOvrCalls,
                        "Public swapchain creation did not reach the injected OVR length failure");
                const auto destroyed = ovrDestroyCalls;
                std::cout << "ovr-create outputNull=" << (output == XR_NULL_HANDLE)
                          << " registered=" << runtime.m_swapchains.size() << " destroyCalls=" << destroyed
                          << " expected=1\n";
                if (!destroyed)
                    ovr_DestroyTextureSwapChain(runtime.m_ovrSession, token<ovrTextureSwapChain>(0x1100));
                require(output == XR_NULL_HANDLE && runtime.m_swapchains.empty() && destroyed == 1,
                        "Failed public swapchain creation leaked its unregistered OVR candidate");
            }
            failOvrLength = false;
            runtime.m_sessionCreated = false;
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "PASS: public swapchain creation rolls back after OVR length failure\n";
        }
        static void ovrReplacementFailure(const wchar_t* backendDirectory) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            initializeBackend(backendDirectory);
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            auto desc = textureDescription();
            desc.Width = desc.Height = 8;
            desc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> image;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11UnorderedAccessView> uav;
            checkHr(device->CreateTexture2D(&desc, nullptr, &image));
            checkHr(device->CreateRenderTargetView(image.Get(), nullptr, &rtv));
            checkHr(device->CreateUnorderedAccessView(image.Get(), nullptr, &uav));
            oldOvrDescription = {};
            oldOvrDescription.Type = ovrTexture_2D;
            oldOvrDescription.ArraySize = oldOvrDescription.SampleCount = oldOvrDescription.MipLevels = 1;
            oldOvrDescription.Width = oldOvrDescription.Height = 8;
            oldOvrDescription.Format = OVR_FORMAT_R8G8B8A8_UNORM;
            ovrCreateCalls = ovrDestroyCalls = invalidOvrCalls = 0;
            destroyedChains.clear();
            failOvrLength = true;
            {
                OvrHooks hooks;
                OpenXrRuntime::Swapchain chain{};
                chain.ovrSwapchainLength = 1;
                chain.dxgiFormatForSubmission = DXGI_FORMAT_R8G8B8A8_UNORM;
                auto& old = chain.stereoProjection[0];
                old.ovrSwapchain = token<ovrTextureSwapChain>(0x1000);
                old.images.push_back(image);
                old.rtvs.push_back(rtv);
                old.uavs.push_back(uav);
                bool threw = false;
                try {
                    runtime.ensureSwapchainPrecompositorResources(chain, {16, 16});
                } catch (const std::exception&) {
                    threw = true;
                }
                require(threw && ovrCreateCalls == 1 && !invalidOvrCalls,
                        "Precompositor replacement did not reach the injected OVR length failure");
                const bool preserved = old.ovrSwapchain == token<ovrTextureSwapChain>(0x1000) &&
                                       old.images.size() == 1 && old.images[0].Get() == image.Get() &&
                                       old.rtvs.size() == 1 && old.rtvs[0].Get() == rtv.Get() && old.uavs.size() == 1 &&
                                       old.uavs[0].Get() == uav.Get();
                const bool candidateDestroyed =
                    destroyedChains.size() == 1 && destroyedChains[0] == token<ovrTextureSwapChain>(0x1100);
                std::cout << "ovr-replacement preserved=" << preserved << " destroys=" << destroyedChains.size()
                          << " candidateOnly=" << candidateDestroyed << '\n';
                for (auto handle : {token<ovrTextureSwapChain>(0x1000), token<ovrTextureSwapChain>(0x1100)})
                    if (std::find(destroyedChains.begin(), destroyedChains.end(), handle) == destroyedChains.end())
                        ovr_DestroyTextureSwapChain(runtime.m_ovrSession, handle);
                old.ovrSwapchain = nullptr;
                require(preserved && candidateDestroyed,
                        "Failed precompositor resize replaced or destroyed the existing handle, images, or views");
            }
            failOvrLength = false;
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "PASS: failed precompositor resize preserves all existing resources\n";
        }
        static void vulkan(bool mutableFormat) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            installD3D(runtime, device, context);
            installVulkanSpy(runtime);
            const std::array<std::pair<VkFormat, DXGI_FORMAT>, 4> formats{
                {{VK_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24G8_TYPELESS},
                 {VK_FORMAT_D32_SFLOAT_S8_UINT, DXGI_FORMAT_R32G8X24_TYPELESS},
                 {VK_FORMAT_D32_SFLOAT, DXGI_FORMAT_R32_TYPELESS},
                 {VK_FORMAT_D16_UNORM, DXGI_FORMAT_R16_TYPELESS}}};
            unsigned failures = 0;
            for (unsigned fixture = 0; fixture < (mutableFormat ? 1u : 4u); ++fixture) {
                vkSpy = {};
                vkSpy.failImageCreation = mutableFormat;
                OpenXrRuntime::Swapchain chain{};
                chain.ovrSwapchainLength = 1;
                chain.xrDesc.arraySize = chain.xrDesc.mipCount = chain.xrDesc.faceCount = chain.xrDesc.sampleCount = 1;
                chain.xrDesc.width = chain.xrDesc.height = 64;
                chain.xrDesc.format = mutableFormat ? VK_FORMAT_R8G8B8A8_UNORM : formats[fixture].first;
                chain.xrDesc.usageFlags =
                    mutableFormat ? XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT
                                  : XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                auto desc = textureDescription(mutableFormat ? DXGI_FORMAT_R8G8B8A8_TYPELESS : formats[fixture].second);
                desc.BindFlags |= mutableFormat ? D3D11_BIND_RENDER_TARGET : D3D11_BIND_DEPTH_STENCIL;
                desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                ComPtr<ID3D11Texture2D> texture;
                checkHr(device->CreateTexture2D(&desc, nullptr, &texture));
                chain.appSwapchain.images.push_back(texture);
                XrSwapchainImageVulkanKHR output{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
                bool threw = false;
                try {
                    require(runtime.getSwapchainImagesVulkan(chain, &output, 1) == XR_SUCCESS,
                            "Vulkan enumeration failed");
                } catch (const std::exception&) {
                    threw = true;
                }
                if (mutableFormat) {
                    require(threw && vkSpy.createCalls == 1 && !vkSpy.invalidCalls,
                            "Mutable-format descriptor fixture did not reach its deliberate allocation failure");
                    std::cout << "vulkan-mutable createFlags=" << vkSpy.createFlags << " usage=" << vkSpy.imageUsage
                              << '\n';
                    if (!(vkSpy.createFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ||
                        (vkSpy.imageUsage & VK_IMAGE_USAGE_STORAGE_BIT))
                        ++failures;
                } else {
                    require(!threw && output.image == vkImage && vkSpy.createCalls == 1 && vkSpy.barriers == 1,
                            "Depth/stencil fixture did not complete Vulkan image import and transition");
                    const VkImageAspectFlags expected = fixture < 2
                                                            ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                            : VK_IMAGE_ASPECT_DEPTH_BIT;
                    std::cout << "vulkan-depth format=" << int(vkSpy.imageFormat) << " aspects=" << vkSpy.aspects
                              << " expected=" << expected << '\n';
                    if (vkSpy.aspects != expected)
                        ++failures;
                }
                runtime.cleanupSwapchainImagesVulkan(chain);
                require(!vkSpy.invalidCalls && chain.vkImages.empty() && chain.vkDeviceMemory.empty(),
                        "Vulkan fixture cleanup used invalid handles");
                require(vkSpy.destroyCalls == (mutableFormat ? 0u : 1u) && vkSpy.freeCalls == (mutableFormat ? 0u : 1u),
                        "Vulkan fixture did not release imported resources exactly once");
            }
            runtime.m_vkDevice = VK_NULL_HANDLE;
            runtime.m_vkQueue = VK_NULL_HANDLE;
            runtime.m_vkFenceForFlush = VK_NULL_HANDLE;
            runtime.m_vkCmdBuffer = VK_NULL_HANDLE;
            checkHr(device->GetDeviceRemovedReason());
            require(!failures,
                    mutableFormat ? "Mutable-format image lacked the create flag or gained storage usage"
                                  : "Combined depth/stencil image transitioned without its stencil aspect");
            std::cout << "PASS: "
                      << (mutableFormat ? "Vulkan mutable image creation flags"
                                        : "Vulkan depth/stencil transition aspects")
                      << '\n';
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 2,
                "usage: nonnr_swapchain_regression alias|mip|ovr-length|ovr-create|ovr-replacement OVRNull-directory | "
                "mutable | stencil | retry | vk-retry");
        const std::wstring mode = argv[1];
        if (mode == L"unused-rtvs") {
            require(argc == 3, "RTV fixture requires the OVRNull directory");
            virtualdesktop_openxr::RuntimeInputRegression::unusedRtvs(argv[2]);
        } else if (mode == L"alias" || mode == L"mip" || mode == L"ovr-length" || mode == L"ovr-create" ||
                   mode == L"ovr-replacement") {
            require(argc == 3, "Alias and mip fixtures require the OVRNull directory");
            if (mode == L"alias")
                virtualdesktop_openxr::RuntimeInputRegression::reverseSlices(argv[2]);
            else if (mode == L"mip")
                virtualdesktop_openxr::RuntimeInputRegression::mipArray(argv[2]);
            else if (mode == L"ovr-length")
                virtualdesktop_openxr::RuntimeInputRegression::ovrLengthFailure(argv[2]);
            else if (mode == L"ovr-create")
                virtualdesktop_openxr::RuntimeInputRegression::ovrCreateFailure(argv[2]);
            else
                virtualdesktop_openxr::RuntimeInputRegression::ovrReplacementFailure(argv[2]);
        } else if (mode == L"vk-retry") {
            require(argc == 2, "Vulkan import-retry fixture takes no backend argument");
            virtualdesktop_openxr::RuntimeInputRegression::vulkanImportRetry();
        } else if (mode == L"retry") {
            require(argc == 2, "Import-retry fixture takes no backend argument");
            virtualdesktop_openxr::RuntimeInputRegression::importRetry();
        } else if (mode == L"mutable" || mode == L"stencil") {
            require(argc == 2, "Vulkan descriptor fixtures take no backend argument");
            virtualdesktop_openxr::RuntimeInputRegression::vulkan(mode == L"mutable");
        } else
            throw std::runtime_error("Unknown fixture mode");
        require(!hookCleanupFailed, "OVR hooks were not restored");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
