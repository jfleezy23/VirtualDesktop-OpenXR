#include "PairCaptureNrEngine.h"
#include <bcrypt.h>
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        bool addressIn(uint64_t address, const NrModuleV1& module) noexcept {
            return address && module.baseObserved && module.imageSize &&
                   module.imageSize <= UINT64_MAX - module.baseObserved && address >= module.baseObserved &&
                   address - module.baseObserved < module.imageSize;
        }
    } // namespace
    bool CaptureEngine::projectFrameSdk(uint32_t frame) noexcept {
        if (frame > 1 || moduleCount_ != 6 || projection_[frame].sdkCount)
            return false;
        auto& out = projection_[frame];
        size_t count{};
        SdkWarmFacts actual{};
        if (!sdkWarmFacts(actual) || actual.invalid || !actual.completeFrame ||
            actual.frameSerial != firstSerial_ + frame || actual.tableLifetime != out.identity.parameterTableLifetime ||
            actual.featureLifetime[0] != out.identity.featureLifetimeLeft ||
            actual.featureLifetime[1] != out.identity.featureLifetimeRight ||
            !sdkCopyObservations(out.sdkRows, 6, count) || count > 6)
            return false;
        const auto module = [&](uint64_t address, uint64_t& index) noexcept {
            index = NR_ABSENT;
            if (!address)
                return true;
            // These are the two actual observed dispatch domains, not a guessed
            // module inferred from an opaque SDK handle or preferred SDK branch.
            if (addressIn(address, modules_[2]))
                index = 2;
            else if (addressIn(address, modules_[3]))
                index = 3;
            return index != NR_ABSENT;
        };
        const auto append = [&](const SdkObservation& observed) noexcept {
            if (out.sdkCount >= 70)
                return false;
            SdkDispatchBinding binding{};
            if (!observed.ended || observed.threw ||
                !module(observed.call.dispatchAddressObserved, binding.dispatchModule) ||
                !module(observed.call.actual.pairedEvaluateAddress, binding.evaluateModule) ||
                !module(observed.call.aux.pairedReleaseAddress, binding.releaseModule))
                return false;
            out.sdk[out.sdkCount++] = {&observed, binding};
            return true;
        };
        for (uint32_t i = 0; i < sdkLifecycleCount_; ++i)
            if (!append(sdkLifecycle_[i]))
                return false;
        for (size_t i = 0; i < count; ++i)
            if (!append(out.sdkRows[i]))
                return false;
        for (uint32_t i = 0; i < moduleCount_; ++i)
            out.modules[i] = {
                &modules_[i], modulePinCalls_[i].kind ? &modulePinCalls_[i] : nullptr, moduleExpectations_[i]};
        return true;
    }
    bool CaptureEngine::finishFrameTransfer(uint32_t frame, SourceReturnGuard& guard) noexcept {
        using namespace vdxrfg::pair_capture::nr;
        if (frame > 1 || !prepared_ || !budget_.preflightComplete || !submit_ || !source_ || !nrSubmitted_[frame] ||
            !postFxAcquired_[frame] || !postFxExposed_[frame] || projection_[frame].assembled)
            return false;
        auto& out = projection_[frame];
        auto& owner = hostWorkOwner();
        bool receiverAdoptionMayHaveOccurred = false;
        const auto reject = [&](uint64_t reason, bool uncertain = false) noexcept {
            uncertain = uncertain || receiverAdoptionMayHaveOccurred;
            if (!uncertain)
                for (uint32_t slot = 0; slot < 3; ++slot)
                    if (out.groupTransfers[slot])
                        owner.finishTransfer(
                            groups_[frame][slot].id, out.groupTransfers[slot], NrReceiverResult::rejected);
            hostCaptureTerminal(reason, uncertain);
            return false;
        };
        // This executes the real all-return source boundary while the original
        // caller lease remains held, before any source/receiver foreign export.
        if (!guard.finish() || !guard.covered())
            return reject(NR_BAD_GROUP, true);
        if (!owner.closeFrame(static_cast<NrFrame>(frame), NrFrameOutcome::complete) ||
            !projectFrameIdentity(frame, guard) || !projectFrameGroups(frame, guard) ||
            !projectFrameControls(frame, guard) || !projectFrameRoles(frame) || !projectFrameSdk(frame))
            return reject(NR_BAD_OPERATION);
        for (uint32_t kind = 1; kind <= 2; ++kind) {
            if (kind == 1 && warm_.mode == NR_NATIVE100)
                continue;
            for (uint32_t eye = 0; eye < 2; ++eye)
                if (!copyPipelineDraw(firstSerial_ + frame, kind, eye, out.pipelineDraws[(kind - 1) * 2 + eye]))
                    return reject(NR_BAD_DRAW);
        }
        for (uint32_t kind = 3; kind <= 4; ++kind) {
            if (!(kind == 3 ? warm_.upscaling : warm_.sharpening))
                continue;
            for (uint32_t eye = 0; eye < 2; ++eye)
                if (!copyPostFxRecord(firstSerial_ + frame, eye, kind, out.postFxRecords[(kind - 3) * 2 + eye]))
                    return reject(NR_BAD_DRAW);
        }
        // A header/identity-only temporary occupies the eventual output workspace;
        // erase it before calling the pure assembler's fresh-output contract.
        out.packet.header = out.header;
        out.packet.identity = out.identity;
        const bool sidecar = copyPostFxSidecar(out.packet, warm_.upscaling, warm_.sharpening, out.postFx);
        std::memset(&out.packet, 0, sizeof out.packet);
        if (!sidecar)
            return reject(NR_BAD_DRAW);
        EnginePacketInput input{};
        input.header = &out.header;
        input.identity = &out.identity;
        input.budget = &budget_;
        input.cohort = {&hostRequest(), &ready_, &fgBudget_, &sources_[frame], &out.postFx, &moduleExpectations_[4]};
        input.refs = {refs_[frame], refCounts_[frame]};
        input.groups = {out.groups, 3};
        input.modules = {out.modules, moduleCount_};
        input.controlCalls = {out.controls, out.controlCount};
        input.aliases = {out.aliases, out.aliasCount};
        input.sdk = {out.sdk, out.sdkCount};
        input.roles = {out.roles, out.roleCount};
        input.pipelineProgram = &nrInputs_[frame].programs;
        DrawPacketInput drawInput{};
        drawInput.frame = &input;
        drawInput.pipeline = {out.pipelineDraws, 4};
        drawInput.postFx = {out.postFxRecords, 4};
        drawInput.shaderModuleIndex = 0;
        NrError error{};
        if (!projectDrawPacket(out.drawProjection, drawInput, error))
            return reject(error.code ? error.code : NR_BAD_DRAW);
        input.drawDescriptors = {out.drawProjection.descriptors.data(), out.drawProjection.descriptorCount};
        input.draws = {out.drawProjection.inputs.data(), out.drawProjection.drawCount};
        if (!assembleEnginePacket(out.packet, input, error))
            return reject(error.code ? error.code : NR_BAD_OPERATION);
        out.assembled = true;
        static_assert(sizeof(FrameProjection::DigestInput) == sizeof(NrPacketV1) + sizeof(NrPostFxSidecarV1));
        out.digestInput.packet = out.packet;
        out.digestInput.postFx = out.postFx;
        for (auto& ref : out.digestInput.packet.refs)
            ref.borrowedIUnknownAddress = 0;
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                       nullptr,
                       0,
                       reinterpret_cast<PUCHAR>(&out.digestInput),
                       static_cast<ULONG>(sizeof out.digestInput),
                       reinterpret_cast<PUCHAR>(out.digest),
                       sizeof out.digest) < 0)
            return reject(NR_BAD_ACK);
        // Exact original immutable packet, source witness and sidecar stay alive in
        // this process-lifetime engine through both synchronous foreign calls.
        if (source_(&out.packet, &sources_[frame]) != 1)
            return reject(NR_BAD_SOURCE, true);
        const uint32_t returned = submit_(&out.packet, &out.postFx, &out.ack);
        if (returned != NR_DEEP_OWNED || !nr_validate_ack(&out.packet, &out.ack, sizeof out.ack, out.digest, &error))
            return reject(NR_BAD_ACK, !definitiveTransferRejection(returned, out.ack));
        receiverAdoptionMayHaveOccurred = true;
        for (uint32_t slot = 0; slot < 3; ++slot)
            if (!owner.finishTransfer(
                    groups_[frame][slot].id, out.groupTransfers[slot], NrReceiverResult::deepOwnedAcknowledged))
                return reject(NR_BAD_GROUP, true);
        if (!hostFinishFrameTransfer(frame))
            return reject(NR_BAD_GROUP, true);
        out.transferred = true;
        guard.transferred();
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
