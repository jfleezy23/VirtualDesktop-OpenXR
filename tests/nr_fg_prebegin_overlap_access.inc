// Included inside the existing test-only friend, never a production class.
// Removing pre-Begin admission, or holding S's lease through independent P
// Begin, must prevent the next public frame from publishing during this gate.
struct PrebeginState {
    std::mutex mutex;
    std::condition_variable changed;
    bool nr{}, armed{}, producerArrived{}, waitHeld{}, beginHeld{}, released{}, published{}, expired{};
    bool allReady{}, independent{}, retained{}, processed{}, serialized{true};
    long long native{-1};
    uint64_t serial{}, token{};
    unsigned bank{}, observedEnds{}, renderCalls{}, maxRenderCalls{};
    std::array<unsigned, 2> nrBefore{};
    std::array<ovrTextureSwapChain, 4> chains{};
    std::array<int, 4> images{{-1, -1, -1, -1}};
    std::array<std::vector<uint8_t>, 4> pixels;
    std::exception_ptr failure;
    bool producerHeld{}, producerHoldObserved{}, beginReleasedWhileProducerHeld{}, firstEndWhileProducerHeld{};
    unsigned returnedEnds{}, endsReturnedWhileProducerHeld{};
    bool failureRetained{}, cancelRetained{}, cleanupVerified{};
    uint64_t futureToken{};
    bool clockFrozen{};
    double frozen{}, clockOffset{}, frozenReal{};
};
inline static PrebeginState prebegin;
inline static decltype(&ovr_GetTimeInSeconds) prebeginOriginalTime{};
inline static VDXRFG_GetApiV4Fn prebeginOriginalApi{};
inline static decltype(VDXRFG_ApiV4::Submit) prebeginOriginalSubmit{};

static double OVR_CDECL prebeginTime() {
    std::lock_guard lock(prebegin.mutex);
    return prebegin.clockFrozen ? prebegin.frozen : prebeginOriginalTime() - prebegin.clockOffset;
}
static void prebeginFreeze() {
    // Caller owns only the harness mutex. The runtime retains its own locks.
    if (!prebegin.clockFrozen) {
        prebegin.frozenReal = prebeginOriginalTime();
        prebegin.frozen = prebegin.frozenReal - prebegin.clockOffset;
        prebegin.clockFrozen = true;
    }
}
static void prebeginThaw() {
    if (prebegin.clockFrozen) {
        prebegin.clockOffset += prebeginOriginalTime() - prebegin.frozenReal;
        prebegin.clockFrozen = false;
    }
}
static HRESULT WINAPI prebeginSubmit(void* context, const VDXRFG_SubmitInfoV3* info, VDXRFG_TicketV1* ticket) noexcept {
    const auto result = prebeginOriginalSubmit(context, info, ticket);
    if (FAILED(result) || !info || !ticket || !ticket->token)
        return result;
    bool selected{};
    {
        std::lock_guard lock(prebegin.mutex);
        selected = prebegin.armed && info->serial == prebegin.serial;
    }
    if (selected) {
        try {
            require(!ticket->prime && ticket->previous_serial + 1 == ticket->current_serial,
                    "pre-Begin setup uses a real consecutive nonprime FG pair");
            auto* fence = static_cast<ID3D12Fence*>(ticket->output_ready_fence);
            wil::unique_handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            require(fence && ticket->output_ready_value && event, "real full-output completion event");
            checkHr(fence->SetEventOnCompletion(ticket->output_ready_value, event.get()));
            require(WaitForSingleObject(event.get(), 5000) == WAIT_OBJECT_0 &&
                        fence->GetCompletedValue() != UINT64_MAX &&
                        fence->GetCompletedValue() >= ticket->output_ready_value,
                    "all actual current FG outputs complete before worker publication");
            std::lock_guard lock(prebegin.mutex);
            prebegin.allReady = true;
            prebegin.token = ticket->token;
            prebegin.bank = ticket->bank;
        } catch (...) {
            std::lock_guard lock(prebegin.mutex);
            prebegin.failure = std::current_exception();
            return E_FAIL;
        }
    }
#ifdef VDXR_PREBEGIN_FAST_REGRESSION
    if (prebegin.armed && info->serial == prebegin.serial + 1) {
        try {
            std::unique_lock lock(prebegin.mutex);
            prebegin.futureToken = ticket->token;
            prebegin.producerHeld = prebegin.producerHoldObserved = true;
            prebegin.changed.notify_all();
            require(prebegin.changed.wait_for(lock, 5s, [] { return prebegin.beginReleasedWhileProducerHeld; }),
                    "fast Begin returns while the actual future Submit still owns its producer lease");
            // This is a bounded harness hold of the producer's existing lease,
            // not a wait added to production. Every current SDK End must
            // finish before this next producer's existing lease is released.
            prebegin.changed.wait_for(
                lock, 250ms, [] { return prebegin.endsReturnedWhileProducerHeld == requestedFrameGeneration; });
            prebegin.producerHeld = false;
            prebegin.changed.notify_all();
        } catch (...) {
            std::lock_guard lock(prebegin.mutex);
            prebegin.failure = std::current_exception();
            prebegin.producerHeld = false;
            prebegin.changed.notify_all();
            return E_FAIL;
        }
    }
#endif
    return result;
}
static HRESULT WINAPI prebeginGetApi(VDXRFG_ApiV4* api) noexcept {
    const auto result = prebeginOriginalApi(api);
    if (SUCCEEDED(result) && api && api->Submit) {
        prebeginOriginalSubmit = api->Submit;
        api->Submit = prebeginSubmit;
    }
    return result;
}
struct PrebeginFixture {
    HMODULE module{};
    explicit PrebeginFixture(const char* assets, bool nr) {
        prebegin.nr = nr;
        require(runtime().m_useDeferredFrameWait,
                "pre-Begin fixture exercises the existing deferred Wait policy and End capacity gate");
        module = LoadLibraryExW((std::filesystem::absolute(assets) / L"vdxr-frame-generation.dll").c_str(),
                                nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(module != nullptr, "load real FG module for pre-Begin readiness observation");
        prebeginOriginalApi = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
        prebeginOriginalTime = reinterpret_cast<decltype(prebeginOriginalTime)>(
            GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_GetTimeInSeconds"));
        require(prebeginOriginalApi && prebeginOriginalTime, "pre-Begin observation exports");
        checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
        checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
        checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&prebeginOriginalApi), prebeginGetApi)));
        checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&prebeginOriginalTime), prebeginTime)));
        checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
    }
    ~PrebeginFixture() {
        if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
            DetourDetach(reinterpret_cast<PVOID*>(&prebeginOriginalApi), prebeginGetApi) ||
            DetourDetach(reinterpret_cast<PVOID*>(&prebeginOriginalTime), prebeginTime) || DetourTransactionCommit())
            std::_Exit(3);
        FreeLibrary(module);
    }
};
static std::vector<uint8_t> prebeginPixels(unsigned role, int image) {
    auto& value = runtime();
    const auto bank = role == 2 ? prebegin.bank : 0u;
    auto* texture = value.m_fgPresentationBanks[bank][role].images.at(image).Get();
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return viewportPixels(texture, {{0, 0}, {static_cast<int>(desc.Width), static_cast<int>(desc.Height)}});
}
static void prebeginBeforeFrame(unsigned frame) {
    if (frame == 4)
        drain();
    if (frame != 5)
        return;
    std::unique_lock lock(prebegin.mutex);
    require(prebegin.changed.wait_for(lock, 5s, [] { return prebegin.waitHeld || prebegin.failure; }),
            "pre-Begin setup reaches the selected current S1 native Wait");
    if (prebegin.failure)
        std::rethrow_exception(prebegin.failure);
}
static void prebeginBeforeEnd(unsigned frame) {
    if (frame == 5) {
        // Finish the app's public Wait/Begin/render/pose work while native
        // Wait is parked. Only then let P enter Begin as S approaches the
        // actual xrEndFrame capacity gate; unrelated SDK queries cannot
        // become the reason the producer is unable to reach that gate.
        std::unique_lock lock(prebegin.mutex);
        prebegin.nrBefore = {{successfulNrEvaluations[0], successfulNrEvaluations[1]}};
        prebegin.producerArrived = true;
        prebegin.changed.notify_all();
        require(prebegin.changed.wait_for(lock, 5s, [] { return prebegin.beginHeld || prebegin.failure; }),
                "selected independent S1 Begin is held before public End enters capacity");
        if (prebegin.failure)
            std::rethrow_exception(prebegin.failure);
        return;
    }
    if (frame != 4)
        return;
    auto& value = runtime();
    std::lock_guard lock(prebegin.mutex);
    prebegin.independent = value.m_deliveryTransport &&
                           value.presentationDevice() != value.m_ovrSubmissionDevice.Get() &&
                           value.presentationContext() != value.m_ovrSubmissionContext.Get();
    require(prebegin.independent && value.m_frameGeneration && !value.m_fgNeedReprime,
            "pre-Begin setup has independent S/P domains and retained FG history");
    prebegin.native = static_cast<long long>(value.m_fgBegunReservation.nativeId);
    prebegin.serial = value.m_frameBegun;
    prebegin.armed = true;
    // Exclude harness-only full-output settlement from photon deadlines.
    // Native Wait resumes this monotonic clock; it is frozen again only while
    // the harness holds Wait/Begin. No runtime sleep or wait is added.
    prebeginFreeze();
}
static void prebeginBeforeWait(long long id) {
    std::lock_guard lock(prebegin.mutex);
    if (prebegin.armed && id == prebegin.native)
        prebeginThaw();
}
static void prebeginAfterWait(long long id) {
    std::unique_lock lock(prebegin.mutex);
    if (!prebegin.armed || id != prebegin.native)
        return;
    prebeginFreeze();
    prebegin.waitHeld = true;
    prebegin.changed.notify_all();
    require(prebegin.changed.wait_for(lock, 5s, [] { return prebegin.producerArrived; }),
            "next public producer arrives before selected S1 Begin");
}
static void prebeginRenderEnter() {
    std::lock_guard lock(prebegin.mutex);
    prebegin.maxRenderCalls = std::max(prebegin.maxRenderCalls, ++prebegin.renderCalls);
    prebegin.serialized &= prebegin.renderCalls == 1;
}
static void prebeginRenderExit() {
    std::lock_guard lock(prebegin.mutex);
    --prebegin.renderCalls;
}
static void prebeginBeforeBegin(long long id) {
    std::unique_lock lock(prebegin.mutex);
    if (!prebegin.armed || id != prebegin.native)
        return;
    try {
        auto& value = runtime();
        require(prebegin.allReady && prebegin.producerArrived && prebegin.waitHeld,
                "held Begin follows a prearrived producer and physically ready current outputs");
        for (unsigned role = 0; role < 4; ++role) {
            if ((role == 1 && requestedFrameGeneration == 2) || (role == 3 && requestedFrameGeneration != 4))
                continue;
            const auto bank = role == 2 ? prebegin.bank : 0u;
            auto& output = value.m_fgPresentationBanks[bank][role];
            int image{-1};
            require(OVR_SUCCESS(ovr_GetTextureSwapChainCurrentIndex(value.m_ovrSession, output.ovrSwapchain, &image)),
                    "current owned role has an actual SDK presentation image");
            prebegin.chains[role] = output.ovrSwapchain;
            prebegin.images[role] = image;
            prebegin.pixels[role] = prebeginPixels(role, image);
        }
        prebegin.beginHeld = true;
        prebegin.changed.notify_all();
        // A bound releases the real SDK path so the RED run reports the
        // missing producer publication, rather than deadlocking cleanup.
#ifdef VDXR_PREBEGIN_FAST_REGRESSION
        prebegin.expired = !prebegin.changed.wait_for(lock, 2s, [] { return prebegin.producerHeld; });
#else
        prebegin.expired = !prebegin.changed.wait_for(lock, 2s, [] { return prebegin.published; });
#endif
        prebegin.beginHeld = false;
        prebegin.released = true;
        prebeginThaw();
        prebegin.changed.notify_all();
    } catch (...) {
        prebegin.failure = std::current_exception();
        prebegin.beginHeld = false;
        prebegin.released = true;
        prebeginThaw();
        prebegin.changed.notify_all();
        throw;
    }
}
static void prebeginAfterBegin(long long id) {
#ifdef VDXR_PREBEGIN_FAST_REGRESSION
    std::lock_guard lock(prebegin.mutex);
    if (prebegin.armed && id == prebegin.native) {
        prebegin.beginReleasedWhileProducerHeld = prebegin.producerHeld;
        prebegin.changed.notify_all();
    }
#else
    (void)id;
#endif
}
static void prebeginAfterNativeEnd(long long id, ovrResult result) {
#ifdef VDXR_PREBEGIN_FAST_REGRESSION
    std::lock_guard lock(prebegin.mutex);
    if (prebegin.armed && id >= prebegin.native && id < prebegin.native + requestedFrameGeneration &&
        OVR_SUCCESS(result)) {
        // Count distinct, consecutive successful RETURNS across the entire
        // current group. A first-End wake cannot mask a later context convoy.
        const auto slot = static_cast<unsigned>(id - prebegin.native);
        require(slot == prebegin.returnedEnds, "current SDK End returns once in exact native slot order");
        ++prebegin.returnedEnds;
        if (!slot)
            prebegin.firstEndWhileProducerHeld = prebegin.producerHeld;
        if (prebegin.producerHeld)
            ++prebegin.endsReturnedWhileProducerHeld;
        prebegin.changed.notify_all();
    }
#else
    (void)id;
    (void)result;
#endif
}
static ovrResult prebeginBeginResult(long long id, ovrResult result) {
#ifdef VDXR_PREBEGIN_FAILURE_REGRESSION
    if (prebegin.armed && id == prebegin.native && OVR_SUCCESS(result))
        return ovrError_DisplayLost;
#endif
    return result;
}
static void prebeginVerifyCleanup() {
#if defined(VDXR_PREBEGIN_FAILURE_REGRESSION) || defined(VDXR_PREBEGIN_CANCEL_REGRESSION)
    auto& value = runtime();
    std::lock_guard lock(prebegin.mutex);
    prebegin.cleanupVerified = !value.m_frameGeneration && !value.m_fgFailedTransaction.ticket.token &&
                               !value.m_fgTransactionForSubmission.ticket.token &&
                               !value.m_fgTransactionForFrame.ticket.token;
    require(prebegin.cleanupVerified, "joined public session cleanup releases current and future FG ticket owners");
#endif
}
static void prebeginAfterEnd(unsigned frame) {
    if (frame != 5)
        return;
    auto& value = runtime();
    std::unique_lock lock(prebegin.mutex);
    if (prebegin.failure)
        std::rethrow_exception(prebegin.failure);
#ifdef VDXR_PREBEGIN_FAST_REGRESSION
    std::cout << "PREBEGIN_FAST_BEGIN producer_submit_held=" << prebegin.producerHoldObserved
              << " Begin_returned_while_producer_held=" << prebegin.beginReleasedWhileProducerHeld
              << " first_End_while_producer_held=" << prebegin.firstEndWhileProducerHeld
              << " current_group_Ends_while_producer_held=" << prebegin.endsReturnedWhileProducerHeld
              << " required_current_group_Ends=" << requestedFrameGeneration << " all_current_Ends_while_producer_held="
              << (prebegin.endsReturnedWhileProducerHeld == requestedFrameGeneration)
              << " producer_public_End_returned=1 expired=" << prebegin.expired << '\n';
    require(prebegin.producerHoldObserved && prebegin.beginReleasedWhileProducerHeld && !prebegin.expired,
            "fast Begin control reaches a real future ticket while its producer lease is held");
    require(prebegin.firstEndWhileProducerHeld && prebegin.returnedEnds == requestedFrameGeneration &&
                prebegin.endsReturnedWhileProducerHeld == requestedFrameGeneration,
            "every current native End must return while the future producer still owns its context lease");
    prebegin.published = true;
    return;
#endif
    if (prebegin.beginHeld) {
        std::unique_lock submission(value.m_asyncSubmissionMutex);
        const auto& next = value.m_fgTransactionForSubmission;
        prebegin.futureToken = next.ticket.token;
        prebegin.retained = value.m_fgActiveSubmission && value.m_fgActiveOwnedPixels &&
                            !value.m_layersForAsyncSubmission.empty() && next.ticket.token &&
                            next.ticket.token != prebegin.token && !next.ticket.prime &&
                            next.ticket.current_serial == prebegin.serial + 1 &&
                            next.ticket.previous_serial == prebegin.serial && next.ticket.bank != prebegin.bank;
        submission.unlock();
        prebegin.processed = prebegin.nr ? successfulNrEvaluations[0] == prebegin.nrBefore[0] + 1 &&
                                               successfulNrEvaluations[1] == prebegin.nrBefore[1] + 1
                                         : !nrEvaluations[0] && !nrEvaluations[1];
        for (unsigned role = 0; role < 4; ++role)
            if (prebegin.images[role] >= 0)
                require(prebeginPixels(role, prebegin.images[role]) == prebegin.pixels[role],
                        "future producer cannot overwrite any current owned presentation role");
        prebegin.published = prebegin.retained && prebegin.processed;
        prebegin.changed.notify_all();
    }
    std::cout << "PREBEGIN_OVERLAP serial=" << prebegin.serial << " native=" << prebegin.native
              << " all_ready=" << prebegin.allReady << " independent=" << prebegin.independent
              << " producer_prearrived=" << prebegin.producerArrived << " begin_held=" << prebegin.beginHeld
              << " published=" << prebegin.published << " nr_processed=" << prebegin.processed
              << " distinct_live_bank=" << prebegin.retained << " expired=" << prebegin.expired << '\n';
    require(prebegin.published,
            "next public NR/FG producer must publish its real ticket while independent current S1 Begin is held");
#ifdef VDXR_PREBEGIN_FAILURE_REGRESSION
    lock.unlock();
    std::unique_lock submission(value.m_asyncSubmissionMutex);
    require(value.m_asyncSubmissionCondVar.wait_for(submission, 5s, [&] { return bool(value.m_asyncSubmissionError); }),
            "forwarded current Begin failure reaches the real worker catch");
    prebegin.failureRetained = value.m_fgFailedTransaction.ticket.token == prebegin.token &&
                               value.m_fgTransactionForSubmission.ticket.token == prebegin.futureToken &&
                               prebegin.futureToken && prebegin.futureToken != prebegin.token &&
                               !value.m_layersForAsyncSubmission.empty();
    require(prebegin.failureRetained && !prebegin.observedEnds,
            "before-first-End failure retains both distinct current and future mailboxes without presenting either");
    std::cout << "PREBEGIN_FAILURE both_tickets_retained=1 current_Ends=0\n";
    throw std::runtime_error("expected controlled before-first-End SDK failure; ownership verified");
#endif
#ifdef VDXR_PREBEGIN_CANCEL_REGRESSION
    {
        std::lock_guard submission(value.m_asyncSubmissionMutex);
        prebegin.cancelRetained = prebegin.retained && prebegin.futureToken;
        value.m_terminateAsyncThread = true;
        value.m_asyncSubmissionCondVar.notify_all();
    }
    require(prebegin.cancelRetained, "cancel begins with both real owners retained after early publication");
    std::cout << "PREBEGIN_CANCEL both_tickets_retained=1\n";
    throw std::runtime_error("expected controlled cancellation after early publication; ownership verified");
#endif
}
static void prebeginObserveEnd(long long id, const ovrLayerHeader* const* layers, unsigned count) {
    std::lock_guard lock(prebegin.mutex);
    if (!prebegin.armed || id < prebegin.native || id >= prebegin.native + requestedFrameGeneration)
        return;
    const auto slot = static_cast<unsigned>(id - prebegin.native);
    const unsigned role = slot == requestedFrameGeneration - 1 ? 2u : slot == 2 ? 3u : slot;
    require(count == 1 && layers && layers[0] && layers[0]->Type == ovrLayerType_EyeFov,
            "pre-Begin current group keeps its exact colour projection");
    const auto& layer = *reinterpret_cast<const ovrLayerEyeFov*>(layers[0]);
    require(layer.ColorTexture[0] == prebegin.chains[role] && layer.ColorTexture[1] == prebegin.chains[role],
            "pre-Begin current group retains original S1/S2/S3/B chain order");
    const auto bank = role == 2 ? prebegin.bank : 0u;
    const auto image = runtime().m_fgPresentationBanks[bank][role].lastCommittedIndex;
    require(image == prebegin.images[role] && prebeginPixels(role, image) == prebegin.pixels[role],
            "every forwarded current End contains its original physically copied role pixels");
    ++prebegin.observedEnds;
}
static void prebeginVerify() {
    std::lock_guard lock(prebegin.mutex);
    if (prebegin.failure)
        std::rethrow_exception(prebegin.failure);
    require(prebegin.published && prebegin.released && !prebegin.expired &&
                prebegin.observedEnds == requestedFrameGeneration && prebegin.serialized && !prebegin.renderCalls &&
                prebegin.maxRenderCalls == 1,
            "held Begin overlap retains pixels, completes every native role, and serializes all SDK render phases");
    std::cout << "PREBEGIN_VERIFIED current_ends=" << prebegin.observedEnds
              << " max_sdk_render_calls=" << prebegin.maxRenderCalls << '\n';
}
