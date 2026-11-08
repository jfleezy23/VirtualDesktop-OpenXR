#pragma once
#include "PairCaptureNrViewDescriptions.h"
#include <array>
#include "NrOwnershipViews.h"
#include "PairCaptureNrWrites.h"
namespace virtualdesktop_openxr::nr_capture {
    // Original descriptor creation/write facts only. GPU handle, use bind and
    // source-write epochs are deliberately unfilled until an actual draw/use join.
    struct PipelineDescriptorCreations {
        std::array<NrDescriptorV1, 13> rows{};
        uint32_t present{};
        bool invalid{};
    };
    struct PipelineProgramFacts {
        uint64_t rootAddress{}, rootLifetime{}, computeAddress{}, computeLifetime{}, graphicsAddress{},
            graphicsLifetime{};
        std::array<uint8_t, 32> rootHash{}, computeHash{}, vertexHash{}, pixelHash{};
    };
    struct PipelineProgramHandle {
        uint32_t slot{};
        bool invalid{};
    };
    void pipelineRootCreated(PipelineProgramHandle&, ID3D12RootSignature*, const void*, size_t) noexcept;
    void pipelineComputeCreated(PipelineProgramHandle&, ID3D12PipelineState*, D3D12_SHADER_BYTECODE) noexcept;
    void pipelineGraphicsCreated(PipelineProgramHandle&,
                                 ID3D12PipelineState*,
                                 D3D12_SHADER_BYTECODE,
                                 D3D12_SHADER_BYTECODE) noexcept;
    bool pipelineProgramCopy(const PipelineProgramHandle&, bool native, PipelineProgramFacts&) noexcept;
    struct PipelineDrawFacts {
        uint64_t serial{}, commandObservation{}, targetCpu{}, targetWriteBefore{};
        NrDrawV1 draw{};
        vdxrfg::pair_capture::nr::NrRecordingOwnership recording{};
        std::array<NrDescriptorV1, 7> descriptors{};
        std::array<ResourceWriteFact, 6> sourceAtBind{};
        std::array<ResourceWriteFact, 2> outputs{};
        std::array<uint8_t, 80> constants{};
        std::array<uint8_t, 32> shaderHash{};
        uint32_t shaderReadMask{}, outputCount{};
        bool bound{}, recorded{}, restored{};
    };
    inline uint32_t pipelineShaderReadMask(uint32_t kind, bool native, bool depth) noexcept {
        return kind == 1 ? (1u | (depth ? 2u : 0u)) : kind == 2 ? (native ? 9u : 13u) : 0u;
    }
    inline bool pipelineOutputFresh(const ResourceWriteFact& write,
                                    const PipelineDrawFacts& row,
                                    size_t slot) noexcept {
        if (slot != 4 && slot != 5 && slot != 6)
            return false;
        const auto& descriptor = row.descriptors[slot];
        return descriptor.resourceAddress && write.resource == descriptor.resourceAddress &&
               write.lifetime == descriptor.resourceLifetimeEpoch && write.contextOrList == row.draw.listOrContext &&
               write.serial == row.serial && write.kind == (row.draw.kind == 1 ? NR_DISPATCH : NR_DRAW) &&
               write.recordingEpoch == row.draw.recordingEpoch &&
               write.sequence > (slot == 6 ? row.targetWriteBefore : row.sourceAtBind[slot].sequence);
    }
    // Fixed process journal: an original cached Batch carries only this scalar slot.
    // Slots are never reused within the child; exhaustion rejects diagnostics.
    struct PipelineDescriptorHandle {
        uint32_t slot{};
        bool invalid{};
    };
    bool pipelineDescriptorCopy(const PipelineDescriptorHandle&, PipelineDescriptorCreations&) noexcept;
    inline bool pipelineCreationIdentity(NrDescriptorV1& row,
                                         uint64_t heap,
                                         uint64_t slot,
                                         uint64_t cpu,
                                         uint64_t increment,
                                         uint64_t resource,
                                         uint64_t resourceLife,
                                         uint64_t observedWrite) noexcept {
        if (!heap || !cpu || !observedWrite || observedWrite == UINT64_MAX || (!!resource != !!resourceLife) ||
            resourceLife == UINT64_MAX ||
            (row.kind != NR_DESC_SRV && row.kind != NR_DESC_UAV && row.kind != NR_DESC_RTV) ||
            (row.kind == NR_DESC_SRV && (row.descriptionKind != NR_D12_SRV || row.apiDescriptionBytes != 40)) ||
            (row.kind == NR_DESC_UAV && (row.descriptionKind != NR_D12_UAV || row.apiDescriptionBytes != 40)) ||
            (row.kind == NR_DESC_RTV && (row.descriptionKind != NR_D12_RTV || row.apiDescriptionBytes != 24)) ||
            (row.kind == NR_DESC_RTV ? (slot != 0 || increment != 0) : (slot >= 12 || !increment))) {
            row = {};
            return false;
        }
        row.objectAddress = cpu;
        row.objectLifetimeEpoch = observedWrite;
        row.heapAddress = heap;
        row.heapSlot = slot;
        row.cpuHandle = cpu;
        row.descriptorIncrement = increment;
        row.resourceAddress = resource;
        row.resourceLifetimeEpoch = resourceLife;
        row.writeEpoch = observedWrite;
        row.sourceWriteEpoch = row.bindEpoch = row.gpuHandle = row.observedMask = 0;
        return true;
    }
    void pipelineDescriptorCreated(PipelineDescriptorHandle&,
                                   uint32_t index,
                                   ID3D12DescriptorHeap*,
                                   D3D12_CPU_DESCRIPTOR_HANDLE,
                                   uint32_t increment,
                                   ID3D12Resource*,
                                   const D3D12_SHADER_RESOURCE_VIEW_DESC&) noexcept;
    void pipelineDescriptorCreated(PipelineDescriptorHandle&,
                                   uint32_t index,
                                   ID3D12DescriptorHeap*,
                                   D3D12_CPU_DESCRIPTOR_HANDLE,
                                   uint32_t increment,
                                   ID3D12Resource*,
                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC&) noexcept;
    void pipelineDescriptorCreated(PipelineDescriptorHandle&,
                                   uint32_t index,
                                   ID3D12DescriptorHeap*,
                                   D3D12_CPU_DESCRIPTOR_HANDLE,
                                   uint32_t increment,
                                   ID3D12Resource*,
                                   const D3D12_RENDER_TARGET_VIEW_DESC&) noexcept;
    bool pipelineObservationEnabled() noexcept;
    void pipelineDrawTable(PipelineDrawFacts&,
                           const PipelineDescriptorHandle&,
                           const PipelineProgramHandle&,
                           const vdxrfg::pair_capture::nr::NrPipelineOwnership&,
                           ID3D12GraphicsCommandList*,
                           uint32_t kind,
                           uint32_t eye,
                           uint64_t gpuTable,
                           const void* constants,
                           size_t count) noexcept;
    void pipelineDispatchRecorded(PipelineDrawFacts&, uint32_t x, uint32_t y, uint32_t z) noexcept;
    void pipelineDrawRecorded(PipelineDrawFacts&,
                              const D3D12_VIEWPORT&,
                              const D3D12_RECT&,
                              uint64_t actualTarget,
                              uint32_t vertices,
                              uint32_t instances,
                              uint32_t startVertex,
                              uint32_t startInstance) noexcept;
    void pipelinePhaseRestored(ID3D12GraphicsCommandList*, uint32_t kind) noexcept;
    bool copyPipelineDraw(uint64_t serial, uint32_t kind, uint32_t eye, PipelineDrawFacts&) noexcept;

} // namespace virtualdesktop_openxr::nr_capture
