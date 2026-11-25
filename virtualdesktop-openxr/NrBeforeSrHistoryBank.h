#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace virtualdesktop_openxr::nr_before_sr {
    struct HistoryEyeStatus {
        bool ready{};                        // The caller has established that this eye's history can be used again.
        std::uint64_t lastSuccessfulEpoch{}; // Zero for new, abandoned or invalidated history.
    };
    // The qualified D3D12 path can keep three command allocators live.
    inline constexpr std::size_t kStereoHistoryBankCount = 3;
    using StereoHistoryBanks = std::array<std::array<HistoryEyeStatus, 2>, kStereoHistoryBankCount>; // [bank][eye]

    struct StereoHistorySelection {
        std::size_t bank{};
        bool reset{}; // Applied to both eyes.
    };

    // Choose once for the pair. Only matching successful stereo bookends can continue one-frame motion.
    // A ready bank with older or partial history can be used after resetting both eyes.
    constexpr std::optional<StereoHistorySelection> SelectStereoHistoryBank(std::uint64_t epoch,
                                                                            const StereoHistoryBanks& banks) noexcept {
        if (!epoch)
            return std::nullopt;
        std::size_t firstReady = banks.size();
        for (std::size_t i = 0; i < banks.size(); ++i) {
            const auto& first = banks[i][0];
            const auto& second = banks[i][1];
            if (!first.ready || !second.ready)
                continue;
            if (first.lastSuccessfulEpoch && first.lastSuccessfulEpoch == epoch - 1 &&
                second.lastSuccessfulEpoch == first.lastSuccessfulEpoch)
                return StereoHistorySelection{i, false};
            if (firstReady == banks.size())
                firstReady = i;
        }
        if (firstReady != banks.size())
            return StereoHistorySelection{firstReady, true};
        return std::nullopt;
    }
} // namespace virtualdesktop_openxr::nr_before_sr
