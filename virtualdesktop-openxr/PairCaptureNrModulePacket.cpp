#include "PairCaptureNrModulePacket.h"
#include "PairCaptureNrSdkPacket.h"
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        bool epoch(uint64_t value) noexcept {
            return value && value != UINT64_MAX;
        }
        bool hashPresent(const uint8_t* hash) noexcept {
            for (unsigned i = 0; i < 32; ++i)
                if (hash[i])
                    return true;
            return false;
        }
        bool expectedShape(const ModuleExpectation& e) noexcept {
            return e.kind >= 1 && e.kind <= 8 && epoch(e.loadLifetimeEpoch) && e.observedAddress &&
                   hashPresent(e.expectedSha256.data());
        }
        bool emptyExpected(const ModuleExpectation& e) noexcept {
            return !e.kind && !e.loadLifetimeEpoch && !e.observedAddress && !hashPresent(e.expectedSha256.data());
        }
        bool moduleShape(const NrModuleV1& m) noexcept {
            return m.moduleKind >= 1 && m.moduleKind <= 8 && m.baseObserved && m.imageSize &&
                   m.imageSize <= UINT32_MAX && m.baseObserved <= UINT64_MAX - m.imageSize &&
                   m.addressRva < m.imageSize && m.fileSize && epoch(m.loadLifetimeEpoch) &&
                   hashPresent(reinterpret_cast<const uint8_t*>(m.sha256Words)) && m.observedMask == 7 &&
                   m.codeViewAge <= UINT32_MAX && m.pinObserved <= 1 && m.pinResult == m.pinObserved &&
                   (m.moduleKind > 2 || m.pinObserved == 1);
        }
        bool pinShape(const NrCallV1& c, uint64_t address) noexcept {
            if (c.kind != NR_PIN || c.beginObserved != 1 || c.endObserved != 1 || c.resultType != NR_BOOLEAN ||
                c.resultBits != 1 || c.classifiedSuccess != 1 || !c.qpcBegin || c.qpcEnd < c.qpcBegin || !c.thread ||
                c.eye != 2 || c.dispatchAddressObserved != address || c.moduleIndex >= 8 ||
                c.observedMask != NR_CALL_OBS)
                return false;
            NrCallV1 expected{};
            expected.kind = c.kind;
            expected.beginObserved = expected.endObserved = 1;
            expected.resultType = NR_BOOLEAN;
            expected.resultBits = expected.classifiedSuccess = 1;
            expected.qpcBegin = c.qpcBegin;
            expected.qpcEnd = c.qpcEnd;
            expected.thread = c.thread;
            expected.eye = 2;
            expected.dispatchAddressObserved = address;
            expected.moduleIndex = c.moduleIndex;
            expected.observedMask = NR_CALL_OBS;
            return !std::memcmp(&c, &expected, sizeof c);
        }
        bool sameImage(const NrModuleV1& a, const NrModuleV1& b) noexcept {
            return a.imageSize == b.imageSize && a.fileSize == b.fileSize &&
                   a.loadLifetimeEpoch == b.loadLifetimeEpoch &&
                   !std::memcmp(a.sha256Words, b.sha256Words, sizeof a.sha256Words) &&
                   !std::memcmp(a.codeViewGuidWords, b.codeViewGuidWords, sizeof a.codeViewGuidWords) &&
                   a.codeViewAge == b.codeViewAge;
        }
        bool compatibleImages(const NrModuleV1& a, const NrModuleV1& b) noexcept {
            if (a.baseObserved == b.baseObserved)
                return sameImage(a, b);
            // Both rows passed moduleShape, so these half-open ends cannot overflow.
            return a.baseObserved >= b.baseObserved + b.imageSize || b.baseObserved >= a.baseObserved + a.imageSize;
        }
        bool packetShape(const NrPacketV1& p) noexcept {
            const auto& h = p.header;
            if (h.magic != NR_MAGIC || h.size != sizeof p || h.version != 1 || h.operation != NR_FRAME_TRANSFER ||
                h.flags || h.frameOrdinal > 1 || h.moduleCount > 8 || h.callCount > 160)
                return false;
            for (uint64_t i = 0; i < h.moduleCount; ++i) {
                const auto& m = p.modules[i];
                if (!moduleShape(m) || (i < 3 && m.moduleKind != i + 1))
                    return false;
                for (uint64_t j = 0; j < i; ++j) {
                    const auto& earlier = p.modules[j];
                    if (m.moduleKind == earlier.moduleKind || !compatibleImages(m, earlier))
                        return false;
                }
                if (m.pinObserved) {
                    bool found = false;
                    for (uint64_t j = 0; j < h.callCount; ++j)
                        if (p.calls[j].moduleIndex == i && pinShape(p.calls[j], m.baseObserved + m.addressRva)) {
                            found = true;
                            break;
                        }
                    if (!found)
                        return false;
                }
            }
            return true;
        }
        bool matches(const NrModuleV1& m, const ModuleExpectation& e) noexcept {
            return expectedShape(e) && m.moduleKind == e.kind && m.loadLifetimeEpoch == e.loadLifetimeEpoch &&
                   !std::memcmp(m.sha256Words, e.expectedSha256.data(), 32) && e.observedAddress >= m.baseObserved &&
                   e.observedAddress - m.baseObserved < m.imageSize;
        }
        bool find(const NrPacketV1& p, const ModuleExpectation& e, uint64_t address, uint64_t& index) noexcept {
            if (e.observedAddress != address)
                return false;
            for (uint64_t i = 0; i < p.header.moduleCount; ++i)
                if (matches(p.modules[i], e)) {
                    index = i;
                    return true;
                }
            return false;
        }
    } // namespace
    bool appendModuleObservation(NrPacketV1& p,
                                 const NrModuleV1& observed,
                                 const NrCallV1* pin,
                                 const ModuleExpectation& expected) noexcept {
        const auto index = p.header.moduleCount;
        if (!packetShape(p) || index >= 8 || !moduleShape(observed) || !matches(observed, expected) ||
            expected.observedAddress - observed.baseObserved != observed.addressRva ||
            (index < 3 && observed.moduleKind != index + 1) ||
            (observed.pinObserved ? (!pin || p.header.callCount >= 160 || !pinShape(*pin, expected.observedAddress))
                                  : pin != nullptr))
            return false;
        for (uint64_t i = 0; i < index; ++i) {
            const auto& earlier = p.modules[i];
            if (earlier.moduleKind == observed.moduleKind || !compatibleImages(earlier, observed))
                return false;
        }
        NrCallV1 projected{};
        if (pin) {
            projected = *pin;
            projected.moduleIndex = index;
        }
        // All rejection checks precede this bounded, allocation-free commit.
        p.modules[p.header.moduleCount++] = observed;
        if (pin)
            p.calls[p.header.callCount++] = projected;
        return true;
    }
    bool matchPacketModule(const NrPacketV1& p, const ModuleExpectation& expected, uint64_t index) noexcept {
        return packetShape(p) && index < p.header.moduleCount && matches(p.modules[index], expected);
    }
    bool bindSdkModules(const NrPacketV1& p,
                        const NrCallV1& call,
                        const SdkModuleExpectations& expected,
                        SdkDispatchBinding& binding) noexcept {
        if (!packetShape(p) || p.header.moduleCount < 3 ||
            (call.kind != NR_CREATE_SDK && call.kind != NR_CREATE_MANUAL && call.kind != NR_INIT_MANUAL &&
             call.kind != NR_EVALUATE && call.kind != NR_RELEASE_FEATURE))
            return false;
        SdkDispatchBinding actual{};
        if (!find(p, expected.dispatch, call.dispatchAddressObserved, actual.dispatchModule))
            return false;
        if (call.kind == NR_INIT_MANUAL) {
            if (call.actual.pairedEvaluateAddress || call.aux.pairedReleaseAddress ||
                !emptyExpected(expected.evaluate) || !emptyExpected(expected.release))
                return false;
        } else {
            if (!find(p, expected.evaluate, call.actual.pairedEvaluateAddress, actual.evaluateModule) ||
                !find(p, expected.release, call.aux.pairedReleaseAddress, actual.releaseModule) ||
                (call.kind == NR_EVALUATE && call.dispatchAddressObserved != call.actual.pairedEvaluateAddress) ||
                (call.kind == NR_RELEASE_FEATURE && call.dispatchAddressObserved != call.aux.pairedReleaseAddress))
                return false;
        }
        binding = actual;
        return true;
    }
    bool matchSourceModule(const NrPacketV1& p,
                           const NrSourceWitnessV1& source,
                           const ModuleExpectation& expected,
                           uint64_t index) noexcept {
        NrError error{};
        if (!nr_validate_source(&source, sizeof source, &error) || source.header.sourceModuleIndex != index ||
            !matchPacketModule(p, expected, index))
            return false;
        for (const auto& copy : source.copies)
            if (copy.callerRva >= p.modules[index].imageSize)
                return false;
        return true;
    }
    bool observePacketModule(const void* address,
                             const ModuleExpectation& expected,
                             bool pin,
                             NrModuleV1& observed,
                             NrCallV1& pinCall) noexcept {
        observed = {};
        pinCall = {};
        if (!expectedShape(expected) || reinterpret_cast<uint64_t>(address) != expected.observedAddress ||
            (expected.kind <= 2 && !pin))
            return false;
        return observePrivateModule(
            address, expected.kind, expected.expectedSha256.data(), pin, observed, pinCall, expected.loadLifetimeEpoch);
    }
} // namespace virtualdesktop_openxr::nr_capture
