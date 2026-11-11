// Real delivery-queue dependency; no completion values are fabricated.
extern "C" size_t deliveryContextSlot(unsigned);
extern "C" size_t deliveryFenceCompletedSlot();

struct LastCopyGate {
    using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
    using Completed = UINT64(STDMETHODCALLTYPE*)(ID3D11Fence*);
    inline static LastCopyGate* active{};
    Signal originalSignal{};
    Completed originalCompleted{};
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11Fence> held, marker;
    ComPtr<ID3D12Fence> releaseFence;
    std::atomic<ID3D11Fence*> consumer{};
    std::atomic<DWORD> workerThread{};
    ComPtr<ID3D11Fence> retainedConsumer;
    std::atomic<UINT64> firstValue{}, tailValue{};
    std::atomic<bool> observe{}, witnessed{}, currentReady{}, inserted{};
    unsigned signals{}, multiplier{};
    bool pendingCurrent{};
    wil::unique_handle snapshotEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    wil::unique_handle markerEvent{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    bool firstMarkerComplete{}, allMarkersComplete{}, pendingAtBegin{};

    static HRESULT STDMETHODCALLTYPE signal(ID3D11DeviceContext4* ctx, ID3D11Fence* fence, UINT64 value) {
        auto& self = *active;
        if (ctx != self.context.Get() || fence == self.marker.Get())
            return self.originalSignal(ctx, fence, value);
        if (!self.consumer.load()) {
            self.retainedConsumer = fence;
            self.workerThread = GetCurrentThreadId();
            self.consumer = fence;
        }
        if (fence != self.consumer.load())
            return self.originalSignal(ctx, fence, value);
        ++self.signals;
        if (self.signals == 2)
            self.firstValue = value;
        if (self.signals == self.multiplier)
            self.tailValue = value;
        auto result = self.originalSignal(ctx, fence, value);
        const unsigned before = self.pendingCurrent ? 1 : self.multiplier - 1;
        if (SUCCEEDED(result) && (self.pendingCurrent || self.multiplier > 2) && self.signals == before) {
            // B, S1, S2, S3 queue order. The inserted Wait is AFTER the
            // predecessor's Signal, BEFORE the held role's copy commands.
            result = ctx->Wait(self.held.Get(), 1);
            self.inserted = SUCCEEDED(result);
            ctx->Flush();
        }
        return result;
    }
    static UINT64 STDMETHODCALLTYPE completed(ID3D11Fence* fence) {
        auto& self = *active;
        const auto value = self.originalCompleted(fence);
        if (self.observe.load() && GetCurrentThreadId() == self.workerThread && fence == self.consumer.load() &&
            self.firstValue && self.tailValue && value != UINT64_MAX &&
            ((!self.pendingCurrent && self.multiplier == 2) || value < self.tailValue.load())) {
            self.currentReady = value >= self.firstValue.load();
            self.witnessed = true;
            SetEvent(self.snapshotEvent.get());
        }
        return value;
    }
    LastCopyGate(
        ID3D12Device* device12, ID3D11Device5* device11, ID3D11DeviceContext4* ctx, unsigned count, bool currentPending)
        : context(ctx), multiplier(count), pendingCurrent(currentPending) {
        require(!active && snapshotEvent && markerEvent, "one real last-copy gate with bounded events");
        checkHr(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&releaseFence)));
        wil::unique_handle shared;
        checkHr(device12->CreateSharedHandle(releaseFence.Get(), nullptr, GENERIC_ALL, nullptr, shared.put()));
        checkHr(device11->OpenSharedFence(shared.get(), IID_PPV_ARGS(&held)));
        checkHr(device11->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker)));
        originalSignal = reinterpret_cast<Signal>((*reinterpret_cast<void***>(ctx))[deliveryContextSlot(4)]);
        originalCompleted =
            reinterpret_cast<Completed>((*reinterpret_cast<void***>(marker.Get()))[deliveryFenceCompletedSlot()]);
        active = this;
        checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
        checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
        checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal)));
        checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalCompleted), completed)));
        checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
    }
    void release() noexcept {
        if (releaseFence && FAILED(releaseFence->Signal(1)))
            std::_Exit(3);
    }
    void mark(UINT64 value) {
        checkHr(context->Signal(marker.Get(), value));
        context->Flush();
    }
    bool waitMarker(UINT64 value, DWORD timeout) {
        checkHr(marker->SetEventOnCompletion(value, markerEvent.get()));
        const auto result = WaitForSingleObject(markerEvent.get(), timeout);
        require(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, "bounded actual End GPU completion event");
        const auto completedValue = marker->GetCompletedValue();
        require(completedValue != UINT64_MAX, "actual End marker device remains alive");
        return completedValue >= value;
    }
    ~LastCopyGate() {
        observe = false;
        release();
        // Owner destroys this only after joining the runtime worker. Drain
        // all submitted presentation work before releasing the GPU gate.
        // Earlier timed-out marker waits may still signal their old event.
        // A fresh event binds this drain exclusively to its final fence value.
        wil::unique_handle drainEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!drainEvent)
            std::_Exit(3);
        if (FAILED(context->Signal(marker.Get(), UINT64_MAX - 1)))
            std::_Exit(3);
        context->Flush();
        if (FAILED(marker->SetEventOnCompletion(UINT64_MAX - 1, drainEvent.get())) ||
            WaitForSingleObject(drainEvent.get(), 5000) != WAIT_OBJECT_0 ||
            marker->GetCompletedValue() != UINT64_MAX - 1)
            std::_Exit(3);
        if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal) ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalCompleted), completed) || DetourTransactionCommit())
            std::_Exit(3);
        active = nullptr;
    }
};
