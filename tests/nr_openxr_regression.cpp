// Black-box NR regression: graphics changes and repeated sessions on a real runtime.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
#define XR_FUNCTIONS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) X(xrGetD3D11GraphicsRequirementsKHR) \
    X(xrCreateSession) X(xrDestroySession) X(xrPollEvent) X(xrBeginSession) X(xrEndSession) \
    X(xrRequestExitSession) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrCreateSwapchain) \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)
#define DECLARE(name) PFN_##name name;
XR_FUNCTIONS(DECLARE)
#undef DECLARE

void checkXr(XrResult result, const char* call) {
    if (XR_FAILED(result)) throw std::runtime_error(std::string(call) + " failed: " + std::to_string(result));
}
void checkHr(HRESULT result) {
    if (FAILED(result)) throw std::runtime_error("D3D failed: " + std::to_string(result));
}
#define XR_CHECK(call) checkXr(call, #call)

struct Images {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageD3D11KHR> images;
    DXGI_FORMAT format{};
    bool depth{};
};

Images createImages(XrSession session, DXGI_FORMAT format, int width, int height,
                    uint32_t samples, bool depth) {
    Images result;
    result.format = format;
    result.depth = depth;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.width = width;
    info.height = height;
    info.sampleCount = samples;
    info.arraySize = 2;
    info.faceCount = info.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(session, &info, &result.swapchain));
    uint32_t count = 0;
    XR_CHECK(xrEnumerateSwapchainImages(result.swapchain, 0, &count, nullptr));
    result.images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(result.swapchain, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(result.images.data())));
    return result;
}

void destroyImages(Images& images) {
    if (images.swapchain) {
        images.images.clear();
        XR_CHECK(xrDestroySwapchain(images.swapchain));
        images.swapchain = XR_NULL_HANDLE;
    }
}

void fillImages(Images& images, ID3D11Device* device, ID3D11DeviceContext* context) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(images.swapchain, &acquire, &index));
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(images.swapchain, &wait));
    D3D11_TEXTURE2D_DESC textureDesc{};
    images.images[index].texture->GetDesc(&textureDesc);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (images.depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
            desc.Format = images.format;
            if (textureDesc.SampleDesc.Count > 1) {
                desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                desc.Texture2DMSArray.FirstArraySlice = eye;
                desc.Texture2DMSArray.ArraySize = 1;
            } else {
                desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.FirstArraySlice = eye;
                desc.Texture2DArray.ArraySize = 1;
            }
            ComPtr<ID3D11DepthStencilView> view;
            checkHr(device->CreateDepthStencilView(images.images[index].texture, &desc, &view));
            context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
        } else {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = images.format;
            if (textureDesc.SampleDesc.Count > 1) {
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                desc.Texture2DMSArray.FirstArraySlice = eye;
                desc.Texture2DMSArray.ArraySize = 1;
            } else {
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.FirstArraySlice = eye;
                desc.Texture2DArray.ArraySize = 1;
            }
            ComPtr<ID3D11RenderTargetView> view;
            checkHr(device->CreateRenderTargetView(images.images[index].texture, &desc, &view));
            const float color[4] = {0.08f, 0.10f + eye * 0.02f, 0.12f, 1.f};
            context->ClearRenderTargetView(view.Get(), color);
        }
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(images.swapchain, &release));
}

void submitFrames(XrSession session, XrSpace space, Images& color, Images* depth,
                  ID3D11Device* device, ID3D11DeviceContext* context,
                  int width, int height, bool unequalEyes = false, bool offsetDepth = false, bool scaledDepth = false,
                  bool cropDepthStress = false) {
    uint32_t randomState = 0x4e525631u;
    const auto nextRandom = [&] { randomState = randomState * 1664525u + 1013904223u; return randomState; };
    const auto crop = [&](int size) {
        XrRect2Di rect{};
        rect.extent.width = size/2 + nextRandom() % (size/2+1);
        rect.extent.height = size/2 + nextRandom() % (size/2+1);
        rect.offset.x = nextRandom() % (size-rect.extent.width+1);
        rect.offset.y = nextRandom() % (size-rect.extent.height+1);
        return rect;
    };
    if (cropDepthStress) std::cout << "STRESS seed=0x4e525631 frames=128 color=768x768 depth=384x384\n";
    for (int frame = 0; frame < (cropDepthStress ? 128 : 8); ++frame) {
        const bool submitDepth = depth && (!cropDepthStress || (frame % 4 != 0));
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session, &wait, &state));
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        XR_CHECK(xrBeginFrame(session, &begin));
        fillImages(color, device, context);
        if (submitDepth) fillImages(*depth, device, context);
        XrCompositionLayerProjectionView views[2] = {
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerDepthInfoKHR depths[2] = {
            {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}, {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            views[eye].pose.orientation.w = 1.f;
            views[eye].pose.position = {eye ? 0.032f : -0.032f, 1.6f, 0.f};
            views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
            views[eye].subImage = {color.swapchain, {{0, 0}, {width, height}}, eye};
            if (unequalEyes && eye) views[eye].subImage.imageRect.extent = {width - 32, height - 16};
            if (cropDepthStress) views[eye].subImage.imageRect = crop(width);
            if (submitDepth) {
                depths[eye].subImage = {depth->swapchain,
                    {{offsetDepth ? 64 : 0, offsetDepth ? 32 : 0},
                     {scaledDepth ? width / 2 : width, scaledDepth ? height / 2 : height}}, eye};
                if (cropDepthStress) depths[eye].subImage.imageRect = crop(384);
                depths[eye].minDepth = 0.f;
                depths[eye].maxDepth = 1.f;
                depths[eye].nearZ = 0.1f;
                depths[eye].farZ = 100.f;
                views[eye].next = &depths[eye];
            }
        }
        if (cropDepthStress) {
            std::cout << "STRESS frame=" << frame << " depth=" << submitDepth;
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const auto printRect = [&](const char* label, const XrRect2Di& rect) {
                    std::cout << ' ' << label << eye << '=' << rect.offset.x << ',' << rect.offset.y
                        << ':' << rect.extent.width << 'x' << rect.extent.height;
                };
                printRect("color", views[eye].subImage.imageRect);
                if (submitDepth) printRect("depth", depths[eye].subImage.imageRect);
            }
            std::cout << '\n';
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = space;
        layer.viewCount = 2;
        layer.views = views;
        const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = 1;
        end.layers = &header;
        // Submitting while shouldRender=false is legal; it still exercises the runtime's NR path.
        XR_CHECK(xrEndFrame(session, &end));
        checkHr(device->GetDeviceRemovedReason());
    }
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        if (argc != 3) throw std::runtime_error("Usage: nr_openxr_regression <loader.dll> <test case>");
        const std::string mode = argv[2];
        for (int instancePass = 0; instancePass < (mode == "instance-restart" ? 2 : 1); ++instancePass) {
        std::cout << "CREATE instance " << instancePass << '\n';
        HMODULE loader = LoadLibraryA(argv[1]);
        if (!loader) throw std::runtime_error("Cannot load OpenXR loader");
        auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
        if (!getProc) throw std::runtime_error("Missing xrGetInstanceProcAddr");
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR NR resource regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 2;
        info.enabledExtensionNames = extensions;
        XrInstance instance = XR_NULL_HANDLE;
        XR_CHECK(xrCreateInstance(&info, &instance));
#define LOAD(name) XR_CHECK(getProc(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)));
        XR_FUNCTIONS(LOAD)
#undef LOAD
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system{};
        XR_CHECK(xrGetSystem(instance, &systemInfo, &system));
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(instance, system, &requirements));
        ComPtr<IDXGIFactory1> factory;
        checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            checkHr(factory->EnumAdapters1(i, &adapter));
            DXGI_ADAPTER_DESC1 desc{};
            checkHr(adapter->GetDesc1(&desc));
            if (!memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID))) break;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checkHr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            &requirements.minFeatureLevel, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
        const int sessions = mode == "restart" ? 3 : 1;
        for (int pass = 0; pass < sessions; ++pass) {
            std::cout << "CREATE session " << pass << '\n';
            XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
            binding.device = device.Get();
            XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
            create.next = &binding;
            create.systemId = system;
            XrSession session{};
            XR_CHECK(xrCreateSession(instance, &create, &session));
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XR_CHECK(xrBeginSession(session, &begin));
            XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            reference.poseInReferenceSpace.orientation.w = 1.f;
            XrSpace space{};
            XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
            const bool cropDepthStress = mode == "crop-depth-stress";
            const int width = cropDepthStress ? 768 : pass == 2 ? 384 : 512;
            Images color = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, width, width, 1, false);
            Images depth = createImages(session, DXGI_FORMAT_D32_FLOAT,
                cropDepthStress ? 384 : mode == "depth-offset" ? width + 64 : mode == "depth-scale" ? width / 2 : width,
                cropDepthStress ? 384 : mode == "depth-offset" ? width + 32 : mode == "depth-scale" ? width / 2 : width, 1, true);
            std::cout << (cropDepthStress ? "FRAMES crop/depth stress\n" : "FRAMES with depth\n");
            submitFrames(session, space, color, &depth, device.Get(), context.Get(), width, width,
                mode == "unequal", mode == "depth-offset", mode == "depth-scale", cropDepthStress);
            if (!GetModuleHandleA("nvngx_dlssnr.dll"))
                throw std::runtime_error("NR module was not loaded; this is not an NR test");
            destroyImages(depth);
            if (!cropDepthStress) {
                std::cout << "FRAMES after depth removal\n";
                submitFrames(session, space, color, nullptr, device.Get(), context.Get(), width, width, mode == "unequal");
            }
            if (mode == "transitions") {
                destroyImages(color);
                color = createImages(session, DXGI_FORMAT_R16G16B16A16_FLOAT, width, width, 1, false);
                std::cout << "FRAMES after format change\n";
                submitFrames(session, space, color, nullptr, device.Get(), context.Get(), width, width);
                destroyImages(color);
                color = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 640, 640, 4, false);
                depth = createImages(session, DXGI_FORMAT_D32_FLOAT, 640, 640, 4, true);
                std::cout << "FRAMES after resolution/MSAA change\n";
                submitFrames(session, space, color, &depth, device.Get(), context.Get(), 640, 640);
                destroyImages(depth);
            }
            if (mode == "pending-resize") {
                // Keep original inputs alive so destruction does not pre-drain the submission queue.
                Images alternate = createImages(session, DXGI_FORMAT_R16G16B16A16_FLOAT, width, width, 1, false);
                std::cout << "FRAMES alternating live formats\n";
                for (int change = 0; change < 4; ++change) {
                    submitFrames(session, space, alternate, nullptr, device.Get(), context.Get(), width, width);
                    submitFrames(session, space, color, nullptr, device.Get(), context.Get(), width, width);
                }
                Images growth = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 640, 640, 1, false);
                std::cout << "FRAMES changing live viewport sizes\n";
                submitFrames(session, space, growth, nullptr, device.Get(), context.Get(), 512, 512);
                submitFrames(session, space, growth, nullptr, device.Get(), context.Get(), 640, 640);
                submitFrames(session, space, growth, nullptr, device.Get(), context.Get(), 384, 384);
                destroyImages(alternate);
                destroyImages(growth);
            }
            destroyImages(color);
            XR_CHECK(xrDestroySpace(space));
            std::cout << "EXIT session " << pass << '\n';
            XR_CHECK(xrRequestExitSession(session));
            std::cout << "EXIT requested\n";
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            bool stopping = false;
            while (xrPollEvent(instance, &event) == XR_SUCCESS) {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
                    reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state == XR_SESSION_STATE_STOPPING)
                    stopping = true;
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            if (!stopping) throw std::runtime_error("Session did not reach STOPPING");
            XR_CHECK(xrEndSession(session));
            std::cout << "EXIT ended\n";
            XR_CHECK(xrDestroySession(session));
            std::cout << "EXIT destroyed\n";
        }
        std::cout << "EXIT instance\n";
        XR_CHECK(xrDestroyInstance(instance));
        std::cout << "EXIT instance destroyed\n";
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH))
            throw std::runtime_error("Module-name API broken after runtime unload");
        FreeLibrary(loader);
        }
        std::cout << "PASS: " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
