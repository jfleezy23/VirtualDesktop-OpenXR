#include "nr_fg_recovery_fixture.h"

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void
        recovery(const wchar_t* backend, const wchar_t* module, unsigned rate, const std::wstring& scenario) {
            const unsigned skipped = scenario == L"skip-s1"      ? 1
                                     : scenario == L"skip-s1-s2" ? 2
                                     : scenario == L"skip-all"   ? 3
                                                                 : 0;
            const bool afterS1 = scenario == L"after-s1-pending-s2";
            const auto keepSlot = [&](unsigned group, unsigned slot) {
                return group != 1 || (afterS1 ? slot != 1 : slot >= skipped);
            };
            const bool retain = scenario == L"normal";
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks backendHooks(backend, state);
            RecoveryClockHooks clock;
            Interop hw;
            EarlyOutputGate gate(hw.gpu.device.Get());
            RecoveryFenceEvents events(hw.device.Get());
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
            StrictNativeLedger ledger;
            const int64_t period = static_cast<int64_t>(1e9 / rate);
            const int64_t first = runtime.ovrTimeToXrTime(clock.origin + .3);
            const auto roleFor = [](unsigned slot) { return slot == 3 ? 2u : slot == 2 ? 3u : slot; };
            for (unsigned group = 0; group < 3; ++group)
                for (unsigned slot = 0; slot < 4; ++slot)
                    if (keepSlot(group, slot))
                        ledger.expected.push_back({static_cast<long long>(4 * group + slot),
                                                   roleFor(slot),
                                                   first + (4 * group + slot) * period,
                                                   slot == 3 ? 1.f : .25f * (slot + 1)});
            OwnedFrameTransaction copyCheck;
            std::atomic<unsigned> ended{};
            state.observePhase = [&](char operation, long long id) { return ledger.phase(operation, id); };
            state.observeWait = [&](long long id) {
                // Markers count actual Ends. Native IDs intentionally contain
                // holes and cannot be used as completion-fence values.
                const auto preceding = ended.load();
                if (preceding) {
                    HANDLE marker = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    require(marker != nullptr, "End marker event");
                    const auto registered = presented->SetEventOnCompletion(preceding, marker);
                    if (FAILED(registered)) {
                        CloseHandle(marker);
                        success(registered, "End marker registration");
                    }
                    const auto waited = WaitForSingleObject(marker, 1000);
                    CloseHandle(marker);
                    require(waited == WAIT_OBJECT_0 && presented->GetCompletedValue() >= preceding &&
                                presented->GetCompletedValue() != UINT64_MAX,
                            "prior actual End GPU work completes before later native Wait");
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
                gate.release();
                events.stop();
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
                gate.holdFirst = group == 1 && !retain && !afterS1;
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
                if (group != 1 || retain) {
                    gate.release();
                    hw.gpu.wait(full, fullValue);
                } else if (afterS1) {
                    gate.releaseFirst();
                    hw.gpu.wait(gate.first.Get(), gate.firstValue);
                }
                recoveryClock = runtime.xrTimeToOvrTime(first + base * period) - .002;
                if (group == 1 && !retain)
                    events.watch(fullValue);
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
                if (group == 1 && !retain && !afterS1) {
                    require(events.waitCount(1, 1000ms), "worker registered a real pending S1 readiness event");
                    require(gate.first && gate.first->GetCompletedValue() < gate.firstValue &&
                                full->GetCompletedValue() < fullValue,
                            "S1 and full work remain genuinely pending at expiry");
                    recoveryClock = runtime.xrTimeToOvrTime(first + (base + skipped - 1) * period) + .001;
                    if (skipped < 3) {
                        const bool advanced = events.waitCount(2, 1000ms);
                        require(!ledger.failed, "expired S1 must be omitted before any Wait/Begin/End for native4");
                        require(advanced, "worker independently awaits later full-ready role after expired S1");
                        require(full->GetCompletedValue() < fullValue,
                                "later-role event is observed before GPU release");
                        gate.release();
                        hw.gpu.wait(full, fullValue);
                        events.stop();
                    }
                }
                if (group == 1 && afterS1) {
                    require(state.waitFor([&] { return state.frames.size() >= 5 || ledger.failed; }, 1000ms),
                            "S1 native4 actually ends before later-role expiry");
                    require(!ledger.failed && events.waitCount(1, 1000ms),
                            "full readiness remains pending after S1 End");
                    std::unique_lock blockedContext(runtime.m_fgContextMutex);
                    require(ledger.closed() && full->GetCompletedValue() < fullValue,
                            "S1 cycle is closed while S2/full remain pending");
                    events.sampleLastPendingFence();
                    recoveryClock = runtime.xrTimeToOvrTime(first + (base + 1) * period) + .001;
                    blockedContext.unlock();
                    require(events.waitPendingSnapshots(2, 1000ms) && !ledger.failed,
                            "expired S2 is followed by a real pending S3 readiness probe");
                    require(full->GetCompletedValue() < fullValue, "full remains held through later-role probes");
                    gate.release();
                    hw.gpu.wait(full, fullValue);
                    events.stop();
                }
                expectedEndCount += group == 1 ? (afterS1 ? 3 : 4 - skipped) : 4;
                const bool complete =
                    state.waitFor([&] { return state.frames.size() >= expectedEndCount || ledger.failed; }, 2000ms);
                require(!ledger.failed, "strict native lifecycle/ID ledger accepts only the selected original slots");
                require(complete, "every selected native End completes within bound");
                if (group == 1 && skipped == 3) {
                    HANDLE marker = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    require(marker != nullptr, "B-only handoff event");
                    const auto registered = presented->SetEventOnCompletion(ended.load(), marker);
                    if (FAILED(registered)) {
                        CloseHandle(marker);
                        success(registered, "B-only marker registration");
                    }
                    const auto waited = WaitForSingleObject(marker, 1000);
                    CloseHandle(marker);
                    require(waited == WAIT_OBJECT_0 && presented->GetCompletedValue() >= ended.load() &&
                                presented->GetCompletedValue() != UINT64_MAX,
                            "actual B7 GPU handoff completes while omitted synthesis remains held");
                    require(full->GetCompletedValue() < fullValue,
                            "B7 does not imply completion of omitted synthetic work");
                    std::unique_lock lease(runtime.m_fgContextMutex);
                    auto& b = runtime.m_fgPresentationBanks[copyCheck.ticket.bank][2];
                    require(hw.read(b.images[0].Get()) == pixels,
                            "B-only recovery has exact pixels before omitted synthesis completes");
                    auto attempted = copyCheck;
                    require(runtime.m_frameGeneration->retire(attempted) == VDXRFG_E_BUSY,
                            "omitted synthetic work still protects the module bank from retirement");
                    lease.unlock();
                    gate.release();
                    events.stop();
                }
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
                if (scenario == L"normal" || scenario == L"skip-s1") {
                    const bool expectedReanchor = group == 1 && skipped != 0;
                    require(runtime.m_fgTimelineReanchor.load() == expectedReanchor,
                            "only a genuine skipped-slot miss reanchors the timeline; normal final B does not");
                    if (expectedReanchor)
                        runtime.m_fgTimelineReanchor = false;
                }
                require(runtime.m_fgWorkerGroupNativeId == base + 4,
                        "next producer reservation uses the original group end, not submitted End count");
                auto& b = runtime.m_fgPresentationBanks[copyCheck.ticket.bank][2];
                require(hw.read(b.images[0].Get()) == pixels, "finalized real B is exact after recovery");
                std::vector<std::vector<uint8_t>> synthetic;
                const unsigned firstSelected = group == 1 ? skipped : 0;
                for (unsigned slot = firstSelected; slot < 3; ++slot) {
                    if (!keepSlot(group, slot))
                        continue;
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
            stopDrainCleanup();
            cleanup.Deactivate();
            hw.gpu.noErrors();
            std::cout << "PASS: recovery-only closed cycles, original IDs/fractions/history, no native headset claim\n";
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
        require(argc == 5,
                "PRIVATE_SDK_DIRECTORY MODULE REFRESH skip-s1|skip-s1-s2|skip-all|normal|after-s1-pending-s2 required");
        const unsigned rate = static_cast<unsigned>(std::stoul(argv[3]));
        require(rate == 72 || rate == 80 || rate == 90 || rate == 96 || rate == 100 || rate == 120 || rate == 144,
                "supported native refresh");
        const std::wstring scenario = argv[4];
        require(scenario == L"skip-s1" || scenario == L"skip-s1-s2" || scenario == L"skip-all" ||
                    scenario == L"normal" || scenario == L"after-s1-pending-s2",
                "known closed-cycle recovery scenario");
        virtualdesktop_openxr::RuntimeFrameGenerationRegression::recovery(argv[1], argv[2], rate, scenario);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
