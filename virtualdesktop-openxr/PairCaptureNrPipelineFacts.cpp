#include "PairCaptureNrPipelineFacts.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrSources.h"
#include "NrOwnershipViews.h"
#include <mutex>
#include <bcrypt.h>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        std::atomic<uint64_t> nextDescriptorWrite{1};
        struct Journal {
            std::mutex mutex;
            std::array<PipelineDescriptorCreations, 64> slots{};
            std::array<PipelineProgramFacts, 64> programs{};
            struct Frame {
                uint64_t serial{};
                std::array<PipelineDrawFacts, 4> draws{};
                bool invalid{};
            };
            std::array<Frame, 8> frames{};
            uint32_t used{}, programUsed{};
        } journal;
        static_assert(sizeof(Journal) <= hostMetadataPipeline);
        static_assert(sizeof(Journal) < 384 * 1024);
        std::atomic<uint64_t> nextDrawBind{1}, nextCommandObservation{1};
        std::atomic<uint64_t> nextProgramLifetime{1};
        bool hashProgram(const void* bytes, size_t count, std::array<uint8_t, 32>& hash) noexcept {
            hash = {};
            return bytes && count && count <= 1048576 &&
                   BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                              nullptr,
                              0,
                              const_cast<PUCHAR>(static_cast<const unsigned char*>(bytes)),
                              static_cast<ULONG>(count),
                              hash.data(),
                              static_cast<ULONG>(hash.size())) >= 0;
        }
        template <class Description>
        void observed(PipelineDescriptorHandle& handleSlot,
                      uint32_t index,
                      ID3D12DescriptorHeap* heap,
                      D3D12_CPU_DESCRIPTOR_HANDLE handle,
                      uint32_t increment,
                      ID3D12Resource* resource,
                      const Description& description) noexcept {
            if (!hostRequest().valid)
                return;
            uint64_t epoch{};
            const auto life = resource ? sdkObservedResourceLifetime(resource) : 0;
            NrDescriptorV1 row{};
            if (index >= 13 || handleSlot.invalid || !viewDescription(description, row) ||
                !vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextDescriptorWrite, epoch) ||
                !pipelineCreationIdentity(row,
                                          reinterpret_cast<uint64_t>(heap),
                                          index == 12 ? 0 : index,
                                          handle.ptr,
                                          increment,
                                          reinterpret_cast<uint64_t>(resource),
                                          life,
                                          epoch)) {
                handleSlot.invalid = true;
                return;
            }
            std::lock_guard lock(journal.mutex);
            if (!handleSlot.slot) {
                if (journal.used == journal.slots.size()) {
                    handleSlot.invalid = true;
                    return;
                }
                handleSlot.slot = ++journal.used;
            }
            if (handleSlot.slot > journal.used) {
                handleSlot.invalid = true;
                return;
            }
            auto& facts = journal.slots[handleSlot.slot - 1];
            facts.rows[index] = row;
            facts.present |= uint32_t(1) << index;
        }
    } // namespace
    bool pipelineDescriptorCopy(const PipelineDescriptorHandle& handle, PipelineDescriptorCreations& out) noexcept {
        out = {};
        if (!handle.slot || handle.invalid)
            return false;
        std::lock_guard lock(journal.mutex);
        if (handle.slot > journal.used || journal.slots[handle.slot - 1].present != 0x1fff)
            return false;
        out = journal.slots[handle.slot - 1];
        return true;
    }
    void pipelineDescriptorCreated(PipelineDescriptorHandle& facts,
                                   uint32_t index,
                                   ID3D12DescriptorHeap* heap,
                                   D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                   uint32_t increment,
                                   ID3D12Resource* resource,
                                   const D3D12_SHADER_RESOURCE_VIEW_DESC& description) noexcept {
        observed(facts, index, heap, handle, increment, resource, description);
    }
    void pipelineDescriptorCreated(PipelineDescriptorHandle& facts,
                                   uint32_t index,
                                   ID3D12DescriptorHeap* heap,
                                   D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                   uint32_t increment,
                                   ID3D12Resource* resource,
                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC& description) noexcept {
        observed(facts, index, heap, handle, increment, resource, description);
    }
    void pipelineDescriptorCreated(PipelineDescriptorHandle& facts,
                                   uint32_t index,
                                   ID3D12DescriptorHeap* heap,
                                   D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                   uint32_t increment,
                                   ID3D12Resource* resource,
                                   const D3D12_RENDER_TARGET_VIEW_DESC& description) noexcept {
        observed(facts, index, heap, handle, increment, resource, description);
    }
    void pipelineRootCreated(PipelineProgramHandle& handle,
                             ID3D12RootSignature* root,
                             const void* bytes,
                             size_t count) noexcept {
        if (!hostRequest().valid)
            return;
        PipelineProgramFacts facts{};
        if (handle.slot || handle.invalid || !root || !hashProgram(bytes, count, facts.rootHash) ||
            !vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextProgramLifetime, facts.rootLifetime)) {
            handle.invalid = true;
            return;
        }
        facts.rootAddress = reinterpret_cast<uint64_t>(root);
        std::lock_guard lock(journal.mutex);
        if (journal.programUsed == journal.programs.size()) {
            handle.invalid = true;
            return;
        }
        handle.slot = ++journal.programUsed;
        journal.programs[handle.slot - 1] = facts;
    }
    void pipelineComputeCreated(PipelineProgramHandle& handle,
                                ID3D12PipelineState* pso,
                                D3D12_SHADER_BYTECODE code) noexcept {
        if (!hostRequest().valid)
            return;
        uint64_t lifetime{};
        std::array<uint8_t, 32> hash{};
        if (handle.invalid || !handle.slot || !pso || !hashProgram(code.pShaderBytecode, code.BytecodeLength, hash) ||
            !vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextProgramLifetime, lifetime)) {
            handle.invalid = true;
            return;
        }
        std::lock_guard lock(journal.mutex);
        if (handle.slot > journal.programUsed || journal.programs[handle.slot - 1].computeAddress) {
            handle.invalid = true;
            return;
        }
        auto& facts = journal.programs[handle.slot - 1];
        facts.computeAddress = reinterpret_cast<uint64_t>(pso);
        facts.computeLifetime = lifetime;
        facts.computeHash = hash;
    }
    void pipelineGraphicsCreated(PipelineProgramHandle& handle,
                                 ID3D12PipelineState* pso,
                                 D3D12_SHADER_BYTECODE vertex,
                                 D3D12_SHADER_BYTECODE pixel) noexcept {
        if (!hostRequest().valid)
            return;
        uint64_t lifetime{};
        std::array<uint8_t, 32> vs{}, ps{};
        if (handle.invalid || !handle.slot || !pso || !hashProgram(vertex.pShaderBytecode, vertex.BytecodeLength, vs) ||
            !hashProgram(pixel.pShaderBytecode, pixel.BytecodeLength, ps) ||
            !vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextProgramLifetime, lifetime)) {
            handle.invalid = true;
            return;
        }
        std::lock_guard lock(journal.mutex);
        if (handle.slot > journal.programUsed || journal.programs[handle.slot - 1].graphicsAddress) {
            handle.invalid = true;
            return;
        }
        auto& facts = journal.programs[handle.slot - 1];
        facts.graphicsAddress = reinterpret_cast<uint64_t>(pso);
        facts.graphicsLifetime = lifetime;
        facts.vertexHash = vs;
        facts.pixelHash = ps;
    }
    bool pipelineProgramCopy(const PipelineProgramHandle& handle, bool native, PipelineProgramFacts& out) noexcept {
        out = {};
        if (!handle.slot || handle.invalid)
            return false;
        std::lock_guard lock(journal.mutex);
        if (handle.slot > journal.programUsed)
            return false;
        const auto& facts = journal.programs[handle.slot - 1];
        if (!facts.rootAddress || !facts.rootLifetime || !facts.graphicsAddress || !facts.graphicsLifetime ||
            (native ? (facts.computeAddress || facts.computeLifetime)
                    : (!facts.computeAddress || !facts.computeLifetime)))
            return false;
        out = facts;
        return true;
    }

    bool pipelineObservationEnabled() noexcept {
        return hostRequest().valid;
    }
    void pipelineDrawTable(PipelineDrawFacts& out,
                           const PipelineDescriptorHandle& descriptors,
                           const PipelineProgramHandle& programs,
                           const vdxrfg::pair_capture::nr::NrPipelineOwnership& graph,
                           ID3D12GraphicsCommandList* list,
                           uint32_t kind,
                           uint32_t eye,
                           uint64_t gpu,
                           const void* constants,
                           size_t bytes) noexcept {
        out = {};
        if (!hostRequest().valid || !list || eye > 1 || (kind != 1 && kind != 2) || !gpu || !constants || bytes != 80)
            return;
        PipelineDescriptorCreations created{};
        PipelineProgramFacts program{};
        if (!pipelineDescriptorCopy(descriptors, created) ||
            !pipelineProgramCopy(programs, graph.nativeAnswer != nullptr, program) ||
            !sdkCurrentRecording(list, out.serial, out.recording) || graph.device != out.recording.device ||
            graph.fence != out.recording.fence ||
            graph.root != reinterpret_cast<ID3D12RootSignature*>(program.rootAddress) ||
            (kind == 1 ? graph.downsample != reinterpret_cast<ID3D12PipelineState*>(program.computeAddress)
                       : graph.resolve != reinterpret_cast<ID3D12PipelineState*>(program.graphicsAddress)))
            return;
        auto& d = out.draw;
        d.kind = kind;
        d.eye = eye;
        d.listOrContext = reinterpret_cast<uint64_t>(list);
        d.recordingEpoch = out.recording.facts.nrRecordingEpoch;
        d.pipelineLifetime = graph.nrPipelineLifetimeEpoch;
        d.pipelineGeneration = graph.nrPipelineGenerationEpoch;
        d.batchEpoch = graph.nrBatchObjectEpoch;
        d.psoAddress = kind == 1 ? program.computeAddress : program.graphicsAddress;
        d.rootAddress = program.rootAddress;
        d.rootTable0 = gpu;
        d.constantsBytes = bytes;
        if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextDrawBind, d.bindEpoch))
            return;
        std::memcpy(out.constants.data(), constants, bytes);
        out.shaderHash = kind == 1 ? program.computeHash : program.pixelHash;
        const auto stride = created.rows[eye * 6].descriptorIncrement;
        if (!stride || stride > (UINT64_MAX - gpu) / 5)
            return;
        for (size_t i = 0; i < 6; ++i) {
            auto& row = out.descriptors[i];
            row = created.rows[eye * 6 + i];
            if (row.heapAddress != reinterpret_cast<uint64_t>(graph.views) || row.heapSlot != eye * 6 + i)
                return;
            row.gpuHandle = gpu + i * stride;
            row.bindEpoch = d.bindEpoch;
            if (row.resourceAddress &&
                !latestOriginalWrite(row.resourceAddress, row.resourceLifetimeEpoch, out.sourceAtBind[i]))
                sourceNrOriginalWrite12(reinterpret_cast<ID3D12Resource*>(row.resourceAddress),
                                        row.resourceLifetimeEpoch,
                                        out.sourceAtBind[i]);
        }
        out.descriptors[6] = created.rows[12];
        ResourceWriteFact previousTarget{};
        if (latestOriginalWrite(
                out.descriptors[6].resourceAddress, out.descriptors[6].resourceLifetimeEpoch, previousTarget))
            out.targetWriteBefore = previousTarget.sequence;
        // Used slots come from the exact compiled shader family, not all bound slots.
        out.shaderReadMask =
            pipelineShaderReadMask(kind, graph.nativeAnswer != nullptr, out.descriptors[1].resourceAddress != 0);
        out.bound = true;
    }
    namespace {
        bool output(PipelineDrawFacts& row, size_t slot, size_t index) noexcept {
            const auto& descriptor = row.descriptors[slot];
            return descriptor.resourceAddress &&
                   latestOriginalWrite(
                       descriptor.resourceAddress, descriptor.resourceLifetimeEpoch, row.outputs[index]) &&
                   pipelineOutputFresh(row.outputs[index], row, slot);
        }
        void publishDraw(PipelineDrawFacts& row) noexcept {
            if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextCommandObservation, row.commandObservation))
                return;
            row.draw.resourceWriteEpoch = row.outputs[0].sequence;
            row.recorded = true;
            std::lock_guard lock(journal.mutex);
            auto& frame = journal.frames[row.serial % journal.frames.size()];
            if (frame.serial != row.serial) {
                frame = {};
                frame.serial = row.serial;
            }
            const auto slot = (row.draw.kind - 1) * 2 + row.draw.eye;
            if (frame.invalid || frame.draws[slot].recorded) {
                frame.invalid = true;
                row.recorded = false;
                return;
            }
            frame.draws[slot] = row;
        }
    } // namespace
    void pipelineDispatchRecorded(PipelineDrawFacts& row, uint32_t x, uint32_t y, uint32_t z) noexcept {
        if (!row.bound || row.draw.kind != 1 || !x || !y || z != 1 || !output(row, 4, 0))
            return;
        row.outputCount = 1;
        if (row.descriptors[5].resourceAddress) {
            if (!output(row, 5, 1))
                return;
            ++row.outputCount;
        }
        row.draw.dispatchX = x;
        row.draw.dispatchY = y;
        row.draw.dispatchZ = z;
        publishDraw(row);
    }
    void pipelineDrawRecorded(PipelineDrawFacts& row,
                              const D3D12_VIEWPORT& viewport,
                              const D3D12_RECT& scissor,
                              uint64_t target,
                              uint32_t vertices,
                              uint32_t instances,
                              uint32_t startVertex,
                              uint32_t startInstance) noexcept {
        if (!row.bound || row.draw.kind != 2 || row.descriptors[6].cpuHandle != target || !output(row, 6, 0))
            return;
        auto& d = row.draw;
        row.targetCpu = target;
        row.outputCount = 1;
        d.viewportXbits = descriptorFloatBits(viewport.TopLeftX);
        d.viewportYbits = descriptorFloatBits(viewport.TopLeftY);
        d.viewportWbits = descriptorFloatBits(viewport.Width);
        d.viewportHbits = descriptorFloatBits(viewport.Height);
        d.minDepthBits = descriptorFloatBits(viewport.MinDepth);
        d.maxDepthBits = descriptorFloatBits(viewport.MaxDepth);
        d.scissorLeftBits = static_cast<uint32_t>(scissor.left);
        d.scissorTopBits = static_cast<uint32_t>(scissor.top);
        d.scissorRightBits = static_cast<uint32_t>(scissor.right);
        d.scissorBottomBits = static_cast<uint32_t>(scissor.bottom);
        d.vertexCount = vertices;
        d.instanceCount = instances;
        d.startVertex = startVertex;
        d.startInstance = startInstance;
        publishDraw(row);
    }
    void pipelinePhaseRestored(ID3D12GraphicsCommandList* list, uint32_t kind) noexcept {
        if (!hostRequest().valid || kind < 1 || kind > 2)
            return;
        uint64_t serial{};
        vdxrfg::pair_capture::nr::NrRecordingOwnership recording{};
        if (!sdkCurrentRecording(list, serial, recording))
            return;
        std::lock_guard lock(journal.mutex);
        auto& frame = journal.frames[serial % journal.frames.size()];
        if (frame.serial != serial || frame.invalid)
            return;
        for (size_t eye = 0; eye < 2; ++eye) {
            auto& row = frame.draws[(kind - 1) * 2 + eye];
            if (row.recorded && row.draw.listOrContext == reinterpret_cast<uint64_t>(list) &&
                row.draw.recordingEpoch == recording.facts.nrRecordingEpoch)
                row.restored = true;
        }
    }
    bool copyPipelineDraw(uint64_t serial, uint32_t kind, uint32_t eye, PipelineDrawFacts& out) noexcept {
        out = {};
        if (!serial || kind < 1 || kind > 2 || eye > 1)
            return false;
        std::lock_guard lock(journal.mutex);
        const auto& frame = journal.frames[serial % journal.frames.size()];
        const auto& row = frame.draws[(kind - 1) * 2 + eye];
        if (frame.serial != serial || frame.invalid || !row.recorded || !row.restored)
            return false;
        out = row;
        return true;
    }

} // namespace virtualdesktop_openxr::nr_capture
