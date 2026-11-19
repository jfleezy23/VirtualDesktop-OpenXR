#define wmain retainedStereoAsyncMain
#include "nonnr_async_regression.cpp"
#undef wmain
#define wmain retainedStereoDeliveryMain
#include "fg_delivery_isolation_regression.cpp"
#undef wmain
#include "../virtualdesktop-openxr/frame_generation_layers.h"

namespace {
    struct StereoClock {
        inline static std::atomic<double> time{};
        inline static decltype(&ovr_GetTimeInSeconds) original{};

        static double OVR_CDECL now() {
            return time.load();
        }
        StereoClock() {
            time = ovr_GetTimeInSeconds();
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            original = reinterpret_cast<decltype(original)>(GetProcAddress(module, "ovr_GetTimeInSeconds"));
            require(original, "clock fixture export");
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&original), now));
            checkDetour(DetourTransactionCommit());
        }
        ~StereoClock() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), now) || DetourTransactionCommit())
                std::_Exit(3);
        }
    };

    struct Handoff {
        long long id{};
        ovrLayer_Union layer{};
        int colourIndex{-1}, depthIndex{-1};
        double submitTime{};
    };

    ovrTextureSwapChainDesc description(bool depth) {
        ovrTextureSwapChainDesc desc{};
        desc.Type = ovrTexture_2D;
        desc.Width = width;
        desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleCount = 1;
        desc.Format = depth ? OVR_FORMAT_D32_FLOAT : OVR_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = depth ? ovrTextureBind_DX_DepthStencil : ovrTextureBind_DX_RenderTarget;
        desc.MiscFlags = depth ? ovrTextureMisc_DX_Typeless : 0;
        return desc;
    }

    int lastCommitted(ovrSession session, ovrTextureSwapChain chain) {
        int length{}, current{};
        require(OVR_SUCCESS(ovr_GetTextureSwapChainLength(session, chain, &length)) && length == 3,
                "test must use the actual rotating OVRNull swapchain");
        require(OVR_SUCCESS(ovr_GetTextureSwapChainCurrentIndex(session, chain, &current)),
                "query actual native swapchain index");
        return (current + length - 1) % length;
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void retainedStereo(const wchar_t* backend,
                                   const wchar_t* module,
                                   bool nr,
                                   bool withDepth,
                                   bool consecutive,
                                   bool historyChange,
                                   bool catchUp,
                                   bool preDeadline,
                                   bool steady,
                                   bool warmup,
                                   bool slowEnd,
                                   unsigned multiplier) {
            OpenXrRuntime runtime;
            BackendState sdk;
            BackendHooks hooks(backend, sdk, false);
            StereoClock clock;
            Interop source;
            DeliveryDevice presentation(source.gpu.device.Get());
            RuntimeInputRegression::seed(runtime);
            ovrGraphicsLuid luid{};
            require(OVR_SUCCESS(ovr_Create(&runtime.m_ovrSession, &luid)), "create private OVRNull session");
            runtime.m_ovrSubmissionDevice = source.device;
            runtime.m_ovrSubmissionContext = source.context;
            runtime.m_deliveryTransport = std::make_unique<DeliveryTransport>(runtime.m_ovrSession,
                                                                              source.device.Get(),
                                                                              source.context.Get(),
                                                                              presentation.device.Get(),
                                                                              presentation.context.Get());
            auto& transport = *runtime.m_deliveryTransport;
            runtime.m_frameGenerationMode = multiplier;
            runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
            runtime.m_fgEyeResolution = {eyeWidth, height};
            runtime.m_idealFrameDuration = 1. / 90;
            constexpr int64_t period = 1000000000 / 90;
            const auto first = runtime.ovrTimeToXrTime(StereoClock::time + .25);
            runtime.m_fgNeedReprime = false;

            FrameGeneration::InitInfo init{source.device.Get(),
                                           source.context.Get(),
                                           source.gpu.device.Get(),
                                           std::filesystem::absolute(module),
                                           eyeWidth,
                                           height,
                                           multiplier,
                                           DXGI_FORMAT_R8G8B8A8_UNORM};
            require(selectDelivery(init, presentation, 0), "independent producer and presenter");
            success(runtime.m_frameGeneration->initialize(init), "initialize production NVIDIA module");
            for (auto& bank : runtime.m_fgPresentationBanks) {
                for (auto& output : bank) {
                    require(OVR_SUCCESS(transport.create(description(false), &output.ovrSwapchain, true)),
                            "create native OVRNull colour swapchain");
                    int count{};
                    require(OVR_SUCCESS(transport.length(output.ovrSwapchain, &count)) && count == 3,
                            "native colour chain rotates");
                    for (int index = 0; index < count; ++index) {
                        ComPtr<ID3D11Texture2D> image;
                        require(OVR_SUCCESS(transport.buffer(output.ovrSwapchain, index, IID_PPV_ARGS(&image))),
                                "owned native colour image");
                        output.images.push_back(std::move(image));
                    }
                }
            }
            if (withDepth) {
                for (auto& bank : runtime.m_fgDepthPresentationBanks) {
                    require(
                        OVR_SUCCESS(transport.createFrameGenerationDepth(description(true), &bank.output.ovrSwapchain)),
                        "create actual private depth bank and native chain");
                    int count{};
                    require(OVR_SUCCESS(transport.length(bank.output.ovrSwapchain, &count)) && count == 3,
                            "native depth chain rotates");
                    for (int index = 0; index < count; ++index) {
                        ComPtr<ID3D11Texture2D> image;
                        require(OVR_SUCCESS(transport.buffer(bank.output.ovrSwapchain, index, IID_PPV_ARGS(&image))),
                                "owned private depth image");
                        bank.output.images.push_back(std::move(image));
                    }
                }
            }

            ComPtr<ID3D11Fence> handoffFence;
            success(presentation.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&handoffFence)),
                    "SDK End presentation-queue proof");
            std::vector<Handoff> ends;
            ends.reserve(16);
            long long endingId{};
            std::mutex resumeMutex;
            std::condition_variable resumeChanged;
            bool resume{}, enteredBegin{};
            const auto resumeNative = [&] {
                std::lock_guard lock(resumeMutex);
                resume = true;
                resumeChanged.notify_all();
            };
            sdk.observePhase = [&](char phase, long long id) {
                if (slowEnd && phase == 'B' && id == 4) {
                    std::unique_lock lock(resumeMutex);
                    enteredBegin = true;
                    resumeChanged.notify_all();
                    require(resumeChanged.wait_for(lock, 5s, [&] { return resume; }),
                            "first Begin waits for the fixture to release pending stereo");
                }
                if (phase == 'E')
                    endingId = id;
                return ovrSuccess;
            };
            sdk.observeWait = [&](long long id) {
                if (catchUp && id == static_cast<long long>(multiplier + 1)) {
                    std::unique_lock lock(resumeMutex);
                    require(resumeChanged.wait_for(lock, 5s, [&] { return resume; }),
                            "test releases the second native slot");
                }
                const auto time = runtime.xrTimeToOvrTime(first + id * period) - .003;
                StereoClock::time = (std::max)(StereoClock::time.load(), time);
            };
            sdk.observeLayers = [&](const ovrLayerHeader* const* layers, unsigned count) {
                require(count == 1 && layers && layers[0], "a single stereo layer must reach each native End");
                Handoff handoff{endingId};
                handoff.submitTime = StereoClock::time;
                if (layers[0]->Type == ovrLayerType_EyeFovDepth) {
                    handoff.layer.EyeFovDepth = *reinterpret_cast<const ovrLayerEyeFovDepth*>(layers[0]);
                    handoff.depthIndex = lastCommitted(runtime.m_ovrSession, handoff.layer.EyeFovDepth.DepthTexture[0]);
                } else {
                    require(layers[0]->Type == ovrLayerType_EyeFov, "invalid stereo End layer type");
                    handoff.layer.EyeFov = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
                }
                handoff.colourIndex = lastCommitted(runtime.m_ovrSession, handoff.layer.EyeFov.ColorTexture[0]);
                ends.push_back(handoff);
                success(presentation.context->Signal(handoffFence.Get(), ends.size()), "native End GPU marker");
                presentation.context->Flush();
                if (slowEnd && endingId == 4) {
                    // S1 has just been submitted. Model a long native End
                    // during which the queued S3 copy can finish.
                    require(completed(handoffFence.Get(), ends.size()), "S3 copy settles inside the slow native End");
                    StereoClock::time = runtime.xrTimeToOvrTime(first + 6 * period) + .001;
                }
            };
            std::optional<ProducerHold> slow;
            auto stop = MakeScopeGuard([&] {
                try {
                    if (slow)
                        slow->finish();
                    resumeNative();
                    RuntimeInputRegression::stop(runtime, sdk);
                } catch (...) {
                    std::_Exit(4);
                }
            });

            FrameGenerationCadence cadence;
            FinalProjectionInput input;
            input.period = period;
            input.nrEnabled = nr;
            const std::array<std::vector<uint8_t>, 5> colours{scene(0), scene(8), scene(16), scene(24), scene(32)};
            for (unsigned eye = 0; eye < 2; ++eye) {
                auto& view = input.views[eye];
                view.rect = {eye * eyeWidth, 0, eyeWidth, height};
                view.orientation[3] = 1;
                view.position[0] = eye ? .032f : -.032f;
                view.fov[0] = view.fov[3] = -.8f;
                view.fov[1] = view.fov[2] = .8f;
            }
            if (!warmup) {
                input.serial = 1;
                input.time = first - period;
                input.slots =
                    cadence.plan(cadence.reserve(0, first - multiplier * period, period, multiplier, 0), 0, false);
                input.colour[0] = input.colour[1] = source.texture(&colours[0]);
                OwnedFrameTransaction prime;
                success(runtime.m_frameGeneration->capture(input, prime), "prime real module history");
                source.await(*runtime.m_frameGeneration, prime);
                source.retire(*runtime.m_frameGeneration, prime);
            }

            RuntimeInputRegression::start(runtime);
            sdk.release();
            runtime.waitForAsyncSubmissionIdle();

            const auto layerFor = [&](unsigned group, unsigned bank) {
                ovrLayer_Union layer{};
                const auto time = runtime.xrTimeToOvrTime(input.time);
                layer.Header.Type = withDepth ? ovrLayerType_EyeFovDepth : ovrLayerType_EyeFov;
                layer.EyeFov.SensorSampleTime = time;
                for (unsigned eye = 0; eye < 2; ++eye) {
                    layer.EyeFov.RenderPose[eye].Orientation.w = 1;
                    layer.EyeFov.RenderPose[eye].Position.x = input.views[eye].position[0];
                    layer.EyeFov.RenderPose[eye].Position.y = float(group);
                }
                if (withDepth) {
                    auto& depth = runtime.m_fgDepthPresentationBanks[bank];
                    layer.EyeFovDepth.ProjectionDesc = {float(group + 2), .5f, 1.f};
                    const auto chain = depth.output.ovrSwapchain;
                    int index{};
                    require(OVR_SUCCESS(transport.acquireFrameGenerationDepthForWrite(chain, &index)),
                            "prior depth reservation was consumed or discarded");
                    depth.token = input.serial;
                    depth.preparedImage = index;
                    ComPtr<ID3D11Texture2D> image;
                    require(OVR_SUCCESS(transport.buffer(chain, index, IID_PPV_ARGS(&image))), "producer depth");
                    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                    desc.Format = DXGI_FORMAT_D32_FLOAT;
                    desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                    ComPtr<ID3D11DepthStencilView> view;
                    success(source.device->CreateDepthStencilView(image.Get(), &desc, &view), "producer depth view");
                    source.context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, (group + 1) / 8.f, 0);
                    require(OVR_SUCCESS(transport.commit(chain)), "publish private depth image to transport");
                    runtime.m_fgDepthDelivery[bank] = transport.prepare({chain}, input.serial);
                }
                return layer;
            };

            for (unsigned group = 0; group < (consecutive ? 4u : 3u); ++group) {
                const bool primeGroup = warmup && group == 0;
                const auto native = warmup ? (group ? 1 + multiplier * (group - 1) : 0) : multiplier * group;
                const auto priorEnds = ends.size();
                const unsigned groupSlots = primeGroup ? 1u : multiplier;
                auto reservation = cadence.reserve(0, first + native * period, period, groupSlots, native);
                if (historyChange && group)
                    ++reservation.historyEpoch;
                const auto previous = input.time;
                input.serial = group + (warmup ? 1u : 2u);
                input.time = reservation.currentTarget;
                input.slots = cadence.plan(reservation, previous, !primeGroup);
                require(input.slots.count == groupSlots, "all configured presentation slots are reserved");
                input.colour[0] = input.colour[1] = source.texture(&colours[group + 1]);
                const bool gated = !steady && (group == 1 || (consecutive && group == 2));
                const unsigned skipped = gated && groupSlots == 4 && !preDeadline && !slowEnd ? 2u : 0u;
                const bool lateEnd = gated && slowEnd;
                const unsigned expectedSlots = groupSlots - skipped - (lateEnd ? 1u : 0u);
                if (gated)
                    slow.emplace(source);
                std::unique_lock producer(runtime.m_fgContextMutex);
                OwnedFrameTransaction ticket;
                success(runtime.m_frameGeneration->capture(input, ticket), "capture changing real projection");
                require((ticket.ticket.prime != 0) == primeGroup &&
                            ticket.ticket.previous_serial + 1 == ticket.ticket.current_serial,
                        "ticket contains the expected live prime or consecutive pair");
                const auto full = static_cast<ID3D12Fence*>(ticket.ticket.output_ready_fence);
                const auto fullValue = ticket.ticket.output_ready_value;
                auto layer = layerFor(group, ticket.ticket.bank);
                if (withDepth)
                    runtime.m_fgDepthPresentationBanks[ticket.ticket.bank].token = ticket.ticket.token;
                if (!gated)
                    source.gpu.wait(full, fullValue);
                else
                    require(slow->after->GetCompletedValue() == 0 && full->GetCompletedValue() < fullValue,
                            "slow current pair is still pending before publication");
                StereoClock::time = runtime.xrTimeToOvrTime(reservation.firstTarget + skipped * period) +
                                    (gated && !preDeadline && !slowEnd ? .001 : -.003);
                {
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    runtime.m_fgReservationForSubmission = reservation;
                    runtime.m_fgTransactionForSubmission = std::move(ticket);
                    runtime.m_layersForAsyncSubmission.assign(1, layer);
                    runtime.m_asyncNextFrameId = input.serial;
                    runtime.m_asyncSubmissionReady = false;
                    runtime.m_asyncSubmissionCondVar.notify_all();
                }
                producer.unlock();

                const auto makeInputReady = [&] {
                    slow->finish();
                    slow.reset();
                    source.gpu.wait(full, fullValue);
                    ComPtr<ID3D11Fence> settled;
                    success(source.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&settled)),
                            "create source readiness witness");
                    success(source.context->Signal(settled.Get(), 1), "source finishes both pending inputs");
                    source.context->Flush();
                    require(completed(settled.Get(), 1), "the current stereo and depth become ready");
                    resumeNative();
                };
                if (lateEnd) {
                    {
                        std::unique_lock lock(resumeMutex);
                        require(resumeChanged.wait_for(lock, 3s, [&] { return enteredBegin; }),
                                "presentation enters the first Begin with the current NR input still pending");
                    }
                    require(slow->after->GetCompletedValue() == 0 && full->GetCompletedValue() < fullValue,
                            "the interim late-input classification is genuine");
                    makeInputReady();
                }
                if (gated && !lateEnd) {
                    require(sdk.waitFor([&] { return ends.size() > priorEnds; }),
                            "first eligible native slot still presents promptly");
                    const bool progressed = completed(handoffFence.Get(), priorEnds + 1, 1000);
                    require(slow->after->GetCompletedValue() == 0 && full->GetCompletedValue() < fullValue,
                            "NR-backed input remains gated at the first missed presentation deadline");
                    if (nr && !historyChange) {
                        require(progressed, "late NR must not block the presentation queue before replay");
                        const unsigned replayed = catchUp ? 1u : expectedSlots;
                        require(sdk.waitFor([&] { return ends.size() >= priorEnds + replayed; }),
                                "every missed slot presents the retained frame");
                        require(completed(handoffFence.Get(), priorEnds + replayed, 1000) &&
                                    slow->after->GetCompletedValue() == 0 && full->GetCompletedValue() < fullValue,
                                "all replay markers complete while the current NR pair and copies remain pending");
                        const auto& committed = ends[warmup ? 0u : multiplier - 1];
                        if (preDeadline) {
                            require(ends[priorEnds].submitTime < runtime.xrTimeToOvrTime(reservation.firstTarget),
                                    "a pending NR frame can reuse the last pair during the native render lead");
                        }
                        for (unsigned slot = 0; slot < replayed; ++slot) {
                            const auto& repeated = ends[priorEnds + slot];
                            require(repeated.layer.EyeFov.ColorTexture[0] == committed.layer.EyeFov.ColorTexture[0] &&
                                        repeated.layer.EyeFov.ColorTexture[1] ==
                                            committed.layer.EyeFov.ColorTexture[1] &&
                                        repeated.colourIndex == committed.colourIndex,
                                    "replay uses the last committed stereo colour, without a new commit");
                            require(repeated.layer.EyeFov.SensorSampleTime == committed.layer.EyeFov.SensorSampleTime,
                                    "stereo replay preserves the original sensor sample time");
                            for (unsigned eye = 0; eye < 2; ++eye) {
                                require(repeated.layer.EyeFov.RenderPose[eye].Position.y ==
                                                committed.layer.EyeFov.RenderPose[eye].Position.y &&
                                            repeated.layer.EyeFov.RenderPose[eye].Position.x ==
                                                committed.layer.EyeFov.RenderPose[eye].Position.x,
                                        "stereo replay keeps both original render poses");
                            }
                            if (withDepth)
                                require(repeated.layer.Header.Type == ovrLayerType_EyeFovDepth &&
                                            repeated.layer.EyeFovDepth.DepthTexture[0] ==
                                                committed.layer.EyeFovDepth.DepthTexture[0] &&
                                            repeated.layer.EyeFovDepth.DepthTexture[1] ==
                                                committed.layer.EyeFovDepth.DepthTexture[1] &&
                                            repeated.depthIndex == committed.depthIndex &&
                                            repeated.layer.EyeFovDepth.ProjectionDesc.Projection22 ==
                                                committed.layer.EyeFovDepth.ProjectionDesc.Projection22,
                                        "stereo replay retains the exact depth chain, image and projection");
                        }
                    } else {
                        require(!progressed, "NR-off or changed history uses the original current-B path");
                        std::lock_guard lock(sdk.mutex);
                        const auto& current = ends[priorEnds];
                        const auto& previous = ends[multiplier - 1];
                        require(current.layer.EyeFov.SensorSampleTime == layer.EyeFov.SensorSampleTime &&
                                    (current.layer.EyeFov.ColorTexture[0] != previous.layer.EyeFov.ColorTexture[0] ||
                                     current.colourIndex != previous.colourIndex),
                                "control case uses a fresh commit rather than replaying the previous stereo");
                    }
                    if (catchUp) {
                        makeInputReady();
                    } else {
                        slow->finish();
                        slow.reset();
                    }
                }
                const bool ended = sdk.waitFor([&] { return ends.size() >= priorEnds + expectedSlots; });
                if (!ended && slowEnd) {
                    {
                        std::lock_guard lock(sdk.mutex);
                        for (const auto& handoff : ends)
                            std::cerr << "observed native End " << handoff.id << std::endl;
                    }
                    std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                    if (runtime.m_asyncSubmissionError)
                        std::rethrow_exception(runtime.m_asyncSubmissionError);
                }
                require(ended, "all eligible original native cycles ended");
                require(completed(handoffFence.Get(), priorEnds + expectedSlots),
                        "actual presentation queue completed all Ends");
                {
                    std::unique_lock lock(runtime.m_asyncSubmissionMutex);
                    require(runtime.m_asyncSubmissionCondVar.wait_for(
                                lock,
                                4s,
                                [&] { return runtime.m_asyncSubmissionError || !runtime.m_fgActiveSubmission; }) &&
                                !runtime.m_asyncSubmissionError,
                            "all selected slots and queued owners retire without worker failure");
                }
                require(ends.size() == priorEnds + expectedSlots, "expired 4x slots cannot submit extra Ends");
                for (unsigned slot = 0; slot < expectedSlots; ++slot) {
                    const auto expectedId = native + skipped + slot + (lateEnd && slot ? 1u : 0u);
                    require(ends[priorEnds + slot].id == expectedId,
                            "remaining native IDs retain their original ordering");
                }
                if (lateEnd) {
                    require(ends[priorEnds].layer.EyeFov.ColorTexture[0] ==
                                runtime.m_fgPresentationBanks[0][0].ovrSwapchain,
                            "input recovered during the first Begin preserves valid S1");
                    require(std::find(sdk.waitedIds.begin(), sdk.waitedIds.end(), 5) == sdk.waitedIds.end() &&
                                std::find(sdk.begunIds.begin(), sdk.begunIds.end(), 5) == sdk.begunIds.end(),
                            "expired S2 has no native Wait or Begin after the slow S1 End");
                }
                if (group == 0 || steady) {
                    const auto& presentationBank = runtime.m_fgPresentationBanks[0];
                    for (unsigned slot = 0; slot + 1 < groupSlots; ++slot) {
                        const unsigned role = slot == 2 ? 3u : slot;
                        require(ends[priorEnds + slot].layer.EyeFov.ColorTexture[0] ==
                                    presentationBank[role].ovrSwapchain,
                                "healthy delivery submits every intended synthetic role before B");
                    }
                }
                if (gated && catchUp)
                    require(ends[priorEnds + 1].layer.EyeFov.SensorSampleTime == layer.EyeFov.SensorSampleTime,
                            "fresh B takes over at the next slot once input is ready");
                if (!gated || catchUp || lateEnd) {
                    const auto& last = ends[priorEnds + expectedSlots - 1];
                    require(last.layer.EyeFov.SensorSampleTime == layer.EyeFov.SensorSampleTime,
                            "fresh current real frame appears after release");
                    const auto chain = last.layer.EyeFov.ColorTexture[0];
                    require(chain == last.layer.EyeFov.ColorTexture[1], "packed two-eye colour is one selected image");
                    ComPtr<ID3D11Texture2D> colour;
                    require(OVR_SUCCESS(ovr_GetTextureSwapChainBufferDX(
                                runtime.m_ovrSession, chain, last.colourIndex, IID_PPV_ARGS(&colour))),
                            "read exact committed real image");
                    require(presentation.read(colour.Get()) == colours[group + 1],
                            "fresh stereo pixels match both eyes");
                    if (withDepth) {
                        ComPtr<ID3D11Texture2D> depth;
                        require(OVR_SUCCESS(ovr_GetTextureSwapChainBufferDX(runtime.m_ovrSession,
                                                                            last.layer.EyeFovDepth.DepthTexture[0],
                                                                            last.depthIndex,
                                                                            IID_PPV_ARGS(&depth))),
                                "read exact committed depth image");
                        const auto bytes = presentation.read(depth.Get());
                        for (size_t offset = 0; offset < bytes.size(); offset += sizeof(float)) {
                            float value{};
                            std::memcpy(&value, bytes.data() + offset, sizeof(value));
                            require(value == (group + 1) / 8.f, "fresh stereo depth texels match the producing frame");
                        }
                    }
                }
            }
            std::cout << "PASS: production " << multiplier << "x worker "
                      << (steady || !nr   ? "retains ordinary FG behavior"
                          : historyChange ? "uses the current frame after a history change"
                          : slowEnd       ? "recovers 4x input then skips the expired S2 native cycle"
                                          : "replays completed stereo and returns to fresh delivery")
                      << (withDepth ? " with synchronized depth" : " colour-only") << '\n';
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 4 || argc == 5,
                "usage: nr_fg_retained_stereo_regression OVRNULL_DIRECTORY FG_DLL "
                "colour|depth|consecutive|catch-up|pre-deadline|history-change|nr-off|steady|warmup|slow-end [2|3|4]");
        const std::wstring mode = argv[3];
        const unsigned multiplier = argc == 5 ? std::stoul(argv[4]) : 3;
        require(multiplier >= 2 && multiplier <= 4, "supported multiplier");
        require(mode == L"colour" || mode == L"depth" || mode == L"consecutive" || mode == L"catch-up" ||
                    mode == L"pre-deadline" || mode == L"history-change" || mode == L"nr-off" || mode == L"steady" ||
                    mode == L"warmup" || mode == L"slow-end",
                "valid regression mode");
        require(multiplier != 4 || (mode != L"catch-up" && mode != L"history-change"),
                "four-slot expiration is exercised separately from partial recovery");
        require(mode != L"slow-end" || multiplier == 4, "slow-end requires the 4x closed-slot scheduler");
        virtualdesktop_openxr::RuntimeFrameGenerationRegression::retainedStereo(
            argv[1],
            argv[2],
            mode != L"nr-off",
            mode == L"depth" || mode == L"consecutive" || mode == L"catch-up" || mode == L"warmup",
            mode == L"consecutive",
            mode == L"history-change",
            mode == L"catch-up",
            mode == L"pre-deadline",
            mode == L"steady" || (mode == L"nr-off" && multiplier == 4),
            mode == L"warmup",
            mode == L"slow-end",
            multiplier);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
