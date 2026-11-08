#pragma once
#include <atomic>
#include <cstdint>
#include <d3d12.h>
namespace vdxrfg::pair_capture::nr {
    // Terminal state disables observation only; original API work is never conditional
    // on this result. Relaxed ordering is sufficient: callers serialize graph mutation.
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    inline bool TryNextObservedEpoch(std::atomic<uint64_t>& next, uint64_t& out) noexcept {
        out = 0;
        auto value = next.load(std::memory_order_relaxed);
        while (value && value < UINT64_MAX) {
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
                out = value;
                return true;
            }
        }
        return false;
    }
    // A facts carrier, not an equality/qualification engine. Native allocation facts
    // remain in their existing separately tagged native packet.
    struct NrCaptureEpochFacts {
        uint64_t hostLifetimeEpoch{}, ownerHostLifetimeEpoch{};
        uint64_t fgHistoryEpoch{}, ticketFgHistoryEpoch{};
        uint64_t nrCommandContextLifetimeEpoch{};
        uint64_t nrPipelineLifetimeEpoch{}, nrPipelineGenerationEpoch{};
        uint64_t nrBatchObjectEpoch{}, nrDescriptorBindEpoch{};
        uint64_t nrListObjectEpoch{}, nrAllocatorObjectEpoch{}, nrRecordingEpoch{};
        enum Presence : uint32_t {
            hostLifetime = 1,
            ownerHostLifetime = 2,
            fgHistory = 4,
            ticketFgHistory = 8,
            nrCommandContextLifetime = 16,
            nrPipelineLifetime = 32,
            nrPipelineGeneration = 64,
            nrBatchObject = 128,
            nrDescriptorBind = 256,
            nrListObject = 512,
            nrAllocatorObject = 1024,
            nrRecording = 2048,
            all = 4095
        };
        uint32_t observed{};
    };
    struct NrRecordingEpochFacts {
        const void* originatingCommandContext{};
        uint64_t nrCommandContextLifetimeEpoch{}, nrListObjectEpoch{}, nrAllocatorObjectEpoch{}, nrRecordingEpoch{};
        enum Presence : uint32_t { device = 1, queue = 2, fence = 4, list = 8, allocator = 16, all = 31 };
        uint32_t observed{}; // Pointer-presence facts read by the actual projection.
    };
    // Context provenance is separate from NrSubmissionKey, which has no context slot.
    inline bool TryBorrowRecordingEpochFacts(const void* context,
                                             uint64_t contextLifetime,
                                             const NrRecordingEpochFacts& candidate,
                                             NrRecordingEpochFacts& out) noexcept {
        const auto facts = candidate;
        out = {};
        if (!context || !contextLifetime || contextLifetime == UINT64_MAX ||
            facts.originatingCommandContext != context || facts.nrCommandContextLifetimeEpoch != contextLifetime ||
            facts.observed != NrRecordingEpochFacts::all || !facts.nrListObjectEpoch ||
            facts.nrListObjectEpoch == UINT64_MAX || !facts.nrAllocatorObjectEpoch ||
            facts.nrAllocatorObjectEpoch == UINT64_MAX || !facts.nrRecordingEpoch ||
            facts.nrRecordingEpoch == UINT64_MAX)
            return false;
        out = facts;
        return true;
    }
    // No ownership, COM calls, allocations, destructors or thread handoff. The caller
    // keeps the actual context/list and Pipeline graph owners stable through finishAcquire.
    struct NrRecordingOwnership {
        ID3D12Device* device{};
        ID3D12CommandQueue* queue{};
        ID3D12Fence* fence{};
        ID3D12GraphicsCommandList* list{};
        ID3D12CommandAllocator* allocator{};
        NrRecordingEpochFacts facts{};
    };
    struct NrPipelineOwnership {
        ID3D12Device* device{};
        ID3D12Fence* fence{};
        ID3D12Resource* inputs[5]{};
        ID3D12DescriptorHeap* views{};
        ID3D12DescriptorHeap* targets{};
        ID3D12RootSignature* root{};
        ID3D12PipelineState* downsample{};
        ID3D12PipelineState* resolve{};
        struct Scratch {
            ID3D12Resource* proxy{};
            ID3D12Resource* answer{};
            ID3D12Resource* depth{};
        } scratch[2]{};
        ID3D12Resource* nativeAnswer{};
        uint64_t nrPipelineLifetimeEpoch{}, nrPipelineGenerationEpoch{}, nrBatchObjectEpoch{}, nrDescriptorBindEpoch{};
        // Successful borrow queried all 19 pointers; bits in declaration order mark
        // non-null presence. A zero optional bit means observed null, not unobserved.
        // Failed borrow returns false and clears the entire output.
        uint32_t pointerPresence{};
        enum EpochPresence : uint32_t {
            pipelineLifetime = 1,
            pipelineGeneration = 2,
            batchObject = 4,
            descriptorBind = 8,
            allEpochs = 15
        };
        uint32_t observedEpochs{};
    };
} // namespace vdxrfg::pair_capture::nr
