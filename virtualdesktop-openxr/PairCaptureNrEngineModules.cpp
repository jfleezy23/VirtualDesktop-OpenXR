#include "PairCaptureNrEngine.h"
#include "PairCaptureNrModules.h"
#include <cstring>
#include <memory>
#include <new>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct WarmModuleRows {
            SdkObservation rows[6]{};
        };
        bool sampleModuleOwner(SdkWarmFacts& warm,
                               ManualModuleObservation& manual,
                               SdkObservation (&rows)[6],
                               size_t& count,
                               uint64_t& sdkAnchor) noexcept {
            if (!hostRequest().valid || !sdkWarmFacts(warm) || warm.invalid || !warm.completeFrame || !warm.tableLive ||
                !warm.featureLive[0] || !warm.featureLive[1])
                return false;
            // Check the current original owner before any cached image/anchor access.
            // This copied journal is not itself a Runtime/module/feature lifetime lease.
            if (!copyManualModuleObservation(warm.evaluateAddress[0], manual) || !manual.ownerLive ||
                !manual.publicationEpoch || manual.functions[3] != warm.evaluateAddress[0] ||
                manual.functions[3] != warm.evaluateAddress[1] || manual.functions[4] != warm.releaseAddress[0] ||
                manual.functions[4] != warm.releaseAddress[1] || !sdkCopyObservations(rows, 6, count) || count > 6)
                return false;
            sdkAnchor = 0;
            for (size_t i = 0; i < count; ++i) {
                const auto& row = rows[i];
                if (row.call.kind == NR_CREATE_SDK && row.ended && !row.threw && row.call.beginObserved &&
                    row.call.endObserved) {
                    if (sdkAnchor && sdkAnchor != row.call.dispatchAddressObserved)
                        return false;
                    sdkAnchor = row.call.dispatchAddressObserved;
                }
            }
            return sdkAnchor != 0;
        }
        bool sameManualOwner(const ManualModuleObservation& a, const ManualModuleObservation& b) noexcept {
            return a.ownerLive && b.ownerLive && a.moduleAddress == b.moduleAddress &&
                   a.loadLifetimeEpoch == b.loadLifetimeEpoch && a.loadBegin == b.loadBegin && a.loadEnd == b.loadEnd &&
                   a.loadThread == b.loadThread && a.publicationEpoch == b.publicationEpoch &&
                   a.publicationQpc == b.publicationQpc && a.publicationThread == b.publicationThread &&
                   !std::memcmp(a.functions, b.functions, sizeof a.functions);
        }
        bool sameSdkOwner(const SdkWarmFacts& a, const SdkWarmFacts& b) noexcept {
            if (a.tableAddress != b.tableAddress || a.tableLifetime != b.tableLifetime)
                return false;
            for (size_t eye = 0; eye < 2; ++eye)
                if (a.featureAddress[eye] != b.featureAddress[eye] ||
                    a.featureLifetime[eye] != b.featureLifetime[eye] ||
                    a.evaluateAddress[eye] != b.evaluateAddress[eye] || a.releaseAddress[eye] != b.releaseAddress[eye])
                    return false;
            return true;
        }
        bool moduleInputs(const NrModuleV1 (&pinned)[2],
                          const NrCallV1 (&pins)[2],
                          const ManualModuleObservation& manual,
                          uint64_t sdkAnchor,
                          ModuleExpectation (&expected)[6],
                          uint64_t (&bases)[6]) noexcept {
            const auto& request = hostRequest();
            const auto set =
                [&](size_t i, uint64_t kind, uint64_t life, uint64_t anchor, const uint8_t (&hash)[32]) noexcept {
                    expected[i].kind = kind;
                    expected[i].loadLifetimeEpoch = life;
                    expected[i].observedAddress = anchor;
                    std::memcpy(expected[i].expectedSha256.data(), hash, 32);
                };
            set(0, 1, pinned[0].loadLifetimeEpoch, pins[0].dispatchAddressObserved, request.hostHash);
            set(1, 2, pinned[1].loadLifetimeEpoch, pins[1].dispatchAddressObserved, request.fgHash);
            set(2, 3, manual.loadLifetimeEpoch, manual.functions[2], request.modelHash);
            set(3, 4, pinned[0].loadLifetimeEpoch, sdkAnchor, request.hostHash);
            const HMODULE bridge = GetModuleHandleW(request.sourceBridgePath);
            if (!bridge)
                return false;
            const auto factory = GetProcAddress(bridge, "VRClientCoreFactory");
            const HMODULE author = GetModuleHandleW(nullptr);
            if (!factory || !author)
                return false;
            set(4, 5, 1, reinterpret_cast<uint64_t>(factory), request.sourceHash);
            set(5, 6, 1, reinterpret_cast<uint64_t>(author), request.authorHash);
            bases[0] = pinned[0].baseObserved;
            bases[1] = pinned[1].baseObserved;
            bases[2] = manual.moduleAddress;
            bases[3] = pinned[0].baseObserved;
            bases[4] = reinterpret_cast<uint64_t>(bridge);
            bases[5] = reinterpret_cast<uint64_t>(author);
            return true;
        }
        bool validateModules(const ModuleExpectation (&expected)[6],
                             const uint64_t (&bases)[6],
                             const NrModuleV1* modules,
                             const ModuleFileLease (&files)[6]) noexcept {
            for (size_t i = 0; i < 6; ++i) {
                const auto& e = expected[i];
                if (modules[i].baseObserved != bases[i] ||
                    !validateLeasedPrivateModule(reinterpret_cast<const void*>(e.observedAddress),
                                                 e.kind,
                                                 e.expectedSha256.data(),
                                                 e.loadLifetimeEpoch,
                                                 modules[i],
                                                 files[i]))
                    return false;
            }
            return modules[3].imageSize == modules[0].imageSize;
        }
    } // namespace
    bool CaptureEngine::attestWarmModules() noexcept {
        // Original producer Runtime/module/feature lease is held by the caller.
        // No Control, SDK observer or pool lock is held during file I/O/PIN calls.
        if (attempted_ || prepared_)
            return false;
        std::lock_guard fileLock(moduleFilesMutex_);
        SdkWarmFacts warm{};
        ManualModuleObservation manual{};
        size_t count{};
        uint64_t sdkAnchor{};
        NrModuleV1 pinned[2]{};
        NrCallV1 pins[2]{};
        ModuleExpectation expected[6]{};
        uint64_t bases[6]{};
        const auto discard = [&]() noexcept {
            for (auto& file : moduleFiles_)
                file.reset();
            modulesAttested_ = false;
            moduleCount_ = 0;
            return false;
        };
        const std::unique_ptr<WarmModuleRows> scratch(new (std::nothrow) WarmModuleRows{});
        if (!scratch)
            return discard();
        auto& rows = scratch->rows;
        if (!sampleModuleOwner(warm, manual, rows, count, sdkAnchor) || !hostPinnedModules(pinned, pins) ||
            !moduleInputs(pinned, pins, manual, sdkAnchor, expected, bases))
            return discard();
        if (modulesAttested_ && sameManualOwner(manual, manualModule_) &&
            validateModules(expected, bases, modules_, moduleFiles_)) {
            // FG history/session requalification does not change immutable bytes
            // or fabricate another process-lifetime PIN observation.
            moduleSdkIdentity_ = warm;
            return true;
        }
        discard();
        ModuleFileLease files[6];
        NrModuleV1 modules[6]{};
        NrCallV1 calls[6]{};
        for (size_t i = 0; i < 6; ++i) {
            const auto& e = expected[i];
            const bool processPin = i >= 4;
            const size_t pinSlot = processPin ? i - 4 : 0;
            const bool alreadyPinned = processPin && processPinCalls_[pinSlot].kind;
            if (alreadyPinned && (processPinModules_[pinSlot].baseObserved != bases[i] ||
                                  processPinCalls_[pinSlot].dispatchAddressObserved != e.observedAddress))
                return false;
            const bool observed = observeLeasedPrivateModule(reinterpret_cast<const void*>(e.observedAddress),
                                                             e.kind,
                                                             e.expectedSha256.data(),
                                                             processPin && !alreadyPinned,
                                                             modules[i],
                                                             calls[i],
                                                             e.loadLifetimeEpoch,
                                                             files[i]);
            if (calls[i].kind)
                calls[i].moduleIndex = i;
            // A successful PIN is permanent even when a later warm stage fails.
            if (processPin && calls[i].kind && calls[i].classifiedSuccess) {
                processPinModules_[pinSlot] = modules[i];
                processPinCalls_[pinSlot] = calls[i];
            }
            if (!observed || modules[i].baseObserved != bases[i])
                return false;
            if (i < 2) {
                if (modules[i].imageSize != pinned[i].imageSize)
                    return false;
                modules[i].pinObserved = pinned[i].pinObserved;
                modules[i].pinResult = pinned[i].pinResult;
                calls[i] = pins[i];
            } else if (alreadyPinned) {
                modules[i].pinObserved = processPinModules_[pinSlot].pinObserved;
                modules[i].pinResult = processPinModules_[pinSlot].pinResult;
                calls[i] = processPinCalls_[pinSlot];
            }
            if (calls[i].kind)
                calls[i].moduleIndex = i;
        }
        if (modules[3].imageSize != modules[0].imageSize)
            return false;
        for (size_t i = 0; i < 6; ++i) {
            moduleFiles_[i].take(files[i]);
            modules_[i] = modules[i];
            modulePinCalls_[i] = calls[i];
            moduleExpectations_[i] = expected[i];
        }
        manualModule_ = manual;
        moduleSdkIdentity_ = warm;
        moduleCount_ = 6;
        modulesAttested_ = true;
        return true;
    }
    bool CaptureEngine::prepareModules() noexcept {
        std::lock_guard fileLock(moduleFilesMutex_);
        const auto reject = [&]() noexcept {
            for (auto& file : moduleFiles_)
                file.reset();
            modulesAttested_ = false;
            moduleCount_ = 0;
            return false;
        };
        SdkWarmFacts warm{};
        ManualModuleObservation manual{};
        const std::unique_ptr<WarmModuleRows> scratch(new (std::nothrow) WarmModuleRows{});
        if (!scratch)
            return reject();
        auto& rows = scratch->rows;
        size_t count{};
        uint64_t sdkAnchor{};
        ModuleExpectation expected[6]{};
        uint64_t bases[6]{};
        if (!modulesAttested_ || moduleCount_ != 6 || !sampleModuleOwner(warm, manual, rows, count, sdkAnchor) ||
            !sameManualOwner(manual, manualModule_) || !sameSdkOwner(warm, moduleSdkIdentity_) ||
            !moduleInputs(pinned_, pinCalls_, manual, sdkAnchor, expected, bases) ||
            !validateModules(expected, bases, modules_, moduleFiles_))
            return reject();
        const std::unique_ptr<SdkObservation[]> lifecycle(new (std::nothrow) SdkObservation[64]{});
        if (!lifecycle)
            return reject();
        size_t lifecycleCount{};
        if (!sdkCopyLifecycle(lifecycle.get(), 64, lifecycleCount) || lifecycleCount > 64)
            return reject();
        // Commit only fresh selected SDK evidence after every cheap identity check.
        // Cached module/hash/PIN observations retain their genuine warm chronology.
        std::memcpy(sdkLifecycle_, lifecycle.get(), lifecycleCount * sizeof(SdkObservation));
        std::memcpy(sdkWarmRows_, rows, count * sizeof *rows);
        sdkLifecycleCount_ = static_cast<uint32_t>(lifecycleCount);
        sdkWarmRowCount_ = static_cast<uint32_t>(count);
        warmSdkWitness_ = warm.totalSequence;
        return true;
    }
    void CaptureEngine::releaseModuleFiles() noexcept {
        // Outside Control.mutex, after receiver settlement and all six releases.
        // Uncertain selected children retain leases until process cleanup, like the
        // existing diagnostic owner; unselected replacement closes them above.
        std::lock_guard fileLock(moduleFilesMutex_);
        for (auto& file : moduleFiles_)
            file.reset();
        modulesAttested_ = false;
    }
} // namespace virtualdesktop_openxr::nr_capture
