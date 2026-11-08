#include "pair_capture_nr_fg.hpp"
#include "pair_capture_nr_private.hpp"
#include "pair_capture_nr_budget.hpp"
#include "vdxr_fg_api.h"
#include "vdxr_fg_depth_validation.hpp"
#include "fg_depth_trace.hpp"
#include "fg_depth_probe_plan.hpp"
#include "xrfg_depth_guidance.hpp"
#ifdef VDXR_FG_DEPTH_AVAILABILITY
#include "depth_availability.hpp"
#endif
#include "pair_capture_private.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"

#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <cstring>

namespace {
    using Microsoft::WRL::ComPtr;

    struct Lease {
        VDXRFG_TicketV1 ticket{};
        uint64_t firstReadyValue{};
    };

    struct DepthPixelProbe {
        ComPtr<ID3D12Resource> readback;
        std::array<ComPtr<ID3D12Resource>, 2> sources;
        vdxrfg::depth_probe::Plan plan;
        uint64_t token{}, generation{}, serialA{}, serialB{}, epoch{};
        bool attempted{}, queued{}, fenceSignaled{}, finished{};
    };

    struct Context {
        ~Context() {
            vdxrfg::pair_capture::nr::destroyed(this);
        }
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Resource> input;
        ComPtr<ID3D12Resource> guideInput;
        std::array<ComPtr<ID3D12Resource>, 2> guideHistory;
#ifdef VDXR_FG_DEPTH_AVAILABILITY
        vdxrfg::DepthAvailability depthAvailability;
#endif
        ComPtr<ID3D12CommandAllocator> guideAllocator;
        ComPtr<ID3D12GraphicsCommandList> guideList;
        std::array<xrfg::D3D12ReprojectionView, 2> previousGuideViews{};
        uint64_t previousGuideSerial{}, previousGuideEpoch{};
        uint32_t previousGuideIndex{};
        DepthPixelProbe depthPixelProbe;
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
        bool colorOpaque{};
        bool depthReconstruction{};
        bool version3{};
        bool version4{};
        bool version5{};
    };

    bool pairBudget(void* opaque, const NrReadyV1& ready, NrFgReadbackBudgetV1& result) noexcept {
        // The original host Context owner lease spans this synchronous call.
        // No ref, allocation, command or Runtime callback is made here.
        auto* context = static_cast<Context*>(opaque);
        if (!context || context->poisoned || context->initializationPoisoned || !context->history ||
            context->generation != ready.fgHistoryGeneration ||
            reinterpret_cast<uint64_t>(context->device.Get()) != ready.deviceAddress ||
            reinterpret_cast<uint64_t>(context->queue.Get()) != ready.queueAddress)
            return false;
        ID3D12Resource* slots[xrfg::D3D12SwapchainHistory::kSlotCount]{};
        uint32_t next{};
        if (!context->history->borrowed_layout_sources(slots, next))
            return false;
        using vdxrfg::pair_capture::nr::observeBudgetResource;
        for (unsigned i = 0; i < 4; ++i) {
            auto* source = i < 2 ? slots[(next + i) % xrfg::D3D12SwapchainHistory::kSlotCount]
                                 : context->guideHistory[i - 2].Get();
            if (!observeBudgetResource(
                    context->device.Get(), source, i, 1, ready.fgContextLifetimeEpoch, result.rows[i]))
                return false;
        }
        NrFgBudgetRowV1 extra{};
        if (!observeBudgetResource(context->device.Get(),
                                   slots[(next + 2) % xrfg::D3D12SwapchainHistory::kSlotCount],
                                   0,
                                   1,
                                   ready.fgContextLifetimeEpoch,
                                   extra) ||
            !nr_fg_budget_same_layout(&result.rows[0], &extra))
            return false;
        if (!context->outputs[0][0] || !context->outputs[1][0] ||
            !observeBudgetResource(context->device.Get(),
                                   context->outputs[0][0].Get(),
                                   0,
                                   2,
                                   ready.fgContextLifetimeEpoch,
                                   result.rows[4]))
            return false;
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned i = 0; i < 4; ++i)
                if (context->outputs[bank][i]) {
                    if (!observeBudgetResource(context->device.Get(),
                                               context->outputs[bank][i].Get(),
                                               bank * 4 + i,
                                               2,
                                               ready.fgContextLifetimeEpoch,
                                               extra) ||
                        !nr_fg_budget_same_layout(&result.rows[4], &extra))
                        return false;
                }
        for (const auto& row : result.rows) {
            if (row.payloadBound > 134217728 - result.totalPayload ||
                row.allocationBytes > 134217728 - result.totalAllocation)
                return false;
            result.totalPayload += row.payloadBound;
            result.totalAllocation += row.allocationBytes;
        }
        return context->generation == ready.fgHistoryGeneration;
    }

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

    HRESULT createImpl(const VDXRFG_CreateInfoV3* info,
                       void** result,
                       uint32_t flags,
                       bool version3,
                       bool version4 = false,
                       ID3D12Resource* guideInput = nullptr,
                       bool version5 = false) noexcept {
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
            vdxrfg::pair_capture::prepare(context.get());
            context->device = static_cast<ID3D12Device*>(info->device);
            context->queue = static_cast<ID3D12CommandQueue*>(info->queue);
            context->input = static_cast<ID3D12Resource*>(info->input);
            context->multiplier = info->multiplier;
            context->version3 = version3;
            context->version4 = version4;
            context->version5 = version5;
            context->hostOwnsReal = (flags & VDXRFG_CREATE_HOST_OWNS_REAL) != 0;
            context->colorOpaque = (flags & VDXRFG_CREATE_COLOR_OPAQUE) != 0;
            context->depthReconstruction = (flags & VDXRFG_CREATE_DEPTH_RECONSTRUCTION) != 0;
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
            if (guideInput) {
                if (!sameDevice(device, guideInput) || guideInput == context->input.Get())
                    return VDXRFG_E_REJECTED;
                const auto guide = guideInput->GetDesc();
                if (guide.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || guide.Width != info->width ||
                    guide.Height != info->height || guide.DepthOrArraySize != 1 || guide.MipLevels != 1 ||
                    guide.SampleDesc.Count != 1 || guide.SampleDesc.Quality != 0 ||
                    guide.Format != DXGI_FORMAT_R32_FLOAT || (guide.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
                    return VDXRFG_E_REJECTED;
                context->guideInput = guideInput;
                auto privateGuide = guide;
                privateGuide.Flags = D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES guideHeap{};
                guideHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
                for (auto& resource : context->guideHistory) {
                    const HRESULT hr = device->CreateCommittedResource(&guideHeap,
                                                                       D3D12_HEAP_FLAG_NONE,
                                                                       &privateGuide,
                                                                       D3D12_RESOURCE_STATE_COMMON,
                                                                       nullptr,
                                                                       IID_PPV_ARGS(&resource));
                    if (FAILED(hr))
                        return hr;
                }
#ifdef VDXR_FG_DEPTH_AVAILABILITY
                if (context->depthReconstruction) {
                    ID3D12Resource* histories[] = {context->guideHistory[0].Get(), context->guideHistory[1].Get()};
                    const HRESULT availabilityHr =
                        context->depthAvailability.initialize(device, histories, info->width, info->height);
                    if (FAILED(availabilityHr))
                        return availabilityHr;
                }
#endif
                HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&context->guideAllocator));
                if (FAILED(hr))
                    return hr;
                hr = device->CreateCommandList(0,
                                               D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               context->guideAllocator.Get(),
                                               nullptr,
                                               IID_PPV_ARGS(&context->guideList));
                if (FAILED(hr))
                    return hr;
                hr = context->guideList->Close();
                vdxrfg::pair_capture::nr::fgClosed(context->guideList.Get(), hr);
                if (FAILED(hr))
                    return hr;
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
            // Host-owned real frames never use the packing lists. Priming still
            // needs a private current image, reused after full core completion.
            const uint32_t currentCount = context->hostOwnsReal ? 1u : 2u;
            for (uint32_t bank = 0; bank < currentCount; ++bank) {
                if (!context->hostOwnsReal) {
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
                    vdxrfg::pair_capture::nr::fgClosed(context->packLists[bank].Get(), hr);
                    if (FAILED(hr)) {
                        return hr;
                    }
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
                                                 std::span<ID3D12Resource* const>(current.data(), currentCount),
                                                 std::span<ID3D12Resource* const>(synthetic.data(), syntheticCount),
                                                 static_cast<DXGI_FORMAT>(info->format),
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 fidelityFxFull ? xrfg::D3D12OpticalFlowBackend::fidelity_fx
                                                                : xrfg::D3D12OpticalFlowBackend::nvidia,
                                                 opticalFlowOptions,
                                                 (flags & VDXRFG_CREATE_GPU_TIMING) != 0,
                                                 !context->hostOwnsReal,
                                                 xrfg::D3D12SyntheticOutputLayout::shared_stereo_atlas,
                                                 info->multiplier >= 3,
                                                 version4 && info->multiplier > 2,
                                                 context->depthReconstruction);
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
            ID3D12Resource* pairOutputs[8]{};
            for (unsigned bank = 0; bank < 2; ++bank)
                for (unsigned role = 0; role < 4; ++role)
                    pairOutputs[bank * 4 + role] = context->outputs[bank][role].Get();
            if (!vdxrfg::pair_capture::nr::requested() || (context->guideHistory[0] && context->guideHistory[1])) {
                vdxrfg::pair_capture::nr::installBudgetProvider(context.get(), pairBudget);
                vdxrfg::pair_capture::bind(
                    context.get(), device, context->queue.Get(), pairOutputs, context->generation);
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

    HRESULT WINAPI createV5(const VDXRFG_CreateInfoV5* info, void** result) noexcept {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (!info || info->struct_size < sizeof(*info) || info->api_version != VDXRFG_API_VERSION_5 ||
            info->base.struct_size < sizeof(info->base) || info->base.api_version != VDXRFG_API_VERSION_3 ||
            (info->guide_input && (info->base.flags & VDXRFG_CREATE_FIDELITYFX_FULL)) ||
            ((info->base.flags & VDXRFG_CREATE_COLOR_OPAQUE) && !info->guide_input) ||
            ((info->base.flags & VDXRFG_CREATE_DEPTH_RECONSTRUCTION) && !info->guide_input) ||
            (info->base.flags &
             ~(VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL |
               VDXRFG_CREATE_OMIT_REAL_OUTPUTS | VDXRFG_CREATE_COLOR_OPAQUE | VDXRFG_CREATE_DEPTH_RECONSTRUCTION)) ||
            ((info->base.flags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS) &&
             !(info->base.flags & VDXRFG_CREATE_HOST_OWNS_REAL)))
            return VDXRFG_E_REJECTED;
#ifndef VDXR_FG_DEPTH_ENDPOINT_PREFERENCE
        if (info->base.flags & VDXRFG_CREATE_COLOR_OPAQUE)
            return VDXRFG_E_REJECTED;
#endif
#ifndef VDXR_FG_DEPTH_RECONSTRUCTION
        if (info->base.flags & VDXRFG_CREATE_DEPTH_RECONSTRUCTION)
            return VDXRFG_E_REJECTED;
#endif
        return createImpl(
            &info->base, result, info->base.flags, true, true, static_cast<ID3D12Resource*>(info->guide_input), true);
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

    // Optional one-shot diagnostic. Allocation failure never changes capture
    // status, and the attempt latch prevents retry/allocation on later frames.
    bool recordDepthPixelProbe(Context& context, const Lease& lease, uint32_t guideIndex, uint64_t epoch) noexcept {
        auto& probe = context.depthPixelProbe;
        probe.attempted = true;
        probe.token = lease.ticket.token;
        probe.generation = lease.ticket.generation;
        probe.serialA = context.previousGuideSerial;
        probe.serialB = lease.ticket.current_serial;
        probe.epoch = epoch;
        probe.sources = {context.guideHistory[context.previousGuideIndex], context.guideHistory[guideIndex]};
        HRESULT result = E_INVALIDARG;
        const auto a = probe.sources[0]->GetDesc(), b = probe.sources[1]->GetDesc();
        probe.plan = vdxrfg::depth_probe::makePlan(a.Width, a.Height);
        if (probe.plan.rowCount && a.Width == b.Width && a.Height == b.Height &&
            a.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && b.Dimension == a.Dimension &&
            a.Format == DXGI_FORMAT_R32_FLOAT && b.Format == a.Format && a.DepthOrArraySize == 1 &&
            b.DepthOrArraySize == 1 && a.MipLevels == 1 && b.MipLevels == 1 && a.SampleDesc.Count == 1 &&
            b.SampleDesc.Count == 1 && a.SampleDesc.Quality == 0 && b.SampleDesc.Quality == 0 &&
            probe.sources[0].Get() != probe.sources[1].Get()) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = probe.plan.bufferBytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            result = context.device->CreateCommittedResource(&heap,
                                                             D3D12_HEAP_FLAG_NONE,
                                                             &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                                             nullptr,
                                                             IID_PPV_ARGS(&probe.readback));
        }
        if (FAILED(result)) {
            probe.finished = true;
            VDXR_FG_DEPTH_TRACE("DepthPixelsUnavailable",
                                TraceLoggingPointer(&context, "Context"),
                                TraceLoggingUInt64(probe.token, "Token"),
                                TraceLoggingString("prepare", "Stage"),
                                TraceLoggingHResult(result, "Result"));
            return false;
        }
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{
            transition(probe.sources[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(probe.sources[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        context.guideList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = probe.sources[endpoint].Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            for (uint32_t row = 0; row < probe.plan.rowCount; ++row) {
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = probe.readback.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Offset = probe.plan.offset(endpoint, row);
                destination.PlacedFootprint.Footprint = {
                    DXGI_FORMAT_R32_FLOAT, probe.plan.width, 1, 1, probe.plan.rowPitch};
                const D3D12_BOX box{0, probe.plan.rows[row], 0, probe.plan.width, probe.plan.rows[row] + 1, 1};
                context.guideList->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
            }
        }
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        context.guideList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        return true;
    }

    void pollDepthPixelProbe(Context& context) noexcept {
        auto& probe = context.depthPixelProbe;
        if (!probe.queued || !probe.fenceSignaled || probe.finished ||
            !TraceLoggingProviderEnabled(g_vdxrFgDepthProvider, 0, 0))
            return;
        const uint64_t completed = context.consumed->GetCompletedValue();
        if (completed == UINT64_MAX) {
            probe.finished = true;
            VDXR_FG_DEPTH_TRACE("DepthPixelsUnavailable",
                                TraceLoggingPointer(&context, "Context"),
                                TraceLoggingUInt64(probe.token, "Token"),
                                TraceLoggingString("consumed-fence-device-removed", "Stage"),
                                TraceLoggingHResult(DXGI_ERROR_DEVICE_REMOVED, "Result"));
            return;
        }
        if (completed < probe.token)
            return;
        // This signal was successfully queued AFTER the probe copies on the
        // same guideList. Map cannot wait for GPU work at this point.
        probe.finished = true;
        void* mapped{};
        const D3D12_RANGE read{0, static_cast<SIZE_T>(probe.plan.bufferBytes)};
        const HRESULT result = probe.readback->Map(0, &read, &mapped);
        if (FAILED(result)) {
            VDXR_FG_DEPTH_TRACE("DepthPixelsUnavailable",
                                TraceLoggingPointer(&context, "Context"),
                                TraceLoggingUInt64(probe.token, "Token"),
                                TraceLoggingString("map-after-completion", "Stage"),
                                TraceLoggingHResult(result, "Result"));
            return;
        }
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                uint64_t positive{}, zero{}, negative{}, nan{}, infinity{};
                float minimum = std::numeric_limits<float>::max(), maximum = std::numeric_limits<float>::lowest();
                float minPositive = minimum, maxPositive = 0;
                const uint32_t eyeWidth = probe.plan.width / 2;
                for (uint32_t row = 0; row < probe.plan.rowCount; ++row) {
                    const auto* pixels = static_cast<const uint8_t*>(mapped) + probe.plan.offset(endpoint, row);
                    for (uint32_t x = eye * eyeWidth; x < (eye + 1) * eyeWidth; ++x) {
                        uint32_t bits{};
                        float value{};
                        std::memcpy(&bits, pixels + x * sizeof(float), sizeof(bits));
                        std::memcpy(&value, &bits, sizeof(value));
                        if ((bits & 0x7f800000u) == 0x7f800000u) {
                            if (bits & 0x007fffffu)
                                ++nan;
                            else
                                ++infinity;
                            continue;
                        }
                        minimum = (std::min)(minimum, value);
                        maximum = (std::max)(maximum, value);
                        if (!(bits & 0x7fffffffu))
                            ++zero;
                        else if (bits & 0x80000000u)
                            ++negative;
                        else {
                            ++positive;
                            minPositive = (std::min)(minPositive, value);
                            maxPositive = (std::max)(maxPositive, value);
                        }
                    }
                }
                const bool finite = positive + zero + negative != 0;
                VDXR_FG_DEPTH_TRACE(
                    "DepthPixelsCompleted",
                    TraceLoggingPointer(&context, "Context"),
                    TraceLoggingUInt64(probe.token, "Token"),
                    TraceLoggingUInt64(probe.generation, "Generation"),
                    TraceLoggingUInt64(probe.serialA, "PreviousGuideSerial"),
                    TraceLoggingUInt64(probe.serialB, "CurrentGuideSerial"),
                    TraceLoggingUInt64(probe.epoch, "GuideEpoch"),
                    TraceLoggingPointer(probe.sources[endpoint].Get(), "GuideResource"),
                    TraceLoggingUInt32(endpoint, "Endpoint"),
                    TraceLoggingUInt32(eye, "Eye"),
                    TraceLoggingPointer(context.consumed.Get(), "CompletedFence"),
                    TraceLoggingUInt64(probe.token, "RequiredFenceValue"),
                    TraceLoggingUInt64(completed, "ObservedFenceValue"),
                    TraceLoggingString("up-to-32-unique-full-width-scanlines-not-full-image", "SamplingScope"),
                    TraceLoggingUInt32(probe.plan.width, "AtlasWidth"),
                    TraceLoggingUInt32(probe.plan.height, "AtlasHeight"),
                    TraceLoggingUInt32(probe.plan.rowCount, "SampledRowCount"),
                    TraceLoggingUInt32Array(
                        probe.plan.rows.data(), static_cast<UINT16>(probe.plan.rowCount), "SampledRows"),
                    TraceLoggingUInt64(uint64_t(eyeWidth) * probe.plan.rowCount, "EyeSampleCount"),
                    TraceLoggingUInt64(positive, "PositiveFiniteInverseZ"),
                    TraceLoggingUInt64(zero, "ExactZeroInfinity"),
                    TraceLoggingUInt64(negative, "NegativeInvalid"),
                    TraceLoggingUInt64(nan, "NaN"),
                    TraceLoggingUInt64(infinity, "NonfiniteInfinity"),
                    TraceLoggingBool(finite, "HasFiniteSamples"),
                    TraceLoggingFloat32(finite ? minimum : 0, "MinFiniteInverseZ"),
                    TraceLoggingFloat32(finite ? maximum : 0, "MaxFiniteInverseZ"),
                    TraceLoggingFloat32(positive ? minPositive : 0, "MinPositiveInverseZ"),
                    TraceLoggingFloat32(maxPositive, "MaxPositiveInverseZ"));
            }
        }
        const D3D12_RANGE noWrites{0, 0};
        probe.readback->Unmap(0, &noWrites);
        // Retain the frozen resources through Context lifetime, including any
        // later poisoned/reset state; do not relabel this startup sample.
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

    HRESULT submitImpl(void* handle,
                       const VDXRFG_SubmitInfoV3* info,
                       VDXRFG_TicketV1* output,
                       const VDXRFG_SubmitInfoV5* depth = nullptr) noexcept {
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
        pollDepthPixelProbe(*context);
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
        vdxrfg::pair_capture::ticket(context, lease->ticket);
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
                vdxrfg::pair_capture::signalled(
                    "original-producer-Wait", context->queue.Get(), fence, info->producer_value, hr);
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
            // The full previous synthesis gate above protects both private
            // atlases and this allocator independently of external output leases.
            const bool hasGuide = depth && depth->guide_mask == 3;
            const uint32_t guideIndex = context->previousGuideIndex ^ 1u;
#ifdef VDXR_FG_DEPTH_ENDPOINT_PREFERENCE
            const bool pairAccepted = hasGuide && vdxrfg::compatibleDepthPair(context->previousSerial,
                                                                              context->previousGuideSerial,
                                                                              context->previousGuideEpoch,
                                                                              info->serial,
                                                                              depth->guide_epoch);
#endif
            if (hasGuide) {
                bool probeRecorded = false;
                hr = context->guideAllocator->Reset();
                vdxrfg::pair_capture::nr::fgAllocatorReset(context->guideAllocator.Get(), hr);
                if (FAILED(hr))
                    return poison();
                hr = context->guideList->Reset(context->guideAllocator.Get(), nullptr);
                vdxrfg::pair_capture::nr::fgReset(context->guideList.Get(), context->guideAllocator.Get(), hr);
                if (FAILED(hr))
                    return poison();
                std::array<D3D12_RESOURCE_BARRIER, 2> barriers{transition(context->guideInput.Get(),
                                                                          D3D12_RESOURCE_STATE_COMMON,
                                                                          D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                               transition(context->guideHistory[guideIndex].Get(),
                                                                          D3D12_RESOURCE_STATE_COMMON,
                                                                          D3D12_RESOURCE_STATE_COPY_DEST)};
                context->guideList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                vdxrfg::pair_capture::barriers(
                    context->guideList.Get(), static_cast<UINT>(barriers.size()), barriers.data());
                context->guideList->CopyResource(context->guideHistory[guideIndex].Get(), context->guideInput.Get());
                vdxrfg::pair_capture::nr::fgGuideCopy(context->guideInput.Get(),
                                                      context->guideHistory[guideIndex].Get(),
                                                      context->guideList.Get(),
                                                      info->serial);
                for (auto& barrier : barriers)
                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                context->guideList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                vdxrfg::pair_capture::barriers(
                    context->guideList.Get(), static_cast<UINT>(barriers.size()), barriers.data());
#ifdef VDXR_FG_DEPTH_AVAILABILITY
                if (context->depthReconstruction) {
                    const VDXRFG_RectV1 publishedRects[] = {info->views[0].rect, info->views[1].rect};
                    hr = context->depthAvailability.record(context->guideList.Get(), guideIndex, publishedRects);
                    if (FAILED(hr))
                        return poison();
                }
#endif
#ifdef VDXR_FG_DEPTH_ENDPOINT_PREFERENCE
                if (pairAccepted && !context->depthPixelProbe.attempted &&
                    TraceLoggingProviderEnabled(g_vdxrFgDepthProvider, 0, 0)) {
                    probeRecorded = recordDepthPixelProbe(*context, *lease, guideIndex, depth->guide_epoch);
                }
#endif
                hr = context->guideList->Close();
                vdxrfg::pair_capture::nr::fgClosed(context->guideList.Get(), hr);
                if (FAILED(hr))
                    return poison();
                ID3D12CommandList* lists[] = {context->guideList.Get()};
                context->queue->ExecuteCommandLists(1, lists);
                vdxrfg::pair_capture::queued(context->queue.Get(), 1, lists);
                if (probeRecorded) {
                    context->depthPixelProbe.queued = true;
                    VDXR_FG_DEPTH_TRACE("DepthPixelsQueued",
                                        TraceLoggingPointer(context, "Context"),
                                        TraceLoggingUInt64(lease->ticket.token, "Token"),
                                        TraceLoggingUInt64(context->depthPixelProbe.serialA, "PreviousGuideSerial"),
                                        TraceLoggingUInt64(context->depthPixelProbe.serialB, "CurrentGuideSerial"),
                                        TraceLoggingUInt64(context->depthPixelProbe.epoch, "GuideEpoch"),
                                        TraceLoggingUInt32(context->depthPixelProbe.plan.rowCount, "SampledRowCount"),
                                        TraceLoggingUInt64(context->depthPixelProbe.plan.bufferBytes, "ReadbackBytes"),
                                        TraceLoggingBool(false, "GpuCompletionObserved"));
                }
            }
            hr = context->queue->Signal(context->consumed.Get(), lease->ticket.token);
            vdxrfg::pair_capture::signalled("original-input-consumed-Signal",
                                            context->queue.Get(),
                                            context->consumed.Get(),
                                            lease->ticket.token,
                                            hr);
            if (FAILED(hr)) {
                return poison();
            }
            if (context->depthPixelProbe.queued && context->depthPixelProbe.token == lease->ticket.token)
                context->depthPixelProbe.fenceSignaled = true;
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
#ifdef VDXR_FG_DEPTH_ENDPOINT_PREFERENCE
            std::shared_ptr<const xrfg::D3D12DepthGuidance> guidance;
            if (pairAccepted) {
                auto pair = std::make_shared<xrfg::D3D12DepthGuidance>();
                pair->previous = context->guideHistory[context->previousGuideIndex];
                pair->current = context->guideHistory[guideIndex];
                pair->serialA = context->previousGuideSerial;
                pair->serialB = info->serial;
                pair->epoch = depth->guide_epoch;
                pair->atlasWidth = static_cast<uint32_t>(context->input->GetDesc().Width);
                pair->atlasHeight = context->input->GetDesc().Height;
                pair->colorOpaque = context->colorOpaque;
                pair->reconstructCamera = context->depthReconstruction;
#ifdef VDXR_FG_DEPTH_AVAILABILITY
                // compatibleDepthPair rejects prime, missing-guide, reset and
                // epoch gaps. Every corresponding owned history was reduced
                // after its CopyResource, before its consumed-fence signal.
                if (context->depthReconstruction)
                    pair->availability = context->depthAvailability.flagsResource();
#endif
                pair->viewsA = context->previousGuideViews;
                pair->viewsB = views;
                guidance = std::move(pair);
            }
            // Always clear the consumer for a legacy/no-guide transaction.
            const bool guidancePassed = static_cast<bool>(guidance);
            context->synthesizer.set_depth_guidance(std::move(guidance));
            VDXR_FG_DEPTH_TRACE(
                "DepthReceiver",
                TraceLoggingPointer(context, "Context"),
                TraceLoggingUInt64(lease->ticket.token, "Token"),
                TraceLoggingUInt64(context->generation, "Generation"),
                TraceLoggingUInt32(depth ? depth->guide_mask : 0, "GuideMask"),
                TraceLoggingUInt64(context->previousSerial, "PreviousColorSerial"),
                TraceLoggingUInt64(info->serial, "CurrentColorSerial"),
                TraceLoggingUInt64(context->previousGuideSerial, "PreviousGuideSerial"),
                TraceLoggingUInt64(depth ? depth->guide_serial : 0, "CurrentGuideSerial"),
                TraceLoggingUInt64(context->previousGuideEpoch, "PreviousGuideEpoch"),
                TraceLoggingUInt64(depth ? depth->guide_epoch : 0, "CurrentGuideEpoch"),
                TraceLoggingBool(hasGuide, "HasValidGuide"),
                TraceLoggingBool(pairAccepted, "PairAccepted"),
                TraceLoggingBool(guidancePassed, "NonNullGuidancePassed"),
                TraceLoggingBool(context->colorOpaque, "ColorOpaque"),
                TraceLoggingPointer(pairAccepted ? context->guideHistory[context->previousGuideIndex].Get() : nullptr,
                                    "SrvA"),
                TraceLoggingPointer(pairAccepted ? context->guideHistory[guideIndex].Get() : nullptr, "SrvB"));
#endif
            xrfg::D3D12FrameSynthesisTicket synthesis{};
            // External leases retain synthetic banks, not this private image.
            // wait_for_previous_submission above proved its full GPU completion.
            const uint32_t currentIndex = context->hostOwnsReal ? 0u : bank;
            if (!context->previousSerial) {
                hr = context->synthesizer.submit_prime(capture, views, currentIndex, &synthesis);
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
                    currentIndex,
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
                vdxrfg::pair_capture::nr::fgAllocatorReset(context->copyAllocators[bank].Get(), hr);
                if (FAILED(hr)) {
                    return poison();
                }
                hr = context->packLists[bank]->Reset(context->copyAllocators[bank].Get(), nullptr);
                vdxrfg::pair_capture::nr::fgReset(
                    context->packLists[bank].Get(), context->copyAllocators[bank].Get(), hr);
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
                vdxrfg::pair_capture::nr::fgClosed(context->packLists[bank].Get(), hr);
                if (FAILED(hr)) {
                    return poison();
                }
                ID3D12CommandList* pack[] = {context->packLists[bank].Get()};
                context->queue->ExecuteCommandLists(1, pack);
                vdxrfg::pair_capture::queued(context->queue.Get(), 1, pack);
            }
            // A host-owned prime has no outer output to pack. Its backend
            // priming work still precedes this ready signal on the same queue.
            hr = context->queue->Signal(context->ready.Get(), lease->ticket.token);
            vdxrfg::pair_capture::signalled("original-full-output-ready-Signal",
                                            context->queue.Get(),
                                            context->ready.Get(),
                                            lease->ticket.token,
                                            hr);
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
            // Publish B even when A was absent. Publication follows successful
            // full-output signaling; failed/unknown work cannot advance identity.
            context->previousGuideSerial = hasGuide ? info->serial : 0;
            context->previousGuideEpoch = hasGuide ? depth->guide_epoch : 0;
            if (hasGuide) {
                context->previousGuideIndex = guideIndex;
                context->previousGuideViews = views;
            }
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

    HRESULT WINAPI submitV5(void* handle, const VDXRFG_SubmitInfoV5* info, VDXRFG_TicketV1* output) noexcept {
        if (!output)
            return E_POINTER;
        *output = {};
        const auto context = static_cast<Context*>(handle);
        if (!context || !context->version5 || !info ||
            !vdxrfg::validDepthEnvelope(*info, context->guideInput.Get() != nullptr))
            return VDXRFG_E_REJECTED;
        vdxrfg::pair_capture::nr::fgAdmissionBegin(handle, info->base.serial);
        return submitImpl(handle, &info->base, output, info);
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
        vdxrfg::pair_capture::nr::fgRetireBegin(context, token);
        if (vdxrfg::pair_capture::holdRetire(context, token))
            return VDXRFG_E_BUSY;
        pollDepthPixelProbe(*context);
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

    HRESULT resetImpl(void* handle, uint32_t flags, bool version5) noexcept {
        auto context = static_cast<Context*>(handle);
        // Preserve the existing NR-off observer. NR invalidates only a validated
        // successful generation change, or a failed reset after work begins.
        if (!vdxrfg::pair_capture::nr::requested())
            vdxrfg::pair_capture::abort(context, "original-reset-requested");
        constexpr uint32_t opacityMask = VDXRFG_RESET_COLOR_OPAQUE | VDXRFG_RESET_COLOR_ALPHA;
        const uint32_t opacity = flags & opacityMask;
        if (!context || (flags & ~(1u | opacityMask)) || opacity == opacityMask) {
            return VDXRFG_E_REJECTED;
        }
        if (opacity) {
            // Only V5 Create can retain a guide, and it rejects FidelityFX
            // with a guide before initializing any backend resources.
            if (!version5 || !context->version5 || !context->guideInput)
                return VDXRFG_E_REJECTED;
#ifndef VDXR_FG_DEPTH_ENDPOINT_PREFERENCE
            return VDXRFG_E_REJECTED;
#endif
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
            if (vdxrfg::pair_capture::nr::requested())
                vdxrfg::pair_capture::nr::fgAbort(context);
            return VDXRFG_E_POISONED;
        }
        ++context->generation;
        vdxrfg::pair_capture::generation(context, context->generation);
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
        context->previousGuideSerial = context->previousGuideEpoch = 0;
        context->previousGuideViews = {};
#ifdef VDXR_FG_DEPTH_ENDPOINT_PREFERENCE
        context->synthesizer.set_depth_guidance({});
#endif
        // Publish the new mode only after every fallible reset operation has
        // succeeded and the old pair is invalidated. Work-slot guidance owns
        // its immutable per-pair snapshot; BUSY/failed resets leave it intact.
        if (opacity)
            context->colorOpaque = opacity == VDXRFG_RESET_COLOR_OPAQUE;
        return S_OK;
    }

    HRESULT WINAPI reset(void* handle, uint32_t flags) noexcept {
        return resetImpl(handle, flags, false);
    }

    HRESULT WINAPI resetV5(void* handle, uint32_t flags) noexcept {
        return resetImpl(handle, flags, true);
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

extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV5(VDXRFG_ApiV5* output) noexcept {
    if (!output || output->struct_size < sizeof(*output) || output->api_version != VDXRFG_API_VERSION_5)
        return VDXRFG_E_REJECTED;
    uint32_t capabilities = VDXRFG_DEPTH_CANONICAL_TRANSPORT;
    uint32_t flags = VDXRFG_CREATE_GPU_TIMING | VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_FIDELITYFX_FULL |
                     VDXRFG_CREATE_OMIT_REAL_OUTPUTS;
#ifdef VDXR_FG_DEPTH_ENDPOINT_PREFERENCE
    capabilities |= VDXRFG_DEPTH_ENDPOINT_PREFERENCE | VDXRFG_DEPTH_OPACITY_RESET;
    flags |= VDXRFG_CREATE_COLOR_OPAQUE;
#endif
#ifdef VDXR_FG_DEPTH_RECONSTRUCTION
    capabilities |= VDXRFG_DEPTH_RECONSTRUCTION;
    flags |= VDXRFG_CREATE_DEPTH_RECONSTRUCTION;
#endif
    const VDXRFG_ApiV5 api{sizeof(VDXRFG_ApiV5),
                           VDXRFG_API_VERSION_5,
                           flags,
                           4,
                           capabilities,
                           0,
                           createV5,
                           submitV5,
                           retire,
                           resetV5,
                           statistics,
                           destroy,
                           outputReadiness};
    *output = api;
    return S_OK;
}
