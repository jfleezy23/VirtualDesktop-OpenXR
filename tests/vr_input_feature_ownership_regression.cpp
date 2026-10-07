// CPU-only opaque-handle ownership test using the real runtime cleanup method.
#include "pch.h"
#include "runtime.h"
#include <atomic>

namespace ngx {
    extern decltype(&::NVSDK_NGX_D3D12_ReleaseFeature) NVSDK_NGX_D3D12_ReleaseFeature;
}

namespace {
    struct FeatureToken {
        uint64_t marker;
    } sdkToken{0x53444b}, manualToken{0x4d414e55414c};
    auto* sdkHandle = reinterpret_cast<NVSDK_NGX_Handle*>(&sdkToken);
    auto* manualHandle = reinterpret_cast<NVSDK_NGX_Handle*>(&manualToken);
    std::atomic<uint32_t> sdkReleases{0}, manualReleases{0}, wrongSdkReleases{0}, wrongManualReleases{0};
    decltype(&::NVSDK_NGX_D3D12_ReleaseFeature) originalSdkRelease = &::NVSDK_NGX_D3D12_ReleaseFeature;

    NVSDK_NGX_Result NVSDK_CONV sdkReleaseSpy(NVSDK_NGX_Handle* handle) {
        if (handle == sdkHandle)
            ++sdkReleases;
        else
            ++wrongSdkReleases;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result NVSDK_CONV manualReleaseSpy(NVSDK_NGX_Handle* handle) {
        if (handle == manualHandle)
            ++manualReleases;
        else
            ++wrongManualReleases;
        return NVSDK_NGX_Result_Success;
    }
    void checkDetour(LONG result) {
        if (result != NO_ERROR)
            throw std::runtime_error("Cannot install SDK provider spy");
    }
    struct ProviderSpies {
        decltype(originalSdkRelease) previousRelease = ngx::NVSDK_NGX_D3D12_ReleaseFeature;
        ProviderSpies() {
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalSdkRelease), sdkReleaseSpy));
            checkDetour(DetourTransactionCommit());
        }
        ~ProviderSpies() {
            ngx::NVSDK_NGX_D3D12_ReleaseFeature = previousRelease;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(reinterpret_cast<PVOID*>(&originalSdkRelease), sdkReleaseSpy);
            DetourTransactionCommit();
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static int run() {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ProviderSpies spies;
            // Positive control proves that the real cleanup reaches the SDK symbol under the detour.
            ngx::NVSDK_NGX_D3D12_ReleaseFeature = &::NVSDK_NGX_D3D12_ReleaseFeature;
            runtime.m_dlssnrFeature[0] = sdkHandle;
            runtime.cleanupDlssnrResources();
            if (sdkReleases != 1 || wrongSdkReleases || wrongManualReleases || manualReleases)
                throw std::runtime_error("SDK cleanup positive control did not exercise the provider spy");
            sdkReleases = 0;

            // Model a left eye created by the SDK before fallback switched the global table for the right eye.
            runtime.m_dlssnrFeature[0] = sdkHandle;
            runtime.m_dlssnrFeature[1] = manualHandle;
#ifdef VDXR_PER_FEATURE_PROVIDER
            runtime.m_dlssnrFeatureRelease[0] = &::NVSDK_NGX_D3D12_ReleaseFeature;
            runtime.m_dlssnrFeatureRelease[1] = manualReleaseSpy;
#endif
            ngx::NVSDK_NGX_D3D12_ReleaseFeature = manualReleaseSpy;
            runtime.cleanupDlssnrResources();
            std::cout << "SDK/manual cleanup=" << sdkReleases << '/' << manualReleases
                      << " wrong SDK/manual=" << wrongSdkReleases << '/' << wrongManualReleases << '\n';
            if (sdkReleases != 1 || manualReleases != 1 || wrongSdkReleases || wrongManualReleases) {
                std::cerr << "FAIL: global provider switch routed an opaque feature to a different owner\n";
                return 1;
            }
            if (runtime.m_dlssnrFeature[0] || runtime.m_dlssnrFeature[1])
                throw std::runtime_error("Successful provider cleanup did not clear feature handles");
            runtime.cleanupDlssnrResources();
            if (sdkReleases != 1 || manualReleases != 1)
                throw std::runtime_error("Repeated cleanup released an opaque feature twice");
            std::cout << "PASS: mixed opaque feature owners remain paired through global dispatch changes\n";
            return 0;
        }
    };
} // namespace virtualdesktop_openxr

int main() {
    try {
        return virtualdesktop_openxr::RuntimeInputRegression::run();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
