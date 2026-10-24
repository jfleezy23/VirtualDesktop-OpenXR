#pragma once

#include "nvidia_synthesize_midpoint_pixel_shader.hpp"
#if defined(VDXR_FG_MRT_SHADER_AVAILABLE) || __has_include("nvidia_shared_3x_synthesize_pixel_shader.hpp")
#include "nvidia_shared_3x_synthesize_pixel_shader.hpp"
#endif
#include <cstdlib>
#include <detours.h>

// Read-only native interception. Shader bytes identify our composition PSOs;
// SDK graphics work is deliberately excluded from the draw contract.
class FgMrtOperationProbe {
    enum class Kind { other, single, dual };
    using CreatePso = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
                                                  const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                                  REFIID,
                                                  void**);
    using CreateRtv = void(STDMETHODCALLTYPE*)(ID3D12Device*,
                                               ID3D12Resource*,
                                               const D3D12_RENDER_TARGET_VIEW_DESC*,
                                               D3D12_CPU_DESCRIPTOR_HANDLE);
    using Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                              ID3D12CommandAllocator*,
                                              ID3D12PipelineState*);
    using SetPso = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    using SetTargets = void(STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    using Draw = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
    using Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    inline static FgMrtOperationProbe* active{};
    const DWORD thread = GetCurrentThreadId();
    ID3D12Device* device{};
    CreatePso createPso{};
    CreateRtv createRtv{};
    Reset reset{};
    SetPso setPso{};
    SetTargets setTargets{};
    Draw draw{};
    Barrier barrier{};
    UINT rtvStride{};
    bool armed{}, overflow{};
    bool rejectSharedPso{};
    unsigned rejectedSharedPsos{};
    struct Pso {
        ID3D12PipelineState* value{};
        Kind kind{};
        UINT targets{};
    };
    struct Rtv {
        SIZE_T handle{};
        ID3D12Resource* resource{};
    };
    struct List {
        ID3D12GraphicsCommandList* value{};
        ID3D12PipelineState* pso{};
        UINT targets{};
        std::array<ID3D12Resource*, 2> resources{};
    };
    struct DrawEvent {
        ID3D12GraphicsCommandList* list{};
        Kind kind{};
        UINT psoTargets{}, targets{}, vertices{}, instances{}, firstVertex{}, firstInstance{};
        std::array<ID3D12Resource*, 2> resources{};
        size_t order{};
    };
    struct Transition {
        ID3D12GraphicsCommandList* list{};
        D3D12_RESOURCE_BARRIER value{};
        size_t order{};
    };
    std::array<Pso, 32> psos{};
    std::array<Rtv, 32> rtvs{};
    std::array<List, 32> lists{};
    std::array<DrawEvent, 16> draws{};
    std::array<Transition, 32> transitions{};
    std::array<ID3D12Resource*, 4> synthetics{};
    size_t psoCount{}, rtvCount{}, listCount{}, drawCount{}, transitionCount{}, order{};

    bool observing() const {
        return GetCurrentThreadId() == thread;
    }
    bool synthetic(ID3D12Resource* value) const {
        for (auto resource : synthetics)
            if (value && value == resource)
                return true;
        return false;
    }
    List* state(ID3D12GraphicsCommandList* value) {
        for (size_t i = 0; i < listCount; ++i)
            if (lists[i].value == value)
                return &lists[i];
        if (listCount == lists.size()) {
            overflow = true;
            return nullptr;
        }
        auto& result = lists[listCount++];
        result.value = value;
        return &result;
    }
    ID3D12Resource* resource(SIZE_T handle) const {
        for (size_t i = 0; i < rtvCount; ++i)
            if (rtvs[i].handle == handle)
                return rtvs[i].resource;
        return nullptr;
    }
    static Kind classify(D3D12_SHADER_BYTECODE shader) {
        if (shader.BytecodeLength == sizeof(g_xrfg_nvidia_synthesize_midpoint_pixel_shader) &&
            std::memcmp(
                shader.pShaderBytecode, g_xrfg_nvidia_synthesize_midpoint_pixel_shader, shader.BytecodeLength) == 0)
            return Kind::single;
#if defined(VDXR_FG_MRT_SHADER_AVAILABLE) || __has_include("nvidia_shared_3x_synthesize_pixel_shader.hpp")
        if (shader.BytecodeLength == sizeof(g_xrfg_nvidia_shared_3x_synthesize_pixel_shader) &&
            std::memcmp(
                shader.pShaderBytecode, g_xrfg_nvidia_shared_3x_synthesize_pixel_shader, shader.BytecodeLength) == 0)
            return Kind::dual;
#endif
        return Kind::other;
    }
    static HRESULT STDMETHODCALLTYPE onCreatePso(ID3D12Device* device,
                                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC* description,
                                                 REFIID iid,
                                                 void** output) {
        auto& self = *active;
        if (self.observing() && device == self.device && self.rejectSharedPso &&
            classify(description->PS) == Kind::dual) {
            if (output)
                *output = nullptr;
            ++self.rejectedSharedPsos;
            return E_NOTIMPL;
        }
        const HRESULT hr = self.createPso(device, description, iid, output);
        if (self.observing() && device == self.device && SUCCEEDED(hr) && output && *output) {
            const auto kind = classify(description->PS);
            if (kind != Kind::other) {
                if (self.psoCount == self.psos.size())
                    self.overflow = true;
                else
                    self.psos[self.psoCount++] = {
                        static_cast<ID3D12PipelineState*>(*output), kind, description->NumRenderTargets};
            }
        }
        return hr;
    }
    static void STDMETHODCALLTYPE onCreateRtv(ID3D12Device* device,
                                              ID3D12Resource* resource,
                                              const D3D12_RENDER_TARGET_VIEW_DESC* description,
                                              D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        auto& self = *active;
        if (self.observing() && device == self.device && self.synthetic(resource)) {
            if (self.rtvCount == self.rtvs.size())
                self.overflow = true;
            else
                self.rtvs[self.rtvCount++] = {handle.ptr, resource};
        }
        self.createRtv(device, resource, description, handle);
    }
    static HRESULT STDMETHODCALLTYPE onReset(ID3D12GraphicsCommandList* list,
                                             ID3D12CommandAllocator* allocator,
                                             ID3D12PipelineState* pso) {
        auto& self = *active;
        const HRESULT hr = self.reset(list, allocator, pso);
        if (self.observing() && SUCCEEDED(hr))
            if (auto* value = self.state(list))
                *value = {list, pso};
        return hr;
    }
    static void STDMETHODCALLTYPE onSetPso(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
        auto& self = *active;
        if (self.observing())
            if (auto* value = self.state(list))
                value->pso = pso;
        self.setPso(list, pso);
    }
    static void STDMETHODCALLTYPE onSetTargets(ID3D12GraphicsCommandList* list,
                                               UINT count,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                                               BOOL contiguous,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
        auto& self = *active;
        if (self.observing()) {
            if (auto* value = self.state(list)) {
                value->targets = count;
                value->resources = {};
                for (UINT i = 0; handles && i < count && i < 2; ++i)
                    value->resources[i] =
                        self.resource(contiguous ? handles[0].ptr + i * self.rtvStride : handles[i].ptr);
            }
        }
        self.setTargets(list, count, handles, contiguous, depth);
    }
    static void STDMETHODCALLTYPE
    onDraw(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance) {
        auto& self = *active;
        if (self.observing() && self.armed) {
            if (const auto* value = self.state(list)) {
                for (size_t i = 0; i < self.psoCount; ++i) {
                    const auto& pso = self.psos[i];
                    if (pso.value != value->pso)
                        continue;
                    if (self.drawCount == self.draws.size())
                        self.overflow = true;
                    else
                        self.draws[self.drawCount++] = {list,
                                                        pso.kind,
                                                        pso.targets,
                                                        value->targets,
                                                        vertices,
                                                        instances,
                                                        firstVertex,
                                                        firstInstance,
                                                        value->resources,
                                                        ++self.order};
                    break;
                }
            }
        }
        self.draw(list, vertices, instances, firstVertex, firstInstance);
    }
    static void STDMETHODCALLTYPE onBarrier(ID3D12GraphicsCommandList* list,
                                            UINT count,
                                            const D3D12_RESOURCE_BARRIER* barriers) {
        auto& self = *active;
        if (self.observing() && self.armed)
            for (UINT i = 0; i < count; ++i)
                if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                    self.synthetic(barriers[i].Transition.pResource)) {
                    if (self.transitionCount == self.transitions.size())
                        self.overflow = true;
                    else
                        self.transitions[self.transitionCount++] = {list, barriers[i], ++self.order};
                }
        self.barrier(list, count, barriers);
    }
    void attach(bool enable) {
        auto operation = enable ? DetourAttach : DetourDetach;
        LONG hr = DetourTransactionBegin();
        const bool started = hr == NO_ERROR;
        if (!hr)
            hr = DetourUpdateThread(GetCurrentThread());
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&createPso), onCreatePso);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&createRtv), onCreateRtv);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&reset), onReset);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&setPso), onSetPso);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&setTargets), onSetTargets);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&draw), onDraw);
        if (!hr)
            hr = operation(reinterpret_cast<PVOID*>(&barrier), onBarrier);
        if (!hr)
            hr = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(hr), "MRT native observer transaction");
    }

  public:
    struct Counts {
        unsigned singleDraws{}, dualDraws{}, singleTargetDraws{}, dualTargetDraws{}, rejectedMrtPsos{};
    };
    FgMrtOperationProbe(Gpu& gpu, std::array<ID3D12Resource*, 4> watched, bool rejectMrtPso = false)
        : device(gpu.device.Get()), rejectSharedPso(rejectMrtPso), synthetics(watched) {
        check(!gpu.debugEnabled && !gpu.diagnostics, "MRT native counting requires a nondebug device");
        rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "MRT observer allocator");
        success(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "MRT observer list");
        success(list->Close(), "close MRT observer list");
        auto d = *reinterpret_cast<void***>(device);
        auto l = *reinterpret_cast<void***>(list.Get());
        createPso = reinterpret_cast<CreatePso>(d[10]);
        createRtv = reinterpret_cast<CreateRtv>(d[20]);
        reset = reinterpret_cast<Reset>(l[10]);
        draw = reinterpret_cast<Draw>(l[12]);
        setPso = reinterpret_cast<SetPso>(l[25]);
        barrier = reinterpret_cast<Barrier>(l[26]);
        setTargets = reinterpret_cast<SetTargets>(l[46]);
        check(!active, "one MRT native observer");
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }
    ~FgMrtOperationProbe() {
        armed = false;
        try {
            attach(false);
        } catch (...) {
            std::_Exit(2);
        }
        active = nullptr;
    }
    void begin() {
        drawCount = transitionCount = order = 0;
        armed = true;
    }
    Counts end(const VDXRFG_TicketV1& ticket) {
        armed = false;
        check(!overflow, "bounded MRT native trace did not overflow");
        check(psoCount != 0, "normal NVIDIA production shader bytecode was classified during Create");
        check(ticket.bank < 2 && ticket.role_mask == 3, "MRT pair owns exactly two synthetic outputs");
        Counts counts;
        counts.rejectedMrtPsos = rejectedSharedPsos;
        const std::array<ID3D12Resource*, 2> expected{synthetics[ticket.bank * 2], synthetics[ticket.bank * 2 + 1]};
        for (size_t i = 0; i < drawCount; ++i) {
            const auto& value = draws[i];
            check(value.vertices == 3 && value.instances == 1 && !value.firstVertex && !value.firstInstance,
                  "classified composition draws one fullscreen triangle");
            check(value.psoTargets == value.targets && (value.targets == 1 || value.targets == 2),
                  "native attachment count matches the classified production PSO");
            counts.singleDraws += value.kind == Kind::single;
            counts.dualDraws += value.kind == Kind::dual;
            counts.singleTargetDraws += value.targets == 1;
            counts.dualTargetDraws += value.targets == 2;
            for (UINT target = 0; target < value.targets; ++target) {
                auto* resource = value.resources[target];
                check(resource == expected[0] || resource == expected[1],
                      "composition attachment belongs to the published synthetic bank");
                if (value.targets == 2)
                    check(resource == expected[target],
                          "MRT attachment order preserves the independently timed phases");
                bool before{}, after{};
                for (size_t j = 0; j < transitionCount; ++j) {
                    const auto& transition = transitions[j];
                    if (transition.value.Transition.pResource != resource)
                        continue;
                    check(transition.list == value.list && transition.value.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                              transition.value.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          "attachment barriers are complete and on the actual composition list");
                    before |= transition.order < value.order &&
                              transition.value.Transition.StateBefore == D3D12_RESOURCE_STATE_COMMON &&
                              transition.value.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET;
                    after |= transition.order > value.order &&
                             transition.value.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET &&
                             transition.value.Transition.StateAfter == D3D12_RESOURCE_STATE_COMMON;
                }
                check(before && after, "each real composition attachment is bracketed by COMMON/RT/COMMON");
            }
        }
        check(drawCount != 0, "pair executed a classified normal NVIDIA composition draw");
        check(transitionCount == 4, "two published synthetic atlases each have exactly one state bracket");
        for (auto* resource : expected) {
            unsigned begins{}, ends{};
            for (size_t i = 0; i < transitionCount; ++i) {
                const auto& value = transitions[i].value.Transition;
                if (value.pResource != resource)
                    continue;
                begins += value.StateBefore == D3D12_RESOURCE_STATE_COMMON &&
                          value.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET;
                ends += value.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET &&
                        value.StateAfter == D3D12_RESOURCE_STATE_COMMON;
            }
            check(begins == 1 && ends == 1,
                  "each atlas stays RT through every attached eye draw and returns COMMON once");
        }
        return counts;
    }
};
