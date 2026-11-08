#pragma once
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrSnapshots.h"
namespace virtualdesktop_openxr::nr_capture {
    inline bool warmRoleRequired(uint64_t mode, bool upscaling, bool sharpening, uint64_t role) noexcept {
        if (mode != NR_NATIVE100 && mode != NR_SPATIAL85)
            return false;
        switch (role) {
        case NR_RAW_COLOUR:
        case NR_RAW_DEPTH:
        case NR_MODEL_COLOUR:
        case NR_MODEL_DEPTH:
        case NR_POST_EVAL_COLOUR:
        case NR_PRE_RESOLVE_COLOUR:
        case NR_POST_RESOLVE:
        case NR_FINAL_COLOUR:
        case NR_CANONICAL_DEPTH:
            return true;
        case NR_NATIVE_ANSWER:
            return mode == NR_NATIVE100;
        case NR_PROXY:
        case NR_PROXY_DEPTH:
        case NR_SPATIAL_ANSWER:
        case NR_EXACT_PROXY:
            return mode == NR_SPATIAL85;
        case NR_POST_EASU:
            return upscaling;
        case NR_POST_CAS:
            return sharpening;
        default:
            return false;
        }
    }
    inline uint32_t warmRoleEyes(uint64_t role) noexcept {
        return role == NR_NATIVE_ANSWER || role == NR_POST_RESOLVE || role == NR_CANONICAL_DEPTH ? 1u : 2u;
    }
    inline uint32_t requiredWarmRoleCount(uint64_t mode, bool upscaling, bool sharpening) noexcept {
        uint32_t count = 0;
        for (uint64_t role = 1; role <= 16; ++role)
            if (warmRoleRequired(mode, upscaling, sharpening, role))
                count += warmRoleEyes(role);
        return count;
    }
    struct WarmRoleLayout {
        uint64_t role{}, eye{}, sourceAddress{}, descriptionObservation{};
        D3D11_TEXTURE2D_DESC texture11{};
        D3D12_RESOURCE_DESC texture12{};
        uint32_t api{};
    };
    struct WarmFrameLayout {
        uint64_t serial{}, mode{}, nrContextLifetime{}, pipelineLifetime{}, pipelineGeneration{};
        std::array<WarmRoleLayout, 28> roles{};
        uint32_t count{};
        bool upscaling{}, sharpening{}, complete{}, invalid{};
    };
    void warmLayoutBegin(uint64_t serial, uint64_t mode, bool upscaling, bool sharpening) noexcept;
    void warmLayout11(uint64_t serial, uint64_t role, uint64_t eye, ID3D11Texture2D*) noexcept;
    void warmLayoutGraph12(uint64_t serial,
                           uint64_t mode,
                           uint64_t nrContextLifetime,
                           const vdxrfg::pair_capture::nr::NrPipelineOwnership&,
                           ID3D12Resource* const nativeDepth[2]) noexcept;
    bool warmLayoutFinish(uint64_t serial) noexcept;
    bool warmLayoutCopy(uint64_t serial, WarmFrameLayout&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
