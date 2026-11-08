#pragma once
#include "pair_capture_nr_abi.h"
#include <cstring>
// Both records must first pass their shared C validator. All host-origin bytes
// remain equal except the explicitly separate completion, admission and Map
// observations below. Additional FG rows never replace a host row.
inline bool hostExtension(const NrPacketV1& host, const NrPacketV1& final) noexcept {
    if (host.header.flags || final.header.flags != 1)
        return false;
    auto header = final.header;
    header.flags = host.header.flags;
#define NR_APPEND_COUNT(name)                                                                                          \
    if (final.header.name < host.header.name)                                                                          \
        return false;                                                                                                  \
    header.name = host.header.name
    NR_APPEND_COUNT(roleCount);
    NR_APPEND_COUNT(refCount);
    NR_APPEND_COUNT(callCount);
    NR_APPEND_COUNT(descriptorCount);
    NR_APPEND_COUNT(drawCount);
    NR_APPEND_COUNT(aliasCount);
    NR_APPEND_COUNT(writeCount);
    NR_APPEND_COUNT(footprintCount);
    NR_APPEND_COUNT(constantsUsedBytes);
    NR_APPEND_COUNT(moduleCount);
#undef NR_APPEND_COUNT
    if ((final.header.requiredRoleMaskLo & host.header.requiredRoleMaskLo) != host.header.requiredRoleMaskLo ||
        (final.header.presentRoleMaskLo & host.header.presentRoleMaskLo) != host.header.presentRoleMaskLo)
        return false;
    header.requiredRoleMaskLo = host.header.requiredRoleMaskLo;
    header.presentRoleMaskLo = host.header.presentRoleMaskLo;
    if (std::memcmp(&header, &host.header, sizeof header) ||
        std::memcmp(&final.budget, &host.budget, sizeof host.budget))
        return false;
    auto identity = final.identity;
    identity.ticketGeneration = host.identity.ticketGeneration;
    identity.previousSerial = host.identity.previousSerial;
    identity.currentSerial = host.identity.currentSerial;
    identity.observedIdentityMask = host.identity.observedIdentityMask;
    if (std::memcmp(&identity, &host.identity, sizeof identity))
        return false;
#define NR_SAME_PREFIX(table, count)                                                                                   \
    if (std::memcmp(host.table, final.table, static_cast<size_t>(host.header.count) * sizeof host.table[0]))           \
    return false
    NR_SAME_PREFIX(refs, refCount);
    NR_SAME_PREFIX(params, paramCount);
    NR_SAME_PREFIX(calls, callCount);
    NR_SAME_PREFIX(descriptors, descriptorCount);
    NR_SAME_PREFIX(draws, drawCount);
    NR_SAME_PREFIX(aliases, aliasCount);
    NR_SAME_PREFIX(writes, writeCount);
    NR_SAME_PREFIX(constants, constantsUsedBytes);
    NR_SAME_PREFIX(modules, moduleCount);
#undef NR_SAME_PREFIX
    for (uint64_t i = 0; i < host.header.groupCount; ++i) {
        auto group = final.groups[i];
        group.completedObserved = host.groups[i].completedObserved;
        group.completedValue = host.groups[i].completedValue;
        if (std::memcmp(&group, &host.groups[i], sizeof group))
            return false;
    }
    for (uint64_t i = 0; i < host.header.roleCount; ++i) {
        auto role = final.roles[i];
        if (host.refs[host.roles[i].snapshotRefIndex].interfaceEnum == NR_TEXTURE11)
            role.payloadBytes = host.roles[i].payloadBytes;
        if (std::memcmp(&role, &host.roles[i], sizeof role))
            return false;
    }
    for (uint64_t i = 0; i < host.header.footprintCount; ++i) {
        auto footprint = final.footprints[i];
        const auto& original = host.footprints[i];
        const auto& role = host.roles[original.roleIndex];
        if (host.refs[role.snapshotRefIndex].interfaceEnum == NR_TEXTURE11) {
            footprint.rowPitch = original.rowPitch;
            footprint.totalBytes = original.totalBytes;
        }
        if (std::memcmp(&footprint, &original, sizeof footprint))
            return false;
    }
    return true;
}
