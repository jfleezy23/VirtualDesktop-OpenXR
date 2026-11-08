#include "PairCaptureNrSdkPacket.h"
#include "../nr-frame-generation/include/pair_capture_nr_control.h"
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        bool epoch(uint64_t value) noexcept {
            return value && value != UINT64_MAX;
        }
        bool moduleContains(const NrPacketV1& p, uint64_t index, uint64_t address) noexcept {
            if (!address || index >= p.header.moduleCount)
                return false;
            const auto& m = p.modules[index];
            return m.moduleKind >= 1 && m.moduleKind <= 8 && m.baseObserved && m.imageSize && m.fileSize &&
                   epoch(m.loadLifetimeEpoch) &&
                   (m.sha256Words[0] || m.sha256Words[1] || m.sha256Words[2] || m.sha256Words[3]) &&
                   address >= m.baseObserved && address - m.baseObserved < m.imageSize;
        }
        bool parameter(const NrParamV1& q, const NrCallV1& call) noexcept {
            const auto key = q.parameterEnum;
            if (key < 1 || key > 39 || q.typeEnum != parameterType(key) ||
                q.tableLifetime != call.parameterTableLifetime || q.observationStatus > NR_NOT_HOST_WRITTEN)
                return false;
            if (key > 30)
                return q.observationStatus == NR_NOT_HOST_WRITTEN && !q.valueLo && !q.valueHi && !q.writeEpoch;
            if (q.observationStatus == NR_OBSERVED_VALUE) {
                if (!epoch(q.writeEpoch))
                    return false;
                if (q.typeEnum == NR_RESOURCE_ID)
                    return q.valueLo && epoch(q.valueHi);
                if (q.valueLo > UINT32_MAX || q.valueHi)
                    return false;
                return q.typeEnum != NR_F32 || (q.valueLo & UINT64_C(0x7f800000)) != UINT64_C(0x7f800000);
            }
            if (call.kind == NR_EVALUATE)
                return false;
            if (q.observationStatus == NR_OBSERVED_NULL)
                return q.typeEnum == NR_RESOURCE_ID && (key == NR_COLOR || key == NR_OUTPUT || key == NR_DEPTH) &&
                       !q.valueLo && !q.valueHi && epoch(q.writeEpoch);
            return q.observationStatus == NR_UNOBSERVED && !q.valueLo && !q.valueHi && !q.writeEpoch;
        }
    } // namespace
    bool appendSdkObservation(NrPacketV1& p, const SdkObservation& o, const SdkDispatchBinding& binding) noexcept {
        const auto& h = p.header;
        const auto& c = o.call;
        const bool mapped = c.kind == NR_CREATE_SDK || c.kind == NR_CREATE_MANUAL || c.kind == NR_EVALUATE;
        const bool init = c.kind == NR_INIT_MANUAL, release = c.kind == NR_RELEASE_FEATURE;
        if (h.magic != NR_MAGIC || h.size != sizeof p || h.version != 1 || h.operation != NR_FRAME_TRANSFER ||
            h.flags || h.frameOrdinal > 1 || h.callCount >= 160 || h.paramCount > 256 || !h.moduleCount ||
            h.moduleCount > 8 || (!mapped && !init && !release) || !o.ended || o.threw || c.beginObserved != 1 ||
            c.endObserved != 1 || !c.qpcBegin || c.qpcEnd < c.qpcBegin || !c.thread || c.eye > 2 ||
            c.resultType != NR_NGX || c.resultBits > UINT32_MAX ||
            c.classifiedSuccess != static_cast<uint64_t>(nr_ngx_not_failed(static_cast<uint32_t>(c.resultBits))) ||
            (c.classifiedSuccess && c.resultBits != 1) || c.flags ||
            (c.observedMask && c.observedMask != NR_CALL_OBS) ||
            !moduleContains(p, binding.dispatchModule, c.dispatchAddressObserved))
            return false;
        if (init) {
            if (c.eye != 2 || !c.deviceAddress || c.featureAddressObserved || c.featureLifetimeEpoch ||
                c.actual.pairedEvaluateAddress || c.aux.pairedReleaseAddress || c.dispatchTableEpoch ||
                binding.evaluateModule != NR_ABSENT || binding.releaseModule != NR_ABSENT)
                return false;
        } else {
            if (c.eye > 1 || !epoch(c.dispatchTableEpoch) ||
                !moduleContains(p, binding.evaluateModule, c.actual.pairedEvaluateAddress) ||
                !moduleContains(p, binding.releaseModule, c.aux.pairedReleaseAddress))
                return false;
            if (c.kind == NR_EVALUATE && c.dispatchAddressObserved != c.actual.pairedEvaluateAddress)
                return false;
            if (release && c.dispatchAddressObserved != c.aux.pairedReleaseAddress)
                return false;
            if (c.classifiedSuccess && (!c.featureAddressObserved || !epoch(c.featureLifetimeEpoch)))
                return false;
            if ((c.kind == NR_CREATE_SDK || c.kind == NR_CREATE_MANUAL) && !c.classifiedSuccess &&
                c.featureLifetimeEpoch)
                return false;
        }
        if (mapped) {
            if (!o.parametersObserved || c.paramCount != 39 || !c.parameterTableAddressObserved ||
                !epoch(c.parameterTableLifetime) || !c.listAddress || !epoch(c.recordingEpoch) || !c.contextAddress ||
                !epoch(c.contextLifetimeEpoch) || !c.deviceAddress || !c.queueAddress || !c.fenceAddress ||
                !c.allocatorAddress || h.paramCount > 256 - 39)
                return false;
            if (c.kind == NR_EVALUATE && (!c.classifiedSuccess || c.resultBits != 1))
                return false;
            uint64_t seen = 0;
            for (const auto& q : o.parameters) {
                if (!parameter(q, c) || (seen & (UINT64_C(1) << q.parameterEnum)))
                    return false;
                seen |= UINT64_C(1) << q.parameterEnum;
            }
        } else {
            if (o.parametersObserved || c.paramCount || c.paramFirst || c.parameterTableAddressObserved ||
                c.parameterTableLifetime || c.listAddress || c.allocatorAddress || c.recordingEpoch || c.queueAddress ||
                c.fenceAddress || c.contextAddress || c.contextLifetimeEpoch || (release && c.deviceAddress))
                return false;
            for (const auto& q : o.parameters)
                if (q.callIndex || q.parameterEnum || q.typeEnum || q.valueLo || q.valueHi || q.tableLifetime ||
                    q.writeEpoch || q.observationStatus)
                    return false;
        }
        // Derived placement does not replace any original API result or parameter bit.
        auto projected = c;
        projected.moduleIndex = binding.dispatchModule;
        projected.observedMask = NR_CALL_OBS;
        projected.paramFirst = mapped ? h.paramCount : 0;
        const auto index = h.callCount;
        if (mapped)
            for (const auto& q : o.parameters) {
                auto copied = q;
                copied.callIndex = index;
                p.params[p.header.paramCount++] = copied;
            }
        p.calls[p.header.callCount++] = projected;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
