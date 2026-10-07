// Runtime-linked non-NR resize/failure regressions. No headset or settings writes.
// OVR output allocation is intercepted; the production upscaler runs on native D3D11 WARP.
#include "pch.h"
#include "runtime.h"
#include <array>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }
    void checkHr(HRESULT value) {
        require(SUCCEEDED(value), "Native D3D operation failed");
    }

    struct OutputChain {
        ovrTextureSwapChainDesc desc{};
        ComPtr<ID3D11Texture2D> image;
    };
    std::map<ovrTextureSwapChain, std::unique_ptr<OutputChain>> chains;
    unsigned commits;
    enum class Failure { None, Texture, Srv, Uav };
    struct CreationState {
        ID3D11Device* device{};
        Failure failure{};
        unsigned failAt{};
        unsigned textures{}, srvs{}, uavs{}, injected{};
    } creation;

    using CreateTexture = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
                                                      const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*,
                                                      ID3D11Texture2D**);
    using CreateSrv = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
                                                  ID3D11Resource*,
                                                  const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                  ID3D11ShaderResourceView**);
    using CreateUav = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
                                                  ID3D11Resource*,
                                                  const D3D11_UNORDERED_ACCESS_VIEW_DESC*,
                                                  ID3D11UnorderedAccessView**);
    CreateTexture originalTexture;
    CreateSrv originalSrv;
    CreateUav originalUav;
    decltype(&ovr_CreateTextureSwapChainDX) originalCreate;
    decltype(&ovr_GetTextureSwapChainLength) originalLength;
    decltype(&ovr_GetTextureSwapChainBufferDX) originalBuffer;
    decltype(&ovr_GetTextureSwapChainCurrentIndex) originalIndex;
    decltype(&ovr_CommitTextureSwapChain) originalCommit;
    decltype(&ovr_DestroyTextureSwapChain) originalDestroy;
    decltype(&ovr_GetTextureSwapChainDesc) originalDesc;

    bool inject(Failure kind, unsigned ordinal) {
        if (creation.failure == kind && creation.failAt == ordinal) {
            ++creation.injected;
            creation.failure = Failure::None;
            return true;
        }
        return false;
    }
    HRESULT STDMETHODCALLTYPE createTexture(ID3D11Device* device,
                                            const D3D11_TEXTURE2D_DESC* desc,
                                            const D3D11_SUBRESOURCE_DATA* data,
                                            ID3D11Texture2D** output) {
        if (device == creation.device && desc && desc->Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            inject(Failure::Texture, ++creation.textures)) {
            if (output)
                *output = nullptr;
            return E_OUTOFMEMORY;
        }
        return originalTexture(device, desc, data, output);
    }
    HRESULT STDMETHODCALLTYPE createSrv(ID3D11Device* device,
                                        ID3D11Resource* resource,
                                        const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                        ID3D11ShaderResourceView** output) {
        if (device == creation.device && desc && desc->Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            inject(Failure::Srv, ++creation.srvs)) {
            if (output)
                *output = nullptr;
            return E_OUTOFMEMORY;
        }
        return originalSrv(device, resource, desc, output);
    }
    HRESULT STDMETHODCALLTYPE createUav(ID3D11Device* device,
                                        ID3D11Resource* resource,
                                        const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
                                        ID3D11UnorderedAccessView** output) {
        if (device == creation.device && desc && desc->Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            inject(Failure::Uav, ++creation.uavs)) {
            if (output)
                *output = nullptr;
            return E_OUTOFMEMORY;
        }
        return originalUav(device, resource, desc, output);
    }

    ovrResult OVR_CDECL createChain(ovrSession,
                                    IUnknown* unknown,
                                    const ovrTextureSwapChainDesc* desc,
                                    ovrTextureSwapChain* output) {
        if (!unknown || !desc || !output || desc->Width <= 0 || desc->Height <= 0)
            return ovrError_InvalidParameter;
        ComPtr<ID3D11Device> device;
        if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&device))))
            return ovrError_InvalidParameter;
        auto chain = std::make_unique<OutputChain>();
        chain->desc = *desc;
        D3D11_TEXTURE2D_DESC native{};
        native.Width = desc->Width;
        native.Height = desc->Height;
        native.ArraySize = native.MipLevels = native.SampleDesc.Count = 1;
        native.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        native.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&native, nullptr, &chain->image)))
            return ovrError_MemoryAllocationFailure;
        const auto handle = reinterpret_cast<ovrTextureSwapChain>(chain.get());
        chains.emplace(handle, std::move(chain));
        *output = handle;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainLength(ovrSession, ovrTextureSwapChain handle, int* output) {
        if (!chains.count(handle) || !output)
            return ovrError_InvalidParameter;
        *output = 1;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainBuffer(ovrSession, ovrTextureSwapChain handle, int index, IID iid, void** output) {
        const auto found = chains.find(handle);
        if (found == chains.end() || index || !output || FAILED(found->second->image->QueryInterface(iid, output)))
            return ovrError_InvalidParameter;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL chainIndex(ovrSession, ovrTextureSwapChain handle, int* output) {
        if (!chains.count(handle) || !output)
            return ovrError_InvalidParameter;
        *output = 0;
        return ovrSuccess;
    }
    ovrResult OVR_CDECL commitChain(ovrSession, ovrTextureSwapChain handle) {
        if (!chains.count(handle))
            return ovrError_InvalidParameter;
        ++commits;
        return ovrSuccess;
    }
    void OVR_CDECL destroyChain(ovrSession, ovrTextureSwapChain handle) {
        if (chains.erase(handle) != 1)
            std::terminate();
    }
    ovrResult OVR_CDECL chainDesc(ovrSession, ovrTextureSwapChain handle, ovrTextureSwapChainDesc* output) {
        const auto found = chains.find(handle);
        if (found == chains.end() || !output)
            return ovrError_InvalidParameter;
        *output = found->second->desc;
        return ovrSuccess;
    }

    class Hooks {
      public:
        Hooks(const wchar_t* directory, ID3D11Device* device) : m_device(device) {
            require(chains.empty(), "Output chains survived the previous test");
            ovrInitParams init{};
            init.Flags = ovrInit_RequestVersion;
            init.RequestedMinorVersion = OVR_MINOR_VERSION;
            const auto path = std::filesystem::absolute(directory).wstring() + L"\\";
            require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, path.c_str())), "OVRNull initialization failed");
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
#define LOAD(variable, name)                                                                                           \
    variable = reinterpret_cast<decltype(variable)>(GetProcAddress(module, #name));                                    \
    require(variable, "Missing OVRNull export: " #name)
            LOAD(originalCreate, ovr_CreateTextureSwapChainDX);
            LOAD(originalLength, ovr_GetTextureSwapChainLength);
            LOAD(originalBuffer, ovr_GetTextureSwapChainBufferDX);
            LOAD(originalIndex, ovr_GetTextureSwapChainCurrentIndex);
            LOAD(originalCommit, ovr_CommitTextureSwapChain);
            LOAD(originalDestroy, ovr_DestroyTextureSwapChain);
            LOAD(originalDesc, ovr_GetTextureSwapChainDesc);
#undef LOAD
            auto** table = *reinterpret_cast<void***>(device);
            originalTexture = reinterpret_cast<CreateTexture>(table[5]);
            originalSrv = reinterpret_cast<CreateSrv>(table[7]);
            originalUav = reinterpret_cast<CreateUav>(table[8]);
            creation = {};
            creation.device = device;
            commits = 0;
            transact(true);
        }
        ~Hooks() {
            try {
                transact(false);
            } catch (...) {
                std::terminate();
            }
            creation = {};
        }
        Hooks(const Hooks&) = delete;
        Hooks& operator=(const Hooks&) = delete;

      private:
        ComPtr<ID3D11Device> m_device;
        static void transact(bool attach) {
            LONG result = DetourTransactionBegin();
            const bool active = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
#define CHANGE(variable, replacement)                                                                                  \
    if (result == NO_ERROR)                                                                                            \
    result = attach ? DetourAttach(reinterpret_cast<PVOID*>(&variable), replacement)                                   \
                    : DetourDetach(reinterpret_cast<PVOID*>(&variable), replacement)
            CHANGE(originalCreate, createChain);
            CHANGE(originalLength, chainLength);
            CHANGE(originalBuffer, chainBuffer);
            CHANGE(originalIndex, chainIndex);
            CHANGE(originalCommit, commitChain);
            CHANGE(originalDestroy, destroyChain);
            CHANGE(originalDesc, chainDesc);
            CHANGE(originalTexture, createTexture);
            CHANGE(originalSrv, createSrv);
            CHANGE(originalUav, createUav);
#undef CHANGE
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (active)
                DetourTransactionAbort();
            require(result == NO_ERROR, "Hook transaction failed");
        }
    };

    template <typename T>
    ComPtr<IUnknown> identity(T* resource) {
        ComPtr<IUnknown> output;
        checkHr(resource->QueryInterface(IID_PPV_ARGS(&output)));
        return output;
    }
    template <typename T>
    bool refersTo(T* view, ID3D11Texture2D* image) {
        if (!view || !image)
            return false;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        return identity(resource.Get()) == identity(image);
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        struct Fixture {
            OpenXrRuntime runtime;
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            std::unique_ptr<Hooks> hooks;
            std::array<OpenXrRuntime::Swapchain, 2> input;
            std::array<XrSwapchainSubImage, 2> views;
            Fixture(const wchar_t* directory) {
                runtime.stopRegistryWatcher();
                checkHr(D3D11CreateDevice(nullptr,
                                          D3D_DRIVER_TYPE_WARP,
                                          nullptr,
                                          0,
                                          nullptr,
                                          0,
                                          D3D11_SDK_VERSION,
                                          &device,
                                          nullptr,
                                          &context));
                checkHr(device.As(&runtime.m_ovrSubmissionDevice));
                checkHr(context.As(&runtime.m_ovrSubmissionContext));
                checkHr(runtime.m_ovrSubmissionDevice->CreateFence(
                    0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&runtime.m_ovrSubmissionCompletionFence)));
                runtime.initializePrecompositorResources();
                D3D11_SAMPLER_DESC sampler{};
                sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sampler.MaxLOD = D3D11_FLOAT32_MAX;
                checkHr(device->CreateSamplerState(&sampler, &runtime.m_linearClampSampler));
                runtime.m_upscalingMultiplier = .5f;
                runtime.m_precompositor.sharpenFactor = .5f;
                hooks = std::make_unique<Hooks>(directory, device.Get());
                for (unsigned eye = 0; eye < 2; ++eye) {
                    auto& chain = input[eye];
                    chain.ovrSwapchainLength = 1;
                    chain.ovrDesc.Width = chain.ovrDesc.Height = 64;
                    chain.dxgiFormatForSubmission = DXGI_FORMAT_R8G8B8A8_UNORM;
                    chain.resolvedSlices.resize(1);
                    chain.resolvedSlices[0].lastCommittedIndex = 0;
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = desc.Height = 64;
                    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    std::vector<uint8_t> pixels(64 * 64 * 4);
                    for (size_t i = 0; i < pixels.size(); i += 4) {
                        pixels[i] = eye ? 40 : 160;
                        pixels[i + 1] = eye ? 160 : 40;
                        pixels[i + 2] = 80;
                        pixels[i + 3] = 255;
                    }
                    D3D11_SUBRESOURCE_DATA initial{pixels.data(), 64 * 4, 0};
                    ComPtr<ID3D11Texture2D> texture;
                    checkHr(device->CreateTexture2D(&desc, &initial, &texture));
                    chain.resolvedSlices[0].images.push_back(std::move(texture));
                    views[eye] = {reinterpret_cast<XrSwapchain>(&chain), {{0, 0}, {32, 32}}, 0};
                }
            }
            ~Fixture() {
                try {
                    runtime.flushSubmissionContext();
                    for (auto& slice : input[0].stereoProjection)
                        if (slice.ovrSwapchain) {
                            ovr_DestroyTextureSwapChain(nullptr, slice.ovrSwapchain);
                            slice.ovrSwapchain = nullptr;
                        }
                    runtime.cleanupSubmissionDevice();
                } catch (...) {
                    std::terminate();
                }
            }
            void render(int width, int height) {
                for (auto& view : views)
                    view.imageRect.extent = {width, height};
                const XrSwapchainSubImage* pointers[]{&views[0], &views[1]};
                ovrLayerEyeFov layer{};
                runtime.upscaler(pointers, layer);
                runtime.flushSubmissionContext();
                for (unsigned eye = 0; eye < 2; ++eye) {
                    require(layer.ColorTexture[eye] == input[0].stereoProjection[eye].ovrSwapchain,
                            "Layer did not use its new output chain");
                    require(layer.Viewport[eye].Size.w == width * 2 && layer.Viewport[eye].Size.h == height * 2,
                            "Layer viewport retained an old resolution");
                }
            }
            bool complete(unsigned eye, int width, int height) const {
                const auto& entry = input[0].intermediate[eye];
                if (!entry.image)
                    return false;
                D3D11_TEXTURE2D_DESC desc{};
                entry.image->GetDesc(&desc);
                return int(desc.Width) == width && int(desc.Height) == height &&
                       refersTo(entry.srv.Get(), entry.image.Get()) && refersTo(entry.uav.Get(), entry.image.Get());
            }
            void pixels() {
                for (unsigned eye = 0; eye < 2; ++eye) {
                    const auto& output = input[0].stereoProjection[eye].images[0];
                    D3D11_TEXTURE2D_DESC desc{};
                    output->GetDesc(&desc);
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.BindFlags = 0;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Texture2D> staging;
                    checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
                    context->CopyResource(staging.Get(), output.Get());
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                    const auto* pixel = static_cast<const uint8_t*>(mapped.pData) +
                                        (desc.Height / 2) * mapped.RowPitch + (desc.Width / 2) * 4;
                    // Output is BGRA; constant inputs survive EASU+CAS, with normal UNORM rounding.
                    const bool okay = std::abs(int(pixel[0]) - 80) <= 3 &&
                                      std::abs(int(pixel[1]) - (eye ? 160 : 40)) <= 3 &&
                                      std::abs(int(pixel[2]) - (eye ? 40 : 160)) <= 3;
                    context->Unmap(staging.Get(), 0);
                    require(okay, "Recovered output lost the distinct eye pixels");
                }
            }
        };

        static void failure(const wchar_t* directory, Failure failure, unsigned eye) {
            Fixture fixture(directory);
            fixture.render(24, 28);
            auto retained = fixture.input[0].intermediate[eye];
            creation.failure = failure;
            creation.failAt = (failure == Failure::Texture ? creation.textures
                               : failure == Failure::Srv   ? creation.srvs
                                                           : creation.uavs) +
                              eye + 1;
            bool threw = false;
            try {
                fixture.render(40, 36);
            } catch (const std::exception&) {
                threw = true;
            }
            require(threw && creation.injected == 1, "Targeted creation failure was not exercised");
            const auto& failed = fixture.input[0].intermediate[eye];
            const bool preserved =
                failed.image == retained.image && failed.srv == retained.srv && failed.uav == retained.uav;
            std::cout << "failure eye=" << eye << " retained-complete-entry=" << preserved << '\n';
            fixture.render(40, 36);
            const bool recovered = fixture.complete(0, 80, 72) && fixture.complete(1, 80, 72);
            std::cout << "retry complete-view-generation=" << recovered << '\n';
            require(preserved, "Failed resize destroyed the previously complete intermediate resource entry");
            require(recovered, "Same-resolution retry kept a missing or stale intermediate view");
            fixture.pixels();
            require(commits == 4, "A failed preparation submitted partial eye output");
            std::cout << "PASS: failed intermediate creation retains the prior entry and retry recovers both eyes\n";
        }
        static void resize(const wchar_t* directory) {
            Fixture fixture(directory);
            for (const auto size : {XrExtent2Di{40, 36}, {16, 20}, {44, 12}, {12, 44}, {24, 28}}) {
                fixture.render(size.width, size.height);
                require(fixture.complete(0, size.width * 2, size.height * 2) &&
                            fixture.complete(1, size.width * 2, size.height * 2),
                        "Resize did not publish matching images and views");
                fixture.pixels();
            }
            require(creation.textures == 10 && creation.srvs == 10 && creation.uavs == 10,
                    "Mixed-axis resize did not replace each eye once");
            std::cout
                << "PASS: high/low and mixed-axis sizes replace complete native entries and preserve eye pixels\n";
        }
        static void reuse(const wchar_t* directory) {
            Fixture fixture(directory);
            fixture.render(24, 28);
            const auto left = fixture.input[0].intermediate[0];
            const auto right = fixture.input[0].intermediate[1];
            fixture.render(24, 28);
            require(fixture.input[0].intermediate[0].image == left.image &&
                        fixture.input[0].intermediate[0].srv == left.srv &&
                        fixture.input[0].intermediate[0].uav == left.uav &&
                        fixture.input[0].intermediate[1].image == right.image &&
                        fixture.input[0].intermediate[1].srv == right.srv &&
                        fixture.input[0].intermediate[1].uav == right.uav,
                    "Same-size render replaced a complete intermediate entry");
            require(creation.textures == 2 && creation.srvs == 2 && creation.uavs == 2,
                    "Same-size render created extra intermediate resources");
            fixture.pixels();
            std::cout << "PASS: same-size rendering reuses complete native entries\n";
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 3, "usage: nonnr_resolution_regression MODE OVRNull-directory");
        using Regression = virtualdesktop_openxr::RuntimeInputRegression;
        const std::wstring mode = argv[1];
        if (mode == L"resize")
            Regression::resize(argv[2]);
        else if (mode == L"reuse")
            Regression::reuse(argv[2]);
        else {
            const unsigned eye = mode.find(L"-right") != std::wstring::npos ? 1 : 0;
            if (mode == L"texture" || mode == L"texture-right")
                Regression::failure(argv[2], Failure::Texture, eye);
            else if (mode == L"srv" || mode == L"srv-right")
                Regression::failure(argv[2], Failure::Srv, eye);
            else if (mode == L"uav" || mode == L"uav-right")
                Regression::failure(argv[2], Failure::Uav, eye);
            else
                throw std::runtime_error("Unknown mode");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
