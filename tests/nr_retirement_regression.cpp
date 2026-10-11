// Real NR-queue work without a downstream D3D11 dependency, as after an output Wait failure.
// A fixture-only owner prevents unsafe GPU access in the failing baseline control.
#include "pch.h"
#include "runtime.h"

namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }
    void checkHr(HRESULT value) {
        require(SUCCEEDED(value), "Native D3D12 setup failed");
    }
    using FenceEvent = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
    FenceEvent originalEvent;
    ID3D12Fence* watchedFence;
    std::atomic<bool> failEvent{}, enteredDrain{};
    HRESULT STDMETHODCALLTYPE event(ID3D12Fence* fence, UINT64 value, HANDLE handle) {
        if (fence == watchedFence) {
            enteredDrain = true;
            if (failEvent)
                return E_OUTOFMEMORY;
        }
        return originalEvent(fence, value, handle);
    }
    class EventHook {
      public:
        explicit EventHook(ID3D12Fence* fence) {
            watchedFence = fence;
            originalEvent = reinterpret_cast<FenceEvent>((*reinterpret_cast<void***>(fence))[9]);
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            LONG status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            require(status == NO_ERROR, "Native drain hook failed");
        }
        ~EventHook() {
            LONG status = DetourTransactionBegin();
            if (status == NO_ERROR)
                status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            if (status != NO_ERROR)
                std::terminate();
            watchedFence = nullptr;
        }
        EventHook(const EventHook&) = delete;
        EventHook& operator=(const EventHook&) = delete;
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void run(bool injectFailure) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D12Device> device;
            checkHr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
            runtime.m_dlssnrDevice = device;
            runtime.m_dlssnrContext = std::make_unique<D3D12Utils::CommandContext>(device.Get(), L"NR retirement");
            auto& context = *runtime.m_dlssnrContext;
            ComPtr<ID3D12Fence> gate;
            checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(65536);
            const CD3DX12_HEAP_PROPERTIES gpuHeap(D3D12_HEAP_TYPE_DEFAULT), uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
            ComPtr<ID3D12Resource> guardedInput, upload;
            checkHr(device->CreateCommittedResource(&gpuHeap,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr,
                                                    IID_PPV_ARGS(&guardedInput)));
            checkHr(device->CreateCommittedResource(&uploadHeap,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr,
                                                    IID_PPV_ARGS(&upload)));
            void* mapped{};
            const D3D12_RANGE noRead{0, 0};
            checkHr(upload->Map(0, &noRead, &mapped));
            memset(mapped, 0x73, 65536);
            upload->Unmap(0, nullptr);
            auto* chain = new OpenXrRuntime::Swapchain{};
            chain->resolvedSlices.resize(1);
            chain->resolvedSlices[0].dlssnrImages.push_back(guardedInput);
            const auto handle = reinterpret_cast<XrSwapchain>(chain);
            runtime.m_swapchains.insert(handle);
            checkHr(context.GetCommandQueue()->Wait(gate.Get(), 1));
            auto commands = context.GetCommandList();
            commands.Commands->CopyBufferRegion(guardedInput.Get(), 0, upload.Get(), 0, 65536);
            context.SubmitCommandList(std::move(commands));
            bool rejected{}, retained{}, early{}, destroyed{}, observedDrain{};
            {
                EventHook hook(context.GetCompletionFence());
                failEvent = injectFailure;
                if (injectFailure) {
                    try {
                        runtime.xrDestroySwapchain(handle);
                    } catch (const std::exception&) {
                        rejected = true;
                    }
                    retained =
                        runtime.m_swapchains.count(handle) && chain->resolvedSlices[0].dlssnrImages[0] == guardedInput;
                }
                failEvent = false;
                enteredDrain = false;
                std::atomic<bool> returned{};
                std::thread retire([&] {
                    try {
                        destroyed = runtime.xrDestroySwapchain(handle) == XR_SUCCESS;
                    } catch (const std::exception&) {
                        destroyed = false;
                    }
                    returned = true;
                });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!enteredDrain && !returned && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                early = returned;
                observedDrain = enteredDrain;
                // Baseline retirement can finish early. Keep guardedInput alive until real queue completion.
                const auto status = gate->Signal(1);
                retire.join();
                checkHr(status);
                context.Flush();
            }
            const bool remaining = runtime.m_swapchains.count(handle) != 0;
            if (remaining)
                require(runtime.xrDestroySwapchain(handle) == XR_SUCCESS, "Fixture cleanup failed");
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "native retirement failure=" << injectFailure << " rejected=" << rejected
                      << " retained=" << retained << " early=" << early << " destroyed=" << destroyed
                      << " observedDrain=" << observedDrain << '\n';
            if (injectFailure)
                require(rejected && retained, "Failed NR drain did not preserve input swapchain ownership");
            require(observedDrain && !early && destroyed && !remaining,
                    "Swapchain retirement bypassed pending NR GPU work");
            std::cout << "PASS: native NR input retirement " << (injectFailure ? "failure/retry" : "blocked") << '\n';
        }
    };
} // namespace virtualdesktop_openxr

int main(int argc, char** argv) {
    try {
        require(argc == 2 && (!strcmp(argv[1], "blocked") || !strcmp(argv[1], "event")),
                "Usage: nr_retirement_regression blocked|event");
        virtualdesktop_openxr::RuntimeInputRegression::run(!strcmp(argv[1], "event"));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
