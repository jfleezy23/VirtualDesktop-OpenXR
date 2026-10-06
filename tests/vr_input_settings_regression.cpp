// Actual refreshSettings must respect the mutex already held by controller-state consumers.
#include "pch.h"
#include "runtime.h"
#include <atomic>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
decltype(&RegGetValueW) originalRegGetValue = RegGetValueW;
std::atomic<DWORD> refreshThread{0};
wil::unique_handle lastPoseRead{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
wil::unique_handle allowPoseRead{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
decltype(&ovr_GetFovStencil) originalStencil = ovr_GetFovStencil;
ovrResult OVR_CDECL fixtureStencil(ovrSession, const ovrFovStencilDesc*, ovrFovStencilMeshBuffer* buffer) {
    buffer->UsedVertexCount = 3;
    buffer->UsedIndexCount = 3;
    return ovrSuccess;
}
LSTATUS WINAPI observeRegGetValue(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags,
                                  LPDWORD type, PVOID data, LPDWORD size) {
    if (GetCurrentThreadId() == refreshThread.load() && value && !wcscmp(value, L"hand_pose_offset_z")) {
        SetEvent(lastPoseRead.get());
        WaitForSingleObject(allowPoseRead.get(), INFINITE);
    }
    return originalRegGetValue(key, subkey, value, flags, type, data, size);
}
void checkDetour(LONG result) {
    if (result != NO_ERROR) throw std::runtime_error("Detours setup failed");
}
}

namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
static int asymmetricVisibilityQuery(const wchar_t* nullRuntimeDirectory) {
    OpenXrRuntime runtime;
    runtime.stopRegistryWatcher();
    ovrInitParams init{};
    init.Flags = ovrInit_RequestVersion;
    init.RequestedMinorVersion = OVR_MINOR_VERSION;
    if (OVR_FAILURE(ovr_InitializeWithPathOverride(&init, nullRuntimeDirectory)))
        throw std::runtime_error("OVRNull library initialization failed");
    // OVRNull initialization only loads the CPU API. No session or graphics device is created.
    originalStencil = reinterpret_cast<decltype(originalStencil)>(
        GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_GetFovStencil"));
    if (!originalStencil) throw std::runtime_error("OVRNull stencil export missing");
    runtime.m_sessionCreated = true;
    runtime.has_XR_KHR_visibility_mask = true;
    checkDetour(DetourTransactionBegin());
    checkDetour(DetourUpdateThread(GetCurrentThread()));
    checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalStencil), fixtureStencil));
    checkDetour(DetourTransactionCommit());
    int failures = 0;
    for (bool nonnullIndices : {false, true}) {
        XrVector2f vertices[3]{{123.f, 456.f}, {123.f, 456.f}, {123.f, 456.f}};
        uint32_t indices[3]{99, 99, 99};
        XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
        mask.vertexCapacityInput = 3;
        mask.vertices = vertices;
        mask.indexCapacityInput = 0;
        mask.indices = nonnullIndices ? indices : nullptr;
        mask.vertexCountOutput = mask.indexCountOutput = 999;
        const auto result = runtime.xrGetVisibilityMaskKHR(reinterpret_cast<XrSession>(1),
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
        if (result != XR_SUCCESS || mask.vertexCountOutput != 3 || mask.indexCountOutput != 3 ||
            vertices[0].x != 123.f || vertices[0].y != 456.f || indices[0] != 99) {
            ++failures;
            std::cerr << "FAIL: zero index capacity must query both visibility counts without writing buffers\n";
        }
    }
    checkDetour(DetourTransactionBegin());
    checkDetour(DetourUpdateThread(GetCurrentThread()));
    checkDetour(DetourDetach(reinterpret_cast<PVOID*>(&originalStencil), fixtureStencil));
    checkDetour(DetourTransactionCommit());
    runtime.m_sessionCreated = false;
    if (failures) return 1;
    std::cout << "PASS: asymmetric visibility capacity queries return counts only\n";
    return 0;
}
static int run() {
    OpenXrRuntime runtime;
    runtime.stopRegistryWatcher();
    checkDetour(DetourTransactionBegin());
    checkDetour(DetourUpdateThread(GetCurrentThread()));
    checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalRegGetValue), observeRegGetValue));
    checkDetour(DetourTransactionCommit());
    std::unique_lock readerLock(runtime.m_actionsAndSpacesMutex);
    wil::unique_handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::exception_ptr failure;
    std::thread writer([&] {
        refreshThread.store(GetCurrentThreadId());
        try { runtime.refreshSettings(); }
        catch (...) { failure = std::current_exception(); }
        SetEvent(completed.get());
    });
    const bool entered = WaitForSingleObject(lastPoseRead.get(), 5000) == WAIT_OBJECT_0;
    SetEvent(allowPoseRead.get());
    const bool respectedReader = WaitForSingleObject(completed.get(), 250) == WAIT_TIMEOUT;
    readerLock.unlock();
    writer.join();
    checkDetour(DetourTransactionBegin());
    checkDetour(DetourUpdateThread(GetCurrentThread()));
    checkDetour(DetourDetach(reinterpret_cast<PVOID*>(&originalRegGetValue), observeRegGetValue));
    checkDetour(DetourTransactionCommit());
    if (failure) std::rethrow_exception(failure);
    if (!entered) throw std::runtime_error("Actual pose settings read was not exercised");
    if (!respectedReader) {
        std::cerr << "FAIL: settings refresh modified controller state while a consumer held its mutex\n";
        return 1;
    }
    std::cout << "PASS: settings refresh waits for controller-state consumers\n";
    return 0;
}
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: vr_input_settings_regression OVRNull-directory");
        const auto guardResult = virtualdesktop_openxr::RuntimeInputRegression::run();
        const auto visibilityResult = virtualdesktop_openxr::RuntimeInputRegression::asymmetricVisibilityQuery(argv[1]);
        return guardResult || visibilityResult ? 1 : 0;
    }
    catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
