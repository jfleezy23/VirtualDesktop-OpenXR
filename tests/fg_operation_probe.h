#pragma once

#include <detours.h>
#include <utility>

// Observe real driver entry points, only on this fixture's calling thread.
struct FgOperationCounts {
    unsigned submits{}, waits{}, copies{}, queries{}, resolves{}, maps{}, failedMaps{}, uninitializedResolves{};
    unsigned listResets{}, listCloses{}, outerBarrierCalls{}, outerTransitions{}, outerCopies{};
    unsigned resourceCopies{}, captureRegionCopies{};
    unsigned syntheticRepackCopies{}, syntheticRenderTransitions{}, renderTargetClears{};
};

class FgOperationProbe {
    using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using Wait = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    using Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                              ID3D12CommandAllocator*,
                                              ID3D12PipelineState*);
    using Close = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    using Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    using Clear = void(STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
    using Copy = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                          const D3D12_TEXTURE_COPY_LOCATION*,
                                          UINT,
                                          UINT,
                                          UINT,
                                          const D3D12_TEXTURE_COPY_LOCATION*,
                                          const D3D12_BOX*);
    using CopyResource = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
    using Query = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
    using Resolve = void(STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64);
    using Map = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
    inline static FgOperationProbe* active{};
    const DWORD thread = GetCurrentThreadId();
    ID3D12CommandQueue* queue{};
    Execute execute{};
    Wait wait{};
    Signal signal{};
    Reset reset{};
    Close close{};
    Barrier barrier{};
    Clear clear{};
    Copy copy{};
    CopyResource copyResource{};
    Query query{};
    Resolve resolve{};
    Map map{};
    bool guardQueryRanges{};
    unsigned mapFailureCountdown{};
    std::array<std::pair<ID3D12QueryHeap*, UINT>, 256> writtenQueries{};
    size_t writtenQueryCount{};
    bool armed{};
    FgOperationCounts counts{};
    struct Event {
        ID3D12GraphicsCommandList* list{};
        D3D12_RESOURCE_BARRIER transition{};
        ID3D12Resource* source{};
        ID3D12Resource* destination{};
        UINT barrierCount{};
    };
    std::array<Event, 2048> events{};
    size_t eventCount{};
    bool overflow{};
    std::array<ID3D12Resource*, 7> atlases{};
    std::array<ID3D12GraphicsCommandList*, 2> outerLists{};
    struct FenceSignal {
        ID3D12Fence* fence{};
        UINT64 value{};
        unsigned order{};
    };
    std::array<FenceSignal, 64> signals{};
    size_t signalCount{};
    std::array<ID3D12CommandList*, 64> executed{};
    size_t executedCount{};
    unsigned queueOrder{}, firstExecuteOrder{}, lastExecuteOrder{};
    ID3D12Resource* captureDestination{};
    unsigned captureSlices{};
    bool validCaptureCopies{true};

    void record(const Event& event) {
        if (eventCount == events.size()) {
            overflow = true;
        } else {
            events[eventCount++] = event;
        }
    }

    bool watches() const {
        return GetCurrentThreadId() == thread && armed;
    }
    static void STDMETHODCALLTYPE onExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
        if (active->watches() && q == active->queue) {
            ++active->counts.submits;
            active->lastExecuteOrder = ++active->queueOrder;
            if (!active->firstExecuteOrder)
                active->firstExecuteOrder = active->lastExecuteOrder;
            for (UINT i = 0; i < n; ++i) {
                if (active->executedCount == active->executed.size())
                    active->overflow = true;
                else
                    active->executed[active->executedCount++] = lists[i];
            }
        }
        active->execute(q, n, lists);
    }
    static HRESULT STDMETHODCALLTYPE onWait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 value) {
        if (active->watches() && q == active->queue) {
            ++active->counts.waits;
            ++active->queueOrder;
        }
        return active->wait(q, f, value);
    }
    static HRESULT STDMETHODCALLTYPE onSignal(ID3D12CommandQueue* q, ID3D12Fence* fence, UINT64 value) {
        if (active->watches() && q == active->queue) {
            const auto order = ++active->queueOrder;
            if (active->signalCount == active->signals.size())
                active->overflow = true;
            else
                active->signals[active->signalCount++] = {fence, value, order};
        }
        return active->signal(q, fence, value);
    }
    static HRESULT STDMETHODCALLTYPE onReset(ID3D12GraphicsCommandList* list,
                                             ID3D12CommandAllocator* allocator,
                                             ID3D12PipelineState* pipeline) {
        if (active->watches())
            ++active->counts.listResets;
        return active->reset(list, allocator, pipeline);
    }
    static HRESULT STDMETHODCALLTYPE onClose(ID3D12GraphicsCommandList* list) {
        if (active->watches())
            ++active->counts.listCloses;
        return active->close(list);
    }
    static void STDMETHODCALLTYPE onBarrier(ID3D12GraphicsCommandList* list,
                                            UINT count,
                                            const D3D12_RESOURCE_BARRIER* barriers) {
        if (active->watches()) {
            for (UINT i = 0; i < count; ++i) {
                active->record({list, barriers[i], nullptr, nullptr, i ? 0u : count});
            }
        }
        active->barrier(list, count, barriers);
    }
    static void STDMETHODCALLTYPE onClear(ID3D12GraphicsCommandList* list,
                                          D3D12_CPU_DESCRIPTOR_HANDLE target,
                                          const FLOAT color[4],
                                          UINT count,
                                          const D3D12_RECT* rects) {
        if (active->watches())
            ++active->counts.renderTargetClears;
        active->clear(list, target, color, count, rects);
    }
    static void STDMETHODCALLTYPE onCopy(ID3D12GraphicsCommandList* list,
                                         const D3D12_TEXTURE_COPY_LOCATION* dst,
                                         UINT x,
                                         UINT y,
                                         UINT z,
                                         const D3D12_TEXTURE_COPY_LOCATION* src,
                                         const D3D12_BOX* box) {
        if (active->watches()) {
            ++active->counts.copies;
            active->record({list, {}, src->pResource, dst->pResource});
            for (const size_t output : {1u, 2u, 4u, 5u})
                if (active->atlases[output] && dst->pResource == active->atlases[output])
                    ++active->counts.syntheticRepackCopies;
            if (src->pResource == active->atlases[0]) {
                const auto input = src->pResource->GetDesc();
                const auto output = dst->pResource->GetDesc();
                const UINT slice = dst->SubresourceIndex;
                ++active->counts.captureRegionCopies;
                active->validCaptureCopies &=
                    src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->SubresourceIndex == 0 &&
                    dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && slice < 2 && !x && !y && !z && box &&
                    input.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && input.DepthOrArraySize == 1 &&
                    output.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && output.DepthOrArraySize == 2 &&
                    output.Width * 2 == input.Width && output.Height == input.Height && output.Format == input.Format;
                if (slice < 2 && box) {
                    active->validCaptureCopies &= !(active->captureSlices & (1u << slice)) &&
                                                  box->left == slice * output.Width &&
                                                  box->right == (slice + 1) * output.Width && box->top == 0 &&
                                                  box->bottom == output.Height && box->front == 0 && box->back == 1;
                    active->captureSlices |= 1u << slice;
                }
                if (active->captureDestination)
                    active->validCaptureCopies &= active->captureDestination == dst->pResource;
                active->captureDestination = dst->pResource;
            }
            for (auto atlas : active->atlases) {
                if (atlas && (src->pResource == atlas || dst->pResource == atlas)) {
                    if (active->outerLists[0] != list && active->outerLists[1] != list) {
                        auto& slot = active->outerLists[0] ? active->outerLists[1] : active->outerLists[0];
                        if (slot)
                            active->overflow = true;
                        slot = list;
                    }
                }
            }
        }
        active->copy(list, dst, x, y, z, src, box);
    }
    static void STDMETHODCALLTYPE onCopyResource(ID3D12GraphicsCommandList* list,
                                                 ID3D12Resource* dst,
                                                 ID3D12Resource* src) {
        if (active->watches())
            ++active->counts.resourceCopies;
        active->copyResource(list, dst, src);
    }
    static void STDMETHODCALLTYPE onQuery(ID3D12GraphicsCommandList* list,
                                          ID3D12QueryHeap* heap,
                                          D3D12_QUERY_TYPE type,
                                          UINT index) {
        if (active->watches() && type == D3D12_QUERY_TYPE_TIMESTAMP) {
            ++active->counts.queries;
            if (active->writtenQueryCount < active->writtenQueries.size())
                active->writtenQueries[active->writtenQueryCount++] = {heap, index};
            else
                active->overflow = true;
        }
        active->query(list, heap, type, index);
    }
    static void STDMETHODCALLTYPE onResolve(ID3D12GraphicsCommandList* list,
                                            ID3D12QueryHeap* heap,
                                            D3D12_QUERY_TYPE type,
                                            UINT first,
                                            UINT n,
                                            ID3D12Resource* dst,
                                            UINT64 offset) {
        if (active->watches() && type == D3D12_QUERY_TYPE_TIMESTAMP) {
            ++active->counts.resolves;
            if (active->guardQueryRanges) {
                bool missing = !n || first > UINT_MAX - n;
                for (UINT i = 0; !missing && i < n; ++i) {
                    bool found = false;
                    for (size_t j = 0; j < active->writtenQueryCount; ++j)
                        found |= active->writtenQueries[j] == std::pair{heap, first + i};
                    missing |= !found;
                }
                if (missing) {
                    ++active->counts.uninitializedResolves;
                    // A negative control must fail without asking the driver
                    // to execute an undefined timestamp resolve.
                    return;
                }
            }
        }
        active->resolve(list, heap, type, first, n, dst, offset);
    }
    static HRESULT STDMETHODCALLTYPE onMap(ID3D12Resource* resource,
                                           UINT index,
                                           const D3D12_RANGE* range,
                                           void** data) {
        if (active->watches()) {
            ++active->counts.maps;
            if (active->mapFailureCountdown && !--active->mapFailureCountdown) {
                ++active->counts.failedMaps;
                if (data)
                    *data = nullptr;
                return E_FAIL;
            }
        }
        return active->map(resource, index, range, data);
    }
    void attach(bool enable) {
        auto operation = enable ? DetourAttach : DetourDetach;
        LONG hr = DetourTransactionBegin();
        const bool started = hr == NO_ERROR;
        if (!hr)
            hr = DetourUpdateThread(GetCurrentThread());
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&execute), onExecute);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&wait), onWait);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&signal), onSignal);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&reset), onReset);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&close), onClose);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&barrier), onBarrier);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&clear), onClear);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&copy), onCopy);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&copyResource), onCopyResource);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&query), onQuery);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&resolve), onResolve);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&map), onMap);
        if (!hr)
            hr = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(hr), "operation observer transaction");
    }

  public:
    explicit FgOperationProbe(Gpu& gpu, std::array<ID3D12Resource*, 7> watchedAtlases, bool guardQueries = false)
        : queue(gpu.queue.Get()), guardQueryRanges(guardQueries), atlases(watchedAtlases) {
        check(!gpu.debugEnabled && !gpu.diagnostics,
              "native operation counting requires a nondebug device; use the separate GBV regression for validation");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        success(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "observer allocator");
        success(gpu.device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
                "observer list");
        success(list->Close(), "observer close");
        auto q = *reinterpret_cast<void***>(queue);
        auto l = *reinterpret_cast<void***>(list.Get());
        execute = reinterpret_cast<Execute>(q[10]);
        wait = reinterpret_cast<Wait>(q[15]);
        signal = reinterpret_cast<Signal>(q[14]);
        close = reinterpret_cast<Close>(l[9]);
        reset = reinterpret_cast<Reset>(l[10]);
        barrier = reinterpret_cast<Barrier>(l[26]);
        clear = reinterpret_cast<Clear>(l[48]);
        copy = reinterpret_cast<Copy>(l[16]);
        copyResource = reinterpret_cast<CopyResource>(l[17]);
        query = reinterpret_cast<Query>(l[53]);
        resolve = reinterpret_cast<Resolve>(l[54]);
        auto sample = gpu.texture();
        map = reinterpret_cast<Map>((*reinterpret_cast<void***>(sample.Get()))[8]);
        check(!active, "one native operation observer");
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }
    ~FgOperationProbe() {
        armed = false;
        try {
            attach(false);
        } catch (...) {
            std::_Exit(2);
        }
        active = nullptr;
    }
    void begin() {
        counts = {};
        eventCount = 0;
        overflow = false;
        outerLists = {};
        signalCount = executedCount = 0;
        writtenQueryCount = 0;
        queueOrder = firstExecuteOrder = lastExecuteOrder = 0;
        captureDestination = nullptr;
        captureSlices = 0;
        validCaptureCopies = true;
        armed = true;
    }
    FgOperationCounts countersOnly() {
        armed = false;
        check(!overflow, "bounded native operation counters did not overflow");
        return counts;
    }
    void failMapAfter(unsigned number) {
        mapFailureCountdown = number;
    }
    FgOperationCounts end(const VDXRFG_TicketV1& ticket) {
        armed = false;
        if (overflow)
            std::fprintf(stderr,
                         "operation trace overflow: events=%zu queries=%zu signals=%zu executed=%zu\n",
                         eventCount,
                         writtenQueryCount,
                         signalCount,
                         executedCount);
        check(!overflow, "bounded native operation trace did not overflow");
        check(counts.captureRegionCopies == 2 && captureSlices == 3 && validCaptureCopies,
              "capture copies both atlas eyes exactly once into the matching two-slice resource");
        unsigned consumedOrder{}, readyOrder{}, consumedSignals{}, readySignals{};
        for (size_t i = 0; i < signalCount; ++i) {
            const auto& value = signals[i];
            if (value.fence == ticket.input_consumed_fence && value.value == ticket.input_consumed_value) {
                consumedOrder = value.order;
                ++consumedSignals;
            }
            if (value.fence == ticket.output_ready_fence && value.value == ticket.output_ready_value) {
                readyOrder = value.order;
                ++readySignals;
            }
        }
        check(consumedSignals == 1 && readySignals == 1 && firstExecuteOrder && firstExecuteOrder < consumedOrder &&
                  consumedOrder < readyOrder && lastExecuteOrder < readyOrder && readyOrder == queueOrder,
              "consumed follows unpack and ready is signaled after all submitted backend/pack work");
        for (const size_t output : {1u, 2u, 4u, 5u}) {
            if (!atlases[output])
                continue;
            auto current = D3D12_RESOURCE_STATE_COMMON;
            unsigned renderTransitions{};
            ID3D12CommandList* composition{};
            for (size_t i = 0; i < eventCount; ++i) {
                const auto& event = events[i];
                const auto& value = event.transition;
                if (event.source || value.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                    value.Transition.pResource != atlases[output] ||
                    (value.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET &&
                     value.Transition.StateAfter != D3D12_RESOURCE_STATE_RENDER_TARGET))
                    continue;
                check(value.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                          value.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                          value.Transition.StateBefore == current &&
                          value.Transition.StateAfter == (current == D3D12_RESOURCE_STATE_COMMON
                                                              ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                              : D3D12_RESOURCE_STATE_COMMON),
                      "direct synthetic atlas has one whole-resource COMMON/RT state pair");
                current = value.Transition.StateAfter;
                ++renderTransitions;
                if (composition)
                    check(composition == event.list, "synthetic atlas transitions bracket the same composition list");
                composition = event.list;
            }
            if (renderTransitions) {
                check(renderTransitions == 2 && current == D3D12_RESOURCE_STATE_COMMON,
                      "direct synthetic atlas is restored to COMMON exactly once");
                unsigned submissions{};
                for (size_t i = 0; i < executedCount; ++i)
                    submissions += executed[i] == composition;
                check(submissions == 1, "synthetic atlas state transitions execute once before output ready");
                counts.syntheticRenderTransitions += renderTransitions;
            }
        }
        // Outer copies are on their own lists. Replay their real transition/copy
        // order, including descriptors that precede the first observed copy.
        // Infer private render targets from copy roles, never from a barrier's
        // claimed StateBefore. Public atlases and the unpack input rest COMMON.
        const auto restingState = [&](ID3D12Resource* resource) {
            for (size_t i = 0; i < eventCount; ++i) {
                const auto& event = events[i];
                if (event.source != resource)
                    continue;
                for (size_t output = 1; output < atlases.size(); ++output)
                    if (atlases[output] && event.destination == atlases[output]) {
                        for (auto atlas : atlases)
                            check(resource != atlas, "private pack sources do not alias public atlases");
                        return D3D12_RESOURCE_STATE_RENDER_TARGET;
                    }
            }
            return D3D12_RESOURCE_STATE_COMMON;
        };
        for (auto list : outerLists) {
            if (!list)
                continue;
            unsigned submissions{};
            for (size_t i = 0; i < executedCount; ++i)
                submissions += executed[i] == list;
            check(submissions == 1, "each recorded outer copy list is submitted exactly once");
            std::array<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>, 8> states{};
            const auto state = [&](ID3D12Resource* resource) -> D3D12_RESOURCE_STATES& {
                for (auto& entry : states) {
                    if (entry.first == resource)
                        return entry.second;
                    if (!entry.first) {
                        entry.first = resource;
                        entry.second = restingState(resource);
                        return entry.second;
                    }
                }
                throw std::runtime_error("too many resources on an outer stereo copy list");
            };
            for (size_t i = 0; i < eventCount; ++i) {
                const auto& event = events[i];
                if (event.list != list)
                    continue;
                if (event.source) {
                    ++counts.outerCopies;
                    check(state(event.source) == D3D12_RESOURCE_STATE_COPY_SOURCE &&
                              state(event.destination) == D3D12_RESOURCE_STATE_COPY_DEST,
                          "each outer copy has both required transitions before it");
                } else {
                    counts.outerBarrierCalls += event.barrierCount != 0;
                    ++counts.outerTransitions;
                    const auto& value = event.transition;
                    check(value.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                              value.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                              value.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          "outer copies use complete nonsplit resource transitions");
                    auto& current = state(value.Transition.pResource);
                    check(current == value.Transition.StateBefore, "outer transitions preserve prior state");
                    current = value.Transition.StateAfter;
                }
            }
            for (const auto& entry : states)
                if (entry.first)
                    check(entry.second == restingState(entry.first),
                          "outer copies restore each public or private resource's resting state");
        }
        return counts;
    }
};
