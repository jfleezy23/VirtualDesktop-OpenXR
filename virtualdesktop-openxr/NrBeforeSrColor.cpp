#include "NrBeforeSrColor.h"

#include "NrBeforeSrColorEncode_DXIL.h"
#include "NrBeforeSrColorResolve_DXIL.h"

#include <cmath>
#include <utility>

namespace virtualdesktop_openxr::nr_before_sr {
    using Microsoft::WRL::ComPtr;

    namespace {
        constexpr auto ModelFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr auto Common = D3D12_RESOURCE_STATE_COMMON;
        constexpr auto Read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto Write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr UINT SrvCount = 4;
        constexpr UINT UavCount = 2;
        constexpr UINT TableSize = SrvCount + UavCount;
        constexpr UINT RootDwords = 9;

        bool onDevice(ID3D12DeviceChild* child, IUnknown* device) noexcept {
            ComPtr<IUnknown> owner;
            return child && device && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == device;
        }

        bool supportsViews(ID3D12Device* device, DXGI_FORMAT format, bool writable = true) noexcept {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            const auto required =
                D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                (writable ? D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW : 0);
            return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                   (support.Support1 & required) == required &&
                   (!writable || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
        }

        bool createScratchTexture(ID3D12Device* device,
                                  Rect active,
                                  DXGI_FORMAT format,
                                  ComPtr<ID3D12Resource>& texture,
                                  D3D12_RESOURCE_STATES initialState = Common) noexcept {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            properties.CreationNodeMask = properties.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = active.width;
            desc.Height = active.height;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&texture)));
        }

        D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                          D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) noexcept {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {resource, 0, before, after};
            return barrier;
        }
    } // namespace

    bool ColorPipeline::initialize(ID3D12Device* device) noexcept {
        if (!device || m_device || device->GetNodeCount() != 1 || !supportsViews(device, ModelFormat))
            return false;
        ComPtr<IUnknown> identity;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))))
            return false;
        D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, SrvCount, 0, 0, 0},
                                        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UavCount, 0, 0, SrvCount}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, RootDwords};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)) ||
            FAILED(device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root))))
            return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.CS = {g_NrBeforeSrColorEncode_DXIL, sizeof(g_NrBeforeSrColorEncode_DXIL)};
        ComPtr<ID3D12PipelineState> encode, resolve;
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&encode))))
            return false;
        desc.CS = {g_NrBeforeSrColorResolve_DXIL, sizeof(g_NrBeforeSrColorResolve_DXIL)};
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&resolve))))
            return false;
        m_root = std::move(root);
        m_encode = std::move(encode);
        m_resolve = std::move(resolve);
        m_device = std::move(identity);
        return true;
    }

    bool ColorScratch::initialize(
        ID3D12Device* device, ID3D12Resource* source, Rect active, ID3D12Resource* depth, Rect depthRect) noexcept {
        if (!device || !source || !depth || m_device || device->GetNodeCount() != 1)
            return false;
        ComPtr<IUnknown> identity;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) || !onDevice(source, identity.Get()) ||
            !onDevice(depth, identity.Get()))
            return false;
        const auto plan = PlanColorViews(source->GetDesc(), active, depth->GetDesc(), depthRect);
        if (!plan || !supportsViews(device, plan->resolvedFormat))
            return false;

        const bool extractDepth = plan->depthPlaneSrv != DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12Resource> input, answer, resolved, extracted;
        if (extractDepth &&
            (!supportsViews(device, plan->depthPlaneSrv, false) || !supportsViews(device, DXGI_FORMAT_R32_FLOAT) ||
             !createScratchTexture(device, active, DXGI_FORMAT_R32_FLOAT, extracted)))
            return false;
        // Match the final SR input state so a replay starts from the same state as the first execution.
        if (!createScratchTexture(device, active, ModelFormat, input) ||
            !createScratchTexture(device, active, ModelFormat, answer) ||
            !createScratchTexture(device, active, plan->resolvedFormat, resolved, Read))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.NumDescriptors = 2 * TableSize;
        ComPtr<ID3D12DescriptorHeap> views;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&views))))
            return false;
        const UINT stride = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        if (!stride)
            return false;
        auto cpu = views->GetCPUDescriptorHandleForHeapStart();
        const auto srv = [&](ID3D12Resource* texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            view.Texture2D.PlaneSlice = 0; // The depth component of D32S8, never the stencil plane.
            device->CreateShaderResourceView(texture, &view, cpu);
            cpu.ptr += stride;
        };
        const auto uav = [&](ID3D12Resource* texture, DXGI_FORMAT format) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(texture, nullptr, &view, cpu);
            cpu.ptr += stride;
        };
        srv(source, plan->resolvedFormat);
        srv(nullptr, ModelFormat);
        srv(nullptr, ModelFormat);
        srv(extractDepth ? depth : nullptr, extractDepth ? plan->depthPlaneSrv : DXGI_FORMAT_R32_FLOAT);
        uav(input.Get(), ModelFormat);
        uav(extracted.Get(), DXGI_FORMAT_R32_FLOAT);
        srv(source, plan->resolvedFormat);
        srv(input.Get(), ModelFormat);
        srv(answer.Get(), ModelFormat);
        srv(nullptr, DXGI_FORMAT_R32_FLOAT);
        uav(resolved.Get(), plan->resolvedFormat);
        uav(nullptr, DXGI_FORMAT_R32_FLOAT);

        m_source = source;
        m_depthSource = depth;
        m_extractedDepth = std::move(extracted);
        m_input = std::move(input);
        m_answer = std::move(answer);
        m_resolved = std::move(resolved);
        m_views = std::move(views);
        m_device = std::move(identity);
        m_stride = stride;
        m_active = active;
        m_depthRect = depthRect;
        m_resolvedFormat = plan->resolvedFormat;
        return true;
    }

    bool ColorScratch::reuseRetired(
        ID3D12Device* device, ID3D12Resource* source, Rect active, ID3D12Resource* depth, Rect depthRect) noexcept {
        const auto sameRect = [](Rect a, Rect b) {
            return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        };
        if (m_phase != Phase::Finished || m_source.Get() != source || m_depthSource.Get() != depth ||
            !sameRect(m_active, active) || !sameRect(m_depthRect, depthRect) || !device)
            return false;
        ComPtr<IUnknown> identity;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) || identity.Get() != m_device.Get())
            return false;

        m_preExposure = 0.f;
        m_phase = Phase::Fresh;
        return true;
    }

    void ColorPipeline::dispatch(ID3D12GraphicsCommandList* list,
                                 const ColorScratch& scratch,
                                 ID3D12PipelineState* program,
                                 UINT firstDescriptor,
                                 float preExposure) const noexcept {
        struct Constants {
            Rect active;
            float exposure;
            UINT depthX, depthY, extractDepth, packedColor;
        };
        static_assert(sizeof(Constants) == RootDwords * sizeof(UINT));
        const Constants constants{scratch.m_active,
                                  preExposure,
                                  scratch.m_depthRect.x,
                                  scratch.m_depthRect.y,
                                  scratch.m_extractedDepth ? 1u : 0u,
                                  scratch.m_resolvedFormat == DXGI_FORMAT_R11G11B10_FLOAT ? 1u : 0u};
        ID3D12DescriptorHeap* heap = scratch.m_views.Get();
        auto table = heap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += UINT64(firstDescriptor) * scratch.m_stride;
        list->SetDescriptorHeaps(1, &heap);
        list->SetComputeRootSignature(m_root.Get());
        list->SetPipelineState(program);
        list->SetComputeRoot32BitConstants(0, RootDwords, &constants, 0);
        list->SetComputeRootDescriptorTable(1, table);
        list->Dispatch((constants.active.width + 7) / 8, (constants.active.height + 7) / 8, 1);
    }

    bool ColorPipeline::encode(ID3D12GraphicsCommandList* list,
                               ColorScratch& scratch,
                               float preExposure) const noexcept {
        if (!m_device || m_device.Get() != scratch.m_device.Get() || !onDevice(list, m_device.Get()) ||
            list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || scratch.m_phase != ColorScratch::Phase::Fresh ||
            !std::isnormal(preExposure) || preExposure <= 0.f)
            return false;
        D3D12_RESOURCE_BARRIER barriers[2]{transition(scratch.m_input.Get(), Common, Write)};
        const UINT count = scratch.m_extractedDepth ? 2u : 1u;
        if (scratch.m_extractedDepth)
            barriers[1] = transition(scratch.m_extractedDepth.Get(), Common, Write);
        list->ResourceBarrier(count, barriers);
        dispatch(list, scratch, m_encode.Get(), 0, preExposure);
        for (UINT i = 0; i < count; ++i)
            std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
        list->ResourceBarrier(count, barriers);
        scratch.m_phase = ColorScratch::Phase::Encoded;
        scratch.m_recordingList = list;
        scratch.m_preExposure = preExposure;
        return true;
    }

    bool ColorPipeline::resolve(ID3D12GraphicsCommandList* list,
                                ColorScratch& scratch,
                                float preExposure) const noexcept {
        if (!m_device || m_device.Get() != scratch.m_device.Get() || scratch.m_phase != ColorScratch::Phase::Encoded ||
            scratch.m_recordingList != list || scratch.m_preExposure != preExposure)
            return false;

        D3D12_RESOURCE_BARRIER readModel[]{transition(scratch.m_input.Get(), Common, Read),
                                           transition(scratch.m_answer.Get(), Common, Read),
                                           transition(scratch.m_resolved.Get(), Read, Write)};
        list->ResourceBarrier(3, readModel);
        dispatch(list, scratch, m_resolve.Get(), TableSize, preExposure);

        D3D12_RESOURCE_BARRIER finish[]{transition(scratch.m_input.Get(), Read, Common),
                                        transition(scratch.m_answer.Get(), Read, Common),
                                        transition(scratch.m_resolved.Get(), Write, Read)};
        list->ResourceBarrier(3, finish);
        scratch.m_phase = ColorScratch::Phase::Finished;
        scratch.m_recordingList = nullptr;
        return true;
    }

    bool ColorPipeline::recover(ID3D12GraphicsCommandList* list, ColorScratch& scratch) const noexcept {
        if (!m_device || m_device.Get() != scratch.m_device.Get() || scratch.m_phase != ColorScratch::Phase::Encoded ||
            scratch.m_recordingList != list)
            return false;
        // Encode left the source in its game read state. Failed vendor commands may own private state.
        // Never issue speculative barriers against their scratch. Retain it until the ledger retires it.
        scratch.m_phase = ColorScratch::Phase::Abandoned;
        scratch.m_recordingList = nullptr;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_before_sr
