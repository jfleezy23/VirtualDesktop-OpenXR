#pragma once

#include <detours.h>

#include <vector>

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
        if (self.failFull && self.first && queue == self.moduleQueue.Get() && fence == self.full.Get() &&
            value > self.primeValue) {
            self.failedFull = true;
            return E_FAIL;
        }
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
    bool failFull{}, failedFull{}, holdFirst{};

    explicit EarlyOutputGate(ID3D12Device* device) {
        check(!active, "one early signal observer");
        success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "tail gate");
        success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&firstGate)), "first signal gate");
        D3D12_COMMAND_QUEUE_DESC description{};
        ComPtr<ID3D12CommandQueue> probe;
        success(device->CreateCommandQueue(&description, IID_PPV_ARGS(&probe)), "signal observer queue");
        original = reinterpret_cast<Signal>((*reinterpret_cast<void***>(probe.Get()))[14]);
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
