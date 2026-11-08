#pragma once
#define VDXRFG_NR_GROUP_OWNER_V3 1
#include "pair_capture_nr_state.hpp"
#include <unknwn.h>
#include <array>
#include <cstddef>
#include <mutex>
#include <utility>
namespace vdxrfg::pair_capture::nr {
    enum class NrFrame : uint32_t { a, b };
    enum class NrGroupKind : uint32_t { raw11, nr12, postFx11 };
    enum class NrFrameOutcome : uint32_t { complete, aborted };
    enum class NrRefRole : uint32_t {
        source = 1,
        snapshot = 2,
        device = 4,
        queueOrContext = 8,
        list = 16,
        allocator = 32,
        fence = 64,
        auxiliary = 128
    };
    enum class NrReceiverResult : uint32_t { rejected, deepOwnedAcknowledged };
    struct NrBorrowedRef {
        IUnknown* reference{};
        uint64_t privateBytes{};
        NrRefRole role{};
    };
    struct NrGroupIdentity {
        NrIdentity source{};
        uint64_t serial{}, occurrence{}, sourceGeneration{}, descriptorEpoch{}, device{};
        NrSubmissionKey submission{};
        uint64_t signalValue{};
    };
    struct NrGroupId {
        uintptr_t owner{};
        uint64_t attempt{};
        NrFrame frame{};
        uint32_t slot{}, generation{};
        bool valid() const noexcept {
            return owner && attempt && generation;
        }
    };
    inline bool sameGroup(const NrGroupId& a, const NrGroupId& b) noexcept {
        return a.owner == b.owner && a.attempt == b.attempt && a.frame == b.frame && a.slot == b.slot &&
               a.generation == b.generation;
    }
    class NrWorkOwner;
    enum class NrForeignKind : uint32_t { acquire, release };
    template <NrForeignKind Kind>
    class NrForeignToken final {
      public:
        NrForeignToken() noexcept = default;
        NrForeignToken(const NrForeignToken&) = delete;
        NrForeignToken& operator=(const NrForeignToken&) = delete;
        NrForeignToken(NrForeignToken&& other) noexcept {
            *this = std::move(other);
        }
        NrForeignToken& operator=(NrForeignToken&& other) noexcept {
            if (this != &other) {
                owner_ = std::exchange(other.owner_, nullptr);
                group_ = other.group_;
                transaction_ = other.transaction_;
            }
            return *this;
        }
        // No owner access or foreign calls on destruction. A lost/overwritten token
        // leaves its fixed transaction unresolved, charged and non-releasable.
        ~NrForeignToken() = default;
        bool valid() const noexcept {
            return owner_ != nullptr;
        }
        bool execute() noexcept;

      private:
        friend class NrWorkOwner;
        NrForeignToken(NrWorkOwner* owner, NrGroupId group, uint32_t tx) noexcept
            : owner_(owner), group_(group), transaction_(tx) {
        }
        NrWorkOwner* owner_{};
        NrGroupId group_{};
        uint32_t transaction_{};
    };
    using NrAcquireToken = NrForeignToken<NrForeignKind::acquire>;
    using NrReleaseToken = NrForeignToken<NrForeignKind::release>;
    // This is caller-owned fixed scalar storage, not a mutable owner submission
    // borrow. Pool hooks append here without an observer mutex, allocation or COM.
    class NrSubmissionToken final {
      public:
        NrSubmissionToken() noexcept = default;
        NrSubmissionToken(const NrSubmissionToken&) = delete;
        NrSubmissionToken& operator=(const NrSubmissionToken&) = delete;
        NrSubmissionToken(NrSubmissionToken&& other) noexcept {
            *this = std::move(other);
        }
        NrSubmissionToken& operator=(NrSubmissionToken&& other) noexcept;
        bool valid() const noexcept {
            return owner_ != nullptr;
        }
        bool recordClose(const NrSubmissionKey& actual, int32_t result) noexcept;
        bool recordExecute(const NrSubmissionKey& actual) noexcept;
        bool recordSignal(const NrSubmissionKey& actual, uint64_t value, int32_t result) noexcept;
        bool endCall() noexcept;

      private:
        friend class NrWorkOwner;
        enum class Kind : uint32_t { close, execute, signal };
        struct Scalar {
            Kind kind{};
            NrSubmissionKey key{};
            uint64_t value{};
            int32_t result{};
        };
        bool append(Kind, const NrSubmissionKey&, uint64_t, int32_t) noexcept;
        NrSubmissionToken(NrWorkOwner* owner, NrGroupId id) noexcept : owner_(owner), group_(id) {
        }
        NrWorkOwner* owner_{};
        NrGroupId group_{};
        std::array<Scalar, 8> rows_{};
        size_t count_{};
        bool ended_{}, overflow_{};
    };
    // Presence and exact actual work epoch are retained separately from ownership
    // qualification. A zero default result is never an observed successful call.
    struct NrCloseFact {
        bool observed{}, qualified{};
        NrSubmissionKey actual{};
        int32_t result{};
    };
    struct NrSignalFact {
        bool observed{}, qualified{}, recovery{};
        NrSubmissionKey actual{};
        uint64_t value{};
        int32_t result{};
    };
    struct NrCallFacts {
        NrCloseFact close{};
        NrSignalFact original{}, covering{}, highest{}, last{};
        uint32_t executeObservations{};
        NrSubmissionKey lastExecuteActual{};
    };
    struct NrGroupSnapshot {
        bool valid{}, sealed{}, releasing{}, settled{}, uncertain{}, exposed{}, possiblyExposed{}, canRelease{},
            originalSignalSucceeded{}, futureReaderPending{}, pendingSubmission{}, worker{};
        // Known-acquired slots before release; unresolved release charges during
        // release. This is not a live COM refcount census after interruption.
        size_t referenceInventory{}, pendingAcquisitions{};
        uint64_t coveringValue{};
        NrCallFacts calls{};
    };
    // One nonrecursive observer mutex protects all owner metadata. Public methods
    // take it internally. execute() briefly claims its single-use transaction,
    // unlocks, then calls foreign COM. Do not wrap execute in another observer lock.
    // Original borrowers keep strong refs through finishAcquire. The owner stores
    // no Runtime/SDK callback and never invokes COM from any destructor.
    class NrWorkOwner final {
      public:
        static constexpr size_t frameCount = 2, groupsPerFrame = 8, referencesPerFrame = 128;
        static constexpr uint64_t maximumBudget = UINT64_C(4) * 1024 * 1024 * 1024;
        NrWorkOwner() noexcept = default;
        ~NrWorkOwner() = default;
        NrWorkOwner(const NrWorkOwner&) = delete;
        NrWorkOwner& operator=(const NrWorkOwner&) = delete;
        static uint64_t metadataBytes() noexcept;
        static bool foreignCallOnThisThread() noexcept;
        bool reserveAttempt(uint64_t epoch,
                            const NrIdentity&,
                            uint64_t serialA,
                            uint64_t budget,
                            uint64_t bytesA,
                            uint64_t bytesB) noexcept;
        NrGroupId openGroup(NrFrame, NrGroupKind, const NrGroupIdentity&) noexcept;
        NrAcquireToken beginAcquire(NrGroupId, const NrBorrowedRef*, size_t) noexcept;
        bool finishAcquire(NrGroupId, NrAcquireToken&&) noexcept;
        bool sealGroup(NrGroupId) noexcept;
        bool closeFrame(NrFrame, NrFrameOutcome) noexcept;
        bool exposeImmediate(NrGroupId, uint64_t queue) noexcept;
        NrSubmissionToken beginSubmission(NrGroupId) noexcept;
        bool reconcileSubmission(NrGroupId, NrSubmissionToken&&) noexcept;
        bool
        observeSignal(NrGroupId, const NrSubmissionKey& actual, uint64_t value, int32_t result, bool recovery) noexcept;
        bool observeCompleted(NrGroupId, const NrSubmissionKey& actual, uint64_t value) noexcept;
        bool abandonGroup(NrGroupId) noexcept;
        bool beginConsumer(NrGroupId) noexcept;
        bool completeConsumption(NrGroupId) noexcept;
        bool discardReaders(NrGroupId) noexcept;
        uint64_t beginTransfer(NrGroupId) noexcept;
        // This records the caller's actual all-or-none receiver result, not an
        // automatic ownership proof. Integration must ACK only after receiver
        // metadata/refs finish successfully with both observer locks released.
        bool finishTransfer(NrGroupId, uint64_t transfer, NrReceiverResult) noexcept;
        bool workerBegin(NrGroupId) noexcept;
        bool workerEnd(NrGroupId) noexcept;
        NrReleaseToken beginRelease(NrGroupId) noexcept;
        bool finishRelease(NrGroupId, NrReleaseToken&&) noexcept;
        bool reject(NrFailure) noexcept;
        bool detachRuntime() noexcept;
        bool rejected() const noexcept;
        bool runtimeDetached() const noexcept;
        NrGroupSnapshot snapshot(NrGroupId) const noexcept;
        uint64_t reservedBytes() const noexcept;

      private:
        template <NrForeignKind>
        friend class NrForeignToken;
        enum class TxPhase : uint32_t { unused, prepared, executing, executed, finalized, quarantined };
        enum class GroupPhase : uint32_t { open, sealed, releasing, settled };
        enum class Reader : uint32_t { pending, active, closed };
        struct RefSlot {
            IUnknown* pointer{};
            uint64_t bytes{};
            uint32_t group{}, role{};
            bool acquired{};
        };
        struct Acquisition {
            TxPhase phase{};
            uint32_t group{}, first{}, count{}, known{};
        };
        struct Group {
            NrGroupId id{};
            NrGroupIdentity identity{};
            NrGroupKind kind{};
            NrSubmissionState submission{};
            NrCallFacts calls{};
            GroupPhase phase{GroupPhase::open};
            Reader reader{Reader::pending};
            uint32_t roleMask{};
            size_t refs{}, pendingAcquisitions{};
            bool uncertain{}, pendingSubmission{}, submissionUsed{}, possibleExposure{}, worker{}, transferStarted{},
                transferPending{};
            uint64_t transfer{};
            TxPhase releasePhase{};
            std::array<IUnknown*, referencesPerFrame> releaseBatch{};
            size_t releaseCount{}, released{};
        };
        struct Frame {
            std::array<RefSlot, referencesPerFrame> refs{};
            std::array<Acquisition, referencesPerFrame> acquisitions{};
            std::array<Group, groupsPerFrame> groups{};
            size_t refCount{}, acquisitionCount{}, groupCount{};
            uint64_t reservation{}, charged{}, occurrence{};
            bool closed{};
        };
        Frame* frame(NrFrame) noexcept;
        const Frame* frame(NrFrame) const noexcept;
        Group* group(NrGroupId) noexcept;
        const Group* group(NrGroupId) const noexcept;
        bool executeForeign(NrForeignKind, NrGroupId, uint32_t) noexcept;
        void quarantine(NrForeignKind, NrGroupId, uint32_t) noexcept;
        void rejectLocked(NrFailure) noexcept;
        bool releasable(const Group&) const noexcept;
        static void signalFact(Group&, const NrSubmissionKey&, uint64_t, int32_t, bool, bool) noexcept;
        mutable std::mutex mutex_{};
        std::array<Frame, frameCount> frames_{};
        NrIdentity identity_{};
        uint64_t epoch_{}, serialA_{}, reservedBytes_{}, nextTransfer_{};
        NrFailure reason_{NrFailure::none};
        bool configured_{}, rejected_{}, detached_{};
    };
    NrWorkOwner& processNrOwner() noexcept;
    template <NrForeignKind Kind>
    bool NrForeignToken<Kind>::execute() noexcept {
        return owner_ && owner_->executeForeign(Kind, group_, transaction_);
    }
} // namespace vdxrfg::pair_capture::nr
