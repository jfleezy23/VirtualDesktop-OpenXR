#pragma once

#include "vdxr_fg_api.h"

namespace vdxrfg {
    inline bool validDepthEnvelope(const VDXRFG_SubmitInfoV5& info, bool hasInput) noexcept {
        if (info.struct_size < sizeof(info) || info.api_version != VDXRFG_API_VERSION_5 ||
            info.base.struct_size < sizeof(info.base) || info.base.api_version != VDXRFG_API_VERSION_3)
            return false;
        if (!info.guide_mask)
            return !info.guide_serial && !info.guide_epoch && !info.guide_encoding;
        return info.guide_mask == 3 && hasInput && info.guide_serial == info.base.serial && info.guide_serial &&
               info.guide_epoch && info.guide_encoding == VDXRFG_GUIDE_ENCODING_INVERSE_Z;
    }

    inline bool compatibleDepthPair(uint64_t previousColorSerial,
                                    uint64_t previousGuideSerial,
                                    uint64_t previousEpoch,
                                    uint64_t currentSerial,
                                    uint64_t currentEpoch) noexcept {
        return previousColorSerial && previousGuideSerial == previousColorSerial && previousEpoch &&
               previousEpoch == currentEpoch && previousColorSerial != UINT64_MAX &&
               currentSerial == previousColorSerial + 1;
    }
} // namespace vdxrfg
