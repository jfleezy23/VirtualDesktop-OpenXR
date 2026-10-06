// Test-only runtime negotiation targets. Never install or register these DLLs.
#define WIN32_LEAN_AND_MEAN
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#ifdef REDIRECT_TEST_SUCCESS
static XrResult XRAPI_CALL getProc(XrInstance, const char*, PFN_xrVoidFunction* function) {
    if (function) *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
#endif
extern "C" __declspec(dllexport) XrVersion XRAPI_CALL getVersion() { return ~XrVersion(0); }
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest* request) {
#ifdef REDIRECT_TEST_SUCCESS
    request->getInstanceProcAddr = getProc;
    request->runtimeInterfaceVersion = 1;
    request->runtimeApiVersion = XR_CURRENT_API_VERSION;
    return XR_SUCCESS;
#else
    (void)request;
    return XR_ERROR_INITIALIZATION_FAILED;
#endif
}
