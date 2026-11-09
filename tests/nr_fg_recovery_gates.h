// Isolated real GPU gates and observers for recovery regressions.
#pragma once

#include <detours.h>

#include <vector>
extern "C" void* fgTailQueueSignal(void*) noexcept;

// Test-only observer: discover the module queue from its prime's full signal,
// then gate precisely its distinct shared first-output signal. No global call
// ordinal or assumption about driver-internal NVIDIA signals is involved.
class EarlyOutputGate {
    using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    inline static EarlyOutputGate* active{};
    Signal original{};
    const DWORD owner = GetCurrentThreadId();
    std::vector<std::pair<ID3D12Fence*, ID3D12CommandQueue*>> signals;
    ComPtr<ID3D12Fence> gate, firstGate;
    ComPtr<ID3D12Fence> full, consumed;
    uint64_t primeValue{};

    static HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
        auto& self = *active;
        if (GetCurrentThreadId() != self.owner)
            return self.original(queue, fence, value);
        ComPtr<ID3D12Fence1> description;
        const bool firstSignal = self.moduleQueue && queue == self.moduleQueue.Get() && !self.first &&
                                 fence != self.full.Get() && fence != self.consumed.Get() &&
                                 SUCCEEDED(fence->QueryInterface(IID_PPV_ARGS(&description))) &&
                                 (description->GetCreationFlags() & D3D12_FENCE_FLAG_SHARED);
        if (firstSignal && self.holdFirst) {
            const auto hr = queue->Wait(self.firstGate.Get(), 1);
            if (FAILED(hr))
                return hr;
        }
        const HRESULT hr = self.original(queue, fence, value);
        if (FAILED(hr))
            return hr;
        if (!self.moduleQueue) {
            self.signals.emplace_back(fence, queue);
            return hr;
        }
        if (firstSignal) {
            self.first = fence;
            self.firstValue = value;
            return queue->Wait(self.gate.Get(), 1);
        }
        return hr;
    }

    void attach(bool enable) {
        LONG error = DetourTransactionBegin();
        const bool started = error == NO_ERROR;
        if (!error)
            error = DetourUpdateThread(GetCurrentThread());
        if (!error)
            error = (enable ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&original), signal);
        if (!error)
            error = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(error), "early signal observer");
    }

  public:
    ComPtr<ID3D12CommandQueue> moduleQueue;
    ComPtr<ID3D12Fence> first;
    UINT64 firstValue{};
    bool holdFirst{};

    explicit EarlyOutputGate(ID3D12Device* device) {
        check(!active, "one early signal observer");
        success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "tail gate");
        success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&firstGate)), "first signal gate");
        D3D12_COMMAND_QUEUE_DESC description{};
        ComPtr<ID3D12CommandQueue> probe;
        success(device->CreateCommandQueue(&description, IID_PPV_ARGS(&probe)), "signal observer queue");
        original = reinterpret_cast<Signal>(fgTailQueueSignal(probe.Get()));
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }

    void arm(const VDXRFG_TicketV1& prime) {
        full = static_cast<ID3D12Fence*>(prime.output_ready_fence);
        consumed = static_cast<ID3D12Fence*>(prime.input_consumed_fence);
        primeValue = prime.output_ready_value;
        for (const auto& entry : signals)
            if (entry.first == full.Get())
                moduleQueue = entry.second;
        check(moduleQueue != nullptr, "prime's full signal identifies the module queue");
        signals.clear();
    }

    // Only rearm after the previous group and its queue are fully drained.
    void rearm(const VDXRFG_TicketV1& previous) {
        check(moduleQueue != nullptr, "rearm preserves the actual module queue");
        full = static_cast<ID3D12Fence*>(previous.output_ready_fence);
        consumed = static_cast<ID3D12Fence*>(previous.input_consumed_fence);
        primeValue = previous.output_ready_value;
        first.Reset();
        firstValue = 0;
        success(gate->Signal(1), "release preceding tail gate");
        success(firstGate->Signal(1), "release preceding first gate");
        gate.Reset();
        firstGate.Reset();
        ComPtr<ID3D12Device> device;
        success(moduleQueue->GetDevice(IID_PPV_ARGS(&device)), "rearm device");
        success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "rearm tail gate");
        success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&firstGate)), "rearm first gate");
    }

    void release() noexcept {
        firstGate->Signal(1);
        gate->Signal(1);
    }

    void releaseFirst() noexcept {
        firstGate->Signal(1);
    }

    ~EarlyOutputGate() {
        release();
        try {
            attach(false);
        } catch (...) {
            std::terminate();
        }
        active = nullptr;
    }
};

#include <d3d11_4.h>
#include <cstdlib>

// Declare AFTER every resource touched by the held pair. A failed RED assertion
// releases the test gates and drains both real GPU domains before those owners
// unwind. Signal failure, device removal and timeouts terminate without unwind.
struct PrefixWorkGuard {
    Gpu& gpu;
    EarlyOutputGate& gate;
    ID3D11Device5* device11{};
    ID3D11DeviceContext4* context11{};
    bool armed{};
    PrefixWorkGuard(Gpu& g, EarlyOutputGate& held, ID3D11Device5* d = nullptr, ID3D11DeviceContext4* c = nullptr)
        : gpu(g), gate(held), device11(d), context11(c) {
    }
    [[noreturn]] static void fatal() noexcept {
        std::fputs("FATAL: test cleanup cannot prove GPU drain; no resource unwind\n", stderr);
        std::_Exit(3);
    }
    void drain() noexcept {
        if (!armed)
            return;
        gate.release();
        try {
            if (!gate.moduleQueue)
                fatal();
            const auto value = gpu.nextFence++;
            if (FAILED(gate.moduleQueue->Signal(gpu.fence.Get(), value)))
                fatal();
            gpu.wait(gpu.fence.Get(), value);
            if (context11) {
                ComPtr<ID3D11Fence> completion;
                if (!device11 || FAILED(device11->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion))))
                    fatal();
                const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event)
                    fatal();
                if (FAILED(context11->Signal(completion.Get(), 1)))
                    fatal();
                context11->Flush();
                if (FAILED(completion->SetEventOnCompletion(1, event)))
                    fatal();
                const auto waited = WaitForSingleObject(event, 10000);
                CloseHandle(event);
                if (waited != WAIT_OBJECT_0)
                    fatal();
                const auto completed = completion->GetCompletedValue();
                if (completed == UINT64_MAX || completed < 1)
                    fatal();
            }
            armed = false;
        } catch (...) {
            fatal();
        }
    }
    ~PrefixWorkGuard() {
        drain();
    }
};

extern "C" void* prefixFenceEventAddress(void*) noexcept;
extern "C" void* recoveryFenceCompletedAddress(void*) noexcept;

// Observe real worker registration on distinct imported readiness fences. No
// completion values, HRESULTs or event state are replaced by this observer.
class RecoveryFenceEvents {
    using Event = HRESULT(STDMETHODCALLTYPE*)(ID3D11Fence*, UINT64, HANDLE);
    using Completed = UINT64(STDMETHODCALLTYPE*)(ID3D11Fence*);
    inline static RecoveryFenceEvents* active{};
    Event original{};
    Completed originalCompleted{};
    const DWORD controller = GetCurrentThreadId();
    std::mutex mutex;
    std::condition_variable changed;
    bool watching{};
    UINT64 target{};
    std::vector<ID3D11Fence*> fences;
    ID3D11Fence* sampledFence{};
    unsigned pendingSnapshots{};
    static UINT64 STDMETHODCALLTYPE completed(ID3D11Fence* fence) {
        auto& self = *active;
        const auto value = self.originalCompleted(fence);
        if (GetCurrentThreadId() != self.controller) {
            std::lock_guard lock(self.mutex);
            if (self.watching && fence == self.sampledFence && value < self.target) {
                ++self.pendingSnapshots;
                self.changed.notify_all();
            }
        }
        return value;
    }
    static HRESULT STDMETHODCALLTYPE event(ID3D11Fence* fence, UINT64 value, HANDLE handle) {
        auto& self = *active;
        const auto result = self.original(fence, value, handle);
        if (SUCCEEDED(result) && GetCurrentThreadId() != self.controller) {
            std::lock_guard lock(self.mutex);
            if (self.watching && value == self.target &&
                std::find(self.fences.begin(), self.fences.end(), fence) == self.fences.end()) {
                self.fences.push_back(fence);
                self.changed.notify_all();
            }
        }
        return result;
    }
    void attach(bool enable) {
        checkDetour(DetourTransactionBegin());
        checkDetour(DetourUpdateThread(GetCurrentThread()));
        checkDetour((enable ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&original), event));
        checkDetour((enable ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&originalCompleted), completed));
        checkDetour(DetourTransactionCommit());
    }

  public:
    explicit RecoveryFenceEvents(ID3D11Device5* device) {
        require(!active, "one recovery event observer");
        ComPtr<ID3D11Fence> sample;
        success(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&sample)), "event observer fence");
        original = reinterpret_cast<Event>(prefixFenceEventAddress(sample.Get()));
        originalCompleted = reinterpret_cast<Completed>(recoveryFenceCompletedAddress(sample.Get()));
        active = this;
        attach(true);
    }
    void watch(UINT64 value) {
        std::lock_guard lock(mutex);
        fences.clear();
        target = value;
        watching = true;
        sampledFence = nullptr;
        pendingSnapshots = 0;
    }
    void stop() {
        std::lock_guard lock(mutex);
        watching = false;
    }
    bool waitCount(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return fences.size() >= count; });
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return fences.size();
    }
    void sampleLastPendingFence() {
        std::lock_guard lock(mutex);
        require(!fences.empty(), "actual full-fence event identifies pending snapshot source");
        sampledFence = fences.back();
        pendingSnapshots = 0;
    }
    bool waitPendingSnapshots(unsigned count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return pendingSnapshots >= count; });
    }
    ~RecoveryFenceEvents() {
        stop();
        try {
            attach(false);
        } catch (...) {
            std::_Exit(3);
        }
        active = nullptr;
    }
};

extern "C" void* recoveryConsumerSignalAddress(void*) noexcept;

// Actual copy-consumer completion is held by a real cross-API GPU fence Wait.
// All Signal calls and completion values are forwarded; none is fabricated.
class RecoveryConsumerGate {
    using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
    inline static RecoveryConsumerGate* active{};
    Signal original{};
    ID3D11DeviceContext4* context{};
    ID3D11Fence* presented{};
    const DWORD controller = GetCurrentThreadId();
    std::mutex mutex;
    ComPtr<ID3D11Fence> consumer, wait11;
    ComPtr<ID3D12Fence> release12;
    UINT64 lastValue{}, heldValue{};
    double expiredTime{};
    std::atomic<double>* clock{};
    bool armed{};
    static HRESULT STDMETHODCALLTYPE signal(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
        auto& self = *active;
        if (context != self.context || fence == self.presented || GetCurrentThreadId() == self.controller)
            return self.original(context, fence, value);
        std::lock_guard lock(self.mutex);
        // This fixture's worker has only FG consumer Signals and the explicitly
        // excluded End marker. Source producer Signals occur on the controller.
        if (!self.consumer)
            self.consumer = fence;
        if (fence != self.consumer.Get())
            return self.original(context, fence, value);
        self.lastValue = value;
        if (!self.armed || value != self.heldValue)
            return self.original(context, fence, value);
        self.armed = false;
        auto result = context->Wait(self.wait11.Get(), 1);
        if (FAILED(result))
            return result;
        result = self.original(context, fence, value);
        if (FAILED(result))
            return result;
        const auto completed = fence->GetCompletedValue();
        self.pendingWitness = completed != UINT64_MAX && completed < value;
        self.inserted = true;
        self.clock->store(self.expiredTime);
        std::fprintf(stdout,
                     "CONSUMER_GATE signal=%llu completed=%llu pending=%u\n",
                     static_cast<unsigned long long>(value),
                     static_cast<unsigned long long>(completed),
                     self.pendingWitness.load() ? 1u : 0u);
        return result;
    }
    void attach(bool enable) {
        checkDetour(DetourTransactionBegin());
        checkDetour(DetourUpdateThread(GetCurrentThread()));
        checkDetour((enable ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&original), signal));
        checkDetour(DetourTransactionCommit());
    }

  public:
    std::atomic<bool> inserted{}, pendingWitness{};
    RecoveryConsumerGate(ID3D12Device* device12,
                         ID3D11Device5* device11,
                         ID3D11DeviceContext4* source,
                         ID3D11Fence* endMarker)
        : context(source), presented(endMarker) {
        require(!active, "one consumer Signal observer");
        success(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&release12)), "consumer gate fence");
        HANDLE shared{};
        success(device12->CreateSharedHandle(release12.Get(), nullptr, GENERIC_ALL, nullptr, &shared),
                "export consumer gate");
        const auto opened = device11->OpenSharedFence(shared, IID_PPV_ARGS(&wait11));
        CloseHandle(shared);
        success(opened, "import real consumer gate");
        original = reinterpret_cast<Signal>(recoveryConsumerSignalAddress(source));
        active = this;
        attach(true);
    }
    void armAfterRealB(double time, std::atomic<double>& controlledClock) {
        std::lock_guard lock(mutex);
        require(consumer && lastValue && consumer->GetCompletedValue() >= lastValue,
                "preceding group identifies and completes the actual consumer fence");
        // Per-group semantic order is real B then S1. This is not a global
        // COM-call ordinal: identity/value come from the completed real group.
        heldValue = lastValue + 2;
        expiredTime = time;
        clock = &controlledClock;
        armed = true;
    }
    void release() noexcept {
        if (release12)
            (void)release12->Signal(1);
    }
    ~RecoveryConsumerGate() {
        release();
        try {
            attach(false);
        } catch (...) {
            std::_Exit(3);
        }
        active = nullptr;
    }
};
