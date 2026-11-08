#include "depth_availability.hpp"
#include "depth_availability_Tiles.hpp"
#include "depth_availability_Reduce.hpp"
#include <cstring>

namespace vdxrfg {
    namespace {
        constexpr auto kFlagsRead =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                          D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) noexcept {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            return b;
        }
        D3D12_RESOURCE_BARRIER uav(ID3D12Resource* resource) noexcept {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = resource;
            return b;
        }
    } // namespace
    HRESULT DepthAvailability::initialize(ID3D12Device* device,
                                          ID3D12Resource* const histories[2],
                                          uint32_t atlasWidth,
                                          uint32_t atlasHeight) noexcept {
        if (device_ || !device || !histories || !histories[0] || !histories[1] || histories[0] == histories[1] ||
            !atlasWidth || (atlasWidth & 1u) || !atlasHeight || atlasWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            atlasHeight > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            return E_INVALIDARG;
        for (unsigned i = 0; i < 2; ++i) {
            const auto d = histories[i]->GetDesc();
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != atlasWidth || d.Height != atlasHeight ||
                d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 || d.SampleDesc.Quality != 0 ||
                d.Format != DXGI_FORMAT_R32_FLOAT || (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
                return E_INVALIDARG;
            Microsoft::WRL::ComPtr<ID3D12Device> owner;
            HRESULT hr = histories[i]->GetDevice(IID_PPV_ARGS(&owner));
            if (FAILED(hr))
                return hr;
            Microsoft::WRL::ComPtr<IUnknown> a, b;
            if (FAILED(hr = device->QueryInterface(IID_PPV_ARGS(&a))) || FAILED(hr = owner.As(&b)))
                return hr;
            if (a.Get() != b.Get())
                return E_INVALIDARG;
        }
        device_ = device;
        histories_[0] = histories[0];
        histories_[1] = histories[1];
        width_ = atlasWidth;
        height_ = atlasHeight;
        tilesX_ = (atlasWidth / 2u + 15u) / 16u;
        tilesY_ = (atlasHeight + 15u) / 16u;
        const uint32_t tileWords = tilesX_ * tilesY_ * 2u;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = uint64_t(tileWords) * 4u;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratch_));
        if (FAILED(hr))
            return hr;
        d.Width = 8;
        hr = device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&flags_));
        if (FAILED(hr))
            return hr;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 4;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_));
        if (FAILED(hr))
            return hr;
        increment_ = device->GetDescriptorHandleIncrementSize(heap.Type);
        auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        for (unsigned i = 0; i < 2; ++i) {
            device->CreateShaderResourceView(histories[i], &srv, cpu);
            cpu.ptr += increment_;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = tileWords;
        uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(scratch_.Get(), nullptr, &uv, cpu);
        cpu.ptr += increment_;
        uv.Buffer.NumElements = 2;
        device->CreateUnorderedAccessView(flags_.Get(), nullptr, &uv, cpu);
        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
        std::array<D3D12_ROOT_PARAMETER, 3> params{};
        for (unsigned i = 0; i < 2; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable = {1, &ranges[i]};
        }
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants = {0, 0, 12};
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = UINT(params.size());
        rd.pParameters = params.data();
        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr))
            return hr;
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
        if (FAILED(hr))
            return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root_.Get();
        ps.CS = {g_depth_availability_Tiles, sizeof(g_depth_availability_Tiles)};
        hr = device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&tilesPipeline_));
        if (FAILED(hr))
            return hr;
        ps.CS = {g_depth_availability_Reduce, sizeof(g_depth_availability_Reduce)};
        return device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&reducePipeline_));
    }
    HRESULT DepthAvailability::record(ID3D12GraphicsCommandList* list,
                                      uint32_t currentHistory,
                                      const VDXRFG_RectV1 rects[2]) noexcept {
        if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || currentHistory > 1 || !rects ||
            !reducePipeline_)
            return E_INVALIDARG;
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& r = rects[eye];
            const uint32_t first = eye * width_ / 2u, last = (eye + 1u) * width_ / 2u;
            if (!r.width || !r.height || r.x < first || r.x >= last || r.y >= height_ || r.width > last - r.x ||
                r.height > height_ - r.y)
                return E_INVALIDARG;
        }
        std::array<uint32_t, 12> constants = {tilesX_, tilesY_, currentHistory, 0};
        static_assert(sizeof(VDXRFG_RectV1) == 4 * sizeof(uint32_t));
        std::memcpy(constants.data() + 4, rects, 2 * sizeof(VDXRFG_RectV1));
        // Track the first COMMON state and same-list repeated records. Across
        // ExecuteCommandLists, buffer decay also permits the recorded promotable
        // read state as StateBefore. Scratch retains explicit UAV reuse ordering.
        const std::array<D3D12_RESOURCE_BARRIER, 3> begin = {
            transition(histories_[currentHistory].Get(),
                       D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(flags_.Get(), flagsState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            flagsState_ == D3D12_RESOURCE_STATE_COMMON
                ? transition(scratch_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                : uav(scratch_.Get())};
        list->ResourceBarrier(UINT(begin.size()), begin.data());
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        auto source = gpu;
        source.ptr += uint64_t(currentHistory) * increment_;
        gpu.ptr += 2ull * increment_;
        list->SetComputeRootDescriptorTable(0, source);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetComputeRoot32BitConstants(2, 12, constants.data(), 0);
        list->SetPipelineState(tilesPipeline_.Get());
        list->Dispatch(tilesX_, tilesY_, 2);
        const auto order = uav(scratch_.Get());
        list->ResourceBarrier(1, &order);
        list->SetPipelineState(reducePipeline_.Get());
        list->Dispatch(1, 1, 1);
        const std::array<D3D12_RESOURCE_BARRIER, 2> end = {
            transition(flags_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kFlagsRead),
            transition(histories_[currentHistory].Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COMMON)};
        list->ResourceBarrier(UINT(end.size()), end.data());
        flagsState_ = kFlagsRead;
        return S_OK;
    }
} // namespace vdxrfg
