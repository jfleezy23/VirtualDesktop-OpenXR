#include "frame_generation.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace virtualdesktop_openxr {
    using Microsoft::WRL::ComPtr;

    struct FrameGeneration::State {
        HMODULE module{};
        VDXRFG_ApiV1 api{};
        void* context{};
        InitInfo info;
        ComPtr<ID3D11Device5> device11;
        ComPtr<ID3D11DeviceContext4> context11;
        ComPtr<ID3D12Device> device12;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D11Texture2D> input11;
        ComPtr<ID3D12Resource> input12;
        std::array<std::array<ComPtr<ID3D11Texture2D>, 3>, 2> outputs11;
        std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs12;
        ComPtr<ID3D11Fence> producer11, consumed11, ready11, consumer11;
        ComPtr<ID3D12Fence> producer12;
        std::array<uint64_t, 2> liveTokens{};
        std::array<uint64_t, 2> consumerValues{};
        std::array<std::array<uint64_t, 3>, 2> roleConsumerValues{};
        std::array<std::array<uint64_t, 3>, 2> registeredCopyValues{};
        std::array<VDXRFG_TicketV1, 2> tickets{};
        std::array<std::array<ComPtr<ID3D11Texture2D>, 3>, 2> retainedDestinations;
        std::array<std::array<ComPtr<ID3D11Texture2D>, 2>, 2> capturedSources;
        std::array<std::array<VDXRFG_ViewV1, 2>, 2> capturedViews{};
        std::array<ComPtr<ID3D11Texture2D>, 2> producerSources;
        uint64_t generation{};
        uint64_t producerValue{}, consumerValue{}, consumedValue{}, previousSerial{};
        bool poisoned{}, hostCompletionUnknown{};
        bool hostOwnsReal{};
        HANDLE copyCompletionEvent{};
        HANDLE readyCompletionEvent{};
        std::array<uint64_t, 2> registeredReadyValues{};

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
            (info.multiplier != 2 && info.multiplier != 3) ||
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
            // Backend choice is required, unlike optional diagnostics/real
            // ownership. An old module must not silently substitute NVIDIA.
            if (info.useFidelityFxFull &&
                (!getApi2 || !(api2.supported_create_flags & VDXRFG_CREATE_FIDELITYFX_FULL))) {
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
            VDXRFG_CreateInfoV1 create{};
            create.struct_size = sizeof(create);
            create.api_version = VDXRFG_API_VERSION;
            create.device = info.device12;
            create.queue = state->queue.Get();
            create.input = state->input12.Get();
            create.width = texture.Width;
            create.height = texture.Height;
            create.format = info.format;
            create.multiplier = info.multiplier;
            for (uint32_t bank = 0; bank < 2; ++bank) {
                for (uint32_t role = 0; role < 3; ++role) {
                    if (role == 1 && info.multiplier == 2) {
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
            if (getApi2) {
                VDXRFG_CreateInfoV2 options{};
                options.struct_size = sizeof(options);
                options.api_version = VDXRFG_API_VERSION_2;
                options.base = create;
                const uint32_t requested =
                    VDXRFG_CREATE_HOST_OWNS_REAL | (info.enableGpuTiming ? VDXRFG_CREATE_GPU_TIMING : 0u);
                options.flags = api2.supported_create_flags & requested;
                if (info.useFidelityFxFull)
                    options.flags |= VDXRFG_CREATE_FIDELITYFX_FULL;
                state->hostOwnsReal = (options.flags & VDXRFG_CREATE_HOST_OWNS_REAL) != 0;
                // V2 gameplay defaults to no GPU timing; diagnostic callers opt
                // in before creation. V1-only modules retain their defaults.
                hr = api2.Create(&options, &state->context);
            } else {
                hr = state->api.Create(&create, &state->context);
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
        if (!m_state || m_state->poisoned || transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        auto& state = *m_state;
        if (state.producerValue == UINT64_MAX || input.slots.count == 0 || input.slots.count > 3 ||
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
        VDXRFG_SubmitInfoV1 submit{};
        submit.struct_size = sizeof(submit);
        submit.api_version = VDXRFG_API_VERSION;
        submit.input = state.input12.Get();
        submit.producer_fence = state.producer12.Get();
        submit.serial = input.serial;
        submit.display_time_ns = input.time;
        submit.display_period_ns = input.period;
        D3D11_TEXTURE2D_DESC description{};
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
            if (description.SampleDesc.Count != 1 || !description.ArraySize || !rect.width || !rect.height ||
                rect.width != state.info.eyeWidth || rect.height != state.info.height ||
                uint64_t(rect.x) + rect.width > description.Width ||
                uint64_t(rect.y) + rect.height > description.Height ||
                (description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && description.Format != state.info.format &&
                 description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)) {
                return VDXRFG_E_REJECTED;
            }
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
                if (input.slots.roles[slot] !=
                        (slot == 0 ? FrameGenerationRole::synthetic1 : FrameGenerationRole::synthetic2) ||
                    !std::isfinite(input.slots.fractions[slot]) || input.slots.fractions[slot] <= 0 ||
                    input.slots.fractions[slot] >= 1 || input.slots.targets[slot] >= input.slots.targets[slot + 1]) {
                    return VDXRFG_E_REJECTED;
                }
                submit.fractions[slot] = input.slots.fractions[slot];
            }
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
        copyStereo(state.context11.Get(), state.input11.Get(), state.info.eyeWidth, input.colour, input.views);
        submit.producer_value = ++state.producerValue;
        HRESULT hr = state.context11->Signal(state.producer11.Get(), submit.producer_value);
        if (FAILED(hr)) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        state.context11->Flush(); // Dispatch the cross-API producer dependency; does not drain the GPU.
        hr = state.api.Submit(state.context, &submit, &transaction.ticket);
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
        }
        if (FAILED(hr)) {
            state.poisoned = hr == VDXRFG_E_POISONED;
            return hr;
        }
        const bool prime = state.previousSerial == 0;
        const uint32_t expectedMask = (state.hostOwnsReal ? 0u : 4u) | (prime                        ? 0u
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
                                                    HANDLE& completionEvent) noexcept {
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
        if (!state.ready11 || !ticket.output_ready_fence || !ticket.output_ready_value) {
            return VDXRFG_E_REJECTED;
        }
        const auto completed = state.ready11->GetCompletedValue();
        if (completed == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (completed >= ticket.output_ready_value) {
            return S_OK;
        }
        auto& registered = state.registeredReadyValues[transaction.ticket.bank];
        if (registered != ticket.output_ready_value) {
            const auto hr = state.ready11->SetEventOnCompletion(ticket.output_ready_value, state.readyCompletionEvent);
            if (FAILED(hr)) {
                return hr;
            }
            registered = ticket.output_ready_value;
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
            role != FrameGenerationRole::synthetic2) {
            return VDXRFG_E_REJECTED;
        }
        const uint32_t index =
            role == FrameGenerationRole::real ? 2 : (role == FrameGenerationRole::synthetic1 ? 0 : 1);
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
        if (role != FrameGenerationRole::real) {
            hr = state.context11->Wait(state.ready11.Get(), ticket.output_ready_value);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        transaction.destinations[index] = destination;
        state.retainedDestinations[transaction.ticket.bank][index] = destination;
        if (role == FrameGenerationRole::real) {
            // B is already finalized by NR, sharpening and upscaling. Reading
            // that retained source avoids waiting for synthesis just to fall
            // back to the real frame. It still obeys the producer's NR fence.
            copyStereo(state.context11.Get(),
                       destination,
                       state.info.eyeWidth,
                       state.capturedSources[transaction.ticket.bank],
                       state.capturedViews[transaction.ticket.bank]);
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
             role != FrameGenerationRole::synthetic2)) {
            return false;
        }
        const unsigned index =
            role == FrameGenerationRole::real ? 2 : (role == FrameGenerationRole::synthetic1 ? 0 : 1);
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
             role != FrameGenerationRole::synthetic2)) {
            return VDXRFG_E_REJECTED;
        }
        const unsigned index =
            role == FrameGenerationRole::real ? 2 : (role == FrameGenerationRole::synthetic1 ? 0 : 1);
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
            m_state->liveTokens[transaction.ticket.bank] = 0;
            m_state->consumerValues[transaction.ticket.bank] = 0;
            m_state->roleConsumerValues[transaction.ticket.bank] = {};
            m_state->registeredCopyValues[transaction.ticket.bank] = {};
            m_state->registeredReadyValues[transaction.ticket.bank] = 0;
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
