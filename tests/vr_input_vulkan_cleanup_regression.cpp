// CPU-only Vulkan rollback checks: local dispatch spies never invoke a graphics driver.
#include "pch.h"
#include "runtime.h"

namespace {
    template <typename Handle>
    Handle token(uintptr_t value) {
        return reinterpret_cast<Handle>(value);
    }
    const auto device = token<VkDevice>(0x1100);
    const auto semaphore = token<VkSemaphore>(0x2200);
    const auto fence = token<VkFence>(0x3300);
    const auto pool = token<VkCommandPool>(0x4400);
    const auto buffer = token<VkCommandBuffer>(0x5500);
    std::array<uint32_t, 6> calls{}; // Idle, semaphore, fence, reset, free, pool.
    uint32_t invalidCalls{};

    VKAPI_ATTR void VKAPI_CALL physicalProperties(VkPhysicalDevice, VkPhysicalDeviceProperties2* properties) {
        auto* identity = static_cast<VkPhysicalDeviceIDProperties*>(properties->pNext);
        identity->deviceLUIDValid = VK_FALSE;
    }
    VKAPI_ATTR VkResult VKAPI_CALL waitIdle(VkDevice value) {
        ++calls[0];
        if (value != device)
            ++invalidCalls;
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL destroySemaphore(VkDevice value, VkSemaphore child, const VkAllocationCallbacks*) {
        ++calls[1];
        if (value != device || child != semaphore)
            ++invalidCalls;
    }
    VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice value, VkFence child, const VkAllocationCallbacks*) {
        ++calls[2];
        if (value != device || child != fence)
            ++invalidCalls;
    }
    VKAPI_ATTR VkResult VKAPI_CALL resetBuffer(VkCommandBuffer child, VkCommandBufferResetFlags) {
        ++calls[3];
        if (child != buffer)
            ++invalidCalls;
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL freeBuffers(VkDevice value,
                                           VkCommandPool owner,
                                           uint32_t count,
                                           const VkCommandBuffer* child) {
        ++calls[4];
        if (value != device || owner != pool || count != 1 || !child || *child != buffer)
            ++invalidCalls;
    }
    VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice value, VkCommandPool child, const VkAllocationCallbacks*) {
        ++calls[5];
        if (value != device || child != pool)
            ++invalidCalls;
    }
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getProc(VkInstance, const char* name) {
#define RETURN_PROC(api, fixture)                                                                                      \
    if (!strcmp(name, #api))                                                                                           \
    return reinterpret_cast<PFN_vkVoidFunction>(fixture)
        RETURN_PROC(vkGetPhysicalDeviceProperties2, physicalProperties);
        RETURN_PROC(vkDeviceWaitIdle, waitIdle);
        RETURN_PROC(vkDestroySemaphore, destroySemaphore);
        RETURN_PROC(vkDestroyFence, destroyFence);
        RETURN_PROC(vkResetCommandBuffer, resetBuffer);
        RETURN_PROC(vkFreeCommandBuffers, freeBuffers);
        RETURN_PROC(vkDestroyCommandPool, destroyPool);
#undef RETURN_PROC
        return nullptr;
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void installDispatch(OpenXrRuntime& runtime) {
            runtime.m_vkDispatch.vkGetInstanceProcAddr = getProc;
            runtime.initializeVulkanDispatch(token<VkInstance>(0x6600));
        }
        static int run() {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            int failures = 0;
            const auto expect = [&](const char* label, const std::array<uint32_t, 6>& expected) {
                if (calls != expected || invalidCalls) {
                    ++failures;
                    std::cerr << "FAIL: " << label << " invalid=" << invalidCalls << " calls=";
                    for (const auto count : calls)
                        std::cerr << count << ' ';
                    std::cerr << '\n';
                } else
                    std::cout << "PASS: " << label << '\n';
                calls = {};
                invalidCalls = 0;
            };
            installDispatch(runtime);
            XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
            binding.instance = token<VkInstance>(0x6600);
            binding.physicalDevice = token<VkPhysicalDevice>(0x7700);
            binding.device = device;
            // This is the actual initialization failure before Vulkan ownership is published.
            if (runtime.initializeVulkan(binding) != XR_ERROR_RUNTIME_FAILURE || runtime.m_vkDevice)
                throw std::runtime_error(
                    "Invalid-LUID fixture did not stop actual Vulkan initialization before device publication");
            runtime.cleanupVulkan();
            expect("invalid-LUID initialization cleanup makes no Vulkan device calls", {});

            installDispatch(runtime);
            runtime.m_vkDevice = device;
            runtime.cleanupVulkan();
            expect("device-only partial initialization skips absent child objects", {1, 0, 0, 0, 0, 0});

            installDispatch(runtime);
            runtime.m_vkDevice = device;
            runtime.m_vkCmdPool = pool;
            runtime.cleanupVulkan();
            expect("command-pool-only initialization skips absent command buffer", {1, 0, 0, 0, 0, 1});

            installDispatch(runtime);
            runtime.m_vkDevice = device;
            runtime.m_vkFenceForFlush = fence;
            runtime.m_vkDispatch.vkDestroySemaphore = nullptr;
            runtime.cleanupVulkan();
            expect("fence cleanup does not depend on semaphore dispatch", {1, 0, 1, 0, 0, 0});

            installDispatch(runtime);
            runtime.m_vkDevice = device;
            runtime.m_vkTimelineSemaphore = semaphore;
            runtime.m_vkFenceForFlush = fence;
            runtime.m_vkCmdPool = pool;
            runtime.m_vkCmdBuffer = buffer;
            runtime.cleanupVulkan();
            expect("fully initialized Vulkan children are released once", {1, 1, 1, 1, 1, 1});
            runtime.cleanupVulkan();
            expect("repeated Vulkan cleanup makes no device calls", {});
            if (runtime.m_vkDevice || runtime.m_vkCmdPool || runtime.m_vkCmdBuffer || runtime.m_vkTimelineSemaphore ||
                runtime.m_vkFenceForFlush)
                throw std::runtime_error("Vulkan cleanup retained a previously owned handle");
            return failures ? 1 : 0;
        }
    };
} // namespace virtualdesktop_openxr

int main() {
    try {
        return virtualdesktop_openxr::RuntimeInputRegression::run();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
