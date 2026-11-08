#pragma once
#include "../nr-frame-generation/include/pair_capture_nr_control.h"
#include <d3d11.h>
#include <d3d12.h>
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    inline uint64_t descriptorFloatBits(float value) noexcept {
        uint32_t bits{};
        std::memcpy(&bits, &value, sizeof bits);
        return bits;
    }
    inline void viewDescriptionHeader(NrDescriptorV1& out, uint64_t kind, uint64_t apiKind, uint64_t bytes) noexcept {
        out = {};
        out.kind = kind;
        out.descriptionKind = apiKind;
        out.apiDescriptionBytes = bytes;
    }
    // Only canonical fields are serialized. API padding and inactive union arms are
    // never hashed as descriptor evidence. Identity/write/bind facts are joined by
    // the actual creation and original binding observers, not by these serializers.
    inline bool viewDescription(const D3D11_SHADER_RESOURCE_VIEW_DESC& d, NrDescriptorV1& out) noexcept {
        out = {};
        if (d.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D && d.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
            return false;
        viewDescriptionHeader(out, NR_DESC_SRV, NR_D11_SRV, sizeof d);
        auto* w = out.canonicalDescriptionWords;
        w[0] = d.Format;
        w[1] = d.ViewDimension;
        if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            w[3] = d.Texture2D.MostDetailedMip;
            w[4] = d.Texture2D.MipLevels;
        } else {
            w[3] = d.Texture2DArray.MostDetailedMip;
            w[4] = d.Texture2DArray.MipLevels;
            w[5] = d.Texture2DArray.FirstArraySlice;
            w[6] = d.Texture2DArray.ArraySize;
        }
        return true;
    }
    inline bool viewDescription(const D3D11_UNORDERED_ACCESS_VIEW_DESC& d, NrDescriptorV1& out) noexcept {
        out = {};
        if (d.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D && d.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2DARRAY)
            return false;
        viewDescriptionHeader(out, NR_DESC_UAV, NR_D11_UAV, sizeof d);
        auto* w = out.canonicalDescriptionWords;
        w[0] = d.Format;
        w[1] = d.ViewDimension;
        if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D)
            w[2] = d.Texture2D.MipSlice;
        else {
            w[2] = d.Texture2DArray.MipSlice;
            w[3] = d.Texture2DArray.FirstArraySlice;
            w[4] = d.Texture2DArray.ArraySize;
        }
        return true;
    }
    inline bool viewDescription(const D3D11_SAMPLER_DESC& d, NrDescriptorV1& out) noexcept {
        viewDescriptionHeader(out, NR_DESC_SAMPLER, NR_D11_SAMPLER, sizeof d);
        auto* w = out.canonicalDescriptionWords;
        w[0] = d.Filter;
        w[1] = d.AddressU;
        w[2] = d.AddressV;
        w[3] = d.AddressW;
        w[4] = descriptorFloatBits(d.MipLODBias);
        w[5] = d.MaxAnisotropy;
        w[6] = d.ComparisonFunc;
        for (size_t i = 0; i < 4; ++i)
            w[7 + i] = descriptorFloatBits(d.BorderColor[i]);
        w[11] = descriptorFloatBits(d.MinLOD);
        w[12] = descriptorFloatBits(d.MaxLOD);
        return true;
    }
    inline bool viewDescription(const D3D12_SHADER_RESOURCE_VIEW_DESC& d, NrDescriptorV1& out) noexcept {
        out = {};
        if (d.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D && d.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY)
            return false;
        viewDescriptionHeader(out, NR_DESC_SRV, NR_D12_SRV, sizeof d);
        auto* w = out.canonicalDescriptionWords;
        w[0] = d.Format;
        w[1] = d.ViewDimension;
        w[2] = d.Shader4ComponentMapping;
        if (d.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            w[3] = d.Texture2D.MostDetailedMip;
            w[4] = d.Texture2D.MipLevels;
            w[7] = d.Texture2D.PlaneSlice;
            w[8] = descriptorFloatBits(d.Texture2D.ResourceMinLODClamp);
        } else {
            w[3] = d.Texture2DArray.MostDetailedMip;
            w[4] = d.Texture2DArray.MipLevels;
            w[5] = d.Texture2DArray.FirstArraySlice;
            w[6] = d.Texture2DArray.ArraySize;
            w[7] = d.Texture2DArray.PlaneSlice;
            w[8] = descriptorFloatBits(d.Texture2DArray.ResourceMinLODClamp);
        }
        return true;
    }
    inline bool viewDescription(const D3D12_UNORDERED_ACCESS_VIEW_DESC& d, NrDescriptorV1& out) noexcept {
        out = {};
        if (d.ViewDimension != D3D12_UAV_DIMENSION_TEXTURE2D && d.ViewDimension != D3D12_UAV_DIMENSION_TEXTURE2DARRAY)
            return false;
        viewDescriptionHeader(out, NR_DESC_UAV, NR_D12_UAV, sizeof d);
        auto* w = out.canonicalDescriptionWords;
        w[0] = d.Format;
        w[1] = d.ViewDimension;
        if (d.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) {
            w[2] = d.Texture2D.MipSlice;
            w[5] = d.Texture2D.PlaneSlice;
        } else {
            w[2] = d.Texture2DArray.MipSlice;
            w[3] = d.Texture2DArray.FirstArraySlice;
            w[4] = d.Texture2DArray.ArraySize;
            w[5] = d.Texture2DArray.PlaneSlice;
        }
        return true;
    }
    inline bool viewDescription(const D3D12_RENDER_TARGET_VIEW_DESC& d, NrDescriptorV1& out) noexcept {
        out = {};
        if (d.ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D && d.ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2DARRAY)
            return false;
        viewDescriptionHeader(out, NR_DESC_RTV, NR_D12_RTV, sizeof d);
        auto* w = out.canonicalDescriptionWords;
        w[0] = d.Format;
        w[1] = d.ViewDimension;
        if (d.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
            w[2] = d.Texture2D.MipSlice;
            w[5] = d.Texture2D.PlaneSlice;
        } else {
            w[2] = d.Texture2DArray.MipSlice;
            w[3] = d.Texture2DArray.FirstArraySlice;
            w[4] = d.Texture2DArray.ArraySize;
            w[5] = d.Texture2DArray.PlaneSlice;
        }
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
