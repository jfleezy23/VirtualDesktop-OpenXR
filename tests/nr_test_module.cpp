// TEST ONLY: unsigned NGX facade for failure injection, never a rendering implementation.
// Copy only beside an isolated test runtime. Never install in Virtual Desktop Streamer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <detours.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>
#include <set>
#include <string_view>

namespace {
    std::array<std::atomic<uint32_t>, 5> calls{}; // Init, Create, Evaluate, Release, Shutdown.
    struct TestFeature {
        uint64_t marker{0x4e52544553544f4eull};
    };
    std::mutex stateMutex;
    std::set<TestFeature*> liveFeatures;
    ID3D12Device* initializedDevice{};
    std::atomic<uint32_t> contractErrors{0};
    std::atomic<uint32_t> successfulShutdowns{0};
    PVOID* moduleNameSlot{};
    PVOID retainedShim{};

    DWORD WINAPI foreignModuleName(HMODULE, wchar_t* out, DWORD size) {
        constexpr wchar_t name[] = L"foreign.dll";
        constexpr DWORD length = static_cast<DWORD>(std::size(name) - 1);
        if (!size)
            return 0;
        if (!out)
            return 0;
        const auto copied = length < size ? length : size - 1;
        for (DWORD i = 0; i < copied; ++i)
            out[i] = name[i];
        out[copied] = L'\0';
        return length < size ? length : size;
    }

    bool replaceModuleNameImport(PVOID expected, PVOID replacement) {
        if (!moduleNameSlot)
            return false;
        DWORD protection{};
        if (!VirtualProtect(moduleNameSlot, sizeof(PVOID), PAGE_READWRITE, &protection))
            return false;
        const bool replaced = InterlockedCompareExchangePointer(moduleNameSlot, replacement, expected) == expected;
        DWORD ignored{};
        const bool protectedAgain = VirtualProtect(moduleNameSlot, sizeof(PVOID), protection, &ignored) != FALSE;
        return replaced && protectedAgain;
    }
} // namespace
#define TEST_EXPORT extern "C" __declspec(dllexport)
TEST_EXPORT uint32_t __cdecl NR_Test_GetVariant() {
#ifdef NR_TEST_MISSING_EVALUATE
    return 1;
#elif defined(NR_TEST_PARTIAL_CREATE)
    return 3;
#elif defined(NR_TEST_FOREIGN_IMPORT)
    return 4;
#else
    return 2;
#endif
}
TEST_EXPORT void __cdecl NR_Test_GetStats(uint32_t* out, uint32_t count) {
    if (!out || count != calls.size())
        return;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = calls[i].load();
}
TEST_EXPORT void __cdecl NR_Test_GetHealth(uint32_t* out, uint32_t count) {
    if (!out || count != 4)
        return;
    std::lock_guard lock(stateMutex);
    out[0] = contractErrors.load();
    out[1] = static_cast<uint32_t>(liveFeatures.size());
    out[2] = initializedDevice ? 1u : 0u;
    out[3] = successfulShutdowns.load();
}
TEST_EXPORT DWORD __cdecl NR_Test_GetModuleName(wchar_t* out, DWORD size) {
    return GetModuleFileNameW(nullptr, out, size);
}
TEST_EXPORT BOOL __cdecl NR_Test_InstallForeignImport() {
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&NR_Test_GetModuleName),
                            &self))
        return FALSE;
    moduleNameSlot = nullptr;
    if (!DetourEnumerateImportsEx(self,
                                  nullptr,
                                  nullptr,
                                  [](PVOID, DWORD, LPCSTR name, PVOID* slot) -> BOOL {
                                      if (name && slot && std::string_view(name) == "GetModuleFileNameW")
                                          moduleNameSlot = slot;
                                      return TRUE;
                                  }) ||
        !moduleNameSlot)
        return FALSE;
    retainedShim = *moduleNameSlot;
    return replaceModuleNameImport(retainedShim, reinterpret_cast<PVOID>(&foreignModuleName));
}
TEST_EXPORT BOOL __cdecl NR_Test_IsForeignImport() {
    return moduleNameSlot && *moduleNameSlot == reinterpret_cast<PVOID>(&foreignModuleName);
}
TEST_EXPORT BOOL __cdecl NR_Test_RestoreShim() {
    return replaceModuleNameImport(reinterpret_cast<PVOID>(&foreignModuleName), retainedShim);
}
TEST_EXPORT NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long, const wchar_t*, ID3D12Device* device, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*) {
    ++calls[0];
    std::lock_guard lock(stateMutex);
    if (!device || initializedDevice) {
        ++contractErrors;
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    initializedDevice = device;
    return NVSDK_NGX_Result_Success; // Success is 0x1 in the retained NGX SDK.
}
TEST_EXPORT NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*,
                                                                      NVSDK_NGX_Feature,
                                                                      NVSDK_NGX_Parameter*,
                                                                      NVSDK_NGX_Handle** handle) {
    ++calls[1];
    if (!handle)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* feature = new (std::nothrow) TestFeature;
    if (!feature)
        return NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
    {
        std::lock_guard lock(stateMutex);
        if (!initializedDevice) {
            ++contractErrors;
            delete feature;
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        }
        liveFeatures.insert(feature);
    }
    *handle = reinterpret_cast<NVSDK_NGX_Handle*>(feature);
    return NVSDK_NGX_Result_Success;
}
TEST_EXPORT NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle) {
    ++calls[3];
    std::lock_guard lock(stateMutex);
    auto* feature = reinterpret_cast<TestFeature*>(handle);
    // Membership must be checked before dereferencing a stale or foreign pointer.
    if (!liveFeatures.count(feature) || feature->marker != 0x4e52544553544f4eull) {
        ++contractErrors;
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    liveFeatures.erase(feature);
    feature->marker = 0;
    delete feature;
    return NVSDK_NGX_Result_Success;
}
#ifndef NR_TEST_MISSING_EVALUATE
TEST_EXPORT NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList*,
                                                                        const NVSDK_NGX_Handle* handle,
                                                                        const NVSDK_NGX_Parameter*,
                                                                        PFN_NVSDK_NGX_ProgressCallback) {
    ++calls[2];
    std::lock_guard lock(stateMutex);
    auto* feature = reinterpret_cast<TestFeature*>(const_cast<NVSDK_NGX_Handle*>(handle));
    if (!initializedDevice || !liveFeatures.count(feature) || feature->marker != 0x4e52544553544f4eull) {
        ++contractErrors;
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    return NVSDK_NGX_Result_Success; // No image output: this facade tests API errors only.
}
#endif
TEST_EXPORT NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* device) {
    const auto attempt = ++calls[4];
    std::lock_guard lock(stateMutex);
    if (!initializedDevice || device != initializedDevice || !liveFeatures.empty()) {
        ++contractErrors;
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
#ifdef NR_TEST_FAIL_SHUTDOWN_ONCE
    if (attempt == 1)
        return NVSDK_NGX_Result_FAIL_PlatformError;
#else
    (void)attempt;
#endif
    initializedDevice = nullptr;
    ++successfulShutdowns;
    return NVSDK_NGX_Result_Success;
}
