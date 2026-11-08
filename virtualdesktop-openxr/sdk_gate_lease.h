#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

namespace virtualdesktop_openxr::detail {
    struct SdkGateTiming {
        const char* staticOp;
        long long nativeId;
        uintptr_t gate;
        int64_t gateEnter, gateAcquired, callEnter, callReturn, releaseEnter, released;
        bool callEntered, callCompleted;
    };

    // One record per recursive acquisition, not a claim that an inner unlock
    // released an outer owner's gate. Observer is a compile-time policy: no
    // runtime callbacks, allocations or diagnostic clocks on the disabled path.
    template <typename Observer>
    class SdkGateLease {
      public:
        SdkGateLease() noexcept : m_enabled(false) {
        }
        SdkGateLease(std::recursive_mutex& gate, const char* operation, long long nativeId = -1)
            : m_lock(gate, std::defer_lock), m_enabled(Observer::enabled()) {
            if (m_enabled) {
                m_timing = {};
                m_timing.staticOp = operation;
                m_timing.nativeId = nativeId;
                m_timing.gate = reinterpret_cast<uintptr_t>(&gate);
                m_timing.gateEnter = Observer::now();
            }
            m_lock.lock();
            if (m_enabled)
                m_timing.gateAcquired = Observer::now();
        }
        SdkGateLease(const SdkGateLease&) = delete;
        SdkGateLease& operator=(const SdkGateLease&) = delete;
        SdkGateLease& operator=(SdkGateLease&&) = delete;
        SdkGateLease(SdkGateLease&& other) noexcept : m_lock(std::move(other.m_lock)), m_enabled(other.m_enabled) {
            if (m_enabled)
                m_timing = other.m_timing;
            other.m_enabled = false;
        }
        ~SdkGateLease() noexcept {
            if (m_lock.owns_lock())
                unlock();
        }
        bool tracing() const noexcept {
            return m_enabled;
        }
        bool owns_lock() const noexcept {
            return m_lock.owns_lock();
        }
        void enterCall() noexcept {
            if (m_enabled) {
                m_timing.callEnter = Observer::now();
                m_timing.callEntered = true;
            }
        }
        void leaveCall() noexcept {
            if (m_enabled) {
                m_timing.callReturn = Observer::now();
                m_timing.callCompleted = true;
            }
        }
        void unlock() {
            if (m_enabled)
                m_timing.releaseEnter = Observer::now();
            m_lock.unlock();
            if (m_enabled) {
                m_timing.released = Observer::now();
                Observer::emit(m_timing);
                m_enabled = false;
            }
        }

      private:
        std::unique_lock<std::recursive_mutex> m_lock;
        bool m_enabled;
        SdkGateTiming m_timing;
    };
} // namespace virtualdesktop_openxr::detail
