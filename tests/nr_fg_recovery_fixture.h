#pragma once
// Recovery-only policy through the real host worker and original module.
// Controlled private SDK: proves host lifecycle/ownership, not VD compatibility.
#define wmain retainedRecoveryAsyncMain
#include "nonnr_async_regression.cpp"
#undef wmain
#define main retainedRecoveryInteropMain
#include "nr_fg_integration_regression.cpp"
#undef main
#include "nr_fg_recovery_gates.h"

namespace {
    std::atomic<double> recoveryClock{};
    decltype(&ovr_GetTimeInSeconds) originalRecoveryTime{};
    decltype(&ovr_CommitTextureSwapChain) originalRecoveryCommit{};
    double OVR_CDECL recoveryTime() {
        return recoveryClock.load();
    }
    ovrResult OVR_CDECL recoveryCommit(ovrSession, ovrTextureSwapChain) {
        return ovrSuccess;
    }
    struct RecoveryClockHooks {
        const double origin = ovr_GetTimeInSeconds();
        RecoveryClockHooks() {
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            originalRecoveryTime =
                reinterpret_cast<decltype(originalRecoveryTime)>(GetProcAddress(module, "ovr_GetTimeInSeconds"));
            originalRecoveryCommit = reinterpret_cast<decltype(originalRecoveryCommit)>(
                GetProcAddress(module, "ovr_CommitTextureSwapChain"));
            require(originalRecoveryTime && originalRecoveryCommit, "private SDK timing/commit exports");
            recoveryClock = origin;
            attach(true);
        }
        void attach(bool enable) {
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            const auto action = enable ? DetourAttach : DetourDetach;
            checkDetour(action(reinterpret_cast<PVOID*>(&originalRecoveryTime), recoveryTime));
            checkDetour(action(reinterpret_cast<PVOID*>(&originalRecoveryCommit), recoveryCommit));
            checkDetour(DetourTransactionCommit());
        }
        ~RecoveryClockHooks() {
            try {
                attach(false);
            } catch (...) {
                std::_Exit(3);
            }
        }
    };

    struct ExpectedEnd {
        long long id;
        unsigned role;
        int64_t target;
        float fraction;
    };
    struct StrictNativeLedger {
        std::vector<ExpectedEnd> expected;
        std::mutex mutex;
        size_t cursor{};
        long long waited{-1}, begun{-1};
        ExpectedEnd ending{};
        std::atomic<bool> failed{};
        std::string error;
        std::vector<std::pair<char, long long>> phases;
        ovrResult phase(char operation, long long id) {
            std::lock_guard lock(mutex);
            phases.emplace_back(operation, id);
            const auto reject = [&] {
                failed = true;
                error = "native phase mismatch: phase=" + std::string(1, operation) +
                        " observed_id=" + std::to_string(id) +
                        " expected_id=" + (cursor < expected.size() ? std::to_string(expected[cursor].id) : "none") +
                        " waited=" + std::to_string(waited) + " begun=" + std::to_string(begun);
                std::cerr << "LEDGER_RED " << error << '\n';
                return ovrError_InvalidParameter;
            };
            if (failed || cursor >= expected.size() || expected[cursor].id != id)
                return reject();
            if (operation == 'W') {
                if (waited != -1 || begun != -1)
                    return reject();
                waited = id;
            } else if (operation == 'B') {
                if (waited != id || begun != -1)
                    return reject();
                waited = -1;
                begun = id;
            } else if (operation == 'E') {
                if (waited != -1 || begun != id)
                    return reject();
                ending = expected[cursor++];
                begun = -1;
            } else
                return reject();
            return ovrSuccess;
        }
        ExpectedEnd currentEnd() {
            std::lock_guard lock(mutex);
            return ending;
        }
        size_t completed() {
            std::lock_guard lock(mutex);
            return cursor;
        }
        bool closed() {
            std::lock_guard lock(mutex);
            return waited == -1 && begun == -1;
        }
    };
    void verifyRecoveryLedgerRejections() {
        const auto initialize = [](StrictNativeLedger& ledger) {
            ledger.expected = {{5, 1, 100, .5f}, {6, 3, 200, .75f}};
        };
        {
            StrictNativeLedger ledger;
            initialize(ledger);
            require(OVR_FAILURE(ledger.phase('W', 4)), "ledger rejects an expired native ID");
        }
        {
            StrictNativeLedger ledger;
            initialize(ledger);
            require(OVR_FAILURE(ledger.phase('B', 5)), "ledger rejects Begin without matching Wait");
        }
        {
            StrictNativeLedger ledger;
            initialize(ledger);
            require(OVR_SUCCESS(ledger.phase('W', 5)) && OVR_SUCCESS(ledger.phase('B', 5)) &&
                        OVR_FAILURE(ledger.phase('W', 6)),
                    "ledger rejects superseding an open Begin");
        }
        {
            StrictNativeLedger ledger;
            initialize(ledger);
            require(OVR_SUCCESS(ledger.phase('W', 5)) && OVR_SUCCESS(ledger.phase('B', 5)) &&
                        OVR_FAILURE(ledger.phase('E', 6)),
                    "ledger rejects End with a different native ID");
        }
        {
            StrictNativeLedger ledger;
            initialize(ledger);
            require(OVR_SUCCESS(ledger.phase('W', 5)) && OVR_SUCCESS(ledger.phase('B', 5)) &&
                        OVR_SUCCESS(ledger.phase('E', 5)) && OVR_FAILURE(ledger.phase('W', 5)),
                    "ledger rejects replay of a completed original slot");
        }
        std::cout << "PASS: strict native ledger rejection controls (CPU only)\n";
    }

} // namespace
