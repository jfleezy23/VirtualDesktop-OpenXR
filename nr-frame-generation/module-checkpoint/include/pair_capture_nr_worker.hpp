#pragma once
#include "pair_capture_nr_receiver.hpp"
#include <filesystem>
namespace vdxrfg::pair_capture::nr {
    struct PayloadRecord {
        uint64_t frame{}, roleIndex{}, bytes{}, rowPitch{};
        std::array<uint8_t, 32> sha256{};
        std::array<char, 64> filename{};
    };
    bool mappedExtent(const NrRoleV1&, uint64_t pitch, uint64_t& bytes) noexcept;
    // This is the host-role portion of the worker. It cannot produce a pair success
    // artifact: the five FG payloads and final native-output/retirement join are
    // independently required by the final artifact validator.
    bool writeHostFrame(Receiver&,
                        unsigned frame,
                        const std::filesystem::path&,
                        NrPacketV1& final,
                        std::array<PayloadRecord, 64>&,
                        uint64_t& written,
                        HANDLE retainedEvents[3]) noexcept;
} // namespace vdxrfg::pair_capture::nr
