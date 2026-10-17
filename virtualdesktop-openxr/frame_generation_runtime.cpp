#include "pch.h"
#include "runtime.h"
#include "log.h"

namespace virtualdesktop_openxr {
    using namespace log;

    bool OpenXrRuntime::prepareFrameGeneration(const XrFrameEndInfo& info) {
        const auto& settings = m_precompositor.dlssnrSettings;
        const auto bypass = [&] {
            m_fgDesiredMultiplier = 1;
            m_fgPreviousCapturedTime = 0;
            m_frameGenerationCadence.reset();
            if (m_frameGeneration && m_frameGeneration->initialized()) {
                const auto result = m_frameGeneration->reset(0);
                if (result != VDXRFG_E_BUSY) {
                    CHECK_HRCMD(result);
                }
            }
            return false;
        };
        const bool eligible = m_frameGenerationMode && !m_frameGenerationUnavailable && m_useAsyncSubmission &&
                              !m_useApplicationDeviceForSubmission &&
                              (!settings.enabled || settings.foveationSize >= .90f) && info.layerCount == 1 &&
                              (settings.enabled || m_d3d11Context || m_d3d12CommandQueue) &&
                              m_layersForFrame.size() == 1 && m_layersForFrame[0].Header.Type == ovrLayerType_EyeFov &&
                              m_precompositor.finalColour[0] && m_precompositor.finalColour[1] &&
                              m_fgBegunReservation.valid && m_fgBegunReservation.shouldRender && info.layers &&
                              info.layers[0] && info.displayTime == m_fgBegunReservation.currentTarget;
        if (!eligible) {
            return bypass();
        }
        const auto& sourceProjection = *reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[0]);
        if (sourceProjection.viewCount != 2 || !sourceProjection.views) {
            return bypass();
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& view = sourceProjection.views[eye];
            const auto& source = *reinterpret_cast<const Swapchain*>(view.subImage.swapchain);
            if (source.xrDesc.createFlags &
                (XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT | XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) {
                return bypass();
            }
        }
        const auto& leftSource = *reinterpret_cast<const Swapchain*>(sourceProjection.views[0].subImage.swapchain);
        const auto& rightSource = *reinterpret_cast<const Swapchain*>(sourceProjection.views[1].subImage.swapchain);
        if (isSRGBFormat(leftSource.dxgiFormatForSubmission) != isSRGBFormat(rightSource.dxgiFormatForSubmission)) {
            return bypass();
        }
        if (m_fgNeedReprime.exchange(false)) {
            m_fgPreviousCapturedTime = 0;
        }
        const auto& projection = m_layersForFrame[0].EyeFov;
        const auto size = projection.Viewport[0].Size;
        if (size.w <= 0 || size.h <= 0 || projection.Viewport[1].Size.w != size.w ||
            projection.Viewport[1].Size.h != size.h) {
            return bypass();
        }
        D3D11_TEXTURE2D_DESC texture{};
        m_precompositor.finalColour[0]->GetDesc(&texture);
        DXGI_FORMAT format = m_precompositor.isProj0SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (texture.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && texture.Format != format &&
            texture.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            return bypass();
        }
        if (m_frameGeneration &&
            (m_fgEyeResolution.w != size.w || m_fgEyeResolution.h != size.h || m_fgFormat != format)) {
            // An active group owns these output banks. Publish this input on
            // the legacy path, which makes the following producer await drain.
            std::unique_lock lock(m_asyncSubmissionMutex);
            if (m_fgActiveSubmission) {
                return bypass();
            }
            lock.unlock();
            cleanupFrameGeneration();
            m_fgPreviousCapturedTime = 0;
        }
        if (!m_frameGeneration) {
            // Create presentation roles before accepting GPU work. Each role has
            // its own chain; committing S2 never changes what S1 points to.
            auto rollback = MakeScopeGuard([&] { cleanupFrameGeneration(); });
            try {
                ComPtr<IDXGIDevice> device;
                CHECK_HRCMD(m_ovrSubmissionDevice.As(&device));
                ComPtr<IDXGIAdapter> adapter;
                CHECK_HRCMD(device->GetAdapter(&adapter));
                CHECK_HRCMD(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_fgDevice)));
                for (auto& bank : m_fgPresentationBanks) {
                    for (unsigned role = 0; role < bank.size(); ++role) {
                        if (role == 1 && m_frameGenerationMode == 2) {
                            continue;
                        }
                        auto& slice = bank[role];
                        ovrTextureSwapChainDesc desc{};
                        desc.Type = ovrTexture_2D;
                        desc.ArraySize = desc.MipLevels = desc.SampleCount = 1;
                        desc.Width = 2 * size.w;
                        desc.Height = size.h;
                        desc.Format =
                            m_precompositor.isProj0SRGB ? OVR_FORMAT_R8G8B8A8_UNORM_SRGB : OVR_FORMAT_R8G8B8A8_UNORM;
                        desc.MiscFlags = ovrTextureMisc_DX_Typeless;
                        CHECK_OVRCMD(ovr_CreateTextureSwapChainDX(
                            m_ovrSession, m_ovrSubmissionDevice.Get(), &desc, &slice.ovrSwapchain));
                        int count{};
                        CHECK_OVRCMD(ovr_GetTextureSwapChainLength(m_ovrSession, slice.ovrSwapchain, &count));
                        CHECK_MSG(count > 0, "Empty frame-generation presentation chain");
                        for (int index = 0; index < count; ++index) {
                            ComPtr<ID3D11Texture2D> image;
                            CHECK_OVRCMD(ovr_GetTextureSwapChainBufferDX(
                                m_ovrSession, slice.ovrSwapchain, index, IID_PPV_ARGS(&image)));
                            slice.images.push_back(std::move(image));
                        }
                        slice.resolution = {desc.Width, desc.Height};
                    }
                }
            } catch (const std::exception& error) {
                // No FG input was submitted. The active rollback owns every
                // partial chain; the complete NR layer remains available.
                m_frameGenerationUnavailable = true;
                ErrorLog("Frame-generation presentation allocation failed: %s; preserving NR.\n", error.what());
                return bypass();
            }
            m_frameGeneration = std::make_unique<FrameGeneration>();
            FrameGeneration::InitInfo init{m_ovrSubmissionDevice.Get(),
                                           m_ovrSubmissionContext.Get(),
                                           m_fgDevice.Get(),
                                           std::filesystem::absolute(dllHome / L"vdxr-frame-generation.dll"),
                                           static_cast<uint32_t>(size.w),
                                           static_cast<uint32_t>(size.h),
                                           m_frameGenerationMode,
                                           format};
            // Start the ETW capture before launching the game. Avoid changing
            // instrumentation or recreating the GPU backend during gameplay.
            init.enableGpuTiming = IsTraceEnabled();
            const auto result = m_frameGeneration->initialize(init);
            if (FAILED(result)) {
                if (result == VDXRFG_E_POISONED) {
                    rollback.Deactivate();
                    CHECK_HRCMD(result);
                }
                m_frameGenerationUnavailable = true;
                ErrorLog("Frame generation unavailable (0x%08x); preserving the NR path.\n",
                         static_cast<unsigned>(result));
                return bypass();
            }
            m_fgEyeResolution = size;
            m_fgFormat = format;
            rollback.Deactivate();
        }
        const auto& previous = m_fgPreviousSettings;
        const auto sharpen = std::clamp(m_precompositor.sharpenFactor, 0.f, 1.f);
        bool changedFov{};
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& before = m_fgPreviousFov[eye];
            const auto& after = projection.Fov[eye];
            changedFov |= before.UpTan != after.UpTan || before.DownTan != after.DownTan ||
                          before.LeftTan != after.LeftTan || before.RightTan != after.RightTan;
        }
        if (m_fgPreviousCapturedTime &&
            (m_fgPreviousHistoryEpoch != m_fgBegunReservation.historyEpoch ||
             m_fgPreviousSpace != info.layers[0]->space || changedFov || m_fgPreviousSharpen != sharpen ||
             settings.enabled != previous.enabled || settings.style != previous.style ||
             settings.intensity != previous.intensity || settings.localToneStrength != previous.localToneStrength ||
             settings.localStructureStrength != previous.localStructureStrength ||
             settings.skinStructureStrength != previous.skinStructureStrength ||
             settings.foveationSize != previous.foveationSize)) {
            const auto result = m_frameGeneration->reset(1);
            if (result == VDXRFG_E_BUSY) {
                return bypass();
            }
            CHECK_HRCMD(result);
            m_fgPreviousCapturedTime = 0;
        }
        FinalProjectionInput input;
        input.colour = m_precompositor.finalColour;
        input.serial = m_frameBegun;
        input.time = info.displayTime;
        input.period = m_fgBegunReservation.period;
        input.slots = m_frameGenerationCadence.plan(
            m_fgBegunReservation, m_fgPreviousCapturedTime, m_fgPreviousCapturedTime != 0);
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& pose = projection.RenderPose[eye];
            auto& view = input.views[eye];
            view.position[0] = pose.Position.x;
            view.position[1] = pose.Position.y;
            view.position[2] = pose.Position.z;
            view.orientation[0] = pose.Orientation.x;
            view.orientation[1] = pose.Orientation.y;
            view.orientation[2] = pose.Orientation.z;
            view.orientation[3] = pose.Orientation.w;
            view.fov[0] = -std::atan(projection.Fov[eye].LeftTan);
            view.fov[1] = std::atan(projection.Fov[eye].RightTan);
            view.fov[2] = std::atan(projection.Fov[eye].UpTan);
            view.fov[3] = -std::atan(projection.Fov[eye].DownTan);
            const auto& rect = projection.Viewport[eye];
            if (rect.Pos.x < 0 || rect.Pos.y < 0) {
                return bypass();
            }
            view.rect = {static_cast<uint32_t>(rect.Pos.x),
                         static_cast<uint32_t>(rect.Pos.y),
                         static_cast<uint32_t>(rect.Size.w),
                         static_cast<uint32_t>(rect.Size.h)};
        }
        const auto result = m_frameGeneration->capture(input, m_fgTransactionForFrame);
        if (result == VDXRFG_E_BUSY || result == VDXRFG_E_REJECTED) {
            return bypass();
        }
        CHECK_HRCMD(result);
        if (!settings.enabled) {
            // Ordinary sources can alias the application's swapchain. Queue B
            // into its private presentation image before xrEndFrame returns,
            // while the producer still owns this context. The worker reuses
            // this exact prepared copy rather than rereading reused app pixels.
            auto& real = m_fgPresentationBanks[m_fgTransactionForFrame.ticket.bank][2];
            int image{};
            CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(m_ovrSession, real.ovrSwapchain, &image));
            CHECK_MSG(image >= 0 && static_cast<size_t>(image) < real.images.size(), "Invalid FG real-frame image");
            CHECK_HRCMD(m_frameGeneration->copyOutput(
                m_fgTransactionForFrame, FrameGenerationRole::real, real.images[image].Get()));
        }
        m_fgPreviousCapturedTime = input.time;
        m_fgPreviousHistoryEpoch = m_fgBegunReservation.historyEpoch;
        m_fgPreviousSpace = info.layers[0]->space;
        m_fgPreviousSharpen = sharpen;
        for (unsigned eye = 0; eye < 2; ++eye) {
            m_fgPreviousFov[eye] = projection.Fov[eye];
        }
        m_fgPreviousSettings = settings;
        m_fgDesiredMultiplier = m_frameGenerationMode;
        return true;
    }

    void OpenXrRuntime::protectFrameGenerationSources() {
        if (m_precompositor.dlssnrSettings.enabled || (!m_d3d11Context && !m_d3d12CommandQueue)) {
            return;
        }
        // Returning from EndFrame permits the app to reuse its images on
        // an independent queue. Order those future writes after both source
        // copies without a CPU wait or a dependency on FG synthesis.
        if (!m_fgSourceFence11 && !m_fgSourceFence12) {
            wil::unique_handle handle;
            CHECK_HRCMD(
                m_ovrSubmissionCompletionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, handle.put()));
            if (m_d3d12CommandQueue) {
                CHECK_HRCMD(m_d3d12Device->OpenSharedHandle(handle.get(), IID_PPV_ARGS(&m_fgSourceFence12)));
            } else {
                CHECK_HRCMD(m_d3d11Device->OpenSharedFence(handle.get(), IID_PPV_ARGS(&m_fgSourceFence11)));
            }
        }
        const auto complete = ++m_submissionFenceValue;
        CHECK_HRCMD(m_ovrSubmissionContext->Signal(m_ovrSubmissionCompletionFence.Get(), complete));
        m_ovrSubmissionContext->Flush();
        if (m_d3d12CommandQueue) {
            CHECK_HRCMD(m_d3d12CommandQueue->Wait(m_fgSourceFence12.Get(), complete));
        } else {
            CHECK_HRCMD(m_d3d11Context->Wait(m_fgSourceFence11.Get(), complete));
            m_d3d11Context->Flush();
        }
    }

    void OpenXrRuntime::cleanupFrameGeneration() {
        if (m_frameGeneration) {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            for (auto* transaction :
                 {&m_fgTransactionForFrame, &m_fgTransactionForSubmission, &m_fgFailedTransaction}) {
                if (!transaction->ticket.token) {
                    continue;
                }
                HRESULT result;
                do {
                    result = m_frameGeneration->retire(*transaction);
                    if (result != VDXRFG_E_BUSY) {
                        break;
                    }
                    std::this_thread::sleep_for(1ms);
                } while (std::chrono::steady_clock::now() < deadline);
                CHECK_HRCMD(result);
            }
            // Leave the complete graph owned on failed settlement. The caller
            // must stop teardown rather than destroying its presentation chains.
            HRESULT result;
            do {
                result = m_frameGeneration->shutdown();
                if (result != VDXRFG_E_BUSY) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
            CHECK_HRCMD(result);
            m_frameGeneration.reset();
        }
        for (auto& bank : m_fgPresentationBanks) {
            for (auto& slice : bank) {
                slice.images.clear();
                if (slice.ovrSwapchain) {
                    ovr_DestroyTextureSwapChain(m_ovrSession, slice.ovrSwapchain);
                }
                slice = {};
            }
        }
        m_fgEyeResolution = {};
        m_fgFormat = DXGI_FORMAT_UNKNOWN;
        m_fgDevice.Reset();
        m_fgSourceFence11.Reset();
        m_fgSourceFence12.Reset();
        m_fgPreviousCapturedTime = 0;
        m_fgFailedLayers.clear();
        m_frameGenerationCadence.reset();
    }

    void OpenXrRuntime::asyncFrameGenerationThread() {
        OwnedFrameTransaction transaction;
        std::vector<ovrLayer_Union> ownedLayers;
        long long nativeId{};
        try {
            SetThreadPriority(GetCurrentThread(),
                              getSetting("async_submission_priority").value_or(THREAD_PRIORITY_TIME_CRITICAL));
            bool begun{};
            bool started{};
            const auto beginNative = [&] {
                CHECK_MSG(nativeId != LLONG_MAX, "Frame-generation native ID exhausted");
                // Timing wait is the SDK's main-thread operation; only the
                // render-thread Begin/End phases lease the immediate context.
                while (true) {
                    {
                        std::unique_lock lock(m_asyncSubmissionMutex);
                        if (m_terminateAsyncThread) {
                            return false;
                        }
                    }
                    const auto result = ovr_WaitToBeginFrame(m_ovrSession, nativeId);
                    if (result == ovrError_NotInitialized) {
                        std::unique_lock lock(m_asyncSubmissionMutex);
                        if (m_asyncSubmissionCondVar.wait_for(lock, 1ms, [&] { return m_terminateAsyncThread; })) {
                            return false;
                        }
                        continue; // No Begin occurred; retry the same native ID.
                    }
                    if (result == ovrError_Timeout) {
                        ErrorLog("Timeout in frame-generation submission thread; continuing native Begin.\n");
                    } else {
                        CHECK_OVRCMD(result);
                    }
                    break;
                }
                {
                    std::unique_lock lock(m_asyncSubmissionMutex);
                    if (m_terminateAsyncThread) {
                        return false;
                    }
                }
                std::unique_lock contextLease(m_fgContextMutex);
                CHECK_OVRCMD(ovr_BeginFrame(m_ovrSession, nativeId));
                begun = true;
                return true;
            };
            while (true) {
                {
                    std::unique_lock lock(m_asyncSubmissionMutex);
                    if (m_terminateAsyncThread) {
                        break;
                    }
                }
                if (!started) {
                    if (!beginNative()) {
                        break;
                    }
                    started = true;
                }
                PredictionReservation reservation;
                {
                    std::unique_lock lock(m_asyncSubmissionMutex);
                    m_lastWaitToBeginFrameTime = std::chrono::high_resolution_clock::now();
                    if (m_layersForAsyncSubmission.empty()) {
                        m_asyncSubmissionReady = true;
                    }
                    m_asyncSubmissionCondVar.notify_all();
                    m_asyncSubmissionCondVar.wait(
                        lock, [&] { return m_terminateAsyncThread || !m_layersForAsyncSubmission.empty(); });
                    if (m_terminateAsyncThread) {
                        break;
                    }
                    ownedLayers.swap(m_layersForAsyncSubmission);
                    transaction = std::move(m_fgTransactionForSubmission);
                    m_fgTransactionForSubmission = {};
                    reservation = m_fgReservationForSubmission;
                    m_fgActiveSubmission = true;
                    m_fgActiveOwnedPixels = transaction.ticket.token != 0;
                    m_fgWorkerGroupNativeId =
                        static_cast<uint64_t>(nativeId) + std::clamp(reservation.multiplier, 1u, 3u);
                    // The mailbox is empty, but a producer must not enqueue
                    // next-frame GPU waits ahead of this group's SDK copies.
                    m_asyncSubmissionReady = false;
                    m_asyncSubmissionCondVar.notify_all();
                }
                const bool hasGpuOutput = transaction.ticket.token != 0;
                std::array<int, 3> preparedImages{{-1, -1, -1}};
                const auto prepareRole = [&](FrameGenerationRole role) {
                    const unsigned index =
                        role == FrameGenerationRole::real ? 2 : (role == FrameGenerationRole::synthetic1 ? 0 : 1);
                    auto& output = m_fgPresentationBanks[transaction.ticket.bank][index];
                    auto& image = preparedImages[index];
                    CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(m_ovrSession, output.ovrSwapchain, &image));
                    CHECK_MSG(image >= 0 && static_cast<size_t>(image) < output.images.size(),
                              "Invalid FG compositor image");
                    CHECK_HRCMD(m_frameGeneration->copyOutput(transaction, role, output.images[image].Get()));
                };
                if (hasGpuOutput) {
                    std::unique_lock contextLease(m_fgContextMutex);
                    // Stage finalized B before any synthesis wait. A missed
                    // synthetic deadline can now use the same 100% NR pixels.
                    prepareRole(FrameGenerationRole::real);
                }
                bool generated = transaction.ticket.token && !transaction.ticket.prime && reservation.valid &&
                                 reservation.nativeId == static_cast<uint64_t>(nativeId);
                if (!reservation.valid || reservation.nativeId != static_cast<uint64_t>(nativeId)) {
                    m_fgNeedReprime = m_fgTimelineReanchor = true;
                }
                if (generated) {
                    const auto deadline = xrTimeToOvrTime(transaction.slots.targets[0]);
                    bool outputReady{};
                    while (true) {
                        {
                            std::unique_lock lock(m_asyncSubmissionMutex);
                            if (m_terminateAsyncThread) {
                                break;
                            }
                        }
                        HANDLE completionEvent{};
                        HRESULT result;
                        {
                            std::unique_lock contextLease(m_fgContextMutex);
                            // An expired group needs its existing real-frame
                            // fallback, not another fallible kernel registration.
                            if (ovr_GetTimeInSeconds() >= deadline) {
                                break;
                            }
                            result = m_frameGeneration->prepareOutputReadyWait(transaction, completionEvent);
                        }
                        if (result != VDXRFG_E_BUSY) {
                            CHECK_HRCMD(result);
                            outputReady = true;
                            break;
                        }
                        if (ovr_GetTimeInSeconds() >= deadline) {
                            break;
                        }
                        // GPU completion wakes us immediately. Bound timeout
                        // retries for shutdown; neither context mutex is held.
                        const auto waited = WaitForSingleObject(completionEvent, 1);
                        if (waited == WAIT_FAILED) {
                            CHECK_HRCMD(HRESULT_FROM_WIN32(GetLastError()));
                        }
                        CHECK_MSG(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT, "FG ready event wait failed");
                    }
                    generated = outputReady && m_frameGenerationCadence.onTime(transaction.slots,
                                                                               ovrTimeToXrTime(ovr_GetTimeInSeconds()));
                    if (!generated) {
                        m_fgNeedReprime = m_fgTimelineReanchor = true;
                    }
                }
                const unsigned cycles =
                    reservation.multiplier >= 1 && reservation.multiplier <= 3 ? reservation.multiplier : 1;
                std::unique_lock contextLease(m_fgContextMutex);
                if (generated) {
                    // All current destination copies precede the next NR/app
                    // Wait on the shared immediate context. Do not commit a
                    // synthetic role until its native slot actually uses it.
                    for (unsigned slot = 0; slot < cycles - 1; ++slot) {
                        prepareRole(transaction.slots.roles[slot]);
                    }
                }
                bool stopping{};
                if (!begun) {
                    // Prepare subsequent groups before the SDK's timing wait.
                    // Waiting first leaves only its short render lead for
                    // cross-API destination copies and completion polling.
                    contextLease.unlock();
                    stopping = !beginNative();
                    contextLease.lock();
                }
                // Retain the lease through the first Commit/End. Admission
                // reopens only after selecting that first role below.
                bool committedReal{};
                ovrLayer_Union realLayer{};
                for (unsigned slot = 0; slot < cycles && !stopping; ++slot) {
                    if (!begun && !beginNative()) {
                        break;
                    }
                    if (!contextLease.owns_lock()) {
                        contextLease.lock();
                    }
                    std::array<const ovrLayerHeader*, ovrMaxLayerCount> pointers{};
                    unsigned layerCount{};
                    ovrLayer_Union layer{};
                    if (hasGpuOutput) {
                        auto role = generated ? transaction.slots.roles[slot] : FrameGenerationRole::real;
                        // A missed later slot is real-only; never retime S2 with
                        // its original fraction or replay an already sent S1.
                        if (generated && slot < cycles - 1) {
                            const auto deadline = xrTimeToOvrTime(transaction.slots.targets[slot]);
                            while (!m_frameGeneration->copyReady(transaction, role) &&
                                   ovr_GetTimeInSeconds() < deadline) {
                                const auto remainingMs = (deadline - ovr_GetTimeInSeconds()) * 1000;
                                const auto timeoutMs = static_cast<DWORD>(std::clamp(std::ceil(remainingMs), 1., 5.));
                                const auto result = m_frameGeneration->waitForCopy(transaction, role, timeoutMs);
                                if (result != VDXRFG_E_BUSY) {
                                    CHECK_HRCMD(result);
                                }
                                std::unique_lock lock(m_asyncSubmissionMutex);
                                if (m_terminateAsyncThread) {
                                    break;
                                }
                            }
                            if (!m_frameGeneration->copyReady(transaction, role) ||
                                ovrTimeToXrTime(ovr_GetTimeInSeconds()) > transaction.slots.targets[slot]) {
                                generated = false;
                                role = FrameGenerationRole::real;
                                m_fgNeedReprime = true;
                                m_fgTimelineReanchor = true;
                            }
                        }
                        const unsigned index =
                            role == FrameGenerationRole::real ? 2 : (role == FrameGenerationRole::synthetic1 ? 0 : 1);
                        if (role == FrameGenerationRole::real && committedReal) {
                            layer = realLayer;
                        } else {
                            auto& output = m_fgPresentationBanks[transaction.ticket.bank][index];
                            const int image = preparedImages[index];
                            CHECK_MSG(image >= 0, "Unprepared FG compositor image");
                            CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, output.ovrSwapchain));
                            output.lastCommittedIndex = image;
                            layer = ownedLayers[0];
                            layer.Header.Type = ovrLayerType_EyeFov;
                            for (unsigned eye = 0; eye < 2; ++eye) {
                                layer.EyeFov.ColorTexture[eye] = output.ovrSwapchain;
                                layer.EyeFov.Viewport[eye].Pos = {static_cast<int>(eye) * m_fgEyeResolution.w, 0};
                                layer.EyeFov.Viewport[eye].Size = m_fgEyeResolution;
                            }
                            if (role == FrameGenerationRole::real) {
                                realLayer = layer;
                                committedReal = true;
                            }
                        }
                        pointers[layerCount++] = &layer.Header;
                    } else {
                        // Unsupported composition preserves the complete layer
                        // list. Only transition reservations have padding repeats.
                        for (const auto& original : ownedLayers) {
                            if (layerCount == pointers.size()) {
                                break;
                            }
                            pointers[layerCount++] = &original.Header;
                        }
                    }
                    ovrViewScaleDesc scale{};
                    scale.HmdToEyePose[0] = m_cachedEyeInfo[0].HmdToEyePose;
                    scale.HmdToEyePose[1] = m_cachedEyeInfo[1].HmdToEyePose;
                    scale.HmdSpaceToWorldScaleInMeters = 1.f;
                    if (!slot) {
                        std::unique_lock lock(m_asyncSubmissionMutex);
                        m_lastWaitToBeginFrameTime = std::chrono::high_resolution_clock::now();
                        // Independent B can be ready while the module is still
                        // busy. Do not provoke a rejected next capture/reset;
                        // an unready or missed group opens capacity on retire.
                        m_asyncSubmissionReady =
                            !hasGpuOutput || (!m_fgNeedReprime && m_frameGeneration->ready(transaction));
                        m_asyncSubmissionCondVar.notify_all();
                    }
                    const auto result = ovr_EndFrame(m_ovrSession, nativeId, &scale, pointers.data(), layerCount);
                    TraceLoggingWrite(log::g_traceProvider,
                                      "FG_Present",
                                      TLArg(nativeId, "NativeId"),
                                      TLArg(generated && slot < cycles - 1, "Synthetic"),
                                      TLArg(!generated && slot < cycles - 1, "PaddingRepeat"),
                                      TLArg(static_cast<int>(result), "Result"));
                    // An attempted cycle is never reused, including End failure.
                    ++nativeId;
                    begun = false;
                    CHECK_OVRCMD(result);
                    contextLease.unlock();
                }
                if (contextLease.owns_lock()) {
                    contextLease.unlock();
                }
                if (transaction.ticket.token) {
                    const auto deadline = std::chrono::steady_clock::now() + 10s;
                    HRESULT result;
                    do {
                        {
                            std::unique_lock retirementLease(m_fgContextMutex);
                            result = m_frameGeneration->retire(transaction);
                        }
                        if (result != VDXRFG_E_BUSY) {
                            break;
                        }
                        std::this_thread::sleep_for(1ms);
                    } while (std::chrono::steady_clock::now() < deadline);
                    CHECK_HRCMD(result);
                }
                ownedLayers.clear();
                {
                    std::unique_lock lock(m_asyncSubmissionMutex);
                    m_fgActiveSubmission = m_fgActiveOwnedPixels = false;
                    m_asyncSubmissionReady = m_layersForAsyncSubmission.empty();
                    m_asyncSubmissionCondVar.notify_all();
                }
            }
        } catch (...) {
            std::unique_lock lock(m_asyncSubmissionMutex);
            // A pending group may already be published: retain both owners.
            m_fgFailedTransaction = std::move(transaction);
            m_fgFailedLayers.swap(ownedLayers);
            m_asyncSubmissionError = std::current_exception();
            m_asyncSubmissionReady = false;
            m_asyncSubmissionCondVar.notify_all();
        }
    }

    void OpenXrRuntime::waitForFrameGenerationCapacity() {
        std::unique_lock lock(m_asyncSubmissionMutex);
        m_asyncSubmissionCondVar.wait(lock, [&] {
            return m_terminateAsyncThread || m_asyncSubmissionError ||
                   (m_asyncSubmissionReady && (!m_fgActiveSubmission || m_fgActiveOwnedPixels));
        });
        if (m_asyncSubmissionError) {
            std::rethrow_exception(m_asyncSubmissionError);
        }
        CHECK_MSG(!m_terminateAsyncThread, "Frame-generation submission is stopping");
    }
} // namespace virtualdesktop_openxr
