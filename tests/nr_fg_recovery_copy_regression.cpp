#include "nr_fg_recovery_fixture.h"

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void
        recovery(const wchar_t* backend, const wchar_t* module, unsigned rate, const std::wstring& scenario) {
            const bool checkAdmission = scenario == L"admission-normal" || scenario == L"admission-skip-s1";
            const bool pendingCopy = scenario == L"pending-s1";
            const bool precompleted = scenario == L"precompleted-late-s1";
            const unsigned skipped = pendingCopy || scenario == L"admission-skip-s1" ? 1 : 0;
            const long long firstSelectedId = 4 + skipped;
            std::atomic<unsigned> admissionChecks{};
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks backendHooks(backend, state);
            RecoveryClockHooks clock;
            Interop hw;
            EarlyOutputGate gate(hw.gpu.device.Get());
            RuntimeInputRegression::seed(runtime);
            runtime.m_ovrSubmissionDevice = hw.device;
            runtime.m_ovrSubmissionContext = hw.context;
            runtime.m_frameGenerationMode = 4;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            runtime.m_fgEyeResolution = {eyeWidth, height};
            runtime.m_idealFrameDuration = 1. / rate;
            FrameGeneration::InitInfo init{hw.device.Get(),
                                           hw.context.Get(),
                                           hw.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           4,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            success(runtime.m_frameGeneration->initialize(init), "initialize original 4x path");
            for (unsigned bank = 0; bank < 2; ++bank)
                for (unsigned role = 0; role < 4; ++role) {
                    auto& output = runtime.m_fgPresentationBanks[bank][role];
                    output.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x8000 + bank * 4 + role));
                    output.images.push_back(hw.texture());
                }
            ComPtr<ID3D11Fence> presented;
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&presented)),
                    "actual End GPU marker");
            RecoveryConsumerGate consumerGate(hw.gpu.device.Get(), hw.device.Get(), hw.context.Get(), presented.Get());
            StrictNativeLedger ledger;
            const int64_t period = static_cast<int64_t>(1e9 / rate);
            const int64_t first = runtime.ovrTimeToXrTime(clock.origin + .3);
            const auto roleFor = [](unsigned slot) { return slot == 3 ? 2u : slot == 2 ? 3u : slot; };
            for (unsigned group = 0; group < 3; ++group)
                for (unsigned slot = group == 1 ? skipped : 0; slot < 4; ++slot)
                    ledger.expected.push_back({static_cast<long long>(4 * group + slot),
                                               roleFor(slot),
                                               first + (4 * group + slot) * period,
                                               slot == 3 ? 1.f : .25f * (slot + 1)});
            OwnedFrameTransaction copyCheck;
            std::atomic<unsigned> ended{};
            state.observePhase = [&](char operation, long long id) -> ovrResult {
                const auto result = ledger.phase(operation, id);
                if (OVR_FAILURE(result))
                    return result;
                if (checkAdmission && id == firstSelectedId) {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    const bool expected = operation == 'E';
                    const bool actual = runtime.m_asyncSubmissionReady;
                    std::fprintf(stdout,
                                 "ADMISSION phase=%c id=%lld actual=%u expected=%u shared_context=%u\n",
                                 operation,
                                 id,
                                 actual ? 1u : 0u,
                                 expected ? 1u : 0u,
                                 runtime.m_deliveryTransport ? 0u : 1u);
                    if (runtime.m_deliveryTransport || actual != expected) {
                        ledger.failed = true;
                        return ovrError_InvalidParameter;
                    }
                    ++admissionChecks;
                }
                return result;
            };
            state.observeWait = [&](long long id) {
                // No helper waitForCopy, event wait, readback or marker wait is
                // permitted here. The pending-copy case merely releases its
                // real GPU gate at the first correctly selected native Wait.
                if (pendingCopy && id == 5) {
                    require(consumerGate.inserted && consumerGate.pendingWitness,
                            "S1 consumer completion was actually pending before native selection");
                    consumerGate.release();
                }
                const double target = runtime.xrTimeToOvrTime(first + id * period);
                recoveryClock = (std::max)(recoveryClock.load(), target - .002);
            };
            state.observeLayers = [&](const ovrLayerHeader* const* layers, unsigned count) {
                require(count == 1 && layers && layers[0] && layers[0]->Type == ovrLayerType_EyeFov,
                        "one exact stereo output per selected native End");
                const auto& projection = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                const auto role =
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(projection.ColorTexture[0]) - 0x8000) % 4;
                const auto expected = ledger.currentEnd();
                std::cout << "END id=" << expected.id << " role=" << role << " expected=" << expected.role
                          << " target_xr_ns=" << expected.target << " fraction=" << expected.fraction
                          << " now_xr_ns=" << runtime.ovrTimeToXrTime(recoveryClock.load()) << '\n';
                require(role == expected.role, "selected role preserves original native ID and interpolation fraction");
                const auto markerValue = ended.load() + 1;
                success(hw.context->Signal(presented.Get(), markerValue), "actual selected End GPU marker");
                hw.context->Flush();
                ended = markerValue;
            };
            FrameGenerationCadence cadence;
            FinalProjectionInput input;
            input.serial = 1;
            input.period = period;
            input.time = first - period;
            input.slots = cadence.plan(cadence.reserve(0, first - 4 * period, period, 4, 0), 0, false);
            const auto initialPixels = scene(0);
            input.colour[0] = input.colour[1] = hw.texture(&initialPixels);
            for (unsigned eye = 0; eye < 2; ++eye) {
                input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
                input.views[eye].orientation[3] = 1;
                input.views[eye].position[0] = eye ? .032f : -.032f;
                input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
                input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
            }
            OwnedFrameTransaction prime;
            success(runtime.m_frameGeneration->capture(input, prime), "capture initial real source");
            hw.await(*runtime.m_frameGeneration, prime);
            gate.arm(prime.ticket);
            hw.retire(*runtime.m_frameGeneration, prime);
            OwnedFrameTransaction transaction;
            const auto stopDrainCleanup = [&] {
                consumerGate.release();
                gate.release();
                state.release();
                {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    runtime.m_terminateAsyncThread = true;
                    runtime.m_asyncSubmissionCondVar.notify_all();
                }
                if (runtime.m_asyncSubmissionThread.joinable()) {
                    runtime.m_asyncSubmissionThread.join();
                    runtime.m_asyncSubmissionThread = {};
                    runtime.m_needStartAsyncSubmissionThread = true;
                }
                PrefixWorkGuard drain(hw.gpu, gate, hw.device.Get(), hw.context.Get());
                drain.armed = true;
                drain.drain();
                runtime.cleanupSessionResources();
            };
            auto cleanup = MakeScopeGuard([&] {
                try {
                    stopDrainCleanup();
                } catch (...) {
                    PrefixWorkGuard::fatal();
                }
            });
            VDXRFG_TicketV1 previousTicket{};
            bool workerStarted{};
            unsigned expectedEndCount{};
            for (unsigned group = 0; group < 3; ++group) {
                const auto previousTime = input.time;
                const unsigned base = 4 * group;
                const auto reservation = cadence.reserve(0, first + base * period, period, 4, base);
                input.serial = 2 + group;
                input.time = reservation.currentTarget;
                input.slots = cadence.plan(reservation, previousTime, true);
                const auto originalPlan = input.slots;
                require(input.slots.count == 4, "original four-slot plan remains intact");
                for (unsigned slot = 0; slot < 3; ++slot)
                    require(std::abs(input.slots.fractions[slot] - .25f * (slot + 1)) < 1e-6f,
                            "source endpoints retain original quarter fractions");
                const auto pixels = scene(24 + group * 16);
                input.colour[0] = input.colour[1] = hw.texture(&pixels);
                if (group)
                    gate.rearm(previousTicket);
                gate.holdFirst = false;
                success(runtime.m_frameGeneration->capture(input, transaction), "capture changing real pair");
                require(!transaction.ticket.prime && transaction.ticket.previous_serial == 1 + group,
                        "capture history remains valid without reprime across recovery");
                require(transaction.ticket.bank < 2 && transaction.slots.count == 4 &&
                            transaction.slots.targets == originalPlan.targets &&
                            transaction.slots.fractions == originalPlan.fractions,
                        "borrowed bank and original role targets/fractions are preserved");
                previousTicket = transaction.ticket;
                copyCheck = transaction;
                const auto full = static_cast<ID3D12Fence*>(transaction.ticket.output_ready_fence);
                const auto fullValue = transaction.ticket.output_ready_value;
                gate.release();
                hw.gpu.wait(full, fullValue);
                if (group == 1 && precompleted) {
                    // Complete the actual destination fence BEFORE publication
                    // and before the worker makes any selection decision.
                    std::unique_lock lease(runtime.m_fgContextMutex);
                    auto& s1 = runtime.m_fgPresentationBanks[0][0];
                    success(runtime.m_frameGeneration->copyOutput(
                                transaction, FrameGenerationRole::synthetic1, s1.images[0].Get()),
                            "prestage exact S1 destination");
                    success(runtime.m_frameGeneration->waitForCopy(transaction, FrameGenerationRole::synthetic1, 1000),
                            "complete actual S1 copy during prepublication setup");
                    require(runtime.m_frameGeneration->copyReady(transaction, FrameGenerationRole::synthetic1),
                            "genuinely completed late-S1 control is ready before selection");
                    std::cout << "SETUP S1 copy complete before publication=1\n";
                }
                if (group == 1 && checkAdmission && skipped) {
                    // Isolate admission from late pending-copy recovery: S2
                    // must already have its actual destination fence complete
                    // when published past its own target. The worker's
                    // prepareRole reuses this same token/role/destination.
                    std::unique_lock lease(runtime.m_fgContextMutex);
                    auto& s2 = runtime.m_fgPresentationBanks[0][1];
                    success(runtime.m_frameGeneration->copyOutput(
                                transaction, FrameGenerationRole::synthetic2, s2.images[0].Get()),
                            "prestage exact S2 admission destination");
                    success(runtime.m_frameGeneration->waitForCopy(transaction, FrameGenerationRole::synthetic2, 1000),
                            "complete actual selected S2 copy before admission publication");
                    require(runtime.m_frameGeneration->copyReady(transaction, FrameGenerationRole::synthetic2),
                            "late selected S2 destination is genuinely complete before publication");
                    std::cout << "SETUP S2 copy complete before admission publication=1\n";
                }
                recoveryClock = runtime.xrTimeToOvrTime(first + base * period) - .002;
                if (group == 1 && pendingCopy)
                    consumerGate.armAfterRealB(runtime.xrTimeToOvrTime(first + base * period) + .001, recoveryClock);
                if (group == 1 && precompleted)
                    recoveryClock = runtime.xrTimeToOvrTime(first + base * period) + .001;
                if (group == 1 && checkAdmission && skipped)
                    recoveryClock = runtime.xrTimeToOvrTime(first + (base + 1) * period) + .001;
                if (!workerStarted) {
                    RuntimeInputRegression::start(runtime);
                    state.release();
                    runtime.waitForAsyncSubmissionIdle();
                    workerStarted = true;
                }
                require(group == 0 || ledger.closed(),
                        "recovery starts only after the preceding native cycle is closed");
                {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    runtime.m_fgReservationForSubmission = reservation;
                    runtime.m_fgTransactionForSubmission = std::move(transaction);
                    // Moving this aggregate leaves its POD ticket token set.
                    // Publication transfers ownership; match production's clear
                    // before using this local as the next capture output.
                    transaction = {};
                    runtime.m_layersForAsyncSubmission.resize(1);
                    runtime.m_layersForAsyncSubmission[0].Header.Type = ovrLayerType_EyeFov;
                    runtime.m_asyncSubmissionReady = false;
                    runtime.m_asyncSubmissionCondVar.notify_all();
                }
                expectedEndCount += group == 1 ? 4 - skipped : 4;
                const bool complete =
                    state.waitFor([&] { return state.frames.size() >= expectedEndCount || ledger.failed; }, 2000ms);
                require(!ledger.failed, "strict native lifecycle/ID ledger accepts only the selected original slots");
                require(complete, "every selected native End completes within bound");
                {
                    std::unique_lock lock(runtime.m_asyncSubmissionMutex);
                    require(runtime.m_asyncSubmissionCondVar.wait_for(
                                lock,
                                2s,
                                [&] {
                                    return runtime.m_asyncSubmissionError ||
                                           (!runtime.m_fgActiveSubmission &&
                                            runtime.m_layersForAsyncSubmission.empty());
                                }),
                            "group fully retires before bank/source reuse");
                    if (runtime.m_asyncSubmissionError)
                        std::rethrow_exception(runtime.m_asyncSubmissionError);
                }
                require(ledger.closed() && ledger.completed() == expectedEndCount && !runtime.m_fgNeedReprime,
                        "all selected cycles close; presentation omission does not invalidate history");
                require(runtime.m_fgWorkerGroupNativeId == base + 4,
                        "next producer reservation uses the original group end, not submitted End count");
                auto& b = runtime.m_fgPresentationBanks[copyCheck.ticket.bank][2];
                require(hw.read(b.images[0].Get()) == pixels, "finalized real B is exact after recovery");
                std::vector<std::vector<uint8_t>> synthetic;
                const unsigned firstSelected = group == 1 ? skipped : 0;
                for (unsigned slot = firstSelected; slot < 3; ++slot) {
                    auto& output = runtime.m_fgPresentationBanks[0][roleFor(slot)];
                    synthetic.push_back(hw.read(output.images[0].Get()));
                    require(synthetic.back() != pixels && synthetic.back() != initialPixels,
                            "selected role contains an actual changing synthetic image");
                    if (synthetic.size() > 1)
                        require(synthetic.back() != synthetic[synthetic.size() - 2],
                                "selected original fractions produce distinct images");
                }
                std::cout << "GROUP base=" << base << " actual_ends=" << expectedEndCount
                          << " history_previous=" << copyCheck.ticket.previous_serial
                          << " history_current=" << copyCheck.ticket.current_serial << " retired=1\n";
            }
            require(ledger.completed() == ledger.expected.size() && ledger.closed(),
                    "exact complete native phase ledger");
            require(!pendingCopy || (consumerGate.inserted && consumerGate.pendingWitness),
                    "actual pending-copy control was exercised");
            require(!checkAdmission || admissionChecks == 3,
                    "shared-context admission checked at Wait, Begin and pre-End of first surviving cycle");
            stopDrainCleanup();
            cleanup.Deactivate();
            hw.gpu.noErrors();
            std::cout << "PASS: actual consumer completion and shared-context admission, no added SDK callback waits\n";
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--ledger-negative") {
            verifyRecoveryLedgerRejections();
            return 0;
        }
        require(argc == 5, "PRIVATE_SDK_DIRECTORY MODULE REFRESH SCENARIO required");
        const unsigned rate = static_cast<unsigned>(std::stoul(argv[3]));
        require(rate == 72 || rate == 80 || rate == 90 || rate == 96 || rate == 100 || rate == 120 || rate == 144,
                "supported native refresh");
        const std::wstring scenario = argv[4];
        require(scenario == L"pending-s1" || scenario == L"precompleted-late-s1" || scenario == L"admission-normal" ||
                    scenario == L"admission-skip-s1",
                "known copy/admission scenario");
        virtualdesktop_openxr::RuntimeFrameGenerationRegression::recovery(argv[1], argv[2], rate, scenario);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
