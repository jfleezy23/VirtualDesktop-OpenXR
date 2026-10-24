#pragma once

#include "fg_gpu_helpers.h"

using AtlasPixels = std::vector<uint8_t>;

// Arbitrary RGBA8 dimensions/array slices for atlas-vs-array fixtures.
struct AtlasGpu : Gpu {
    using Gpu::Gpu;

    ComPtr<ID3D12Resource> texture(UINT imageWidth,
                                   UINT imageHeight,
                                   UINT slices = 1,
                                   D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = imageWidth;
        description.Height = imageHeight;
        description.DepthOrArraySize = static_cast<UINT16>(slices);
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> result;
        success(device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&result)),
                "atlas fixture texture");
        return result;
    }

    void execute(ID3D12GraphicsCommandList* list) {
        success(list->Close(), "close atlas fixture commands");
        ID3D12CommandList* lists[]{list};
        queue->ExecuteCommandLists(1, lists);
        const auto value = nextFence++;
        success(queue->Signal(fence.Get(), value), "atlas fixture signal");
        wait(fence.Get(), value);
        noErrors();
    }

    AtlasPixels transfer(ID3D12Resource* resource,
                         const AtlasPixels* pixels = nullptr,
                         D3D12_RESOURCE_STATES restingState = D3D12_RESOURCE_STATE_COMMON) {
        const auto description = resource->GetDesc();
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(description.DepthOrArraySize);
        UINT64 bytes{};
        device->GetCopyableFootprints(
            &description, 0, description.DepthOrArraySize, 0, layouts.data(), nullptr, nullptr, &bytes);
        const size_t rowBytes = static_cast<size_t>(description.Width) * 4;
        const size_t pixelBytes = rowBytes * description.Height * description.DepthOrArraySize;
        check(!pixels || pixels->size() == pixelBytes, "complete atlas fixture upload");
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.SampleDesc.Count = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = pixels ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> staging;
        success(
            device->CreateCommittedResource(&heap,
                                            D3D12_HEAP_FLAG_NONE,
                                            &buffer,
                                            pixels ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr,
                                            IID_PPV_ARGS(&staging)),
            "atlas fixture staging");
        const auto copyRows = [&](uint8_t* destination, const uint8_t* source, bool upload) {
            for (UINT slice = 0; slice < description.DepthOrArraySize; ++slice) {
                for (UINT y = 0; y < description.Height; ++y) {
                    const size_t packed = (slice * description.Height + y) * rowBytes;
                    const size_t padded = layouts[slice].Offset + y * layouts[slice].Footprint.RowPitch;
                    std::memcpy(
                        destination + (upload ? padded : packed), source + (upload ? packed : padded), rowBytes);
                }
            }
        };
        if (pixels) {
            void* mapped{};
            const D3D12_RANGE empty{};
            success(staging->Map(0, &empty, &mapped), "atlas fixture upload map");
            copyRows(static_cast<uint8_t*>(mapped), pixels->data(), true);
            staging->Unmap(0, nullptr);
        }
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "atlas fixture transfer allocator");
        success(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "atlas fixture transfer list");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource,
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              restingState,
                              pixels ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        for (UINT slice = 0; slice < description.DepthOrArraySize; ++slice) {
            D3D12_TEXTURE_COPY_LOCATION image{}, linear{};
            image.pResource = resource;
            image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            image.SubresourceIndex = slice;
            linear.pResource = staging.Get();
            linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            linear.PlacedFootprint = layouts[slice];
            list->CopyTextureRegion(pixels ? &image : &linear, 0, 0, 0, pixels ? &linear : &image, nullptr);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        execute(list.Get());
        AtlasPixels result;
        if (!pixels) {
            result.resize(pixelBytes);
            void* mapped{};
            const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
            success(staging->Map(0, &range, &mapped), "atlas fixture readback map");
            copyRows(result.data(), static_cast<const uint8_t*>(mapped), false);
            const D3D12_RANGE empty{};
            staging->Unmap(0, &empty);
        }
        return result;
    }
};
