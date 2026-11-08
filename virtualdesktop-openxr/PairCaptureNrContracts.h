#pragma once
#include "../nr-frame-generation/include/pair_capture_nr_abi.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace virtualdesktop_openxr::nr_capture {
    // Scalar projection called only while the original CommandContext pool lock is held.
    // It observes a prospective value; it neither reserves nor advances that timeline.
    inline bool observeProspective(uint64_t contextLifetime,
                                   uint64_t currentCounter,
                                   uint64_t& lifetime,
                                   uint64_t& nextValue) noexcept {
        lifetime = nextValue = 0;
        if (!contextLifetime || contextLifetime == UINT64_MAX || currentCounter >= UINT64_MAX - 1)
            return false;
        lifetime = contextLifetime;
        nextValue = currentCounter + 1;
        return true;
    }
    inline uint64_t parameterType(uint64_t key) noexcept {
        if (key == NR_CREATION_NODE || key == NR_VISIBILITY_NODE)
            return NR_U32;
        if (key == NR_COLOR || key == NR_OUTPUT || key == NR_DEPTH || key == NR_MVEC_ABSENT)
            return NR_RESOURCE_ID;
        if ((key >= NR_INTENSITY && key <= NR_SKIN) || key == NR_MVEC_SCALE_X_ABSENT || key == NR_MVEC_SCALE_Y_ABSENT)
            return NR_F32;
        if (key >= NR_WORLD_VIEW_ABSENT)
            return NR_MATRIX_ABSENT;
        return NR_I32;
    }
    // The host owns this fixed scalar mirror under its existing caller serialization.
    // No pointer in these rows is dereferenced and no SDK value is queried.
    class ParameterMirror {
      public:
        bool bind(uint64_t address, uint64_t lifetime) noexcept {
            if (!address || !lifetime || lifetime == UINT64_MAX || (address_ == address && lifetime_ == lifetime))
                return false;
            address_ = address;
            lifetime_ = lifetime;
            nextWrite_ = 1;
            invalid_ = false;
            values_ = {};
            for (uint64_t i = 0; i < values_.size(); ++i) {
                values_[i].parameterEnum = i + 1;
                values_[i].typeEnum = parameterType(i + 1);
                values_[i].tableLifetime = lifetime;
                values_[i].observationStatus = i < 30 ? NR_UNOBSERVED : NR_NOT_HOST_WRITTEN;
            }
            return true;
        }
        bool set(uint64_t key, uint64_t type, uint64_t lo, uint64_t hi) noexcept {
            if (!address_ || invalid_ || key < 1 || key > 30 || type != parameterType(key) ||
                nextWrite_ == UINT64_MAX || !nextWrite_) {
                invalid_ = true;
                return false;
            }
            if (type == NR_RESOURCE_ID) {
                if ((lo && (!hi || hi == UINT64_MAX)) || (!lo && hi)) {
                    invalid_ = true;
                    return false;
                }
            } else if (lo > UINT32_MAX || hi) {
                invalid_ = true;
                return false;
            }
            auto& value = values_[key - 1];
            value.observationStatus = type == NR_RESOURCE_ID && !lo ? NR_OBSERVED_NULL : NR_OBSERVED_VALUE;
            value.valueLo = lo;
            value.valueHi = hi;
            value.writeEpoch = nextWrite_++;
            return true;
        }
        bool snapshot(uint64_t call, NrParamV1* rows, size_t count) const noexcept {
            if (!rows || count != values_.size() || invalid_ || !address_)
                return false;
            std::memcpy(rows, values_.data(), sizeof(values_));
            for (size_t i = 0; i < count; ++i)
                rows[i].callIndex = call;
            return true;
        }
        uint64_t address() const noexcept {
            return address_;
        }
        uint64_t lifetime() const noexcept {
            return lifetime_;
        }
        bool valid() const noexcept {
            return address_ && !invalid_;
        }

      private:
        std::array<NrParamV1, 39> values_{};
        uint64_t address_{}, lifetime_{}, nextWrite_{1};
        bool invalid_{};
    };
    enum class GuardAction { none, producerBoundary, covered, terminateChild };
    struct GuardFacts {
        bool rawExposed{}, nrExposed{}, postFxExposed{};
        bool nrExecutePossible{}, nrSignalObserved{}, nrSignalSucceeded{};
        bool outputWaitObserved{}, outputWaitSucceeded{};
        bool producerSignalObserved{}, producerSignalSucceeded{}, flushObserved{};
        bool appWaitObserved{}, appWaitSucceeded{};
        GuardAction action() const noexcept {
            if (!rawExposed && !nrExposed && !postFxExposed)
                return GuardAction::none;
            if (nrExecutePossible &&
                (!nrSignalObserved || !nrSignalSucceeded || !outputWaitObserved || !outputWaitSucceeded))
                return GuardAction::terminateChild;
            if ((producerSignalObserved && !producerSignalSucceeded) || (appWaitObserved && !appWaitSucceeded))
                return GuardAction::terminateChild;
            if (producerSignalObserved && producerSignalSucceeded && flushObserved && appWaitObserved &&
                appWaitSucceeded)
                return GuardAction::covered;
            return GuardAction::producerBoundary;
        }
    };
} // namespace virtualdesktop_openxr::nr_capture
