// Bounded allocation-only SDK capability check. No frame is submitted here.
#include "fg_gpu_helpers.h"
#include <OVR_CAPI_D3D.h>
#include <d3d11_4.h>
#include <iostream>

extern "C" ovrResult OVR_CDECL ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
    void statusSample(const ovrSessionStatus& status, unsigned sample) {
        std::cout << "SDK_STATUS {\"sample\":" << sample
                  << ",\"hmd_present\":" << (status.HmdPresent ? "true" : "false")
                  << ",\"hmd_mounted\":" << (status.HmdMounted ? "true" : "false")
                  << ",\"visible\":" << (status.IsVisible ? "true" : "false")
                  << ",\"display_lost\":" << (status.DisplayLost ? "true" : "false")
                  << ",\"should_quit\":" << (status.ShouldQuit ? "true" : "false")
                  << ",\"should_recenter\":" << (status.ShouldRecenter ? "true" : "false")
                  << ",\"input_focus\":" << (status.HasInputFocus ? "true" : "false")
                  << ",\"overlay_present\":" << (status.OverlayPresent ? "true" : "false")
                  << ",\"depth_requested\":" << (status.DepthRequested ? "true" : "false") << "}\n";
    }
    void sdk(ovrResult result, const char* operation) {
        if (OVR_FAILURE(result)) {
            // Capture before teardown can replace the calling thread's error.
            ovrErrorInfo error{};
            ovr_GetLastErrorInfo(&error);
            char text[768];
            sprintf_s(text, "%s (ovrResult %d; SDK %d: %.512s)", operation, result, error.Result, error.ErrorString);
            throw std::runtime_error(text);
        }
    }
    struct Session {
        bool initialized{};
        ovrSession value{};
        ovrTextureSwapChain chains[2]{};
        ~Session() {
            if (value) {
                for (auto chain : chains)
                    if (chain)
                        ovr_DestroyTextureSwapChain(value, chain);
                ovr_Destroy(value);
            }
            if (initialized)
                ovr_Shutdown();
        }
    };
    struct Device {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Device() = default;
        explicit Device(IDXGIAdapter* adapter) {
            const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
            success(D3D11CreateDevice(adapter,
                                      D3D_DRIVER_TYPE_UNKNOWN,
                                      nullptr,
                                      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                      &level,
                                      1,
                                      D3D11_SDK_VERSION,
                                      &device,
                                      nullptr,
                                      &context),
                    "probe D3D11 device");
        }
    };
    bool owns(ID3D11Device* device, ID3D11Texture2D* texture) {
        ComPtr<ID3D11Device> actual;
        ComPtr<IUnknown> expectedIdentity, actualIdentity;
        texture->GetDevice(&actual);
        success(device->QueryInterface(IID_PPV_ARGS(&expectedIdentity)), "requested device identity");
        success(actual.As(&actualIdentity), "actual SDK image device identity");
        return expectedIdentity == actualIdentity;
    }
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        check(argc == 4, "usage: absolute-sdk-prefix native|control producer-first|delivery-first|single-domain");
        const bool control = std::wstring(argv[2]) == L"control";
        check(control || std::wstring(argv[2]) == L"native", "explicit SDK backend kind");
        const bool deliveryFirst = std::wstring(argv[3]) == L"delivery-first";
        const bool single = std::wstring(argv[3]) == L"single-domain";
        check(deliveryFirst || single || std::wstring(argv[3]) == L"producer-first", "explicit creation order");
        // The shim appends LibOVRRT64_1.dll. VD's prefix ends in VirtualDesktop.
        const std::filesystem::path prefix(argv[1]);
        check(prefix.is_absolute(), "absolute override prefix");
        const auto expected = std::filesystem::path(prefix.wstring() + L"LibOVRRT64_1.dll");
        check(std::filesystem::is_regular_file(expected), "expected override library exists");
        // Keep both devices alive until the session destroys its chains.
        Device producer, delivery;
        Session session;
        ovrInitParams params{};
        params.Flags = ovrInit_RequestVersion | ovrInit_FocusAware;
        params.RequestedMinorVersion = OVR_MINOR_VERSION;
        sdk(ovr_InitializeWithPathOverride(&params, prefix.c_str()), "initialize exact override SDK");
        session.initialized = true;
        const auto loaded = GetModuleHandleW(expected.c_str());
        check(loaded != nullptr, "expected SDK DLL is mapped");
        wchar_t loadedPath[32768]{};
        const auto length = GetModuleFileNameW(loaded, loadedPath, static_cast<DWORD>(std::size(loadedPath)));
        check(length && length < std::size(loadedPath) && std::filesystem::equivalent(expected, loadedPath),
              "actual loaded SDK matches requested file");
        ovrGraphicsLuid graphics{};
        sdk(ovr_Create(&session.value, &graphics), "create probe SDK session");
        ovrSessionStatus status{};
        sdk(ovr_GetSessionStatus(session.value, &status), "probe session status");
        statusSample(status, 0);
        check(!status.DisplayLost && !status.ShouldQuit, "SDK session is not lost or exiting");
        // These match runtime session classification. No persistent user
        // resolution, codec, NR, FG or registry setting is written.
        const bool classificationVdxr = ovr_SetBool(session.value, "IsVDXR", ovrTrue) != ovrFalse;
        const bool classificationBridge = ovr_SetBool(session.value, "IsOpenComposite", ovrFalse) != ovrFalse;
        std::cout << "SDK_PROBE_SESSION {\"control_backend\":" << (control ? "true" : "false")
                  << ",\"vdxr_setbool_return\":" << (classificationVdxr ? "true" : "false")
                  << ",\"bridge_setbool_return\":" << (classificationBridge ? "true" : "false")
                  << ",\"hmd_present\":" << (status.HmdPresent ? "true" : "false")
                  << ",\"hmd_mounted\":" << (status.HmdMounted ? "true" : "false") << "}\n";

        if (!control) {
            // Match normal initialization: the first status call starts VD's
            // visible transition; classification follows it. Refresh health
            // afterward. Mounted/visible are focus states, not swapchain
            // allocation preconditions. Log them without claiming scanout.
            sdk(ovr_GetSessionStatus(session.value, &status), "probe allocation session health");
            statusSample(status, 1);
            check(!status.DisplayLost && !status.ShouldQuit, "SDK session lost or exiting before allocation");
            check(status.HmdPresent, "native headset not present before allocation");
        }

        ComPtr<IDXGIFactory4> factory;
        success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "probe factory");
        ComPtr<IDXGIAdapter> adapter;
        if (control) {
            // The private OVRNull control does not advertise a physical LUID.
            ComPtr<IDXGIFactory6> modern;
            success(factory.As(&modern), "control factory6");
            success(modern->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
                    "control adapter");
        } else {
            LUID luid{};
            static_assert(sizeof(luid) == sizeof(graphics.Reserved));
            std::memcpy(&luid, graphics.Reserved, sizeof(luid));
            success(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "SDK requested graphics adapter");
        }
        producer = Device(adapter.Get());
        delivery = Device(adapter.Get());
        check(producer.device != delivery.device, "independent SDK probe devices");
        const std::array<ID3D11Device*, 2> devices{producer.device.Get(),
                                                   single ? producer.device.Get() : delivery.device.Get()};
        ovrTextureSwapChainDesc desc{};
        desc.Type = ovrTexture_2D;
        desc.ArraySize = desc.MipLevels = desc.SampleCount = 1;
        desc.Width = 128;
        desc.Height = 64;
        desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.MiscFlags = ovrTextureMisc_DX_Typeless;
        desc.BindFlags = 0; // Match FG colour presentation's flags.
        std::array<int, 2> lengths{};
        std::array<bool, 2> correctOwner{};
        for (unsigned step = 0; step < 2; ++step) {
            const unsigned index = deliveryFirst ? 1 - step : step;
            sdk(ovr_CreateTextureSwapChainDX(session.value, devices[index], &desc, &session.chains[index]),
                index ? "create delivery SDK chain" : "create producer SDK chain");
            sdk(ovr_GetTextureSwapChainLength(session.value, session.chains[index], &lengths[index]),
                "SDK chain length");
            check(lengths[index] > 0 && lengths[index] < 64, "bounded SDK image count");
            correctOwner[index] = true;
            for (int image = 0; image < lengths[index]; ++image) {
                ComPtr<ID3D11Texture2D> texture;
                sdk(ovr_GetTextureSwapChainBufferDX(
                        session.value, session.chains[index], image, IID_PPV_ARGS(&texture)),
                    "SDK image from requested domain");
                correctOwner[index] = correctOwner[index] && owns(devices[index], texture.Get());
                D3D11_TEXTURE2D_DESC actual{};
                texture->GetDesc(&actual);
                check(actual.Width == 128 && actual.Height == 64 && actual.ArraySize == 1 && actual.MipLevels == 1 &&
                          actual.SampleDesc.Count == 1,
                      "SDK output description matches request");
            }
            std::cout << "SDK_CHAIN {\"index\":" << index
                      << ",\"requested_device_owner\":" << (correctOwner[index] ? "true" : "false") << "}\n";
            check(correctOwner[index], "SDK image must belong to its requested device");
        }
        std::cout << "SDK_DEVICE_PROBE {\"control_backend\":" << (control ? "true" : "false")
                  << ",\"delivery_first\":" << (deliveryFirst ? "true" : "false")
                  << ",\"two_devices_requested\":" << (single ? "false" : "true")
                  << ",\"vdxr_setbool_return\":" << (classificationVdxr ? "true" : "false")
                  << ",\"bridge_setbool_return\":" << (classificationBridge ? "true" : "false")
                  << ",\"producer_images\":" << lengths[0] << ",\"delivery_images\":" << lengths[1]
                  << ",\"requested_device_ownership\":true,\"allocation_only\":true,"
                  << "\"hmd_mounted_at_allocation\":" << (status.HmdMounted ? "true" : "false")
                  << ",\"visible_at_allocation\":" << (status.IsVisible ? "true" : "false")
                  << ",\"native_submission_tested\":false,\"headset_scanout_tested\":false}\n";
        std::puts("PASS: SDK allocation and requested device ownership only");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
