#pragma once
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrSnapshotLayout.h"
#include <wrl/client.h>
namespace virtualdesktop_openxr::nr_capture {
    struct SnapshotPlan {
        uint64_t role{}, eye{}, group{}, copyMethod{}, subresources{};
        D3D11_TEXTURE2D_DESC texture11{};
        D3D12_RESOURCE_DESC texture12{}, readback12{};
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
        std::array<UINT, 2> rows{};
        std::array<uint64_t, 2> rowBytes{};
        SnapshotLayout layout{};
    };
    struct SnapshotCopyFact {
        uint64_t source{}, destination{}, contextOrList{}, sourceSubresource{}, destinationOffset{};
        uint64_t beginQpc{}, endQpc{}, thread{};
        bool beginObserved{}, endObserved{};
    };
    struct SnapshotAllocation {
        SnapshotPlan plan{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> initial11;
        Microsoft::WRL::ComPtr<ID3D12Resource> initial12;
        // Borrowed from initial reference until adoption; thereafter from host owner.
        ID3D11Texture2D* texture11{};
        ID3D12Resource* buffer12{};
        uint64_t allocationId{}, lifetime{}, writeEpoch{};
        NrDescriptorV1 sourceDescription{};
        vdxrfg::pair_capture::nr::NrGroupId ownerGroup{};
        std::array<SnapshotCopyFact, 2> copies{};
        bool allocated{}, adopted{}, copied{};
    };
    // Fixed two-frame role storage. A role may never be silently deduplicated.
    class SnapshotStore final {
      public:
        static constexpr size_t frames = 2, rolesPerFrame = 28;
        bool plan11(uint32_t slot, uint64_t role, uint64_t eye, uint64_t group, const D3D11_TEXTURE2D_DESC&) noexcept;
        bool plan12(uint32_t slot, uint64_t role, uint64_t eye, ID3D12Device*, const D3D12_RESOURCE_DESC&) noexcept;
        bool
        sealPlan(uint32_t count, uint64_t pairOnlyAllocation, uint64_t pairOnlyPayload, NrBudgetV1& budget) noexcept;
        bool allocate(ID3D11Device*, ID3D12Device*) noexcept;
        bool releaseInitialAfterAcquire(uint32_t frame, uint32_t slot, const OwnedGroupBoundary&) noexcept;
        bool copy11(uint32_t frame,
                    uint32_t slot,
                    ID3D11DeviceContext4*,
                    ID3D11Texture2D*,
                    const OwnedGroupBoundary&,
                    uint64_t sourceLifetime,
                    uint64_t sourceWrite) noexcept;
        bool copy12(uint32_t frame,
                    uint32_t slot,
                    ID3D12GraphicsCommandList*,
                    ID3D12Resource*,
                    const OwnedGroupBoundary&,
                    uint64_t sourceLifetime,
                    uint64_t sourceWrite) noexcept;
        SnapshotAllocation* get(uint32_t frame, uint32_t slot) noexcept;
        bool discardUnexposed() noexcept;
        uint32_t count() const noexcept {
            return count_;
        }
        uint64_t initialReferences(uint32_t frame) const noexcept;

      private:
        std::array<SnapshotPlan, rolesPerFrame> plan_{};
        std::array<std::array<SnapshotAllocation, rolesPerFrame>, frames> allocations_{};
        uint32_t count_{};
        uint64_t nextAllocation_{1}, nextCopy_{1}, nextDescription_{1};
        bool sealed_{}, allocated_{}, anyCopy_{};
    };
} // namespace virtualdesktop_openxr::nr_capture
