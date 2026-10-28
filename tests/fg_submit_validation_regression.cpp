#include "fg_gpu_helpers.h"

// Invalid submissions must reject before taking ownership, updating counters,
// or publishing outputs. Exercise the public V3 API and the actual module.
int main(int argc, char** argv) {
    try {
        check(argc == 3, "module and foreign-token/foreign-busy/fractions required");
        const std::string scenario = argv[2];
        check(scenario == "foreign-token" || scenario == "foreign-busy" || scenario == "fractions",
              "known validation scenario");
        Gpu gpu;
        const auto path = std::filesystem::absolute(argv[1]);
        const HMODULE module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load module");
        const auto getApi = reinterpret_cast<VDXRFG_GetApiV3Fn>(GetProcAddress(module, "VDXRFG_GetApiV3"));
        check(getApi != nullptr, "V3 export");
        VDXRFG_ApiV3 api{};
        api.struct_size = sizeof(api);
        api.api_version = VDXRFG_API_VERSION_3;
        success(getApi(&api), "negotiate V3");
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
        create.multiplier = 4;
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned role = 0; role < 4; ++role) {
                outputs[bank][role] = gpu.texture();
                create.outputs[bank][role] = outputs[bank][role].Get();
            }
        void* context{};
        success(api.Create(&create, &context), "create validation context");
        VDXRFG_SubmitInfoV3 info{};
        info.struct_size = sizeof(info);
        info.api_version = VDXRFG_API_VERSION_3;
        info.input = input.Get();
        info.serial = 1;
        info.display_time_ns = 1000000000;
        info.display_period_ns = 10000000;
        for (unsigned eye = 0; eye < 2; ++eye) {
            info.views[eye].orientation[3] = 1;
            info.views[eye].position[0] = eye ? .032f : -.032f;
            info.views[eye].fov[0] = info.views[eye].fov[3] = -.8f;
            info.views[eye].fov[1] = info.views[eye].fov[2] = .8f;
            info.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
        }
        const auto pixels = scene(0);
        gpu.transfer(input.Get(), &pixels);
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        ComPtr<ID3D12Device> foreignDevice;
        ComPtr<ID3D12Fence> foreignFence;
        if (scenario != "fractions") {
            success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "foreign adapter factory");
            success(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "separate validation adapter");
            success(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&foreignDevice)),
                    "separate fence device");
            success(foreignDevice->CreateFence(1, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreignFence)), "foreign fence");
        }
        const auto completed = [&](const VDXRFG_TicketV1& ticket) {
            if (ticket.token && ticket.output_ready_fence) {
                gpu.wait(static_cast<ID3D12Fence*>(ticket.output_ready_fence), ticket.output_ready_value);
                success(api.Retire(context, ticket.token), "retire settled validation ticket");
            }
        };
        if (scenario == "foreign-token") {
            auto invalid = info;
            invalid.producer_fence = foreignFence.Get();
            invalid.producer_value = 1;
            VDXRFG_TicketV1 rejected{};
            check(api.Submit(context, &invalid, &rejected) == VDXRFG_E_REJECTED && !rejected.token,
                  "foreign fence rejects without ownership");
            VDXRFG_TicketV1 prime{};
            success(api.Submit(context, &info, &prime), "prime after foreign fence rejection");
            completed(prime);
            success(api.Destroy(context), "destroy validation context");
            check(prime.token == 1, "foreign fence rejection must not consume an ownership token");
        } else {
            VDXRFG_TicketV1 prime{};
            success(api.Submit(context, &info, &prime), "prime validation context");
            gpu.wait(static_cast<ID3D12Fence*>(prime.output_ready_fence), prime.output_ready_value);
            info.serial = 2;
            info.display_time_ns = 2000000000;
            info.target_time_ns[3] = info.display_time_ns;
            for (unsigned i = 0; i < 3; ++i) {
                info.target_time_ns[i] = 1000000000 + (i + 1) * 250000000;
                info.fractions[i] = (i + 1) * .25f;
            }
            if (scenario == "fractions") {
                auto invalid = info;
                invalid.target_time_ns[0] = 1500000000;
                invalid.target_time_ns[1] = 1500000001;
                invalid.fractions[0] = .500005f;
                invalid.fractions[1] = .499995f;
                VDXRFG_TicketV1 rejected{};
                const HRESULT status = api.Submit(context, &invalid, &rejected);
                completed(rejected);
                completed(prime);
                if (status != VDXRFG_E_REJECTED || rejected.token) {
                    success(api.Destroy(context), "destroy after accepted invalid fractions");
                    check(false, "non-increasing fractions must reject before capturing or publishing pixels");
                }
                VDXRFG_TicketV1 pair{};
                success(api.Submit(context, &info, &pair), "valid pair recovers after fraction rejection");
                completed(pair);
                check(pair.token == prime.token + 1 && pair.previous_serial == prime.current_serial,
                      "fraction rejection preserves history and token sequence");
            } else {
                VDXRFG_TicketV1 pair{};
                success(api.Submit(context, &info, &pair), "retain second validation bank");
                gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
                info.serial = 3;
                info.display_time_ns += 1000000000;
                for (auto& target : info.target_time_ns)
                    target += 1000000000;
                info.producer_fence = foreignFence.Get();
                info.producer_value = 1;
                VDXRFG_TicketV1 rejected{};
                const HRESULT status = api.Submit(context, &info, &rejected);
                VDXRFG_StatisticsV1 statistics{};
                statistics.struct_size = sizeof(statistics);
                statistics.api_version = VDXRFG_API_VERSION;
                success(api.GetStatistics(context, &statistics), "read rejection counters");
                completed(pair);
                completed(prime);
                success(api.Destroy(context), "destroy busy validation context");
                check(status == VDXRFG_E_REJECTED && !rejected.token && !statistics.busy_rejections,
                      "foreign fence rejects before busy admission and leaves counters unchanged");
            }
            if (scenario == "fractions")
                success(api.Destroy(context), "destroy fraction validation context");
        }
        gpu.noErrors();
        FreeLibrary(module);
        std::printf("PASS: submission validation %s preserves ownership and history\n", scenario.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
