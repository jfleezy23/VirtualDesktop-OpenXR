#include "PairCaptureNrEngine.h"
#include "PairCaptureNrPostFx.h"
#include <atomic>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        std::atomic<uint64_t> nextPostFxDescription{1};
        bool sameDescription(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) noexcept {
            return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
                   a.ArraySize == b.ArraySize && a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
                   a.SampleDesc.Quality == b.SampleDesc.Quality && a.Usage == b.Usage && a.BindFlags == b.BindFlags &&
                   a.CPUAccessFlags == b.CPUAccessFlags && a.MiscFlags == b.MiscFlags;
        }
    } // namespace
    ID3D11Texture2D* CaptureEngine::postFxRoleSource(uint32_t frame, uint64_t role, uint64_t eye) const noexcept {
        if (frame > 1 || eye > 2)
            return nullptr;
        const auto& input = postFxInputs_[frame];
        if (eye == 2)
            return role == NR_CANONICAL_DEPTH ? input.canonical : nullptr;
        switch (role) {
        case NR_POST_EASU:
            return input.easu[eye];
        case NR_POST_CAS:
            return input.cas[eye];
        case NR_FINAL_COLOUR:
            return input.finalColour[eye];
        default:
            return nullptr;
        }
    }
    bool CaptureEngine::acquirePostFx(uint32_t frame, const CapturePostFxInput& input, SourceReturnGuard&) noexcept {
        using namespace vdxrfg::pair_capture::nr;
        if (frame > 1 || !budget_.preflightComplete || !nrSubmitted_[frame] || postFxAcquired_[frame] ||
            input.fgContext != ready_.fgContextAddress || !input.canonical || !input.canonicalLifetime ||
            nrBankTextureLifetime(input.canonical) != input.canonicalLifetime || !input.finalColour[0] ||
            !input.finalColour[1] ||
            input.bindingCount != 2u * (unsigned(warm_.upscaling) + unsigned(warm_.sharpening)) ||
            ((warm_.upscaling || warm_.sharpening) && !input.originalGlobalUnbindReturned))
            return false;
        for (uint32_t eye = 0; eye < 2; ++eye)
            if ((!!input.easu[eye] != warm_.upscaling) || (!!input.cas[eye] != warm_.sharpening))
                return false;
        postFxInputs_[frame] = input;
        NrBorrowedRef borrowed[30]{};
        const uint32_t start = refCounts_[frame];
        uint32_t count = 0;
        uint64_t descriptorObservation = 0;
        uint32_t sourceSlots[7]{}, sourceCount = 0;
        auto append = [&](IUnknown* pointer,
                          uint64_t type,
                          uint64_t role,
                          uint64_t life,
                          uint64_t allocation = 0,
                          uint64_t bytes = 0,
                          bool unique = false) noexcept {
            if (!pointer || !life || life == UINT64_MAX)
                return false;
            if (unique)
                for (uint32_t i = 0; i < count; ++i) {
                    const auto& row = refs_[frame][start + i];
                    if (row.borrowedIUnknownAddress == reinterpret_cast<uint64_t>(pointer) && row.interfaceEnum == type)
                        return row.roleMask == role && row.owningSourceLifetimeEpoch == life &&
                               row.physicalAllocationId == allocation && row.chargedPrivateBytes == bytes;
                }
            if (count >= 30 || start + count >= 89)
                return false;
            borrowed[count] = {pointer, bytes, static_cast<NrRefRole>(role)};
            refs_[frame][start + count] = {
                reinterpret_cast<uint64_t>(pointer), type, role, 2, allocation, bytes, life, 1};
            ++count;
            return true;
        };
        for (uint32_t slot = 0; slot < snapshots_.count(); ++slot) {
            const auto* a = snapshots_.get(frame, slot);
            if (!a || a->plan.group != 2)
                continue;
            auto* resource = postFxRoleSource(frame, a->plan.role, a->plan.eye);
            if (!resource || a->adopted || !a->allocated || sourceCount >= 7)
                return false;
            D3D11_TEXTURE2D_DESC actual{};
            resource->GetDesc(&actual);
            if (!sameDescription(actual, a->plan.texture11) ||
                !TryNextObservedEpoch(nextPostFxDescription, descriptorObservation) ||
                !append(resource, NR_TEXTURE11, 1, nrBankTextureLifetime(resource)) ||
                !append(a->texture11, NR_TEXTURE11, 2, a->lifetime, a->allocationId, a->plan.layout.allocationCharge))
                return false;
            sourceSlots[sourceCount++] = slot;
        }
        if (sourceCount != 3u + 2u * (unsigned(warm_.upscaling) + unsigned(warm_.sharpening)) ||
            !append(preparedInput_.producerDevice, NR_DEVICE11, 4, status_.hostLifetimeEpoch) ||
            !append(preparedInput_.producerContext, NR_CONTEXT11, 8, status_.hostLifetimeEpoch) ||
            !append(producerFence_, NR_FENCE11, 64, fenceFacts_.producerLifetime) ||
            !append(fgFence_, NR_FENCE12, 64, fenceFacts_.fgLifetime))
            return false;
        bool seen[2][2]{};
        for (uint32_t i = 0; i < input.bindingCount; ++i) {
            const auto& binding = input.bindings[i];
            PostFxRecord record{};
            if (binding.eye > 1 || (binding.kind != 3 && binding.kind != 4) || seen[binding.kind - 3][binding.eye] ||
                (binding.kind == 3 ? !warm_.upscaling : !warm_.sharpening) ||
                !copyPostFxRecord(firstSerial_ + frame,
                                  static_cast<uint32_t>(binding.eye),
                                  static_cast<uint32_t>(binding.kind),
                                  record) ||
                !postFxRecordComplete(record) ||
                record.draw.listOrContext != reinterpret_cast<uint64_t>(preparedInput_.producerContext) ||
                record.draw.psoAddress != reinterpret_cast<uint64_t>(binding.shader) ||
                record.draw.rootAddress != reinterpret_cast<uint64_t>(binding.buffer) ||
                record.draw.rootTable0 != reinterpret_cast<uint64_t>(binding.srv) ||
                record.draw.rootTable1 != reinterpret_cast<uint64_t>(binding.uav) ||
                record.samplerAddress != reinterpret_cast<uint64_t>(binding.sampler) ||
                record.descriptors[1].resourceAddress !=
                    reinterpret_cast<uint64_t>(binding.kind == 3 ? input.easu[binding.eye] : input.cas[binding.eye]))
                return false;
            seen[binding.kind - 3][binding.eye] = true;
            if (!append(binding.shader, NR_SHADER11, 128, record.shaderLifetime, 0, 0, true) ||
                !append(binding.buffer, NR_BUFFER11, 128, record.bufferLifetime, 0, 0, true) ||
                !append(binding.srv, NR_VIEW11, 128, record.descriptors[0].objectLifetimeEpoch, 0, 0, true) ||
                !append(binding.uav, NR_VIEW11, 128, record.descriptors[1].objectLifetimeEpoch, 0, 0, true) ||
                (binding.kind == 3 &&
                 !append(binding.sampler, NR_SAMPLER11, 128, record.descriptors[2].objectLifetimeEpoch, 0, 0, true)))
                return false;
        }
        auto& boundary = groups_[frame][2];
        boundary.actual = groups_[frame][0].actual;
        boundary.boundary = groups_[frame][0].boundary;
        boundary.fence11 = producerFence_;
        NrGroupIdentity identity{};
        identity.source = ownerIdentity_;
        identity.serial = firstSerial_ + frame;
        identity.occurrence = sources_[frame].header.publicWitnessId;
        identity.sourceGeneration = sourceGeneration_[frame];
        identity.descriptorEpoch = descriptorObservation;
        identity.device = reinterpret_cast<uint64_t>(preparedInput_.producerDevice);
        identity.submission = boundary.actual;
        identity.signalValue = boundary.boundary;
        auto& owner = hostWorkOwner();
        boundary.id =
            owner.openGroup(static_cast<NrFrame>(frame), vdxrfg::pair_capture::nr::NrGroupKind::postFx11, identity);
        groupIdentities_[frame][2] = identity;
        auto token = owner.beginAcquire(boundary.id, borrowed, count);
        if (!boundary.id.valid() || !token.valid() || !token.execute() ||
            !owner.finishAcquire(boundary.id, std::move(token)) || !owner.sealGroup(boundary.id) ||
            !hostRegisterGroup(frame, 2, boundary)) {
            hostCaptureTerminal(NR_BAD_REF, true);
            return false;
        }
        refCounts_[frame] += count;
        for (uint32_t i = 0; i < sourceCount; ++i) {
            const auto slot = sourceSlots[i];
            const auto* a = snapshots_.get(frame, slot);
            if (!a ||
                !matchingAcquiredSnapshot(refs_[frame] + start,
                                          count,
                                          reinterpret_cast<uint64_t>(a->texture11),
                                          a->lifetime,
                                          a->allocationId,
                                          a->plan.layout.allocationCharge,
                                          2) ||
                !snapshots_.releaseInitialAfterAcquire(frame, slot, boundary)) {
                hostCaptureTerminal(NR_BAD_REF, true);
                return false;
            }
        }
        postFxAcquired_[frame] = true;
        return true;
    }
    bool CaptureEngine::capturePostFxRole(uint32_t frame,
                                          uint64_t role,
                                          uint64_t eye,
                                          SourceReturnGuard& guard,
                                          const SnapshotRoleMetadata& geometry) noexcept {
        if (frame > 1 || !postFxAcquired_[frame] || !geometry.geometryObserved ||
            geometry.hostSerial != firstSerial_ + frame || !geometry.rectWidth || !geometry.rectHeight ||
            !geometry.cropWidth || !geometry.cropHeight)
            return false;
        auto* resource = postFxRoleSource(frame, role, eye);
        ResourceWriteFact writer{};
        const auto life = nrBankTextureLifetime(resource);
        if (!resource || !life ||
            !heldSourceIdentity(
                refs_[frame], refCounts_[frame], reinterpret_cast<uint64_t>(resource), life, NR_TEXTURE11, 2) ||
            !sourceProducerWrite11(resource, writer) || writer.serial != firstSerial_ + frame)
            return false;
        if (role == NR_CANONICAL_DEPTH &&
            (writer.kind != NR_DISPATCH || writer.api != 11 ||
             writer.contextOrList != reinterpret_cast<uint64_t>(preparedInput_.producerContext)))
            return false;
        uint32_t slot = 0;
        for (; slot < snapshots_.count(); ++slot) {
            const auto* a = snapshots_.get(frame, slot);
            if (a && a->plan.role == role && a->plan.eye == eye)
                break;
        }
        if (slot == snapshots_.count())
            return false;
        if (!postFxExposed_[frame]) {
            if (!guard.exposePostFx(groups_[frame][2]))
                return false;
            postFxExposed_[frame] = true;
        }
        if (!snapshots_.copy11(
                frame, slot, preparedInput_.producerContext, resource, groups_[frame][2], life, writer.sequence)) {
            hostCaptureTerminal(NR_BAD_ROLE, true);
            return false;
        }
        roleWriters_[frame][slot] = writer;
        roleMetadata_[frame][slot] = geometry;
        roleMetadata_[frame][slot].sourceOccurrence = sources_[frame].header.publicWitnessId;
        // A D12 writer is projected through the actual reverse API5 storage alias
        // by packet assembly; its original list and write event remain unchanged.
        return true;
    }
    bool CaptureEngine::matchesFinalColour(uint32_t frame, ID3D11Texture2D* const actual[2]) const noexcept {
        return frame < 2 && postFxAcquired_[frame] && actual && actual[0] == postFxInputs_[frame].finalColour[0] &&
               actual[1] == postFxInputs_[frame].finalColour[1] &&
               heldSourceIdentity(refs_[frame],
                                  refCounts_[frame],
                                  reinterpret_cast<uint64_t>(actual[0]),
                                  nrBankTextureLifetime(actual[0]),
                                  NR_TEXTURE11,
                                  2) &&
               heldSourceIdentity(refs_[frame],
                                  refCounts_[frame],
                                  reinterpret_cast<uint64_t>(actual[1]),
                                  nrBankTextureLifetime(actual[1]),
                                  NR_TEXTURE11,
                                  2);
    }
} // namespace virtualdesktop_openxr::nr_capture
