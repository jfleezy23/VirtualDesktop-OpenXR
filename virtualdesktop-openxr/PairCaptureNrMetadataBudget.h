#pragma once
#include <cstddef>
namespace virtualdesktop_openxr::nr_capture {
    // One shared 8MiB diagnostic CPU reservation: host5MiB + receiver3MiB.
    // These are conservative bounded charges, not measured process working set.
    inline constexpr size_t hostMetadataEngine = 2 * 1024 * 1024;
    inline constexpr size_t hostMetadataJournals = 2 * 1024 * 1024;
    inline constexpr size_t hostMetadataStack = 1 * 1024 * 1024;
    inline constexpr size_t hostMetadataTotal = hostMetadataEngine + hostMetadataJournals + hostMetadataStack;
    inline constexpr size_t hostMetadataSdk = 384 * 1024;
    inline constexpr size_t hostMetadataPostFx = 384 * 1024;
    inline constexpr size_t hostMetadataPipeline = 384 * 1024;
    inline constexpr size_t hostMetadataBank = 128 * 1024;
    inline constexpr size_t hostMetadataSource = 128 * 1024;
    inline constexpr size_t hostMetadataWarm = 64 * 1024;
    inline constexpr size_t hostMetadataFence = 128 * 1024;
    inline constexpr size_t hostMetadataModule = 16 * 1024;
    inline constexpr size_t hostMetadataControl = 128 * 1024;
    inline constexpr size_t hostMetadataWrites = 64 * 1024;
    inline constexpr size_t hostMetadataMisc = 240 * 1024;
    static_assert(hostMetadataSdk + hostMetadataPostFx + hostMetadataPipeline + hostMetadataBank + hostMetadataSource +
                      hostMetadataWarm + hostMetadataFence + hostMetadataModule + hostMetadataControl +
                      hostMetadataWrites + hostMetadataMisc ==
                  hostMetadataJournals);
    static_assert(hostMetadataTotal == 5 * 1024 * 1024);
    // Engine includes its SnapshotStore, SDK snapshot, future preallocated packets,
    // and packet projection workspace. Misc covers fixed counters/TLS/request state.
    // Private stack/scratch charge includes nested scalar rows, module hashing's
    // fixed64KiB file buffer+UTF16 path, packet/hash scratch and negotiation packet.
    // Final call-chain audit must stay below this reserved1MiB before arming.
} // namespace virtualdesktop_openxr::nr_capture
