// Real GPU output through the production native presenter. OVRNull's timing
// and presentation entrypoints are intercepted; no headset is claimed here.
#define wmain retainedAsyncFixtureMain
#include "nonnr_async_regression.cpp"
#undef wmain
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main

namespace {
    std::wstring allocationFault;
    unsigned allocatedChains{}, destroyedChains{}, allocationAttempts{}, injectedAllocationFailures{};
    unsigned allocatedImages{};
    unsigned allocationFailureAt{3};
    uint64_t allocatedTexelBytes{};
    decltype(&ovr_CreateTextureSwapChainDX) originalAllocation;
    decltype(&ovr_GetTextureSwapChainLength) originalLength;
    decltype(&ovr_GetTextureSwapChainBufferDX) originalBuffer;
    decltype(&ovr_DestroyTextureSwapChain) originalDestroy;
    ovrResult OVR_CDECL failAllocation(ovrSession session,
                                       IUnknown* device,
                                       const ovrTextureSwapChainDesc* desc,
                                       ovrTextureSwapChain* chain) {
        ++allocationAttempts;
        if (allocationFault == L"allocation-create" && allocationAttempts == allocationFailureAt) {
            ++injectedAllocationFailures;
            return ovrError_MemoryAllocationFailure;
        }
        // BackendHooks replaces allocation with IDs for nongraphics fixtures.
        // Use its original trampoline here so length/buffer inspect real chains.
        const auto result = originalCreateChain(session, device, desc, chain);
        if (OVR_SUCCESS(result)) {
            ++allocatedChains;
        }
        return result;
    }
    ovrResult OVR_CDECL failLength(ovrSession session, ovrTextureSwapChain chain, int* count) {
        if (allocatedChains == allocationFailureAt && allocationFault == L"allocation-length") {
            ++injectedAllocationFailures;
            return ovrError_RuntimeException;
        }
        if (allocatedChains == allocationFailureAt && allocationFault == L"allocation-empty") {
            ++injectedAllocationFailures;
            *count = 0;
            return ovrSuccess;
        }
        return originalLength(session, chain, count);
    }
    ovrResult OVR_CDECL failBuffer(ovrSession session, ovrTextureSwapChain chain, int index, IID iid, void** image) {
        if (allocatedChains == allocationFailureAt && allocationFault == L"allocation-buffer") {
            ++injectedAllocationFailures;
            return ovrError_RuntimeException;
        }
        const auto result = originalBuffer(session, chain, index, iid, image);
        if (OVR_SUCCESS(result)) {
            ComPtr<ID3D11Texture2D> texture;
            success(static_cast<IUnknown*>(*image)->QueryInterface(IID_PPV_ARGS(&texture)), "allocated SDK texture");
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            require(desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1 &&
                        (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                         desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
                    "presentation allocation accounting requires an RGBA8 stereo atlas");
            ++allocatedImages;
            allocatedTexelBytes += uint64_t(desc.Width) * desc.Height * 4;
        }
        return result;
    }
    void OVR_CDECL countDestroy(ovrSession session, ovrTextureSwapChain chain) {
        ++destroyedChains;
        originalDestroyChain(session, chain);
    }
    struct AllocationHooks {
        AllocationHooks(const std::wstring& scenario, unsigned multiplier) {
            allocationFault = scenario;
            const bool last = scenario.find(L"-last") != std::wstring::npos;
            if (last)
                allocationFault.resize(allocationFault.size() - 5);
            allocationFailureAt = last ? multiplier + 1 : 3;
            allocatedChains = destroyedChains = allocationAttempts = injectedAllocationFailures = 0;
            allocatedImages = 0;
            allocatedTexelBytes = 0;
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalAllocation =
                reinterpret_cast<decltype(originalAllocation)>(GetProcAddress(module, "ovr_CreateTextureSwapChainDX"));
            originalLength =
                reinterpret_cast<decltype(originalLength)>(GetProcAddress(module, "ovr_GetTextureSwapChainLength"));
            originalBuffer =
                reinterpret_cast<decltype(originalBuffer)>(GetProcAddress(module, "ovr_GetTextureSwapChainBufferDX"));
            originalDestroy =
                reinterpret_cast<decltype(originalDestroy)>(GetProcAddress(module, "ovr_DestroyTextureSwapChain"));
            require(originalAllocation && originalLength && originalBuffer && originalDestroy,
                    "allocation exports required");
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalAllocation), failAllocation));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalLength), failLength));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalBuffer), failBuffer));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalDestroy), countDestroy));
            checkDetour(DetourTransactionCommit());
        }
        ~AllocationHooks() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalAllocation), failAllocation) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalLength), failLength) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalBuffer), failBuffer) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDestroy), countDestroy) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
        }
    };
    std::atomic<double> capacityClockTime{};
    decltype(&ovr_GetTimeInSeconds) originalCapacityTime{};
    double OVR_CDECL capacityTime() {
        return capacityClockTime.load();
    }
    // Capacity admission deliberately parks End and schedules another thread.
    // Its correctness must not depend on CPU readback/scheduler overhead fitting
    // inside the synthetic deadline. Native Wait advances this fixture clock;
    // GPU completion and fixture watchdogs still use real bounded waits.
    struct CapacityClockHook {
        bool enabled{};
        bool monotonic{};
        double origin{};
        explicit CapacityClockHook(bool useClock, bool neverReverse = false)
            : enabled(useClock), monotonic(neverReverse) {
            if (!enabled)
                return;
            originalCapacityTime = reinterpret_cast<decltype(originalCapacityTime)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_GetTimeInSeconds"));
            require(originalCapacityTime != nullptr, "capacity clock export required");
            origin = originalCapacityTime();
            capacityClockTime = origin;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalCapacityTime), capacityTime));
            checkDetour(DetourTransactionCommit());
        }
        void advance(long long nativeId) const {
            if (enabled)
                capacityClockTime =
                    monotonic ? std::max(capacityClockTime.load(), origin + nativeId * .01) : origin + nativeId * .01;
        }
        ~CapacityClockHook() {
            if (enabled &&
                (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                 DetourDetach(reinterpret_cast<PVOID*>(&originalCapacityTime), capacityTime) != NO_ERROR ||
                 DetourTransactionCommit() != NO_ERROR)) {
                std::_Exit(2);
            }
        }
    };
    // Observe the COM boundary: LTCG can inline copyReady and waitForCopy.
    // The fixture settles real GPU work with an event before forwarding its
    // first observed read. This measures queries, not native frame latency.
    struct CopyReadyQueryProbe {
        enum class Mode {
            Complete,
            PendingReady,
            BusyFinalReady,
            StaleWake,
            LateReady,
            StopReady,
            WaitError,
            BeginReady,
            BeginPending
        };
        using Completed = UINT64(STDMETHODCALLTYPE*)(ID3D11Fence*);
        using Event = HRESULT(STDMETHODCALLTYPE*)(ID3D11Fence*, UINT64, HANDLE);
        using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
        inline static Completed originalCompleted{};
        inline static Event originalEvent{};
        inline static Signal originalSignal{};
        inline static CopyReadyQueryProbe* active{};
        inline static thread_local bool settling{};
        ID3D11DeviceContext4* context{};
        ID3D11Fence* consumer{};
        void* completedEntry{};
        uint64_t latestValue{};
        std::atomic<DWORD> worker{};
        long long nativeId{-1}, lastEnd{-1};
        unsigned multiplier{}, endCount{}, workerSignals{}, unexpectedSignals{}, unclassifiedReads{};
        bool slotOpen{}, coveredFence{};
        bool enabled{};
        Mode mode{Mode::Complete};
        std::function<void()> afterWake;
        ComPtr<ID3D11Fence> pendingGate;
        ComPtr<ID3D12Fence> pendingRelease;
        bool gated{};
        unsigned actualPendingReads{}, eventReleases{}, pendingAtRegistration{};
        wil::unique_handle event;
        std::array<unsigned, 8> slotReads{}, completeReads{}, settles{}, waitEvents{}, pendingReads{};
        std::array<uint64_t, 8> waitValues{};
        std::array<unsigned, 2> retirementReads{};
        HRESULT settleFailure{S_OK};
        unsigned delayedSlot{};
        bool controlledBegin{}, finalBDue{}, multiLate{}, hardAfterSoft{}, retainLateWait{};
        std::array<bool, 8> readyBeforeBeginReturn{};

        bool beginMode() const {
            return mode == Mode::BeginReady || mode == Mode::BeginPending;
        }
        void settleInsideBegin(long long id) {
            require(consumer && id >= 0 && id < 8, "controlled Begin has an actual consumer fence");
            settling = true;
            const auto registered = consumer->SetEventOnCompletion(latestValue, event.get());
            const auto waited = SUCCEEDED(registered) ? WaitForSingleObject(event.get(), 5000) : WAIT_FAILED;
            const auto completed = originalCompleted(consumer);
            settling = false;
            success(registered, "settle actual prepared copy inside controlled Begin");
            require(waited == WAIT_OBJECT_0 && completed != UINT64_MAX && completed >= latestValue,
                    "copy is physically complete before controlled Begin returns late");
            readyBeforeBeginReturn[static_cast<unsigned>(id)] = true;
        }

        static HRESULT STDMETHODCALLTYPE registerEvent(ID3D11Fence* fence, UINT64 value, HANDLE handle) {
            auto* probe = active;
            const bool observed = probe && !settling && GetCurrentThreadId() == probe->worker.load() &&
                                  fence == probe->consumer && probe->slotOpen && probe->nativeId >= 0 &&
                                  probe->nativeId < 2 * probe->multiplier;
            if (observed) {
                const auto index = static_cast<unsigned>(probe->nativeId);
                ++probe->waitEvents[index];
                probe->waitValues[index] = value;
                if (probe->mode == Mode::WaitError)
                    return E_FAIL;
            }
            const auto result = originalEvent(fence, value, handle);
            if (observed && probe->pendingRelease && probe->nativeId == probe->multiplier && SUCCEEDED(result) &&
                !probe->eventReleases) {
                const auto completed = originalCompleted(fence);
                probe->pendingAtRegistration += completed != UINT64_MAX && completed < value;
                const auto released = probe->pendingRelease->Signal(1);
                if (FAILED(released))
                    probe->settleFailure = released;
                ++probe->eventReleases;
            }
            return result;
        }

        static HRESULT STDMETHODCALLTYPE signal(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
            auto* probe = active;
            if (probe && context == probe->context && GetCurrentThreadId() == probe->worker.load()) {
                if (!probe->consumer) {
                    probe->consumer = fence;
                    probe->coveredFence = (*reinterpret_cast<void***>(fence))[8] == probe->completedEntry;
                }
                if (probe->consumer == fence) {
                    ++probe->workerSignals;
                    probe->latestValue = value;
                    // Hold the first pair's real and synthetic consumer signals
                    // until waitForCopy registers its actual completion event.
                    if (probe->pendingGate && probe->workerSignals == 2) {
                        const auto blocked = context->Wait(probe->pendingGate.Get(), 1);
                        if (FAILED(blocked))
                            probe->settleFailure = blocked;
                        else
                            probe->gated = true;
                    }
                } else {
                    ++probe->unexpectedSignals;
                }
            }
            return originalSignal(context, fence, value);
        }
        static UINT64 STDMETHODCALLTYPE completed(ID3D11Fence* fence) {
            auto* probe = active;
            if (!probe || settling || GetCurrentThreadId() != probe->worker.load() || fence != probe->consumer)
                return originalCompleted(fence);
            const bool slot = probe->slotOpen && probe->nativeId >= 0 && probe->nativeId < 2 * probe->multiplier;
            if (slot) {
                const auto index = static_cast<unsigned>(probe->nativeId);
                if (++probe->slotReads[index] == 1 && !(probe->pendingGate && probe->nativeId == probe->multiplier)) {
                    ++probe->settles[index];
                    settling = true;
                    const auto registered = fence->SetEventOnCompletion(probe->latestValue, probe->event.get());
                    const auto waited =
                        SUCCEEDED(registered) ? WaitForSingleObject(probe->event.get(), 5000) : WAIT_FAILED;
                    settling = false;
                    if (FAILED(registered) || waited != WAIT_OBJECT_0)
                        probe->settleFailure = FAILED(registered) ? registered : E_FAIL;
                }
            } else if (probe->lastEnd >= 0 && probe->lastEnd < 2 * probe->multiplier) {
                ++probe->retirementReads[probe->lastEnd / probe->multiplier];
            } else {
                ++probe->unclassifiedReads;
            }
            const auto result = originalCompleted(fence);
            if (slot) {
                const auto index = static_cast<unsigned>(probe->nativeId);
                const auto read = probe->slotReads[index];
                if (probe->pendingGate && probe->nativeId == probe->multiplier && read <= 2 && result != UINT64_MAX &&
                    result < probe->latestValue)
                    ++probe->actualPendingReads;
                if (probe->mode == Mode::BeginPending && probe->nativeId == probe->multiplier + probe->delayedSlot) {
                    ++probe->pendingReads[index];
                    return 0; // Scripted negative control, not a physically held GPU fence.
                }
                if (probe->mode != Mode::Complete && !probe->beginMode()) {
                    if (read == 3 && probe->afterWake)
                        probe->afterWake();
                    const bool pending = read <= 2 || probe->mode == Mode::StaleWake ||
                                         (probe->mode == Mode::BusyFinalReady && read <= 4);
                    if (pending) {
                        ++probe->pendingReads[index];
                        return 0;
                    }
                }
                const auto target = probe->waitValues[index] ? probe->waitValues[index] : probe->latestValue;
                if (result != UINT64_MAX && result >= target)
                    ++probe->completeReads[index];
            }
            return result;
        }
        CopyReadyQueryProbe(ID3D11Device5* device,
                            ID3D11DeviceContext4* immediate,
                            ID3D12Device* device12,
                            unsigned count,
                            bool useProbe,
                            const std::wstring& scenario)
            : context(immediate), multiplier(count), enabled(useProbe) {
            if (!enabled)
                return;
            require(!active, "one consumer fence observer");
            ComPtr<ID3D11Fence> sample;
            success(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&sample)), "consumer observer sample");
            event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            require(event.get() != nullptr, "consumer observer completion event");
            // Installed D3D11 SDK slots: Fence::GetCompletedValue=8, Context4::Signal=147.
            completedEntry = (*reinterpret_cast<void***>(sample.Get()))[8];
            originalCompleted = reinterpret_cast<Completed>(completedEntry);
            originalEvent = reinterpret_cast<Event>((*reinterpret_cast<void***>(sample.Get()))[9]);
            originalSignal = reinterpret_cast<Signal>((*reinterpret_cast<void***>(context))[147]);
            mode = scenario == L"query-wait-ready"                                         ? Mode::PendingReady
                   : scenario == L"query-wait-busy-final"                                  ? Mode::BusyFinalReady
                   : scenario == L"query-wait-stale"                                       ? Mode::StaleWake
                   : scenario == L"query-wait-late" || scenario == L"query-wait-slot-late" ? Mode::LateReady
                   : scenario == L"query-wait-stop"                                        ? Mode::StopReady
                   : scenario == L"query-wait-error"                                       ? Mode::WaitError
                   : scenario.rfind(L"begin-ready-s", 0) == 0                              ? Mode::BeginReady
                   : scenario.rfind(L"begin-expired-s", 0) == 0 || scenario.rfind(L"begin-at-b-s", 0) == 0 ||
                           scenario == L"begin-multi-ready" || scenario == L"begin-soft-hard-s2"
                       ? Mode::BeginReady
                   : scenario.rfind(L"begin-pending-s", 0) == 0 || scenario == L"prepared-late" ? Mode::BeginPending
                                                                                                : Mode::Complete;
            controlledBegin = scenario.rfind(L"begin-", 0) == 0;
            retainLateWait = scenario == L"query-wait-slot-late";
            finalBDue = scenario.rfind(L"begin-expired-s", 0) == 0 || scenario.rfind(L"begin-at-b-s", 0) == 0;
            hardAfterSoft = scenario == L"begin-soft-hard-s2";
            multiLate = scenario == L"begin-multi-ready" || hardAfterSoft;
            if (controlledBegin && !multiLate)
                delayedSlot = static_cast<unsigned>(scenario.back() - L'1');
            if (mode == Mode::PendingReady) {
                success(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&pendingGate)),
                        "worker pending-copy gate");
                wil::unique_handle shared;
                success(pendingGate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, shared.put()),
                        "export worker pending-copy gate");
                success(device12->OpenSharedHandle(shared.get(), IID_PPV_ARGS(&pendingRelease)),
                        "import worker pending-copy gate release");
            }
            active = this;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalCompleted), completed));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalEvent), registerEvent));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal));
            checkDetour(DetourTransactionCommit());
        }
        ~CopyReadyQueryProbe() {
            if (!enabled)
                return;
            releasePending();
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalCompleted), completed) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalEvent), registerEvent) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR)
                std::_Exit(2);
            active = nullptr;
        }
        void releasePending() const noexcept {
            if (pendingRelease && FAILED(pendingRelease->Signal(1)))
                std::_Exit(2);
        }
        void beginNative(long long id) {
            if (enabled) {
                worker = GetCurrentThreadId();
                nativeId = id;
                slotOpen = true;
            }
        }
        void recordEnd() {
            if (enabled) {
                ++endCount;
                lastEnd = nativeId;
                slotOpen = false;
            }
        }
        void validate(unsigned multiplier, bool late) const {
            if (!enabled)
                return;
            if (beginMode()) {
                const bool pending = mode == Mode::BeginPending;
                const bool fallback = pending || finalBDue || hardAfterSoft;
                const unsigned fallbackSlot = hardAfterSoft ? 1u : delayedSlot;
                require(endCount == 2 * multiplier && coveredFence && !unexpectedSignals && !unclassifiedReads,
                        "controlled Begin observes the actual worker consumer and complete native groups");
                if (controlledBegin)
                    require(readyBeforeBeginReturn[multiplier + delayedSlot],
                            "controlled late Begin returns only after the actual copy is complete");
                for (unsigned slot = 0; slot < multiplier - 1; ++slot) {
                    const auto id = multiplier + slot;
                    const unsigned expected = fallback && slot > fallbackSlot  ? 0u
                                              : pending && slot == delayedSlot ? 2u
                                                                               : 1u;
                    std::cout << "LATE_BEGIN_QUERY delayed_slot=" << delayedSlot << " native_id=" << id
                              << " reads=" << slotReads[id] << " pending=" << pendingReads[id]
                              << " wait_events=" << waitEvents[id] << '\n';
                    require(slotReads[id] == expected && waitEvents[id] == 0,
                            "already-ready late Begin adds no waits; pending deadline fallback stays sticky");
                }
                return;
            }
            if (mode != Mode::Complete) {
                const bool stopped = mode == Mode::StopReady;
                const bool failed = mode == Mode::WaitError;
                require(coveredFence && workerSignals > 0 && !unexpectedSignals && !unclassifiedReads &&
                            endCount == (failed    ? multiplier
                                         : stopped ? multiplier + 1
                                                   : 2 * multiplier),
                        "actual worker copy-wait observations are bounded by native End and cleanup");
                success(settleFailure, "real copy completes before pending snapshot injection");
                if (mode == Mode::PendingReady) {
                    std::cout << "COPY_WAIT_REAL_PENDING initial_reads=" << actualPendingReads
                              << " pending_at_registration=" << pendingAtRegistration
                              << " event_releases=" << eventReleases << '\n';
                    require(gated && actualPendingReads == 2 && pendingAtRegistration == 1 && eventReleases == 1,
                            "first pair is genuinely GPU-pending until the production wait registers its event");
                }
                for (unsigned id = multiplier; id < 2 * multiplier; ++id) {
                    const bool observed =
                        id == multiplier || ((mode == Mode::PendingReady || retainLateWait) && id < 2 * multiplier - 1);
                    const unsigned reads = !observed                                                 ? 0u
                                           : failed                                                  ? 2u
                                           : stopped                                                 ? 4u
                                           : mode == Mode::BusyFinalReady || mode == Mode::StaleWake ? 5u
                                                                                                     : 3u;
                    std::cout << "COPY_WAIT_QUERY native_id=" << id << " fence_reads=" << slotReads[id]
                              << " pending_reads=" << pendingReads[id] << " complete_reads=" << completeReads[id]
                              << " wait_events=" << waitEvents[id] << " expected_reads=" << reads << '\n';
                    require(slotReads[id] == reads && waitEvents[id] == unsigned(observed),
                            "successful wait reuses its completion snapshot; BUSY retains the final ready probe");
                    require(pendingReads[id] == (!observed                      ? 0u
                                                 : mode == Mode::StaleWake      ? 5u
                                                 : mode == Mode::BusyFinalReady ? 4u
                                                                                : 2u) &&
                                completeReads[id] == (!observed || failed || mode == Mode::StaleWake ? 0u
                                                      : stopped                                      ? 2u
                                                                                                     : 1u),
                            "pending-to-wake script covers real registration, completion and stale-event fallback");
                }
                return;
            }
            require(endCount == 2 * multiplier && coveredFence && workerSignals > 0 && !unexpectedSignals &&
                        !unclassifiedReads,
                    "real worker consumer fence and every native End delimit the observed reads");
            success(settleFailure, "bounded event settlement of actual consumer copy fence");
            std::cout << "COPY_READY_RETIREMENT prime_reads=" << retirementReads[0]
                      << " pair_reads=" << retirementReads[1] << '\n';
            require(retirementReads[0] > 0 && retirementReads[1] > 0,
                    "actual retirement consumer reads are observed separately after native End");
            for (unsigned id = 0; id < 2 * multiplier; ++id) {
                const unsigned slot = id % multiplier;
                std::cout << "COPY_READY_QUERY multiplier=" << multiplier << " native_id=" << id << " slot=" << slot
                          << " fence_reads=" << slotReads[id] << " complete_reads=" << completeReads[id]
                          << " fixture_event_waits=" << settles[id] << " late=" << late << '\n';
            }
            for (unsigned id = 0; id < 2 * multiplier; ++id) {
                const unsigned slot = id % multiplier;
                const unsigned expected = id >= multiplier && slot < multiplier - 1 && (!late || slot == 0) ? 1 : 0;
                require(settles[id] == expected && completeReads[id] == slotReads[id],
                        "observed slot queries forward actual already-complete GPU copy results");
                require(slotReads[id] == expected,
                        "one already-true consumer fence read per synthetic slot before native End");
            }
        }
    };
    struct LateBeginHook {
        inline static decltype(&ovr_BeginFrame) original{};
        inline static LateBeginHook* active{};
        bool enabled{};
        std::function<void(long long)> beforeReturn;
        static ovrResult OVR_CDECL begin(ovrSession session, long long id) {
            const auto result = original(session, id);
            if (OVR_SUCCESS(result) && active && active->beforeReturn)
                active->beforeReturn(id);
            return result;
        }
        explicit LateBeginHook(bool useHook) : enabled(useHook) {
            if (!enabled)
                return;
            require(!active, "one controlled Begin delay");
            original = interceptedBegin;
            active = this;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&original), begin));
            checkDetour(DetourTransactionCommit());
        }
        ~LateBeginHook() {
            if (!enabled)
                return;
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), begin) || DetourTransactionCommit())
                std::_Exit(2);
            active = nullptr;
        }
    };
    decltype(&ovr_CommitTextureSwapChain) originalFgCommit;
    decltype(&ovr_GetTextureSwapChainCurrentIndex) originalFgIndex;
    struct PresentationRing {
        std::vector<ComPtr<ID3D11Texture2D>> images;
        unsigned current{}, commits{};
        int committed{-1};
    };
    std::map<ovrTextureSwapChain, PresentationRing> presentationRings;
    long long commitFaultId{-1};
    ovrResult OVR_CDECL ringIndex(ovrSession, ovrTextureSwapChain chain, int* index) {
        const auto found = presentationRings.find(chain);
        if (found == presentationRings.end())
            return ovrError_InvalidParameter;
        *index = static_cast<int>(found->second.current);
        return ovrSuccess;
    }
    ovrResult OVR_CDECL fakeCommit(ovrSession, ovrTextureSwapChain chain) {
        if (commitFaultId >= 0) {
            std::lock_guard lock(backendState->mutex);
            if (backendState->begunIds.back() == commitFaultId) {
                ++backendState->faultCalls;
                backendState->changed.notify_all();
                return ovrError_DisplayLost;
            }
        }
        auto& ring = presentationRings.at(chain);
        ring.committed = static_cast<int>(ring.current);
        ring.current = (ring.current + 1) % ring.images.size();
        ++ring.commits;
        return ovrSuccess;
    }
    struct CommitHook {
        CommitHook() {
            presentationRings.clear();
            commitFaultId = -1;
            originalFgCommit = reinterpret_cast<decltype(originalFgCommit)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_CommitTextureSwapChain"));
            require(originalFgCommit != nullptr, "commit export required");
            originalFgIndex = reinterpret_cast<decltype(originalFgIndex)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_GetTextureSwapChainCurrentIndex"));
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalFgCommit), fakeCommit));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalFgIndex), ringIndex));
            checkDetour(DetourTransactionCommit());
        }
        ~CommitHook() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalFgCommit), fakeCommit) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalFgIndex), ringIndex) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
            presentationRings.clear();
        }
    };

    decltype(VDXRFG_ApiV1::Retire) originalModuleRetire;
    std::atomic<uint64_t> heldRetirement{};
    std::atomic<unsigned> retirementBusyCalls{};
    HRESULT WINAPI delayedModuleRetire(void* context, uint64_t token) noexcept {
        if (token == heldRetirement.load()) {
            ++retirementBusyCalls;
            return VDXRFG_E_BUSY;
        }
        return originalModuleRetire(context, token);
    }
    struct RetirementHook {
        HMODULE module{};
        explicit RetirementHook(const wchar_t* path, bool enabled) {
            if (!enabled)
                return;
            module = LoadLibraryW(std::filesystem::absolute(path).c_str());
            require(module != nullptr, "retain module while retirement trampoline is attached");
            const auto getApi = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
            VDXRFG_ApiV1 api{sizeof(api), VDXRFG_API_VERSION};
            require(getApi != nullptr, "retirement API export required");
            success(getApi(&api), "load real module retirement API");
            originalModuleRetire = api.Retire;
            heldRetirement = 0;
            retirementBusyCalls = 0;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalModuleRetire), delayedModuleRetire));
            checkDetour(DetourTransactionCommit());
        }
        ~RetirementHook() {
            if (module) {
                if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                    DetourDetach(reinterpret_cast<PVOID*>(&originalModuleRetire), delayedModuleRetire) != NO_ERROR ||
                    DetourTransactionCommit() != NO_ERROR)
                    std::_Exit(2);
                FreeLibrary(module);
            }
        }
    };

    decltype(&ovr_GetPredictedDisplayTime) originalHistoryPrediction;
    decltype(&ovr_ClearShouldRecenterFlag) originalHistoryClear;
    double historyClockBase;
    double OVR_CDECL historyPrediction(ovrSession, long long id) {
        return historyClockBase + id / 90.;
    }
    void OVR_CDECL historyClear(ovrSession) {
        std::lock_guard lock(backendState->mutex);
        backendState->recenter = false;
        ++backendState->recenterClears;
    }
    struct HistoryHooks {
        HistoryHooks() {
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalHistoryPrediction = reinterpret_cast<decltype(originalHistoryPrediction)>(
                GetProcAddress(module, "ovr_GetPredictedDisplayTime"));
            originalHistoryClear =
                reinterpret_cast<decltype(originalHistoryClear)>(GetProcAddress(module, "ovr_ClearShouldRecenterFlag"));
            require(originalHistoryPrediction && originalHistoryClear, "history clock/status exports required");
            historyClockBase = ovr_GetTimeInSeconds() + .01;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalHistoryPrediction), historyPrediction));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalHistoryClear), historyClear));
            checkDetour(DetourTransactionCommit());
        }
        ~HistoryHooks() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalHistoryPrediction), historyPrediction) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalHistoryClear), historyClear) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR) {
                std::_Exit(2);
            }
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
#include "fg_live_recovery_fixture.h"
        template <typename Runtime>
        static auto presentationMiss(Runtime& runtime, int)
            -> decltype(runtime.noteFrameGenerationPresentationMiss(), void()) {
            runtime.noteFrameGenerationPresentationMiss();
        }
        template <typename Runtime>
        static void presentationMiss(Runtime& runtime, ...) {
            // Exact old presentation-failure recovery policy, for behavioral RED.
            runtime.m_fgNeedReprime = runtime.m_fgTimelineReanchor = true;
        }
        static void
        allocation(const wchar_t* backend, const wchar_t* module, unsigned multiplier, const std::wstring& scenario) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            Interop hw;
            ovrGraphicsLuid luid{};
            require(OVR_SUCCESS(ovr_Create(&runtime.m_ovrSession, &luid)), "allocation fixture owns a valid session");
            RuntimeInputRegression::seed(runtime);
            runtime.m_ovrSubmissionDevice = hw.device;
            runtime.m_ovrSubmissionContext = hw.context;
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_fgDesiredMultiplier = multiplier;
            runtime.m_precompositor.dlssnrSettings.enabled = true;
            runtime.m_precompositor.dlssnrSettings.foveationSize = 1.f;
            runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = hw.texture();
            runtime.m_fgBegunReservation.valid = true;
            runtime.m_fgBegunReservation.shouldRender = true;
            runtime.m_fgBegunReservation.currentTarget = 1000000000;
            runtime.m_layersForFrame.resize(1);
            auto& projection = runtime.m_layersForFrame[0].EyeFov;
            projection.Header.Type = ovrLayerType_EyeFov;
            projection.Viewport[0].Size = projection.Viewport[1].Size = {eyeWidth, height};
            XrCompositionLayerProjection xrProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            OpenXrRuntime::Swapchain colourInputs[2]{};
            XrCompositionLayerProjectionView views[2]{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&colourInputs[eye]);
            }
            xrProjection.viewCount = 2;
            xrProjection.views = views;
            const XrCompositionLayerBaseHeader* layers[]{
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&xrProjection)};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = 1000000000;
            end.layerCount = 1;
            end.layers = layers;
            AllocationHooks failures(scenario, multiplier);
            auto cleanup = MakeScopeGuard([&] { runtime.cleanupSessionResources(); });
            const auto previousHome = dllHome;
            auto restoreHome = MakeScopeGuard([&] { dllHome = previousHome; });
            const bool countAllocations = scenario == L"allocation-count" || scenario == L"allocation-resize";
            if (countAllocations) {
                dllHome = std::filesystem::absolute(module).parent_path();
                runtime.m_frameBegun = 1;
                runtime.m_fgBegunReservation = runtime.m_frameGenerationCadence.reserve(
                    1000000000 - multiplier * 10000000, 1010000000 - multiplier * 10000000, 10000000, multiplier, 0);
                end.displayTime = runtime.m_fgBegunReservation.currentTarget;
                for (unsigned eye = 0; eye < 2; ++eye) {
                    projection.Viewport[eye].Pos = {static_cast<int>(eye * eyeWidth), 0};
                    projection.RenderPose[eye].Orientation.w = 1.f;
                    projection.Fov[eye] = {1.f, 1.f, 1.f, 1.f};
                }
            }
            bool accepted{};
            std::exception_ptr failure;
            try {
                accepted = runtime.prepareFrameGeneration(end);
            } catch (...) {
                failure = std::current_exception();
            }
            if (countAllocations) {
                if (failure)
                    std::rethrow_exception(failure);
                require(accepted, "allocation count uses the production capture path and real SDK chains");
                std::cout << "PRESENTATION_ALLOCATION multiplier=" << multiplier << " chains=" << allocatedChains
                          << " images=" << allocatedImages << " descriptor_texel_bytes=" << allocatedTexelBytes
                          << " expected_chains=" << multiplier + 1 << '\n';
                require(allocatedChains == multiplier + 1,
                        "worker-owned synthetic roles use one SDK chain each while real B keeps two banks");
                if (scenario == L"allocation-resize") {
                    // Keep the source textures alive while rebuilding smaller presentation atlases.
                    ++runtime.m_frameBegun;
                    end.displayTime += multiplier * 10000000;
                    runtime.m_fgBegunReservation.currentTarget = end.displayTime;
                    for (auto& viewport : projection.Viewport)
                        viewport.Size.w /= 2;
                    require(runtime.prepareFrameGeneration(end), "resolution change recreates the presentation graph");
                    require(allocatedChains == 2 * (multiplier + 1) && destroyedChains == multiplier + 1,
                            "resize retires the old sparse graph before creating exactly one replacement graph");
                    for (const auto& bank : runtime.m_fgPresentationBanks)
                        for (const auto& role : bank)
                            if (role.ovrSwapchain)
                                require(role.resolution.w == eyeWidth, "replacement SDK atlases have the new extent");
                    std::cout << "PRESENTATION_RESIZE allocated=" << allocatedChains
                              << " destroyed_before_cleanup=" << destroyedChains << '\n';
                }
                runtime.cleanupSessionResources();
                cleanup.Deactivate();
                require(destroyedChains == allocatedChains, "all presentation chains are destroyed exactly once");
                hw.gpu.noErrors();
                return;
            }
            require(injectedAllocationFailures == 1,
                    "the intended failure fired after earlier real chains were allocated");
            require(!failure && !accepted, "optional allocation failure preserves the original NR submission");
            require(runtime.m_frameGenerationUnavailable && !runtime.m_frameGeneration &&
                        runtime.m_fgDesiredMultiplier.load() == 1 && !runtime.m_fgTransactionForFrame.ticket.token,
                    "failed optional allocation disables only FG without accepting GPU work");
            require(allocatedChains >= 2 && allocatedChains == destroyedChains,
                    "partial presentation roles are all rolled back");
            const auto attempts = allocationAttempts;
            require(!runtime.prepareFrameGeneration(end) && attempts == allocationAttempts,
                    "failed optional allocation is not retried every frame");
            require(runtime.m_precompositor.dlssnrSettings.enabled &&
                        runtime.m_precompositor.dlssnrSettings.foveationSize == 1.f &&
                        runtime.m_precompositor.finalColour[0] && runtime.m_layersForFrame.size() == 1,
                    "fallback retains final NR pixels, coverage and layer");
            runtime.cleanupSessionResources();
            cleanup.Deactivate();
            hw.gpu.noErrors();
        }
        static void startup(const wchar_t* backend, const std::wstring& scenario) {
            OpenXrRuntime runtime;
            BackendState state;
            state.fault = scenario == L"startup-timeout" ? Fault::Timeout : Fault::NotInitialized;
            state.recoverAfterFaults = scenario == L"startup-retry" ? 3 : 0;
            BackendHooks hooks(backend, state);
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = 3;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            RuntimeInputRegression::start(runtime);
            state.release();
            if (scenario == L"startup-stop") {
                require(state.waitFor([&] { return state.faultCalls >= 2; }),
                        "persistent uninitialized backend retries before any Begin");
            } else {
                runtime.waitForFrameGenerationCapacity();
                std::lock_guard lock(state.mutex);
                require(state.beginCalls == 1 && state.faultCalls == (scenario == L"startup-retry" ? 3u : 1u),
                        "recoverable native Wait outcomes preserve baseline startup behavior");
                for (const auto id : state.waitedIds) {
                    require(id == 0, "pre-Begin retries do not advance the compositor ID");
                }
            }
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
            require(!runtime.m_asyncSubmissionError && !runtime.m_asyncSubmissionThread.joinable(),
                    "stopping a retry does not turn it into a permanent submission failure");
        }
        static void statusSnapshots(const wchar_t* backend) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            HistoryHooks history;
            ovrGraphicsLuid luid{};
            require(OVR_SUCCESS(ovr_Create(&runtime.m_ovrSession, &luid)), "status fixture owns a valid Null session");
            historyClockBase = ovr_GetTimeInSeconds() + 1.;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameCompleted = runtime.m_frameBegun;
            runtime.m_isHeadless = false;
            runtime.m_frameGenerationMode = 3;
            runtime.m_idealFrameDuration = runtime.m_predictedFrameDuration = 1. / 90.;
            auto cleanup = MakeScopeGuard([&] { RuntimeInputRegression::stop(runtime, state); });
            RuntimeInputRegression::start(runtime);
            state.release();
            runtime.waitForFrameGenerationCapacity();
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            const auto wait = [&] {
                require(runtime.xrWaitFrame(reinterpret_cast<XrSession>(1), nullptr, &frame) == XR_SUCCESS,
                        "public Wait succeeds with controlled clock and status");
                require(runtime.m_fgWaitedReservation.valid, "status snapshot has a valid prediction");
                return runtime.m_fgWaitedReservation;
            };
            const auto begin = [&] {
                // No graphics are submitted in this status-only fixture. Account
                // for completion of the old frame before calling public Begin.
                runtime.m_frameCompleted = runtime.m_frameBegun;
                require(runtime.xrBeginFrame(reinterpret_cast<XrSession>(1), nullptr) == XR_SUCCESS,
                        "public Begin copies its waited snapshot");
            };
            const auto initial = wait();
            begin();
            {
                std::lock_guard lock(state.mutex);
                state.recenter = true;
            }
            const auto recentered = wait();
            require(recentered.historyEpoch > initial.historyEpoch &&
                        runtime.m_fgBegunReservation.historyEpoch == initial.historyEpoch,
                    "pipelined next Wait preserves the previous begun epoch");
            begin();
            require(runtime.m_fgBegunReservation.historyEpoch == recentered.historyEpoch && runtime.m_shouldRecenter,
                    "recenter epoch belongs to the next begun frame while app events are pending");
            {
                std::lock_guard lock(state.mutex);
                require(state.recenterClears == 1, "first recenter flag acknowledged");
                state.recenter = true;
            }
            const auto secondRecenter = wait();
            require(secondRecenter.historyEpoch > recentered.historyEpoch,
                    "new backend recenter invalidates history even with earlier app events pending");
            begin();
            const auto stillAsserted = wait();
            require(stillAsserted.historyEpoch == secondRecenter.historyEpoch,
                    "one pending recenter signal does not repeatedly invalidate history");
            begin();
            {
                std::lock_guard lock(state.mutex);
                state.visible = false;
            }
            const auto hidden = wait();
            require(!hidden.shouldRender && hidden.historyEpoch > stillAsserted.historyEpoch,
                    "hidden Wait records the visibility discontinuity");
            begin();
            {
                std::lock_guard lock(state.mutex);
                state.visible = true;
            }
            const auto visibleAgain = wait();
            require(visibleAgain.shouldRender && visibleAgain.historyEpoch > hidden.historyEpoch &&
                        !runtime.m_fgBegunReservation.shouldRender,
                    "visible again re-primes even when hidden frame had no capture");
            begin();
            require(runtime.m_fgBegunReservation.historyEpoch == visibleAgain.historyEpoch &&
                        runtime.m_fgBegunReservation.shouldRender,
                    "public Begin retains render eligibility and epoch from its own Wait");
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
        }
        static void history(const wchar_t* backend,
                            const wchar_t* module,
                            const std::wstring& scenario,
                            unsigned multiplier = 3,
                            unsigned rate = 100) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_isHeadless = false;
            runtime.m_hmdStatus.IsVisible = true;
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_fgEyeResolution = {eyeWidth, height};
            runtime.m_fgFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           multiplier,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize finalized-color history control");
            auto cleanup = MakeScopeGuard([&] { runtime.cleanupSessionResources(); });
            runtime.m_precompositor.dlssnrSettings.enabled = true;
            runtime.m_precompositor.dlssnrSettings.foveationSize = 1.f;
            runtime.m_layersForFrame.resize(1);
            auto& projection = runtime.m_layersForFrame[0].EyeFov;
            projection.Header.Type = ovrLayerType_EyeFov;
            for (unsigned eye = 0; eye < 2; ++eye) {
                projection.Viewport[eye] = {{static_cast<int>(eye * eyeWidth), 0}, {eyeWidth, height}};
                projection.RenderPose[eye].Orientation.w = 1.f;
                projection.RenderPose[eye].Position.x = eye ? .032f : -.032f;
                projection.Fov[eye] = {1.f, 1.f, 1.f, 1.f};
            }
            XrCompositionLayerProjection xrProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            OpenXrRuntime::Swapchain colourInputs[2]{};
            XrCompositionLayerProjectionView views[2]{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&colourInputs[eye]);
            }
            xrProjection.viewCount = 2;
            xrProjection.views = views;
            xrProjection.space = reinterpret_cast<XrSpace>(uintptr_t(0x100));
            const XrCompositionLayerBaseHeader* layers[]{
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&xrProjection)};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.layerCount = 1;
            end.layers = layers;
            const int64_t period = 1000000000LL / rate;
            uint64_t epoch = 1;
            uint64_t lastCapturedSerial{};
            OwnedFrameTransaction held;
            const auto capture = [&](unsigned frame, bool expectedPrime, bool hold = false) {
                runtime.m_frameBegun = frame;
                end.displayTime = 1000000000 + frame * multiplier * period;
                runtime.m_fgBegunReservation =
                    runtime.m_frameGenerationCadence.reserve(end.displayTime - multiplier * period,
                                                             end.displayTime - (multiplier - 1) * period,
                                                             period,
                                                             multiplier,
                                                             frame * multiplier);
                runtime.m_fgBegunReservation.historyEpoch = epoch;
                const auto pixels = scene(frame * 8);
                runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = hw.texture(&pixels);
                require(runtime.prepareFrameGeneration(end), "eligible finalized input remains on FG path");
                auto& transaction = runtime.m_fgTransactionForFrame;
                hw.await(*runtime.m_frameGeneration, transaction);
                // Settle first so a failed assertion cannot leave this fixture owning live GPU work.
                const bool prime = transaction.ticket.prime != 0;
                const auto previousSerial = transaction.ticket.previous_serial;
                if (hold) {
                    held = std::move(transaction);
                    transaction = {};
                } else {
                    hw.retire(*runtime.m_frameGeneration, transaction);
                }
                require(prime == expectedPrime,
                        "history discontinuity primes once; unchanged endpoints form a real synthesized pair");
                if (!expectedPrime)
                    require(previousSerial == lastCapturedSerial, "capture uses the newest valid endpoint history");
                lastCapturedSerial = frame;
            };
            capture(1, true);
            if (scenario == L"history-pipelined") {
                // The next Wait observes recentering before this frame's End.
                runtime.m_recenterTime = end.displayTime + 1;
                runtime.m_fgHistoryEpoch = epoch + 1;
            }
            capture(2, false, scenario == L"history-busy");
            if (scenario == L"history-presentation-miss" || scenario == L"history-pending-miss") {
                if (scenario == L"history-pending-miss")
                    capture(3, false, true);
                presentationMiss(runtime, 0);
                require(runtime.m_fgTimelineReanchor.load(), "presentation miss retains native timeline recovery");
                if (held.ticket.token) {
                    require(held.ticket.current_serial == 3 && runtime.m_frameGeneration->ready(held),
                            "older presentation miss does not invalidate newer pending capture");
                    hw.retire(*runtime.m_frameGeneration, held);
                }
                capture(scenario == L"history-pending-miss" ? 4 : 3, false);
                ++epoch;
                const unsigned changed = scenario == L"history-pending-miss" ? 5 : 4;
                capture(changed, true);
                capture(changed + 1, false);
                runtime.cleanupSessionResources();
                cleanup.Deactivate();
                hw.gpu.noErrors();
                return;
            }
            if (scenario == L"history-busy") {
                projection.Fov[0].LeftTan = .9f;
                runtime.m_frameBegun = 3;
                end.displayTime += 3 * period;
                runtime.m_fgBegunReservation = runtime.m_frameGenerationCadence.reserve(
                    end.displayTime - 3 * period, end.displayTime - 2 * period, period, 3, 9);
                runtime.m_fgBegunReservation.historyEpoch = epoch;
                const auto token = held.ticket.token;
                require(!runtime.prepareFrameGeneration(end) && !runtime.m_fgTransactionForFrame.ticket.token &&
                            held.ticket.token == token && runtime.m_frameGeneration->ready(held),
                        "metadata change bypasses rather than reset or overwrite a live output");
                require(runtime.m_fgDesiredMultiplier.load() == 3,
                        "busy metadata transition preserves the requested recovery cadence");
                hw.retire(*runtime.m_frameGeneration, held);
                capture(4, true);
                capture(5, false);
                runtime.cleanupSessionResources();
                cleanup.Deactivate();
                hw.gpu.noErrors();
                return;
            }
            if (scenario == L"history-recenter" || scenario == L"history-pipelined") {
                runtime.m_recenterTime = end.displayTime + 1;
                ++epoch;
            } else if (scenario == L"history-fov") {
                projection.Fov[0].LeftTan = projection.Fov[1].LeftTan = .9f;
            } else if (scenario == L"history-space") {
                xrProjection.space = reinterpret_cast<XrSpace>(uintptr_t(0x200));
            } else if (scenario == L"history-sharpen") {
                runtime.m_precompositor.sharpenFactor = .5f;
            } else {
                require(scenario == L"history-visibility" || scenario == L"history-visibility-gap",
                        "known history discontinuity required");
                runtime.m_hmdStatus.IsVisible = false;
                runtime.m_fgBegunReservation.shouldRender = false;
                if (scenario == L"history-visibility") {
                    require(!runtime.prepareFrameGeneration(end) && !runtime.m_fgTransactionForFrame.ticket.token &&
                                runtime.m_fgDesiredMultiplier.load() == 1,
                            "hidden session bypasses synthesis without changing finalized color");
                }
                runtime.m_hmdStatus.IsVisible = true;
                epoch += 2;
            }
            capture(3, true);
            capture(4, false);
            runtime.cleanupSessionResources();
            cleanup.Deactivate();
            hw.gpu.noErrors();
        }
        static void cleanupProducer(const wchar_t* backend, const wchar_t* module) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           3,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize pending-producer cleanup host");
            FinalProjectionInput input;
            FrameGenerationCadence cadence;
            input.serial = 1;
            input.period = 10000000;
            input.time = 1000000000;
            input.slots = cadence.plan(
                cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 0), 0, false);
            const auto pixels = scene(0);
            input.colour[0] = input.colour[1] = hw.texture(&pixels);
            for (unsigned eye = 0; eye < 2; ++eye) {
                input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
                input.views[eye].orientation[3] = 1.f;
                input.views[eye].position[0] = eye ? .032f : -.032f;
                input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
                input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
            }
            OwnedFrameTransaction prime;
            success(runtime.m_frameGeneration->capture(input, prime), "seed duplicate-serial rejection control");
            hw.await(*runtime.m_frameGeneration, prime);
            hw.retire(*runtime.m_frameGeneration, prime);
            ComPtr<ID3D11Fence> gate;
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "pending producer gate");
            HANDLE handle{};
            success(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "export producer gate");
            ComPtr<ID3D12Fence> release;
            const auto opened = hw.gpu.device->OpenSharedHandle(handle, IID_PPV_ARGS(&release));
            CloseHandle(handle);
            success(opened, "import producer release fence");
            input.time += 3 * input.period;
            input.slots =
                cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 3),
                             input.time - 30000000,
                             true);
            success(hw.context->Wait(gate.Get(), 1), "park host producer before rejected duplicate");
            OwnedFrameTransaction rejected;
            const auto result = runtime.m_frameGeneration->capture(input, rejected);
            const auto shutdown = runtime.m_frameGeneration->shutdown();
            std::atomic<HRESULT> releaseResult{E_FAIL};
            std::thread releaser([&] {
                std::this_thread::sleep_for(75ms);
                releaseResult = release->Signal(1);
            });
            auto join = MakeScopeGuard([&] { releaser.join(); });
            require(result == VDXRFG_E_REJECTED && !rejected.ticket.token && shutdown == VDXRFG_E_BUSY,
                    "rejected admission leaves actual producer work pending without a module ticket");
            const auto start = std::chrono::steady_clock::now();
            runtime.cleanupFrameGeneration();
            const auto elapsed = std::chrono::steady_clock::now() - start;
            releaser.join();
            join.Deactivate();
            success(releaseResult, "release pending producer");
            require(!runtime.m_frameGeneration && elapsed >= 50ms && elapsed < 2s,
                    "cleanup retries BUSY producer completion before releasing graph");
            runtime.cleanupSessionResources();
            hw.gpu.noErrors();
        }
        static void bypass(const std::wstring& scenario) {
            OpenXrRuntime runtime;
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = 3;
            runtime.m_fgDesiredMultiplier = 3;
            runtime.m_precompositor.dlssnrSettings.enabled = true;
            runtime.m_precompositor.dlssnrSettings.foveationSize = 1.f;
            runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = hw.texture();
            runtime.m_fgBegunReservation.valid = true;
            runtime.m_fgBegunReservation.currentTarget = 1000000000;
            runtime.m_layersForFrame.resize(1);
            runtime.m_layersForFrame[0].Header.Type = ovrLayerType_EyeFov;
            runtime.m_fgBegunReservation.shouldRender = true;
            runtime.m_layersForFrame[0].EyeFov.Viewport[0].Size = {eyeWidth, height};
            runtime.m_layersForFrame[0].EyeFov.Viewport[1].Size = {eyeWidth, height};
            if (scenario == L"unequal-eyes") {
                runtime.m_layersForFrame[0].EyeFov.Viewport[1].Size.w -= 4;
            } else if (scenario == L"bgrx") {
                D3D11_TEXTURE2D_DESC desc{};
                runtime.m_precompositor.finalColour[0]->GetDesc(&desc);
                desc.Format = DXGI_FORMAT_B8G8R8X8_UNORM;
                ComPtr<ID3D11Texture2D> bgra;
                success(hw.device->CreateTexture2D(&desc, nullptr, &bgra), "unsupported final BGRX control");
                runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = bgra;
            }
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = 1000000000;
            end.layerCount = 1;
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            OpenXrRuntime::Swapchain colourInputs[2]{};
            XrCompositionLayerProjectionView views[2]{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&colourInputs[eye]);
            }
            projection.viewCount = 2;
            projection.views = views;
            if (scenario == L"static" || scenario == L"protected") {
                colourInputs[1].xrDesc.createFlags = scenario == L"static" ? XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT
                                                                           : XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
            } else if (scenario == L"nr-off") {
                runtime.m_precompositor.dlssnrSettings.enabled = false;
            } else if (scenario == L"low-coverage") {
                runtime.m_precompositor.dlssnrSettings.foveationSize = .89f;
            } else if (scenario == L"app-submission") {
                runtime.m_useApplicationDeviceForSubmission = true;
            } else if (scenario == L"synchronous") {
                runtime.m_useAsyncSubmission = false;
            } else if (scenario == L"depth") {
                runtime.m_layersForFrame[0].Header.Type = ovrLayerType_EyeFovDepth;
            } else if (scenario == L"overlay" || scenario == L"second-projection") {
                runtime.m_layersForFrame.resize(2);
                runtime.m_layersForFrame[1].Header.Type =
                    scenario == L"overlay" ? ovrLayerType_Quad : ovrLayerType_EyeFov;
                end.layerCount = 2;
            }
            const XrCompositionLayerBaseHeader* layers[]{
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
            end.layers = layers;
            require(!runtime.prepareFrameGeneration(end) && !runtime.m_frameGeneration,
                    "unsupported final composition bypasses before loading a module");
            require(runtime.m_fgDesiredMultiplier.load() == 1,
                    "unsupported format or eye extents restore native application cadence");
            runtime.cleanupSessionResources();
        }
        static void run(const wchar_t* backend,
                        const wchar_t* module,
                        unsigned multiplier,
                        bool late,
                        bool capacity,
                        bool failedPending,
                        const std::wstring& scenario) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            CommitHook commits;
            const bool delayedRetirement = scenario == L"shared-retire";
            const bool pipeline = scenario == L"shared-ring" || scenario == L"shared-ring-one" || delayedRetirement;
            const bool waitQuery = scenario.rfind(L"query-wait-", 0) == 0;
            const bool beginScenario = scenario.rfind(L"begin-", 0) == 0;
            const bool preBegin = scenario.rfind(L"prebegin-", 0) == 0;
            const bool queryScenario = scenario == L"copy-ready-query" || scenario == L"query-late" || waitQuery ||
                                       beginScenario || scenario == L"prepared-late";
            const bool preparedLate = scenario == L"prepared-late" || scenario == L"query-late";
            CapacityClockHook capacityClock(capacity || pipeline || preparedLate || queryScenario || preBegin,
                                            preBegin);
            if (capacity || pipeline || preparedLate || queryScenario || preBegin)
                state.observeWait = [&](long long nativeId) {
                    capacityClock.advance(preparedLate && nativeId == multiplier ? 2 * multiplier + 10 : nativeId);
                };
            Interop hw;
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            runtime.m_fgEyeResolution = {eyeWidth, height};
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           multiplier,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize presenter GPU module");
            RetirementHook retirements(module, delayedRetirement);
            CopyReadyQueryProbe readyQueries(
                hw.device.Get(), hw.context.Get(), hw.gpu.device.Get(), multiplier, queryScenario, scenario);
            LateBeginHook lateBegin(beginScenario || preBegin);
            std::array<int64_t, 4> originalTargets{};
            std::array<std::vector<uint8_t>, 4> originalDelayedPixels;
            unsigned delayedBegins{};
            uint64_t pairGeneration{};
            bool simulatedReanchorConsumed{};
            lateBegin.beforeReturn = [&](long long id) {
                if (id < multiplier || id >= 2 * multiplier)
                    return;
                if (preBegin) {
                    if (id == multiplier)
                        for (unsigned slot = 0; slot < multiplier - 1; ++slot) {
                            const auto role = slot == 2 ? 3u : slot;
                            auto& ring =
                                presentationRings.at(reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6000 + role)));
                            originalDelayedPixels[slot] = hw.read(ring.images[ring.current].Get());
                        }
                    return;
                }
                const auto slot = static_cast<unsigned>(id - multiplier);
                if (slot >= readyQueries.delayedSlot && slot < multiplier - 1)
                    readyQueries.settleInsideBegin(id);
                if ((slot == readyQueries.delayedSlot || readyQueries.multiLate) && slot < multiplier - 1 &&
                    (!readyQueries.hardAfterSoft || slot <= 1)) {
                    const auto role = slot == 2 ? 3u : slot;
                    auto& ring = presentationRings.at(reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6000 + role)));
                    originalDelayedPixels[slot] = hw.read(ring.images[ring.current].Get());
                    const bool hard = readyQueries.finalBDue || (readyQueries.hardAfterSoft && slot == 1);
                    const auto target = hard ? originalTargets[multiplier - 1] : originalTargets[slot];
                    capacityClockTime =
                        runtime.xrTimeToOvrTime(target) + (scenario.rfind(L"begin-at-b-s", 0) == 0 ? 0. : .001);
                    ++delayedBegins;
                }
            };
            if (scenario == L"query-wait-busy-final" || scenario == L"query-wait-stale" ||
                scenario == L"query-wait-late")
                readyQueries.afterWake = [&] { capacityClockTime = capacityClock.origin + 1.; };
            if (scenario == L"query-wait-slot-late")
                readyQueries.afterWake = [&] {
                    capacityClockTime = runtime.xrTimeToOvrTime(originalTargets[0]) + .001;
                    const auto selected = runtime.ovrTimeToXrTime(capacityClockTime.load());
                    require(selected > originalTargets[0] && selected < originalTargets[multiplier - 1],
                            "late successful wait must finish strictly between current slot and real B deadlines");
                    std::cout << "LATE_COPY_WINDOW slot_target=" << originalTargets[0] << " completion=" << selected
                              << " B_target=" << originalTargets[multiplier - 1] << '\n';
                };
            if (scenario == L"query-wait-stop")
                readyQueries.afterWake = [&] {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    runtime.m_terminateAsyncThread = true;
                    runtime.m_asyncSubmissionCondVar.notify_all();
                };
            if (queryScenario)
                state.observeWait = [&](long long nativeId) {
                    if (readyQueries.multiLate && nativeId == multiplier + 1) {
                        simulatedReanchorConsumed = runtime.m_fgTimelineReanchor.exchange(false);
                        std::cout << "SIMULATED_REANCHOR_CONSUMPTION consumed=" << simulatedReanchorConsumed << '\n';
                    }
                    readyQueries.beginNative(nativeId);
                    capacityClock.advance(preparedLate && nativeId == multiplier ? 2 * multiplier + 10 : nativeId);
                };
            for (unsigned bank = 0; bank < 2; ++bank) {
                for (unsigned role = 0; role < 4; ++role) {
                    if ((bank && role != 2) || (role == 1 && multiplier == 2) || (role == 3 && multiplier != 4)) {
                        continue;
                    }
                    auto& slice = runtime.m_fgPresentationBanks[bank][role];
                    slice.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6000 + bank * 4 + role));
                    auto& ring = presentationRings[slice.ovrSwapchain];
                    const unsigned imageCount = scenario == L"ring-one" || scenario == L"shared-ring-one" ? 1 : 3;
                    ring.current = (role + 1) % imageCount;
                    for (unsigned image = 0; image < imageCount; ++image)
                        slice.images.push_back(hw.texture());
                    ring.images = slice.images;
                }
            }
            std::vector<std::vector<uint8_t>> frames;
            std::vector<ovrTextureSwapChain> chains;
            std::atomic<int64_t> diagnosticFirstTarget{};
            state.observeLayers = [&](const ovrLayerHeader* const* layers, unsigned count) {
                readyQueries.recordEnd();
                require(count == 1 && layers[0] && layers[0]->Type == ovrLayerType_EyeFov,
                        "each generated slot presents a stereo color projection");
                const auto& eye = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                require(eye.ColorTexture[0] == eye.ColorTexture[1] && eye.Viewport[0].Pos.x == 0 &&
                            eye.Viewport[1].Pos.x == eyeWidth && eye.Viewport[0].Size.w == eyeWidth,
                        "both eyes use distinct rectangles of an owned output");
                if (capacity) {
                    const auto now = runtime.ovrTimeToXrTime(ovr_GetTimeInSeconds());
                    std::cout << "Capacity End frame=" << frames.size() << " now=" << now
                              << " S1=" << diagnosticFirstTarget.load()
                              << " lead_ns=" << diagnosticFirstTarget.load() - now
                              << " need_reprime=" << runtime.m_fgNeedReprime.load() << " chain=" << eye.ColorTexture[0]
                              << '\n';
                }
                const auto& ring = presentationRings.at(eye.ColorTexture[0]);
                require(ring.committed >= 0, "presented SDK chain must first commit its selected current image");
                frames.push_back(hw.read(ring.images[ring.committed].Get()));
                chains.push_back(eye.ColorTexture[0]);
            };
            std::mutex gateMutex;
            std::condition_variable gateChanged;
            long long gateId{-1};
            bool atGate{}, releaseGate{};
            OwnedFrameTransaction queued;
            PredictionReservation queuedReservation;
            auto cleanup = MakeScopeGuard([&] {
                readyQueries.releasePending();
                heldRetirement = 0;
                {
                    std::lock_guard lock(gateMutex);
                    releaseGate = true;
                    gateChanged.notify_all();
                }
                if (queued.ticket.token)
                    runtime.m_fgTransactionForFrame = std::move(queued);
                RuntimeInputRegression::stop(runtime, state);
            });
            if (pipeline)
                state.observeWait = [&](long long id) {
                    capacityClock.advance(id);
                    std::unique_lock lock(gateMutex);
                    if (id == gateId) {
                        atGate = true;
                        gateChanged.notify_all();
                        require(gateChanged.wait_for(lock, 10s, [&] { return releaseGate; }),
                                "bounded producer overlap gate");
                    }
                };
            RuntimeInputRegression::start(runtime);
            state.release();
            runtime.waitForAsyncSubmissionIdle();
            const auto a = scene(0), b = scene(24);
            FrameGenerationCadence cadence;
            constexpr int64_t period = 10000000;
            const auto first = runtime.ovrTimeToXrTime(ovr_GetTimeInSeconds()) + 20000000;
            int64_t previous{};
            std::vector<unsigned> ticketBanks;
            const unsigned groups = delayedRetirement ? 3 : pipeline ? 5 : 2;
            for (unsigned group = 0; group < groups; ++group) {
                runtime.waitForAsyncSubmissionIdle();
                const auto reservation = pipeline && group ? queuedReservation
                                                           : cadence.reserve(first - period,
                                                                             first + group * multiplier * period,
                                                                             period,
                                                                             multiplier,
                                                                             group * multiplier);
                FinalProjectionInput input;
                input.serial = group + 1;
                input.time = reservation.currentTarget;
                input.period = period;
                input.slots = cadence.plan(reservation, previous, group != 0);
                input.colour[0] = input.colour[1] = hw.texture((pipeline ? group % 2 : group) ? &b : &a);
                for (unsigned eye = 0; eye < 2; ++eye) {
                    input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
                    input.views[eye].orientation[3] = 1;
                    input.views[eye].position[0] = eye ? .032f : -.032f;
                    input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
                    input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
                }
                OwnedFrameTransaction transaction;
                if (pipeline && group) {
                    transaction = std::move(queued);
                    queued = {};
                } else {
                    success(runtime.m_frameGeneration->capture(input, transaction), "capture next presenter group");
                }
                hw.await(*runtime.m_frameGeneration, transaction);
                if (preBegin && group) {
                    require(!transaction.ticket.prime && transaction.ticket.current_serial == 2 &&
                                transaction.slots.count == multiplier && runtime.m_frameGeneration->ready(transaction),
                            "pre-Begin experiment requires a real nonprime pair with physically complete outputs");
                    pairGeneration = transaction.ownerGeneration;
                    originalTargets = transaction.slots.targets;
                    HANDLE unusedEvent{};
                    require(runtime.m_frameGeneration->prepareOutputReadyWait(
                                transaction, unusedEvent, FrameGenerationRole::synthetic1) == S_OK,
                            "S1 actual output fence is complete before publication and pre-Begin awaitRole");
                    const auto target =
                        scenario == L"prebegin-expired" ? originalTargets[multiplier - 1] : originalTargets[0];
                    const auto selected =
                        runtime.xrTimeToOvrTime(target) + (scenario == L"prebegin-ontime" ? -.001 : .001);
                    require(selected >= capacityClockTime.load(), "pre-Begin fixture clock must never move backward");
                    capacityClockTime = selected;
                    const auto selectedNs = runtime.ovrTimeToXrTime(selected);
                    require(scenario != L"prebegin-ready-late" ||
                                (selectedNs > originalTargets[0] && selectedNs < originalTargets[multiplier - 1]),
                            "late-ready publication is strictly after S1 but before final B target");
                    std::wcout << L"PREBEGIN_SETUP scenario=" << scenario << L" serial="
                               << transaction.ticket.current_serial << L" prime=" << transaction.ticket.prime
                               << L" physically_ready=1 now=" << selectedNs << L" S1=" << originalTargets[0] << L" B="
                               << originalTargets[multiplier - 1] << L'\n';
                }
                if ((beginScenario || scenario == L"query-wait-slot-late") && group)
                    originalTargets = transaction.slots.targets;
                OwnedFrameTransaction pending;
                PredictionReservation pendingReservation;
                const auto activeToken = transaction.ticket.token;
                const auto activeBank = transaction.ticket.bank;
                if (delayedRetirement && group == 1)
                    heldRetirement = activeToken;
                ticketBanks.push_back(activeBank);
                if (pipeline)
                    std::cout << "SHARED_GROUP group=" << group << " bank=" << activeBank << '\n';
                if (pipeline && group + 1 < groups) {
                    std::lock_guard lock(gateMutex);
                    gateId = group * multiplier + 1;
                    atGate = releaseGate = false;
                }
                if (failedPending) {
                    pendingReservation =
                        cadence.reserve(first - period, first + multiplier * period, period, multiplier, multiplier);
                    auto nextInput = input;
                    nextInput.serial = 2;
                    nextInput.time = pendingReservation.currentTarget;
                    nextInput.slots = cadence.plan(pendingReservation, input.time, true);
                    nextInput.colour[0] = nextInput.colour[1] = hw.texture(&b);
                    success(runtime.m_frameGeneration->capture(nextInput, pending),
                            "retain second bank before failure");
                    hw.await(*runtime.m_frameGeneration, pending);
                    std::lock_guard lock(state.mutex);
                    state.gateEnd = true;
                    state.releaseEnd = false;
                    state.fault = Fault::End;
                }
                if (late && group && !preparedLate && !waitQuery) {
                    for (auto& target : transaction.slots.targets) {
                        target = 1;
                    }
                }
                const bool injectedFailure = group && scenario.rfind(L"fail-", 0) == 0;
                const unsigned failedSlot = scenario.find(L"-s1") != std::wstring::npos   ? 0
                                            : scenario.find(L"-s2") != std::wstring::npos ? 1
                                            : scenario.find(L"-s3") != std::wstring::npos ? 2
                                                                                          : multiplier - 1;
                if (injectedFailure) {
                    std::lock_guard lock(state.mutex);
                    state.faultFrameId = multiplier + failedSlot;
                    std::wcout << L"FAULT_INJECTION: scenario=" << scenario << L" multiplier=" << multiplier
                               << L" slot=" << failedSlot << L" native_id=" << state.faultFrameId << L'\n';
                    state.fault = scenario.find(L"wait") != std::wstring::npos    ? Fault::Wait
                                  : scenario.find(L"begin") != std::wstring::npos ? Fault::Begin
                                                                                  : Fault::End;
                    if (scenario.find(L"commit") != std::wstring::npos) {
                        state.fault = Fault::None;
                        commitFaultId = state.faultFrameId;
                    }
                }
                if (capacity && !group) {
                    std::lock_guard lock(state.mutex);
                    state.gateEnd = true;
                    state.releaseEnd = false;
                }
                if (group && scenario == L"stop-before-begin") {
                    state.observeWait = [&](long long id) {
                        if (id == static_cast<long long>(multiplier)) {
                            std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                            runtime.m_terminateAsyncThread = true;
                            runtime.m_asyncSubmissionCondVar.notify_all();
                        }
                    };
                }
                if (capacity) {
                    diagnosticFirstTarget = reservation.firstTarget;
                    const auto now = runtime.ovrTimeToXrTime(ovr_GetTimeInSeconds());
                    std::cout << "Capacity publish group=" << group << " now=" << now
                              << " S1=" << reservation.firstTarget << " lead_ns=" << reservation.firstTarget - now
                              << " prime=" << transaction.ticket.prime << " slots=" << transaction.slots.count
                              << " bank=" << transaction.ticket.bank
                              << " need_reprime=" << runtime.m_fgNeedReprime.load() << '\n';
                }
                {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    runtime.m_fgReservationForSubmission = reservation;
                    runtime.m_fgTransactionForSubmission = std::move(transaction);
                    runtime.m_layersForAsyncSubmission.resize(1);
                    runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                    runtime.m_asyncSubmissionReady = false;
                    runtime.m_asyncSubmissionCondVar.notify_all();
                }
                if (pipeline && group + 1 < groups) {
                    {
                        std::unique_lock lock(gateMutex);
                        if (!gateChanged.wait_for(lock, 10s, [&] { return atGate; })) {
                            std::lock_guard submissionLock(runtime.m_asyncSubmissionMutex);
                            if (runtime.m_asyncSubmissionError)
                                std::rethrow_exception(runtime.m_asyncSubmissionError);
                            require(false, "active group parks after its first native presentation");
                        }
                    }
                    runtime.waitForFrameGenerationCapacity();
                    std::unique_lock contextLease(runtime.m_fgContextMutex);
                    queuedReservation = cadence.reserve(first - period,
                                                        first + (group + 1) * multiplier * period,
                                                        period,
                                                        multiplier,
                                                        (group + 1) * multiplier);
                    auto next = input;
                    next.serial = group + 2;
                    next.time = queuedReservation.currentTarget;
                    next.slots = cadence.plan(queuedReservation, input.time, true);
                    next.colour[0] = next.colour[1] = hw.texture((group + 1) % 2 ? &b : &a);
                    success(runtime.m_frameGeneration->capture(next, queued), "overlapped next producer capture");
                    hw.await(*runtime.m_frameGeneration, queued);
                    require(queued.ticket.bank != activeBank, "live active ticket forces the other module bank");
                    auto& nextReal = runtime.m_fgPresentationBanks[queued.ticket.bank][2];
                    int image{};
                    require(OVR_SUCCESS(ovr_GetTextureSwapChainCurrentIndex(
                                runtime.m_ovrSession, nextReal.ovrSwapchain, &image)),
                            "next producer B current SDK index");
                    success(runtime.m_frameGeneration->copyOutput(
                                queued, FrameGenerationRole::real, nextReal.images[image].Get()),
                            "producer precopies B while current synthetic group is active");
                    const auto& currentReal =
                        presentationRings.at(reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6002 + activeBank * 4)));
                    const auto currentImage = group ? currentReal.current : currentReal.committed;
                    require(hw.read(currentReal.images[currentImage].Get()) == (group % 2 ? b : a),
                            "overlapped producer B cannot overwrite the active group's prepared real image");
                    contextLease.unlock();
                    {
                        std::lock_guard lock(gateMutex);
                        releaseGate = true;
                        gateChanged.notify_all();
                    }
                }
                if (group && scenario == L"stop-before-begin") {
                    require(state.waitFor([&] { return state.waitCalls > multiplier; }),
                            "termination reaches first native timing wait after current copies are staged");
                    RuntimeInputRegression::stop(runtime, state);
                    cleanup.Deactivate();
                    require(!runtime.m_asyncSubmissionError && !runtime.m_frameGeneration &&
                                !runtime.m_fgFailedTransaction.ticket.token && state.frames.size() == multiplier,
                            "staged ticket is retired without an extra End or submission fault on stop");
                    hw.gpu.noErrors();
                    return;
                }
                if (group && (scenario == L"query-wait-stop" || scenario == L"query-wait-error")) {
                    if (scenario == L"query-wait-error") {
                        std::unique_lock lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_asyncSubmissionCondVar.wait_for(
                                    lock, 2s, [&] { return runtime.m_asyncSubmissionError != nullptr; }),
                                "copy completion event registration error reaches the real worker error path");
                        require(runtime.m_fgFailedTransaction.ticket.token == activeToken,
                                "copy wait error retains its transaction for joined cleanup");
                    } else {
                        require(state.waitFor([&] { return state.frames.size() == multiplier + 1; }),
                                "stop after successful copy wait finishes the current native slot");
                    }
                    RuntimeInputRegression::stop(runtime, state);
                    cleanup.Deactivate();
                    require(!runtime.m_frameGeneration && !runtime.m_fgFailedTransaction.ticket.token &&
                                state.frames.size() == (scenario == L"query-wait-error" ? multiplier : multiplier + 1),
                            "copy wait stop/error cleanup emits no later native End and retains no ticket");
                    require(bool(runtime.m_asyncSubmissionError) == (scenario == L"query-wait-error"),
                            "successful copy wait stop stays distinct from an event registration error");
                    readyQueries.validate(multiplier, false);
                    hw.gpu.noErrors();
                    return;
                }
                if (failedPending) {
                    require(state.waitFor([&] { return state.enteredEnd == 1; }), "failed-active gate reached");
                    const auto pendingToken = pending.ticket.token;
                    {
                        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_asyncSubmissionReady && runtime.m_fgActiveOwnedPixels,
                                "owned active group permits one pending payload");
                        runtime.m_fgReservationForSubmission = pendingReservation;
                        runtime.m_fgTransactionForSubmission = std::move(pending);
                        runtime.m_layersForAsyncSubmission.resize(1);
                        runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                        runtime.m_asyncSubmissionReady = false;
                        runtime.m_asyncSubmissionCondVar.notify_all();
                    }
                    state.release();
                    {
                        std::unique_lock lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_asyncSubmissionCondVar.wait_for(
                                    lock, 2s, [&] { return runtime.m_asyncSubmissionError != nullptr; }),
                                "native End failure wakes producers");
                        require(runtime.m_fgFailedTransaction.ticket.token == activeToken &&
                                    runtime.m_fgTransactionForSubmission.ticket.token == pendingToken,
                                "failed active cannot overwrite pending bank retirement identity");
                    }
                    RuntimeInputRegression::stop(runtime, state);
                    cleanup.Deactivate();
                    require(!runtime.m_frameGeneration && !runtime.m_fgFailedTransaction.ticket.token &&
                                !runtime.m_fgTransactionForSubmission.ticket.token,
                            "joined failure cleanup settles both retained transactions before unloading");
                    hw.gpu.noErrors();
                    return;
                }
                if (capacity && !group) {
                    require(state.waitFor([&] { return state.enteredEnd == 1; }), "active group reached parked End");
                    std::atomic<bool> admitted{};
                    std::thread producer([&] {
                        runtime.waitForFrameGenerationCapacity();
                        admitted = true;
                    });
                    const auto deadline = std::chrono::steady_clock::now() + 200ms;
                    while (!admitted && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(1ms);
                    }
                    const bool overlapAllowed = admitted;
                    state.release();
                    producer.join();
                    require(overlapAllowed, "owned active pixels free one pending mailbox before native End completes");
                }
                if (injectedFailure) {
                    require(state.waitFor([&] { return state.faultCalls == 1; }),
                            "fault reaches its intended native slot");
                    bool failed{};
                    try {
                        runtime.waitForAsyncSubmissionIdle();
                    } catch (const std::exception&) {
                        failed = true;
                    }
                    require(failed, "partial presentation stops the producer with an explicit runtime error");
                    {
                        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_fgFailedTransaction.ticket.token == activeToken,
                                "failed synthetic group retains its ticket until joined cleanup");
                    }
                    {
                        std::lock_guard lock(state.mutex);
                        // End records its attempted layer before returning the injected error;
                        // Wait/Begin failures have no submitted layer for their failed slot.
                        const unsigned observedPairSlots = failedSlot + (state.fault == Fault::End ? 1u : 0u);
                        require(state.frames.size() == multiplier + observedPairSlots &&
                                    frames.size() == state.frames.size() && chains.size() == frames.size(),
                                "failure does not replay earlier synthetic slots or present later slots");
                        for (unsigned i = 0; i < state.frames.size(); ++i) {
                            require(state.frames[i].id == i, "failed cycle does not reuse a compositor ID");
                        }
                        for (unsigned slot = 0; slot < observedPairSlots; ++slot) {
                            const auto role = slot == multiplier - 1 ? FrameGenerationRole::real
                                              : slot == 0            ? FrameGenerationRole::synthetic1
                                              : slot == 1            ? FrameGenerationRole::synthetic2
                                                                     : FrameGenerationRole::synthetic3;
                            const auto index = multiplier + slot;
                            const auto expectedChain = reinterpret_cast<ovrTextureSwapChain>(
                                uintptr_t(0x6000 + (role == FrameGenerationRole::real ? activeBank * 4 : 0) +
                                          static_cast<unsigned>(role)));
                            require(chains[index] == expectedChain,
                                    "each observed fault-path slot uses its intended role and owned bank");
                            if (role == FrameGenerationRole::real) {
                                require(frames[index] == b, "failed End at B still records bit-exact real pixels");
                            } else {
                                require(frames[index] != a && frames[index] != b,
                                        "every observed synthetic fault-path slot contains an intermediate frame");
                            }
                            for (unsigned earlier = 0; earlier < slot; ++earlier) {
                                require(frames[index] != frames[multiplier + earlier] &&
                                            chains[index] != chains[multiplier + earlier],
                                        "observed fault-path roles have distinct pixels and separate chains");
                            }
                        }
                    }
                    RuntimeInputRegression::stop(runtime, state);
                    cleanup.Deactivate();
                    hw.gpu.noErrors();
                    return;
                }
                const bool presented =
                    state.waitFor([&] { return state.frames.size() == (group + 1) * multiplier; }, 10s);
                if (preBegin && !presented) {
                    std::lock_guard failure(runtime.m_asyncSubmissionMutex);
                    if (runtime.m_asyncSubmissionError)
                        std::rethrow_exception(runtime.m_asyncSubmissionError);
                }
                require(presented, "bounded native presentation group");
                if (delayedRetirement && group == 1) {
                    const auto deadline = std::chrono::steady_clock::now() + 2s;
                    while (!retirementBusyCalls && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(1ms);
                    require(retirementBusyCalls != 0, "real worker reached delayed module retirement");
                    const auto pendingToken = queued.ticket.token;
                    ticketBanks.push_back(queued.ticket.bank);
                    {
                        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_fgActiveSubmission, "unretired group still owns shared synthetic roles");
                        runtime.m_fgReservationForSubmission = queuedReservation;
                        runtime.m_fgTransactionForSubmission = std::move(queued);
                        queued = {};
                        runtime.m_layersForAsyncSubmission.resize(1);
                        runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                        runtime.m_asyncSubmissionReady = false;
                        runtime.m_asyncSubmissionCondVar.notify_all();
                    }
                    const auto busyBefore = retirementBusyCalls.load();
                    std::this_thread::sleep_for(20ms);
                    {
                        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                        require(runtime.m_fgActiveSubmission &&
                                    runtime.m_fgTransactionForSubmission.ticket.token == pendingToken &&
                                    retirementBusyCalls > busyBefore,
                                "pending group cannot dequeue or reuse synthetic destinations before retirement");
                    }
                    for (unsigned role : {0u, 1u, 3u}) {
                        const auto found =
                            presentationRings.find(reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6000 + role)));
                        if (found != presentationRings.end())
                            require(found->second.commits == 1,
                                    "retirement delay preserves prior synthetic ring state");
                    }
                    heldRetirement = 0;
                    require(state.waitFor([&] { return state.frames.size() == groups * multiplier; }, 10s),
                            "pending group presents after actual module retirement succeeds");
                    break;
                }
                previous = input.time;
            }
            runtime.waitForAsyncSubmissionIdle();
            if (pipeline) {
                require(ticketBanks == (delayedRetirement ? std::vector<unsigned>({0, 1, 0})
                                                          : std::vector<unsigned>({0, 1, 0, 1, 0})),
                        "real groups alternate both live module ticket banks");
                require(frames.size() == groups * multiplier, "every pipelined native slot reaches End");
                for (unsigned group = 0; group < groups; ++group) {
                    const auto& real = group % 2 ? b : a;
                    for (unsigned slot = 0; slot < multiplier; ++slot) {
                        const auto position = group * multiplier + slot;
                        const unsigned role = !group || slot == multiplier - 1 ? 2 : slot == 2 ? 3 : slot;
                        const auto expectedChain = reinterpret_cast<ovrTextureSwapChain>(
                            uintptr_t(0x6000 + (role == 2 ? ticketBanks[group] * 4 : 0) + role));
                        require(chains[position] == expectedChain && state.frames[position].id == position,
                                "each bank uses the exact intended synthetic role and consecutive native ID");
                        require(role == 2 ? frames[position] == real : frames[position] != a && frames[position] != b,
                                "current SDK ring image contains exact real pixels or an intermediate synthetic");
                        if (role != 2)
                            for (unsigned earlier = 0; earlier < slot; ++earlier)
                                require(frames[position] != frames[group * multiplier + earlier],
                                        "synthetic roles remain pixel-distinct after cross-bank ring reuse");
                    }
                }
                for (const auto& [chain, ring] : presentationRings)
                    require(ring.commits >=
                                (delayedRetirement && chain == reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6006))
                                     ? 1u
                                     : 2u),
                            "retained SDK chains advance and reuse their image rings");
                std::cout << "SHARED_PRESENTATION groups=" << groups << " delayed_retire_calls=" << retirementBusyCalls
                          << " ring_images=" << (scenario == L"shared-ring-one" ? 1 : 3)
                          << " native_slots=" << frames.size() << '\n';
                RuntimeInputRegression::stop(runtime, state);
                cleanup.Deactivate();
                hw.gpu.noErrors();
                return;
            }
            {
                std::lock_guard lock(state.mutex);
                require(frames.size() == 2 * multiplier, "prime and pair use native slot counts");
                for (unsigned i = 0; i < frames.size(); ++i) {
                    require(state.frames[i].id == i && state.waitedIds[i] == i && state.begunIds[i] == i,
                            "native Wait/Begin/End IDs are independent, consecutive and monotonic");
                    if (i < multiplier) {
                        require(frames[i] == a, "prime padding repeats exact real input");
                    }
                }
                require(frames.back() == b, "native B output is bit-exact");
                if (capacity) {
                    for (unsigned index = multiplier; index < frames.size(); ++index) {
                        size_t differingBytes{};
                        for (size_t byte = 0; byte < b.size(); ++byte)
                            differingBytes += frames[index][byte] != b[byte];
                        std::cout << "Capacity output frame=" << index << " equals_A=" << (frames[index] == a)
                                  << " equals_B=" << (frames[index] == b) << " differs_B_bytes=" << differingBytes
                                  << " chain_is_B=" << (chains[index] == chains.back())
                                  << " need_reprime=" << runtime.m_fgNeedReprime.load() << '\n';
                    }
                }
                if (preBegin) {
                    const bool expired = scenario == L"prebegin-expired";
                    bool exactRoles = true;
                    for (unsigned slot = 0; slot < multiplier; ++slot) {
                        const bool real = expired || slot == multiplier - 1;
                        const auto role = real ? 2u : slot == 2 ? 3u : slot;
                        const auto expectedChain = reinterpret_cast<ovrTextureSwapChain>(
                            uintptr_t(0x6000 + (real ? ticketBanks.back() * 4 : 0) + role));
                        const auto index = multiplier + slot;
                        const bool exact =
                            chains[index] == expectedChain && frames[index] == (real ? b : originalDelayedPixels[slot]);
                        exactRoles &= exact;
                        std::cout << "PREBEGIN_ROLE native_id=" << index << " slot=" << slot
                                  << " expected_role=" << role << " exact_B=" << (frames[index] == b)
                                  << " exact_original=" << exact << '\n';
                    }
                    require(!runtime.m_fgNeedReprime.load(), "presentation miss must preserve captured history");
                    require(runtime.m_fgTimelineReanchor.load() == (scenario != L"prebegin-ontime"),
                            "only a missed presentation requests timeline recovery");
                    {
                        std::unique_lock lease(runtime.m_fgContextMutex);
                        const auto next = cadence.reserve(
                            first - period, first + 2 * multiplier * period, period, multiplier, 2 * multiplier);
                        FinalProjectionInput follow;
                        follow.serial = 3;
                        follow.time = next.currentTarget;
                        follow.period = period;
                        follow.slots = cadence.plan(next, previous, true);
                        follow.colour[0] = follow.colour[1] = hw.texture(&a);
                        for (unsigned eye = 0; eye < 2; ++eye) {
                            follow.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
                            follow.views[eye].orientation[3] = 1;
                            follow.views[eye].position[0] = eye ? .032f : -.032f;
                            follow.views[eye].fov[0] = follow.views[eye].fov[3] = -.8f;
                            follow.views[eye].fov[1] = follow.views[eye].fov[2] = .8f;
                        }
                        OwnedFrameTransaction history;
                        success(runtime.m_frameGeneration->capture(follow, history),
                                "capture follow-up history witness");
                        hw.await(*runtime.m_frameGeneration, history);
                        require(!history.ticket.prime && history.ticket.previous_serial == 2 &&
                                    history.ticket.current_serial == 3 && history.ownerGeneration == pairGeneration,
                                "follow-up actual ticket keeps pair history and generation");
                        std::cout << "PREBEGIN_HISTORY previous=" << history.ticket.previous_serial
                                  << " current=" << history.ticket.current_serial << " prime=" << history.ticket.prime
                                  << '\n';
                        hw.retire(*runtime.m_frameGeneration, history);
                    }
                    require(exactRoles,
                            "pre-Begin already-ready S retains exact original role pixels until final B is due");
                } else if (beginScenario) {
                    const bool pending = readyQueries.mode == CopyReadyQueryProbe::Mode::BeginPending;
                    const bool fallback = pending || readyQueries.finalBDue || readyQueries.hardAfterSoft;
                    const unsigned fallbackSlot = readyQueries.hardAfterSoft ? 1u : readyQueries.delayedSlot;
                    const auto& delayedPixels = originalDelayedPixels[readyQueries.delayedSlot];
                    require(
                        delayedBegins == (readyQueries.hardAfterSoft ? 2u
                                          : readyQueries.multiLate   ? multiplier - 1
                                                                     : 1) &&
                            delayedPixels != a && delayedPixels != b,
                        "controlled Begin delays one native slot containing the original prepared synthetic pixels");
                    std::wcout << L"LATE_BEGIN_SCENARIO scenario=" << scenario << L" delayed_slot="
                               << readyQueries.delayedSlot << L'\n';
                    for (unsigned slot = 0; slot < multiplier; ++slot) {
                        const bool real = slot == multiplier - 1 || (fallback && slot >= fallbackSlot);
                        const unsigned role = real ? 2u : slot == 2 ? 3u : slot;
                        const auto index = multiplier + slot;
                        const auto expectedChain = reinterpret_cast<ovrTextureSwapChain>(
                            uintptr_t(0x6000 + (real ? ticketBanks.back() * 4 : 0) + role));
                        std::cout << "LATE_BEGIN_ROLE native_id=" << index << " slot=" << slot
                                  << " expected_role=" << role << " exact_B=" << (frames[index] == b)
                                  << " chain_is_B=" << (chains[index] == chains.back())
                                  << " scripted_pending=" << pending << " final_B_due=" << readyQueries.finalBDue
                                  << '\n';
                        require(chains[index] == expectedChain &&
                                    (real ? frames[index] == b : frames[index] != a && frames[index] != b),
                                "late ready S is retained only before B is due; pending or expired groups select "
                                "immediate sticky B");
                    }
                    if (!fallback || readyQueries.hardAfterSoft)
                        require(frames[multiplier + readyQueries.delayedSlot] == delayedPixels,
                                "late ready slot presents bit-exact original prepared fraction, without resynthesis");
                    if (fallback && fallbackSlot + 1 < multiplier - 1)
                        require(readyQueries.readyBeforeBeginReturn[multiplier + fallbackSlot + 1],
                                "later synthetic is actually copy-ready while sticky B still forbids its presentation");
                    if (readyQueries.hardAfterSoft) {
                        require(simulatedReanchorConsumed && runtime.m_fgTimelineReanchor.load() &&
                                    !runtime.m_fgNeedReprime.load(),
                                "hard B fallback must reassert reanchor after simulated producer consumption of a "
                                "retained soft miss");
                    } else if (readyQueries.multiLate) {
                        for (unsigned slot = 0; slot < multiplier - 1; ++slot)
                            require(frames[multiplier + slot] == originalDelayedPixels[slot],
                                    "multiple moderate late slots retain their original distinct prepared fractions");
                        require(simulatedReanchorConsumed && !runtime.m_fgTimelineReanchor.load() &&
                                    !runtime.m_fgNeedReprime.load(),
                                "simulated producer consumption of first miss must not be rearmed by later slots in "
                                "the group");
                    } else {
                        require(runtime.m_fgTimelineReanchor.load() && !runtime.m_fgNeedReprime.load(),
                                "late Begin requests next-frame timing reanchor while retaining captured history");
                    }
                } else if (late && scenario != L"query-wait-slot-late") {
                    for (unsigned i = multiplier; i < frames.size(); ++i) {
                        require(frames[i] == b, "late group uses real B, never retimed synthetic roles");
                    }
                    require(runtime.m_fgTimelineReanchor.load() && !runtime.m_fgNeedReprime.load(),
                            "presentation lateness recovers native timing without discarding valid captured history");
                    if (preparedLate)
                        for (unsigned role : {0u, 1u, 3u}) {
                            const auto found =
                                presentationRings.find(reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x6000 + role)));
                            if (found != presentationRings.end()) {
                                require(found->second.commits == 0,
                                        "prepared synthetic images are never committed after missing the first slot");
                                const auto pixels = hw.read(found->second.images[found->second.current].Get());
                                require(
                                    pixels != a && pixels != b,
                                    "missed-slot control actually copied each intermediate synthetic before fallback");
                            }
                        }
                } else {
                    require(frames[multiplier] != a && frames[multiplier] != b && chains[multiplier] != chains.back(),
                            "presented S1 is distinct and committed on its own chain");
                    if (scenario == L"query-wait-slot-late")
                        require(runtime.m_fgTimelineReanchor.load() && !runtime.m_fgNeedReprime.load(),
                                "retained late completion reanchors timing without invalidating captured history");
                    for (unsigned firstRole = multiplier; firstRole < frames.size(); ++firstRole)
                        for (unsigned secondRole = firstRole + 1; secondRole < frames.size(); ++secondRole)
                            require(frames[firstRole] != frames[secondRole] && chains[firstRole] != chains[secondRole],
                                    "every synthetic and B has distinct pixels and a separate committed chain");
                }
            }
            RuntimeInputRegression::stop(runtime, state);
            cleanup.Deactivate();
            readyQueries.validate(multiplier, late);
            hw.gpu.noErrors();
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(argc == 5 || argc == 6, "backend, module, multiplier, scenario and optional refresh required");
        const unsigned multiplier = std::wcstoul(argv[3], nullptr, 10);
        require(multiplier >= 2 && multiplier <= 4, "2X, 3X or 4X required");
        const std::wstring scenario = argv[4];
        if (scenario == L"bgrx" || scenario == L"unequal-eyes" || scenario == L"static" || scenario == L"protected" ||
            scenario == L"nr-off" || scenario == L"low-coverage" || scenario == L"app-submission" ||
            scenario == L"synchronous" || scenario == L"depth" || scenario == L"overlay" ||
            scenario == L"second-projection") {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::bypass(scenario);
        } else if (scenario.rfind(L"allocation-", 0) == 0) {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::allocation(argv[1], argv[2], multiplier, scenario);
        } else if (scenario == L"cleanup-producer") {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::cleanupProducer(argv[1], argv[2]);
        } else if (scenario == L"status-snapshots") {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::statusSnapshots(argv[1]);
        } else if (scenario.rfind(L"startup-", 0) == 0) {
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::startup(argv[1], scenario);
        } else if (scenario == L"live-miss" || scenario == L"live-pending-miss") {
            const unsigned rate = argc == 6 ? std::wcstoul(argv[5], nullptr, 10) : 100;
            require(rate >= 30 && rate <= 240, "bounded live-test refresh rate");
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::liveRecovery(
                argv[1], argv[2], multiplier, rate, scenario == L"live-pending-miss");
        } else if (scenario.rfind(L"history-", 0) == 0) {
            const unsigned rate = argc == 6 ? std::wcstoul(argv[5], nullptr, 10) : 100;
            require(rate >= 30 && rate <= 240, "bounded history-test refresh rate");
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::history(
                argv[1], argv[2], scenario, multiplier, rate);
        } else {
            require(scenario == L"prebegin-ontime" || scenario == L"prebegin-ready-late" ||
                        scenario == L"prebegin-expired" || scenario == L"normal" || scenario == L"ring-one" ||
                        scenario == L"shared-ring" || scenario == L"shared-ring-one" || scenario == L"shared-retire" ||
                        scenario == L"late" || scenario == L"capacity" || scenario == L"prepared-late" ||
                        scenario == L"capacity-late" || scenario == L"copy-ready-query" || scenario == L"query-late" ||
                        scenario == L"query-wait-ready" || scenario == L"query-wait-busy-final" ||
                        scenario == L"query-wait-stale" || scenario == L"query-wait-late" ||
                        scenario == L"query-wait-slot-late" || scenario == L"query-wait-stop" ||
                        scenario == L"query-wait-error" || scenario == L"begin-ready-s1" ||
                        scenario == L"begin-ready-s2" || scenario == L"begin-ready-s3" ||
                        scenario == L"begin-pending-s1" || scenario == L"begin-pending-s2" ||
                        scenario == L"begin-pending-s3" || scenario == L"begin-expired-s1" ||
                        scenario == L"begin-expired-s2" || scenario == L"begin-expired-s3" ||
                        scenario == L"begin-at-b-s1" || scenario == L"begin-multi-ready" ||
                        scenario == L"begin-soft-hard-s2" || scenario == L"stop-before-begin" ||
                        scenario == L"failed-pending" || scenario == L"fail-wait-s1" || scenario == L"fail-wait-s2" ||
                        scenario == L"fail-wait-s3" || scenario == L"fail-wait-b" || scenario == L"fail-begin-s1" ||
                        scenario == L"fail-begin-s2" || scenario == L"fail-begin-s3" || scenario == L"fail-begin-b" ||
                        scenario == L"fail-end-s1" || scenario == L"fail-end-s2" || scenario == L"fail-end-s3" ||
                        scenario == L"fail-end-b" || scenario == L"fail-commit-s1" || scenario == L"fail-commit-s2" ||
                        scenario == L"fail-commit-s3" || scenario == L"fail-commit-b",
                    "unknown worker scenario");
            require(scenario.find(L"-s2") == std::wstring::npos || multiplier >= 3,
                    "S2 fault requires a three- or four-frame multiplier");
            require(scenario.find(L"-s3") == std::wstring::npos || multiplier == 4,
                    "S3 fault requires a four-frame multiplier");
            require((scenario != L"begin-multi-ready" && scenario != L"begin-soft-hard-s2") || multiplier == 4,
                    "multi-late reanchor consumption uses a four-frame group");
            virtualdesktop_openxr::RuntimeFrameGenerationRegression::run(
                argv[1],
                argv[2],
                multiplier,
                scenario == L"late" || scenario == L"capacity-late" || scenario == L"prepared-late" ||
                    scenario == L"query-late" || scenario == L"query-wait-busy-final" ||
                    scenario == L"query-wait-stale" || scenario == L"query-wait-late" ||
                    scenario == L"query-wait-slot-late",
                scenario == L"capacity" || scenario == L"capacity-late",
                scenario == L"failed-pending",
                scenario);
        }
        std::cout << "PASS: real GPU native presenter roles, retained pixels, IDs and late bypass\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
