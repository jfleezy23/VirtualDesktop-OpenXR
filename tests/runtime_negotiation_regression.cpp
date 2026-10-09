// Actual-DLL negotiation matrix. Redirect reads are suppressed process-locally;
// no registry writes, runtime singleton, graphics device or headset is required.
#define WIN32_LEAN_AND_MEAN
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <detours.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    decltype(&RegGetValueW) originalRead = RegGetValueW;
    unsigned redirectReads{};
    LSTATUS WINAPI
    readRegistry(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD bytes) {
        if (key == HKEY_LOCAL_MACHINE && value && !_wcsicmp(value, L"redirect_to")) {
            ++redirectReads;
            return ERROR_FILE_NOT_FOUND;
        }
        return originalRead(key, subkey, value, flags, type, data, bytes);
    }
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    struct RedirectSuppression {
        RedirectSuppression() {
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto attached = DetourAttach(reinterpret_cast<PVOID*>(&originalRead), readRegistry);
            if (updated || attached) {
                DetourTransactionAbort();
                throw std::runtime_error("Detour attach failed");
            }
            require(DetourTransactionCommit() == NO_ERROR, "Detour commit failed");
        }
        ~RedirectSuppression() {
            const auto begun = DetourTransactionBegin();
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto detached = DetourDetach(reinterpret_cast<PVOID*>(&originalRead), readRegistry);
            const auto committed = DetourTransactionCommit();
            if (begun || updated || detached || committed)
                std::terminate();
        }
    };
    XrResult XRAPI_PTR sentinel(XrInstance, const char*, PFN_xrVoidFunction*) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    struct Case {
        const char* label;
        bool success;
        std::function<void(XrNegotiateLoaderInfo&, XrNegotiateRuntimeRequest&)> mutate;
    };
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf;
    HMODULE runtime{};
    try {
        require(argc == 2, "Usage: runtime_negotiation_regression <actual-runtime.dll>");
        runtime = LoadLibraryW(argv[1]);
        require(runtime != nullptr, "Cannot load actual runtime");
        const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
            GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
        require(negotiate != nullptr, "Negotiation export missing");
        unsigned failures = 0;
        {
            RedirectSuppression suppressed;
            const std::vector<Case> cases = {
                {"exact interface", true, [](auto&, auto&) {}},
                {"future maximum interface", true, [](auto& l, auto&) { l.maxInterfaceVersion = 2; }},
                {"unbounded maximum interface", true, [](auto& l, auto&) { l.maxInterfaceVersion = UINT32_MAX; }},
                {"zero-to-unbounded interface",
                 true,
                 [](auto& l, auto&) {
                     l.minInterfaceVersion = 0;
                     l.maxInterfaceVersion = UINT32_MAX;
                 }},
                {"interface below supported",
                 false,
                 [](auto& l, auto&) { l.minInterfaceVersion = l.maxInterfaceVersion = 0; }},
                {"interface above supported",
                 false,
                 [](auto& l, auto&) { l.minInterfaceVersion = l.maxInterfaceVersion = 2; }},
                {"reversed interface",
                 false,
                 [](auto& l, auto&) {
                     l.minInterfaceVersion = 2;
                     l.maxInterfaceVersion = 1;
                 }},
                {"exact API", true, [](auto& l, auto&) { l.minApiVersion = l.maxApiVersion = XR_CURRENT_API_VERSION; }},
                {"loader-style API range",
                 true,
                 [](auto& l, auto&) { l.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff); }},
                {"API below supported", false, [](auto& l, auto&) { l.maxApiVersion = XR_CURRENT_API_VERSION - 1; }},
                {"API above supported",
                 false,
                 [](auto& l, auto&) {
                     l.minApiVersion = XR_CURRENT_API_VERSION + 1;
                     l.maxApiVersion = XR_CURRENT_API_VERSION + 2;
                 }},
                {"reversed API",
                 false,
                 [](auto& l, auto&) {
                     l.minApiVersion = XR_CURRENT_API_VERSION + 1;
                     l.maxApiVersion = XR_CURRENT_API_VERSION - 1;
                 }},
                {"loader type",
                 false,
                 [](auto& l, auto&) { l.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST; }},
                {"loader version", false, [](auto& l, auto&) { ++l.structVersion; }},
                {"loader short size", false, [](auto& l, auto&) { --l.structSize; }},
                {"loader long size", false, [](auto& l, auto&) { ++l.structSize; }},
                {"request type", false, [](auto&, auto& r) { r.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO; }},
                {"request version", false, [](auto&, auto& r) { ++r.structVersion; }},
                {"request short size", false, [](auto&, auto& r) { --r.structSize; }},
                {"request long size", false, [](auto&, auto& r) { ++r.structSize; }},
            };
            for (const auto& item : cases) {
                XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                             XR_LOADER_INFO_STRUCT_VERSION,
                                             sizeof(XrNegotiateLoaderInfo),
                                             1,
                                             1,
                                             XR_MAKE_VERSION(1, 0, 0),
                                             XR_CURRENT_API_VERSION};
                XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
                                                  XR_RUNTIME_INFO_STRUCT_VERSION,
                                                  sizeof(XrNegotiateRuntimeRequest),
                                                  91,
                                                  92,
                                                  sentinel};
                item.mutate(loader, request);
                const auto previous = request;
                const auto reads = redirectReads;
                const auto result = negotiate(&loader, &request);
                bool correct = redirectReads > reads;
                if (item.success) {
                    HMODULE owner{};
                    correct &= result == XR_SUCCESS &&
                               request.runtimeInterfaceVersion == XR_CURRENT_LOADER_RUNTIME_VERSION &&
                               request.runtimeApiVersion == XR_CURRENT_API_VERSION && request.getInstanceProcAddr &&
                               request.getInstanceProcAddr != sentinel;
                    correct &= request.getInstanceProcAddr &&
                               GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                  reinterpret_cast<LPCWSTR>(request.getInstanceProcAddr),
                                                  &owner) &&
                               owner == runtime;
                } else {
                    correct &= result == XR_ERROR_INITIALIZATION_FAILED &&
                               request.runtimeInterfaceVersion == previous.runtimeInterfaceVersion &&
                               request.runtimeApiVersion == previous.runtimeApiVersion &&
                               request.getInstanceProcAddr == previous.getInstanceProcAddr;
                }
                correct &= request.structType == previous.structType &&
                           request.structVersion == previous.structVersion && request.structSize == previous.structSize;
                std::cout << (correct ? "PASS: " : "FAIL: ") << item.label << " result=" << result << '\n';
                failures += !correct;
            }
            XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                         XR_LOADER_INFO_STRUCT_VERSION,
                                         sizeof(XrNegotiateLoaderInfo),
                                         1,
                                         1,
                                         XR_MAKE_VERSION(1, 0, 0),
                                         XR_CURRENT_API_VERSION};
            XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
                                              XR_RUNTIME_INFO_STRUCT_VERSION,
                                              sizeof(XrNegotiateRuntimeRequest),
                                              91,
                                              92,
                                              sentinel};
            for (const auto pointers : {1, 2, 3}) {
                const auto result = negotiate(pointers & 1 ? nullptr : &loader, pointers & 2 ? nullptr : &request);
                const bool correct = result == XR_ERROR_INITIALIZATION_FAILED &&
                                     request.runtimeInterfaceVersion == 91 && request.runtimeApiVersion == 92 &&
                                     request.getInstanceProcAddr == sentinel;
                std::cout << (correct ? "PASS: " : "FAIL: ") << "null structure combination " << pointers << '\n';
                failures += !correct;
            }
        }
        require(FreeLibrary(runtime) != FALSE, "Runtime unload failed");
        runtime = nullptr;
        std::cout << "failures=" << failures << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (runtime)
            FreeLibrary(runtime);
        return 1;
    }
}
