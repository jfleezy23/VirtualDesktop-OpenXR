#include "pch.h"
#include "runtime.h"
#include "game_profiles.h"
#include "PairCaptureNrPrivate.h"

namespace virtualdesktop_openxr {
    void OpenXrRuntime::initializeRuntimeDelivery() {
        const auto settings = vdxr_profiles::readCurrent(xr::utf8_to_wide(RegPrefix));
        const auto mode = settings.get("FrameGenerationMode").value_or(0);
        if (m_deliveryTransport || m_isHeadless || m_useOculusRuntime || m_useApplicationDeviceForSubmission ||
            (!m_d3d11Device && !m_d3d12CommandQueue) || (mode != 2 && mode != 3 && mode != 4) ||
            getSetting("quirk_disable_async_submission").value_or(false) || nr_capture::hostRequest().valid)
            return;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        CHECK_HRCMD(m_ovrSubmissionDevice.As(&dxgi));
        CHECK_HRCMD(dxgi->GetAdapter(&adapter));
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        CHECK_HRCMD(D3D11CreateDevice(adapter.Get(),
                                      D3D_DRIVER_TYPE_UNKNOWN,
                                      nullptr,
                                      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                      &level,
                                      1,
                                      D3D11_SDK_VERSION,
                                      &device,
                                      nullptr,
                                      &context));
        ComPtr<ID3D11Device5> presentation;
        ComPtr<ID3D11DeviceContext4> immediate;
        CHECK_HRCMD(device.As(&presentation));
        CHECK_HRCMD(context.As(&immediate));
        m_deliveryTransport = std::make_unique<DeliveryTransport>(m_ovrSession,
                                                                  m_ovrSubmissionDevice.Get(),
                                                                  m_ovrSubmissionContext.Get(),
                                                                  presentation.Get(),
                                                                  immediate.Get());
    }
    ID3D11Device5* OpenXrRuntime::presentationDevice() const {
        return m_deliveryTransport ? m_deliveryTransport->presentationDevice() : m_ovrSubmissionDevice.Get();
    }
    ID3D11DeviceContext4* OpenXrRuntime::presentationContext() const {
        return m_deliveryTransport ? m_deliveryTransport->presentationContext() : m_ovrSubmissionContext.Get();
    }
    ovrResult OpenXrRuntime::createSubmissionSwapchain(ovrSession session,
                                                       IUnknown* device,
                                                       const ovrTextureSwapChainDesc* desc,
                                                       ovrTextureSwapChain* chain,
                                                       bool nativeOnly) const {
        if (!m_deliveryTransport)
            return ::ovr_CreateTextureSwapChainDX(session, device, desc, chain);
        CHECK_MSG(session == m_ovrSession && desc, "Invalid delivery session/descriptor");
        return m_deliveryTransport->create(*desc, chain, nativeOnly);
    }
    ovrResult OpenXrRuntime::submissionSwapchainLength(ovrSession session,
                                                       ovrTextureSwapChain chain,
                                                       int* value) const {
        return m_deliveryTransport ? m_deliveryTransport->length(chain, value)
                                   : ::ovr_GetTextureSwapChainLength(session, chain, value);
    }
    ovrResult OpenXrRuntime::submissionSwapchainDescription(ovrSession session,
                                                            ovrTextureSwapChain chain,
                                                            ovrTextureSwapChainDesc* value) const {
        return m_deliveryTransport ? m_deliveryTransport->description(chain, value)
                                   : ::ovr_GetTextureSwapChainDesc(session, chain, value);
    }
    ovrResult OpenXrRuntime::submissionSwapchainIndex(ovrSession session, ovrTextureSwapChain chain, int* value) const {
        return m_deliveryTransport ? m_deliveryTransport->current(chain, value)
                                   : ::ovr_GetTextureSwapChainCurrentIndex(session, chain, value);
    }
    ovrResult OpenXrRuntime::submissionSwapchainBuffer(
        ovrSession session, ovrTextureSwapChain chain, int index, REFIID iid, void** value) const {
        return m_deliveryTransport ? m_deliveryTransport->buffer(chain, index, iid, value)
                                   : ::ovr_GetTextureSwapChainBufferDX(session, chain, index, iid, value);
    }
    ovrResult OpenXrRuntime::commitSubmissionSwapchain(ovrSession session, ovrTextureSwapChain chain) const {
        return m_deliveryTransport ? m_deliveryTransport->commit(chain) : ::ovr_CommitTextureSwapChain(session, chain);
    }
    void OpenXrRuntime::destroySubmissionSwapchain(ovrSession session, ovrTextureSwapChain chain) const {
        if (m_deliveryTransport)
            m_deliveryTransport->destroy(chain);
        else
            ::ovr_DestroyTextureSwapChain(session, chain);
    }
    DeliveryTransport::Batch OpenXrRuntime::prepareDeliveryLayers(const std::vector<ovrLayer_Union>& layers,
                                                                  uint64_t serial) {
        if (!m_deliveryTransport)
            return {};
        std::vector<ovrTextureSwapChain> selected;
        selected.reserve(layers.size() * 4);
        for (const auto& layer : layers) {
            switch (layer.Header.Type) {
            case ovrLayerType_EyeFov:
            case ovrLayerType_EyeFovDepth:
                for (const auto chain : layer.EyeFov.ColorTexture)
                    selected.push_back(chain);
                if (layer.Header.Type == ovrLayerType_EyeFovDepth)
                    for (const auto chain : layer.EyeFovDepth.DepthTexture)
                        selected.push_back(chain);
                break;
            case ovrLayerType_Quad:
                selected.push_back(layer.Quad.ColorTexture);
                break;
            case ovrLayerType_Cylinder:
                selected.push_back(layer.Cylinder.ColorTexture);
                break;
            case ovrLayerType_Cube:
                selected.push_back(layer.Cube.CubeMapTexture);
                break;
            default:
                break;
            }
        }
        return m_deliveryTransport->prepare(selected, serial);
    }
    ovrResult OpenXrRuntime::waitToBeginSubmissionFrame(ovrSession session, long long frame) const {
        return nativeRenderingCall([&] { return ::ovr_WaitToBeginFrame(session, frame); }, "Wait", frame);
    }
    ovrResult OpenXrRuntime::beginSubmissionFrame(ovrSession session, long long frame) const {
        return nativeRenderingCall([&] { return ::ovr_BeginFrame(session, frame); }, "Begin", frame);
    }
    ovrResult OpenXrRuntime::endSubmissionFrame(ovrSession session,
                                                long long frame,
                                                const ovrViewScaleDesc* scale,
                                                const ovrLayerHeader* const* layers,
                                                unsigned count) const {
        return nativeRenderingCall([&] { return ::ovr_EndFrame(session, frame, scale, layers, count); }, "End", frame);
    }
} // namespace virtualdesktop_openxr
