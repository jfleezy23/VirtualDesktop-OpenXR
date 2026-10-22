#include "fg_gpu_helpers.h"
#include "fg_operation_probe.h"
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

static_assert(sizeof(VDXRFG_CreateInfoV1) == 112);
static_assert(sizeof(VDXRFG_ApiV1) == 56);
static_assert(sizeof(VDXRFG_TicketV1) == 80);
static_assert(sizeof(VDXRFG_SubmitInfoV1) == 208);

struct Images {
    std::array<std::vector<uint8_t>, 3> pixels;
    VDXRFG_StatisticsV1 statistics{};
    FgOperationCounts operations{};
    FgOperationCounts primeOperations{};
    FgOperationCounts resetPrimeOperations{};
    std::array<FgOperationCounts, 2> resetCaptureOperations{};
};

// A CPU-wait regression must fail without stranding the fixture's GPU queue.
class ProducerWatchdog {
    ID3D12Fence* fence;
    std::mutex mutex;
    std::condition_variable condition;
    bool released{};
    bool timedOut{};
    std::thread worker;

  public:
    explicit ProducerWatchdog(ID3D12Fence* gate)
        : fence(gate), worker([this] {
              std::unique_lock lock(mutex);
              if (!condition.wait_for(lock, std::chrono::seconds(2), [this] { return released; })) {
                  timedOut = true;
                  fence->Signal(1);
              }
          }) {
    }
    HRESULT release() {
        const auto hr = fence->Signal(1);
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        condition.notify_one();
        worker.join();
        return hr;
    }
    bool expired() const {
        return timedOut;
    }
    ~ProducerWatchdog() {
        if (worker.joinable())
            release();
    }
};

Images generated(HMODULE module,
                 uint32_t multiplier,
                 bool legacy,
                 uint32_t flags,
                 bool auditPrimeTiming = false,
                 bool guardFfxQueries = false) {
    const bool hostOwned = !legacy && (flags & VDXRFG_CREATE_HOST_OWNS_REAL);
    // Count module calls without debug-layer command-list aliases. Resource
    // states are checked separately by the mandatory GBV state regression.
    Gpu gpu(false);
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
    std::array<ID3D12Resource*, 7> watched{input.Get()};
    for (UINT bank = 0; bank < 2; ++bank)
        for (UINT role = 0; role < 3; ++role)
            watched[1 + bank * 3 + role] = outputs[bank][role].Get();
    // NVIDIA may record a marker after recording its resolve, but executes
    // that marker first. This recording-order guard is FFX-only.
    FgOperationProbe observer(gpu, watched, guardFfxQueries || (flags & 4u));
    VDXRFG_TicketV1 prime{};
    observer.begin();
    success(api.Submit(context, &info, &prime), "prime history");
    const auto primeOperations = observer.end(prime);
    gpu.wait(static_cast<ID3D12Fence*>(prime.output_ready_fence), prime.output_ready_value);
    check(prime.prime == 1 && prime.role_mask == (hostOwned ? 0u : 4u) && prime.token,
          "prime retains ownership even without a module real output");
    check(gpu.transfer(outputs[prime.bank][2].Get(), nullptr) == (hostOwned ? sentinel : a),
          "prime writes B only when the module publishes it");
    check(api.Reset(context, 0) == VDXRFG_E_BUSY && api.Destroy(context) == VDXRFG_E_BUSY,
          "prime remains owned until retire");
    if (auditPrimeTiming) {
        VDXRFG_StatisticsV1 stats{};
        stats.struct_size = sizeof(stats);
        stats.api_version = 1;
        observer.begin();
        success(api.GetStatistics(context, &stats), "prime timing is unavailable");
        const auto counters = observer.countersOnly();
        check(!counters.maps && !stats.last_gpu_us, "prime never harvests an unresolved timing sample");
    }
    gpu.transfer(input.Get(), &b);
    info.serial = 2;
    info.display_time_ns += multiplier * info.display_period_ns;
    for (UINT i = 0; i < multiplier - 1; ++i) {
        info.target_time_ns[i] = 1000000000 + (i + 1) * info.display_period_ns;
        info.fractions[i] = float(i + 1) / multiplier;
    }
    VDXRFG_TicketV1 pair{};
    observer.begin();
    success(api.Submit(context, &info, &pair), "synthesize fixed temporal pair");
    const auto operations = observer.end(pair);
    gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
    check(!operations.uninitializedResolves, "every resolved timestamp was written by the current transaction");
    check(!pair.prime && pair.role_mask == ((multiplier == 3 ? 3u : 1u) | (hostOwned ? 0u : 4u)) &&
              pair.bank != prime.bank,
          "pair role mask and distinct bank");
    Images result;
    result.operations = operations;
    result.primeOperations = primeOperations;
    for (UINT role = 0; role < 3; ++role) {
        if (role == 1 && multiplier == 2)
            continue;
        result.pixels[role] = gpu.transfer(outputs[pair.bank][role].Get(), nullptr);
    }
    check(result.pixels[2] == (hostOwned ? sentinel : b), "pair B exact or deliberately untouched");
    check(gpu.transfer(outputs[prime.bank][2].Get(), nullptr) == (hostOwned ? sentinel : a),
          "held prime survives pair");
    if (const auto directory = std::getenv("VDXR_FG_V2_OUTPUT")) {
        for (UINT role = 0; role < 3; ++role) {
            const auto path =
                std::filesystem::path(directory) / (std::to_string(multiplier) + "-" + std::to_string(legacy) + "-" +
                                                    std::to_string(flags) + "-" + std::to_string(role) + ".rgba");
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(result.pixels[role].data()), result.pixels[role].size());
            check(stream.good(), "write independent baseline/candidate pixel evidence");
        }
    }
    success(api.Retire(context, prime.token), "retire prime");
    success(api.Retire(context, pair.token), "retire pair");
    result.statistics.struct_size = sizeof(result.statistics);
    result.statistics.api_version = 1;
    success(api.GetStatistics(context, &result.statistics), "read counters and optional timing");
    check(result.statistics.captures == 2 && result.statistics.pairs == 1 &&
              result.statistics.synthetic_frames == multiplier - 1 && !result.statistics.live_tickets,
          "counters independent of timing selection");
    success(api.Reset(context, 0), "reset drained context");
    // Exercise a skipped prime on a fresh pack list, and again after this same
    // bank has recorded a pair. The producer gate proves ready is still ordered
    // after actual GPU work even when no outer output roles are published.
    for (UINT cycle = 0; cycle < 2; ++cycle) {
        gpu.transfer(input.Get(), &a);
        ComPtr<ID3D12Fence> gate;
        success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "reset prime producer gate");
        info.serial = 3 + cycle * 2;
        info.display_time_ns = 2000000000 + cycle * 1000000000;
        info.producer_fence = gate.Get();
        info.producer_value = 1;
        VDXRFG_TicketV1 resetPrime{};
        ProducerWatchdog watchdog(gate.Get());
        observer.begin();
        const auto submitted = api.Submit(context, &info, &resetPrime);
        // Unpark before any assertion can throw; no failed fixture may strand a queue.
        const bool pending = SUCCEEDED(submitted) && resetPrime.input_consumed_fence && resetPrime.output_ready_fence &&
                             resetPrime.input_consumed_value && resetPrime.output_ready_value &&
                             static_cast<ID3D12Fence*>(resetPrime.input_consumed_fence)->GetCompletedValue() <
                                 resetPrime.input_consumed_value &&
                             static_cast<ID3D12Fence*>(resetPrime.output_ready_fence)->GetCompletedValue() <
                                 resetPrime.output_ready_value;
        const bool busy = api.Retire(context, resetPrime.token) == VDXRFG_E_BUSY &&
                          api.Reset(context, 0) == VDXRFG_E_BUSY && api.Destroy(context) == VDXRFG_E_BUSY;
        success(watchdog.release(), "unpark reset prime producer");
        const auto resetOperations = observer.end(resetPrime);
        check(!watchdog.expired(), "prime submit and busy lifecycle checks return without waiting for the GPU");
        success(submitted, "prime after reset without a CPU GPU wait");
        check(pending && busy, "unpublished prime retains pending consume/ready fences and exclusive ownership");
        check(resetPrime.prime && resetPrime.role_mask == (hostOwned ? 0u : 4u) && resetPrime.bank == 0 &&
                  resetPrime.token > pair.token && resetPrime.generation > pair.generation,
              "reset prime retains fresh generation and nonreused ownership identity");
        result.resetPrimeOperations = resetOperations;
        result.resetCaptureOperations[cycle] = resetOperations;
        gpu.wait(static_cast<ID3D12Fence*>(resetPrime.output_ready_fence), resetPrime.output_ready_value);
        if (auditPrimeTiming) {
            VDXRFG_StatisticsV1 stats{};
            stats.struct_size = sizeof(stats);
            stats.api_version = 1;
            observer.begin();
            success(api.GetStatistics(context, &stats), "reset prime has no complete timing sample");
            const auto counters = observer.countersOnly();
            check(!counters.maps && !stats.last_gpu_us && !stats.pack_gpu_us && !stats.flow_gpu_us &&
                      !stats.composition_gpu_us,
                  "reset prime never republishes timing from a prior use of its work slot");
        }
        success(api.Retire(context, resetPrime.token), "retire reset prime before reusing same allocator bank");
        gpu.transfer(input.Get(), &b);
        info.producer_fence = nullptr;
        info.producer_value = 0;
        ++info.serial;
        const auto previousTime = info.display_time_ns;
        info.display_time_ns += multiplier * info.display_period_ns;
        for (UINT i = 0; i < multiplier - 1; ++i)
            info.target_time_ns[i] = previousTime + (i + 1) * info.display_period_ns;
        VDXRFG_TicketV1 reused{};
        success(api.Submit(context, &info, &reused), "record pair after skipped prime on same bank");
        gpu.wait(static_cast<ID3D12Fence*>(reused.output_ready_fence), reused.output_ready_value);
        check(!reused.prime && reused.bank == resetPrime.bank, "pair reuses retired prime bank safely");
        for (UINT role = 0; role < multiplier - 1; ++role)
            check(gpu.transfer(outputs[reused.bank][role].Get(), nullptr) == result.pixels[role],
                  "same-bank reuse and reset preserve exact synthetic pixels");
        success(api.Retire(context, reused.token), "retire same-bank pair");
        success(api.Reset(context, 0), "reset context after reused pair");
    }
    if (auditPrimeTiming) {
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            // Three work slots can each retain one cached and one pending
            // sample. Do not poll while building this backlog.
            for (unsigned frame = 0; frame < 9; ++frame) {
                gpu.transfer(input.Get(), frame % 2 ? &b : &a);
                ++info.serial;
                const auto before = info.display_time_ns;
                info.display_time_ns += multiplier * info.display_period_ns;
                for (UINT i = 0; i < multiplier - 1; ++i)
                    info.target_time_ns[i] = before + (i + 1) * info.display_period_ns;
                VDXRFG_TicketV1 backlog{};
                observer.begin();
                success(api.Submit(context, &info, &backlog), "build unpolled timing backlog");
                const auto recorded = observer.end(backlog);
                gpu.wait(static_cast<ID3D12Fence*>(backlog.output_ready_fence), backlog.output_ready_value);
                check(!recorded.uninitializedResolves, "backlog resolves only initialized timestamps");
                success(api.Retire(context, backlog.token), "retire unpolled pair");
            }
            if (cycle == 0 || cycle == 3) {
                VDXRFG_StatisticsV1 latest{};
                latest.struct_size = sizeof(latest);
                latest.api_version = 1;
                observer.begin();
                if (cycle == 3)
                    observer.failMapAfter(1);
                success(api.GetStatistics(context, &latest), "poll cached and pending backlog together");
                const auto firstPoll = observer.countersOnly();
                check(firstPoll.maps == 3 && firstPoll.failedMaps == (cycle == 3 ? 1u : 0u) && latest.last_gpu_us > 0,
                      "one poll harvests all three pending samples beyond the three cached samples");
                auto repeated = latest;
                observer.begin();
                success(api.GetStatistics(context, &repeated), "repeat completed timing poll");
                const auto secondPoll = observer.countersOnly();
                check(!secondPoll.maps && repeated.last_gpu_us == latest.last_gpu_us &&
                          repeated.pack_gpu_us == latest.pack_gpu_us && repeated.flow_gpu_us == latest.flow_gpu_us &&
                          repeated.composition_gpu_us == latest.composition_gpu_us,
                      "latest completed timing persists without another pending readback");
            }
            observer.begin();
            if (cycle == 2)
                observer.failMapAfter(1);
            success(api.Reset(context, 0), "discard cached and pending timing, including a failed readback");
            const auto drained = observer.countersOnly();
            check(drained.failedMaps == (cycle == 2 ? 1u : 0u),
                  "requested reset readback fault was actually exercised");
            VDXRFG_StatisticsV1 fresh{};
            fresh.struct_size = sizeof(fresh);
            fresh.api_version = 1;
            observer.begin();
            success(api.GetStatistics(context, &fresh), "poll timing after backlog reset");
            const auto polled = observer.countersOnly();
            check(!polled.maps && !fresh.last_gpu_us && !fresh.pack_gpu_us && !fresh.flow_gpu_us &&
                      !fresh.composition_gpu_us,
                  "reset drains all old-generation timing and clears published intervals");
        }
    }
    success(api.Destroy(context), "destroy drained context");
    gpu.noErrors();
    return result;
}

void barrierBatching(HMODULE module) {
    for (uint32_t multiplier : {2u, 3u}) {
        for (UINT mode = 0; mode < 5; ++mode) {
            const bool legacy = mode == 0;
            const uint32_t flags = mode > 2
                                       ? VDXRFG_CREATE_HOST_OWNS_REAL | ((mode == 4) ? VDXRFG_CREATE_GPU_TIMING : 0)
                                       : (mode == 2 ? VDXRFG_CREATE_GPU_TIMING : 0);
            const bool hostOwned = (flags & VDXRFG_CREATE_HOST_OWNS_REAL) != 0;
            const auto result = generated(module, multiplier, legacy, flags);
            std::printf("%uX mode%u pair outer barrier calls=%u descriptors=%u copies=%u\n",
                        multiplier,
                        mode,
                        result.operations.outerBarrierCalls,
                        result.operations.outerTransitions,
                        result.operations.outerCopies);
            check(result.operations.outerBarrierCalls == 4 &&
                      result.operations.outerTransitions == (multiplier + (hostOwned ? 0u : 1u)) * 4 &&
                      result.operations.outerCopies == (multiplier + (hostOwned ? 0u : 1u)) * 2,
                  "pair batches barrier calls without losing any transitions or eye copies");
            for (const auto& prime : {result.primeOperations, result.resetPrimeOperations})
                check(prime.outerBarrierCalls == (hostOwned ? 2u : 4u) &&
                          prime.outerTransitions == (hostOwned ? 4u : 8u) && prime.outerCopies == (hostOwned ? 2u : 4u),
                      "initial and reset prime preserve all transitions and required eye copies");
        }
    }
}

void emptyPrimeSubmission(HMODULE module) {
    for (uint32_t multiplier : {2u, 3u}) {
        const auto ordinary = generated(module, multiplier, false, 0);
        const auto owned = generated(module, multiplier, false, VDXRFG_CREATE_HOST_OWNS_REAL);
        const auto timedOwned =
            generated(module, multiplier, false, VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_GPU_TIMING);
        const auto legacy = generated(module, multiplier, true, 0);
        for (const auto& result : {owned, timedOwned}) {
            for (const auto& prime : {result.primeOperations, result.resetPrimeOperations}) {
                std::printf("%uX host-owned prime submissions=%u resets=%u closes=%u\n",
                            multiplier,
                            prime.submits,
                            prime.listResets,
                            prime.listCloses);
                check(prime.submits == 3 && prime.listResets + 1 == ordinary.primeOperations.listResets &&
                          prime.listCloses + 1 == ordinary.primeOperations.listCloses,
                      "empty outer prime removes exactly one submission/reset/close and retains backend work");
            }
            check(result.pixels[0] == ordinary.pixels[0] && result.pixels[1] == ordinary.pixels[1],
                  "empty prime elision leaves following generated images exact");
        }
        check(ordinary.primeOperations.submits == 4 && legacy.primeOperations.submits == 4 &&
                  ordinary.resetPrimeOperations.submits == 4 && legacy.resetPrimeOperations.submits == 4,
              "V1 and ordinary V2 primes retain actual real-image pack submissions");
        check(ordinary.operations.submits == owned.operations.submits,
              "nonprime submission count is unchanged by empty prime elision");
    }
}

void directHistoryCapture(HMODULE module) {
    bool reduced = true;
    for (uint32_t multiplier : {2u, 3u}) {
        for (const bool fidelityFx : {false, true}) {
            for (UINT mode = fidelityFx ? 1u : 0u; mode < 5; ++mode) {
                const bool legacy = mode == 0;
                const bool hostOwned = mode >= 3;
                const bool timed = legacy || mode == 2 || mode == 4;
                const uint32_t flags = (fidelityFx ? VDXRFG_CREATE_FIDELITYFX_FULL : 0u) |
                                       (hostOwned ? VDXRFG_CREATE_HOST_OWNS_REAL : 0u) |
                                       (timed && !legacy ? VDXRFG_CREATE_GPU_TIMING : 0u);
                const auto result = generated(module, multiplier, legacy, flags);
                const auto verify = [&](const FgOperationCounts& operations, bool prime, const char* phase) {
                    std::printf("history %s %uX mode%u %s submits=%u resets=%u closes=%u textureCopies=%u "
                                "resourceCopies=%u captureCopies=%u\n",
                                fidelityFx ? "ffx" : "nvidia",
                                multiplier,
                                mode,
                                phase,
                                operations.submits,
                                operations.listResets,
                                operations.listCloses,
                                operations.copies,
                                operations.resourceCopies,
                                operations.captureRegionCopies);
                    // Removing the normalization list and the following
                    // whole-resource history copy leaves one owned capture.
                    // These are literal driver-call budgets, not counters
                    // supplied by the module under test.
                    const unsigned submits = prime ? (hostOwned ? 3u : 4u) : (fidelityFx ? 4u : timed ? 6u : 5u);
                    const unsigned resets =
                        prime ? (fidelityFx ? (hostOwned ? 3u : 4u) : (hostOwned ? 4u : 5u)) : submits;
                    const unsigned copies = prime ? (hostOwned ? 2u : 4u) : (multiplier + (hostOwned ? 0u : 1u)) * 2u;
                    const unsigned resourceCopies = hostOwned && !prime ? 0u : 1u;
                    reduced &= operations.submits == submits && operations.listResets == resets &&
                               operations.listCloses == resets && operations.copies == copies &&
                               operations.resourceCopies == resourceCopies && operations.captureRegionCopies == 2;
                };
                verify(result.primeOperations, true, "prime");
                verify(result.operations, false, "pair");
                verify(result.resetCaptureOperations[0], true, "fresh-reset-prime");
                verify(result.resetCaptureOperations[1], true, "reused-reset-prime");
            }
        }
    }
    check(reduced,
          "owned history retains capture and submission budgets while host-owned pairs skip the unused real copy");
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
        check(argc == 2 || argc == 3, "module path and optional barriers/prime/capture/history mode required");
        HMODULE module =
            LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load real module");
        auto get = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
        check(get != nullptr, "module publishes optional creation and diagnostic negotiation");
        rejectEnvelopes(get);
        const std::string mode = argc == 3 ? argv[2] : "all";
        check(mode == "all" || mode == "barriers" || mode == "prime" || mode == "capture" || mode == "history",
              "recognized test mode");
        if (mode == "all" || mode == "capture") {
            timingComparison(module);
            hostRealComparison(module);
        }
        if (mode == "all" || mode == "barriers")
            barrierBatching(module);
        if (mode == "all" || mode == "prime")
            emptyPrimeSubmission(module);
        if (mode == "all" || mode == "history")
            directHistoryCapture(module);
        FreeLibrary(module);
        std::puts("PASS: optional creation negotiation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
