#include "frame_generation.h"
#include "FrameGenerationCopyCS.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace virtualdesktop_openxr {
    using Microsoft::WRL::ComPtr;

    namespace {
        struct BgraCopy {
            ComPtr<ID3D11Texture2D> source, output;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11UnorderedAccessView> uav;
            ComPtr<ID3D11ComputeShader> shader;
            ComPtr<ID3DDeviceContextState> contextState;
            HRESULT initialize(const FrameGeneration::InitInfo& info) noexcept;
            void copy(ID3D11DeviceContext4* context,
                      ID3D11Texture2D* destination,
                      uint32_t eyeWidth,
                      uint32_t bgraMask,
                      const std::array<ComPtr<ID3D11Texture2D>, 2>& sources,
                      const std::array<VDXRFG_ViewV1, 2>& views) const;
        };

        bool isBgra(DXGI_FORMAT format) {
            return format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                   format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
    } // namespace

    struct FrameGeneration::State {
        HMODULE module{};
        VDXRFG_ApiV1 api{};
        VDXRFG_ApiV3 api3{};
        VDXRFG_ApiV4 api4{};
        void* context{};
        InitInfo info;
        ComPtr<ID3D11Device5> device11;
        ComPtr<ID3D11DeviceContext4> context11;
        ComPtr<ID3D12Device> device12;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D11Texture2D> input11;
        ComPtr<ID3D12Resource> input12;
        std::array<std::array<ComPtr<ID3D11Texture2D>, 4>, 2> outputs11;
        std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> outputs12;
        ComPtr<ID3D11Fence> producer11, consumed11, ready11, firstReady11, consumer11;
        ComPtr<ID3D12Fence> firstReady12;
        ComPtr<ID3D12Fence> producer12;
        std::array<uint64_t, 2> liveTokens{};
        std::array<uint64_t, 2> consumerValues{};
        std::array<std::array<uint64_t, 4>, 2> roleConsumerValues{};
        std::array<std::array<uint64_t, 4>, 2> registeredCopyValues{};
        std::array<VDXRFG_TicketV1, 2> tickets{};
        std::array<std::array<ComPtr<ID3D11Texture2D>, 4>, 2> retainedDestinations;
        std::array<std::array<ComPtr<ID3D11Texture2D>, 2>, 2> capturedSources;
        std::array<std::array<VDXRFG_ViewV1, 2>, 2> capturedViews{};
        std::array<uint32_t, 2> capturedBgraMask{};
        BgraCopy bgra;
        std::array<ComPtr<ID3D11Texture2D>, 2> producerSources;
        uint64_t generation{};
        uint64_t producerValue{}, consumerValue{}, consumedValue{}, previousSerial{};
        uint64_t inputOwnerToken{}, inputOwnerGeneration{};
        bool poisoned{}, hostCompletionUnknown{};
        bool hostOwnsReal{};
        HANDLE copyCompletionEvent{};
        HANDLE readyCompletionEvent{};
        std::array<uint64_t, 2> registeredReadyValues{};
        std::array<uint64_t, 2> firstReadyValues{}, registeredFirstReadyValues{};
        std::array<bool, 2> queuedFullReadyWait{}, queuedFirstReadyWait{};

        ~State() {
            // FrameGeneration::shutdown has already released context ownership.
            if (copyCompletionEvent) {
                CloseHandle(copyCompletionEvent);
            }
            if (readyCompletionEvent) {
                CloseHandle(readyCompletionEvent);
            }
            if (module) {
                FreeLibrary(module);
            }
        }
    };

    namespace {
        std::atomic<uint64_t> nextGeneration{1};
        HRESULT shareTexture(ID3D11Texture2D* texture, ID3D12Device* device, ID3D12Resource** result) {
            ComPtr<IDXGIResource1> dxgi;
            HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&dxgi));
            if (FAILED(hr)) {
                return hr;
            }
            HANDLE handle{};
            hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
            if (FAILED(hr)) {
                return hr;
            }
            hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(result));
            CloseHandle(handle);
            return hr;
        }

        HRESULT importFence(ID3D12Device* device, ID3D11Device5* consumer, ID3D12Fence* fence, ID3D11Fence** result) {
            HANDLE handle{};
            HRESULT hr = device->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle);
            if (FAILED(hr)) {
                return hr;
            }
            hr = consumer->OpenSharedFence(handle, IID_PPV_ARGS(result));
            CloseHandle(handle);
            return hr;
        }

        bool sameDevice(ID3D11Device* expected, ID3D11Texture2D* texture) {
            ComPtr<ID3D11Device> owner;
            ComPtr<IUnknown> a, b;
            texture->GetDevice(&owner);
            if (owner.Get() == expected) {
                return true;
            }
            // Different interface pointers may still identify the same COM device.
            return SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&a))) && SUCCEEDED(owner.As(&b)) &&
                   a.Get() == b.Get();
        }

        // Sources and view bounds have already been validated and retained.
        // Copy only subresource zero, including for array/mip-backed inputs.
        void copyStereo(ID3D11DeviceContext* context,
                        ID3D11Texture2D* destination,
                        uint32_t eyeWidth,
                        const std::array<ComPtr<ID3D11Texture2D>, 2>& sources,
                        const std::array<VDXRFG_ViewV1, 2>& views) {
            const auto& left = views[0].rect;
            const auto& right = views[1].rect;
            if (sources[0].Get() == sources[1].Get() && left.width == eyeWidth && right.width == eyeWidth &&
                left.y == right.y && left.height == right.height && uint64_t(left.x) + left.width == right.x) {
                const D3D11_BOX box{left.x, left.y, 0, right.x + right.width, left.y + left.height, 1};
                context->CopySubresourceRegion(destination, 0, 0, 0, 0, sources[0].Get(), 0, &box);
            } else {
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    const auto& rect = views[eye].rect;
                    const D3D11_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
                    context->CopySubresourceRegion(destination, 0, eye * eyeWidth, 0, 0, sources[eye].Get(), 0, &box);
                }
            }
        }

        HRESULT BgraCopy::initialize(const FrameGeneration::InitInfo& info) noexcept {
            if (shader) {
                return S_OK;
            }
            BgraCopy candidate;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = info.eyeWidth;
            desc.Height = info.height;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = info.device11->CreateTexture2D(&desc, nullptr, &candidate.source);
            if (FAILED(hr))
                return hr;
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            hr = info.device11->CreateShaderResourceView(candidate.source.Get(), &srvDesc, &candidate.srv);
            if (FAILED(hr))
                return hr;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            hr = info.device11->CreateTexture2D(&desc, nullptr, &candidate.output);
            if (FAILED(hr))
                return hr;
            hr = info.device11->CreateUnorderedAccessView(candidate.output.Get(), nullptr, &candidate.uav);
            if (FAILED(hr))
                return hr;
            hr = info.device11->CreateComputeShader(
                g_FrameGenerationCopyCS, sizeof(g_FrameGenerationCopyCS), nullptr, &candidate.shader);
            if (FAILED(hr))
                return hr;
            const auto level = info.device11->GetFeatureLevel();
            const auto flags = (info.device11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                                   ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                                   : 0u;
            hr = info.device11->CreateDeviceContextState(
                flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &candidate.contextState);
            if (FAILED(hr))
                return hr;
            *this = std::move(candidate);
            return S_OK;
        }

        void BgraCopy::copy(ID3D11DeviceContext4* context,
                            ID3D11Texture2D* destination,
                            uint32_t eyeWidth,
                            uint32_t bgraMask,
                            const std::array<ComPtr<ID3D11Texture2D>, 2>& sources,
                            const std::array<VDXRFG_ViewV1, 2>& views) const {
            // One eye-sized cache also supports different source families in
            // the two eyes, with no per-frame views, constants or CPU waits.
            struct Restore {
                ID3D11DeviceContext4* context;
                ComPtr<ID3DDeviceContextState> saved;
                ~Restore() {
                    context->SwapDeviceContextState(saved.Get(), nullptr);
                }
            } restore{context};
            context->SwapDeviceContextState(contextState.Get(), &restore.saved);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const auto& rect = views[eye].rect;
                const D3D11_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
                if (!(bgraMask & (1u << eye))) {
                    context->CopySubresourceRegion(destination, 0, eye * eyeWidth, 0, 0, sources[eye].Get(), 0, &box);
                    continue;
                }
                context->CopySubresourceRegion(source.Get(), 0, 0, 0, 0, sources[eye].Get(), 0, &box);
                context->CSSetShader(shader.Get(), nullptr, 0);
                context->CSSetShaderResources(0, 1, srv.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                context->Dispatch((eyeWidth + 15) / 16, (rect.height + 15) / 16, 1);
                ID3D11ShaderResourceView* nullSrv{};
                ID3D11UnorderedAccessView* nullUav{};
                context->CSSetShaderResources(0, 1, &nullSrv);
                context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
                context->CopySubresourceRegion(destination, 0, eye * eyeWidth, 0, 0, output.Get(), 0, nullptr);
            }
        }
    } // namespace

    FrameGeneration::FrameGeneration() = default;

    FrameGeneration::~FrameGeneration() {
        if (FAILED(shutdown())) {
            // Retain the complete module/COM graph on uncertain GPU completion.
            // No destructor may release only part of an owned transaction.
            (void)m_state.release();
        }
    }

    HRESULT FrameGeneration::initialize(const InitInfo& info) noexcept {
#ifndef _WIN64
        return E_NOTIMPL;
#else
        if (m_state || !info.device11 || !info.context11 || !info.device12 || !info.modulePath.is_absolute() ||
            !info.eyeWidth || info.eyeWidth > UINT32_MAX / 2 || !info.height ||
            (info.multiplier != 2 && info.multiplier != 3 && info.multiplier != 4) ||
            (info.format != DXGI_FORMAT_R8G8B8A8_UNORM && info.format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            return VDXRFG_E_REJECTED;
        }
        ComPtr<ID3D11Device> contextDevice;
        ComPtr<IUnknown> contextIdentity, deviceIdentity;
        info.context11->GetDevice(&contextDevice);
        if (info.context11->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || FAILED(contextDevice.As(&contextIdentity)) ||
            FAILED(info.device11->QueryInterface(IID_PPV_ARGS(&deviceIdentity))) ||
            contextIdentity.Get() != deviceIdentity.Get()) {
            return VDXRFG_E_REJECTED;
        }
        try {
            auto state = std::make_unique<State>();
            state->generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
            if (!state->generation || state->generation == UINT64_MAX) {
                return VDXRFG_E_REJECTED;
            }
            state->info = info;
            state->device11 = info.device11;
            state->context11 = info.context11;
            state->device12 = info.device12;
            state->copyCompletionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!state->copyCompletionEvent) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            state->readyCompletionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!state->readyCompletionEvent) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            state->module = LoadLibraryExW(
                info.modulePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!state->module) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            auto getApi = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV1"));
            if (!getApi) {
                return VDXRFG_E_REJECTED;
            }
            state->api.struct_size = sizeof(state->api);
            state->api.api_version = VDXRFG_API_VERSION;
            HRESULT hr = getApi(&state->api);
            if (FAILED(hr) || state->api.api_version != VDXRFG_API_VERSION ||
                state->api.struct_size != sizeof(state->api) || !state->api.Create || !state->api.Submit ||
                !state->api.Retire || !state->api.Reset || !state->api.GetStatistics || !state->api.Destroy) {
                return VDXRFG_E_REJECTED;
            }
            VDXRFG_ApiV2 api2{};
            const auto getApi2 = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV2"));
            if (getApi2) {
                api2.struct_size = sizeof(api2);
                api2.api_version = VDXRFG_API_VERSION_2;
                hr = getApi2(&api2);
                if (FAILED(hr) || api2.struct_size != sizeof(api2) || api2.api_version != VDXRFG_API_VERSION_2 ||
                    api2.reserved || !api2.Create || !api2.Submit || !api2.Retire || !api2.Reset ||
                    !api2.GetStatistics || !api2.Destroy) {
                    return VDXRFG_E_REJECTED;
                }
                state->api.Submit = api2.Submit;
                state->api.Retire = api2.Retire;
                state->api.Reset = api2.Reset;
                state->api.GetStatistics = api2.GetStatistics;
                state->api.Destroy = api2.Destroy;
            }
            const auto getApi3 = reinterpret_cast<VDXRFG_GetApiV3Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV3"));
            auto& api3 = state->api3;
            if (getApi3) {
                api3.struct_size = sizeof(api3);
                api3.api_version = VDXRFG_API_VERSION_3;
                hr = getApi3(&api3);
                if (FAILED(hr) || api3.struct_size != sizeof(api3) || api3.api_version != VDXRFG_API_VERSION_3 ||
                    api3.max_multiplier < info.multiplier || !api3.Create || !api3.Submit || !api3.Retire ||
                    !api3.Reset || !api3.GetStatistics || !api3.Destroy)
                    return VDXRFG_E_REJECTED;
                state->api.Retire = api3.Retire;
                state->api.Reset = api3.Reset;
                state->api.GetStatistics = api3.GetStatistics;
                state->api.Destroy = api3.Destroy;
            }
            const auto getApi4 = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV4"));
            if (getApi4) {
                auto& api4 = state->api4;
                api4.struct_size = sizeof(api4);
                api4.api_version = VDXRFG_API_VERSION_4;
                hr = getApi4(&api4);
                if (FAILED(hr) || api4.struct_size != sizeof(api4) || api4.api_version != VDXRFG_API_VERSION_4 ||
                    api4.max_multiplier < info.multiplier || !api4.Create || !api4.Submit || !api4.Retire ||
                    !api4.Reset || !api4.GetStatistics || !api4.Destroy || !api4.GetOutputReadiness)
                    return VDXRFG_E_REJECTED;
                // Normalize the V3-payload dispatch. V4's extra query stays
                // separate; every legacy full-ticket fence keeps its meaning.
                api3 = {sizeof(api3),
                        VDXRFG_API_VERSION_3,
                        api4.supported_create_flags,
                        api4.max_multiplier,
                        api4.Create,
                        api4.Submit,
                        api4.Retire,
                        api4.Reset,
                        api4.GetStatistics,
                        api4.Destroy};
                state->api.Retire = api4.Retire;
                state->api.Reset = api4.Reset;
                state->api.GetStatistics = api4.GetStatistics;
                state->api.Destroy = api4.Destroy;
            }
            const bool extended = getApi3 || getApi4;
            if (info.multiplier == 4 && !extended)
                return VDXRFG_E_REJECTED;
            const uint32_t supportedFlags = extended ? api3.supported_create_flags : api2.supported_create_flags;
            uint32_t createFlags = supportedFlags & (VDXRFG_CREATE_HOST_OWNS_REAL |
                                                     (info.enableGpuTiming ? VDXRFG_CREATE_GPU_TIMING : 0u));
            if (info.useFidelityFxFull)
                createFlags |= VDXRFG_CREATE_FIDELITYFX_FULL;
            if (extended && (createFlags & VDXRFG_CREATE_HOST_OWNS_REAL) &&
                (supportedFlags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS))
                createFlags |= VDXRFG_CREATE_OMIT_REAL_OUTPUTS;
            state->hostOwnsReal = (createFlags & VDXRFG_CREATE_HOST_OWNS_REAL) != 0;
            // Backend choice is required, unlike optional diagnostics/real
            // ownership. An old module must not silently substitute NVIDIA.
            if (info.useFidelityFxFull && (!(supportedFlags & VDXRFG_CREATE_FIDELITYFX_FULL))) {
                return VDXRFG_E_REJECTED;
            }
            D3D12_COMMAND_QUEUE_DESC queue{};
            hr = info.device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&state->queue));
            if (FAILED(hr)) {
                return hr;
            }
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = 2 * info.eyeWidth;
            texture.Height = info.height;
            texture.MipLevels = texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            texture.SampleDesc.Count = 1;
            texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            hr = info.device11->CreateTexture2D(&texture, nullptr, &state->input11);
            if (FAILED(hr)) {
                return hr;
            }
            hr = shareTexture(state->input11.Get(), info.device12, &state->input12);
            if (FAILED(hr)) {
                return hr;
            }
            VDXRFG_CreateInfoV3 create{};
            create.struct_size = sizeof(create);
            create.api_version = VDXRFG_API_VERSION_3;
            create.flags = createFlags;
            create.device = info.device12;
            create.queue = state->queue.Get();
            create.input = state->input12.Get();
            create.width = texture.Width;
            create.height = texture.Height;
            create.format = info.format;
            create.multiplier = info.multiplier;
            for (uint32_t bank = 0; bank < 2; ++bank) {
                for (uint32_t role = 0; role < 4; ++role) {
                    if ((role == 1 && info.multiplier == 2) || (role == 3 && info.multiplier != 4) ||
                        (role == 2 && (createFlags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS))) {
                        continue;
                    }
                    hr = info.device11->CreateTexture2D(&texture, nullptr, &state->outputs11[bank][role]);
                    if (FAILED(hr)) {
                        return hr;
                    }
                    hr = shareTexture(state->outputs11[bank][role].Get(), info.device12, &state->outputs12[bank][role]);
                    if (FAILED(hr)) {
                        return hr;
                    }
                    create.outputs[bank][role] = state->outputs12[bank][role].Get();
                }
            }
            hr = info.device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state->producer11));
            if (FAILED(hr)) {
                return hr;
            }
            HANDLE handle{};
            hr = state->producer11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
            if (FAILED(hr)) {
                return hr;
            }
            hr = info.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&state->producer12));
            CloseHandle(handle);
            if (FAILED(hr)) {
                return hr;
            }
            hr = info.device11->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->consumer11));
            if (FAILED(hr)) {
                return hr;
            }
            if (extended) {
                hr = api3.Create(&create, &state->context);
            } else {
                VDXRFG_CreateInfoV1 legacy{};
                legacy.struct_size = sizeof(legacy);
                legacy.api_version = VDXRFG_API_VERSION;
                legacy.device = create.device;
                legacy.queue = create.queue;
                legacy.input = create.input;
                legacy.width = create.width;
                legacy.height = create.height;
                legacy.format = create.format;
                legacy.multiplier = create.multiplier;
                for (unsigned bank = 0; bank < 2; ++bank)
                    for (unsigned role = 0; role < 3; ++role)
                        legacy.outputs[bank][role] = create.outputs[bank][role];
                if (getApi2) {
                    VDXRFG_CreateInfoV2 options{};
                    options.struct_size = sizeof(options);
                    options.api_version = VDXRFG_API_VERSION_2;
                    options.base = legacy;
                    options.flags = createFlags;
                    hr = api2.Create(&options, &state->context);
                } else {
                    hr = state->api.Create(&legacy, &state->context);
                }
            }
            if (FAILED(hr)) {
                if (state->context) {
                    state->poisoned = true;
                    m_state = std::move(state);
                }
                return hr;
            }
            if (!state->context) {
                return VDXRFG_E_REJECTED;
            }
            m_state = std::move(state);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
#endif
    }

    HRESULT FrameGeneration::capture(const FinalProjectionInput& input, OwnedFrameTransaction& transaction) noexcept {
        if (!m_state) {
            return VDXRFG_E_REJECTED;
        }
        auto& state = *m_state;
        // Packing can overwrite the shared atlas before Submit rejects. No
        // failed capture may leave an older ticket claiming those pixels.
        state.inputOwnerToken = state.inputOwnerGeneration = 0;
        if (state.poisoned || transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        if (state.producerValue == UINT64_MAX || input.slots.count == 0 || input.slots.count > 4 ||
            (input.slots.count != 1 && input.slots.count != state.info.multiplier) || !input.serial ||
            input.time <= 0 || input.period <= 0 ||
            input.slots.roles[input.slots.count - 1] != FrameGenerationRole::real ||
            input.slots.targets[input.slots.count - 1] != input.time) {
            return VDXRFG_E_REJECTED;
        }
        if (state.producer11->GetCompletedValue() == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (state.producer11->GetCompletedValue() < state.producerValue ||
            std::all_of(state.liveTokens.begin(), state.liveTokens.end(), [](uint64_t token) { return token != 0; })) {
            return VDXRFG_E_BUSY;
        }
        VDXRFG_SubmitInfoV3 submit{};
        submit.struct_size = sizeof(submit);
        submit.api_version = VDXRFG_API_VERSION_3;
        submit.input = state.input12.Get();
        submit.producer_fence = state.producer12.Get();
        submit.serial = input.serial;
        submit.display_time_ns = input.time;
        submit.display_period_ns = input.period;
        D3D11_TEXTURE2D_DESC description{};
        uint32_t bgraMask{};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& view = input.views[eye];
            const auto& rect = view.rect;
            if (!input.colour[eye]) {
                return VDXRFG_E_REJECTED;
            }
            // Final NR eyes share one packed texture. Its device and immutable
            // description need checking once, but each view still needs validation.
            if (eye == 0 || input.colour[eye].Get() != input.colour[0].Get()) {
                if (!sameDevice(state.device11.Get(), input.colour[eye].Get())) {
                    return VDXRFG_E_REJECTED;
                }
                input.colour[eye]->GetDesc(&description);
            }
            // Runtime-resolved array slice zero can alias the app array; its
            // selected image is subresource zero. Higher slices are resolved
            // to standalone images before reaching this interface.
            const bool eyeBgra = isBgra(description.Format);
            if (description.SampleDesc.Count != 1 || !description.ArraySize || !rect.width || !rect.height ||
                rect.width != state.info.eyeWidth || rect.height != state.info.height ||
                uint64_t(rect.x) + rect.width > description.Width ||
                uint64_t(rect.y) + rect.height > description.Height ||
                (!eyeBgra && description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                 description.Format != state.info.format && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)) {
                return VDXRFG_E_REJECTED;
            }
            if (eyeBgra)
                bgraMask |= 1u << eye;
            submit.views[eye] = view;
            submit.views[eye].rect.x = eye * state.info.eyeWidth;
            submit.views[eye].rect.y = 0;
            float norm{};
            for (const float component : view.orientation) {
                if (!std::isfinite(component)) {
                    return VDXRFG_E_REJECTED;
                }
                norm += component * component;
            }
            if (std::abs(norm - 1.f) > .02f) {
                return VDXRFG_E_REJECTED;
            }
            for (const float component : view.position) {
                if (!std::isfinite(component)) {
                    return VDXRFG_E_REJECTED;
                }
            }
            for (const float component : view.fov) {
                if (!std::isfinite(component)) {
                    return VDXRFG_E_REJECTED;
                }
            }
            if (view.fov[0] >= view.fov[1] || view.fov[3] >= view.fov[2]) {
                return VDXRFG_E_REJECTED;
            }
        }
        for (uint32_t slot = 0; slot < input.slots.count; ++slot) {
            submit.target_time_ns[slot] = input.slots.targets[slot];
            if (slot < input.slots.count - 1) {
                if (input.slots.roles[slot] != (slot == 0   ? FrameGenerationRole::synthetic1
                                                : slot == 1 ? FrameGenerationRole::synthetic2
                                                            : FrameGenerationRole::synthetic3) ||
                    !std::isfinite(input.slots.fractions[slot]) || input.slots.fractions[slot] <= 0 ||
                    input.slots.fractions[slot] >= 1 || input.slots.targets[slot] >= input.slots.targets[slot + 1]) {
                    return VDXRFG_E_REJECTED;
                }
                submit.fractions[slot] = input.slots.fractions[slot];
            }
        }
        if (bgraMask) {
            const auto result = state.bgra.initialize(state.info);
            // No GPU work or history change occurred. Optional conversion
            // allocation failure must preserve the complete real-frame path.
            if (FAILED(result))
                return VDXRFG_E_REJECTED;
        }
        if (input.slots.count == 1 && state.previousSerial) {
            const auto resetResult = state.api.Reset(state.context, 0);
            if (FAILED(resetResult)) {
                return resetResult;
            }
            state.previousSerial = 0;
        }
        // A subsequent producer never overwrites a registered input still read
        // by the FG queue. This is a GPU dependency, not a CPU completion wait.
        if (state.consumed11 && state.consumedValue) {
            HRESULT hr = state.context11->Wait(state.consumed11.Get(), state.consumedValue);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        state.producerSources = input.colour;
        if (bgraMask) {
            state.bgra.copy(
                state.context11.Get(), state.input11.Get(), state.info.eyeWidth, bgraMask, input.colour, input.views);
        } else {
            copyStereo(state.context11.Get(), state.input11.Get(), state.info.eyeWidth, input.colour, input.views);
        }
        submit.producer_value = ++state.producerValue;
        HRESULT hr = state.context11->Signal(state.producer11.Get(), submit.producer_value);
        if (FAILED(hr)) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        state.context11->Flush(); // Dispatch the cross-API producer dependency; does not drain the GPU.
        if (state.api3.Submit) {
            hr = state.api3.Submit(state.context, &submit, &transaction.ticket);
        } else {
            VDXRFG_SubmitInfoV1 legacy{};
            legacy.struct_size = sizeof(legacy);
            legacy.api_version = VDXRFG_API_VERSION;
            legacy.input = submit.input;
            legacy.producer_fence = submit.producer_fence;
            legacy.producer_value = submit.producer_value;
            legacy.serial = submit.serial;
            legacy.display_time_ns = submit.display_time_ns;
            legacy.display_period_ns = submit.display_period_ns;
            for (unsigned i = 0; i < 3; ++i)
                legacy.target_time_ns[i] = submit.target_time_ns[i];
            for (unsigned i = 0; i < 2; ++i) {
                legacy.fractions[i] = submit.fractions[i];
                legacy.views[i] = submit.views[i];
            }
            hr = state.api.Submit(state.context, &legacy, &transaction.ticket);
        }
        if (transaction.ticket.token) {
            transaction.ownerGeneration = state.generation;
            transaction.slots = input.slots;
            if (transaction.ticket.bank >= state.liveTokens.size() || state.liveTokens[transaction.ticket.bank]) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
            state.liveTokens[transaction.ticket.bank] = transaction.ticket.token;
            state.tickets[transaction.ticket.bank] = transaction.ticket;
            state.capturedSources[transaction.ticket.bank] = input.colour;
            state.capturedViews[transaction.ticket.bank] = input.views;
            state.capturedBgraMask[transaction.ticket.bank] = bgraMask;
        }
        if (FAILED(hr)) {
            state.poisoned = hr == VDXRFG_E_POISONED;
            return hr;
        }
        const bool prime = state.previousSerial == 0;
        const uint32_t expectedMask = (state.hostOwnsReal ? 0u : 4u) | (prime                        ? 0u
                                                                        : state.info.multiplier == 4 ? 11u
                                                                        : state.info.multiplier == 3 ? 3u
                                                                                                     : 1u);
        if (!transaction.ticket.token || transaction.ticket.prime != static_cast<uint32_t>(prime) ||
            transaction.ticket.role_mask != expectedMask || transaction.ticket.reserved ||
            transaction.ticket.current_serial != input.serial ||
            transaction.ticket.previous_serial != state.previousSerial || !transaction.ticket.generation ||
            !transaction.ticket.input_consumed_fence || !transaction.ticket.output_ready_fence ||
            !transaction.ticket.input_consumed_value || !transaction.ticket.output_ready_value) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        transaction.slots = input.slots;
        state.previousSerial = input.serial;
        state.liveTokens[transaction.ticket.bank] = transaction.ticket.token;
        if (!state.consumed11) {
            hr = importFence(state.device12.Get(),
                             state.device11.Get(),
                             static_cast<ID3D12Fence*>(transaction.ticket.input_consumed_fence),
                             &state.consumed11);
            if (FAILED(hr)) {
                state.poisoned = true;
                return VDXRFG_E_POISONED;
            }
        }
        state.consumedValue = transaction.ticket.input_consumed_value;
        if (!state.ready11) {
            hr = importFence(state.device12.Get(),
                             state.device11.Get(),
                             static_cast<ID3D12Fence*>(transaction.ticket.output_ready_fence),
                             &state.ready11);
            if (FAILED(hr)) {
                state.poisoned = true;
                return VDXRFG_E_POISONED;
            }
        }
        if (state.api4.GetOutputReadiness) {
            for (const unsigned role : {0u, 1u, 3u}) {
                if (!(transaction.ticket.role_mask & (1u << role)))
                    continue;
                VDXRFG_OutputReadinessV4 output{sizeof(output), VDXRFG_API_VERSION_4, nullptr, 0};
                hr = state.api4.GetOutputReadiness(state.context, transaction.ticket.token, role, &output);
                if (FAILED(hr) || output.struct_size != sizeof(output) || output.api_version != VDXRFG_API_VERSION_4 ||
                    !output.fence || !output.value) {
                    state.poisoned = state.hostCompletionUnknown = true;
                    return VDXRFG_E_POISONED;
                }
                if (output.fence == transaction.ticket.output_ready_fence) {
                    if (output.value == transaction.ticket.output_ready_value)
                        continue;
                } else if (role == 0) {
                    const auto fence = static_cast<ID3D12Fence*>(output.fence);
                    if (!state.firstReady11) {
                        hr = importFence(state.device12.Get(), state.device11.Get(), fence, &state.firstReady11);
                        if (FAILED(hr)) {
                            state.poisoned = state.hostCompletionUnknown = true;
                            return VDXRFG_E_POISONED;
                        }
                        state.firstReady12 = fence;
                    }
                    if (state.firstReady12.Get() == fence) {
                        state.firstReadyValues[transaction.ticket.bank] = output.value;
                        continue;
                    }
                }
                // Later roles must use the authoritative full completion.
                // Reject a broken negotiation while retaining all owned work.
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        state.inputOwnerToken = transaction.ticket.token;
        state.inputOwnerGeneration = transaction.ticket.generation;
        return S_OK;
    }

    bool FrameGeneration::ready(const OwnedFrameTransaction& transaction) const noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || !transaction.ticket.output_ready_fence ||
            transaction.ticket.bank >= 2 || transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return false;
        }
        const auto& ticket = m_state->tickets[transaction.ticket.bank];
        const auto value = m_state->ready11->GetCompletedValue();
        return value != UINT64_MAX && value >= ticket.output_ready_value;
    }

    HRESULT FrameGeneration::prepareOutputReadyWait(const OwnedFrameTransaction& transaction,
                                                    HANDLE& completionEvent,
                                                    FrameGenerationRole role) noexcept {
        completionEvent = nullptr;
        if (!m_state || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        auto& state = *m_state;
        if (state.poisoned) {
            return VDXRFG_E_POISONED;
        }
        const auto& ticket = state.tickets[transaction.ticket.bank];
        const auto index = static_cast<unsigned>(role);
        if (index >= 4 || (role != FrameGenerationRole::real && !(ticket.role_mask & (1u << index))))
            return VDXRFG_E_REJECTED;
        const bool first = role == FrameGenerationRole::synthetic1 && state.firstReadyValues[ticket.bank] != 0;
        const auto fence = first ? state.firstReady11.Get() : state.ready11.Get();
        const auto value = first ? state.firstReadyValues[ticket.bank] : ticket.output_ready_value;
        if (!fence || !ticket.output_ready_fence || !value) {
            return VDXRFG_E_REJECTED;
        }
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (completed >= value) {
            return S_OK;
        }
        auto& registered =
            first ? state.registeredFirstReadyValues[ticket.bank] : state.registeredReadyValues[ticket.bank];
        if (registered != value) {
            const auto hr = fence->SetEventOnCompletion(value, state.readyCompletionEvent);
            if (FAILED(hr)) {
                return hr;
            }
            registered = value;
        }
        // A timeout keeps the kernel registration live. Never reset or close
        // this event here: an older target may still signal it after a timeout.
        completionEvent = state.readyCompletionEvent;
        return VDXRFG_E_BUSY;
    }

    HRESULT FrameGeneration::copyOutput(OwnedFrameTransaction& transaction,
                                        FrameGenerationRole role,
                                        ID3D11Texture2D* destination) noexcept {
        if (!m_state || m_state->poisoned || !destination || !transaction.ticket.token ||
            transaction.ticket.bank >= 2 || transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        auto& state = *m_state;
        if (role != FrameGenerationRole::real && role != FrameGenerationRole::synthetic1 &&
            role != FrameGenerationRole::synthetic2 && role != FrameGenerationRole::synthetic3) {
            return VDXRFG_E_REJECTED;
        }
        const uint32_t index = static_cast<unsigned>(role);
        const auto& ticket = state.tickets[transaction.ticket.bank];
        const bool available =
            (role == FrameGenerationRole::real && state.hostOwnsReal) || (ticket.role_mask & (1u << index));
        if (!available || state.consumerValue == UINT64_MAX || !sameDevice(state.device11.Get(), destination)) {
            return VDXRFG_E_REJECTED;
        }
        D3D11_TEXTURE2D_DESC description{};
        destination->GetDesc(&description);
        if (description.Width != 2 * state.info.eyeWidth || description.Height != state.info.height ||
            description.ArraySize != 1 || description.MipLevels != 1 || description.SampleDesc.Count != 1 ||
            (description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && description.Format != state.info.format &&
             description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)) {
            return VDXRFG_E_REJECTED;
        }
        if (state.roleConsumerValues[transaction.ticket.bank][index]) {
            // A prepared role identifies one SDK image for the ticket's entire
            // lifetime. A cloned transaction must not redirect that GPU copy.
            if (state.retainedDestinations[transaction.ticket.bank][index].Get() != destination) {
                return VDXRFG_E_REJECTED;
            }
            transaction.destinations[index] = destination;
            transaction.consumerValue = state.roleConsumerValues[transaction.ticket.bank][index];
            return S_OK;
        }
        for (unsigned bank = 0; bank < state.liveTokens.size(); ++bank) {
            for (const auto& prepared : state.retainedDestinations[bank]) {
                if (prepared.Get() == destination) {
                    return VDXRFG_E_REJECTED;
                }
            }
            for (const auto& source : state.capturedSources[bank]) {
                if (source.Get() == destination) {
                    return VDXRFG_E_REJECTED;
                }
            }
        }
        HRESULT hr = S_OK;
        bool early = role == FrameGenerationRole::synthetic1 && state.firstReadyValues[ticket.bank] != 0;
        if (early) {
            const auto completed = state.ready11->GetCompletedValue();
            if (completed == UINT64_MAX) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
            // A completed whole batch needs just the existing shared wait;
            // do not add a separate early wait when it cannot buy time.
            early = completed < ticket.output_ready_value;
        }
        if (role != FrameGenerationRole::real && !state.queuedFullReadyWait[ticket.bank] &&
            (!early || !state.queuedFirstReadyWait[ticket.bank])) {
            const auto fence = early ? state.firstReady11.Get() : state.ready11.Get();
            const auto value = early ? state.firstReadyValues[ticket.bank] : ticket.output_ready_value;
            hr = state.context11->Wait(fence, value);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
            if (early)
                state.queuedFirstReadyWait[ticket.bank] = true;
            else
                state.queuedFullReadyWait[ticket.bank] = true;
        }
        transaction.destinations[index] = destination;
        state.retainedDestinations[transaction.ticket.bank][index] = destination;
        if (role == FrameGenerationRole::real) {
            // B is already finalized by NR, sharpening and upscaling. Reading
            // that retained source avoids waiting for synthesis just to fall
            // back to the real frame. It still obeys the producer's NR fence.
            if (state.capturedBgraMask[transaction.ticket.bank]) {
                const auto completed = state.hostOwnsReal && state.inputOwnerToken == ticket.token &&
                                               state.inputOwnerGeneration == ticket.generation && state.consumed11
                                           ? state.consumed11->GetCompletedValue()
                                           : UINT64_MAX;
                if (completed != UINT64_MAX && completed >= ticket.input_consumed_value) {
                    auto views = state.capturedViews[transaction.ticket.bank];
                    for (uint32_t eye = 0; eye < 2; ++eye) {
                        views[eye].rect.x = eye * state.info.eyeWidth;
                        views[eye].rect.y = 0;
                    }
                    // Only the written canonical eye rectangles are valid;
                    // coalesce them only when copyStereo proves adjacency.
                    const std::array<ComPtr<ID3D11Texture2D>, 2> sources{state.input11, state.input11};
                    copyStereo(state.context11.Get(), destination, state.info.eyeWidth, sources, views);
                } else {
                    state.bgra.copy(state.context11.Get(),
                                    destination,
                                    state.info.eyeWidth,
                                    state.capturedBgraMask[transaction.ticket.bank],
                                    state.capturedSources[transaction.ticket.bank],
                                    state.capturedViews[transaction.ticket.bank]);
                }
            } else {
                copyStereo(state.context11.Get(),
                           destination,
                           state.info.eyeWidth,
                           state.capturedSources[transaction.ticket.bank],
                           state.capturedViews[transaction.ticket.bank]);
            }
        } else {
            state.context11->CopyResource(destination, state.outputs11[transaction.ticket.bank][index].Get());
        }
        transaction.consumerValue = ++state.consumerValue;
        state.consumerValues[transaction.ticket.bank] = transaction.consumerValue;
        state.roleConsumerValues[transaction.ticket.bank][index] = transaction.consumerValue;
        hr = state.context11->Signal(state.consumer11.Get(), transaction.consumerValue);
        if (FAILED(hr)) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        state.context11->Flush(); // Queue the copy before presenting; never a CPU drain.
        return S_OK;
    }

    bool FrameGeneration::copyReady(const OwnedFrameTransaction& transaction, FrameGenerationRole role) const noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token ||
            (role != FrameGenerationRole::real && role != FrameGenerationRole::synthetic1 &&
             role != FrameGenerationRole::synthetic2 && role != FrameGenerationRole::synthetic3)) {
            return false;
        }
        const unsigned index = static_cast<unsigned>(role);
        const auto value = m_state->roleConsumerValues[transaction.ticket.bank][index];
        const auto completed = m_state->consumer11->GetCompletedValue();
        return value && completed != UINT64_MAX && completed >= value;
    }

    HRESULT FrameGeneration::waitForCopy(const OwnedFrameTransaction& transaction,
                                         FrameGenerationRole role,
                                         DWORD timeoutMs) noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token ||
            (role != FrameGenerationRole::real && role != FrameGenerationRole::synthetic1 &&
             role != FrameGenerationRole::synthetic2 && role != FrameGenerationRole::synthetic3)) {
            return VDXRFG_E_REJECTED;
        }
        const unsigned index = static_cast<unsigned>(role);
        auto& state = *m_state;
        const auto value = state.roleConsumerValues[transaction.ticket.bank][index];
        if (!value || timeoutMs == INFINITE) {
            return VDXRFG_E_REJECTED;
        }
        if (copyReady(transaction, role)) {
            return S_OK;
        }
        if (state.consumer11->GetCompletedValue() == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        auto& registered = state.registeredCopyValues[transaction.ticket.bank][index];
        if (registered != value) {
            const auto hr = state.consumer11->SetEventOnCompletion(value, state.copyCompletionEvent);
            if (FAILED(hr)) {
                return hr;
            }
            registered = value;
        }
        const auto result = WaitForSingleObject(state.copyCompletionEvent, timeoutMs);
        if (result == WAIT_FAILED) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        // A prior timed-out registration may wake this reusable event. Only
        // the authoritative current role fence decides whether it is ready.
        const auto completed = state.consumer11->GetCompletedValue();
        if (completed == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        return completed >= value ? S_OK : VDXRFG_E_BUSY;
    }

    HRESULT FrameGeneration::retire(OwnedFrameTransaction& transaction) noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        const auto completed = m_state->consumer11->GetCompletedValue();
        if (completed == UINT64_MAX) {
            m_state->poisoned = true;
            return VDXRFG_E_POISONED;
        }
        if (completed < m_state->consumerValues[transaction.ticket.bank]) {
            return VDXRFG_E_BUSY;
        }
        const auto hr = m_state->api.Retire(m_state->context, transaction.ticket.token);
        if (SUCCEEDED(hr)) {
            if (m_state->inputOwnerToken == transaction.ticket.token) {
                m_state->inputOwnerToken = m_state->inputOwnerGeneration = 0;
            }
            m_state->liveTokens[transaction.ticket.bank] = 0;
            m_state->consumerValues[transaction.ticket.bank] = 0;
            m_state->roleConsumerValues[transaction.ticket.bank] = {};
            m_state->registeredCopyValues[transaction.ticket.bank] = {};
            m_state->registeredReadyValues[transaction.ticket.bank] = 0;
            m_state->registeredFirstReadyValues[transaction.ticket.bank] = 0;
            m_state->firstReadyValues[transaction.ticket.bank] = 0;
            m_state->queuedFirstReadyWait[transaction.ticket.bank] = false;
            m_state->queuedFullReadyWait[transaction.ticket.bank] = false;
            m_state->tickets[transaction.ticket.bank] = {};
            m_state->retainedDestinations[transaction.ticket.bank] = {};
            m_state->capturedSources[transaction.ticket.bank] = {};
            m_state->capturedViews[transaction.ticket.bank] = {};
            transaction = {};
        }
        return hr;
    }

    HRESULT FrameGeneration::reset(uint32_t reason) noexcept {
        if (!m_state || m_state->poisoned) {
            return VDXRFG_E_REJECTED;
        }
        m_state->inputOwnerToken = m_state->inputOwnerGeneration = 0;
        const auto hr = m_state->api.Reset(m_state->context, reason);
        if (SUCCEEDED(hr)) {
            m_state->previousSerial = 0;
        }
        return hr;
    }

    HRESULT FrameGeneration::shutdown() noexcept {
        if (!m_state) {
            return S_OK;
        }
        if (m_state->hostCompletionUnknown) {
            return VDXRFG_E_POISONED;
        }
        const auto produced = m_state->producer11->GetCompletedValue();
        if (produced == UINT64_MAX) {
            m_state->poisoned = m_state->hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (produced < m_state->producerValue) {
            return VDXRFG_E_BUSY;
        }
        if (std::any_of(
                m_state->liveTokens.begin(), m_state->liveTokens.end(), [](uint64_t token) { return token != 0; })) {
            return VDXRFG_E_BUSY;
        }
        if (m_state->context) {
            const auto hr = m_state->api.Destroy(m_state->context);
            if (FAILED(hr)) {
                return hr;
            }
            m_state->context = nullptr;
        }
        m_state.reset();
        return S_OK;
    }

    bool FrameGeneration::initialized() const noexcept {
        return m_state && m_state->context && !m_state->poisoned;
    }

    HRESULT FrameGeneration::getStatistics(VDXRFG_StatisticsV1& statistics) const noexcept {
        if (!m_state || !m_state->context) {
            return VDXRFG_E_REJECTED;
        }
        statistics = {};
        statistics.struct_size = sizeof(statistics);
        statistics.api_version = VDXRFG_API_VERSION;
        return m_state->api.GetStatistics(m_state->context, &statistics);
    }
} // namespace virtualdesktop_openxr
