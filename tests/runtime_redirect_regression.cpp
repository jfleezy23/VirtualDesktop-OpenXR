// Actual-DLL CPU negotiation regression. Live mode reads the real redirect_to;
// other modes supply it process-locally. No registry writes or VR session creation.
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
unsigned redirectReadLimit{2};
LSTATUS WINAPI
readRegistry(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD bytes) {
    if (key != HKEY_LOCAL_MACHINE || !value || _wcsicmp(value, L"redirect_to"))
        return originalReadRegistry(key, subkey, value, flags, type, data, bytes);
    ++redirectReads;
    // Bound a missing self-redirect guard without overflowing the process stack.
    // The first recursive lookup is suppressed after one (self) or two (chain) hops.
    if (redirectReads > redirectReadLimit)
        return ERROR_FILE_NOT_FOUND;
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
void checkEvent(HANDLE event, const std::wstring& name) {
    const auto opened = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name.c_str());
    if (!opened)
        throw std::runtime_error("OpenEventW could not open the private test event");
    const bool live = SetEvent(opened) && WaitForSingleObject(event, 0) == WAIT_OBJECT_0 && ResetEvent(event);
    CloseHandle(opened);
    if (!live)
        throw std::runtime_error("OpenEventW did not return the live private test event");
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf;
    HMODULE runtime{};
    HANDLE event{};
    bool attached = false;
    try {
        if (argc != 4 && argc != 5)
            throw std::runtime_error("Usage: <actual-runtime.dll> <target.dll> failure|success|self|chain|live [wide]");
        if (argc == 5 && std::wstring(argv[4]) != L"wide")
            throw std::runtime_error("Unknown interface range option");
        const std::wstring mode = argv[3];
        const bool self = mode == L"self";
        const bool live = mode == L"live";
        const bool chain = mode == L"chain" || live;
        const bool actualTarget = self || chain;
        const bool success = mode == L"success" || actualTarget;
        if (!success && mode != L"failure")
            throw std::runtime_error("Unknown mode");
        redirectReadLimit = chain ? 4u : 2u;
        const std::wstring eventName = L"Local\\VDXRRedirectRegression-" + std::to_wstring(GetCurrentProcessId()) +
                                       L"-" + std::to_wstring(GetTickCount64());
        if (chain) {
            event = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
            if (!event || GetLastError() == ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Cannot create a unique private test event");
            checkEvent(event, eventName);
        }
        redirectPath = std::filesystem::absolute(argv[2]).wstring();
        if (GetModuleHandleW(redirectPath.c_str()))
            throw std::runtime_error("Target was already mapped before negotiation");
        runtime = LoadLibraryW(argv[1]);
        if (!runtime)
            throw std::runtime_error("Cannot load actual runtime " + std::to_string(GetLastError()));
        if (self != (GetModuleHandleW(redirectPath.c_str()) == runtime))
            throw std::runtime_error("Self mode requires the caller DLL; other modes require a distinct target DLL");
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
            GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
        if (!negotiate)
            throw std::runtime_error("Actual runtime has no negotiation export");
        if (!live) {
            detour(true);
            attached = true;
        }
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
        if (attached) {
            detour(false);
            attached = false;
        }
        const auto target = GetModuleHandleW(redirectPath.c_str());
        const bool mapped = target != nullptr;
        std::cout << "result=" << result << " targetMapped=" << mapped
                  << " redirectSource=" << (live ? "real" : "synthetic") << " syntheticRegistryReads=" << redirectReads
                  << '\n';
        if (!live && redirectReads != redirectReadLimit)
            throw std::runtime_error(actualTarget ? "Negotiation recursively followed its own redirect target"
                                                  : "Actual runtime did not read synthetic redirection data");
        const auto checkLocalNegotiation = [&]() {
            // Negotiation avoids dispatch, which creates the real runtime singleton.
            const auto localNegotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
                GetProcAddress(target, "xrNegotiateLoaderRuntimeInterface"));
            if (!localNegotiate)
                throw std::runtime_error("Actual target has no negotiation export");
            XrNegotiateRuntimeRequest localRequest{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
                                                   XR_RUNTIME_INFO_STRUCT_VERSION,
                                                   sizeof(XrNegotiateRuntimeRequest)};
            const auto previousReads = redirectReads;
            redirectReadLimit = 0;
            detour(true);
            attached = true;
            const auto localResult = localNegotiate(&loader, &localRequest);
            detour(false);
            attached = false;
            if (redirectReads != previousReads + 1 || localResult != XR_SUCCESS ||
                localRequest.getInstanceProcAddr != request.getInstanceProcAddr ||
                localRequest.runtimeInterfaceVersion != request.runtimeInterfaceVersion ||
                localRequest.runtimeApiVersion != request.runtimeApiVersion)
                throw std::runtime_error("Redirect changed the target's direct local negotiation result");
        };
        if (success) {
            if (result != XR_SUCCESS || !mapped || !request.getInstanceProcAddr)
                throw std::runtime_error(live ? "Real redirect_to did not return dispatch from the expected target"
                                              : "Successful redirect must retain valid mapped dispatch code");
            if (request.runtimeInterfaceVersion < loader.minInterfaceVersion ||
                request.runtimeInterfaceVersion > loader.maxInterfaceVersion ||
                request.runtimeApiVersion < loader.minApiVersion || request.runtimeApiVersion > loader.maxApiVersion)
                throw std::runtime_error("Redirect returned incompatible negotiated versions");
            HMODULE dispatchOwner{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(request.getInstanceProcAddr),
                                    &dispatchOwner) ||
                dispatchOwner != target || (chain && dispatchOwner == runtime))
                throw std::runtime_error("Negotiated dispatch address does not belong to the expected target module");
            if (actualTarget) {
                checkLocalNegotiation();
            } else {
                PFN_xrVoidFunction function{};
                if (request.getInstanceProcAddr(XR_NULL_HANDLE, "testUnimplementedFunction", &function) !=
                    XR_ERROR_FUNCTION_UNSUPPORTED)
                    throw std::runtime_error("Returned dispatch pointer is not the live target implementation");
            }
        } else if (result != XR_ERROR_INITIALIZATION_FAILED || mapped || request.getInstanceProcAddr) {
            throw std::runtime_error("Failed redirect retained a module reference or changed the target error");
        }
        if (chain) {
            std::cout << "Checking private event with both runtime modules mapped\n";
            checkEvent(event, eventName);
        }
        if (!FreeLibrary(runtime))
            throw std::runtime_error("Caller runtime unload failed");
        runtime = nullptr;
        if (self && GetModuleHandleW(redirectPath.c_str()))
            throw std::runtime_error("Self redirect retained an extra module reference");
        if (chain) {
            if (GetModuleHandleW(argv[1]) || GetModuleHandleW(redirectPath.c_str()) != target)
                throw std::runtime_error("Caller unload must retain only the distinct target runtime");
            std::cout << "Checking private event after caller unload\n";
            checkEvent(event, eventName);
            std::cout << "Checking direct target negotiation after caller unload\n";
            checkLocalNegotiation();
            CloseHandle(event);
            event = nullptr;
        }
        std::cout << "PASS: "
                  << (self      ? "self redirect uses local dispatch without recursion or module leak"
                      : live    ? "real redirect retains distinct custom runtime dispatch after caller unload"
                      : chain   ? "stock redirect retains distinct custom runtime dispatch without recursion"
                      : success ? "successful redirect retains dispatch module"
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
        if (event)
            CloseHandle(event);
        return 1;
    }
}
