#include "NrSpatialPipeline.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cassert>
#include <new>
#include <vector>
#include "NrSpatialDownsampleCS.h"
#include "NrSpatialResolvePS.h"
#include "NrNativeResolvePS.h"
#include "FullScreenQuadVS.h"
#if defined(NR_SPATIAL_TEST_OBSERVER)
#include "../tests/nr_spatial_test_observer.h"
#define NR_SPATIAL_OBSERVE(...) test_observer::__VA_ARGS__
#else
#define NR_SPATIAL_OBSERVE(...) ((void)0)
#endif

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
                a.outputFormat != b.outputFormat || a.nativeAnswer != b.nativeAnswer)
                return false;
            for (size_t i = 0; i < 2; ++i) {
                const auto& x = a.eyes[i];
                const auto& y = b.eyes[i];
                if (x.colorFormat != y.colorFormat || !Same(x.colorRect, y.colorRect) ||
                    !Same(ProcessingRect(x.colorRect, x.processingRect),
                          ProcessingRect(y.colorRect, y.processingRect)) ||
                    x.transitionWidth != y.transitionWidth || bool(x.depth) != bool(y.depth) ||
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
        D3D12_RESOURCE_DESC Description(ID3D12Resource* r) {
            NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Description, r));
            return r->GetDesc();
        }
        bool Contains(const D3D12_RESOURCE_DESC& d, Rect rect) {
            if (!rect.width || !rect.height)
                return false;
            return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize >= 1 && d.MipLevels == 1 &&
                   d.SampleDesc.Count == 1 && uint64_t(rect.x) + rect.width <= d.Width &&
                   uint64_t(rect.y) + rect.height <= d.Height;
        }
        bool ColorView(const D3D12_RESOURCE_DESC& d, DXGI_FORMAT view) {
            const auto f = d.Format;
            const auto raw = RawColorFormat(view);
            return raw == DXGI_FORMAT_R8G8B8A8_UNORM
                       ? f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                             f == DXGI_FORMAT_R8G8B8A8_TYPELESS
                       : f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                             f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        }
        bool DepthView(const D3D12_RESOURCE_DESC& d, DXGI_FORMAT view) {
            const auto f = d.Format;
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
        // A maximum of four native resources plus two per-eye scratch resources
        // can be adjacent before a dispatch/draw. Flush only at actual GPU use.
        struct Transitions {
            ID3D12GraphicsCommandList* list;
            std::array<D3D12_RESOURCE_BARRIER, 6> barriers{};
            UINT count{};
            void Add(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
                assert(count < barriers.size());
                auto& b = barriers[count++];
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            }
            void Flush() {
                if (count) {
                    NR_SPATIAL_OBSERVE(Barriers(count, barriers.data()));
                    list->ResourceBarrier(count, barriers.data());
                    count = 0;
                }
            }
        };
        struct Constants {
            Rect color, depth, processing;
            Extent work;
            UINT srgb, hasDepth, outputX, transition, reserved[2]{};
        };
        static_assert(sizeof(Constants) == 20 * sizeof(UINT));
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
            ComPtr<ID3D12Resource> nativeAnswer;
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
            if (!device || !fence || !input.output || !IsSupportedColorFormat(input.outputFormat) ||
                input.eyes[0].colorFormat != input.eyes[1].colorFormat)
                throw Failure{E_INVALIDARG};
            const auto out = Description(input.output);
            if (out.DepthOrArraySize != 1 || !(out.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
                !ColorView(out, input.outputFormat))
                throw Failure{E_INVALIDARG};
            for (UINT e = 0; e < 2; ++e) {
                const auto& eye = input.eyes[e];
                if (!eye.color)
                    throw Failure{E_INVALIDARG};
                const auto color = Description(eye.color);
                const auto crop = ProcessingRect(eye.colorRect, eye.processingRect);
                Extent work{};
                if (!crop.width || !crop.height || uint64_t(crop.x) + crop.width > eye.colorRect.width ||
                    uint64_t(crop.y) + crop.height > eye.colorRect.height ||
                    eye.transitionWidth > (std::min)(crop.width, crop.height) / 4 ||
                    (input.nativeAnswer
                         ? input.processingPercent != 100 || eye.transitionWidth != 0
                         : !TryWorkingExtent({crop.width, crop.height}, input.processingPercent, work)) ||
                    !IsCompatibleColorOutput(eye.colorFormat, input.outputFormat) || !Contains(color, eye.colorRect) ||
                    !ColorView(color, eye.colorFormat) || (color.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                    eye.color == input.output ||
                    !Contains(out, {e ? input.outputEyeStride : 0, 0, eye.colorRect.width, eye.colorRect.height}))
                    throw Failure{E_INVALIDARG};
                if (eye.depth) {
                    const auto depth = Description(eye.depth);
                    if (!Contains(depth, eye.depthRect) || !DepthView(depth, eye.depthFormat) ||
                        (depth.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || eye.depth == input.output ||
                        eye.depth == eye.color)
                        throw Failure{E_INVALIDARG};
                }
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
            parameters[0].Constants = {0, 0, 20};
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
            if (!input.nativeAnswer) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
                compute.pRootSignature = g->root.Get();
                compute.CS = {g_NrSpatialDownsampleCS, sizeof(g_NrSpatialDownsampleCS)};
                Check(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&g->downsample)));
            }
            D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
            graphics.pRootSignature = g->root.Get();
            graphics.VS = {g_FullScreenQuadVS, sizeof(g_FullScreenQuadVS)};
            graphics.PS = input.nativeAnswer
                              ? D3D12_SHADER_BYTECODE{g_NrNativeResolvePS, sizeof(g_NrNativeResolvePS)}
                              : D3D12_SHADER_BYTECODE{g_NrSpatialResolvePS, sizeof(g_NrSpatialResolvePS)};
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
            if (input.nativeAnswer) {
                // Preserve the existing vendor target's format family and dimensions,
                // including typeless BGRA. Do not silently substitute RGBA storage.
                auto answerDesc = Description(input.output);
                answerDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                Check(device->CreateCommittedResource(&heap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &answerDesc,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr,
                                                      IID_PPV_ARGS(&g->nativeAnswer)));
                return g;
            }
            for (UINT e = 0; e < 2; ++e) {
                auto& scratch = g->scratch[e];
                const auto& eye = input.eyes[e];
                const auto crop = ProcessingRect(eye.colorRect, eye.processingRect);
                TryWorkingExtent({crop.width, crop.height}, input.processingPercent, scratch.extent);
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
            // Scratch resources and formats are immutable for this generation.
            // Initialize their descriptors once, including the null depth UAV.
            for (UINT e = 0; e < 2; ++e) {
                auto handle = b->views->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += SIZE_T(e * 6 + 2) * viewStride;
                const auto& scratch = g.scratch[e];
                for (auto* resource :
                     {scratch.proxy.Get(), input.nativeAnswer ? g.nativeAnswer.Get() : scratch.answer.Get()}) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Format = input.nativeAnswer ? RawColorFormat(input.outputFormat) : DXGI_FORMAT_R8G8B8A8_UNORM;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Texture2D.MipLevels = 1;
                    NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Descriptor, resource));
                    device->CreateShaderResourceView(resource, &srv, handle);
                    handle.ptr += viewStride;
                }
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Descriptor, scratch.proxy.Get()));
                device->CreateUnorderedAccessView(scratch.proxy.Get(), nullptr, &uav, handle);
                handle.ptr += viewStride;
                uav.Format = DXGI_FORMAT_R32_FLOAT;
                NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Descriptor, scratch.depth.Get()));
                device->CreateUnorderedAccessView(scratch.depth.Get(), nullptr, &uav, handle);
            }
            BindBatch(*b, input);
            return b;
        }
        // Only called for a new batch or one whose completion was positively
        // observed. Descriptor writes cannot fail and allocate no host memory.
        void BindBatch(Batch& batch, const FrameInput& input) {
            auto* b = &batch;
            b->input = input;
            b->owners = {
                input.eyes[0].color, input.eyes[0].depth, input.eyes[1].color, input.eyes[1].depth, input.output};
            b->completion = 0;
            D3D12_RENDER_TARGET_VIEW_DESC target{};
            target.Format = RawColorFormat(input.outputFormat);
            target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Descriptor, input.output));
            device->CreateRenderTargetView(input.output, &target, b->targets->GetCPUDescriptorHandleForHeapStart());
            for (UINT e = 0; e < 2; ++e) {
                auto handle = b->views->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += SIZE_T(e * 6) * viewStride;
                auto srv = [&](ID3D12Resource* r, DXGI_FORMAT format) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
                    d.Format = format;
                    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    // Resolved slice zero can alias the app's array. Other
                    // slices are already flattened by the existing host.
                    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    d.Texture2DArray.MipLevels = 1;
                    d.Texture2DArray.ArraySize = 1;
                    NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Descriptor, r));
                    device->CreateShaderResourceView(r, &d, handle);
                    handle.ptr += viewStride;
                };
                const auto& eye = input.eyes[e];
                srv(eye.color, RawColorFormat(eye.colorFormat));
                srv(eye.depth, eye.depth ? eye.depthFormat : DXGI_FORMAT_R32_FLOAT);
            }
        }
        Constants Parameters(UINT e) const {
            const auto& eye = active->input.eyes[e];
            return {eye.colorRect,
                    eye.depthRect,
                    ProcessingRect(eye.colorRect, eye.processingRect),
                    generation->scratch[e].extent,
                    IsSrgb(eye.colorFormat),
                    bool(eye.depth),
                    e ? active->input.outputEyeStride : 0,
                    eye.transitionWidth,
                    {}};
        }
        void
        NativeTransitions(Transitions& transitions, D3D12_RESOURCE_STATES state, bool entering, bool includeDepth) {
            std::array<ID3D12Resource*, 4> seen{};
            size_t count = 0;
            for (const auto& eye : active->input.eyes) {
                for (auto* r : {eye.color, includeDepth ? eye.depth : nullptr}) {
                    if (!r || std::find(seen.begin(), seen.begin() + count, r) != seen.begin() + count)
                        continue;
                    seen[count++] = r;
                    transitions.Add(r,
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
                    p.BindBatch(*batch, input);
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
    ID3D12Resource* Pipeline::GetNativeAnswer() const {
        return impl_->generation ? impl_->generation->nativeAnswer.Get() : nullptr;
    }
    void Pipeline::RecordInputs(ID3D12GraphicsCommandList* list) {
        auto& p = *impl_;
        assert(p.active && list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT);
        assert(!p.active->input.nativeAnswer);
        if (p.active->input.nativeAnswer)
            return;
        Transitions transitions{list};
        p.NativeTransitions(transitions, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true, true);
        ID3D12DescriptorHeap* heaps[]{p.active->views.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(p.generation->root.Get());
        list->SetPipelineState(p.generation->downsample.Get());
        for (UINT e = 0; e < 2; ++e) {
            const auto& s = p.generation->scratch[e];
            transitions.Add(s.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (s.depth)
                transitions.Add(s.depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const auto constants = p.Parameters(e);
            list->SetComputeRoot32BitConstants(0, 20, &constants, 0);
            auto handle = p.active->views->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += UINT64(e * 6) * p.viewStride;
            list->SetComputeRootDescriptorTable(1, handle);
            transitions.Flush();
            NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Dispatch));
            list->Dispatch((s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
            transitions.Add(s.proxy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (s.depth)
                transitions.Add(s.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
        p.NativeTransitions(transitions, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false, true);
        transitions.Flush();
    }
    void Pipeline::RecordResolve(ID3D12GraphicsCommandList* list) {
        auto& p = *impl_;
        assert(p.active && list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT);
        Transitions transitions{list};
        p.NativeTransitions(transitions, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true, false);
        transitions.Add(p.active->input.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (p.generation->nativeAnswer)
            transitions.Add(p.generation->nativeAnswer.Get(),
                            D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
            if (!p.active->input.nativeAnswer) {
                transitions.Add(s.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                transitions.Add(
                    s.answer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            const auto constants = p.Parameters(e);
            list->SetGraphicsRoot32BitConstants(0, 20, &constants, 0);
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
            transitions.Flush();
            NR_SPATIAL_OBSERVE(Record(test_observer::Kind::Draw));
            list->DrawInstanced(3, 1, 0, 0);
            if (!p.active->input.nativeAnswer) {
                transitions.Add(s.proxy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                transitions.Add(
                    s.answer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
        }
        if (p.generation->nativeAnswer)
            transitions.Add(p.generation->nativeAnswer.Get(),
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COMMON);
        transitions.Add(p.active->input.output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        p.NativeTransitions(transitions, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false, false);
        transitions.Flush();
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
