#include "vdxr_fg_api.h"
#include "xrfg/d3d12_frame_synthesizer.hpp"

#include <wrl/client.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace {
    using Microsoft::WRL::ComPtr;

    struct Lease {
        VDXRFG_TicketV1 ticket{};
        uint64_t firstReadyValue{};
    };

    struct Context {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Resource> input;
        std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> outputs;
        std::array<ComPtr<ID3D12Resource>, 2> isolatedCurrent;
        std::array<ComPtr<ID3D12CommandAllocator>, 2> copyAllocators;
        std::array<ComPtr<ID3D12GraphicsCommandList>, 2> packLists;
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        xrfg::D3D12FrameSynthesizer synthesizer;
        ComPtr<ID3D12Fence> ready;
        ComPtr<ID3D12Fence> firstReady;
        ComPtr<ID3D12Fence> consumed;
        std::array<Lease, 2> leases;
        VDXRFG_StatisticsV1 statistics{};
        uint64_t nextToken{1}, generation{1}, previousSerial{};
        uint64_t lastTimingSerial{};
        int64_t previousTime{};
        uint32_t multiplier{};
        bool poisoned{};
        bool initializationPoisoned{};
        bool hostOwnsReal{};
        bool version3{};
        bool version4{};
    };

    template <typename T>
    bool version(const T* info) {
        return info && info->struct_size >= sizeof(T) && info->api_version == VDXRFG_API_VERSION;
    }

    bool sameDevice(ID3D12Device* device, ID3D12DeviceChild* object) {
        ComPtr<ID3D12Device> owner;
        ComPtr<IUnknown> left, right;
        return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&owner))) &&
               SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&left))) && SUCCEEDED(owner.As(&right)) &&
               left.Get() == right.Get();
    }

    HRESULT createImpl(
        const VDXRFG_CreateInfoV3* info, void** result, uint32_t flags, bool version3, bool version4 = false) noexcept {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (!info || !info->device || !info->queue || !info->input || info->release_state ||
            (info->multiplier != 2 && info->multiplier != 3 && !(version3 && info->multiplier == 4)) || !info->width ||
            !info->height || (info->width & 1)) {
            return VDXRFG_E_REJECTED;
        }
        try {
            auto context = std::make_unique<Context>();
            context->device = static_cast<ID3D12Device*>(info->device);
            context->queue = static_cast<ID3D12CommandQueue*>(info->queue);
            context->input = static_cast<ID3D12Resource*>(info->input);
            context->multiplier = info->multiplier;
            context->version3 = version3;
            context->version4 = version4;
            context->hostOwnsReal = (flags & VDXRFG_CREATE_HOST_OWNS_REAL) != 0;
            auto device = context->device.Get();
            if (!sameDevice(device, context->queue.Get()) || !sameDevice(device, context->input.Get()) ||
                context->queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                return VDXRFG_E_REJECTED;
            }
            const auto description = context->input->GetDesc();
            if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.Width != info->width ||
                description.Height != info->height || description.DepthOrArraySize != 1 || description.MipLevels != 1 ||
                description.SampleDesc.Count != 1 ||
                (info->format != DXGI_FORMAT_R8G8B8A8_UNORM && info->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
                (description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                 description.Format != static_cast<DXGI_FORMAT>(info->format))) {
                return VDXRFG_E_REJECTED;
            }
            std::array<ID3D12Resource*, 2> current{};
            std::array<ID3D12Resource*, 6> synthetic{};
            uint32_t syntheticCount = 0;
            for (uint32_t bank = 0; bank < 2; ++bank) {
                for (uint32_t role = 0; role < 4; ++role) {
                    auto resource = static_cast<ID3D12Resource*>(info->outputs[bank][role]);
                    if ((role == 1 && info->multiplier == 2) || (role == 3 && info->multiplier != 4) ||
                        (role == 2 && (flags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS))) {
                        if (resource) {
                            return VDXRFG_E_REJECTED;
                        }
                        continue;
                    }
                    if (!sameDevice(device, resource) || resource == context->input.Get()) {
                        return VDXRFG_E_REJECTED;
                    }
                    const auto output = resource->GetDesc();
                    if (output.Dimension != description.Dimension || output.Width != description.Width ||
                        output.Height != description.Height || output.DepthOrArraySize != 1 || output.MipLevels != 1 ||
                        output.SampleDesc.Count != 1 || output.Format != description.Format ||
                        !(output.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
                        return VDXRFG_E_REJECTED;
                    }
                    for (const auto& priorBank : context->outputs) {
                        for (const auto& prior : priorBank) {
                            if (prior.Get() == resource) {
                                return VDXRFG_E_REJECTED;
                            }
                        }
                    }
                    context->outputs[bank][role] = resource;
                    if (role == 2) {
                        current[bank] = resource;
                    } else {
                        synthetic[syntheticCount++] = resource;
                    }
                }
            }
            auto isolatedDescription = description;
            isolatedDescription.Width /= 2;
            isolatedDescription.DepthOrArraySize = 2;
            isolatedDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            HRESULT hr = S_OK;
            for (uint32_t bank = 0; bank < 2; ++bank) {
                hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&context->copyAllocators[bank]));
                if (FAILED(hr)) {
                    return hr;
                }
                hr = device->CreateCommandList(0,
                                               D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               context->copyAllocators[bank].Get(),
                                               nullptr,
                                               IID_PPV_ARGS(&context->packLists[bank]));
                if (FAILED(hr)) {
                    return hr;
                }
                hr = context->packLists[bank]->Close();
                if (FAILED(hr)) {
                    return hr;
                }
                hr = device->CreateCommittedResource(&heap,
                                                     D3D12_HEAP_FLAG_NONE,
                                                     &isolatedDescription,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                     nullptr,
                                                     IID_PPV_ARGS(&context->isolatedCurrent[bank]));
                if (FAILED(hr)) {
                    return hr;
                }
                current[bank] = context->isolatedCurrent[bank].Get();
            }
            context->history = std::make_shared<xrfg::D3D12SwapchainHistory>();
            ID3D12Resource* input[] = {context->input.Get()};
            hr = context->history->initialize(device,
                                              context->queue.Get(),
                                              input,
                                              D3D12_RESOURCE_STATE_COMMON,
                                              nullptr,
                                              xrfg::D3D12HistorySourceLayout::stereo_atlas);
            if (FAILED(hr)) {
                return hr;
            }
            const bool fidelityFxFull = (flags & VDXRFG_CREATE_FIDELITYFX_FULL) != 0;
            xrfg::D3D12NvidiaOpticalFlowOptions opticalFlowOptions{};
            if (fidelityFxFull)
                opticalFlowOptions.input_scale = xrfg::D3D12OpticalFlowInputScale::full;
            hr = context->synthesizer.initialize(device,
                                                 context->queue.Get(),
                                                 context->history,
                                                 current,
                                                 std::span<ID3D12Resource* const>(synthetic.data(), syntheticCount),
                                                 static_cast<DXGI_FORMAT>(info->format),
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 fidelityFxFull ? xrfg::D3D12OpticalFlowBackend::fidelity_fx
                                                                : xrfg::D3D12OpticalFlowBackend::nvidia,
                                                 opticalFlowOptions,
                                                 (flags & VDXRFG_CREATE_GPU_TIMING) != 0,
                                                 !context->hostOwnsReal,
                                                 xrfg::D3D12SyntheticOutputLayout::shared_stereo_atlas,
                                                 true,
                                                 version4 && info->multiplier > 2);
            if (FAILED(hr)) {
                if (hr == HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE)) {
                    context->initializationPoisoned = context->poisoned = true;
                    *result = context.release();
                    return VDXRFG_E_POISONED;
                }
                return hr;
            }
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&context->ready));
            if (FAILED(hr)) {
                return hr;
            }
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&context->consumed));
            if (FAILED(hr)) {
                return hr;
            }
            if (version4 && info->multiplier > 2) {
                hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&context->firstReady));
                if (FAILED(hr))
                    return hr;
            }
            *result = context.release();
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT createLegacy(const VDXRFG_CreateInfoV1* info, void** result, uint32_t flags) noexcept {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (!version(info) || info->reserved[0] || info->reserved[1] || info->reserved[2])
            return VDXRFG_E_REJECTED;
        VDXRFG_CreateInfoV3 normalized{};
        normalized.device = info->device;
        normalized.queue = info->queue;
        normalized.input = info->input;
        normalized.width = info->width;
        normalized.height = info->height;
        normalized.format = info->format;
        normalized.multiplier = info->multiplier;
        normalized.release_state = info->release_state;
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned role = 0; role < 3; ++role)
                normalized.outputs[bank][role] = info->outputs[bank][role];
        return createImpl(&normalized, result, flags, false);
    }

    HRESULT WINAPI create(const VDXRFG_CreateInfoV1* info, void** result) noexcept {
        return createLegacy(info, result, VDXRFG_CREATE_GPU_TIMING);
    }

    HRESULT WINAPI createV2(const VDXRFG_CreateInfoV2* info, void** result) noexcept {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (!info || info->struct_size < sizeof(*info) || info->api_version != VDXRFG_API_VERSION_2 || info->reserved ||
            (info->flags & ~(VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL)))
            return VDXRFG_E_REJECTED;
        return createLegacy(&info->base, result, info->flags);
    }

    HRESULT WINAPI createV3(const VDXRFG_CreateInfoV3* info, void** result) noexcept {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (!info || info->struct_size < sizeof(*info) || info->api_version != VDXRFG_API_VERSION_3 ||
            (info->flags & ~(VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL |
                             VDXRFG_CREATE_OMIT_REAL_OUTPUTS)) ||
            ((info->flags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS) && !(info->flags & VDXRFG_CREATE_HOST_OWNS_REAL)))
            return VDXRFG_E_REJECTED;
        return createImpl(info, result, info->flags, true);
    }

    HRESULT WINAPI createV4(const VDXRFG_CreateInfoV3* info, void** result) noexcept {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (!info || info->struct_size < sizeof(*info) || info->api_version != VDXRFG_API_VERSION_3 ||
            (info->flags & ~(VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL |
                             VDXRFG_CREATE_OMIT_REAL_OUTPUTS)) ||
            ((info->flags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS) && !(info->flags & VDXRFG_CREATE_HOST_OWNS_REAL)))
            return VDXRFG_E_REJECTED;
        return createImpl(info, result, info->flags, true, true);
    }

    bool viewsValid(const Context& context, const VDXRFG_SubmitInfoV3& info) {
        const auto description = context.input->GetDesc();
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& view = info.views[eye];
            const auto& rect = view.rect;
            if (!rect.width || !rect.height || rect.x >= description.Width || rect.y >= description.Height ||
                rect.width > description.Width - rect.x || rect.height > description.Height - rect.y ||
                rect.x < eye * description.Width / 2 || rect.x + rect.width > (eye + 1) * description.Width / 2) {
                return false;
            }
            float norm = 0;
            for (float component : view.orientation) {
                if (!std::isfinite(component)) {
                    return false;
                }
                norm += component * component;
            }
            if (norm < 0.99f || norm > 1.01f) {
                return false;
            }
            for (float component : view.position) {
                if (!std::isfinite(component)) {
                    return false;
                }
            }
            for (float angle : view.fov) {
                if (!std::isfinite(angle) || angle <= -1.56f || angle >= 1.56f) {
                    return false;
                }
            }
            if (view.fov[0] >= view.fov[1] || view.fov[3] >= view.fov[2]) {
                return false;
            }
        }
        const auto& a = info.views[0].rect;
        const auto& b = info.views[1].rect;
        return a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y || b.y + b.height <= a.y;
    }

    D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                      D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        return barrier;
    }

    void copyStereo(ID3D12GraphicsCommandList* list, ID3D12Resource* atlas, ID3D12Resource* isolated) {
        const uint32_t eyeWidth = static_cast<uint32_t>(atlas->GetDesc().Width / 2);
        const uint32_t height = atlas->GetDesc().Height;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION wide{};
            wide.pResource = atlas;
            wide.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION mono{};
            mono.pResource = isolated;
            mono.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            mono.SubresourceIndex = eye;
            const D3D12_BOX source{0, 0, 0, eyeWidth, height, 1};
            list->CopyTextureRegion(&wide, eye * eyeWidth, 0, 0, &mono, &source);
        }
    }

    HRESULT submitImpl(void* handle, const VDXRFG_SubmitInfoV3* info, VDXRFG_TicketV1* output) noexcept {
        if (!output) {
            return E_POINTER;
        }
        *output = {};
        auto context = static_cast<Context*>(handle);
        if (!context || !info || info->input != context->input.Get() || !info->serial ||
            info->serial <= context->previousSerial || info->display_time_ns <= 0 || info->display_period_ns <= 0 ||
            !viewsValid(*context, *info) || (info->producer_value && !info->producer_fence) ||
            (info->producer_fence &&
             !sameDevice(context->device.Get(), static_cast<ID3D12Fence*>(info->producer_fence)))) {
            return VDXRFG_E_REJECTED;
        }
        if (context->poisoned) {
            return VDXRFG_E_POISONED;
        }
        if (context->previousSerial) {
            if (info->display_time_ns <= context->previousTime) {
                return VDXRFG_E_REJECTED;
            }
            const auto interval = info->display_time_ns - context->previousTime;
            for (uint32_t i = 0; i < context->multiplier - 1; ++i) {
                const float fraction = info->fractions[i];
                if (!std::isfinite(fraction) || fraction <= 0.05f || fraction >= 0.95f ||
                    info->target_time_ns[i] <= context->previousTime ||
                    info->target_time_ns[i] >= info->display_time_ns ||
                    std::abs(static_cast<double>(info->target_time_ns[i] - context->previousTime) / interval -
                             fraction) > 0.00001 ||
                    (i &&
                     (info->target_time_ns[i] <= info->target_time_ns[i - 1] || fraction <= info->fractions[i - 1]))) {
                    return VDXRFG_E_REJECTED;
                }
            }
        }
        Lease* lease = nullptr;
        uint32_t bank = 0;
        for (; bank < 2; ++bank) {
            if (!context->leases[bank].ticket.token) {
                lease = &context->leases[bank];
                break;
            }
        }
        const HRESULT previousStatus = context->synthesizer.wait_for_previous_submission(0);
        if (!lease || previousStatus == HRESULT_FROM_WIN32(ERROR_BUSY)) {
            ++context->statistics.busy_rejections;
            return VDXRFG_E_BUSY;
        }
        if (FAILED(previousStatus)) {
            context->poisoned = true;
            return VDXRFG_E_POISONED;
        }
        if (context->nextToken == std::numeric_limits<uint64_t>::max()) {
            return VDXRFG_E_REJECTED;
        }
        // Ownership identity precedes the first GPU operation, even a queue Wait.
        lease->ticket.token = context->nextToken++;
        lease->ticket.generation = context->generation;
        lease->ticket.current_serial = info->serial;
        lease->ticket.previous_serial = context->previousSerial;
        lease->ticket.bank = bank;
        const auto poison = [&]() {
            context->poisoned = true;
            *output = lease->ticket;
            return VDXRFG_E_POISONED;
        };
        try {
            HRESULT hr = S_OK;
            if (info->producer_fence) {
                auto fence = static_cast<ID3D12Fence*>(info->producer_fence);
                hr = context->queue->Wait(fence, info->producer_value);
                if (FAILED(hr)) {
                    return poison();
                }
            }
            // Capture owns the selected history slot and copies both eyes
            // directly from the atlas before publishing input consumption.
            xrfg::D3D12HistoryCaptureTicket capture{};
            hr = context->history->capture(0, &capture);
            if (FAILED(hr)) {
                return poison();
            }
            hr = context->queue->Signal(context->consumed.Get(), lease->ticket.token);
            if (FAILED(hr)) {
                return poison();
            }
            lease->ticket.input_consumed_fence = context->consumed.Get();
            lease->ticket.input_consumed_value = lease->ticket.token;
            hr = context->history->commit(capture);
            if (FAILED(hr)) {
                return poison();
            }
            std::array<xrfg::D3D12ReprojectionView, 2> views{};
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const auto& input = info->views[eye];
                auto& view = views[eye];
                view.pose.position = {input.position[0], input.position[1], input.position[2]};
                view.pose.orientation = {
                    input.orientation[0], input.orientation[1], input.orientation[2], input.orientation[3]};
                view.fov = {input.fov[0], input.fov[1], input.fov[2], input.fov[3]};
                view.image_rect = {input.rect.x - eye * static_cast<uint32_t>(context->input->GetDesc().Width / 2),
                                   input.rect.y,
                                   input.rect.width,
                                   input.rect.height};
                view.array_slice = eye;
            }
            xrfg::D3D12FrameSynthesisTicket synthesis{};
            if (!context->previousSerial) {
                hr = context->synthesizer.submit_prime(capture, views, bank, &synthesis);
                lease->ticket.prime = 1;
            } else {
                const uint32_t index = bank * (context->multiplier - 1);
                std::array<xrfg::D3D12ExtraSynthetic, 2> extra{};
                const uint32_t extraCount = context->multiplier - 2;
                for (uint32_t i = 0; i < extraCount; ++i)
                    extra[i] = xrfg::D3D12ExtraSynthetic{index + i + 1, info->fractions[i + 1]};
                hr = context->synthesizer.submit_pair_outputs(
                    capture,
                    views,
                    views,
                    index,
                    bank,
                    &synthesis,
                    std::nullopt,
                    {},
                    false,
                    info->fractions[0],
                    std::span<const xrfg::D3D12ExtraSynthetic>(extra.data(), extraCount),
                    context->firstReady.Get(),
                    context->firstReady ? lease->ticket.token : 0);
            }
            if (FAILED(hr)) {
                return poison();
            }
            lease->firstReadyValue = synthesis.first_output_signal_value;
            std::array<D3D12_RESOURCE_BARRIER, 6> packBarriers{};
            UINT packBarrierCount = 0;
            for (uint32_t role = 0; role < 4; ++role) {
                if (role != 2 || context->hostOwnsReal) {
                    continue;
                }
                packBarriers[packBarrierCount++] = transition(
                    context->outputs[bank][role].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                packBarriers[packBarrierCount++] = transition(context->isolatedCurrent[bank].Get(),
                                                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                              D3D12_RESOURCE_STATE_COPY_SOURCE);
            }
            if (packBarrierCount) {
                hr = context->copyAllocators[bank]->Reset();
                if (FAILED(hr)) {
                    return poison();
                }
                hr = context->packLists[bank]->Reset(context->copyAllocators[bank].Get(), nullptr);
                if (FAILED(hr)) {
                    return poison();
                }
                context->packLists[bank]->ResourceBarrier(packBarrierCount, packBarriers.data());
                // Each selected role has distinct outer and isolated resources.
                // Keep all transitions while batching their driver calls.
                for (UINT i = 0; i < packBarrierCount; i += 2)
                    copyStereo(context->packLists[bank].Get(),
                               packBarriers[i].Transition.pResource,
                               packBarriers[i + 1].Transition.pResource);
                for (UINT i = 0; i < packBarrierCount; ++i)
                    std::swap(packBarriers[i].Transition.StateBefore, packBarriers[i].Transition.StateAfter);
                context->packLists[bank]->ResourceBarrier(packBarrierCount, packBarriers.data());
                hr = context->packLists[bank]->Close();
                if (FAILED(hr)) {
                    return poison();
                }
                ID3D12CommandList* pack[] = {context->packLists[bank].Get()};
                context->queue->ExecuteCommandLists(1, pack);
            }
            // A host-owned prime has no outer output to pack. Its backend
            // priming work still precedes this ready signal on the same queue.
            hr = context->queue->Signal(context->ready.Get(), lease->ticket.token);
            if (FAILED(hr)) {
                return poison();
            }
            lease->ticket.output_ready_fence = context->ready.Get();
            lease->ticket.output_ready_value = lease->ticket.token;
            lease->ticket.role_mask = (context->hostOwnsReal ? 0u : 4u) | (lease->ticket.prime        ? 0u
                                                                           : context->multiplier == 4 ? 11u
                                                                           : context->multiplier == 3 ? 3u
                                                                                                      : 1u);
            context->previousSerial = info->serial;
            context->previousTime = info->display_time_ns;
            ++context->statistics.captures;
            if (!lease->ticket.prime) {
                ++context->statistics.pairs;
                context->statistics.synthetic_frames += context->multiplier - 1;
            }
            *output = lease->ticket;
            return S_OK;
        } catch (...) {
            return poison();
        }
    }

    HRESULT WINAPI submitV3(void* handle, const VDXRFG_SubmitInfoV3* info, VDXRFG_TicketV1* output) noexcept {
        if (!output)
            return E_POINTER;
        *output = {};
        const auto context = static_cast<Context*>(handle);
        if (!context || !context->version3 || !info || info->struct_size < sizeof(*info) ||
            info->api_version != VDXRFG_API_VERSION_3)
            return VDXRFG_E_REJECTED;
        return submitImpl(handle, info, output);
    }

    HRESULT WINAPI submit(void* handle, const VDXRFG_SubmitInfoV1* info, VDXRFG_TicketV1* output) noexcept {
        if (!output)
            return E_POINTER;
        *output = {};
        const auto context = static_cast<Context*>(handle);
        if (!context || context->version3 || !version(info))
            return VDXRFG_E_REJECTED;
        VDXRFG_SubmitInfoV3 normalized{};
        normalized.input = info->input;
        normalized.producer_fence = info->producer_fence;
        normalized.producer_value = info->producer_value;
        normalized.serial = info->serial;
        normalized.display_time_ns = info->display_time_ns;
        normalized.display_period_ns = info->display_period_ns;
        for (unsigned i = 0; i < 3; ++i)
            normalized.target_time_ns[i] = info->target_time_ns[i];
        for (unsigned i = 0; i < 2; ++i) {
            normalized.fractions[i] = info->fractions[i];
            normalized.views[i] = info->views[i];
        }
        return submitImpl(handle, &normalized, output);
    }

    HRESULT WINAPI outputReadiness(void* handle,
                                   uint64_t token,
                                   uint32_t role,
                                   VDXRFG_OutputReadinessV4* output) noexcept {
        if (!output)
            return E_POINTER;
        if (output->struct_size < sizeof(*output) || output->api_version != VDXRFG_API_VERSION_4)
            return VDXRFG_E_REJECTED;
        output->fence = nullptr;
        output->value = 0;
        const auto context = static_cast<Context*>(handle);
        if (!context || !context->version4 || !token || role >= 4)
            return VDXRFG_E_REJECTED;
        if (context->poisoned)
            return VDXRFG_E_POISONED;
        for (const auto& lease : context->leases) {
            if (lease.ticket.token != token)
                continue;
            if (!(lease.ticket.role_mask & (1u << role)))
                return VDXRFG_E_REJECTED;
            const bool first = role == 0 && lease.firstReadyValue != 0;
            output->fence = first ? context->firstReady.Get() : context->ready.Get();
            output->value = first ? lease.firstReadyValue : lease.ticket.output_ready_value;
            return S_OK;
        }
        return VDXRFG_E_REJECTED;
    }

    HRESULT WINAPI retire(void* handle, uint64_t token) noexcept {
        auto context = static_cast<Context*>(handle);
        if (!context || !token) {
            return VDXRFG_E_REJECTED;
        }
        if (context->poisoned) {
            return VDXRFG_E_POISONED;
        }
        for (auto& lease : context->leases) {
            if (lease.ticket.token == token) {
                const uint64_t completed = context->ready->GetCompletedValue();
                if (completed == UINT64_MAX) {
                    context->poisoned = true;
                    return VDXRFG_E_POISONED;
                }
                if (completed < lease.ticket.output_ready_value) {
                    return VDXRFG_E_BUSY;
                }
                lease = {};
                return S_OK;
            }
        }
        return VDXRFG_E_REJECTED;
    }

    HRESULT WINAPI reset(void* handle, uint32_t) noexcept {
        auto context = static_cast<Context*>(handle);
        if (!context) {
            return VDXRFG_E_REJECTED;
        }
        if (context->poisoned) {
            return VDXRFG_E_POISONED;
        }
        for (const auto& lease : context->leases) {
            if (lease.ticket.token) {
                return VDXRFG_E_BUSY;
            }
        }
        HRESULT hr = context->synthesizer.retire_previous();
        if (SUCCEEDED(hr)) {
            hr = context->history->invalidate();
        }
        if (FAILED(hr)) {
            context->poisoned = true;
            return VDXRFG_E_POISONED;
        }
        ++context->generation;
        // Consume old-generation timing before capture serials restart.
        xrfg::D3D12NvidiaGpuTiming discarded{};
        // Every work slot can hold one cached and one pending sample. A
        // failed sample is discarded by the core; keep draining the others.
        for (uint32_t i = 0; i < 2 * xrfg::D3D12SwapchainHistory::kSlotCount; ++i) {
            if (context->synthesizer.consume_nvidia_gpu_timing(&discarded) == S_FALSE) {
                break;
            }
        }
        context->lastTimingSerial = 0;
        context->statistics.last_gpu_us = context->statistics.pack_gpu_us = context->statistics.flow_gpu_us =
            context->statistics.composition_gpu_us = 0;
        context->previousSerial = 0;
        context->previousTime = 0;
        return S_OK;
    }

    HRESULT WINAPI statistics(void* handle, VDXRFG_StatisticsV1* output) noexcept {
        auto context = static_cast<Context*>(handle);
        if (!context || !version(output)) {
            return VDXRFG_E_REJECTED;
        }
        xrfg::D3D12NvidiaGpuTiming timing{};
        for (uint32_t i = 0; i < 2 * xrfg::D3D12SwapchainHistory::kSlotCount; ++i) {
            const auto result = context->synthesizer.consume_nvidia_gpu_timing(&timing);
            if (result == S_FALSE)
                break;
            if (result != S_OK)
                continue;
            if (timing.previous_serial && timing.current_serial > context->lastTimingSerial) {
                context->lastTimingSerial = timing.current_serial;
                context->statistics.last_gpu_us = timing.total_microseconds;
                context->statistics.pack_gpu_us = timing.pack_microseconds;
                context->statistics.flow_gpu_us = timing.eye0_microseconds + timing.eye1_microseconds;
                context->statistics.composition_gpu_us = timing.composition_microseconds;
            }
        }
        *output = context->statistics;
        output->struct_size = sizeof(*output);
        output->api_version = VDXRFG_API_VERSION;
        output->poisoned = context->poisoned;
        for (const auto& lease : context->leases) {
            output->live_tickets += lease.ticket.token != 0;
        }
        return S_OK;
    }

    HRESULT WINAPI destroy(void* handle) noexcept {
        auto context = static_cast<Context*>(handle);
        if (context && context->initializationPoisoned) {
            if (FAILED(context->synthesizer.wait_for_idle())) {
                return VDXRFG_E_POISONED;
            }
            delete context;
            return S_OK;
        }
        const HRESULT hr = reset(handle, 0);
        if (FAILED(hr)) {
            return hr;
        }
        delete static_cast<Context*>(handle);
        return S_OK;
    }
} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV1(VDXRFG_ApiV1* output) noexcept {
    if (!version(output)) {
        return VDXRFG_E_REJECTED;
    }
    const VDXRFG_ApiV1 api{
        sizeof(VDXRFG_ApiV1), VDXRFG_API_VERSION, create, submit, retire, reset, statistics, destroy};
    *output = api;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV2(VDXRFG_ApiV2* output) noexcept {
    if (!output || output->struct_size < sizeof(*output) || output->api_version != VDXRFG_API_VERSION_2) {
        return VDXRFG_E_REJECTED;
    }
    const VDXRFG_ApiV2 api{sizeof(VDXRFG_ApiV2),
                           VDXRFG_API_VERSION_2,
                           VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL,
                           0,
                           createV2,
                           submit,
                           retire,
                           reset,
                           statistics,
                           destroy};
    *output = api;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV3(VDXRFG_ApiV3* output) noexcept {
    if (!output || output->struct_size < sizeof(*output) || output->api_version != VDXRFG_API_VERSION_3)
        return VDXRFG_E_REJECTED;
    const VDXRFG_ApiV3 api{sizeof(VDXRFG_ApiV3),
                           VDXRFG_API_VERSION_3,
                           VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL |
                               VDXRFG_CREATE_OMIT_REAL_OUTPUTS,
                           4,
                           createV3,
                           submitV3,
                           retire,
                           reset,
                           statistics,
                           destroy};
    *output = api;
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV4(VDXRFG_ApiV4* output) noexcept {
    if (!output || output->struct_size < sizeof(*output) || output->api_version != VDXRFG_API_VERSION_4)
        return VDXRFG_E_REJECTED;
    const VDXRFG_ApiV4 api{sizeof(VDXRFG_ApiV4),
                           VDXRFG_API_VERSION_4,
                           VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL |
                               VDXRFG_CREATE_OMIT_REAL_OUTPUTS,
                           4,
                           createV4,
                           submitV3,
                           retire,
                           reset,
                           statistics,
                           destroy,
                           outputReadiness};
    *output = api;
    return S_OK;
}
