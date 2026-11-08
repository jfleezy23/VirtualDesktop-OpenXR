#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrModules.h"
#include "PairCaptureNrEngineFacts.h"
#include "PairCaptureNrEngine.h"
#include "../nr-frame-generation/include/vdxr_fg_api.h"
#include "../nr-frame-generation/include/pair_capture_nr_control.h"
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <memory>
#include <new>

namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct WarmFrame {
            uint64_t serial{}, contextLifetime{}, signalValue{}, ticket{}, previous{}, history{};
            SdkWarmFacts sdk{};
            int32_t signalResult{}, waitResult{}, admissionResult{}, retirementResult{};
            bool nrObserved{}, inventory{}, layoutComplete{}, moduleAttested{}, admitted{}, retired{};
        };
        struct Control {
            std::mutex mutex;
            NrHostStatusV1 status{};
            OwnedGroupBoundary groups[2][3]{};
            bool releaseBusy{};
            NrReadyV1 ready{};
            NrSourceWitnessV1 latest{};
            std::array<WarmFrame, 8> warm{};
            NrModuleV1 pinnedModules[2]{};
            NrCallV1 pinCalls[2]{};
            ReceiverCall receiver{};
            HMODULE receiverModule{};
            const void* instance{};
            uint64_t sessionHandle{}, sessionLifetime{}, sourceObservations{}, lastWitness{};
            bool hasSource{}, terminal{};
        };
        Control control;
        vdxrfg::pair_capture::nr::NrWorkOwner workOwner;
        static_assert(sizeof(Control) + sizeof(vdxrfg::pair_capture::nr::NrWorkOwner) <= hostMetadataControl);
        std::atomic<uint64_t> nextInstance{1}, nextSession{1};
        bool number(const wchar_t* text, uint64_t& value) noexcept {
            value = 0;
            if (!text || !*text)
                return false;
            for (const auto* c = text; *c; ++c) {
                if (*c < L'0' || *c > L'9' || value > (UINT64_MAX - static_cast<uint64_t>(*c - L'0')) / 10)
                    return false;
                value = value * 10 + static_cast<uint64_t>(*c - L'0');
            }
            return value && value != UINT64_MAX;
        }
        bool environment(const wchar_t* name, wchar_t* target, DWORD capacity) noexcept {
            const auto size = GetEnvironmentVariableW(name, target, capacity);
            return size && size < capacity;
        }
        unsigned hex(wchar_t c) noexcept {
            return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : 16;
        }
        bool words(const wchar_t* text, uint64_t out[2]) noexcept {
            out[0] = out[1] = 0;
            if (std::wcslen(text) != 32)
                return false;
            for (size_t i = 0; i < 32; ++i) {
                auto digit = hex(text[i]);
                if (digit > 15)
                    return false;
                out[i / 16] = (out[i / 16] << 4) | digit;
            }
            return (out[0] | out[1]) != 0;
        }
        bool hash(const wchar_t* text, uint8_t out[32]) noexcept {
            if (std::wcslen(text) != 64)
                return false;
            for (size_t i = 0; i < 32; ++i) {
                auto a = hex(text[2 * i]), b = hex(text[2 * i + 1]);
                if (a > 15 || b > 15)
                    return false;
                out[i] = static_cast<uint8_t>((a << 4) | b);
            }
            return true;
        }
        void loadRequest(HostRequest& r) noexcept {
            wchar_t text[128]{};
            if (!environment(L"VDXR_PAIR_NR_ABI", text, 128) || std::wcscmp(text, L"1"))
                return;
            if (!environment(L"VDXR_PAIR_NR_MODE", text, 128))
                return;
            r.mode = !std::wcscmp(text, L"native100")   ? NR_NATIVE100
                     : !std::wcscmp(text, L"spatial85") ? NR_SPATIAL85
                                                        : 0;
            if (!r.mode || !environment(L"VDXR_PAIR_NONCE", text, 128) || !words(text, r.nonce))
                return;
            if (!environment(L"VDXR_PAIR_ATTEMPT", text, 128) || !number(text, r.attempt))
                return;
            if (!environment(L"VDXR_PAIR_PID", text, 128) || !number(text, r.pid) || r.pid != GetCurrentProcessId())
                return;
            if (!environment(L"VDXR_PAIR_CREATION", text, 128) || !number(text, r.creation))
                return;
            FILETIME created{}, ended{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user) ||
                r.creation != ((uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime))
                return;
            if (!environment(L"VDXR_PAIR_MIN_SERIAL", text, 128) || !number(text, r.minimumSerial))
                return;
            if (!environment(L"VDXR_PAIR_MIN_TIME", text, 128) || !number(text, r.minimumTime) ||
                r.minimumTime > INT64_MAX)
                return;
            uint64_t bytes{};
            if (!environment(L"VDXR_PAIR_MAX_BYTES", text, 128) || !number(text, bytes) || bytes != 134217728)
                return;
            uint64_t startupSession[2]{};
            if (!environment(L"VDXR_PAIR_SESSION", text, 128) || !words(text, startupSession))
                return;
            if (!environment(L"VDXR_PAIR_NR_HOST_SHA256", text, 128) || !hash(text, r.hostHash))
                return;
            if (!environment(L"VDXR_PAIR_NR_FG_SHA256", text, 128) || !hash(text, r.fgHash))
                return;
            if (!environment(L"VDXR_PAIR_NR_MODEL_SHA256", text, 128) || !hash(text, r.modelHash))
                return;
            if (!environment(L"VDXR_PAIR_NR_SOURCE_SHA256", text, 128) || !hash(text, r.sourceHash))
                return;
            if (!environment(L"VDXR_PAIR_NR_AUTHOR_SHA256", text, 128) || !hash(text, r.authorHash))
                return;
            if (!environment(L"VDXR_PAIR_NR_SOURCE_PATH", r.sourceBridgePath, 4096))
                return;
            if (!environment(L"VDXR_PAIR_ARM_FILE", r.armFile, 4096) ||
                !environment(L"VDXR_PAIR_OUTPUT", r.output, 4096))
                return;
            const auto absolute = [](const wchar_t* path) {
                return (std::wcslen(path) > 2 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
                       (path[0] == L'\\' && path[1] == L'\\');
            };
            if (!absolute(r.armFile) || !absolute(r.output) || !absolute(r.sourceBridgePath))
                return;
            r.valid = true;
            return;
        }
        WarmFrame* findWarm(uint64_t serial) noexcept {
            for (auto& row : control.warm)
                if (row.serial == serial)
                    return &row;
            return nullptr;
        }
        void clearWarmStatusLocked() noexcept {
            auto& s = control.status;
            s.flags &=
                ~(uint64_t(NR_HOST_NATIVE_WARM) | NR_HOST_SPATIAL_WARM | NR_HOST_FEATURE_CREATED |
                  NR_HOST_TWO_EYE_EVALUATE | NR_HOST_NR_OUTPUT_WAIT | NR_HOST_FG_ADMITTED | NR_HOST_ORIGINAL_RETIRED);
            s.warmSerial = s.nrContextLifetimeEpoch = s.evaluateLeftSequence = s.evaluateRightSequence = 0;
            s.originalNrSignalValue = s.outputWaitResultBits = 0;
            if (s.state == NR_HOST_WARM)
                s.state = NR_HOST_UNARMED;
        }
        void invalidateWarmLocked() noexcept {
            if (selectedWarmInvalidation(control.status.state)) {
                control.terminal = true;
                control.status.state = NR_HOST_UNCERTAIN;
                control.status.failureReason = NR_BAD_IDENTITY;
                return;
            }
            control.warm = {};
            clearWarmStatusLocked();
        }
        void qualifyWarm(WarmFrame& row) noexcept {
            if (!hostStateMayPublishWarm(control.status.state))
                return;
            if (control.terminal || !row.nrObserved || !row.inventory || !row.layoutComplete || !row.moduleAttested ||
                !row.admitted || !row.retired || row.signalResult < 0 || row.waitResult < 0 ||
                row.admissionResult != 0 || row.retirementResult != 0 || !row.sdk.completeFrame || row.sdk.invalid ||
                !row.sdk.tableLive || !row.sdk.featureLive[0] || !row.sdk.featureLive[1] || !row.signalValue ||
                row.signalValue == UINT64_MAX || row.history != control.ready.fgHistoryGeneration ||
                !(control.status.flags & NR_HOST_RECEIVER_NEGOTIATED))
                return;
            auto& s = control.status;
            s.state = NR_HOST_WARM;
            s.warmSerial = row.serial;
            s.nrContextLifetimeEpoch = row.contextLifetime;
            s.evaluateLeftSequence = row.sdk.evaluateSequence[0];
            s.evaluateRightSequence = row.sdk.evaluateSequence[1];
            s.originalNrSignalValue = row.signalValue;
            s.outputWaitResultBits = static_cast<uint32_t>(row.waitResult);
            s.flags |= NR_HOST_FEATURE_CREATED | NR_HOST_TWO_EYE_EVALUATE | NR_HOST_NR_OUTPUT_WAIT |
                       NR_HOST_FG_ADMITTED | NR_HOST_ORIGINAL_RETIRED |
                       (s.mode == NR_NATIVE100 ? NR_HOST_NATIVE_WARM : NR_HOST_SPATIAL_WARM);
        }
    } // namespace
    const HostRequest& hostRequest() noexcept {
        struct StoredRequest {
            HostRequest value{};
            StoredRequest() noexcept {
                loadRequest(value);
            }
        };
        static const StoredRequest request;
        return request.value;
    }
    bool hostLifetimeFacts(HostLifetimeFacts& out) noexcept {
        out = {};
        if (!hostRequest().valid)
            return false;
        std::lock_guard lock(control.mutex);
        if (control.terminal || !control.instance || !control.status.hostLifetimeEpoch || !control.sessionHandle ||
            !control.sessionLifetime)
            return false;
        out.instance = reinterpret_cast<uint64_t>(control.instance);
        out.instanceLifetime = control.status.hostLifetimeEpoch;
        out.sessionHandle = control.sessionHandle;
        out.sessionLifetime = control.sessionLifetime;
        return true;
    }
    void hostInstanceCreated(const void* instance) noexcept {
        const auto& r = hostRequest();
        if (!r.valid || !instance)
            return;
        uint64_t epoch{};
        if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextInstance, epoch))
            return;
        std::lock_guard lock(control.mutex);
        if (control.instance) {
            control.terminal = true;
            return;
        }
        control.instance = instance;
        auto& s = control.status;
        s.size = 256;
        s.version = 1;
        s.state = NR_HOST_UNARMED;
        s.flags = NR_HOST_REQUEST_VALID;
        s.attempt = r.attempt;
        s.nonceLo = r.nonce[0];
        s.nonceHi = r.nonce[1];
        s.mode = r.mode;
        s.pid = r.pid;
        s.creationFiletime = r.creation;
        s.hostLifetimeEpoch = epoch;
    }
    void hostSessionCreated(const void* instance, uint64_t handle) noexcept {
        if (!hostRequest().valid || !handle)
            return;
        std::lock_guard lock(control.mutex);
        if (control.terminal || control.status.state == NR_HOST_SETTLED)
            return;
        uint64_t epoch{};
        if (!vdxrfg::pair_capture::nr::TryNextObservedEpoch(nextSession, epoch))
            return;
        if (instance != control.instance || control.sessionHandle) {
            control.terminal = true;
            return;
        }
        control.sessionHandle = handle;
        control.sessionLifetime = epoch;
    }
    void hostSessionDestroyed(const void* instance) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(control.mutex);
        if (instance != control.instance || control.terminal)
            return;
        if (selectedWarmInvalidation(control.status.state)) {
            control.terminal = true;
            control.status.state = NR_HOST_UNCERTAIN;
            control.status.failureReason = NR_BAD_IDENTITY;
            return;
        }
        control.sessionHandle = 0;
        control.hasSource = false;
        if (hostStateMayPublishWarm(control.status.state))
            invalidateWarmLocked();
    }
    bool hostBindReceiver(const NrReadyV1& ready,
                          ReceiverCall receiver,
                          HMODULE module,
                          ID3D12Device* device,
                          ID3D12CommandQueue* queue,
                          void* context) noexcept {
        const auto& r = hostRequest();
        if (!r.valid || !receiver || !module || ready.size != 256 || ready.version != 1 || ready.status != 1 ||
            ready.observedFlags != 1 || ready.attempt != r.attempt || ready.nonceLo != r.nonce[0] ||
            ready.nonceHi != r.nonce[1] || ready.pid != r.pid || ready.creationFiletime != r.creation ||
            ready.mode != r.mode || !(ready.sessionNonceLo | ready.sessionNonceHi) ||
            ready.fgContextAddress != reinterpret_cast<uint64_t>(context) ||
            ready.deviceAddress != reinterpret_cast<uint64_t>(device) ||
            ready.queueAddress != reinterpret_cast<uint64_t>(queue) || !ready.fgContextLifetimeEpoch ||
            ready.fgContextLifetimeEpoch == UINT64_MAX || !ready.fgHistoryGeneration ||
            ready.fgHistoryGeneration == UINT64_MAX)
            return false;
        for (auto v : ready.reserved)
            if (v)
                return false;
        const std::unique_ptr<NrPacketV1> packetStorage(new (std::nothrow) NrPacketV1);
        if (!packetStorage)
            return false;
        std::memset(packetStorage.get(), 0, sizeof(NrPacketV1));
        bool alreadyPinned = false;
        {
            std::lock_guard lock(control.mutex);
            alreadyPinned = (control.status.flags & NR_HOST_DUAL_PIN) && control.receiverModule == module &&
                            control.receiver == receiver;
            if (control.terminal)
                return false;
        }
        NrModuleV1 modules[2]{};
        NrCallV1 pinCalls[2]{};
        if (!alreadyPinned) {
            // First actual process-lifetime PIN observation for each separate module.
            // No observer mutex is held while hashing files or invoking Win32/CNG.
            const bool hostPin = observePrivateModule(reinterpret_cast<const void*>(&VDXR_PrivatePairNRStatusV1),
                                                      1,
                                                      r.hostHash,
                                                      true,
                                                      modules[0],
                                                      pinCalls[0],
                                                      1);
            const bool fgPin =
                hostPin && observePrivateModule(
                               reinterpret_cast<const void*>(receiver), 2, r.fgHash, true, modules[1], pinCalls[1], 1);
            pinCalls[0].moduleIndex = 0;
            pinCalls[1].moduleIndex = 1;
            if (!hostPin || !fgPin || modules[1].baseObserved != reinterpret_cast<uint64_t>(module)) {
                // A successful first PIN is permanent even if the second fails.
                std::lock_guard lock(control.mutex);
                for (size_t i = 0; i < 2; ++i) {
                    control.pinnedModules[i] = modules[i];
                    control.pinCalls[i] = pinCalls[i];
                }
                control.terminal = true;
                control.status.state = NR_HOST_ABORTED;
                control.status.failureReason = NR_BAD_MODULE;
                return false;
            }
        }
        // Negotiation is a CPU-only synchronous call before holding our metadata mutex.
        auto& packet = *packetStorage;
        packet.header.magic = NR_MAGIC;
        packet.header.size = sizeof packet;
        packet.header.version = 1;
        packet.header.operation = NR_NEGOTIATE;
        NrAckV1 ack{};
        const auto returned = receiver(&packet, &ack);
        if (returned != NR_NEGOTIATED || ack.status != NR_NEGOTIATED || ack.size != 128 || ack.version != 1 ||
            ack.reasonEnum || ack.requestAttempt || ack.frameOrdinal || ack.transferId || ack.receiverCommitEpoch ||
            ack.roleCount != 64 || ack.refCount != 128 || ack.groupCount != 8 || ack.packetDigestLo ||
            ack.packetDigestHi || ack.acceptedBudgetBytes != 134217728 || ack.observedFlags != 1 || ack.reserved)
            return false;
        std::lock_guard lock(control.mutex);
        if (control.terminal)
            return false;
        if (control.ready.fgContextAddress && (control.ready.fgContextAddress != ready.fgContextAddress ||
                                               control.ready.fgContextLifetimeEpoch != ready.fgContextLifetimeEpoch ||
                                               control.ready.fgHistoryGeneration != ready.fgHistoryGeneration)) {
            auto& s = control.status;
            if (selectedReceiverDrift(s.state)) {
                control.terminal = true;
                s.state = NR_HOST_UNCERTAIN;
                s.failureReason = NR_BAD_IDENTITY;
                return false;
            }
            control.warm = {};
            s.state = NR_HOST_UNARMED;
            s.flags = NR_HOST_REQUEST_VALID;
        }
        if (!alreadyPinned)
            for (size_t i = 0; i < 2; ++i) {
                control.pinnedModules[i] = modules[i];
                control.pinCalls[i] = pinCalls[i];
            }
        control.status.flags |= NR_HOST_DUAL_PIN;
        control.ready = ready;
        control.receiver = receiver;
        control.receiverModule = module;
        auto& s = control.status;
        s.sessionNonceLo = ready.sessionNonceLo;
        s.sessionNonceHi = ready.sessionNonceHi;
        s.fgContextLifetimeEpoch = ready.fgContextLifetimeEpoch;
        s.fgHistoryGeneration = ready.fgHistoryGeneration;
        s.flags |= NR_HOST_RECEIVER_NEGOTIATED;
        return true;
    }
    void hostInvalidateWarm() noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(control.mutex);
        invalidateWarmLocked();
    }
    void hostWarmFrame(uint64_t serial,
                       uint64_t contextLifetime,
                       const SdkWarmFacts& sdk,
                       uint64_t signal,
                       int32_t signalResult,
                       int32_t waitResult,
                       bool inventory) noexcept {
        if (!hostRequest().valid || !serial)
            return;
        if (!contextLifetime || !inventory || signalResult < 0 || waitResult < 0 || !sdk.completeFrame || sdk.invalid) {
            hostInvalidateWarm();
            return;
        }
        std::lock_guard lock(control.mutex);
        if (control.terminal)
            return;
        if (control.status.nrContextLifetimeEpoch && control.status.nrContextLifetimeEpoch != contextLifetime)
            invalidateWarmLocked();
        auto* row = findWarm(serial);
        if (!row) {
            row = &control.warm[serial % control.warm.size()];
            *row = {};
            row->serial = serial;
        }
        row->contextLifetime = contextLifetime;
        row->sdk = sdk;
        row->signalValue = signal;
        row->signalResult = signalResult;
        row->waitResult = waitResult;
        row->inventory = inventory;
        row->nrObserved = true;
        control.status.lastSerial = serial;
        qualifyWarm(*row);
    }
    void hostWarmLayoutComplete(uint64_t serial, bool complete) noexcept {
        if (!hostRequest().valid || !serial)
            return;
        std::lock_guard lock(control.mutex);
        auto* row = findWarm(serial);
        if (!row)
            return;
        if (!complete) {
            invalidateWarmLocked();
            return;
        }
        row->layoutComplete = complete;
        qualifyWarm(*row);
    }
    void hostModuleAttested(uint64_t serial, uint64_t history, bool complete) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(control.mutex);
        if (control.terminal || !hostStateMayPublishWarm(control.status.state))
            return;
        if (!complete) {
            // Withdraw old readiness without erasing genuine original admissions.
            for (auto& warm : control.warm)
                warm.moduleAttested = false;
            clearWarmStatusLocked();
        }
        auto* row = findWarm(serial);
        if (!row || !history || history != control.ready.fgHistoryGeneration)
            return;
        row->moduleAttested = complete;
        qualifyWarm(*row);
    }
    void hostOriginalAdmission(
        uint64_t serial, uint64_t ticket, uint64_t previous, uint64_t history, int32_t result) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(control.mutex);
        auto* row = findWarm(serial);
        if (!row)
            return;
        if (result != 0 || !ticket) {
            invalidateWarmLocked();
            return;
        }
        row->ticket = ticket;
        row->previous = previous;
        row->history = history;
        row->admissionResult = result;
        row->admitted = ticket && result == 0;
        qualifyWarm(*row);
    }
    void hostOriginalRetirement(uint64_t serial, uint64_t ticket, uint64_t history, int32_t result) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(control.mutex);
        auto* row = findWarm(serial);
        if (!row || row->ticket != ticket || row->history != history)
            return;
        // BUSY is pending retirement; preserve the row for the original caller's retry.
        if (result == VDXRFG_E_BUSY)
            return;
        if (result != 0) {
            invalidateWarmLocked();
            return;
        }
        row->retirementResult = result;
        row->retired = result == 0;
        qualifyWarm(*row);
    }
    vdxrfg::pair_capture::nr::NrWorkOwner& hostWorkOwner() noexcept {
        return workOwner;
    }
    bool hostRegisterGroup(uint32_t frame, uint32_t group, const OwnedGroupBoundary& boundary) noexcept {
        using namespace vdxrfg::pair_capture::nr;
        if (frame > 1 || group > 2 || !boundary.id.valid() ||
            boundary.id.owner != reinterpret_cast<uintptr_t>(&workOwner) ||
            static_cast<uint32_t>(boundary.id.frame) != frame || !boundary.boundary ||
            boundary.boundary == UINT64_MAX || !boundary.actual.queue || !boundary.actual.fence ||
            (group == 1 ? (!boundary.fence12 || boundary.fence11) : (!boundary.fence11 || boundary.fence12)))
            return false;
        const auto actualFence =
            group == 1 ? reinterpret_cast<uint64_t>(boundary.fence12) : reinterpret_cast<uint64_t>(boundary.fence11);
        if (actualFence != boundary.actual.fence)
            return false;
        std::lock_guard lock(control.mutex);
        if (control.terminal || control.groups[frame][group].id.valid() ||
            boundary.id.attempt != control.status.attempt)
            return false;
        control.groups[frame][group] = boundary;
        return true;
    }
    bool hostFinishFrameTransfer(uint32_t frame) noexcept {
        if (frame > 1)
            return false;
        OwnedGroupBoundary groups[3]{};
        {
            std::lock_guard lock(control.mutex);
            if (control.terminal || control.status.transferredFrames != frame)
                return false;
            for (size_t i = 0; i < 3; ++i)
                groups[i] = control.groups[frame][i];
        }
        for (const auto& g : groups) {
            const auto snapshot = workOwner.snapshot(g.id);
            if (!snapshot.valid || !snapshot.sealed || snapshot.futureReaderPending || snapshot.pendingSubmission ||
                !snapshot.originalSignalSucceeded)
                return false;
        }
        std::lock_guard lock(control.mutex);
        if (control.terminal || control.status.transferredFrames != frame)
            return false;
        ++control.status.transferredFrames;
        control.status.state = control.status.transferredFrames == 2 ? NR_HOST_TRANSFERRED : NR_HOST_ACTIVE;
        return true;
    }
    bool hostReceiverBinding(NrReadyV1& ready, ReceiverCall& receiver, HMODULE& module) noexcept {
        ready = {};
        receiver = nullptr;
        module = nullptr;
        std::lock_guard lock(control.mutex);
        if (control.terminal || !(control.status.flags & NR_HOST_RECEIVER_NEGOTIATED) ||
            !(control.status.flags & NR_HOST_DUAL_PIN))
            return false;
        ready = control.ready;
        receiver = control.receiver;
        module = control.receiverModule;
        return true;
    }
    bool hostPinnedModules(NrModuleV1 (&modules)[2], NrCallV1 (&calls)[2]) noexcept {
        std::lock_guard lock(control.mutex);
        if (!(control.status.flags & NR_HOST_DUAL_PIN) || control.terminal)
            return false;
        for (size_t i = 0; i < 2; ++i) {
            modules[i] = control.pinnedModules[i];
            calls[i] = control.pinCalls[i];
        }
        return true;
    }
    bool hostLatestSource(NrSourceWitnessV1& out) noexcept {
        out = {};
        std::lock_guard lock(control.mutex);
        if (!control.hasSource || control.terminal)
            return false;
        out = control.latest;
        return true;
    }
    bool hostBeginSelectedRaw(uint32_t frame, uint64_t serial, const NrReadyV1& ready, uint64_t witness) noexcept {
        std::lock_guard lock(control.mutex);
        auto& s = control.status;
        if (control.terminal || frame > 1 || !serial || !control.hasSource ||
            control.latest.header.publicWitnessId != witness ||
            control.ready.fgContextAddress != ready.fgContextAddress ||
            control.ready.fgContextLifetimeEpoch != ready.fgContextLifetimeEpoch ||
            control.ready.fgHistoryGeneration != ready.fgHistoryGeneration ||
            control.ready.bindSequence != ready.bindSequence || control.ready.sessionNonceLo != ready.sessionNonceLo ||
            control.ready.sessionNonceHi != ready.sessionNonceHi ||
            (frame == 0 ? (s.state != NR_HOST_WARM || s.transferredFrames != 0)
                        : (s.state != NR_HOST_ACTIVE || s.transferredFrames != 1 || s.lastSerial == UINT64_MAX ||
                           serial != s.lastSerial + 1)))
            return false;
        s.state = frame == 0 ? NR_HOST_ARMED : NR_HOST_ACTIVE;
        s.lastSerial = serial;
        s.flags |= NR_HOST_PRIVATE_ARM | NR_HOST_SOURCE_BOUND;
        return true;
    }
    void hostCaptureTerminal(uint64_t reason, bool uncertain) noexcept {
        std::lock_guard lock(control.mutex);
        // A later all-return guard must not downgrade already uncertain work or
        // replace the original failure with its generic missing-transfer reason.
        if (control.terminal && (control.status.state == NR_HOST_UNCERTAIN || !uncertain))
            return;
        control.terminal = true;
        control.status.state = uncertain ? NR_HOST_UNCERTAIN : NR_HOST_ABORTED;
        control.status.failureReason = reason;
    }
} // namespace virtualdesktop_openxr::nr_capture
extern "C" uint32_t NR_PRIVATE_CALL VDXR_PrivatePairNRSourceV1(const NrSourceWitnessV1* witness) noexcept {
    using namespace virtualdesktop_openxr::nr_capture;
    const auto& r = hostRequest();
    if (!r.valid || !witness)
        return 0;
    NrSourceWitnessV1 copy = *witness;
    NrError error{};
    if (!nr_validate_source(&copy, sizeof copy, &error) || copy.header.requestNonceLo != r.nonce[0] ||
        copy.header.requestNonceHi != r.nonce[1] || copy.header.pid != r.pid ||
        copy.header.creationFiletime != r.creation || copy.header.endThread != GetCurrentThreadId() ||
        copy.header.sourceModuleIndex != 4)
        return 0;
    std::lock_guard lock(control.mutex);
    if (control.terminal || !control.instance || !control.sessionHandle || control.sourceObservations > 8192 - 4 ||
        copy.header.publicWitnessId <= control.lastWitness)
        return 0;
    control.latest = copy;
    control.hasSource = true;
    control.sourceObservations += 4;
    control.lastWitness = copy.header.publicWitnessId;
    control.status.sourceWitnessId = copy.header.publicWitnessId;
    control.status.sourceSubmissionId = copy.header.applicationSubmissionId;
    return 1;
}
extern "C" uint32_t NR_PRIVATE_CALL VDXR_PrivatePairNRStatusV1(NrHostStatusV1* status) noexcept {
    using namespace virtualdesktop_openxr::nr_capture;
    if (!status || status->size != 256 || status->version != 1)
        return 0;
    std::lock_guard lock(control.mutex);
    *status = control.status;
    status->size = 256;
    status->version = 1;
    return 1;
}

extern "C" uint32_t NR_PRIVATE_CALL VDXR_PrivatePairNRReleaseV1(const NrHostReleaseV1* request) noexcept {
    using namespace virtualdesktop_openxr::nr_capture;
    const auto& configured = hostRequest();
    if (!configured.valid || !request)
        return 0;
    const NrHostReleaseV1 input = *request;
    if (input.size != 64 || input.version != 1 || input.reserved || input.attempt != configured.attempt ||
        input.nonceLo != configured.nonce[0] || input.nonceHi != configured.nonce[1] || input.pid != configured.pid ||
        input.creationFiletime != configured.creation)
        return 0;
    NrReadyV1 ready{};
    ReceiverCall receiver{};
    OwnedGroupBoundary groups[2][3]{};
    {
        std::lock_guard lock(control.mutex);
        if (control.terminal)
            return 0;
        if (control.status.state == NR_HOST_SETTLED)
            return 1;
        if (control.releaseBusy || control.status.transferredFrames != 2 ||
            control.status.state != NR_HOST_TRANSFERRED || !control.receiver)
            return 0;
        control.releaseBusy = true;
        ready = control.ready;
        receiver = control.receiver;
        for (size_t f = 0; f < 2; ++f)
            for (size_t g = 0; g < 3; ++g)
                groups[f][g] = control.groups[f][g];
    }
    struct ReleaseGuard {
        ~ReleaseGuard() {
            std::lock_guard lock(control.mutex);
            control.releaseBusy = false;
        }
    } releaseGuard;
    const std::unique_ptr<NrPacketV1> queryStorage(new (std::nothrow) NrPacketV1);
    if (!queryStorage)
        return 0;
    std::memset(queryStorage.get(), 0, sizeof(NrPacketV1));
    auto& query = *queryStorage;
    query.header.magic = NR_MAGIC;
    query.header.size = sizeof query;
    query.header.version = 1;
    query.header.operation = NR_QUERY;
    query.header.attempt = configured.attempt;
    query.identity.nonce[0] = configured.nonce[0];
    query.identity.nonce[1] = configured.nonce[1];
    query.identity.pid = configured.pid;
    query.identity.creationFiletime = configured.creation;
    query.identity.sessionNonce[0] = ready.sessionNonceLo;
    query.identity.sessionNonce[1] = ready.sessionNonceHi;
    query.identity.fgContextAddress = ready.fgContextAddress;
    query.identity.fgContextLifetimeEpoch = ready.fgContextLifetimeEpoch;
    query.identity.fgHistoryGeneration = ready.fgHistoryGeneration;
    NrAckV1 ack{};
    NrError error{};
    // Foreign export and all following COM calls run without either metadata lock.
    const auto returned = receiver(&query, &ack);
    if (!nr_validate_control_ack(&query, returned, &ack, sizeof ack, &error)) {
        hostCaptureTerminal(NR_BAD_ACK, true);
        return 0;
    }
    if (returned == NR_UNCERTAIN || returned == NR_REJECTED) {
        hostCaptureTerminal(NR_BAD_ACK, returned == NR_UNCERTAIN);
        return 0;
    }
    if (returned != NR_SETTLED)
        return 0;
    uint64_t released = 0;
    for (const auto& frame : groups)
        for (const auto& group : frame) {
            if (!group.id.valid()) {
                hostCaptureTerminal(NR_BAD_GROUP, true);
                return 0;
            }
            auto state = workOwner.snapshot(group.id);
            if (state.settled) {
                ++released;
                continue;
            }
            const auto completed =
                group.fence12 ? group.fence12->GetCompletedValue() : group.fence11->GetCompletedValue();
            if (completed == UINT64_MAX) {
                hostCaptureTerminal(NR_BAD_GROUP, true);
                return 0;
            }
            if (completed < group.boundary)
                return 0;
            if (!workOwner.observeCompleted(group.id, group.actual, completed)) {
                hostCaptureTerminal(NR_BAD_GROUP, true);
                return 0;
            }
            state = workOwner.snapshot(group.id);
            if (!state.canRelease)
                return 0;
            auto token = workOwner.beginRelease(group.id);
            if (!token.valid() || !token.execute() || !workOwner.finishRelease(group.id, std::move(token))) {
                hostCaptureTerminal(NR_BAD_REF, true);
                return 0;
            }
            state = workOwner.snapshot(group.id);
            if (!state.settled || state.referenceInventory || state.futureReaderPending || state.worker) {
                hostCaptureTerminal(NR_BAD_GROUP, true);
                return 0;
            }
            ++released;
        }
    if (released != 6)
        return 0;
    {
        std::lock_guard lock(control.mutex);
        if (control.terminal)
            return 0;
        control.status.releasedGroups = released;
        control.status.flags |= NR_HOST_ALL_REFS_RELEASED | NR_HOST_NO_READERS;
        control.status.state = NR_HOST_SETTLED;
    }
    captureEngine().releaseModuleFiles();
    // Receiver SETTLED already proved internal manifest publication and closure.
    // This does not assert external source/math/depth adjudication.
    return 1;
}
