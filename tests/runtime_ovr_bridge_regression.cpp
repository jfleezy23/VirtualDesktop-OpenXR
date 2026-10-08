// CPU-only actual-DLL regression: no VR session or production registry writes.
// Markers stand in for external modules; runtime negotiation and event hooks are real.
#define WIN32_LEAN_AND_MEAN
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <detours.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    decltype(&RegGetValueW) originalRegistry = RegGetValueW;
    decltype(&RegOpenKeyExW) originalOpenKey = RegOpenKeyExW;
    decltype(&CreateEventW) originalCreateEvent = CreateEventW;
    decltype(&OpenEventW) originalOpenEvent = OpenEventW;
    std::wstring markerDirectory;
    bool ownInjection{};
    unsigned fakeEvents{};
    unsigned forwardedEvents{};
    void* eventCode{};

    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }
    bool runtimeKey(LPCWSTR key) {
        return key && (!_wcsicmp(key, L"SOFTWARE\\Virtual Desktop, Inc.\\OpenXR") || !_wcsicmp(key, L"SOFTWARE\\VDXR"));
    }
    LSTATUS WINAPI
    readRegistry(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
        if (key == HKEY_LOCAL_MACHINE && runtimeKey(subkey)) {
            if (!value || _wcsicmp(value, L"quirk_force_own_injection"))
                return ERROR_FILE_NOT_FOUND;
            if (!size)
                return ERROR_INVALID_PARAMETER;
            const DWORD capacity = *size;
            *size = sizeof(DWORD);
            if (type)
                *type = REG_DWORD;
            if (!data)
                return ERROR_SUCCESS;
            if (capacity < sizeof(DWORD))
                return ERROR_MORE_DATA;
            const DWORD result = ownInjection ? 1 : 0;
            memcpy(data, &result, sizeof(result));
            return ERROR_SUCCESS;
        }
        if (key == HKEY_LOCAL_MACHINE && subkey &&
            !_wcsicmp(subkey, L"SOFTWARE\\Virtual Desktop, Inc.\\Virtual Desktop Streamer") && value &&
            !_wcsicmp(value, L"Path")) {
            if (!size)
                return ERROR_INVALID_PARAMETER;
            const DWORD required = static_cast<DWORD>((markerDirectory.size() + 1) * sizeof(wchar_t));
            const DWORD capacity = *size;
            *size = required;
            if (type)
                *type = REG_SZ;
            if (!data)
                return ERROR_SUCCESS;
            if (capacity < required)
                return ERROR_MORE_DATA;
            memcpy(data, markerDirectory.c_str(), required);
            return ERROR_SUCCESS;
        }
        return originalRegistry(key, subkey, value, flags, type, data, size);
    }
    LSTATUS WINAPI openKey(HKEY key, LPCWSTR subkey, DWORD options, REGSAM access, PHKEY result) {
        // Disable only this process's runtime watcher. Other registry access stays native.
        if (key == HKEY_LOCAL_MACHINE && runtimeKey(subkey))
            return ERROR_FILE_NOT_FOUND;
        return originalOpenKey(key, subkey, options, access, result);
    }
    HANDLE WINAPI createEvent(LPSECURITY_ATTRIBUTES attributes, BOOL manual, BOOL signaled, LPCWSTR name) {
        if (!name && manual && signaled)
            ++fakeEvents;
        return originalCreateEvent(attributes, manual, signaled, name);
    }
    HANDLE WINAPI openEvent(DWORD access, BOOL inherit, LPCWSTR name) {
        ++forwardedEvents;
        return originalOpenEvent(access, inherit, name);
    }
    void checkDetour(LONG error) {
        require(error == NO_ERROR, "Test detour transaction failed");
    }
    void observers(bool attach) {
        checkDetour(DetourTransactionBegin());
        checkDetour(DetourUpdateThread(GetCurrentThread()));
        checkDetour(attach ? DetourAttach(reinterpret_cast<PVOID*>(&originalRegistry), readRegistry)
                           : DetourDetach(reinterpret_cast<PVOID*>(&originalRegistry), readRegistry));
        checkDetour(attach ? DetourAttach(reinterpret_cast<PVOID*>(&originalOpenKey), openKey)
                           : DetourDetach(reinterpret_cast<PVOID*>(&originalOpenKey), openKey));
        checkDetour(attach ? DetourAttach(reinterpret_cast<PVOID*>(&originalCreateEvent), createEvent)
                           : DetourDetach(reinterpret_cast<PVOID*>(&originalCreateEvent), createEvent));
        checkDetour(DetourTransactionCommit());
    }
    void bridgeHook(bool attach) {
        checkDetour(DetourTransactionBegin());
        checkDetour(DetourUpdateThread(GetCurrentThread()));
        checkDetour(attach ? DetourAttach(reinterpret_cast<PVOID*>(&originalOpenEvent), openEvent)
                           : DetourDetach(reinterpret_cast<PVOID*>(&originalOpenEvent), openEvent));
        checkDetour(DetourTransactionCommit());
    }
    std::array<unsigned char, 16> eventEntry() {
        std::array<unsigned char, 16> bytes{};
        // Resolve Windows export thunks once, before any OpenEventW detour is installed.
        memcpy(bytes.data(), eventCode, bytes.size());
        return bytes;
    }
    void checkOperationEvent(const std::wstring& name) {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
        require(event != nullptr, "Unrelated operation event failed to open");
        const DWORD state = WaitForSingleObject(event, 0);
        CloseHandle(event);
        require(state == WAIT_TIMEOUT, "Operation event must remain nonsignaled");
    }
    HMODULE marker(const wchar_t* name) {
        const auto path = std::filesystem::path(markerDirectory) / name;
        HMODULE module = LoadLibraryW(path.c_str());
        require(module != nullptr, "Test marker failed to load");
        return module;
    }
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf;
    // Failure exits intentionally abandon this isolated process's test hooks; the OS reclaims them.
    try {
        require(argc == 4, "Usage: <actual-bundle-runtime.dll> <marker-directory> <mode>");
        const std::wstring mode(argv[3]);
        const bool bridgePresent =
            mode == L"bridge" || mode == L"bridge-own" || mode == L"unload-bridge" || mode == L"bridge-no-plugin";
        const bool lateBridge = mode == L"late-bridge";
        ownInjection = mode == L"bridge-own" || mode == L"native-own";
        require(bridgePresent || lateBridge || mode == L"native" || mode == L"native-own", "Unknown test mode");
        markerDirectory = std::filesystem::absolute(argv[2]).wstring();
        eventCode = DetourCodeFromPointer(
            reinterpret_cast<PVOID>(GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "OpenEventW")), nullptr);
        require(eventCode != nullptr, "Cannot resolve native OpenEventW");
        require(!GetModuleHandleW(L"OVRPlugin.dll") && !GetModuleHandleW(L"LibReviveXR64.dll"),
                "Markers must not be preloaded");
        const bool pluginPresent = mode != L"bridge-no-plugin";
        HMODULE plugin = pluginPresent ? marker(L"OVRPlugin.dll") : nullptr;
        HMODULE bridge = bridgePresent ? marker(L"LibReviveXR64.dll") : nullptr;
        observers(true);
        if (bridgePresent)
            bridgeHook(true);
        const auto beforeRuntime = eventEntry();
        const std::wstring eventName = L"VDXR.BridgeRegression.Operation." + std::to_wstring(GetCurrentProcessId());
        HANDLE operation = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        require(operation != nullptr, "Cannot create test operation event");
        HMODULE runtime = LoadLibraryW(argv[1]);
        require(runtime != nullptr, "Actual runtime failed to load");
        const bool hookChanged = beforeRuntime != eventEntry();
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
            GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
        require(negotiate != nullptr, "Missing runtime negotiation export");
        XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                     XR_LOADER_INFO_STRUCT_VERSION,
                                     sizeof(XrNegotiateLoaderInfo),
                                     1,
                                     1,
                                     XR_MAKE_VERSION(1, 0, 0),
                                     XR_CURRENT_API_VERSION};
        XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
                                          XR_RUNTIME_INFO_STRUCT_VERSION,
                                          sizeof(XrNegotiateRuntimeRequest)};
        require(negotiate(&loader, &request) == XR_SUCCESS && request.getInstanceProcAddr,
                "Actual runtime negotiation failed");
        const auto getFunction = [&](XrInstance instance, const char* name) {
            PFN_xrVoidFunction result{};
            require(request.getInstanceProcAddr(instance, name, &result) == XR_SUCCESS && result,
                    "Cannot resolve runtime entry point");
            return result;
        };
        auto create = reinterpret_cast<PFN_xrCreateInstance>(getFunction(XR_NULL_HANDLE, "xrCreateInstance"));
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "Revive");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        XrInstance instance{};
        fakeEvents = 0;
        require(create(&info, &instance) == XR_SUCCESS, "Actual instance creation failed");
        auto properties =
            reinterpret_cast<PFN_xrGetInstanceProperties>(getFunction(instance, "xrGetInstanceProperties"));
        XrInstanceProperties output{XR_TYPE_INSTANCE_PROPERTIES};
        require(properties(instance, &output) == XR_SUCCESS &&
                    !strcmp(output.runtimeName, pluginPresent ? "Oculus" : "VirtualDesktopXR"),
                "Bridge detection must preserve application classification");
        const bool injected = GetModuleHandleW(L"VirtualDesktop.Injector64.dll") != nullptr;
        std::cout << "bridge=" << bridgePresent << " hookChanged=" << hookChanged << " injected=" << injected
                  << " fakeEvents=" << fakeEvents << '\n';
        require(hookChanged == !bridgePresent, "Revive owns OpenEventW; VDXR must not add a redundant hook");
        require(injected == (!bridgePresent && !ownInjection), "Runtime chose a redundant or missing injector path");
        require(fakeEvents == ((!bridgePresent && ownInjection) ? 1u : 0u),
                "Runtime chose a redundant or missing fake detection event");
        checkOperationEvent(eventName);
        if (bridgePresent) {
            for (unsigned cycle = 0; cycle != 16; ++cycle) {
                bridgeHook(false);
                checkOperationEvent(eventName);
                bridgeHook(true);
                checkOperationEvent(eventName);
            }
            require(forwardedEvents >= 17, "Bridge forwarding did not run");
        }
        if (lateBridge)
            bridge = marker(L"LibReviveXR64.dll");
        if (mode == L"unload-bridge") {
            FreeLibrary(bridge);
            bridge = nullptr;
        }
        auto destroy = reinterpret_cast<PFN_xrDestroyInstance>(getFunction(instance, "xrDestroyInstance"));
        require(destroy(instance) == XR_SUCCESS, "Actual instance teardown failed");
        require(FreeLibrary(runtime) != FALSE, "Actual runtime unload failed");
        require(!GetModuleHandleW(argv[1]), "Runtime remained mapped after successful teardown");
        require(beforeRuntime == eventEntry(), "Runtime teardown failed to restore its own hook state");
        checkOperationEvent(eventName);
        if (bridgePresent)
            bridgeHook(false);
        observers(false);
        if (bridge)
            FreeLibrary(bridge);
        if (plugin)
            FreeLibrary(plugin);
        CloseHandle(operation);
        std::cout << "PASS: Oculus bridge hook and injection lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        // Exit without C++ static teardown of the loaded runtime after failed assertions.
        ExitProcess(1);
    }
}
