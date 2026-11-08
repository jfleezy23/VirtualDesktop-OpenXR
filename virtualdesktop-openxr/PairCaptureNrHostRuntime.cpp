#include "pch.h"
#include "runtime.h"
#include "PairCaptureNrWarmLayouts.h"
#include "PairCaptureNrEngine.h"
#include <cstring>
namespace virtualdesktop_openxr {
    void OpenXrRuntime::observePrivateNrRawFrame(const XrFrameEndInfo& info) {
        const auto& request = nr_capture::hostRequest();
        if (!request.valid)
            return;
        const auto& settings = m_precompositor.dlssnrSettings;
        const uint64_t actualMode = settings.processingResolution == 100  ? NR_NATIVE100
                                    : settings.processingResolution == 85 ? NR_SPATIAL85
                                                                          : 0;
        if (!settings.enabled || actualMode != request.mode || settings.foveationSize != 1.f || !m_useAsyncSubmission ||
            m_useApplicationDeviceForSubmission || m_isHeadless || isD3D12Session() || isVulkanSession() ||
            isOpenGLSession()) {
            nr_capture::hostInvalidateWarm();
            return;
        }
        nr_capture::warmLayoutBegin(m_frameBegun,
                                    actualMode,
                                    std::abs(m_upscalingMultiplier - 1.f) > FLT_EPSILON,
                                    m_precompositor.sharpenFactor > 0.f);
        if (info.layerCount != 1 || !info.layers || !info.layers[0] ||
            info.layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            return;
        const auto& layer = *reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[0]);
        if (layer.viewCount != 2 || !layer.views)
            return;
        nr_capture::CapturePreflightInput input{};
        input.serial = m_frameBegun;
        input.displayTime = static_cast<uint64_t>(info.displayTime);
        input.upscaling = std::abs(m_upscalingMultiplier - 1.f) > FLT_EPSILON;
        input.sharpening = m_precompositor.sharpenFactor > 0.f;
        const auto bits = [](float value) noexcept {
            uint32_t result{};
            std::memcpy(&result, &value, sizeof result);
            return result;
        };
        const auto describe = [&](nr_capture::CaptureRawSource& raw,
                                  const XrSwapchainSubImage& sub,
                                  const Swapchain& source,
                                  uint64_t role,
                                  uint32_t eye) noexcept {
            if (sub.imageArrayIndex >= source.xrDesc.arraySize || source.xrDesc.faceCount != 1 ||
                sub.imageRect.offset.x < 0 || sub.imageRect.offset.y < 0 || sub.imageRect.extent.width <= 0 ||
                sub.imageRect.extent.height <= 0)
                return false;
            raw.swapchain = &source;
            raw.releasedIndex = static_cast<uint32_t>(source.lastReleasedIndex);
            raw.selectedArraySlice = sub.imageArrayIndex;
            raw.role = role;
            raw.eye = eye;
            raw.texture = source.appSwapchain.images[source.lastReleasedIndex].Get();
            raw.metadata.hostSerial = m_frameBegun;
            raw.metadata.projectionIndex = 0;
            raw.metadata.releasedIndex = raw.releasedIndex;
            raw.metadata.selectedArraySlice = sub.imageArrayIndex;
            raw.metadata.rectXbits = static_cast<uint32_t>(sub.imageRect.offset.x);
            raw.metadata.rectYbits = static_cast<uint32_t>(sub.imageRect.offset.y);
            raw.metadata.rectWidth = static_cast<uint32_t>(sub.imageRect.extent.width);
            raw.metadata.rectHeight = static_cast<uint32_t>(sub.imageRect.extent.height);
            raw.metadata.cropXbits = raw.metadata.rectXbits;
            raw.metadata.cropYbits = raw.metadata.rectYbits;
            raw.metadata.cropWidth = raw.metadata.rectWidth;
            raw.metadata.cropHeight = raw.metadata.rectHeight;
            raw.metadata.geometryObserved = true;
            return raw.texture != nullptr;
        };
        const auto& entry = vdxrfg::pair_capture::currentEntry;
        if (entry.serial == m_frameBegun && entry.displayTime == info.displayTime && entry.count && entry.count <= 48 &&
            entry.producerDevice == m_ovrSubmissionDevice.Get() &&
            entry.producerContext == m_ovrSubmissionContext.Get()) {
            input.entryQpc = entry.qpc;
            input.leaseQpc = entry.leaseQpc;
            input.frameThread = entry.thread;
            input.stackCount = entry.count;
            for (size_t i = 0; i < entry.count; ++i)
                input.stack[i] = reinterpret_cast<uint64_t>(entry.addresses[i]);
        }
        input.rawSettings.enabled = settings.enabled;
        input.rawSettings.style = static_cast<uint32_t>(settings.style);
        input.rawSettings.intensity = bits(settings.intensity);
        input.rawSettings.tone = bits(settings.localToneStrength);
        input.rawSettings.structure = bits(settings.localStructureStrength);
        input.rawSettings.skin = bits(settings.skinStructureStrength);
        input.rawSettings.processing = settings.processingResolution;
        input.rawSettings.coverage = 100; // actual checked foveationSize==1 above
        input.rawSettings.upscaling = input.upscaling;
        input.rawSettings.sharpening = input.sharpening;
        input.rawSettings.sharpenFactor = bits(m_precompositor.sharpenFactor);
        // These four display dimensions are first-eye active rectangles, not
        // global/native/model working sizes or packed allocation dimensions.
        input.rawSettings.inputWidth = layer.views[0].subImage.imageRect.extent.width;
        input.rawSettings.inputHeight = layer.views[0].subImage.imageRect.extent.height;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& view = layer.views[eye];
            if (!m_swapchains.count(view.subImage.swapchain))
                return;
            const auto& colour = *reinterpret_cast<const Swapchain*>(view.subImage.swapchain);
            if (colour.lastReleasedIndex < 0 ||
                static_cast<size_t>(colour.lastReleasedIndex) >= colour.appSwapchain.images.size())
                return;
            if (!describe(input.raw[eye * 2], view.subImage, colour, NR_RAW_COLOUR, eye))
                return;
            if (eye == 0)
                input.rawSettings.srgb = isSRGBFormat(colour.dxgiFormatForSubmission);
            nr_capture::warmLayout11(
                m_frameBegun, NR_RAW_COLOUR, eye, colour.appSwapchain.images[colour.lastReleasedIndex].Get());
            const XrCompositionLayerDepthInfoKHR* depth{};
            auto* depthEntry = reinterpret_cast<const XrBaseInStructure*>(view.next);
            uint32_t entries = 0;
            while (depthEntry && entries++ < 16) {
                if (depthEntry->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                    if (depth)
                        return;
                    depth = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(depthEntry);
                }
                depthEntry = depthEntry->next;
            }
            if (depthEntry || !depth || !m_swapchains.count(depth->subImage.swapchain))
                return;
            const auto& source = *reinterpret_cast<const Swapchain*>(depth->subImage.swapchain);
            if (source.lastReleasedIndex < 0 ||
                static_cast<size_t>(source.lastReleasedIndex) >= source.appSwapchain.images.size())
                return;
            if (!describe(input.raw[eye * 2 + 1], depth->subImage, source, NR_RAW_DEPTH, eye))
                return;
            auto& depthGeometry = input.raw[eye * 2 + 1].metadata;
            depthGeometry.minDepthBits = bits(depth->minDepth);
            depthGeometry.maxDepthBits = bits(depth->maxDepth);
            depthGeometry.nearZBits = bits(depth->nearZ);
            depthGeometry.farZBits = bits(depth->farZ);
            nr_capture::warmLayout11(
                m_frameBegun, NR_RAW_DEPTH, eye, source.appSwapchain.images[source.lastReleasedIndex].Get());
        }
        // Warm-only: borrow pre-existing contexts and original shared fences. No
        // initialization or fallback is introduced to make a diagnostic eligible.
        if (!m_dlssnrContext || !m_frameGeneration || !m_dlssnrDevice || !m_dlssnrInFence || !m_dlssnrOutFence)
            return;
        FrameGeneration::PrivateNrCanonicalBorrow fg{};
        if (!m_frameGeneration->borrowPrivateNrCanonical(fg) || !fg.device12 ||
            fg.device != m_ovrSubmissionDevice.Get() || fg.producer != m_ovrSubmissionContext.Get())
            return;
        input.nrContext = m_dlssnrContext.get();
        input.nrContextLifetime = m_dlssnrContext->GetObservedLifetimeEpoch();
        input.producerDevice = m_ovrSubmissionDevice.Get();
        input.producerContext = m_ovrSubmissionContext.Get();
        input.applicationDevice = m_d3d11Device.Get();
        input.applicationContext = m_d3d11Context.Get();
        input.nrDevice = m_dlssnrDevice.Get();
        input.fgDevice = fg.device12;
        input.originalInputProducerFence = m_ovrSubmissionCompletionFence.Get();
        input.originalInputAlias = m_dlssnrInFence.Get();
        input.originalNrFence = m_dlssnrContext->GetCompletionFence();
        input.originalOutputAlias = m_dlssnrOutFence.Get();
        auto* guard = nr_capture::SourceReturnGuard::current();
        if (!guard)
            return;
        auto& engine = nr_capture::captureEngine();
        if (!engine.resourcesPrepared() && !engine.prepareResources(input))
            return;
        const auto frame = engine.plannedFrame(m_frameBegun);
        if (frame < 2 && engine.budget().preflightComplete && !engine.acquireRaw(frame, input, *guard))
            nr_capture::hostCaptureTerminal(NR_BAD_ROLE, true);
    }
} // namespace virtualdesktop_openxr
