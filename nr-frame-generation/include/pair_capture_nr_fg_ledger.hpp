#pragma once
#include "pair_capture_core.hpp"
#include <unknwn.h>
#include <array>
#include <mutex>
namespace vdxrfg::pair_capture::nr {
    enum class FgRef : unsigned {
        output0,
        output1,
        output2,
        output3,
        output4,
        output5,
        output6,
        output7,
        colourA,
        colourB,
        depthA,
        depthB,
        srvHeap,
        device,
        queue,
        copyAllocator,
        copyList,
        copyFence,
        copyFence11,
        productFence,
        info11,
        info12,
        submissionDevice,
        readback0,
        readback1,
        readback2,
        readback3,
        readback4,
        originalList,
        originalAllocator,
        root,
        pso,
        unusedB0,
        b1,
        rtvHeap,
        count
    };
    static_assert(static_cast<unsigned>(FgRef::count) == 35);
    // FG resources retain the original CopyOwnership dependency rather than
    // inventing additional host NrWorkOwner submission groups.
    class FgLedger final {
      public:
        bool reserve(uint64_t hostRefs, uint64_t allocationBytes) noexcept {
            std::lock_guard lock(mutex_);
            if (reserved_ || !hostRefs || hostRefs > 89 || hostRefs + 35 + 3 > 128 || !allocationBytes ||
                allocationBytes > 134217728)
                return false;
            reserved_ = true;
            limit_ = allocationBytes;
            return true;
        }
        bool acquire(FgRef slot, IUnknown* pointer, uint64_t bytes) noexcept {
            const auto i = static_cast<unsigned>(slot);
            {
                std::lock_guard lock(mutex_);
                if (!reserved_ || sealed_ || uncertain_ || !pointer || i >= refs_.size() || slot == FgRef::unusedB0 ||
                    refs_[i].state != 0 || bytes > limit_ - charged_)
                    return false;
                refs_[i] = {pointer, bytes, 1};
                charged_ += bytes;
                ++pending_;
            }
            try {
                pointer->AddRef();
            } catch (...) {
                std::lock_guard lock(mutex_);
                uncertain_ = true;
                return false;
            }
            {
                std::lock_guard lock(mutex_);
                refs_[i].state = 2;
                --pending_;
            }
            return true;
        }
        // Successful Create* returns one owned reference. Its source caller must
        // reserve the entire allocation budget before calling Create*, then transfer
        // that reference here without a local ComPtr or second AddRef.
        bool adopt(FgRef slot, IUnknown* pointer, uint64_t bytes) noexcept {
            const auto i = static_cast<unsigned>(slot);
            std::lock_guard lock(mutex_);
            if (!reserved_ || sealed_ || uncertain_ || !pointer || i >= refs_.size() || slot == FgRef::unusedB0 ||
                refs_[i].state != 0 || bytes > limit_ - charged_)
                return false;
            refs_[i] = {pointer, bytes, 2};
            charged_ += bytes;
            return true;
        }
        bool reserveCreate(FgRef slot, uint64_t bytes) noexcept {
            const auto i = static_cast<unsigned>(slot);
            std::lock_guard lock(mutex_);
            if (!reserved_ || sealed_ || uncertain_ || i >= refs_.size() || slot == FgRef::unusedB0 || refs_[i].state ||
                bytes > limit_ - charged_)
                return false;
            refs_[i] = {nullptr, bytes, 1};
            charged_ += bytes;
            ++pending_;
            return true;
        }
        bool finishCreate(FgRef slot, IUnknown* pointer, bool succeeded) noexcept {
            const auto i = static_cast<unsigned>(slot);
            std::lock_guard lock(mutex_);
            if (i >= refs_.size() || refs_[i].state != 1 || refs_[i].pointer)
                return false;
            refs_[i].pointer = pointer;
            refs_[i].state = pointer ? 2u : 0u;
            --pending_;
            if (!succeeded || !pointer) {
                uncertain_ = true;
                return false;
            }
            return true;
        }
        bool observedAbsent(FgRef slot) noexcept {
            const auto i = static_cast<unsigned>(slot);
            std::lock_guard lock(mutex_);
            if (i >= refs_.size() || refs_[i].state != 1 || refs_[i].pointer || refs_[i].bytes)
                return false;
            refs_[i].state = 3;
            --pending_;
            return true;
        }
        bool seal() noexcept {
            std::lock_guard lock(mutex_);
            if (!reserved_ || sealed_ || uncertain_ || pending_)
                return false;
            sealed_ = true;
            return true;
        }
        bool sealed() const noexcept {
            std::lock_guard lock(mutex_);
            return sealed_;
        }
        // The synchronous builder may access its already-owned stable slot before
        // seal. Worker readers use borrow(), which requires the immutable table.
        IUnknown* builder(FgRef slot) const noexcept {
            std::lock_guard lock(mutex_);
            const auto i = static_cast<unsigned>(slot);
            return !closed_ && !releasing_ && i < refs_.size() && refs_[i].state == 2 ? refs_[i].pointer : nullptr;
        }
        IUnknown* borrow(FgRef slot) const noexcept {
            std::lock_guard lock(mutex_);
            const auto i = static_cast<unsigned>(slot);
            return sealed_ && !closed_ && !releasing_ && i < refs_.size() && refs_[i].state == 2 ? refs_[i].pointer
                                                                                                 : nullptr;
        }
        bool close(const CopyOwnership& ownership, bool readersClosed) noexcept {
            std::lock_guard lock(mutex_);
            if (!sealed_ || uncertain_ || closed_ || !readersClosed || !ownership.canRelease() ||
                !ownership.canRetire())
                return false;
            closed_ = true;
            return true;
        }
        bool release() noexcept {
            std::array<IUnknown*, 35> batch{};
            {
                std::lock_guard lock(mutex_);
                if (!closed_ || uncertain_ || releasing_)
                    return false;
                releasing_ = true;
                for (unsigned i = 0; i < refs_.size(); ++i)
                    if (refs_[i].state == 2)
                        batch[i] = refs_[i].pointer;
            }
            try {
                for (auto* reference : batch)
                    if (reference)
                        reference->Release();
            } catch (...) {
                std::lock_guard lock(mutex_);
                uncertain_ = true;
                return false;
            }
            {
                std::lock_guard lock(mutex_);
                for (auto& reference : refs_)
                    reference = {};
            }
            return true;
        }

      private:
        struct Ref {
            IUnknown* pointer{};
            uint64_t bytes{};
            unsigned state{};
        };
        mutable std::mutex mutex_;
        std::array<Ref, 35> refs_{};
        uint64_t limit_{}, charged_{};
        unsigned pending_{};
        bool reserved_{}, sealed_{}, uncertain_{}, closed_{}, releasing_{};
    };
} // namespace vdxrfg::pair_capture::nr
