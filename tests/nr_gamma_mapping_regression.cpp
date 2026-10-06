// Runtime-linked mapping test. Build with headers/objects captured from the SAME build.
// Only the retained declared format is injected; the real source texture/mapped DXGI
// format and all allocation/shader execution are production code. No Vulkan/GL API claim.
#include "pch.h"
#include "runtime.h"

extern "C" XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);
namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
    static PFN_xrGetInstanceProcAddr getDispatch(const char* runtimePath) {
        dllHome = std::filesystem::absolute(runtimePath).parent_path();
        XrNegotiateLoaderInfo info{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
            XR_LOADER_INFO_STRUCT_VERSION,sizeof(XrNegotiateLoaderInfo),1,1,
            XR_MAKE_VERSION(1,0,0),XR_CURRENT_API_VERSION};
        XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
            XR_RUNTIME_INFO_STRUCT_VERSION,sizeof(XrNegotiateRuntimeRequest)};
        const auto result = xrNegotiateLoaderRuntimeInterface(&info,&request);
        if (XR_FAILED(result) || !request.getInstanceProcAddr) throw std::runtime_error("Linked runtime negotiation failed");
        return request.getInstanceProcAddr;
    }
    static void setDeclaredFormat(XrSwapchain chain, int64_t format) {
        ((OpenXrRuntime::Swapchain*)chain)->xrDesc.format = format;
    }
};
}
#define NR_GAMMA_MAPPING_TEST
#include "nr_pipeline_regression.cpp"
