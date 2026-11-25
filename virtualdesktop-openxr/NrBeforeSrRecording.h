#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace virtualdesktop_openxr::nr_before_sr {
    // Abandoned means no qualifying execution was confirmed; unobserved submissions may have run.
    enum class RecordingOutcome { Pending, Executed, Abandoned, Quarantined };

    class RecordingGeneration {
      public:
        RecordingGeneration(uintptr_t list, uintptr_t allocator, uint64_t generation)
            : list(list), allocator(allocator), generation(generation) {
        }
        const uintptr_t list; // Canonical IUnknown identity, never dereferenced here.
        const uintptr_t allocator;
        const uint64_t generation;
        RecordingOutcome outcome() const noexcept {
            return m_outcome.load();
        }

      private:
        friend class RecordingLedger;
        std::atomic<RecordingOutcome> m_outcome{RecordingOutcome::Pending};
    };

    // A legal successful D3D12 allocator Reset proves GPU work from that allocator has
    // completed and its prior command streams can no longer be submitted. A command-list
    // Reset alone has neither property. Submission observations supplement allocator proof.
    // The caller serializes each observed native Execute + private Signal. No GPU waits.
    class RecordingLedger {
        static constexpr size_t recordingLimit = 64;
        static constexpr size_t ownerLimit = 16;
        static constexpr size_t submissionLimit = 64;
        using Owners = std::vector<std::shared_ptr<void>>;

        struct Slot {
            std::shared_ptr<RecordingGeneration> generation;
            Owners owners;
            std::shared_ptr<void> identities; // Retains the exact COM list and allocator across address reuse.
            bool replayable{}, allocatorReset{};
            size_t submitting{};  // Execute snapshots lacking a committed signal.
            uint64_t lastFence{}; // Covers all successful signals on the sole authorized queue.
        };
        struct Flight {
            uint64_t id{}, slots{};
        };
        struct Storage {
            std::array<Slot, recordingLimit> slots{};
            std::array<Flight, submissionLimit> flights{};
        };

      public:
        using Generation = std::shared_ptr<const RecordingGeneration>;
        class AllocatorReset {
          public:
            AllocatorReset() = default;

          private:
            friend class RecordingLedger;
            AllocatorReset(const Storage* owner, uintptr_t allocator, uint64_t watermark)
                : owner(owner), allocator(allocator), watermark(watermark) {
            }
            const Storage* owner{};
            uintptr_t allocator{};
            uint64_t watermark{};
        };
        class Submission {
          public:
            Submission() = default;
            explicit operator bool() const noexcept {
                return m_id != 0;
            }

          private:
            friend class RecordingLedger;
            Submission(const Storage* owner, uint64_t id) : m_owner(owner), m_id(id) {
            }
            const Storage* m_owner{};
            uint64_t m_id{};
        };

        explicit RecordingLedger(uintptr_t queue) : m_queue(queue), m_storage(std::make_unique<Storage>()) {
        }
        RecordingLedger(const RecordingLedger&) = delete;
        RecordingLedger& operator=(const RecordingLedger&) = delete;
        ~RecordingLedger() {
            // The hook owner must first exclude concurrent callbacks. A still-replayable
            // list or unproven signal cannot be made safe merely by unloading the session.
            // Pin live owners on unsafe teardown; the hook owner must retain its DLL too.
            for (const auto& slot : m_storage->slots) {
                if (slot.generation) {
                    quarantineLocked();
                    (void)m_storage.release();
                    break;
                }
            }
        }

        // Bind ONLY after successful native list Reset to this allocator. The lease must
        // retain both canonical IUnknown identities. Initial, unobserved lists cannot attach.
        bool bind(uintptr_t list, uintptr_t allocator, const std::shared_ptr<void>& identities) try {
            if (!list)
                return false;
            Slot retired; // Release displaced COM identities after unlocking.
            std::lock_guard<std::mutex> lock(m_mutex);
            Slot* empty{};
            Slot* stale{};
            Slot* oldestObservation{};
            for (auto& slot : m_storage->slots) {
                if (!slot.generation) {
                    empty = &slot;
                    continue;
                }
                if (slot.generation->list == list)
                    slot.replayable = false;
                if (!slot.owners.empty())
                    continue;
                if (!slot.replayable)
                    stale = &slot;
                else if (!oldestObservation || slot.generation->generation < oldestObservation->generation->generation)
                    oldestObservation = &slot;
            }
            // Observing Reset attaches no GPU work. On saturation, forgetting such a
            // live binding merely declines that list until its next successful Reset.
            auto* available = stale ? stale : (empty ? empty : oldestObservation);
            if (!m_queue || m_stopped || !allocator || !identities || !available || m_generation == UINT64_MAX)
                return false;
            auto next = std::make_shared<RecordingGeneration>(list, allocator, ++m_generation);
            retired = std::move(*available);
            if (retired.generation)
                retired.generation->m_outcome.store(RecordingOutcome::Abandoned);
            *available = {};
            available->generation = std::move(next);
            available->identities = identities;
            available->replayable = true;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }

        // Attach every dependency before recording commands referencing it.
        Generation attach(uintptr_t list, const std::shared_ptr<void>& owner) try {
            if (!list || !owner)
                return {};
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopped)
                return {};
            for (auto& slot : m_storage->slots) {
                if (!slot.replayable || slot.generation->list != list)
                    continue;
                for (const auto& existing : slot.owners)
                    if (!existing.owner_before(owner) && !owner.owner_before(existing))
                        return slot.generation;
                if (slot.owners.size() >= ownerLimit)
                    return {};
                slot.owners.push_back(owner);
                return slot.generation;
            }
            return {};
        } catch (const std::bad_alloc&) {
            return {};
        }

        Generation current(uintptr_t list) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& slot : m_storage->slots)
                if (slot.replayable && slot.generation->list == list)
                    return slot.generation;
            return {};
        }

        // Snapshot the old list generation before native Reset. A successful Reset
        // stops future list replay, but cannot certify completion of submitted work.
        void afterReset(const Generation& generation, bool succeeded) {
            if (!succeeded || !generation)
                return;
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& slot : m_storage->slots)
                if (slot.generation == generation)
                    slot.replayable = false;
        }

        // A watermark from BEFORE native allocator Reset excludes any newer binding,
        // even when hook callbacks complete in a different order.
        AllocatorReset beforeAllocatorReset(uintptr_t allocator) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return AllocatorReset(m_storage.get(), allocator, m_generation);
        }
        void afterAllocatorReset(uintptr_t allocator, AllocatorReset ticket, bool succeeded) {
            if (!succeeded || !allocator || ticket.allocator != allocator || ticket.owner != m_storage.get())
                return;
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& slot : m_storage->slots) {
                if (slot.generation && slot.generation->allocator == allocator &&
                    slot.generation->generation <= ticket.watermark) {
                    slot.replayable = false;
                    slot.allocatorReset = true;
                }
            }
        }

        Submission beforeExecute(uintptr_t queue, const uintptr_t* lists, size_t count) {
            return beforeExecute(queue, count, [lists](size_t i) { return lists ? lists[i] : uintptr_t{}; });
        }
        // Canonicalize each incoming identity once, outside the ledger lock. Batch size is
        // unbounded; our storage is bounded by the number of retained recordings instead.
        template <class IdentityAt>
        Submission beforeExecute(uintptr_t queue, size_t count, IdentityAt identityAt) {
            struct Candidate {
                Generation generation;
                size_t slot{};
            };
            std::array<Candidate, recordingLimit> observed{};
            size_t candidates{};
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_quarantined || !count)
                    return {};
                for (size_t i = 0; i < recordingLimit; ++i) {
                    auto& slot = m_storage->slots[i];
                    if (slot.replayable && !slot.owners.empty()) {
                        observed[candidates++] = {slot.generation, i};
                    }
                }
            }
            if (!candidates)
                return {};
            uint64_t mask = 0, duplicates = 0;
            for (size_t j = 0; j < count; ++j) {
                const auto id = identityAt(j);
                if (!id)
                    continue;
                for (size_t i = 0; i < candidates; ++i) {
                    if (observed[i].generation->list != id)
                        continue;
                    const auto bit = uint64_t{1} << observed[i].slot;
                    duplicates |= mask & bit;
                    mask |= bit;
                }
            }
            if (!mask)
                return {};
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_quarantined)
                return {};
            for (size_t i = 0; i < candidates; ++i) {
                const auto bit = uint64_t{1} << observed[i].slot;
                if (!(mask & bit))
                    continue;
                auto& slot = m_storage->slots[observed[i].slot];
                if (slot.generation != observed[i].generation)
                    mask &= ~bit;
                else if (queue != m_queue || (duplicates & bit) || slot.submitting || slot.lastFence) {
                    // The game still owns submission. A second or foreign execution may already reference
                    // this temporal history, so retain all owners and decline further recording.
                    quarantineLocked();
                    return {};
                }
            }
            if (!mask || queue != m_queue)
                return {};
            if (m_submission != UINT64_MAX) {
                for (auto& flight : m_storage->flights) {
                    if (flight.id)
                        continue;
                    flight = {++m_submission, mask};
                    for (size_t i = 0; i < recordingLimit; ++i)
                        if (mask & (uint64_t{1} << i))
                            ++m_storage->slots[i].submitting;
                    return Submission(m_storage.get(), flight.id);
                }
            }
            // Native Execute must still proceed. Retain all owners permanently when its
            // submission capacity cannot furnish a reliable completion receipt.
            quarantineLocked();
            return {};
        }

        // Called after native Execute and the subsequent private queue Signal. A missing,
        // failed or non-monotonic signal must never act as proof of completion.
        void afterSignal(Submission ticket, uint64_t value, bool succeeded) {
            if (!ticket)
                return;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_quarantined)
                return;
            if (ticket.m_owner == m_storage.get() && succeeded && value > m_signaled && value != UINT64_MAX) {
                for (auto& flight : m_storage->flights) {
                    if (flight.id != ticket.m_id)
                        continue;
                    for (size_t i = 0; i < recordingLimit; ++i) {
                        if (flight.slots & (uint64_t{1} << i)) {
                            auto& slot = m_storage->slots[i];
                            --slot.submitting;
                            slot.lastFence = value;
                        }
                    }
                    m_signaled = value;
                    flight = {};
                    return;
                }
            }
            quarantineLocked();
        }

        void poll(uint64_t completed) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (completed == UINT64_MAX) { // D3D12 removed-device sentinel.
                quarantineLocked();
                return;
            }
            if (completed > m_completed)
                m_completed = completed;
        }
        void stop() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopped = true;
            for (auto& slot : m_storage->slots)
                if (slot.owners.empty())
                    slot.replayable = false;
        }
        // Resume the same queue without dropping any owners or restoring invalid list generations.
        bool resume() {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_quarantined)
                return false;
            m_stopped = false;
            return true;
        }
        void quarantine() {
            std::lock_guard<std::mutex> lock(m_mutex);
            quarantineLocked();
        }
        bool quarantined() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_quarantined;
        }
        bool empty() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& slot : m_storage->slots)
                if (slot.generation)
                    return false;
            return true;
        }

      private:
        void quarantineLocked() {
            m_stopped = m_quarantined = true;
            for (auto& slot : m_storage->slots)
                if (slot.generation)
                    slot.generation->m_outcome.store(RecordingOutcome::Quarantined);
        }

      public:
        // Called on the serialized model thread. Reset, Execute, Signal and polling only
        // publish observations. Releasing model handles/SDK maps may call vendor code.
        void drain() {
            std::array<Slot, recordingLimit> retired{};
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_quarantined)
                return;
            for (size_t i = 0; i < recordingLimit; ++i) {
                auto& slot = m_storage->slots[i];
                if (!slot.generation || slot.replayable || slot.submitting || slot.lastFence > m_completed)
                    continue;
                if (!slot.owners.empty() && !slot.allocatorReset)
                    continue;
                const auto outcome = slot.lastFence ? RecordingOutcome::Executed : RecordingOutcome::Abandoned;
                slot.generation->m_outcome.store(outcome);
                retired[i] = std::move(slot);
                slot = {};
            }
            // retired is destroyed after lock, on the calling thread.
        }

      private:
        const uintptr_t m_queue;
        mutable std::mutex m_mutex;
        std::unique_ptr<Storage> m_storage;
        uint64_t m_generation{}, m_submission{}, m_signaled{}, m_completed{};
        bool m_stopped{}, m_quarantined{};
    };
} // namespace virtualdesktop_openxr::nr_before_sr
