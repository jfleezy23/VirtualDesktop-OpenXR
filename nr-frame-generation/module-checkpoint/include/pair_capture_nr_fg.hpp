#pragma once
#include "vdxr_fg_api.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <mutex>
namespace vdxrfg::pair_capture {
    struct Host;
}
namespace vdxrfg::pair_capture::nr {
    void fgPrepare(void*) noexcept;
    bool fgBound(void*, ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource* const[8], const wchar_t*) noexcept;
    bool fgInvalidateBudget(void*) noexcept;
    bool fgForgetContext(void*) noexcept;
    void fgReset(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, HRESULT) noexcept;
    void fgAllocatorReset(ID3D12CommandAllocator*, HRESULT) noexcept;
    void fgClosed(ID3D12GraphicsCommandList*, HRESULT) noexcept;
    void fgRoot(ID3D12GraphicsCommandList*, ID3D12RootSignature*) noexcept;
    void fgGuideCopy(ID3D12Resource*, ID3D12Resource*, ID3D12GraphicsCommandList*, uint64_t) noexcept;
    void fgRtvCreated(ID3D12Resource*,
                      const D3D12_RENDER_TARGET_VIEW_DESC&,
                      D3D12_CPU_DESCRIPTOR_HANDLE,
                      ID3D12DescriptorHeap*,
                      UINT,
                      UINT) noexcept;
    void fgCbvWrite(ID3D12Resource*, UINT) noexcept;
    void fgSrvCreated(void*,
                      ID3D12Resource*,
                      const D3D12_SHADER_RESOURCE_VIEW_DESC&,
                      ID3D12DescriptorHeap*,
                      UINT,
                      UINT,
                      D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
    void fgCbv(void*,
               ID3D12Resource*,
               ID3D12DescriptorHeap*,
               UINT,
               UINT,
               UINT,
               UINT,
               const D3D12_CONSTANT_BUFFER_VIEW_DESC&,
               D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
    void fgRootTable(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) noexcept;
    void fgTargets(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*) noexcept;
    void fgViewport(ID3D12GraphicsCommandList*, const D3D12_VIEWPORT&, const D3D12_RECT&) noexcept;
    void fgConfigure(void*,
                     ID3D12Resource*,
                     ID3D12Resource*,
                     uint64_t,
                     uint64_t,
                     uint64_t,
                     const D3D12_SHADER_RESOURCE_VIEW_DESC&) noexcept;
    void fgSources(void*,
                   ID3D12Resource*,
                   ID3D12Resource*,
                   const D3D12_SHADER_RESOURCE_VIEW_DESC&,
                   ID3D12DescriptorHeap*,
                   UINT) noexcept;
    void fgTicket(void*, const VDXRFG_TicketV1&) noexcept;
    void fgAdmissionBegin(void*, uint64_t) noexcept;
    void fgRetireBegin(void*, uint64_t) noexcept;
    void fgHistory(ID3D12Resource*, ID3D12Resource*, ID3D12GraphicsCommandList*, uint64_t, uint32_t) noexcept;
    void fgBarriers(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*) noexcept;
    void fgQueued(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
    void fgSignalled(const char*, ID3D12CommandQueue*, ID3D12Fence*, uint64_t, HRESULT) noexcept;
    void fgDraw(void*,
                ID3D12GraphicsCommandList*,
                ID3D12PipelineState*,
                UINT,
                UINT,
                UINT,
                const void*,
                const void*,
                size_t,
                UINT,
                ID3D12Resource*,
                const unsigned char*,
                size_t) noexcept;
    void fgHost(const Host&) noexcept;
    bool fgHoldRetire(void*, uint64_t) noexcept;
    void fgAbort(void*) noexcept;
    HRESULT fgOutputWait(void*, uint64_t, uint32_t, ID3D11DeviceContext4*) noexcept;
    uint32_t fgStatus(uint32_t) noexcept;
    void fgQuery() noexcept;
    // Serializes control-only ABORT and final file publication. Never held for
    // transfer acquisition or GPU/COM operations; separate from observer locks.
    std::recursive_mutex& fgPublicationGate() noexcept;
    size_t fgFixedMetadataBytes() noexcept;
} // namespace vdxrfg::pair_capture::nr
