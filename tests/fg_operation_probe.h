#pragma once

#include <detours.h>

// Observe real driver entry points, only on this fixture's calling thread.
struct FgOperationCounts {
    unsigned submits{}, waits{}, copies{}, queries{}, resolves{};
};

class FgOperationProbe {
    using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using Wait = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    using Copy = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                          const D3D12_TEXTURE_COPY_LOCATION*,
                                          UINT,
                                          UINT,
                                          UINT,
                                          const D3D12_TEXTURE_COPY_LOCATION*,
                                          const D3D12_BOX*);
    using Query = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
    using Resolve = void(STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64);
    inline static FgOperationProbe* active{};
    const DWORD thread = GetCurrentThreadId();
    ID3D12CommandQueue* queue{};
    Execute execute{};
    Wait wait{};
    Copy copy{};
    Query query{};
    Resolve resolve{};
    bool armed{};
    FgOperationCounts counts{};

    bool watches() const {
        return GetCurrentThreadId() == thread && armed;
    }
    static void STDMETHODCALLTYPE onExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
        if (active->watches() && q == active->queue)
            ++active->counts.submits;
        active->execute(q, n, lists);
    }
    static HRESULT STDMETHODCALLTYPE onWait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 value) {
        if (active->watches() && q == active->queue)
            ++active->counts.waits;
        return active->wait(q, f, value);
    }
    static void STDMETHODCALLTYPE onCopy(ID3D12GraphicsCommandList* list,
                                         const D3D12_TEXTURE_COPY_LOCATION* dst,
                                         UINT x,
                                         UINT y,
                                         UINT z,
                                         const D3D12_TEXTURE_COPY_LOCATION* src,
                                         const D3D12_BOX* box) {
        if (active->watches())
            ++active->counts.copies;
        active->copy(list, dst, x, y, z, src, box);
    }
    static void STDMETHODCALLTYPE onQuery(ID3D12GraphicsCommandList* list,
                                          ID3D12QueryHeap* heap,
                                          D3D12_QUERY_TYPE type,
                                          UINT index) {
        if (active->watches() && type == D3D12_QUERY_TYPE_TIMESTAMP)
            ++active->counts.queries;
        active->query(list, heap, type, index);
    }
    static void STDMETHODCALLTYPE onResolve(ID3D12GraphicsCommandList* list,
                                            ID3D12QueryHeap* heap,
                                            D3D12_QUERY_TYPE type,
                                            UINT first,
                                            UINT n,
                                            ID3D12Resource* dst,
                                            UINT64 offset) {
        if (active->watches() && type == D3D12_QUERY_TYPE_TIMESTAMP)
            ++active->counts.resolves;
        active->resolve(list, heap, type, first, n, dst, offset);
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
            hr = operation(reinterpret_cast<PVOID*>(&copy), onCopy);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&query), onQuery);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&resolve), onResolve);
        if (!hr)
            hr = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(hr), "operation observer transaction");
    }

  public:
    explicit FgOperationProbe(Gpu& gpu) : queue(gpu.queue.Get()) {
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
        copy = reinterpret_cast<Copy>(l[16]);
        query = reinterpret_cast<Query>(l[53]);
        resolve = reinterpret_cast<Resolve>(l[54]);
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
        armed = true;
    }
    FgOperationCounts end() {
        armed = false;
        return counts;
    }
};
