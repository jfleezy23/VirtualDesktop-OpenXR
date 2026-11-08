#pragma once
#include <d3d12.h>
#include "pair_capture_nr_receiver.hpp"
#include "pair_capture_nr_draw.hpp"
namespace vdxrfg::pair_capture::nr {
    bool requested() noexcept;
    void prepare(void* context) noexcept;
    void bind(
        void* context, ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource* const outputs[8], uint64_t history) noexcept;
    void generation(void* context, uint64_t history) noexcept;
    void invalidateContext(void* context) noexcept;
    void destroyed(void* context) noexcept;
    Receiver& receiver() noexcept;
    struct FgDrawObservation {
        uint64_t slot{}, list{}, pso{}, eye{}, flags{}, second{}, viewIndex{}, b1Resource{};
        std::array<uint8_t, fgConstantBytes> constants{};
        std::array<uint8_t, 32> shaderSha256{};
    };
    void draw(void* slot,
              ID3D12GraphicsCommandList*,
              ID3D12PipelineState*,
              uint32_t eye,
              uint32_t flags,
              uint32_t second,
              const void* b1,
              const void* b0,
              size_t b0Size,
              uint32_t viewIndex,
              ID3D12Resource*,
              const unsigned char* shader,
              size_t shaderSize) noexcept;
} // namespace vdxrfg::pair_capture::nr
