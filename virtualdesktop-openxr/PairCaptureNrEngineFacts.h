#pragma once
#include "PairCaptureNrPrivateApi.h"
#include <cstdio>
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    // Actual xrEndFrame entry/producer-lease facts, before either raw acquisition.
    // The original instrumentation fills a nonnull prefix and clears unused slots.
    template <class Entry>
    inline bool captureEntryEligible(const Entry& input, uint64_t minimumTime, uint64_t currentThread) noexcept {
        if (!minimumTime || !input.displayTime || input.displayTime < minimumTime || !input.entryQpc ||
            input.leaseQpc <= input.entryQpc || !currentThread || !input.frameThread ||
            input.frameThread != currentThread || !input.stackCount || input.stackCount > 48)
            return false;
        for (uint64_t i = 0; i < 48; ++i)
            if (i < input.stackCount ? !input.stack[i] : input.stack[i] != 0)
                return false;
        return true;
    }
    // The actual receiver's pre-adoption rejection shape. A contradictory ACK or
    // any commit/adoption field cannot prove that ownership was not acquired.
    inline bool definitiveTransferRejection(uint32_t returned, const NrAckV1& ack) noexcept {
        return returned == NR_REJECTED && ack.size == sizeof ack && ack.version == 1 && ack.status == NR_REJECTED &&
               !ack.requestAttempt && !ack.frameOrdinal && !ack.transferId && !ack.receiverCommitEpoch &&
               !ack.roleCount && !ack.refCount && !ack.groupCount && !ack.packetDigestLo && !ack.packetDigestHi &&
               !ack.acceptedBudgetBytes && !ack.observedFlags && !ack.reserved;
    }
    inline bool hostStateMayPublishWarm(uint64_t state) noexcept {
        return state == NR_HOST_UNARMED || state == NR_HOST_WARM;
    }
    inline bool selectedWarmInvalidation(uint64_t state) noexcept {
        return state >= NR_HOST_ARMED && state <= NR_HOST_TRANSFERRED;
    }
    inline bool selectedReceiverDrift(uint64_t state) noexcept {
        return state >= NR_HOST_ARMED && state <= NR_HOST_SETTLED;
    }

    // Exact original six-field arm file. Context lifetime is independently bound by
    // Ready and the budget query; it is not added to or inferred from this line.
    inline bool exactCaptureArm(const char* bytes, size_t count, const NrReadyV1& r) noexcept {
        if (!bytes || !count || count > 511 || r.size != sizeof r || r.version != 1 || r.status != 1 ||
            r.observedFlags != 1 || !r.attempt || !(r.nonceLo | r.nonceHi) || !r.pid || !r.creationFiletime ||
            !(r.sessionNonceLo | r.sessionNonceHi) || !r.fgContextAddress || !r.fgContextLifetimeEpoch ||
            !r.fgHistoryGeneration)
            return false;
        for (auto value : r.reserved)
            if (value)
                return false;
        char expected[512]{};
        const int size = std::snprintf(expected,
                                       sizeof expected,
                                       "%016llx%016llx %llu %llu %016llx%016llx %llu %llu\n",
                                       static_cast<unsigned long long>(r.nonceLo),
                                       static_cast<unsigned long long>(r.nonceHi),
                                       static_cast<unsigned long long>(r.pid),
                                       static_cast<unsigned long long>(r.creationFiletime),
                                       static_cast<unsigned long long>(r.sessionNonceLo),
                                       static_cast<unsigned long long>(r.sessionNonceHi),
                                       static_cast<unsigned long long>(r.fgContextAddress),
                                       static_cast<unsigned long long>(r.fgHistoryGeneration));
        return size > 0 && static_cast<size_t>(size) < sizeof expected && count == static_cast<size_t>(size) &&
               std::memcmp(bytes, expected, count) == 0;
    }
    inline bool privateExportInModule(uint64_t address, const NrModuleV1& module) noexcept {
        return address && module.baseObserved && module.imageSize && module.pinObserved == 1 && module.pinResult == 1 &&
               module.imageSize <= UINT64_MAX - module.baseObserved && address >= module.baseObserved &&
               address - module.baseObserved < module.imageSize;
    }
    // Exact immutable row passed in the completed owner acquisition; no pointer probe.
    inline bool matchingAcquiredSnapshot(const NrRefV1* refs,
                                         uint32_t count,
                                         uint64_t pointer,
                                         uint64_t lifetime,
                                         uint64_t allocation,
                                         uint64_t bytes,
                                         uint64_t group,
                                         uint64_t expectedType = NR_TEXTURE11) noexcept {
        if (!refs || !pointer || !lifetime || !allocation || !bytes || count > 89 ||
            (expectedType != NR_TEXTURE11 && expectedType != NR_RESOURCE12))
            return false;
        uint32_t matches = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = refs[i];
            if (r.borrowedIUnknownAddress != pointer)
                continue;
            if (r.interfaceEnum != expectedType || r.roleMask != 2 || r.groupIndex != group ||
                r.owningSourceLifetimeEpoch != lifetime || r.physicalAllocationId != allocation ||
                r.chargedPrivateBytes != bytes || r.observedNonNull != 1)
                return false;
            ++matches;
        }
        return matches == 1;
    }

    inline bool heldSourceIdentity(
        const NrRefV1* refs, uint32_t count, uint64_t pointer, uint64_t life, uint64_t type, uint64_t group) noexcept {
        if (!refs || count > 89 || !pointer || !life || (type != NR_TEXTURE11 && type != NR_RESOURCE12))
            return false;
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = refs[i];
            if (r.borrowedIUnknownAddress != pointer || r.interfaceEnum != type || r.groupIndex != group ||
                r.roleMask != 1)
                continue;
            if (r.owningSourceLifetimeEpoch != life || r.observedNonNull != 1)
                return false;
            found = true;
        }
        return found;
    }

    // Original subImage.imageArrayIndex is distinct from the released buffer index.
    // OpenXR projection images select mip zero; actual texture MipLevels determines
    // the D3D subresource. Numeric matching performs no pointer dereference.
    inline bool selectRawSourceCopy(const NrSourceWitnessV1& source,
                                    uint64_t target,
                                    uint64_t slice,
                                    uint64_t arraySize,
                                    uint64_t mipLevels,
                                    uint32_t& selected) noexcept {
        selected = UINT32_MAX;
        if (!target || !arraySize || arraySize > 2 || slice >= arraySize || !mipLevels || mipLevels > UINT32_MAX ||
            slice > UINT32_MAX / mipLevels)
            return false;
        const uint64_t subresource = slice * mipLevels;
        for (uint32_t i = 0; i < 4; ++i) {
            const auto& copy = source.copies[i];
            if (copy.destinationAddress != target || copy.destinationSubresource != subresource)
                continue;
            if (selected != UINT32_MAX) {
                selected = UINT32_MAX;
                return false;
            }
            selected = i;
        }
        return selected != UINT32_MAX;
    }

} // namespace virtualdesktop_openxr::nr_capture
