#include "PairCaptureNrWrites.h"
#include "PairCaptureNrMetadataBudget.h"
#include <Windows.h>
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        std::mutex writeMutex;
        ResourceWriteTable writes;
        bool invalid{};
    } // namespace
    static_assert(sizeof(ResourceWriteTable) + sizeof(std::mutex) + sizeof(bool) <= hostMetadataWrites);
    bool observeOriginalWrite(uint64_t resource,
                              uint64_t lifetime,
                              uint64_t serial,
                              uint64_t contextOrList,
                              uint64_t recordingEpoch,
                              uint64_t api,
                              uint64_t kind,
                              uint64_t actualCallSequence,
                              uint64_t sourceAddress) noexcept {
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        ResourceWriteFact fact{};
        fact.resource = resource;
        fact.lifetime = lifetime;
        fact.serial = serial;
        fact.contextOrList = contextOrList;
        fact.recordingEpoch = recordingEpoch;
        fact.api = api;
        fact.kind = kind;
        fact.actualCallSequence = actualCallSequence;
        fact.sourceAddress = sourceAddress;
        fact.thread = GetCurrentThreadId();
        fact.qpc = static_cast<uint64_t>(q.QuadPart);
        std::lock_guard lock(writeMutex);
        if (invalid || !writes.observe(fact)) {
            invalid = true;
            return false;
        }
        return true;
    }
    OriginalWriteStatus originalWriteStatus(uint64_t resource, uint64_t lifetime, ResourceWriteFact& out) noexcept {
        out = {};
        std::lock_guard lock(writeMutex);
        if (invalid)
            return OriginalWriteStatus::invalid;
        return writes.latest(resource, lifetime, out) ? OriginalWriteStatus::observed : OriginalWriteStatus::absent;
    }
    bool latestOriginalWrite(uint64_t resource, uint64_t lifetime, ResourceWriteFact& out) noexcept {
        return originalWriteStatus(resource, lifetime, out) == OriginalWriteStatus::observed;
    }
} // namespace virtualdesktop_openxr::nr_capture
