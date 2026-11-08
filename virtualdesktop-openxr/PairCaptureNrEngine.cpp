#include "PairCaptureNrEngine.h"
#include "PairCaptureNrMetadataBudget.h"
#include "../nr-frame-generation/include/pair_capture_nr_budget.hpp"
#include <atomic>
#include <cstring>
#include <new>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        std::atomic<uint64_t> nextSourceSeal{1}, nextRawDescription{1}, nextPrivateFence{1}, nextOwnerReservation{1},
            nextPrivateExposure{1};
        uint64_t timestamp() noexcept {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return static_cast<uint64_t>(value.QuadPart);
        }
        bool armFileMatches(const wchar_t* path, const NrReadyV1& ready) noexcept {
            const HANDLE file = CreateFileW(
                path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER length{};
            char bytes[512]{};
            DWORD read{};
            const bool complete = GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart < 512 &&
                                  ReadFile(file, bytes, static_cast<DWORD>(length.QuadPart), &read, nullptr) &&
                                  read == static_cast<DWORD>(length.QuadPart);
            const bool closed = CloseHandle(file) != FALSE;
            return complete && closed && exactCaptureArm(bytes, read, ready);
        }
        bool sameTexture(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) noexcept {
            return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
                   a.ArraySize == b.ArraySize && a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
                   a.SampleDesc.Quality == b.SampleDesc.Quality && a.Usage == b.Usage && a.BindFlags == b.BindFlags &&
                   a.CPUAccessFlags == b.CPUAccessFlags && a.MiscFlags == b.MiscFlags;
        }
        const WarmRoleLayout* warmRole(const WarmFrameLayout& w, uint64_t role, uint64_t eye) noexcept {
            for (uint32_t i = 0; i < w.count; ++i)
                if (w.roles[i].role == role && w.roles[i].eye == eye)
                    return &w.roles[i];
            return nullptr;
        }
    } // namespace
    CaptureEngine& captureEngine() noexcept {
        // Process-lifetime diagnostic owner. A terminal child retains uncertain
        // work through process exit; static destruction must not release it early.
        alignas(CaptureEngine) static unsigned char storage[sizeof(CaptureEngine)]{};
        static auto* engine = ::new (static_cast<void*>(storage)) CaptureEngine;
        return *engine;
    }
    bool CaptureEngine::rejectUnexposed(uint64_t reason) noexcept {
        const bool discarded = snapshots_.discardUnexposed();
        // This method is only used before owner acquisition or command exposure.
        initialFgFence_.Reset();
        initialAppFence_.Reset();
        initialProducerFence_.Reset();
        prepared_ = false;
        releaseModuleFiles();
        hostCaptureTerminal(reason, !discarded);
        return false;
    }
    bool CaptureEngine::createPrivateFence(const CapturePreflightInput& input) noexcept {
        auto& f = fenceFacts_;
        f.thread = GetCurrentThreadId();
        f.producerDevice = reinterpret_cast<uint64_t>(input.producerDevice);
        f.applicationDevice = reinterpret_cast<uint64_t>(input.applicationDevice);
        f.fgDevice = reinterpret_cast<uint64_t>(input.fgDevice);
        f.createBegin = timestamp();
        const HRESULT created = input.producerDevice->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(initialProducerFence_.GetAddressOf()));
        f.createEnd = timestamp();
        f.createResult = created;
        f.createObserved = true;
        f.producerFence = reinterpret_cast<uint64_t>(initialProducerFence_.Get());
        if (FAILED(created) || !initialProducerFence_ ||
            !vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextPrivateFence, f.producerLifetime))
            return false;
        producerFence_ = initialProducerFence_.Get();
        HANDLE handle{};
        f.exportBegin = timestamp();
        const HRESULT exported = initialProducerFence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
        f.exportEnd = timestamp();
        f.exportResult = exported;
        f.exportObserved = true;
        f.handle = reinterpret_cast<uint64_t>(handle);
        if (FAILED(exported) || !handle) {
            if (handle)
                f.handleClosed = CloseHandle(handle) != FALSE;
            return false;
        }
        f.appImportBegin = timestamp();
        const HRESULT appImported =
            input.applicationDevice->OpenSharedFence(handle, IID_PPV_ARGS(initialAppFence_.GetAddressOf()));
        f.appImportEnd = timestamp();
        f.appImportResult = appImported;
        f.appImportObserved = true;
        f.applicationFence = reinterpret_cast<uint64_t>(initialAppFence_.Get());
        if (SUCCEEDED(appImported) && initialAppFence_) {
            appFence_ = initialAppFence_.Get();
            if (appFence_ == producerFence_)
                f.applicationLifetime = f.producerLifetime;
            else if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextPrivateFence, f.applicationLifetime)) {
                f.handleClosed = CloseHandle(handle) != FALSE;
                return false;
            }
            f.fgImportBegin = timestamp();
            const HRESULT fgImported =
                input.fgDevice->OpenSharedHandle(handle, IID_PPV_ARGS(initialFgFence_.GetAddressOf()));
            f.fgImportEnd = timestamp();
            f.fgImportResult = fgImported;
            f.fgImportObserved = true;
            f.fgFence = reinterpret_cast<uint64_t>(initialFgFence_.Get());
            if (SUCCEEDED(fgImported) && initialFgFence_) {
                fgFence_ = initialFgFence_.Get();
                if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextPrivateFence, f.fgLifetime)) {
                    f.handleClosed = CloseHandle(handle) != FALSE;
                    return false;
                }
            }
        }
        f.handleClosed = CloseHandle(handle) != FALSE;
        return f.handleClosed && f.appImportObserved && f.appImportResult >= 0 && f.applicationFence &&
               f.fgImportObserved && f.fgImportResult >= 0 && f.fgFence;
    }
    bool CaptureEngine::observeRawCohort(uint32_t frame, const CapturePreflightInput& input) noexcept {
        if (frame > 1)
            return false;
        if (!hostLatestSource(sources_[frame]) || sources_[frame].header.endThread != GetCurrentThreadId() ||
            !sources_[frame].header.applicationSubmissionId || !sources_[frame].header.publicWitnessId)
            return false;
        NrError error{};
        if (!nr_validate_source(&sources_[frame], sizeof sources_[frame], &error))
            return false;
        if (moduleCount_ == 6)
            for (const auto& copy : sources_[frame].copies)
                if (sources_[frame].header.sourceModuleIndex != 4 || copy.callerRva >= modules_[4].imageSize)
                    return false;
        bool sourceRows[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            const auto& raw = input.raw[i];
            // The retained Source640 author contract exports subresource zero.
            // Preserve the actual slice and reject an unsupported selected slice.
            if (!raw.swapchain || !raw.texture || raw.eye > 1 || raw.selectedArraySlice != 0 ||
                (raw.role != NR_RAW_COLOUR && raw.role != NR_RAW_DEPTH))
                return false;
            for (uint32_t j = 0; j < i; ++j)
                if (input.raw[j].role == raw.role && input.raw[j].eye == raw.eye)
                    return false;
            D3D11_TEXTURE2D_DESC creation{}, actual{};
            if (!sourceGet(raw.swapchain, raw.releasedIndex, rawFacts_[frame][i], creation))
                return false;
            const auto& fact = rawFacts_[frame][i];
            if (!sourceAliasComplete(
                    fact, reinterpret_cast<uint64_t>(raw.texture), fact.application, raw.releasedIndex) ||
                fact.producerDevice != reinterpret_cast<uint64_t>(input.producerDevice) ||
                fact.applicationDevice != reinterpret_cast<uint64_t>(input.applicationDevice))
                return false;
            const auto* expected = warmRole(warm_, raw.role, raw.eye);
            raw.texture->GetDesc(&actual);
            if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextRawDescription, rawDescriptorEpoch_[frame]))
                return false;
            if (!expected || expected->api != 11 || !sameTexture(creation, actual) ||
                !sameTexture(expected->texture11, actual))
                return false;
            uint32_t copyIndex{};
            if (!selectRawSourceCopy(sources_[frame],
                                     fact.application,
                                     raw.selectedArraySlice,
                                     actual.ArraySize,
                                     actual.MipLevels,
                                     copyIndex) ||
                sourceRows[copyIndex])
                return false;
            sourceRows[copyIndex] = true;
            rawCopyIndex_[frame][i] = copyIndex;
        }
        return vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextSourceSeal, sourceGeneration_[frame]);
    }
    bool CaptureEngine::prepareResources(const CapturePreflightInput& input) noexcept {
        const auto& request = hostRequest();
        if (attempted_ || !request.valid || !input.serial || input.serial < request.minimumSerial ||
            input.serial == UINT64_MAX || input.displayTime < request.minimumTime || !input.nrContext ||
            !input.nrContextLifetime || !input.producerDevice || !input.producerContext || !input.applicationDevice ||
            !input.applicationContext || !input.nrDevice || !input.fgDevice ||
            input.producerDevice == input.applicationDevice ||
            !captureEntryEligible(input, request.minimumTime, GetCurrentThreadId()))
            return false;
        status_ = {};
        status_.size = sizeof status_;
        status_.version = 1;
        ReceiverCall receiver{};
        HMODULE module{};
        if (!VDXR_PrivatePairNRStatusV1(&status_) || status_.state != NR_HOST_WARM ||
            status_.nrContextLifetimeEpoch != input.nrContextLifetime || !status_.warmSerial ||
            status_.warmSerial >= input.serial || !hostReceiverBinding(ready_, receiver, module) ||
            ready_.deviceAddress != reinterpret_cast<uint64_t>(input.fgDevice) ||
            status_.fgContextLifetimeEpoch != ready_.fgContextLifetimeEpoch ||
            status_.fgHistoryGeneration != ready_.fgHistoryGeneration || !hostPinnedModules(pinned_, pinCalls_) ||
            !armFileMatches(request.armFile, ready_))
            return false;
        if (!warmLayoutCopy(status_.warmSerial, warm_) || !warm_.complete || warm_.invalid ||
            warm_.mode != request.mode || warm_.nrContextLifetime != input.nrContextLifetime ||
            warm_.upscaling != input.upscaling || warm_.sharpening != input.sharpening ||
            warm_.count != requiredWarmRoleCount(request.mode, input.upscaling, input.sharpening))
            return false;
        if (!observeRawCohort(0, input) || !input.originalNrFence || !input.originalOutputAlias ||
            !sourceNrFenceGet(input.nrContext, input.nrContextLifetime, nrFenceFacts_) ||
            nrFenceFacts_.alias.sourceAddress != reinterpret_cast<uint64_t>(input.originalNrFence) ||
            nrFenceFacts_.alias.targetAddress != reinterpret_cast<uint64_t>(input.originalOutputAlias) ||
            nrFenceFacts_.alias.sourceDevice != reinterpret_cast<uint64_t>(input.nrDevice) ||
            nrFenceFacts_.alias.targetDevice != reinterpret_cast<uint64_t>(input.producerDevice) ||
            nrFenceFacts_.alias.observedMask != 3 || !nrFenceFacts_.exportCall.classifiedSuccess ||
            !nrFenceFacts_.importCall.classifiedSuccess)
            return false;
        if (!input.originalInputProducerFence || !input.originalInputAlias ||
            !sourceNrInputFenceGet(input.nrContext, input.nrContextLifetime, nrInputFenceFacts_) ||
            nrInputFenceFacts_.alias.sourceAddress != reinterpret_cast<uint64_t>(input.originalInputProducerFence) ||
            nrInputFenceFacts_.alias.targetAddress != reinterpret_cast<uint64_t>(input.originalInputAlias) ||
            nrInputFenceFacts_.alias.sourceDevice != reinterpret_cast<uint64_t>(input.producerDevice) ||
            nrInputFenceFacts_.alias.targetDevice != reinterpret_cast<uint64_t>(input.nrDevice) ||
            nrInputFenceFacts_.alias.observedMask != 3 || !nrInputFenceFacts_.exportCall.classifiedSuccess ||
            !nrInputFenceFacts_.importCall.classifiedSuccess)
            return false;
        const auto budgetAddress = GetProcAddress(module, "VDXRFG_PrivatePairNRBudgetV1");
        const auto submitAddress = GetProcAddress(module, "VDXRFG_PrivatePairNRSubmitWithPostFxV1");
        const auto sourceAddress = GetProcAddress(module, "VDXRFG_PrivatePairNRSourceV1");
        if (!privateExportInModule(reinterpret_cast<uint64_t>(budgetAddress), pinned_[1]) ||
            !privateExportInModule(reinterpret_cast<uint64_t>(submitAddress), pinned_[1]) ||
            !privateExportInModule(reinterpret_cast<uint64_t>(sourceAddress), pinned_[1]))
            return false;
        fgBudgetCall_ = reinterpret_cast<BudgetCall>(budgetAddress);
        submit_ = reinterpret_cast<SubmitCall>(submitAddress);
        source_ = reinterpret_cast<SourceCall>(sourceAddress);
        fgBudget_ = {};
        fgBudget_.size = sizeof fgBudget_;
        fgBudget_.version = 1;
        fgBudget_.attempt = ready_.attempt;
        fgBudget_.nonce[0] = ready_.nonceLo;
        fgBudget_.nonce[1] = ready_.nonceHi;
        fgBudget_.pid = ready_.pid;
        fgBudget_.creationFiletime = ready_.creationFiletime;
        fgBudget_.sessionNonce[0] = ready_.sessionNonceLo;
        fgBudget_.sessionNonce[1] = ready_.sessionNonceHi;
        fgBudget_.fgContextAddress = ready_.fgContextAddress;
        fgBudget_.fgContextLifetimeEpoch = ready_.fgContextLifetimeEpoch;
        fgBudget_.fgHistoryGeneration = ready_.fgHistoryGeneration;
        fgBudget_.actualNrMode = ready_.mode;
        // Foreign query observes real warm resource layouts. No observer lock held.
        const auto fgBudget = fgBudgetCall_;
        if (fgBudget(&fgBudget_) != 1 || !nr_validate_fg_budget(&fgBudget_, sizeof fgBudget_) ||
            !nr_fg_budget_matches_ready(&fgBudget_, &ready_))
            return false;
        for (uint32_t i = 0; i < 2; ++i) {
            auto& call = luidCalls_[i];
            call = {};
            call.kind = NR_DEVICE_LUID;
            call.eye = 2;
            call.deviceAddress = reinterpret_cast<uint64_t>(i ? input.fgDevice : input.nrDevice);
            call.resultType = NR_VOID;
            call.beginObserved = 1;
            call.thread = GetCurrentThreadId();
            call.qpcBegin = timestamp();
            const LUID actual = i ? input.fgDevice->GetAdapterLuid() : input.nrDevice->GetAdapterLuid();
            call.qpcEnd = timestamp();
            call.endObserved = 1;
            call.observedMask = NR_CALL_OBS;
            call.actual.actualCountArgument = actual.LowPart;
            call.aux.auxiliaryValue = static_cast<uint32_t>(actual.HighPart);
        }
        if (luidCalls_[0].actual.actualCountArgument != luidCalls_[1].actual.actualCountArgument ||
            luidCalls_[0].aux.auxiliaryValue != luidCalls_[1].aux.auxiliaryValue)
            return false;
        luidLow_ = luidCalls_[0].actual.actualCountArgument;
        luidHigh_ = luidCalls_[0].aux.auxiliaryValue;
        // Full file attestation and genuine PIN calls already completed in warmup.
        // Selection only validates their held identities and samples current SDK facts.
        if (!hostLifetimeFacts(hostLifetime_) || hostLifetime_.instanceLifetime != status_.hostLifetimeEpoch)
            return false;
        if (!prepareModules()) {
            hostInvalidateWarm();
            return false;
        }
        attempted_ = true;
        for (uint32_t slot = 0; slot < warm_.count; ++slot) {
            const auto& r = warm_.roles[slot];
            bool planned = false;
            if (r.api == 11)
                planned = snapshots_.plan11(
                    slot, r.role, r.eye, r.role == NR_RAW_COLOUR || r.role == NR_RAW_DEPTH ? 0 : 2, r.texture11);
            else if (r.api == 12)
                planned = snapshots_.plan12(slot, r.role, r.eye, input.nrDevice, r.texture12);
            if (!planned)
                return rejectUnexposed(NR_BAD_ROLE);
        }
        if (!snapshots_.sealPlan(warm_.count, fgBudget_.totalAllocation, fgBudget_.totalPayload, budget_))
            return rejectUnexposed(NR_BAD_BUDGET);
        ownerIdentity_.nonceLow = request.nonce[0];
        ownerIdentity_.nonceHigh = request.nonce[1];
        ownerIdentity_.pid = static_cast<uint32_t>(request.pid);
        ownerIdentity_.creation = request.creation;
        ownerIdentity_.sessionLow = ready_.sessionNonceLo;
        ownerIdentity_.sessionHigh = ready_.sessionNonceHi;
        ownerIdentity_.fgContext = ready_.fgContextAddress;
        ownerIdentity_.fgGeneration = ready_.fgHistoryGeneration;
        ownerIdentity_.nrContext = reinterpret_cast<uintptr_t>(input.nrContext);
        ownerIdentity_.nrGeneration = input.nrContextLifetime;
        if (!hostWorkOwner().reserveAttempt(
                request.attempt, ownerIdentity_, input.serial, captureByteCap, budget_.bytesA, budget_.bytesB))
            return rejectUnexposed(NR_BAD_BUDGET);
        ownerReserved_ = true;
        // Control owns one source-observed host instance and never replaces it:
        // hostInstanceCreated rejects a second instance. Revalidate that actual
        // lease after this global WorkOwner's successful reservation.
        if (!hostLifetimeFacts(reservedHost_) || reservedHost_.instance != hostLifetime_.instance ||
            reservedHost_.instanceLifetime != hostLifetime_.instanceLifetime ||
            reservedHost_.sessionHandle != hostLifetime_.sessionHandle ||
            reservedHost_.sessionLifetime != hostLifetime_.sessionLifetime)
            return rejectUnexposed(NR_BAD_IDENTITY);
        reservedOwnerNumericId_ = reinterpret_cast<uint64_t>(&hostWorkOwner());
        reservedOwnerAttempt_ = request.attempt;
        // Wire ownerHostEpoch is a reference to the host that owns this reservation.
        // The independent reservation event is internal and must not occupy it.
        ownerHostEpoch_ = reservedHost_.instanceLifetime;
        if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextOwnerReservation, reservationObservationEpoch_))
            return rejectUnexposed(NR_BAD_IDENTITY);
        if (!createPrivateFence(input) || !snapshots_.allocate(input.producerDevice, input.nrDevice))
            return rejectUnexposed(NR_BAD_REF);
        preparedInput_ = input;
        firstSerial_ = input.serial;
        if (!completePreflight(input))
            return rejectUnexposed(NR_BAD_BUDGET);
        prepared_ = true;
        return true;
    }
    bool CaptureEngine::completePreflight(const CapturePreflightInput& input) noexcept {
        SdkWarmFacts actual{};
        HostLifetimeFacts current{};
        if (!ownerReserved_ || !reservationObservationEpoch_ || !submit_ || !source_ || !fgBudgetCall_ ||
            moduleCount_ != 6 || !warm_.complete || warm_.invalid || !warm_.count || warm_.count > 28 ||
            snapshots_.count() != warm_.count || snapshots_.initialReferences(0) != warm_.count ||
            snapshots_.initialReferences(1) != warm_.count || !producerFence_ || !appFence_ || !fgFence_ ||
            !fenceFacts_.handleClosed || !fenceFacts_.fgImportObserved || fenceFacts_.fgImportResult < 0 ||
            !fenceFacts_.appImportObserved || fenceFacts_.appImportResult < 0 || !sdkWarmFacts(actual) ||
            actual.invalid || !actual.completeFrame || !actual.tableLive || !actual.featureLive[0] ||
            !actual.featureLive[1] || !warmSdkWitness_ || !hostLifetimeFacts(current) ||
            current.instance != reservedHost_.instance || current.instanceLifetime != reservedHost_.instanceLifetime ||
            current.sessionHandle != reservedHost_.sessionHandle ||
            current.sessionLifetime != reservedHost_.sessionLifetime ||
            input.nrContextLifetime != warm_.nrContextLifetime ||
            !nr_validate_fg_budget(&fgBudget_, sizeof fgBudget_) || !nr_fg_budget_matches_ready(&fgBudget_, &ready_) ||
            !budget_.allArithmeticChecked || budget_.preflightComplete || budget_.firstExposureSequence ||
            budget_.cpuMetadataReservation != 8 * 1024 * 1024 || budget_.physicalPrivatePeakBytes > captureByteCap)
            return false;
        for (const auto& copy : sources_[0].copies)
            if (sources_[0].header.sourceModuleIndex != 4 || copy.callerRva >= modules_[4].imageSize)
                return false;
        uint64_t bytes[2]{}, payload[2]{};
        uint32_t counts[3]{};
        for (uint32_t frame = 0; frame < 2; ++frame)
            for (uint32_t slot = 0; slot < warm_.count; ++slot) {
                const auto* a = snapshots_.get(frame, slot);
                if (!a || !a->allocated || a->adopted || a->copied || !a->allocationId || !a->lifetime ||
                    a->plan.group > 2 ||
                    (a->plan.copyMethod == 1 ? (!a->texture11 || a->buffer12) : (!a->buffer12 || a->texture11)) ||
                    !addBytes(bytes[frame], a->plan.layout.allocationCharge) ||
                    !addBytes(payload[frame], a->plan.layout.payloadBound))
                    return false;
                if (frame == 0)
                    ++counts[a->plan.group];
            }
        // Fixed reviewed capacities: raw15 + NR<=44 + postFX<=30 = sender89.
        // Each later group still must acquire/seal its actual inventory before its
        // own exposure. This census is the whole-attempt prepaid ceiling only.
        if (counts[0] != 4 || counts[1] > 17 || counts[2] > 7 || counts[0] + counts[1] + counts[2] != warm_.count ||
            bytes[0] != budget_.bytesA || bytes[1] != budget_.bytesB || bytes[0] != bytes[1] ||
            payload[0] != payload[1] || budget_.roleCountA != warm_.count || budget_.roleCountB != warm_.count ||
            !privateExportInModule(pinCalls_[0].dispatchAddressObserved, pinned_[0]) ||
            !privateExportInModule(pinCalls_[1].dispatchAddressObserved, pinned_[1]))
            return false;
        budget_.retainedOriginalRefs = 178 - 2 * warm_.count; // conservative reference reservation
        budget_.senderRefCountA = budget_.senderRefCountB = 89;
        budget_.receiverRefCountA = 89;
        budget_.receiverRefCountB = 127;
        budget_.groupCountA = budget_.groupCountB = 3;
        budget_.handleCount = 64; // whole-attempt transient-handle capacity, not live handles
        budget_.featureWitnessCount = 2;
        budget_.pinHostComplete = budget_.pinFgComplete = 1;
        budget_.preflightComplete = 1;
        return true;
    }
    bool CaptureEngine::observeFinalDimensions(uint32_t frame, uint64_t width, uint64_t height) noexcept {
        if (frame > 1 || !groups_[frame][0].id.valid() || !width || !height || width > 16384 || height > 16384 ||
            frameInputs_[frame].rawSettings.outputWidth || frameInputs_[frame].rawSettings.outputHeight)
            return false;
        frameInputs_[frame].rawSettings.outputWidth = width;
        frameInputs_[frame].rawSettings.outputHeight = height;
        return true;
    }
    bool CaptureEngine::acquireRaw(uint32_t frame,
                                   const CapturePreflightInput& input,
                                   SourceReturnGuard& guard) noexcept {
        using namespace vdxrfg::pair_capture::nr;
        const auto& request = hostRequest();
        if (!prepared_ || !budget_.preflightComplete || !ownerReserved_ || frame > 1 ||
            !captureEntryEligible(input, request.minimumTime, GetCurrentThreadId()) || groups_[frame][0].id.valid() ||
            input.serial != firstSerial_ + frame || input.nrContext != preparedInput_.nrContext ||
            input.nrContextLifetime != preparedInput_.nrContextLifetime ||
            input.producerDevice != preparedInput_.producerDevice ||
            input.producerContext != preparedInput_.producerContext ||
            input.applicationDevice != preparedInput_.applicationDevice ||
            input.applicationContext != preparedInput_.applicationContext ||
            input.nrDevice != preparedInput_.nrDevice || input.fgDevice != preparedInput_.fgDevice ||
            input.originalInputProducerFence != preparedInput_.originalInputProducerFence ||
            input.originalInputAlias != preparedInput_.originalInputAlias ||
            input.originalNrFence != preparedInput_.originalNrFence ||
            input.originalOutputAlias != preparedInput_.originalOutputAlias ||
            input.upscaling != preparedInput_.upscaling || input.sharpening != preparedInput_.sharpening)
            return false;
        if (frame == 1) {
            if (!observeRawCohort(frame, input) ||
                sources_[1].header.publicWitnessId <= sources_[0].header.publicWitnessId ||
                sources_[1].header.applicationSubmissionId <= sources_[0].header.applicationSubmissionId)
                return false;
        } else {
            NrSourceWitnessV1 current{};
            if (!hostLatestSource(current) || std::memcmp(&current, &sources_[0], sizeof current))
                return false;
            for (uint32_t i = 0; i < 4; ++i)
                if (input.raw[i].swapchain != preparedInput_.raw[i].swapchain ||
                    input.raw[i].releasedIndex != preparedInput_.raw[i].releasedIndex ||
                    input.raw[i].selectedArraySlice != preparedInput_.raw[i].selectedArraySlice ||
                    input.raw[i].texture != preparedInput_.raw[i].texture ||
                    input.raw[i].role != preparedInput_.raw[i].role || input.raw[i].eye != preparedInput_.raw[i].eye)
                    return false;
        }
        if (!hostBeginSelectedRaw(frame, input.serial, ready_, sources_[frame].header.publicWitnessId))
            return false;
        frameInputs_[frame] = input;
        auto& boundary = groups_[frame][0];
        boundary.actual.queue = reinterpret_cast<uint64_t>(input.producerContext);
        boundary.actual.fence = reinterpret_cast<uint64_t>(producerFence_);
        boundary.boundary = uint64_t(frame) + 1;
        boundary.fence11 = producerFence_;
        NrGroupIdentity identity{};
        identity.source = ownerIdentity_;
        identity.serial = input.serial;
        identity.occurrence = sources_[frame].header.publicWitnessId;
        identity.sourceGeneration = sourceGeneration_[frame];
        identity.descriptorEpoch = rawDescriptorEpoch_[frame];
        identity.device = reinterpret_cast<uint64_t>(input.producerDevice);
        identity.submission = boundary.actual;
        identity.signalValue = boundary.boundary;
        auto& owner = hostWorkOwner();
        boundary.id =
            owner.openGroup(static_cast<NrFrame>(frame), vdxrfg::pair_capture::nr::NrGroupKind::raw11, identity);
        groupIdentities_[frame][0] = identity;
        if (!boundary.id.valid()) {
            hostCaptureTerminal(NR_BAD_GROUP, false);
            return false;
        }
        NrBorrowedRef borrowed[15]{};
        uint32_t count = 0;
        uint32_t slots[4]{};
        auto append = [&](IUnknown* pointer,
                          uint64_t type,
                          uint64_t role,
                          uint64_t lifetime,
                          uint64_t allocation = 0,
                          uint64_t bytes = 0) noexcept {
            if (!pointer || !lifetime || count >= 15)
                return false;
            borrowed[count] = {pointer, bytes, static_cast<NrRefRole>(role)};
            auto& row = refs_[frame][count];
            row.borrowedIUnknownAddress = reinterpret_cast<uint64_t>(pointer);
            row.interfaceEnum = type;
            row.roleMask = role;
            row.groupIndex = 0;
            row.physicalAllocationId = allocation;
            row.chargedPrivateBytes = bytes;
            row.owningSourceLifetimeEpoch = lifetime;
            row.observedNonNull = 1;
            ++count;
            return true;
        };
        for (uint32_t i = 0; i < 4; ++i) {
            const auto& raw = input.raw[i];
            SnapshotAllocation* allocation = nullptr;
            for (uint32_t slot = 0; slot < snapshots_.count(); ++slot) {
                auto* candidate = snapshots_.get(frame, slot);
                if (candidate && candidate->plan.role == raw.role && candidate->plan.eye == raw.eye) {
                    allocation = candidate;
                    slots[i] = slot;
                    break;
                }
            }
            if (!allocation || allocation->adopted || !allocation->allocated || allocation->plan.group != 0 ||
                !append(raw.texture, NR_TEXTURE11, 1, rawFacts_[frame][i].producerLifetime) ||
                !append(allocation->texture11,
                        NR_TEXTURE11,
                        2,
                        allocation->lifetime,
                        allocation->allocationId,
                        allocation->plan.layout.allocationCharge)) {
                hostCaptureTerminal(NR_BAD_REF, false);
                return false;
            }
        }
        // Original device/context control refs use their actual Runtime-owner lease.
        // This is owning-source lifetime, not a claimed COM creation timestamp.
        const uint64_t runtimeLease = status_.hostLifetimeEpoch;
        if (!append(input.producerDevice, NR_DEVICE11, 4, runtimeLease) ||
            !append(input.producerContext, NR_CONTEXT11, 8, runtimeLease) ||
            !append(producerFence_, NR_FENCE11, 64, fenceFacts_.producerLifetime) ||
            !append(fgFence_, NR_FENCE12, 64, fenceFacts_.fgLifetime) ||
            !append(appFence_, NR_FENCE11, 64, fenceFacts_.applicationLifetime) ||
            !append(input.applicationDevice, NR_DEVICE11, 4, runtimeLease) ||
            !append(input.applicationContext, NR_CONTEXT11, 8, runtimeLease) || count != 15) {
            hostCaptureTerminal(NR_BAD_REF, false);
            return false;
        }
        auto token = owner.beginAcquire(boundary.id, borrowed, count);
        if (!token.valid() || !token.execute() || !owner.finishAcquire(boundary.id, std::move(token))) {
            hostCaptureTerminal(NR_BAD_REF, true);
            return false;
        }
        refCounts_[frame] = count;
        if (!owner.sealGroup(boundary.id) || !hostRegisterGroup(frame, 0, boundary)) {
            hostCaptureTerminal(NR_BAD_GROUP, true);
            return false;
        }
        for (uint32_t i = 0; i < 4; ++i) {
            const auto* allocation = snapshots_.get(frame, slots[i]);
            if (!allocation ||
                !matchingAcquiredSnapshot(refs_[frame],
                                          count,
                                          reinterpret_cast<uint64_t>(allocation->texture11),
                                          allocation->lifetime,
                                          allocation->allocationId,
                                          allocation->plan.layout.allocationCharge,
                                          0) ||
                !snapshots_.releaseInitialAfterAcquire(frame, slots[i], boundary)) {
                hostCaptureTerminal(NR_BAD_REF, true);
                return false;
            }
        }
        if (frame == 0) {
            // Exactly these three initial refs are now represented in the immutable
            // completed acquisition above. Subsequent use borrows that owner ledger.
            initialFgFence_.Reset();
            initialAppFence_.Reset();
            initialProducerFence_.Reset();
        }
        ReturnBinding binding{};
        binding.producerDevice = input.producerDevice;
        binding.producerContext = input.producerContext;
        binding.producerFence = producerFence_;
        binding.applicationDevice = input.applicationDevice;
        binding.applicationContext = input.applicationContext;
        binding.applicationFence = appFence_;
        binding.value = boundary.boundary;
        binding.nrContext = reinterpret_cast<uint64_t>(input.nrContext);
        binding.nrContextLifetime = input.nrContextLifetime;
        binding.originalNrFence = input.originalNrFence;
        binding.originalOutputAlias = input.originalOutputAlias;
        binding.raw = boundary;
        if (!guard.arm(binding) || !guard.exposeRaw()) {
            hostCaptureTerminal(NR_BAD_GROUP, true);
            return false;
        }
        if (frame == 0 && !TryNextObservedEpoch(nextPrivateExposure, budget_.firstExposureSequence)) {
            hostCaptureTerminal(NR_BAD_BUDGET, true);
            return false;
        }
        for (uint32_t i = 0; i < 4; ++i) {
            const auto index = rawCopyIndex_[frame][i];
            const NrSourceCopyV1* writer = index < 4 ? &sources_[frame].copies[index] : nullptr;
            // API4 export/import plus source640 proves this same shared storage write.
            // The actual writer interface remains the imported app endpoint.
            if (!writer || !snapshots_.copy11(frame,
                                              slots[i],
                                              input.producerContext,
                                              input.raw[i].texture,
                                              boundary,
                                              rawFacts_[frame][i].producerLifetime,
                                              writer->destinationWriteObservationEpoch)) {
                hostCaptureTerminal(NR_BAD_ROLE, true);
                return false;
            }
            ResourceWriteFact observed{};
            observed.resource = writer->destinationAddress;
            observed.lifetime = rawFacts_[frame][i].applicationLifetime;
            observed.sequence = writer->destinationWriteObservationEpoch;
            observed.serial = sources_[frame].header.applicationSubmissionId;
            observed.contextOrList = writer->actualContextAddress;
            observed.api = 11;
            observed.kind = NR_COPY;
            observed.actualCallSequence = writer->actualCopySequence;
            observed.sourceAddress = writer->sourceAddress;
            observed.thread = sources_[frame].header.sourceThread;
            observed.qpc = writer->qpcAfter;
            roleWriters_[frame][slots[i]] = observed;
            roleMetadata_[frame][slots[i]] = input.raw[i].metadata;
            roleMetadata_[frame][slots[i]].sourceOccurrence = sources_[frame].header.publicWitnessId;
            roleWriterAlias_[frame][slots[i]] = rawFacts_[frame][i].sharedHandle;
        }
        return true;
    }
    bool CaptureEngine::acquireNr(uint32_t frame, const CaptureNrInput& input) noexcept {
        using namespace vdxrfg::pair_capture::nr;
        if (!prepared_ || !budget_.preflightComplete || frame > 1 || !groups_[frame][0].id.valid() ||
            nrAcquired_[frame] || !input.pipelineObject ||
            input.prospectiveContextLifetime != preparedInput_.nrContextLifetime || !input.prospectiveSignalValue ||
            input.prospectiveSignalValue == UINT64_MAX)
            return false;
        NrRecordingEpochFacts actual{};
        if (!TryBorrowRecordingEpochFacts(
                preparedInput_.nrContext, preparedInput_.nrContextLifetime, input.recording.facts, actual))
            return false;
        const auto& r = input.recording;
        const auto& g = input.pipeline;
        const bool native = warm_.mode == NR_NATIVE100;
        if (r.device != preparedInput_.nrDevice || r.fence != preparedInput_.originalNrFence || !r.queue || !r.list ||
            !r.allocator || g.device != r.device || g.fence != r.fence ||
            g.observedEpochs != NrPipelineOwnership::allEpochs || g.nrPipelineLifetimeEpoch != warm_.pipelineLifetime ||
            g.nrPipelineGenerationEpoch != warm_.pipelineGeneration || !g.nrBatchObjectEpoch ||
            !g.nrDescriptorBindEpoch || !g.views || !g.targets || !g.root || !g.resolve || !g.inputs[0] ||
            !g.inputs[2] || !g.inputs[4] || input.programs.rootAddress != reinterpret_cast<uint64_t>(g.root) ||
            input.programs.graphicsAddress != reinterpret_cast<uint64_t>(g.resolve) || !input.programs.rootLifetime ||
            !input.programs.graphicsLifetime)
            return false;
        if (native) {
            if (!g.nativeAnswer || g.downsample || g.inputs[1] || g.inputs[3] || !input.nativeDepth[0] ||
                !input.nativeDepth[1] || input.programs.computeAddress || input.programs.computeLifetime)
                return false;
            for (const auto& scratch : g.scratch)
                if (scratch.proxy || scratch.answer || scratch.depth)
                    return false;
        } else {
            if (g.nativeAnswer || !g.downsample || !g.inputs[1] || !g.inputs[3] || input.nativeDepth[0] ||
                input.nativeDepth[1] || input.programs.computeAddress != reinterpret_cast<uint64_t>(g.downsample) ||
                !input.programs.computeLifetime)
                return false;
            for (const auto& scratch : g.scratch)
                if (!scratch.proxy || !scratch.answer || !scratch.depth)
                    return false;
        }
        auto& boundary = groups_[frame][1];
        boundary.actual.queue = reinterpret_cast<uint64_t>(r.queue);
        boundary.actual.fence = reinterpret_cast<uint64_t>(r.fence);
        boundary.actual.list = reinterpret_cast<uint64_t>(r.list);
        boundary.actual.allocator = reinterpret_cast<uint64_t>(r.allocator);
        boundary.actual.recordingEpoch = actual.nrRecordingEpoch;
        boundary.boundary = input.prospectiveSignalValue;
        boundary.fence12 = r.fence;
        NrGroupIdentity identity{};
        identity.source = ownerIdentity_;
        identity.serial = firstSerial_ + frame;
        identity.occurrence = sources_[frame].header.publicWitnessId;
        identity.sourceGeneration = sourceGeneration_[frame];
        identity.descriptorEpoch = input.pipeline.nrDescriptorBindEpoch;
        identity.device = reinterpret_cast<uint64_t>(r.device);
        identity.submission = boundary.actual;
        identity.signalValue = boundary.boundary;
        auto& owner = hostWorkOwner();
        boundary.id =
            owner.openGroup(static_cast<NrFrame>(frame), vdxrfg::pair_capture::nr::NrGroupKind::nr12, identity);
        groupIdentities_[frame][1] = identity;
        if (!boundary.id.valid()) {
            hostCaptureTerminal(NR_BAD_GROUP, true);
            return false;
        }
        NrBorrowedRef borrowed[44]{};
        uint32_t count = 0, start = refCounts_[frame], presence = 0, bit = 0;
        auto append = [&](IUnknown* pointer,
                          uint64_t type,
                          uint64_t role,
                          uint64_t life,
                          uint64_t allocation = 0,
                          uint64_t bytes = 0) noexcept {
            if (!pointer || !life || life == UINT64_MAX || count >= 44 || start + count >= 89)
                return false;
            borrowed[count] = {pointer, bytes, static_cast<NrRefRole>(role)};
            auto& row = refs_[frame][start + count];
            row = {reinterpret_cast<uint64_t>(pointer), type, role, 1, allocation, bytes, life, 1};
            ++count;
            return true;
        };
        auto graphRef = [&](IUnknown* pointer, uint64_t type, uint64_t role, uint64_t life) noexcept {
            const uint32_t flag = 1u << bit++;
            if (!pointer)
                return true;
            presence |= flag;
            return append(pointer, type, role, life);
        };
        auto resourceRef = [&](ID3D12Resource* resource) noexcept {
            return graphRef(resource, NR_RESOURCE12, 1, resource ? sdkObservedResourceLifetime(resource) : 0);
        };
        bool complete = graphRef(g.device, NR_DEVICE12, 4, actual.nrCommandContextLifetimeEpoch) &&
                        graphRef(g.fence, NR_FENCE12, 64, actual.nrCommandContextLifetimeEpoch);
        for (auto* resource : g.inputs)
            complete = complete && resourceRef(resource);
        complete = complete && graphRef(g.views, NR_HEAP12, 128, g.nrBatchObjectEpoch) &&
                   graphRef(g.targets, NR_HEAP12, 128, g.nrBatchObjectEpoch) &&
                   graphRef(g.root, NR_ROOT12, 128, input.programs.rootLifetime) &&
                   graphRef(g.downsample, NR_PSO12, 128, input.programs.computeLifetime) &&
                   graphRef(g.resolve, NR_PSO12, 128, input.programs.graphicsLifetime);
        for (const auto& scratch : g.scratch)
            complete =
                complete && resourceRef(scratch.proxy) && resourceRef(scratch.answer) && resourceRef(scratch.depth);
        complete = complete && resourceRef(g.nativeAnswer);
        if (!complete || bit != 19 || presence != g.pointerPresence ||
            !append(r.device, NR_DEVICE12, 4, actual.nrCommandContextLifetimeEpoch) ||
            !append(r.queue, NR_QUEUE12, 8, actual.nrCommandContextLifetimeEpoch) ||
            !append(r.fence, NR_FENCE12, 64, actual.nrCommandContextLifetimeEpoch) ||
            !append(r.list, NR_LIST12, 16, actual.nrListObjectEpoch) ||
            !append(r.allocator, NR_ALLOCATOR12, 32, actual.nrAllocatorObjectEpoch)) {
            hostCaptureTerminal(NR_BAD_REF, true);
            return false;
        }
        if (native)
            for (auto* depth : input.nativeDepth)
                if (!append(depth, NR_RESOURCE12, 1, sdkObservedResourceLifetime(depth))) {
                    hostCaptureTerminal(NR_BAD_REF, true);
                    return false;
                }
        for (uint32_t slot = 0; slot < snapshots_.count(); ++slot) {
            const auto* a = snapshots_.get(frame, slot);
            if (a && a->plan.group == 1)
                if (a->adopted || !a->allocated ||
                    !append(
                        a->buffer12, NR_RESOURCE12, 2, a->lifetime, a->allocationId, a->plan.layout.allocationCharge)) {
                    hostCaptureTerminal(NR_BAD_REF, true);
                    return false;
                }
        }
        // The imported D11 return fence is auxiliary to the primary NR submission fence.
        if (!append(preparedInput_.originalOutputAlias, NR_FENCE11, 128, nrFenceFacts_.alias.targetLifetime)) {
            hostCaptureTerminal(NR_BAD_REF, true);
            return false;
        }
        auto token = owner.beginAcquire(boundary.id, borrowed, count);
        if (!token.valid() || !token.execute() || !owner.finishAcquire(boundary.id, std::move(token)) ||
            !owner.sealGroup(boundary.id) || !hostRegisterGroup(frame, 1, boundary)) {
            hostCaptureTerminal(NR_BAD_REF, true);
            return false;
        }
        refCounts_[frame] += count;
        for (uint32_t slot = 0; slot < snapshots_.count(); ++slot) {
            const auto* a = snapshots_.get(frame, slot);
            if (a && a->plan.group == 1)
                if (!matchingAcquiredSnapshot(refs_[frame] + start,
                                              count,
                                              reinterpret_cast<uint64_t>(a->buffer12),
                                              a->lifetime,
                                              a->allocationId,
                                              a->plan.layout.allocationCharge,
                                              1,
                                              NR_RESOURCE12) ||
                    !snapshots_.releaseInitialAfterAcquire(frame, slot, boundary)) {
                    hostCaptureTerminal(NR_BAD_REF, true);
                    return false;
                }
        }
        nrInputs_[frame] = input;
        nrAcquired_[frame] = true;
        return true;
    }
    ID3D12Resource* CaptureEngine::nrRoleSource(uint32_t frame, uint64_t role, uint64_t eye) const noexcept {
        if (frame > 1 || eye > 2)
            return nullptr;
        const auto& g = nrInputs_[frame].pipeline;
        if (eye == 2)
            return role == NR_NATIVE_ANSWER ? g.nativeAnswer : role == NR_POST_RESOLVE ? g.inputs[4] : nullptr;
        switch (role) {
        case NR_MODEL_COLOUR:
        case NR_POST_EVAL_COLOUR:
        case NR_PRE_RESOLVE_COLOUR:
            return g.inputs[eye * 2];
        case NR_MODEL_DEPTH:
            return warm_.mode == NR_NATIVE100 ? nrInputs_[frame].nativeDepth[eye] : g.inputs[eye * 2 + 1];
        case NR_PROXY:
        case NR_EXACT_PROXY:
            return g.scratch[eye].proxy;
        case NR_PROXY_DEPTH:
            return g.scratch[eye].depth;
        case NR_SPATIAL_ANSWER:
            return g.scratch[eye].answer;
        default:
            return nullptr;
        }
    }
    bool CaptureEngine::captureNrRole(uint32_t frame,
                                      uint64_t role,
                                      uint64_t eye,
                                      SourceReturnGuard& guard,
                                      const SnapshotRoleMetadata& geometry) noexcept {
        if (frame > 1 || !nrAcquired_[frame] || nrSubmitted_[frame] || nrSubmission_[frame].valid())
            return false;
        if (!geometry.geometryObserved || geometry.hostSerial != firstSerial_ + frame || !geometry.rectWidth ||
            !geometry.rectHeight || !geometry.cropWidth || !geometry.cropHeight)
            return false;
        auto* resource = nrRoleSource(frame, role, eye);
        if (!resource)
            return false;
        const uint64_t life = sdkObservedResourceLifetime(resource);
        if (!heldSourceIdentity(
                refs_[frame], refCounts_[frame], reinterpret_cast<uint64_t>(resource), life, NR_RESOURCE12, 1))
            return false;
        ResourceWriteFact writer{};
        SourceTextureFacts alias{};
        bool direct = latestOriginalWrite(reinterpret_cast<uint64_t>(resource), life, writer);
        if (!direct) {
            if (!sourceNrGet(resource, alias) ||
                !nrAliasComplete(alias,
                                 alias.producer,
                                 reinterpret_cast<uint64_t>(resource),
                                 reinterpret_cast<uint64_t>(preparedInput_.producerDevice),
                                 reinterpret_cast<uint64_t>(preparedInput_.nrDevice)) ||
                alias.applicationLifetime != life || !sourceNrOriginalWrite12(resource, life, writer))
                return false;
        }
        uint32_t slot = 0;
        for (; slot < snapshots_.count(); ++slot) {
            const auto* a = snapshots_.get(frame, slot);
            if (a && a->plan.role == role && a->plan.eye == eye)
                break;
        }
        if (slot == snapshots_.count())
            return false;
        // This marks private recording, not Execute. The original Submit facts later
        // determine possible queue exposure and its exact completion boundary.
        guard.recordNrPrivateCommands();
        if (!snapshots_.copy12(
                frame, slot, nrInputs_[frame].recording.list, resource, groups_[frame][1], life, writer.sequence)) {
            hostCaptureTerminal(NR_BAD_ROLE, true);
            return false;
        }
        roleWriters_[frame][slot] = writer;
        roleMetadata_[frame][slot] = geometry;
        roleMetadata_[frame][slot].sourceOccurrence = sources_[frame].header.publicWitnessId;
        roleWriterAlias_[frame][slot] = direct ? 0 : alias.sharedHandle;
        return true;
    }
    vdxrfg::pair_capture::nr::NrSubmissionToken* CaptureEngine::beginNrSubmission(uint32_t frame) noexcept {
        if (frame > 1 || !nrAcquired_[frame] || nrSubmitted_[frame] || nrSubmission_[frame].valid())
            return nullptr;
        auto& owner = hostWorkOwner();
        const auto& boundary = groups_[frame][1];
        nrSubmission_[frame] = owner.beginSubmission(boundary.id);
        return nrSubmission_[frame].valid() ? &nrSubmission_[frame] : nullptr;
    }
    bool CaptureEngine::finishNrSubmission(uint32_t frame,
                                           const NrWarmSubmissionFacts& facts,
                                           uint64_t returned,
                                           SourceReturnGuard& guard) noexcept {
        if (frame > 1 || !nrSubmission_[frame].valid() || nrSubmitted_[frame])
            return false;
        const auto& boundary = groups_[frame][1];
        const bool actual = warmSubmissionComplete(facts,
                                                   reinterpret_cast<uint64_t>(preparedInput_.nrContext),
                                                   preparedInput_.nrContextLifetime,
                                                   boundary.actual,
                                                   returned) &&
                            returned == boundary.boundary;
        const bool reconciled = hostWorkOwner().reconcileSubmission(boundary.id, std::move(nrSubmission_[frame]));
        guard.observeNr(facts, actual && reconciled);
        if (!actual || !reconciled) {
            const bool possible =
                facts.calls.executeObservations != 0 || (facts.calls.close.observed && facts.calls.close.result >= 0);
            hostCaptureTerminal(NR_BAD_GROUP, possible);
            return false;
        }
        nrSubmitted_[frame] = true;
        return true;
    }
    static_assert(sizeof(CaptureEngine) <= hostMetadataEngine,
                  "Engine and its preallocated projection workspace exceed host share");
} // namespace virtualdesktop_openxr::nr_capture
