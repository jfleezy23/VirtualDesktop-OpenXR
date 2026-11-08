#include "fg_gpu_helpers.h"
#include "nvidia_shared_3x_synthesize_pixel_shader.hpp"

#include <detours.h>
#include <cstdlib>

// Observe real driver calls only during module creation. The RT initial state
// distinguishes private current images from otherwise identical COMMON history.
class ResourceProbe {
    using Allocator = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**);
    using List = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    using Resource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
                                                 const D3D12_HEAP_PROPERTIES*,
                                                 D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC*,
                                                 D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*,
                                                 REFIID,
                                                 void**);
    using Pso = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
    using Rtv = void(STDMETHODCALLTYPE*)(ID3D12Device*,
                                         ID3D12Resource*,
                                         const D3D12_RENDER_TARGET_VIEW_DESC*,
                                         D3D12_CPU_DESCRIPTOR_HANDLE);
    inline static ResourceProbe* active{};
    ID3D12Device* device{};
    const DWORD thread = GetCurrentThreadId();
    Allocator allocator{};
    List list{};
    Resource resource{};
    Pso pso{};
    Rtv rtv{};
    std::array<ID3D12Resource*, 2> currents{};
    bool watches(ID3D12Device* value) const {
        return armed && value == device && GetCurrentThreadId() == thread;
    }
    static HRESULT STDMETHODCALLTYPE onAllocator(ID3D12Device* d, D3D12_COMMAND_LIST_TYPE t, REFIID i, void** out) {
        auto& s = *active;
        if (s.watches(d))
            ++s.counts.allocators;
        return s.allocator(d, t, i, out);
    }
    static HRESULT STDMETHODCALLTYPE onList(ID3D12Device* d,
                                            UINT n,
                                            D3D12_COMMAND_LIST_TYPE t,
                                            ID3D12CommandAllocator* a,
                                            ID3D12PipelineState* p,
                                            REFIID i,
                                            void** out) {
        auto& s = *active;
        if (s.watches(d))
            ++s.counts.lists;
        return s.list(d, n, t, a, p, i, out);
    }
    static HRESULT STDMETHODCALLTYPE onResource(ID3D12Device* d,
                                                const D3D12_HEAP_PROPERTIES* h,
                                                D3D12_HEAP_FLAGS f,
                                                const D3D12_RESOURCE_DESC* desc,
                                                D3D12_RESOURCE_STATES state,
                                                const D3D12_CLEAR_VALUE* clear,
                                                REFIID i,
                                                void** out) {
        auto& s = *active;
        const bool current = s.watches(d) && desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                             desc->Width == eyeWidth && desc->Height == height && desc->DepthOrArraySize == 2 &&
                             desc->Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                             state == D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (current && s.failCurrent) {
            ++s.counts.rejectedCurrents;
            *out = nullptr;
            return E_OUTOFMEMORY;
        }
        const auto hr = s.resource(d, h, f, desc, state, clear, i, out);
        if (current && SUCCEEDED(hr)) {
            if (s.counts.currents < s.currents.size())
                s.currents[s.counts.currents] = static_cast<ID3D12Resource*>(*out);
            ++s.counts.currents;
            s.counts.currentBytes += d->GetResourceAllocationInfo(0, 1, desc).SizeInBytes;
        }
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE onPso(ID3D12Device* d,
                                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                           REFIID i,
                                           void** out) {
        auto& s = *active;
        if (s.watches(d) && desc &&
            desc->PS.BytecodeLength == sizeof(g_xrfg_nvidia_shared_3x_synthesize_pixel_shader) &&
            std::memcmp(desc->PS.pShaderBytecode,
                        g_xrfg_nvidia_shared_3x_synthesize_pixel_shader,
                        desc->PS.BytecodeLength) == 0) {
            ++s.counts.mrtPsos;
            if (s.failMrt) {
                *out = nullptr;
                return E_FAIL;
            }
        }
        return s.pso(d, desc, i, out);
    }
    static void STDMETHODCALLTYPE onRtv(ID3D12Device* d,
                                        ID3D12Resource* r,
                                        const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                        D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        auto& s = *active;
        if (s.watches(d))
            for (auto current : s.currents)
                if (r && r == current)
                    ++s.counts.currentRtvs;
        s.rtv(d, r, desc, handle);
    }
    void attach(bool enabled) {
        auto operation = enabled ? DetourAttach : DetourDetach;
        LONG status = DetourTransactionBegin();
        const bool started = status == NO_ERROR;
        if (!status)
            status = DetourUpdateThread(GetCurrentThread());
        if (!status)
            status = operation(reinterpret_cast<PVOID*>(&allocator), onAllocator);
        if (!status)
            status = operation(reinterpret_cast<PVOID*>(&list), onList);
        if (!status)
            status = operation(reinterpret_cast<PVOID*>(&resource), onResource);
        if (!status)
            status = operation(reinterpret_cast<PVOID*>(&pso), onPso);
        if (!status)
            status = operation(reinterpret_cast<PVOID*>(&rtv), onRtv);
        if (!status)
            status = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(status), "resource observer transaction");
    }

  public:
    struct Counts {
        unsigned allocators{}, lists{}, currents{}, currentRtvs{}, mrtPsos{}, rejectedCurrents{};
        uint64_t currentBytes{};
    } counts;
    bool armed{}, failCurrent{}, failMrt{};
    explicit ResourceProbe(Gpu& gpu) : device(gpu.device.Get()) {
        check(!gpu.debugEnabled && !gpu.diagnostics, "resource observer requires native driver device");
        auto table = *reinterpret_cast<void***>(device);
        allocator = reinterpret_cast<Allocator>(table[9]);
        pso = reinterpret_cast<Pso>(table[10]);
        list = reinterpret_cast<List>(table[12]);
        rtv = reinterpret_cast<Rtv>(table[20]);
        resource = reinterpret_cast<Resource>(table[27]);
        check(!active, "one native resource observer");
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }
    ~ResourceProbe() {
        armed = false;
        try {
            attach(false);
        } catch (...) {
            std::_Exit(2);
        }
        active = nullptr;
    }
};

struct Run {
    ResourceProbe::Counts counts;
    std::vector<std::vector<uint8_t>> synthetic;
};

Run exercise(const char* path,
             unsigned version,
             unsigned multiplier,
             bool hostOwned,
             bool fidelityFx,
             bool failCurrent,
             bool failMrt) {
    const auto module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(module != nullptr, "load resource fixture module");
    Gpu gpu(false);
    VDXRFG_ApiV1 api1{};
    VDXRFG_ApiV2 api2{};
    VDXRFG_ApiV3 api3{};
    VDXRFG_ApiV4 api4{};
    api1.struct_size = sizeof(api1);
    api1.api_version = 1;
    api2.struct_size = sizeof(api2);
    api2.api_version = 2;
    api3.struct_size = sizeof(api3);
    api3.api_version = 3;
    api4.struct_size = sizeof(api4);
    api4.api_version = 4;
    const auto get1 = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
    const auto get2 = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
    const auto get3 = reinterpret_cast<VDXRFG_GetApiV3Fn>(GetProcAddress(module, "VDXRFG_GetApiV3"));
    const auto get4 = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
    check(get1 && get2 && get3 && get4, "resource fixture API exports");
    success(get1(&api1), "V1 API");
    success(get2(&api2), "V2 API");
    success(get3(&api3), "V3 API");
    success(get4(&api4), "V4 API");
    auto input = gpu.texture();
    std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> outputs;
    VDXRFG_CreateInfoV3 create{};
    create.struct_size = sizeof(create);
    create.api_version = 3;
    create.device = gpu.device.Get();
    create.queue = gpu.queue.Get();
    create.input = input.Get();
    create.width = width;
    create.height = height;
    create.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    create.multiplier = multiplier;
    create.flags = (hostOwned ? VDXRFG_CREATE_HOST_OWNS_REAL : 0u) | (fidelityFx ? VDXRFG_CREATE_FIDELITYFX_FULL : 0u);
    const std::vector<uint8_t> sentinel(width * height * 4, 0x5a);
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned role = 0; role < 4; ++role) {
            if ((role == 1 && multiplier == 2) || (role == 3 && multiplier != 4))
                continue;
            outputs[bank][role] = gpu.texture();
            create.outputs[bank][role] = outputs[bank][role].Get();
            gpu.transfer(outputs[bank][role].Get(), &sentinel);
        }
    VDXRFG_CreateInfoV2 legacy{};
    legacy.struct_size = sizeof(legacy);
    legacy.api_version = 2;
    legacy.flags = create.flags;
    auto& base = legacy.base;
    base.struct_size = sizeof(base);
    base.api_version = 1;
    base.device = create.device;
    base.queue = create.queue;
    base.input = create.input;
    base.width = width;
    base.height = height;
    base.format = create.format;
    base.multiplier = multiplier;
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned role = 0; role < 3; ++role)
            base.outputs[bank][role] = create.outputs[bank][role];
    void* context{};
    Run result;
    {
        ResourceProbe probe(gpu);
        probe.failCurrent = failCurrent;
        probe.failMrt = failMrt;
        probe.armed = true;
        const auto hr = version == 1   ? api1.Create(&base, &context)
                        : version == 2 ? api2.Create(&legacy, &context)
                        : version == 3 ? api3.Create(&create, &context)
                                       : api4.Create(&create, &context);
        probe.armed = false;
        result.counts = probe.counts;
        if (failCurrent) {
            check(hr == E_OUTOFMEMORY && !context && probe.counts.rejectedCurrents == 1,
                  "failed private-current allocation returns original error and no context");
            check(FreeLibrary(module) != 0, "unload rejected context module");
            return result;
        }
        success(hr, "create resource fixture context");
    }
    VDXRFG_SubmitInfoV3 info{};
    info.struct_size = sizeof(info);
    info.api_version = 3;
    info.input = input.Get();
    info.display_period_ns = 10000000;
    for (UINT eye = 0; eye < 2; ++eye) {
        auto& view = info.views[eye];
        view.orientation[3] = 1;
        view.position[0] = eye ? .032f : -.032f;
        view.fov[0] = view.fov[3] = -.8f;
        view.fov[1] = view.fov[2] = .8f;
        view.rect = {eye * eyeWidth, 0, eyeWidth, height};
    }
    auto submit = [&](UINT shift, bool prime) {
        const auto pixels = scene(shift);
        gpu.transfer(input.Get(), &pixels);
        const auto previousTime = info.display_time_ns;
        ++info.serial;
        info.display_time_ns =
            prime ? 1000000000 + info.serial * 1000000000 : previousTime + multiplier * info.display_period_ns;
        for (unsigned phase = 0; phase < multiplier - 1; ++phase) {
            info.fractions[phase] = static_cast<float>(phase + 1) / multiplier;
            info.target_time_ns[phase] = previousTime + (phase + 1) * info.display_period_ns;
        }
        VDXRFG_TicketV1 ticket{};
        HRESULT hr{};
        if (version >= 3)
            hr = version == 4 ? api4.Submit(context, &info, &ticket) : api3.Submit(context, &info, &ticket);
        else {
            VDXRFG_SubmitInfoV1 old{};
            old.struct_size = sizeof(old);
            old.api_version = 1;
            old.input = info.input;
            old.serial = info.serial;
            old.display_time_ns = info.display_time_ns;
            old.display_period_ns = info.display_period_ns;
            for (unsigned phase = 0; phase < 2; ++phase) {
                old.fractions[phase] = info.fractions[phase];
                old.views[phase] = info.views[phase];
            }
            for (unsigned phase = 0; phase < 3; ++phase)
                old.target_time_ns[phase] = info.target_time_ns[phase];
            hr = version == 1 ? api1.Submit(context, &old, &ticket) : api2.Submit(context, &old, &ticket);
        }
        success(hr, "submit resource lifetime fixture");
        gpu.wait(static_cast<ID3D12Fence*>(ticket.output_ready_fence), ticket.output_ready_value);
        check(ticket.token && ticket.bank < 2 && ticket.prime == static_cast<unsigned>(prime) &&
                  ticket.role_mask == ((hostOwned ? 0u : 4u) | (prime             ? 0u
                                                                : multiplier == 4 ? 11u
                                                                : multiplier == 3 ? 3u
                                                                                  : 1u)),
              "ticket metadata preserves synthetic roles and real ownership");
        check(gpu.transfer(outputs[ticket.bank][2].Get(), nullptr) == (hostOwned ? sentinel : pixels),
              "real B is exact for module ownership and remains untouched for host ownership");
        if (!prime)
            for (unsigned role : {0u, 1u, 3u})
                if (ticket.role_mask & (1u << role))
                    result.synthetic.push_back(gpu.transfer(outputs[ticket.bank][role].Get(), nullptr));
        return ticket;
    };
    auto retire = [&](const VDXRFG_TicketV1& ticket) {
        success(api1.Retire(context, ticket.token), "retire completed resource ticket");
    };
    const auto prime = submit(0, true);
    const auto pair = submit(24, false);
    check(prime.bank == 0 && pair.bank == 1, "prime remains leased while pair uses independent synthetic bank");
    std::array<std::vector<uint8_t>, 4> held;
    for (unsigned role : {0u, 1u, 3u})
        if (pair.role_mask & (1u << role))
            held[role] = gpu.transfer(outputs[pair.bank][role].Get(), nullptr);
    check(api1.Reset(context, 0) == VDXRFG_E_BUSY, "reset cannot discard either leased bank");
    retire(prime);
    const auto recycled = submit(48, false);
    check(recycled.bank == 0, "released prime bank recycles without changing synthetic indexing");
    for (unsigned role : {0u, 1u, 3u})
        if (pair.role_mask & (1u << role))
            check(gpu.transfer(outputs[pair.bank][role].Get(), nullptr) == held[role],
                  "leased synthetic bytes survive other bank reuse");
    retire(pair);
    const auto other = submit(72, false);
    check(other.bank == 1, "second bank recycles independently");
    retire(recycled);
    retire(other);
    success(api1.Reset(context, 0), "reset settled context");
    const auto reprime = submit(0, true);
    const auto repair = submit(24, false);
    check(reprime.generation > prime.generation && reprime.bank == 0 && repair.bank == 1,
          "reset retains priming and bank ownership");
    retire(reprime);
    retire(repair);
    success(api1.Destroy(context), "destroy settled resource context");
    check(FreeLibrary(module) != 0, "unload resource fixture module");
    return result;
}

int main(int argc, char** argv) {
    try {
        check(argc == 6 || argc == 7,
              "usage: fg_resource_regression MODULE nvidia|fidelityfx-full 2|3|4 1|2|3|4 "
              "lean|current-failure|mrt-failure [REFERENCE_MODULE]");
        const unsigned multiplier = static_cast<unsigned>(std::stoul(argv[3]));
        const unsigned version = static_cast<unsigned>(std::stoul(argv[4]));
        const bool fidelityFx = std::string(argv[2]) == "fidelityfx-full";
        check(fidelityFx || std::string(argv[2]) == "nvidia", "known resource backend");
        check(multiplier >= 2 && multiplier <= 4 && version >= 1 && version <= 4 && (version >= 3 || multiplier < 4) &&
                  (version != 1 || !fidelityFx),
              "supported resource API/multiplier/backend");
        const std::string mode = argv[5];
        check(mode == "lean" || mode == "current-failure" || mode == "mrt-failure", "known resource test mode");
        const bool failMrt = mode == "mrt-failure";
        if (mode == "current-failure")
            exercise(argv[1], version, multiplier, version != 1, fidelityFx, true, false);
        const auto owned = exercise(argv[1], version, multiplier, false, fidelityFx, false, failMrt);
        if (argc == 7) {
            const auto reference = exercise(argv[6], version, multiplier, false, fidelityFx, false, failMrt);
            check(owned.synthetic == reference.synthetic, "module-owned synthetic bytes match frozen reference module");
        }
        const auto& counts = owned.counts;
        check(counts.currents == 2 && counts.currentRtvs == 4,
              "module-owned real retains two private current images and four RTVs");
        const unsigned expectedMrt = !fidelityFx && multiplier == 3 ? 1u : 0u;
        std::printf(
            "owned V%u %ux %s: allocators=%u lists=%u currents=%u current_RTVs=%u current_bytes=%llu MRT_PSOs=%u\n",
            version,
            multiplier,
            argv[2],
            counts.allocators,
            counts.lists,
            counts.currents,
            counts.currentRtvs,
            static_cast<unsigned long long>(counts.currentBytes),
            counts.mrtPsos);
        bool budgets = counts.mrtPsos == expectedMrt;
        if (version != 1) {
            const auto host = exercise(argv[1], version, multiplier, true, fidelityFx, false, failMrt);
            if (argc == 7) {
                const auto reference = exercise(argv[6], version, multiplier, true, fidelityFx, false, failMrt);
                check(host.synthetic == reference.synthetic,
                      "host-owned synthetic bytes match frozen reference module");
            }
            const auto& h = host.counts;
            std::printf(
                "host V%u %ux %s: allocators=%u lists=%u currents=%u current_RTVs=%u current_bytes=%llu MRT_PSOs=%u\n",
                version,
                multiplier,
                argv[2],
                h.allocators,
                h.lists,
                h.currents,
                h.currentRtvs,
                static_cast<unsigned long long>(h.currentBytes),
                h.mrtPsos);
            check(host.synthetic == owned.synthetic,
                  "every synthetic byte matches module-owned control through both banks and reset");
            budgets &= h.allocators + 2 == counts.allocators && h.lists + 2 == counts.lists && h.currents == 1 &&
                       h.currentRtvs == 2 && h.currentBytes * 2 == counts.currentBytes && h.mrtPsos == expectedMrt;
        }
        check(budgets,
              "native resource budgets: no host packing pair, one host current image, MRT PSO only for NVIDIA 3x");
        std::puts("PASS: module resource budgets, ownership, synthetic bytes and reset");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
