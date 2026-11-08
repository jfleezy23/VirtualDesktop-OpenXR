#pragma once
#include "../nr-frame-generation/include/pair_capture_nr_abi.h"
namespace virtualdesktop_openxr::nr_capture {
    // Fixed scalar provenance from original source creation, QI, export, import,
    // and release events. No observer owns or dereferences these numeric identities.
    struct SourceTextureFacts {
        uint64_t swapchain{}, swapchainLifetime{}, imageIndex{};
        uint64_t producer{}, producerLifetime{}, producerDevice{};
        uint64_t application{}, applicationLifetime{}, applicationDevice{}, importInterfaceThis{};
        uint64_t sharedHandle{}, queryInterfaceThis{}, releaseEpoch{}, releasedIndex{};
        uint64_t queryBegin{}, queryEnd{}, exportBegin{}, exportEnd{}, importBegin{}, importEnd{}, thread{};
        uint32_t exportAccess{};
        int32_t queryResult{}, exportResult{}, importResult{};
        bool queryObserved{}, exportObserved{}, importObserved{}, exportNt{}, importNt{}, live{};
    };
    inline bool sourceAliasComplete(const SourceTextureFacts& r,
                                    uint64_t producer,
                                    uint64_t application,
                                    uint64_t releasedIndex) noexcept {
        return r.producer == producer && producer && r.application == application && application &&
               r.producerLifetime && r.applicationLifetime && r.producerDevice && r.applicationDevice &&
               r.sharedHandle && r.queryObserved && r.queryResult >= 0 && r.queryInterfaceThis && r.exportObserved &&
               r.exportResult >= 0 && r.importObserved && r.importResult >= 0 && r.importInterfaceThis &&
               r.exportNt == r.importNt && r.exportAccess == (r.exportNt ? UINT64_C(0x10000000) : 0) &&
               r.releaseEpoch && r.releasedIndex == releasedIndex;
    }
    // API5 joins an observed producer Texture11 bank to the actual imported
    // Resource12 interface. It has no app release-index claim.
    inline bool nrAliasComplete(const SourceTextureFacts& r,
                                uint64_t source,
                                uint64_t target,
                                uint64_t sourceDevice,
                                uint64_t nrDevice) noexcept {
        return r.live && source && target && r.producer == source && r.application == target && r.producerLifetime &&
               r.applicationLifetime && r.producerDevice == sourceDevice && sourceDevice &&
               r.applicationDevice == nrDevice && nrDevice && r.importInterfaceThis == nrDevice && r.sharedHandle &&
               r.queryObserved && r.queryResult >= 0 && r.queryInterfaceThis && r.exportObserved &&
               r.exportResult >= 0 && r.importObserved && r.importResult >= 0 && r.exportNt == r.importNt &&
               r.exportAccess == (r.exportNt ? UINT64_C(0x10000000) : 0);
    }
    inline NrCallV1 sourceExportCall(const SourceTextureFacts& r) noexcept {
        NrCallV1 c{};
        c.kind = 16;
        c.beginObserved = r.exportObserved;
        c.endObserved = r.exportObserved;
        c.resultType = NR_HRESULT;
        c.resultBits = static_cast<uint32_t>(r.exportResult);
        c.classifiedSuccess = r.exportObserved && r.exportResult >= 0;
        c.qpcBegin = r.exportBegin;
        c.qpcEnd = r.exportEnd;
        c.thread = r.thread;
        c.eye = 2;
        c.deviceAddress = r.producerDevice;
        c.queueAddress = r.queryInterfaceThis;
        c.fenceAddress = r.producer;
        c.flags = r.exportNt ? 1 : 0;
        c.actual.actualCountArgument = (uint64_t(static_cast<uint32_t>(r.queryResult)) << 32) | r.exportAccess;
        c.aux.auxiliaryValue = r.sharedHandle;
        c.observedMask = r.exportObserved ? NR_CALL_OBS : 0;
        return c;
    }
    inline NrCallV1 sourceImportCall(const SourceTextureFacts& r) noexcept {
        NrCallV1 c{};
        c.kind = 17;
        c.beginObserved = r.importObserved;
        c.endObserved = r.importObserved;
        c.resultType = NR_HRESULT;
        c.resultBits = static_cast<uint32_t>(r.importResult);
        c.classifiedSuccess = r.importObserved && r.importResult >= 0;
        c.qpcBegin = r.importBegin;
        c.qpcEnd = r.importEnd;
        c.thread = r.thread;
        c.eye = 2;
        c.deviceAddress = r.applicationDevice;
        c.queueAddress = r.importInterfaceThis;
        c.fenceAddress = r.application;
        c.flags = r.importNt ? 1 : 0;
        c.aux.auxiliaryValue = r.sharedHandle;
        c.observedMask = r.importObserved ? NR_CALL_OBS : 0;
        return c;
    }
} // namespace virtualdesktop_openxr::nr_capture
