// Black-box compositor depth geometry test using real D3D11 depth pixels.
// Settings/runtime selection are controlled by the caller; this executable never changes them.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <OVR_CAPI_D3D.h>
#include <detours.h>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
#define XR_FUNCTIONS(X)                                                                                                \
    X(xrCreateInstance)                                                                                                \
    X(xrDestroyInstance)                                                                                               \
    X(xrGetSystem)                                                                                                     \
    X(xrGetD3D11GraphicsRequirementsKHR)                                                                               \
    X(xrCreateSession)                                                                                                 \
    X(xrBeginSession)                                                                                                  \
    X(xrCreateReferenceSpace)                                                                                          \
    X(xrCreateSwapchain)                                                                                               \
    X(xrEnumerateSwapchainImages)                                                                                      \
    X(xrAcquireSwapchainImage)                                                                                         \
    X(xrWaitSwapchainImage)                                                                                            \
    X(xrReleaseSwapchainImage)                                                                                         \
    X(xrWaitFrame)                                                                                                     \
    X(xrBeginFrame)                                                                                                    \
    X(xrEndFrame)
#define DECLARE(name) PFN_##name name;
XR_FUNCTIONS(DECLARE)
#undef DECLARE
void checkHr(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("D3D failure " + std::to_string(result));
}
void checkXr(XrResult result, const char* call) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}
#define XR_CHECK(call) checkXr(call, #call)
void checkOvr(ovrResult result) {
    if (OVR_FAILURE(result))
        throw std::runtime_error("OVR failure " + std::to_string(result));
}
decltype(&ovr_EndFrame) originalEnd;
decltype(&ovr_CommitTextureSwapChain) originalCommit;
decltype(&ovr_GetTextureSwapChainBufferDX) getBuffer;
decltype(&ovr_GetTextureSwapChainCurrentIndex) getIndex;
std::mutex captureMutex;
std::condition_variable ready;
std::map<ovrTextureSwapChain, int> written;
std::string captureFailure;
int captured{};
bool depthPixelsDefined{true};
ovrResult OVR_CDECL hookCommit(ovrSession session, ovrTextureSwapChain chain) {
    int index{};
    const auto query = getIndex(session, chain, &index);
    const auto result = originalCommit(session, chain);
    if (OVR_SUCCESS(query) && OVR_SUCCESS(result)) {
        std::lock_guard lock(captureMutex);
        written[chain] = index;
    }
    return result;
}
ComPtr<ID3D11Texture2D> getCommitted(ovrSession session, ovrTextureSwapChain chain) {
    const auto found = written.find(chain);
    if (found == written.end())
        throw std::runtime_error("Submitted texture was not committed");
    ComPtr<ID3D11Texture2D> image;
    checkOvr(getBuffer(session, chain, found->second, IID_PPV_ARGS(&image)));
    return image;
}
float depthAt(const uint8_t* bytes, DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_D16_UNORM) {
        uint16_t value{};
        memcpy(&value, bytes, 2);
        return value / 65535.f;
    }
    if (format == DXGI_FORMAT_R24G8_TYPELESS || format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
        uint32_t value{};
        memcpy(&value, bytes, 4);
        return (value & 0xffffffu) / 16777215.f;
    }
    float value{};
    memcpy(&value, bytes, 4);
    return value;
}
void checkDepth(ovrSession session, const ovrLayerEyeFovDepth& layer, unsigned eye) {
    auto color = getCommitted(session, layer.ColorTexture[eye]);
    auto depth = getCommitted(session, layer.DepthTexture[eye]);
    D3D11_TEXTURE2D_DESC colorDesc{}, desc{};
    color->GetDesc(&colorDesc);
    depth->GetDesc(&desc);
    if (desc.Width != colorDesc.Width || desc.Height != colorDesc.Height)
        throw std::runtime_error("Depth does not map 1:1 to final color resource dimensions");
    const auto& rect = layer.Viewport[eye];
    if (rect.Pos.x < 0 || rect.Pos.y < 0 || rect.Pos.x + rect.Size.w > int(desc.Width) ||
        rect.Pos.y + rect.Size.h > int(desc.Height))
        throw std::runtime_error("Final color viewport is outside depth");
    // OpenXR permits acquire/wait/release without exporting images. Contents are then
    // undefined, but the runtime must still submit valid, committed depth resources.
    if (!depthPixelsDefined)
        return;
    const auto format = desc.Format;
    const UINT stride = format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_D16_UNORM                   ? 2
                        : format == DXGI_FORMAT_R32G8X24_TYPELESS || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8
                                                                                                                : 4;
    ComPtr<ID3D11Device> device;
    depth->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), depth.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const int probes[][2] = {{1, 1}, {7, 1}, {1, 7}, {7, 7}, {3, 3}, {5, 5}};
    bool correct = true;
    for (const auto& probe : probes) {
        const auto x = rect.Pos.x + rect.Size.w * probe[0] / 8, y = rect.Pos.y + rect.Size.h * probe[1] / 8;
        const auto* bytes = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * stride;
        const float actual = depthAt(bytes, format);
        const float expected = (eye ? .55f : .15f) + (probe[0] > 4 ? .1f : 0.f) + (probe[1] > 4 ? .2f : 0.f);
        correct &= std::isfinite(actual) && std::abs(actual - expected) < .0001f;
        std::cout << "eye=" << eye << " depth(" << x << ',' << y << ")=" << actual << " expected=" << expected << '\n';
    }
    context->Unmap(staging.Get(), 0);
    if (!correct)
        throw std::runtime_error("Submitted depth pixels do not follow the final color geometry");
    checkHr(device->GetDeviceRemovedReason());
}
ovrResult OVR_CDECL hookEnd(ovrSession session,
                            long long frame,
                            const ovrViewScaleDesc* scale,
                            const ovrLayerHeader* const* layers,
                            unsigned count) {
    {
        std::lock_guard lock(captureMutex);
        try {
            bool found = false;
            for (unsigned i = 0; i < count; ++i)
                if (layers[i] && layers[i]->Type == ovrLayerType_EyeFovDepth) {
                    const auto& layer = *reinterpret_cast<const ovrLayerEyeFovDepth*>(layers[i]);
                    for (unsigned eye = 0; eye < 2; ++eye)
                        checkDepth(session, layer, eye);
                    found = true;
                    break;
                }
            if (!found)
                throw std::runtime_error("No depth layer submitted; enable compositor depth for this test");
            ++captured;
        } catch (const std::exception& error) {
            captureFailure = error.what();
        }
    }
    const auto result = originalEnd(session, frame, scale, layers, count);
    ready.notify_all();
    return result;
}
struct Images {
    XrSwapchain chain{};
    std::vector<XrSwapchainImageD3D11KHR> images;
};
void enumerateImages(Images& result) {
    uint32_t count{};
    XR_CHECK(xrEnumerateSwapchainImages(result.chain, 0, &count, nullptr));
    result.images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(
        result.chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(result.images.data())));
    if (!count)
        throw std::runtime_error("Image enumeration returned no images");
    for (const auto& image : result.images)
        if (!image.texture)
            throw std::runtime_error("Image enumeration returned a null texture");
}
Images createImages(
    XrSession session, DXGI_FORMAT format, uint32_t width, uint32_t height, bool depth, bool enumerate = true) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags =
        (depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) |
        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.width = width;
    info.height = height;
    info.arraySize = 2;
    info.sampleCount = info.faceCount = info.mipCount = 1;
    Images result;
    XR_CHECK(xrCreateSwapchain(session, &info, &result.chain));
    if (enumerate)
        enumerateImages(result);
    return result;
}
uint32_t acquireImage(Images& images) {
    uint32_t index{};
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(images.chain, &acquire, &index));
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(images.chain, &wait));
    return index;
}
void releaseImage(Images& images) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(images.chain, &release));
}
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        if (argc < 3 || argc > 6)
            throw std::runtime_error("Usage: <loader.dll> <matching|cropped|scaled|cropped-scaled> [--nr] "
                                     "[--d16|--d24|--d32s8] [--no-depth-enumeration|--late-depth-enumeration]");
        const std::string geometry = argv[2];
        bool requireNr = false, noDepthEnumeration = false, lateDepthEnumeration = false;
        DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;
        for (int i = 3; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--nr")
                requireNr = true;
            else if (option == "--d16")
                depthFormat = DXGI_FORMAT_D16_UNORM;
            else if (option == "--d24")
                depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            else if (option == "--d32s8")
                depthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            else if (option == "--no-depth-enumeration")
                noDepthEnumeration = true;
            else if (option == "--late-depth-enumeration")
                noDepthEnumeration = lateDepthEnumeration = true;
            else
                throw std::runtime_error("Unknown option");
        }
        const bool cropped = geometry == "cropped" || geometry == "cropped-scaled";
        const bool scaled = geometry == "scaled" || geometry == "cropped-scaled";
        if (!cropped && !scaled && geometry != "matching")
            throw std::runtime_error("Unknown geometry");
        const int depthSize = scaled ? 256 : 512;
        const XrRect2Di depthRect{{cropped ? 64 : 0, cropped ? 32 : 0}, {depthSize, depthSize}};
        HMODULE loader = LoadLibraryA(argv[1]);
        if (!loader)
            throw std::runtime_error("Cannot load OpenXR loader");
        auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
        if (!getProc)
            throw std::runtime_error("Missing loader dispatch");
        XR_CHECK(getProc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, "VDXR compositor depth regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 2;
        info.enabledExtensionNames = extensions;
        XrInstance instance{};
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
            if (!memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)))
                break;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checkHr(D3D11CreateDevice(adapter.Get(),
                                  D3D_DRIVER_TYPE_UNKNOWN,
                                  nullptr,
                                  0,
                                  &requirements.minFeatureLevel,
                                  1,
                                  D3D11_SDK_VERSION,
                                  &device,
                                  nullptr,
                                  &context));
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.Get();
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
        create.next = &binding;
        create.systemId = system;
        XrSession session{};
        XR_CHECK(xrCreateSession(instance, &create, &session));
        HMODULE ovr = GetModuleHandleA("LibOVRRT64_1.dll");
        if (!ovr)
            throw std::runtime_error("OVRNull was not loaded");
#define OVR_LOAD(variable, name)                                                                                       \
    variable = reinterpret_cast<decltype(variable)>(GetProcAddress(ovr, #name));                                       \
    if (!variable)                                                                                                     \
        throw std::runtime_error("Missing " #name);
        OVR_LOAD(originalEnd, ovr_EndFrame)
        OVR_LOAD(originalCommit, ovr_CommitTextureSwapChain)
        OVR_LOAD(getBuffer, ovr_GetTextureSwapChainBufferDX)
        OVR_LOAD(getIndex, ovr_GetTextureSwapChainCurrentIndex)
#undef OVR_LOAD
        checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
        checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
        checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalCommit), hookCommit)));
        checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalEnd), hookEnd)));
        checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XR_CHECK(xrBeginSession(session, &begin));
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        reference.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace space{};
        XR_CHECK(xrCreateReferenceSpace(session, &reference, &space));
        auto color = createImages(session, DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, false);
        auto depth = createImages(session,
                                  depthFormat,
                                  depthRect.offset.x + depthSize,
                                  depthRect.offset.y + depthSize,
                                  true,
                                  !noDepthEnumeration);
        depthPixelsDefined = !noDepthEnumeration;
        const char* vsSource = "float4 main(uint id:SV_VertexID):SV_Position{float2 "
                               "p=float2(id==1?2:0,id==2?2:0);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
        const char* psSource =
            "cbuffer c{int2 origin;int2 extent;float base;float3 pad;};float main(float4 "
            "p:SV_Position):SV_Depth{return base+(p.x>=origin.x+extent.x/2?0.1:0)+(p.y>=origin.y+extent.y/2?0.2:0);}";
        ComPtr<ID3DBlob> vsCode, psCode, errors;
        checkHr(D3DCompile(
            vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, &errors));
        checkHr(D3DCompile(
            psSource, strlen(psSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psCode, &errors));
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        checkHr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checkHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        D3D11_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = TRUE;
        dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ComPtr<ID3D11DepthStencilState> ds;
        checkHr(device->CreateDepthStencilState(&dsDesc, &ds));
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = 32;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb;
        checkHr(device->CreateBuffer(&cbDesc, nullptr, &cb));
        const std::vector<uint32_t> rgba(512 * 512, 0xff604020);
        for (int frame = 0; frame < 3; ++frame) {
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState state{XR_TYPE_FRAME_STATE};
            XR_CHECK(xrWaitFrame(session, &wait, &state));
            XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
            XR_CHECK(xrBeginFrame(session, &fb));
            const auto ci = acquireImage(color), di = acquireImage(depth);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                context->UpdateSubresource(color.images[ci].texture,
                                           D3D11CalcSubresource(0, eye, 1),
                                           nullptr,
                                           rgba.data(),
                                           512 * 4,
                                           512 * 512 * 4);
                if (depth.images.empty())
                    continue;
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                desc.Format = depthFormat;
                desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.FirstArraySlice = eye;
                desc.Texture2DArray.ArraySize = 1;
                ComPtr<ID3D11DepthStencilView> dsv;
                checkHr(device->CreateDepthStencilView(depth.images[di].texture, &desc, &dsv));
                struct Constants {
                    int origin[2], extent[2];
                    float base, padding[3];
                } constants{{depthRect.offset.x, depthRect.offset.y}, {depthSize, depthSize}, eye ? .55f : .15f, {}};
                context->UpdateSubresource(cb.Get(), 0, nullptr, &constants, 0, 0);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->PSSetShader(ps.Get(), nullptr, 0);
                context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
                context->OMSetDepthStencilState(ds.Get(), 0);
                context->OMSetRenderTargets(0, nullptr, dsv.Get());
                D3D11_VIEWPORT vp{
                    0, 0, float(depthRect.offset.x + depthSize), float(depthRect.offset.y + depthSize), 0, 1};
                context->RSSetViewports(1, &vp);
                context->Draw(3, 0);
            }
            context->OMSetRenderTargets(0, nullptr, nullptr);
            releaseImage(color);
            releaseImage(depth);
            XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            XrCompositionLayerDepthInfoKHR depths[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                        {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
            for (uint32_t eye = 0; eye < 2; ++eye) {
                views[eye].pose.orientation.w = 1;
                views[eye].pose.position = {eye ? .032f : -.032f, 1.6f, 0};
                views[eye].fov = {-.8f, .8f, .8f, -.8f};
                views[eye].subImage = {color.chain, {{0, 0}, {512, 512}}, eye};
                views[eye].next = &depths[eye];
                depths[eye].subImage = {depth.chain, depthRect, eye};
                depths[eye].minDepth = 0;
                depths[eye].maxDepth = 1;
                depths[eye].nearZ = .1f;
                depths[eye].farZ = 100;
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
            XR_CHECK(xrEndFrame(session, &end));
            std::unique_lock lock(captureMutex);
            if (!ready.wait_for(
                    lock, std::chrono::seconds(30), [&] { return captured > frame || !captureFailure.empty(); }))
                throw std::runtime_error("Depth capture timed out");
            if (!captureFailure.empty())
                throw std::runtime_error(captureFailure);
            if (lateDepthEnumeration && frame == 0) {
                enumerateImages(depth);
                depthPixelsDefined = true;
            }
        }
        if (requireNr && !GetModuleHandleA("nvngx_dlssnr.dll"))
            throw std::runtime_error("NR module was not loaded");
        // Detach before instance teardown can unload OVRNull.
        checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
        checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
        checkHr(HRESULT_FROM_WIN32(DetourDetach(reinterpret_cast<PVOID*>(&originalEnd), hookEnd)));
        checkHr(HRESULT_FROM_WIN32(DetourDetach(reinterpret_cast<PVOID*>(&originalCommit), hookCommit)));
        checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        XR_CHECK(xrDestroyInstance(instance));
        FreeLibrary(loader);
        std::cout << "PASS: " << geometry << " committed compositor depth follows both final color viewports\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
