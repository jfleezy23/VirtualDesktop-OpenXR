#pragma once
#include "PairCaptureNrModules.h"
#include "../nr-frame-generation/include/pair_capture_nr_control.h"
#include <array>
namespace virtualdesktop_openxr::nr_capture {
    struct SdkDispatchBinding;
    struct ModuleExpectation {
        uint64_t kind{}, loadLifetimeEpoch{}, observedAddress{};
        std::array<uint8_t, 32> expectedSha256{};
    };
    struct SdkModuleExpectations {
        ModuleExpectation dispatch{}, evaluate{}, release{};
    };
    // Memory-only helpers. Expectations come from the actual owner/load/PIN event,
    // never an NR context epoch or an address-derived synthetic lifetime. Slots
    // 0/1/2 are host/FG/actual NR provider (kinds 1/2/3). Later kinds are explicit
    // caller labels; no constant SDK dispatch index is assumed.
    // A successful append or match changes only derived wire placement. Every
    // failure leaves the complete packet and any binding output unchanged.
    bool appendModuleObservation(NrPacketV1&,
                                 const NrModuleV1&,
                                 const NrCallV1* actualPin,
                                 const ModuleExpectation&) noexcept;
    bool matchPacketModule(const NrPacketV1&, const ModuleExpectation&, uint64_t expectedIndex) noexcept;
    bool bindSdkModules(const NrPacketV1&, const NrCallV1&, const SdkModuleExpectations&, SdkDispatchBinding&) noexcept;
    bool matchSourceModule(const NrPacketV1&,
                           const NrSourceWitnessV1&,
                           const ModuleExpectation&,
                           uint64_t expectedIndex) noexcept;
    // Call ONLY under the original module/feature lease, outside observer locks.
    // Delegates to the existing actual Win32/CNG observer, with no runtime load or
    // SDK/COM/GPU call. Optional PIN is process-lifetime and cannot be rolled back;
    // failed actual PIN observations remain available in the returned outputs.
    bool observePacketModule(
        const void* actualFunction, const ModuleExpectation&, bool pin, NrModuleV1&, NrCallV1&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
