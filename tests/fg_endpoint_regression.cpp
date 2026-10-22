#include "fg_gpu_helpers.h"

#include "fullscreen_vertex_shader.hpp"
#include "nvidia_pack_flow_input_shader.hpp"
#include "nvidia_pack_luma_input_shader.hpp"
#include "nvidia_synthesize_midpoint_pixel_shader.hpp"
#include "pack_flow_input_shader.hpp"
#include "synthesize_midpoint_pixel_shader.hpp"

#include <algorithm>

namespace {
    UINT side = 64;
    constexpr UINT eyes = 2, block = 8;
    using Pixels = std::vector<uint8_t>;

    struct Mapping {
        float rotation[4]{0, 0, 0, 1};
        float sourceTangents[4]{-1, 1, 1, -1};
        float targetTangents[4]{-1, 1, 1, -1};
        float sourceRect[4]{0, 0, static_cast<float>(side), static_cast<float>(side)};
        float targetRect[4]{0, 0, static_cast<float>(side), static_cast<float>(side)};
    };
    struct Parameters {
        UINT width{side}, height{side}, arraySize{eyes}, packedWidth{side};
        UINT packedHeight{side * eyes}, eyeStride{side}, flowWidth{side / block}, flowHeight{side * eyes / block};
        UINT flowBlock{block}, viewIndex{}, useGameMotion{}, padding{};
        float gameMotionRect[4]{}, gameMotionScale[2]{}, gameMotionJitter[2]{};
        Mapping mappings[eyes];
        UINT slice{}, flags{};
    };
    static_assert(sizeof(Parameters) == 62 * sizeof(UINT));

    uint8_t* pixel(Pixels& image, UINT eye, UINT x, UINT y) {
        return image.data() + ((eye * side + y) * side + x) * 4;
    }
    Pixels flat() {
        Pixels image(side * side * eyes * 4);
        for (UINT eye = 0; eye < eyes; ++eye) {
            for (UINT y = 0; y < side; ++y) {
                for (UINT x = 0; x < side; ++x) {
                    auto* value = pixel(image, eye, x, y);
                    value[0] = value[1] = value[2] = static_cast<uint8_t>(eye ? 237 : 255);
                    value[3] = static_cast<uint8_t>(eye ? 193 : 91);
                }
            }
        }
        return image;
    }
    void band(Pixels& image, UINT first, UINT last, uint8_t value) {
        for (UINT eye = 0; eye < eyes; ++eye) {
            for (UINT y = 0; y < side; ++y) {
                for (UINT x = first; x <= last; ++x) {
                    auto* color = pixel(image, eye, x, y);
                    color[0] = color[1] = color[2] = value;
                }
            }
        }
    }

    struct ShaderFixture {
        Gpu gpu;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> fidelityfx, nvidia;
        ComPtr<ID3D12DescriptorHeap> descriptors, targets;
        std::array<ComPtr<ID3D12Resource>, 5> resources;
        UINT descriptorStride{}, targetStride{};

        explicit ShaderFixture(bool srgb) {
            const auto imageFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            const auto allocationFormat = srgb ? DXGI_FORMAT_R8G8B8A8_TYPELESS : imageFormat;
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0, 0};
            ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
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
            ComPtr<ID3DBlob> serialized, errors;
            success(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
                    "serialize production-layout root signature");
            success(gpu.device->CreateRootSignature(
                        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)),
                    "root signature");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = root.Get();
            pipeline.VS = {g_xrfg_fullscreen_vertex_shader, sizeof(g_xrfg_fullscreen_vertex_shader)};
            pipeline.PS = {g_xrfg_synthesize_midpoint_pixel_shader, sizeof(g_xrfg_synthesize_midpoint_pixel_shader)};
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
            pipeline.RTVFormats[0] = imageFormat;
            pipeline.SampleDesc.Count = 1;
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&fidelityfx)),
                    "FFX production PSO");
            pipeline.PS = {g_xrfg_nvidia_synthesize_midpoint_pixel_shader,
                           sizeof(g_xrfg_nvidia_synthesize_midpoint_pixel_shader)};
            success(gpu.device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&nvidia)), "NVIDIA production PSO");
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = 10;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            success(gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)), "shader descriptors");
            descriptorStride = gpu.device->GetDescriptorHandleIncrementSize(heap.Type);
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = eyes;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            success(gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets)), "target descriptors");
            targetStride = gpu.device->GetDescriptorHandleIncrementSize(heap.Type);
            resources[0] = texture(side, side, eyes, allocationFormat);
            resources[1] = texture(side, side, eyes, allocationFormat);
            resources[2] = texture(side / block, side * eyes / block, 1, DXGI_FORMAT_R32G32_SINT);
            resources[3] = texture(side / block, side * eyes / block, 1, DXGI_FORMAT_R32_UINT);
            resources[4] = texture(side, side, eyes, allocationFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            for (UINT index = 0; index < 7; ++index) {
                const UINT resource = std::min(index, 3u);
                auto desc = resources[resource]->GetDesc();
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = index < 2 ? imageFormat : desc.Format;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (index < 2 || index == 6) {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.ArraySize = index < 2 ? eyes : 1;
                    view.Texture2DArray.MipLevels = 1;
                } else {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    view.Texture2D.MipLevels = 1;
                }
                if (index == 6)
                    view.Format = DXGI_FORMAT_R32G32_FLOAT;
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
            for (UINT eye = 0; eye < eyes; ++eye) {
                D3D12_RENDER_TARGET_VIEW_DESC view{};
                view.Format = imageFormat;
                view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.FirstArraySlice = eye;
                view.Texture2DArray.ArraySize = 1;
                auto handle = targets->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += eye * targetStride;
                gpu.device->CreateRenderTargetView(resources[4].Get(), &view, handle);
            }
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
            description.MipLevels = description.SampleDesc.Count = 1;
            description.Format = format;
            description.Flags = flags;
            ComPtr<ID3D12Resource> result;
            success(gpu.device->CreateCommittedResource(&heap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &description,
                                                        D3D12_RESOURCE_STATE_COMMON,
                                                        nullptr,
                                                        IID_PPV_ARGS(&result)),
                    "fixture texture");
            return result;
        }

        Pixels transfer(ID3D12Resource* resource, const void* input, UINT bytesPerPixel) {
            auto description = resource->GetDesc();
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(description.DepthOrArraySize);
            UINT64 bytes{};
            gpu.device->GetCopyableFootprints(
                &description, 0, description.DepthOrArraySize, 0, layouts.data(), nullptr, nullptr, &bytes);
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
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
                    "staging buffer");
            const size_t rowBytes = static_cast<size_t>(description.Width) * bytesPerPixel;
            auto copyRows = [&](void* destination, const void* source, bool upload) {
                for (UINT slice = 0; slice < description.DepthOrArraySize; ++slice) {
                    for (UINT y = 0; y < description.Height; ++y) {
                        size_t packedOffset = (slice * description.Height + y) * rowBytes;
                        size_t paddedOffset = layouts[slice].Offset + y * layouts[slice].Footprint.RowPitch;
                        std::memcpy(static_cast<uint8_t*>(destination) + (upload ? paddedOffset : packedOffset),
                                    static_cast<const uint8_t*>(source) + (upload ? packedOffset : paddedOffset),
                                    rowBytes);
                    }
                }
            };
            if (input) {
                void* mapped{};
                D3D12_RANGE empty{};
                success(staging->Map(0, &empty, &mapped), "map upload");
                copyRows(mapped, input, true);
                staging->Unmap(0, nullptr);
            }
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            success(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                    "allocator");
            success(gpu.device->CreateCommandList(
                        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
                    "copy list");
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
                D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
                success(staging->Map(0, &readRange, &mapped), "map readback");
                copyRows(result.data(), mapped, false);
                D3D12_RANGE empty{};
                staging->Unmap(0, &empty);
            }
            return result;
        }

        void execute(ID3D12GraphicsCommandList* list) {
            success(list->Close(), "close fixture commands");
            ID3D12CommandList* lists[]{list};
            gpu.queue->ExecuteCommandLists(1, lists);
            auto fence = gpu.nextFence++;
            success(gpu.queue->Signal(gpu.fence.Get(), fence), "fixture signal");
            gpu.wait(gpu.fence.Get(), fence);
            gpu.noErrors();
        }

        Pixels draw(Parameters parameters, bool useNvidia) {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            success(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                    "draw allocator");
            success(gpu.device->CreateCommandList(0,
                                                  D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  allocator.Get(),
                                                  useNvidia ? nvidia.Get() : fidelityfx.Get(),
                                                  IID_PPV_ARGS(&list)),
                    "draw list");
            std::array<D3D12_RESOURCE_BARRIER, 5> barriers{};
            for (UINT i = 0; i < barriers.size(); ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition = {resources[i].Get(),
                                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_COMMON,
                                          i == 4 ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                 : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
            }
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            list->SetGraphicsRootSignature(root.Get());
            ID3D12DescriptorHeap* heaps[]{descriptors.Get()};
            list->SetDescriptorHeaps(1, heaps);
            auto table = descriptors->GetGPUDescriptorHandleForHeapStart();
            list->SetGraphicsRootDescriptorTable(1, table);
            table.ptr += 7 * descriptorStride;
            list->SetGraphicsRootDescriptorTable(2, table);
            D3D12_VIEWPORT viewport{0, 0, static_cast<float>(side), static_cast<float>(side), 0, 1};
            D3D12_RECT scissor{0, 0, static_cast<LONG>(side), static_cast<LONG>(side)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT eye = 0; eye < eyes; ++eye) {
                parameters.slice = parameters.viewIndex = eye;
                list->SetGraphicsRoot32BitConstants(0, 62, &parameters, 0);
                auto target = targets->GetCPUDescriptorHandleForHeapStart();
                target.ptr += eye * targetStride;
                list->OMSetRenderTargets(1, &target, FALSE, nullptr);
                constexpr float unwritten[4]{1, 0, 1, 0};
                list->ClearRenderTargetView(target, unwritten, 0, nullptr);
                list->DrawInstanced(3, 1, 0, 0);
            }
            for (auto& barrier : barriers)
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            execute(list.Get());
            return transfer(resources[4].Get(), nullptr, 4);
        }
    };

    void compareChannel(uint8_t actual,
                        double expected,
                        const char* scenario,
                        UINT eye,
                        UINT x,
                        UINT y,
                        UINT channel,
                        double tolerance = 2.0) {
        if (std::abs(actual - expected) > tolerance) {
            char message[256];
            sprintf_s(message,
                      "%s eye=%u pixel=(%u,%u) channel=%u: actual=%u expected=%.3f",
                      scenario,
                      eye,
                      x,
                      y,
                      channel,
                      actual,
                      expected);
            throw std::runtime_error(message);
        }
    }

    int runGhosting(const std::string& scenario) {
        const bool stationary = scenario.rfind("stationary-alias", 0) == 0;
        const bool srgb = scenario.find("srgb") != std::string::npos;
        const bool reverse = scenario.find("reverse") != std::string::npos;
        const bool rotation = scenario == "stationary-alias-rotation";
        const bool asymmetric = scenario == "stationary-alias-asymmetric";
        const bool alphaOnly = scenario == "ghost-alpha-motion";
        const bool thin = scenario.rfind("ghost-thin-motion", 0) == 0;
        const int displacement = stationary ? -24 : thin ? 2 : alphaOnly ? 18 : 24;
        const int movement = reverse ? -displacement : displacement;
        const double start = stationary ? 0 : thin ? 30 : reverse ? 42 : 20;
        const double barWidth = thin ? 1 : 3;
        const auto backgroundFor = [&](UINT eye) { return srgb && thin ? 40 : alphaOnly ? 50 : eye ? 40 : 26; };
        const auto foregroundFor = [&](UINT eye) { return srgb && thin ? 30 : alphaOnly ? 200 : eye ? 160 : 179; };
        const auto decode = [](double value) {
            value /= 255.0;
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        const auto encode = [](double value) {
            return 255 * (value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - 0.055);
        };
        ShaderFixture fixture(srgb);
        bool allPassed = true;
        for (bool useNvidia : {true, false}) {
            for (float requested : {0.3f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.7f}) {
                Parameters parameters;
                parameters.flags = (static_cast<UINT>(std::lround(requested * 65535.0f)) << 8) | (srgb ? 2u : 0u);
                const double fraction = double(parameters.flags >> 8) / 65535.0;
                auto previous = flat(), current = flat();
                for (UINT eye = 0; eye < eyes; ++eye) {
                    for (UINT y = 0; y < side; ++y) {
                        for (UINT x = 0; x < side; ++x) {
                            auto* a = pixel(previous, eye, x, y);
                            auto* b = pixel(current, eye, x, y);
                            for (UINT c = 0; c < 4; ++c) {
                                if (stationary) {
                                    a[c] = b[c] = c == 3 ? (eye ? 193 : 91) : x % 24 < 8 ? (eye ? 237 : 255) : 0;
                                } else {
                                    const bool movingChannel = alphaOnly ? c == 3 : c < 3;
                                    const int background = backgroundFor(eye);
                                    const int foreground = foregroundFor(eye);
                                    a[c] = static_cast<uint8_t>(
                                        movingChannel ? (x >= start && x < start + barWidth ? foreground : background)
                                        : alphaOnly   ? 120
                                        : eye         ? 193
                                                      : 91);
                                    b[c] = static_cast<uint8_t>(
                                        movingChannel
                                            ? (x >= start + movement && x < start + movement + barWidth ? foreground
                                                                                                        : background)
                                            : a[c]);
                                }
                            }
                        }
                    }
                }
                std::vector<int32_t> flow((side / block) * (side * eyes / block) * 2);
                std::vector<uint32_t> auxiliary((side / block) * (side * eyes / block));
                for (size_t i = 0; i < flow.size(); i += 2)
                    flow[i] = -movement * (useNvidia ? 32 : 1);
                if (asymmetric) {
                    for (auto& mapping : parameters.mappings) {
                        mapping.sourceTangents[0] = mapping.targetTangents[0] = -0.8f;
                        mapping.sourceTangents[1] = mapping.targetTangents[1] = 1.2f;
                        mapping.sourceTangents[2] = mapping.targetTangents[2] = 1.1f;
                        mapping.sourceTangents[3] = mapping.targetTangents[3] = -0.9f;
                    }
                }
                if (rotation) {
                    // Opposite eye rotations: H0(x,y)=(y,63-x), H1(x,y)=(63-y,x).
                    // Paint A through H^-1 independently, so A(H(p)) is exactly B(p).
                    for (UINT eye = 0; eye < eyes; ++eye) {
                        parameters.mappings[eye].rotation[2] = (eye ? -1 : 1) * std::sqrt(0.5f);
                        parameters.mappings[eye].rotation[3] = std::sqrt(0.5f);
                        for (UINT y = 0; y < side; ++y)
                            for (UINT x = 0; x < side; ++x)
                                std::memcpy(pixel(previous, eye, x, y),
                                            pixel(current, eye, eye ? y : side - 1 - y, eye ? side - 1 - x : x),
                                            4);
                        if (!useNvidia) {
                            for (UINT y = 0; y < side / block; ++y) {
                                for (UINT x = 0; x < side / block; ++x) {
                                    const auto offset = ((eye * side / block + y) * (side / block) + x) * 2;
                                    flow[offset] = eye ? 56 - 8 * int(x + y) : 8 * (int(y) - int(x));
                                    flow[offset + 1] =
                                        eye ? 8 * (int(x) - int(y)) - movement : 56 + movement - 8 * int(x + y);
                                }
                            }
                        }
                    }
                }
                fixture.transfer(fixture.resources[0].Get(), previous.data(), 4);
                fixture.transfer(fixture.resources[1].Get(), current.data(), 4);
                fixture.transfer(fixture.resources[2].Get(), flow.data(), 8);
                fixture.transfer(fixture.resources[3].Get(), auxiliary.data(), 4);
                auto output = fixture.draw(parameters, useNvidia);
                for (UINT eye = 0; eye < eyes; ++eye) {
                    size_t bad{};
                    double maximum{};
                    for (UINT y = 4; y + 4 < side; ++y) {
                        for (UINT x = stationary ? 2u : 16u; x < (stationary ? side - 2 : 48u); ++x) {
                            for (UINT c = 0; c < 4; ++c) {
                                double expected = pixel(current, eye, x, y)[c];
                                if (!stationary && (alphaOnly ? c == 3 : c < 3)) {
                                    // Independently render the continuous translated bar at this time.
                                    const double first = start + movement * fraction;
                                    const double coverage = std::clamp(double(x) - first + 1, 0.0, 1.0) *
                                                            std::clamp(first + barWidth - double(x), 0.0, 1.0);
                                    const double background = backgroundFor(eye);
                                    const double foreground = foregroundFor(eye);
                                    expected = srgb && c < 3
                                                   ? encode(decode(background) +
                                                            (decode(foreground) - decode(background)) * coverage)
                                                   : background + (foreground - background) * coverage;
                                }
                                const double error = std::abs(pixel(output, eye, x, y)[c] - expected);
                                maximum = std::max(maximum, error);
                                bad += error > 2.0;
                            }
                        }
                    }
                    std::printf("ghost-control=%s backend=%s fraction=%.6f eye=%u bad=%zu max=%.3f\n",
                                scenario.c_str(),
                                useNvidia ? "NVIDIA" : "FidelityFX",
                                fraction,
                                eye,
                                bad,
                                maximum);
                    allPassed = allPassed && !bad;
                }
            }
        }
        check(allPassed,
              "static images must stay still; moving detail and alpha must retain their interpolated position");
        std::puts("PASS: production endpoint shader draw and readback");
        return 0;
    }

    int runHalfPixelBoundary(bool reverse, bool vertical = false) {
        ShaderFixture fixture(false);
        for (bool useNvidia : {true, false}) {
            for (float requested : {0.3f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.7f}) {
                Parameters parameters;
                parameters.flags = static_cast<UINT>(std::lround(requested * 65535.0f)) << 8;
                const double fraction = double(parameters.flags >> 8) / 65535.0;
                int displacement{};
                double shift{};
                for (int trial = 1; trial <= 24; ++trial) {
                    shift = trial * (1 - fraction);
                    const double remainder = shift - std::floor(shift);
                    if (remainder > 0.1 && remainder <= 0.5) {
                        displacement = reverse ? -trial : trial;
                        break;
                    }
                }
                check(displacement != 0, "construct current endpoint in half-pixel margin");
                const UINT checkedCoordinate = reverse ? 55u - UINT(std::floor(shift)) : 8u + UINT(std::floor(shift));
                const UINT checkedX = vertical ? 32u : checkedCoordinate;
                const UINT checkedY = vertical ? checkedCoordinate : 32u;
                const double endpoint = checkedCoordinate - displacement * (1 - fraction);
                check(reverse ? endpoint > 55 && endpoint <= 55.5 : endpoint >= 7.5 && endpoint < 8,
                      "half-pixel endpoint outside pixel-center domain");
                for (auto& mapping : parameters.mappings) {
                    mapping.sourceRect[0] = mapping.sourceRect[1] = 8;
                    mapping.targetRect[0] = mapping.targetRect[1] = 8;
                    mapping.sourceRect[2] = mapping.sourceRect[3] = 48;
                    mapping.targetRect[2] = mapping.targetRect[3] = 48;
                }
                auto previous = flat(), current = flat();
                for (UINT eye = 0; eye < eyes; ++eye)
                    for (UINT y = 0; y < side; ++y)
                        for (UINT x = 0; x < side; ++x)
                            for (UINT c = 0; c < 4; ++c) {
                                const bool inside = x >= 8 && x <= 55 && y >= 8 && y <= 55;
                                const auto value = static_cast<uint8_t>(c == 3 ? (inside ? (eye ? 193 : 91) : 17)
                                                                               : 100 + eye * 80 + (inside ? 20 : 0));
                                pixel(previous, eye, x, y)[c] = pixel(current, eye, x, y)[c] = value;
                            }
                std::vector<int32_t> flow((side / block) * (side * eyes / block) * 2);
                std::vector<uint32_t> auxiliary((side / block) * (side * eyes / block));
                for (size_t i = 0; i < flow.size(); i += 2)
                    flow[i + (vertical ? 1 : 0)] = displacement * (useNvidia ? 32 : 1);
                fixture.transfer(fixture.resources[0].Get(), previous.data(), 4);
                fixture.transfer(fixture.resources[1].Get(), current.data(), 4);
                fixture.transfer(fixture.resources[2].Get(), flow.data(), 8);
                fixture.transfer(fixture.resources[3].Get(), auxiliary.data(), 4);
                auto output = fixture.draw(parameters, useNvidia);
                for (UINT eye = 0; eye < eyes; ++eye)
                    for (UINT c = 0; c < 4; ++c)
                        compareChannel(pixel(output, eye, checkedX, checkedY)[c],
                                       pixel(current, eye, checkedX, checkedY)[c],
                                       "unsupported half-pixel endpoint preserves current view",
                                       eye,
                                       checkedX,
                                       checkedY,
                                       c,
                                       2);
                std::printf("verified half-pixel %s backend=%s fraction=%.6f endpoint=%.6f both eyes\n",
                            vertical ? (reverse ? "bottom" : "top") : (reverse ? "right" : "left"),
                            useNvidia ? "NVIDIA" : "FidelityFX",
                            fraction,
                            endpoint);
            }
        }
        std::puts("PASS: production endpoint shader draw and readback");
        return 0;
    }

    int runPreviousCameraPadding(bool reverse, bool vertical = false) {
        ShaderFixture fixture(false);
        bool allPassed = true;
        for (bool useNvidia : {true, false}) {
            for (float requested : {1.0f / 3.0f, 0.5f, 2.0f / 3.0f}) {
                Parameters parameters;
                parameters.flags = static_cast<UINT>(std::lround(requested * 65535.0f)) << 8;
                const double fraction = double(parameters.flags >> 8) / 65535.0;
                const float cameraShift = reverse ? 4.4f : -4.4f;
                const UINT checkedCoordinate = reverse ? 51u : 12u;
                const UINT checkedX = vertical ? 32u : checkedCoordinate;
                const UINT checkedY = vertical ? checkedCoordinate : 32u;
                for (auto& mapping : parameters.mappings) {
                    mapping.sourceRect[0] = mapping.sourceRect[1] = 8;
                    mapping.targetRect[0] = mapping.targetRect[1] = 8;
                    mapping.sourceRect[2] = mapping.sourceRect[3] = 48;
                    mapping.targetRect[2] = mapping.targetRect[3] = 48;
                    // Identity rotation and a translated target FOV give H(p)=p+shift.
                    // The source image remains white inside the view and black outside.
                    const float tangentShift = cameraShift * (vertical ? -2.0f : 2.0f) / 48.0f;
                    mapping.targetTangents[vertical ? 2 : 0] += tangentShift;
                    mapping.targetTangents[vertical ? 3 : 1] += tangentShift;
                }
                auto previous = flat(), current = flat();
                for (UINT eye = 0; eye < eyes; ++eye)
                    for (UINT y = 0; y < side; ++y)
                        for (UINT x = 0; x < side; ++x)
                            for (UINT c = 0; c < 4; ++c) {
                                const bool inside = x >= 8 && x <= 55 && y >= 8 && y <= 55;
                                const auto value = static_cast<uint8_t>(inside ? (c == 3 ? (eye ? 193 : 91) : 255) : 0);
                                pixel(previous, eye, x, y)[c] = pixel(current, eye, x, y)[c] = value;
                            }
                std::vector<int32_t> flow((side / block) * (side * eyes / block) * 2);
                std::vector<uint32_t> auxiliary((side / block) * (side * eyes / block));
                // FFX stores raw source displacement; NVIDIA stores camera residuals
                // in S10.5. Both put the previous sample within a partial source texel:
                // e.g. p=12, H(p)=7.6 and FFX raw=-4 give H(p+0.4*t)<8.
                const int displacement = (reverse ? 1 : -1) * (useNvidia ? -13 : 4);
                for (size_t i = 0; i < flow.size(); i += 2)
                    flow[i + (vertical ? 1 : 0)] = displacement;
                fixture.transfer(fixture.resources[0].Get(), previous.data(), 4);
                fixture.transfer(fixture.resources[1].Get(), current.data(), 4);
                fixture.transfer(fixture.resources[2].Get(), flow.data(), 8);
                fixture.transfer(fixture.resources[3].Get(), auxiliary.data(), 4);
                auto output = fixture.draw(parameters, useNvidia);
                for (UINT eye = 0; eye < eyes; ++eye) {
                    size_t bad{};
                    double maximum{};
                    for (UINT c = 0; c < 4; ++c) {
                        const double error = std::abs(double(pixel(output, eye, checkedX, checkedY)[c]) -
                                                      pixel(current, eye, checkedX, checkedY)[c]);
                        maximum = std::max(maximum, error);
                        bad += error > 2.0;
                    }
                    std::printf("previous-camera-padding=%s backend=%s fraction=%.6f eye=%u bad=%zu max=%.3f\n",
                                vertical ? (reverse ? "bottom" : "top") : (reverse ? "right" : "left"),
                                useNvidia ? "NVIDIA" : "FidelityFX",
                                fraction,
                                eye,
                                bad,
                                maximum);
                    allPassed = allPassed && !bad;
                }
            }
        }
        check(allPassed, "previous camera endpoint must preserve view color without sampling inactive padding");
        std::puts("PASS: production endpoint shader draw and readback");
        return 0;
    }

    double expectedPackPattern(double sourceX, double sourceY, UINT sourceWidth, UINT sourceHeight, UINT eye) {
        // Independently interpolate the two literal step edges painted below.
        const double horizontal = std::clamp(sourceX - (double(sourceWidth / 2) - 1), 0.0, 1.0);
        const double bottom = std::clamp(sourceY - (double(sourceHeight) - 7), 0.0, 1.0);
        const double interior = (eye ? 96 : 32) + 40 * horizontal;
        return interior * (1 - bottom) + (eye ? 192 : 255) * bottom;
    }

    int runNvidiaInputScale(const std::string& scenario) {
        ShaderFixture fixture(false);
        const bool full = scenario == "nvidia-full-input-padding";
        const bool threeQuarter = scenario == "nvidia-three-quarter-input-padding";
        const bool smallInput = scenario == "nvidia-small-half-input-padding";
        const bool cropped = scenario == "nvidia-half-input-cropped-view";
        const double nominalScale = full ? 1.0 : threeQuarter ? 0.75 : 0.5;
        Parameters parameters;
        parameters.width = smallInput ? 40 : 129;
        parameters.height = smallInput ? 32 : 128;
        parameters.packedWidth = smallInput ? 64 : full ? 132 : threeQuarter ? 100 : 68;
        parameters.packedHeight = smallInput ? 64 : full ? 128 : threeQuarter ? 96 : 64;
        parameters.eyeStride = 0;
        // Explicit nominal scale must survive alignment and the minimum allocation.
        parameters.flags = (full ? 1u : threeQuarter ? 2u : 3u) << 2;
        for (auto& mapping : parameters.mappings) {
            mapping.sourceRect[2] = mapping.targetRect[2] = float(parameters.width);
            mapping.sourceRect[3] = mapping.targetRect[3] = float(parameters.height);
            if (cropped) {
                mapping.sourceRect[0] = mapping.sourceRect[1] = 9;
                mapping.targetRect[0] = mapping.targetRect[1] = 9;
                mapping.sourceRect[2] = mapping.sourceRect[3] = 47;
                mapping.targetRect[2] = mapping.targetRect[3] = 47;
            }
        }
        Pixels input(parameters.width * parameters.height * eyes * 4);
        for (UINT eye = 0; eye < eyes; ++eye)
            for (UINT y = 0; y < parameters.height; ++y)
                for (UINT x = 0; x < parameters.width; ++x) {
                    const size_t offset = ((eye * parameters.height + y) * parameters.width + x) * 4;
                    // Vertical detail checks horizontal sampling; the last six source
                    // rows check vertical sampling and distinguish the two eyes.
                    const auto marker = static_cast<uint8_t>(
                        y >= parameters.height - 6 ? (eye ? 192 : 255)
                                                   : (eye ? 96 : 32) + (x >= parameters.width / 2 ? 40 : 0));
                    const bool inside = x >= 9 && x <= 55 && y >= 9 && y <= 55;
                    const auto value = static_cast<uint8_t>(cropped ? (inside ? (eye ? 192 : 255) : 0) : marker);
                    input[offset] = input[offset + 1] = input[offset + 2] = value;
                    input[offset + 3] = 255;
                }
        for (UINT index = 0; index < 2; ++index) {
            fixture.resources[index] =
                fixture.texture(parameters.width, parameters.height, eyes, DXGI_FORMAT_R8G8B8A8_UNORM);
            fixture.transfer(fixture.resources[index].Get(), input.data(), 4);
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.ArraySize = eyes;
            view.Texture2DArray.MipLevels = 1;
            auto handle = fixture.descriptors->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += index * fixture.descriptorStride;
            fixture.gpu.device->CreateShaderResourceView(fixture.resources[index].Get(), &view, handle);
        }
        bool allPassed = true;
        for (bool luma : {false, true}) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = fixture.root.Get();
            pipeline.CS = luma ? D3D12_SHADER_BYTECODE{g_xrfg_nvidia_pack_luma_input_shader,
                                                       sizeof(g_xrfg_nvidia_pack_luma_input_shader)}
                               : D3D12_SHADER_BYTECODE{g_xrfg_nvidia_pack_flow_input_shader,
                                                       sizeof(g_xrfg_nvidia_pack_flow_input_shader)};
            ComPtr<ID3D12PipelineState> pack;
            success(fixture.gpu.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pack)),
                    "NVIDIA production pack PSO");
            const UINT bytesPerPixel = luma ? 1 : 4;
            const auto format = luma ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            std::array<ComPtr<ID3D12Resource>, 2> packed;
            for (UINT index = 0; index < packed.size(); ++index) {
                packed[index] = fixture.texture(parameters.packedWidth,
                                                parameters.packedHeight,
                                                1,
                                                format,
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                auto handle = fixture.descriptors->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += (index + 8) * fixture.descriptorStride;
                fixture.gpu.device->CreateUnorderedAccessView(packed[index].Get(), nullptr, &view, handle);
            }
            for (UINT eye = 0; eye < eyes; ++eye) {
                parameters.slice = parameters.viewIndex = eye;
                ComPtr<ID3D12CommandAllocator> allocator;
                ComPtr<ID3D12GraphicsCommandList> list;
                success(fixture.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                   IID_PPV_ARGS(&allocator)),
                        "pack allocator");
                success(fixture.gpu.device->CreateCommandList(
                            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pack.Get(), IID_PPV_ARGS(&list)),
                        "pack list");
                std::array<D3D12_RESOURCE_BARRIER, 4> barriers{};
                for (UINT index = 0; index < barriers.size(); ++index) {
                    barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barriers[index].Transition = {index < 2 ? fixture.resources[index].Get() : packed[index - 2].Get(),
                                                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                  D3D12_RESOURCE_STATE_COMMON,
                                                  index < 2 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
                }
                list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                list->SetComputeRootSignature(fixture.root.Get());
                list->SetComputeRoot32BitConstants(0, 62, &parameters, 0);
                ID3D12DescriptorHeap* heaps[]{fixture.descriptors.Get()};
                list->SetDescriptorHeaps(1, heaps);
                auto table = fixture.descriptors->GetGPUDescriptorHandleForHeapStart();
                list->SetComputeRootDescriptorTable(1, table);
                table.ptr += 7 * fixture.descriptorStride;
                list->SetComputeRootDescriptorTable(2, table);
                list->Dispatch((parameters.packedWidth + 7) / 8, (parameters.packedHeight + 7) / 8, 1);
                for (auto& barrier : barriers)
                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                fixture.execute(list.Get());
                for (UINT endpoint = 0; endpoint < packed.size(); ++endpoint) {
                    auto output = fixture.transfer(packed[endpoint].Get(), nullptr, bytesPerPixel);
                    size_t bad{};
                    double maximum{};
                    for (UINT y = 0; y < parameters.packedHeight; ++y)
                        for (UINT x = 0; x < parameters.packedWidth; ++x)
                            for (UINT c = 0; c < bytesPerPixel; ++c) {
                                const double sourceX = (x + 0.5) / nominalScale - 0.5;
                                const double sourceY = (y + 0.5) / nominalScale - 0.5;
                                bool active = sourceX < parameters.width && sourceY < parameters.height;
                                // Previous sampling maps through the view's pixel-center
                                // domain. The ceil-width last partial texel may be outside it.
                                if (!endpoint)
                                    active =
                                        active && sourceX <= parameters.width - 1 && sourceY <= parameters.height - 1;
                                if (cropped)
                                    active = sourceX >= 9 && sourceX <= 55 && sourceY >= 9 && sourceY <= 55;
                                const double expected =
                                    c == 3 ? 255
                                    : active
                                        ? (cropped ? (eye ? 192 : 255)
                                                   : expectedPackPattern(
                                                         sourceX, sourceY, parameters.width, parameters.height, eye))
                                        : 0;
                                const size_t offset = (y * parameters.packedWidth + x) * bytesPerPixel + c;
                                const double error = std::abs(double(output[offset]) - expected);
                                if (!bad && error > 2.0)
                                    std::printf("first pack mismatch pixel=(%u,%u) source=(%.6f,%.6f) channel=%u "
                                                "actual=%u expected=%.3f\n",
                                                x,
                                                y,
                                                sourceX,
                                                sourceY,
                                                c,
                                                output[offset],
                                                expected);
                                maximum = std::max(maximum, error);
                                bad += error > 2.0;
                            }
                    std::printf("%s format=%s endpoint=%s eye=%u bad=%zu max=%.3f\n",
                                scenario.c_str(),
                                luma ? "R8" : "RGBA8",
                                endpoint ? "current" : "previous",
                                eye,
                                bad,
                                maximum);
                    allPassed = allPassed && !bad;
                }
            }
        }
        check(allPassed, "NVIDIA packing must preserve nominal-scale detail and leave allocation padding black");
        std::puts("PASS: production NVIDIA pack shader dispatch and readback");
        return 0;
    }

    int runFfxInputScale(const std::string& scenario) {
        ShaderFixture fixture(false);
        const bool full = scenario == "ffx-full-input-padding";
        const bool threeQuarter = scenario == "ffx-three-quarter-input-padding";
        const bool smallInput = scenario == "ffx-small-half-input-padding";
        const double nominalScale = full ? 1.0 : threeQuarter ? 0.75 : 0.5;
        Parameters parameters;
        parameters.width = smallInput ? 40 : 129;
        parameters.height = smallInput ? 32 : 128;
        parameters.packedWidth = smallInput ? 64 : full ? 136 : threeQuarter ? 104 : 72;
        parameters.packedHeight = smallInput ? 96 : full ? 320 : threeQuarter ? 256 : 192;
        parameters.eyeStride = smallInput ? 80 : full ? 192 : threeQuarter ? 160 : 128;
        parameters.flags = (full ? 1u : threeQuarter ? 2u : 3u) << 2;
        Pixels input(parameters.width * parameters.height * eyes * 4);
        for (UINT eye = 0; eye < eyes; ++eye)
            for (UINT y = 0; y < parameters.height; ++y)
                for (UINT x = 0; x < parameters.width; ++x) {
                    const size_t offset = ((eye * parameters.height + y) * parameters.width + x) * 4;
                    const auto value = static_cast<uint8_t>(
                        y >= parameters.height - 6 ? (eye ? 192 : 255)
                                                   : (eye ? 96 : 32) + (x >= parameters.width / 2 ? 40 : 0));
                    input[offset] = input[offset + 1] = input[offset + 2] = value;
                    input[offset + 3] = 255;
                }
        fixture.resources[1] = fixture.texture(parameters.width, parameters.height, eyes, DXGI_FORMAT_R8G8B8A8_UNORM);
        fixture.transfer(fixture.resources[1].Get(), input.data(), 4);
        D3D12_SHADER_RESOURCE_VIEW_DESC sourceView{};
        sourceView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sourceView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sourceView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sourceView.Texture2DArray.ArraySize = eyes;
        sourceView.Texture2DArray.MipLevels = 1;
        auto sourceHandle = fixture.descriptors->GetCPUDescriptorHandleForHeapStart();
        sourceHandle.ptr += fixture.descriptorStride;
        fixture.gpu.device->CreateShaderResourceView(fixture.resources[1].Get(), &sourceView, sourceHandle);
        auto packed = fixture.texture(parameters.packedWidth,
                                      parameters.packedHeight,
                                      1,
                                      DXGI_FORMAT_R8G8B8A8_UNORM,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_UNORDERED_ACCESS_VIEW_DESC packedView{};
        packedView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        packedView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto packedHandle = fixture.descriptors->GetCPUDescriptorHandleForHeapStart();
        packedHandle.ptr += 7 * fixture.descriptorStride;
        fixture.gpu.device->CreateUnorderedAccessView(packed.Get(), nullptr, &packedView, packedHandle);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = fixture.root.Get();
        pipeline.CS = {g_xrfg_pack_flow_input_shader, sizeof(g_xrfg_pack_flow_input_shader)};
        ComPtr<ID3D12PipelineState> pack;
        success(fixture.gpu.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pack)),
                "FFX production pack PSO");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        success(fixture.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "FFX pack allocator");
        success(fixture.gpu.device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pack.Get(), IID_PPV_ARGS(&list)),
                "FFX pack list");
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
        for (UINT index = 0; index < barriers.size(); ++index) {
            barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[index].Transition = {index ? packed.Get() : fixture.resources[1].Get(),
                                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_COMMON,
                                          index ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        }
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        list->SetComputeRootSignature(fixture.root.Get());
        list->SetComputeRoot32BitConstants(0, 62, &parameters, 0);
        ID3D12DescriptorHeap* heaps[]{fixture.descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);
        auto table = fixture.descriptors->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(1, table);
        table.ptr += 7 * fixture.descriptorStride;
        list->SetComputeRootDescriptorTable(2, table);
        list->Dispatch((parameters.packedWidth + 7) / 8, (parameters.packedHeight + 7) / 8, 1);
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        fixture.execute(list.Get());
        auto output = fixture.transfer(packed.Get(), nullptr, 4);
        size_t bad{};
        double maximum{};
        for (UINT y = 0; y < parameters.packedHeight; ++y)
            for (UINT x = 0; x < parameters.packedWidth; ++x)
                for (UINT c = 0; c < 4; ++c) {
                    const UINT eye = y / parameters.eyeStride;
                    const UINT localY = y - eye * parameters.eyeStride;
                    const double sourceX = (x + 0.5) / nominalScale - 0.5;
                    const double sourceY = (localY + 0.5) / nominalScale - 0.5;
                    const bool active = eye < eyes && sourceX < parameters.width && sourceY < parameters.height;
                    const double expected =
                        c == 3   ? 255
                        : active ? expectedPackPattern(sourceX, sourceY, parameters.width, parameters.height, eye)
                                 : 0;
                    const size_t offset = (y * parameters.packedWidth + x) * 4 + c;
                    const double error = std::abs(double(output[offset]) - expected);
                    if (!bad && error > 2.0)
                        std::printf(
                            "first pack mismatch pixel=(%u,%u) source=(%.6f,%.6f) channel=%u actual=%u expected=%.3f\n",
                            x,
                            y,
                            sourceX,
                            sourceY,
                            c,
                            output[offset],
                            expected);
                    maximum = std::max(maximum, error);
                    bad += error > 2.0;
                }
        std::printf("%s both eyes/stride/right/bottom bad=%zu max=%.3f\n", scenario.c_str(), bad, maximum);
        check(!bad, "FFX packing must preserve nominal-scale detail, both eyes, and black right/bottom padding");
        std::puts("PASS: production FFX pack shader dispatch and readback");
        return 0;
    }

    int runValidEdgeCenters() {
        ShaderFixture fixture(false);
        for (bool useNvidia : {true, false}) {
            for (float requested : {0.3f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.7f}) {
                Parameters parameters;
                parameters.flags = static_cast<UINT>(std::lround(requested * 65535.0f)) << 8;
                const double fraction = double(parameters.flags >> 8) / 65535.0;
                for (auto& mapping : parameters.mappings) {
                    mapping.sourceRect[0] = mapping.sourceRect[1] = 8;
                    mapping.targetRect[0] = mapping.targetRect[1] = 8;
                    mapping.sourceRect[2] = mapping.sourceRect[3] = 48;
                    mapping.targetRect[2] = mapping.targetRect[3] = 48;
                }
                auto previous = flat(), current = flat();
                for (UINT eye = 0; eye < eyes; ++eye)
                    for (UINT y = 0; y < side; ++y)
                        for (UINT x = 0; x < side; ++x) {
                            pixel(previous, eye, x, y)[3] = eye ? 40 : 80;
                            pixel(current, eye, x, y)[3] = eye ? 200 : 160;
                        }
                std::vector<int32_t> flow((side / block) * (side * eyes / block) * 2);
                std::vector<uint32_t> auxiliary((side / block) * (side * eyes / block));
                fixture.transfer(fixture.resources[0].Get(), previous.data(), 4);
                fixture.transfer(fixture.resources[1].Get(), current.data(), 4);
                fixture.transfer(fixture.resources[2].Get(), flow.data(), 8);
                fixture.transfer(fixture.resources[3].Get(), auxiliary.data(), 4);
                auto output = fixture.draw(parameters, useNvidia);
                // Different alpha in each capture distinguishes interpolation from B fallback.
                for (UINT eye = 0; eye < eyes; ++eye)
                    for (UINT x : {8u, 55u})
                        for (UINT y : {8u, 55u})
                            for (UINT c = 0; c < 4; ++c)
                                compareChannel(pixel(output, eye, x, y)[c],
                                               pixel(previous, eye, x, y)[c] * (1 - fraction) +
                                                   pixel(current, eye, x, y)[c] * fraction,
                                               "valid first and last centers retain interpolation",
                                               eye,
                                               x,
                                               y,
                                               c,
                                               2);
                std::printf("verified valid edge centers backend=%s fraction=%.6f both eyes\n",
                            useNvidia ? "NVIDIA" : "FidelityFX",
                            fraction);
            }
        }
        std::puts("PASS: production endpoint shader draw and readback");
        return 0;
    }

    int run(const std::string& scenario) {
        if (scenario == "ffx-half-input-padding" || scenario == "ffx-full-input-padding" ||
            scenario == "ffx-three-quarter-input-padding" || scenario == "ffx-small-half-input-padding")
            return runFfxInputScale(scenario);
        if (scenario == "nvidia-half-input-bottom" || scenario == "nvidia-full-input-padding" ||
            scenario == "nvidia-three-quarter-input-padding" || scenario == "nvidia-small-half-input-padding" ||
            scenario == "nvidia-half-input-cropped-view")
            return runNvidiaInputScale(scenario);
        if (scenario == "previous-camera-left" || scenario == "previous-camera-right" ||
            scenario == "previous-camera-top" || scenario == "previous-camera-bottom")
            return runPreviousCameraPadding(scenario == "previous-camera-right" || scenario == "previous-camera-bottom",
                                            scenario == "previous-camera-top" || scenario == "previous-camera-bottom");
        if (scenario == "valid-edge-centers")
            return runValidEdgeCenters();
        if (scenario == "half-pixel-left" || scenario == "half-pixel-right" || scenario == "half-pixel-top" ||
            scenario == "half-pixel-bottom")
            return runHalfPixelBoundary(scenario == "half-pixel-right" || scenario == "half-pixel-bottom",
                                        scenario == "half-pixel-top" || scenario == "half-pixel-bottom");
        if (scenario.rfind("stationary-alias", 0) == 0 || scenario.rfind("ghost-thin-motion", 0) == 0 ||
            scenario == "ghost-alpha-motion" || scenario.rfind("ghost-large-motion", 0) == 0)
            return runGhosting(scenario);
        const bool uniform = scenario == "uniform" || scenario == "uniform-srgb";
        const bool gradient = scenario == "nearwhite-gradient" || scenario == "nearwhite-gradient-srgb";
        const bool rotationCap = scenario == "rotation-cap";
        const bool rotation = scenario == "rotation-motion" || scenario == "rotation-static" || rotationCap;
        const bool falseMatch =
            scenario == "false-match" || scenario == "false-match-srgb" || scenario == "dark-false-match-srgb";
        const bool periodic = scenario == "translated-detail";
        const bool translated = periodic || scenario == "translated-detail-aperiodic";
        const bool srgb = scenario == "uniform-srgb" || scenario == "nearwhite-gradient-srgb" ||
                          scenario == "false-match-srgb" || scenario == "dark-false-match-srgb";
        side = rotationCap ? 1024 : 64;
        ShaderFixture fixture(srgb);
        for (bool useNvidia : {true, false}) {
            if (useNvidia && !uniform && !gradient && !rotation && !translated)
                continue;
            for (float requested : {0.3f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.7f}) {
                Parameters parameters;
                parameters.flags = (static_cast<UINT>(std::lround(requested * 65535.0f)) << 8) | (srgb ? 2u : 0u);
                const double fraction = double(parameters.flags >> 8) / 65535.0;
                Pixels previous = flat(), current = flat();
                std::vector<int32_t> flow((side / block) * (side * eyes / block) * 2);
                std::vector<uint32_t> auxiliary((side / block) * (side * eyes / block));
                const int displacement =
                    gradient ? 0 : (translated || scenario == "border" || scenario == "border-no-neighbors" ? 8 : 16);
                for (size_t i = 0; i < flow.size(); i += 2)
                    flow[i] = displacement * (useNvidia ? 32 : 1);
                if (rotationCap) {
                    // This scene is static after camera compensation: A(x,y)=B(1023-y,x).
                    // At p=(900,500), raw B->A=(-400,-377), a valid >512px vector.
                    // Paint only the two WRONG sample sites caused by clipping its
                    // length before camera compensation. Correct output is B(p)=black;
                    // the cap calculation constructs adversarial input, not the oracle.
                    constexpr double px = 900, py = 500, rawX = -400, rawY = -377;
                    const double scale = 512.0 / std::hypot(rawX, rawY);
                    const double dx = 1023.0 - (py + rawY * scale) - px;
                    const double dy = px + rawX * scale - py;
                    band(current, 0, side - 1, 0);
                    for (UINT eye = 0; eye < eyes; ++eye) {
                        parameters.mappings[eye].rotation[2] = std::sqrt(0.5f);
                        parameters.mappings[eye].rotation[3] = std::sqrt(0.5f);
                        for (double position : {fraction, fraction - 1.0}) {
                            const int centerX = static_cast<int>(std::floor(px + dx * position));
                            const int centerY = static_cast<int>(std::floor(py + dy * position));
                            for (int y = centerY - 2; y <= centerY + 3; ++y)
                                for (int x = centerX - 2; x <= centerX + 3; ++x)
                                    std::fill_n(pixel(current, eye, UINT(x), UINT(y)),
                                                3,
                                                static_cast<uint8_t>(eye ? 237 : 255));
                        }
                        for (UINT y = 0; y < side; ++y)
                            for (UINT x = 0; x < side; ++x)
                                std::memcpy(pixel(previous, eye, x, y), pixel(current, eye, side - 1 - y, x), 4);
                        for (UINT y = 0; y < side / block; ++y) {
                            for (UINT x = 0; x < side / block; ++x) {
                                const size_t offset = ((eye * side / block + y) * (side / block) + x) * 2;
                                flow[offset] = useNvidia ? 0 : 8 * (int(y) - int(x));
                                flow[offset + 1] = useNvidia ? 0 : int(side) - 8 - 8 * (int(x) + int(y));
                            }
                        }
                    }
                } else if (rotation) {
                    // A +90-degree target-to-source roll maps H(x,y)=(y,63-x).
                    // A's horizontal band maps to target x34..38 (motion) or
                    // x26..30 (static), while B's vertical band is x26..30.
                    // Thus the exact target-space backward displacement is
                    // (8,0) or (0,0). At the checked pixel, both interpolated
                    // endpoint samples lie fully inside the white band.
                    const int movement = scenario == "rotation-motion" ? 8 : 0;
                    const UINT firstRow = static_cast<UINT>(33 - movement);
                    for (UINT eye = 0; eye < eyes; ++eye) {
                        parameters.mappings[eye].rotation[2] = std::sqrt(0.5f);
                        parameters.mappings[eye].rotation[3] = std::sqrt(0.5f);
                        for (UINT y = 0; y < side; ++y) {
                            for (UINT x = 0; x < side; ++x) {
                                const uint8_t white = static_cast<uint8_t>(eye ? 237 : 255);
                                std::fill_n(pixel(previous, eye, x, y),
                                            3,
                                            y >= firstRow && y <= firstRow + 4 ? white : uint8_t{});
                                std::fill_n(pixel(current, eye, x, y), 3, x >= 26 && x <= 30 ? white : uint8_t{});
                            }
                        }
                        for (UINT y = 0; y < side / block; ++y) {
                            for (UINT x = 0; x < side / block; ++x) {
                                const size_t offset = ((eye * side / block + y) * (side / block) + x) * 2;
                                // FFX vectors live at grid centers (8i+3.5,8j+3.5).
                                // Raw B->A is H(p+(movement,0))-p, an affine
                                // integer field that bilinear flow lookup reproduces.
                                flow[offset] = useNvidia ? movement * 32 : 8 * (int(y) - int(x));
                                flow[offset + 1] = useNvidia ? 0 : 56 - movement - 8 * (int(x) + int(y));
                            }
                        }
                    }
                } else if (uniform) {
                    // Alternate vectors at tile boundaries; constant color must remain exactly constant.
                    for (size_t i = 0; i < flow.size(); i += 2)
                        if ((i / 2) % 2)
                            flow[i] = -flow[i];
                } else if (gradient) {
                    // A/B are identical physical inputs, so zero flow must reproduce
                    // these 253/254/255 ramps. Steps at x/y22 and43 deliberately avoid
                    // the engine's 8px grid; that grid may not create additional steps.
                    for (UINT eye = 0; eye < eyes; ++eye) {
                        for (UINT y = 0; y < side; ++y) {
                            for (UINT x = 0; x < side; ++x) {
                                auto* a = pixel(previous, eye, x, y);
                                a[0] = static_cast<uint8_t>(253 + (eye ? side - 1 - x : x) * 3 / side);
                                a[1] = static_cast<uint8_t>(253 + y * 3 / side);
                                a[2] = 255;
                            }
                        }
                    }
                    current = previous;
                } else if (falseMatch || scenario == "center-detail" || scenario == "isolated-detail" ||
                           scenario == "raw-endpoint") {
                    if (scenario == "dark-false-match-srgb") {
                        // Code 64 sRGB becomes about 0.051 in linear light. Comparing
                        // that linear value to a 48/255 encoded threshold can accept
                        // two unrelated black midpoint samples. Original endpoints
                        // differ by 64 encoded codes and must reject completely.
                        for (size_t offset = 0; offset < previous.size(); offset += 4) {
                            previous[offset] = previous[offset + 1] = previous[offset + 2] = 64;
                        }
                        current = previous;
                    }
                    band(previous, 35, scenario == "isolated-detail" ? 44 : 51, 0);
                    band(current, 18, 29, 0);
                    if (scenario == "center-detail") {
                        const auto original = flat();
                        for (UINT eye = 0; eye < eyes; ++eye) {
                            for (UINT y = 0; y < side; ++y) {
                                std::memcpy(pixel(previous, eye, 48, y),
                                            original.data() + ((eye * side + y) * side + 48) * 4,
                                            4);
                            }
                        }
                    }
                    if (scenario == "isolated-detail") {
                        for (UINT eye = 0; eye < eyes; ++eye) {
                            auto* a = pixel(previous, eye, 48, 32);
                            a[0] = a[1] = a[2] = 0;
                        }
                    }
                    if (scenario == "raw-endpoint") {
                        parameters.mappings[0].sourceRect[0] = parameters.mappings[1].sourceRect[0] = 8;
                        parameters.mappings[0].sourceRect[2] = parameters.mappings[1].sourceRect[2] = 56;
                        parameters.mappings[0].targetRect[2] = parameters.mappings[1].targetRect[2] = 56;
                        band(previous, 52, 59, 255); // Double-warped endpoint agrees; raw endpoint does not.
                        for (UINT eye = 0; eye < eyes; ++eye) {
                            for (UINT y = 0; y < side; ++y) {
                                auto* b = pixel(current, eye, 32, y);
                                b[0] = b[1] = b[2] = 255;
                            }
                        }
                    }
                } else if (scenario == "border") {
                    // At x60 the raw endpoint x68 is outside the source image, although
                    // the 0.3-fraction warped samples both hit black and agree. The
                    // current endpoint remains white and must replace that false match.
                    band(previous, 61, 63, 0);
                    band(current, 53, 57, 0);
                } else if (scenario == "border-no-neighbors") {
                    // The center B(32,32)->A(40,32) is valid, but every +/-2 neighbor
                    // lies outside this narrow view. The valid mismatching center
                    // must still reject; an empty neighborhood cannot count as a match.
                    for (auto& mapping : parameters.mappings) {
                        mapping.sourceRect[0] = 39;
                        mapping.sourceRect[1] = 31;
                        mapping.targetRect[0] = mapping.targetRect[1] = 31;
                        mapping.sourceRect[2] = mapping.sourceRect[3] = 3;
                        mapping.targetRect[2] = mapping.targetRect[3] = 3;
                    }
                    band(previous, 39, 41, 0);
                } else if (translated) {
                    for (UINT eye = 0; eye < eyes; ++eye) {
                        for (UINT y = 0; y < side; ++y) {
                            for (UINT x = 0; x < side; ++x) {
                                // Retain the original period-8/+8 motion oracle. The separate
                                // aperiodic control adds two codes per +8 correspondence.
                                const auto value = [periodic](UINT p) {
                                    return static_cast<uint8_t>(((p % 8) < 2 ? 230 : 35) + (periodic ? 0 : p / 4));
                                };
                                auto* a = pixel(previous, eye, x, y);
                                auto* b = pixel(current, eye, x, y);
                                a[0] = a[1] = a[2] = value(x);
                                b[0] = b[1] = b[2] = value(x + displacement);
                            }
                        }
                    }
                } else if (scenario == "scene-change" || scenario == "repeated") {
                    band(previous, 0, side - 1, 40);
                    if (scenario == "scene-change")
                        auxiliary[1] = 1;
                    else
                        parameters.flags |= 1;
                } else {
                    throw std::runtime_error("unknown endpoint scenario");
                }
                fixture.transfer(fixture.resources[0].Get(), previous.data(), 4);
                fixture.transfer(fixture.resources[1].Get(), current.data(), 4);
                fixture.transfer(fixture.resources[2].Get(), flow.data(), 8);
                fixture.transfer(fixture.resources[3].Get(), auxiliary.data(), 4);
                auto output = fixture.draw(parameters, useNvidia);
                for (UINT eye = 0; eye < eyes; ++eye) {
                    for (UINT y = 0; y < side; ++y) {
                        for (UINT x = 0; x < side; ++x) {
                            const auto offset = ((eye * side + y) * side + x) * 4;
                            if (rotation && (x != (rotationCap                     ? 900u
                                                   : scenario == "rotation-motion" ? 32u
                                                                                   : 28u) ||
                                             y != (rotationCap ? 500u : 32u)))
                                continue;
                            if (falseMatch || scenario == "center-detail" || scenario == "isolated-detail" ||
                                scenario == "raw-endpoint" || scenario == "border" ||
                                scenario == "border-no-neighbors") {
                                const UINT checkedX = scenario == "border" ? 60 : 32;
                                if (x != checkedX || y != 32)
                                    continue;
                            }
                            for (UINT channel = 0; channel < 4; ++channel) {
                                double expected = current[offset + channel];
                                if (rotation && channel < 3)
                                    expected = rotationCap ? 0 : eye ? 237 : 255;
                                if ((scenario == "center-detail" || scenario == "isolated-detail") && channel < 3)
                                    expected = 0;
                                if (scenario == "repeated")
                                    expected = previous[offset + channel] * (1 - fraction) +
                                               current[offset + channel] * fraction;
                                if (translated) {
                                    if (x < 8 || x >= 48)
                                        continue;
                                    const double location = x + displacement * fraction;
                                    const UINT left = static_cast<UINT>(std::floor(location));
                                    const double weight = location - left;
                                    expected = pixel(previous, eye, left, y)[channel] * (1 - weight) +
                                               pixel(previous, eye, left + 1, y)[channel] * weight;
                                }
                                const double tolerance =
                                    uniform || gradient || falseMatch ? (srgb && channel < 3 ? 1.0 : 0.0) : 2.0;
                                compareChannel(output[offset + channel],
                                               expected,
                                               scenario.c_str(),
                                               eye,
                                               x,
                                               y,
                                               channel,
                                               tolerance);
                                if (uniform) {
                                    const auto eyeOffset = eye * side * side * 4;
                                    compareChannel(output[offset + channel],
                                                   output[eyeOffset + channel],
                                                   "exact spatial constancy",
                                                   eye,
                                                   x,
                                                   y,
                                                   channel,
                                                   0.0);
                                }
                                if (gradient) {
                                    // Require every grid seam to retain the supplied
                                    // step exactly, even if an sRGB round trip elsewhere
                                    // is allowed one code of quantization tolerance.
                                    if (x > 0 && x % block == 0) {
                                        const int actualStep = output[offset + channel] - output[offset + channel - 4];
                                        const int inputStep = current[offset + channel] - current[offset + channel - 4];
                                        check(actualStep == inputStep,
                                              "unexpected 8px horizontal near-white tile seam");
                                    }
                                    if (y > 0 && y % block == 0) {
                                        const int actualStep =
                                            output[offset + channel] - output[offset + channel - side * 4];
                                        const int inputStep =
                                            current[offset + channel] - current[offset + channel - side * 4];
                                        check(actualStep == inputStep, "unexpected 8px vertical near-white tile seam");
                                    }
                                }
                            }
                        }
                    }
                }
                std::printf("verified %s backend=%s fraction=%.6f both eyes\n",
                            scenario.c_str(),
                            useNvidia ? "NVIDIA" : "FidelityFX",
                            fraction);
            }
        }
        std::puts("PASS: production endpoint shader draw and readback");
        return 0;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: fg_endpoint_regression scenario");
        return run(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
