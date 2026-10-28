#pragma once

// Test-only boundary trace. Included by the production translation unit only
// with NR_SPATIAL_TEST_OBSERVER; recording uses bounded, allocation-free storage.
#include <array>
#include <cassert>
#include <d3d12.h>

namespace virtualdesktop_openxr::nr_spatial::test_observer {
    enum class Kind { Description, Descriptor, Transition, Dispatch, Draw };
    struct Event {
        Kind kind;
        ID3D12Resource* resource{};
        D3D12_RESOURCE_STATES before{}, after{};
    };
    inline std::array<Event, 128> events{};
    inline size_t count{}, barrierCalls{};
    inline bool enabled{};
    inline void Reset() {
        count = barrierCalls = 0;
        enabled = true;
    }
    inline void Record(Kind kind,
                       ID3D12Resource* resource = nullptr,
                       D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_COMMON) {
        if (!enabled)
            return;
        assert(count < events.size());
        events[count++] = {kind, resource, before, after};
    }
    inline void Barriers(UINT n, const D3D12_RESOURCE_BARRIER* barriers) {
        if (!enabled)
            return;
        ++barrierCalls;
        for (UINT i = 0; i < n; ++i) {
            assert(barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
            const auto& t = barriers[i].Transition;
            assert(t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
            Record(Kind::Transition, t.pResource, t.StateBefore, t.StateAfter);
        }
    }
} // namespace virtualdesktop_openxr::nr_spatial::test_observer
