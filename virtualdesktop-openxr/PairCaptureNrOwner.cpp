#include "PairCaptureNrOwner.h"
namespace vdxrfg::pair_capture::nr {
    namespace {
        thread_local bool foreign = false;
        struct ForeignScope {
            bool prior{foreign};
            ForeignScope() noexcept {
                foreign = true;
            }
            ~ForeignScope() {
                foreign = prior;
            }
        };
        constexpr uint32_t rawRoles = 1 | 2 | 4 | 8 | 64, listRoles = rawRoles | 16 | 32;
        bool validRole(uint32_t role) noexcept {
            return role && role <= 128 && (role & (role - 1)) == 0;
        }
    } // namespace
    NrSubmissionToken& NrSubmissionToken::operator=(NrSubmissionToken&& other) noexcept {
        if (this != &other) {
            owner_ = std::exchange(other.owner_, nullptr);
            group_ = other.group_;
            rows_ = other.rows_;
            count_ = other.count_;
            ended_ = other.ended_;
            overflow_ = other.overflow_;
        }
        return *this;
    }
    bool NrSubmissionToken::append(Kind kind, const NrSubmissionKey& actual, uint64_t value, int32_t result) noexcept {
        if (!owner_ || ended_ || count_ == rows_.size()) {
            overflow_ = true;
            return false;
        }
        rows_[count_++] = {kind, actual, value, result};
        return true;
    }
    bool NrSubmissionToken::recordClose(const NrSubmissionKey& key, int32_t result) noexcept {
        return append(Kind::close, key, 0, result);
    }
    bool NrSubmissionToken::recordExecute(const NrSubmissionKey& key) noexcept {
        return append(Kind::execute, key, 0, 0);
    }
    bool NrSubmissionToken::recordSignal(const NrSubmissionKey& key, uint64_t value, int32_t result) noexcept {
        return append(Kind::signal, key, value, result);
    }
    bool NrSubmissionToken::endCall() noexcept {
        if (!owner_ || ended_) {
            overflow_ = true;
            return false;
        }
        ended_ = true;
        return true;
    }
    uint64_t NrWorkOwner::metadataBytes() noexcept {
        return sizeof(NrWorkOwner) + frameCount * groupsPerFrame *
                                         (sizeof(NrSubmissionToken) + sizeof(NrAcquireToken) + sizeof(NrReleaseToken));
    }
    bool NrWorkOwner::foreignCallOnThisThread() noexcept {
        return foreign;
    }
    void NrWorkOwner::rejectLocked(NrFailure why) noexcept {
        rejected_ = true;
        if (reason_ == NrFailure::none)
            reason_ = why == NrFailure::none ? NrFailure::invalid_source : why;
        for (auto& f : frames_)
            f.closed = true;
    }
    bool NrWorkOwner::reserveAttempt(uint64_t epoch,
                                     const NrIdentity& identity,
                                     uint64_t serialA,
                                     uint64_t budget,
                                     uint64_t bytesA,
                                     uint64_t bytesB) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto metadata = metadataBytes();
        if (foreign || configured_ || rejected_ || !epoch || !validIdentity(identity) || !serialA ||
            serialA == UINT64_MAX || !bytesA || !bytesB || budget > maximumBudget || budget < metadata ||
            bytesA > budget - metadata || bytesB > budget - metadata - bytesA) {
            if (!foreign)
                rejectLocked(NrFailure::capacity);
            return false;
        }
        frames_[0].reservation = bytesA;
        frames_[1].reservation = bytesB;
        epoch_ = epoch;
        serialA_ = serialA;
        identity_ = identity;
        reservedBytes_ = metadata + bytesA + bytesB;
        configured_ = true;
        return true;
    }
    NrWorkOwner::Frame* NrWorkOwner::frame(NrFrame id) noexcept {
        const auto n = static_cast<size_t>(id);
        return configured_ && n < frames_.size() ? &frames_[n] : nullptr;
    }
    const NrWorkOwner::Frame* NrWorkOwner::frame(NrFrame id) const noexcept {
        const auto n = static_cast<size_t>(id);
        return configured_ && n < frames_.size() ? &frames_[n] : nullptr;
    }
    NrWorkOwner::Group* NrWorkOwner::group(NrGroupId id) noexcept {
        auto* f = frame(id.frame);
        return f && id.owner == reinterpret_cast<uintptr_t>(this) && id.attempt == epoch_ && id.slot < f->groupCount &&
                       sameGroup(f->groups[id.slot].id, id)
                   ? &f->groups[id.slot]
                   : nullptr;
    }
    const NrWorkOwner::Group* NrWorkOwner::group(NrGroupId id) const noexcept {
        const auto* f = frame(id.frame);
        return f && id.owner == reinterpret_cast<uintptr_t>(this) && id.attempt == epoch_ && id.slot < f->groupCount &&
                       sameGroup(f->groups[id.slot].id, id)
                   ? &f->groups[id.slot]
                   : nullptr;
    }
    NrGroupId NrWorkOwner::openGroup(NrFrame id, NrGroupKind kind, const NrGroupIdentity& actual) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* f = frame(id);
        if (foreign || !f || rejected_ || detached_ || f->closed)
            return {};
        if (f->groupCount == groupsPerFrame || !sameIdentity(identity_, actual.source) ||
            actual.serial != serialA_ + static_cast<uint64_t>(id) || !actual.occurrence || !actual.sourceGeneration ||
            !actual.descriptorEpoch || !actual.device || (f->occurrence && f->occurrence != actual.occurrence) ||
            (kind != NrGroupKind::raw11 && kind != NrGroupKind::nr12 && kind != NrGroupKind::postFx11)) {
            rejectLocked(NrFailure::capacity);
            return {};
        }
        auto& g = f->groups[f->groupCount];
        if (!g.submission.reserve(actual.submission,
                                  kind == NrGroupKind::nr12 ? NrSubmissionKind::list12 : NrSubmissionKind::immediate11,
                                  actual.signalValue)) {
            rejectLocked(NrFailure::invalid_source);
            return {};
        }
        g.id = {reinterpret_cast<uintptr_t>(this),
                epoch_,
                id,
                static_cast<uint32_t>(f->groupCount),
                static_cast<uint32_t>(f->groupCount + 1)};
        g.identity = actual;
        g.kind = kind;
        f->occurrence = actual.occurrence;
        ++f->groupCount;
        return g.id;
    }
    NrAcquireToken NrWorkOwner::beginAcquire(NrGroupId id, const NrBorrowedRef* refs, size_t count) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        auto* f = frame(id.frame);
        if (foreign || !g || !f || rejected_ || detached_ || f->closed || g->phase != GroupPhase::open ||
            g->uncertain || g->pendingAcquisitions)
            return {};
        if (!refs || !count || count > referencesPerFrame - f->refCount || f->acquisitionCount == referencesPerFrame) {
            rejectLocked(NrFailure::capacity);
            return {};
        }
        uint64_t bytes = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!refs[i].reference || !validRole(static_cast<uint32_t>(refs[i].role)) ||
                refs[i].privateBytes > f->reservation - f->charged - bytes) {
                rejectLocked(NrFailure::capacity);
                return {};
            }
            bytes += refs[i].privateBytes;
        }
        const auto tx = static_cast<uint32_t>(f->acquisitionCount++);
        auto& a = f->acquisitions[tx];
        a = {TxPhase::prepared, id.slot, static_cast<uint32_t>(f->refCount), static_cast<uint32_t>(count), 0};
        for (size_t i = 0; i < count; ++i)
            f->refs[f->refCount++] = {
                refs[i].reference, refs[i].privateBytes, id.slot, static_cast<uint32_t>(refs[i].role), false};
        f->charged += bytes;
        g->refs += count;
        ++g->pendingAcquisitions;
        return {this, id, tx};
    }
    void NrWorkOwner::quarantine(NrForeignKind kind, NrGroupId id, uint32_t transaction) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        auto* f = frame(id.frame);
        if (!g || !f)
            return;
        g->uncertain = true;
        if (kind == NrForeignKind::release)
            g->releasePhase = TxPhase::quarantined;
        else if (transaction < f->acquisitionCount)
            f->acquisitions[transaction].phase = TxPhase::quarantined;
    }
    bool NrWorkOwner::executeForeign(NrForeignKind kind, NrGroupId id, uint32_t transaction) noexcept {
        Frame* f = nullptr;
        Group* g = nullptr;
        Acquisition* acquire = nullptr;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            g = group(id);
            f = frame(id.frame);
            if (!g || !f)
                return false;
            if (kind == NrForeignKind::acquire) {
                if (transaction >= f->acquisitionCount)
                    return false;
                acquire = &f->acquisitions[transaction];
                if (acquire->group != id.slot)
                    return false;
                if (foreign || acquire->phase != TxPhase::prepared) {
                    g->uncertain = true;
                    acquire->phase = TxPhase::quarantined;
                    return false;
                }
                acquire->phase = TxPhase::executing;
                count = acquire->count;
            } else {
                if (foreign || transaction != id.generation || g->phase != GroupPhase::releasing ||
                    g->releasePhase != TxPhase::prepared) {
                    g->uncertain = true;
                    g->releasePhase = TxPhase::quarantined;
                    return false;
                }
                g->releasePhase = TxPhase::executing;
                count = g->releaseCount;
            }
        }
        size_t known = 0;
        bool returned = true;
        try {
            ForeignScope scope;
            for (; known < count; ++known) {
                if (kind == NrForeignKind::acquire)
                    f->refs[acquire->first + known].pointer->AddRef();
                else {
                    auto* reference = std::exchange(g->releaseBatch[known], nullptr);
                    reference->Release();
                }
            }
        } catch (...) {
            returned = false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (kind == NrForeignKind::acquire) {
            acquire->known = static_cast<uint32_t>(known);
            for (size_t i = 0; i < known; ++i)
                f->refs[acquire->first + i].acquired = true;
            if (returned && acquire->phase == TxPhase::executing) {
                acquire->phase = TxPhase::executed;
                return true;
            }
            acquire->phase = TxPhase::quarantined;
        } else {
            g->released = known;
            if (returned && g->releasePhase == TxPhase::executing) {
                g->releasePhase = TxPhase::executed;
                return true;
            }
            g->releasePhase = TxPhase::quarantined;
        }
        g->uncertain = true;
        return false;
    }
    bool NrWorkOwner::finishAcquire(NrGroupId expected, NrAcquireToken&& token) noexcept {
        if (token.owner_ != this) {
            if (token.owner_)
                token.owner_->quarantine(NrForeignKind::acquire, token.group_, token.transaction_);
            token.owner_ = nullptr;
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto id = token.group_;
        const auto tx = token.transaction_;
        token.owner_ = nullptr;
        auto* f = frame(id.frame);
        auto* g = group(id);
        if (!g || !f || tx >= f->acquisitionCount)
            return false;
        auto& a = f->acquisitions[tx];
        if (foreign || !sameGroup(expected, id) || a.group != id.slot || a.phase != TxPhase::executed ||
            a.known != a.count) {
            g->uncertain = true;
            a.phase = TxPhase::quarantined;
            return false;
        }
        for (size_t i = 0; i < a.count; ++i)
            g->roleMask |= f->refs[a.first + i].role;
        a.phase = TxPhase::finalized;
        --g->pendingAcquisitions;
        return true;
    }
    bool NrWorkOwner::sealGroup(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || rejected_ || detached_ || g->phase != GroupPhase::open || g->uncertain ||
            g->pendingAcquisitions || !g->refs)
            return false;
        const auto needed = g->kind == NrGroupKind::nr12 ? listRoles : rawRoles;
        if ((g->roleMask & needed) != needed)
            return false;
        g->phase = GroupPhase::sealed;
        return true;
    }
    bool NrWorkOwner::closeFrame(NrFrame id, NrFrameOutcome outcome) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* f = frame(id);
        if (foreign || !f || f->closed || (outcome != NrFrameOutcome::complete && outcome != NrFrameOutcome::aborted))
            return false;
        f->closed = true;
        if (outcome == NrFrameOutcome::aborted)
            rejectLocked(NrFailure::invalid_source);
        return true;
    }
    bool NrWorkOwner::exposeImmediate(NrGroupId id, uint64_t queue) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        auto* f = frame(id.frame);
        if (foreign || !g || !f || rejected_ || detached_ || f->closed || g->phase != GroupPhase::sealed ||
            g->uncertain || g->pendingAcquisitions || g->pendingSubmission)
            return false;
        return g->submission.exposeImmediate(queue);
    }
    NrSubmissionToken NrWorkOwner::beginSubmission(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        auto* f = frame(id.frame);
        if (foreign || !g || !f || g->phase != GroupPhase::sealed || g->kind != NrGroupKind::nr12 || g->uncertain ||
            g->pendingAcquisitions || g->submissionUsed)
            return {};
        g->submissionUsed = true;
        g->pendingSubmission = true;
        g->possibleExposure = true;
        return {this, id};
    }
    void NrWorkOwner::signalFact(Group& g,
                                 const NrSubmissionKey& actual,
                                 uint64_t value,
                                 int32_t result,
                                 bool recovery,
                                 bool qualified) noexcept {
        const NrSignalFact fact{true, qualified, recovery, actual, value, result};
        g.calls.last = fact;
        if (!recovery && !g.calls.original.observed)
            g.calls.original = fact;
        if (sameSubmission(actual, g.identity.submission) && value && value != UINT64_MAX &&
            (!g.calls.highest.observed || value >= g.calls.highest.value))
            g.calls.highest = fact;
        if (qualified && result >= 0 && !g.calls.covering.observed && g.submission.coveringValue() == value)
            g.calls.covering = fact;
    }
    bool NrWorkOwner::reconcileSubmission(NrGroupId expected, NrSubmissionToken&& token) noexcept {
        if (token.owner_ != this) {
            if (token.owner_) {
                std::lock_guard<std::mutex> other(token.owner_->mutex_);
                if (auto* g = token.owner_->group(token.group_))
                    g->uncertain = true;
            }
            token.owner_ = nullptr;
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(token.group_);
        token.owner_ = nullptr;
        if (!g)
            return false;
        bool valid = !foreign && sameGroup(expected, g->id) && g->pendingSubmission && token.ended_ && !token.overflow_;
        const bool failedClose =
            token.count_ == 1 && token.rows_[0].kind == NrSubmissionToken::Kind::close && token.rows_[0].result < 0;
        const bool full = token.count_ == 3 && token.rows_[0].kind == NrSubmissionToken::Kind::close &&
                          token.rows_[0].result >= 0 && token.rows_[1].kind == NrSubmissionToken::Kind::execute &&
                          token.rows_[2].kind == NrSubmissionToken::Kind::signal;
        valid = valid && (failedClose || full);
        for (size_t i = 0; i < token.count_; ++i)
            valid = valid && sameSubmission(token.rows_[i].key, g->identity.submission);
        g->pendingSubmission = false;
        // Preserve factual scalar observations even when the cohort cannot qualify.
        for (size_t i = 0; i < token.count_; ++i) {
            const auto& row = token.rows_[i];
            if (row.kind == NrSubmissionToken::Kind::close && !g->calls.close.observed)
                g->calls.close = {true, false, row.key, row.result};
            if (row.kind == NrSubmissionToken::Kind::execute) {
                ++g->calls.executeObservations;
                g->calls.lastExecuteActual = row.key;
            }
            if (!valid && row.kind == NrSubmissionToken::Kind::signal)
                signalFact(*g, row.key, row.value, row.result, false, false);
        }
        if (!valid) {
            for (size_t i = 0; i < token.count_; ++i)
                if (token.rows_[i].kind == NrSubmissionToken::Kind::execute)
                    g->submission.beforeExecute(token.rows_[i].key);
            g->uncertain = true;
            return false;
        }
        bool accepted = g->submission.observeClose(token.rows_[0].result);
        g->calls.close.qualified = accepted;
        if (failedClose) {
            g->possibleExposure = false;
            accepted = accepted && g->submission.abandonRecording();
        } else {
            accepted = accepted && g->submission.beforeExecute(token.rows_[1].key);
            const auto& row = token.rows_[2];
            const bool signalAccepted =
                accepted && g->submission.observeSignal(row.key.queue, row.key.fence, row.value, row.result, false);
            signalFact(*g, row.key, row.value, row.result, false, signalAccepted);
            accepted = signalAccepted;
        }
        if (!accepted)
            g->uncertain = true;
        return accepted;
    }
    bool NrWorkOwner::observeSignal(
        NrGroupId id, const NrSubmissionKey& actual, uint64_t value, int32_t result, bool recovery) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (!g)
            return false;
        const bool qualified = !foreign && g->phase == GroupPhase::sealed && !g->pendingSubmission && !g->uncertain &&
                               sameSubmission(actual, g->identity.submission) &&
                               g->submission.observeSignal(actual.queue, actual.fence, value, result, recovery);
        signalFact(*g, actual, value, result, recovery, qualified);
        return qualified;
    }
    bool NrWorkOwner::observeCompleted(NrGroupId id, const NrSubmissionKey& actual, uint64_t value) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        return !foreign && g && g->phase == GroupPhase::sealed && !g->pendingSubmission && !g->uncertain &&
               sameSubmission(actual, g->identity.submission) &&
               g->submission.observeCompleted(actual.queue, actual.fence, value);
    }
    bool NrWorkOwner::abandonGroup(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        return !foreign && g && (g->phase == GroupPhase::open || g->phase == GroupPhase::sealed) &&
               !g->pendingAcquisitions && !g->pendingSubmission && !g->uncertain && g->submission.abandonRecording();
    }
    bool NrWorkOwner::beginConsumer(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || rejected_ || detached_ || g->phase != GroupPhase::sealed || g->pendingAcquisitions ||
            g->uncertain || g->reader != Reader::pending || g->transferStarted)
            return false;
        g->reader = Reader::active;
        return true;
    }
    bool NrWorkOwner::completeConsumption(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || g->phase == GroupPhase::releasing || g->phase == GroupPhase::settled ||
            g->reader != Reader::active)
            return false;
        g->reader = Reader::closed;
        return true;
    }
    bool NrWorkOwner::discardReaders(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || g->phase == GroupPhase::releasing || g->phase == GroupPhase::settled ||
            g->reader != Reader::pending || g->transferPending)
            return false;
        g->reader = Reader::closed;
        return true;
    }
    uint64_t NrWorkOwner::beginTransfer(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || rejected_ || detached_ || g->phase != GroupPhase::sealed || g->pendingAcquisitions ||
            g->uncertain || g->reader != Reader::pending || g->transferStarted)
            return 0;
        g->transferStarted = true;
        g->transferPending = true;
        g->transfer = ++nextTransfer_;
        return g->transfer;
    }
    bool NrWorkOwner::finishTransfer(NrGroupId id, uint64_t transfer, NrReceiverResult result) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || g->phase == GroupPhase::releasing || g->phase == GroupPhase::settled ||
            !g->transferPending || !transfer || g->transfer != transfer || g->reader != Reader::pending)
            return false;
        if (result != NrReceiverResult::rejected && result != NrReceiverResult::deepOwnedAcknowledged)
            return false;
        g->transferPending = false;
        if (result == NrReceiverResult::rejected)
            return false;
        g->reader = Reader::closed;
        return true;
    }
    bool NrWorkOwner::workerBegin(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || detached_ || g->phase != GroupPhase::sealed || g->pendingAcquisitions || g->worker ||
            g->uncertain)
            return false;
        g->worker = true;
        return true;
    }
    bool NrWorkOwner::workerEnd(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        if (foreign || !g || g->phase == GroupPhase::releasing || g->phase == GroupPhase::settled || !g->worker)
            return false;
        g->worker = false;
        return true;
    }
    bool NrWorkOwner::releasable(const Group& g) const noexcept {
        return (g.phase == GroupPhase::open || g.phase == GroupPhase::sealed) && !g.uncertain &&
               !g.pendingAcquisitions && !g.pendingSubmission && !g.transferPending && !g.worker &&
               g.reader == Reader::closed && g.submission.canRelease();
    }
    NrReleaseToken NrWorkOwner::beginRelease(NrGroupId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(id);
        auto* f = frame(id.frame);
        if (foreign || !g || !f || !releasable(*g))
            return {};
        for (size_t i = 0; i < f->refCount; ++i)
            if (f->refs[i].group == id.slot && f->refs[i].acquired)
                g->releaseBatch[g->releaseCount++] = std::exchange(f->refs[i].pointer, nullptr);
        g->phase = GroupPhase::releasing;
        g->releasePhase = TxPhase::prepared;
        return {this, id, id.generation};
    }
    bool NrWorkOwner::finishRelease(NrGroupId expected, NrReleaseToken&& token) noexcept {
        if (token.owner_ != this) {
            if (token.owner_)
                token.owner_->quarantine(NrForeignKind::release, token.group_, token.transaction_);
            token.owner_ = nullptr;
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto* g = group(token.group_);
        const auto actual = token.group_;
        const auto tx = token.transaction_;
        token.owner_ = nullptr;
        if (!g)
            return false;
        if (foreign || !sameGroup(expected, actual) || tx != actual.generation || g->phase != GroupPhase::releasing ||
            g->releasePhase != TxPhase::executed || g->released != g->releaseCount) {
            g->uncertain = true;
            g->releasePhase = TxPhase::quarantined;
            return false;
        }
        g->phase = GroupPhase::settled;
        g->releasePhase = TxPhase::finalized;
        return true;
    }
    bool NrWorkOwner::reject(NrFailure why) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        rejectLocked(why);
        return true;
    }
    bool NrWorkOwner::detachRuntime() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        detached_ = true;
        rejectLocked(NrFailure::runtime_detached);
        return true;
    }
    bool NrWorkOwner::rejected() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_;
    }
    bool NrWorkOwner::runtimeDetached() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return detached_;
    }
    uint64_t NrWorkOwner::reservedBytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return reservedBytes_;
    }
    NrGroupSnapshot NrWorkOwner::snapshot(NrGroupId id) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto* g = group(id);
        const auto* f = frame(id.frame);
        if (!g || !f)
            return {};
        size_t owned = 0;
        for (size_t i = 0; i < f->refCount; ++i)
            if (f->refs[i].group == id.slot && f->refs[i].acquired)
                ++owned;
        if (g->phase == GroupPhase::releasing)
            owned = g->releaseCount - g->released;
        else if (g->phase == GroupPhase::settled)
            owned = 0;
        return {true,
                g->phase == GroupPhase::sealed,
                g->phase == GroupPhase::releasing,
                g->phase == GroupPhase::settled,
                g->uncertain || g->pendingAcquisitions != 0 || g->pendingSubmission ||
                    g->phase == GroupPhase::releasing || (g->submission.exposed() && !g->submission.canRelease()),
                g->submission.exposed(),
                g->possibleExposure,
                releasable(*g),
                g->submission.originalSignalSucceeded(),
                g->reader == Reader::pending,
                g->pendingSubmission,
                g->worker,
                owned,
                g->pendingAcquisitions,
                g->submission.coveringValue(),
                g->calls};
    }
    NrWorkOwner& processNrOwner() noexcept {
        static NrWorkOwner owner;
        return owner;
    }
} // namespace vdxrfg::pair_capture::nr
