#include "fg_gpu_helpers.h"

#include "fullscreen_vertex_shader.hpp"
#include "nvidia_synthesize_midpoint_pixel_shader.hpp"
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

    int run(const std::string& scenario) {
        const bool uniform = scenario == "uniform" || scenario == "uniform-srgb";
        const bool gradient = scenario == "nearwhite-gradient" || scenario == "nearwhite-gradient-srgb";
        const bool rotationCap = scenario == "rotation-cap";
        const bool rotation = scenario == "rotation-motion" || scenario == "rotation-static" || rotationCap;
        const bool falseMatch =
            scenario == "false-match" || scenario == "false-match-srgb" || scenario == "dark-false-match-srgb";
        const bool srgb = scenario == "uniform-srgb" || scenario == "nearwhite-gradient-srgb" ||
                          scenario == "false-match-srgb" || scenario == "dark-false-match-srgb";
        side = rotationCap ? 1024 : 64;
        ShaderFixture fixture(srgb);
        for (bool useNvidia : {true, false}) {
            if (useNvidia && !uniform && !gradient && !rotation)
                continue;
            for (float requested : {0.3f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.7f}) {
                Parameters parameters;
                parameters.flags = (static_cast<UINT>(std::lround(requested * 65535.0f)) << 8) | (srgb ? 2u : 0u);
                const double fraction = double(parameters.flags >> 8) / 65535.0;
                Pixels previous = flat(), current = flat();
                std::vector<int32_t> flow((side / block) * (side * eyes / block) * 2);
                std::vector<uint32_t> auxiliary((side / block) * (side * eyes / block));
                const int displacement = gradient ? 0
                                                  : (scenario == "translated-detail" || scenario == "border" ||
                                                             scenario == "border-no-neighbors"
                                                         ? 8
                                                         : 16);
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
                } else if (scenario == "translated-detail") {
                    for (UINT eye = 0; eye < eyes; ++eye) {
                        for (UINT y = 0; y < side; ++y) {
                            for (UINT x = 0; x < side; ++x) {
                                auto value = [](UINT p) { return static_cast<uint8_t>((p % 8) < 2 ? 230 : 35); };
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
                                if (scenario == "translated-detail") {
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
