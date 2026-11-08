#pragma once
#include "PairCaptureNrPacketBuilder.h"
#include "PairCaptureNrModulePacket.h"
#include "PairCaptureNrSdkPacket.h"
#include "PairCaptureNrPipelineFacts.h"
#include "PairCaptureNrPostFx.h"
namespace virtualdesktop_openxr::nr_capture {
    template <class T>
    struct EnginePacketSpan {
        const T* data{};
        uint64_t count{};
    };
    struct EnginePacketRoleInput {
        const SnapshotAllocation* allocation{};
        const SnapshotRoleMetadata* metadata{};
        const ResourceWriteFact* writer{};
    };
    struct EnginePacketModuleInput {
        const NrModuleV1* module{};
        const NrCallV1* actualPin{};
        ModuleExpectation expectation{};
    };
    struct EnginePacketSdkInput {
        const SdkObservation* observation{};
        SdkDispatchBinding binding{};
    };
    struct EnginePacketDrawInput {
        const NrDrawV1* draw{};
        EnginePacketSpan<uint8_t> shaderSha256{};
        EnginePacketSpan<uint8_t> constants{};
    };
    struct EnginePacketCohort {
        const HostRequest* request{};
        const NrReadyV1* ready{};
        const NrFgReadbackBudgetV1* fgBudget{};
        const NrSourceWitnessV1* source{};
        const NrPostFxSidecarV1* postFx{};
        const ModuleExpectation* sourceModule{}; // actual kind5/index4 full-file binding
    };
    struct EnginePacketInput {
        // Caller-observed header/identity/budget. All header placement counts,
        // constantsUsedBytes and presentRoleMask fields must be zero initially.
        const NrHeaderV1* header{};
        const NrIdentityV1* identity{};
        const NrBudgetV1* budget{};
        EnginePacketCohort cohort{};
        EnginePacketSpan<NrRefV1> refs{};                    // <=89 actual acquired refs
        EnginePacketSpan<NrGroupV1> groups{};                // exactly3 actual owner rows
        EnginePacketSpan<EnginePacketModuleInput> modules{}; // ordered, <=8
        // Copied first, preserving alias export/import indices relative to this
        // exact span. Excludes PIN, SDK and diagnostic-copy calls appended by the
        // other supplied observations; never duplicates those actual API calls.
        EnginePacketSpan<NrCallV1> controlCalls{};
        EnginePacketSpan<NrAliasV1> aliases{};
        EnginePacketSpan<EnginePacketSdkInput> sdk{};
        EnginePacketSpan<EnginePacketRoleInput> roles{}; // <=28; original geometry/slice
        // Immutable caller-projected actual descriptor dictionary. Draw descriptor
        // ranges are relative to this span; heap slots/handles/stamps are untouched.
        // Host total (role GetDesc + this dictionary) must leave14 of64 FG slots.
        EnginePacketSpan<NrDescriptorV1> drawDescriptors{};
        EnginePacketSpan<EnginePacketDrawInput> draws{}; // host kinds1..4, <=8
        const PipelineProgramFacts* pipelineProgram{};
    };
    // The caller supplies immutable, valid, disjoint input storage and a preallocated
    // entirely-zero packet. Nonzero destinations reject unchanged; every build
    // failure restores the fresh destination to all zero. No packet-sized stack
    // copy, ownership action, pointer probe, observer callback or API work occurs.
    // Only placement counts/indices/constants offsets are derived. All actual
    // observations remain caller facts. Success includes the full scalar gates;
    // it is not an exposure, AddRef, deep ACK or proof of GPU execution.
    bool assembleEnginePacket(NrPacketV1& fresh, const EnginePacketInput&, NrError&) noexcept;
    bool
    validateEnginePacket(const NrPacketV1&, const EnginePacketCohort&, const PipelineProgramFacts&, NrError&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
