#pragma once
#include "../nr-frame-generation/include/pair_capture_nr_abi.h"
#include <array>
#include <cstddef>
namespace virtualdesktop_openxr::nr_capture {
    // A returned original producer command is observed, not GPU completion.
    // Input resources possibly touched by an opaque SDK do not receive a fabricated
    // write event. Their prior observed producer write remains unchanged.
    struct ResourceWriteFact {
        uint64_t resource{}, lifetime{}, sequence{}, serial{}, contextOrList{}, recordingEpoch{};
        uint64_t api{}, kind{}, actualCallSequence{}, sourceAddress{}, thread{}, qpc{};
    };
    class ResourceWriteTable final {
      public:
        static constexpr size_t capacity = 256;
        bool observe(ResourceWriteFact& fact) noexcept {
            if (!fact.resource || !fact.lifetime || fact.lifetime == UINT64_MAX || fact.sequence || !fact.serial ||
                !fact.contextOrList || !fact.thread || (fact.api != 11 && fact.api != 12) ||
                (fact.kind != NR_COPY && fact.kind != NR_DISPATCH && fact.kind != NR_DRAW &&
                 fact.kind != NR_EVALUATE) ||
                (fact.api == 12 && !fact.recordingEpoch) ||
                (fact.kind == NR_EVALUATE && (fact.api != 12 || !fact.actualCallSequence)) || !next_ ||
                next_ == UINT64_MAX)
                return false;
            size_t slot = 0;
            for (; slot < count_; ++slot)
                if (rows_[slot].resource == fact.resource && rows_[slot].lifetime == fact.lifetime)
                    break;
            if (slot == count_) {
                if (count_ == capacity)
                    return false;
                ++count_;
            }
            fact.sequence = next_++;
            rows_[slot] = fact;
            return true;
        }
        bool latest(uint64_t resource, uint64_t lifetime, ResourceWriteFact& out) const noexcept {
            out = {};
            for (size_t i = 0; i < count_; ++i)
                if (rows_[i].resource == resource && rows_[i].lifetime == lifetime) {
                    out = rows_[i];
                    return true;
                }
            return false;
        }

      private:
        std::array<ResourceWriteFact, capacity> rows_{};
        size_t count_{};
        uint64_t next_{1};
    };
    bool observeOriginalWrite(uint64_t resource,
                              uint64_t lifetime,
                              uint64_t serial,
                              uint64_t contextOrList,
                              uint64_t recordingEpoch,
                              uint64_t api,
                              uint64_t kind,
                              uint64_t actualCallSequence = 0,
                              uint64_t sourceAddress = 0) noexcept;
    enum class OriginalWriteStatus { absent, observed, invalid };
    OriginalWriteStatus originalWriteStatus(uint64_t resource, uint64_t lifetime, ResourceWriteFact&) noexcept;
    bool latestOriginalWrite(uint64_t resource, uint64_t lifetime, ResourceWriteFact&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
