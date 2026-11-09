#include "pch.h"
#include "runtime.h"
#include "log.h"
#include "utils.h"
#include "frame_generation_layers.h"

namespace virtualdesktop_openxr {
    using namespace log;

    namespace {
        DXGI_FORMAT depthShaderFormat(DXGI_FORMAT format) {
            switch (format) {
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_R16_TYPELESS:
                return DXGI_FORMAT_R16_UNORM;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_R24G8_TYPELESS:
                return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_R32_TYPELESS:
                return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32G8X24_TYPELESS:
                return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            default:
                return DXGI_FORMAT_UNKNOWN;
            }
        }
    } // namespace

    bool OpenXrRuntime::prepareFrameGeneration(const XrFrameEndInfo& info) {
        CHECK_MSG(!m_fgDepthCompletionUnknown, "FG depth completion unknown; refusing new captures");
        CHECK_MSG(!m_fgTransactionForFrame.ticket.token, "Unpublished FG transaction; refusing new captures");
        m_fgDepthSnapshotForFrame = false;
        const auto& settings = m_precompositor.dlssnrSettings;
        const auto bypass = [&](bool temporary = false) {
            // Live history pressure must not shorten the next recovery prime.
            m_fgDesiredMultiplier = temporary ? m_frameGenerationMode : 1;
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
                              m_layersForFrame.size() == 1 &&
                              (m_layersForFrame[0].Header.Type == ovrLayerType_EyeFov ||
                               m_layersForFrame[0].Header.Type == ovrLayerType_EyeFovDepth) &&
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
        const bool hasDepth = projection.Header.Type == ovrLayerType_EyeFovDepth;
        const auto size = projection.Viewport[0].Size;
        if (size.w <= 0 || size.h <= 0 || projection.Viewport[1].Size.w != size.w ||
            projection.Viewport[1].Size.h != size.h) {
            return bypass();
        }
        if (hasDepth) {
            // Depth sources remain app-owned even when NR owns the colour.
            // Preserve the old complete fallback for APIs whose return-queue
            // protection is not implemented by protectFrameGenerationSources.
            if (!m_d3d11Context && !m_d3d12CommandQueue)
                return bypass();
            const auto& depthProjection = m_layersForFrame[0].EyeFovDepth.ProjectionDesc;
            if (!std::isfinite(depthProjection.Projection22) || !std::isfinite(depthProjection.Projection23) ||
                !std::isfinite(depthProjection.Projection32) || !depthProjection.Projection23 ||
                !depthProjection.Projection32)
                return bypass();
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& image = m_precompositor.finalDepth[eye];
                const auto& rect = projection.Viewport[eye];
                if (!image || !m_layersForFrame[0].EyeFovDepth.DepthTexture[eye] || rect.Pos.x < 0 || rect.Pos.y < 0)
                    return bypass();
                D3D11_TEXTURE2D_DESC desc{};
                image->GetDesc(&desc);
                ComPtr<ID3D11Device> owner;
                image->GetDevice(&owner);
                ComPtr<IUnknown> sourceIdentity, submissionIdentity;
                if (FAILED(owner.As(&sourceIdentity)) || FAILED(m_ovrSubmissionDevice.As(&submissionIdentity)) ||
                    sourceIdentity.Get() != submissionIdentity.Get() || desc.SampleDesc.Count != 1 || !desc.ArraySize ||
                    depthShaderFormat(desc.Format) == DXGI_FORMAT_UNKNOWN ||
                    uint64_t(rect.Pos.x) + rect.Size.w > desc.Width || uint64_t(rect.Pos.y) + rect.Size.h > desc.Height)
                    return bypass();
                auto entry = reinterpret_cast<const XrBaseInStructure*>(sourceProjection.views[eye].next);
                while (entry && entry->type != XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR)
                    entry = entry->next;
                if (!entry)
                    return bypass();
                const auto& depth = *reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(entry);
                const auto& source = *reinterpret_cast<const Swapchain*>(depth.subImage.swapchain);
                if (source.xrDesc.createFlags &
                    (XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT | XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))
                    return bypass();
            }
        }
        D3D11_TEXTURE2D_DESC texture{};
        m_precompositor.finalColour[0]->GetDesc(&texture);
        DXGI_FORMAT format = m_precompositor.isProj0SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (texture.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && texture.Format != format &&
            texture.Format != DXGI_FORMAT_R8G8B8A8_UNORM && texture.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS &&
            texture.Format != DXGI_FORMAT_B8G8R8A8_UNORM && texture.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            return bypass();
        }
        // Validate the actual final rays and original source pair before
        // requesting any persistent guide memory. This value copy also keeps
        // caller-owned OpenXR next chains out of backend state.
        FinalProjectionInput input;
        input.colour = m_precompositor.finalColour;
        input.depth = m_precompositor.originalFgDepth;
        input.serial = m_frameBegun;
        input.nrEnabled = settings.enabled;
        input.entry = vdxrfg::pair_capture::currentEntry;
        input.time = info.displayTime;
        input.period = m_fgBegunReservation.period;
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
            if (rect.Pos.x < 0 || rect.Pos.y < 0)
                return bypass();
            view.rect = {static_cast<uint32_t>(rect.Pos.x),
                         static_cast<uint32_t>(rect.Pos.y),
                         static_cast<uint32_t>(rect.Size.w),
                         static_cast<uint32_t>(rect.Size.h)};
            auto& guide = input.depth[eye];
            // Sharpen-only dispatches can pad an unaligned viewport rather
            // than resize it. Do not describe that padding as an affine map.
            if (std::abs(m_upscalingMultiplier - 1.f) <= FLT_EPSILON &&
                (view.rect.width != guide.colourRect.width || view.rect.height != guide.colourRect.height))
                guide.affineVerified = false;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (!std::isfinite(view.position[axis]) || !std::isfinite(guide.captureView.position[axis]) ||
                    std::abs(view.position[axis] - guide.captureView.position[axis]) > 1e-5f)
                    guide.affineVerified = false;
            float norm{};
            for (unsigned axis = 0; axis < 4; ++axis) {
                if (!std::isfinite(view.orientation[axis]) || !std::isfinite(guide.captureView.orientation[axis]) ||
                    !std::isfinite(view.fov[axis]) || !std::isfinite(guide.captureView.fov[axis]) ||
                    std::abs(view.orientation[axis] - guide.captureView.orientation[axis]) > 1e-5f ||
                    std::abs(view.fov[axis] - guide.captureView.fov[axis]) > 1e-5f)
                    guide.affineVerified = false;
                norm += view.orientation[axis] * view.orientation[axis];
            }
            if (!std::isfinite(norm) || std::abs(norm - 1.f) > .02f || view.fov[0] >= view.fov[1] ||
                view.fov[3] >= view.fov[2])
                guide.affineVerified = false;
        }
        if (!m_fgDepthGuidanceRequested && m_frameGenerationDepthMode != 0 && m_frameGenerationBackend != 1 &&
            frameGenerationDepthSourceValid(m_ovrSubmissionDevice.Get(), input.depth[0]) &&
            frameGenerationDepthSourceValid(m_ovrSubmissionDevice.Get(), input.depth[1])) {
            // A session upgrades at most once. Gaps, resize and unsupported
            // modules retain this request so they cannot cause recreate loops.
            m_fgDepthGuidanceRequested = true;
        }
        const bool guideUpgrade =
            m_frameGeneration && m_fgDepthGuidanceRequested && !m_frameGeneration->depthGuidanceRequested();
        // Admission above permits exactly one native projection. VDXR's
        // existing first-layer policy ignores source alpha, even when an XR
        // bridge sets BLEND_TEXTURE_SOURCE_ALPHA. Describe the finalized
        // projection sent to VD, rather than those unused application flags.
        constexpr bool opaqueInput = true;
        if (m_frameGeneration) {
            const auto opacityResult = m_frameGeneration->updateOpaqueInput(opaqueInput);
            if (opacityResult == VDXRFG_E_BUSY)
                return bypass(true);
            CHECK_HRCMD(opacityResult);
            if (opacityResult == S_OK)
                m_fgPreviousCapturedTime = 0;
        }
        if (m_frameGeneration &&
            (m_fgEyeResolution.w != size.w || m_fgEyeResolution.h != size.h || m_fgFormat != format || guideUpgrade)) {
            // An active group owns these output banks. Publish this input on
            // the legacy path, which makes the following producer await drain.
            std::unique_lock lock(m_asyncSubmissionMutex);
            if (m_fgActiveSubmission) {
                return bypass(guideUpgrade);
            }
            lock.unlock();
            // Alignment has already queued reads for this frame. Keep its
            // sources through backend teardown for real-B capture or the
            // complete-layer fallback and its application return-queue fence.
            const auto finalDepthForFrame = m_precompositor.finalDepth;
            const auto originalDepthForFrame = m_precompositor.originalFgDepth;
            cleanupFrameGeneration();
            m_precompositor.finalDepth = finalDepthForFrame;
            m_precompositor.originalFgDepth = originalDepthForFrame;
            m_fgPreviousCapturedTime = 0;
            // An upgrade reparses a fresh input next frame. Ordinary resize
            // keeps the existing same-frame recreation path and owned sources.
            if (guideUpgrade)
                return bypass(true);
        }
        if (!m_frameGeneration) {
            // Each synthetic role has its own chain, shared across ticket banks:
            // the worker retires a group before preparing the next. Real B stays
            // banked because the producer can precopy it while a group is active.
            const auto finalDepthForFrame = m_precompositor.finalDepth;
            const auto originalDepthForFrame = m_precompositor.originalFgDepth;
            auto rollback = MakeScopeGuard([&] {
                cleanupFrameGeneration();
                m_precompositor.finalDepth = finalDepthForFrame;
                m_precompositor.originalFgDepth = originalDepthForFrame;
            });
            try {
                ComPtr<IDXGIDevice> device;
                CHECK_HRCMD(m_ovrSubmissionDevice.As(&device));
                ComPtr<IDXGIAdapter> adapter;
                CHECK_HRCMD(device->GetAdapter(&adapter));
                CHECK_HRCMD(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_fgDevice)));
                for (unsigned bankIndex = 0; bankIndex < m_fgPresentationBanks.size(); ++bankIndex) {
                    auto& bank = m_fgPresentationBanks[bankIndex];
                    for (unsigned role = 0; role < bank.size(); ++role) {
                        if ((bankIndex && role != static_cast<unsigned>(FrameGenerationRole::real)) ||
                            (role == 1 && m_frameGenerationMode == 2) || (role == 3 && m_frameGenerationMode != 4)) {
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
                        CHECK_OVRCMD(createSubmissionSwapchain(
                            m_ovrSession, presentationDevice(), &desc, &slice.ovrSwapchain, true));
                        int count{};
                        CHECK_OVRCMD(submissionSwapchainLength(m_ovrSession, slice.ovrSwapchain, &count));
                        CHECK_MSG(count > 0, "Empty frame-generation presentation chain");
                        for (int index = 0; index < count; ++index) {
                            ComPtr<ID3D11Texture2D> image;
                            CHECK_OVRCMD(submissionSwapchainBuffer(
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
            // The runtime does not consume module GPU statistics. Dedicated
            // offscreen benchmarks request their own timing-enabled contexts.
            init.deliveryDevice11 = m_deliveryTransport ? presentationDevice() : nullptr;
            init.deliveryContext11 = m_deliveryTransport ? presentationContext() : nullptr;
            init.enableGpuTiming = false;
            init.useFidelityFxFull = m_frameGenerationBackend == 1;
            init.enableDepthGuidance = m_fgDepthGuidanceRequested;
            init.opaqueInput = opaqueInput;
            init.depthMode = m_frameGenerationDepthMode;
            const auto result = m_frameGeneration->initialize(init);
            if (FAILED(result)) {
                if (result == VDXRFG_E_POISONED) {
                    rollback.Deactivate();
                    CHECK_HRCMD(result);
                }
                m_frameGenerationUnavailable = true;
                ErrorLog("Frame generation unavailable (backend %s, 0x%08x); preserving the real-frame path.\n",
                         init.useFidelityFxFull ? "FidelityFX full" : "NVIDIA",
                         static_cast<unsigned>(result));
                return bypass();
            }
            if (m_frameGenerationDepthMode == 2 && m_frameGeneration->negotiatedDepthMode() != 2 &&
                !m_fgDepthModeFallbackWarned) {
                m_fgDepthModeFallbackWarned = true;
                ErrorLog("Depth reconstruction requested; using Standard FG until compatible NVIDIA V5 support "
                         "and usable game depth are available.\n");
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
             settings.foveationSize != previous.foveationSize ||
             settings.processingResolution != previous.processingResolution)) {
            const auto result = m_frameGeneration->reset(1);
            if (result == VDXRFG_E_BUSY) {
                return bypass(true);
            }
            CHECK_HRCMD(result);
            m_fgPreviousCapturedTime = 0;
        }
        input.slots = m_frameGenerationCadence.plan(
            m_fgBegunReservation, m_fgPreviousCapturedTime, m_fgPreviousCapturedTime != 0);
        const bool collectDepthDiagnostics = IsTraceEnabled();
        FrameGeneration::CaptureDiagnostics depthDiagnostics{};
        const auto result = m_frameGeneration->capture(
            input, m_fgTransactionForFrame, collectDepthDiagnostics ? &depthDiagnostics : nullptr);
        TraceLoggingWrite(
            log::g_traceProvider,
            "FG_Capture",
            TLArg(input.serial, "Serial"),
            TLArg(m_frameGenerationMode, "RequestedMultiplier"),
            TLArg(m_fgBegunReservation.multiplier, "ReservedMultiplier"),
            TLArg(input.slots.count, "Slots"),
            TLArg(input.time, "Target"),
            TLArg(m_fgPreviousCapturedTime, "PreviousTarget"),
            TLArg(static_cast<int>(result), "Result"),
            TLArg(collectDepthDiagnostics, "DepthDiagnosticsCollected"),
            TLArg(depthDiagnostics.apiVersion, "SubmitEnvelopeVersion"),
            TLArg(depthDiagnostics.submitCalled, "ApiSubmitCalled"),
            TLArg(static_cast<int>(depthDiagnostics.submitResult), "ApiSubmitResult"),
            TLArg(depthDiagnostics.guideMask, "GuideMask"),
            TLArg(depthDiagnostics.guideSerial, "GuideSerial"),
            TLArg(depthDiagnostics.guideEpoch, "GuideEpoch"),
            TLArg(depthDiagnostics.guideEncoding, "GuideEncoding"),
            TLArg(depthDiagnostics.previousSerial, "ReturnedPreviousSerial"),
            TLArg(depthDiagnostics.currentSerial, "ReturnedCurrentSerial"),
            TLArg(depthDiagnostics.ticketToken, "ReturnedTicketToken"),
            TLArg(m_frameGeneration->depthGuidanceEnabled(), "DepthContextAvailable"),
            TLArg(m_frameGeneration->depthGuidanceRequested(), "DepthRequested"),
            TLArg(m_frameGenerationBackend, "Backend"),
            TLArg(m_frameGenerationDepthMode, "RequestedDepthMode"),
            TLArg(m_frameGeneration->negotiatedDepthMode(), "NegotiatedDepthMode"),
            TLArg((input.depth[0].affineVerified ? 1u : 0u) | (input.depth[1].affineVerified ? 2u : 0u),
                  "DepthAffineMask"),
            TLArg((frameGenerationDepthSourceValid(m_ovrSubmissionDevice.Get(), input.depth[0]) ? 1u : 0u) |
                      (frameGenerationDepthSourceValid(m_ovrSubmissionDevice.Get(), input.depth[1]) ? 2u : 0u),
                  "DepthSourceValidMask"));
        if (result == VDXRFG_E_BUSY || result == VDXRFG_E_REJECTED) {
            return bypass(result == VDXRFG_E_BUSY);
        }
        CHECK_HRCMD(result);
        if (hasDepth) {
            snapshotFrameGenerationDepth(m_fgTransactionForFrame, m_layersForFrame[0].EyeFovDepth);
            if (m_deliveryTransport) {
                auto& depth = m_fgDepthPresentationBanks[m_fgTransactionForFrame.ticket.bank];
                CHECK_OVRCMD(commitSubmissionSwapchain(m_ovrSession, depth.output.ovrSwapchain));
                m_fgDepthDelivery[m_fgTransactionForFrame.ticket.bank] = m_deliveryTransport->prepare(
                    {depth.output.ovrSwapchain}, m_fgTransactionForFrame.ticket.current_serial);
                // The producer snapshot now has a bank-owned GPU-ready receipt.
                // P consumes it before B's signal; failed P work retains both
                // transport and transaction and stops subsequent publication.
                m_fgDepthCompletionUnknown = false;
            }
        }
        if (!m_deliveryTransport && (!settings.enabled || hasDepth)) {
            // Ordinary sources can alias the application's swapchain. Queue B
            // into its private presentation image before xrEndFrame returns,
            // while the producer still owns this context. The worker reuses
            // this exact prepared copy rather than rereading reused app pixels.
            // For depth groups this signal also covers the preceding snapshot,
            // including NR sessions: the real color/depth pair is one consumer.
            auto& real = m_fgPresentationBanks[m_fgTransactionForFrame.ticket.bank][2];
            int image{};
            CHECK_OVRCMD(submissionSwapchainIndex(m_ovrSession, real.ovrSwapchain, &image));
            CHECK_MSG(image >= 0 && static_cast<size_t>(image) < real.images.size(), "Invalid FG real-frame image");
            CHECK_HRCMD(m_frameGeneration->copyOutput(
                m_fgTransactionForFrame, FrameGenerationRole::real, real.images[image].Get()));
            // The real role's successfully queued signal now owns every depth
            // read/write before it. Failure above deliberately keeps the graph.
            if (hasDepth)
                m_fgDepthCompletionUnknown = false;
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

    void OpenXrRuntime::snapshotFrameGenerationDepth(OwnedFrameTransaction& transaction,
                                                     const ovrLayerEyeFovDepth& layer) {
        CHECK_MSG(transaction.ticket.bank < m_fgDepthPresentationBanks.size(), "Invalid FG depth bank");
        auto& bank = m_fgDepthPresentationBanks[transaction.ticket.bank];
        // Submit cannot hand out this bank until its preceding ticket retires.
        // Retain sources and caches even if allocation, copy or signaling fails.
        if (m_deliveryTransport)
            m_fgDepthDelivery[transaction.ticket.bank] = {};
        bank.token = transaction.ticket.token;
        bank.sources = m_precompositor.finalDepth;
        if (!bank.output.ovrSwapchain) {
            ovrTextureSwapChainDesc desc{};
            desc.Type = ovrTexture_2D;
            desc.Format = OVR_FORMAT_D32_FLOAT;
            desc.Width = 2 * m_fgEyeResolution.w;
            desc.Height = m_fgEyeResolution.h;
            desc.ArraySize = desc.MipLevels = desc.SampleCount = 1;
            desc.BindFlags = ovrTextureBind_DX_DepthStencil;
            desc.MiscFlags = ovrTextureMisc_DX_Typeless;
            CHECK_OVRCMD(
                createSubmissionSwapchain(m_ovrSession, m_ovrSubmissionDevice.Get(), &desc, &bank.output.ovrSwapchain));
            int count{};
            CHECK_OVRCMD(submissionSwapchainLength(m_ovrSession, bank.output.ovrSwapchain, &count));
            CHECK_MSG(count > 0, "Empty FG depth presentation chain");
            for (int index = 0; index < count; ++index) {
                ComPtr<ID3D11Texture2D> image;
                CHECK_OVRCMD(
                    submissionSwapchainBuffer(m_ovrSession, bank.output.ovrSwapchain, index, IID_PPV_ARGS(&image)));
                D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_D32_FLOAT;
                view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                ComPtr<ID3D11DepthStencilView> dsv;
                CHECK_HRCMD(m_ovrSubmissionDevice->CreateDepthStencilView(image.Get(), &view, &dsv));
                bank.output.images.push_back(std::move(image));
                bank.output.dsvs.push_back(std::move(dsv));
            }
        }
        CHECK_OVRCMD(submissionSwapchainIndex(m_ovrSession, bank.output.ovrSwapchain, &bank.preparedImage));
        CHECK_MSG(bank.preparedImage >= 0 && static_cast<size_t>(bank.preparedImage) < bank.output.dsvs.size(),
                  "Invalid FG depth presentation image");
        for (unsigned eye = 0; eye < 2; ++eye) {
            D3D11_TEXTURE2D_DESC desc{}, existing{};
            bank.sources[eye]->GetDesc(&desc);
            const auto srvFormat = depthShaderFormat(desc.Format);
            desc.Format = utils::getTypelessFormat(desc.Format);
            desc.MipLevels = desc.ArraySize = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = desc.CPUAccessFlags = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            if (bank.inputCopies[eye])
                bank.inputCopies[eye]->GetDesc(&existing);
            if (!bank.inputCopies[eye] || existing.Width != desc.Width || existing.Height != desc.Height ||
                existing.Format != desc.Format) {
                ComPtr<ID3D11Texture2D> image;
                CHECK_HRCMD(m_ovrSubmissionDevice->CreateTexture2D(&desc, nullptr, &image));
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = srvFormat;
                view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                view.Texture2D.MipLevels = 1;
                ComPtr<ID3D11ShaderResourceView> srv;
                CHECK_HRCMD(m_ovrSubmissionDevice->CreateShaderResourceView(image.Get(), &view, &srv));
                bank.inputCopies[eye] = std::move(image);
                bank.srvs[eye] = std::move(srv);
            }
        }
        if (m_d3d11Device == m_ovrSubmissionDevice && !m_d3d11ContextState) {
            m_ovrSubmissionContext->SwapDeviceContextState(m_ovrSubmissionContextState.Get(),
                                                           m_d3d11ContextState.ReleaseAndGetAddressOf());
        }
        m_fgDepthSnapshotForFrame = true;
        m_fgDepthCompletionUnknown = true;
        for (unsigned eye = 0; eye < 2; ++eye) {
            // Depth/stencil copies must cover the entire subresource. The
            // shader then point-maps the established final-color viewport.
            m_ovrSubmissionContext->CopySubresourceRegion(
                bank.inputCopies[eye].Get(), 0, 0, 0, 0, bank.sources[eye].Get(), 0, nullptr);
            const auto& rect = layer.Viewport[eye];
            const XrRect2Di source{{rect.Pos.x, rect.Pos.y}, {rect.Size.w, rect.Size.h}};
            const ovrRecti destination{{static_cast<int>(eye) * m_fgEyeResolution.w, 0}, m_fgEyeResolution};
            renderDepth(bank.srvs[eye].Get(), bank.output.dsvs[bank.preparedImage].Get(), source, destination);
        }
    }

    void OpenXrRuntime::protectFrameGenerationSources(bool ordinaryFallback) {
        // NR's private colour does not cover depth alignment on an ordinary
        // fallback. Those queued source reads also precede application reuse.
        const bool hasDepthSources = m_precompositor.finalDepth[0] || m_precompositor.finalDepth[1];
        const bool hasGuideSources =
            m_frameGeneration && m_frameGeneration->depthGuidanceEnabled() &&
            (m_precompositor.originalFgDepth[0].source || m_precompositor.originalFgDepth[1].source);
        if ((m_precompositor.dlssnrSettings.enabled && !(m_deliveryTransport && ordinaryFallback) &&
             !m_fgDepthSnapshotForFrame && !hasDepthSources && !hasGuideSources) ||
            (!m_d3d11Context && !m_d3d12CommandQueue)) {
            return;
        }
        // Returning from EndFrame permits the app to reuse its images on
        // an independent queue. Order those future writes after both source
        // copies without a CPU wait or a dependency on FG synthesis.
        const auto complete = ++m_submissionFenceValue;
        const auto signalResult = m_ovrSubmissionContext->Signal(m_ovrSubmissionCompletionFence.Get(), complete);
        if (m_frameGeneration)
            m_frameGeneration->diagnostic("source-protection-Signal",
                                          m_frameBegun,
                                          m_ovrSubmissionContext.Get(),
                                          m_ovrSubmissionCompletionFence.Get(),
                                          complete,
                                          signalResult);
        CHECK_HRCMD(signalResult);
        m_ovrSubmissionContext->Flush();
        if (m_frameGeneration)
            m_frameGeneration->diagnostic("source-protection-Flush-void",
                                          m_frameBegun,
                                          m_ovrSubmissionContext.Get(),
                                          m_ovrSubmissionDevice.Get(),
                                          complete);
        orderApplicationAfterSourceReads(complete);
    }

    void OpenXrRuntime::orderApplicationAfterSourceReads(uint64_t complete) {
        if (!m_fgSourceFence11 && !m_fgSourceFence12) {
            wil::unique_handle handle;
            const auto exportResult =
                m_ovrSubmissionCompletionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, handle.put());
            if (m_frameGeneration)
                m_frameGeneration->diagnostic("source-protection-export11",
                                              m_frameBegun,
                                              m_ovrSubmissionCompletionFence.Get(),
                                              m_ovrSubmissionDevice.Get(),
                                              reinterpret_cast<uintptr_t>(handle.get()),
                                              exportResult);
            CHECK_HRCMD(exportResult);
            if (m_d3d12CommandQueue) {
                const auto importResult =
                    m_d3d12Device->OpenSharedHandle(handle.get(), IID_PPV_ARGS(&m_fgSourceFence12));
                if (m_frameGeneration)
                    m_frameGeneration->diagnostic("source-protection-import12",
                                                  m_frameBegun,
                                                  m_fgSourceFence12.Get(),
                                                  m_d3d12Device.Get(),
                                                  reinterpret_cast<uintptr_t>(handle.get()),
                                                  importResult);
                CHECK_HRCMD(importResult);
            } else {
                const auto importResult =
                    m_d3d11Device->OpenSharedFence(handle.get(), IID_PPV_ARGS(&m_fgSourceFence11));
                if (m_frameGeneration)
                    m_frameGeneration->diagnostic("source-protection-import11",
                                                  m_frameBegun,
                                                  m_fgSourceFence11.Get(),
                                                  m_d3d11Device.Get(),
                                                  reinterpret_cast<uintptr_t>(handle.get()),
                                                  importResult);
                CHECK_HRCMD(importResult);
            }
        }
        if (m_d3d12CommandQueue) {
            const auto waitResult = m_d3d12CommandQueue->Wait(m_fgSourceFence12.Get(), complete);
            if (m_frameGeneration)
                m_frameGeneration->diagnostic("application-Wait12",
                                              m_frameBegun,
                                              m_d3d12CommandQueue.Get(),
                                              m_fgSourceFence12.Get(),
                                              complete,
                                              waitResult);
            if (m_frameGeneration)
                m_frameGeneration->diagnostic(
                    "application-device12", m_frameBegun, m_d3d12Device.Get(), m_d3d12CommandQueue.Get(), 12);
            CHECK_HRCMD(waitResult);
        } else {
            const auto waitResult = m_d3d11Context->Wait(m_fgSourceFence11.Get(), complete);
            if (m_frameGeneration)
                m_frameGeneration->diagnostic("application-Wait11",
                                              m_frameBegun,
                                              m_d3d11Context.Get(),
                                              m_fgSourceFence11.Get(),
                                              complete,
                                              waitResult);
            if (m_frameGeneration)
                m_frameGeneration->diagnostic(
                    "application-device11", m_frameBegun, m_d3d11Device.Get(), m_d3d11Context.Get(), 11);
            CHECK_HRCMD(waitResult);
            m_d3d11Context->Flush();
            if (m_frameGeneration)
                m_frameGeneration->diagnostic(
                    "application-Flush-void", m_frameBegun, m_d3d11Context.Get(), m_d3d11Device.Get(), complete);
        }
    }

    void OpenXrRuntime::cleanupFrameGeneration() {
        if (m_fgDepthCompletionUnknown && m_deliveryTransport) {
            bool certified = true;
            for (size_t bank = 0; bank < m_fgDepthPresentationBanks.size(); ++bank)
                if (m_fgDepthPresentationBanks[bank].token && m_fgDepthPresentationBanks[bank].preparedImage >= 0)
                    certified &= m_fgDepthDelivery[bank].sourceReady != 0;
            if (certified) {
                m_deliveryTransport->drain();
                m_fgDepthCompletionUnknown = false;
            }
        }
        CHECK_MSG(!m_fgDepthCompletionUnknown, "FG depth completion unknown; retaining the complete graph");
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
                    destroySubmissionSwapchain(m_ovrSession, slice.ovrSwapchain);
                }
                slice = {};
            }
        }
        for (auto& bank : m_fgDepthPresentationBanks) {
            bank.sources = {};
            bank.inputCopies = {};
            bank.srvs = {};
            bank.output.dsvs.clear();
            bank.output.images.clear();
            if (bank.output.ovrSwapchain)
                destroySubmissionSwapchain(m_ovrSession, bank.output.ovrSwapchain);
            bank = {};
        }
        m_fgDepthSnapshotForFrame = false;
        m_fgDepthDelivery = {};
        m_precompositor.finalDepth = {};
        m_precompositor.originalFgDepth = {};
        m_fgEyeResolution = {};
        m_fgFormat = DXGI_FORMAT_UNKNOWN;
        m_fgDevice.Reset();
        m_fgSourceFence11.Reset();
        m_fgSourceFence12.Reset();
        m_fgPreviousCapturedTime = 0;
        m_fgFailedLayers.clear();
        m_frameGenerationCadence.reset();
        if (m_deliveryTransport)
            m_deliveryTransport->collectRetired();
    }

    void OpenXrRuntime::asyncFrameGenerationThread() {
        OwnedFrameTransaction transaction;
        std::vector<ovrLayer_Union> ownedLayers;
        DeliveryTransport::Batch ownedDelivery;
        long long nativeId{};
        try {
            SetThreadPriority(GetCurrentThread(),
                              getSetting("async_submission_priority").value_or(THREAD_PRIORITY_TIME_CRITICAL));
            bool begun{};
            bool started{};
            struct NativeTiming {
                bool enabled{};
                double waitEnter{}, waitReturn{}, predictionBefore{}, predictionAfter{};
                double beginLeaseEnter{}, beginLeaseAcquired{}, beginEnter{}, beginReturn{};
                int waitResult{}, beginResult{};
                unsigned waitAttempts{};
            } timing;
            const auto timingNow = [](bool enabled) { return enabled ? ovr_GetTimeInSeconds() : 0.; };
            const auto beginNative = [&] {
                CHECK_MSG(nativeId != LLONG_MAX, "Frame-generation native ID exhausted");
                timing = {};
                timing.enabled = IsTraceEnabled();
                if (timing.enabled) {
                    timing.waitEnter = ovr_GetTimeInSeconds();
                    timing.predictionBefore = ovr_GetPredictedDisplayTime(m_ovrSession, nativeId);
                }
                // Timing wait is the SDK's main-thread operation; only the
                // render-thread Begin/End phases lease the immediate context.
                while (true) {
                    {
                        std::unique_lock lock(m_asyncSubmissionMutex);
                        if (m_terminateAsyncThread) {
                            return false;
                        }
                    }
                    const auto result = waitToBeginSubmissionFrame(m_ovrSession, nativeId);
                    if (timing.enabled) {
                        timing.waitReturn = ovr_GetTimeInSeconds();
                        timing.waitResult = static_cast<int>(result);
                        ++timing.waitAttempts;
                    }
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
                    // Do not repeatedly reacquire the context between native
                    // frames while an admitted producer is already waiting.
                    // Its grant ends at acquisition, so a later GPU-fence
                    // yield still permits this group to finish and retire.
                    m_asyncSubmissionCondVar.wait(lock, [&] {
                        return m_deliveryTransport || !m_fgProducerContextWaiting || m_terminateAsyncThread ||
                               m_asyncSubmissionError;
                    });
                    if (m_asyncSubmissionError) {
                        std::rethrow_exception(m_asyncSubmissionError);
                    }
                    if (m_terminateAsyncThread) {
                        return false;
                    }
                }
                if (timing.enabled) {
                    timing.predictionAfter = ovr_GetPredictedDisplayTime(m_ovrSession, nativeId);
                    timing.beginLeaseEnter = ovr_GetTimeInSeconds();
                }
                // Independent delivery keeps the SDK on its presentation
                // context. Its blocking Begin must not lease the producer's
                // unrelated context; the SDK rendering gate still applies.
                std::unique_lock contextLease(m_fgContextMutex, std::defer_lock);
                if (!m_deliveryTransport) {
                    contextLease.lock();
                }
                timing.beginLeaseAcquired = timing.beginEnter = timingNow(timing.enabled);
                const auto beginResult = beginSubmissionFrame(m_ovrSession, nativeId);
                timing.beginReturn = timingNow(timing.enabled);
                timing.beginResult = static_cast<int>(beginResult);
                CHECK_OVRCMD(beginResult);
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
                    ownedDelivery = std::move(m_deliveryForSubmission);
                    m_deliveryForSubmission = {};
                    transaction = std::move(m_fgTransactionForSubmission);
                    m_fgTransactionForSubmission = {};
                    reservation = m_fgReservationForSubmission;
                    m_fgActiveSubmission = true;
                    m_fgActiveOwnedPixels = transaction.ticket.token != 0;
                    m_fgWorkerGroupNativeId =
                        static_cast<uint64_t>(nativeId) + std::clamp(reservation.multiplier, 1u, 4u);
                    // The mailbox is empty, but a producer must not enqueue
                    // next-frame GPU waits ahead of this group's SDK copies.
                    m_asyncSubmissionReady = false;
                    m_asyncSubmissionCondVar.notify_all();
                }
                const bool hasGpuOutput = transaction.ticket.token != 0;
                const bool traceGroup = IsTraceEnabled();
                std::array<double, 4> readyEnter{}, readyReturn{}, copyQueued{}, copyObserved{};
                std::array<int, 4> preparedImages{{-1, -1, -1, -1}};
                const auto presentationRole = [&](FrameGenerationRole role) -> SwapchainSlice& {
                    const auto bank = role == FrameGenerationRole::real ? transaction.ticket.bank : 0u;
                    return m_fgPresentationBanks[bank][static_cast<unsigned>(role)];
                };
                const auto prepareRole = [&](FrameGenerationRole role) {
                    const unsigned index = static_cast<unsigned>(role);
                    auto& output = presentationRole(role);
                    auto& image = preparedImages[index];
                    CHECK_OVRCMD(submissionSwapchainIndex(m_ovrSession, output.ovrSwapchain, &image));
                    CHECK_MSG(image >= 0 && static_cast<size_t>(image) < output.images.size(),
                              "Invalid FG compositor image");
                    CHECK_HRCMD(m_frameGeneration->copyOutput(transaction, role, output.images[image].Get()));
                    copyQueued[index] = timingNow(traceGroup);
                };
                if (hasGpuOutput) {
                    std::unique_lock contextLease(m_fgContextMutex);
                    // Stage finalized B before any synthesis wait. A missed
                    // synthetic deadline can now use the same 100% NR pixels.
                    if (m_deliveryTransport && ownedLayers[0].Header.Type == ovrLayerType_EyeFovDepth)
                        m_deliveryTransport->copy(m_fgDepthDelivery[transaction.ticket.bank]);
                    prepareRole(FrameGenerationRole::real);
                }
                if (!hasGpuOutput && m_deliveryTransport) {
                    std::unique_lock contextLease(m_fgContextMutex);
                    m_deliveryTransport->execute(ownedDelivery);
                }
                bool generated = transaction.ticket.token && !transaction.ticket.prime && reservation.valid &&
                                 reservation.nativeId == static_cast<uint64_t>(nativeId);
                if (!reservation.valid || reservation.nativeId != static_cast<uint64_t>(nativeId)) {
                    m_fgNeedReprime = m_fgTimelineReanchor = true;
                }
                const unsigned cycles =
                    reservation.multiplier >= 1 && reservation.multiplier <= 4 ? reservation.multiplier : 1;
                const bool recoverExpiredSlots = generated && cycles == 4 && !begun;
                unsigned firstUsefulSlot{};
                const auto awaitRole = [&](FrameGenerationRole role, int64_t target) {
                    const auto deadline = xrTimeToOvrTime(target);
                    const auto realDeadline = xrTimeToOvrTime(transaction.slots.targets[transaction.slots.count - 1]);
                    const auto index = static_cast<unsigned>(role);
                    readyEnter[index] = timingNow(traceGroup);
                    const auto finish = [&](bool result) {
                        readyReturn[index] = timingNow(traceGroup);
                        return result;
                    };
                    while (true) {
                        {
                            std::unique_lock lock(m_asyncSubmissionMutex);
                            if (m_terminateAsyncThread) {
                                return finish(false);
                            }
                        }
                        HANDLE completionEvent{};
                        HRESULT result;
                        {
                            std::unique_lock contextLease(m_fgContextMutex);
                            const auto now = ovr_GetTimeInSeconds();
                            // A ready S can still preserve this group's motion
                            // before B is due. Past its own slot, only observe
                            // readiness: never register or wait for pending work.
                            if (now >= realDeadline) {
                                return finish(false);
                            }
                            result = m_frameGeneration->prepareOutputReadyWait(
                                transaction, completionEvent, role, now < deadline);
                        }
                        if (result != VDXRFG_E_BUSY) {
                            CHECK_HRCMD(result);
                            return finish(ovr_GetTimeInSeconds() < realDeadline);
                        }
                        if (!completionEvent || ovr_GetTimeInSeconds() >= deadline) {
                            return finish(false);
                        }
                        // GPU completion wakes us immediately. Bound timeout
                        // retries for shutdown; neither context mutex is held.
                        const auto waited = WaitForSingleObject(completionEvent, 1);
                        if (waited == WAIT_FAILED) {
                            CHECK_HRCMD(HRESULT_FROM_WIN32(GetLastError()));
                        }
                        CHECK_MSG(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT, "FG ready event wait failed");
                    }
                };
                bool retainedMissNoted{};
                const auto noteRetainedPresentationMiss = [&] {
                    if (!retainedMissNoted) {
                        noteFrameGenerationPresentationMiss();
                        retainedMissNoted = true;
                    }
                };
                if (generated && !recoverExpiredSlots) {
                    generated = awaitRole(FrameGenerationRole::synthetic1, transaction.slots.targets[0]);
                    if (!generated) {
                        noteFrameGenerationPresentationMiss();
                    }
                }
                double leaseEnter = timingNow(traceGroup);
                std::unique_lock contextLease(m_fgContextMutex);
                double leaseAcquired = timingNow(traceGroup);
                if (generated && !recoverExpiredSlots) {
                    // Only S1 may be complete. Do not queue an unresolved tail
                    // dependency ahead of its native handoff or independent B.
                    const unsigned copies = m_frameGeneration->ready(transaction) ? cycles - 1 : 1;
                    for (unsigned slot = 0; slot < copies; ++slot) {
                        prepareRole(transaction.slots.roles[slot]);
                    }
                }
                bool admissionOpened{};
                const auto openAdmission = [&] {
                    if (admissionOpened)
                        return;
                    bool currentCopiesQueued = true;
                    if (generated)
                        for (unsigned slot = firstUsefulSlot; slot < cycles - 1; ++slot)
                            currentCopiesQueued &=
                                preparedImages[static_cast<unsigned>(transaction.slots.roles[slot])] >= 0;
                    const bool ready = !hasGpuOutput || (!m_fgNeedReprime && currentCopiesQueued &&
                                                         m_frameGeneration->ready(transaction));
                    if (ready) {
                        std::unique_lock lock(m_asyncSubmissionMutex);
                        m_asyncSubmissionReady = true;
                        admissionOpened = true;
                        m_asyncSubmissionCondVar.notify_all();
                    }
                };
                bool stopping{};
                if (m_deliveryTransport) {
                    // The current copies precede any next-producer work.
                    // Admit that work before the SDK timing/Begin calls when
                    // the existing full-output ownership predicate permits it.
                    openAdmission();
                }
                if (!begun && !recoverExpiredSlots) {
                    // Prepare subsequent groups before the SDK's timing wait.
                    // Waiting first leaves only its short render lead for
                    // cross-API destination copies and completion polling.
                    contextLease.unlock();
                    stopping = !beginNative();
                    leaseEnter = timingNow(traceGroup);
                }
                // Acquire the producer lease below only if presentation still
                // needs mutable FG state or pending-copy settlement.
                bool committedReal{};
                ovrLayer_Union realLayer{};
                for (unsigned slot = 0; slot < cycles && !stopping; ++slot) {
                    if (recoverExpiredSlots && generated && !begun && slot < cycles - 1) {
                        while (slot < cycles - 1) {
                            // A slightly late ready image is still useful until
                            // the next original slot. Never bunch up obsolete
                            // images or relabel S3 with S1's target/fraction.
                            const auto now = ovrTimeToXrTime(ovr_GetTimeInSeconds());
                            while (slot < cycles - 1 && now >= transaction.slots.targets[slot + 1]) {
                                ++slot;
                                noteRetainedPresentationMiss();
                            }
                            firstUsefulSlot = slot;
                            if (slot == cycles - 1)
                                break;
                            const auto role = transaction.slots.roles[slot];
                            if (preparedImages[static_cast<unsigned>(role)] < 0) {
                                if (contextLease.owns_lock())
                                    contextLease.unlock();
                                if (!awaitRole(role, transaction.slots.targets[slot])) {
                                    ++slot;
                                    firstUsefulSlot = slot;
                                    noteRetainedPresentationMiss();
                                    continue;
                                }
                                leaseEnter = timingNow(traceGroup);
                                contextLease.lock();
                                leaseAcquired = timingNow(traceGroup);
                                const unsigned copies = m_frameGeneration->ready(transaction) ? cycles - 1 : slot + 1;
                                for (unsigned tail = slot; tail < copies; ++tail)
                                    if (preparedImages[static_cast<unsigned>(transaction.slots.roles[tail])] < 0)
                                        prepareRole(transaction.slots.roles[tail]);
                                // Acquiring the producer context or preparing
                                // copies can consume a slot. Reselect before
                                // beginning any native cycle, never afterwards.
                                continue;
                            }
                            // A queued destination copy is not a ready image.
                            // If its own deadline has passed, try the next role
                            // before opening a native cycle. Add no copy wait.
                            if (now >= transaction.slots.targets[slot] &&
                                !m_frameGeneration->copyReady(transaction, role)) {
                                ++slot;
                                firstUsefulSlot = slot;
                                noteRetainedPresentationMiss();
                                continue;
                            }
                            break;
                        }
                        if (slot == cycles - 1) {
                            generated = false;
                            noteRetainedPresentationMiss();
                        }
                        nativeId = static_cast<long long>(reservation.nativeId + slot);
                        if (m_deliveryTransport)
                            openAdmission();
                        if (contextLease.owns_lock())
                            contextLease.unlock();
                    }
                    if (!recoverExpiredSlots && generated && slot && slot < cycles - 1 &&
                        preparedImages[static_cast<unsigned>(transaction.slots.roles[slot])] < 0) {
                        // Prepare the tail as soon as it exists, before the
                        // next native timing wait. Admission stays closed until
                        // every current copy is ahead of the next NR/app Wait.
                        if (contextLease.owns_lock())
                            contextLease.unlock();
                        const bool outputReady =
                            awaitRole(transaction.slots.roles[slot], transaction.slots.targets[slot]);
                        contextLease.lock();
                        if (outputReady) {
                            for (unsigned tail = slot; tail < cycles - 1; ++tail)
                                prepareRole(transaction.slots.roles[tail]);
                            openAdmission();
                        } else {
                            generated = false;
                            noteFrameGenerationPresentationMiss();
                        }
                        contextLease.unlock();
                    }
                    if (!begun && !beginNative()) {
                        break;
                    }
                    // B was queued first, then the synthetic roles in order.
                    // The last prepared role's completion covers every current
                    // image. Those images and this ticket remain worker-owned
                    // until retirement, independently of the next producer.
                    const auto lastCopyRole =
                        generated && cycles > 1 ? transaction.slots.roles[cycles - 2] : FrameGenerationRole::real;
                    const bool independentPresentation = hasGpuOutput && m_deliveryTransport && admissionOpened &&
                                                         m_frameGeneration->copyReady(transaction, lastCopyRole);
                    if (independentPresentation) {
                        if (contextLease.owns_lock()) {
                            contextLease.unlock();
                        }
                        leaseEnter = leaseAcquired = 0.; // No producer lease was needed.
                    } else if (!contextLease.owns_lock()) {
                        leaseEnter = timingNow(traceGroup);
                        contextLease.lock();
                        leaseAcquired = timingNow(traceGroup);
                    }
                    std::array<const ovrLayerHeader*, ovrMaxLayerCount> pointers{};
                    unsigned layerCount{};
                    ovrLayer_Union layer{};
                    FrameGenerationRole selectedRole = FrameGenerationRole::real;
                    double copyWaitEnter{}, copyWaitReturn{}, commitEnter{}, commitReturn{};
                    int commitResult{};
                    if (hasGpuOutput) {
                        auto role = generated ? transaction.slots.roles[slot] : FrameGenerationRole::real;
                        // A missed later slot is real-only; never retime S2 with
                        // its original fraction or replay an already sent S1.
                        if (generated && slot < cycles - 1) {
                            copyWaitEnter = timingNow(traceGroup);
                            const auto deadline = xrTimeToOvrTime(transaction.slots.targets[slot]);
                            bool copyIsReady =
                                independentPresentation || m_frameGeneration->copyReady(transaction, role);
                            while (!copyIsReady && ovr_GetTimeInSeconds() < deadline) {
                                const auto remainingMs = (deadline - ovr_GetTimeInSeconds()) * 1000;
                                const auto timeoutMs = static_cast<DWORD>(std::clamp(std::ceil(remainingMs), 1., 5.));
                                const auto result = m_frameGeneration->waitForCopy(transaction, role, timeoutMs);
                                if (result != VDXRFG_E_BUSY) {
                                    CHECK_HRCMD(result);
                                }
                                {
                                    std::unique_lock lock(m_asyncSubmissionMutex);
                                    if (m_terminateAsyncThread) {
                                        break;
                                    }
                                    if (result == S_OK) {
                                        copyIsReady = true;
                                        break;
                                    }
                                }
                                copyIsReady = m_frameGeneration->copyReady(transaction, role);
                            }
                            const bool copyCompleted = copyIsReady || m_frameGeneration->copyReady(transaction, role);
                            const auto selectionTime = copyCompleted ? ovrTimeToXrTime(ovr_GetTimeInSeconds()) : 0;
                            const bool lateCopy = copyCompleted && selectionTime > transaction.slots.targets[slot];
                            const bool retainLateCopy = selectionTime < transaction.slots.targets[cycles - 1];
                            if (!copyCompleted || (lateCopy && !retainLateCopy)) {
                                generated = false;
                                role = FrameGenerationRole::real;
                                noteFrameGenerationPresentationMiss();
                            } else {
                                // Use authoritative completion from the first
                                // probe, an allowed wait, or the final probe.
                                // Keep the fraction and order until B is due;
                                // then prefer fresh B over stale interpolation.
                                if (lateCopy)
                                    noteRetainedPresentationMiss();
                                copyObserved[static_cast<unsigned>(role)] = timingNow(traceGroup);
                            }
                            copyWaitReturn = timingNow(traceGroup);
                        }
                        selectedRole = role;
                        const unsigned index = static_cast<unsigned>(role);
                        if (role == FrameGenerationRole::real && committedReal) {
                            layer = realLayer;
                        } else {
                            auto& output = presentationRole(role);
                            const int image = preparedImages[index];
                            CHECK_MSG(image >= 0, "Unprepared FG compositor image");
                            commitEnter = timingNow(traceGroup);
                            const auto committed = commitSubmissionSwapchain(m_ovrSession, output.ovrSwapchain);
                            commitReturn = timingNow(traceGroup);
                            commitResult = static_cast<int>(committed);
                            CHECK_OVRCMD(committed);
                            output.lastCommittedIndex = image;
                            ovrTextureSwapChain depth{};
                            if (role == FrameGenerationRole::real &&
                                ownedLayers[0].Header.Type == ovrLayerType_EyeFovDepth) {
                                auto& bank = m_fgDepthPresentationBanks[transaction.ticket.bank];
                                CHECK_MSG(bank.token == transaction.ticket.token && bank.preparedImage >= 0 &&
                                              static_cast<size_t>(bank.preparedImage) < bank.output.images.size(),
                                          "Unprepared FG real depth image");
                                // A partial pair commit must fail with both
                                // owners retained, never submit mismatched B.
                                if (m_deliveryTransport)
                                    m_deliveryTransport->commitCopied(m_fgDepthDelivery[transaction.ticket.bank]);
                                else
                                    CHECK_OVRCMD(commitSubmissionSwapchain(m_ovrSession, bank.output.ovrSwapchain));
                                bank.output.lastCommittedIndex = bank.preparedImage;
                                depth = bank.output.ovrSwapchain;
                            }
                            layer = frameGenerationPresentationLayer(
                                ownedLayers[0], role, output.ovrSwapchain, depth, m_fgEyeResolution);
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
                    if (!slot || recoverExpiredSlots) {
                        openAdmission();
                    }
                    const auto endEnter = timingNow(timing.enabled);
                    const auto result = endSubmissionFrame(m_ovrSession, nativeId, &scale, pointers.data(), layerCount);
                    const auto endReturn = timingNow(timing.enabled);
                    TraceLoggingWrite(log::g_traceProvider,
                                      "FG_Present",
                                      TLArg(nativeId, "NativeId"),
                                      TLArg(transaction.ticket.current_serial, "Serial"),
                                      TLArg(slot, "Slot"),
                                      TLArg(cycles, "Slots"),
                                      TLArg(generated && slot < cycles - 1, "Synthetic"),
                                      TLArg(!generated && slot < cycles - 1, "PaddingRepeat"),
                                      TLArg(static_cast<int>(result), "Result"));
                    if (timing.enabled && traceGroup) {
                        const auto role = static_cast<unsigned>(selectedRole);
                        const auto requestedRole =
                            slot < transaction.slots.count ? static_cast<unsigned>(transaction.slots.roles[slot]) : 2u;
                        const int64_t target =
                            reservation.valid ? reservation.firstTarget + slot * reservation.period : 0;
                        const float fraction =
                            selectedRole == FrameGenerationRole::real ? 1.f : transaction.slots.fractions[slot];
                        TraceLoggingWrite(log::g_traceProvider,
                                          "FG_NativeTiming",
                                          TLArg(nativeId, "NativeId"),
                                          TLArg(reservation.nativeId, "ReservedNativeId"),
                                          TLArg(transaction.ticket.current_serial, "Serial"),
                                          TLArg(slot, "Slot"),
                                          TLArg(cycles, "Slots"),
                                          TLArg(role, "Role"),
                                          TLArg(requestedRole, "RequestedRole"),
                                          TLArg(fraction, "Fraction"),
                                          TLArg(target, "ReservedPhotonTarget"),
                                          TLArg(reservation.period, "NativePeriod"),
                                          TLArg(timing.waitEnter, "WaitEnter"),
                                          TLArg(timing.waitReturn, "WaitReturn"),
                                          TLArg(timing.waitResult, "WaitResult"),
                                          TLArg(timing.waitAttempts, "WaitAttempts"),
                                          TLArg(timing.predictionBefore, "PredictionBeforeWait"),
                                          TLArg(timing.predictionAfter, "PredictionAfterWait"),
                                          TLArg(timing.beginLeaseEnter, "BeginLeaseEnter"),
                                          TLArg(timing.beginLeaseAcquired, "BeginLeaseAcquired"),
                                          TLArg(timing.beginEnter, "BeginEnter"),
                                          TLArg(timing.beginReturn, "BeginReturn"),
                                          TLArg(timing.beginResult, "BeginResult"),
                                          TLArg(leaseEnter, "PresentLeaseEnter"),
                                          TLArg(leaseAcquired, "PresentLeaseAcquired"),
                                          TLArg(independentPresentation, "IndependentPresentation"),
                                          TLArg(readyEnter[role], "ReadyWaitEnter"),
                                          TLArg(readyReturn[role], "ReadyWaitReturn"),
                                          TLArg(readyEnter[requestedRole], "RequestedReadyWaitEnter"),
                                          TLArg(readyReturn[requestedRole], "RequestedReadyWaitReturn"),
                                          TLArg(copyQueued[role], "CopyQueued"),
                                          TLArg(copyObserved[role], "CopyObservedComplete"),
                                          TLArg(copyWaitEnter, "CopyWaitEnter"),
                                          TLArg(copyWaitReturn, "CopyWaitReturn"),
                                          TLArg(commitEnter, "CommitEnter"),
                                          TLArg(commitReturn, "CommitReturn"),
                                          TLArg(commitResult, "CommitResult"),
                                          TLArg(endEnter, "EndEnter"),
                                          TLArg(endReturn, "EndReturn"),
                                          TLArg(static_cast<int>(result), "EndResult"));
                    }
                    // An attempted cycle is never reused, including End failure.
                    ++nativeId;
                    begun = false;
                    CHECK_OVRCMD(result);
                    // Output can finish during End, including repeated B.
                    // Reopen under the same ownership predicate before another
                    // blocking Begin can strand the next producer until retire.
                    openAdmission();
                    if (contextLease.owns_lock()) {
                        contextLease.unlock();
                    }
                }
                if (contextLease.owns_lock()) {
                    contextLease.unlock();
                }
                if (transaction.ticket.token) {
                    const auto deadline = std::chrono::steady_clock::now() + 10s;
                    const auto retirementSerial = transaction.ticket.current_serial;
                    HRESULT result;
                    do {
                        HANDLE completionEvent{};
                        HRESULT waitResult = S_OK;
                        {
                            std::unique_lock retirementLease(m_fgContextMutex);
                            result = m_frameGeneration->retire(transaction);
                            if (result == VDXRFG_E_BUSY) {
                                waitResult = m_frameGeneration->prepareRetirementWait(transaction, completionEvent);
                                if (waitResult == S_OK) {
                                    // Completion can race the first retirement
                                    // attempt. Retry immediately, before treating
                                    // BUSY as an opaque module/private hold.
                                    result = m_frameGeneration->retire(transaction);
                                }
                            }
                        }
                        if (result != VDXRFG_E_BUSY) {
                            break;
                        }
                        const auto waitEnter = timingNow(traceGroup);
                        if (waitResult == VDXRFG_E_BUSY) {
                            CHECK_MSG(completionEvent, "Missing FG retirement completion event");
                            while (true) {
                                const auto waited = WaitForSingleObject(completionEvent, 5);
                                CHECK_MSG(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT,
                                          "FG retirement event wait failed");
                                if (waited == WAIT_OBJECT_0) {
                                    break;
                                }
                                // Timeout observes only cancellation/deadline.
                                // Recheck GPU readiness after an actual wake.
                                std::unique_lock lock(m_asyncSubmissionMutex);
                                CHECK_MSG(!m_terminateAsyncThread, "FG retirement completion is stopping");
                                CHECK_MSG(std::chrono::steady_clock::now() < deadline,
                                          "FG retirement completion timed out");
                            }
                        } else {
                            CHECK_HRCMD(waitResult);
                            // An optional module/private observer can retain a
                            // ticket after both public fences are complete. It
                            // exposes no completion fence; keep its bounded,
                            // cancellation-aware compatibility retry.
                            std::unique_lock lock(m_asyncSubmissionMutex);
                            m_asyncSubmissionCondVar.wait_for(lock, 1ms, [&] { return m_terminateAsyncThread; });
                            CHECK_MSG(!m_terminateAsyncThread, "FG private retirement is stopping");
                        }
                        if (traceGroup) {
                            TraceLoggingWrite(g_traceProvider,
                                              "FG_RetirementWait",
                                              TLArg(retirementSerial, "Serial"),
                                              TLArg(waitResult == VDXRFG_E_BUSY, "GpuCompletion"),
                                              TLArg(waitEnter, "WaitEnter"),
                                              TLArg(timingNow(true), "WaitReturn"));
                        }
                    } while (std::chrono::steady_clock::now() < deadline);
                    CHECK_HRCMD(result);
                }
                ownedLayers.clear();
                {
                    std::unique_lock lock(m_asyncSubmissionMutex);
                    m_fgActiveSubmission = m_fgActiveOwnedPixels = false;
                    if (m_fgProducerFenceEvent)
                        CHECK_MSG(SetEvent(m_fgProducerFenceEvent.get()), "FG producer retirement wake failed");
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

    void OpenXrRuntime::noteFrameGenerationPresentationMiss() noexcept {
        // Presentation lateness does not invalidate captured source pixels.
        // Keep native recovery separate; the next pair must still pass all
        // epoch, projection, appearance and actual-endpoint cadence checks.
        m_fgTimelineReanchor = true;
    }

    void OpenXrRuntime::lockFrameGenerationProducerContext(std::unique_lock<std::mutex>& contextLease) {
        {
            std::unique_lock lock(m_asyncSubmissionMutex);
            m_fgProducerContextWaiting = true;
        }
        // No state lock may span context acquisition. Always withdraw intent,
        // including a failed acquisition or restoration during unwinding.
        auto clearRequest = MakeScopeGuard([&] {
            std::unique_lock lock(m_asyncSubmissionMutex);
            m_fgProducerContextWaiting = false;
            m_asyncSubmissionCondVar.notify_all();
        });
        contextLease.lock();
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
