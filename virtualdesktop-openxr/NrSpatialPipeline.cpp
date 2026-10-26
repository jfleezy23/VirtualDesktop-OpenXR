#include "NrSpatialPipeline.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cassert>
#include <new>
#include <vector>
#include "NrSpatialDownsampleCS.h"
#include "NrSpatialResolvePS.h"
#include "FullScreenQuadVS.h"

namespace virtualdesktop_openxr::nr_spatial {
    using Microsoft::WRL::ComPtr;
    namespace {
        struct Failure {
            HRESULT result;
        };
        void Check(HRESULT result) {
            if (FAILED(result))
                throw Failure{result};
        }
        bool Same(Rect a, Rect b) {
            return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        }
        bool Geometry(const FrameInput& a, const FrameInput& b) {
            if (a.processingPercent != b.processingPercent || a.outputEyeStride != b.outputEyeStride ||
                a.outputFormat != b.outputFormat)
                return false;
            for (size_t i = 0; i < 2; ++i) {
                const auto& x = a.eyes[i];
                const auto& y = b.eyes[i];
                if (x.colorFormat != y.colorFormat || !Same(x.colorRect, y.colorRect) ||
                    bool(x.depth) != bool(y.depth) ||
                    (x.depth && (x.depthFormat != y.depthFormat || !Same(x.depthRect, y.depthRect))))
                    return false;
            }
            return true;
        }
        bool Identity(const FrameInput& a, const FrameInput& b) {
            if (a.output != b.output)
                return false;
            for (size_t i = 0; i < 2; ++i)
                if (a.eyes[i].color != b.eyes[i].color || a.eyes[i].depth != b.eyes[i].depth)
                    return false;
            return true;
        }
        bool Contains(ID3D12Resource* r, Rect rect) {
            if (!r || !rect.width || !rect.height)
                return false;
            const auto d = r->GetDesc();
            return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize >= 1 && d.MipLevels == 1 &&
                   d.SampleDesc.Count == 1 && uint64_t(rect.x) + rect.width <= d.Width &&
                   uint64_t(rect.y) + rect.height <= d.Height;
        }
        bool ColorView(ID3D12Resource* r, DXGI_FORMAT view) {
            const auto f = r->GetDesc().Format;
            const auto raw = RawColorFormat(view);
            return raw == DXGI_FORMAT_R8G8B8A8_UNORM
                       ? f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                             f == DXGI_FORMAT_R8G8B8A8_TYPELESS
                       : f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                             f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        }
        bool DepthView(ID3D12Resource* r, DXGI_FORMAT view) {
            const auto f = r->GetDesc().Format;
            switch (view) {
            case DXGI_FORMAT_R32_FLOAT:
                return f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS;
            case DXGI_FORMAT_R16_UNORM:
                return f == DXGI_FORMAT_R16_UNORM || f == DXGI_FORMAT_R16_TYPELESS;
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                return f == DXGI_FORMAT_R24G8_TYPELESS;
            case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
                return f == DXGI_FORMAT_R32G8X24_TYPELESS;
            default:
                return false;
            }
        }
        void Transition(ID3D12GraphicsCommandList* list,
                        ID3D12Resource* r,
                        D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            list->ResourceBarrier(1, &b);
        }
        struct Constants {
            Rect color, depth;
            Extent work;
            UINT srgb, hasDepth, outputX, reserved[3]{};
        };
        static_assert(sizeof(Constants) == 16 * sizeof(UINT));
    } // namespace

    struct Pipeline::Impl {
        struct Scratch {
            ComPtr<ID3D12Resource> proxy, answer, depth;
            Extent extent;
        };
        struct Batch {
            FrameInput input;
            std::array<ComPtr<ID3D12Resource>, 5> owners;
            ComPtr<ID3D12DescriptorHeap> views, targets;
            uint64_t completion{};
        };
        struct Generation {
            FrameInput geometry;
            ComPtr<ID3D12RootSignature> root;
            ComPtr<ID3D12PipelineState> downsample, resolve;
            std::array<Scratch, 2> scratch;
            std::vector<std::unique_ptr<Batch>> batches;
        };
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence> fence;
        std::unique_ptr<Generation> generation;
        Batch* active{};
        UINT viewStride{};

        explicit Impl(ID3D12Device* d, ID3D12Fence* f) : device(d), fence(f) {
            if (d)
                viewStride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        void Validate(const FrameInput& input) {
            if (!device || !fence || !input.output || !IsSupportedColorFormat(input.outputFormat))
                throw Failure{E_INVALIDARG};
            const auto out = input.output->GetDesc();
            if (out.DepthOrArraySize != 1 || !(out.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
                !ColorView(input.output, input.outputFormat))
                throw Failure{E_INVALIDARG};
            for (UINT e = 0; e < 2; ++e) {
                const auto& eye = input.eyes[e];
                Extent work{};
                if (!TryWorkingExtent({eye.colorRect.width, eye.colorRect.height}, input.processingPercent, work) ||
                    eye.colorFormat != input.outputFormat || !Contains(eye.color, eye.colorRect) ||
                    !ColorView(eye.color, eye.colorFormat) ||
                    (eye.color->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                    eye.color == input.output ||
                    !Contains(input.output,
                              {e ? input.outputEyeStride : 0, 0, eye.colorRect.width, eye.colorRect.height}))
                    throw Failure{E_INVALIDARG};
                if (eye.depth && (!Contains(eye.depth, eye.depthRect) || !DepthView(eye.depth, eye.depthFormat) ||
                                  (eye.depth->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                                  eye.depth == input.output || eye.depth == eye.color))
                    throw Failure{E_INVALIDARG};
            }
            if (input.outputEyeStride < input.eyes[0].colorRect.width)
                throw Failure{E_INVALIDARG};
        }
        ComPtr<ID3D12Resource> Texture(Extent extent, DXGI_FORMAT format) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = extent.width;
            d.Height = extent.height;
            d.DepthOrArraySize = d.MipLevels = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result)));
            return result;
        }
        std::unique_ptr<Generation> CreateGeneration(const FrameInput& input) {
            auto g = std::make_unique<Generation>();
            g->geometry = input;
            D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
                                             {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4}};
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants = {0, 0, 16};
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1].DescriptorTable = {2, ranges};
            D3D12_ROOT_SIGNATURE_DESC desc{};
            desc.NumParameters = 2;
            desc.pParameters = parameters;
            desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> serialized, error;
            Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
            Check(device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g->root)));
            D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
            compute.pRootSignature = g->root.Get();
            compute.CS = {g_NrSpatialDownsampleCS, sizeof(g_NrSpatialDownsampleCS)};
            Check(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&g->downsample)));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
            graphics.pRootSignature = g->root.Get();
            graphics.VS = {g_FullScreenQuadVS, sizeof(g_FullScreenQuadVS)};
            graphics.PS = {g_NrSpatialResolvePS, sizeof(g_NrSpatialResolvePS)};
            auto& blend = graphics.BlendState.RenderTarget[0];
            blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            graphics.SampleMask = UINT_MAX;
            graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            graphics.RasterizerState.DepthClipEnable = TRUE;
            graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            graphics.NumRenderTargets = 1;
            graphics.RTVFormats[0] = RawColorFormat(input.outputFormat);
            graphics.SampleDesc.Count = 1;
            Check(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&g->resolve)));
            for (UINT e = 0; e < 2; ++e) {
                auto& scratch = g->scratch[e];
                const auto& eye = input.eyes[e];
                TryWorkingExtent({eye.colorRect.width, eye.colorRect.height}, input.processingPercent, scratch.extent);
                scratch.proxy = Texture(scratch.extent, DXGI_FORMAT_R8G8B8A8_UNORM);
                scratch.answer = Texture(scratch.extent, DXGI_FORMAT_R8G8B8A8_UNORM);
                if (eye.depth)
                    scratch.depth = Texture(scratch.extent, DXGI_FORMAT_R32_FLOAT);
            }
            return g;
        }
        std::unique_ptr<Batch> CreateBatch(const FrameInput& input, Generation& g) {
            auto b = std::make_unique<Batch>();
            D3D12_DESCRIPTOR_HEAP_DESC h{};
            h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            h.NumDescriptors = 12;
            h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&b->views)));
            h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            h.NumDescriptors = 1;
            h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            Check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&b->targets)));
            BindBatch(*b, input, g);
            return b;
        }
        // Only called for a new batch or one whose completion was positively
        // observed. Descriptor writes cannot fail and allocate no host memory.
        void BindBatch(Batch& batch, const FrameInput& input, Generation& g) {
            auto* b = &batch;
            b->input = input;
            b->owners = {
                input.eyes[0].color, input.eyes[0].depth, input.eyes[1].color, input.eyes[1].depth, input.output};
            b->completion = 0;
            D3D12_RENDER_TARGET_VIEW_DESC target{};
            target.Format = RawColorFormat(input.outputFormat);
            target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(input.output, &target, b->targets->GetCPUDescriptorHandleForHeapStart());
            for (UINT e = 0; e < 2; ++e) {
                auto handle = b->views->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += SIZE_T(e * 6) * viewStride;
                auto srv = [&](ID3D12Resource* r, DXGI_FORMAT format, bool native = false) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
                    d.Format = format;
                    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    if (native) {
                        // Resolved slice zero can alias the app's array. Other
                        // slices are already flattened by the existing host.
                        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                        d.Texture2DArray.MipLevels = 1;
                        d.Texture2DArray.ArraySize = 1;
                    } else {
                        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        d.Texture2D.MipLevels = 1;
                    }
                    device->CreateShaderResourceView(r, &d, handle);
                    handle.ptr += viewStride;
                };
                const auto& eye = input.eyes[e];
                const auto& s = g.scratch[e];
                srv(eye.color, RawColorFormat(eye.colorFormat), true);
                srv(eye.depth, eye.depth ? eye.depthFormat : DXGI_FORMAT_R32_FLOAT, true);
                srv(s.proxy.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                srv(s.answer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                device->CreateUnorderedAccessView(s.proxy.Get(), nullptr, &uav, handle);
                handle.ptr += viewStride;
                uav.Format = DXGI_FORMAT_R32_FLOAT;
                device->CreateUnorderedAccessView(s.depth.Get(), nullptr, &uav, handle);
            }
        }
        Constants Parameters(UINT e) const {
            const auto& eye = active->input.eyes[e];
            return {eye.colorRect,
                    eye.depthRect,
                    generation->scratch[e].extent,
                    IsSrgb(eye.colorFormat),
                    bool(eye.depth),
                    e ? active->input.outputEyeStride : 0,
                    {}};
        }
        void NativeTransitions(ID3D12GraphicsCommandList* list,
                               D3D12_RESOURCE_STATES state,
                               bool entering,
                               bool includeDepth) {
            std::array<ID3D12Resource*, 4> seen{};
            size_t count = 0;
            for (const auto& eye : active->input.eyes) {
                for (auto* r : {eye.color, includeDepth ? eye.depth : nullptr}) {
                    if (!r || std::find(seen.begin(), seen.begin() + count, r) != seen.begin() + count)
                        continue;
                    seen[count++] = r;
                    Transition(list,
                               r,
                               entering ? D3D12_RESOURCE_STATE_COMMON : state,
                               entering ? state : D3D12_RESOURCE_STATE_COMMON);
                }
            }
        }
    };

    Pipeline::Pipeline(ID3D12Device* device, ID3D12Fence* fence) : impl_(std::make_unique<Impl>(device, fence)) {
    }
    Pipeline::~Pipeline() = default;
    bool Pipeline::Matches(const FrameInput& input) const {
        return impl_->generation && Geometry(impl_->generation->geometry, input);
    }
    HRESULT Pipeline::Prepare(const FrameInput& input) {
        auto& p = *impl_;
        if (p.active)
            return DXGI_ERROR_WAS_STILL_DRAWING;
        try {
            p.Validate(input);
            const uint64_t completed = p.fence->GetCompletedValue();
            if (completed == UINT64_MAX)
                return DXGI_ERROR_DEVICE_REMOVED;
            if (p.generation && !Matches(input))
                return E_INVALIDARG; // Caller drains and replaces generation.
            if (!p.generation) {
                auto g = p.CreateGeneration(input);
                g->batches.push_back(p.CreateBatch(input, *g));
                p.active = g->batches.back().get();
                p.generation = std::move(g);
                return S_OK;
            }
            for (auto& batch : p.generation->batches) {
                if (batch->completion <= completed && Identity(batch->input, input)) {
                    p.active = batch.get();
                    return S_OK;
                }
            }
            for (auto& batch : p.generation->batches) {
                if (batch->completion <= completed) {
                    p.BindBatch(*batch, input, *p.generation);
                    p.active = batch.get();
                    return S_OK;
                }
            }
            // Warm-up grows only immutable descriptor/reference batches. The same
            // scratch resources are ordered by the caller's single DIRECT queue.
            auto batch = p.CreateBatch(input, *p.generation);
            p.generation->batches.push_back(std::move(batch));
            p.active = p.generation->batches.back().get();
            return S_OK;
        } catch (const Failure& e) {
            return e.result;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
    }
    WorkingEye Pipeline::GetWorkingEye(uint32_t eye) const {
        if (!impl_->generation || eye >= 2)
            return {};
        const auto& s = impl_->generation->scratch[eye];
        return {s.proxy.Get(), s.answer.Get(), s.depth.Get(), s.extent};
    }
    void Pipeline::RecordInputs(ID3D12GraphicsCommandList* list) {
        auto& p = *impl_;
        assert(p.active && list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT);
        p.NativeTransitions(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true, true);
        ID3D12DescriptorHeap* heaps[]{p.active->views.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(p.generation->root.Get());
        list->SetPipelineState(p.generation->downsample.Get());
        for (UINT e = 0; e < 2; ++e) {
            const auto& s = p.generation->scratch[e];
            Transition(list, s.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (s.depth)
                Transition(list, s.depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const auto constants = p.Parameters(e);
            list->SetComputeRoot32BitConstants(0, 16, &constants, 0);
            auto handle = p.active->views->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += UINT64(e * 6) * p.viewStride;
            list->SetComputeRootDescriptorTable(1, handle);
            list->Dispatch((s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
            Transition(list, s.proxy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (s.depth)
                Transition(list, s.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
        p.NativeTransitions(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false, true);
    }
    void Pipeline::RecordResolve(ID3D12GraphicsCommandList* list) {
        auto& p = *impl_;
        assert(p.active && list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT);
        p.NativeTransitions(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true, false);
        Transition(list, p.active->input.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Vendor evaluation may replace every relevant binding. Re-establish the
        // complete draw state, including output, root constants, viewport and scissor.
        ID3D12DescriptorHeap* heaps[]{p.active->views.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(p.generation->root.Get());
        list->SetPipelineState(p.generation->resolve.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 0, nullptr);
        list->IASetIndexBuffer(nullptr);
        const auto target = p.active->targets->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        const float blend[4]{};
        list->OMSetBlendFactor(blend);
        list->OMSetStencilRef(0);
        for (UINT e = 0; e < 2; ++e) {
            const auto& s = p.generation->scratch[e];
            Transition(list, s.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(list, s.answer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const auto constants = p.Parameters(e);
            list->SetGraphicsRoot32BitConstants(0, 16, &constants, 0);
            auto handle = p.active->views->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += UINT64(e * 6) * p.viewStride;
            list->SetGraphicsRootDescriptorTable(1, handle);
            const D3D12_VIEWPORT viewport{
                float(constants.outputX), 0, float(constants.color.width), float(constants.color.height), 0, 1};
            const D3D12_RECT scissor{LONG(constants.outputX),
                                     0,
                                     LONG(constants.outputX + constants.color.width),
                                     LONG(constants.color.height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->DrawInstanced(3, 1, 0, 0);
            Transition(list, s.proxy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(list, s.answer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        Transition(list, p.active->input.output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        p.NativeTransitions(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false, false);
    }
    void Pipeline::MarkSubmitted(uint64_t value) {
        // Zero and UINT64_MAX cannot be proven completion tokens: retain ownership.
        if (!impl_->active || !value || value == UINT64_MAX)
            return;
        impl_->active->completion = value;
        impl_->active = nullptr;
    }
    void Pipeline::DiscardRecording() {
        impl_->active = nullptr;
    }
    void Pipeline::Clear() {
        impl_->active = nullptr;
        impl_->generation.reset();
    }
} // namespace virtualdesktop_openxr::nr_spatial
