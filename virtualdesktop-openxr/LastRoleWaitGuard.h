#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

// The worker certifies the fence, destination and ticket identities before evaluating this decision.
struct VdxrLastRoleWaitFactsV1 {
    uint32_t structSize, abiVersion, multiplier, selectedRole;
    uint64_t ownerGeneration, ticketGeneration, ticketToken, serial;
    uint64_t selectedValue, realValue, completedValue;
    uint32_t bank, flags;
};
static_assert(sizeof(VdxrLastRoleWaitFactsV1) == 80);
static_assert(alignof(VdxrLastRoleWaitFactsV1) == 8);
static_assert(offsetof(VdxrLastRoleWaitFactsV1, ownerGeneration) == 16);
static_assert(offsetof(VdxrLastRoleWaitFactsV1, bank) == 72);

namespace virtualdesktop_openxr::fg_live {
    enum GuardFlag : uint32_t {
        identitiesValidated = 1u << 0,
        independentDelivery = 1u << 1,
        admissionOpened = 1u << 2,
        fullOutputObserved = 1u << 3,
        requiredCopiesStaged = 1u << 4,
        nativeCycleOpen = 1u << 5,
        depthOrderingCertified = 1u << 6,
        graphHealthy = 1u << 7,
        originalWaitRequested = 1u << 8
    };
    constexpr uint32_t requiredGuardFlags = (1u << 9) - 1;

    inline uint32_t authorizeCpp(const VdxrLastRoleWaitFactsV1& f) noexcept {
        const uint32_t lastRole = f.multiplier == 2 ? 0u : f.multiplier == 3 ? 1u : 3u;
        return f.structSize == sizeof(f) && f.abiVersion == 1 && f.multiplier >= 2 && f.multiplier <= 4 &&
               f.selectedRole == lastRole && f.ownerGeneration && f.ticketGeneration && f.ticketToken && f.serial &&
               f.bank < 2 && f.flags == requiredGuardFlags && f.realValue && f.selectedValue > f.realValue &&
               f.completedValue != UINT64_MAX && f.completedValue >= f.realValue && f.completedValue < f.selectedValue;
    }
} // namespace virtualdesktop_openxr::fg_live
