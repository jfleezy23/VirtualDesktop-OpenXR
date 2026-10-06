// Exercise actual runtime negotiation from an isolated path longer than MAX_PATH.
// Run in a child process: the baseline overstates its WCHAR buffer capacity.
#define WIN32_LEAN_AND_MEAN
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 2 || std::filesystem::absolute(argv[1]).wstring().size() <= MAX_PATH) return 2;
    HMODULE runtime = LoadLibraryW(argv[1]);
    if (!runtime) { std::cerr << "LoadLibrary failed: " << GetLastError() << '\n'; return 2; }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(runtime,"xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) return 2;
    XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
        XR_LOADER_INFO_STRUCT_VERSION,sizeof(XrNegotiateLoaderInfo),1,1,
        XR_MAKE_VERSION(1,0,0),XR_CURRENT_API_VERSION};
    XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
        XR_RUNTIME_INFO_STRUCT_VERSION,sizeof(XrNegotiateRuntimeRequest)};
    const auto result = negotiate(&loader,&request);
    FreeLibrary(runtime);
    std::cout << "long-path negotiation result=" << result << " expected=" << XR_ERROR_INITIALIZATION_FAILED << '\n';
    if (result != XR_ERROR_INITIALIZATION_FAILED || request.getInstanceProcAddr) return 1;
    std::cout << "PASS: oversized runtime path is rejected without overflowing the buffer\n";
    return 0;
}
