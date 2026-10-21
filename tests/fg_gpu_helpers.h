#pragma once

#include "vdxr_fg_api.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
constexpr UINT width = 1024, height = 256, eyeWidth = width / 2;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void success(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char message[256];
        sprintf_s(message, "%s (HRESULT %08lx)", operation, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> diagnostics;
    uint64_t nextFence{1};
    bool debugEnabled{};

    explicit Gpu(bool enableValidation = true) {
        ComPtr<ID3D12Debug> debug;
        if (enableValidation && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debugEnabled = true;
        }
        ComPtr<IDXGIFactory6> factory;
        success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(
                             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_ADAPTER_DESC1 description{};
            success(adapter->GetDesc1(&description), "adapter description");
            if (description.VendorId == 0x10de && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                std::wprintf(L"NVIDIA adapter: %ls; debug layer=%u\n", description.Description, debugEnabled);
                break;
            }
            adapter.Reset();
        }
        check(device != nullptr, "NVIDIA hardware required; no WARP fallback");
        D3D12_COMMAND_QUEUE_DESC queueInfo{};
        queueInfo.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        success(device->CreateCommandQueue(&queueInfo, IID_PPV_ARGS(&queue)), "direct queue");
        success(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fixture fence");
        device.As(&diagnostics);
    }

    void wait(ID3D12Fence* completion, uint64_t value) {
        check(completion != nullptr && completion->GetCompletedValue() != UINT64_MAX, "valid completion fence");
        if (completion->GetCompletedValue() >= value) {
            return;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(event != nullptr, "create bounded completion event");
        const HRESULT hr = completion->SetEventOnCompletion(value, event);
        const DWORD status = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
        CloseHandle(event);
        success(hr, "completion event");
        check(status == WAIT_OBJECT_0, "GPU completion exceeded 10 seconds");
        check(completion->GetCompletedValue() != UINT64_MAX, "GPU removed during completion");
    }

    ComPtr<ID3D12Resource> texture() {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> resource;
        success(device->CreateCommittedResource(&heap,
                                                D3D12_HEAP_FLAG_NONE,
                                                &description,
                                                D3D12_RESOURCE_STATE_COMMON,
                                                nullptr,
                                                IID_PPV_ARGS(&resource)),
                "create texture");
        return resource;
    }

    std::vector<uint8_t> transfer(ID3D12Resource* texture, const std::vector<uint8_t>* pixels) {
        const auto description = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint64_t bytes{};
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
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
            "staging resource");
        if (pixels) {
            void* data{};
            const D3D12_RANGE empty{};
            success(staging->Map(0, &empty, &data), "upload map");
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(static_cast<uint8_t*>(data) + footprint.Footprint.RowPitch * y,
                            pixels->data() + width * 4 * y,
                            width * 4);
            }
            staging->Unmap(0, nullptr);
        }
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "copy allocator");
        success(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "copy list");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture,
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COMMON,
                              pixels ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION image{};
        image.pResource = texture;
        image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION linear{};
        linear.pResource = staging.Get();
        linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        linear.PlacedFootprint = footprint;
        list->CopyTextureRegion(pixels ? &image : &linear, 0, 0, 0, pixels ? &linear : &image, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        success(list->Close(), "close copy list");
        ID3D12CommandList* commands[] = {list.Get()};
        queue->ExecuteCommandLists(1, commands);
        const uint64_t value = nextFence++;
        success(queue->Signal(fence.Get(), value), "copy signal");
        wait(fence.Get(), value);
        std::vector<uint8_t> result;
        if (!pixels) {
            result.resize(width * height * 4);
            void* data{};
            const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
            success(staging->Map(0, &range, &data), "readback map");
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(result.data() + width * 4 * y,
                            static_cast<uint8_t*>(data) + footprint.Footprint.RowPitch * y,
                            width * 4);
            }
            const D3D12_RANGE empty{};
            staging->Unmap(0, &empty);
        }
        return result;
    }

    void noErrors() {
        if (!diagnostics) {
            return;
        }
        for (uint64_t index = 0; index < diagnostics->GetNumStoredMessages(); ++index) {
            SIZE_T size{};
            success(diagnostics->GetMessage(index, nullptr, &size), "debug message size");
            std::vector<uint8_t> storage(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            success(diagnostics->GetMessage(index, message, &size), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                throw std::runtime_error(message->pDescription);
            }
        }
    }
};

std::vector<uint8_t> scene(UINT shift) {
    std::vector<uint8_t> pixels(width * height * 4);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            auto* p = pixels.data() + (y * width + x) * 4;
            const UINT eye = x / eyeWidth;
            const UINT local = x % eyeWidth;
            // Stationary high-frequency texture supports flow, while each eye
            // has a different moving shape/channel. Alpha is deliberately nonopaque.
            const uint8_t texture = static_cast<uint8_t>(20 + ((local / 8 + y / 8) % 2) * 12);
            p[0] = p[1] = p[2] = texture;
            const UINT center = (eye ? 300 : 120) + (eye ? 0 : shift);
            if (local >= center && local < center + 48 && y >= 96 && y < 160) {
                p[eye ? 0 : 1] = 240;
            }
            p[3] = static_cast<uint8_t>(80 + (local % 80));
        }
    }
    return pixels;
}
