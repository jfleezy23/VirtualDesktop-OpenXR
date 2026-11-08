#pragma once
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrWarmSubmission.h"
namespace virtualdesktop_openxr::nr_capture {
    struct ReturnBinding {
        ID3D11Device* producerDevice{};
        ID3D11DeviceContext4* producerContext{};
        ID3D11Fence* producerFence{};
        ID3D11Device* applicationDevice{};
        ID3D11DeviceContext4* applicationContext{};
        ID3D11Fence* applicationFence{};
        uint64_t value{}, nrContext{}, nrContextLifetime{};
        ID3D12Fence* originalNrFence{};
        ID3D11Fence* originalOutputAlias{};
        OwnedGroupBoundary raw{};
    };
    // Caller-thread scope only; no Runtime pointer or worker callback. The actual
    // groups own all borrowed COM objects before arm and before any private copy.
    class SourceReturnGuard final {
      public:
        SourceReturnGuard() noexcept;
        ~SourceReturnGuard() noexcept;
        SourceReturnGuard(const SourceReturnGuard&) = delete;
        SourceReturnGuard& operator=(const SourceReturnGuard&) = delete;
        bool arm(const ReturnBinding&) noexcept;
        bool exposeRaw() noexcept;
        bool exposePostFx(const OwnedGroupBoundary&) noexcept;
        void recordNrPrivateCommands() noexcept;
        void observeNr(const NrWarmSubmissionFacts&, bool submissionQualified) noexcept;
        void observeOutputWait(ID3D11DeviceContext4* actualContext,
                               ID3D11Fence* actualFence,
                               uint64_t actualValue,
                               int32_t actualResult,
                               uint64_t begin,
                               uint64_t end) noexcept;
        void observeInputSignal(
            ID3D11Device*, ID3D11DeviceContext4*, ID3D11Fence*, uint64_t, int32_t, uint64_t, uint64_t) noexcept;
        void observeInputWait(ID3D12Device*,
                              const void* context,
                              uint64_t contextLifetime,
                              ID3D12CommandQueue*,
                              ID3D12Fence*,
                              uint64_t,
                              int32_t,
                              uint64_t,
                              uint64_t) noexcept;
        // Bounds of the enclosing original Submit, not individual pool call timings.
        void observeSubmission(const NrWarmSubmissionFacts&, uint64_t begin, uint64_t end) noexcept;
        const std::array<NrCallV1, 2>& inputCalls() const noexcept {
            return inputCalls_;
        }
        const NrWarmSubmissionFacts& submissionFacts() const noexcept {
            return submissionFacts_;
        }
        uint64_t submissionBegin() const noexcept {
            return submissionBegin_;
        }
        uint64_t submissionEnd() const noexcept {
            return submissionEnd_;
        }
        uint64_t submissionThread() const noexcept {
            return submissionThread_;
        }
        bool finish() noexcept;
        void transferred() noexcept {
            transferred_ = finished_;
        }
        bool covered() const noexcept {
            return finished_ && facts_.action() == GuardAction::covered;
        }
        const GuardFacts& facts() const noexcept {
            return facts_;
        }
        const NrCallV1& originalWait() const noexcept {
            return nrWait_;
        }
        const std::array<NrCallV1, 4>& calls() const noexcept {
            return calls_;
        }
        static SourceReturnGuard* current() noexcept;

      private:
        [[noreturn]] static void terminate(uint64_t reason) noexcept;
        ReturnBinding binding_{};
        OwnedGroupBoundary postFx_{};
        GuardFacts facts_{};
        NrCallV1 nrWait_{};
        std::array<NrCallV1, 2> inputCalls_{};
        NrWarmSubmissionFacts submissionFacts_{};
        uint64_t submissionBegin_{}, submissionEnd_{}, submissionThread_{};
        uint64_t nrSignalValue_{};
        std::array<NrCallV1, 4> calls_{};
        SourceReturnGuard* previous_{};
        bool entered_{}, armed_{}, finished_{}, transferred_{};
    };
} // namespace virtualdesktop_openxr::nr_capture
