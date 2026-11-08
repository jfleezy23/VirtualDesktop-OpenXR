// MIT License
// Copyright (c) 2026 VirtualDesktop-OpenXR contributors.

#include "pch.h"
#include "PairCaptureNrEngine.h"
#include "PairCaptureNrWarmLayouts.h"
#include "runtime.h"
#include "PairCaptureNrSdk.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrReturnGuard.h"
#include "PairCaptureNrSources.h"
#include "log.h"

namespace virtualdesktop_openxr {

    bool OpenXrRuntime::upliftSpatialLayer(const XrSwapchainSubImage** views,
                                           const XrSwapchainSubImage** depth,
                                           const bool* depthInverted,
                                           ovrLayerEyeFov& layer,
                                           bool finalColour,
                                           std::unique_lock<std::mutex>* producerLease) {
        using namespace log;
        const auto& settings = m_precompositor.dlssnrSettings;
        const auto fallback = [&](int reason, const char* description) {
            if (m_nrSpatialStatus != reason) {
                Log("NR processing resolution: native fallback (%s)", description);
                m_nrSpatialStatus = reason;
            }
            return false;
        };
        nr_spatial::FrameInput input;
        input.processingPercent = settings.processingResolution;
        const ovrSizei resolution{std::max(views[0]->imageRect.extent.width, views[1]->imageRect.extent.width),
                                  std::max(views[0]->imageRect.extent.height, views[1]->imageRect.extent.height)};
        auto& firstSwapchain = *reinterpret_cast<Swapchain*>(views[0]->swapchain);
        const auto sourceFormat = ovrToDxgiTextureFormat(firstSwapchain.ovrDesc.Format);
        input.outputFormat = nr_spatial::OutputColorFormat(sourceFormat, m_frameGenerationMode != 0);
        const auto format = dxgiToOvrTextureFormat(input.outputFormat);
        input.outputEyeStride = resolution.w;
        if (!nr_spatial::IsSupportedColorFormat(sourceFormat)) {
            return fallback(2, "source format retains the qualified native NR path");
        }
        const bool pairedDepth = depth[0] && depth[1];
        std::array<ovrSizei, 2> working{};
        std::array<ovrTextureSwapChain, 4> sourceChains{};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            auto& swapchain = *reinterpret_cast<Swapchain*>(views[eye]->swapchain);
            auto& source = swapchain.resolvedSlices[views[eye]->imageArrayIndex];
            sourceChains[eye * 2] = source.ovrSwapchain;
            const auto& rect = views[eye]->imageRect;
            auto& target = input.eyes[eye];
            target.colorFormat = swapchain.dxgiFormatForSubmission;
            if (target.colorFormat != sourceFormat || rect.offset.x < 0 || rect.offset.y < 0 ||
                rect.extent.width <= 0 || rect.extent.height <= 0 || source.lastCommittedIndex < 0 ||
                static_cast<size_t>(source.lastCommittedIndex) >= source.dlssnrImages.size()) {
                return fallback(3, "unsupported color view or unavailable input generation");
            }
            target.color = source.dlssnrImages[source.lastCommittedIndex].Get();
            target.colorRect = {static_cast<uint32_t>(rect.offset.x),
                                static_cast<uint32_t>(rect.offset.y),
                                static_cast<uint32_t>(rect.extent.width),
                                static_cast<uint32_t>(rect.extent.height)};
            nr_spatial::Extent extent{};
            const nr_spatial::Extent native{target.colorRect.width, target.colorRect.height};
            target.processingRect = nr_spatial::CoverageRect(native, settings.foveationSize);
            target.transitionWidth = nr_spatial::TransitionWidth(native, target.processingRect, settings.foveationSize);
            if (!nr_spatial::TryWorkingExtent(
                    {target.processingRect.width, target.processingRect.height}, input.processingPercent, extent)) {
                return fallback(4, "working grid is too small or has no effective reduction");
            }
            working[eye] = {static_cast<int>(extent.width), static_cast<int>(extent.height)};
            if (pairedDepth) {
                auto& depthSwapchain = *reinterpret_cast<Swapchain*>(depth[eye]->swapchain);
                auto& depthSource = depthSwapchain.resolvedSlices[depth[eye]->imageArrayIndex];
                sourceChains[eye * 2 + 1] = depthSource.ovrSwapchain;
                const auto& depthRect = depth[eye]->imageRect;
                if (depthRect.offset.x < 0 || depthRect.offset.y < 0 || depthRect.extent.width <= 0 ||
                    depthRect.extent.height <= 0 || depthSource.lastCommittedIndex < 0 ||
                    static_cast<size_t>(depthSource.lastCommittedIndex) >= depthSource.dlssnrImages.size()) {
                    return fallback(5, "unavailable paired depth generation");
                }
                target.depth = depthSource.dlssnrImages[depthSource.lastCommittedIndex].Get();
                target.depthFormat = getShaderResourceViewFormat(depthSwapchain.dxgiFormatForSubmission);
                target.depthRect = {static_cast<uint32_t>(depthRect.offset.x),
                                    static_cast<uint32_t>(depthRect.offset.y),
                                    static_cast<uint32_t>(depthRect.extent.width),
                                    static_cast<uint32_t>(depthRect.extent.height)};
            }
        }

        if (!m_dlssnrOutputSwapchain || m_dlssnrOutputSwapchainResolution.w < resolution.w ||
            m_dlssnrOutputSwapchainResolution.h < resolution.h || m_dlssnrOutputSwapchainFormat != format) {
            flushSubmissionContext();
            m_dlssnrContext->Flush();
            if (m_nrSpatialPipeline) {
                m_nrSpatialPipeline->DiscardRecording();
                m_nrSpatialPipeline->Clear();
            }
            ensureDlssnrSwapchainResources(format, resolution);
        }
        int imageIndex{};
        CHECK_OVRCMD(submissionSwapchainIndex(m_ovrSession, m_dlssnrOutputSwapchain, &imageIndex));
        input.output = m_dlssnrOutputSwapchainImages.at(imageIndex).Get();
        if (!m_nrSpatialPipeline) {
            try {
                m_nrSpatialPipeline =
                    std::make_unique<nr_spatial::Pipeline>(m_dlssnrDevice.Get(), m_dlssnrContext->GetCompletionFence());
            } catch (const std::bad_alloc&) {
                return fallback(6, "spatial state allocation failed");
            }
        }
        const bool changed = !m_nrSpatialPipeline->Matches(input);
        bool featureChanged = !m_nrSpatialFeatures || m_nrSpatialNeedsFeatureReset;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            featureChanged |= m_nrSpatialFeatureExtents[eye].w != working[eye].w ||
                              m_nrSpatialFeatureExtents[eye].h != working[eye].h;
        }
        if (changed || featureChanged || m_nrSpatialNeedsDrain) {
            // Submit may have executed before its signal failed. Flush proves the
            // recovery boundary before descriptors, features or scratch can retire.
            m_dlssnrContext->Flush();
            m_nrSpatialPipeline->DiscardRecording();
            releaseDlssnrFeatures();
            if (changed) {
                m_nrSpatialPipeline->Clear();
            }
            m_nrSpatialNeedsDrain = false;
            m_nrSpatialNeedsFeatureReset = false;
        }
        const auto prepared = m_nrSpatialPipeline->Prepare(input);
        if (prepared == DXGI_ERROR_DEVICE_REMOVED || prepared == DXGI_ERROR_DEVICE_RESET) {
            CHECK_HRCMD(prepared);
        }
        if (FAILED(prepared)) {
            return fallback(7, "spatial resources or views could not be prepared");
        }

        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "DLSSNRSpatial");
        const bool reset = changed || featureChanged || sourceChains != m_nrSpatialSourceChains ||
                           m_nrSpatialLastFrame == UINT64_MAX || m_frameBegun != m_nrSpatialLastFrame + 1;
        bool submitAttempted{}, registered{};
        nr_capture::NrWarmSubmissionFacts nrWarmSubmission{};
        uint64_t nrSubmitBegin{};
        auto& nrEngine = nr_capture::captureEngine();
        const auto nrFrame = nrEngine.selectedFrame(m_frameBegun);
        auto* nrReturnGuard = nr_capture::SourceReturnGuard::current();
        const auto nrCapture = [&](uint64_t role, uint64_t eye) {
            if (nrFrame < 2) {
                nr_capture::SnapshotRoleMetadata geometry{};
                geometry.hostSerial = m_frameBegun;
                geometry.geometryObserved = true;
                if (eye == 2) {
                    geometry.rectWidth = uint64_t(resolution.w) * 2;
                    geometry.rectHeight = resolution.h;
                    geometry.releasedIndex = static_cast<uint32_t>(imageIndex);
                } else {
                    const auto* sub = role == NR_MODEL_DEPTH ? depth[eye] : views[eye];
                    CHECK_MSG(sub, "private NR original geometry missing");
                    geometry.rectXbits = static_cast<uint32_t>(sub->imageRect.offset.x);
                    geometry.rectYbits = static_cast<uint32_t>(sub->imageRect.offset.y);
                    geometry.rectWidth = sub->imageRect.extent.width;
                    geometry.rectHeight = sub->imageRect.extent.height;
                    geometry.releasedIndex =
                        static_cast<uint32_t>(reinterpret_cast<Swapchain*>(sub->swapchain)->lastReleasedIndex);
                    if (role == NR_PROXY || role == NR_PROXY_DEPTH || role == NR_SPATIAL_ANSWER ||
                        role == NR_EXACT_PROXY) {
                        geometry.rectXbits = geometry.rectYbits = 0;
                        geometry.rectWidth = working[eye].w;
                        geometry.rectHeight = working[eye].h;
                    }
                    if (role == NR_MODEL_DEPTH || role == NR_PROXY_DEPTH) {
                        const auto& original = m_precompositor.originalFgDepth[eye];
                        const auto bits = [](float value) noexcept {
                            uint32_t b{};
                            std::memcpy(&b, &value, sizeof b);
                            return b;
                        };
                        geometry.minDepthBits = bits(original.minDepth);
                        geometry.maxDepthBits = bits(original.maxDepth);
                        geometry.nearZBits = bits(original.nearZ);
                        geometry.farZBits = bits(original.farZ);
                    }
                }
                // Resolved NR inputs and pipeline intermediates select slice zero;
                // original app array selection remains in raw role/source640.
                geometry.cropXbits = geometry.rectXbits;
                geometry.cropYbits = geometry.rectYbits;
                geometry.cropWidth = geometry.rectWidth;
                geometry.cropHeight = geometry.rectHeight;
                CHECK_MSG(nrReturnGuard && nrEngine.captureNrRole(nrFrame, role, eye, *nrReturnGuard, geometry),
                          "private NR phase snapshot could not be recorded");
            }
        };
        try {
            ++m_submissionFenceValue;
            const auto nrInputSignalBegin = nr_capture::sourceClock();
            const auto nrInputSignalResult =
                m_ovrSubmissionContext->Signal(m_ovrSubmissionCompletionFence.Get(), m_submissionFenceValue);
            const auto nrInputSignalEnd = nr_capture::sourceClock();
            if (auto* returnGuard = nr_capture::SourceReturnGuard::current())
                returnGuard->observeInputSignal(m_ovrSubmissionDevice.Get(),
                                                m_ovrSubmissionContext.Get(),
                                                m_ovrSubmissionCompletionFence.Get(),
                                                m_submissionFenceValue,
                                                nrInputSignalResult,
                                                nrInputSignalBegin,
                                                nrInputSignalEnd);
            CHECK_HRESULT(nrInputSignalResult, "original NR input producer Signal");
            // Raw app-to-S copies end at this existing input-ready value.
            // Protect application reuse without waiting for inference/output.
            if (m_deliveryTransport)
                orderApplicationAfterSourceReads(m_submissionFenceValue);
            const auto nrInputWaitBegin = nr_capture::sourceClock();
            const auto nrInputWaitQueue = m_dlssnrContext->GetCommandQueue();
            const auto nrInputWaitResult = nrInputWaitQueue->Wait(m_dlssnrInFence.Get(), m_submissionFenceValue);
            const auto nrInputWaitEnd = nr_capture::sourceClock();
            if (auto* returnGuard = nr_capture::SourceReturnGuard::current())
                returnGuard->observeInputWait(m_dlssnrDevice.Get(),
                                              m_dlssnrContext.get(),
                                              m_dlssnrContext->GetObservedLifetimeEpoch(),
                                              nrInputWaitQueue,
                                              m_dlssnrInFence.Get(),
                                              m_submissionFenceValue,
                                              nrInputWaitResult,
                                              nrInputWaitBegin,
                                              nrInputWaitEnd);
            CHECK_HRESULT(nrInputWaitResult, "original NR input queue Wait");
            auto commandList = m_dlssnrContext->GetCommandList();
            vdxrfg::pair_capture::nr::NrRecordingOwnership nrRecordingFacts{};
            if (m_dlssnrContext->BorrowRecordingOwnership(commandList, nrRecordingFacts))
                nr_capture::sdkRecording(m_frameBegun, m_dlssnrContext.get(), nrRecordingFacts);

            if (nrFrame < 2) {
                nr_capture::CaptureNrInput nrSelected{};
                nrSelected.recording = nrRecordingFacts;
                nrSelected.pipelineObject = m_nrSpatialPipeline.get();
                CHECK_MSG(m_nrSpatialPipeline && m_nrSpatialPipeline->BorrowActiveOwnership(nrSelected.pipeline) &&
                              m_nrSpatialPipeline->BorrowActivePrograms(nrSelected.programs),
                          "private NR actual pipeline borrow failed");
                CHECK_MSG(m_dlssnrContext->TryObserveNextSubmission(nrSelected.prospectiveContextLifetime,
                                                                    nrSelected.prospectiveSignalValue) &&
                              nrEngine.acquireNr(nrFrame, nrSelected),
                          "private NR late submission reservation or acquisition failed");
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    nrCapture(NR_MODEL_COLOUR, eye);
                    nrCapture(NR_MODEL_DEPTH, eye);
                }
            }
            m_nrSpatialPipeline->RecordInputs(commandList.Commands.Get());
            if (nrFrame < 2)
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    nrCapture(NR_PROXY, eye);
                    nrCapture(NR_PROXY_DEPTH, eye);
                }
            m_nrSpatialFeatures = true;
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const auto work = m_nrSpatialPipeline->GetWorkingEye(eye);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.DepthInverted", depthInverted[eye]);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.Enabled", true);
                createDlssnrFeature(eye, working[eye], commandList.Commands.Get());
                m_nrSpatialFeatureExtents[eye] = working[eye];
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.Color", work.proxy);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.Output", work.answer);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.ColorSubrectBaseX", 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.ColorSubrectBaseY", 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.ColorSubrectWidth", working[eye].w);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.ColorSubrectHeight", working[eye].h);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.OutputSubrectBaseX", 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.OutputSubrectBaseY", 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.OutputSubrectWidth", working[eye].w);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.OutputSubrectHeight", working[eye].h);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.Depth", work.depth);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.DepthSubrectBaseX", 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.DepthSubrectBaseY", 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.DepthSubrectWidth", work.depth ? working[eye].w : 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.DepthSubrectHeight", work.depth ? working[eye].h : 0);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.Reset", reset);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.UseAutoMask", true);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.Style", settings.style);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.Intensity", settings.intensity);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.LocalToneStrength", settings.localToneStrength);
                nr_capture::ObserveSet(
                    m_ngxParameters, "DLSSNR.LocalStructureStrength", settings.localStructureStrength);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.SkinStructureStrength", settings.skinStructureStrength);
                nr_capture::ObserveSet(m_ngxParameters, "DLSSNR.UICorrection", 0);
                CHECK_NGXCMD(nr_capture::ObserveSdkCall(
                    NR_EVALUATE,
                    eye,
                    reinterpret_cast<uint64_t>(m_dlssnrFeatureEvaluate[eye]),
                    commandList.Commands.Get(),
                    m_ngxParameters,
                    &m_dlssnrFeature[eye],
                    reinterpret_cast<uint64_t>(m_dlssnrFeatureEvaluate[eye]),
                    reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                    [&] {
                        return m_dlssnrFeatureEvaluate[eye](
                            commandList.Commands.Get(), m_dlssnrFeature[eye], m_ngxParameters, nullptr);
                    }));
            }
            if (nrFrame < 2)
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    nrCapture(NR_SPATIAL_ANSWER, eye);
                    nrCapture(NR_POST_EVAL_COLOUR, eye);
                    nrCapture(NR_PRE_RESOLVE_COLOUR, eye);
                    nrCapture(NR_EXACT_PROXY, eye);
                }
            m_nrSpatialPipeline->RecordResolve(commandList.Commands.Get());
            nrCapture(NR_POST_RESOLVE, 2);
            vdxrfg::pair_capture::nr::NrPipelineOwnership nrWarmGraph{};
            const bool nrWarmInventory =
                nr_capture::hostRequest().valid && m_nrSpatialPipeline &&
                m_nrSpatialPipeline->BorrowActiveOwnership(nrWarmGraph) &&
                nrWarmGraph.observedEpochs == vdxrfg::pair_capture::nr::NrPipelineOwnership::allEpochs &&
                nrWarmGraph.device == nrRecordingFacts.device && nrWarmGraph.fence == nrRecordingFacts.fence &&
                (nrWarmGraph.scratch[0].proxy && nrWarmGraph.scratch[0].answer && nrWarmGraph.scratch[0].depth &&
                 nrWarmGraph.scratch[1].proxy && nrWarmGraph.scratch[1].answer && nrWarmGraph.scratch[1].depth);
            if (nrWarmInventory) {
                ID3D12Resource* noNativeDepth[2]{};
                nr_capture::warmLayoutGraph12(m_frameBegun,
                                              NR_SPATIAL85,
                                              nrRecordingFacts.facts.nrCommandContextLifetimeEpoch,
                                              nrWarmGraph,
                                              noNativeDepth);
            }
            auto* nrToken = nrFrame < 2 ? nrEngine.beginNrSubmission(nrFrame) : nullptr;
            CHECK_MSG(nrFrame >= 2 || nrToken, "private NR submission token unavailable");
            submitAttempted = true;
            nrSubmitBegin = nr_capture::sourceClock();
            const auto fence = m_dlssnrContext->SubmitCommandList(
                std::move(commandList), nrToken, nr_capture::hostRequest().valid ? &nrWarmSubmission : nullptr);
            const auto nrSubmitEnd = nr_capture::sourceClock();
            if (auto* returnGuard = nr_capture::SourceReturnGuard::current())
                returnGuard->observeSubmission(nrWarmSubmission, nrSubmitBegin, nrSubmitEnd);
            if (nrFrame < 2)
                CHECK_MSG(nrReturnGuard &&
                              nrEngine.finishNrSubmission(nrFrame, nrWarmSubmission, fence, *nrReturnGuard),
                          "private NR actual submission facts did not reconcile");
            m_nrSpatialPipeline->MarkSubmitted(fence);
            registered = true;
            // Future NR must not block the active group's SDK GPU handoffs.
            waitForFrameGenerationProducerFence(m_dlssnrOutFence.Get(), fence, producerLease, "NR-output");
            const auto nrOutputWaitBegin = nr_capture::sourceClock();
            const auto nrOutputWaitResult = m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fence);
            const auto nrOutputWaitEnd = nr_capture::sourceClock();
            if (nr_capture::hostRequest().valid) {
                nr_capture::SdkWarmFacts nrSdkWarm{};
                const vdxrfg::pair_capture::nr::NrSubmissionKey actualBorrow{
                    reinterpret_cast<uint64_t>(nrRecordingFacts.queue),
                    reinterpret_cast<uint64_t>(nrRecordingFacts.fence),
                    reinterpret_cast<uint64_t>(nrRecordingFacts.list),
                    reinterpret_cast<uint64_t>(nrRecordingFacts.allocator),
                    nrRecordingFacts.facts.nrRecordingEpoch};
                const bool completeSubmission =
                    nr_capture::warmSubmissionComplete(nrWarmSubmission,
                                                       reinterpret_cast<uint64_t>(m_dlssnrContext.get()),
                                                       nrRecordingFacts.facts.nrCommandContextLifetimeEpoch,
                                                       actualBorrow,
                                                       fence);
                if (auto* returnGuard = nr_capture::SourceReturnGuard::current()) {
                    returnGuard->observeNr(nrWarmSubmission, completeSubmission);
                    returnGuard->observeOutputWait(m_ovrSubmissionContext.Get(),
                                                   m_dlssnrOutFence.Get(),
                                                   fence,
                                                   nrOutputWaitResult,
                                                   nrOutputWaitBegin,
                                                   nrOutputWaitEnd);
                }
                nr_capture::sdkWarmFacts(nrSdkWarm);
                nr_capture::hostWarmFrame(m_frameBegun,
                                          nrRecordingFacts.facts.nrCommandContextLifetimeEpoch,
                                          nrSdkWarm,
                                          fence,
                                          completeSubmission ? nrWarmSubmission.calls.original.result : E_FAIL,
                                          nrOutputWaitResult,
                                          nrWarmInventory && completeSubmission);
            }
            CHECK_HRESULT(nrOutputWaitResult, "m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fence)");
        } catch (...) {
            if (nrFrame < 2 && nrReturnGuard && nrSubmitBegin)
                nrEngine.finishNrSubmission(nrFrame, nrWarmSubmission, 0, *nrReturnGuard);
            if (auto* returnGuard = nr_capture::SourceReturnGuard::current()) {
                if (nrSubmitBegin && !returnGuard->submissionEnd())
                    returnGuard->observeSubmission(nrWarmSubmission, nrSubmitBegin, nr_capture::sourceClock());
            }
            if (auto* returnGuard = nr_capture::SourceReturnGuard::current())
                if (!returnGuard->facts().nrSignalObserved)
                    returnGuard->observeNr(nrWarmSubmission, false);
            nr_capture::hostInvalidateWarm();
            m_nrSpatialNeedsFeatureReset = true;
            if (!submitAttempted) {
                m_nrSpatialPipeline->DiscardRecording();
            } else if (!registered) {
                // Keep every referenced object until a later successful recovery drain.
                m_nrSpatialNeedsDrain = true;
            }
            throw;
        }

        m_precompositor.pendingColourCommits[0] = m_dlssnrOutputSwapchain;
        m_dlssnrOutputSwapchainLastWrittenIndex = imageIndex;
        if (m_frameGenerationMode && finalColour) {
            m_precompositor.finalColour[0] = m_dlssnrOutputSwapchainTextures[imageIndex];
            m_precompositor.finalColour[1] = m_precompositor.finalColour[0];
        }
        for (uint32_t eye = 0; eye < 2; ++eye) {
            layer.ColorTexture[eye] = m_dlssnrOutputSwapchain;
            layer.Viewport[eye].Pos = {static_cast<int>(eye) * resolution.w, 0};
            layer.Viewport[eye].Size = {views[eye]->imageRect.extent.width, views[eye]->imageRect.extent.height};
        }
        if (changed || m_nrSpatialStatus != 100 + static_cast<int>(input.processingPercent)) {
            Log("NR processing resolution: %u%%, coverage %.0f%%; left %dx%d -> %dx%d, right %dx%d -> %dx%d",
                input.processingPercent,
                std::clamp(settings.foveationSize, 0.1f, 1.f) * 100.f,
                views[0]->imageRect.extent.width,
                views[0]->imageRect.extent.height,
                working[0].w,
                working[0].h,
                views[1]->imageRect.extent.width,
                views[1]->imageRect.extent.height,
                working[1].w,
                working[1].h);
            m_nrSpatialStatus = 100 + input.processingPercent;
        }
        m_nrSpatialLastFrame = m_frameBegun;
        m_nrSpatialSourceChains = sourceChains;
        TraceLoggingWriteStop(local, "DLSSNRSpatial");
        return true;
    }
} // namespace virtualdesktop_openxr
