#include "PairCaptureNrSnapshots.h"
#include "PairCaptureNrDescriptions.h"
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        uint64_t clock() noexcept {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            return static_cast<uint64_t>(q.QuadPart);
        }
        bool same11(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) noexcept {
            return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
                   a.ArraySize == b.ArraySize && a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
                   a.SampleDesc.Quality == b.SampleDesc.Quality && a.Usage == b.Usage && a.BindFlags == b.BindFlags &&
                   a.CPUAccessFlags == b.CPUAccessFlags && a.MiscFlags == b.MiscFlags;
        }
        bool same12(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) noexcept {
            return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width &&
                   a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
                   a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
                   a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout && a.Flags == b.Flags;
        }
        bool roleEye(uint64_t role, uint64_t eye) noexcept {
            return role >= 1 && role <= 16 && eye <= 2;
        }
        SnapshotCopyFact beginCopy(const void* source,
                                   const void* target,
                                   const void* context,
                                   uint64_t subresource,
                                   uint64_t offset) noexcept {
            SnapshotCopyFact f{};
            f.source = reinterpret_cast<uint64_t>(source);
            f.destination = reinterpret_cast<uint64_t>(target);
            f.contextOrList = reinterpret_cast<uint64_t>(context);
            f.sourceSubresource = subresource;
            f.destinationOffset = offset;
            f.beginObserved = true;
            f.beginQpc = clock();
            f.thread = GetCurrentThreadId();
            return f;
        }
        void endCopy(SnapshotCopyFact& f) noexcept {
            f.endQpc = clock();
            f.endObserved = true;
        }
    } // namespace
    bool SnapshotStore::plan11(
        uint32_t slot, uint64_t role, uint64_t eye, uint64_t group, const D3D11_TEXTURE2D_DESC& desc) noexcept {
        if (sealed_ || slot >= rolesPerFrame || plan_[slot].role || !roleEye(role, eye) || (group != 0 && group != 2) ||
            desc.SampleDesc.Quality)
            return false;
        SnapshotPlan plan{};
        plan.role = role;
        plan.eye = eye;
        plan.group = group;
        plan.copyMethod = 1;
        plan.texture11 = desc;
        plan.subresources = desc.ArraySize;
        if (!layout11(desc.Width,
                      desc.Height,
                      desc.ArraySize,
                      desc.MipLevels,
                      desc.SampleDesc.Count,
                      desc.Format,
                      plan.layout))
            return false;
        plan_[slot] = plan;
        return true;
    }
    bool SnapshotStore::plan12(
        uint32_t slot, uint64_t role, uint64_t eye, ID3D12Device* device, const D3D12_RESOURCE_DESC& desc) noexcept {
        if (sealed_ || slot >= rolesPerFrame || plan_[slot].role || !roleEye(role, eye) || !device ||
            desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || !desc.DepthOrArraySize ||
            desc.DepthOrArraySize > 2 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
            desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || !desc.Width || desc.Width > UINT32_MAX || !desc.Height ||
            !pixelBytes(desc.Format))
            return false;
        SnapshotPlan plan{};
        plan.role = role;
        plan.eye = eye;
        plan.group = 1;
        plan.copyMethod = 2;
        plan.texture12 = desc;
        plan.subresources = desc.DepthOrArraySize;
        uint64_t apiBytes{};
        device->GetCopyableFootprints(&desc,
                                      0,
                                      static_cast<UINT>(plan.subresources),
                                      0,
                                      plan.footprints.data(),
                                      plan.rows.data(),
                                      plan.rowBytes.data(),
                                      &apiBytes);
        uint64_t extent = apiBytes;
        for (size_t i = 0; i < plan.subresources; ++i) {
            const auto& f = plan.footprints[i];
            uint64_t rowBytes{}, bytes{}, end = f.Offset;
            if (!multiplyBytes(desc.Width, pixelBytes(desc.Format), rowBytes) || plan.rowBytes[i] != rowBytes ||
                plan.rows[i] != desc.Height || f.Footprint.Width != desc.Width || f.Footprint.Height != desc.Height ||
                f.Footprint.Depth != 1 || f.Footprint.RowPitch < rowBytes || f.Footprint.RowPitch % 256 ||
                f.Offset % 512 || !multiplyBytes(f.Footprint.RowPitch, plan.rows[i], bytes) || !addBytes(end, bytes))
                return false;
            if (end > extent)
                extent = end;
            if (f.Footprint.RowPitch > plan.layout.rowPitchBound)
                plan.layout.rowPitchBound = f.Footprint.RowPitch;
        }
        if (!alignBytes(extent, 512, plan.layout.payloadBound) || !plan.layout.payloadBound ||
            plan.layout.payloadBound > captureByteCap)
            return false;
        plan.layout.rowBytes = plan.rowBytes[0];
        plan.readback12.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        plan.readback12.Width = plan.layout.payloadBound;
        plan.readback12.Height = 1;
        plan.readback12.DepthOrArraySize = 1;
        plan.readback12.MipLevels = 1;
        plan.readback12.SampleDesc.Count = 1;
        plan.readback12.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &plan.readback12);
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX ||
            allocation.SizeInBytes < plan.layout.payloadBound || allocation.SizeInBytes > captureByteCap ||
            !allocation.Alignment)
            return false;
        plan.layout.allocationCharge = allocation.SizeInBytes;
        plan_[slot] = plan;
        return true;
    }
    bool SnapshotStore::sealPlan(uint32_t count,
                                 uint64_t pairAllocation,
                                 uint64_t pairPayload,
                                 NrBudgetV1& budget) noexcept {
        budget = {};
        if (sealed_ || !count || count > rolesPerFrame || !pairAllocation || !pairPayload ||
            pairAllocation < pairPayload)
            return false;
        uint64_t payload = pairPayload, physical = pairAllocation, perFrame = 0, perFramePayload = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (!plan_[i].role)
                return false;
            for (uint32_t j = 0; j < i; ++j)
                if (plan_[j].role == plan_[i].role && plan_[j].eye == plan_[i].eye)
                    return false;
            if (!addBytes(perFrame, plan_[i].layout.allocationCharge) ||
                !addBytes(perFramePayload, plan_[i].layout.payloadBound))
                return false;
        }
        for (size_t frame = 0; frame < 2; ++frame)
            if (!addBytes(physical, perFrame) || !addBytes(payload, perFramePayload))
                return false;
        uint64_t complete = physical;
        constexpr uint64_t metadata = 8 * 1024 * 1024, control = 8 * 1024 * 1024;
        if (!addBytes(complete, metadata) || !addBytes(complete, control) || complete > captureByteCap)
            return false;
        budget.capBytes = captureByteCap;
        budget.cpuMetadataReservation = metadata;
        budget.privatePayloadReservation = payload;
        budget.privateAllocationReservation = physical + control;
        budget.mappedPitchReservation = payload;
        budget.bytesA = perFrame;
        budget.bytesB = perFrame;
        budget.pairOnlyBytes = pairAllocation;
        budget.physicalPrivatePeakBytes = complete;
        budget.logicalSenderCharge = complete;
        budget.logicalReceiverCharge = complete;
        budget.roleCountA = count;
        budget.roleCountB = count;
        budget.pairOnlyRoleCount = 5;
        budget.allocationCount = uint64_t(count) * 2 + 5;
        budget.descriptorCapacity = 64;
        budget.sourceObservationCapacity = 8192;
        budget.allArithmeticChecked = 1;
        // preflightComplete/PIN/ref inventory remain unset until actual caller joins.
        count_ = count;
        sealed_ = true;
        return true;
    }
    bool SnapshotStore::allocate(ID3D11Device* device11, ID3D12Device* device12) noexcept {
        if (!sealed_ || allocated_ || !device11 || !device12)
            return false;
        for (auto& frame : allocations_)
            for (uint32_t slot = 0; slot < count_; ++slot) {
                auto& a = frame[slot];
                a.plan = plan_[slot];
                HRESULT result{};
                if (a.plan.copyMethod == 1) {
                    auto desc = a.plan.texture11;
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.BindFlags = 0;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    desc.MiscFlags = 0;
                    result = device11->CreateTexture2D(&desc, nullptr, a.initial11.GetAddressOf());
                    a.texture11 = a.initial11.Get();
                } else {
                    D3D12_HEAP_PROPERTIES heap{};
                    heap.Type = D3D12_HEAP_TYPE_READBACK;
                    heap.CreationNodeMask = 1;
                    heap.VisibleNodeMask = 1;
                    result = device12->CreateCommittedResource(&heap,
                                                               D3D12_HEAP_FLAG_NONE,
                                                               &a.plan.readback12,
                                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                                               nullptr,
                                                               IID_PPV_ARGS(a.initial12.GetAddressOf()));
                    a.buffer12 = a.initial12.Get();
                }
                if (FAILED(result) || (!a.texture11 && !a.buffer12)) {
                    discardUnexposed();
                    return false;
                }
                a.allocationId = a.lifetime = nextAllocation_++;
                a.allocated = true;
            }
        allocated_ = true;
        return true;
    }
    SnapshotAllocation* SnapshotStore::get(uint32_t frame, uint32_t slot) noexcept {
        return frame < 2 && slot < count_ ? &allocations_[frame][slot] : nullptr;
    }
    bool SnapshotStore::releaseInitialAfterAcquire(uint32_t frame,
                                                   uint32_t slot,
                                                   const OwnedGroupBoundary& group) noexcept {
        auto* a = get(frame, slot);
        if (!a || !a->allocated || a->adopted || static_cast<uint32_t>(group.id.frame) != frame ||
            group.id.slot != a->plan.group)
            return false;
        const auto state = hostWorkOwner().snapshot(group.id);
        if (!state.valid || state.pendingAcquisitions || !state.referenceInventory)
            return false;
        // The caller has successfully acquired the advertised snapshot ref; release
        // our initial creation reference outside the owner's mutex.
        a->initial11.Reset();
        a->initial12.Reset();
        a->ownerGroup = group.id;
        a->adopted = true;
        return true;
    }
    bool SnapshotStore::copy11(uint32_t frame,
                               uint32_t slot,
                               ID3D11DeviceContext4* context,
                               ID3D11Texture2D* source,
                               const OwnedGroupBoundary& group,
                               uint64_t sourceLifetime,
                               uint64_t sourceWrite) noexcept {
        auto* a = get(frame, slot);
        if (!a || !a->adopted || !vdxrfg::pair_capture::nr::sameGroup(a->ownerGroup, group.id) || !nextCopy_ ||
            nextCopy_ == UINT64_MAX || !nextDescription_ || nextDescription_ == UINT64_MAX || a->copied ||
            a->plan.copyMethod != 1 || !context || !source || source == a->texture11 ||
            group.actual.queue != reinterpret_cast<uint64_t>(context))
            return false;
        const auto state = hostWorkOwner().snapshot(group.id);
        if (!state.valid || !state.sealed || !state.exposed)
            return false;
        D3D11_TEXTURE2D_DESC actual{};
        source->GetDesc(&actual);
        if (!same11(actual, a->plan.texture11) || !resourceDescription11(actual,
                                                                         reinterpret_cast<uint64_t>(source),
                                                                         sourceLifetime,
                                                                         sourceWrite,
                                                                         nextDescription_,
                                                                         a->sourceDescription))
            return false;
        ++nextDescription_;
        a->copies[0] = beginCopy(source, a->texture11, context, 0, 0);
        anyCopy_ = true;
        context->CopyResource(a->texture11, source);
        endCopy(a->copies[0]);
        a->writeEpoch = nextCopy_++;
        a->copied = true;
        return true;
    }
    bool SnapshotStore::copy12(uint32_t frame,
                               uint32_t slot,
                               ID3D12GraphicsCommandList* list,
                               ID3D12Resource* source,
                               const OwnedGroupBoundary& group,
                               uint64_t sourceLifetime,
                               uint64_t sourceWrite) noexcept {
        auto* a = get(frame, slot);
        if (!a || !a->adopted || !vdxrfg::pair_capture::nr::sameGroup(a->ownerGroup, group.id) || !nextCopy_ ||
            nextCopy_ == UINT64_MAX || !nextDescription_ || nextDescription_ == UINT64_MAX || a->copied ||
            a->plan.copyMethod != 2 || !list || !source || source == a->buffer12 ||
            group.actual.list != reinterpret_cast<uint64_t>(list) || !group.actual.recordingEpoch)
            return false;
        const auto state = hostWorkOwner().snapshot(group.id);
        if (!state.valid || !state.sealed)
            return false;
        const auto actual = source->GetDesc();
        if (!same12(actual, a->plan.texture12) || !resourceDescription12(actual,
                                                                         reinterpret_cast<uint64_t>(source),
                                                                         sourceLifetime,
                                                                         sourceWrite,
                                                                         nextDescription_,
                                                                         a->sourceDescription))
            return false;
        ++nextDescription_;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        anyCopy_ = true;
        list->ResourceBarrier(1, &barrier);
        for (UINT i = 0; i < a->plan.subresources; ++i) {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = source;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = i;
            to.pResource = a->buffer12;
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = a->plan.footprints[i];
            a->copies[i] = beginCopy(source, a->buffer12, list, i, to.PlacedFootprint.Offset);
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            endCopy(a->copies[i]);
        }
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
        a->writeEpoch = nextCopy_++;
        a->copied = true;
        return true;
    }
    uint64_t SnapshotStore::initialReferences(uint32_t frame) const noexcept {
        if (frame > 1)
            return 0;
        uint64_t result = 0;
        for (const auto& a : allocations_[frame])
            result += (a.initial11 ? 1 : 0) + (a.initial12 ? 1 : 0);
        return result;
    }
    bool SnapshotStore::discardUnexposed() noexcept {
        if (anyCopy_)
            return false;
        for (const auto& frame : allocations_)
            for (const auto& a : frame)
                if (a.adopted)
                    return false;
        for (auto& frame : allocations_)
            for (auto& a : frame) {
                a.initial11.Reset();
                a.initial12.Reset();
                a.texture11 = nullptr;
                a.buffer12 = nullptr;
                a.allocated = false;
            }
        allocated_ = false;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
