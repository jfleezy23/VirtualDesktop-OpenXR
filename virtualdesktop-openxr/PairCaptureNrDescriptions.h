#pragma once
#include "../nr-frame-generation/include/pair_capture_nr_control.h"
#include <d3d11.h>
#include <d3d12.h>
namespace virtualdesktop_openxr::nr_capture {
    inline bool descriptionIdentity(uint64_t resource,
                                    uint64_t lifetime,
                                    uint64_t sourceWrite,
                                    uint64_t observation,
                                    NrDescriptorV1& row) noexcept {
        row = {};
        if (!resource || !lifetime || lifetime == UINT64_MAX || !sourceWrite || sourceWrite == UINT64_MAX ||
            !observation || observation == UINT64_MAX)
            return false;
        row.kind = NR_DESC_RESOURCE_DESCRIPTION;
        row.objectAddress = row.resourceAddress = resource;
        row.objectLifetimeEpoch = row.resourceLifetimeEpoch = lifetime;
        row.sourceWriteEpoch = sourceWrite;
        row.writeEpoch = observation;
        row.observedMask = 15;
        return true;
    }
    // These serialize the caller's actual GetDesc result without normalizing its
    // format, flags, dimensions, layout or native API sizeof. No pointer is read.
    inline bool resourceDescription11(const D3D11_TEXTURE2D_DESC& description,
                                      uint64_t resource,
                                      uint64_t lifetime,
                                      uint64_t sourceWrite,
                                      uint64_t observation,
                                      NrDescriptorV1& row) noexcept {
        static_assert(sizeof description == 44);
        if (!descriptionIdentity(resource, lifetime, sourceWrite, observation, row))
            return false;
        row.apiDescriptionBytes = sizeof description;
        row.descriptionKind = NR_D11_TEXTURE2D_DESCRIPTION;
        auto* w = row.canonicalDescriptionWords;
        w[0] = description.Width;
        w[1] = description.Height;
        w[2] = description.MipLevels;
        w[3] = description.ArraySize;
        w[4] = description.Format;
        w[5] = description.SampleDesc.Count;
        w[6] = description.SampleDesc.Quality;
        w[7] = description.Usage;
        w[8] = description.BindFlags;
        w[9] = description.CPUAccessFlags;
        w[10] = description.MiscFlags;
        return true;
    }
    inline bool resourceDescription12(const D3D12_RESOURCE_DESC& description,
                                      uint64_t resource,
                                      uint64_t lifetime,
                                      uint64_t sourceWrite,
                                      uint64_t observation,
                                      NrDescriptorV1& row) noexcept {
        static_assert(sizeof description == 56);
        if (!descriptionIdentity(resource, lifetime, sourceWrite, observation, row))
            return false;
        row.apiDescriptionBytes = sizeof description;
        row.descriptionKind = NR_D12_RESOURCE_DESCRIPTION;
        auto* w = row.canonicalDescriptionWords;
        w[0] = description.Dimension;
        w[1] = description.Alignment;
        w[2] = description.Width;
        w[3] = description.Height;
        w[4] = description.DepthOrArraySize;
        w[5] = description.MipLevels;
        w[6] = description.Format;
        w[7] = description.SampleDesc.Count;
        w[8] = description.SampleDesc.Quality;
        w[9] = description.Layout;
        w[10] = description.Flags;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
