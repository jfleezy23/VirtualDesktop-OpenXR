#include "PairCaptureNrEngine.h"
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    bool CaptureEngine::appendControl(uint32_t frame, const NrCallV1& actual, uint64_t& index) noexcept {
        if (frame > 1)
            return false;
        auto& out = projection_[frame];
        if (out.controlCount >= 128 || !actual.beginObserved || !actual.endObserved || !actual.qpcBegin ||
            actual.qpcEnd < actual.qpcBegin || !actual.thread || actual.observedMask != NR_CALL_OBS ||
            (actual.resultType == NR_VOID && (actual.resultBits || actual.classifiedSuccess)))
            return false;
        index = out.controlCount;
        out.controls[out.controlCount++] = actual;
        return true;
    }
    bool CaptureEngine::appendAlias(uint32_t frame,
                                    const NrAliasV1& observed,
                                    const NrCallV1& actualExport,
                                    const NrCallV1& actualImport,
                                    uint64_t& index) noexcept {
        if (frame > 1)
            return false;
        auto& out = projection_[frame];
        if (out.aliasCount >= 32 || out.controlCount > 126 || observed.observedMask != 3 || !observed.sourceAddress ||
            !observed.targetAddress || !observed.sourceLifetime || !observed.targetLifetime ||
            !observed.exportedHandleObserved || actualExport.kind != NR_EXPORT_FENCE ||
            actualImport.kind != NR_IMPORT_FENCE || !actualExport.classifiedSuccess || !actualImport.classifiedSuccess)
            return false;
        auto alias = observed;
        const auto actualIndex = [&](const NrCallV1& call, uint64_t& placed) noexcept {
            // Cached imports/exports are one original API event, even when several
            // phase snapshots associate different observed writes to that storage.
            for (uint32_t i = 0; i < out.controlCount; ++i)
                if (!std::memcmp(&out.controls[i], &call, sizeof call)) {
                    placed = i;
                    return true;
                }
            return appendControl(frame, call, placed);
        };
        if (!actualIndex(actualExport, alias.exportCallIndex) || !actualIndex(actualImport, alias.importCallIndex))
            return false;
        index = out.aliasCount;
        out.aliases[out.aliasCount++] = alias;
        return true;
    }
    bool CaptureEngine::projectFrameControls(uint32_t frame, SourceReturnGuard& guard) noexcept {
        if (frame > 1 || !projection_[frame].groupsProjected || projection_[frame].controlCount || !guard.covered())
            return false;
        uint64_t unused{};
        for (const auto& call : luidCalls_)
            if (!appendControl(frame, call, unused))
                return false;
        for (const auto* observed : {&nrFenceFacts_, &nrInputFenceFacts_})
            if (!appendAlias(frame, observed->alias, observed->exportCall, observed->importCall, unused))
                return false;
        const auto& f = fenceFacts_;
        NrCallV1 ex{};
        ex.kind = NR_EXPORT_FENCE;
        ex.eye = 2;
        ex.resultType = NR_HRESULT;
        ex.beginObserved = f.exportObserved;
        ex.endObserved = f.exportObserved;
        ex.classifiedSuccess = f.exportResult >= 0;
        ex.resultBits = static_cast<uint32_t>(f.exportResult);
        ex.qpcBegin = f.exportBegin;
        ex.qpcEnd = f.exportEnd;
        ex.thread = f.thread;
        ex.deviceAddress = f.producerDevice;
        ex.fenceAddress = f.producerFence;
        ex.aux.auxiliaryValue = f.handle;
        ex.observedMask = NR_CALL_OBS;
        ex.flags = 1;
        for (uint32_t target = 0; target < 2; ++target) {
            NrCallV1 im{};
            im.kind = NR_IMPORT_FENCE;
            im.eye = 2;
            im.resultType = NR_HRESULT;
            im.beginObserved = im.endObserved = target ? f.fgImportObserved : f.appImportObserved;
            im.resultBits = static_cast<uint32_t>(target ? f.fgImportResult : f.appImportResult);
            im.classifiedSuccess = static_cast<int32_t>(im.resultBits) >= 0;
            im.qpcBegin = target ? f.fgImportBegin : f.appImportBegin;
            im.qpcEnd = target ? f.fgImportEnd : f.appImportEnd;
            im.thread = f.thread;
            im.deviceAddress = target ? f.fgDevice : f.applicationDevice;
            im.fenceAddress = target ? f.fgFence : f.applicationFence;
            im.aux.auxiliaryValue = f.handle;
            im.observedMask = NR_CALL_OBS;
            im.flags = 1;
            NrAliasV1 alias{};
            alias.sourceAddress = f.producerFence;
            alias.sourceLifetime = f.producerLifetime;
            alias.sourceDevice = f.producerDevice;
            alias.targetAddress = im.fenceAddress;
            alias.targetLifetime = target ? f.fgLifetime : f.applicationLifetime;
            alias.targetDevice = im.deviceAddress;
            alias.exportedHandleObserved = f.handle;
            alias.apiKind = target ? 3 : 2;
            alias.observedMask = 3;
            if (!appendAlias(frame, alias, ex, im, unused))
                return false;
        }
        for (const auto& call : guard.inputCalls())
            if (!appendControl(frame, call, unused))
                return false;
        NrCallV1 submission[3]{};
        if (!projectNrSubmission(guard.submissionFacts(),
                                 groups_[frame][1].actual,
                                 groups_[frame][1].boundary,
                                 reinterpret_cast<uint64_t>(preparedInput_.nrDevice),
                                 0,
                                 guard.submissionBegin(),
                                 guard.submissionEnd(),
                                 guard.submissionThread(),
                                 submission))
            return false;
        for (const auto& call : submission)
            if (!appendControl(frame, call, unused))
                return false;
        if (!appendControl(frame, guard.originalWait(), unused))
            return false;
        for (const auto& call : guard.calls())
            if (!appendControl(frame, call, unused))
                return false;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
