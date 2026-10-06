// Black-box NR/postprocessing composition test. The caller configures registry settings;
// this executable only reads the submitted pixels and never changes runtime configuration.
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
#include <OVR_CAPI_D3D.h>
#include <detours.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
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
void checkHr(HRESULT result) {
    if (FAILED(result)) throw std::runtime_error("D3D failed: " + std::to_string(result));
}
void checkXr(XrResult result, const char* call) {
    if (XR_FAILED(result)) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}
#define XR_CHECK(call) checkXr(call, #call)
void checkOvr(ovrResult result) {
    if (OVR_FAILURE(result)) throw std::runtime_error("OVR readback failed: " + std::to_string(result));
}

struct Pixels { uint32_t width{}, height{}; std::vector<uint8_t> rgba; };
decltype(&ovr_EndFrame) originalEndFrame;
decltype(&ovr_CommitTextureSwapChain) originalCommit;
decltype(&ovr_GetTextureSwapChainBufferDX) getBuffer;
decltype(&ovr_GetTextureSwapChainCurrentIndex) getIndex;
#ifdef NR_GAMMA_MAPPING_TEST
decltype(&ovr_GetTextureSwapChainDesc) getDescription;
void verifyGamma(ovrSession session, ovrTextureSwapChain chain, const Pixels& pixels, int eye) {
    ovrTextureSwapChainDesc desc{};
    checkOvr(getDescription(session,chain,&desc));
    const bool srgb = desc.Format == OVR_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == OVR_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto linear = [](float value) { return value <= .04045f ? value/12.92f : std::pow((value+.055f)/1.055f,2.4f); };
    const uint8_t expected[3] = {static_cast<uint8_t>(eye ? 35 : 180),60,static_cast<uint8_t>(eye ? 180 : 35)};
    const auto offset = (static_cast<size_t>(2)*pixels.width+2)*4;
    bool correct = srgb;
    for (int channel = 0; channel < 3; ++channel) {
        const float encoded = pixels.rgba[offset+channel]/255.f;
        const float actual = srgb ? linear(encoded) : encoded;
        const float reference = linear(expected[channel]/255.f);
        correct &= std::abs(actual-reference) < .02f;
        std::cout << "gamma eye=" << eye << " channel=" << channel << " outputSRGB=" << srgb
                  << " linear=" << actual << " expectedLinear=" << reference << '\n';
    }
    if (!correct) throw std::runtime_error("Mapped source and actual destination encoding disagree");
}
#endif
std::mutex captureMutex;
std::condition_variable captureReady;
std::map<ovrTextureSwapChain, int> writtenIndices;
std::array<Pixels, 2> lastPixels;
std::string captureFailure;
int capturedFrames{};
bool twoLayers{};
int expectedOriginalFrame{};

// OVRNull increments its stored index on commit and has unusual current-index semantics.
// Record the index the runtime actually wrote instead of guessing it after commit.
ovrResult OVR_CDECL hookCommit(ovrSession session, ovrTextureSwapChain chain) {
    int index = -1;
    const auto query = getIndex(session, chain, &index);
    const auto result = originalCommit(session, chain);
    if (OVR_SUCCESS(query) && OVR_SUCCESS(result)) {
        std::lock_guard lock(captureMutex);
        writtenIndices[chain] = index;
    }
    return result;
}

Pixels readPixels(ovrSession session, ovrTextureSwapChain chain, const ovrRecti& viewport) {
    const auto found = writtenIndices.find(chain);
    if (found == writtenIndices.end()) throw std::runtime_error("Submitted chain has no recorded commit");
    ComPtr<ID3D11Texture2D> texture;
    checkOvr(getBuffer(session, chain, found->second, IID_PPV_ARGS(&texture)));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (viewport.Pos.x < 0 || viewport.Pos.y < 0 || viewport.Size.w <= 0 || viewport.Size.h <= 0 ||
        viewport.Pos.x + viewport.Size.w > static_cast<int>(desc.Width) ||
        viewport.Pos.y + viewport.Size.h > static_cast<int>(desc.Height))
        throw std::runtime_error("Submitted viewport is outside the texture");
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!bgra && desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        throw std::runtime_error("Readback requires an RGBA8/BGRA8 output");
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
    // This is queued on the submission device after its NR completion wait.
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    Pixels pixels{static_cast<uint32_t>(viewport.Size.w), static_cast<uint32_t>(viewport.Size.h), {}};
    pixels.rgba.resize(static_cast<size_t>(pixels.width) * pixels.height * 4);
    for (uint32_t y = 0; y < pixels.height; ++y) {
        const auto* source = static_cast<const uint8_t*>(mapped.pData) +
            (viewport.Pos.y + y) * mapped.RowPitch + viewport.Pos.x * 4;
        auto* dest = pixels.rgba.data() + static_cast<size_t>(y) * pixels.width * 4;
        for (uint32_t x = 0; x < pixels.width; ++x) {
            dest[4*x] = source[4*x + (bgra ? 2 : 0)];
            dest[4*x+1] = source[4*x+1];
            dest[4*x+2] = source[4*x + (bgra ? 0 : 2)];
            dest[4*x+3] = source[4*x+3];
        }
    }
    context->Unmap(staging.Get(), 0);
    checkHr(device->GetDeviceRemovedReason());
    return pixels;
}

ovrResult OVR_CDECL hookEndFrame(ovrSession session, long long frame,
    const ovrViewScaleDesc* scale, const ovrLayerHeader* const* layers, unsigned count) {
    {
        std::lock_guard lock(captureMutex);
        try {
            unsigned projections = 0;
            for (unsigned i = 0; i < count; ++i) {
                if (!layers[i] || (layers[i]->Type != ovrLayerType_EyeFov &&
                    layers[i]->Type != ovrLayerType_EyeFovDepth)) continue;
                const auto& layer = *reinterpret_cast<const ovrLayerEyeFov*>(layers[i]);
                for (int eye = 0; eye < 2; ++eye) {
                    const auto chain = layer.ColorTexture[eye] ? layer.ColorTexture[eye] : layer.ColorTexture[0];
                    auto pixels = readPixels(session, chain, layer.Viewport[eye]);
                    if (projections == 0) lastPixels[eye] = pixels;
                    if (twoLayers && projections == 1) {
                        const int expected[3] = { (eye ? 35 : 180) + expectedOriginalFrame,
                            60 + expectedOriginalFrame, (eye ? 180 : 35) + expectedOriginalFrame };
                        const auto pixel = (static_cast<size_t>(2)*pixels.width+2)*4;
                        for (int channel = 0; channel < 3; ++channel)
                            if (pixels.rgba[pixel+channel] != expected[channel])
                                throw std::runtime_error("Second projection did not submit the current original image");
                    }
#ifdef NR_GAMMA_MAPPING_TEST
                    verifyGamma(session,chain,lastPixels[eye],eye);
#endif
                }
                ++projections;
            }
            if (projections != (twoLayers ? 2u : 1u)) throw std::runtime_error("Unexpected submitted projection count");
            ++capturedFrames;
        } catch (const std::exception& error) { captureFailure = error.what(); }
    }
    const auto result = originalEndFrame(session, frame, scale, layers, count);
    captureReady.notify_all();
    return result;
}

void attachHooks() {
    HMODULE module = GetModuleHandleA("LibOVRRT64_1.dll");
    if (!module) throw std::runtime_error("OVRNull LibOVRRT64_1.dll was not loaded");
#define OVR_LOAD(variable, name) \
    variable = reinterpret_cast<decltype(variable)>(GetProcAddress(module, #name)); \
    if (!variable) throw std::runtime_error("Missing " #name);
    OVR_LOAD(originalEndFrame, ovr_EndFrame)
    OVR_LOAD(originalCommit, ovr_CommitTextureSwapChain)
    OVR_LOAD(getBuffer, ovr_GetTextureSwapChainBufferDX)
    OVR_LOAD(getIndex, ovr_GetTextureSwapChainCurrentIndex)
#ifdef NR_GAMMA_MAPPING_TEST
    OVR_LOAD(getDescription, ovr_GetTextureSwapChainDesc)
#endif
#undef OVR_LOAD
    checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
    checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
    checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalCommit), hookCommit)));
    checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalEndFrame), hookEndFrame)));
    checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
}

uint64_t fingerprint(const Pixels& image) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto value : image.rgba) { hash ^= value; hash *= 1099511628211ull; }
    return hash;
}
void checkEye(const Pixels& image, int eye) {
    if (image.width < 32 || image.height < 32) throw std::runtime_error("Empty/undersized capture");
    // Test whole image, four corners, and both seam-adjacent edges. Opposite eye colors
    // catch eye swaps, wrong double-wide offsets, and cross-eye filter contamination.
    const uint32_t edge = std::min(image.width, image.height) / 32;
    const uint32_t boxes[][4] = {{0,0,image.width,image.height}, {0,0,edge,edge},
        {image.width-edge,0,edge,edge}, {0,image.height-edge,edge,edge},
        {image.width-edge,image.height-edge,edge,edge},
        {0,image.height/2,edge,edge}, {image.width-edge,image.height/2,edge,edge}};
    for (const auto& box : boxes) {
        int64_t difference = 0;
        for (uint32_t y = box[1]; y < box[1]+box[3]; ++y)
            for (uint32_t x = box[0]; x < box[0]+box[2]; ++x) {
                const auto offset = (static_cast<size_t>(y)*image.width+x)*4;
                difference += int(image.rgba[offset + (eye ? 2 : 0)]) -
                              int(image.rgba[offset + (eye ? 0 : 2)]);
            }
        if (difference <= static_cast<int64_t>(box[2])*box[3]*8)
            throw std::runtime_error("Eye/corner/seam color invariant failed for eye " + std::to_string(eye));
    }
}
void save(const std::string& path, const std::array<Pixels,2>& eyes) {
    std::ofstream output(path, std::ios::binary);
    output.write("NRP1", 4);
    for (int eye = 0; eye < 2; ++eye) {
        checkEye(eyes[eye], eye);
        output.write(reinterpret_cast<const char*>(&eyes[eye].width), 4);
        output.write(reinterpret_cast<const char*>(&eyes[eye].height), 4);
        output.write(reinterpret_cast<const char*>(eyes[eye].rgba.data()), eyes[eye].rgba.size());
        std::cout << "eye=" << eye << " size=" << eyes[eye].width << 'x' << eyes[eye].height
                  << " fingerprint=" << std::hex << fingerprint(eyes[eye]) << std::dec << '\n';
    }
    if (!output) throw std::runtime_error("Cannot write pixel artifact: " + path);
}
std::array<Pixels,2> load(const char* path) {
    std::ifstream input(path, std::ios::binary);
    char magic[4]{};
    input.read(magic, 4);
    if (memcmp(magic,"NRP1",4)) throw std::runtime_error("Invalid pixel artifact");
    std::array<Pixels,2> eyes;
    for (int eye = 0; eye < 2; ++eye) {
        input.read(reinterpret_cast<char*>(&eyes[eye].width), 4);
        input.read(reinterpret_cast<char*>(&eyes[eye].height), 4);
        if (!eyes[eye].width || !eyes[eye].height || eyes[eye].width > 8192 || eyes[eye].height > 8192)
            throw std::runtime_error("Invalid artifact dimensions");
        eyes[eye].rgba.resize(static_cast<size_t>(eyes[eye].width)*eyes[eye].height*4);
        input.read(reinterpret_cast<char*>(eyes[eye].rgba.data()), eyes[eye].rgba.size());
        if (!input) throw std::runtime_error("Truncated pixel artifact");
        checkEye(eyes[eye], eye);
    }
    return eyes;
}
void compare(const char* nrPath, const char* sharpenPath, const char* combinedPath) {
    const auto nr = load(nrPath), sharpen = load(sharpenPath), combined = load(combinedPath);
    for (int eye = 0; eye < 2; ++eye) {
        if (sharpen[eye].width != combined[eye].width || sharpen[eye].height != combined[eye].height)
            throw std::runtime_error("Comparator requires matching sharpen/combined dimensions");
        if (nr[eye].width == sharpen[eye].width && nr[eye].height == sharpen[eye].height &&
            nr[eye].rgba == sharpen[eye].rgba)
            throw std::runtime_error("NR-only equals sharpen-only: NR effect was not observable");
        size_t changed = 0;
        uint64_t distance = 0;
        for (size_t i = 0; i < sharpen[eye].rgba.size(); i += 4) {
            bool different = false;
            for (size_t channel = 0; channel < 3; ++channel) {
                const int delta = int(sharpen[eye].rgba[i+channel])-int(combined[eye].rgba[i+channel]);
                distance += std::abs(delta);
                different |= delta != 0;
            }
            changed += different;
        }
        std::cout << "eye=" << eye << " combined-vs-sharpen changedPixels=" << changed
                  << " rgbDistance=" << distance << '\n';
        if (!changed) throw std::runtime_error("Combined output exactly equals sharpen-only: NR was discarded");
    }
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        if (argc == 5 && std::string(argv[1]) == "compare") {
            compare(argv[2], argv[3], argv[4]);
            std::cout << "PASS: NR survives postprocessing, eyes/corners/seams are distinct\n";
            return 0;
        }
        if (argc < 4 || argc > 8) throw std::runtime_error("Usage: <loader.dll> <nr|sharpen|upscale|combined|combined-upscale|combined-upscale-sharpen> <pixels.bin> [--unequal] [--srgb] [--two-layers] [--direct-color]; or compare <nr.bin> <postprocess.bin> <combined.bin>");
        const std::string mode = argv[2];
        if (mode != "nr" && mode != "sharpen" && mode != "upscale" && mode != "upscale-sharpen" && mode != "combined" &&
            mode != "combined-upscale" && mode != "combined-upscale-sharpen") throw std::runtime_error("Unknown mode");
        bool unequal = false, srgb = false, directColor = false;
#ifdef NR_GAMMA_MAPPING_TEST
        if (mode != "sharpen" && mode != "upscale") throw std::runtime_error("Gamma reference requires NR-disabled postprocessing");
        srgb = true;
        int64_t declaredFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
#endif
        for (int i = 4; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--unequal") unequal = true;
            else if (option == "--srgb") srgb = true;
            else if (option == "--two-layers") twoLayers = true;
            else if (option == "--direct-color") directColor = true;
#ifdef NR_GAMMA_MAPPING_TEST
            else if (option == "--raw-vk-srgb") declaredFormat = 43; // VK_FORMAT_R8G8B8A8_SRGB.
            else if (option == "--raw-gl-srgb") declaredFormat = 0x8c43; // GL_SRGB8_ALPHA8.
#endif
            else throw std::runtime_error("Unknown option: " + option);
        }
        HMODULE loader = nullptr;
#ifdef NR_GAMMA_MAPPING_TEST
        auto getProc = virtualdesktop_openxr::RuntimeInputRegression::getDispatch(argv[1]);
        std::cout << "Metadata injection test: declared format=" << declaredFormat
                  << "; source resource is D3D11 SRGB, not a Vulkan/OpenGL session\n";
#else
        loader = LoadLibraryA(argv[1]);
        if (!loader) throw std::runtime_error("Cannot load OpenXR loader");
        auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader,"xrGetInstanceProcAddr"));
#endif
        if (!getProc) throw std::runtime_error("Missing xrGetInstanceProcAddr");
        XR_CHECK(getProc(XR_NULL_HANDLE,"xrCreateInstance",reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)));
        const char* extension = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName,"VDXR NR pipeline pixel regression");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,0);
        info.enabledExtensionCount = 1; info.enabledExtensionNames = &extension;
        XrInstance instance{};
        XR_CHECK(xrCreateInstance(&info,&instance));
#define LOAD(name) XR_CHECK(getProc(instance,#name,reinterpret_cast<PFN_xrVoidFunction*>(&name)));
        XR_FUNCTIONS(LOAD)
#undef LOAD
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system{};
        XR_CHECK(xrGetSystem(instance,&systemInfo,&system));
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(instance,system,&requirements));
        ComPtr<IDXGIFactory1> factory; checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            checkHr(factory->EnumAdapters1(i,&adapter));
            DXGI_ADAPTER_DESC1 desc{}; checkHr(adapter->GetDesc1(&desc));
            if (!memcmp(&desc.AdapterLuid,&requirements.adapterLuid,sizeof(LUID))) break;
        }
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        checkHr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,
            &requirements.minFeatureLevel,1,D3D11_SDK_VERSION,&device,nullptr,&context));
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device = device.Get();
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO}; create.next = &binding; create.systemId = system;
        XrSession session{}; XR_CHECK(xrCreateSession(instance,&create,&session));
        attachHooks();
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XR_CHECK(xrBeginSession(session,&begin));
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; reference.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace space{}; XR_CHECK(xrCreateReferenceSpace(session,&reference,&space));
        constexpr uint32_t width = 512, height = 512;
        XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        chainInfo.format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        chainInfo.width = directColor ? width*2 : width; chainInfo.height = height; chainInfo.arraySize = directColor ? 1 : 2;
        chainInfo.sampleCount = chainInfo.faceCount = chainInfo.mipCount = 1;
        XrSwapchain chain{}; XR_CHECK(xrCreateSwapchain(session,&chainInfo,&chain));
#ifdef NR_GAMMA_MAPPING_TEST
        virtualdesktop_openxr::RuntimeInputRegression::setDeclaredFormat(chain,declaredFormat);
#endif
        uint32_t count{}; XR_CHECK(xrEnumerateSwapchainImages(chain,0,&count,nullptr));
        std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
        std::array<std::vector<uint8_t>,2> pattern;
        for (int eye = 0; eye < 2; ++eye) {
            pattern[eye].resize(width*height*4);
            for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                auto* pixel = pattern[eye].data() + (y*width+x)*4;
                const bool border = x < 20 || y < 20 || x >= width-20 || y >= height-20;
                const int detail = border ? 0 : int((x*1103515245u+y*12345u) >> 24)%48;
                pixel[0] = static_cast<uint8_t>((eye ? 35 : 180) + detail);
                pixel[1] = static_cast<uint8_t>(60 + (border ? 0 : (x/16+y/16)%2*35) + detail/2);
                pixel[2] = static_cast<uint8_t>((eye ? 180 : 35) + detail);
                pixel[3] = 255;
            }
        }
        for (int frame = 0; frame < 12; ++frame) {
            if (twoLayers && frame) for (auto& eye : pattern)
                for (size_t pixel = 0; pixel < eye.size(); pixel += 4)
                    for (int channel = 0; channel < 3; ++channel) ++eye[pixel+channel];
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState state{XR_TYPE_FRAME_STATE};
            XR_CHECK(xrWaitFrame(session,&wait,&state));
            XrFrameBeginInfo frameBegin{XR_TYPE_FRAME_BEGIN_INFO}; XR_CHECK(xrBeginFrame(session,&frameBegin));
            uint32_t index{}; XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XR_CHECK(xrAcquireSwapchainImage(chain,&acquire,&index));
            XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; imageWait.timeout = XR_INFINITE_DURATION;
            XR_CHECK(xrWaitSwapchainImage(chain,&imageWait));
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const D3D11_BOX eyeBox{eye*width,0,0,(eye+1)*width,height,1};
                context->UpdateSubresource(images[index].texture,D3D11CalcSubresource(0,directColor ? 0 : eye,1),
                    directColor ? &eyeBox : nullptr,pattern[eye].data(),width*4,width*height*4);
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; XR_CHECK(xrReleaseSwapchainImage(chain,&release));
            XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            for (uint32_t eye = 0; eye < 2; ++eye) {
                views[eye].pose.orientation.w = 1.f; views[eye].pose.position = {eye ? 0.032f : -0.032f,1.6f,0.f};
                views[eye].fov = {-0.8f,0.8f,0.8f,-0.8f};
                const int offsetX = directColor ? int(eye*width) : 0;
                views[eye].subImage = {chain,{{offsetX,0},{width,height}},directColor ? 0 : eye};
                if (unequal && eye) views[eye].subImage.imageRect = {{offsetX+16,8},{width-32,height-16}};
            }
            XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; layer.space = space; layer.viewCount = 2; layer.views = views;
            XrCompositionLayerProjection secondLayer = layer;
            const XrCompositionLayerBaseHeader* headers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer),
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&secondLayer) };
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime = state.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; end.layerCount = twoLayers ? 2 : 1; end.layers = headers;
            if (twoLayers) { std::lock_guard lock(captureMutex); writtenIndices.clear(); expectedOriginalFrame = frame; }
            XR_CHECK(xrEndFrame(session,&end));
            std::unique_lock lock(captureMutex);
            if (!captureReady.wait_for(lock,std::chrono::seconds(30),[&]{return capturedFrames > frame || !captureFailure.empty();}))
                throw std::runtime_error("Submitted frame did not reach the readback hook");
            if (!captureFailure.empty()) throw std::runtime_error(captureFailure);
        }
        if (mode != "sharpen" && mode != "upscale" && mode != "upscale-sharpen" && !GetModuleHandleA("nvngx_dlssnr.dll"))
            throw std::runtime_error("NR module was not loaded");
        { std::lock_guard lock(captureMutex); save(argv[3],lastPixels); }
        XR_CHECK(xrDestroySwapchain(chain)); XR_CHECK(xrDestroySpace(space)); XR_CHECK(xrRequestExitSession(session));
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance,&event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
        XR_CHECK(xrEndSession(session)); XR_CHECK(xrDestroySession(session));
        checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
        checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
        checkHr(HRESULT_FROM_WIN32(DetourDetach(reinterpret_cast<PVOID*>(&originalEndFrame),hookEndFrame)));
        checkHr(HRESULT_FROM_WIN32(DetourDetach(reinterpret_cast<PVOID*>(&originalCommit),hookCommit)));
        checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        XR_CHECK(xrDestroyInstance(instance)); if (loader) FreeLibrary(loader);
        std::cout << "PASS: captured " << mode << " submitted pixels; composition requires comparator\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
