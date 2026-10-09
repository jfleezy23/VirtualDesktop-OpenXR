// Actual-DLL CPU negotiation regression. The registry detour only reads synthetic
// redirect_to data; this test never writes registry values or creates a VR session.
#define WIN32_LEAN_AND_MEAN
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <detours.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

decltype(&RegGetValueW) originalReadRegistry = RegGetValueW;
std::wstring redirectPath;
unsigned redirectReads{};
LSTATUS WINAPI
readRegistry(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD bytes) {
    if (key != HKEY_LOCAL_MACHINE || !value || _wcsicmp(value, L"redirect_to"))
        return originalReadRegistry(key, subkey, value, flags, type, data, bytes);
    ++redirectReads;
    if (!bytes)
        return ERROR_INVALID_PARAMETER;
    const DWORD required = static_cast<DWORD>((redirectPath.size() + 1) * sizeof(wchar_t));
    if (type)
        *type = REG_SZ;
    const DWORD capacity = *bytes;
    *bytes = required;
    if (!data)
        return ERROR_SUCCESS;
    if (capacity < required)
        return ERROR_MORE_DATA;
    memcpy(data, redirectPath.c_str(), required);
    return ERROR_SUCCESS;
}
void checkDetour(LONG result) {
    if (result)
        throw std::runtime_error("Detour error " + std::to_string(result));
}
void detour(bool attach) {
    checkDetour(DetourTransactionBegin());
    checkDetour(DetourUpdateThread(GetCurrentThread()));
    checkDetour(attach ? DetourAttach(reinterpret_cast<PVOID*>(&originalReadRegistry), readRegistry)
                       : DetourDetach(reinterpret_cast<PVOID*>(&originalReadRegistry), readRegistry));
    checkDetour(DetourTransactionCommit());
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf;
    HMODULE runtime{};
    bool attached = false;
    try {
        if (argc != 4 && argc != 5)
            throw std::runtime_error("Usage: <actual-runtime.dll> <test-target.dll> failure|success [wide]");
        if (argc == 5 && std::wstring(argv[4]) != L"wide")
            throw std::runtime_error("Unknown interface range option");
        const bool success = std::wstring(argv[3]) == L"success";
        if (!success && std::wstring(argv[3]) != L"failure")
            throw std::runtime_error("Unknown mode");
        redirectPath = std::filesystem::absolute(argv[2]).wstring();
        if (GetModuleHandleW(redirectPath.c_str()))
            throw std::runtime_error("Target was already mapped before negotiation");
        runtime = LoadLibraryW(argv[1]);
        if (!runtime)
            throw std::runtime_error("Cannot load actual runtime " + std::to_string(GetLastError()));
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
            GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
        if (!negotiate)
            throw std::runtime_error("Actual runtime has no negotiation export");
        detour(true);
        attached = true;
        XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                     XR_LOADER_INFO_STRUCT_VERSION,
                                     sizeof(XrNegotiateLoaderInfo),
                                     1,
                                     argc == 5 ? 2u : 1u,
                                     XR_MAKE_VERSION(1, 0, 0),
                                     XR_CURRENT_API_VERSION};
        XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
                                          XR_RUNTIME_INFO_STRUCT_VERSION,
                                          sizeof(XrNegotiateRuntimeRequest)};
        const auto result = negotiate(&loader, &request);
        detour(false);
        attached = false;
        const bool mapped = GetModuleHandleW(redirectPath.c_str()) != nullptr;
        std::cout << "result=" << result << " targetMapped=" << mapped << " syntheticRegistryReads=" << redirectReads
                  << '\n';
        if (redirectReads != 2)
            throw std::runtime_error("Actual runtime did not read synthetic redirection data");
        if (success) {
            if (result != XR_SUCCESS || !mapped || !request.getInstanceProcAddr)
                throw std::runtime_error("Successful redirect must retain valid mapped dispatch code");
            if (request.runtimeInterfaceVersion < loader.minInterfaceVersion ||
                request.runtimeInterfaceVersion > loader.maxInterfaceVersion ||
                request.runtimeApiVersion < loader.minApiVersion || request.runtimeApiVersion > loader.maxApiVersion)
                throw std::runtime_error("Redirect returned incompatible negotiated versions");
            PFN_xrVoidFunction function{};
            if (request.getInstanceProcAddr(XR_NULL_HANDLE, "testUnimplementedFunction", &function) !=
                XR_ERROR_FUNCTION_UNSUPPORTED)
                throw std::runtime_error("Returned dispatch pointer is not the live target implementation");
        } else if (result != XR_ERROR_INITIALIZATION_FAILED || mapped || request.getInstanceProcAddr) {
            throw std::runtime_error("Failed redirect retained a module reference or changed the target error");
        }
        FreeLibrary(runtime);
        runtime = nullptr;
        std::cout << "PASS: "
                  << (success ? "successful redirect retains dispatch module"
                              : "failed redirect releases unused module")
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (attached) {
            try {
                detour(false);
            } catch (...) {
            }
        }
        if (runtime)
            FreeLibrary(runtime);
        return 1;
    }
}
