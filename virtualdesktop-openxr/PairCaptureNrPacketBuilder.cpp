#include "PairCaptureNrPacketBuilder.h"
#include "PairCaptureNrDescriptions.h"
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        bool epoch(uint64_t value) noexcept {
            return value && value != UINT64_MAX;
        }
        bool range(uint64_t first, uint64_t count, uint64_t bound) noexcept {
            return first <= bound && count <= bound - first;
        }
        bool emptyCopy(const SnapshotCopyFact& c) noexcept {
            return !c.source && !c.destination && !c.contextOrList && !c.sourceSubresource && !c.destinationOffset &&
                   !c.beginQpc && !c.endQpc && !c.thread && !c.beginObserved && !c.endObserved;
        }
        bool storageAlias(const NrPacketV1& p, uint64_t index) noexcept {
            if (index >= p.header.aliasCount)
                return false;
            const auto& a = p.aliases[index];
            if ((a.apiKind != NR_TEXTURE11_TO_TEXTURE11 && a.apiKind != NR_TEXTURE11_TO_RESOURCE12) ||
                a.observedMask != 3 || a.reserved || !a.sourceAddress || !a.targetAddress ||
                a.sourceAddress == a.targetAddress || !a.sourceDevice || !a.targetDevice || !epoch(a.sourceLifetime) ||
                !epoch(a.targetLifetime) || !epoch(a.sourceWriteEpoch) || !epoch(a.targetWriteEpoch) ||
                !a.exportedHandleObserved || a.exportCallIndex >= p.header.callCount ||
                a.importCallIndex >= p.header.callCount)
                return false;
            const auto& ex = p.calls[a.exportCallIndex];
            const auto& im = p.calls[a.importCallIndex];
            return ex.kind == NR_EXPORT_FENCE && im.kind == NR_IMPORT_FENCE && ex.beginObserved && ex.endObserved &&
                   im.beginObserved && im.endObserved && ex.resultType == NR_HRESULT && im.resultType == NR_HRESULT &&
                   ex.resultBits <= INT32_MAX && im.resultBits <= INT32_MAX && ex.classifiedSuccess == 1 &&
                   im.classifiedSuccess == 1 && ex.deviceAddress == a.sourceDevice &&
                   im.deviceAddress == a.targetDevice && ex.fenceAddress == a.sourceAddress &&
                   im.fenceAddress == a.targetAddress && ex.aux.auxiliaryValue == a.exportedHandleObserved &&
                   im.aux.auxiliaryValue == a.exportedHandleObserved;
        }
        bool writerRelation(const NrPacketV1& p,
                            const NrDescriptorV1& source,
                            const SnapshotRoleMetadata& m,
                            const ResourceWriteFact& w) noexcept {
            if (m.writerAliasCount > m.writerAliasIndices.size() || !w.resource || !epoch(w.lifetime) ||
                !epoch(w.sequence) || !epoch(w.serial) || !w.contextOrList || !w.thread || !w.qpc ||
                (w.api != 11 && w.api != 12) || (w.api == 12 && !epoch(w.recordingEpoch)) ||
                (w.api == 11 && w.recordingEpoch) ||
                (w.kind != NR_COPY && w.kind != NR_DISPATCH && w.kind != NR_DRAW && w.kind != NR_EVALUATE) ||
                (w.kind == NR_EVALUATE && (w.api != 12 || !epoch(w.actualCallSequence))))
                return false;
            uint64_t address = w.resource, life = w.lifetime, write = w.sequence, api = w.api;
            for (uint64_t i = 0; i < m.writerAliasIndices.size(); ++i) {
                const auto index = m.writerAliasIndices[i];
                if (i >= m.writerAliasCount) {
                    if (index != NR_ABSENT)
                        return false;
                    continue;
                }
                if (!storageAlias(p, index))
                    return false;
                for (uint64_t j = 0; j < i; ++j)
                    if (m.writerAliasIndices[j] == index)
                        return false;
                const auto& a = p.aliases[index];
                const uint64_t targetApi = a.apiKind == NR_TEXTURE11_TO_RESOURCE12 ? 12 : 11;
                if (address == a.sourceAddress && life == a.sourceLifetime && write == a.sourceWriteEpoch &&
                    api == 11) {
                    address = a.targetAddress;
                    life = a.targetLifetime;
                    write = a.targetWriteEpoch;
                    api = targetApi;
                } else if (address == a.targetAddress && life == a.targetLifetime && write == a.targetWriteEpoch &&
                           api == targetApi) {
                    address = a.sourceAddress;
                    life = a.sourceLifetime;
                    write = a.sourceWriteEpoch;
                    api = 11;
                } else
                    return false;
                if (i + 1 < m.writerAliasCount && address == w.resource && life == w.lifetime && write == w.sequence)
                    return false;
            }
            const uint64_t expectedApi = source.descriptionKind == NR_D11_TEXTURE2D_DESCRIPTION  ? 11
                                         : source.descriptionKind == NR_D12_RESOURCE_DESCRIPTION ? 12
                                                                                                 : 0;
            if (m.writerAliasCount && m.aliasIndexOrAbsent != m.writerAliasIndices[m.writerAliasCount - 1])
                return false;
            if (m.aliasIndexOrAbsent != NR_ABSENT) {
                if (!storageAlias(p, m.aliasIndexOrAbsent))
                    return false;
                const auto& a = p.aliases[m.aliasIndexOrAbsent];
                const uint64_t targetApi = a.apiKind == NR_TEXTURE11_TO_RESOURCE12 ? 12 : 11;
                if (!((expectedApi == 11 && a.sourceAddress == source.resourceAddress &&
                       a.sourceLifetime == source.resourceLifetimeEpoch &&
                       a.sourceWriteEpoch == source.sourceWriteEpoch) ||
                      (expectedApi == targetApi && a.targetAddress == source.resourceAddress &&
                       a.targetLifetime == source.resourceLifetimeEpoch &&
                       a.targetWriteEpoch == source.sourceWriteEpoch)))
                    return false;
            }
            return api == expectedApi && address == source.resourceAddress && life == source.resourceLifetimeEpoch &&
                   write == source.sourceWriteEpoch;
        }
    } // namespace
    bool appendSnapshotRole(NrPacketV1& p,
                            const SnapshotAllocation& a,
                            const SnapshotRoleMetadata& m,
                            const ResourceWriteFact& writer) noexcept {
        const auto& h = p.header;
        const auto& plan = a.plan;
        const auto& description = a.sourceDescription;
        const uint64_t subresources = plan.subresources;
        const uint64_t footprintFirst = h.footprintCount;
        const uint64_t callFirst = h.callCount;
        if (h.magic != NR_MAGIC || h.size != sizeof p || h.version != 1 || h.operation != NR_FRAME_TRANSFER ||
            h.flags || h.frameOrdinal > 1 || h.refCount > 128 || h.roleCount >= 64 || h.descriptorCount >= 64 ||
            footprintFirst > 128 || h.writeCount >= 128 || callFirst > 160 || h.groupCount != 3 || h.aliasCount > 64 ||
            !h.moduleCount || h.moduleCount > 8 || m.copyModuleIndex >= h.moduleCount ||
            (h.actualNrMode != NR_NATIVE100 && h.actualNrMode != NR_SPATIAL85) || !a.allocated || !a.adopted ||
            !a.copied || a.initial11.Get() || a.initial12.Get() || !epoch(a.allocationId) || !epoch(a.lifetime) ||
            !epoch(a.writeEpoch) || !m.geometryObserved || plan.role < 1 || plan.role > 16 || plan.group > 2 ||
            !subresources || subresources > 2 ||
            (plan.role <= 2    ? plan.group != 0
             : plan.role <= 12 ? plan.group != 1
                               : plan.group != 2) ||
            (plan.role == 7 || plan.role == 12 || plan.role == 16 ? plan.eye != 2 : plan.eye > 1) ||
            !(h.requiredRoleMaskLo & NR_ROLE_BIT(plan.role)) ||
            static_cast<uint64_t>(a.ownerGroup.frame) != h.frameOrdinal || a.ownerGroup.slot != plan.group ||
            m.hostSerial != p.identity.hostSerial || !epoch(m.hostSerial) ||
            m.sourceOccurrence != p.identity.sourceOccurrence || !epoch(m.sourceOccurrence) || m.projectionIndex ||
            m.sourceRefIndex >= h.refCount || m.snapshotRefIndex >= h.refCount ||
            m.sourceRefIndex == m.snapshotRefIndex ||
            (m.aliasIndexOrAbsent != NR_ABSENT && m.aliasIndexOrAbsent >= h.aliasCount))
            return false;
        const bool d12 = plan.group == 1;
        if (plan.copyMethod != (d12 ? 2u : 1u) || (d12 ? (!a.buffer12 || a.texture11) : (!a.texture11 || a.buffer12)))
            return false;
        const auto source = description.resourceAddress;
        const auto snapshot = d12 ? reinterpret_cast<uint64_t>(a.buffer12) : reinterpret_cast<uint64_t>(a.texture11);
        const auto& sourceRef = p.refs[m.sourceRefIndex];
        const auto& snapshotRef = p.refs[m.snapshotRefIndex];
        const auto type = d12 ? NR_RESOURCE12 : NR_TEXTURE11;
        if (!source || source == snapshot || sourceRef.borrowedIUnknownAddress != source ||
            snapshotRef.borrowedIUnknownAddress != snapshot || sourceRef.interfaceEnum != type ||
            snapshotRef.interfaceEnum != type || sourceRef.groupIndex != plan.group ||
            snapshotRef.groupIndex != plan.group || !(sourceRef.roleMask & 1) || !(snapshotRef.roleMask & 2) ||
            sourceRef.observedNonNull != 1 || snapshotRef.observedNonNull != 1 ||
            sourceRef.owningSourceLifetimeEpoch != description.resourceLifetimeEpoch ||
            snapshotRef.owningSourceLifetimeEpoch != a.lifetime || snapshotRef.physicalAllocationId != a.allocationId ||
            snapshotRef.chargedPrivateBytes != plan.layout.allocationCharge ||
            !writerRelation(p, description, m, writer))
            return false;
        for (uint64_t i = 0; i < h.roleCount; ++i) {
            const auto& r = p.roles[i];
            if ((r.role == plan.role && r.eye == plan.eye) || r.snapshotAddressObserved == snapshot ||
                r.snapshotRefIndex >= h.refCount || p.refs[r.snapshotRefIndex].physicalAllocationId == a.allocationId)
                return false;
        }
        const auto& group = p.groups[plan.group];
        const auto copyContext = d12 ? group.actualList : group.actualQueueOrContext;
        if (group.kind != plan.group || group.frameOrdinal != h.frameOrdinal || !copyContext ||
            (d12 ? (!epoch(group.recordingEpoch) || !group.actualAllocator || !p.identity.nrContextAddress ||
                    !epoch(p.identity.nrContextLifetimeEpoch))
                 : (group.actualList || group.actualAllocator || group.recordingEpoch)))
            return false;
        NrRoleV1 role{};
        NrDescriptorV1 expected{};
        std::array<NrFootprintV1, 2> footprints{};
        std::array<NrCallV1, 2> calls{};
        role.role = plan.role;
        role.eye = plan.eye;
        role.phase = m.phase;
        role.groupIndex = plan.group;
        role.sourceRefIndex = m.sourceRefIndex;
        role.snapshotRefIndex = m.snapshotRefIndex;
        role.sourceAddressObserved = source;
        role.snapshotAddressObserved = snapshot;
        role.sourceLifetimeEpoch = description.resourceLifetimeEpoch;
        role.sourceWriteEpoch = description.sourceWriteEpoch;
        role.snapshotWriteEpoch = a.writeEpoch;
        role.descriptorEpoch = description.writeEpoch;
        role.sourceOccurrence = m.sourceOccurrence;
        role.hostSerial = m.hostSerial;
        role.projectionIndex = m.projectionIndex;
        role.releasedIndex = m.releasedIndex;
        role.selectedArraySlice = m.selectedArraySlice;
        role.subresourceCount = subresources;
        role.resourceDimension = 3;
        if (d12) {
            const auto& d = plan.texture12;
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width > 16384 || d.Height > 16384 ||
                d.DepthOrArraySize != subresources || d.MipLevels != 1 || d.SampleDesc.Count != 1 ||
                d.SampleDesc.Quality || d.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
                !resourceDescription12(
                    d, source, role.sourceLifetimeEpoch, role.sourceWriteEpoch, role.descriptorEpoch, expected))
                return false;
            role.width = d.Width;
            role.height = d.Height;
            role.depthOrArray = d.DepthOrArraySize;
            role.mipLevels = d.MipLevels;
            role.format = d.Format;
            role.sampleCount = d.SampleDesc.Count;
            role.sampleQuality = d.SampleDesc.Quality;
            role.layout = d.Layout;
            role.resourceFlags = d.Flags;
            const auto& target = plan.readback12;
            if (target.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || target.Width != plan.layout.payloadBound ||
                target.Height != 1 || target.DepthOrArraySize != 1 || target.MipLevels != 1 ||
                target.Format != DXGI_FORMAT_UNKNOWN || target.SampleDesc.Count != 1 || target.SampleDesc.Quality ||
                target.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR || target.Flags)
                return false;
        } else {
            const auto& d = plan.texture11;
            SnapshotLayout actual{};
            if (d.Width > 16384 || d.Height > 16384 || d.ArraySize != subresources || d.SampleDesc.Quality ||
                !layout11(d.Width, d.Height, d.ArraySize, d.MipLevels, d.SampleDesc.Count, d.Format, actual) ||
                actual.rowBytes != plan.layout.rowBytes || actual.rowPitchBound != plan.layout.rowPitchBound ||
                actual.payloadBound != plan.layout.payloadBound ||
                actual.allocationCharge != plan.layout.allocationCharge ||
                !resourceDescription11(
                    d, source, role.sourceLifetimeEpoch, role.sourceWriteEpoch, role.descriptorEpoch, expected))
                return false;
            role.width = d.Width;
            role.height = d.Height;
            role.depthOrArray = d.ArraySize;
            role.mipLevels = d.MipLevels;
            role.format = d.Format;
            role.sampleCount = d.SampleDesc.Count;
            role.sampleQuality = d.SampleDesc.Quality;
            role.resourceUsage = d.Usage;
            role.bindFlags = d.BindFlags;
            role.cpuAccessFlags = d.CPUAccessFlags;
            role.miscFlags = d.MiscFlags;
        }
        if (std::memcmp(&expected, &description, sizeof expected) || !role.width || !role.height ||
            m.selectedArraySlice >= role.depthOrArray || m.rectXbits > INT32_MAX || m.rectYbits > INT32_MAX ||
            m.minDepthBits > UINT32_MAX || m.maxDepthBits > UINT32_MAX || m.nearZBits > UINT32_MAX ||
            m.farZBits > UINT32_MAX || m.cropXbits > INT32_MAX || m.cropYbits > INT32_MAX || !m.rectWidth ||
            !m.rectHeight || !m.cropWidth || !m.cropHeight || !range(m.rectXbits, m.rectWidth, role.width) ||
            !range(m.rectYbits, m.rectHeight, role.height) || !range(m.cropXbits, m.cropWidth, role.width) ||
            !range(m.cropYbits, m.cropHeight, role.height) || !plan.layout.payloadBound ||
            plan.layout.payloadBound > captureByteCap || plan.layout.allocationCharge < plan.layout.payloadBound ||
            plan.layout.allocationCharge > captureByteCap)
            return false;
        const auto bpp = pixelBytes(role.format);
        uint64_t rowBytes{}, end = 0, maxPitch = 0;
        if (!bpp || !multiplyBytes(role.width, bpp, rowBytes) || rowBytes != plan.layout.rowBytes ||
            footprintFirst > 128 - subresources)
            return false;
        for (uint64_t i = 0; i < subresources; ++i) {
            auto& f = footprints[i];
            f.roleIndex = h.roleCount;
            f.subresourceIndex = i;
            f.rowBytes = rowBytes;
            f.rowCount = role.height;
            f.depth = 1;
            if (d12) {
                const auto& observed = plan.footprints[i];
                const auto& shape = observed.Footprint;
                if (shape.Format != plan.texture12.Format || shape.Width != role.width || shape.Height != role.height ||
                    shape.Depth != 1 || shape.RowPitch % 256 || observed.Offset % 512 || shape.RowPitch < rowBytes ||
                    plan.rows[i] != role.height || plan.rowBytes[i] != rowBytes)
                    return false;
                f.offset = observed.Offset;
                f.rowPitch = shape.RowPitch;
            } else {
                f.offset = end;
                f.rowPitch = plan.layout.rowPitchBound;
            }
            if (f.offset < end || !multiplyBytes(f.rowPitch, f.rowCount, f.totalBytes) ||
                !range(f.offset, f.totalBytes, plan.layout.payloadBound))
                return false;
            end = f.offset + f.totalBytes;
            if (f.rowPitch > maxPitch)
                maxPitch = f.rowPitch;
        }
        if (maxPitch != plan.layout.rowPitchBound)
            return false;
        const uint64_t copyCount = d12 ? subresources : 1;
        if (callFirst > 160 - copyCount)
            return false;
        for (uint64_t i = 0; i < a.copies.size(); ++i) {
            const auto& observed = a.copies[i];
            if (i >= copyCount) {
                if (!emptyCopy(observed))
                    return false;
                continue;
            }
            if (!observed.beginObserved || !observed.endObserved || !observed.beginQpc ||
                observed.endQpc < observed.beginQpc || !observed.thread || (!i && writer.qpc > observed.beginQpc) ||
                observed.source != source || observed.destination != snapshot ||
                observed.contextOrList != copyContext || observed.sourceSubresource != (d12 ? i : 0) ||
                observed.destinationOffset != (d12 ? plan.footprints[i].Offset : 0))
                return false;
            if (i && (observed.thread != a.copies[0].thread || observed.beginQpc < a.copies[i - 1].endQpc))
                return false;
            auto& c = calls[i];
            c.kind = NR_COPY;
            c.beginObserved = c.endObserved = 1;
            c.resultType = NR_VOID;
            c.qpcBegin = observed.beginQpc;
            c.qpcEnd = observed.endQpc;
            c.thread = observed.thread;
            c.eye = plan.eye;
            c.moduleIndex = m.copyModuleIndex;
            c.deviceAddress = group.deviceAddress;
            c.queueAddress = group.actualQueueOrContext;
            c.listAddress = d12 ? group.actualList : 0;
            c.allocatorAddress = d12 ? group.actualAllocator : 0;
            c.recordingEpoch = group.recordingEpoch;
            c.contextAddress = d12 ? p.identity.nrContextAddress : copyContext;
            c.contextLifetimeEpoch = d12 ? p.identity.nrContextLifetimeEpoch : 0;
            c.actual.actualCountArgument = observed.sourceSubresource;
            c.aux.auxiliaryValue = observed.destinationOffset;
            c.observedMask = NR_CALL_OBS;
        }
        role.rectXbits = m.rectXbits;
        role.rectYbits = m.rectYbits;
        role.rectWidth = m.rectWidth;
        role.rectHeight = m.rectHeight;
        role.cropXbits = m.cropXbits;
        role.cropYbits = m.cropYbits;
        role.cropWidth = m.cropWidth;
        role.cropHeight = m.cropHeight;
        role.minDepthBits = m.minDepthBits;
        role.maxDepthBits = m.maxDepthBits;
        role.nearZBits = m.nearZBits;
        role.farZBits = m.farZBits;
        role.copyMethod = plan.copyMethod;
        role.copyListAddress = copyContext;
        role.recordingEpoch = group.recordingEpoch;
        role.descriptorFirst = h.descriptorCount;
        role.descriptorCount = 1;
        role.footprintFirst = footprintFirst;
        role.footprintCount = subresources;
        role.payloadBytes = plan.layout.payloadBound;
        role.chargedBytes = plan.layout.allocationCharge;
        role.mapPitchBound = plan.layout.rowPitchBound;
        role.observedMask = 15;
        role.aliasIndexOrAbsent = m.aliasIndexOrAbsent;
        role.callIndexOrAbsent = callFirst;
        NrWriteV1 association{};
        association.resourceAddress = source;
        association.resourceLifetime = role.sourceLifetimeEpoch;
        association.writeEpoch = role.sourceWriteEpoch;
        association.sourceOccurrence = m.sourceOccurrence;
        association.phase = writer.kind;
        association.listOrContextAddress = writer.contextOrList;
        association.recordingEpoch = writer.recordingEpoch;
        association.callOrRoleIndex = h.roleCount;
        // All failures precede this commit; only bounded local copies occur.
        p.roles[p.header.roleCount++] = role;
        p.descriptors[p.header.descriptorCount++] = description;
        std::memcpy(p.footprints + footprintFirst, footprints.data(), subresources * sizeof footprints[0]);
        p.header.footprintCount = footprintFirst + subresources;
        p.writes[p.header.writeCount++] = association;
        std::memcpy(p.calls + callFirst, calls.data(), copyCount * sizeof calls[0]);
        p.header.callCount = callFirst + copyCount;
        p.header.presentRoleMaskLo |= NR_ROLE_BIT(plan.role);
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
