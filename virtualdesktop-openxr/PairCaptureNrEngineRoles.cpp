#include "PairCaptureNrEngine.h"
namespace virtualdesktop_openxr::nr_capture {
    bool CaptureEngine::projectFrameRoles(uint32_t frame) noexcept {
        if (frame > 1 || !projection_[frame].controlCount || projection_[frame].roleCount)
            return false;
        auto& out = projection_[frame];
        const auto reference = [&](uint64_t address,
                                   uint64_t life,
                                   uint64_t type,
                                   uint64_t group,
                                   uint64_t role,
                                   uint64_t& index) noexcept {
            for (uint32_t i = 0; i < refCounts_[frame]; ++i) {
                const auto& r = refs_[frame][i];
                if (r.borrowedIUnknownAddress == address && r.owningSourceLifetimeEpoch == life &&
                    r.interfaceEnum == type && r.groupIndex == group && r.roleMask == role && r.observedNonNull == 1) {
                    index = i;
                    return true;
                }
            }
            return false;
        };
        for (uint32_t slot = 0; slot < snapshots_.count(); ++slot) {
            const auto* a = snapshots_.get(frame, slot);
            if (!a || !a->allocated || !a->adopted || !a->copied || out.roleCount >= 28)
                return false;
            auto& metadata = roleMetadata_[frame][slot];
            const auto& writer = roleWriters_[frame][slot];
            const auto& description = a->sourceDescription;
            const bool d12 = a->plan.group == 1;
            const uint64_t snapshot = reinterpret_cast<uint64_t>(d12 ? static_cast<IUnknown*>(a->buffer12)
                                                                     : static_cast<IUnknown*>(a->texture11));
            if (!reference(description.resourceAddress,
                           description.resourceLifetimeEpoch,
                           d12 ? NR_RESOURCE12 : NR_TEXTURE11,
                           a->plan.group,
                           1,
                           metadata.sourceRefIndex) ||
                !reference(snapshot,
                           a->lifetime,
                           d12 ? NR_RESOURCE12 : NR_TEXTURE11,
                           a->plan.group,
                           2,
                           metadata.snapshotRefIndex))
                return false;
            metadata.copyModuleIndex = 0;
            metadata.aliasIndexOrAbsent = NR_ABSENT;
            metadata.writerAliasCount = 0;
            metadata.writerAliasIndices = {NR_ABSENT, NR_ABSENT};
            if (d12 && a->plan.role == NR_POST_RESOLVE) {
                SourceTextureFacts fact{};
                if (!sourceNrGet(reinterpret_cast<ID3D12Resource*>(description.resourceAddress), fact) ||
                    !nrAliasComplete(fact,
                                     fact.producer,
                                     description.resourceAddress,
                                     reinterpret_cast<uint64_t>(preparedInput_.producerDevice),
                                     reinterpret_cast<uint64_t>(preparedInput_.nrDevice)) ||
                    fact.applicationLifetime != description.resourceLifetimeEpoch ||
                    writer.resource != description.resourceAddress ||
                    writer.lifetime != description.resourceLifetimeEpoch ||
                    writer.sequence != description.sourceWriteEpoch || writer.api != 12 || writer.kind != NR_DRAW)
                    return false;
                // The original resolve wrote the shared Resource12 endpoint. The
                // observed API5 import also identifies the bank consumed by D11 FX,
                // independently of which endpoint wrote each later phase snapshot.
                NrAliasV1 output{};
                output.sourceAddress = fact.producer;
                output.sourceLifetime = fact.producerLifetime;
                output.targetAddress = fact.application;
                output.targetLifetime = fact.applicationLifetime;
                output.sourceDevice = fact.producerDevice;
                output.targetDevice = fact.applicationDevice;
                output.sourceWriteEpoch = output.targetWriteEpoch = writer.sequence;
                output.exportedHandleObserved = fact.sharedHandle;
                output.apiKind = NR_TEXTURE11_TO_RESOURCE12;
                output.observedMask = 3;
                output.format = a->plan.texture12.Format;
                output.subresource = metadata.selectedArraySlice;
                uint64_t outputIndex{};
                if (!appendAlias(frame, output, sourceExportCall(fact), sourceImportCall(fact), outputIndex))
                    return false;
            }
            if (writer.resource != description.resourceAddress ||
                writer.lifetime != description.resourceLifetimeEpoch ||
                writer.sequence != description.sourceWriteEpoch) {
                SourceTextureFacts fact{};
                uint64_t kind{};
                if (a->plan.group == 0) {
                    uint32_t raw = 0;
                    for (; raw < 4; ++raw)
                        if (frameInputs_[frame].raw[raw].role == a->plan.role &&
                            frameInputs_[frame].raw[raw].eye == a->plan.eye)
                            break;
                    if (raw == 4 || rawCopyIndex_[frame][raw] >= 4)
                        return false;
                    fact = rawFacts_[frame][raw];
                    kind = NR_TEXTURE11_TO_TEXTURE11;
                    const auto& source = sources_[frame].copies[rawCopyIndex_[frame][raw]];
                    if (writer.resource != source.destinationAddress ||
                        writer.sequence != source.destinationWriteObservationEpoch ||
                        writer.lifetime != fact.applicationLifetime || description.resourceAddress != fact.producer ||
                        description.resourceLifetimeEpoch != fact.producerLifetime)
                        return false;
                } else {
                    auto* imported =
                        reinterpret_cast<ID3D12Resource*>(d12 ? description.resourceAddress : writer.resource);
                    if (!sourceNrGet(imported, fact) ||
                        !nrAliasComplete(fact,
                                         fact.producer,
                                         fact.application,
                                         reinterpret_cast<uint64_t>(preparedInput_.producerDevice),
                                         reinterpret_cast<uint64_t>(preparedInput_.nrDevice)))
                        return false;
                    kind = NR_TEXTURE11_TO_RESOURCE12;
                    if (d12) {
                        if (description.resourceAddress != fact.application ||
                            description.resourceLifetimeEpoch != fact.applicationLifetime)
                            return false;
                        if (writer.resource != fact.producer || writer.lifetime != fact.producerLifetime) {
                            SourceTextureFacts application{};
                            ResourceWriteFact actual{};
                            if (!sourceApplicationWrite11(fact.producer, fact.producerLifetime, actual, &application) ||
                                std::memcmp(&actual, &writer, sizeof writer) ||
                                writer.sequence != description.sourceWriteEpoch ||
                                application.producerDevice != fact.producerDevice)
                                return false;
                            // The actual app copy wrote the API4 imported interface.
                            // Walk backwards to shared Texture11 storage, then
                            // forwards through API5 to the consumed Resource12.
                            NrAliasV1 first{};
                            first.sourceAddress = application.producer;
                            first.sourceLifetime = application.producerLifetime;
                            first.targetAddress = application.application;
                            first.targetLifetime = application.applicationLifetime;
                            first.sourceDevice = application.producerDevice;
                            first.targetDevice = application.applicationDevice;
                            first.sourceWriteEpoch = description.sourceWriteEpoch;
                            first.targetWriteEpoch = writer.sequence;
                            first.exportedHandleObserved = application.sharedHandle;
                            first.apiKind = NR_TEXTURE11_TO_TEXTURE11;
                            first.observedMask = 3;
                            first.format = a->plan.texture12.Format;
                            first.subresource = metadata.selectedArraySlice;
                            uint64_t firstIndex{};
                            if (!appendAlias(frame,
                                             first,
                                             sourceExportCall(application),
                                             sourceImportCall(application),
                                             firstIndex))
                                return false;
                            metadata.writerAliasIndices[metadata.writerAliasCount++] = firstIndex;
                        }
                    } else if (writer.resource != fact.application || writer.lifetime != fact.applicationLifetime ||
                               description.resourceAddress != fact.producer ||
                               description.resourceLifetimeEpoch != fact.producerLifetime)
                        return false;
                }
                NrAliasV1 alias{};
                alias.sourceAddress = fact.producer;
                alias.sourceLifetime = fact.producerLifetime;
                alias.targetAddress = fact.application;
                alias.targetLifetime = fact.applicationLifetime;
                alias.sourceDevice = fact.producerDevice;
                alias.targetDevice = fact.applicationDevice;
                alias.sourceWriteEpoch =
                    writer.resource == fact.producer ? writer.sequence : description.sourceWriteEpoch;
                alias.targetWriteEpoch =
                    writer.resource == fact.application ? writer.sequence : description.sourceWriteEpoch;
                alias.exportedHandleObserved = fact.sharedHandle;
                alias.apiKind = kind;
                alias.observedMask = 3;
                alias.format = d12 ? a->plan.texture12.Format : a->plan.texture11.Format;
                alias.subresource = metadata.selectedArraySlice;
                uint64_t aliasIndex{};
                if (!appendAlias(frame, alias, sourceExportCall(fact), sourceImportCall(fact), aliasIndex))
                    return false;
                if (metadata.writerAliasCount >= metadata.writerAliasIndices.size())
                    return false;
                metadata.writerAliasIndices[metadata.writerAliasCount++] = aliasIndex;
                metadata.aliasIndexOrAbsent = aliasIndex;
            }
            out.roles[out.roleCount++] = {a, &metadata, &writer};
        }
        return out.roleCount == warm_.count;
    }
} // namespace virtualdesktop_openxr::nr_capture
