// MIT License
//
// Copyright(c) 2022-2024 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "log.h"
#include "runtime.h"
#include "utils.h"

#include "UpscalingCS.h"
#include "SharpeningCS.h"
#include "AlignDepthPS.h"

#define A_CPU
#include <ffx_a.h>
#include <ffx_cas.h>
#include <ffx_fsr1.h>

namespace virtualdesktop_openxr {

    using namespace virtualdesktop_openxr::log;
    using namespace virtualdesktop_openxr::utils;
    using namespace xr::math;

    struct UpscaleCSConstants {
        alignas(8) XrOffset2Df topLeftNormalized;
        alignas(4) uint32_t isSRGB;
        alignas(4) uint32_t padding;
        alignas(8) XrOffset2Di sourceMin;
        alignas(8) XrOffset2Di sourceMax;
        alignas(8) XrExtent2Di sourceSize;
        alignas(8) XrExtent2Di outputSize;
        alignas(16) uint32_t const0[4];
        alignas(16) uint32_t const1[4];
        alignas(16) uint32_t const2[4];
        alignas(16) uint32_t const3[4];
    };

    struct SharpenCSConstants {
        alignas(8) XrOffset2Di topLeft;
        alignas(4) uint32_t isSRGB;
        alignas(4) uint32_t padding;
        alignas(8) XrOffset2Di sourceMin;
        alignas(8) XrOffset2Di sourceMax;
        alignas(8) XrExtent2Di outputSize;
        alignas(8) uint32_t padding2[2];
        alignas(16) uint32_t const0[4];
        alignas(16) uint32_t const1[4];
    };
    static_assert(sizeof(UpscaleCSConstants) == 112);
    static_assert(sizeof(SharpenCSConstants) == 80);

    struct AlignDepthPSConstants {
        XrOffset2Di sourceOrigin;
        XrExtent2Di sourceExtent;
        XrOffset2Di destinationOrigin;
        XrExtent2Di destinationExtent;
    };
    static_assert(sizeof(AlignDepthPSConstants) == 32);

    void OpenXrRuntime::upscaler(const XrSwapchainSubImage** subImages, ovrLayerEyeFov& layer) {
        const bool upscaling = std::abs(m_upscalingMultiplier - 1.f) > FLT_EPSILON;
        const bool sharpening = m_precompositor.sharpenFactor > 0.f;

        // We will store our stereo projection in the left eye swapchain.
        Swapchain& xrSwapchain = *(Swapchain*)subImages[xr::StereoView::Left]->swapchain;
        const bool outputIsSRGB = isSRGBFormat(xrSwapchain.dxgiFormatForSubmission);
        ovrSizei resolution = ovrSizei{
            (int)xr::math::AlignTo<4>(
                (uint32_t)(std::max(subImages[0]->imageRect.extent.width, subImages[1]->imageRect.extent.width) /
                           m_upscalingMultiplier)),
            (int)xr::math::AlignTo<4>(
                (uint32_t)(std::max(subImages[0]->imageRect.extent.height, subImages[1]->imageRect.extent.height) /
                           m_upscalingMultiplier))};
        ensureSwapchainPrecompositorResources(xrSwapchain, resolution);
        if (upscaling && sharpening) {
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                ovrSizei currentResolution{};
                if (xrSwapchain.intermediate[eye].image) {
                    D3D11_TEXTURE2D_DESC desc{};
                    xrSwapchain.intermediate[eye].image->GetDesc(&desc);
                    currentResolution.w = desc.Width;
                    currentResolution.h = desc.Height;
                }
                if (!xrSwapchain.intermediate[eye].image || currentResolution.w != resolution.w ||
                    currentResolution.h != resolution.h) {
                    // Publish the resized image and its views together. A failed view allocation must
                    // leave the previous complete generation available and retryable on the next frame.
                    IntermediateTexture candidate;
                    {
                        D3D11_TEXTURE2D_DESC desc{};
                        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        desc.Width = resolution.w;
                        desc.Height = resolution.h;
                        desc.ArraySize = 1;
                        desc.MipLevels = 1;
                        desc.SampleDesc.Count = 1;
                        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                        CHECK_HRCMD(
                            m_ovrSubmissionDevice->CreateTexture2D(&desc, nullptr, candidate.image.GetAddressOf()));
                        setDebugName(
                            candidate.image.Get(),
                            fmt::format("Precompositor Intermediate Texture [{}, {}]", eye, (void*)&xrSwapchain));
                    }
                    {
                        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        desc.Texture2D.MipLevels = -1;
                        CHECK_HRCMD(m_ovrSubmissionDevice->CreateShaderResourceView(
                            candidate.image.Get(), &desc, candidate.srv.GetAddressOf()));
                        setDebugName(candidate.srv.Get(),
                                     fmt::format("Precompositor Intermediate SRV [{}, {}]", eye, (void*)&xrSwapchain));
                    }
                    {
                        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        CHECK_HRCMD(m_ovrSubmissionDevice->CreateUnorderedAccessView(
                            candidate.image.Get(), &desc, candidate.uav.GetAddressOf()));
                        setDebugName(candidate.uav.Get(),
                                     fmt::format("Precompositor Intermediate UAV [{}, {}]", eye, (void*)&xrSwapchain));
                    }
                    xrSwapchain.intermediate[eye] = std::move(candidate);
                }
            }
        }

        // We are about to do something destructive to the application context. Save the context. It will be
        // restored at the end of xrEndFrame().
        if (m_d3d11Device == m_ovrSubmissionDevice && !m_d3d11ContextState) {
            m_ovrSubmissionContext->SwapDeviceContextState(m_ovrSubmissionContextState.Get(),
                                                           m_d3d11ContextState.ReleaseAndGetAddressOf());
        }

        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            Swapchain& swapchain = *(Swapchain*)subImages[eye]->swapchain;
            ID3D11ShaderResourceView* inputSrv = nullptr;
            XrRect2Di inputRect = subImages[eye]->imageRect;
            ovrSizei inputSize = {swapchain.ovrDesc.Width, swapchain.ovrDesc.Height};
            auto& slice = swapchain.resolvedSlices[subImages[eye]->imageArrayIndex];
            if ((int)slice.srvs.size() <= slice.lastCommittedIndex) {
                slice.srvs.resize(slice.lastCommittedIndex + 1);
            }
            if (!slice.srvs[slice.lastCommittedIndex]) {
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                desc.Format = getShaderResourceViewFormat(swapchain.dxgiFormatForSubmission);
                desc.Texture2D.MipLevels = -1;
                CHECK_HRCMD(m_ovrSubmissionDevice->CreateShaderResourceView(
                    slice.images[slice.lastCommittedIndex].Get(),
                    &desc,
                    slice.srvs[slice.lastCommittedIndex].ReleaseAndGetAddressOf()));
                setDebugName(slice.srvs[slice.lastCommittedIndex].Get(),
                             fmt::format("Runtime Slice Copy SRV[{}, {}, {}]",
                                         subImages[eye]->imageArrayIndex,
                                         slice.lastCommittedIndex,
                                         (void*)&swapchain));
            }
            inputSrv = slice.srvs[slice.lastCommittedIndex].Get();
            const ovrSizei outputResolution = {
                (int)xr::math::AlignTo<4>((uint32_t)(inputRect.extent.width / m_upscalingMultiplier)),
                (int)xr::math::AlignTo<4>((uint32_t)(inputRect.extent.height / m_upscalingMultiplier))};

            // Prepare swapchain outputs.
            int imageIndex = 0;
            CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(
                m_ovrSession, xrSwapchain.stereoProjection[eye].ovrSwapchain, &imageIndex));

            if (upscaling) {
                m_ovrSubmissionContext->CSSetShader(m_upscaleShader.Get(), nullptr, 0);
                {
                    UpscaleCSConstants constants{};
                    constants.topLeftNormalized = {(float)inputRect.offset.x / inputSize.w,
                                                   (float)inputRect.offset.y / inputSize.h};
                    constants.sourceMin = inputRect.offset;
                    constants.sourceMax = {inputRect.offset.x + inputRect.extent.width - 1,
                                           inputRect.offset.y + inputRect.extent.height - 1};
                    constants.sourceSize = {inputSize.w, inputSize.h};
                    constants.outputSize = {outputResolution.w, outputResolution.h};
                    // If we apply sharpening at the next stage, we use half-precision floats for the intermediate
                    // texture and we will do conversion to sRGB at the sharpening stage.
                    constants.isSRGB = !sharpening && outputIsSRGB;

                    FsrEasuCon(constants.const0,
                               constants.const1,
                               constants.const2,
                               constants.const3,
                               (AF1)inputRect.extent.width,
                               (AF1)inputRect.extent.height,
                               (AF1)inputSize.w,
                               (AF1)inputSize.h,
                               (AF1)outputResolution.w,
                               (AF1)outputResolution.h);

                    D3D11_MAPPED_SUBRESOURCE mappedResources;
                    CHECK_HRCMD(m_ovrSubmissionContext->Map(
                        m_upscalerConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                    memcpy(mappedResources.pData, &constants, sizeof(constants));
                    m_ovrSubmissionContext->Unmap(m_upscalerConstants.Get(), 0);
                    m_ovrSubmissionContext->CSSetConstantBuffers(0, 1, m_upscalerConstants.GetAddressOf());
                }
                if (sharpening) {
                    m_ovrSubmissionContext->CSSetUnorderedAccessViews(
                        0, 1, xrSwapchain.intermediate[eye].uav.GetAddressOf(), nullptr);
                } else {
                    m_ovrSubmissionContext->CSSetUnorderedAccessViews(
                        0, 1, xrSwapchain.stereoProjection[eye].uavs[imageIndex].GetAddressOf(), nullptr);
                }
                m_ovrSubmissionContext->CSSetSamplers(0, 1, m_linearClampSampler.GetAddressOf());
                m_ovrSubmissionContext->CSSetShaderResources(0, 1, &inputSrv);

                const uint32_t blockWidth = 16;
                const uint32_t blockHeight = 16;
                m_ovrSubmissionContext->Dispatch(((outputResolution.w + blockWidth - 1) / blockWidth),
                                                 ((outputResolution.h + blockHeight - 1) / blockHeight),
                                                 1);
            }

            if (sharpening) {
                m_ovrSubmissionContext->CSSetShader(m_sharpenShader.Get(), nullptr, 0);
                {
                    SharpenCSConstants constants{};
                    // If we upscaled at the previous stage, the image occupies the entire texture.
                    constants.topLeft = !upscaling ? inputRect.offset : XrOffset2Di{0, 0};
                    constants.sourceMin = constants.topLeft;
                    constants.sourceMax = !upscaling ? XrOffset2Di{inputRect.offset.x + inputRect.extent.width - 1,
                                                                   inputRect.offset.y + inputRect.extent.height - 1}
                                                     : XrOffset2Di{outputResolution.w - 1, outputResolution.h - 1};
                    constants.outputSize = {outputResolution.w, outputResolution.h};
                    constants.isSRGB = outputIsSRGB;

                    CasSetup(constants.const0,
                             constants.const1,
                             std::clamp(m_precompositor.sharpenFactor, 0.f, 1.f),
                             (AF1)outputResolution.w,
                             (AF1)outputResolution.h,
                             (AF1)outputResolution.w,
                             (AF1)outputResolution.h);

                    D3D11_MAPPED_SUBRESOURCE mappedResources;
                    CHECK_HRCMD(m_ovrSubmissionContext->Map(
                        m_upscalerConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                    memcpy(mappedResources.pData, &constants, sizeof(constants));
                    m_ovrSubmissionContext->Unmap(m_upscalerConstants.Get(), 0);
                    m_ovrSubmissionContext->CSSetConstantBuffers(0, 1, m_upscalerConstants.GetAddressOf());
                }
                m_ovrSubmissionContext->CSSetUnorderedAccessViews(
                    0, 1, xrSwapchain.stereoProjection[eye].uavs[imageIndex].GetAddressOf(), nullptr);
                if (upscaling) {
                    m_ovrSubmissionContext->CSSetShaderResources(
                        0, 1, xrSwapchain.intermediate[eye].srv.GetAddressOf());
                } else {
                    m_ovrSubmissionContext->CSSetShaderResources(0, 1, &inputSrv);
                }

                const uint32_t blockWidth = 16;
                const uint32_t blockHeight = 16;
                m_ovrSubmissionContext->Dispatch(((outputResolution.w + blockWidth - 1) / blockWidth),
                                                 ((outputResolution.h + blockHeight - 1) / blockHeight),
                                                 1);
            }

            CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, xrSwapchain.stereoProjection[eye].ovrSwapchain));

            // Patch the layer.
            layer.ColorTexture[eye] = xrSwapchain.stereoProjection[eye].ovrSwapchain;
            layer.Viewport[eye].Pos = {0, 0};
            layer.Viewport[eye].Size = outputResolution;
        }

        // Unbind all resources to avoid D3D validation errors.
        {
            m_ovrSubmissionContext->CSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer* nullCBV[] = {nullptr};
            m_ovrSubmissionContext->CSSetConstantBuffers(0, 1, nullCBV);
            ID3D11SamplerState* nullSampler[] = {nullptr};
            m_ovrSubmissionContext->CSSetSamplers(0, 1, nullSampler);
            ID3D11ShaderResourceView* nullSRV[] = {nullptr};
            m_ovrSubmissionContext->CSSetShaderResources(0, 1, nullSRV);
            ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
            m_ovrSubmissionContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        }
    }

    void OpenXrRuntime::alignDepthLayer(const XrSwapchainSubImage** color,
                                        const XrSwapchainSubImage** depth,
                                        ovrLayerEyeFovDepth& layer) {
        Swapchain& owner = *(Swapchain*)color[0]->swapchain;
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            if (!depth[eye]) {
                continue;
            }
            Swapchain& input = *(Swapchain*)depth[eye]->swapchain;
            const auto& sourceRect = depth[eye]->imageRect;
            const auto& destinationRect = layer.Viewport[eye];
            ovrTextureSwapChainDesc colorDesc{};
            CHECK_OVRCMD(ovr_GetTextureSwapChainDesc(m_ovrSession, layer.ColorTexture[eye], &colorDesc));
            if (input.ovrDesc.Width == colorDesc.Width && input.ovrDesc.Height == colorDesc.Height &&
                sourceRect.offset.x == destinationRect.Pos.x && sourceRect.offset.y == destinationRect.Pos.y &&
                sourceRect.extent.width == destinationRect.Size.w &&
                sourceRect.extent.height == destinationRect.Size.h) {
                continue;
            }

            // Cache per projection layer: a later layer may use the same source color swapchain
            // with different depth or viewports and must not replace an earlier submitted image.
            auto& output = owner.depthProjection[m_precompositor.layerIndex][eye];
            ovrTextureSwapChainDesc outputDesc{};
            if (output.ovrSwapchain) {
                CHECK_OVRCMD(ovr_GetTextureSwapChainDesc(m_ovrSession, output.ovrSwapchain, &outputDesc));
            }
            if (!output.ovrSwapchain || outputDesc.Width != colorDesc.Width || outputDesc.Height != colorDesc.Height) {
                outputDesc = {};
                outputDesc.Type = ovrTexture_2D;
                outputDesc.Format = OVR_FORMAT_D32_FLOAT;
                outputDesc.Width = colorDesc.Width;
                outputDesc.Height = colorDesc.Height;
                outputDesc.ArraySize = outputDesc.MipLevels = outputDesc.SampleCount = 1;
                outputDesc.BindFlags = ovrTextureBind_DX_DepthStencil;
                SwapchainSlice candidate;
                CHECK_OVRCMD(ovr_CreateTextureSwapChainDX(
                    m_ovrSession, m_ovrSubmissionDevice.Get(), &outputDesc, &candidate.ovrSwapchain));
                auto releaseCandidate = MakeScopeGuard([&] {
                    if (candidate.ovrSwapchain) {
                        ovr_DestroyTextureSwapChain(m_ovrSession, candidate.ovrSwapchain);
                    }
                });
                int count = 0;
                CHECK_OVRCMD(ovr_GetTextureSwapChainLength(m_ovrSession, candidate.ovrSwapchain, &count));
                CHECK_MSG(count > 0, "Aligned depth swapchain has no images");
                for (int index = 0; index < count; index++) {
                    ComPtr<ID3D11Texture2D> image;
                    CHECK_OVRCMD(ovr_GetTextureSwapChainBufferDX(
                        m_ovrSession, candidate.ovrSwapchain, index, IID_PPV_ARGS(image.GetAddressOf())));
                    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                    desc.Format = DXGI_FORMAT_D32_FLOAT;
                    desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                    ComPtr<ID3D11DepthStencilView> dsv;
                    CHECK_HRCMD(m_ovrSubmissionDevice->CreateDepthStencilView(image.Get(), &desc, dsv.GetAddressOf()));
                    candidate.images.push_back(std::move(image));
                    candidate.dsvs.push_back(std::move(dsv));
                }
                if (output.ovrSwapchain) {
                    flushSubmissionContext();
                    output.dsvs.clear();
                    output.images.clear();
                    ovr_DestroyTextureSwapChain(m_ovrSession, output.ovrSwapchain);
                }
                output = std::move(candidate);
                candidate.ovrSwapchain = nullptr;
            }

            auto& source = input.resolvedSlices[depth[eye]->imageArrayIndex];
            if ((int)source.srvs.size() <= source.lastCommittedIndex) {
                source.srvs.resize(source.lastCommittedIndex + 1);
            }
            if (!source.srvs[source.lastCommittedIndex]) {
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.Format = getShaderResourceViewFormat(input.dxgiFormatForSubmission);
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipLevels = 1;
                CHECK_HRCMD(m_ovrSubmissionDevice->CreateShaderResourceView(
                    source.images[source.lastCommittedIndex].Get(),
                    &desc,
                    source.srvs[source.lastCommittedIndex].ReleaseAndGetAddressOf()));
            }
            if (m_d3d11Device == m_ovrSubmissionDevice && !m_d3d11ContextState) {
                m_ovrSubmissionContext->SwapDeviceContextState(m_ovrSubmissionContextState.Get(),
                                                               m_d3d11ContextState.ReleaseAndGetAddressOf());
            }
            int index = 0;
            CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(m_ovrSession, output.ovrSwapchain, &index));
            CHECK_MSG(index >= 0 && (size_t)index < output.dsvs.size(), "Aligned depth swapchain is incomplete");
            AlignDepthPSConstants constants{sourceRect.offset,
                                            sourceRect.extent,
                                            {destinationRect.Pos.x, destinationRect.Pos.y},
                                            {destinationRect.Size.w, destinationRect.Size.h}};
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK_HRCMD(
                m_ovrSubmissionContext->Map(m_alignDepthConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, &constants, sizeof(constants));
            m_ovrSubmissionContext->Unmap(m_alignDepthConstants.Get(), 0);
            m_ovrSubmissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_ovrSubmissionContext->VSSetShader(m_fullQuadVS.Get(), nullptr, 0);
            m_ovrSubmissionContext->PSSetShader(m_alignDepthShader.Get(), nullptr, 0);
            m_ovrSubmissionContext->PSSetConstantBuffers(0, 1, m_alignDepthConstants.GetAddressOf());
            m_ovrSubmissionContext->PSSetShaderResources(0, 1, source.srvs[source.lastCommittedIndex].GetAddressOf());
            m_ovrSubmissionContext->OMSetRenderTargets(0, nullptr, output.dsvs[index].Get());
            m_ovrSubmissionContext->OMSetDepthStencilState(m_noDepthReadState.Get(), 0xff);
            m_ovrSubmissionContext->RSSetState(nullptr);
            D3D11_VIEWPORT viewport{(float)destinationRect.Pos.x,
                                    (float)destinationRect.Pos.y,
                                    (float)destinationRect.Size.w,
                                    (float)destinationRect.Size.h,
                                    0.f,
                                    1.f};
            m_ovrSubmissionContext->RSSetViewports(1, &viewport);
            m_ovrSubmissionContext->Draw(3, 0);
            m_ovrSubmissionContext->OMSetRenderTargets(0, nullptr, nullptr);
            m_ovrSubmissionContext->VSSetShader(nullptr, nullptr, 0);
            m_ovrSubmissionContext->PSSetShader(nullptr, nullptr, 0);
            ID3D11ShaderResourceView* nullSrv[] = {nullptr};
            m_ovrSubmissionContext->PSSetShaderResources(0, 1, nullSrv);
            ID3D11Buffer* nullCb[] = {nullptr};
            m_ovrSubmissionContext->PSSetConstantBuffers(0, 1, nullCb);
            CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, output.ovrSwapchain));
            layer.DepthTexture[eye] = output.ovrSwapchain;
        }
    }

    void OpenXrRuntime::initializePrecompositorResources() {
        CHECK_HRCMD(m_ovrSubmissionDevice->CreateComputeShader(
            g_SharpeningCS, sizeof(g_SharpeningCS), nullptr, m_sharpenShader.ReleaseAndGetAddressOf()));
        setDebugName(m_sharpenShader.Get(), "Sharpen CS");
        CHECK_HRCMD(m_ovrSubmissionDevice->CreateComputeShader(
            g_UpscalingCS, sizeof(g_UpscalingCS), nullptr, m_upscaleShader.ReleaseAndGetAddressOf()));
        setDebugName(m_sharpenShader.Get(), "Upscale CS");
        CHECK_HRCMD(m_ovrSubmissionDevice->CreatePixelShader(
            g_AlignDepthPS, sizeof(g_AlignDepthPS), nullptr, m_alignDepthShader.ReleaseAndGetAddressOf()));
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(AlignDepthPSConstants);
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            CHECK_HRCMD(
                m_ovrSubmissionDevice->CreateBuffer(&desc, nullptr, m_alignDepthConstants.ReleaseAndGetAddressOf()));
        }
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = (UINT)((std::max(sizeof(SharpenCSConstants), sizeof(UpscaleCSConstants)) + 15) / 16) * 16;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            CHECK_HRCMD(
                m_ovrSubmissionDevice->CreateBuffer(&desc, nullptr, m_upscalerConstants.ReleaseAndGetAddressOf()));
            setDebugName(m_upscalerConstants.Get(), "Upscale/Sharpen Constants");
        }
    }

} // namespace virtualdesktop_openxr
