// Black-box failure injection through the runtime's negotiated OpenXR API dispatch.
// Direct dispatch is needed: the upstream loader masks DestroyInstance errors and
// removes its instance even when runtime cleanup failed, preventing a same-handle retry.
// The caller prepares an isolated runtime + TEST ONLY nvngx_dlssnr.dll and settings.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <detours.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
#define XR_FUNCTIONS(X)                                                                                                \
    X(xrCreateInstance)                                                                                                \
    X(xrDestroyInstance)                                                                                               \
    X(xrGetSystem)                                                                                                     \
    X(xrGetD3D11GraphicsRequirementsKHR)                                                                               \
    X(xrCreateSession)                                                                                                 \
    X(xrDestroySession)                                                                                                \
    X(xrBeginSession)                                                                                                  \
    X(xrCreateReferenceSpace)                                                                                          \
    X(xrCreateSwapchain)                                                                                               \
    X(xrEnumerateSwapchainImages)                                                                                      \
    X(xrAcquireSwapchainImage)                                                                                         \
    X(xrWaitSwapchainImage)                                                                                            \
    X(xrReleaseSwapchainImage)                                                                                         \
    X(xrWaitFrame)                                                                                                     \
    X(xrBeginFrame)                                                                                                    \
    X(xrEndFrame)
#define DECLARE(name) PFN_##name name;
XR_FUNCTIONS(DECLARE)
#undef DECLARE
void checkHr(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("D3D failed: " + std::to_string(result));
}
void checkXr(XrResult result, const char* call) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(call) + " failed: " + std::to_string(result));
}
#define XR_CHECK(call) checkXr(call, #call)
void requireResult(XrResult actual, XrResult expected, const char* call) {
    std::cout << call << " result=" << actual << " expected=" << expected << '\n';
    if (actual != expected)
        throw std::runtime_error(std::string(call) + " returned the wrong result");
}

namespace {
    using GetStats = void(__cdecl*)(uint32_t*, uint32_t);
    decltype(&RegGetValueW) originalRegGetValue = RegGetValueW;
    DWORD faultThread{};
    std::atomic<bool> faultArmed{false};
    bool faultObserved{};
    uint32_t initCountAtFault{};
    GetStats inspectStats{};
    LSTATUS WINAPI
    faultRegGetValue(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
        if (GetCurrentThreadId() == faultThread && value && !wcscmp(value, L"mirror_window") &&
            faultArmed.exchange(false)) {
            faultObserved = true;
            std::array<uint32_t, 5> stats{};
            inspectStats(stats.data(), static_cast<uint32_t>(stats.size()));
            initCountAtFault = stats[0];
            throw std::runtime_error("TEST ONLY: settings read after manual NR reinitialization");
        }
        return originalRegGetValue(key, subkey, value, flags, type, data, size);
    }
    void checkDetour(LONG result) {
        if (result != NO_ERROR)
            throw std::runtime_error("Cannot install isolated settings fault");
    }
    struct SettingsFault {
        SettingsFault(GetStats stats) {
            faultThread = GetCurrentThreadId();
            inspectStats = stats;
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalRegGetValue), faultRegGetValue));
            checkDetour(DetourTransactionCommit());
            faultArmed = true;
        }
        ~SettingsFault() {
            faultArmed = false;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(reinterpret_cast<PVOID*>(&originalRegGetValue), faultRegGetValue);
            DetourTransactionCommit();
        }
    };
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    // Crash/abort failures should terminate this child test without displaying modal dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        if (argc != 3)
            throw std::runtime_error(
                "Usage: <isolated runtime.dll> <missing-export|shutdown-retry|partial-create|foreign-import>");
        const std::string mode = argv[2];
        const uint32_t variant = mode == "missing-export"   ? 1u
                                 : mode == "shutdown-retry" ? 2u
                                 : mode == "partial-create" ? 3u
                                 : mode == "foreign-import" ? 4u
                                                            : 0u;
        if (!variant)
            throw std::runtime_error("Unknown mode");
        const std::filesystem::path runtimePath = std::filesystem::absolute(argv[1]);
        const auto mockPath = runtimePath.parent_path() / L"nvngx_dlssnr.dll";
        // Hold a private reference so counters remain inspectable after runtime cleanup/unload.
        HMODULE mock = LoadLibraryW(mockPath.c_str());
        if (!mock)
            throw std::runtime_error("Cannot load isolated failure-injection module");
        using GetVariant = uint32_t(__cdecl*)();
        auto getVariant = reinterpret_cast<GetVariant>(GetProcAddress(mock, "NR_Test_GetVariant"));
        auto getStats = reinterpret_cast<GetStats>(GetProcAddress(mock, "NR_Test_GetStats"));
        auto getHealth = reinterpret_cast<GetStats>(GetProcAddress(mock, "NR_Test_GetHealth"));
        using GetModuleName = DWORD(__cdecl*)(wchar_t*, DWORD);
        auto getModuleName = reinterpret_cast<GetModuleName>(GetProcAddress(mock, "NR_Test_GetModuleName"));
        if (!getVariant || !getStats || !getHealth || !getModuleName || getVariant() != variant)
            throw std::runtime_error("Wrong facade: refusing to run against a vendor NR module");
        HMODULE runtime = LoadLibraryW(runtimePath.c_str());
        if (!runtime)
            throw std::runtime_error("Cannot load isolated runtime");
        bool ownsRuntimeReference = true;
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
            GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
        if (!negotiate)
            throw std::runtime_error("Missing runtime negotiation export");
        XrNegotiateLoaderInfo loaderInfo{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                         XR_LOADER_INFO_STRUCT_VERSION,
                                         sizeof(XrNegotiateLoaderInfo),
                                         1,
                                         1,
                                         XR_MAKE_VERSION(1, 0, 0),
                                         XR_CURRENT_API_VERSION};
        XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
                                          XR_RUNTIME_INFO_STRUCT_VERSION,
                                          sizeof(XrNegotiateRuntimeRequest)};
        XR_CHECK(negotiate(&loaderInfo, &request));
        if (!request.getInstanceProcAddr)
            throw std::runtime_error("Negotiation returned null dispatch");
        auto getProc = request.getInstanceProcAddr;
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extension = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR NR module failure regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = &extension;
        const auto requirePinnedRuntime = [&] {
            if (!ownsRuntimeReference || !FreeLibrary(runtime))
                throw std::runtime_error("Cannot release test's runtime reference");
            ownsRuntimeReference = false;
            if (GetModuleHandleW(runtimePath.c_str()) != runtime)
                throw std::runtime_error("Failed teardown did not pin surviving runtime code");
            XrInstance rejected{};
            requireResult(xrCreateInstance(&info, &rejected),
                          XR_ERROR_LIMIT_REACHED,
                          "CreateInstance while failed instance remains");
            if (rejected != XR_NULL_HANDLE)
                throw std::runtime_error("Rejected instance creation published a handle");
        };
        XrInstance instance{};
        XR_CHECK(xrCreateInstance(&info, &instance));
#define LOAD(name) XR_CHECK(getProc(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)));
        XR_FUNCTIONS(LOAD)
#undef LOAD
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system{};
        XR_CHECK(xrGetSystem(instance, &systemInfo, &system));
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(instance, system, &requirements));
        ComPtr<IDXGIFactory1> factory;
        checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            checkHr(factory->EnumAdapters1(i, &adapter));
            DXGI_ADAPTER_DESC1 desc{};
            checkHr(adapter->GetDesc1(&desc));
            if (!memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)))
                break;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checkHr(D3D11CreateDevice(adapter.Get(),
                                  D3D_DRIVER_TYPE_UNKNOWN,
                                  nullptr,
                                  0,
                                  &requirements.minFeatureLevel,
                                  1,
                                  D3D11_SDK_VERSION,
                                  &device,
                                  nullptr,
                                  &context));
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.Get();
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
        create.next = &binding;
        create.systemId = system;
        XrSession session{};
        XR_CHECK(xrCreateSession(instance, &create, &session));
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XR_CHECK(xrBeginSession(session, &begin));
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        reference.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace space{};
        XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
        constexpr uint32_t width = 64, height = 64;
        XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        chainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        chainInfo.width = width;
        chainInfo.height = height;
        chainInfo.arraySize = 2;
        chainInfo.sampleCount = chainInfo.faceCount = chainInfo.mipCount = 1;
        XrSwapchain chain{};
        XR_CHECK(xrCreateSwapchain(session, &chainInfo, &chain));
        uint32_t count{};
        XR_CHECK(xrEnumerateSwapchainImages(chain, 0, &count, nullptr));
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(
            chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session, &wait, &state));
        XrFrameBeginInfo frameBegin{XR_TYPE_FRAME_BEGIN_INFO};
        XR_CHECK(xrBeginFrame(session, &frameBegin));
        uint32_t index{};
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XR_CHECK(xrAcquireSwapchainImage(chain, &acquire, &index));
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xrWaitSwapchainImage(chain, &imageWait));
        std::vector<uint32_t> input(width * height, 0xff804020u);
        for (uint32_t eye = 0; eye < 2; ++eye)
            context->UpdateSubresource(images[index].texture,
                                       D3D11CalcSubresource(0, eye, 1),
                                       nullptr,
                                       input.data(),
                                       width * 4,
                                       width * height * 4);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XR_CHECK(xrReleaseSwapchainImage(chain, &release));
        XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                     {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            views[eye].pose.orientation.w = 1.f;
            views[eye].pose.position = {eye ? 0.032f : -0.032f, 1.6f, 0.f};
            views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
            views[eye].subImage = {chain, {{0, 0}, {width, height}}, eye};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = space;
        layer.viewCount = 2;
        layer.views = views;
        const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = 1;
        end.layers = &header;
        std::array<uint32_t, 5> stats{};
        if (mode == "missing-export") {
            std::cout << "FIRST EndFrame with incomplete module\n";
            requireResult(xrEndFrame(session, &end), XR_ERROR_RUNTIME_FAILURE, "first EndFrame");
            std::cout << "RETRY same EndFrame with incomplete module\n";
            requireResult(xrEndFrame(session, &end), XR_ERROR_RUNTIME_FAILURE, "retry EndFrame");
            getStats(stats.data(), static_cast<uint32_t>(stats.size()));
            if (stats[0] || stats[1] || stats[2])
                throw std::runtime_error("Incomplete module published callable NGX state");
            requireResult(xrDestroyInstance(instance), XR_SUCCESS, "DestroyInstance after failed frames");
        } else {
            XR_CHECK(xrEndFrame(session, &end));
            wchar_t name[64]{};
            if (getModuleName(name, 64) != 9 || std::wstring(name) != L"nvngx.dll")
                throw std::runtime_error("NR import shim returned the wrong full filename/length");
            for (DWORD size : {1u, 9u}) {
                std::fill(std::begin(name), std::end(name), L'!');
                SetLastError(ERROR_SUCCESS);
                if (getModuleName(name, size) != size || name[size - 1] != L'\0' || name[size] != L'!' ||
                    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                    throw std::runtime_error("NR import shim truncation violated buffer/error contract");
            }
            if (getModuleName(name, 10) != 9 || std::wstring(name) != L"nvngx.dll")
                throw std::runtime_error("NR import shim exact-fit buffer failed");
            name[0] = L'!';
            if (getModuleName(name, 0) != 0 || name[0] != L'!')
                throw std::runtime_error("NR import shim touched a zero-size buffer");
            getStats(stats.data(), static_cast<uint32_t>(stats.size()));
            if (!stats[0] || stats[1] != 2 || stats[2] != 2)
                throw std::runtime_error("Facade was not used for both eyes; failure injection did not run");
            if (mode == "partial-create") {
                requireResult(xrDestroySession(session), XR_SUCCESS, "DestroySession before partial create");
                getStats(stats.data(), static_cast<uint32_t>(stats.size()));
                if (stats[0] != 1 || stats[4] != 1 || stats[3] != 2)
                    throw std::runtime_error("Initial NR session did not initialize/shut down exactly once");
                const auto oldInitCount = stats[0];
                XrSession partial = reinterpret_cast<XrSession>(uintptr_t(0xbadf00d));
                XrResult failedCreate{};
                {
                    SettingsFault fault(getStats);
                    failedCreate = xrCreateSession(instance, &create, &partial);
                }
                requireResult(failedCreate, XR_ERROR_RUNTIME_FAILURE, "CreateSession after NR reinit settings fault");
                getStats(stats.data(), static_cast<uint32_t>(stats.size()));
                std::array<uint32_t, 4> health{};
                getHealth(health.data(), static_cast<uint32_t>(health.size()));
                std::cout << "fault observed=" << faultObserved << " init at fault=" << initCountAtFault
                          << " init/shutdown=" << stats[0] << '/' << stats[4] << " initialized device=" << health[2]
                          << '\n';
                if (!faultObserved || initCountAtFault != oldInitCount + 1)
                    throw std::runtime_error("Settings fault did not run after actual manual NR reinitialization");
                if (stats[0] != oldInitCount + 1 || stats[4] != stats[0] || health[2] || health[3] != stats[0])
                    throw std::runtime_error("Failed CreateSession retained an initialized NR device without shutdown");
                if (partial != XR_NULL_HANDLE)
                    throw std::runtime_error("Failed CreateSession published/retained a session handle");
                XrSession retry{};
                XR_CHECK(xrCreateSession(instance, &create, &retry));
                requireResult(xrDestroySession(retry), XR_SUCCESS, "DestroySession after retry create");
                getStats(stats.data(), static_cast<uint32_t>(stats.size()));
                if (stats[0] != oldInitCount + 2 || stats[4] != stats[0])
                    throw std::runtime_error("Session retry did not pair every manual NR initialization with shutdown");
                requireResult(xrDestroyInstance(instance), XR_SUCCESS, "DestroyInstance after partial create retry");
            } else if (mode == "foreign-import") {
                using ChangeImport = BOOL(__cdecl*)();
                const auto installForeign =
                    reinterpret_cast<ChangeImport>(GetProcAddress(mock, "NR_Test_InstallForeignImport"));
                const auto isForeign = reinterpret_cast<ChangeImport>(GetProcAddress(mock, "NR_Test_IsForeignImport"));
                const auto restoreShim = reinterpret_cast<ChangeImport>(GetProcAddress(mock, "NR_Test_RestoreShim"));
                if (!installForeign || !isForeign || !restoreShim || !installForeign() || !isForeign())
                    throw std::runtime_error("Cannot install isolated third-party import replacement");
                if (getModuleName(name, 64) != 11 || std::wstring(name) != L"foreign.dll")
                    throw std::runtime_error("Foreign import fixture was not called through the facade's actual IAT");
                const auto result = xrDestroyInstance(instance);
                // Test the replacement first: a clobber must be observable even if teardown reports success.
                if (!isForeign())
                    throw std::runtime_error("DestroyInstance clobbered a third-party NR import replacement");
                requireResult(result, XR_ERROR_RUNTIME_FAILURE, "DestroyInstance with foreign import");
                requirePinnedRuntime();
                if (!restoreShim())
                    throw std::runtime_error("Cannot restore retained VDXR shim for retry");
                requireResult(xrDestroyInstance(instance), XR_SUCCESS, "DestroyInstance after restoring owned import");
            } else {
                // Leave the session/space/swapchain active to exercise instance-owned teardown.
                std::cout << "FIRST DestroyInstance: inject Shutdown1 failure\n";
                requireResult(xrDestroyInstance(instance), XR_ERROR_RUNTIME_FAILURE, "first DestroyInstance");
                getStats(stats.data(), static_cast<uint32_t>(stats.size()));
                if (stats[4] != 1)
                    throw std::runtime_error("Expected exactly one rejected shutdown");
                requirePinnedRuntime();
                std::cout << "RETRY DestroyInstance: Shutdown1 now succeeds\n";
                requireResult(xrDestroyInstance(instance), XR_SUCCESS, "retry DestroyInstance");
                getStats(stats.data(), static_cast<uint32_t>(stats.size()));
                if (stats[4] != 2 || stats[3] != 2)
                    throw std::runtime_error("Cleanup retry did not finish exactly once for each feature/device");
            }
            if (getModuleName(name, 64) == 9 && std::wstring(name) == L"nvngx.dll")
                throw std::runtime_error("NR import was not restored after successful shutdown");
        }
        std::array<uint32_t, 4> health{};
        getHealth(health.data(), static_cast<uint32_t>(health.size()));
        getStats(stats.data(), static_cast<uint32_t>(stats.size()));
        if (health[0] || health[1] || health[2] || health[3] != stats[0] || stats[3] != stats[1])
            throw std::runtime_error(
                "Facade detected invalid handles, unpaired initialization, or unreleased features");
        checkHr(device->GetDeviceRemovedReason());
        std::cout << "mock calls init/create/evaluate/release/shutdown=";
        getStats(stats.data(), static_cast<uint32_t>(stats.size()));
        for (const auto value : stats)
            std::cout << value << ' ';
        std::cout << '\n';
        // Successful destruction must also permit a fresh instance in the same runtime module.
        XrInstance fresh{};
        XR_CHECK(xrCreateInstance(&info, &fresh));
        requireResult(xrDestroyInstance(fresh), XR_SUCCESS, "DestroyInstance after fresh CreateInstance");
        if (ownsRuntimeReference)
            FreeLibrary(runtime);
        FreeLibrary(mock);
        std::cout << "PASS: " << mode << " API failure and cleanup retry\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
