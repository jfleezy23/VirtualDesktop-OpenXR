#include "PairCaptureNrModules.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrSources.h"
#include <array>
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct ManualModuleJournal {
            std::mutex mutex;
            std::array<ManualModuleObservation, 8> rows{};
            uint64_t nextLifetime{1}, nextPublication{1};
            size_t count{};
            bool invalid{};
        } moduleLease;
        static_assert(sizeof(ManualModuleJournal) <= hostMetadataModule);
    } // namespace
    void observeManualModuleLoad(const void* module, uint64_t begin, uint64_t end) noexcept {
        if (!hostRequest().valid || !module)
            return;
        const auto thread = GetCurrentThreadId();
        std::lock_guard lock(moduleLease.mutex);
        if (!begin || end < begin || !moduleLease.nextLifetime || moduleLease.nextLifetime == UINT64_MAX ||
            moduleLease.count == moduleLease.rows.size()) {
            moduleLease.invalid = true;
            return;
        }
        for (size_t i = 0; i < moduleLease.count; ++i)
            if (moduleLease.rows[i].moduleAddress == reinterpret_cast<uint64_t>(module) &&
                moduleLease.rows[i].ownerLive) {
                moduleLease.invalid = true;
                return;
            }
        auto& row = moduleLease.rows[moduleLease.count++];
        row.moduleAddress = reinterpret_cast<uint64_t>(module);
        row.loadLifetimeEpoch = moduleLease.nextLifetime++;
        row.loadBegin = begin;
        row.loadEnd = end;
        row.loadThread = thread;
        row.ownerLive = true;
    }
    void observeManualModulePublication(const void* module, const uint64_t (&functions)[5]) noexcept {
        if (!hostRequest().valid)
            return;
        const auto thread = GetCurrentThreadId();
        const auto qpc = sourceClock();
        std::lock_guard lock(moduleLease.mutex);
        for (size_t i = moduleLease.count; i; --i) {
            auto& row = moduleLease.rows[i - 1];
            if (row.moduleAddress != reinterpret_cast<uint64_t>(module) || !row.ownerLive)
                continue;
            if (row.publicationEpoch || !moduleLease.nextPublication || moduleLease.nextPublication == UINT64_MAX) {
                moduleLease.invalid = true;
                return;
            }
            for (auto function : functions)
                if (!function) {
                    moduleLease.invalid = true;
                    return;
                }
            row.publicationEpoch = moduleLease.nextPublication++;
            row.publicationQpc = qpc;
            row.publicationThread = thread;
            for (size_t f = 0; f < 5; ++f)
                row.functions[f] = functions[f];
            return;
        }
        moduleLease.invalid = true;
    }
    void observeManualModuleOwnerRelease(const void* module) noexcept {
        if (!hostRequest().valid || !module)
            return;
        std::lock_guard lock(moduleLease.mutex);
        for (size_t i = moduleLease.count; i; --i) {
            auto& row = moduleLease.rows[i - 1];
            if (row.moduleAddress == reinterpret_cast<uint64_t>(module) && row.ownerLive) {
                // Original owner reset has returned. No claim about unrelated OS
                // module references or the actual loader's final unmapping time.
                row.ownerLive = false;
                return;
            }
        }
        moduleLease.invalid = true;
    }
    bool copyManualModuleObservation(uint64_t actualDispatch, ManualModuleObservation& out) noexcept {
        out = {};
        if (!hostRequest().valid || !actualDispatch)
            return false;
        std::lock_guard lock(moduleLease.mutex);
        if (moduleLease.invalid)
            return false;
        for (size_t i = moduleLease.count; i; --i) {
            const auto& row = moduleLease.rows[i - 1];
            if (!row.ownerLive || !row.publicationEpoch)
                continue;
            for (auto function : row.functions)
                if (function == actualDispatch) {
                    out = row;
                    return true;
                }
        }
        return false;
    }
} // namespace virtualdesktop_openxr::nr_capture
