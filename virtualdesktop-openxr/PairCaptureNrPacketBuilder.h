#pragma once
#include "PairCaptureNrSnapshots.h"
#include "PairCaptureNrWrites.h"
namespace virtualdesktop_openxr::nr_capture {
    // Original geometry and caller observations, never inferred from allocation size.
    // All numeric addresses remain observations; these helpers acquire no references.
    struct SnapshotRoleMetadata {
        uint64_t phase{}, hostSerial{}, sourceOccurrence{}, projectionIndex{}, releasedIndex{}, selectedArraySlice{};
        uint64_t rectXbits{}, rectYbits{}, rectWidth{}, rectHeight{};
        uint64_t cropXbits{}, cropYbits{}, cropWidth{}, cropHeight{};
        // Raw 32-bit encodings, including observed infinite/reversed depth ranges.
        uint64_t minDepthBits{}, maxDepthBits{}, nearZBits{}, farZBits{};
        uint64_t sourceRefIndex{}, snapshotRefIndex{}, copyModuleIndex{};
        uint64_t aliasIndexOrAbsent{NR_ABSENT};
        // Ordered storage-alias relation from actual writer endpoint to role source.
        // Endpoint address, lifetime and write epoch must match at EVERY hop.
        std::array<uint64_t, 2> writerAliasIndices{NR_ABSENT, NR_ABSENT};
        uint64_t writerAliasCount{};
        bool geometryObserved{};
    };
    // Packet references/groups/modules and any aliases are populated by the real
    // owner before this call. The allocation must already be adopted and copied.
    // Appends one role, actual GetDesc, footprints, writer association and actual
    // copy-call records, or leaves EVERY packet byte unchanged on failure.
    // Writer/copy QPC observations share QueryPerformanceCounter's domain; the
    // writer must precede or coincide with the first diagnostic copy call. This
    // records CPU observation order, never GPU completion. Writer serial remains
    // caller-domain evidence (public submission or host serial), not host equality.
    // A role-associated write reached via shared storage is not a claim that the
    // producer Copy/SDK called the role's interface. Source640/SDK/API observers and
    // the complete offline graph retain that actual writer endpoint independently.
    bool appendSnapshotRole(NrPacketV1&,
                            const SnapshotAllocation&,
                            const SnapshotRoleMetadata&,
                            const ResourceWriteFact&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
