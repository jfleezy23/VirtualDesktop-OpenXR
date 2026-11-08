#include <d3d12sdklayers.h>
#include "fg_gpu_helpers.h"
#include "fg_early_ready_test_gate.h"

#include "nvidia_shared_3x_synthesize_pixel_shader.hpp"
#include <map>
class WorkProbe {
    using CreateList = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    using Name = HRESULT(STDMETHODCALLTYPE*)(ID3D12Object*, LPCWSTR);
    using AllocReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);
    using Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                              ID3D12CommandAllocator*,
                                              ID3D12PipelineState*);
    using Close = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using Pso = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
    using Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    using Draw = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
    using Dispatch = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
    using CopyTexture = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                 const D3D12_TEXTURE_COPY_LOCATION*,
                                                 UINT,
                                                 UINT,
                                                 UINT,
                                                 const D3D12_TEXTURE_COPY_LOCATION*,
                                                 const D3D12_BOX*);
    using CopyResource = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
    inline static WorkProbe* active{};
    CreateList createList{};
    Name name{};
    AllocReset allocReset{};
    Reset reset{};
    Close close{};
    Execute execute{};
    Pso pso{};
    Barrier barrier{};
    Draw draw{};
    Dispatch dispatch{};
    CopyTexture copyTexture{};
    CopyResource copyResource{};
    ID3D12Device* device{};
    const DWORD owner = GetCurrentThreadId();
    struct List {
        ID3D12CommandAllocator* allocator{};
        bool tail{};
        unsigned barriers{}, restores{}, work{};
    };
    std::map<void*, List> lists;
    bool armed{}, rejectMrt{}, enabled{};
    bool watches() const {
        return armed && GetCurrentThreadId() == owner;
    }
    static HRESULT STDMETHODCALLTYPE onCreateList(ID3D12Device* d,
                                                  UINT n,
                                                  D3D12_COMMAND_LIST_TYPE t,
                                                  ID3D12CommandAllocator* a,
                                                  ID3D12PipelineState* p,
                                                  REFIID i,
                                                  void** o) {
        auto& s = *active;
        const auto hr = s.createList(d, n, t, a, p, i, o);
        if (SUCCEEDED(hr) && d == s.device && GetCurrentThreadId() == s.owner)
            s.lists[*o] = {a, false};
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE onName(ID3D12Object* object, LPCWSTR value) {
        auto& s = *active;
        const auto it = s.lists.find(object);
        if (GetCurrentThreadId() == s.owner && it != s.lists.end())
            it->second.tail = value && std::wcscmp(value, L"OFXR Later Synthetics") == 0;
        return s.name(object, value);
    }
    static HRESULT STDMETHODCALLTYPE onAllocReset(ID3D12CommandAllocator* a) {
        auto& s = *active;
        if (s.watches()) {
            ++s.counts.allocators;
            for (const auto& [l, v] : s.lists)
                if (v.allocator == a && v.tail) {
                    ++s.counts.tailAllocators;
                    break;
                }
        }
        return s.allocReset(a);
    }
    static HRESULT STDMETHODCALLTYPE onReset(ID3D12GraphicsCommandList* l,
                                             ID3D12CommandAllocator* a,
                                             ID3D12PipelineState* p) {
        auto& s = *active;
        if (s.watches()) {
            ++s.counts.resets;
            auto& state = s.lists[l];
            state.barriers = state.restores = state.work = 0;
            if (s.lists.contains(l) && s.lists[l].tail)
                ++s.counts.tailResets;
        }
        return s.reset(l, a, p);
    }
    static HRESULT STDMETHODCALLTYPE onClose(ID3D12GraphicsCommandList* l) {
        auto& s = *active;
        if (s.watches())
            ++s.counts.closes;
        return s.close(l);
    }
    static void STDMETHODCALLTYPE onExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) {
        auto& s = *active;
        if (s.watches()) {
            ++s.counts.executes;
            s.counts.submittedLists += n;
            for (UINT i = 0; i < n; ++i) {
                const auto it = s.lists.find(l[i]);
                if (it != s.lists.end() && it->second.barriers == 1 && it->second.restores == 1 && it->second.work == 0)
                    ++s.counts.restoreOnlyLists;
            }
        }
        s.execute(q, n, l);
    }
    static HRESULT STDMETHODCALLTYPE onPso(ID3D12Device* d,
                                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                           REFIID i,
                                           void** o) {
        auto& s = *active;
        if (s.rejectMrt && d == s.device && GetCurrentThreadId() == s.owner &&
            desc->PS.BytecodeLength == sizeof(g_xrfg_nvidia_shared_3x_synthesize_pixel_shader) &&
            std::memcmp(desc->PS.pShaderBytecode,
                        g_xrfg_nvidia_shared_3x_synthesize_pixel_shader,
                        desc->PS.BytecodeLength) == 0) {
            ++s.rejected;
            *o = nullptr;
            return E_FAIL;
        }
        return s.pso(d, desc, i, o);
    }
    static void STDMETHODCALLTYPE onBarrier(ID3D12GraphicsCommandList* list,
                                            UINT n,
                                            const D3D12_RESOURCE_BARRIER* values) {
        auto& s = *active;
        if (s.watches()) {
            auto& state = s.lists[list];
            state.barriers += n;
            for (UINT i = 0; i < n; ++i) {
                const auto& b = values[i];
                if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                    b.Transition.StateBefore !=
                        (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
                    b.Transition.StateAfter != D3D12_RESOURCE_STATE_COMMON)
                    continue;
                const auto d = b.Transition.pResource->GetDesc();
                if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == eyeWidth && d.Height == height &&
                    d.DepthOrArraySize == 2)
                    ++state.restores;
            }
        }
        s.barrier(list, n, values);
    }
    static void STDMETHODCALLTYPE onDraw(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, UINT d) {
        auto& s = *active;
        if (s.watches()) {
            ++s.counts.draws;
            ++s.lists[l].work;
        }
        s.draw(l, a, b, c, d);
    }
    static void STDMETHODCALLTYPE onDispatch(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c) {
        auto& s = *active;
        if (s.watches()) {
            ++s.counts.dispatches;
            ++s.lists[l].work;
        }
        s.dispatch(l, a, b, c);
    }
    static void STDMETHODCALLTYPE onCopyTexture(ID3D12GraphicsCommandList* l,
                                                const D3D12_TEXTURE_COPY_LOCATION* d,
                                                UINT x,
                                                UINT y,
                                                UINT z,
                                                const D3D12_TEXTURE_COPY_LOCATION* src,
                                                const D3D12_BOX* box) {
        auto& s = *active;
        if (s.watches()) {
            ++s.counts.textureCopies;
            ++s.lists[l].work;
        }
        s.copyTexture(l, d, x, y, z, src, box);
    }
    static void STDMETHODCALLTYPE onCopyResource(ID3D12GraphicsCommandList* l, ID3D12Resource* d, ID3D12Resource* src) {
        auto& s = *active;
        if (s.watches()) {
            ++s.counts.resourceCopies;
            ++s.lists[l].work;
        }
        s.copyResource(l, d, src);
    }
    void attach(bool enable) {
        auto op = enable ? DetourAttach : DetourDetach;
        LONG e = DetourTransactionBegin();
        const bool started = !e;
        if (!e)
            e = DetourUpdateThread(GetCurrentThread());
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&createList), onCreateList);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&name), onName);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&allocReset), onAllocReset);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&reset), onReset);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&close), onClose);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&execute), onExecute);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&pso), onPso);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&barrier), onBarrier);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&draw), onDraw);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&dispatch), onDispatch);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&copyTexture), onCopyTexture);
        if (!e)
            e = op(reinterpret_cast<PVOID*>(&copyResource), onCopyResource);
        if (!e)
            e = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(e), "work observer");
    }

  public:
    struct Counts {
        unsigned allocators{}, resets{}, closes{}, executes{}, submittedLists{}, tailAllocators{}, tailResets{},
            restoreOnlyLists{}, draws{}, dispatches{}, textureCopies{}, resourceCopies{};
    } counts;
    unsigned rejected{};
    WorkProbe(Gpu& gpu, bool failMrt, bool observe) : device(gpu.device.Get()), rejectMrt(failMrt), enabled(observe) {
        if (!enabled)
            return;
        ComPtr<ID3D12CommandAllocator> a;
        ComPtr<ID3D12GraphicsCommandList> l;
        success(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "probe allocator");
        success(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l)),
                "probe list");
        success(l->Close(), "probe close");
        auto d = *reinterpret_cast<void***>(device), v = *reinterpret_cast<void***>(l.Get());
        createList = reinterpret_cast<CreateList>(d[12]);
        pso = reinterpret_cast<Pso>(d[10]);
        name = reinterpret_cast<Name>(v[6]);
        reset = reinterpret_cast<Reset>(v[10]);
        close = reinterpret_cast<Close>(v[9]);
        barrier = reinterpret_cast<Barrier>(v[26]);
        draw = reinterpret_cast<Draw>(v[12]);
        dispatch = reinterpret_cast<Dispatch>(v[14]);
        copyTexture = reinterpret_cast<CopyTexture>(v[16]);
        copyResource = reinterpret_cast<CopyResource>(v[17]);
        allocReset = reinterpret_cast<AllocReset>((*reinterpret_cast<void***>(a.Get()))[8]);
        execute = reinterpret_cast<Execute>((*reinterpret_cast<void***>(gpu.queue.Get()))[10]);
        check(!active, "one work observer");
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }
    ~WorkProbe() {
        if (!enabled)
            return;
        armed = false;
        try {
            attach(false);
        } catch (...) {
            std::terminate();
        }
        active = nullptr;
    }
    void begin() {
        counts = {};
        armed = enabled;
    }
    Counts end() {
        armed = false;
        return counts;
    }
};

struct Run {
    WorkProbe::Counts prime, pair;
    std::vector<std::vector<uint8_t>> pixels;
};
Run run(const char* path, unsigned multiplier, bool fidelityFx, bool timing, bool failMrt, bool checkLean) {
    Run result;
    HMODULE module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(module != nullptr, "load synthesis module");
    const auto get = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
    check(get != nullptr, "module must negotiate V4 role readiness");
    VDXRFG_ApiV4 api{};
    api.struct_size = sizeof(api);
    api.api_version = VDXRFG_API_VERSION_4;
    success(get(&api), "negotiate role readiness");
    check(api.GetOutputReadiness && api.max_multiplier >= 4, "complete V4 table");
    const bool validation = std::getenv("VDXR_FG_WORK_GBV") != nullptr;
    Gpu gpu(validation);
    EarlyOutputGate gate(gpu.device.Get());
    WorkProbe probe(gpu, failMrt, !validation);
    auto input = gpu.texture();
    std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> outputs;
    VDXRFG_CreateInfoV3 create{};
    create.struct_size = sizeof(create);
    create.api_version = VDXRFG_API_VERSION_3;
    create.device = gpu.device.Get();
    create.queue = gpu.queue.Get();
    create.input = input.Get();
    create.width = width;
    create.height = height;
    create.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    create.multiplier = multiplier;
    create.flags = VDXRFG_CREATE_HOST_OWNS_REAL | (fidelityFx ? VDXRFG_CREATE_FIDELITYFX_FULL : 0) |
                   (timing ? VDXRFG_CREATE_GPU_TIMING : 0);
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned role = 0; role < 4; ++role) {
            if ((role == 1 && multiplier == 2) || (role == 3 && multiplier != 4))
                continue;
            outputs[bank][role] = gpu.texture();
            create.outputs[bank][role] = outputs[bank][role].Get();
        }
    void* context{};
    success(api.Create(&create, &context), "create V4 synthesis");
    VDXRFG_SubmitInfoV3 info{};
    info.struct_size = sizeof(info);
    info.api_version = VDXRFG_API_VERSION_3;
    info.input = input.Get();
    info.serial = 1;
    info.display_time_ns = 1000000000;
    info.display_period_ns = 8333333;
    for (unsigned eye = 0; eye < 2; ++eye) {
        info.views[eye].orientation[3] = 1;
        info.views[eye].position[0] = eye ? .032f : -.032f;
        info.views[eye].fov[0] = info.views[eye].fov[3] = -.8f;
        info.views[eye].fov[1] = info.views[eye].fov[2] = .8f;
        info.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
    }
    const auto a = scene(0), b = scene(24);
    gpu.transfer(input.Get(), &a);
    VDXRFG_TicketV1 prime{};
    probe.begin();
    success(api.Submit(context, &info, &prime), "prime V4 history");
    const auto primeCounts = probe.end();
    gpu.wait(static_cast<ID3D12Fence*>(prime.output_ready_fence), prime.output_ready_value);
    const auto query = [&](uint64_t token, unsigned role, VDXRFG_OutputReadinessV4& ready) {
        ready = {sizeof(ready), VDXRFG_API_VERSION_4, nullptr, 0};
        return api.GetOutputReadiness(context, token, role, &ready);
    };
    VDXRFG_OutputReadinessV4 ready{};
    check(query(prime.token, 0, ready) == VDXRFG_E_REJECTED && !ready.fence && !ready.value,
          "prime cannot expose synthetic readiness");
    success(api.Retire(context, prime.token), "retire prime");
    gpu.transfer(input.Get(), &b);
    info.serial = 2;
    info.display_time_ns += multiplier * info.display_period_ns;
    for (unsigned i = 0; i < multiplier - 1; ++i) {
        info.target_time_ns[i] = 1000000000 + (i + 1) * info.display_period_ns;
        info.fractions[i] = static_cast<float>(i + 1) / multiplier;
    }

    gate.arm(prime);
    probe.begin();
    VDXRFG_TicketV1 pair{};
    success(api.Submit(context, &info, &pair), "submit observed V4 pair");
    const auto pairCounts = probe.end();
    const bool split = multiplier == 4 || (multiplier == 3 && (fidelityFx || failMrt));
    check(bool(gate.first) == split, "actual separate tail matches fallback/backend composition");
    success(query(pair.token, 0, ready), "query S1");
    if (split) {
        gpu.wait(gate.first.Get(), gate.firstValue);
        check(ready.fence == gate.first.Get() && ready.value == gate.firstValue, "S1 owns actual early fence");
        VDXRFG_OutputReadinessV4 later{};
        success(query(pair.token, 1, later), "query S2");
        check(later.fence == pair.output_ready_fence && later.value == pair.output_ready_value,
              "S2 retains whole completion");
        if (multiplier == 4) {
            success(query(pair.token, 3, later), "query S3");
            check(later.fence == pair.output_ready_fence && later.value == pair.output_ready_value,
                  "S3 retains whole completion");
        }
        check(static_cast<ID3D12Fence*>(pair.output_ready_fence)->GetCompletedValue() < pair.output_ready_value,
              "tail remains GPU-gated after S1");
        check(api.Retire(context, pair.token) == VDXRFG_E_BUSY && api.Reset(context, 0) == VDXRFG_E_BUSY &&
                  api.Destroy(context) == VDXRFG_E_BUSY,
              "early S1 never retires/reset/destroys full ownership");
    }
    gate.release();
    gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
    for (unsigned i = 0; i < multiplier - 1; ++i) {
        const unsigned role = i == 2 ? 3 : i;
        result.pixels.push_back(gpu.transfer(outputs[pair.bank][role].Get(), nullptr));
    }
    success(api.Retire(context, pair.token), "retire full pair");
    success(api.Destroy(context), "destroy completed context");
    FreeLibrary(module);
    std::printf(
        "work backend=%s multiplier=%u timing=%u fallback=%u prime_tail_allocator=%u prime_tail_list=%u "
        "pair_tail_allocator=%u pair_tail_list=%u pair_allocators=%u resets=%u closes=%u executes=%u lists=%u\n",
        fidelityFx ? "fidelityfx-full" : "nvidia",
        multiplier,
        timing,
        failMrt,
        primeCounts.tailAllocators,
        primeCounts.tailResets,
        pairCounts.tailAllocators,
        pairCounts.tailResets,
        pairCounts.allocators,
        pairCounts.resets,
        pairCounts.closes,
        pairCounts.executes,
        pairCounts.submittedLists);
    std::printf("work restore_only_lists=%u draws=%u dispatches=%u texture_copies=%u resource_copies=%u\n",
                pairCounts.restoreOnlyLists,
                pairCounts.draws,
                pairCounts.dispatches,
                pairCounts.textureCopies,
                pairCounts.resourceCopies);
    gpu.noErrors();
    if (validation) {
        check(gpu.debugEnabled && gpu.diagnostics, "GPU validation has an active diagnostic queue");
        for (UINT64 i = 0; i < gpu.diagnostics->GetNumStoredMessages(); ++i) {
            SIZE_T size{};
            success(gpu.diagnostics->GetMessage(i, nullptr, &size), "validation message size");
            std::vector<uint8_t> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            success(gpu.diagnostics->GetMessage(i, message, &size), "validation message");
            check(message->ID != D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_UNSUPPORTED,
                  "GPU-based validation must be supported by the driver");
        }
    }
    if (!validation) {
        const unsigned expectedDraws = !fidelityFx && multiplier == 3 && !failMrt ? 2 : 2 * (multiplier - 1);
#ifdef VDXR_FG_PATCH_MOTION_EXPERIMENT
        // One proposal dispatch per eye supplements the two existing pack dispatches.
        constexpr unsigned nvidiaDispatches = 4;
#else
        constexpr unsigned nvidiaDispatches = 2;
#endif
        check(pairCounts.draws == expectedDraws && pairCounts.dispatches == (fidelityFx ? 25u : nvidiaDispatches) &&
                  pairCounts.textureCopies == 2 && pairCounts.resourceCopies == 0,
              "all V4 synthetic phases retain composition and one stereo-history capture budgets");
        check(!failMrt || probe.rejected == 1, "fallback actually rejects exactly the optional MRT PSO");
        check(primeCounts.tailAllocators == 0 && primeCounts.tailResets == 0,
              "prime must not reset an unused tail allocator");
        check(pairCounts.tailAllocators == unsigned(split) && pairCounts.tailResets == unsigned(split),
              "reset tail allocator only for a recorded tail");
        if (checkLean)
            check(pairCounts.restoreOnlyLists == 0,
                  "ordinary host-owned pair must not execute a barrier-only current restoration list");
    }
    result.prime = primeCounts;
    result.pair = pairCounts;
    return result;
}
int main(int argc, char** argv) {
    try {
        check(argc == 5 || argc == 6, "module multiplier backend timing [mrt-failure]");
        const auto multiplier = static_cast<unsigned>(std::stoul(argv[2]));
        check(multiplier >= 2 && multiplier <= 4, "supported multiplier");
        const std::string backend(argv[3]);
        check(backend == "nvidia" || backend == "fidelityfx-full", "supported backend");
        const bool failMrt = argc == 6;
        check(!failMrt || (std::string(argv[5]) == "mrt-failure" && multiplier == 3 && backend == "nvidia"),
              "fallback mode");
        const std::string timing(argv[4]);
        check(timing == "off" || timing == "on", "supported timing mode");
        ComPtr<ID3D12Debug1> debug;
        if (std::getenv("VDXR_FG_WORK_GBV")) {
            check(!failMrt, "MRT failure injection is a separate native-driver control");
            success(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "required GPU validation");
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
            debug->SetEnableSynchronizedCommandQueueValidation(TRUE);
            std::puts("GPU-based and synchronized queue validation enabled; native work counters disabled");
        }
        const auto result = run(argv[1], multiplier, backend == "fidelityfx-full", timing == "on", failMrt, true);
        if (const char* baseline = std::getenv("VDXR_FG_WORK_BASELINE")) {
            const auto reference =
                run(baseline, multiplier, backend == "fidelityfx-full", timing == "on", failMrt, false);
            check(result.pixels == reference.pixels,
                  "all direct synthetic pixels match frozen pre-copy-list module bit for bit");
            const auto& a = result.pair;
            const auto& b = reference.pair;
            check(a.allocators + 1 == b.allocators && a.resets + 1 == b.resets && a.closes + 1 == b.closes &&
                      a.executes + 1 == b.executes && a.submittedLists + 1 == b.submittedLists,
                  "remove exactly one allocator reset/list reset/close/execute/submitted list");
            check(a.draws == b.draws && a.dispatches == b.dispatches && a.textureCopies == b.textureCopies &&
                      a.resourceCopies == b.resourceCopies,
                  "composition draws, compute and capture/copy work unchanged");
            check(std::memcmp(&result.prime, &reference.prime, sizeof(result.prime)) == 0, "prime work unchanged");
            std::puts("PASS: exact direct pixels and one-list work reduction against frozen baseline");
        }
        std::puts("PASS: native tail work and V4 full ownership");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
