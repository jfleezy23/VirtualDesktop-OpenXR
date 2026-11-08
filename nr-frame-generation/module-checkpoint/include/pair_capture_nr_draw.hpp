#pragma once
#include "pair_capture_nr_abi.h"
#include <array>
#include <cstring>
namespace vdxrfg::pair_capture::nr {
    inline constexpr size_t fgRootBytes = 248, fgCbvBytes = 256, fgConstantBytes = 504;
    inline bool copyFgConstants(std::array<uint8_t, fgConstantBytes>& target,
                                const void* root,
                                size_t rootBytes,
                                const void* cbv,
                                size_t cbvBytes) noexcept {
        if (!root || !cbv || rootBytes != fgRootBytes || cbvBytes != fgCbvBytes)
            return false;
        std::memcpy(target.data(), root, fgRootBytes);
        std::memcpy(target.data() + fgRootBytes, cbv, fgCbvBytes);
        return true;
    }
    inline bool appendFgConstants(NrPacketV1& final,
                                  NrDrawV1& draw,
                                  const void* root,
                                  size_t rootBytes,
                                  const void* cbv,
                                  size_t cbvBytes) noexcept {
        const auto offset = final.header.constantsUsedBytes;
        if (final.header.flags != 1 || !root || !cbv || rootBytes != fgRootBytes || cbvBytes != fgCbvBytes ||
            offset > sizeof final.constants || fgConstantBytes > sizeof final.constants - offset)
            return false;
        std::array<uint8_t, fgConstantBytes> observed{};
        if (!copyFgConstants(observed, root, rootBytes, cbv, cbvBytes))
            return false;
        std::memcpy(final.constants + offset, observed.data(), observed.size());
        draw.constantsOffset = offset;
        draw.constantsBytes = fgConstantBytes;
        final.header.constantsUsedBytes += fgConstantBytes;
        return true;
    }
} // namespace vdxrfg::pair_capture::nr
