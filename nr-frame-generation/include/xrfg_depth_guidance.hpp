#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"

#include <array>
#include <cstdint>
#include <wrl/client.h>

namespace xrfg {
    // Private module/derivative bridge. These COMMON atlases and capture views
    // remain owned through the full synthesis completion fence.
    struct D3D12DepthGuidance {
        Microsoft::WRL::ComPtr<ID3D12Resource> previous, current;
        uint64_t serialA{}, serialB{}, epoch{};
        uint32_t atlasWidth{}, atlasHeight{};
        bool colorOpaque{};
        bool reconstructCamera{};
        std::array<D3D12ReprojectionView, 2> viewsA{}, viewsB{};
    };
} // namespace xrfg
