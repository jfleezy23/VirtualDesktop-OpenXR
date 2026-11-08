#pragma once
#include <cstdint>
namespace vdxrfg::pair_capture {
    struct ArmIdentity {
        uint32_t pid{};
        uint64_t creation{}, sessionLow{}, sessionHigh{};
        uintptr_t context{};
        uint64_t generation{};
    };
    constexpr bool sameArm(const ArmIdentity& actual, const ArmIdentity& expected) noexcept {
        return actual.pid && actual.creation && (actual.sessionLow || actual.sessionHigh) && actual.context &&
               actual.generation && actual.pid == expected.pid && actual.creation == expected.creation &&
               actual.sessionLow == expected.sessionLow && actual.sessionHigh == expected.sessionHigh &&
               actual.context == expected.context && actual.generation == expected.generation;
    }
    struct CopyOwnership {
        bool productComplete{}, copyQueued{}, copyComplete{}, workerExited{}, hostWaitQueued{};
        constexpr bool uncertain() const noexcept {
            return !productComplete || ((copyQueued || hostWaitQueued) && !copyComplete);
        }
        constexpr bool canRetire() const noexcept {
            return productComplete && (copyComplete || workerExited && !copyQueued && !hostWaitQueued);
        }
        constexpr bool canRelease() const noexcept {
            return workerExited && !uncertain();
        }
    };
    enum class Phase { disabled, armed, a_provisional, a_admitted, b_provisional, pair_admitted, incomplete };
    struct Selector {
        Phase phase{Phase::disabled};
        uint64_t minimumSerial{}, minimumTime{}, capacity{}, bytes{}, serialA{}, timeA{}, serialB{}, tokenA{}, tokenB{},
            generation{}, epoch{};
        uintptr_t context{}, producerContext{};
        constexpr void arm(uint64_t serial, uint64_t time, uint64_t cap) noexcept {
            if (phase != Phase::disabled || !serial || !time || !cap) {
                abort();
                return;
            }
            minimumSerial = serial;
            minimumTime = time;
            capacity = cap;
            phase = Phase::armed;
        }
        constexpr void abort() noexcept {
            phase = Phase::incomplete;
        }
        constexpr bool accepts(uint64_t serial) const noexcept {
            return (phase == Phase::a_provisional && serial == serialA) ||
                   (phase == Phase::b_provisional && serial == serialB);
        }
        constexpr bool outputWait(uint64_t token, uint32_t role, bool copySafe) const noexcept {
            return role == 0 && token && token == tokenB && !copySafe;
        }
        constexpr bool hold(uint64_t token, bool copySafe) const noexcept {
            return token && token == tokenB && !copySafe;
        }
        constexpr bool begin(uint64_t serial, uint64_t time, uintptr_t ctx, uintptr_t producer) noexcept {
            if (phase == Phase::disabled || phase == Phase::incomplete || phase == Phase::pair_admitted)
                return false;
            if (phase == Phase::armed) {
                if (serial < minimumSerial || time < minimumTime)
                    return false;
                if (!ctx || !producer) {
                    abort();
                    return false;
                }
                serialA = serial;
                timeA = time;
                context = ctx;
                producerContext = producer;
                phase = Phase::a_provisional;
                return true;
            }
            if (phase != Phase::a_admitted || serialA == UINT64_MAX || serial != serialA + 1 || time <= timeA ||
                ctx != context || producer != producerContext) {
                abort();
                return false;
            }
            serialB = serial;
            phase = Phase::b_provisional;
            return true;
        }
        constexpr bool reserve(uint64_t amount) noexcept {
            if ((phase != Phase::a_provisional && phase != Phase::b_provisional && phase != Phase::pair_admitted) ||
                !amount || amount > capacity - bytes) {
                abort();
                return false;
            }
            bytes += amount;
            return true;
        }
        constexpr bool admit(int32_t result,
                             uint64_t token,
                             uint64_t previous,
                             uint64_t current,
                             uint64_t gen,
                             uint64_t guideEpoch,
                             uintptr_t ctx) noexcept {
            if (result != 0 || !token || !gen || !guideEpoch || ctx != context) {
                abort();
                return false;
            }
            if (phase == Phase::a_provisional && current == serialA) {
                tokenA = token;
                generation = gen;
                epoch = guideEpoch;
                phase = Phase::a_admitted;
                return true;
            }
            if (phase == Phase::b_provisional && previous == serialA && current == serialB && token != tokenA &&
                gen == generation && guideEpoch == epoch) {
                tokenB = token;
                phase = Phase::pair_admitted;
                return true;
            }
            abort();
            return false;
        }
    };
} // namespace vdxrfg::pair_capture
