// Included inside the existing friend regression struct. Real GPU capture,
// real production worker; the native clock and SDK handoff are controlled.
static void
liveRecovery(const wchar_t* backend, const wchar_t* module, unsigned multiplier, unsigned rate, bool pending) {
    require(!pending || multiplier >= 3, "pending case needs a later synthetic slot");
    OpenXrRuntime runtime;
    BackendState state;
    BackendHooks hooks(backend, state);
    CommitHook commits;
    CapacityClockHook clock(true);
    Interop hw;
    RuntimeInputRegression::seed(runtime);
    runtime.m_isHeadless = false;
    runtime.m_hmdStatus.IsVisible = true;
    runtime.m_ovrSubmissionDevice = hw.device;
    runtime.m_ovrSubmissionContext = hw.context;
    runtime.m_frameGenerationMode = multiplier;
    runtime.m_fgEyeResolution = {eyeWidth, height};
    runtime.m_fgFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    runtime.m_frameGeneration = std::make_unique<FrameGeneration>();
    FrameGeneration::InitInfo init{hw.device.Get(),
                                   hw.context.Get(),
                                   hw.gpu.device.Get(),
                                   std::filesystem::absolute(module),
                                   eyeWidth,
                                   height,
                                   multiplier,
                                   DXGI_FORMAT_R8G8B8A8_UNORM};
    success(runtime.m_frameGeneration->initialize(init), "initialize live recovery graph");
    std::map<ovrTextureSwapChain, unsigned> rolesByChain;
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned role = 0; role < 4; ++role) {
            if ((bank && role != 2) || (role == 1 && multiplier == 2) || (role == 3 && multiplier != 4))
                continue;
            auto& output = runtime.m_fgPresentationBanks[bank][role];
            output.ovrSwapchain = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0xe000 + bank * 4 + role));
            output.images.push_back(hw.texture());
            presentationRings[output.ovrSwapchain].images = output.images;
            rolesByChain[output.ovrSwapchain] = role;
        }
    runtime.m_precompositor.dlssnrSettings.enabled = true;
    runtime.m_precompositor.dlssnrSettings.foveationSize = 1.f;
    runtime.m_layersForFrame.resize(1);
    auto& projection = runtime.m_layersForFrame[0].EyeFov;
    projection.Header.Type = ovrLayerType_EyeFov;
    for (unsigned eye = 0; eye < 2; ++eye) {
        projection.Viewport[eye] = {{static_cast<int>(eye * eyeWidth), 0}, {eyeWidth, height}};
        projection.RenderPose[eye].Orientation.w = 1.f;
        projection.RenderPose[eye].Position.x = eye ? .032f : -.032f;
        projection.Fov[eye] = {1.f, 1.f, 1.f, 1.f};
    }
    OpenXrRuntime::Swapchain colourInputs[2]{};
    XrCompositionLayerProjectionView views[2]{};
    for (unsigned eye = 0; eye < 2; ++eye)
        views[eye].subImage.swapchain = reinterpret_cast<XrSwapchain>(&colourInputs[eye]);
    XrCompositionLayerProjection xrProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    xrProjection.viewCount = 2;
    xrProjection.views = views;
    xrProjection.space = reinterpret_cast<XrSpace>(uintptr_t(0x100));
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&xrProjection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.layerCount = 1;
    end.layers = layers;
    const int64_t period = 1000000000LL / rate;
    const int64_t base = runtime.ovrTimeToXrTime(clock.origin) + 100000000;
    OwnedFrameTransaction b, c;
    PredictionReservation bReservation, cReservation;
    std::mutex phaseMutex;
    std::condition_variable phaseChanged;
    bool atLaterSlot{}, releaseLaterSlot{};
    auto cleanup = MakeScopeGuard([&] {
        {
            std::lock_guard lock(phaseMutex);
            releaseLaterSlot = true;
        }
        phaseChanged.notify_all();
        if (b.ticket.token)
            runtime.m_fgTransactionForSubmission = std::move(b);
        if (c.ticket.token)
            runtime.m_fgTransactionForFrame = std::move(c);
        RuntimeInputRegression::stop(runtime, state);
    });
    const auto capture = [&](unsigned serial, OwnedFrameTransaction& output, PredictionReservation& reservation) {
        const uint64_t nativeId = serial >= 2 ? (serial - 2) * multiplier : 0;
        runtime.m_frameBegun = serial;
        end.displayTime = base + serial * multiplier * period;
        reservation = runtime.m_frameGenerationCadence.reserve(runtime.ovrTimeToXrTime(capacityClockTime.load()),
                                                               end.displayTime - (multiplier - 1) * period,
                                                               period,
                                                               multiplier,
                                                               nativeId);
        reservation.historyEpoch = 1;
        runtime.m_fgBegunReservation = reservation;
        const auto pixels = scene(serial * 8);
        runtime.m_precompositor.finalColour[0] = runtime.m_precompositor.finalColour[1] = hw.texture(&pixels);
        require(runtime.prepareFrameGeneration(end), "actual prepare accepts live recovery input");
        output = std::move(runtime.m_fgTransactionForFrame);
        runtime.m_fgTransactionForFrame = {};
        hw.await(*runtime.m_frameGeneration, output);
        if (serial > 1)
            require(!output.ticket.prime && output.ticket.previous_serial == serial - 1,
                    "live worker miss preserves the newest endpoint without a recovery prime");
    };
    OwnedFrameTransaction a;
    PredictionReservation unused;
    capture(1, a, unused);
    hw.retire(*runtime.m_frameGeneration, a);
    capture(2, b, bReservation);
    const auto bObservation = b;
    std::vector<unsigned> roles;
    std::vector<ComPtr<ID3D11Texture2D>> handoffSnapshots;
    state.observeLayers = [&](const ovrLayerHeader* const* sent, unsigned count) {
        require(count == 1 && sent[0]->Type == ovrLayerType_EyeFov, "live stereo projection handoff");
        const auto& eye = *reinterpret_cast<const ovrLayerEyeFov*>(sent[0]);
        const auto role = rolesByChain.at(eye.ColorTexture[0]);
        if (pending && roles.empty())
            require(role == 0 && runtime.m_frameGeneration->copyReady(bObservation, FrameGenerationRole::real),
                    "completed S1 handoff also has an already completed current-B copy");
        roles.push_back(role);
        const auto& ring = presentationRings.at(eye.ColorTexture[0]);
        require(ring.committed >= 0 && static_cast<size_t>(ring.committed) < ring.images.size(),
                "handoff reads the actually committed SDK image");
        // Preserve these pixels before this one-image SDK ring is reused.
        // Mapping here would drain the GPU and change the readiness scenario.
        auto snapshot = hw.texture();
        hw.context->CopyResource(snapshot.Get(), ring.images[ring.committed].Get());
        handoffSnapshots.push_back(std::move(snapshot));
    };
    state.observeWait = [&](long long id) {
        if (pending && id == 1) {
            std::unique_lock lock(phaseMutex);
            atLaterSlot = true;
            phaseChanged.notify_all();
            require(phaseChanged.wait_for(lock, 3s, [&] { return releaseLaterSlot; }),
                    "bounded pending producer phase");
        }
        const auto target = bReservation.firstTarget + id * period;
        const bool late = pending ? id == 1 : id == 0;
        const double next = runtime.xrTimeToOvrTime(late ? bReservation.currentTarget + 1000000 : target - 2000000);
        capacityClockTime = (std::max)(capacityClockTime.load(), next);
    };
    const auto publish = [&](OwnedFrameTransaction& input, const PredictionReservation& reservation) {
        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
        require(!runtime.m_fgTransactionForSubmission.ticket.token, "mailbox has one transaction owner");
        runtime.m_fgTransactionForSubmission = std::move(input);
        input = {};
        runtime.m_fgReservationForSubmission = reservation;
        runtime.m_layersForAsyncSubmission = runtime.m_layersForFrame;
        runtime.m_asyncSubmissionReady = false;
        runtime.m_asyncSubmissionCondVar.notify_all();
    };
    const auto settled = [&] {
        std::unique_lock lock(runtime.m_asyncSubmissionMutex);
        require(runtime.m_asyncSubmissionCondVar.wait_for(lock,
                                                          3s,
                                                          [&] {
                                                              return runtime.m_asyncSubmissionError ||
                                                                     (!runtime.m_fgActiveSubmission &&
                                                                      runtime.m_layersForAsyncSubmission.empty());
                                                          }),
                "live worker bounded retirement");
        if (runtime.m_asyncSubmissionError)
            std::rethrow_exception(runtime.m_asyncSubmissionError);
    };
    RuntimeInputRegression::start(runtime);
    state.release();
    runtime.waitForAsyncSubmissionIdle();
    publish(b, bReservation);
    if (pending) {
        {
            std::unique_lock lock(phaseMutex);
            require(phaseChanged.wait_for(lock, 3s, [&] { return atLaterSlot; }), "S1 admits next producer before S2");
        }
        runtime.waitForFrameGenerationCapacity();
        {
            std::unique_lock lease(runtime.m_fgContextMutex);
            capture(3, c, cReservation);
        }
        publish(c, cReservation);
        {
            std::lock_guard lock(phaseMutex);
            releaseLaterSlot = true;
        }
        phaseChanged.notify_all();
        settled();
    } else {
        settled();
        capture(3, c, cReservation);
        publish(c, cReservation);
        settled();
    }
    capture(4, c, cReservation);
    publish(c, cReservation);
    settled();
    require(roles.size() == 3 * multiplier, "B/C/D consume every native slot");
    std::vector<std::vector<uint8_t>> handoffPixels;
    {
        std::unique_lock lease(runtime.m_fgContextMutex);
        for (const auto& snapshot : handoffSnapshots)
            handoffPixels.push_back(hw.read(snapshot.Get()));
    }
    require(handoffPixels.size() == roles.size(), "every handoff has an independent pixel snapshot");
    for (unsigned slot = 0; slot < multiplier; ++slot)
        std::cout << "LIVE_DIAGNOSTIC slot=" << slot << " role=" << roles[slot]
                  << " equals_B=" << (handoffPixels[slot] == scene(16))
                  << " equals_A=" << (handoffPixels[slot] == scene(8)) << "\\n";
    for (unsigned slot = pending ? 1u : 0u; slot < multiplier; ++slot)
        require(handoffPixels[slot] == scene(16), "missed slots show exact current B pixels, not previous A");
    for (unsigned group = 1; group < 3; ++group)
        require(handoffPixels[group * multiplier + multiplier - 1] == scene((group + 2) * 8),
                "later real endpoints retain the correct source and stereo eye order");
    for (unsigned slot = 0; slot < multiplier; ++slot)
        require(roles[slot] == (pending && slot == 0 ? 0u : 2u), "late B never resumes older synthetic output");
    for (unsigned group = 1; group < 3; ++group)
        for (unsigned slot = 0; slot < multiplier; ++slot) {
            const unsigned role = slot == multiplier - 1 ? 2 : slot == 0 ? 0 : slot == 1 ? 1 : 3;
            require(roles[group * multiplier + slot] == role, "live C/D synthesize every distinct temporal role");
        }
    require(runtime.m_fgTimelineReanchor && !runtime.m_fgNeedReprime,
            "native recovery remains separate from valid history");
    RuntimeInputRegression::stop(runtime, state);
    cleanup.Deactivate();
    hw.gpu.noErrors();
}
