#include "fg_gpu_helpers.h"
#include "fg_mrt_operation_probe.h"

int main(int argc, char** argv) {
    try {
        check(argc == 2 || argc == 3, "usage: fg_mrt_operation_regression MODULE [mrt-pso-failure]");
        const bool failMrtPso = argc == 3;
        check(!failMrtPso || std::string(argv[2]) == "mrt-pso-failure", "known native MRT failure injection");
        const HMODULE module = LoadLibraryA(argv[1]);
        check(module != nullptr, "load real shared-atlas module");
        Gpu gpu(false);
        VDXRFG_ApiV1 api{};
        api.struct_size = sizeof(api);
        api.api_version = 1;
        const auto get1 = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
        check(get1 != nullptr, "real lifecycle export");
        success(get1(&api), "real lifecycle table");
        VDXRFG_ApiV2 api2{};
        api2.struct_size = sizeof(api2);
        api2.api_version = 2;
        const auto get2 = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
        check(get2 != nullptr, "real V2 creation export");
        success(get2(&api2), "real creation table");
        auto input = gpu.texture();
        std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs;
        std::array<ID3D12Resource*, 4> watched{};
        VDXRFG_CreateInfoV2 create{};
        create.struct_size = sizeof(create);
        create.api_version = 2;
        create.flags = VDXRFG_CREATE_HOST_OWNS_REAL;
        auto& base = create.base;
        base.struct_size = sizeof(base);
        base.api_version = 1;
        base.device = gpu.device.Get();
        base.queue = gpu.queue.Get();
        base.input = input.Get();
        base.width = width;
        base.height = height;
        base.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        base.multiplier = 3;
        for (UINT bank = 0; bank < 2; ++bank)
            for (UINT role = 0; role < 3; ++role) {
                outputs[bank][role] = gpu.texture();
                base.outputs[bank][role] = outputs[bank][role].Get();
                if (role < 2)
                    watched[bank * 2 + role] = outputs[bank][role].Get();
            }
        // Attach before Create so exact shader bytecode, RTV resources and the
        // initial command-list PSO are observed rather than guessed from draws.
        FgMrtOperationProbe observer(gpu, watched, failMrtPso);
        void* context{};
        success(api2.Create(&create, &context), "create default medium NVIDIA context");
        check(context != nullptr, "successful creation owns a context");
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
        check(prime.prime && !prime.role_mask, "host-owned prime publishes no synthetic output");
        gpu.transfer(input.Get(), &b);
        info.serial = 2;
        info.display_time_ns += 3 * info.display_period_ns;
        info.target_time_ns[0] = 1010000000;
        info.target_time_ns[1] = 1020000000;
        info.fractions[0] = 1.0f / 3.0f;
        info.fractions[1] = 2.0f / 3.0f;
        VDXRFG_TicketV1 pair{};
        observer.begin();
        success(api.Submit(context, &info, &pair), "submit normal NVIDIA 3x pair");
        const auto counts = observer.end(pair);
        gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
        check(!pair.prime && pair.token && pair.bank != prime.bank, "pair owns its independent output bank");
        const auto first = gpu.transfer(outputs[pair.bank][0].Get(), nullptr);
        const auto second = gpu.transfer(outputs[pair.bank][1].Get(), nullptr);
        check(first.size() == width * height * 4 && second.size() == first.size() && first != second,
              "two independently timed synthetic images exist");
        success(api.Retire(context, prime.token), "retire prime");
        success(api.Retire(context, pair.token), "retire pair");
        success(api.Destroy(context), "destroy context");
        check(FreeLibrary(module) != 0, "unload real module after successful cleanup");
        std::printf("native normal NVIDIA3x: legacy_draws=%u MRT_draws=%u one_RTV_draws=%u two_RTV_draws=%u\n",
                    counts.singleDraws,
                    counts.dualDraws,
                    counts.singleTargetDraws,
                    counts.dualTargetDraws);
        if (failMrtPso) {
            check(counts.rejectedMrtPsos == 1 && counts.singleDraws == 4 && counts.dualDraws == 0 &&
                      counts.singleTargetDraws == 4 && counts.dualTargetDraws == 0,
                  "unsupported MRT PSO falls back to four real legacy draws after successful pair completion");
        } else {
            check(counts.singleDraws == 0 && counts.dualDraws == 2 && counts.singleTargetDraws == 0 &&
                      counts.dualTargetDraws == 2,
                  "normal NVIDIA 3x shares S1/S2 in exactly two real two-target composition draws");
        }
        std::puts("PASS: normal NVIDIA 3x shared S1/S2 native operation contract");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
