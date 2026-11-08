#include "PairCaptureNrSources.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrPrivate.h"
#include <array>
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct FenceJournal {
            std::mutex mutex;
            std::array<NrOutputFenceFacts, 64> rows{}, inputs{};
            size_t inputCount{};
            size_t count{};
            uint64_t nextImportedLifetime{1};
            bool invalid{};
        } fences;
        static_assert(sizeof(FenceJournal) <= hostMetadataFence);
        NrOutputFenceFacts* fenceRow(const void* context, uint64_t lifetime) noexcept {
            for (size_t i = fences.count; i; --i) {
                auto& row = fences.rows[i - 1];
                if (row.context == reinterpret_cast<uint64_t>(context) && row.contextLifetime == lifetime)
                    return &row;
            }
            return nullptr;
        }
        NrCallV1 fenceCall(uint64_t kind,
                           const void* device,
                           const void* object,
                           HANDLE handle,
                           int32_t result,
                           uint64_t begin,
                           uint64_t end) noexcept {
            NrCallV1 call{};
            call.kind = kind;
            call.eye = 2;
            call.deviceAddress = reinterpret_cast<uint64_t>(device);
            call.queueAddress = reinterpret_cast<uint64_t>(device); // Actual interface this.
            call.fenceAddress = reinterpret_cast<uint64_t>(object);
            call.aux.auxiliaryValue = reinterpret_cast<uint64_t>(handle);
            call.resultType = NR_HRESULT;
            call.resultBits = static_cast<uint32_t>(result);
            call.classifiedSuccess = result >= 0;
            call.beginObserved = call.endObserved = 1;
            call.qpcBegin = begin;
            call.qpcEnd = end;
            call.thread = GetCurrentThreadId();
            call.observedMask = NR_CALL_OBS;
            return call;
        }
    } // namespace
    void sourceNrFenceExportObserved(const void* context,
                                     uint64_t lifetime,
                                     ID3D12Device* device,
                                     ID3D12Fence* fence,
                                     HANDLE handle,
                                     int32_t result,
                                     uint64_t begin,
                                     uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(fences.mutex);
        if (!context || !lifetime || lifetime == UINT64_MAX || !device || !fence || fenceRow(context, lifetime) ||
            fences.count == fences.rows.size()) {
            fences.invalid = true;
            return;
        }
        auto& row = fences.rows[fences.count++];
        row.context = reinterpret_cast<uint64_t>(context);
        row.contextLifetime = lifetime;
        row.alias.sourceAddress = reinterpret_cast<uint64_t>(fence);
        // Completion fence lifetime is bounded by its actual owning CommandContext.
        row.alias.sourceLifetime = lifetime;
        row.alias.sourceDevice = reinterpret_cast<uint64_t>(device);
        row.alias.exportedHandleObserved = reinterpret_cast<uint64_t>(handle);
        row.alias.apiKind = 1;
        row.exportCall = fenceCall(NR_EXPORT_FENCE, device, fence, handle, result, begin, end);
        row.exportCall.actual.actualCountArgument = GENERIC_ALL;
    }
    void sourceNrFenceImportObserved(const void* context,
                                     uint64_t lifetime,
                                     ID3D11Device* device,
                                     ID3D11Fence* fence,
                                     HANDLE handle,
                                     int32_t result,
                                     uint64_t begin,
                                     uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(fences.mutex);
        auto* row = fenceRow(context, lifetime);
        if (!row || row->alias.exportedHandleObserved != reinterpret_cast<uint64_t>(handle) || !device ||
            row->importCall.beginObserved) {
            fences.invalid = true;
            return;
        }
        row->alias.targetAddress = reinterpret_cast<uint64_t>(fence);
        row->alias.targetDevice = reinterpret_cast<uint64_t>(device);
        if (result >= 0 && fence) {
            if (!fences.nextImportedLifetime || fences.nextImportedLifetime == UINT64_MAX) {
                fences.invalid = true;
                return;
            }
            row->alias.targetLifetime = fences.nextImportedLifetime++;
        }
        row->importCall = fenceCall(NR_IMPORT_FENCE, device, fence, handle, result, begin, end);
        row->alias.observedMask = 3;
    }
    bool sourceNrFenceGet(const void* context, uint64_t lifetime, NrOutputFenceFacts& out) noexcept {
        out = {};
        if (!hostRequest().valid)
            return false;
        std::lock_guard lock(fences.mutex);
        const auto* row = fenceRow(context, lifetime);
        if (!row || fences.invalid)
            return false;
        out = *row;
        return true;
    }
    void sourceNrInputFenceExportObserved(const void* context,
                                          uint64_t lifetime,
                                          ID3D11Device* device,
                                          ID3D11Fence* fence,
                                          HANDLE handle,
                                          int32_t result,
                                          uint64_t begin,
                                          uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        NrHostStatusV1 status{};
        status.size = sizeof status;
        status.version = 1;
        if (!VDXR_PrivatePairNRStatusV1(&status) || !status.hostLifetimeEpoch)
            return;
        const auto thread = GetCurrentThreadId();
        std::lock_guard lock(fences.mutex);
        if (!context || !lifetime || !device || !fence || fences.inputCount == fences.inputs.size()) {
            fences.invalid = true;
            return;
        }
        for (size_t i = 0; i < fences.inputCount; ++i)
            if (fences.inputs[i].context == reinterpret_cast<uint64_t>(context) &&
                fences.inputs[i].contextLifetime == lifetime) {
                fences.invalid = true;
                return;
            }
        auto& row = fences.inputs[fences.inputCount++];
        row.context = reinterpret_cast<uint64_t>(context);
        row.contextLifetime = lifetime;
        row.alias.sourceAddress = reinterpret_cast<uint64_t>(fence);
        row.alias.sourceLifetime = status.hostLifetimeEpoch;
        row.alias.sourceDevice = reinterpret_cast<uint64_t>(device);
        row.alias.exportedHandleObserved = reinterpret_cast<uint64_t>(handle);
        row.alias.apiKind = 3;
        auto& call = row.exportCall;
        call.kind = NR_EXPORT_FENCE;
        call.eye = 2;
        call.deviceAddress = reinterpret_cast<uint64_t>(device);
        call.queueAddress = reinterpret_cast<uint64_t>(fence);
        call.fenceAddress = reinterpret_cast<uint64_t>(fence);
        call.aux.auxiliaryValue = reinterpret_cast<uint64_t>(handle);
        call.actual.actualCountArgument = GENERIC_ALL;
        call.resultType = NR_HRESULT;
        call.resultBits = static_cast<uint32_t>(result);
        call.classifiedSuccess = result >= 0;
        call.beginObserved = call.endObserved = 1;
        call.qpcBegin = begin;
        call.qpcEnd = end;
        call.thread = thread;
        call.observedMask = NR_CALL_OBS;
    }
    void sourceNrInputFenceImportObserved(const void* context,
                                          uint64_t lifetime,
                                          ID3D12Device* device,
                                          ID3D12Fence* fence,
                                          HANDLE handle,
                                          int32_t result,
                                          uint64_t begin,
                                          uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        const auto thread = GetCurrentThreadId();
        std::lock_guard lock(fences.mutex);
        for (size_t i = fences.inputCount; i; --i) {
            auto& row = fences.inputs[i - 1];
            if (row.context != reinterpret_cast<uint64_t>(context) || row.contextLifetime != lifetime)
                continue;
            if (row.alias.exportedHandleObserved != reinterpret_cast<uint64_t>(handle) ||
                row.importCall.beginObserved || !device || !fences.nextImportedLifetime ||
                fences.nextImportedLifetime == UINT64_MAX) {
                fences.invalid = true;
                return;
            }
            row.alias.targetAddress = reinterpret_cast<uint64_t>(fence);
            row.alias.targetDevice = reinterpret_cast<uint64_t>(device);
            if (result >= 0 && fence)
                row.alias.targetLifetime = fences.nextImportedLifetime++;
            row.alias.observedMask = 3;
            auto& call = row.importCall;
            call.kind = NR_IMPORT_FENCE;
            call.eye = 2;
            call.deviceAddress = call.queueAddress = reinterpret_cast<uint64_t>(device);
            call.fenceAddress = reinterpret_cast<uint64_t>(fence);
            call.aux.auxiliaryValue = reinterpret_cast<uint64_t>(handle);
            call.resultType = NR_HRESULT;
            call.resultBits = static_cast<uint32_t>(result);
            call.classifiedSuccess = result >= 0;
            call.beginObserved = call.endObserved = 1;
            call.qpcBegin = begin;
            call.qpcEnd = end;
            call.thread = thread;
            call.observedMask = NR_CALL_OBS;
            return;
        }
        fences.invalid = true;
    }
    bool sourceNrInputFenceGet(const void* context, uint64_t lifetime, NrOutputFenceFacts& out) noexcept {
        out = {};
        if (!hostRequest().valid)
            return false;
        std::lock_guard lock(fences.mutex);
        if (fences.invalid)
            return false;
        for (size_t i = fences.inputCount; i; --i)
            if (fences.inputs[i - 1].context == reinterpret_cast<uint64_t>(context) &&
                fences.inputs[i - 1].contextLifetime == lifetime) {
                out = fences.inputs[i - 1];
                return true;
            }
        return false;
    }

} // namespace virtualdesktop_openxr::nr_capture
