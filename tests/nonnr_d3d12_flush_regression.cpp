// Runtime-linked D3D12 drain failures and real GPU completion; no VR session or settings writes.
#include "pch.h"
#include "runtime.h"
#include <iostream>

namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }
    void checkHr(HRESULT value) {
        require(SUCCEEDED(value), "D3D12 fixture setup failed");
    }
    enum class Failure { None, Signal, Event, Wait, Allocator };
    using QueueSignal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    using FenceEvent = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
    QueueSignal originalSignal;
    FenceEvent originalEvent;
    using AllocatorReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);
    using ListReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                  ID3D12CommandAllocator*,
                                                  ID3D12PipelineState*);
    AllocatorReset originalAllocatorReset;
    ListReset originalListReset;
    ID3D12CommandAllocator* watchedAllocator;
    ID3D12GraphicsCommandList* watchedList;
    unsigned allocatorCalls, listCalls;
    bool completedWait, invalidResetOrder;
    decltype(&WaitForSingleObject) originalWait = WaitForSingleObject;
    ID3D12CommandQueue* watchedQueue;
    ID3D12Fence* watchedFence;
    HANDLE watchedEvent;
    HANDLE enteredWait;
    Failure failure;
    unsigned signalCalls, eventCalls, waitCalls;
    UINT64 signaledValue;
    thread_local bool watchThisThread;
    HRESULT STDMETHODCALLTYPE allocatorReset(ID3D12CommandAllocator* allocator) {
        if (watchThisThread && allocator == watchedAllocator) {
            ++allocatorCalls;
            invalidResetOrder |= !completedWait || watchedFence->GetCompletedValue() < signaledValue;
            if (failure == Failure::Allocator)
                return E_FAIL;
        }
        return originalAllocatorReset(allocator);
    }
    HRESULT STDMETHODCALLTYPE listReset(ID3D12GraphicsCommandList* list,
                                        ID3D12CommandAllocator* allocator,
                                        ID3D12PipelineState* state) {
        if (watchThisThread && list == watchedList) {
            ++listCalls;
            invalidResetOrder |= allocatorCalls != listCalls;
        }
        return originalListReset(list, allocator, state);
    }
    class WatchThread {
      public:
        explicit WatchThread(bool active) : m_previous(watchThisThread) {
            watchThisThread = active;
        }
        ~WatchThread() {
            watchThisThread = m_previous;
        }
        WatchThread(const WatchThread&) = delete;
        WatchThread& operator=(const WatchThread&) = delete;

      private:
        bool m_previous;
    };

    HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
        if (!watchThisThread || queue != watchedQueue || fence != watchedFence)
            return originalSignal(queue, fence, value);
        ++signalCalls;
        completedWait = false;
        signaledValue = value;
        return failure == Failure::Signal ? E_FAIL : originalSignal(queue, fence, value);
    }
    HRESULT STDMETHODCALLTYPE event(ID3D12Fence* fence, UINT64 value, HANDLE handle) {
        if (!watchThisThread || fence != watchedFence)
            return originalEvent(fence, value, handle);
        ++eventCalls;
        watchedEvent = handle;
        // Fail an unexpected baseline registration instead of waiting forever on an unqueued signal.
        if (failure == Failure::Signal)
            return E_UNEXPECTED;
        if (failure == Failure::Event)
            return E_OUTOFMEMORY;
        return originalEvent(fence, value, handle);
    }
    DWORD WINAPI wait(HANDLE handle, DWORD milliseconds) {
        if (!watchThisThread || !watchedEvent || handle != watchedEvent)
            return originalWait(handle, milliseconds);
        ++waitCalls;
        if (enteredWait)
            SetEvent(enteredWait);
        if (failure == Failure::Wait) {
            completedWait = false;
            SetLastError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        // Bound the fixture even if production's drain never completes.
        const auto result = originalWait(handle, std::min<DWORD>(milliseconds, 5000));
        completedWait = result == WAIT_OBJECT_0;
        return result;
    }
    class Hooks {
      public:
        Hooks(ID3D12CommandQueue* queue,
              ID3D12Fence* fence,
              ID3D12CommandAllocator* allocator = nullptr,
              ID3D12GraphicsCommandList* list = nullptr)
            : m_queue(queue), m_fence(fence), m_previousActive(watchThisThread) {
            watchedQueue = queue;
            watchedFence = fence;
            watchedEvent = enteredWait = nullptr;
            signalCalls = eventCalls = waitCalls = 0;
            signaledValue = 0;
            originalSignal = reinterpret_cast<QueueSignal>((*reinterpret_cast<void***>(queue))[14]);
            originalEvent = reinterpret_cast<FenceEvent>((*reinterpret_cast<void***>(fence))[9]);
            watchedAllocator = allocator;
            watchedList = list;
            allocatorCalls = listCalls = 0;
            completedWait = invalidResetOrder = false;
            if (allocator)
                originalAllocatorReset = reinterpret_cast<AllocatorReset>((*reinterpret_cast<void***>(allocator))[8]);
            if (list)
                originalListReset = reinterpret_cast<ListReset>((*reinterpret_cast<void***>(list))[10]);
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            LONG result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal);
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (result == NO_ERROR && allocator)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalAllocatorReset), allocatorReset);
            if (result == NO_ERROR && list)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalListReset), listReset);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            require(result == NO_ERROR, "Detour attach failed");
            watchThisThread = true;
        }
        ~Hooks() {
            watchThisThread = false;
            LONG result = DetourTransactionBegin();
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal);
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (result == NO_ERROR && watchedAllocator)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalAllocatorReset), allocatorReset);
            if (result == NO_ERROR && watchedList)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalListReset), listReset);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            if (result != NO_ERROR)
                std::terminate();
            watchedQueue = nullptr;
            watchedFence = nullptr;
            watchedEvent = enteredWait = nullptr;
            watchedAllocator = nullptr;
            watchedList = nullptr;
            watchThisThread = m_previousActive;
        }
        Hooks(const Hooks&) = delete;
        Hooks& operator=(const Hooks&) = delete;

      private:
        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<ID3D12Fence> m_fence;
        bool m_previousActive;
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void run(const std::wstring& mode) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D12Device> device;
            checkHr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
            D3D12_COMMAND_QUEUE_DESC desc{};
            ComPtr<ID3D12CommandQueue> queue;
            ComPtr<ID3D12Fence> fence;
            checkHr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())));
            checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
            runtime.m_d3d12Device = device;
            runtime.m_d3d12CommandQueue = queue;
            runtime.m_d3d12Fence = fence;
            checkHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(runtime.m_d3d12CommandAllocator.GetAddressOf())));
            checkHr(device->CreateCommandList(0,
                                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              runtime.m_d3d12CommandAllocator.Get(),
                                              nullptr,
                                              IID_PPV_ARGS(runtime.m_d3d12CommandList.GetAddressOf())));
            checkHr(runtime.m_d3d12CommandList->Close());
            if (mode == L"allocator" || mode == L"allocator-fail" || mode == L"allocator-wait" ||
                mode == L"allocator-sampled") {
                ComPtr<ID3D11Device> sourceDevice;
                checkHr(D3D11CreateDevice(nullptr,
                                          D3D_DRIVER_TYPE_HARDWARE,
                                          nullptr,
                                          0,
                                          nullptr,
                                          0,
                                          D3D11_SDK_VERSION,
                                          sourceDevice.GetAddressOf(),
                                          nullptr,
                                          nullptr));
                checkHr(sourceDevice.As(&runtime.m_ovrSubmissionDevice));
                D3D11_TEXTURE2D_DESC textureDesc{};
                textureDesc.Width = textureDesc.Height = 64;
                textureDesc.ArraySize = textureDesc.MipLevels = textureDesc.SampleDesc.Count = 1;
                textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                OpenXrRuntime::Swapchain chain{};
                chain.ovrSwapchainLength = 2;
                chain.xrDesc.usageFlags = mode == L"allocator-sampled" ? XR_SWAPCHAIN_USAGE_SAMPLED_BIT
                                                                       : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                for (unsigned i = 0; i < 2; ++i) {
                    ComPtr<ID3D11Texture2D> image;
                    checkHr(sourceDevice->CreateTexture2D(&textureDesc, nullptr, image.GetAddressOf()));
                    chain.appSwapchain.images.push_back(std::move(image));
                }
                XrSwapchainImageD3D12KHR images[2]{{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR},
                                                   {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR}};
                Hooks hooks(
                    queue.Get(), fence.Get(), runtime.m_d3d12CommandAllocator.Get(), runtime.m_d3d12CommandList.Get());
                failure = mode == L"allocator-fail"   ? Failure::Allocator
                          : mode == L"allocator-wait" ? Failure::Wait
                                                      : Failure::None;
                if (failure != Failure::None) {
                    bool threw = false;
                    try {
                        runtime.getSwapchainImagesD3D12(chain, images, 2);
                    } catch (const std::exception&) {
                        threw = true;
                    }
                    failure = Failure::None;
                    // Even an unexpected success must be drained before assertions unwind image owners.
                    runtime.flushD3D12CommandQueue();
                    require(threw && chain.d3d12Images.empty() && listCalls == 0,
                            "Failed drain/allocator reset published imports or reset the command list");
                    require(allocatorCalls == (mode == L"allocator-fail" ? 1u : 0u),
                            "Allocator reset occurred before completion or missed its injected failure");
                    allocatorCalls = listCalls = 0;
                }
                require(runtime.getSwapchainImagesD3D12(chain, images, 2) == XR_SUCCESS, "D3D12 import/retry failed");
                const auto expectedResets = mode == L"allocator-sampled" ? 0u : 1u;
                require(mode != L"allocator-sampled" || signalCalls == 0,
                        "Sampled-only import introduced a queue drain");
                runtime.flushD3D12CommandQueue();
                require(allocatorCalls == expectedResets && listCalls == expectedResets && !invalidResetOrder,
                        "Import did not complete the queue, reset allocator storage, then reset the list");
                const auto retained = chain.d3d12Images;
                const auto signals = signalCalls;
                require(runtime.getSwapchainImagesD3D12(chain, images, 2) == XR_SUCCESS &&
                            chain.d3d12Images == retained && signalCalls == signals &&
                            allocatorCalls == expectedResets && listCalls == expectedResets,
                        "Cached import drained/reset again");
                // Another newly imported swapchain must reuse the completed allocator too.
                OpenXrRuntime::Swapchain nextChain{};
                nextChain.ovrSwapchainLength = 2;
                nextChain.xrDesc.usageFlags = chain.xrDesc.usageFlags;
                for (unsigned i = 0; i < 2; ++i) {
                    ComPtr<ID3D11Texture2D> image;
                    checkHr(sourceDevice->CreateTexture2D(&textureDesc, nullptr, image.GetAddressOf()));
                    nextChain.appSwapchain.images.push_back(std::move(image));
                }
                completedWait = false;
                const auto imported = runtime.getSwapchainImagesD3D12(nextChain, images, 2);
                runtime.flushD3D12CommandQueue();
                require(imported == XR_SUCCESS && allocatorCalls == 2 * expectedResets &&
                            listCalls == 2 * expectedResets && !invalidResetOrder,
                        "Repeated import did not reuse completed command storage");
            } else if (mode == L"noqueue" || mode == L"nofence") {
                if (mode == L"noqueue")
                    runtime.m_d3d12CommandQueue.Reset();
                else
                    runtime.m_d3d12Fence.Reset();
                runtime.flushD3D12CommandQueue();
                require(runtime.m_fenceValue == 0, "Inactive flush changed its fence counter");
                runtime.cleanupD3D12();
            } else if (mode == L"success") {
                Hooks hooks(queue.Get(), fence.Get());
                failure = Failure::None;
                runtime.flushD3D12CommandQueue();
                runtime.flushD3D12CommandQueue();
                require(signalCalls == 2 && eventCalls == 2 && waitCalls == 2 && signaledValue == 2 &&
                            fence->GetCompletedValue() >= 2,
                        "Repeated flush did not establish GPU completion");
                runtime.cleanupD3D12();
                require(!runtime.m_d3d12Device && !runtime.m_d3d12CommandQueue && !runtime.m_d3d12Fence,
                        "Completed cleanup retained its D3D12 resources");
            } else if (mode == L"signal" || mode == L"event" || mode == L"wait") {
                Hooks hooks(queue.Get(), fence.Get());
                failure = mode == L"signal" ? Failure::Signal : mode == L"event" ? Failure::Event : Failure::Wait;
                std::string message;
                try {
                    runtime.cleanupD3D12();
                } catch (const std::exception& error) {
                    message = error.what();
                }
                const bool retained = runtime.m_d3d12Device.Get() == device.Get() &&
                                      runtime.m_d3d12CommandQueue.Get() == queue.Get() &&
                                      runtime.m_d3d12Fence.Get() == fence.Get() && runtime.m_d3d12CommandAllocator &&
                                      runtime.m_d3d12CommandList;
                const auto signals = signalCalls, events = eventCalls, waits = waitCalls;
                const auto failedValue = signaledValue;
                // Drain and release real resources before reporting any assertion failure.
                failure = Failure::None;
                runtime.cleanupD3D12();
                require(retained, "Failed drain released resources without established GPU completion");
                require(!message.empty(), "Failed drain was reported as successful");
                require(signals == 1, "Failure did not exercise the real flush signal");
                require(signaledValue > failedValue && fence->GetCompletedValue() >= signaledValue,
                        "Retry did not drain with a fresh monotonic fence value");
                if (mode == L"signal") {
                    require(events == 0 && waits == 0 && message.find("Signal") != std::string::npos,
                            "Failed Signal reached event registration instead of reporting the Signal error");
                } else if (mode == L"event") {
                    require(events == 1 && waits == 0 && message.find("SetEventOnCompletion") != std::string::npos,
                            "Failed event registration entered a wait or hid its error");
                } else {
                    require(events == 1 && waits == 1, "Wait-failure injection missed the real flush event");
                }
                std::cout << "retained=" << retained << " signal=" << signals << " event=" << events
                          << " wait=" << waits << " error=" << message << '\n';
            } else if (mode == L"blocked") {
                ComPtr<ID3D12Fence> gate;
                checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gate.GetAddressOf())));
                wil::unique_handle waiting(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                wil::unique_handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                require(!!waiting && !!done, "Cannot create wait observation events");
                Hooks hooks(queue.Get(), fence.Get());
                failure = Failure::None;
                enteredWait = waiting.get();
                checkHr(queue->Wait(gate.Get(), 1));
                WatchThread mainInactive(false);
                std::exception_ptr error;
                std::thread worker([&] {
                    WatchThread active(true);
                    try {
                        runtime.cleanupD3D12();
                    } catch (...) {
                        error = std::current_exception();
                    }
                    SetEvent(done.get());
                });
                const bool reachedWait = originalWait(waiting.get(), 3000) == WAIT_OBJECT_0;
                const bool blocked = originalWait(done.get(), 100) == WAIT_TIMEOUT;
                const HRESULT released = gate->Signal(1);
                worker.join();
                checkHr(released);
                if (error)
                    std::rethrow_exception(error);
                require(reachedWait && blocked, "Cleanup completed before pending queue work was released");
                require(!runtime.m_d3d12Device && fence->GetCompletedValue() >= 1,
                        "Completed blocked cleanup did not drain and release resources");
            } else {
                throw std::runtime_error("Unknown fixture mode");
            }
            checkHr(device->GetDeviceRemovedReason());
            std::cout << "PASS: D3D12 flush " << xr::wide_to_utf8(mode) << '\n';
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::cout << std::unitbuf;
    try {
        require(argc == 2, "usage: nonnr_d3d12_flush_regression success|signal|event|wait|blocked|noqueue|nofence");
        virtualdesktop_openxr::RuntimeInputRegression::run(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
