#pragma once

#include <d3d12.h>
#include <optional>

namespace virtualdesktop_openxr::nr_before_sr {
    struct BeforeSrStartup {
        bool observing{}, usesEarlyPlacement{}, hubrisFrameGeneration{};
    };

    struct ModelAppearance {
        int style{};
        float intensity{1.f}, localToneStrength{0.2f}, localStructureStrength{0.7f}, skinStructureStrength{0.5f};
    };

    // Selected only for the qualified Hubris shipping image and D3D12 binding.
    BeforeSrStartup
    StartBeforeSr(ID3D12Device*, ID3D12CommandQueue*, ID3D12GraphicsCommandList*, ID3D12CommandAllocator*) noexcept;
    void StopBeforeSr() noexcept;
    bool BeforeSrModelUnavailable() noexcept;
    bool HasBeforeSrModel() noexcept;
    void UpdateBeforeSrAppearance(
        int style, float intensity, float tone, float detail, float skin, bool enabled, bool validProfile) noexcept;
    void BeginBeforeSrFrame() noexcept;
    std::optional<ModelAppearance> EndBeforeSrFrame(bool stereoProjection) noexcept;
} // namespace virtualdesktop_openxr::nr_before_sr
