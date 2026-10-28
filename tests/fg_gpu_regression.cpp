#include "fg_gpu_helpers.h"
#include <cstdlib>

void run(const char* modulePath, uint32_t multiplier, const std::string& requestedScenario = "motion") {
    const bool early = requestedScenario.rfind("early-", 0) == 0;
    const auto scenario = early ? requestedScenario.substr(6) : requestedScenario;
    const bool stationary = scenario == "stationary";
    Gpu gpu;
    HMODULE module =
        LoadLibraryExA(modulePath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(module != nullptr, "load real synthesis module");
    auto getApi = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
    check(getApi != nullptr, "real API export");
    VDXRFG_ApiV1 api{};
    api.struct_size = sizeof(api);
    api.api_version = 1;
    success(getApi(&api), "negotiate ABI");
    VDXRFG_ApiV3 api3{};
    if (early) {
        auto get4 = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
        check(get4 != nullptr, "early-readiness API export");
        VDXRFG_ApiV4 api4{};
        api4.struct_size = sizeof(api4);
        api4.api_version = VDXRFG_API_VERSION_4;
        success(get4(&api4), "negotiate early-readiness pixel fixture");
        // V4 creation/submission deliberately reuse the V3 payloads.
        api3.Create = api4.Create;
        api3.Submit = api4.Submit;
        api3.Retire = api4.Retire;
        api3.Reset = api4.Reset;
        api3.GetStatistics = api4.GetStatistics;
        api3.Destroy = api4.Destroy;
    } else if (multiplier == 4) {
        auto get3 = reinterpret_cast<VDXRFG_GetApiV3Fn>(GetProcAddress(module, "VDXRFG_GetApiV3"));
        check(get3 != nullptr, "four-frame API export");
        api3.struct_size = sizeof(api3);
        api3.api_version = VDXRFG_API_VERSION_3;
        success(get3(&api3), "negotiate four-frame ABI");
    }
    auto input = gpu.texture();
    std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> outputs;
    VDXRFG_CreateInfoV1 create{};
    create.struct_size = sizeof(create);
    create.api_version = 1;
    create.device = gpu.device.Get();
    create.queue = gpu.queue.Get();
    create.input = input.Get();
    create.width = width;
    create.height = height;
    create.format = scenario == "gamma-alpha" ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    create.multiplier = multiplier;
    VDXRFG_CreateInfoV3 create3{};
    for (UINT bank = 0; bank < 2; ++bank) {
        for (UINT role = 0; role < 4; ++role) {
            if ((role == 1 && multiplier == 2) || (role == 3 && multiplier != 4)) {
                continue;
            }
            outputs[bank][role] = gpu.texture();
            if (role < 3)
                create.outputs[bank][role] = outputs[bank][role].Get();
            create3.outputs[bank][role] = outputs[bank][role].Get();
        }
    }
    void* context{};
    const auto selectedBackend = std::getenv("VDXR_FG_TEST_BACKEND");
    const bool fidelityFx = selectedBackend && std::string(selectedBackend) == "fidelityfx-full";
    check(!selectedBackend || fidelityFx || std::string(selectedBackend) == "nvidia", "known GPU fixture backend");
    if (multiplier == 4 || early) {
        create3.struct_size = sizeof(create3);
        create3.api_version = VDXRFG_API_VERSION_3;
        create3.device = create.device;
        create3.queue = create.queue;
        create3.input = create.input;
        create3.width = create.width;
        create3.height = create.height;
        create3.format = create.format;
        create3.multiplier = multiplier;
        create3.flags = VDXRFG_CREATE_GPU_TIMING | (fidelityFx ? VDXRFG_CREATE_FIDELITYFX_FULL : 0);
        if (multiplier == 4) {
            auto alias = create3;
            alias.outputs[1][3] = alias.outputs[0][1];
            check(api3.Create(&alias, &context) == VDXRFG_E_REJECTED && !context,
                  "S3 cannot alias another bank or phase");
        }
        success(api3.Create(&create3, &context), "initialize four-frame synthesis");
    } else if (fidelityFx) {
        auto get2 = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
        check(get2 != nullptr, "optional backend creation export");
        VDXRFG_ApiV2 api2{};
        api2.struct_size = sizeof(api2);
        api2.api_version = VDXRFG_API_VERSION_2;
        success(get2(&api2), "negotiate FidelityFX GPU fixture");
        check(api2.supported_create_flags & VDXRFG_CREATE_FIDELITYFX_FULL, "explicit full-resolution capability");
        VDXRFG_CreateInfoV2 options{};
        options.struct_size = sizeof(options);
        options.api_version = VDXRFG_API_VERSION_2;
        options.base = create;
        options.flags = VDXRFG_CREATE_FIDELITYFX_FULL | VDXRFG_CREATE_GPU_TIMING;
        success(api2.Create(&options, &context), "initialize FidelityFX full GPU synthesis");
    } else {
        success(api.Create(&create, &context), "initialize NVIDIA GPU synthesis");
    }
    const auto submit = [&](void* handle, const VDXRFG_SubmitInfoV3* value, VDXRFG_TicketV1* ticket) {
        if (multiplier == 4 || early)
            return api3.Submit(handle, value, ticket);
        VDXRFG_SubmitInfoV1 legacy{};
        legacy.struct_size = sizeof(legacy);
        legacy.api_version = VDXRFG_API_VERSION;
        legacy.input = value->input;
        legacy.producer_fence = value->producer_fence;
        legacy.producer_value = value->producer_value;
        legacy.serial = value->serial;
        legacy.display_time_ns = value->display_time_ns;
        legacy.display_period_ns = value->display_period_ns;
        for (unsigned i = 0; i < 3; ++i)
            legacy.target_time_ns[i] = value->target_time_ns[i];
        for (unsigned i = 0; i < 2; ++i) {
            legacy.fractions[i] = value->fractions[i];
            legacy.views[i] = value->views[i];
        }
        return api.Submit(handle, &legacy, ticket);
    };
    VDXRFG_SubmitInfoV3 info{};
    info.struct_size = sizeof(info);
    info.api_version = VDXRFG_API_VERSION_3;
    info.input = input.Get();
    info.serial = 1;
    info.display_period_ns = 10000000;
    info.display_time_ns = 1000000000;
    for (UINT eye = 0; eye < 2; ++eye) {
        info.views[eye].orientation[3] = 1;
        info.views[eye].position[0] = eye ? 0.032f : -0.032f;
        info.views[eye].fov[0] = info.views[eye].fov[3] = -0.8f;
        info.views[eye].fov[1] = info.views[eye].fov[2] = 0.8f;
        info.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
        if (scenario == "cropped-eyes") {
            info.views[eye].rect = {eye * eyeWidth + 16, 8, eyeWidth - 32, height - 16};
        }
    }
    const auto a = scene(0), b = scene(stationary ? 0 : 24);
    gpu.transfer(input.Get(), &a);
    VDXRFG_TicketV1 prime{};
    success(submit(context, &info, &prime), "prime capture");
    check(prime.prime && prime.role_mask == 4, "prime publishes only B");
    gpu.wait(static_cast<ID3D12Fence*>(prime.output_ready_fence), prime.output_ready_value);
    check(gpu.transfer(outputs[prime.bank][2].Get(), nullptr) == a, "prime B is bit-exact including alpha");
    check(api.Reset(context, 0) == VDXRFG_E_BUSY, "reset cannot invalidate retained prime");
    check(api.Destroy(context) == VDXRFG_E_BUSY, "destroy retains context while consumer owns pixels");
    // Hold bank zero to model a presenter which has not released its frame.
    gpu.transfer(input.Get(), &b);
    info.serial = 2;
    info.display_time_ns += multiplier * info.display_period_ns;
    for (UINT i = 0; i < multiplier - 1; ++i) {
        info.target_time_ns[i] = 1000000000 + (i + 1) * info.display_period_ns;
        info.fractions[i] = static_cast<float>(i + 1) / multiplier;
    }
    VDXRFG_TicketV1 pair{};
    success(submit(context, &info, &pair), "generate temporal pair");
    check(!pair.prime && pair.bank != prime.bank &&
              pair.role_mask == (multiplier == 4   ? 15u
                                 : multiplier == 3 ? 7u
                                                   : 5u),
          "pair uses distinct bank and exact role set");
    gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
    const auto real = gpu.transfer(outputs[pair.bank][2].Get(), nullptr);
    const auto s1 = gpu.transfer(outputs[pair.bank][0].Get(), nullptr);
    check(real == b, "real B stays bit-exact after generation");
    check(stationary || (s1 != a && s1 != b), "synthetic is not an endpoint repeat");
    std::vector<uint8_t> s2;
    if (multiplier >= 3) {
        s2 = gpu.transfer(outputs[pair.bank][1].Get(), nullptr);
        check(stationary || (s2 != s1 && s2 != a && s2 != b), "3X has two distinct intermediates");
    }
    if (multiplier == 4) {
        const auto s3 = gpu.transfer(outputs[pair.bank][3].Get(), nullptr);
        check(s3 != s1 && s3 != s2 && s3 != a && s3 != b, "4X S3 is a distinct written intermediate");
        const auto center = [](const std::vector<uint8_t>& pixels) {
            double sum = 0, weight = 0;
            for (unsigned y = 96; y < 160; ++y)
                for (unsigned x = 64; x < 240; ++x) {
                    const auto index = (y * width + x) * 4;
                    const double mass = (std::max)(0, int(pixels[index + 1]) - int(pixels[index]) - 30);
                    sum += x * mass;
                    weight += mass;
                }
            check(weight > 0, "moving feature remains visible");
            return sum / weight;
        };
        check(center(a) < center(s1) && center(s1) < center(s2) && center(s2) < center(s3) && center(s3) < center(b),
              "S1/S2/S3 geometric positions follow quarter/half/three-quarter temporal order");
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto differentEye = [&](const auto& left, const auto& right) {
                for (unsigned y = 0; y < height; ++y)
                    for (unsigned x = eye * eyeWidth; x < (eye + 1) * eyeWidth; ++x)
                        for (unsigned c = 0; c < 3; ++c)
                            if (left[(y * width + x) * 4 + c] != right[(y * width + x) * 4 + c])
                                return true;
                return false;
            };
            // The fixture's left eye moves; the right eye is deliberately static.
            if (eye == 0)
                check(differentEye(s3, s2) && differentEye(s3, s1), "S3 advances the moving eye independently");
        }
    }
    size_t rightEyeChanges = 0;
    size_t blendedPixels = 0;
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            for (UINT channel = 0; channel < 3; ++channel) {
                const size_t index = (y * width + x) * 4 + channel;
                if (x >= eyeWidth && std::abs(int(s1[index]) - int(b[index])) > 2) {
                    ++rightEyeChanges;
                }
                const float blend = a[index] * (1 - info.fractions[0]) + b[index] * info.fractions[0];
                if (x < eyeWidth && std::abs(s1[index] - blend) > 20) {
                    ++blendedPixels;
                }
            }
        }
    }
    std::printf("Stereo control: stationary=%u right-eye changed channels=%zu motion-vs-blend channels=%zu\n",
                stationary,
                rightEyeChanges,
                blendedPixels);
    for (UINT x : {10u, 130u, 520u, 830u, 1000u}) {
        const size_t i = (128 * width + x) * 4;
        std::printf("x=%u A=%u,%u,%u,%u S1=%u,%u,%u,%u B=%u,%u,%u,%u\n",
                    x,
                    a[i],
                    a[i + 1],
                    a[i + 2],
                    a[i + 3],
                    s1[i],
                    s1[i + 1],
                    s1[i + 2],
                    s1[i + 3],
                    b[i],
                    b[i + 1],
                    b[i + 2],
                    b[i + 3]);
    }
    if (const char* outputDirectory = std::getenv("VDXR_FG_TEST_OUTPUT")) {
        const auto path =
            std::filesystem::path(outputDirectory) / (stationary ? "stationary-s1.rgba" : "moving-s1.rgba");
        std::ofstream dump(path, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(s1.data()), s1.size());
        check(dump.good(), "write stereo-control evidence");
        if (!s2.empty()) {
            const auto secondPath =
                std::filesystem::path(outputDirectory) / (stationary ? "stationary-s2.rgba" : "moving-s2.rgba");
            std::ofstream secondDump(secondPath, std::ios::binary);
            secondDump.write(reinterpret_cast<const char*>(s2.data()), s2.size());
            check(secondDump.good(), "write second stereo-control evidence");
        }
    }
    if (stationary) {
        for (const auto* synthetic : std::array<const std::vector<uint8_t>*, 2>{&s1, &s2}) {
            if (synthetic->empty())
                continue;
            size_t bad{};
            int maximum{};
            for (size_t i = 0; i < b.size(); ++i) {
                const int error = std::abs(int((*synthetic)[i]) - int(b[i]));
                maximum = (std::max)(maximum, error);
                bad += error > 2;
            }
            std::printf(
                "Stationary role=%s channels-over-2=%zu max=%d\n", synthetic == &s1 ? "S1" : "S2", bad, maximum);
            check(!bad, "real optical flow must preserve stationary RGBA in both eyes");
        }
    }
    check(stationary || blendedPixels > 100, "synthetic contains motion compensation, not endpoint crossfade");
    size_t displaced = 0;
    for (UINT y = 104; y < 152; ++y) {
        for (UINT x = 168; x < 176; ++x) {
            const size_t index = (y * width + x) * 4 + 1;
            if (s1[index] > 100 && a[index] < 40) {
                ++displaced;
            }
        }
    }
    check(stationary || displaced > 20, "optical flow moves content beyond previous endpoint, not a repeated frame");
    check(gpu.transfer(outputs[prime.bank][2].Get(), nullptr) == a, "retained bank survives later pair");
    VDXRFG_TicketV1 rejected{};
    info.serial = 3;
    info.display_time_ns += multiplier * info.display_period_ns;
    for (UINT i = 0; i < multiplier - 1; ++i) {
        info.target_time_ns[i] += multiplier * info.display_period_ns;
    }
    check(submit(context, &info, &rejected) == VDXRFG_E_BUSY && !rejected.token,
          "third retained transaction rejects before work");
    success(api.Retire(context, prime.token), "retire prime after host readback");
    check(FAILED(api.Retire(context, prime.token)), "stale token rejected");
    success(api.Retire(context, pair.token), "retire pair after host readback");
    VDXRFG_StatisticsV1 statistics{};
    statistics.struct_size = sizeof(statistics);
    statistics.api_version = 1;
    success(api.GetStatistics(context, &statistics), "GPU statistics");
    check(statistics.captures == 2 && statistics.pairs == 1 && statistics.synthetic_frames == multiplier - 1 &&
              statistics.live_tickets == 0,
          "statistics distinguish real/generated ownership");
    std::printf("GPU spans: total=%llu us pack=%llu us flow=%llu us composition=%llu us\n",
                statistics.last_gpu_us,
                statistics.pack_gpu_us,
                statistics.flow_gpu_us,
                statistics.composition_gpu_us);
    VDXRFG_StatisticsV1 second = statistics;
    success(api.GetStatistics(context, &second), "repeat statistics poll");
    check(second.last_gpu_us == statistics.last_gpu_us && second.flow_gpu_us == statistics.flow_gpu_us,
          "latest completed timing persists across polling");
    if (stationary) {
        // Move, hold, reverse, hold. Identical pairs after genuine movement
        // must not retain the estimator's previous direction or displacement.
        int previousOffset{};
        uint64_t serial = 3;
        for (int offset : {24, 48, 48, 48, 48, 24, 0, 0}) {
            const auto next = scene(offset);
            gpu.transfer(input.Get(), &next);
            info.serial = serial++;
            VDXRFG_TicketV1 held{};
            success(submit(context, &info, &held), "submit move-hold-reverse capture");
            check(!held.prime && held.role_mask == (multiplier == 4   ? 15u
                                                    : multiplier == 3 ? 7u
                                                                      : 5u),
                  "warm capture role set");
            gpu.wait(static_cast<ID3D12Fence*>(held.output_ready_fence), held.output_ready_value);
            check(gpu.transfer(outputs[held.bank][2].Get(), nullptr) == next, "warm capture B is exact");
            for (UINT role = 0; role < multiplier - 1; ++role) {
                const auto synthetic = gpu.transfer(outputs[held.bank][role].Get(), nullptr);
                if (offset == previousOffset) {
                    size_t bad{};
                    for (size_t i = 0; i < next.size(); ++i)
                        bad += std::abs(int(synthetic[i]) - int(next[i])) > 2;
                    std::printf("Stationary after motion serial=%llu role=S%u channels-over-2=%zu\n",
                                info.serial,
                                role + 1,
                                bad);
                    check(!bad, "stationary captures after motion discard stale displacement");
                }
            }
            success(api.Retire(context, held.token), "retire move-hold-reverse capture");
            previousOffset = offset;
            info.display_time_ns += multiplier * info.display_period_ns;
            for (UINT i = 0; i < multiplier - 1; ++i)
                info.target_time_ns[i] += multiplier * info.display_period_ns;
        }
    }
    success(api.Reset(context, 1), "drained reset");
    // A deliberately parked producer must not make Submit block the CPU or
    // allow reuse/retirement of pixels which have not yet been captured.
    ComPtr<ID3D12Fence> producerGate;
    success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerGate)), "producer gate");
    info.producer_fence = producerGate.Get();
    info.producer_value = 1;
    VDXRFG_TicketV1 parked{};
    const auto start = GetTickCount64();
    const HRESULT submitted = submit(context, &info, &parked);
    const auto elapsed = GetTickCount64() - start;
    const bool safelyParked =
        SUCCEEDED(submitted) && parked.token &&
        static_cast<ID3D12Fence*>(parked.input_consumed_fence)->GetCompletedValue() < parked.input_consumed_value &&
        api.Retire(context, parked.token) == VDXRFG_E_BUSY && api.Reset(context, 1) == VDXRFG_E_BUSY &&
        api.Destroy(context) == VDXRFG_E_BUSY;
    success(producerGate->Signal(1), "release parked producer before reporting failure");
    success(submitted, "parked Submit accepts without waiting on CPU");
    check(safelyParked && elapsed < 1000, "parked input retains ticket and pending fences without CPU drain");
    gpu.wait(static_cast<ID3D12Fence*>(parked.output_ready_fence), parked.output_ready_value);
    check(gpu.transfer(outputs[parked.bank][2].Get(), nullptr) == b, "blocked producer still captures exact B");
    success(api.Retire(context, parked.token), "settled parked transaction retires");
    success(api.Destroy(context), "drained destroy");
    if (multiplier == 4) {
        // The reduced allocation contract is opt-in. Legacy B requirements
        // remain in force unless both ownership and omission are requested.
        auto omitted = create3;
        omitted.outputs[0][2] = omitted.outputs[1][2] = nullptr;
        context = nullptr;
        check(api3.Create(&omitted, &context) == VDXRFG_E_REJECTED && !context,
              "V3 without omission still requires both B outputs");
        omitted.flags |= VDXRFG_CREATE_HOST_OWNS_REAL | VDXRFG_CREATE_OMIT_REAL_OUTPUTS;
        success(api3.Create(&omitted, &context), "explicit host-owned B accepts null outer real allocations");
        info.producer_fence = nullptr;
        info.producer_value = 0;
        info.serial = 1;
        info.display_time_ns = 1000000000;
        gpu.transfer(input.Get(), &a);
        VDXRFG_TicketV1 hostPrime{};
        success(api3.Submit(context, &info, &hostPrime), "host-owned prime without B allocation");
        check(hostPrime.prime && hostPrime.role_mask == 0, "host-owned prime still owns completion with no roles");
        gpu.wait(static_cast<ID3D12Fence*>(hostPrime.output_ready_fence), hostPrime.output_ready_value);
        gpu.transfer(input.Get(), &b);
        info.serial = 2;
        info.display_time_ns += multiplier * info.display_period_ns;
        for (unsigned i = 0; i < multiplier - 1; ++i) {
            info.target_time_ns[i] = 1000000000 + (i + 1) * info.display_period_ns;
            info.fractions[i] = static_cast<float>(i + 1) / multiplier;
        }
        VDXRFG_TicketV1 hostPair{};
        success(api3.Submit(context, &info, &hostPair), "host-owned four-frame pair");
        check(hostPair.role_mask == 11 && hostPair.bank != hostPrime.bank,
              "only S1/S2/S3 published in a separate bank");
        gpu.wait(static_cast<ID3D12Fence*>(hostPair.output_ready_fence), hostPair.output_ready_value);
        const auto third = gpu.transfer(outputs[hostPair.bank][3].Get(), nullptr);
        check(third != a && third != b && third != gpu.transfer(outputs[hostPair.bank][1].Get(), nullptr),
              "omitting outer B preserves the third synthetic output");
        success(api3.Retire(context, hostPrime.token), "retire omitted-B prime");
        success(api3.Retire(context, hostPair.token), "retire all omitted-B output consumers");
        success(api3.Destroy(context), "destroy omitted-B context");
    }
    gpu.noErrors();
    FreeLibrary(module);
    std::printf("PASS: %s %uX pixels, distinct frames, two banks and retirement\n",
                fidelityFx ? "FidelityFX full" : "NVIDIA",
                multiplier);
}

int main(int argc, char** argv) {
    try {
        check(argc == 3 || argc == 4, "module path and multiplier required");
        run(argv[1], static_cast<uint32_t>(std::stoul(argv[2])), argc == 4 ? argv[3] : "motion");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
