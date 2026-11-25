#pragma once

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <utility>

namespace virtualdesktop_openxr::nr_before_sr {
    // The recording owner retains the captured state through GPU completion and possible replay.
    // The guard covers every ordinary return and C++ unwind before entering native SR.
    template <class List, class Pipeline>
    class RestorePipelineOnExit {
      public:
        RestorePipelineOnExit(List* list, Pipeline* state) noexcept : m_list(list), m_state(state) {
        }
        RestorePipelineOnExit(const RestorePipelineOnExit&) = delete;
        RestorePipelineOnExit& operator=(const RestorePipelineOnExit&) = delete;
        ~RestorePipelineOnExit() {
            m_list->SetPipelineState(m_state);
        }

      private:
        List* const m_list;
        Pipeline* const m_state;
    };

    // Adopt only direct lists whose public graphics interfaces share the verified method table.
    // The ledger supplies a unique generation after a successful, observed native Reset.
    // Holding the list prevents address reuse. A missing entry merely declines recording.
    template <class List, class Pipeline, size_t Capacity = 64>
    class PipelineStateTracker {
        using PipelinePtr = Microsoft::WRL::ComPtr<Pipeline>;
        struct Entry {
            Microsoft::WRL::ComPtr<List> list;
            PipelinePtr state;
            std::uint64_t generation{};
        };
        std::array<Entry, Capacity> m_entries{};
        std::mutex m_mutex;

        auto find(List* list) noexcept {
            return std::find_if(m_entries.begin(), m_entries.end(), [=](auto& e) { return e.list.Get() == list; });
        }

      public:
        template <class Qualify>
        void reset(List* list, std::uint64_t generation, Pipeline* state, Qualify qualify) noexcept {
            if (!list || !generation)
                return;
            Entry retired; // Drop strong references outside the lock, including on eviction.
            {
                std::lock_guard lock(m_mutex);
                auto entry = find(list);
                if (entry != m_entries.end()) {
                    if (generation > entry->generation) {
                        retired.state = std::move(entry->state);
                        entry->state = state;
                        entry->generation = generation;
                    }
                    return;
                }
            }
            if (!qualify(list))
                return;
            // The application must serialize recording on each individual list, including Reset. Other lists
            // may adopt concurrently, so choose the eviction target only after qualification.
            Entry next{list, state, generation};
            std::lock_guard lock(m_mutex);
            auto entry = std::min_element(
                m_entries.begin(), m_entries.end(), [](auto& a, auto& b) { return a.generation < b.generation; });
            retired = std::move(*entry);
            *entry = std::move(next);
        }

        void set(List* list, Pipeline* state) noexcept {
            if (!list)
                return;
            PipelinePtr retired;
            std::lock_guard lock(m_mutex);
            auto entry = find(list);
            if (entry != m_entries.end() && entry->state.Get() != state) {
                retired = std::move(entry->state);
                entry->state = state;
            }
        }

        PipelinePtr capture(List* list, std::uint64_t generation) noexcept {
            if (!list || !generation)
                return {};
            std::lock_guard lock(m_mutex);
            const auto entry = find(list);
            return entry != m_entries.end() && entry->generation == generation ? entry->state : PipelinePtr{};
        }
    };
} // namespace virtualdesktop_openxr::nr_before_sr
