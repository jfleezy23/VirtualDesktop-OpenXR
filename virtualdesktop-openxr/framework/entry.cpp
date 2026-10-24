// MIT License
//
// Copyright(c) 2022-2024 Matthieu Bucchianeri
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

#include <runtime.h>

#include "dispatch.h"
#include "log.h"

#include "version.h"

#ifndef RUNTIME_NAMESPACE
#error Must define RUNTIME_NAMESPACE
#endif

namespace RUNTIME_NAMESPACE {
    std::filesystem::path dllHome;

    // The path to store logs & others.
    std::filesystem::path programData;

    namespace log {
        // The file logger.
        std::ofstream logStream;
    } // namespace log
} // namespace RUNTIME_NAMESPACE

using namespace RUNTIME_NAMESPACE;
using namespace RUNTIME_NAMESPACE::log;

namespace {
    bool retainBundledHookOwner() {
#if defined(_WIN64)
        const auto installedPath =
            RegGetString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Virtual Desktop, Inc.\\Virtual Desktop Streamer", "Path");
        if (!installedPath) {
            return true;
        }
        const auto bundledPath = std::filesystem::path(*installedPath) / L"OpenXR" / L"virtualdesktop-openxr.dll";
        wil::unique_hmodule bundled;
        // This acquires only an already loaded module; direct loading must not introduce the stock runtime.
        if (!GetModuleHandleExW(0, bundledPath.c_str(), bundled.put())) {
            return true;
        }
        HMODULE current{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&dllHome),
                                &current)) {
            return false;
        }
        if (bundled.get() == current || !GetProcAddress(bundled.get(), "xrNegotiateLoaderRuntimeInterface") ||
            !GetProcAddress(bundled.get(), "getVersion")) {
            return true;
        }
        // Stock owns a process-wide detour even after forwarding. Its unload can remove Revive's newer hook.
        // Pin the retained module's base, since an exported function can itself forward into another module.
        HMODULE pinned{};
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  reinterpret_cast<LPCWSTR>(bundled.get()),
                                  &pinned) != FALSE;
#else
        return true;
#endif
    }
} // namespace

extern "C" {

XrVersion __declspec(dllexport) XRAPI_CALL getVersion() {
    return XR_MAKE_VERSION(RuntimeVersionMajor, RuntimeVersionMinor, RuntimeVersionPatch);
}

// Entry point for the loader.
XrResult __declspec(dllexport) XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                            XrNegotiateRuntimeRequest* runtimeRequest) {
    TraceLocalActivity(local);
    TraceLoggingWriteStart(local, "xrNegotiateLoaderRuntimeInterface");

    // Retrieve the path of the DLL.
    if (dllHome.empty()) {
        HMODULE module;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&dllHome,
                               &module)) {
            wchar_t path[_MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
            if (!length || length >= std::size(path)) {
                return XR_ERROR_INITIALIZATION_FAILED;
            }
            dllHome = std::filesystem::path(path).parent_path();
        } else {
            // Falling back to loading config/writing logs to the current working directory.
            DebugLog("Failed to locate DLL\n");
        }
    }

#ifndef STANDALONE_RUNTIME
    // This is the location for other Virtual Desktop logs.
    {
        WCHAR* path = nullptr;
        SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path);
        programData = std::filesystem::path(path ? path : L"") / L"Virtual Desktop";
        CoTaskMemFree(path);
    }
#else
    {
        WCHAR* path = nullptr;
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
        programData = std::filesystem::path(path ? path : L"") / LOG_FOLDER;
        CoTaskMemFree(path);
    }
#endif
    CreateDirectoryW(programData.wstring().c_str(), nullptr);

    // Start logging to file.
    if (!logStream.is_open()) {
        std::wstring logFile = (programData / ("OpenXR.log")).wstring();
        logStream.open(logFile, std::ios_base::ate);
    }

    Log("%s (%ls)\n", RuntimePrettyName.c_str(), dllHome.wstring().c_str());

#ifndef STANDALONE_RUNTIME
    {
        // Trampoline to the standalone/development runtime.
        const auto standaloneLibraryPath = RegGetString(HKEY_LOCAL_MACHINE, StandaloneRegPrefix, "redirect_to");
        HMODULE standaloneLibrary = nullptr;
        PFN_xrNegotiateLoaderRuntimeInterface pfnNegotiateLoaderRuntimeInterface = nullptr;
        decltype(getVersion)* pfnGetVersion = nullptr;
        if (standaloneLibraryPath && (standaloneLibrary = LoadLibraryW(standaloneLibraryPath.value().c_str())) &&
            (pfnNegotiateLoaderRuntimeInterface = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(
                 standaloneLibrary, "xrNegotiateLoaderRuntimeInterface")) &&
            // A redirected bundled build shares this setting; negotiate locally when it points back to us.
            pfnNegotiateLoaderRuntimeInterface != &xrNegotiateLoaderRuntimeInterface &&
            (pfnGetVersion = (decltype(getVersion)*)GetProcAddress(standaloneLibrary, "getVersion"))) {
            if (pfnGetVersion() >= getVersion()) {
                Log("Redirecting to standalone runtime (%ls)\n", standaloneLibraryPath.value().c_str());
                auto result = pfnNegotiateLoaderRuntimeInterface(loaderInfo, runtimeRequest);
                if (XR_SUCCEEDED(result) && !retainBundledHookOwner()) {
                    if (runtimeRequest) {
                        runtimeRequest->getInstanceProcAddr = nullptr;
                        runtimeRequest->runtimeInterfaceVersion = 0;
                        runtimeRequest->runtimeApiVersion = 0;
                    }
                    result = XR_ERROR_INITIALIZATION_FAILED;
                }
                // Successful dispatch pointers need the module for the process lifetime; failed negotiation does not.
                if (XR_FAILED(result)) {
                    FreeLibrary(standaloneLibrary);
                }
                return result;
            } else {
                Log("Cancelled redirection to older standalone runtime.\n");
            }
        }
        if (standaloneLibrary) {
            FreeLibrary(standaloneLibrary);
        }
    }
#endif

    if (!loaderInfo || !runtimeRequest || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxApiVersion < XR_CURRENT_API_VERSION || loaderInfo->minApiVersion > XR_CURRENT_API_VERSION) {
        Log("xrNegotiateLoaderRuntimeInterface validation failed\n");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (!retainBundledHookOwner()) {
        Log("Failed to retain the bundled runtime hook owner\n");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    // This is it! Tell the loader to use our API implementation.
    runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr;
    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;

    TraceLoggingWriteStop(local, "xrNegotiateLoaderRuntimeInterface");

    return XR_SUCCESS;
}
}
