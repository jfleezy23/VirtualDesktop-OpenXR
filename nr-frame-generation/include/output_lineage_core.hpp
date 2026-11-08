#pragma once
// Private, allocation-free original-copy reuse identity. No COM/GPU operations.
#include <cstdint>
namespace vdxrfg::pair_capture {
    struct OutputIdentity {
        std::uintptr_t context{};
        std::uint64_t generation{}, token{}, ticketGeneration{};
        std::uint32_t bank{}, role{};
        std::uintptr_t source11{}, source12{}, destination11{}, fence11{};
    };
    struct SuccessfulCopy {
        OutputIdentity identity{};
        std::uint64_t copyId{}, value{};
    };
    inline bool preparedMatches(const OutputIdentity& a,
                                const SuccessfulCopy& prior,
                                std::uint64_t roleValue,
                                std::uintptr_t retained) noexcept {
        const auto& b = prior.identity;
        return prior.copyId && prior.value && prior.value == roleValue && retained == a.destination11 &&
               a.context == b.context && a.generation == b.generation && a.token == b.token &&
               a.ticketGeneration == b.ticketGeneration && a.bank == b.bank && a.role == b.role &&
               a.source11 == b.source11 && a.source12 == b.source12 && a.destination11 == b.destination11 &&
               a.fence11 == b.fence11;
    }
} // namespace vdxrfg::pair_capture
