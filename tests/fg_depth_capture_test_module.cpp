// Test-only transport observer. No optical flow or synthetic image production.
#include "vdxr_fg_api.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <new>

using Microsoft::WRL::ComPtr;
namespace {
    struct Observer {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Resource> input[2], snapshot[2];
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        VDXRFG_SubmitInfoV5 last{};
        uint64_t next{}, live{};
        bool poisoned{};
    };
    Observer* observer{};
    HRESULT WINAPI legacyCreate(const VDXRFG_CreateInfoV1*, void** out) noexcept {
        if (out)
            *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT WINAPI legacySubmit(void*, const VDXRFG_SubmitInfoV1*, VDXRFG_TicketV1* out) noexcept {
        if (out)
            *out = {};
        return E_NOTIMPL;
    }
    HRESULT WINAPI create(const VDXRFG_CreateInfoV5* info, void** out) noexcept {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (!info || observer || !info->base.device || !info->base.queue || !info->base.input || !info->guide_input ||
            info->struct_size != sizeof(*info) || info->api_version != 5 ||
            !(info->base.flags & VDXRFG_CREATE_HOST_OWNS_REAL))
            return E_INVALIDARG;
        auto* state = new (std::nothrow) Observer;
        if (!state)
            return E_OUTOFMEMORY;
        state->device = static_cast<ID3D12Device*>(info->base.device);
        state->queue = static_cast<ID3D12CommandQueue*>(info->base.queue);
        state->input[0] = static_cast<ID3D12Resource*>(info->guide_input);
        state->input[1] = static_cast<ID3D12Resource*>(info->base.input);
        HRESULT hr = state->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state->fence));
        if (SUCCEEDED(hr))
            hr = state->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&state->allocator));
        if (SUCCEEDED(hr))
            hr = state->device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, state->allocator.Get(), nullptr, IID_PPV_ARGS(&state->commands));
        if (SUCCEEDED(hr))
            hr = state->commands->Close();
        for (unsigned index = 0; index < 2 && SUCCEEDED(hr); ++index) {
            auto desc = state->input[index]->GetDesc();
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            hr = state->device->CreateCommittedResource(&heap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_COMMON,
                                                        nullptr,
                                                        IID_PPV_ARGS(&state->snapshot[index]));
        }
        if (FAILED(hr)) {
            delete state;
            return hr;
        }
        observer = state;
        *out = state;
        return S_OK;
    }
    HRESULT WINAPI submit(void* context, const VDXRFG_SubmitInfoV5* info, VDXRFG_TicketV1* out) noexcept {
        if (!out)
            return E_POINTER;
        *out = {};
        auto* state = static_cast<Observer*>(context);
        if (!state || !info || state->poisoned || state->live || !info->base.producer_fence ||
            info->struct_size != sizeof(*info) || info->api_version != 5 || info->base.input != state->input[1].Get() ||
            (info->guide_mask != 0 && info->guide_mask != 3))
            return VDXRFG_E_REJECTED;
        HRESULT hr = state->allocator->Reset();
        if (SUCCEEDED(hr))
            hr = state->commands->Reset(state->allocator.Get(), nullptr);
        if (FAILED(hr))
            return hr;
        for (unsigned index = 0; index < 2; ++index) {
            if (index == 0 && !info->guide_mask)
                continue;
            D3D12_RESOURCE_BARRIER barriers[2]{};
            for (unsigned resource = 0; resource < 2; ++resource) {
                auto& barrier = barriers[resource];
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = resource ? state->snapshot[index].Get() : state->input[index].Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter =
                    resource ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            state->commands->ResourceBarrier(2, barriers);
            state->commands->CopyResource(state->snapshot[index].Get(), state->input[index].Get());
            for (auto& barrier : barriers) {
                auto before = barrier.Transition.StateBefore;
                barrier.Transition.StateBefore = barrier.Transition.StateAfter;
                barrier.Transition.StateAfter = before;
            }
            state->commands->ResourceBarrier(2, barriers);
        }
        hr = state->commands->Close();
        if (FAILED(hr))
            return hr;
        hr = state->queue->Wait(static_cast<ID3D12Fence*>(info->base.producer_fence), info->base.producer_value);
        if (FAILED(hr))
            return hr;
        ID3D12CommandList* lists[]{state->commands.Get()};
        state->queue->ExecuteCommandLists(1, lists);
        state->last = *info;
        state->live = ++state->next;
        *out = {state->live,
                1,
                0,
                info->base.serial,
                state->fence.Get(),
                state->fence.Get(),
                state->live,
                state->live,
                0,
                0,
                1,
                0};
        hr = state->queue->Signal(state->fence.Get(), state->live);
        if (FAILED(hr)) {
            state->poisoned = true;
            return VDXRFG_E_POISONED;
        }
        return S_OK;
    }
    HRESULT WINAPI retire(void* context, uint64_t token) noexcept {
        auto* state = static_cast<Observer*>(context);
        if (!state || token != state->live || state->poisoned)
            return VDXRFG_E_REJECTED;
        const auto done = state->fence->GetCompletedValue();
        if (done == UINT64_MAX)
            return VDXRFG_E_POISONED;
        if (done < token)
            return VDXRFG_E_BUSY;
        state->live = 0;
        return S_OK;
    }
    HRESULT WINAPI reset(void* context, uint32_t) noexcept {
        auto* state = static_cast<Observer*>(context);
        return state && !state->live ? S_OK : VDXRFG_E_BUSY;
    }
    HRESULT WINAPI stats(void*, VDXRFG_StatisticsV1*) noexcept {
        return S_OK;
    }
    HRESULT WINAPI destroy(void* context) noexcept {
        auto* state = static_cast<Observer*>(context);
        if (!state || state->live || state->poisoned)
            return VDXRFG_E_BUSY;
        observer = nullptr;
        delete state;
        return S_OK;
    }
    HRESULT WINAPI readiness(void*, uint64_t, uint32_t, VDXRFG_OutputReadinessV4*) noexcept {
        return VDXRFG_E_REJECTED; // No synthetic roles exist in this observer.
    }
} // namespace
extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV1(VDXRFG_ApiV1* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = {sizeof(*out), 1, legacyCreate, legacySubmit, retire, reset, stats, destroy};
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV5(VDXRFG_ApiV5* out) noexcept {
    if (!out)
        return E_POINTER;
    *out = {sizeof(*out),
            5,
            VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_OMIT_REAL_OUTPUTS,
            4,
            VDXRFG_DEPTH_CANONICAL_TRANSPORT | VDXRFG_DEPTH_ENDPOINT_PREFERENCE,
            0,
            create,
            submit,
            retire,
            reset,
            stats,
            destroy,
            readiness};
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI FG_DEPTH_TEST_GetSnapshot(unsigned index, ID3D12Resource** out) {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!observer || !observer->live || index > 1 || (!index && !observer->last.guide_mask))
        return VDXRFG_E_REJECTED;
    return observer->snapshot[index].CopyTo(out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI FG_DEPTH_TEST_LastSubmit(VDXRFG_SubmitInfoV5* out) {
    if (!out || !observer || !observer->live)
        return VDXRFG_E_REJECTED;
    *out = observer->last;
    return S_OK;
}
