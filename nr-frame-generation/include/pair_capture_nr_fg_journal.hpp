#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace vdxrfg::pair_capture::nr {
    enum class FgEvent : uint64_t {
        reset = 1,
        close,
        execute,
        signal,
        wait,
        historyCopy,
        guideCopy,
        consumerCopy,
        outputCopy,
        descriptor,
        draw,
        admission,
        retire,
        nativeOutput,
        fenceExport,
        fenceImport,
        textureExport,
        textureImport,
        retain,
        allocate,
        completed,
        inputCopy,
        readersClosed,
        workerEnd,
        released,
        allocatorReset,
        getDevice,
        queryInterface,
        initialClose,
        supplementalReleased
    };
    struct FgEventRow {
        FgEvent kind{};
        uint64_t sequence{}, qpcObserved{}, thread{}, list{}, allocator{}, recording{}, queue{}, fence{}, value{},
            source{}, target{}, serial{}, epoch{}, result{}, count{}, aux[8]{};
    };
    struct FgRecording {
        uint64_t list{}, allocator{}, epoch{}, closeSequence{}, executeSequence{}, queue{};
    };
    // CPU facts only. The observer's mutex protects this table; no method owns or
    // calls COM. Rows describe observed original calls, not prospective submissions.
    struct FgJournal {
        std::array<FgEventRow, 1024> rows{};
        std::array<FgRecording, 64> recordings{};
        uint64_t count{}, nextEpoch{};
        bool overflow{};
        FgEventRow* append(FgEventRow row) noexcept {
            if (!row.qpcObserved || !row.thread || count == rows.size()) {
                overflow = true;
                return nullptr;
            }
            row.sequence = count + 1;
            rows[static_cast<size_t>(count++)] = row;
            return &rows[static_cast<size_t>(count - 1)];
        }
        FgRecording* recording(uint64_t list) noexcept {
            for (auto& row : recordings)
                if (row.list == list && list)
                    return &row;
            return nullptr;
        }
        bool allocatorReset(uint64_t allocator, uint32_t result, uint64_t qpc, uint64_t thread) noexcept {
            if (!allocator)
                return false;
            FgEventRow row{};
            row.kind = FgEvent::allocatorReset;
            row.allocator = allocator;
            row.result = result;
            row.qpcObserved = qpc;
            row.thread = thread;
            return append(row) != nullptr && !(result & 0x80000000u);
        }
        bool reset(uint64_t list, uint64_t allocator, uint32_t result, uint64_t qpc, uint64_t thread) noexcept {
            if (!list || !allocator)
                return false;
            FgEventRow event{};
            event.kind = FgEvent::reset;
            event.list = list;
            event.allocator = allocator;
            event.result = result;
            event.qpcObserved = qpc;
            event.thread = thread;
            if (result & 0x80000000u) {
                append(event);
                return false;
            }
            auto* row = recording(list);
            if (!row)
                for (auto& available : recordings)
                    if (!available.list) {
                        row = &available;
                        break;
                    }
            if (!row || nextEpoch == UINT64_MAX)
                return false;
            event.recording = ++nextEpoch;
            if (!append(event))
                return false;
            *row = {list, allocator, event.recording, 0, 0, 0};
            return true;
        }
        bool closed(uint64_t list, uint32_t result, uint64_t qpc, uint64_t thread) noexcept {
            auto* row = recording(list);
            if (!row || row->closeSequence)
                return false;
            FgEventRow event{};
            event.kind = FgEvent::close;
            event.list = list;
            event.allocator = row->allocator;
            event.recording = row->epoch;
            event.result = result;
            event.qpcObserved = qpc;
            event.thread = thread;
            auto* saved = append(event);
            if (!saved || result & 0x80000000u)
                return false;
            row->closeSequence = saved->sequence;
            return true;
        }
        bool execute(uint64_t queue, const uint64_t* lists, size_t size, uint64_t qpc, uint64_t thread) noexcept {
            if (!queue || !lists || !size || size > 16 || rows.size() - count < size)
                return false;
            for (size_t i = 0; i < size; ++i) {
                const auto* row = recording(lists[i]);
                if (!row || !row->closeSequence || row->executeSequence)
                    return false;
                for (size_t j = 0; j < i; ++j)
                    if (lists[i] == lists[j])
                        return false;
            }
            for (size_t i = 0; i < size; ++i) {
                auto* row = recording(lists[i]);
                FgEventRow event{};
                event.kind = FgEvent::execute;
                event.list = row->list;
                event.allocator = row->allocator;
                event.recording = row->epoch;
                event.queue = queue;
                event.count = size;
                event.qpcObserved = qpc;
                event.thread = thread;
                event.aux[0] = i;
                auto* saved = append(event);
                if (!saved)
                    return false;
                row->queue = queue;
                row->executeSequence = saved->sequence;
            }
            return true;
        }
    };
    static_assert(sizeof(FgJournal) < 256 * 1024);
} // namespace vdxrfg::pair_capture::nr
