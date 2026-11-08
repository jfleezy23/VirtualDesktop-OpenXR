#pragma once
#include "PairCaptureNrEnginePacket.h"
namespace virtualdesktop_openxr::nr_capture {
    struct DrawPacketInput {
        const EnginePacketInput* frame{};
        // Exactly four kind/eye indexed observations. Disabled stages are empty.
        EnginePacketSpan<PipelineDrawFacts> pipeline{}; // (kind-1)*2+eye
        EnginePacketSpan<PostFxRecord> postFx{};        // (kind-3)*2+eye
        uint64_t shaderModuleIndex{};                   // actual host shader-module placement
    };
    struct DrawPacketProjection {
        std::array<NrDescriptorV1, 21> descriptors{};
        std::array<NrDrawV1, 8> draws{};
        std::array<std::array<uint8_t, 32>, 8> hashes{};
        std::array<std::array<uint8_t, 112>, 8> constants{};
        std::array<EnginePacketDrawInput, 8> inputs{};
        uint64_t descriptorCount{}, drawCount{};
    };
    // Pure bounded projection of actual observations. No COM/query, SDK, observer,
    // ownership, allocation or GPU action. Caller storage is valid and disjoint.
    // Failure clears output. Success owns every copied byte; inputs point into out,
    // so out must stay at the same address until assembleEnginePacket returns.
    // The compact dictionary records representative end-phase stamps. Per-draw
    // consumed inputs and distinct output events are checked before compaction.
    bool projectDrawPacket(DrawPacketProjection& out, const DrawPacketInput&, NrError&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
