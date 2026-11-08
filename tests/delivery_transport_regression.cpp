#define wmain retainedAllocationProbeMain
#include "sdk_delivery_device_probe.cpp"
#undef wmain
#define wmain retainedDeliveryProbeMain
#include "fg_delivery_isolation_regression.cpp"
#undef wmain
#include "../virtualdesktop-openxr/delivery_transport.h"
#include <atomic>
#include <exception>
#include <future>
#include <ScopeGuard.h>

namespace {
    struct TransportFaults {
        using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
        inline static Signal originalSignal{};
        using Wait = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
        inline static Wait originalWait{};
        inline static decltype(&ovr_CommitTextureSwapChain) originalCommit{};
        inline static TransportFaults* active{};
        ID3D11DeviceContext4* context{};
        ovrTextureSwapChain badCommit{};
        bool badSignal{}, badWait{};
        unsigned injections{};
        static HRESULT STDMETHODCALLTYPE wait(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
            if (active && active->context == context && active->badWait) {
                active->badWait = false;
                ++active->injections;
                return E_FAIL;
            }
            return originalWait(context, fence, value);
        }
        static HRESULT STDMETHODCALLTYPE signal(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
            if (active && active->context == context && active->badSignal) {
                active->badSignal = false;
                ++active->injections;
                return E_FAIL;
            }
            return originalSignal(context, fence, value);
        }
        static ovrResult OVR_CDECL commit(ovrSession session, ovrTextureSwapChain chain) {
            if (active && active->badCommit == chain) {
                ++active->injections;
                return ovrError_RuntimeException;
            }
            return originalCommit(session, chain);
        }
        explicit TransportFaults(ID3D11DeviceContext4* target) : context(target) {
            check(!active, "one transport fault hook");
            originalSignal = reinterpret_cast<Signal>((*reinterpret_cast<void***>(target))[deliveryContextSlot(4)]);
            originalWait = reinterpret_cast<Wait>((*reinterpret_cast<void***>(target))[deliveryContextSlot(3)]);
            originalCommit = reinterpret_cast<decltype(originalCommit)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_CommitTextureSwapChain"));
            check(originalCommit, "controlled SDK Commit export");
            active = this;
            check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalCommit), commit) == NO_ERROR &&
                      DetourTransactionCommit() == NO_ERROR,
                  "install transport failure hooks");
        }
        ~TransportFaults() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalCommit), commit) || DetourTransactionCommit())
                std::_Exit(3);
            active = nullptr;
        }
    };

    struct RetirementObservation {
        using Signal = TransportFaults::Signal;
        inline static Signal originalSignal{};
        inline static decltype(&ovr_DestroyTextureSwapChain) originalDestroy{};
        inline static RetirementObservation* active{};
        ID3D11DeviceContext4* producer{};
        ID3D11DeviceContext4* presentation{};
        ovrTextureSwapChain pinned{}, eligible{};
        std::atomic<unsigned> producerSignals{}, presentationSignals{}, pinnedDestroys{}, eligibleDestroys{};
        std::atomic<ID3D11Fence*> producerCompletion{};
        std::atomic<UINT64> producerCompletionValue{};

        static HRESULT STDMETHODCALLTYPE signal(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
            if (active) {
                if (context == active->producer) {
                    active->producerCompletionValue = value;
                    active->producerCompletion = fence;
                    ++active->producerSignals;
                }
                if (context == active->presentation)
                    ++active->presentationSignals;
            }
            return originalSignal(context, fence, value);
        }
        static void OVR_CDECL destroy(ovrSession session, ovrTextureSwapChain chain) {
            if (active) {
                if (chain == active->pinned)
                    ++active->pinnedDestroys;
                if (chain == active->eligible)
                    ++active->eligibleDestroys;
            }
            originalDestroy(session, chain);
        }
        RetirementObservation(ID3D11DeviceContext4* source, ID3D11DeviceContext4* target, ovrTextureSwapChain chain)
            : producer(source), presentation(target), pinned(chain) {
            check(!active, "one retirement observer");
            const auto producerSignal = (*reinterpret_cast<void***>(source))[deliveryContextSlot(4)];
            const auto presentationSignal = (*reinterpret_cast<void***>(target))[deliveryContextSlot(4)];
            check(producerSignal == presentationSignal, "retirement observer covers both real context signals");
            originalSignal = reinterpret_cast<Signal>(producerSignal);
            originalDestroy = reinterpret_cast<decltype(originalDestroy)>(
                GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_DestroyTextureSwapChain"));
            check(originalDestroy, "controlled SDK Destroy export");
            active = this;
            check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalDestroy), destroy) == NO_ERROR &&
                      DetourTransactionCommit() == NO_ERROR,
                  "install forwarding retirement observers");
        }
        ~RetirementObservation() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDestroy), destroy) || DetourTransactionCommit())
                std::_Exit(3);
            active = nullptr;
        }
    };
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3, "usage: private-SDK-prefix metadata|pixels|reuse");
        const std::wstring scenario = argv[2];
        const bool fenceDrain = scenario.rfind(L"drain-fence-", 0) == 0;
        check(scenario == L"metadata" || scenario == L"pixels" || scenario == L"reuse" || scenario == L"pending" ||
                  scenario == L"cancel" || scenario == L"failed-wait" || scenario == L"failed-signal" ||
                  scenario == L"partial-commit" || scenario == L"native-commit-failure" ||
                  scenario == L"pinned-retire" || fenceDrain,
              "known transport case");
        auto hwOwner = std::make_unique<Interop>();
        auto& hw = *hwOwner;
        auto presentationOwner = std::make_unique<DeliveryDevice>(hw.gpu.device.Get());
        auto& presentation = *presentationOwner;
        auto sessionOwner = std::make_unique<Session>();
        auto& session = *sessionOwner;
        ovrInitParams init{};
        init.Flags = ovrInit_RequestVersion | ovrInit_FocusAware;
        init.RequestedMinorVersion = OVR_MINOR_VERSION;
        sdk(ovr_InitializeWithPathOverride(&init, argv[1]), "initialize controlled SDK");
        session.initialized = true;
        ovrGraphicsLuid luid{};
        sdk(ovr_Create(&session.value, &luid), "create controlled SDK session");
        auto transportOwner = std::make_unique<DeliveryTransport>(
            session.value, hw.device.Get(), hw.context.Get(), presentation.device.Get(), presentation.context.Get());
        auto& transport = *transportOwner;
        auto retainUnknownGraph = MakeScopeGuard([&] {
            if (transport.completionUnknown()) {
                (void)transportOwner.release();
                (void)sessionOwner.release();
                (void)presentationOwner.release();
                (void)hwOwner.release();
            }
        });
        ovrTextureSwapChainDesc desc{};
        desc.Type = ovrTexture_2D;
        desc.Width = width;
        desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleCount = 1;
        desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.MiscFlags = ovrTextureMisc_DX_Typeless;
        desc.BindFlags = ovrTextureBind_DX_RenderTarget;
        ovrTextureSwapChain chain{};
        sdk(transport.create(desc, &chain), "create producer-backed native chain");
        if (fenceDrain) {
            using Mode = FenceCompletionScript::Mode;
            check(scenario == L"drain-fence-ready" || scenario == L"drain-fence-stale" ||
                      scenario == L"drain-fence-split-sentinel" || scenario == L"drain-fence-removed-initial" ||
                      scenario == L"drain-fence-removed-postwake",
                  "known drain COM fence observation");
            const bool ready = scenario == L"drain-fence-ready";
            const bool initialRemoved = scenario == L"drain-fence-removed-initial";
            FenceCompletionScript observed(hw.device.Get(), hw.context.Get());
            observed.begin(ready                                         ? Mode::PendingReady
                           : initialRemoved                              ? Mode::RemovedInitial
                           : scenario == L"drain-fence-removed-postwake" ? Mode::RemovedAfterWake
                           : scenario == L"drain-fence-split-sentinel"   ? Mode::SplitSentinel
                                                                         : Mode::StaleWake);
            bool failed{};
            try {
                transport.drain();
            } catch (...) {
                failed = true;
            }
            observed.end();
            observed.report("DeliveryTransport::drain");
            std::printf("DRAIN_COM_FAULT_RESULT failed=%u unknown=%u\n",
                        unsigned(failed),
                        unsigned(transport.completionUnknown()));
            check(observed.covered && observed.signals == 1 && observed.signaledValue,
                  "public drain reached its real producer Signal and observed COM fence");
            check(failed == !ready && transport.completionUnknown() == !ready,
                  "post-wake pending or removed snapshot cannot certify drain, including scripted zero-then-MAX");
            check(observed.initialReads == 1 && observed.postReads == (initialRemoved ? 0u : 1u) &&
                      observed.eventCalls == (initialRemoved ? 0u : 1u),
                  "drain uses exactly one initial and one authoritative post-wake completion sample");
            if (ready) {
                transport.shutdown();
                hw.gpu.noErrors();
            } else {
                bool refused{};
                try {
                    transport.shutdown();
                } catch (...) {
                    refused = true;
                }
                check(refused && transport.completionUnknown(),
                      "injected uncertain completion retains the delivery graph on shutdown");
            }
            std::puts("PASS: bounded COM fence snapshot observation; no physical GPU reset is claimed");
            return 0;
        }
        if (scenario == L"pinned-retire") {
            RetirementObservation observed(hw.context.Get(), presentation.context.Get(), chain);
            const auto collectWithHeldQueues = [&](bool canReclaim) {
                ProducerHold producerHold(hw);
                ProducerHold presentationHold(
                    presentation.device.Get(), presentation.context.Get(), hw.gpu.device.Get());
                const auto producerBefore = observed.producerSignals.load();
                const auto presentationBefore = observed.presentationSignals.load();
                const auto pinnedBefore = observed.pinnedDestroys.load();
                const auto eligibleBefore = observed.eligibleDestroys.load();
                auto collection = std::async(std::launch::async, [&] { transport.collectRetired(); });
                const bool returnedWhileHeld =
                    collection.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready;
                const bool producerStillHeld = producerHold.after->GetCompletedValue() == 0;
                const bool presentationStillHeld = presentationHold.after->GetCompletedValue() == 0;
                const auto producerWhileHeld = observed.producerSignals.load();
                const auto presentationWhileHeld = observed.presentationSignals.load();
                const bool retainedWhileHeld =
                    observed.pinnedDestroys == pinnedBefore && observed.eligibleDestroys == eligibleBefore;

                // Release only fixture-owned fences before any assertion or future teardown.
                const auto releasedProducer = producerHold.release->Signal(1);
                producerHold.armed = FAILED(releasedProducer);
                bool producerDrainComplete = !canReclaim;
                std::exception_ptr producerCompletionError;
                if (canReclaim && producerWhileHeld == producerBefore + 1) {
                    try {
                        producerDrainComplete =
                            completed(observed.producerCompletion.load(), observed.producerCompletionValue.load());
                    } catch (...) {
                        producerCompletionError = std::current_exception();
                    }
                }
                const bool returnedWithPresentationHeld =
                    collection.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
                const bool retainedWithPresentationHeld =
                    observed.pinnedDestroys == pinnedBefore && observed.eligibleDestroys == eligibleBefore;
                const auto releasedPresentation = presentationHold.release->Signal(1);
                presentationHold.armed = FAILED(releasedPresentation);
                if (collection.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready) {
                    std::fputs("FAIL: retirement collector did not settle after owned GPU gate releases\n", stderr);
                    std::_Exit(3); // A live worker must not outlive its transport, session or device graph.
                }
                collection.get();
                if (producerCompletionError)
                    std::rethrow_exception(producerCompletionError);
                success(releasedProducer, "release retirement producer hold");
                success(releasedPresentation, "release retirement presentation hold");
                check(completed(producerHold.after.Get(), 1) && completed(presentationHold.after.Get(), 1),
                      "both fixture-owned GPU queue holds resume");
                check(producerStillHeld && presentationStillHeld, "retirement checks run behind real GPU waits");
                check(retainedWhileHeld && retainedWithPresentationHeld,
                      "native retirement retains both chains until both GPU queues can complete");
                if (canReclaim) {
                    check(producerDrainComplete, "producer drain completes while presentation remains held");
                    check(!returnedWhileHeld && !returnedWithPresentationHeld,
                          "eligible retirement waits for producer and presentation completion");
                    check(observed.producerSignals == producerBefore + 1 &&
                              observed.presentationSignals == presentationBefore + 1,
                          "eligible retirement retains one real completion signal on each GPU queue");
                } else {
                    check(returnedWhileHeld,
                          "empty or entirely batch-pinned retirement returns before GPU gate release");
                    check(producerWhileHeld == producerBefore && presentationWhileHeld == presentationBefore &&
                              observed.producerSignals == producerBefore &&
                              observed.presentationSignals == presentationBefore,
                          "empty or entirely batch-pinned retirement queues no GPU drain signals");
                }
                check(!transport.completionUnknown(), "retirement collection preserves known completion");
            };

            collectWithHeldQueues(false); // An active chain is not a retirement candidate.
            sdk(transport.commit(chain), "receipt pins chain before retirement");
            auto pinnedBatch = transport.prepare({chain}, 1);
            check(pinnedBatch.images.size() == 1, "one published receipt retains the retired owner");
            transport.destroy(chain);
            collectWithHeldQueues(false);
            check(observed.pinnedDestroys == 0, "batch-pinned native chain remains alive");
            {
                auto heldSdk = transport.sdkLease();
                auto collection = std::async(std::launch::async, [&] { transport.collectRetired(); });
                const bool returnedWhileSdkHeld =
                    collection.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready;
                heldSdk.unlock();
                if (collection.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready) {
                    std::fputs("FAIL: pinned retirement collector did not settle after SDK gate release\n", stderr);
                    std::_Exit(3);
                }
                collection.get();
                check(returnedWhileSdkHeld, "entirely batch-pinned retirement bypasses the SDK rendering gate");
                check(observed.pinnedDestroys == 0, "SDK-gate bypass retains the published batch owner");
            }
            transport.execute(pinnedBatch);
            check(transport.statistics().nativeCopies == 1 && transport.statistics().nativeCommits == 1,
                  "published batch remains executable after logical retirement");

            ovrTextureSwapChain eligible{};
            sdk(transport.create(desc, &eligible, true), "create unpinned native retirement candidate");
            observed.eligible = eligible;
            transport.destroy(eligible);
            collectWithHeldQueues(true);
            check(observed.eligibleDestroys == 1 && observed.pinnedDestroys == 0,
                  "mixed retirement destroys exactly the eligible chain and retains the batch pin");
            pinnedBatch = {};
            collectWithHeldQueues(true);
            check(observed.eligibleDestroys == 1 && observed.pinnedDestroys == 1,
                  "dropping the published batch permits exactly one native destruction");
            collectWithHeldQueues(false);
            transport.shutdown();
            check(observed.eligibleDestroys == 1 && observed.pinnedDestroys == 1,
                  "repeated collection and shutdown cannot destroy either native chain twice");
            hw.gpu.noErrors();
            std::puts("PASS: pinned retirement bypasses GPU drains; eligible retirement drains both queues once");
            return 0;
        }
        if (scenario == L"metadata") {
            auto heldSdk = transport.sdkLease();
            auto query = std::async(std::launch::async, [&] {
                int index{};
                sdk(transport.current(chain, &index), "cached private producer index");
                sdk(transport.commit(chain), "private produced receipt");
                return index;
            });
            const bool independent = query.wait_for(std::chrono::milliseconds(30)) == std::future_status::ready;
            heldSdk.unlock();
            (void)query.get();
            check(independent, "private producer metadata must not wait behind the SDK rendering gate");
            transport.shutdown();
            std::puts("PASS: cached private producer metadata bypasses native rendering gate");
            return 0;
        }
        int count{}, index{}, nativeIndex{};
        sdk(transport.length(chain, &count), "producer image count");
        sdk(transport.current(chain, &index), "prepared producer index");
        sdk(ovr_GetTextureSwapChainCurrentIndex(session.value, chain, &nativeIndex), "native initial index");
        ComPtr<ID3D11Texture2D> source, native;
        sdk(transport.buffer(chain, index, IID_PPV_ARGS(&source)), "private producer source");
        sdk(ovr_GetTextureSwapChainBufferDX(session.value, chain, nativeIndex, IID_PPV_ARGS(&native)), "native target");
        check(owns(hw.device.Get(), source.Get()) && owns(presentation.device.Get(), native.Get()),
              "producer and native allocation ownership are distinct");
        const auto first = scene(0), corrected = scene(24);
        hw.context->UpdateSubresource(source.Get(), 0, nullptr, first.data(), width * 4, 0);
        sdk(transport.commit(chain), "early producer receipt");
        int before{};
        sdk(ovr_GetTextureSwapChainCurrentIndex(session.value, chain, &before), "native index before finalization");
        check(before == nativeIndex, "early producer receipt cannot advance native commit");
        // Mirrors alpha correction occurring after the existing Commit sites.
        hw.context->UpdateSubresource(source.Get(), 0, nullptr, corrected.data(), width * 4, 0);
        auto unused = transport.prepare({}, 1);
        transport.execute(unused);
        check(transport.statistics().nativeCopies == 0 && transport.statistics().nativeCommits == 0,
              "accepted FG does not copy or commit unused private intermediates");
        ovrTextureSwapChain second{};
        if (scenario == L"partial-commit") {
            sdk(transport.create(desc, &second), "second pair chain");
            sdk(transport.commit(second), "second pair receipt");
        }
        auto batch = transport.prepare({chain, chain, second}, 1);
        check(batch.images.size() == (second ? 2u : 1u),
              "shared chains deduplicate while distinct pair members remain");
        if (scenario == L"failed-wait" || scenario == L"failed-signal" || scenario == L"partial-commit" ||
            scenario == L"native-commit-failure") {
            ovrTextureSwapChain direct{};
            sdk(transport.create(desc, &direct, true), "create direct P target before failure");
            int beforeDirect{}, afterDirect{};
            sdk(ovr_GetTextureSwapChainCurrentIndex(session.value, direct, &beforeDirect), "direct initial index");
            TransportFaults faults(presentation.context.Get());
            if (scenario == L"native-commit-failure") {
                faults.badCommit = direct;
                check(OVR_FAILURE(transport.commit(direct)) && faults.injections == 1,
                      "one real native-only Commit failure is injected");
                check(transport.completionUnknown(),
                      "native-only Commit failure must poison uncertain SDK advancement");
                bool refusedDrain{}, refusedShutdown{};
                try {
                    transport.drain();
                } catch (...) {
                    refusedDrain = true;
                }
                try {
                    transport.shutdown();
                } catch (...) {
                    refusedShutdown = true;
                }
                check(refusedDrain && refusedShutdown && transport.completionUnknown(),
                      "failed native-only Commit cannot certify drain or completed teardown");
                check(transport.current(direct, &index) == ovrError_RuntimeException &&
                          transport.current(chain, &index) == ovrError_RuntimeException,
                      "failed native-only Commit closes both acquisition domains");
                std::puts("PASS: failed native-only Commit poisons acquisition; harness quarantines graph at exit");
                return 0;
            }
            if (scenario == L"failed-wait")
                faults.badWait = true;
            else if (scenario == L"failed-signal")
                faults.badSignal = true;
            else
                faults.badCommit = second;
            bool failed{};
            try {
                transport.execute(batch);
            } catch (...) {
                failed = true;
            }
            faults.badWait = false;
            check(failed && faults.injections == 1 && transport.completionUnknown(),
                  "real GPU dependency failure poisons graph");
            check(transport.current(chain, &index) == ovrError_RuntimeException &&
                      transport.current(direct, &index) == ovrError_RuntimeException,
                  "poisoned graph refuses private and native acquisition specifically for poison");
            check(OVR_FAILURE(transport.commit(direct)), "poisoned graph refuses native-only Commit");
            if (scenario == L"partial-commit") {
                check(transport.statistics().nativeCopies == 2 && transport.statistics().nativeCommits == 1,
                      "only the first pair member advances before injected second Commit failure");
                std::puts("PASS: partial native pair failure refuses reuse; harness quarantines the graph at exit");
                return 0;
            }
            sdk(ovr_GetTextureSwapChainCurrentIndex(session.value, direct, &afterDirect), "direct index after refusal");
            check(beforeDirect == afterDirect &&
                      transport.statistics().nativeCopies == (scenario == L"failed-signal" ? 1u : 0u) &&
                      transport.statistics().nativeCommits == 0,
                  "failed dependency publishes no native image, including after a queued copy");
            std::puts("PASS: failed GPU dependency rejects native Commit; harness quarantines the graph at exit");
            return 0;
        }
        if (scenario == L"cancel") {
            transport.shutdown();
            bool rejected{};
            try {
                transport.execute(batch);
            } catch (...) {
                rejected = true;
            }
            check(rejected, "terminal shutdown cancels an outstanding published receipt");
            std::puts("PASS: cancelled pending receipt cannot execute after shutdown");
            return 0;
        }
        if (scenario == L"pending") {
            const auto rejects = [&](const auto& operation, const char* message) {
                bool rejected{};
                try {
                    operation();
                } catch (...) {
                    rejected = true;
                }
                check(rejected && !transport.completionUnknown(), message);
            };
            auto malformed = batch;
            malformed.images.push_back(batch.images[0]);
            rejects([&] { transport.copy(malformed); }, "duplicate receipt rejected before GPU writes");
            malformed = batch;
            malformed.images[0].owner.reset();
            rejects([&] { transport.copy(malformed); }, "null receipt rejected before GPU writes");
            rejects([&] { (void)transport.prepare({chain}, 2); }, "second publication cannot reserve the same source");
            check(transport.statistics().nativeCopies == 0, "invalid receipts queue no copies");
            int reserved{};
            check(OVR_FAILURE(transport.current(chain, &reserved)),
                  "published private slot cannot be reacquired before execute");
            check(OVR_FAILURE(transport.commit(chain)), "published receipt cannot be overwritten before execute");
        }
        if (scenario == L"pending") {
            transport.copy(batch);
            int stillCurrent{};
            sdk(ovr_GetTextureSwapChainCurrentIndex(session.value, chain, &stillCurrent),
                "index before deferred Commit");
            check(stillCurrent == nativeIndex && transport.statistics().nativeCommits == 0,
                  "depth-style copy cannot advance native presentation before B emission");
            DeliveryTransport foreign(session.value,
                                      hw.device.Get(),
                                      hw.context.Get(),
                                      presentation.device.Get(),
                                      presentation.context.Get());
            bool rejected{};
            try {
                foreign.commitCopied(batch);
            } catch (...) {
                rejected = true;
            }
            check(rejected && !foreign.completionUnknown() && foreign.statistics().nativeCommits == 0,
                  "foreign copied receipt cannot commit on a different transport");
            transport.commitCopied(batch);
            const auto beforeReplay = transport.statistics();
            rejected = false;
            try {
                transport.execute(batch);
            } catch (...) {
                rejected = true;
            }
            check(rejected && !transport.completionUnknown() &&
                      transport.statistics().nativeCopies == beforeReplay.nativeCopies &&
                      transport.statistics().nativeCommits == beforeReplay.nativeCommits,
                  "replayed receipt queues neither GPU copies nor another native Commit");
            foreign.shutdown();
        } else {
            transport.execute(batch);
        }
        presentation.drain();
        check(presentation.read(native.Get()) == corrected, "native image includes writes after early Commit receipt");
        const auto statistics = transport.statistics();
        check(statistics.nativeCopies == 1 && statistics.nativeCommits == 1 &&
                  statistics.privateExports == static_cast<uint64_t>(count),
              "one final copy and one resource export per private allocation");
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            HANDLE duplicate{};
            success(transport.duplicateSharedHandle(source.Get(), &duplicate), "duplicate canonical NT export");
            ComPtr<ID3D12Resource> imported;
            const auto result = hw.gpu.device->OpenSharedHandle(duplicate, IID_PPV_ARGS(&imported));
            CloseHandle(duplicate);
            success(result, "NR D3D12 imports reuse the original export");
        }
        if (scenario == L"reuse") {
            sdk(transport.current(chain, &index), "next private index");
            sdk(transport.buffer(chain, index, IID_PPV_ARGS(&source)), "next private image");
            sdk(ovr_GetTextureSwapChainCurrentIndex(session.value, chain, &nativeIndex), "next actual native index");
            sdk(ovr_GetTextureSwapChainBufferDX(session.value, chain, nativeIndex, IID_PPV_ARGS(&native)),
                "next actual native target");
            hw.context->UpdateSubresource(source.Get(), 0, nullptr, corrected.data(), width * 4, 0);
            sdk(transport.commit(chain), "next produced receipt");
            batch = transport.prepare({chain}, 2);
            ProducerHold hold(presentation.device.Get(), presentation.context.Get(), hw.gpu.device.Get());
            transport.execute(batch);
            transport.orderProducerAfterNativeReads();
            hw.context->UpdateSubresource(source.Get(), 0, nullptr, first.data(), width * 4, 0);
            ComPtr<ID3D11Fence> after;
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&after)), "reuse marker");
            success(hw.context->Signal(after.Get(), 1), "mark attempted source reuse");
            hw.context->Flush();
            const bool passedEarly = completed(after.Get(), 1, 10);
            hold.finish();
            check(!passedEarly && completed(after.Get(), 1), "source reuse waits exactly for P's read completion");
            check(presentation.read(native.Get()) == corrected,
                  "source overwrite cannot corrupt the transported frame");
        }
        batch = {};
        transport.destroy(chain);
        transport.shutdown();
        hw.gpu.noErrors();
        std::puts("PASS: final-selection native transport, late writes, exports and reuse");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
