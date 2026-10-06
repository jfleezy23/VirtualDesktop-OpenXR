// MIT License
//
// Copyright(c) 2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
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

#define NVSDK_NGX_Parameter_DLSSNR_Width "DLSSNR.Width"
#define NVSDK_NGX_Parameter_DLSSNR_Height "DLSSNR.Height"
#define NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset "DLSSNR.Hint.Render.Preset"
#define NVSDK_NGX_Parameter_DLSSNR_Enabled "DLSSNR.Enabled"
#define NVSDK_NGX_Parameter_DLSSNR_DepthInverted "DLSSNR.DepthInverted"

#define NVSDK_NGX_Parameter_DLSSNR_Color "DLSSNR.Color"
#define NVSDK_NGX_Parameter_DLSSNR_Output "DLSSNR.Output"
#define NVSDK_NGX_Parameter_DLSSNR_Depth "DLSSNR.Depth"
#define NVSDK_NGX_Parameter_DLSSNR_MVec "DLSSNR.MVec"
#define NVSDK_NGX_Parameter_DLSSNR_MVecScaleX "DLSSNR.MVecScaleX"
#define NVSDK_NGX_Parameter_DLSSNR_MVecScaleY "DLSSNR.MVecScaleY"
#define NVSDK_NGX_Parameter_DLSSNR_Reset "DLSSNR.Reset"
#define NVSDK_NGX_Parameter_DLSSNR_UICorrection "DLSSNR.UICorrection"

#define NVSDK_NGX_Parameter_DLSSNR_Style "DLSSNR.Style"
#define NVSDK_NGX_Parameter_DLSSNR_Intensity "DLSSNR.Intensity"
#define NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength "DLSSNR.LocalToneStrength"
#define NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength "DLSSNR.LocalStructureStrength"
#define NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength "DLSSNR.SkinStructureStrength"
#define NVSDK_NGX_Parameter_DLSSNR_UseAutoMask "DLSSNR.UseAutoMask"

#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX "DLSSNR.ColorSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY "DLSSNR.ColorSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth "DLSSNR.ColorSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight "DLSSNR.ColorSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX "DLSSNR.OutputSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY "DLSSNR.OutputSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth "DLSSNR.OutputSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight "DLSSNR.OutputSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX "DLSSNR.MVecSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY "DLSSNR.MVecSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth "DLSSNR.MVecSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight "DLSSNR.MVecSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX "DLSSNR.DepthSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY "DLSSNR.DepthSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth "DLSSNR.DepthSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight "DLSSNR.DepthSubrectHeight"

namespace ngx {

    const bool forceManualLoad = false;
    wil::unique_hmodule dlssnrModule;
    using InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long,
                                                const wchar_t*,
                                                ID3D12Device*,
                                                NVSDK_NGX_Version,
                                                const NVSDK_NGX_Parameter*);
    InitExt Init_Ext{nullptr};
    decltype(&::NVSDK_NGX_D3D12_Shutdown1) Shutdown1{nullptr};
    ComPtr<ID3D12Device> initializedDevice;
    auto NVSDK_NGX_D3D12_CreateFeature = &::NVSDK_NGX_D3D12_CreateFeature;
    auto NVSDK_NGX_D3D12_ReleaseFeature = &::NVSDK_NGX_D3D12_ReleaseFeature;
    auto NVSDK_NGX_D3D12_EvaluateFeature = &::NVSDK_NGX_D3D12_EvaluateFeature;

    // Hack to avoid NVSDK_NGX_Result_FAIL_PlatformError when loading the DLSS-NR DLL outside of NGX. Pretend that the
    // caller is NGX.
    DEFINE_DETOUR_FUNCTION(DWORD, GetModuleFileNameW, HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
        HMODULE callerModule;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)_ReturnAddress(),
                           &callerModule);
        if (callerModule == dlssnrModule.get()) {
            static const std::wstring nvngx = L"nvngx.dll";
            _snwprintf_s(lpFilename, nSize, _TRUNCATE, nvngx.c_str());
            if (nSize < nvngx.size() + 1) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
            }
            return nSize;
        }

        return original_GetModuleFileNameW(hModule, lpFilename, nSize);
    }

} // namespace ngx

namespace virtualdesktop_openxr {

    using namespace virtualdesktop_openxr::log;
    using namespace virtualdesktop_openxr::utils;

    void OpenXrRuntime::upliftLayer(const XrSwapchainSubImage** views,
                                    const XrSwapchainSubImage** depth,
                                    float nearZ,
                                    float farZ,
                                    ovrLayerEyeFov& layer) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "DLSSNR");
        const auto& settings = m_precompositor.dlssnrSettings;

        const ovrSizei resolution = {std::max(layer.Viewport[0].Size.w, layer.Viewport[1].Size.w),
                                     std::max(layer.Viewport[0].Size.h, layer.Viewport[1].Size.h)};
        const float foveatedScale = std::clamp(settings.foveationSize, 0.1f, 1.f);
        const ovrSizei foveatedResolution = {(int)xr::math::AlignTo<4>((uint32_t)(resolution.w * foveatedScale)),
                                             (int)xr::math::AlignTo<4>((uint32_t)(resolution.h * foveatedScale))};
        // TODO: Plumb in the eye tracking data.

        struct BOX {
            int32_t left;
            int32_t top;
            int32_t right;
            int32_t bottom;
        };
        const auto SafeClamp = [](BOX& box, const BOX& limit) {
            box.left = std::max(limit.left, box.left);
            box.right = std::min(limit.right, box.right);
            box.top = std::max(limit.top, box.top);
            box.bottom = std::min(limit.bottom, box.bottom);
        };

        const BOX inputBox[2] = {{views[0]->imageRect.offset.x,
                                  views[0]->imageRect.offset.y,
                                  views[0]->imageRect.offset.x + views[0]->imageRect.extent.width,
                                  views[0]->imageRect.offset.y + views[0]->imageRect.extent.height},
                                 {views[1]->imageRect.offset.x,
                                  views[1]->imageRect.offset.y,
                                  views[1]->imageRect.offset.x + views[1]->imageRect.extent.width,
                                  views[1]->imageRect.offset.y + views[1]->imageRect.extent.height}};

        const BOX outputBox[2] = {{0, 0, views[0]->imageRect.extent.width, views[0]->imageRect.extent.height},
                                  {resolution.w, 0, resolution.w + views[1]->imageRect.extent.width,
                                   views[1]->imageRect.extent.height}};

        // Resize resources if needed.
        const auto format = ((Swapchain*)views[0]->swapchain)->ovrDesc.Format;
        if (!m_dlssnrOutputSwapchain || m_dlssnrOutputSwapchainResolution.w < resolution.w ||
            m_dlssnrOutputSwapchainResolution.h < resolution.h || m_dlssnrOutputSwapchainFormat != format) {
            // Finish in-flight work before releasing the resources.
            flushSubmissionContext();
            m_dlssnrContext->Flush();

            ensureDlssnrSwapchainResources(format, resolution);
        }
        if (m_dlssnrFeatureResolution.w < foveatedResolution.w || m_dlssnrFeatureResolution.h < foveatedResolution.h) {
            // Finish in-flight work before releasing the resources.
            m_dlssnrContext->Flush();

            // Destroy DLSS-NR feature - it will be re-created in the loop further below.
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                if (m_dlssnrFeature[eye]) {
                    ngx::NVSDK_NGX_D3D12_ReleaseFeature(m_dlssnrFeature[eye]);
                    m_dlssnrFeature[eye] = nullptr;
                }
            }

            m_dlssnrFeatureResolution = foveatedResolution;
        }

        // Serialize inputs.
        m_submissionFenceValue++;
        CHECK_HRCMD(m_ovrSubmissionContext->Signal(m_ovrSubmissionCompletionFence.Get(), m_submissionFenceValue));
        CHECK_HRCMD(m_dlssnrContext->GetCommandQueue()->Wait(m_dlssnrInFence.Get(), m_submissionFenceValue));

        // Time to apply the effect.
        auto cmdList = m_dlssnrContext->GetCommandList();
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            const ovrSizei eyeResolution = {views[eye]->imageRect.extent.width, views[eye]->imageRect.extent.height};
            const ovrSizei eyeFoveatedResolution = {
                std::min(eyeResolution.w, (int)xr::math::AlignTo<4>((uint32_t)(eyeResolution.w * foveatedScale))),
                std::min(eyeResolution.h, (int)xr::math::AlignTo<4>((uint32_t)(eyeResolution.h * foveatedScale)))};
            const ovrVector2i offset = {(eyeResolution.w - eyeFoveatedResolution.w) / 2,
                                       (eyeResolution.h - eyeFoveatedResolution.h) / 2};
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, nearZ > farZ);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, true);

            // (Re)create with the desired resolution.
            if (!m_dlssnrFeature[eye]) {
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Width, foveatedResolution.w);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Height, foveatedResolution.h);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, 0);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

                if (ngx::forceManualLoad ||
                    NVSDK_NGX_FAILED(ngx::NVSDK_NGX_D3D12_CreateFeature(cmdList.Commands.Get(),
                                                                        NVSDK_NGX_Feature_Reserved18 /* DLSS-NR */,
                                                                        m_ngxParameters,
                                                                        &m_dlssnrFeature[eye]))) {
                    // User might be using an unsigned nvngx_dlssnr.dll that gets bounced by the NGX SDK. Try loading it
                    // manually.
                    if (!ngx::dlssnrModule) {
                        DetourDllAttach("Kernel32.dll",
                                        "GetModuleFileNameW",
                                        ngx::hooked_GetModuleFileNameW,
                                        ngx::original_GetModuleFileNameW);

                        const auto nvngx_dlssnrPath = virtualdesktop_openxr::dllHome / L"nvngx_dlssnr.dll";
                        *ngx::dlssnrModule.put() = LoadLibraryW(nvngx_dlssnrPath.c_str());
                        // clang-format off
                        CHECK_MSG(ngx::dlssnrModule.get(), "Failed to load DLSS-NR module");
                        ngx::Init_Ext = (ngx::InitExt)GetProcAddress(ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_Init_Ext");
                        ngx::Shutdown1 = (decltype(ngx::Shutdown1))GetProcAddress(
                            ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_Shutdown1");
                        ngx::NVSDK_NGX_D3D12_CreateFeature = (decltype(ngx::NVSDK_NGX_D3D12_CreateFeature))GetProcAddress(
                            ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_CreateFeature");
                        ngx::NVSDK_NGX_D3D12_ReleaseFeature = (decltype(ngx::NVSDK_NGX_D3D12_ReleaseFeature))GetProcAddress(
                            ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_ReleaseFeature");
                        ngx::NVSDK_NGX_D3D12_EvaluateFeature = (decltype(ngx::NVSDK_NGX_D3D12_EvaluateFeature))GetProcAddress(
                            ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_EvaluateFeature");
                        // clang-format on
                        CHECK_MSG(ngx::Init_Ext && ngx::Shutdown1 && ngx::NVSDK_NGX_D3D12_CreateFeature &&
                                      ngx::NVSDK_NGX_D3D12_ReleaseFeature && ngx::NVSDK_NGX_D3D12_EvaluateFeature,
                                  "DLSS-NR module is missing required exports");
                    }

                    if (ngx::initializedDevice.Get() != m_dlssnrDevice.Get()) {
                        CHECK_NGXCMD(ngx::Init_Ext(123456,
                                              virtualdesktop_openxr::programData.c_str(),
                                              m_dlssnrDevice.Get(),
                                              NVSDK_NGX_Version_API,
                                              nullptr));
                        ngx::initializedDevice = m_dlssnrDevice;
                    }

                    CHECK_NGXCMD(ngx::NVSDK_NGX_D3D12_CreateFeature(cmdList.Commands.Get(),
                                                                    NVSDK_NGX_Feature_Reserved18 /* DLSS-NR */,
                                                                    m_ngxParameters,
                                                                    &m_dlssnrFeature[eye]));
                }
            }

            // Prepare swapchain outputs.
            int imageIndex = 0;
            CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(m_ovrSession, m_dlssnrOutputSwapchain, &imageIndex));

            // Prepare feature evaluation.
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Output, m_dlssnrOutputSwapchainImages[imageIndex].Get());

            Swapchain& xrSwapchain = *(Swapchain*)views[eye]->swapchain;
            ID3D12Resource* inputImage =
                xrSwapchain.resolvedSlices[views[eye]->imageArrayIndex]
                    .dlssnrImages[xrSwapchain.resolvedSlices[views[eye]->imageArrayIndex].lastCommittedIndex]
                    .Get();
            BOX colorBox{};
            colorBox.left = views[eye]->imageRect.offset.x + offset.x;
            colorBox.top = views[eye]->imageRect.offset.y + offset.y;
            colorBox.right = colorBox.left + eyeFoveatedResolution.w;
            colorBox.bottom = colorBox.top + eyeFoveatedResolution.h;
            SafeClamp(colorBox, inputBox[eye]);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Color, inputImage);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, colorBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, colorBox.top);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, colorBox.right - colorBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, colorBox.bottom - colorBox.top);

            BOX destBox{};
            destBox.left = eye * resolution.w + offset.x; // Double-wide.
            destBox.top = offset.y;
            destBox.right = destBox.left + eyeFoveatedResolution.w;
            destBox.bottom = destBox.top + eyeFoveatedResolution.h;
            SafeClamp(destBox, outputBox[eye]);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, destBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, destBox.top);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, destBox.right - destBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, destBox.bottom - destBox.top);

#if 0
            // TODO: Use NVOFA to estimate motion.
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, motion);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, w);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, h);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, motionVectorScaleX);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, motionVectorScaleY);
#endif

            // The parameter map persists across frames. Clear optional inputs before rebinding them.
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Depth, (ID3D12Resource*)nullptr);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, 0);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, 0);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, 0);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, 0);
            if (depth[0] && depth[1]) {
                Swapchain& xrDepthSwapchain = *(Swapchain*)depth[eye]->swapchain;
                m_ngxParameters->Set(
                    NVSDK_NGX_Parameter_DLSSNR_Depth,
                    xrDepthSwapchain.resolvedSlices[depth[eye]->imageArrayIndex]
                        .dlssnrImages[xrDepthSwapchain.resolvedSlices[depth[eye]->imageArrayIndex].lastCommittedIndex]
                        .Get());
                BOX depthBox{};
                const auto& rect = depth[eye]->imageRect;
                // Map the color crop into the depth image's own coordinates, including its offset.
                depthBox.left = rect.offset.x + (int)((int64_t)offset.x * rect.extent.width / eyeResolution.w);
                depthBox.top = rect.offset.y + (int)((int64_t)offset.y * rect.extent.height / eyeResolution.h);
                depthBox.right = rect.offset.x + (int)(((int64_t)(offset.x + eyeFoveatedResolution.w) *
                                                       rect.extent.width + eyeResolution.w - 1) / eyeResolution.w);
                depthBox.bottom = rect.offset.y + (int)(((int64_t)(offset.y + eyeFoveatedResolution.h) *
                                                        rect.extent.height + eyeResolution.h - 1) / eyeResolution.h);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, depthBox.left);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, depthBox.top);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, depthBox.right - depthBox.left);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, depthBox.bottom - depthBox.top);
            }

            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Reset, false);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, true);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Style, settings.style);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Intensity, settings.intensity);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, settings.localToneStrength);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, settings.localStructureStrength);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, settings.skinStructureStrength);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);

            if (foveatedScale < 1.f) {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                src.pResource = inputImage;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                dst.pResource = m_dlssnrOutputSwapchainImages[imageIndex].Get();

                const auto ToD3dBox = [](D3D12_BOX& d3dBox, const BOX& box) {
                    d3dBox.left = box.left;
                    d3dBox.top = box.top;
                    d3dBox.front = 0;
                    d3dBox.right = box.right;
                    d3dBox.bottom = box.bottom;
                    d3dBox.back = 1;
                };

                D3D12_BOX d3dBox;
                BOX box{};
                box.left = views[eye]->imageRect.offset.x;
                box.top = views[eye]->imageRect.offset.y;
                box.right = box.left + eyeResolution.w;
                box.bottom = box.top + offset.y;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(&dst, eye * resolution.w, 0, 0, &src, &d3dBox);
                box.top = views[eye]->imageRect.offset.y + offset.y + eyeFoveatedResolution.h;
                box.bottom = views[eye]->imageRect.offset.y + eyeResolution.h;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(
                    &dst, eye * resolution.w, eyeResolution.h - (box.bottom - box.top), 0, &src, &d3dBox);

                box.left = views[eye]->imageRect.offset.x;
                box.top = views[eye]->imageRect.offset.y + offset.y;
                box.right = box.left + offset.x;
                box.bottom = box.top + eyeFoveatedResolution.h;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(
                    &dst, eye * resolution.w, box.top - views[eye]->imageRect.offset.y, 0, &src, &d3dBox);
                box.left = views[eye]->imageRect.offset.x + offset.x + eyeFoveatedResolution.w;
                box.right = views[eye]->imageRect.offset.x + eyeResolution.w;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(&dst,
                                                    eye * resolution.w + eyeResolution.w - (box.right - box.left),
                                                    box.top - views[eye]->imageRect.offset.y,
                                                    0,
                                                    &src,
                                                    &d3dBox);
            }

            // Go!
            CHECK_NGXCMD(ngx::NVSDK_NGX_D3D12_EvaluateFeature(
                cmdList.Commands.Get(), m_dlssnrFeature[eye], m_ngxParameters, nullptr));
        }

        // Serialize and commit output.
        const auto fenceValue = m_dlssnrContext->SubmitCommandList(cmdList);
        CHECK_HRCMD(m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fenceValue));
        CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, m_dlssnrOutputSwapchain));

        // Patch the layer.
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            layer.ColorTexture[eye] = m_dlssnrOutputSwapchain;
            layer.Viewport[eye].Pos = {(int)eye * resolution.w, 0};
            layer.Viewport[eye].Size = {views[eye]->imageRect.extent.width, views[eye]->imageRect.extent.height};
        }

        TraceLoggingWriteStop(local, "DLSSNR");
    }

    // For output swapchain resources.
    void OpenXrRuntime::ensureDlssnrSwapchainResources(ovrTextureFormat format, const ovrSizei& resolution) {
        if (m_dlssnrOutputSwapchain) {
            m_dlssnrOutputSwapchainImages.clear();
            ovr_DestroyTextureSwapChain(m_ovrSession, m_dlssnrOutputSwapchain);
            m_dlssnrOutputSwapchain = nullptr;
        }
        m_dlssnrOutputSwapchainResolution = {0, 0};
        m_dlssnrOutputSwapchainFormat = OVR_FORMAT_UNKNOWN;

        ovrTextureSwapChainDesc desc{};
        desc.Type = ovrTexture_2D;
        desc.ArraySize = 1;
        desc.Width = resolution.w * 2; // Double-wide.
        desc.Height = resolution.h;
        desc.MipLevels = 1;
        desc.SampleCount = 1;
        desc.Format = format;
        desc.BindFlags = ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess;
        desc.MiscFlags = ovrTextureMisc_DX_Typeless;
        CHECK_OVRCMD(
            ovr_CreateTextureSwapChainDX(m_ovrSession, m_ovrSubmissionDevice.Get(), &desc, &m_dlssnrOutputSwapchain));

        int count = -1;
        CHECK_OVRCMD(ovr_GetTextureSwapChainLength(m_ovrSession, m_dlssnrOutputSwapchain, &count));

        // Query the textures for the swapchain.
        for (int i = 0; i < count; i++) {
            ComPtr<ID3D11Texture2D> texture;
            CHECK_OVRCMD(ovr_GetTextureSwapChainBufferDX(
                m_ovrSession, m_dlssnrOutputSwapchain, i, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));

            // Export to the DLSS context.
            ComPtr<IDXGIResource1> dxgiResource;
            CHECK_HRCMD(texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf())));

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);

            wil::unique_handle ntHandle;
            HANDLE textureHandle;
            if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                CHECK_HRCMD(dxgiResource->GetSharedHandle(&textureHandle));
            } else {
                CHECK_HRCMD(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ntHandle.put()));
                textureHandle = ntHandle.get();
            }
            ComPtr<ID3D12Resource> d3d12Resource;
            CHECK_HRCMD(
                m_dlssnrDevice->OpenSharedHandle(textureHandle, IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf())));

            m_dlssnrOutputSwapchainImages.push_back(std::move(d3d12Resource));
        }
        m_dlssnrOutputSwapchainResolution = resolution;
        m_dlssnrOutputSwapchainFormat = format;
    }

    // For input swapchain(s) resources.
    void OpenXrRuntime::ensureSwapchainDlssnrResources(Swapchain& xrSwapchain, uint32_t slice) {
        if (xrSwapchain.resolvedSlices[slice].dlssnrImages.empty()) {
            // Query the textures for the swapchain.
            for (int i = 0; i < xrSwapchain.resolvedSlices[slice].images.size(); i++) {
                // Export to the DLSS context.
                wil::unique_handle handle;
                ComPtr<IDXGIResource1> dxgiResource;
                CHECK_HRCMD(xrSwapchain.resolvedSlices[slice].images[i]->QueryInterface(
                    IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf())));

                D3D11_TEXTURE2D_DESC desc{};
                xrSwapchain.resolvedSlices[slice].images[i]->GetDesc(&desc);

                wil::unique_handle ntHandle;
                HANDLE textureHandle;
                if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                    CHECK_HRCMD(dxgiResource->GetSharedHandle(&textureHandle));
                } else {
                    CHECK_HRCMD(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ntHandle.put()));
                    textureHandle = ntHandle.get();
                }
                ComPtr<ID3D12Resource> d3d12Resource;
                CHECK_HRCMD(m_dlssnrDevice->OpenSharedHandle(textureHandle,
                                                             IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf())));

                xrSwapchain.resolvedSlices[slice].dlssnrImages.push_back(std::move(d3d12Resource));
            }
        }
    }

    void OpenXrRuntime::initializeDlssnrResources() {
        // DLSS-NR requires a D3D12 context. Create one on the matching adapter.
        ComPtr<IDXGIDevice> dxgiDevice;
        CHECK_HRCMD(m_ovrSubmissionDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));

        ComPtr<IDXGIAdapter> dxgiAdapter;
        CHECK_HRCMD(dxgiDevice->GetAdapter(dxgiAdapter.ReleaseAndGetAddressOf()));
        CHECK_HRCMD(D3D12CreateDevice(
            dxgiAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_dlssnrDevice.ReleaseAndGetAddressOf())));

        m_dlssnrContext = std::make_unique<D3D12Utils::CommandContext>(m_dlssnrDevice.Get());

        // We'll use a fence to serialize pre-compositor output to DLSS-NR input.
        {
            wil::unique_handle fenceHandle;
            CHECK_HRCMD(m_ovrSubmissionCompletionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, fenceHandle.put()));
            CHECK_HRCMD(m_dlssnrDevice->OpenSharedHandle(fenceHandle.get(),
                                                         IID_PPV_ARGS(m_dlssnrInFence.ReleaseAndGetAddressOf())));
        }

        // We'll use a fence to serialize DLSS-NR output to OVR input.
        {
            wil::unique_handle handle;
            CHECK_HRCMD(m_dlssnrDevice->CreateSharedHandle(
                m_dlssnrContext->GetCompletionFence(), nullptr, GENERIC_ALL, nullptr, handle.put()));
            CHECK_HRCMD(m_ovrSubmissionDevice->OpenSharedFence(
                handle.get(), IID_PPV_ARGS(m_dlssnrOutFence.ReleaseAndGetAddressOf())));
        }

        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        const wchar_t* paths[] = {virtualdesktop_openxr::dllHome.c_str()};
#ifdef _DEBUG
        commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
#else
        commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
#endif
        commonInfo.PathListInfo.Path = paths;
        commonInfo.PathListInfo.Length = std::size(paths);
        CHECK_NGXCMD(NVSDK_NGX_D3D12_Init(
            123456, virtualdesktop_openxr::programData.c_str(), m_dlssnrDevice.Get(), &commonInfo));

        CHECK_NGXCMD(NVSDK_NGX_D3D12_GetCapabilityParameters(&m_ngxParameters));
        if (ngx::dlssnrModule) {
            CHECK_NGXCMD(ngx::Init_Ext(123456,
                                     virtualdesktop_openxr::programData.c_str(),
                                     m_dlssnrDevice.Get(),
                                     NVSDK_NGX_Version_API,
                                     nullptr));
            ngx::initializedDevice = m_dlssnrDevice;
        }
    }

    void OpenXrRuntime::cleanupDlssnrResources() {
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            if (m_dlssnrFeature[eye]) {
                ngx::NVSDK_NGX_D3D12_ReleaseFeature(m_dlssnrFeature[eye]);
                m_dlssnrFeature[eye] = nullptr;
            }
        }
        if (m_ngxParameters) {
            NVSDK_NGX_D3D12_DestroyParameters(m_ngxParameters);
            m_ngxParameters = nullptr;
        }
        // Shut down only our direct NR module. The public NGX SDK may serve the host's DLSS.
        if (ngx::initializedDevice) {
            CHECK_NGXCMD(ngx::Shutdown1(ngx::initializedDevice.Get()));
            ngx::initializedDevice.Reset();
        }
    }

    void OpenXrRuntime::unloadDlssnrModule() {
        // Device cleanup must finish before FreeLibrary, which invokes DLL destructors under the loader lock.
        CHECK_MSG(!ngx::initializedDevice, "DLSS-NR device must be shut down before module unload");
        DetourDllDetach("Kernel32.dll", "GetModuleFileNameW", ngx::hooked_GetModuleFileNameW,
                        ngx::original_GetModuleFileNameW);
        ngx::NVSDK_NGX_D3D12_CreateFeature = &::NVSDK_NGX_D3D12_CreateFeature;
        ngx::NVSDK_NGX_D3D12_ReleaseFeature = &::NVSDK_NGX_D3D12_ReleaseFeature;
        ngx::NVSDK_NGX_D3D12_EvaluateFeature = &::NVSDK_NGX_D3D12_EvaluateFeature;
        ngx::Init_Ext = nullptr;
        ngx::Shutdown1 = nullptr;
        ngx::dlssnrModule.reset();
    }

} // namespace virtualdesktop_openxr
