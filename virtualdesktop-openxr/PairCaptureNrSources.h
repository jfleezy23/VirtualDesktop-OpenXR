#pragma once
#include "PairCaptureNrSourceFacts.h"
#include <d3d11_4.h>
#include <d3d12.h>
namespace virtualdesktop_openxr::nr_capture {
    uint64_t sourceClock() noexcept;
    void sourceSwapchainCreated(const void*) noexcept;
    void sourceSwapchainDestroyed(const void*) noexcept;
    void sourceTextureCreated(const void* swapchain,
                              uint32_t index,
                              ID3D11Texture2D*,
                              ID3D11Device* actualDevice,
                              const D3D11_TEXTURE2D_DESC&) noexcept;
    void sourceQueryObserved(const void* swapchain,
                             uint32_t index,
                             const void* texture,
                             const void* actualInterface,
                             int32_t result,
                             uint64_t begin,
                             uint64_t end) noexcept;
    void sourceExportObserved(const void* swapchain,
                              uint32_t index,
                              HANDLE handle,
                              bool nt,
                              uint32_t access,
                              int32_t result,
                              uint64_t begin,
                              uint64_t end) noexcept;
    void sourceImportObserved(const void* swapchain,
                              uint32_t index,
                              ID3D11Device* actualDevice,
                              const void* interfaceThis,
                              ID3D11Texture2D* output,
                              HANDLE handle,
                              bool nt,
                              int32_t result,
                              uint64_t begin,
                              uint64_t end) noexcept;
    void sourceImageReleased(const void* swapchain, uint32_t index) noexcept;
    bool sourceGet(const void* swapchain, uint32_t index, SourceTextureFacts&, D3D11_TEXTURE2D_DESC&) noexcept;
    void nrBankTextureObserved(ID3D11Texture2D*, ID3D11Device*) noexcept;
    uint64_t nrBankTextureLifetime(ID3D11Texture2D*) noexcept;
    struct ResourceWriteFact;
    // Resolve an actual app-interface copy through its observed API4 storage alias.
    // The returned fact retains the source640 writer; no producer command is added.
    bool sourceApplicationWrite11(uint64_t producer,
                                  uint64_t producerLifetime,
                                  ResourceWriteFact&,
                                  SourceTextureFacts* applicationAlias = nullptr) noexcept;
    bool sourceProducerWrite11(ID3D11Texture2D*, ResourceWriteFact&) noexcept;
    void sourceBankWriteObserved(ID3D11Texture2D*,
                                 ID3D11DeviceContext4*,
                                 uint64_t serial,
                                 uint64_t kind,
                                 ID3D11Texture2D* actualSource = nullptr) noexcept;
    void sourceNrQueryObserved(
        ID3D11Texture2D*, const void* actualInterface, int32_t result, uint64_t begin, uint64_t end) noexcept;
    void sourceNrExportObserved(
        ID3D11Texture2D*, HANDLE, bool nt, uint32_t access, int32_t result, uint64_t begin, uint64_t end) noexcept;
    void sourceNrImportObserved(ID3D11Texture2D*,
                                ID3D12Device*,
                                ID3D12Resource*,
                                uint64_t targetLifetime,
                                HANDLE,
                                bool nt,
                                int32_t result,
                                uint64_t begin,
                                uint64_t end) noexcept;
    bool sourceNrGet(ID3D12Resource*, SourceTextureFacts&) noexcept;
    bool sourceNrOriginalWrite12(ID3D12Resource*, uint64_t lifetime, ResourceWriteFact&) noexcept;
    struct NrOutputFenceFacts {
        uint64_t context{}, contextLifetime{};
        NrAliasV1 alias{};
        NrCallV1 exportCall{}, importCall{};
    };
    void sourceNrFenceExportObserved(
        const void*, uint64_t, ID3D12Device*, ID3D12Fence*, HANDLE, int32_t, uint64_t, uint64_t) noexcept;
    void sourceNrFenceImportObserved(
        const void*, uint64_t, ID3D11Device*, ID3D11Fence*, HANDLE, int32_t, uint64_t, uint64_t) noexcept;
    bool sourceNrFenceGet(const void*, uint64_t, NrOutputFenceFacts&) noexcept;
    void sourceNrInputFenceExportObserved(
        const void*, uint64_t, ID3D11Device*, ID3D11Fence*, HANDLE, int32_t, uint64_t, uint64_t) noexcept;
    void sourceNrInputFenceImportObserved(
        const void*, uint64_t, ID3D12Device*, ID3D12Fence*, HANDLE, int32_t, uint64_t, uint64_t) noexcept;
    bool sourceNrInputFenceGet(const void*, uint64_t, NrOutputFenceFacts&) noexcept;

} // namespace virtualdesktop_openxr::nr_capture
