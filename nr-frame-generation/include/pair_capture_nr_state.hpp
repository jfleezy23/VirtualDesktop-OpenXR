#pragma once
#include <cstdint>
#include <limits>

namespace vdxrfg::pair_capture::nr {
    struct NrIdentity {
        uint64_t nonceLow{}, nonceHigh{};
        uint32_t pid{};
        uint64_t creation{}, sessionLow{}, sessionHigh{};
        uintptr_t fgContext{};
        uint64_t fgGeneration{};
        uintptr_t nrContext{};
        uint64_t nrGeneration{};
    };
    inline bool validIdentity(const NrIdentity& id) noexcept {
        return (id.nonceLow || id.nonceHigh) && id.pid && id.creation && (id.sessionLow || id.sessionHigh) &&
               id.fgContext && id.fgGeneration && id.nrContext && id.nrGeneration;
    }
    inline bool sameIdentity(const NrIdentity& a, const NrIdentity& b) noexcept {
        return a.nonceLow == b.nonceLow && a.nonceHigh == b.nonceHigh && a.pid == b.pid && a.creation == b.creation &&
               a.sessionLow == b.sessionLow && a.sessionHigh == b.sessionHigh && a.fgContext == b.fgContext &&
               a.fgGeneration == b.fgGeneration && a.nrContext == b.nrContext && a.nrGeneration == b.nrGeneration;
    }
    struct NrArm {
        NrIdentity identity{};
        uint64_t minimumSerial{}, minimumTime{};
        bool exactMinimum{};
    };
    struct NrCandidate {
        NrIdentity identity{};
        uint64_t serial{}, time{}, occurrence{}, settingsEpoch{};
        uintptr_t producerContext{};
        uint32_t layerCount{}, eyeMask{};
        bool asyncProducer{};
    };
    struct NrAdmission {
        int32_t result{};
        uint64_t token{}, previous{}, current{}, generation{}, guideEpoch{};
        uintptr_t context{};
    };
    enum class NrPhase { off, armed, a_provisional, a_admitted, b_provisional, pair_admitted, incomplete };
    enum class NrFailure {
        none,
        invalid_arm,
        invalid_source,
        missing_confirmation,
        rejected_admission,
        invalid_ticket,
        gap,
        reset,
        busy,
        transfer_rejected,
        capacity,
        runtime_detached
    };

    class NrSelectionState {
      public:
        bool arm(const NrArm& arm) noexcept {
            if (phase_ != NrPhase::off || !arm.minimumSerial || !arm.minimumTime || !validIdentity(arm.identity))
                return fail(NrFailure::invalid_arm);
            arm_ = arm;
            phase_ = NrPhase::armed;
            return true;
        }
        bool begin(const NrCandidate& source) noexcept {
            if (phase_ == NrPhase::off || phase_ == NrPhase::incomplete || phase_ == NrPhase::pair_admitted)
                return false;
            if (phase_ == NrPhase::armed) {
                if (source.serial < arm_.minimumSerial || source.time < arm_.minimumTime)
                    return false;
                if (arm_.exactMinimum && source.serial != arm_.minimumSerial)
                    return fail(NrFailure::gap);
                if (!validCandidate(source))
                    return fail(NrFailure::invalid_source);
                a_ = source;
                phase_ = NrPhase::a_provisional;
                confirmed_ = false;
                return true;
            }
            if (phase_ != NrPhase::a_admitted || a_.serial == std::numeric_limits<uint64_t>::max() ||
                source.serial != a_.serial + 1 || source.time <= a_.time)
                return fail(NrFailure::gap);
            if (!validCandidate(source) || source.settingsEpoch != a_.settingsEpoch ||
                source.producerContext != a_.producerContext || source.occurrence <= a_.occurrence)
                return fail(NrFailure::invalid_source);
            b_ = source;
            phase_ = NrPhase::b_provisional;
            confirmed_ = false;
            return true;
        }
        bool confirm(uint64_t serial, uint64_t occurrence) noexcept {
            const auto* source = current();
            if (!source || confirmed_ || source->serial != serial || source->occurrence != occurrence)
                return fail(NrFailure::invalid_source);
            confirmed_ = true;
            return true;
        }
        bool admit(const NrAdmission& ticket) noexcept {
            const auto* source = current();
            if (!source || !confirmed_)
                return fail(NrFailure::missing_confirmation);
            if (ticket.result != 0)
                return fail(NrFailure::rejected_admission);
            if (!ticket.token || !ticket.generation || !ticket.guideEpoch || ticket.current != source->serial ||
                ticket.generation != arm_.identity.fgGeneration || ticket.context != arm_.identity.fgContext)
                return fail(NrFailure::invalid_ticket);
            if (phase_ == NrPhase::a_provisional) {
                tokenA_ = ticket.token;
                guideEpoch_ = ticket.guideEpoch;
                phase_ = NrPhase::a_admitted;
                return true;
            }
            if (ticket.previous != a_.serial || ticket.token == tokenA_ || ticket.guideEpoch != guideEpoch_)
                return fail(NrFailure::invalid_ticket);
            tokenB_ = ticket.token;
            phase_ = NrPhase::pair_admitted;
            return true;
        }
        void abort(NrFailure reason) noexcept {
            fail(reason);
        }
        NrPhase phase() const noexcept {
            return phase_;
        }
        NrFailure failure() const noexcept {
            return failure_;
        }
        uint64_t serialA() const noexcept {
            return a_.serial;
        }
        uint64_t serialB() const noexcept {
            return b_.serial;
        }
        uint64_t tokenA() const noexcept {
            return tokenA_;
        }
        uint64_t tokenB() const noexcept {
            return tokenB_;
        }

      private:
        bool validCandidate(const NrCandidate& source) const noexcept {
            return sameIdentity(source.identity, arm_.identity) && source.occurrence && source.settingsEpoch &&
                   source.producerContext && source.layerCount == 1 && source.eyeMask == 3 && source.asyncProducer;
        }
        const NrCandidate* current() const noexcept {
            return phase_ == NrPhase::a_provisional ? &a_ : phase_ == NrPhase::b_provisional ? &b_ : nullptr;
        }
        bool fail(NrFailure reason) noexcept {
            if (failure_ == NrFailure::none)
                failure_ = reason == NrFailure::none ? NrFailure::invalid_source : reason;
            phase_ = NrPhase::incomplete;
            return false;
        }
        NrArm arm_{};
        NrCandidate a_{}, b_{};
        NrPhase phase_{NrPhase::off};
        NrFailure failure_{NrFailure::none};
        uint64_t tokenA_{}, tokenB_{}, guideEpoch_{};
        bool confirmed_{};
    };

    enum class NrSubmissionKind { list12, immediate11 };
    // These are declared logical identities, not cross-API COM pointer equality.
    // Integration must bind them to actual observed calls/imports and list epochs.
    struct NrSubmissionKey {
        uint64_t queue{}, fence{}, list{}, allocator{}, recordingEpoch{};
    };
    inline bool sameSubmission(const NrSubmissionKey& a, const NrSubmissionKey& b) noexcept {
        return a.queue == b.queue && a.fence == b.fence && a.list == b.list && a.allocator == b.allocator &&
               a.recordingEpoch == b.recordingEpoch;
    }

    // Observes original calls. It does not invoke Close/Execute/Signal or drain a
    // queue. Call beforeExecute/exposeImmediate BEFORE exposing GPU references.
    class NrSubmissionState {
      public:
        bool reserve(const NrSubmissionKey& key, NrSubmissionKind kind, uint64_t value) noexcept {
            if (reserved_ || (kind != NrSubmissionKind::list12 && kind != NrSubmissionKind::immediate11) ||
                !key.queue || !key.fence || !value || value == UINT64_MAX ||
                (kind == NrSubmissionKind::list12 && (!key.list || !key.allocator || !key.recordingEpoch)))
                return false;
            key_ = key;
            kind_ = kind;
            expectedValue_ = value;
            reserved_ = true;
            return true;
        }
        bool observeClose(int32_t result) noexcept {
            if (!reserved_ || kind_ != NrSubmissionKind::list12 || closeSeen_ || exposed_ || abandoned_)
                return false;
            closeSeen_ = true;
            closeSucceeded_ = result >= 0;
            return true;
        }
        bool beforeExecute(const NrSubmissionKey& actual) noexcept {
            const bool qualified = reserved_ && kind_ == NrSubmissionKind::list12 && closeSeen_ && closeSucceeded_ &&
                                   !exposed_ && !abandoned_ && sameSubmission(key_, actual);
            exposed_ = true;
            if (!qualified)
                unprovable_ = true;
            return qualified;
        }
        bool exposeImmediate(uint64_t queue) noexcept {
            const bool qualified =
                reserved_ && kind_ == NrSubmissionKind::immediate11 && queue == key_.queue && !exposed_ && !abandoned_;
            exposed_ = true;
            if (!qualified)
                unprovable_ = true;
            return qualified;
        }
        bool observeSignal(uint64_t queue, uint64_t fence, uint64_t value, int32_t result, bool recovery) noexcept {
            if (!reserved_ || !exposed_ || abandoned_ || queue != key_.queue || fence != key_.fence || !value ||
                value == UINT64_MAX ||
                (recovery ? (value <= highestAttempt_ || value <= expectedValue_)
                          : originalSeen_ || highestAttempt_ || value != expectedValue_))
                return false;
            highestAttempt_ = value;
            if (!recovery) {
                originalSeen_ = true;
                originalSucceeded_ = result >= 0;
            }
            if (result >= 0 && !coveringValue_)
                coveringValue_ = value;
            return true;
        }
        bool observeCompleted(uint64_t queue, uint64_t fence, uint64_t value) noexcept {
            if (!reserved_ || !exposed_ || abandoned_ || unprovable_ || queue != key_.queue || fence != key_.fence)
                return false;
            // An already proved boundary stays proved if the device is removed later.
            if (complete_)
                return true;
            if (value == UINT64_MAX) {
                unprovable_ = true;
                return false;
            }
            if (!coveringValue_ || unprovable_)
                return false;
            if (value >= coveringValue_)
                complete_ = true;
            return complete_;
        }
        bool abandonRecording() noexcept {
            if (!reserved_ || exposed_)
                return false;
            abandoned_ = true;
            return true;
        }
        bool canRelease() const noexcept {
            return reserved_ && (abandoned_ || complete_) && !unprovable_;
        }
        bool exposed() const noexcept {
            return exposed_;
        }
        bool originalSignalSucceeded() const noexcept {
            return originalSucceeded_;
        }
        uint64_t coveringValue() const noexcept {
            return coveringValue_;
        }

      private:
        NrSubmissionKey key_{};
        NrSubmissionKind kind_{NrSubmissionKind::list12};
        uint64_t expectedValue_{}, highestAttempt_{}, coveringValue_{};
        bool reserved_{}, closeSeen_{}, closeSucceeded_{}, exposed_{}, abandoned_{}, originalSeen_{},
            originalSucceeded_{}, complete_{}, unprovable_{};
    };
} // namespace vdxrfg::pair_capture::nr
