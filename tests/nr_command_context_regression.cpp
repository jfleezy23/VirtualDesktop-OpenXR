// Native command completion/error ownership controls. No headset or settings changes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <winrt/base.h>
#include <detours.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <d3dx12.h>

using Microsoft::WRL::ComPtr;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void checkHr(HRESULT value) {
    require(SUCCEEDED(value), "D3D12 call failed");
}
#define CHECK_HRCMD(expression) checkHr(expression)
#define CHECK_MSG(condition, message) require(bool(condition), message)
#include "../virtualdesktop-openxr/D3D12Utils.h"

namespace {
    enum class Failure { None, Signal, Event, Wait, Removed };
    std::atomic<Failure> failure{Failure::None};
    ID3D12CommandQueue* watchedQueue;
    ID3D12Fence* watchedFence;
    ID3D12Device* watchedDevice;
    std::atomic<unsigned> signalCalls{}, eventCalls{}, waitCalls{};
    std::atomic<UINT64> lastSignalValue{};
    std::atomic<bool> enteredWait{};
    bool removeAfterWait{};
    std::atomic<bool> postwaitInjected{};
    std::atomic<HANDLE> watchedEvent{};
    using QueueSignal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    using FenceEvent = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
    using FenceValue = UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*);
    using CreateAllocator = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**);
    QueueSignal originalSignal;
    FenceEvent originalEvent;
    FenceValue originalValue;
    CreateAllocator originalAllocator;
    decltype(&WaitForSingleObject) originalWait = WaitForSingleObject;
    const GUID probeId = {0xc4771402, 0xc41a, 0x45df, {0xb3, 0x77, 0x50, 0x98, 0x27, 0xe5, 0x13, 0xf7}};
    struct Lifetimes {
        std::atomic<unsigned> lists{}, allocators{};
    };
    std::shared_ptr<Lifetimes> lifetimes;
    class Probe final : public IUnknown {
      public:
        Probe(std::shared_ptr<Lifetimes> state, bool allocator) : m_state(std::move(state)), m_allocator(allocator) {
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
            if (!result)
                return E_POINTER;
            *result = nullptr;
            if (iid != __uuidof(IUnknown))
                return E_NOINTERFACE;
            *result = this;
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {
            return ++m_references;
        }
        ULONG STDMETHODCALLTYPE Release() override {
            const ULONG result = --m_references;
            if (!result) {
                if (m_allocator)
                    ++m_state->allocators;
                else
                    ++m_state->lists;
                delete this;
            }
            return result;
        }

      private:
        std::atomic<ULONG> m_references{1};
        std::shared_ptr<Lifetimes> m_state;
        bool m_allocator;
    };
    void observe(ID3D12Object* object, bool allocator) {
        auto* probe = new Probe(lifetimes, allocator);
        const auto result = object->SetPrivateDataInterface(probeId, probe);
        probe->Release();
        checkHr(result);
    }
    HRESULT STDMETHODCALLTYPE createAllocator(ID3D12Device* device,
                                              D3D12_COMMAND_LIST_TYPE type,
                                              REFIID iid,
                                              void** result) {
        const auto status = originalAllocator(device, type, iid, result);
        if (device == watchedDevice && SUCCEEDED(status) && iid == __uuidof(ID3D12CommandAllocator))
            observe(static_cast<ID3D12CommandAllocator*>(*result), true);
        return status;
    }
    HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
        if (queue == watchedQueue && fence == watchedFence) {
            ++signalCalls;
            lastSignalValue = value;
            if (failure == Failure::Signal)
                return E_FAIL;
        }
        return originalSignal(queue, fence, value);
    }
    HRESULT STDMETHODCALLTYPE event(ID3D12Fence* fence, UINT64 value, HANDLE handle) {
        if (fence == watchedFence) {
            ++eventCalls;
            watchedEvent = handle;
            if (failure == Failure::Event)
                return E_OUTOFMEMORY;
        }
        return originalEvent(fence, value, handle);
    }
    UINT64 STDMETHODCALLTYPE completed(ID3D12Fence* fence) {
        if (fence == watchedFence && failure == Failure::Removed)
            return UINT64_MAX;
        return originalValue(fence);
    }
    DWORD WINAPI wait(HANDLE handle, DWORD timeout) {
        if (!watchedEvent.load() || handle != watchedEvent.load())
            return originalWait(handle, timeout);
        ++waitCalls;
        enteredWait = true;
        if (failure == Failure::Wait) {
            SetLastError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        const auto result = originalWait(handle, std::min<DWORD>(timeout, 5000));
        if (result == WAIT_OBJECT_0 && removeAfterWait) {
            postwaitInjected = true;
            failure = Failure::Removed;
        }
        return result;
    }
    class Hooks {
      public:
        Hooks(ID3D12Device* device, D3D12Utils::CommandContext& context) {
            watchedDevice = device;
            watchedQueue = context.GetCommandQueue();
            watchedFence = context.GetCompletionFence();
            originalSignal = reinterpret_cast<QueueSignal>((*reinterpret_cast<void***>(watchedQueue))[14]);
            originalEvent = reinterpret_cast<FenceEvent>((*reinterpret_cast<void***>(watchedFence))[9]);
            originalValue = reinterpret_cast<FenceValue>((*reinterpret_cast<void***>(watchedFence))[8]);
            originalAllocator = reinterpret_cast<CreateAllocator>((*reinterpret_cast<void***>(device))[9]);
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            LONG status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalValue), completed);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalAllocator), createAllocator);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            require(status == NO_ERROR, "Native hook installation failed");
        }
        ~Hooks() {
            failure = Failure::None;
            LONG status = DetourTransactionBegin();
            if (status == NO_ERROR)
                status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal);
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalValue), completed);
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalAllocator), createAllocator);
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            if (status != NO_ERROR)
                std::terminate();
            watchedDevice = nullptr;
            watchedQueue = nullptr;
            watchedFence = nullptr;
            watchedEvent = nullptr;
        }
        Hooks(const Hooks&) = delete;
        Hooks& operator=(const Hooks&) = delete;
    };
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        require(argc == 2, "Usage: nr_command_context_regression signal|event|wait|removed|postwait|complete");
        const std::string mode(argv[1]);
        require(mode == "signal" || mode == "event" || mode == "wait" || mode == "removed" || mode == "postwait" ||
                    mode == "complete",
                "Unknown mode");
        ComPtr<ID3D12Device> device;
        checkHr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        lifetimes = std::make_shared<Lifetimes>();
        bool submitRejected{}, flushRejected{}, ownersRetained{}, recoveryComplete{}, blocked{}, completedFastpath{},
            removedRejected{}, reused{}, failedSignalDidNotWait{};
        unsigned postwaitEvents{}, postwaitWaits{};
        {
            D3D12Utils::CommandContext context(device.Get(), L"NR completion regression");
            Hooks hooks(device.Get(), context);
            ComPtr<ID3D12Fence> gate;
            checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
            checkHr(context.GetCommandQueue()->Wait(gate.Get(), 1));
            auto commands = context.GetCommandList();
            auto* originalList = commands.Commands.Get();
            observe(commands.Commands.Get(), false);
            if (mode == "signal")
                failure = Failure::Signal;
            try {
                context.SubmitCommandList(std::move(commands));
            } catch (const std::exception&) {
                submitRejected = true;
            }
            if (mode == "signal") {
                try {
                    context.Flush();
                } catch (const std::exception&) {
                    flushRejected = true;
                }
                failedSignalDidNotWait = eventCalls == 0 && waitCalls == 0;
            }
            if (mode == "event" || mode == "wait" || mode == "removed") {
                failure = mode == "event" ? Failure::Event : mode == "wait" ? Failure::Wait : Failure::Removed;
                try {
                    context.Flush();
                } catch (const std::exception&) {
                    flushRejected = true;
                }
            }
            ownersRetained = lifetimes->lists == 0 && lifetimes->allocators == 0;
            failure = Failure::None;
            enteredWait = false;
            removeAfterWait = mode == "postwait";
            std::atomic<bool> returned{};
            std::thread drain([&] {
                try {
                    context.Flush();
                    recoveryComplete = true;
                } catch (const std::exception&) {
                    recoveryComplete = false;
                }
                returned = true;
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!enteredWait && !returned && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            blocked = enteredWait && !returned;
            // Always release the GPU gate and join before making assertions or unwinding the context.
            const auto gateStatus = gate->Signal(1);
            drain.join();
            postwaitEvents = eventCalls;
            postwaitWaits = waitCalls;
            removeAfterWait = false;
            failure = Failure::None;
            checkHr(gateStatus);
            // Repair an unaccepted baseline signal on the real GPU queue, never by CPU-signaling completion.
            if (!recoveryComplete) {
                checkHr(context.GetCommandQueue()->Signal(context.GetCompletionFence(), lastSignalValue));
                context.Flush();
            }
            checkHr(device->GetDeviceRemovedReason());
            const auto before = eventCalls.load();
            context.Flush();
            completedFastpath = before == eventCalls;
            if (mode == "removed") {
                failure = Failure::Removed;
                try {
                    context.GetCommandList();
                } catch (const std::exception&) {
                    removedRejected = true;
                }
                failure = Failure::None;
            }
            auto recycled = context.GetCommandList();
            reused = recycled.Commands.Get() == originalList;
        }
        std::cout << "native mode=" << mode << " submitRejected=" << submitRejected
                  << " flushRejected=" << flushRejected << " retained=" << ownersRetained << " blocked=" << blocked
                  << " recovery=" << recoveryComplete << " released=" << lifetimes->lists << ','
                  << lifetimes->allocators << " completedFastpath=" << completedFastpath
                  << " removedRejected=" << removedRejected << " reused=" << reused
                  << " failedSignalDidNotWait=" << failedSignalDidNotWait << '\n';
        require(ownersRetained, "Submitted command owners were released before GPU completion");
        require(blocked, "Drain failed to wait for its real GPU queue");
        if (mode == "signal")
            require(submitRejected && flushRejected && recoveryComplete && failedSignalDidNotWait,
                    "Failed Signal was published, ignored during drain, or recovery never queued completion");
        if (mode == "event" || mode == "wait" || mode == "removed")
            require(flushRejected && recoveryComplete, "Completion failure was ignored or retry failed");
        if (mode == "removed")
            require(removedRejected, "Device removal was accepted as allocator reuse completion");
        if (mode == "postwait")
            require(postwaitInjected && postwaitEvents == 1 && postwaitWaits == 1 && !recoveryComplete,
                    "A real successful wait did not inject/reject device removal");
        require(reused || mode == "removed", "Recovery lost the originally submitted command owner");
        require(lifetimes->lists == 1 && lifetimes->allocators == 1, "Completed command owners were not released");
        if (mode == "complete")
            require(completedFastpath, "Already-completed drain still registered a wait event");
        std::cout << "PASS: native NR completion " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
