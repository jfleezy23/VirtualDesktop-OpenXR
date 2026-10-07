// Exercise the production OVRNull driver through IDriver, including real mirror pixels.
// No private swapchain state or copied producer/consumer index algorithm is used.
#include "../OVRNull/driver.h"
#include <array>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>

using namespace ovrnull::driver;
void checkHr(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("D3D failed: " + std::to_string(result));
}
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void clearImage(IDriver& driver,
                void* chain,
                int index,
                ID3D11Device* device,
                ID3D11DeviceContext* context,
                const std::array<uint8_t, 3>& rgb) {
    auto* image = driver.GetSwapchainImage(chain, index);
    require(image != nullptr, "Public image getter returned null");
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    checkHr(device->CreateRenderTargetView(image, &desc, &rtv));
    const float color[4] = {rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f, 1.f};
    context->ClearRenderTargetView(rtv.Get(), color);
}

void checkMirror(IDriver& driver,
                 void* mirror,
                 ID3D11Device* device,
                 ID3D11DeviceContext* context,
                 const std::array<std::array<uint8_t, 3>, 2>& expected) {
    auto* image = driver.GetSwapchainImage(mirror, 0);
    require(image != nullptr, "Mirror texture is missing");
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), image);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    bool correct = true;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto x = eye ? 96u : 32u;
        const auto* actual = static_cast<const uint8_t*>(mapped.pData) + 32 * mapped.RowPitch + x * 4;
        std::cout << "eye=" << eye << " expected=";
        for (const auto value : expected[eye])
            std::cout << unsigned(value) << ',';
        std::cout << " actual=";
        for (uint32_t channel = 0; channel < 3; ++channel) {
            std::cout << unsigned(actual[channel]) << ',';
            correct &= std::abs(int(actual[channel]) - int(expected[eye][channel])) <= 1;
        }
        std::cout << '\n';
    }
    context->Unmap(staging.Get(), 0);
    require(correct, "Consumer mirror did not read the image written immediately before commit");
    checkHr(device->GetDeviceRemovedReason());
}

void exercise(bool staticImage, IDriver& driver, ID3D11Device* device, ID3D11DeviceContext* context) {
    ovrTextureSwapChainDesc desc{};
    desc.Type = ovrTexture_2D;
    desc.Format = OVR_FORMAT_R8G8B8A8_UNORM;
    desc.Width = desc.Height = 64;
    desc.ArraySize = desc.MipLevels = desc.SampleCount = 1;
    desc.BindFlags = ovrTextureBind_DX_RenderTarget;
    desc.StaticImage = staticImage;
    void* chains[2] = {driver.CreateSwapchain(desc), driver.CreateSwapchain(desc)};
    require(chains[0] && chains[1], "Color swapchain creation failed");
    const int length = staticImage ? 1 : 3; // Public OVRNull backend ring length.
    for (const auto chain : chains) {
        require(bool(driver.GetSwapchainDesc(chain).StaticImage) == staticImage, "Swapchain static descriptor changed");
        require(driver.GetSwapchainImageIndex(chain) == 0,
                "Initial free image does not match VDXR's first released image zero");
        for (int index = 0; index < length; ++index)
            clearImage(driver, chain, index, device, context, {0, 0, 0});
    }
    desc.Width = 128;
    desc.StaticImage = true;
    auto* mirror = driver.CreateSwapchain(desc);
    require(mirror != nullptr, "Mirror swapchain creation failed");
    driver.SetMirrorTexture(mirror);
    std::array<std::vector<int>, 2> observed;
    std::array<std::set<int>, 2> used;
    for (int frame = 0; frame < 9; ++frame) {
        std::cout << (staticImage ? "static" : "dynamic") << " frame=" << frame << '\n';
        const uint8_t changing = static_cast<uint8_t>(20 + frame * 17);
        const std::array<std::array<uint8_t, 3>, 2> colors = {{{changing, 40, 180}, {180, 40, changing}}};
        ovrLayerEyeFov layer{};
        layer.Header.Type = ovrLayerType_EyeFov;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const int index = driver.GetSwapchainImageIndex(chains[eye]);
            require(index >= 0 && index < length, "Producer index is outside the public ring");
            require(driver.GetSwapchainImageIndex(chains[eye]) == index &&
                        driver.GetSwapchainImageIndex(chains[eye]) == index,
                    "Repeated producer index reads advanced the swapchain");
            observed[eye].push_back(index);
            used[eye].insert(index);
            if (frame >= length)
                require(index == observed[eye][frame - length], "Producer ring did not wrap");
            clearImage(driver, chains[eye], index, device, context, colors[eye]);
            require(driver.CommitSwapchainImage(chains[eye]), "Commit failed");
            const int next = driver.GetSwapchainImageIndex(chains[eye]);
            require(driver.GetSwapchainImageIndex(chains[eye]) == next, "Repeated post-commit index read advanced");
            require(staticImage ? next == index : next != index,
                    "Commit did not preserve static/advance dynamic index");
            layer.ColorTexture[eye] = reinterpret_cast<ovrTextureSwapChain>(chains[eye]);
            layer.Viewport[eye] = {{0, 0}, {64, 64}};
            layer.Fov[eye] = driver.GetEyeFov(static_cast<ovrEyeType>(eye));
            layer.RenderPose[eye].Orientation.w = 1.f;
        }
        require(driver.SubmitFrame({&layer.Header}), "Mirror compositor rejected a valid layer");
        checkMirror(driver, mirror, device, context, colors);
    }
    for (uint32_t eye = 0; eye < 2; ++eye)
        require(used[eye].size() == static_cast<size_t>(length), "Producer did not use every ring image");
    driver.SetMirrorTexture(nullptr);
    driver.DestroySwapchain(mirror);
    driver.DestroySwapchain(chains[0]);
    driver.DestroySwapchain(chains[1]);
}

int main() {
    std::cout << std::unitbuf;
    try {
        std::unique_ptr<IDriver> driver(CreateDriver());
        require(driver != nullptr, "Production CreateDriver returned null");
        const auto requiredLuid = driver->GetAdapterLuid();
        ComPtr<IDXGIFactory1> factory;
        checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;; ++index) {
            checkHr(factory->EnumAdapters1(index, &adapter));
            DXGI_ADAPTER_DESC1 desc{};
            checkHr(adapter->GetDesc1(&desc));
            if (!memcmp(&desc.AdapterLuid, &requiredLuid, sizeof(LUID)))
                break;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checkHr(D3D11CreateDevice(adapter.Get(),
                                  D3D_DRIVER_TYPE_UNKNOWN,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  D3D11_SDK_VERSION,
                                  &device,
                                  nullptr,
                                  &context));
        driver->SetSubmissionDevice(device.Get());
        exercise(false, *driver, device.Get(), context.Get());
        exercise(true, *driver, device.Get(), context.Get());
        driver.reset();
        std::cout << "PASS: producer/consumer pixels, stable queries, ring wrap, static image\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
