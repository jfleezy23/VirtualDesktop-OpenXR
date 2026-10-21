#include "fg_gpu_helpers.h"
#include <cstdlib>

void run(const char* modulePath, uint32_t multiplier, const std::string& scenario = "motion") {
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
    auto input = gpu.texture();
    std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs;
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
    for (UINT bank = 0; bank < 2; ++bank) {
        for (UINT role = 0; role < 3; ++role) {
            if (role == 1 && multiplier == 2) {
                continue;
            }
            outputs[bank][role] = gpu.texture();
            create.outputs[bank][role] = outputs[bank][role].Get();
        }
    }
    void* context{};
    const auto selectedBackend = std::getenv("VDXR_FG_TEST_BACKEND");
    const bool fidelityFx = selectedBackend && std::string(selectedBackend) == "fidelityfx-full";
    check(!selectedBackend || fidelityFx || std::string(selectedBackend) == "nvidia", "known GPU fixture backend");
    if (fidelityFx) {
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
    VDXRFG_SubmitInfoV1 info{};
    info.struct_size = sizeof(info);
    info.api_version = 1;
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
    success(api.Submit(context, &info, &prime), "prime capture");
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
    success(api.Submit(context, &info, &pair), "generate temporal pair");
    check(!pair.prime && pair.bank != prime.bank && pair.role_mask == (multiplier == 3 ? 7u : 5u),
          "pair uses distinct bank and exact role set");
    gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
    const auto real = gpu.transfer(outputs[pair.bank][2].Get(), nullptr);
    const auto s1 = gpu.transfer(outputs[pair.bank][0].Get(), nullptr);
    check(real == b, "real B stays bit-exact after generation");
    check(stationary || (s1 != a && s1 != b), "synthetic is not an endpoint repeat");
    if (multiplier == 3) {
        const auto s2 = gpu.transfer(outputs[pair.bank][1].Get(), nullptr);
        check(stationary || (s2 != s1 && s2 != a && s2 != b), "3X has two distinct intermediates");
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
    check(api.Submit(context, &info, &rejected) == VDXRFG_E_BUSY && !rejected.token,
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
    success(api.Reset(context, 1), "drained reset");
    // A deliberately parked producer must not make Submit block the CPU or
    // allow reuse/retirement of pixels which have not yet been captured.
    ComPtr<ID3D12Fence> producerGate;
    success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerGate)), "producer gate");
    info.producer_fence = producerGate.Get();
    info.producer_value = 1;
    VDXRFG_TicketV1 parked{};
    const auto start = GetTickCount64();
    const HRESULT submitted = api.Submit(context, &info, &parked);
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
