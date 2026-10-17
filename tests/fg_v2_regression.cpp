#include "fg_gpu_helpers.h"
#include "fg_operation_probe.h"

static_assert(sizeof(VDXRFG_CreateInfoV1) == 112);
static_assert(sizeof(VDXRFG_ApiV1) == 56);
static_assert(sizeof(VDXRFG_TicketV1) == 80);
static_assert(sizeof(VDXRFG_SubmitInfoV1) == 208);

struct Images {
    std::array<std::vector<uint8_t>, 3> pixels;
    VDXRFG_StatisticsV1 statistics{};
    FgOperationCounts operations{};
};

Images generated(HMODULE module, uint32_t multiplier, bool legacy, uint32_t flags) {
    const bool hostOwned = !legacy && (flags & VDXRFG_CREATE_HOST_OWNS_REAL);
    Gpu gpu;
    VDXRFG_ApiV1 api{};
    api.struct_size = sizeof(api);
    api.api_version = 1;
    auto get1 = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
    success(get1(&api), "legacy lifecycle table");
    VDXRFG_ApiV2 api2{};
    api2.struct_size = sizeof(api2);
    api2.api_version = 2;
    auto get2 = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
    success(get2(&api2), "optional creation table");
    auto input = gpu.texture();
    std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs;
    VDXRFG_CreateInfoV2 create{};
    create.struct_size = sizeof(create);
    create.api_version = 2;
    create.flags = flags;
    auto& base = create.base;
    base.struct_size = sizeof(base);
    base.api_version = 1;
    base.device = gpu.device.Get();
    base.queue = gpu.queue.Get();
    base.input = input.Get();
    base.width = width;
    base.height = height;
    base.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    base.multiplier = multiplier;
    const auto sentinel = scene(64);
    for (UINT bank = 0; bank < 2; ++bank) {
        for (UINT role = 0; role < 3; ++role) {
            if (role == 1 && multiplier == 2)
                continue;
            outputs[bank][role] = gpu.texture();
            if (role == 2)
                gpu.transfer(outputs[bank][role].Get(), &sentinel);
            base.outputs[bank][role] = outputs[bank][role].Get();
        }
    }
    void* context{};
    success(legacy ? api.Create(&base, &context) : api2.Create(&create, &context), "create synthesis context");
    check(context != nullptr, "successful creation returns owned context");
    VDXRFG_SubmitInfoV1 info{};
    info.struct_size = sizeof(info);
    info.api_version = 1;
    info.input = input.Get();
    info.serial = 1;
    info.display_time_ns = 1000000000;
    info.display_period_ns = 10000000;
    for (UINT eye = 0; eye < 2; ++eye) {
        auto& view = info.views[eye];
        view.orientation[3] = 1;
        view.position[0] = eye ? .032f : -.032f;
        view.fov[0] = view.fov[3] = -.8f;
        view.fov[1] = view.fov[2] = .8f;
        view.rect = {eye * eyeWidth, 0, eyeWidth, height};
    }
    const auto a = scene(0), b = scene(24);
    gpu.transfer(input.Get(), &a);
    VDXRFG_TicketV1 prime{};
    success(api.Submit(context, &info, &prime), "prime history");
    gpu.wait(static_cast<ID3D12Fence*>(prime.output_ready_fence), prime.output_ready_value);
    check(prime.prime == 1 && prime.role_mask == (hostOwned ? 0u : 4u) && prime.token,
          "prime retains ownership even without a module real output");
    check(gpu.transfer(outputs[prime.bank][2].Get(), nullptr) == (hostOwned ? sentinel : a),
          "prime writes B only when the module publishes it");
    check(api.Reset(context, 0) == VDXRFG_E_BUSY && api.Destroy(context) == VDXRFG_E_BUSY,
          "prime remains owned until retire");
    gpu.transfer(input.Get(), &b);
    info.serial = 2;
    info.display_time_ns += multiplier * info.display_period_ns;
    for (UINT i = 0; i < multiplier - 1; ++i) {
        info.target_time_ns[i] = 1000000000 + (i + 1) * info.display_period_ns;
        info.fractions[i] = float(i + 1) / multiplier;
    }
    VDXRFG_TicketV1 pair{};
    FgOperationProbe observer(gpu);
    observer.begin();
    success(api.Submit(context, &info, &pair), "synthesize fixed temporal pair");
    const auto operations = observer.end();
    gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
    check(!pair.prime && pair.role_mask == ((multiplier == 3 ? 3u : 1u) | (hostOwned ? 0u : 4u)) &&
              pair.bank != prime.bank,
          "pair role mask and distinct bank");
    Images result;
    result.operations = operations;
    for (UINT role = 0; role < 3; ++role) {
        if (role == 1 && multiplier == 2)
            continue;
        result.pixels[role] = gpu.transfer(outputs[pair.bank][role].Get(), nullptr);
    }
    check(result.pixels[2] == (hostOwned ? sentinel : b), "pair B exact or deliberately untouched");
    check(gpu.transfer(outputs[prime.bank][2].Get(), nullptr) == (hostOwned ? sentinel : a),
          "held prime survives pair");
    success(api.Retire(context, prime.token), "retire prime");
    success(api.Retire(context, pair.token), "retire pair");
    result.statistics.struct_size = sizeof(result.statistics);
    result.statistics.api_version = 1;
    success(api.GetStatistics(context, &result.statistics), "read counters and optional timing");
    check(result.statistics.captures == 2 && result.statistics.pairs == 1 &&
              result.statistics.synthetic_frames == multiplier - 1 && !result.statistics.live_tickets,
          "counters independent of timing selection");
    success(api.Reset(context, 0), "reset drained context");
    success(api.Destroy(context), "destroy drained context");
    gpu.noErrors();
    return result;
}

void hostRealComparison(HMODULE module) {
    for (uint32_t multiplier : {2u, 3u}) {
        const auto ordinary = generated(module, multiplier, false, 0);
        const auto owned = generated(module, multiplier, false, VDXRFG_CREATE_HOST_OWNS_REAL);
        check(ordinary.pixels[0] == owned.pixels[0] && ordinary.pixels[1] == owned.pixels[1],
              "host-owned real preserves exact generated pixels");
        std::printf("%uX pair copy calls ordinary/host-owned=%u/%u\n",
                    multiplier,
                    ordinary.operations.copies,
                    owned.operations.copies);
        check(ordinary.operations.copies == owned.operations.copies + 2,
              "host-owned real removes exactly one outer stereo repack");
    }
}

void timingComparison(HMODULE module) {
    for (uint32_t multiplier : {2u, 3u}) {
        const auto legacy = generated(module, multiplier, true, 0);
        const auto timed = generated(module, multiplier, false, VDXRFG_CREATE_GPU_TIMING);
        const auto untimed = generated(module, multiplier, false, 0);
        check(legacy.pixels == timed.pixels && timed.pixels == untimed.pixels,
              "timing selection preserves exact synthetic and real GPU pixels");
        check(legacy.statistics.last_gpu_us > 0 && timed.statistics.last_gpu_us > 0,
              "legacy and explicit diagnostics collect actual timing");
        check(!untimed.statistics.last_gpu_us && !untimed.statistics.pack_gpu_us && !untimed.statistics.flow_gpu_us &&
                  !untimed.statistics.composition_gpu_us,
              "disabled diagnostics expose unavailable timing with counters intact");
        std::printf("%uX pair operations on/off: submits=%u/%u waits=%u/%u queries=%u/%u resolves=%u/%u\n",
                    multiplier,
                    timed.operations.submits,
                    untimed.operations.submits,
                    timed.operations.waits,
                    untimed.operations.waits,
                    timed.operations.queries,
                    untimed.operations.queries,
                    timed.operations.resolves,
                    untimed.operations.resolves);
        check(timed.operations.submits == untimed.operations.submits + 1 &&
                  timed.operations.waits == untimed.operations.waits + 1 && timed.operations.queries > 0 &&
                  !untimed.operations.queries && timed.operations.resolves == 1 && !untimed.operations.resolves,
              "timing off removes marker submission and queries while retaining real dependencies");
    }
}

void rejectEnvelopes(VDXRFG_GetApiV2Fn get) {
    check(FAILED(get(nullptr)), "null V2 table rejected");
    struct ExtendedTable {
        VDXRFG_ApiV2 table;
        uint64_t sentinel;
    } extended{};
    extended.table.struct_size = sizeof(extended);
    extended.table.api_version = VDXRFG_API_VERSION_2;
    extended.sentinel = 0x123456789abcdef0ull;
    success(get(&extended.table), "larger caller envelope accepts known V2 prefix");
    check(extended.table.struct_size == sizeof(VDXRFG_ApiV2) && extended.sentinel == 0x123456789abcdef0ull,
          "returned size identifies known table and leaves caller tail untouched");
    VDXRFG_ApiV2 api{};
    api.struct_size = sizeof(api) - 1;
    api.api_version = 2;
    check(FAILED(get(&api)), "short V2 table rejected");
    api.struct_size = sizeof(api);
    api.api_version = 1;
    check(FAILED(get(&api)), "V2 export rejects wrong table version");
    api.api_version = 2;
    success(get(&api), "negotiate V2 without GPU initialization");
    check(api.struct_size == sizeof(api) && api.api_version == 2 && !api.reserved &&
              (api.supported_create_flags & VDXRFG_CREATE_GPU_TIMING) && api.Create && api.Submit && api.Retire &&
              api.Reset && api.GetStatistics && api.Destroy,
          "complete creation capabilities and lifecycle table");
    check(FAILED(api.Create(nullptr, nullptr)), "null creation outputs rejected");
    VDXRFG_CreateInfoV2 create{};
    create.struct_size = sizeof(create);
    create.api_version = 2;
    create.base.struct_size = sizeof(create.base);
    create.base.api_version = 1;
    create.base.device = create.base.queue = create.base.input = reinterpret_cast<void*>(1);
    const auto rejected = [&](const VDXRFG_CreateInfoV2& input) {
        void* context = reinterpret_cast<void*>(1);
        check(api.Create(&input, &context) == VDXRFG_E_REJECTED && !context,
              "invalid creation envelope clears output before dereferencing resources");
    };
    auto bad = create;
    bad.struct_size--;
    rejected(bad);
    bad = create;
    bad.api_version = 1;
    rejected(bad);
    bad = create;
    bad.flags = 0x80000000u;
    rejected(bad);
    bad = create;
    bad.reserved = 1;
    rejected(bad);
    bad = create;
    bad.base.struct_size--;
    rejected(bad);
    bad = create;
    bad.base.api_version = 2;
    rejected(bad);
    bad = create;
    bad.base.reserved[1] = 1;
    rejected(bad);
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "module path required");
        HMODULE module =
            LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load real module");
        auto get = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
        check(get != nullptr, "module publishes optional creation and diagnostic negotiation");
        rejectEnvelopes(get);
        timingComparison(module);
        hostRealComparison(module);
        FreeLibrary(module);
        std::puts("PASS: optional creation negotiation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
