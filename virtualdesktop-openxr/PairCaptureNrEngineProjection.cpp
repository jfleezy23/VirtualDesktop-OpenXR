#include "PairCaptureNrEngine.h"
#include <atomic>
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        std::atomic<uint64_t> nextPacketTransfer{1}, nextPacketMutation{1};
        bool sameHost(const HostLifetimeFacts& a, const HostLifetimeFacts& b) noexcept {
            return a.instance == b.instance && a.instanceLifetime == b.instanceLifetime &&
                   a.sessionHandle == b.sessionHandle && a.sessionLifetime == b.sessionLifetime;
        }
    } // namespace
    bool CaptureEngine::projectFrameIdentity(uint32_t frame, SourceReturnGuard& guard) noexcept {
        using namespace vdxrfg::pair_capture::nr;
        if (frame > 1 || !prepared_ || !ownerReserved_ || !budget_.preflightComplete || !nrSubmitted_[frame] ||
            !postFxAcquired_[frame] || !guard.covered())
            return false;
        auto& out = projection_[frame];
        if (out.identityProjected || out.header.transferId)
            return false;
        const auto& request = hostRequest();
        HostLifetimeFacts actualHost{};
        if (!hostLifetimeFacts(actualHost) || !sameHost(actualHost, reservedHost_) ||
            reservedOwnerNumericId_ != reinterpret_cast<uint64_t>(&hostWorkOwner()) ||
            reservedOwnerAttempt_ != request.attempt || ownerHostEpoch_ != reservedHost_.instanceLifetime ||
            !reservationObservationEpoch_)
            return false;
        const auto& input = frameInputs_[frame];
        const auto& source = sources_[frame];
        const auto& nr = nrInputs_[frame];
        const auto& graph = nr.pipeline;
        const auto& recording = nr.recording;
        SdkWarmFacts sdk{};
        if (!sdkWarmFacts(sdk) || !sdk.completeFrame || sdk.invalid || !sdk.tableLive || !sdk.featureLive[0] ||
            !sdk.featureLive[1] || !sdk.totalSequence || !input.rawSettings.outputWidth ||
            !input.rawSettings.outputHeight || !input.entryQpc || input.leaseQpc <= input.entryQpc ||
            !input.stackCount || input.stackCount > 48 || rawCopyIndex_[frame][0] >= 4 || !nr.pipelineObject)
            return false;
        NrIdentityV1 x{};
        x.nonce[0] = request.nonce[0];
        x.nonce[1] = request.nonce[1];
        x.pid = request.pid;
        x.creationFiletime = request.creation;
        x.sessionNonce[0] = ready_.sessionNonceLo;
        x.sessionNonce[1] = ready_.sessionNonceHi;
        x.hostInstanceAddress = reservedHost_.instance;
        x.hostLifetimeEpoch = reservedHost_.instanceLifetime;
        x.ownerHostEpoch = ownerHostEpoch_;
        x.sessionHandleObserved = reservedHost_.sessionHandle;
        x.sessionLifetimeEpoch = reservedHost_.sessionLifetime;
        x.hostSerial = input.serial;
        x.displayTimeBits = input.displayTime;
        x.sourceOccurrence = source.header.publicWitnessId;
        x.applicationSubmissionId = source.header.applicationSubmissionId;
        x.corpusFrameIndex = source.header.corpusFrameIndex;
        x.publicJournalWitness = source.header.publicWitnessId;
        x.eyeMask = 3;
        x.frameThread = input.frameThread;
        x.xrEntryQpc = input.entryQpc;
        x.producerLeaseQpc = input.leaseQpc;
        x.sourceGeneration = sourceGeneration_[frame];
        // Representative original source write event, not an aggregate generation:
        // exact four endpoint/write associations remain in source640 and role rows.
        x.sourceWriteEpoch = source.copies[rawCopyIndex_[frame][0]].destinationWriteObservationEpoch;
        x.fgContextAddress = ready_.fgContextAddress;
        x.fgContextLifetimeEpoch = ready_.fgContextLifetimeEpoch;
        x.fgHistoryGeneration = ready_.fgHistoryGeneration; // no ticket exists before original admission
        x.nrContextAddress = reinterpret_cast<uint64_t>(input.nrContext);
        x.nrContextLifetimeEpoch = input.nrContextLifetime;
        x.pipelineAddress = reinterpret_cast<uint64_t>(nr.pipelineObject);
        x.pipelineLifetimeEpoch = graph.nrPipelineLifetimeEpoch;
        x.pipelineGenerationEpoch = graph.nrPipelineGenerationEpoch;
        x.batchObjectEpoch = graph.nrBatchObjectEpoch;
        x.descriptorBindEpoch = graph.nrDescriptorBindEpoch;
        x.listObjectEpoch = recording.facts.nrListObjectEpoch;
        x.allocatorObjectEpoch = recording.facts.nrAllocatorObjectEpoch;
        x.recordingEpoch = recording.facts.nrRecordingEpoch;
        x.producerContextAddress = reinterpret_cast<uint64_t>(input.producerContext);
        x.producerDeviceAddress = reinterpret_cast<uint64_t>(input.producerDevice);
        x.nrDeviceAddress = reinterpret_cast<uint64_t>(recording.device);
        x.nrQueueAddress = reinterpret_cast<uint64_t>(recording.queue);
        x.nrFenceAddress = reinterpret_cast<uint64_t>(recording.fence);
        x.originalNrSignalValue = guard.submissionFacts().calls.original.value;
        x.privateProducerFenceAddress = reinterpret_cast<uint64_t>(producerFence_);
        x.privateProducerValue = groups_[frame][0].boundary;
        x.importedAppFenceAddress = reinterpret_cast<uint64_t>(appFence_);
        x.importedFgFenceAddress = reinterpret_cast<uint64_t>(fgFence_);
        x.luidLow = luidLow_;
        x.luidHigh = luidHigh_;
        x.actualProcessingPercent = input.rawSettings.processing;
        x.actualCoveragePercent = input.rawSettings.coverage;
        x.settingsObservedMask = 65535;
        x.pipelinePointerPresence = graph.pointerPresence;
        x.pipelineEpochObservedMask = graph.observedEpochs;
        x.featureLifetimeLeft = sdk.featureLifetime[0];
        x.featureLifetimeRight = sdk.featureLifetime[1];
        x.parameterTableLifetime = sdk.tableLifetime;
        x.warmFeatureWitnessEpoch = warmSdkWitness_;
        x.stackCount = input.stackCount;
        std::memcpy(x.entryStackAddresses, input.stack, sizeof input.stack);
        x.observedIdentityMask = 127;
        x.settings = input.rawSettings;
        NrHeaderV1 h{};
        h.magic = NR_MAGIC;
        h.size = sizeof(NrPacketV1);
        h.version = NR_ABI_VERSION;
        h.operation = NR_FRAME_TRANSFER;
        h.attempt = request.attempt;
        h.frameOrdinal = frame;
        if (!TryNextObservedEpoch(nextPacketTransfer, h.transferId) ||
            !TryNextObservedEpoch(nextPacketMutation, h.packetMutationEpoch))
            return false;
        h.sourceObservationCount = source.header.copyCount;
        h.frameOutcome = 1;
        h.actualNrMode = request.mode;
        for (uint64_t role = 1; role <= 16; ++role)
            if (warmRoleRequired(request.mode, input.upscaling, input.sharpening, role))
                h.requiredRoleMaskLo |= NR_ROLE_BIT(role);
        h.guardCoverage = 2;
        h.packetFrozen = 1;
        out.header = h;
        out.identity = x;
        out.identityProjected = true;
        return true;
    }
    bool CaptureEngine::projectFrameGroups(uint32_t frame, SourceReturnGuard& guard) noexcept {
        using namespace vdxrfg::pair_capture::nr;
        if (frame > 1 || !projection_[frame].identityProjected || projection_[frame].groupsProjected ||
            !guard.covered())
            return false;
        auto& out = projection_[frame];
        auto& owner = hostWorkOwner();
        for (uint32_t slot = 0; slot < 3; ++slot) {
            const auto& boundary = groups_[frame][slot];
            const auto& id = groupIdentities_[frame][slot];
            const auto actual = owner.snapshot(boundary.id);
            if (!actual.valid || !actual.sealed || actual.pendingAcquisitions || actual.pendingSubmission ||
                !actual.referenceInventory || !actual.originalSignalSucceeded || !actual.calls.original.qualified ||
                !actual.calls.covering.qualified || boundary.id.owner != reservedOwnerNumericId_ ||
                boundary.id.attempt != reservedOwnerAttempt_ || boundary.id.slot != slot ||
                !sameIdentity(id.source, ownerIdentity_) || id.serial != out.identity.hostSerial ||
                !sameSubmission(id.submission, boundary.actual) || id.signalValue != boundary.boundary)
                return false;
            // Snapshot uncertainty also includes ordinary pending GPU completion.
            // The atomic transfer gate rejects protocol uncertainty; release still requires completion.
            const uint64_t transfer = owner.beginTransfer(boundary.id);
            if (!transfer)
                return false; // preceding groups remain explicitly pending on any failure
            out.groupTransfers[slot] = transfer;
            auto& g = out.groups[slot];
            g.kind = slot;
            g.senderOwnerNumericId = boundary.id.owner;
            g.senderAttempt = boundary.id.attempt;
            g.frameOrdinal = static_cast<uint64_t>(boundary.id.frame);
            g.senderSlot = boundary.id.slot;
            g.senderGeneration = boundary.id.generation;
            g.transferId = transfer;
            g.sourceOccurrence = id.occurrence;
            g.sourceGeneration = id.sourceGeneration;
            g.descriptorEpoch = id.descriptorEpoch;
            g.deviceAddress = id.device;
            g.actualQueueOrContext = boundary.actual.queue;
            g.actualFence = boundary.actual.fence;
            g.actualList = boundary.actual.list;
            g.actualAllocator = boundary.actual.allocator;
            g.recordingEpoch = boundary.actual.recordingEpoch;
            g.nrContextLifetimeEpoch = id.source.nrGeneration;
            g.expectedSignalValue = boundary.boundary;
            g.closeObserved = actual.calls.close.observed;
            g.closeResultBits = static_cast<uint32_t>(actual.calls.close.result);
            g.executeObservationCount = actual.calls.executeObservations;
            g.originalSignalObserved = actual.calls.original.observed;
            g.originalSignalValue = actual.calls.original.value;
            g.originalSignalResultBits = static_cast<uint32_t>(actual.calls.original.result);
            g.coveringSignalObserved = actual.calls.covering.observed;
            g.coveringSignalValue = actual.calls.covering.value;
            g.coveringSignalResultBits = static_cast<uint32_t>(actual.calls.covering.result);
            // This is GPU ordering of source return, never a fabricated CPU completion.
            g.sourceReturnWaitObserved = guard.calls()[2].endObserved;
            g.sourceReturnWaitResultBits = guard.calls()[2].resultBits;
            g.qualificationMask = slot == 1 ? 31 : 28;
        }
        out.groupsProjected = true;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
