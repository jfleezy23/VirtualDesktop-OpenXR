#pragma once
#include "../nr-frame-generation/include/pair_capture_nr_abi.h"
namespace virtualdesktop_openxr::nr_capture {
    // A read-only file lease keeps the bytes hashed during warmup immutable. It
    // does not own the loaded HMODULE; callers must still hold its original owner.
    struct ModuleFileLease final {
        void* handle{};
        uint64_t volume{}, fileId[2]{};
        ModuleFileLease() noexcept = default;
        ~ModuleFileLease() noexcept;
        ModuleFileLease(const ModuleFileLease&) = delete;
        ModuleFileLease& operator=(const ModuleFileLease&) = delete;
        void reset() noexcept;
        void take(ModuleFileLease&) noexcept;
    };
    // Called under the original module/feature lifetime lease, outside observer and
    // pool locks. observedLifetime is supplied by the actual load/PIN owner event;
    // this routine does not infer lifetime equality from a function address.
    // SHA-256 is Windows CNG. No borrowed COM/SDK address is dereferenced here.
    bool observePrivateModule(const void* functionAddress,
                              uint64_t kind,
                              const uint8_t expectedSha256[32],
                              bool pin,
                              NrModuleV1&,
                              NrCallV1&,
                              uint64_t observedLifetime) noexcept;
    bool observeLeasedPrivateModule(const void*,
                                    uint64_t kind,
                                    const uint8_t expectedSha256[32],
                                    bool pin,
                                    NrModuleV1&,
                                    NrCallV1&,
                                    uint64_t observedLifetime,
                                    ModuleFileLease&) noexcept;
    // No hashing or new PIN: recheck the loaded image, current path/file identity,
    // expected hash and original load lifetime against an already-held file lease.
    bool validateLeasedPrivateModule(const void*,
                                     uint64_t kind,
                                     const uint8_t expectedSha256[32],
                                     uint64_t observedLifetime,
                                     const NrModuleV1&,
                                     const ModuleFileLease&) noexcept;
    struct ManualModuleObservation {
        uint64_t moduleAddress{}, loadLifetimeEpoch{}, loadBegin{}, loadEnd{}, loadThread{};
        uint64_t publicationEpoch{}, publicationQpc{}, publicationThread{};
        uint64_t functions[5]{}; // original Init, Shutdown, Create, Evaluate, Release
        bool ownerLive{};
    };
    void observeManualModuleLoad(const void*, uint64_t begin, uint64_t end) noexcept;
    void observeManualModulePublication(const void*, const uint64_t (&functions)[5]) noexcept;
    void observeManualModuleOwnerRelease(const void*) noexcept;
    bool copyManualModuleObservation(uint64_t actualDispatch, ManualModuleObservation&) noexcept;

} // namespace virtualdesktop_openxr::nr_capture
