#include "PairCaptureNrReturnGuard.h"
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        thread_local SourceReturnGuard* activeGuard{};
        uint64_t clock() noexcept {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            return static_cast<uint64_t>(q.QuadPart);
        }
        NrCallV1 beginCall(uint64_t kind,
                           uint64_t resultType,
                           ID3D11Device* device,
                           ID3D11DeviceContext4* context,
                           ID3D11Fence* fence,
                           uint64_t value) noexcept {
            NrCallV1 c{};
            c.kind = kind;
            c.resultType = resultType;
            c.beginObserved = 1;
            c.eye = 2;
            c.qpcBegin = clock();
            c.thread = GetCurrentThreadId();
            c.deviceAddress = reinterpret_cast<uint64_t>(device);
            c.contextAddress = c.queueAddress = reinterpret_cast<uint64_t>(context);
            c.fenceAddress = reinterpret_cast<uint64_t>(fence);
            c.signalValue = value;
            return c;
        }
        void endCall(NrCallV1& c, int32_t result = 0) noexcept {
            c.qpcEnd = clock();
            c.endObserved = 1;
            c.resultBits = static_cast<uint32_t>(result);
            c.classifiedSuccess = c.resultType != NR_VOID && result >= 0;
            c.observedMask = NR_CALL_OBS;
        }
    } // namespace
    SourceReturnGuard::SourceReturnGuard() noexcept {
        if (hostRequest().valid) {
            previous_ = activeGuard;
            activeGuard = this;
            entered_ = true;
        }
    }
    SourceReturnGuard::~SourceReturnGuard() noexcept {
        if (armed_ && facts_.action() != GuardAction::none) {
            if (!finish())
                terminate(NR_BAD_GROUP);
            if (!transferred_)
                hostCaptureTerminal(NR_BAD_OPERATION, false);
        }
        if (entered_)
            activeGuard = previous_;
    }
    SourceReturnGuard* SourceReturnGuard::current() noexcept {
        return activeGuard;
    }
    bool SourceReturnGuard::arm(const ReturnBinding& b) noexcept {
        if (!entered_ || previous_ || armed_ || !b.producerDevice || !b.producerContext || !b.producerFence ||
            !b.applicationDevice || !b.applicationContext || !b.applicationFence || !b.value || b.value == UINT64_MAX ||
            !b.nrContext || !b.nrContextLifetime || b.nrContextLifetime == UINT64_MAX || !b.originalNrFence ||
            !b.originalOutputAlias || b.raw.actual.queue != reinterpret_cast<uint64_t>(b.producerContext) ||
            b.raw.actual.fence != reinterpret_cast<uint64_t>(b.producerFence) || b.raw.boundary != b.value ||
            b.raw.fence11 != b.producerFence || b.raw.fence12)
            return false;
        const auto state = hostWorkOwner().snapshot(b.raw.id);
        if (!state.valid || !state.sealed || state.pendingAcquisitions || state.referenceInventory < 7)
            return false;
        binding_ = b;
        armed_ = true;
        return true;
    }
    bool SourceReturnGuard::exposeRaw() noexcept {
        if (!armed_ || facts_.rawExposed)
            return false;
        if (!hostWorkOwner().exposeImmediate(binding_.raw.id, reinterpret_cast<uint64_t>(binding_.producerContext)))
            return false;
        facts_.rawExposed = true;
        return true;
    }
    bool SourceReturnGuard::exposePostFx(const OwnedGroupBoundary& b) noexcept {
        if (!armed_ || facts_.postFxExposed || b.actual.queue != binding_.raw.actual.queue ||
            b.actual.fence != binding_.raw.actual.fence || b.boundary != binding_.value)
            return false;
        if (!hostWorkOwner().exposeImmediate(b.id, b.actual.queue))
            return false;
        postFx_ = b;
        facts_.postFxExposed = true;
        return true;
    }
    void SourceReturnGuard::recordNrPrivateCommands() noexcept {
        if (armed_)
            facts_.nrExposed = true;
    }
    void SourceReturnGuard::observeNr(const NrWarmSubmissionFacts& actual, bool qualified) noexcept {
        if (!armed_)
            return;
        // Negative Close with factual no Execute is safely unsubmitted. A successful
        // Close followed by an interrupted Submit remains conservative uncertainty.
        facts_.nrExecutePossible =
            actual.calls.executeObservations != 0 ||
            (actual.calls.close.observed && actual.calls.close.result >= 0 && !actual.calls.original.observed);
        facts_.nrSignalObserved = actual.calls.original.observed;
        nrSignalValue_ = actual.calls.original.value;
        facts_.nrSignalSucceeded =
            qualified && actual.calls.original.observed && actual.calls.original.result >= 0 &&
            actual.context == binding_.nrContext && actual.contextLifetime == binding_.nrContextLifetime &&
            actual.calls.original.actual.fence == reinterpret_cast<uint64_t>(binding_.originalNrFence);
    }
    void SourceReturnGuard::observeOutputWait(ID3D11DeviceContext4* context,
                                              ID3D11Fence* fence,
                                              uint64_t value,
                                              int32_t result,
                                              uint64_t begin,
                                              uint64_t end) noexcept {
        if (!armed_)
            return;
        nrWait_ = {};
        nrWait_.kind = NR_WAIT;
        nrWait_.resultType = NR_HRESULT;
        nrWait_.beginObserved = nrWait_.endObserved = 1;
        nrWait_.resultBits = static_cast<uint32_t>(result);
        nrWait_.classifiedSuccess = result >= 0;
        nrWait_.qpcBegin = begin;
        nrWait_.qpcEnd = end;
        nrWait_.thread = GetCurrentThreadId();
        nrWait_.eye = 2;
        nrWait_.observedMask = NR_CALL_OBS;
        nrWait_.deviceAddress = reinterpret_cast<uint64_t>(binding_.producerDevice);
        nrWait_.queueAddress = nrWait_.contextAddress = reinterpret_cast<uint64_t>(context);
        nrWait_.fenceAddress = reinterpret_cast<uint64_t>(fence);
        nrWait_.signalValue = value;
        facts_.outputWaitObserved = true;
        facts_.outputWaitSucceeded = result >= 0 && context == binding_.producerContext &&
                                     fence == binding_.originalOutputAlias && value == nrSignalValue_ && value &&
                                     value != UINT64_MAX;
    }
    void SourceReturnGuard::observeInputSignal(ID3D11Device* device,
                                               ID3D11DeviceContext4* context,
                                               ID3D11Fence* fence,
                                               uint64_t value,
                                               int32_t result,
                                               uint64_t begin,
                                               uint64_t end) noexcept {
        auto& call = inputCalls_[0];
        call = {};
        call.kind = NR_SIGNAL;
        call.eye = 2;
        call.deviceAddress = reinterpret_cast<uint64_t>(device);
        call.contextAddress = call.queueAddress = reinterpret_cast<uint64_t>(context);
        call.fenceAddress = reinterpret_cast<uint64_t>(fence);
        call.signalValue = value;
        call.resultType = NR_HRESULT;
        call.resultBits = static_cast<uint32_t>(result);
        call.classifiedSuccess = result >= 0;
        call.beginObserved = call.endObserved = 1;
        call.qpcBegin = begin;
        call.qpcEnd = end;
        call.thread = GetCurrentThreadId();
        call.observedMask = NR_CALL_OBS;
    }
    void SourceReturnGuard::observeInputWait(ID3D12Device* device,
                                             const void* context,
                                             uint64_t lifetime,
                                             ID3D12CommandQueue* queue,
                                             ID3D12Fence* fence,
                                             uint64_t value,
                                             int32_t result,
                                             uint64_t begin,
                                             uint64_t end) noexcept {
        auto& call = inputCalls_[1];
        call = {};
        call.kind = NR_WAIT;
        call.eye = 2;
        call.deviceAddress = reinterpret_cast<uint64_t>(device);
        call.contextAddress = reinterpret_cast<uint64_t>(context);
        call.contextLifetimeEpoch = lifetime;
        call.queueAddress = reinterpret_cast<uint64_t>(queue);
        call.fenceAddress = reinterpret_cast<uint64_t>(fence);
        call.signalValue = value;
        call.resultType = NR_HRESULT;
        call.resultBits = static_cast<uint32_t>(result);
        call.classifiedSuccess = result >= 0;
        call.beginObserved = call.endObserved = 1;
        call.qpcBegin = begin;
        call.qpcEnd = end;
        call.thread = GetCurrentThreadId();
        call.observedMask = NR_CALL_OBS;
    }
    void SourceReturnGuard::observeSubmission(const NrWarmSubmissionFacts& facts,
                                              uint64_t begin,
                                              uint64_t end) noexcept {
        submissionFacts_ = facts;
        submissionBegin_ = begin;
        submissionEnd_ = end;
        submissionThread_ = GetCurrentThreadId();
    }
    [[noreturn]] void SourceReturnGuard::terminate(uint64_t reason) noexcept {
        hostCaptureTerminal(reason, true);
        ExitProcess(0x4e520001u);
    }
    bool SourceReturnGuard::finish() noexcept {
        if (!armed_)
            return false;
        const auto action = facts_.action();
        if (action == GuardAction::none)
            return true;
        if (action == GuardAction::terminateChild)
            terminate(NR_BAD_GROUP);
        if (action == GuardAction::covered) {
            finished_ = true;
            return true;
        }
        calls_[0] = beginCall(NR_SIGNAL,
                              NR_HRESULT,
                              binding_.producerDevice,
                              binding_.producerContext,
                              binding_.producerFence,
                              binding_.value);
        const auto signal = binding_.producerContext->Signal(binding_.producerFence, binding_.value);
        endCall(calls_[0], signal);
        facts_.producerSignalObserved = true;
        facts_.producerSignalSucceeded = signal >= 0;
        if (signal < 0)
            terminate(NR_BAD_GROUP);
        bool ownerRecorded = true;
        if (facts_.rawExposed)
            ownerRecorded =
                hostWorkOwner().observeSignal(binding_.raw.id, binding_.raw.actual, binding_.value, signal, false) &&
                ownerRecorded;
        if (facts_.postFxExposed)
            ownerRecorded = hostWorkOwner().observeSignal(postFx_.id, postFx_.actual, binding_.value, signal, false) &&
                            ownerRecorded;
        calls_[1] = beginCall(NR_FLUSH, NR_VOID, binding_.producerDevice, binding_.producerContext, nullptr, 0);
        binding_.producerContext->Flush();
        endCall(calls_[1]);
        facts_.flushObserved = true;
        calls_[2] = beginCall(NR_WAIT,
                              NR_HRESULT,
                              binding_.applicationDevice,
                              binding_.applicationContext,
                              binding_.applicationFence,
                              binding_.value);
        const auto wait = binding_.applicationContext->Wait(binding_.applicationFence, binding_.value);
        endCall(calls_[2], wait);
        facts_.appWaitObserved = true;
        facts_.appWaitSucceeded = wait >= 0;
        if (wait < 0)
            terminate(NR_BAD_GROUP);
        calls_[3] = beginCall(NR_FLUSH, NR_VOID, binding_.applicationDevice, binding_.applicationContext, nullptr, 0);
        binding_.applicationContext->Flush();
        endCall(calls_[3]);
        finished_ = facts_.action() == GuardAction::covered;
        if (!ownerRecorded || !finished_)
            terminate(NR_BAD_GROUP);
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
