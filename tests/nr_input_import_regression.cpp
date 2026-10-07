// Runtime-linked NR input import recovery with real native D3D11/D3D12 resources.
// No NGX initialization, submitted GPU work, registry writes, or fake successful imports.
#include "pch.h"
#include "runtime.h"
#include <array>

namespace {
    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    void checkHr(HRESULT result) {
        if (FAILED(result))
            throw std::runtime_error("Native D3D prerequisite failed: " + std::to_string(result));
    }
    ComPtr<IUnknown> identity(IUnknown* object) {
        ComPtr<IUnknown> result;
        checkHr(object->QueryInterface(IID_PPV_ARGS(&result)));
        return result;
    }

    using OpenShared = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, HANDLE, REFIID, void**);
    using ExportShared =
        HRESULT(STDMETHODCALLTYPE*)(IDXGIResource1*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*);
    OpenShared originalOpen{};
    ExportShared originalExport{};
    struct Observations {
        ID3D12Device* device{};
        std::array<IDXGIResource1*, 2> resources{};
        std::array<unsigned, 2> exports{};
        std::array<HRESULT, 2> exportResults{};
        std::array<HANDLE, 2> exportedHandles{};
        std::array<HANDLE, 16> openedHandles{};
        std::array<HRESULT, 16> openResults{};
        unsigned opens{};
        unsigned failOnOpen{};
        bool injected{};
        bool overflow{};
    };
    Observations* watched{};

    HRESULT STDMETHODCALLTYPE openShared(ID3D12Device* device, HANDLE handle, REFIID iid, void** output) {
        if (!watched || device != watched->device || iid != __uuidof(ID3D12Resource))
            return originalOpen(device, handle, iid, output);
        const auto index = watched->opens++;
        if (index >= watched->openedHandles.size()) {
            watched->overflow = true;
            return originalOpen(device, handle, iid, output);
        }
        watched->openedHandles[index] = handle;
        HRESULT result;
        if (watched->failOnOpen && watched->opens == watched->failOnOpen) {
            if (output)
                *output = nullptr;
            watched->injected = true;
            result = E_OUTOFMEMORY;
        } else {
            result = originalOpen(device, handle, iid, output);
        }
        watched->openResults[index] = result;
        return result;
    }
    HRESULT STDMETHODCALLTYPE exportShared(
        IDXGIResource1* resource, const SECURITY_ATTRIBUTES* attributes, DWORD access, LPCWSTR name, HANDLE* output) {
        const auto result = originalExport(resource, attributes, access, name, output);
        if (watched) {
            for (size_t i = 0; i < watched->resources.size(); ++i) {
                if (resource != watched->resources[i])
                    continue;
                ++watched->exports[i];
                watched->exportResults[i] = result;
                if (SUCCEEDED(result) && output)
                    watched->exportedHandles[i] = *output;
            }
        }
        return result;
    }

    class ImportHooks {
      public:
        Observations observations;
        ImportHooks(ID3D12Device* device, const std::vector<ComPtr<ID3D11Texture2D>>& images) : m_device(device) {
            require(!watched && images.size() == 2, "Import hook fixture requires two resources and no existing hook");
            for (size_t i = 0; i < images.size(); ++i) {
                checkHr(images[i].As(&m_resources[i]));
                observations.resources[i] = m_resources[i].Get();
            }
            observations.device = device;
            // SDK ID3D12Device::OpenSharedHandle slot 32; IDXGIResource1::CreateSharedHandle slot 13.
            originalOpen = reinterpret_cast<OpenShared>((*reinterpret_cast<void***>(device))[32]);
            originalExport = reinterpret_cast<ExportShared>((*reinterpret_cast<void***>(m_resources[0].Get()))[13]);
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalOpen), openShared);
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalExport), exportShared);
            watched = &observations;
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (result != NO_ERROR) {
                watched = nullptr;
                throw std::runtime_error("Native import hook installation failed: " + std::to_string(result));
            }
        }
        ~ImportHooks() {
            LONG result = DetourTransactionBegin();
            const bool transaction = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalExport), exportShared);
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalOpen), openShared);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (transaction)
                DetourTransactionAbort();
            if (result != NO_ERROR) {
                std::cerr << "FAIL: Native import hook removal failed: " << result << '\n';
                std::terminate();
            }
            watched = nullptr;
        }
        ImportHooks(const ImportHooks&) = delete;
        ImportHooks& operator=(const ImportHooks&) = delete;

      private:
        ComPtr<ID3D12Device> m_device;
        std::array<ComPtr<IDXGIResource1>, 2> m_resources;
    };

    void checkHandle(HANDLE handle, bool live) {
        DWORD flags{};
        const bool actual = GetHandleInformation(handle, &flags) != FALSE;
        const DWORD error = actual ? ERROR_SUCCESS : GetLastError();
        std::cout << "NT handle live=" << actual << " expected=" << live << " error=" << error << '\n';
        require(actual == live && (live || error == ERROR_INVALID_HANDLE),
                live ? "NT export closed after a downstream import failure" : "NT export outlived its swapchain owner");
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static ComPtr<ID3D11Device> installDevice(OpenXrRuntime& runtime) {
            ComPtr<ID3D11Device> device;
            checkHr(D3D11CreateDevice(nullptr,
                                      D3D_DRIVER_TYPE_HARDWARE,
                                      nullptr,
                                      0,
                                      nullptr,
                                      0,
                                      D3D11_SDK_VERSION,
                                      &device,
                                      nullptr,
                                      nullptr));
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            checkHr(device.As(&dxgiDevice));
            checkHr(dxgiDevice->GetAdapter(&adapter));
            DXGI_ADAPTER_DESC desc{};
            checkHr(adapter->GetDesc(&desc));
            std::wcout << L"native adapter=" << desc.Description << L" vendor=" << desc.VendorId << '\n';
            checkHr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&runtime.m_dlssnrDevice)));
            return device;
        }
        static std::unique_ptr<OpenXrRuntime::Swapchain> makeSurface(ID3D11Device* device, bool nt) {
            auto owner = std::make_unique<OpenXrRuntime::Swapchain>();
            owner->resolvedSlices.resize(1);
            for (UINT width : {32u, 48u}) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = width;
                desc.Height = 32;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                desc.MiscFlags = nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                                    : D3D11_RESOURCE_MISC_SHARED;
                ComPtr<ID3D11Texture2D> texture;
                checkHr(device->CreateTexture2D(&desc, nullptr, &texture));
                owner->resolvedSlices[0].images.push_back(std::move(texture));
            }
            return owner;
        }
        static void checkImports(OpenXrRuntime& runtime, const OpenXrRuntime::Swapchain& chain) {
            const auto& imports = chain.resolvedSlices[0].dlssnrImages;
            require(imports.size() == 2, "NR input import cache is incomplete");
            const auto deviceIdentity = identity(runtime.m_dlssnrDevice.Get());
            for (size_t i = 0; i < imports.size(); ++i) {
                require(imports[i] != nullptr, "NR input import is null");
                const auto desc = imports[i]->GetDesc();
                std::cout << "import=" << i << " width=" << desc.Width << " expected=" << (i ? 48 : 32) << '\n';
                require(desc.Width == (i ? 48u : 32u) && desc.Height == 32, "NR import order or description changed");
                ComPtr<ID3D12Device> importedDevice;
                checkHr(imports[i]->GetDevice(IID_PPV_ARGS(&importedDevice)));
                require(identity(importedDevice.Get()) == deviceIdentity,
                        "NR import belongs to a different native device");
            }
        }
        static void ntControl(OpenXrRuntime& runtime, ID3D11Device* device) {
            auto owner = makeSurface(device, true);
            ImportHooks hooks(runtime.m_dlssnrDevice.Get(), owner->resolvedSlices[0].images);
            runtime.ensureSwapchainDlssnrResources(*owner, 0);
            checkImports(runtime, *owner);
            require(hooks.observations.opens == 2 && hooks.observations.exports == std::array<unsigned, 2>{1, 1} &&
                        SUCCEEDED(hooks.observations.openResults[0]) && SUCCEEDED(hooks.observations.openResults[1]) &&
                        !hooks.observations.overflow,
                    "Native NT export/import control prerequisites failed");
            std::cout << "PASS: synthetic NT native export/import control\n";
        }
        static void retry(bool nt) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            auto device = installDevice(runtime);
            if (nt)
                ntControl(runtime, device.Get());
            auto owner = makeSurface(device.Get(), nt);
            ImportHooks hooks(runtime.m_dlssnrDevice.Get(), owner->resolvedSlices[0].images);
            auto& observed = hooks.observations;
            observed.failOnOpen = nt ? 1 : 2;
            bool threw{};
            try {
                runtime.ensureSwapchainDlssnrResources(*owner, 0);
            } catch (const std::exception&) {
                threw = true;
            }
            std::cout << "failure observed: threw=" << threw << " opens=" << observed.opens
                      << " injected=" << observed.injected
                      << " cachedAfterFailure=" << owner->resolvedSlices[0].dlssnrImages.size() << '\n';
            require(threw && observed.injected && observed.opens == (nt ? 1u : 2u) && !observed.overflow,
                    "Expected downstream native import fault was not observed");
            if (nt) {
                require(observed.exports[0] == 1 && SUCCEEDED(observed.exportResults[0]) && observed.exportedHandles[0],
                        "Native NT export prerequisite failed before the injected fault");
                // Check before unrelated native allocations could recycle a closed handle value.
                checkHandle(observed.exportedHandles[0], true);
            } else {
                require(SUCCEEDED(observed.openResults[0]) && observed.openResults[1] == E_OUTOFMEMORY,
                        "Native legacy prefix prerequisite failed before the injected fault");
            }
            require(owner->resolvedSlices[0].dlssnrImages.empty(),
                    "Failed preparation published a partial NR input cache");
            observed.failOnOpen = 0;
            runtime.ensureSwapchainDlssnrResources(*owner, 0);
            checkImports(runtime, *owner);
            require(observed.opens == (nt ? 3u : 4u), "Retry did not import every native source image");
            if (nt) {
                require(observed.openedHandles[0] == observed.openedHandles[1] &&
                            observed.exports == std::array<unsigned, 2>{1, 1},
                        "Retry re-exported a source resource instead of borrowing its retained NT handle");
            }
            std::cout << "complete retry native resource opens=" << observed.opens << " expected=" << (nt ? 3 : 4)
                      << " exportsByResource=" << observed.exports[0] << ',' << observed.exports[1] << '\n';
            const auto imports = owner->resolvedSlices[0].dlssnrImages;
            const auto opens = observed.opens;
            runtime.ensureSwapchainDlssnrResources(*owner, 0);
            require(observed.opens == opens && owner->resolvedSlices[0].dlssnrImages == imports,
                    "Completed NR imports were replaced or reopened on a cached call");
            std::cout << "cached call additional resource opens=" << observed.opens - opens
                      << " retained import identities=2\n";
            if (nt) {
                require(observed.exports == std::array<unsigned, 2>{1, 1}, "Cached call re-exported an NT resource");
                const auto handles = observed.exportedHandles;
                owner.reset();
                for (auto handle : handles)
                    checkHandle(handle, false);
            }
            checkHr(device->GetDeviceRemovedReason());
            checkHr(runtime.m_dlssnrDevice->GetDeviceRemovedReason());
            std::cout << "PASS: " << (nt ? "synthetic NT" : "native legacy")
                      << " failed import is retryable, complete imports are cached, device health is good\n";
        }
    };
} // namespace virtualdesktop_openxr

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        require(argc == 2, "Usage: nr_input_import_regression <legacy-retry|nt-retry>");
        const std::string mode = argv[1];
        require(mode == "legacy-retry" || mode == "nt-retry", "Unknown test mode");
        virtualdesktop_openxr::RuntimeInputRegression::retry(mode == "nt-retry");
        std::cout << "PASS: native observations detached and runtime teardown completed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
