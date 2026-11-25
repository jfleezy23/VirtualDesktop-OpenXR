#pragma once

#include "NrBeforeSrPolicy.h"

#include <array>
#include <limits>

namespace virtualdesktop_openxr::nr_before_sr {
    struct StereoViewKey {
        std::uint64_t generation{};
        Rect output{};
        std::uint32_t sdkThread{};
    };
    using StereoPair = std::array<StereoViewKey, 2>;

    // The caller serializes frame boundaries and the entry/return of each native call.
    // Neither handle values nor call order identify an eye.
    class StereoGate {
      public:
        void begin() noexcept {
            if (m_open)
                complete(false); // A discarded/overlapping frame breaks the consecutive sequence.
            m_count = 0;
            m_recorded = 0;
            if (m_epoch == std::numeric_limits<std::uint64_t>::max())
                return;
            ++m_epoch;
            m_open = true;
            m_invalid = m_inFlight != 0;
            if (m_invalid)
                m_calibrated = false;
        }

        std::uint64_t enterEvaluation() noexcept {
            if (++m_inFlight > 1 && m_open) {
                // A native evaluation can re-enter its hook before the first eye returns.
                m_invalid = true;
                m_calibrated = false;
            }
            return m_open ? m_epoch : 0;
        }

        // An empty view represents an unqualified evaluation or a failed native call.
        bool leaveEvaluation(std::uint64_t epoch, StereoViewKey view, bool recorded) noexcept {
            --m_inFlight;
            if (!m_open || epoch != m_epoch)
                return false;
            m_recorded += recorded;
            if (!m_invalid) {
                if (!view.generation || !view.output.width || !view.output.height || m_count == m_seen.size() ||
                    (m_count && (m_seen[0].generation == view.generation || m_seen[0].sdkThread != view.sdkThread))) {
                    m_invalid = true;
                    m_calibrated = false;
                } else {
                    if (m_calibrated && !matches(m_candidate[0], view) && !matches(m_candidate[1], view))
                        m_calibrated = false;
                    m_seen[m_count++] = view;
                }
            }
            return true;
        }

        bool complete(bool stereoProjection) noexcept {
            // Even otherwise qualified views cannot renew calibration if only one eye was treated.
            const bool valid =
                m_open && stereoProjection && !m_invalid && !m_inFlight && m_count == m_seen.size() && m_recorded != 1;
            if (valid) {
                m_calibrated =
                    m_hasCandidate && ((matches(m_seen[0], m_candidate[0]) && matches(m_seen[1], m_candidate[1])) ||
                                       (matches(m_seen[0], m_candidate[1]) && matches(m_seen[1], m_candidate[0])));
                if (!m_calibrated)
                    m_candidate = m_seen;
                m_hasCandidate = true;
            } else {
                m_hasCandidate = false;
                m_calibrated = false;
            }
            m_open = false;
            return valid;
        }

        bool open() const noexcept {
            return m_open;
        }
        bool evaluationInFlight() const noexcept {
            return m_inFlight != 0;
        }
        std::uint64_t epoch() const noexcept {
            return m_epoch;
        }
        unsigned modelsRecorded() const noexcept {
            return m_recorded;
        }

        // The pair is an eligibility prediction for this open frame. Final coverage is only
        // known at complete(). An unexpected later call cannot undo an earlier GPU recording.
        const StereoPair* expected() const noexcept {
            return m_open && m_calibrated ? &m_candidate : nullptr;
        }

        bool canApply(std::uint64_t generation, Rect output, std::uint32_t sdkThread = 0) const noexcept {
            if (!expected() || m_count == m_seen.size() || (m_count && m_seen[0].generation == generation))
                return false;
            const StereoViewKey view{generation, output, sdkThread};
            return matches(m_candidate[0], view) || matches(m_candidate[1], view);
        }

      private:
        static bool matches(const StereoViewKey& a, const StereoViewKey& b) noexcept {
            return a.generation == b.generation && a.sdkThread == b.sdkThread && a.output.x == b.output.x &&
                   a.output.y == b.output.y && a.output.width == b.output.width && a.output.height == b.output.height;
        }

        StereoPair m_candidate{}, m_seen{};
        std::size_t m_count{}, m_inFlight{};
        std::uint64_t m_epoch{};
        unsigned m_recorded{};
        bool m_open{}, m_invalid{}, m_hasCandidate{}, m_calibrated{};
    };
} // namespace virtualdesktop_openxr::nr_before_sr
