#include "pair_capture_nr_receiver.hpp"
#include "pair_capture_nr_host_reservation.h"
#include "pair_capture_nr_source_match.h"
#include <cstring>
#include <bcrypt.h>

namespace vdxrfg::pair_capture::nr {
    namespace {
        NrIdentity identity(const NrPacketV1& p) noexcept {
            const auto& x = p.identity;
            return {x.nonce[0],
                    x.nonce[1],
                    static_cast<uint32_t>(x.pid),
                    x.creationFiletime,
                    x.sessionNonce[0],
                    x.sessionNonce[1],
                    static_cast<uintptr_t>(x.fgContextAddress),
                    x.fgHistoryGeneration,
                    static_cast<uintptr_t>(x.nrContextAddress),
                    x.nrContextLifetimeEpoch};
        }
        NrSubmissionKey key(const NrGroupV1& g) noexcept {
            return {g.actualQueueOrContext, g.actualFence, g.actualList, g.actualAllocator, g.recordingEpoch};
        }
        NrSubmissionKey key(const NrCallV1& c) noexcept {
            return {c.queueAddress, c.fenceAddress, c.listAddress, c.allocatorAddress, c.recordingEpoch};
        }
        const NrCallV1* uniqueCall(const NrPacketV1& p, uint64_t kind, uint64_t fence) noexcept {
            const NrCallV1* found = nullptr;
            for (size_t i = 0; i < p.header.callCount; ++i) {
                const auto& call = p.calls[i];
                if (call.kind == kind && call.fenceAddress == fence) {
                    if (found)
                        return nullptr;
                    found = &call;
                }
            }
            return found;
        }
        bool observedSubmission(const NrPacketV1& p) noexcept {
            for (const auto kind : {NR_CLOSE, NR_EXECUTE, NR_SIGNAL})
                if (!uniqueCall(p, kind, p.groups[1].actualFence))
                    return false;
            return uniqueCall(p, NR_SIGNAL, p.groups[0].actualFence) != nullptr;
        }
        bool sourceMatches(const NrPacketV1& p, const NrSourceWitnessV1& source) noexcept {
            return nr_capture_source_matches(&p, &source) != 0;
        }
        bool controlShape(const NrPacketV1& p) noexcept {
            if (p.header.magic != NR_MAGIC || p.header.size != sizeof p || p.header.version != NR_ABI_VERSION ||
                (p.header.operation != NR_QUERY && p.header.operation != NR_ABORT))
                return false;
            NrPacketV1 remaining = p;
            remaining.header.magic = remaining.header.size = remaining.header.version = remaining.header.operation =
                remaining.header.attempt = 0;
            auto& x = remaining.identity;
            x.nonce[0] = x.nonce[1] = x.pid = x.creationFiletime = x.sessionNonce[0] = x.sessionNonce[1] = 0;
            x.fgContextAddress = x.fgContextLifetimeEpoch = x.fgHistoryGeneration = 0;
            const auto* bytes = reinterpret_cast<const unsigned char*>(&remaining);
            for (size_t i = 0; i < sizeof remaining; ++i)
                if (bytes[i])
                    return false;
            return true;
        }
    } // namespace
    bool packetDigest(const NrPacketV1& packet, std::array<uint8_t, 32>& digest) noexcept {
        NrPacketV1 sanitized = packet;
        for (auto& ref : sanitized.refs)
            ref.borrowedIUnknownAddress = 0;
        return BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                          nullptr,
                          0,
                          reinterpret_cast<PUCHAR>(&sanitized),
                          static_cast<ULONG>(sizeof sanitized),
                          digest.data(),
                          static_cast<ULONG>(digest.size())) >= 0;
    }
    bool transferDigest(const NrPacketV1& packet,
                        const NrPostFxSidecarV1& facts,
                        std::array<uint8_t, 32>& digest) noexcept {
        struct CanonicalDigestInput {
            NrPacketV1 packet;
            NrPostFxSidecarV1 facts;
        } input{packet, facts};
        static_assert(sizeof(input) == sizeof(packet) + sizeof(facts));
        for (auto& ref : input.packet.refs)
            ref.borrowedIUnknownAddress = 0;
        return BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                          nullptr,
                          0,
                          reinterpret_cast<PUCHAR>(&input),
                          static_cast<ULONG>(sizeof input),
                          digest.data(),
                          static_cast<ULONG>(digest.size())) >= 0;
    }
    bool Receiver::bind(const RequestBinding& binding) noexcept {
        if (!binding.attempt || binding.attempt == UINT64_MAX ||
            (binding.mode != NR_NATIVE100 && binding.mode != NR_SPATIAL85) || !(binding.nonce[0] | binding.nonce[1]) ||
            !binding.pid || !binding.creation || !(binding.session[0] | binding.session[1]) || !binding.context ||
            !binding.lifetime || !binding.history || !binding.minimumSerial || binding.minimumSerial == UINT64_MAX ||
            !binding.minimumTime || binding.budget != 134217728)
            return false;
        std::lock_guard lock(mutex_);
        if (bound_ || terminal_ || frames_[0].phase != Phase::empty || frames_[1].phase != Phase::empty)
            return false;
        binding_ = binding;
        bound_ = true;
        return true;
    }
    bool Receiver::attachSource(const NrPacketV1& p, const NrSourceWitnessV1& source) noexcept {
        NrError error{};
        if (!nr_validate_packet(&p, sizeof p, &error) || p.header.operation != NR_FRAME_TRANSFER ||
            !nr_validate_source(&source, sizeof source, &error))
            return false;
        if (!sourceMatches(p, source))
            return false;
        const auto& x = p.identity;
        std::lock_guard lock(mutex_);
        if (!bound_ || terminal_ || p.header.attempt != binding_.attempt || p.header.actualNrMode != binding_.mode ||
            x.nonce[0] != binding_.nonce[0] || x.nonce[1] != binding_.nonce[1] || x.pid != binding_.pid ||
            x.creationFiletime != binding_.creation || x.sessionNonce[0] != binding_.session[0] ||
            x.sessionNonce[1] != binding_.session[1] || x.fgContextAddress != binding_.context ||
            x.fgContextLifetimeEpoch != binding_.lifetime || x.fgHistoryGeneration != binding_.history)
            return false;
        auto& frame = frames_[static_cast<size_t>(p.header.frameOrdinal)];
        if (frame.sourceTransfer)
            return frame.sourceTransfer == p.header.transferId && !std::memcmp(&frame.source, &source, sizeof source);
        if (frame.phase != Phase::empty)
            return false;
        frame.source = source;
        frame.sourceTransfer = p.header.transferId;
        return true;
    }
    const NrPacketV1* Receiver::inspect(unsigned n) const noexcept {
        std::lock_guard lock(mutex_);
        return n < frames_.size() && !terminal_ && frames_[n].phase == Phase::committed ? &frames_[n].packet : nullptr;
    }
    const NrSourceWitnessV1* Receiver::source(unsigned n) const noexcept {
        std::lock_guard lock(mutex_);
        return n < frames_.size() && frames_[n].sourceTransfer ? &frames_[n].source : nullptr;
    }
    const NrPostFxSidecarV1* Receiver::postFx(unsigned n) const noexcept {
        std::lock_guard lock(mutex_);
        return n < frames_.size() && frames_[n].ack.status == NR_DEEP_OWNED ? &frames_[n].postFx : nullptr;
    }
    NrAckV1 Receiver::acknowledgement(unsigned n) const noexcept {
        std::lock_guard lock(mutex_);
        return n < frames_.size() ? frames_[n].ack : NrAckV1{};
    }
    void Receiver::fail() noexcept {
        std::lock_guard lock(mutex_);
        terminal_ = true;
    }
    bool Receiver::artifactPublished() noexcept {
        std::lock_guard lock(mutex_);
        if (terminal_)
            return false;
        for (const auto& frame : frames_)
            if (frame.phase != Phase::settled || !frame.workerEnded || !frame.readersClosed || frame.scratch)
                return false;
        artifactPublished_ = true;
        return true;
    }
    bool Receiver::acquire(Frame& frame, const NrPacketV1& p) noexcept {
        const auto ordinal = static_cast<NrFrame>(p.header.frameOrdinal);
        if (p.header.frameOrdinal == 0 && !owner_.reserveAttempt(p.header.attempt,
                                                                 identity(p),
                                                                 p.identity.hostSerial,
                                                                 p.budget.capBytes,
                                                                 p.budget.bytesA,
                                                                 p.budget.bytesB))
            return false;
        for (size_t g = 0; g < 3; ++g) {
            const auto& row = p.groups[g];
            NrGroupIdentity id{identity(p),
                               p.identity.hostSerial,
                               row.sourceOccurrence,
                               row.sourceGeneration,
                               row.descriptorEpoch,
                               row.deviceAddress,
                               key(row),
                               row.expectedSignalValue};
            const auto group = owner_.openGroup(ordinal, static_cast<NrGroupKind>(g), id);
            if (!group.valid())
                return false;
            frame.groups[g] = group;
            std::array<NrBorrowedRef, 128> refs{};
            size_t count = 0;
            for (size_t i = 0; i < p.header.refCount; ++i) {
                const auto& ref = p.refs[i];
                if (ref.groupIndex != g)
                    continue;
                // This is the already typed interface's IUnknown prefix. Only
                // AddRef/Release is used here, never QI or a graphics method.
                auto* value = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(ref.borrowedIUnknownAddress));
                refs[count++] = {value, ref.chargedPrivateBytes, static_cast<NrRefRole>(ref.roleMask)};
                frame.held[i] = value;
            }
            auto token = owner_.beginAcquire(group, refs.data(), count);
            if (!token.valid() || !token.execute() || !owner_.finishAcquire(group, std::move(token)))
                return false;
            if (!owner_.sealGroup(group))
                return false;
            // Replay actual observed facts from the immutable packet. No original
            // API is called and no prospective key is substituted for an observation.
            if (g == 1) {
                const auto& close = *uniqueCall(p, NR_CLOSE, row.actualFence);
                const auto& execute = *uniqueCall(p, NR_EXECUTE, row.actualFence);
                const auto& signal = *uniqueCall(p, NR_SIGNAL, row.actualFence);
                auto submission = owner_.beginSubmission(group);
                if (!submission.valid() ||
                    !submission.recordClose(key(close), static_cast<int32_t>(close.resultBits)) ||
                    !submission.recordExecute(key(execute)) ||
                    !submission.recordSignal(
                        key(signal), signal.signalValue, static_cast<int32_t>(signal.resultBits)) ||
                    !submission.endCall() || !owner_.reconcileSubmission(group, std::move(submission)))
                    return false;
            } else {
                const auto& signal = *uniqueCall(p, NR_SIGNAL, row.actualFence);
                if (!owner_.exposeImmediate(group, signal.queueAddress) ||
                    !owner_.observeSignal(
                        group, key(signal), signal.signalValue, static_cast<int32_t>(signal.resultBits), false))
                    return false;
            }
        }
        return owner_.closeFrame(ordinal, NrFrameOutcome::complete);
    }
    uint32_t Receiver::transact(const NrPacketV1* borrowed,
                                NrAckV1* ack,
                                const NrPostFxSidecarV1* borrowedFacts) noexcept {
        if (!ack)
            return NR_REJECTED;
        *ack = {};
        ack->size = sizeof(*ack);
        ack->version = NR_ABI_VERSION;
        ack->status = NR_REJECTED;
        if (!borrowed)
            return NR_REJECTED;
        // The caller contract keeps all packet bytes immutable for this call.
        const NrPacketV1 packet = *borrowed;
        const NrPostFxSidecarV1 facts = borrowedFacts ? *borrowedFacts : NrPostFxSidecarV1{};
        NrError error{};
        if (borrowedFacts && packet.header.operation != NR_FRAME_TRANSFER)
            return NR_REJECTED;
        if (!nr_validate_packet(&packet, sizeof packet, &error)) {
            ack->reasonEnum = error.code;
            return NR_REJECTED;
        }
        if (packet.header.operation == NR_QUERY || packet.header.operation == NR_ABORT) {
            // The control ACK has no transfer proof, even when its status is the
            // same numeric value as DEEP_OWNED. Unknown cohorts cannot mutate state.
            if (!controlShape(packet))
                return NR_REJECTED;
            ack->requestAttempt = packet.header.attempt;
            ack->observedFlags = 1;
            std::lock_guard lock(mutex_);
            const auto& x = packet.identity;
            if (!bound_ || packet.header.attempt != binding_.attempt || x.nonce[0] != binding_.nonce[0] ||
                x.nonce[1] != binding_.nonce[1] || x.pid != binding_.pid || x.creationFiletime != binding_.creation ||
                x.sessionNonce[0] != binding_.session[0] || x.sessionNonce[1] != binding_.session[1] ||
                x.fgContextAddress != binding_.context || x.fgContextLifetimeEpoch != binding_.lifetime ||
                x.fgHistoryGeneration != binding_.history)
                return NR_REJECTED;
            if (packet.header.operation == NR_ABORT)
                terminal_ = true;
            ack->requestAttempt = binding_.attempt;
            ack->observedFlags = 1;
            // Only the actual complete FG artifact publication path sets this
            // flag, after its native/retire gates and both custody releases.
            ack->status = terminal_ ? NR_UNCERTAIN : artifactPublished_ ? NR_SETTLED : NR_DEEP_OWNED;
            return static_cast<uint32_t>(ack->status);
        }
        if (packet.header.operation == NR_NEGOTIATE) {
            {
                std::lock_guard lock(mutex_);
                negotiated_ = true;
            }
            ack->status = NR_NEGOTIATED;
            ack->roleCount = 64;
            ack->refCount = 128;
            ack->groupCount = 8;
            ack->acceptedBudgetBytes = 134217728;
            ack->observedFlags = 1;
            return static_cast<uint32_t>(ack->status);
        }
        if (packet.header.operation != NR_FRAME_TRANSFER || !nr_capture_host_reference_reservation(&packet))
            return NR_REJECTED;
        if (!borrowedFacts || !nr_validate_postfx(&packet, &facts, sizeof facts))
            return NR_REJECTED;
        if (!observedSubmission(packet)) {
            ack->reasonEnum = NR_BAD_CALL;
            return NR_REJECTED;
        }
        if (transaction_.test_and_set(std::memory_order_acquire))
            return NR_REJECTED;
        struct Unlock {
            std::atomic_flag& flag;
            ~Unlock() {
                flag.clear(std::memory_order_release);
            }
        } unlock{transaction_};
        std::array<uint8_t, 32> digest{};
        if (!transferDigest(packet, facts, digest))
            return NR_REJECTED;
        const auto n = static_cast<size_t>(packet.header.frameOrdinal);
        auto& frame = frames_[n];
        uint64_t epoch{};
        {
            std::lock_guard lock(mutex_);
            if (!negotiated_ || !bound_ || terminal_ || frame.sourceTransfer != packet.header.transferId ||
                !sourceMatches(packet, frame.source))
                return NR_REJECTED;
            const auto& x = packet.identity;
            if (packet.header.attempt != binding_.attempt || packet.header.actualNrMode != binding_.mode ||
                x.nonce[0] != binding_.nonce[0] || x.nonce[1] != binding_.nonce[1] || x.pid != binding_.pid ||
                x.creationFiletime != binding_.creation || x.sessionNonce[0] != binding_.session[0] ||
                x.sessionNonce[1] != binding_.session[1] || x.fgContextAddress != binding_.context ||
                x.fgContextLifetimeEpoch != binding_.lifetime || x.fgHistoryGeneration != binding_.history ||
                x.hostSerial < binding_.minimumSerial || x.displayTimeBits < binding_.minimumTime ||
                packet.budget.capBytes != binding_.budget)
                return NR_REJECTED;
            // ABI transfer IDs are unique within the bound attempt, not per ordinal.
            // The other frame necessarily has different canonical bytes. Reject the
            // collision before opening any owner group or calling foreign AddRef.
            for (size_t i = 0; i < frames_.size(); ++i)
                if (i != n && frames_[i].phase != Phase::empty &&
                    frames_[i].packet.header.transferId == packet.header.transferId)
                    return NR_REJECTED;
            if (frame.phase == Phase::committed) {
                // Full canonical bytes include original pointer words. Digest alone
                // intentionally omits those words and cannot authorize replay.
                if (std::memcmp(&frame.packet, &packet, sizeof packet) ||
                    std::memcmp(&frame.postFx, &facts, sizeof facts) || frame.digest != digest)
                    return NR_REJECTED;
                *ack = frame.ack;
                return static_cast<uint32_t>(ack->status);
            }
            if (frame.phase != Phase::empty || (n == 1 && frames_[0].phase != Phase::committed))
                return NR_REJECTED;
            if (n == 1 && (!sameIdentity(identity(frames_[0].packet), identity(packet)) ||
                           packet.header.attempt != frames_[0].packet.header.attempt ||
                           packet.identity.hostSerial != frames_[0].packet.identity.hostSerial + 1 ||
                           std::memcmp(&packet.budget, &frames_[0].packet.budget, sizeof packet.budget)))
                return NR_REJECTED;
            frame.phase = Phase::receiving;
            epoch = frame.epoch = ++nextEpoch_;
        }
        // No receiver metadata lock overlaps the owner's metadata lock or foreign
        // COM acquisition. A partial acquisition is retained and never published.
        const bool acquired = acquire(frame, packet);
        {
            std::lock_guard lock(mutex_);
            if (!acquired || terminal_ || frame.phase != Phase::receiving || frame.epoch != epoch) {
                terminal_ = true;
                frame.phase = Phase::uncertain;
                ack->status = NR_UNCERTAIN;
                ack->reasonEnum = NR_BAD_REF;
                return NR_UNCERTAIN;
            }
            frame.packet = packet;
            frame.postFx = facts;
            frame.digest = digest;
            ack->status = NR_DEEP_OWNED;
            ack->requestAttempt = packet.header.attempt;
            ack->frameOrdinal = packet.header.frameOrdinal;
            ack->transferId = packet.header.transferId;
            ack->receiverCommitEpoch = epoch;
            ack->roleCount = packet.header.roleCount;
            ack->refCount = packet.header.refCount;
            ack->groupCount = packet.header.groupCount;
            std::memcpy(&ack->packetDigestLo, digest.data(), 16);
            ack->acceptedBudgetBytes = packet.budget.physicalPrivatePeakBytes;
            ack->observedFlags = 1;
            frame.ack = *ack;
            frame.phase = Phase::committed;
        }
        return static_cast<uint32_t>(ack->status);
    }
    bool Receiver::committed(unsigned n) const noexcept {
        std::lock_guard lock(mutex_);
        return n < frames_.size() && frames_[n].phase == Phase::committed;
    }
    const NrPacketV1* Receiver::beginRead(unsigned n) noexcept {
        {
            std::lock_guard lock(mutex_);
            if (n >= frames_.size() || terminal_ || frames_[n].phase != Phase::committed || frames_[n].workerStarted)
                return nullptr;
            frames_[n].workerStarted = true;
        }
        auto& frame = frames_[n];
        for (const auto id : frame.groups) {
            if (!owner_.beginConsumer(id) || !owner_.workerBegin(id)) {
                std::lock_guard lock(mutex_);
                terminal_ = true;
                frame.phase = Phase::uncertain;
                return nullptr;
            }
        }
        return &frame.packet;
    }
    void* Receiver::borrow(unsigned n, uint64_t i, uint64_t type) const noexcept {
        std::lock_guard lock(mutex_);
        if (n >= frames_.size())
            return nullptr;
        const auto& frame = frames_[n];
        if (frame.phase != Phase::committed || !frame.workerStarted || frame.workerEnded || frame.readersClosed ||
            i >= frame.packet.header.refCount || frame.packet.refs[i].interfaceEnum != type)
            return nullptr;
        return frame.held[static_cast<size_t>(i)];
    }
    bool Receiver::completed(unsigned n, unsigned g, const NrSubmissionKey& actual, uint64_t value) noexcept {
        NrGroupId id{};
        {
            std::lock_guard lock(mutex_);
            if (n >= frames_.size() || g >= 3 || frames_[n].phase != Phase::committed || !frames_[n].workerStarted ||
                frames_[n].workerEnded)
                return false;
            id = frames_[n].groups[g];
        }
        if (!owner_.observeCompleted(id, actual, value))
            return false;
        {
            std::lock_guard lock(mutex_);
            frames_[n].completion[g] = value;
        }
        return true;
    }
    bool Receiver::closeReaders(unsigned n) noexcept {
        {
            std::lock_guard lock(mutex_);
            if (n >= frames_.size() || frames_[n].phase != Phase::committed || !frames_[n].workerStarted ||
                frames_[n].readersClosed)
                return false;
            frames_[n].readersClosed = true;
        }
        for (const auto id : frames_[n].groups)
            if (!owner_.completeConsumption(id)) {
                std::lock_guard lock(mutex_);
                terminal_ = true;
                frames_[n].phase = Phase::uncertain;
                return false;
            }
        return true;
    }
    bool Receiver::endWorker(unsigned n) noexcept {
        {
            std::lock_guard lock(mutex_);
            if (n >= frames_.size() || frames_[n].phase != Phase::committed || !frames_[n].workerStarted ||
                frames_[n].workerEnded || (frames_[n].scratch & 1))
                return false;
            frames_[n].workerEnded = true;
        }
        for (const auto id : frames_[n].groups)
            if (!owner_.workerEnd(id)) {
                std::lock_guard lock(mutex_);
                terminal_ = true;
                frames_[n].phase = Phase::uncertain;
                return false;
            }
        return true;
    }
    bool Receiver::release(unsigned n) noexcept {
        {
            std::lock_guard lock(mutex_);
            if (n >= frames_.size() || terminal_ || frames_[n].phase != Phase::committed || !frames_[n].workerEnded ||
                !frames_[n].readersClosed || frames_[n].scratch)
                return false;
            for (auto value : frames_[n].completion)
                if (!value)
                    return false;
            frames_[n].phase = Phase::releasing;
        }
        for (const auto id : frames_[n].groups) {
            auto token = owner_.beginRelease(id);
            if (!token.valid() || !token.execute() || !owner_.finishRelease(id, std::move(token))) {
                std::lock_guard lock(mutex_);
                terminal_ = true;
                frames_[n].phase = Phase::uncertain;
                return false;
            }
        }
        {
            std::lock_guard lock(mutex_);
            frames_[n].held = {};
            frames_[n].phase = Phase::settled;
        }
        return true;
    }
    bool Receiver::claimScratch(unsigned n, unsigned kind) noexcept {
        std::lock_guard lock(mutex_);
        if (n >= frames_.size() || (kind != 1 && kind != 2) || frames_[n].phase != Phase::committed ||
            (frames_[n].scratch & kind) ||
            (kind == 1 && (!frames_[n].workerStarted || frames_[n].workerEnded || frames_[n].readersClosed)))
            return false;
        // All three scratch leases are prepaid in the shared B reservation even
        // while mapping A. They are never persistent packet reference rows.
        const auto& budget = frames_[n].packet.budget;
        const auto maximum = budget.senderRefCountB + 35 + 3;
        if (maximum > budget.receiverRefCountB || maximum > 128)
            return false;
        frames_[n].scratch |= kind;
        return true;
    }
    bool Receiver::returnScratch(unsigned n, unsigned kind) noexcept {
        std::lock_guard lock(mutex_);
        if (n >= frames_.size() || (kind != 1 && kind != 2) || !(frames_[n].scratch & kind))
            return false;
        frames_[n].scratch &= ~kind;
        return true;
    }
} // namespace vdxrfg::pair_capture::nr
