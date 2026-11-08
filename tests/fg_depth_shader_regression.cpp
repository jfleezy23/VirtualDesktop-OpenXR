#include "fg_gpu_helpers.h"
#include "fullscreen_vertex_shader.hpp"
#include "nvidia_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_depth_synthesize_pixel_shader.hpp"
#include "nvidia_depth_shared_synthesize_pixel_shader.hpp"
#include "nvidia_shared_3x_synthesize_pixel_shader.hpp"

#include <algorithm>
#include <map>

namespace {
    constexpr UINT srvCount = 9, uavCount = 3, descriptorCount = 13;
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
        ComPtr<ID3D12PipelineState> baseline, baselineDual, single, dual;
        ComPtr<ID3D12Resource> guidance;
        ComPtr<ID3D12DescriptorHeap> descriptors, targets;
        std::array<ComPtr<ID3D12Resource>, 8> resources;
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
            D3D12_DESCRIPTOR_RANGE ranges[3]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvCount, 0, 0, 0},
                                             {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, 12},
                                             {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavCount, 0, 0, 0}};
            D3D12_ROOT_PARAMETER parameters[3]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants = {0, 0, 62};
            for (UINT i = 0; i < 2; ++i) {
                parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[i + 1].DescriptorTable = {i == 0 ? 2u : 1u, &ranges[i == 0 ? 0 : 2]};
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
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&baseline)),
                    "frozen legacy NVIDIA shader PSO");
            pipeline.PS = {g_xrfg_nvidia_depth_synthesize_pixel_shader,
                           sizeof(g_xrfg_nvidia_depth_synthesize_pixel_shader)};
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&single)), "depth single PSO");
            pipeline.PS = {g_xrfg_nvidia_depth_shared_synthesize_pixel_shader,
                           sizeof(g_xrfg_nvidia_depth_shared_synthesize_pixel_shader)};
            pipeline.NumRenderTargets = 2;
            pipeline.RTVFormats[1] = viewFormat;
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&dual)),
                    "candidate two-target NVIDIA shader PSO");
            pipeline.PS = {g_xrfg_nvidia_shared_3x_synthesize_pixel_shader,
                           sizeof(g_xrfg_nvidia_shared_3x_synthesize_pixel_shader)};
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&baselineDual)),
                    "frozen legacy two-target NVIDIA shader PSO");
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = descriptorCount * eyes;
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
            resources[6] = texture(imageWidth * eyes, imageHeight, 1, DXGI_FORMAT_R32_FLOAT);
            resources[7] = texture(imageWidth * eyes, imageHeight, 1, DXGI_FORMAT_R32_FLOAT);
            D3D12_HEAP_PROPERTIES cbHeap{};
            cbHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC cbDesc{};
            cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            cbDesc.Width = 256 * eyes;
            cbDesc.Height = cbDesc.DepthOrArraySize = cbDesc.MipLevels = 1;
            cbDesc.SampleDesc.Count = 1;
            cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            success(gpu.device->CreateCommittedResource(&cbHeap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &cbDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr,
                                                        IID_PPV_ARGS(&guidance)),
                    "depth guidance buffer");
            for (UINT eye = 0; eye < eyes; ++eye) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbView{guidance->GetGPUVirtualAddress() + eye * 256, 256};
                auto cbHandle = descriptors->GetCPUDescriptorHandleForHeapStart();
                cbHandle.ptr += (eye * descriptorCount + 12) * descriptorStride;
                gpu.device->CreateConstantBufferView(&cbView, cbHandle);
                for (UINT index = 0; index < srvCount; ++index) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                    view.Format = index < 2                  ? viewFormat
                                  : index == 3 || index == 5 ? DXGI_FORMAT_R32_UINT
                                                             : DXGI_FORMAT_R32G32_SINT;
                    if (index >= 7)
                        view.Format = DXGI_FORMAT_R32_FLOAT;
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
                    handle.ptr += (eye * descriptorCount + index) * descriptorStride;
                    gpu.device->CreateShaderResourceView(index < 4    ? resources[index].Get()
                                                         : index >= 7 ? resources[index - 1].Get()
                                                                      : nullptr,
                                                         &view,
                                                         handle);
                }
                for (UINT index = 0; index < uavCount; ++index) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                    view.Format = index == 3 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
                    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
                    handle.ptr += (eye * descriptorCount + index + srvCount) * descriptorStride;
                    gpu.device->CreateUnorderedAccessView(nullptr, nullptr, &view, handle);
                }
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
        void inputs(const std::array<Pixels, 6>& bytes) {
            constexpr UINT resourceIndices[]{0, 1, 2, 3, 6, 7};
            constexpr UINT pixelBytes[]{4, 4, 8, 4, 4, 4};
            for (UINT i = 0; i < 6; ++i)
                transfer(resources[resourceIndices[i]].Get(), bytes[i].data(), pixelBytes[i]);
        }
        void setGuidance(const Pixels& bytes, bool enabled) {
            void* mapped{};
            const D3D12_RANGE empty{};
            success(guidance->Map(0, &empty, &mapped), "depth guidance upload");
            std::memset(mapped, 0, 256 * eyes);
            for (UINT eye = 0; eye < eyes; ++eye) {
                auto* destination = static_cast<uint8_t*>(mapped) + eye * 256;
                std::memcpy(destination, bytes.data() + eye * 48, 48);
                if (!enabled)
                    std::memset(destination, 0, sizeof(UINT));
            }
            guidance->Unmap(0, nullptr);
        }
        Pair render(
            Parameters parameters, std::array<UINT, 2> packedPhases, bool shared, bool reversed, bool legacy = false) {
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
                                                  legacy ? (shared ? baselineDual.Get() : baseline.Get())
                                                         : (shared ? dual.Get() : single.Get()),
                                                  IID_PPV_ARGS(&list)),
                    "controlled render list");
            std::array<D3D12_RESOURCE_BARRIER, 8> barriers{};
            for (UINT i = 0; i < resources.size(); ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition = {resources[i].Get(),
                                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_COMMON,
                                          (i < 4 || i >= 6) ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                            : D3D12_RESOURCE_STATE_RENDER_TARGET};
            }
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
            list->SetGraphicsRootSignature(root.Get());
            ID3D12DescriptorHeap* heaps[]{descriptors.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT lowFlags = parameters.flags & 0xffu;
            parameters.padding = packedPhases[1] << 8;
            for (UINT phase = 0; phase < (shared ? 1u : 2u); ++phase) {
                parameters.flags = lowFlags | (packedPhases[phase] << 8);
                for (UINT view = 0; view < eyes; ++view) {
                    const UINT eye = reversed ? 1 - view : view;
                    auto table = descriptors->GetGPUDescriptorHandleForHeapStart();
                    table.ptr += view * descriptorCount * descriptorStride;
                    list->SetGraphicsRootDescriptorTable(1, table);
                    table.ptr += srvCount * descriptorStride;
                    list->SetGraphicsRootDescriptorTable(2, table);
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

namespace {
    Pixels readBytes(const std::filesystem::path& path, size_t expected) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        check(bool(input), ("cannot open fixture buffer: " + path.string()).c_str());
        const auto length = input.tellg();
        check(length >= 0 && static_cast<size_t>(length) == expected,
              ("wrong fixture byte length: " + path.string()).c_str());
        Pixels result(expected);
        input.seekg(0);
        input.read(reinterpret_cast<char*>(result.data()), std::streamsize(result.size()));
        check(bool(input), "incomplete fixture read");
        return result;
    }
    void writeBytes(const std::filesystem::path& path, const Pixels& bytes) {
        std::ofstream output(path, std::ios::binary);
        check(bool(output), "cannot create native output buffer");
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        check(bool(output), "incomplete native output write");
    }
    constexpr size_t atlasPixels = imageWidth * eyes * imageHeight;
    struct Replay {
        std::filesystem::path directory;
        std::map<std::string, std::string> manifest;
        std::array<Pixels, 6> inputs;
        Parameters parameters;
        Pixels guidance = Pixels(96);
        std::map<UINT, Pixels> masks, replacements, d0Masks, d1Masks, truth;
        explicit Replay(const std::filesystem::path& path) : directory(path) {
            std::ifstream file(path / "manifest.txt");
            check(bool(file), "fixture manifest.txt required");
            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                const auto delimiter = line.find('=');
                check(delimiter != std::string::npos && delimiter > 0, "manifest requires key=value lines");
                check(manifest.emplace(line.substr(0, delimiter), line.substr(delimiter + 1)).second,
                      "duplicate manifest key");
            }
            constexpr const char* names[]{
                "previous.bin", "current.bin", "flow.bin", "cost.bin", "depth_previous.bin", "depth_current.bin"};
            constexpr size_t sizes[]{imageWidth * imageHeight * eyes * 4,
                                     imageWidth * imageHeight * eyes * 4,
                                     17 * 22 * 8,
                                     17 * 22 * 4,
                                     atlasPixels * 4,
                                     atlasPixels * 4};
            for (UINT i = 0; i < 6; ++i)
                inputs[i] = readBytes(path / names[i], sizes[i]);
            parameters.packedWidth = 68;
            parameters.packedHeight = 88;
            parameters.eyeStride = 44;
            parameters.flowWidth = 17;
            parameters.flowHeight = 22;
            parameters.flags = 0x14;
            if (std::filesystem::exists(path / "parameters.bin")) {
                const auto bytes = readBytes(path / "parameters.bin", sizeof(parameters));
                std::memcpy(&parameters, bytes.data(), bytes.size());
            }
            check(parameters.width == imageWidth && parameters.height == imageHeight && parameters.arraySize == eyes &&
                      parameters.packedWidth == 68 && parameters.packedHeight == 88 && parameters.eyeStride == 44 &&
                      parameters.flowWidth == 17 && parameters.flowHeight == 22 && parameters.flowBlockSize == 4,
                  "fixture geometry must match controlled 67x43 stereo scale1 binding");
            const UINT enabled = 1;
            const float identity = 1;
            std::memcpy(guidance.data(), &enabled, 4);
            std::memcpy(guidance.data() + 28, &identity, 4);
            if (std::filesystem::exists(path / "guidance.bin")) {
                const auto bytes = readBytes(path / "guidance.bin", 48);
                std::copy(bytes.begin(), bytes.end(), guidance.begin());
            }
            std::copy_n(guidance.begin(), 48, guidance.begin() + 48);
            if (std::filesystem::exists(path / "guidance_eye1.bin")) {
                const auto bytes = readBytes(path / "guidance_eye1.bin", 48);
                std::copy(bytes.begin(), bytes.end(), guidance.begin() + 48);
            }
            for (UINT phase : {16384u, 21845u, 32768u, 43690u, 49151u}) {
                const auto stem = "phase_" + std::to_string(phase);
                // The unversioned mask is the exported active policy. Older
                // D0/D1/D2 masks remain diagnostic evidence, not the oracle
                // for a later independently exported operator.
                masks.emplace(phase, readBytes(path / (stem + "_mask.bin"), atlasPixels));
                replacements.emplace(phase, readBytes(path / (stem + "_rgba.bin"), atlasPixels * 4));
                for (auto mask : masks.at(phase))
                    check(mask <= 1, "activation mask values must be zero or one");
                if (std::filesystem::exists(path / (stem + "_d0_mask.bin")))
                    d0Masks.emplace(phase, readBytes(path / (stem + "_d0_mask.bin"), atlasPixels));
                if (std::filesystem::exists(path / (stem + "_d1_mask.bin")))
                    d1Masks.emplace(phase, readBytes(path / (stem + "_d1_mask.bin"), atlasPixels));
                if (std::filesystem::exists(path / ("truth_" + std::to_string(phase) + ".bin")))
                    truth.emplace(phase,
                                  readBytes(path / ("truth_" + std::to_string(phase) + ".bin"), atlasPixels * 4));
            }
        }
    };
    size_t compare(const Pixels& expected, const Pixels& actual, const char* label, UINT phase) {
        check(expected.size() == actual.size(), "comparison byte lengths differ");
        const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin());
        if (mismatch.first != expected.end()) {
            const size_t byte = size_t(mismatch.first - expected.begin());
            std::fprintf(stderr,
                         "%s phase=%u pixel=(%zu,%zu) channel=%zu expected=%u actual=%u\n",
                         label,
                         phase,
                         (byte / 4) % (imageWidth * eyes),
                         byte / (imageWidth * eyes * 4),
                         byte % 4,
                         *mismatch.first,
                         *mismatch.second);
            throw std::runtime_error("native exact RGBA comparison failed");
        }
        return actual.size();
    }
    uint64_t rgbError(const Pixels& rgba, const Pixels& truth) {
        uint64_t result{};
        for (size_t i = 0; i < rgba.size(); ++i)
            if (i % 4 != 3)
                result += uint64_t(std::abs(int(rgba[i]) - int(truth[i])));
        return result;
    }

    // Red contract test for a proposed, explicitly negotiated opaque-input mode.
    // Header.x becomes named flags: bit 0 enables guidance, bit 1 says source
    // alpha is semantically ignored. No reserved CB words are used. The retained
    // D3 shaders do not implement this contract and are expected to fail it.
    // This shader-boundary test does not establish host/API negotiation correctness.
    void opaqueInputContract(Fixture& fixture, const Replay& replay) {
        const auto quality = replay.manifest.find("quality");
        check(quality != replay.manifest.end() && quality->second == "improves",
              "opaque contract requires an independently exported positive D3 fixture");
        for (UINT endpoint : {0u, 1u})
            for (size_t byte = 3; byte < replay.inputs[endpoint].size(); byte += 4)
                check(replay.inputs[endpoint][byte] == 255, "positive fixture must have opaque stored alpha");
        constexpr UINT guideEnabled = 1, semanticOpaque = 2;
        constexpr std::array<std::array<UINT, 2>, 3> phasesList{{{16384, 49151}, {21845, 43690}, {32768, 32768}}};
        std::array<size_t, eyes> positiveChanges{};
        size_t contractMismatches{};
        const auto setFlags = [&](UINT flags) {
            auto guidance = replay.guidance;
            for (UINT eye = 0; eye < eyes; ++eye)
                std::memcpy(guidance.data() + eye * 48, &flags, sizeof(flags));
            fixture.setGuidance(guidance, true);
        };
        const auto withAlpha = [&](uint8_t alpha) {
            auto inputs = replay.inputs;
            for (UINT endpoint : {0u, 1u})
                for (size_t byte = 3; byte < inputs[endpoint].size(); byte += 4)
                    inputs[endpoint][byte] = alpha;
            return inputs;
        };
        const auto contractCompare =
            [&](const Pixels& expected, const Pixels& actual, const char* label, UINT phase, bool shared, UINT alpha) {
                size_t mismatches{};
                for (size_t byte = 0; byte < expected.size(); ++byte)
                    mismatches += expected[byte] != actual[byte];
                contractMismatches += mismatches;
                std::printf("opaque_contract=%s phase=%u shared=%u stored_alpha=%u mismatched_bytes=%zu\n",
                            label,
                            phase,
                            UINT(shared),
                            alpha,
                            mismatches);
            };
        for (const auto& phases : phasesList) {
            fixture.inputs(replay.inputs);
            setFlags(guideEnabled);
            const auto baseline255 = fixture.render(replay.parameters, phases, false, false, true);
            const auto guided255 = fixture.render(replay.parameters, phases, false, false);
            const auto shared255 = fixture.render(replay.parameters, phases, true, false);
            for (UINT role = 0; role < 2; ++role) {
                auto expected = baseline255[role];
                const auto& mask = replay.masks.at(phases[role]);
                const auto& replacements = replay.replacements.at(phases[role]);
                for (size_t pixel = 0; pixel < atlasPixels; ++pixel) {
                    if (mask[pixel])
                        std::copy_n(replacements.data() + pixel * 4, 3, expected.data() + pixel * 4);
                    bool changed{};
                    for (UINT channel = 0; channel < 3; ++channel)
                        changed |= expected[pixel * 4 + channel] != baseline255[role][pixel * 4 + channel];
                    positiveChanges[(pixel % (imageWidth * eyes)) / imageWidth] += changed;
                }
                compare(expected, guided255[role], "opaque alpha255 independent positive oracle", phases[role]);
                compare(guided255[role], shared255[role], "opaque alpha255 single/shared", phases[role]);
            }
            // First prove the true alpha-aware controls, including fully
            // transparent and partially transparent inputs, before testing the
            // new semantics. Removing the existing guards would fail these.
            for (uint8_t alpha : {uint8_t(0), uint8_t(128)}) {
                fixture.inputs(withAlpha(alpha));
                setFlags(guideEnabled);
                const auto baseline = fixture.render(replay.parameters, phases, false, false, true);
                for (bool shared : {false, true}) {
                    const auto guarded = fixture.render(replay.parameters, phases, shared, false);
                    for (UINT role = 0; role < 2; ++role)
                        compare(baseline[role], guarded[role], "true transparency preserved", phases[role]);
                }
                setFlags(guideEnabled | semanticOpaque);
                for (bool shared : {false, true}) {
                    const auto guided = fixture.render(replay.parameters, phases, shared, false);
                    for (UINT role = 0; role < 2; ++role) {
                        auto expected = guided255[role];
                        // The requested fix changes eligibility, never alpha.
                        // Compare the entire image, including inactive pixels.
                        for (size_t byte = 3; byte < expected.size(); byte += 4)
                            expected[byte] = baseline[role][byte];
                        contractCompare(expected, guided[role], "semantic-opaque", phases[role], shared, alpha);
                    }
                }
            }
            // Opacity must not enable a disabled guide, and must not turn a
            // repeated capture into a geometric depth correspondence.
            for (UINT guard = 0; guard < 2; ++guard) {
                fixture.inputs(replay.inputs);
                auto parameters = replay.parameters;
                if (guard == 1)
                    parameters.flags |= 1;
                setFlags(guard == 0 ? semanticOpaque : guideEnabled | semanticOpaque);
                for (bool shared : {false, true}) {
                    const auto baseline = fixture.render(parameters, phases, shared, false, true);
                    const auto guided = fixture.render(parameters, phases, shared, false);
                    for (UINT role = 0; role < 2; ++role)
                        contractCompare(baseline[role],
                                        guided[role],
                                        guard == 0 ? "disabled-guide" : "repeated-capture",
                                        phases[role],
                                        shared,
                                        255);
                }
            }
        }
        for (UINT eye = 0; eye < eyes; ++eye) {
            std::printf("opaque positive control eye=%u changed_pixels=%zu\n", eye, positiveChanges[eye]);
            check(positiveChanges[eye] > 0, "positive control did not exercise depth correction in both eyes");
        }
        fixture.gpu.noErrors();
        check(contractMismatches == 0, "semantic opaque-input contract differs from exact native control");
        std::printf("PASS: semantic opaque RGB invariant to stored alpha; alpha-aware, disabled and repeated guards "
                    "preserved\n");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::filesystem::path> directories;
        std::filesystem::path outputDirectory;
        bool validateOnly = false;
        bool opaqueContract = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--validate-fixtures")
                validateOnly = true;
            else if (argument == "--opaque-input-contract")
                opaqueContract = true;
            else if (argument == "--fixture-dir" && i + 1 < argc)
                directories.emplace_back(argv[++i]);
            else if (argument == "--output-dir" && i + 1 < argc)
                outputDirectory = argv[++i];
            else
                throw std::runtime_error(
                    "usage: fg_depth_shader_regression [--validate-fixtures] [--opaque-input-contract] --fixture-dir "
                    "DIR [--fixture-dir DIR] [--output-dir NEW_DIR]");
        }
        check(!directories.empty(), "at least one --fixture-dir is required");
        check(!opaqueContract || outputDirectory.empty(),
              "opaque contract reports comparisons directly; --output-dir is unsupported");
        std::vector<Replay> replays;
        for (const auto& directory : directories)
            replays.emplace_back(directory);
        if (validateOnly) {
            std::printf("PASS: %zu fixture manifests/buffer layouts validated; GPU not executed\n", replays.size());
            return 0;
        }
        if (!outputDirectory.empty()) {
            check(!std::filesystem::exists(outputDirectory), "output directory must be new");
            std::filesystem::create_directories(outputDirectory);
        }
        Fixture fixture(false, 1);
        if (opaqueContract) {
            for (const auto& replay : replays)
                opaqueInputContract(fixture, replay);
            return 0;
        }
        size_t comparedBytes{}, changedPixels{}, qualityFailures{};
        constexpr std::array<std::array<UINT, 2>, 3> pairs{{{16384, 49151}, {21845, 43690}, {32768, 32768}}};
        for (size_t caseIndex = 0; caseIndex < replays.size(); ++caseIndex) {
            const auto& replay = replays[caseIndex];
            fixture.inputs(replay.inputs);
            size_t caseChanged{};
            uint64_t caseErrorBefore{}, caseErrorAfter{};
            for (const auto& phases : pairs) {
                fixture.setGuidance(replay.guidance, false);
                const auto baseline = fixture.render(replay.parameters, phases, false, false, true);
                const auto disabled = fixture.render(replay.parameters, phases, false, false);
                const auto disabledShared = fixture.render(replay.parameters, phases, true, false);
                fixture.setGuidance(replay.guidance, true);
                const auto single = fixture.render(replay.parameters, phases, false, false);
                const auto shared = fixture.render(replay.parameters, phases, true, false);
                for (UINT role = 0; role < 2; ++role) {
                    const UINT phase = phases[role];
                    if (!outputDirectory.empty()) {
                        const auto prefix = "case_" + std::to_string(caseIndex) + "_phase_" + std::to_string(phase);
                        writeBytes(outputDirectory / (prefix + "_baseline.bin"), baseline[role]);
                        writeBytes(outputDirectory / (prefix + "_single.bin"), single[role]);
                        writeBytes(outputDirectory / (prefix + "_shared.bin"), shared[role]);
                    }
                    comparedBytes += compare(baseline[role], disabled[role], "disabled single baseline", phase);
                    comparedBytes += compare(baseline[role], disabledShared[role], "disabled shared baseline", phase);
                    comparedBytes += compare(single[role], shared[role], "single/shared", phase);
                    Pixels expected = baseline[role];
                    size_t activated{}, changed{}, d0Activated{}, d1Activated{};
                    const auto& mask = replay.masks.at(phase);
                    const auto& replacement = replay.replacements.at(phase);
                    for (size_t pixel = 0; pixel < atlasPixels; ++pixel) {
                        if (mask[pixel]) {
                            ++activated;
                            std::copy_n(replacement.data() + pixel * 4, 3, expected.data() + pixel * 4);
                        }
                        bool rgbChanged = false;
                        for (UINT channel = 0; channel < 3; ++channel)
                            rgbChanged =
                                rgbChanged || baseline[role][pixel * 4 + channel] != single[role][pixel * 4 + channel];
                        changed += rgbChanged;
                        check(baseline[role][pixel * 4 + 3] == single[role][pixel * 4 + 3],
                              "guidance changed native alpha");
                    }
                    if (replay.d0Masks.count(phase))
                        d0Activated =
                            std::count(replay.d0Masks.at(phase).begin(), replay.d0Masks.at(phase).end(), uint8_t(1));
                    if (replay.d1Masks.count(phase))
                        d1Activated =
                            std::count(replay.d1Masks.at(phase).begin(), replay.d1Masks.at(phase).end(), uint8_t(1));
                    if (!outputDirectory.empty())
                        writeBytes(outputDirectory / ("case_" + std::to_string(caseIndex) + "_phase_" +
                                                      std::to_string(phase) + "_expected.bin"),
                                   expected);
                    comparedBytes += compare(expected, single[role], "CPU guided mask/replacement overlay", phase);
                    changedPixels += changed;
                    caseChanged += changed;
                    std::printf("case=%zu phase=%u cpu_d0_active=%zu cpu_d1_active=%zu cpu_guided_active=%zu "
                                "native_changed=%zu alpha_exact=true\n",
                                caseIndex,
                                phase,
                                d0Activated,
                                d1Activated,
                                activated,
                                changed);
                    if (replay.truth.count(phase)) {
                        const auto before = rgbError(baseline[role], replay.truth.at(phase));
                        const auto after = rgbError(single[role], replay.truth.at(phase));
                        caseErrorBefore += before;
                        caseErrorAfter += after;
                        qualityFailures += after > before;
                        std::printf("quality case=%zu phase=%u baseline_rgb_l1=%llu guided_rgb_l1=%llu status=%s\n",
                                    caseIndex,
                                    phase,
                                    static_cast<unsigned long long>(before),
                                    static_cast<unsigned long long>(after),
                                    after > before ? "FAIL" : "PASS");
                    }
                }
            }
            const auto quality = replay.manifest.find("quality");
            if (quality != replay.manifest.end() && quality->second == "improves") {
                check(caseChanged > 0, "quality fixture failed to exercise depth guidance");
                check(!replay.truth.empty() && caseErrorAfter < caseErrorBefore,
                      "quality fixture did not reduce analytic RGB error");
            }
            if (quality != replay.manifest.end() && quality->second == "preserves")
                check(caseChanged == 0, "guard fixture changed baseline bytes");
        }
        // These guards must remain native-byte identical even when guidance is enabled.
        for (UINT guard = 0; guard < 4; ++guard) {
            auto inputs = replays.front().inputs;
            auto parameters = replays.front().parameters;
            if (guard < 2) {
                const float depth = guard == 0 ? 1.0f : -1.0f;
                for (UINT endpoint : {4u, 5u})
                    for (size_t byte = 0; byte < inputs[endpoint].size(); byte += 4)
                        std::memcpy(inputs[endpoint].data() + byte, &depth, 4);
            } else if (guard == 2) {
                parameters.flags |= 1;
            } else {
                for (size_t byte = 3; byte < inputs[1].size(); byte += 4)
                    inputs[1][byte] = 128;
            }
            fixture.inputs(inputs);
            fixture.setGuidance(replays.front().guidance, true);
            for (const auto& phases : pairs) {
                const auto baseline = fixture.render(parameters, phases, false, false, true);
                const auto single = fixture.render(parameters, phases, false, false);
                const auto shared = fixture.render(parameters, phases, true, false);
                // Production never selects MRT for repeated captures. Verify
                // its unchanged legacy policy separately from the single-
                // output repeat path that production actually dispatches.
                const auto sharedReference =
                    guard == 2 ? fixture.render(parameters, phases, true, false, true) : baseline;
                for (UINT role = 0; role < 2; ++role) {
                    comparedBytes +=
                        compare(baseline[role], single[role], "native preservation guard single", phases[role]);
                    comparedBytes +=
                        compare(sharedReference[role], shared[role], "native preservation guard shared", phases[role]);
                }
            }
            std::printf("guard=%u native single/shared baseline exact all six roles\n", guard);
        }
        check(qualityFailures == 0, "FAIL quality: guided RGB error exceeds baseline; counterexample retained");
        fixture.gpu.noErrors();
        std::printf(
            "PASS: %zu exact RGBA bytes compared, %zu changed pixels; native baseline/single/shared all six roles\n",
            comparedBytes,
            changedPixels);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
