#include "fg_gpu_helpers.h"
#include "fullscreen_vertex_shader.hpp"
#include "nvidia_synthesize_midpoint_pixel_shader.hpp"
#if defined(VDXR_FG_MRT_SHADER_AVAILABLE) || __has_include("nvidia_shared_3x_synthesize_pixel_shader.hpp")
#include "nvidia_shared_3x_synthesize_pixel_shader.hpp"
#define FG_HAS_SHARED_3X_SHADER 1
#else
#define FG_HAS_SHARED_3X_SHADER 0
#endif

#include <algorithm>

namespace {
    constexpr UINT imageWidth = 67, imageHeight = 43, eyes = 2, flowBlock = 4;
    using Pixels = std::vector<uint8_t>;
    using Pair = std::array<Pixels, 2>;
    struct Mapping {
        float rotation[4]{0, 0, 0, 1};
        float sourceTangents[4]{-1, 1, 1, -1};
        float targetTangents[4]{-1, 1, 1, -1};
        float sourceRect[4]{0, 0, float(imageWidth), float(imageHeight)};
        float targetRect[4]{0, 0, float(imageWidth), float(imageHeight)};
    };
    struct Parameters {
        UINT width{imageWidth}, height{imageHeight}, arraySize{eyes}, packedWidth{};
        UINT packedHeight{}, eyeStride{}, flowWidth{}, flowHeight{}, flowBlockSize{flowBlock};
        UINT viewIndex{}, useGameMotion{}, padding{};
        float gameMotionRect[4]{}, gameMotionScale[2]{}, gameMotionJitter[2]{};
        Mapping mappings[eyes];
        UINT slice{}, flags{};
    };
    static_assert(sizeof(Parameters) == 62 * sizeof(UINT));

    struct Fixture {
        Gpu gpu;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> single, dual;
        ComPtr<ID3D12DescriptorHeap> descriptors, targets;
        std::array<ComPtr<ID3D12Resource>, 6> resources;
        UINT descriptorStride{}, targetStride{}, flowWidth{}, flowHeight{}, eyeStride{};
        const UINT scaleBits;
        const bool srgb;

        Fixture(bool useSrgb, UINT scale) : scaleBits(scale), srgb(useSrgb) {
            const float ratio = scale == 3 ? .5f : scale == 2 ? .75f : 1.0f;
            const UINT packedWidth = (UINT(std::ceil(imageWidth * ratio)) + 3) & ~3u;
            eyeStride = (UINT(std::ceil(imageHeight * ratio)) + 3) & ~3u;
            flowWidth = packedWidth / flowBlock;
            flowHeight = eyeStride * eyes / flowBlock;
            const auto viewFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            const auto allocationFormat = srgb ? DXGI_FORMAT_R8G8B8A8_TYPELESS : viewFormat;
            D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0, 0},
                                             {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0}};
            D3D12_ROOT_PARAMETER parameters[3]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants = {0, 0, 62};
            for (UINT i = 0; i < 2; ++i) {
                parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[i + 1].DescriptorTable = {1, &ranges[i]};
            }
            D3D12_ROOT_SIGNATURE_DESC description{};
            description.NumParameters = 3;
            description.pParameters = parameters;
            D3D12_STATIC_SAMPLER_DESC linearClamp{};
            linearClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            linearClamp.AddressU = linearClamp.AddressV = linearClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            linearClamp.MaxAnisotropy = 1;
            linearClamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            linearClamp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            linearClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            description.NumStaticSamplers = 1;
            description.pStaticSamplers = &linearClamp;
            ComPtr<ID3DBlob> serialized, errors;
            success(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
                    "serialize exact 64-DWORD fixture root");
            success(gpu.device->CreateRootSignature(
                        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)),
                    "MRT fixture root");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = root.Get();
            pipeline.VS = {g_xrfg_fullscreen_vertex_shader, sizeof(g_xrfg_fullscreen_vertex_shader)};
            pipeline.PS = {g_xrfg_nvidia_synthesize_midpoint_pixel_shader,
                           sizeof(g_xrfg_nvidia_synthesize_midpoint_pixel_shader)};
            auto& blend = pipeline.BlendState.RenderTarget[0];
            blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pipeline.SampleMask = UINT_MAX;
            pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pipeline.RasterizerState.DepthClipEnable = TRUE;
            pipeline.DepthStencilState.DepthEnable = FALSE;
            pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            pipeline.DepthStencilState.StencilEnable = FALSE;
            pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline.NumRenderTargets = 1;
            pipeline.RTVFormats[0] = viewFormat;
            pipeline.SampleDesc.Count = 1;
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&single)),
                    "frozen legacy NVIDIA shader PSO");
#if FG_HAS_SHARED_3X_SHADER
            pipeline.PS = {g_xrfg_nvidia_shared_3x_synthesize_pixel_shader,
                           sizeof(g_xrfg_nvidia_shared_3x_synthesize_pixel_shader)};
            pipeline.NumRenderTargets = 2;
            pipeline.RTVFormats[1] = viewFormat;
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&dual)),
                    "candidate two-target NVIDIA shader PSO");
#endif
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = 10;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            success(gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)), "MRT shader descriptors");
            descriptorStride = gpu.device->GetDescriptorHandleIncrementSize(heap.Type);
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = 4;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            success(gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets)), "MRT target descriptors");
            targetStride = gpu.device->GetDescriptorHandleIncrementSize(heap.Type);
            resources[0] = texture(imageWidth, imageHeight, eyes, allocationFormat);
            resources[1] = texture(imageWidth, imageHeight, eyes, allocationFormat);
            resources[2] = texture(flowWidth, flowHeight, 1, DXGI_FORMAT_R32G32_SINT);
            resources[3] = texture(flowWidth, flowHeight, 1, DXGI_FORMAT_R32_UINT);
            for (UINT phase = 0; phase < 2; ++phase)
                resources[phase + 4] =
                    texture(imageWidth * 2, imageHeight, 1, allocationFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            for (UINT index = 0; index < 7; ++index) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = index < 2                  ? viewFormat
                              : index == 3 || index == 5 ? DXGI_FORMAT_R32_UINT
                                                         : DXGI_FORMAT_R32G32_SINT;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (index < 2 || index == 6) {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.ArraySize = index < 2 ? eyes : 1;
                    view.Texture2DArray.MipLevels = 1;
                    if (index == 6)
                        view.Format = DXGI_FORMAT_R32G32_FLOAT;
                } else {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    view.Texture2D.MipLevels = 1;
                }
                auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += index * descriptorStride;
                gpu.device->CreateShaderResourceView(index < 4 ? resources[index].Get() : nullptr, &view, handle);
            }
            for (UINT index = 0; index < 3; ++index) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += (index + 7) * descriptorStride;
                gpu.device->CreateUnorderedAccessView(nullptr, nullptr, &view, handle);
            }
            for (UINT phase = 0; phase < 2; ++phase)
                for (UINT eye = 0; eye < 2; ++eye) {
                    D3D12_RENDER_TARGET_VIEW_DESC view{};
                    view.Format = viewFormat;
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    gpu.device->CreateRenderTargetView(resources[phase + 4].Get(), &view, target(phase, eye));
                }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE target(UINT phase, UINT eye) const {
            auto handle = targets->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (phase * eyes + eye) * targetStride;
            return handle;
        }
        ComPtr<ID3D12Resource> texture(
            UINT w, UINT h, UINT slices, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = w;
            description.Height = h;
            description.DepthOrArraySize = static_cast<UINT16>(slices);
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Format = format;
            description.Flags = flags;
            ComPtr<ID3D12Resource> result;
            success(gpu.device->CreateCommittedResource(&heap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &description,
                                                        D3D12_RESOURCE_STATE_COMMON,
                                                        nullptr,
                                                        IID_PPV_ARGS(&result)),
                    "controlled shader texture");
            return result;
        }
        void execute(ID3D12GraphicsCommandList* list) {
            success(list->Close(), "close controlled shader commands");
            ID3D12CommandList* lists[]{list};
            gpu.queue->ExecuteCommandLists(1, lists);
            const auto value = gpu.nextFence++;
            success(gpu.queue->Signal(gpu.fence.Get(), value), "controlled shader signal");
            gpu.wait(gpu.fence.Get(), value);
            gpu.noErrors();
        }
        Pixels transfer(ID3D12Resource* resource, const void* input, UINT bytesPerPixel) {
            const auto description = resource->GetDesc();
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(description.DepthOrArraySize);
            UINT64 bytes{};
            gpu.device->GetCopyableFootprints(
                &description, 0, description.DepthOrArraySize, 0, layouts.data(), nullptr, nullptr, &bytes);
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = input ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
            ComPtr<ID3D12Resource> staging;
            success(gpu.device->CreateCommittedResource(&heap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &buffer,
                                                        input ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                              : D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr,
                                                        IID_PPV_ARGS(&staging)),
                    "controlled shader staging");
            const size_t rowBytes = size_t(description.Width) * bytesPerPixel;
            const auto copyRows = [&](void* destination, const void* source, bool upload) {
                for (UINT slice = 0; slice < description.DepthOrArraySize; ++slice)
                    for (UINT y = 0; y < description.Height; ++y) {
                        const size_t packedOffset = (slice * description.Height + y) * rowBytes;
                        const size_t paddedOffset = layouts[slice].Offset + y * layouts[slice].Footprint.RowPitch;
                        std::memcpy(static_cast<uint8_t*>(destination) + (upload ? paddedOffset : packedOffset),
                                    static_cast<const uint8_t*>(source) + (upload ? packedOffset : paddedOffset),
                                    rowBytes);
                    }
            };
            if (input) {
                void* mapped{};
                const D3D12_RANGE empty{};
                success(staging->Map(0, &empty, &mapped), "controlled upload map");
                copyRows(mapped, input, true);
                staging->Unmap(0, nullptr);
            }
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            success(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                    "controlled transfer allocator");
            success(gpu.device->CreateCommandList(
                        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
                    "controlled transfer list");
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {resource,
                                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_COMMON,
                                  input ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE};
            list->ResourceBarrier(1, &barrier);
            for (UINT slice = 0; slice < description.DepthOrArraySize; ++slice) {
                D3D12_TEXTURE_COPY_LOCATION textureLocation{}, bufferLocation{};
                textureLocation.pResource = resource;
                textureLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                textureLocation.SubresourceIndex = slice;
                bufferLocation.pResource = staging.Get();
                bufferLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                bufferLocation.PlacedFootprint = layouts[slice];
                list->CopyTextureRegion(input ? &textureLocation : &bufferLocation,
                                        0,
                                        0,
                                        0,
                                        input ? &bufferLocation : &textureLocation,
                                        nullptr);
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
            execute(list.Get());
            Pixels result;
            if (!input) {
                result.resize(rowBytes * description.Height * description.DepthOrArraySize);
                void* mapped{};
                const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
                success(staging->Map(0, &range, &mapped), "controlled readback map");
                copyRows(result.data(), mapped, false);
                const D3D12_RANGE empty{};
                staging->Unmap(0, &empty);
            }
            return result;
        }
        Parameters parameters(UINT scenario) const {
            Parameters result;
            result.packedWidth = flowWidth * flowBlock;
            result.packedHeight = flowHeight * flowBlock;
            result.eyeStride = eyeStride;
            result.flowWidth = flowWidth;
            result.flowHeight = flowHeight;
            result.flags = (scaleBits << 2) | 0x10u | (srgb ? 2u : 0u);
            for (UINT view = 0; view < eyes; ++view) {
                auto& mapping = result.mappings[view];
                if (scenario >= 2) {
                    mapping.targetRect[0] = float(view ? 7 : 3);
                    mapping.targetRect[1] = float(view ? 2 : 5);
                    mapping.targetRect[2] = float(imageWidth - (view ? 13 : 9));
                    mapping.targetRect[3] = float(imageHeight - (view ? 7 : 11));
                    std::copy_n(mapping.targetRect, 4, mapping.sourceRect);
                }
                if (scenario >= 4) {
                    const float halfYaw = view ? -.021f : .013f;
                    mapping.rotation[1] = std::sin(halfYaw);
                    mapping.rotation[3] = std::cos(halfYaw);
                    mapping.sourceTangents[0] = view ? -1.07f : -.94f;
                    mapping.sourceTangents[1] = view ? .92f : 1.09f;
                }
                if (scenario == 6) {
                    mapping.sourceRect[0] += 5;
                    mapping.sourceRect[2] -= 9;
                }
            }
            return result;
        }
        void inputs(UINT scenario) {
            Pixels previous(imageWidth * imageHeight * eyes * 4), current(previous.size());
            for (UINT eye = 0; eye < eyes; ++eye)
                for (UINT y = 0; y < imageHeight; ++y)
                    for (UINT x = 0; x < imageWidth; ++x) {
                        const size_t offset = ((eye * imageHeight + y) * imageWidth + x) * 4;
                        for (UINT channel = 0; channel < 3; ++channel) {
                            const UINT value = (x * (channel + 3) + y * (channel + 5) + eye * 71 + channel * 19) % 256;
                            previous[offset + channel] = uint8_t(scenario == 7 ? value % 23 : value);
                            current[offset + channel] =
                                uint8_t(scenario == 7 ? (value + 3) % 23 : (value + 13 + x % 7) % 256);
                        }
                        previous[offset + 3] = scenario < 2 ? 0 : uint8_t((x * 7 + y * 11 + eye * 53) % 256);
                        current[offset + 3] = scenario < 2 ? 255 : uint8_t((x * 13 + y * 3 + eye * 79) % 256);
                    }
            std::vector<int32_t> flow(flowWidth * flowHeight * 2);
            std::vector<uint32_t> cost(flowWidth * flowHeight);
            constexpr uint32_t mixed[4]{0, 255, 64, 192};
            for (UINT y = 0; y < flowHeight; ++y)
                for (UINT x = 0; x < flowWidth; ++x) {
                    const size_t offset = y * flowWidth + x;
                    const bool reverse = ((x + y) & 1) != 0;
                    flow[offset * 2] = scenario == 0 ? 0
                                       : scenario == 1
                                           ? 192
                                           : (reverse ? -1 : 1) * (scenario == 3 ? 32760 : int(32 + (x % 5) * 23));
                    flow[offset * 2 + 1] = scenario < 2 ? 0 : int(y % 3) * 31 - 32;
                    cost[offset] = scenario == 0                    ? 0
                                   : scenario == 1 || scenario == 6 ? 255
                                   : scenario == 3                  ? 127
                                                                    : mixed[(x + y * 3) % 4];
                }
            transfer(resources[0].Get(), previous.data(), 4);
            transfer(resources[1].Get(), current.data(), 4);
            transfer(resources[2].Get(), flow.data(), 8);
            transfer(resources[3].Get(), cost.data(), 4);
        }
        Pair render(Parameters parameters, std::array<UINT, 2> packedPhases, bool shared, bool reversed) {
            check(!shared || dual != nullptr, "normal NVIDIA shared-3x MRT shader is not available");
            Pixels seed(imageWidth * 2 * imageHeight * 4);
            for (size_t i = 0; i < seed.size(); ++i)
                seed[i] = uint8_t((i * 29 + 17) % 256);
            for (UINT phase = 0; phase < 2; ++phase)
                transfer(resources[phase + 4].Get(), seed.data(), 4);
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            success(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                    "controlled render allocator");
            success(gpu.device->CreateCommandList(0,
                                                  D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  allocator.Get(),
                                                  shared ? dual.Get() : single.Get(),
                                                  IID_PPV_ARGS(&list)),
                    "controlled render list");
            std::array<D3D12_RESOURCE_BARRIER, 6> barriers{};
            for (UINT i = 0; i < resources.size(); ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition = {resources[i].Get(),
                                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_COMMON,
                                          i < 4 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                : D3D12_RESOURCE_STATE_RENDER_TARGET};
            }
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
            list->SetGraphicsRootSignature(root.Get());
            ID3D12DescriptorHeap* heaps[]{descriptors.Get()};
            list->SetDescriptorHeaps(1, heaps);
            auto table = descriptors->GetGPUDescriptorHandleForHeapStart();
            list->SetGraphicsRootDescriptorTable(1, table);
            table.ptr += 7 * descriptorStride;
            list->SetGraphicsRootDescriptorTable(2, table);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT lowFlags = parameters.flags & 0xffu;
            parameters.padding = packedPhases[1] << 8;
            for (UINT phase = 0; phase < (shared ? 1u : 2u); ++phase) {
                parameters.flags = lowFlags | (packedPhases[phase] << 8);
                for (UINT view = 0; view < eyes; ++view) {
                    const UINT eye = reversed ? 1 - view : view;
                    parameters.slice = eye;
                    parameters.viewIndex = view;
                    list->SetGraphicsRoot32BitConstants(0, 62, &parameters, 0);
                    const auto& rect = parameters.mappings[view].targetRect;
                    const float origin = float(eye * imageWidth);
                    D3D12_VIEWPORT viewport{origin + rect[0], rect[1], rect[2], rect[3], 0, 1};
                    D3D12_RECT scissor{LONG(origin + rect[0]),
                                       LONG(rect[1]),
                                       LONG(origin + rect[0] + rect[2]),
                                       LONG(rect[1] + rect[3])};
                    list->RSSetViewports(1, &viewport);
                    list->RSSetScissorRects(1, &scissor);
                    const D3D12_CPU_DESCRIPTOR_HANDLE handles[]{target(phase, eye), target(1, eye)};
                    list->OMSetRenderTargets(shared ? 2 : 1, handles, FALSE, nullptr);
                    list->DrawInstanced(3, 1, 0, 0);
                }
            }
            for (auto& barrier : barriers)
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
            execute(list.Get());
            return {transfer(resources[4].Get(), nullptr, 4), transfer(resources[5].Get(), nullptr, 4)};
        }
    };
} // namespace

int main() {
    try {
        // Literal independently quantized phase fields. The second pair cannot
        // be implemented as 1-f1; zero retains the legacy midpoint sentinel.
        constexpr std::array<std::array<UINT, 2>, 5> phases{
            {{21845, 43690}, {21846, 43690}, {1, 65535}, {9830, 49152}, {0, 32768}}};
        unsigned comparisons{};
        for (bool srgb : {false, true})
            for (UINT scale : {1u, 2u, 3u}) {
                Fixture fixture(srgb, scale);
                for (UINT scenario = 0; scenario < 8; ++scenario) {
                    fixture.inputs(scenario);
                    for (const auto& phase : phases) {
                        const auto parameters = fixture.parameters(scenario);
                        const auto reference = fixture.render(parameters, phase, false, (scenario & 1) != 0);
                        if (scenario < 2 && phase == phases[0]) {
                            const size_t centerAlpha = ((imageHeight / 2) * imageWidth * 2 + imageWidth / 2) * 4 + 3;
                            check(reference[0][centerAlpha] == 85 && reference[1][centerAlpha] == 170,
                                  "frozen legacy phases retain literal 1/3 and 2/3 alpha, including uniform cost255 "
                                  "with nonzero motion");
                        }
                        const auto candidate = fixture.render(parameters, phase, true, (scenario & 1) != 0);
                        for (UINT output = 0; output < 2; ++output) {
                            if (reference[output] != candidate[output]) {
                                const auto mismatch = std::mismatch(
                                    reference[output].begin(), reference[output].end(), candidate[output].begin());
                                const auto byte = size_t(mismatch.first - reference[output].begin());
                                std::fprintf(stderr,
                                             "MRT mismatch srgb=%u scale=%u scenario=%u packed=(%u,%u) phase=%u "
                                             "pixel=(%zu,%zu) channel=%zu legacy=%u candidate=%u\n",
                                             srgb,
                                             scale,
                                             scenario,
                                             phase[0],
                                             phase[1],
                                             output,
                                             (byte / 4) % (imageWidth * 2),
                                             byte / (imageWidth * 2 * 4),
                                             byte % 4,
                                             *mismatch.first,
                                             *mismatch.second);
                                throw std::runtime_error("shared synthesis differs from frozen legacy RGBA bytes");
                            }
                            ++comparisons;
                        }
                    }
                }
                fixture.gpu.noErrors();
            }
        std::printf("PASS: %u exact RGBA controlled-flow phase comparisons including seeded undrawn padding\n",
                    comparisons);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
