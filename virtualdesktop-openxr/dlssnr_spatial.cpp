// MIT License
// Copyright (c) 2026 VirtualDesktop-OpenXR contributors.

#include "pch.h"
#include "runtime.h"
#include "log.h"

namespace virtualdesktop_openxr {

    bool OpenXrRuntime::upliftSpatialLayer(const XrSwapchainSubImage** views,
                                           const XrSwapchainSubImage** depth,
                                           const bool* depthInverted,
                                           ovrLayerEyeFov& layer,
                                           bool finalColour) {
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
        CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(m_ovrSession, m_dlssnrOutputSwapchain, &imageIndex));
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
        try {
            ++m_submissionFenceValue;
            CHECK_HRCMD(m_ovrSubmissionContext->Signal(m_ovrSubmissionCompletionFence.Get(), m_submissionFenceValue));
            CHECK_HRCMD(m_dlssnrContext->GetCommandQueue()->Wait(m_dlssnrInFence.Get(), m_submissionFenceValue));
            auto commandList = m_dlssnrContext->GetCommandList();
            m_nrSpatialPipeline->RecordInputs(commandList.Commands.Get());
            m_nrSpatialFeatures = true;
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const auto work = m_nrSpatialPipeline->GetWorkingEye(eye);
                m_ngxParameters->Set("DLSSNR.DepthInverted", depthInverted[eye]);
                m_ngxParameters->Set("DLSSNR.Enabled", true);
                createDlssnrFeature(eye, working[eye], commandList.Commands.Get());
                m_nrSpatialFeatureExtents[eye] = working[eye];
                m_ngxParameters->Set("DLSSNR.Color", work.proxy);
                m_ngxParameters->Set("DLSSNR.Output", work.answer);
                m_ngxParameters->Set("DLSSNR.ColorSubrectBaseX", 0);
                m_ngxParameters->Set("DLSSNR.ColorSubrectBaseY", 0);
                m_ngxParameters->Set("DLSSNR.ColorSubrectWidth", working[eye].w);
                m_ngxParameters->Set("DLSSNR.ColorSubrectHeight", working[eye].h);
                m_ngxParameters->Set("DLSSNR.OutputSubrectBaseX", 0);
                m_ngxParameters->Set("DLSSNR.OutputSubrectBaseY", 0);
                m_ngxParameters->Set("DLSSNR.OutputSubrectWidth", working[eye].w);
                m_ngxParameters->Set("DLSSNR.OutputSubrectHeight", working[eye].h);
                m_ngxParameters->Set("DLSSNR.Depth", work.depth);
                m_ngxParameters->Set("DLSSNR.DepthSubrectBaseX", 0);
                m_ngxParameters->Set("DLSSNR.DepthSubrectBaseY", 0);
                m_ngxParameters->Set("DLSSNR.DepthSubrectWidth", work.depth ? working[eye].w : 0);
                m_ngxParameters->Set("DLSSNR.DepthSubrectHeight", work.depth ? working[eye].h : 0);
                m_ngxParameters->Set("DLSSNR.Reset", reset);
                m_ngxParameters->Set("DLSSNR.UseAutoMask", true);
                m_ngxParameters->Set("DLSSNR.Style", settings.style);
                m_ngxParameters->Set("DLSSNR.Intensity", settings.intensity);
                m_ngxParameters->Set("DLSSNR.LocalToneStrength", settings.localToneStrength);
                m_ngxParameters->Set("DLSSNR.LocalStructureStrength", settings.localStructureStrength);
                m_ngxParameters->Set("DLSSNR.SkinStructureStrength", settings.skinStructureStrength);
                m_ngxParameters->Set("DLSSNR.UICorrection", 0);
                CHECK_NGXCMD(m_dlssnrFeatureEvaluate[eye](
                    commandList.Commands.Get(), m_dlssnrFeature[eye], m_ngxParameters, nullptr));
            }
            m_nrSpatialPipeline->RecordResolve(commandList.Commands.Get());
            submitAttempted = true;
            const auto fence = m_dlssnrContext->SubmitCommandList(std::move(commandList));
            m_nrSpatialPipeline->MarkSubmitted(fence);
            registered = true;
            CHECK_HRCMD(m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fence));
        } catch (...) {
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
