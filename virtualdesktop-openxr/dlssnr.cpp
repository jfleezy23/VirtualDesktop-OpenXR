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
#include "PairCaptureNrEngine.h"
#include "PairCaptureNrPostFx.h"
#include "PairCaptureNrWarmLayouts.h"

#include "log.h"
#include "runtime.h"
#include "PairCaptureNrSdk.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrReturnGuard.h"
#include "PairCaptureNrSources.h"
#include "PairCaptureNrModules.h"
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
    using InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(
        unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    InitExt Init_Ext{nullptr};
    decltype(&::NVSDK_NGX_D3D12_Shutdown1) Shutdown1{nullptr};
    ComPtr<ID3D12Device> initializedDevice;
    auto NVSDK_NGX_D3D12_CreateFeature = &::NVSDK_NGX_D3D12_CreateFeature;
    auto NVSDK_NGX_D3D12_ReleaseFeature = &::NVSDK_NGX_D3D12_ReleaseFeature;
    auto NVSDK_NGX_D3D12_EvaluateFeature = &::NVSDK_NGX_D3D12_EvaluateFeature;

    struct ImportPatch {
        PVOID* slot;
        PVOID original;
    };
    std::vector<ImportPatch> moduleNameImports;

    // The loader can unload a runtime even after DestroyInstance fails. Retain our code only on unsafe teardown;
    // otherwise a surviving NR import or device could refer to an unmapped DLL. Recovery then needs a process restart.
    void retainRuntimeOnCleanupFailure() noexcept {
        HMODULE runtime;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&retainRuntimeOnCleanupFailure),
                                &runtime)) {
            OutputDebugStringA("VDXR: could not retain runtime after NR cleanup failure\n");
        }
    }

    // Spoof only this NR module's import, rather than changing Kernel32 code for every thread in the host.
    DWORD WINAPI nrGetModuleFileNameW(HMODULE, LPWSTR filename, DWORD size) {
        constexpr wchar_t name[] = L"nvngx.dll";
        constexpr DWORD length = static_cast<DWORD>(std::size(name) - 1);
        if (!size) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (!filename) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
        const DWORD copied = std::min(length, size - 1);
        std::copy_n(name, copied, filename);
        filename[copied] = L'\0';
        if (size <= length) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        return length;
    }

    void restoreModuleNameImports() {
        while (!moduleNameImports.empty()) {
            const auto patch = moduleNameImports.back();
            DWORD protection;
            CHECK_MSG(VirtualProtect(patch.slot, sizeof(PVOID), PAGE_READWRITE, &protection),
                      "Cannot restore NR import protection");
            const bool restored = InterlockedCompareExchangePointer(
                                      patch.slot, patch.original, reinterpret_cast<PVOID>(&nrGetModuleFileNameW)) ==
                                  reinterpret_cast<PVOID>(&nrGetModuleFileNameW);
            // A later importer may still call our shim. Preserve its slot and retain both modules on failure.
            if (restored) {
                moduleNameImports.pop_back();
            }
            DWORD ignored;
            CHECK_MSG(VirtualProtect(patch.slot, sizeof(PVOID), protection, &ignored),
                      "Cannot restore NR import page protection");
            CHECK_MSG(restored, "NR import was replaced by another owner");
        }
    }

    void installModuleNameImports(HMODULE module) {
        std::vector<ImportPatch> imports;
        CHECK_MSG(DetourEnumerateImportsEx(module,
                                           &imports,
                                           nullptr,
                                           [](PVOID context, DWORD, LPCSTR name, PVOID* slot) -> BOOL {
                                               if (name && slot && std::string_view(name) == "GetModuleFileNameW") {
                                                   static_cast<std::vector<ImportPatch>*>(context)->push_back(
                                                       {slot, *slot});
                                               }
                                               return TRUE;
                                           }),
                  "Cannot enumerate NR imports");
        moduleNameImports.reserve(imports.size());
        for (const auto& patch : imports) {
            DWORD protection;
            CHECK_MSG(VirtualProtect(patch.slot, sizeof(PVOID), PAGE_READWRITE, &protection),
                      "Cannot update NR import protection");
            const bool installed = InterlockedCompareExchangePointer(patch.slot,
                                                                     reinterpret_cast<PVOID>(&nrGetModuleFileNameW),
                                                                     patch.original) == patch.original;
            if (installed) {
                moduleNameImports.push_back(patch);
            }
            DWORD ignored;
            CHECK_MSG(VirtualProtect(patch.slot, sizeof(PVOID), protection, &ignored),
                      "Cannot restore NR import page protection");
            CHECK_MSG(installed, "NR import changed during installation");
        }
    }

    void loadModule(const std::filesystem::path& path) {
        // Declared before candidate: original candidate destruction happens first.
        // A cleanup-failure move into the global owner remains a live lease.
        struct NrCandidateLease {
            HMODULE module{};
            ~NrCandidateLease() noexcept {
                if (module && dlssnrModule.get() != module)
                    virtualdesktop_openxr::nr_capture::observeManualModuleOwnerRelease(module);
            }
        } nrCandidateLease;
        const auto nrLoadBegin = virtualdesktop_openxr::nr_capture::hostRequest().valid
                                     ? virtualdesktop_openxr::nr_capture::sourceClock()
                                     : 0;
        wil::unique_hmodule candidate(LoadLibraryW(path.c_str()));
        if (virtualdesktop_openxr::nr_capture::hostRequest().valid)
            nrCandidateLease.module = candidate.get();
        if (virtualdesktop_openxr::nr_capture::hostRequest().valid)
            virtualdesktop_openxr::nr_capture::observeManualModuleLoad(
                candidate.get(), nrLoadBegin, virtualdesktop_openxr::nr_capture::sourceClock());
        CHECK_MSG(candidate.get(), "Failed to load DLSS-NR module");
        const auto init = reinterpret_cast<InitExt>(GetProcAddress(candidate.get(), "NVSDK_NGX_D3D12_Init_Ext"));
        const auto shutdown =
            reinterpret_cast<decltype(Shutdown1)>(GetProcAddress(candidate.get(), "NVSDK_NGX_D3D12_Shutdown1"));
        const auto create = reinterpret_cast<decltype(NVSDK_NGX_D3D12_CreateFeature)>(
            GetProcAddress(candidate.get(), "NVSDK_NGX_D3D12_CreateFeature"));
        const auto release = reinterpret_cast<decltype(NVSDK_NGX_D3D12_ReleaseFeature)>(
            GetProcAddress(candidate.get(), "NVSDK_NGX_D3D12_ReleaseFeature"));
        const auto evaluate = reinterpret_cast<decltype(NVSDK_NGX_D3D12_EvaluateFeature)>(
            GetProcAddress(candidate.get(), "NVSDK_NGX_D3D12_EvaluateFeature"));
        CHECK_MSG(init && shutdown && create && release && evaluate, "DLSS-NR module is missing required exports");
        try {
            installModuleNameImports(candidate.get());
        } catch (...) {
            try {
                restoreModuleNameImports();
            } catch (...) {
                // A live import must never outlive its target. Keep both modules if rollback itself fails.
                retainRuntimeOnCleanupFailure();
                dlssnrModule = std::move(candidate);
            }
            throw;
        }
        // Publish only the complete table. A rejected DLL must leave the SDK fallback callable for the next frame.
        dlssnrModule = std::move(candidate);
        Init_Ext = init;
        Shutdown1 = shutdown;
        NVSDK_NGX_D3D12_CreateFeature = create;
        NVSDK_NGX_D3D12_ReleaseFeature = release;
        NVSDK_NGX_D3D12_EvaluateFeature = evaluate;
        if (virtualdesktop_openxr::nr_capture::hostRequest().valid) {
            const uint64_t nrFunctions[5] = {reinterpret_cast<uint64_t>(init),
                                             reinterpret_cast<uint64_t>(shutdown),
                                             reinterpret_cast<uint64_t>(create),
                                             reinterpret_cast<uint64_t>(evaluate),
                                             reinterpret_cast<uint64_t>(release)};
            virtualdesktop_openxr::nr_capture::observeManualModulePublication(dlssnrModule.get(), nrFunctions);
        }
    }

} // namespace ngx

namespace virtualdesktop_openxr {

    using namespace virtualdesktop_openxr::log;
    using namespace virtualdesktop_openxr::utils;

    void OpenXrRuntime::createDlssnrFeature(uint32_t eye,
                                            const ovrSizei& resolution,
                                            ID3D12GraphicsCommandList* commands) {
        if (!m_dlssnrFeature[eye]) {
            nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Width, resolution.w);
            nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Height, resolution.h);
            // Select the lightweight network explicitly; the appearance Style is independent.
            nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, 2 /* Preset B */);
            nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

            // Keep each opaque feature paired with the dispatch table that created it.
            auto release = ngx::NVSDK_NGX_D3D12_ReleaseFeature;
            auto evaluate = ngx::NVSDK_NGX_D3D12_EvaluateFeature;
            const auto create = ngx::NVSDK_NGX_D3D12_CreateFeature;
            // Only complete loadModule publication replaces this original SDK
            // pointer. A retained module alone does not identify the active table.
            const auto createKind = create == &::NVSDK_NGX_D3D12_CreateFeature ? NR_CREATE_SDK : NR_CREATE_MANUAL;
            if (ngx::forceManualLoad || NVSDK_NGX_FAILED(nr_capture::ObserveSdkCall(
                                            createKind,
                                            eye,
                                            reinterpret_cast<uint64_t>(create),
                                            commands,
                                            m_ngxParameters,
                                            &m_dlssnrFeature[eye],
                                            reinterpret_cast<uint64_t>(evaluate),
                                            reinterpret_cast<uint64_t>(release),
                                            [&] {
                                                return create(commands,
                                                              NVSDK_NGX_Feature_Reserved18 /* DLSS-NR */,
                                                              m_ngxParameters,
                                                              &m_dlssnrFeature[eye]);
                                            }))) {
                // User might be using an unsigned nvngx_dlssnr.dll that gets bounced by the NGX SDK. Try loading it
                // manually.
                if (!ngx::dlssnrModule) {
                    const auto nvngx_dlssnrPath = virtualdesktop_openxr::dllHome / L"nvngx_dlssnr.dll";
                    ngx::loadModule(nvngx_dlssnrPath);
                }

                if (ngx::initializedDevice.Get() != m_dlssnrDevice.Get()) {
                    CHECK_MSG(ngx::Init_Ext && ngx::Shutdown1, "DLSS-NR module load did not complete");
                    CHECK_NGXCMD(nr_capture::ObserveSdkInit(
                        reinterpret_cast<uint64_t>(ngx::Init_Ext), m_dlssnrDevice.Get(), [&] {
                            return ngx::Init_Ext(123456,
                                                 virtualdesktop_openxr::programData.c_str(),
                                                 m_dlssnrDevice.Get(),
                                                 NVSDK_NGX_Version_API,
                                                 nullptr);
                        }));
                    ngx::initializedDevice = m_dlssnrDevice;
                }

                release = ngx::NVSDK_NGX_D3D12_ReleaseFeature;
                evaluate = ngx::NVSDK_NGX_D3D12_EvaluateFeature;
                const auto manualCreate = ngx::NVSDK_NGX_D3D12_CreateFeature;
                CHECK_NGXCMD(nr_capture::ObserveSdkCall(NR_CREATE_MANUAL,
                                                        eye,
                                                        reinterpret_cast<uint64_t>(manualCreate),
                                                        commands,
                                                        m_ngxParameters,
                                                        &m_dlssnrFeature[eye],
                                                        reinterpret_cast<uint64_t>(evaluate),
                                                        reinterpret_cast<uint64_t>(release),
                                                        [&] {
                                                            return manualCreate(
                                                                commands,
                                                                NVSDK_NGX_Feature_Reserved18 /* DLSS-NR */,
                                                                m_ngxParameters,
                                                                &m_dlssnrFeature[eye]);
                                                        }));
            }
            m_dlssnrFeatureRelease[eye] = release;
            m_dlssnrFeatureEvaluate[eye] = evaluate;
        }
    }

    // Caller must have established completion of all NR work using these features.
    void OpenXrRuntime::releaseDlssnrFeatures() {
        for (uint32_t eye = 0; eye < xr::StereoView::Count; ++eye) {
            if (m_dlssnrFeature[eye]) {
                CHECK_NGXCMD(
                    nr_capture::ObserveSdkCall(NR_RELEASE_FEATURE,
                                               eye,
                                               reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                                               nullptr,
                                               m_ngxParameters,
                                               &m_dlssnrFeature[eye],
                                               reinterpret_cast<uint64_t>(m_dlssnrFeatureEvaluate[eye]),
                                               reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                                               [&] { return m_dlssnrFeatureRelease[eye](m_dlssnrFeature[eye]); }));
                m_dlssnrFeature[eye] = nullptr;
                m_dlssnrFeatureRelease[eye] = nullptr;
                m_dlssnrFeatureEvaluate[eye] = nullptr;
            }
        }
        m_dlssnrFeatureResolution = {0, 0};
        m_nrSpatialFeatureExtents = {};
        m_nrSpatialFeatures = false;
        m_nrSpatialLastFrame = UINT64_MAX;
        m_nrSpatialSourceChains = {};
    }

    void OpenXrRuntime::upliftLayer(const XrSwapchainSubImage** views,
                                    const XrSwapchainSubImage** depth,
                                    const bool* depthInverted,
                                    ovrLayerEyeFov& layer,
                                    bool finalColour,
                                    std::unique_lock<std::mutex>* producerLease) {
        // Recovery precedes route selection: changing processing/format must not
        // bypass an uncertain submission's retained descriptors and resources.
        if (m_nrNativeNeedsDrain || m_nrNativeNeedsFeatureReset || m_nrSpatialNeedsDrain ||
            m_nrSpatialNeedsFeatureReset) {
            m_dlssnrContext->Flush();
            if (m_nrNativePipeline)
                m_nrNativePipeline->DiscardRecording();
            if (m_nrSpatialPipeline)
                m_nrSpatialPipeline->DiscardRecording();
            releaseDlssnrFeatures();
            m_nrNativeNeedsDrain = false;
            m_nrNativeNeedsFeatureReset = false;
            m_nrSpatialNeedsDrain = false;
            m_nrSpatialNeedsFeatureReset = false;
        }
        if (m_precompositor.dlssnrSettings.processingResolution < 100 &&
            upliftSpatialLayer(views, depth, depthInverted, layer, finalColour, producerLease)) {
            // Entering spatial mode already drained the previous native features
            // before preparing its own generation. Retire the inactive scratch.
            if (m_nrNativePipeline) {
                m_nrNativePipeline->DiscardRecording();
                m_nrNativePipeline.reset();
            }
            return;
        }
        // Retire an optional generation before returning to the unchanged native route.
        if (m_nrSpatialPipeline) {
            m_dlssnrContext->Flush();
            m_nrSpatialPipeline->DiscardRecording();
            m_nrSpatialPipeline.reset();
            releaseDlssnrFeatures();
            m_nrSpatialNeedsDrain = false;
            m_nrSpatialNeedsFeatureReset = false;
        }
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

        const BOX outputBox[2] = {
            {0, 0, views[0]->imageRect.extent.width, views[0]->imageRect.extent.height},
            {resolution.w, 0, resolution.w + views[1]->imageRect.extent.width, views[1]->imageRect.extent.height}};

        // Resize resources if needed.
        const auto format = ((Swapchain*)views[0]->swapchain)->ovrDesc.Format;
        if (!m_dlssnrOutputSwapchain || m_dlssnrOutputSwapchainResolution.w < resolution.w ||
            m_dlssnrOutputSwapchainResolution.h < resolution.h || m_dlssnrOutputSwapchainFormat != format) {
            // Finish in-flight work before releasing the resources.
            flushSubmissionContext();
            m_dlssnrContext->Flush();
            if (m_nrNativePipeline) {
                m_nrNativePipeline->DiscardRecording();
                m_nrNativePipeline->Clear();
            }
            ensureDlssnrSwapchainResources(format, resolution);
        }
        if (m_dlssnrFeatureResolution.w < foveatedResolution.w || m_dlssnrFeatureResolution.h < foveatedResolution.h) {
            // Finish in-flight work before releasing the resources.
            m_dlssnrContext->Flush();

            // Destroy DLSS-NR feature - it will be re-created in the loop further below.
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                if (m_dlssnrFeature[eye]) {
                    CHECK_NGXCMD(
                        nr_capture::ObserveSdkCall(NR_RELEASE_FEATURE,
                                                   eye,
                                                   reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                                                   nullptr,
                                                   m_ngxParameters,
                                                   &m_dlssnrFeature[eye],
                                                   reinterpret_cast<uint64_t>(m_dlssnrFeatureEvaluate[eye]),
                                                   reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                                                   [&] { return m_dlssnrFeatureRelease[eye](m_dlssnrFeature[eye]); }));
                    m_dlssnrFeature[eye] = nullptr;
                    m_dlssnrFeatureRelease[eye] = nullptr;
                    m_dlssnrFeatureEvaluate[eye] = nullptr;
                }
            }

            m_dlssnrFeatureResolution = foveatedResolution;
        }

        int imageIndex = 0;
        CHECK_OVRCMD(submissionSwapchainIndex(m_ovrSession, m_dlssnrOutputSwapchain, &imageIndex));
        nr_spatial::FrameInput nativeInput{};
        nativeInput.nativeAnswer = true;
        nativeInput.output = m_dlssnrOutputSwapchainImages.at(imageIndex).Get();
        nativeInput.outputFormat = ovrToDxgiTextureFormat(format);
        nativeInput.outputEyeStride = resolution.w;
        bool guard = nr_spatial::IsSupportedColorFormat(nativeInput.outputFormat);
        for (uint32_t eye = 0; eye < 2 && guard; ++eye) {
            auto& swapchain = *reinterpret_cast<Swapchain*>(views[eye]->swapchain);
            auto& source = swapchain.resolvedSlices[views[eye]->imageArrayIndex];
            const auto& rect = views[eye]->imageRect;
            auto& target = nativeInput.eyes[eye];
            target.colorFormat = swapchain.dxgiFormatForSubmission;
            guard = rect.offset.x >= 0 && rect.offset.y >= 0 && rect.extent.width > 0 && rect.extent.height > 0 &&
                    source.lastCommittedIndex >= 0 &&
                    static_cast<size_t>(source.lastCommittedIndex) < source.dlssnrImages.size();
            if (!guard)
                break;
            target.color = source.dlssnrImages[source.lastCommittedIndex].Get();
            target.colorRect = {static_cast<uint32_t>(rect.offset.x),
                                static_cast<uint32_t>(rect.offset.y),
                                static_cast<uint32_t>(rect.extent.width),
                                static_cast<uint32_t>(rect.extent.height)};
            // Exactly the legacy vendor crop, including its alignment and centering.
            const uint32_t width = std::min(
                target.colorRect.width, xr::math::AlignTo<4>(static_cast<uint32_t>(rect.extent.width * foveatedScale)));
            const uint32_t height =
                std::min(target.colorRect.height,
                         xr::math::AlignTo<4>(static_cast<uint32_t>(rect.extent.height * foveatedScale)));
            target.processingRect = {
                (target.colorRect.width - width) / 2, (target.colorRect.height - height) / 2, width, height};
        }
        if (guard) {
            try {
                if (!m_nrNativePipeline)
                    m_nrNativePipeline = std::make_unique<nr_spatial::Pipeline>(m_dlssnrDevice.Get(),
                                                                                m_dlssnrContext->GetCompletionFence());
            } catch (const std::bad_alloc&) {
                guard = false;
            }
            if (guard && m_nrNativePipeline->GetNativeAnswer() && !m_nrNativePipeline->Matches(nativeInput)) {
                m_dlssnrContext->Flush();
                m_nrNativePipeline->DiscardRecording();
                m_nrNativePipeline->Clear();
            }
            if (guard) {
                const auto prepared = m_nrNativePipeline->Prepare(nativeInput);
                if (prepared == DXGI_ERROR_DEVICE_REMOVED || prepared == DXGI_ERROR_DEVICE_RESET)
                    CHECK_HRCMD(prepared);
                guard = SUCCEEDED(prepared);
            }
        }
        if (m_nrNativeStatus != (guard ? 1 : 2)) {
            Log("NR native color protection: %s",
                guard ? "direct resolve active" : "unqualified format/view or unavailable resources; legacy fallback");
            m_nrNativeStatus = guard ? 1 : 2;
        }

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
            // Serialize inputs.
            m_submissionFenceValue++;
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

            // Time to apply the effect.
            auto cmdList = m_dlssnrContext->GetCommandList();
            vdxrfg::pair_capture::nr::NrRecordingOwnership nrRecordingFacts{};
            if (m_dlssnrContext->BorrowRecordingOwnership(cmdList, nrRecordingFacts))
                nr_capture::sdkRecording(m_frameBegun, m_dlssnrContext.get(), nrRecordingFacts);

            ID3D12Resource* nrNativeDepth[2]{};
            if (nr_capture::hostRequest().valid && depth[0] && depth[1]) {
                // Cache both actual sources before eye zero can mutate the parameter table.
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    if (!m_swapchains.count(depth[eye]->swapchain))
                        break;
                    const auto& source = *reinterpret_cast<const Swapchain*>(depth[eye]->swapchain);
                    if (depth[eye]->imageArrayIndex >= source.resolvedSlices.size())
                        break;
                    const auto& slice = source.resolvedSlices[depth[eye]->imageArrayIndex];
                    if (slice.lastCommittedIndex < 0 ||
                        static_cast<size_t>(slice.lastCommittedIndex) >= slice.dlssnrImages.size())
                        break;
                    nrNativeDepth[eye] = slice.dlssnrImages[slice.lastCommittedIndex].Get();
                }
            }
            if (nrFrame < 2) {
                nr_capture::CaptureNrInput nrSelected{};
                nrSelected.recording = nrRecordingFacts;
                nrSelected.pipelineObject = m_nrNativePipeline.get();
                CHECK_MSG(m_nrNativePipeline && m_nrNativePipeline->BorrowActiveOwnership(nrSelected.pipeline) &&
                              m_nrNativePipeline->BorrowActivePrograms(nrSelected.programs),
                          "private NR actual pipeline borrow failed");
                nrSelected.nativeDepth[0] = nrNativeDepth[0];
                nrSelected.nativeDepth[1] = nrNativeDepth[1];
                CHECK_MSG(m_dlssnrContext->TryObserveNextSubmission(nrSelected.prospectiveContextLifetime,
                                                                    nrSelected.prospectiveSignalValue) &&
                              nrEngine.acquireNr(nrFrame, nrSelected),
                          "private NR late submission reservation or acquisition failed");
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    nrCapture(NR_MODEL_COLOUR, eye);
                    nrCapture(NR_MODEL_DEPTH, eye);
                }
            }
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                const ovrSizei eyeResolution = {views[eye]->imageRect.extent.width,
                                                views[eye]->imageRect.extent.height};
                const ovrSizei eyeFoveatedResolution = {
                    std::min(eyeResolution.w, (int)xr::math::AlignTo<4>((uint32_t)(eyeResolution.w * foveatedScale))),
                    std::min(eyeResolution.h, (int)xr::math::AlignTo<4>((uint32_t)(eyeResolution.h * foveatedScale)))};
                const ovrVector2i offset = {(eyeResolution.w - eyeFoveatedResolution.w) / 2,
                                            (eyeResolution.h - eyeFoveatedResolution.h) / 2};
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthInverted, depthInverted[eye]);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Enabled, true);

                createDlssnrFeature(eye, foveatedResolution, cmdList.Commands.Get());

                // Prepare feature evaluation.
                nr_capture::ObserveSet(m_ngxParameters,
                                       NVSDK_NGX_Parameter_DLSSNR_Output,
                                       guard ? m_nrNativePipeline->GetNativeAnswer() : nativeInput.output);

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
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Color, inputImage);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, colorBox.left);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, colorBox.top);
                nr_capture::ObserveSet(
                    m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, colorBox.right - colorBox.left);
                nr_capture::ObserveSet(
                    m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, colorBox.bottom - colorBox.top);

                BOX destBox{};
                destBox.left = eye * resolution.w + offset.x; // Double-wide.
                destBox.top = offset.y;
                destBox.right = destBox.left + eyeFoveatedResolution.w;
                destBox.bottom = destBox.top + eyeFoveatedResolution.h;
                SafeClamp(destBox, outputBox[eye]);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, destBox.left);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, destBox.top);
                nr_capture::ObserveSet(
                    m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, destBox.right - destBox.left);
                nr_capture::ObserveSet(
                    m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, destBox.bottom - destBox.top);

#if 0
            // TODO: Use NVOFA to estimate motion.
            nr_capture::ObserveSet(m_ngxParameters,NVSDK_NGX_Parameter_DLSSNR_MVec, motion);
            nr_capture::ObserveSet(m_ngxParameters,NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0);
            nr_capture::ObserveSet(m_ngxParameters,NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0);
            nr_capture::ObserveSet(m_ngxParameters,NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, w);
            nr_capture::ObserveSet(m_ngxParameters,NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, h);
            nr_capture::ObserveSet(m_ngxParameters,NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, motionVectorScaleX);
            nr_capture::ObserveSet(m_ngxParameters,NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, motionVectorScaleY);
#endif

                if (depth[0] && depth[1]) {
                    Swapchain& xrDepthSwapchain = *(Swapchain*)depth[eye]->swapchain;
                    nr_capture::ObserveSet(
                        m_ngxParameters,
                        NVSDK_NGX_Parameter_DLSSNR_Depth,
                        xrDepthSwapchain.resolvedSlices[depth[eye]->imageArrayIndex]
                            .dlssnrImages[xrDepthSwapchain.resolvedSlices[depth[eye]->imageArrayIndex]
                                              .lastCommittedIndex]
                            .Get());
                    BOX depthBox{};
                    const auto& rect = depth[eye]->imageRect;
                    // Map the color crop into the depth image's own coordinates, including its offset.
                    depthBox.left = rect.offset.x + (int)((int64_t)offset.x * rect.extent.width / eyeResolution.w);
                    depthBox.top = rect.offset.y + (int)((int64_t)offset.y * rect.extent.height / eyeResolution.h);
                    depthBox.right =
                        rect.offset.x + (int)(((int64_t)(offset.x + eyeFoveatedResolution.w) * rect.extent.width +
                                               eyeResolution.w - 1) /
                                              eyeResolution.w);
                    depthBox.bottom =
                        rect.offset.y + (int)(((int64_t)(offset.y + eyeFoveatedResolution.h) * rect.extent.height +
                                               eyeResolution.h - 1) /
                                              eyeResolution.h);
                    nr_capture::ObserveSet(
                        m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, depthBox.left);
                    nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, depthBox.top);
                    nr_capture::ObserveSet(
                        m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, depthBox.right - depthBox.left);
                    nr_capture::ObserveSet(
                        m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, depthBox.bottom - depthBox.top);
                } else {
                    // The parameter map persists across frames. Clear all optional depth bindings when unavailable.
                    nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Depth, (ID3D12Resource*)nullptr);
                    nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, 0);
                    nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, 0);
                    nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, 0);
                    nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, 0);
                }

                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Reset, false);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, true);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Style, settings.style);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_Intensity, settings.intensity);
                nr_capture::ObserveSet(
                    m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, settings.localToneStrength);
                nr_capture::ObserveSet(m_ngxParameters,
                                       NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength,
                                       settings.localStructureStrength);
                nr_capture::ObserveSet(
                    m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, settings.skinStructureStrength);
                nr_capture::ObserveSet(m_ngxParameters, NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);

                if (foveatedScale < 1.f && !guard) {
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
                CHECK_NGXCMD(nr_capture::ObserveSdkCall(NR_EVALUATE,
                                                        eye,
                                                        reinterpret_cast<uint64_t>(m_dlssnrFeatureEvaluate[eye]),
                                                        cmdList.Commands.Get(),
                                                        m_ngxParameters,
                                                        &m_dlssnrFeature[eye],
                                                        reinterpret_cast<uint64_t>(m_dlssnrFeatureEvaluate[eye]),
                                                        reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                                                        [&] {
                                                            return m_dlssnrFeatureEvaluate[eye](cmdList.Commands.Get(),
                                                                                                m_dlssnrFeature[eye],
                                                                                                m_ngxParameters,
                                                                                                nullptr);
                                                        }));
            }

            if (nrFrame < 2) {
                nrCapture(NR_NATIVE_ANSWER, 2);
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    nrCapture(NR_POST_EVAL_COLOUR, eye);
                    nrCapture(NR_PRE_RESOLVE_COLOUR, eye);
                }
            }
            // Serialize and commit output.
            if (guard)
                m_nrNativePipeline->RecordResolve(cmdList.Commands.Get());
            nrCapture(NR_POST_RESOLVE, 2);
            vdxrfg::pair_capture::nr::NrPipelineOwnership nrWarmGraph{};
            const bool nrWarmInventory =
                nr_capture::hostRequest().valid && m_nrNativePipeline &&
                m_nrNativePipeline->BorrowActiveOwnership(nrWarmGraph) &&
                nrWarmGraph.observedEpochs == vdxrfg::pair_capture::nr::NrPipelineOwnership::allEpochs &&
                nrWarmGraph.device == nrRecordingFacts.device && nrWarmGraph.fence == nrRecordingFacts.fence &&
                nrWarmGraph.nativeAnswer;
            if (nrWarmInventory)
                nr_capture::warmLayoutGraph12(m_frameBegun,
                                              NR_NATIVE100,
                                              nrRecordingFacts.facts.nrCommandContextLifetimeEpoch,
                                              nrWarmGraph,
                                              nrNativeDepth);
            auto* nrToken = nrFrame < 2 ? nrEngine.beginNrSubmission(nrFrame) : nullptr;
            CHECK_MSG(nrFrame >= 2 || nrToken, "private NR submission token unavailable");
            submitAttempted = true;
            nrSubmitBegin = nr_capture::sourceClock();
            const auto fenceValue = m_dlssnrContext->SubmitCommandList(
                std::move(cmdList), nrToken, nr_capture::hostRequest().valid ? &nrWarmSubmission : nullptr);
            const auto nrSubmitEnd = nr_capture::sourceClock();
            if (auto* returnGuard = nr_capture::SourceReturnGuard::current())
                returnGuard->observeSubmission(nrWarmSubmission, nrSubmitBegin, nrSubmitEnd);
            if (nrFrame < 2)
                CHECK_MSG(nrReturnGuard &&
                              nrEngine.finishNrSubmission(nrFrame, nrWarmSubmission, fenceValue, *nrReturnGuard),
                          "private NR actual submission facts did not reconcile");
            CHECK_MSG(fenceValue && fenceValue != UINT64_MAX, "NR submission did not return a valid completion fence");
            if (guard)
                m_nrNativePipeline->MarkSubmitted(fenceValue);
            registered = true;
            // Future NR must not block the active group's SDK GPU handoffs.
            waitForFrameGenerationProducerFence(m_dlssnrOutFence.Get(), fenceValue, producerLease, "NR-output");
            const auto nrOutputWaitBegin = nr_capture::sourceClock();
            const auto nrOutputWaitResult = m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fenceValue);
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
                                                       fenceValue);
                if (auto* returnGuard = nr_capture::SourceReturnGuard::current()) {
                    returnGuard->observeNr(nrWarmSubmission, completeSubmission);
                    returnGuard->observeOutputWait(m_ovrSubmissionContext.Get(),
                                                   m_dlssnrOutFence.Get(),
                                                   fenceValue,
                                                   nrOutputWaitResult,
                                                   nrOutputWaitBegin,
                                                   nrOutputWaitEnd);
                }
                nr_capture::sdkWarmFacts(nrSdkWarm);
                nr_capture::hostWarmFrame(m_frameBegun,
                                          nrRecordingFacts.facts.nrCommandContextLifetimeEpoch,
                                          nrSdkWarm,
                                          fenceValue,
                                          completeSubmission ? nrWarmSubmission.calls.original.result : E_FAIL,
                                          nrOutputWaitResult,
                                          nrWarmInventory && completeSubmission);
            }
            CHECK_HRESULT(nrOutputWaitResult, "m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fenceValue)");
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
            m_nrNativeNeedsFeatureReset = true;
            if (!submitAttempted) {
                if (guard)
                    m_nrNativePipeline->DiscardRecording();
            } else if (!registered) {
                m_nrNativeNeedsDrain = true;
            }
            throw;
        }
        // Later sharpening/upscaling can replace this intermediate. Commit only
        // if a final submitted layer still references the NR chain.
        m_precompositor.pendingColourCommits[0] = m_dlssnrOutputSwapchain;
        m_dlssnrOutputSwapchainLastWrittenIndex = imageIndex;

        // Both eye rectangles reference one packed texture. Later postprocessing
        // supplies its own final textures, so only retain this one when final.
        if (m_frameGenerationMode && finalColour) {
            m_precompositor.finalColour[0] = m_dlssnrOutputSwapchainTextures[imageIndex];
            m_precompositor.finalColour[1] = m_precompositor.finalColour[0];
        }

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
        m_dlssnrOutputSwapchainImages.clear();
        m_dlssnrOutputSwapchainTextures.clear();
        m_dlssnrOutputSwapchainSrvs.clear();
        if (m_dlssnrOutputSwapchain) {
            destroySubmissionSwapchain(m_ovrSession, m_dlssnrOutputSwapchain);
            m_dlssnrOutputSwapchain = nullptr;
        }
        m_dlssnrOutputSwapchainLastWrittenIndex = -1;
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
            createSubmissionSwapchain(m_ovrSession, m_ovrSubmissionDevice.Get(), &desc, &m_dlssnrOutputSwapchain));

        int count = -1;
        CHECK_OVRCMD(submissionSwapchainLength(m_ovrSession, m_dlssnrOutputSwapchain, &count));
        m_dlssnrOutputSwapchainImages.reserve(count);
        m_dlssnrOutputSwapchainTextures.reserve(count);
        m_dlssnrOutputSwapchainSrvs.reserve(count);

        // Query the textures for the swapchain.
        for (int i = 0; i < count; i++) {
            ComPtr<ID3D11Texture2D> texture;
            CHECK_OVRCMD(submissionSwapchainBuffer(
                m_ovrSession, m_dlssnrOutputSwapchain, i, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
            nr_capture::nrBankTextureObserved(texture.Get(), m_ovrSubmissionDevice.Get());

            // A typed SRV preserves the same sRGB decoding used by the compositor.
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Format = getShaderResourceViewFormat(ovrToDxgiTextureFormat(format));
            srvDesc.Texture2D.MipLevels = 1;
            ComPtr<ID3D11ShaderResourceView> srv;
            CHECK_HRCMD(m_ovrSubmissionDevice->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf()));
            nr_capture::postFxSrvCreated(texture.Get(), srv.Get(), srvDesc);

            // Export to the DLSS context.
            ComPtr<IDXGIResource1> dxgiResource;
            const auto nrQueryBegin = nr_capture::sourceClock();
            const auto nrQueryResult = texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf()));
            nr_capture::sourceNrQueryObserved(
                texture.Get(), dxgiResource.Get(), nrQueryResult, nrQueryBegin, nr_capture::sourceClock());
            CHECK_HRESULT(nrQueryResult,
                          "texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf()))");

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);

            wil::unique_handle ntHandle;
            HANDLE textureHandle;
            if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                const auto nrExportBegin = nr_capture::sourceClock();
                const auto nrExportResult = dxgiResource->GetSharedHandle(&textureHandle);
                nr_capture::sourceNrExportObserved(texture.Get(),
                                                   SUCCEEDED(nrExportResult) ? textureHandle : nullptr,
                                                   false,
                                                   0,
                                                   nrExportResult,
                                                   nrExportBegin,
                                                   nr_capture::sourceClock());
                CHECK_HRESULT(nrExportResult, "dxgiResource->GetSharedHandle(&textureHandle)");
            } else {
                const auto nrExportBegin = nr_capture::sourceClock();
                auto nrExportResult = m_deliveryTransport
                                          ? m_deliveryTransport->duplicateSharedHandle(texture.Get(), ntHandle.put())
                                          : S_FALSE;
                if (nrExportResult == S_FALSE)
                    nrExportResult = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ntHandle.put());
                nr_capture::sourceNrExportObserved(texture.Get(),
                                                   ntHandle.get(),
                                                   true,
                                                   GENERIC_ALL,
                                                   nrExportResult,
                                                   nrExportBegin,
                                                   nr_capture::sourceClock());
                CHECK_HRESULT(nrExportResult,
                              "dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ntHandle.put())");
                textureHandle = ntHandle.get();
            }
            ComPtr<ID3D12Resource> d3d12Resource;
            const auto nrImportBegin = nr_capture::sourceClock();
            const auto nrImportResult =
                m_dlssnrDevice->OpenSharedHandle(textureHandle, IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf()));
            if (SUCCEEDED(nrImportResult))
                nr_capture::sdkResourceCreated(d3d12Resource.Get());
            nr_capture::sourceNrImportObserved(texture.Get(),
                                               m_dlssnrDevice.Get(),
                                               d3d12Resource.Get(),
                                               nr_capture::sdkObservedResourceLifetime(d3d12Resource.Get()),
                                               textureHandle,
                                               (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0,
                                               nrImportResult,
                                               nrImportBegin,
                                               nr_capture::sourceClock());
            CHECK_HRESULT(nrImportResult,
                          "m_dlssnrDevice->OpenSharedHandle(textureHandle, "
                          "IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf()))");
            // Publish the same bank only after all its resource setup succeeds.
            m_dlssnrOutputSwapchainImages.push_back(std::move(d3d12Resource));
            m_dlssnrOutputSwapchainTextures.push_back(std::move(texture));
            m_dlssnrOutputSwapchainSrvs.push_back(std::move(srv));
        }
        m_dlssnrOutputSwapchainResolution = resolution;
        m_dlssnrOutputSwapchainFormat = format;
    }

    // For input swapchain(s) resources.
    void OpenXrRuntime::ensureSwapchainDlssnrResources(Swapchain& xrSwapchain, uint32_t slice) {
        auto& resolved = xrSwapchain.resolvedSlices[slice];
        if (resolved.dlssnrImages.empty()) {
            std::vector<ComPtr<ID3D12Resource>> candidate;
            candidate.reserve(resolved.images.size());
            // Query the textures for the swapchain.
            for (const auto& image : resolved.images) {
                // Export to the DLSS context.
                ComPtr<IDXGIResource1> dxgiResource;
                const auto nrQueryBegin = nr_capture::sourceClock();
                const auto nrQueryResult = image.As(&dxgiResource);
                nr_capture::sourceNrQueryObserved(
                    image.Get(), dxgiResource.Get(), nrQueryResult, nrQueryBegin, nr_capture::sourceClock());
                CHECK_HRESULT(nrQueryResult, "image.As(&dxgiResource)");

                D3D11_TEXTURE2D_DESC desc{};
                image->GetDesc(&desc);

                HANDLE textureHandle;
                if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                    const auto nrExportBegin = nr_capture::sourceClock();
                    const auto nrExportResult = dxgiResource->GetSharedHandle(&textureHandle);
                    nr_capture::sourceNrExportObserved(image.Get(),
                                                       SUCCEEDED(nrExportResult) ? textureHandle : nullptr,
                                                       false,
                                                       0,
                                                       nrExportResult,
                                                       nrExportBegin,
                                                       nr_capture::sourceClock());
                    CHECK_HRESULT(nrExportResult, "dxgiResource->GetSharedHandle(&textureHandle)");
                } else {
                    ComPtr<IUnknown> identity;
                    CHECK_HRCMD(image.As(&identity));
                    auto [entry, inserted] = xrSwapchain.dlssnrNtTextureHandles.try_emplace(identity.Get());
                    auto& [resource, ownedHandle] = entry->second;
                    if (inserted) {
                        resource = std::move(identity);
                    }
                    // A successful NT export cannot be recreated, even when the subsequent import fails.
                    if (!ownedHandle) {
                        const auto nrExportBegin = nr_capture::sourceClock();
                        auto nrExportResult =
                            m_deliveryTransport
                                ? m_deliveryTransport->duplicateSharedHandle(image.Get(), ownedHandle.put())
                                : S_FALSE;
                        if (nrExportResult == S_FALSE)
                            nrExportResult =
                                dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ownedHandle.put());
                        nr_capture::sourceNrExportObserved(image.Get(),
                                                           ownedHandle.get(),
                                                           true,
                                                           GENERIC_ALL,
                                                           nrExportResult,
                                                           nrExportBegin,
                                                           nr_capture::sourceClock());
                        CHECK_HRESULT(
                            nrExportResult,
                            "dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ownedHandle.put())");
                    }
                    textureHandle = ownedHandle.get();
                }
                ComPtr<ID3D12Resource> d3d12Resource;
                const auto nrImportBegin = nr_capture::sourceClock();
                const auto nrImportResult = m_dlssnrDevice->OpenSharedHandle(
                    textureHandle, IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf()));
                if (SUCCEEDED(nrImportResult))
                    nr_capture::sdkResourceCreated(d3d12Resource.Get());
                nr_capture::sourceNrImportObserved(image.Get(),
                                                   m_dlssnrDevice.Get(),
                                                   d3d12Resource.Get(),
                                                   nr_capture::sdkObservedResourceLifetime(d3d12Resource.Get()),
                                                   textureHandle,
                                                   (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0,
                                                   nrImportResult,
                                                   nrImportBegin,
                                                   nr_capture::sourceClock());
                CHECK_HRESULT(nrImportResult,
                              "m_dlssnrDevice->OpenSharedHandle(textureHandle, "
                              "IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf()))");
                candidate.push_back(std::move(d3d12Resource));
            }
            resolved.dlssnrImages = std::move(candidate);
        }
    }

    void OpenXrRuntime::initializeDlssnrResources() {
        if (m_dlssnrResourcesInitialized) {
            return;
        }
        ComPtr<ID3D12Device> device;
        std::unique_ptr<D3D12Utils::CommandContext> context;
        ComPtr<ID3D12Fence> inFence;
        ComPtr<ID3D11Fence> outFence;
        NVSDK_NGX_Parameter* parameters{};
        auto releaseParameters = MakeScopeGuard([&] {
            if (parameters) {
                NVSDK_NGX_D3D12_DestroyParameters(parameters);
            }
        });
        // DLSS-NR requires a D3D12 context. Create one on the matching adapter.
        ComPtr<IDXGIDevice> dxgiDevice;
        CHECK_HRCMD(m_ovrSubmissionDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));

        ComPtr<IDXGIAdapter> dxgiAdapter;
        CHECK_HRCMD(dxgiDevice->GetAdapter(dxgiAdapter.ReleaseAndGetAddressOf()));
        if (m_dlssnrDevice) {
            // Retry capability/module setup on the same successfully initialized
            // SDK device; never repeatedly initialize a different device graph.
            device = m_dlssnrDevice;
        } else {
            CHECK_HRCMD(D3D12CreateDevice(
                dxgiAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));
        }

        context = std::make_unique<D3D12Utils::CommandContext>(device.Get());

        // We'll use a fence to serialize pre-compositor output to DLSS-NR input.
        {
            wil::unique_handle fenceHandle;
            const auto nrInputExportBegin = nr_capture::sourceClock();
            const auto nrInputExportResult =
                m_ovrSubmissionCompletionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, fenceHandle.put());
            nr_capture::sourceNrInputFenceExportObserved(context.get(),
                                                         context->GetObservedLifetimeEpoch(),
                                                         m_ovrSubmissionDevice.Get(),
                                                         m_ovrSubmissionCompletionFence.Get(),
                                                         fenceHandle.get(),
                                                         nrInputExportResult,
                                                         nrInputExportBegin,
                                                         nr_capture::sourceClock());
            CHECK_HRESULT(
                nrInputExportResult,
                "m_ovrSubmissionCompletionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, fenceHandle.put())");
            const auto nrInputImportBegin = nr_capture::sourceClock();
            const auto nrInputImportResult =
                device->OpenSharedHandle(fenceHandle.get(), IID_PPV_ARGS(inFence.ReleaseAndGetAddressOf()));
            nr_capture::sourceNrInputFenceImportObserved(context.get(),
                                                         context->GetObservedLifetimeEpoch(),
                                                         device.Get(),
                                                         inFence.Get(),
                                                         fenceHandle.get(),
                                                         nrInputImportResult,
                                                         nrInputImportBegin,
                                                         nr_capture::sourceClock());
            CHECK_HRESULT(
                nrInputImportResult,
                "device->OpenSharedHandle(fenceHandle.get(), IID_PPV_ARGS(inFence.ReleaseAndGetAddressOf()))");
        }

        // We'll use a fence to serialize DLSS-NR output to OVR input.
        {
            wil::unique_handle handle;
            auto* nrCompletionFence = context->GetCompletionFence();
            const auto nrFenceExportBegin = nr_capture::sourceClock();
            const auto nrFenceExportResult =
                device->CreateSharedHandle(nrCompletionFence, nullptr, GENERIC_ALL, nullptr, handle.put());
            nr_capture::sourceNrFenceExportObserved(context.get(),
                                                    context->GetObservedLifetimeEpoch(),
                                                    device.Get(),
                                                    nrCompletionFence,
                                                    handle.get(),
                                                    nrFenceExportResult,
                                                    nrFenceExportBegin,
                                                    nr_capture::sourceClock());
            CHECK_HRESULT(nrFenceExportResult,
                          "device->CreateSharedHandle(context->GetCompletionFence(), nullptr, GENERIC_ALL, nullptr, "
                          "handle.put())");
            const auto nrFenceImportBegin = nr_capture::sourceClock();
            const auto nrFenceImportResult =
                m_ovrSubmissionDevice->OpenSharedFence(handle.get(), IID_PPV_ARGS(outFence.ReleaseAndGetAddressOf()));
            nr_capture::sourceNrFenceImportObserved(context.get(),
                                                    context->GetObservedLifetimeEpoch(),
                                                    m_ovrSubmissionDevice.Get(),
                                                    outFence.Get(),
                                                    handle.get(),
                                                    nrFenceImportResult,
                                                    nrFenceImportBegin,
                                                    nr_capture::sourceClock());
            CHECK_HRESULT(nrFenceImportResult,
                          "m_ovrSubmissionDevice->OpenSharedFence(handle.get(), "
                          "IID_PPV_ARGS(outFence.ReleaseAndGetAddressOf()))");
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
        if (!m_dlssnrDevice) {
            CHECK_NGXCMD(
                NVSDK_NGX_D3D12_Init(123456, virtualdesktop_openxr::programData.c_str(), device.Get(), &commonInfo));
        }

        // Capability acquisition can fail after SDK initialization. Retain its
        // device across that failure; readiness still requires the complete
        // graph below. Global NGX shutdown could disturb the host game's DLSS.
        m_dlssnrDevice = device;

        CHECK_NGXCMD(NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters));
        if (ngx::dlssnrModule) {
            CHECK_MSG(ngx::Init_Ext && ngx::Shutdown1, "DLSS-NR module load did not complete");
            CHECK_NGXCMD(nr_capture::ObserveSdkInit(reinterpret_cast<uint64_t>(ngx::Init_Ext), device.Get(), [&] {
                return ngx::Init_Ext(
                    123456, virtualdesktop_openxr::programData.c_str(), device.Get(), NVSDK_NGX_Version_API, nullptr);
            }));
            ngx::initializedDevice = device;
        }
        // Publish the complete context/fences/parameters before allowing NR.
        m_dlssnrDevice = std::move(device);
        m_dlssnrContext = std::move(context);
        m_dlssnrInFence = std::move(inFence);
        m_dlssnrOutFence = std::move(outFence);
        m_ngxParameters = parameters;
        nr_capture::sdkTableCreated(m_ngxParameters);
        parameters = nullptr;
        m_dlssnrResourcesInitialized = true;
    }

    void OpenXrRuntime::cleanupDlssnrResources() {
        m_dlssnrResourcesInitialized = false;
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            if (m_dlssnrFeature[eye]) {
                CHECK_NGXCMD(
                    nr_capture::ObserveSdkCall(NR_RELEASE_FEATURE,
                                               eye,
                                               reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                                               nullptr,
                                               m_ngxParameters,
                                               &m_dlssnrFeature[eye],
                                               reinterpret_cast<uint64_t>(m_dlssnrFeatureEvaluate[eye]),
                                               reinterpret_cast<uint64_t>(m_dlssnrFeatureRelease[eye]),
                                               [&] { return m_dlssnrFeatureRelease[eye](m_dlssnrFeature[eye]); }));
                m_dlssnrFeature[eye] = nullptr;
                m_dlssnrFeatureRelease[eye] = nullptr;
                m_dlssnrFeatureEvaluate[eye] = nullptr;
            }
        }
        if (m_ngxParameters) {
            CHECK_NGXCMD(NVSDK_NGX_D3D12_DestroyParameters(m_ngxParameters));
            nr_capture::sdkTableDestroyed(m_ngxParameters);
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
        ngx::restoreModuleNameImports();
        ngx::NVSDK_NGX_D3D12_CreateFeature = &::NVSDK_NGX_D3D12_CreateFeature;
        ngx::NVSDK_NGX_D3D12_ReleaseFeature = &::NVSDK_NGX_D3D12_ReleaseFeature;
        ngx::NVSDK_NGX_D3D12_EvaluateFeature = &::NVSDK_NGX_D3D12_EvaluateFeature;
        ngx::Init_Ext = nullptr;
        ngx::Shutdown1 = nullptr;
        const auto nrReleasedModule = ngx::dlssnrModule.get();
        ngx::dlssnrModule.reset();
        nr_capture::observeManualModuleOwnerRelease(nrReleasedModule);
    }

    void OpenXrRuntime::retainDlssnrRuntimeOnCleanupFailure() noexcept {
        ngx::retainRuntimeOnCleanupFailure();
    }

} // namespace virtualdesktop_openxr
