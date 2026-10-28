#include "fg_gpu_helpers.h"

#include "fg_early_ready_test_gate.h"

namespace {
    void run(const char* path, unsigned multiplier, bool fidelityFx, bool failTail) {
        HMODULE module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load synthesis module");
        const auto get = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
        check(get != nullptr, "module must negotiate V4 role readiness");
        VDXRFG_ApiV4 api{};
        api.struct_size = sizeof(api);
        api.api_version = VDXRFG_API_VERSION_4;
        success(get(&api), "negotiate role readiness");
        check(api.GetOutputReadiness && api.max_multiplier >= 4, "complete V4 table");
        Gpu gpu;
        EarlyOutputGate gate(gpu.device.Get());
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
        create.flags = fidelityFx ? VDXRFG_CREATE_FIDELITYFX_FULL : 0;
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
        success(api.Submit(context, &info, &prime), "prime V4 history");
        gpu.wait(static_cast<ID3D12Fence*>(prime.output_ready_fence), prime.output_ready_value);
        const auto query = [&](uint64_t token, unsigned role, VDXRFG_OutputReadinessV4& ready) {
            ready = {sizeof(ready), VDXRFG_API_VERSION_4, nullptr, 0};
            return api.GetOutputReadiness(context, token, role, &ready);
        };
        VDXRFG_OutputReadinessV4 ready{};
        check(query(prime.token, 0, ready) == VDXRFG_E_REJECTED && !ready.fence && !ready.value,
              "prime cannot expose synthetic readiness");
        success(query(prime.token, 2, ready), "prime real readiness");
        check(ready.fence == prime.output_ready_fence && ready.value == prime.output_ready_value,
              "prime retains full completion");
        success(api.Retire(context, prime.token), "retire prime");
        gpu.transfer(input.Get(), &b);
        info.serial = 2;
        info.display_time_ns += multiplier * info.display_period_ns;
        for (unsigned i = 0; i < multiplier - 1; ++i) {
            info.target_time_ns[i] = 1000000000 + (i + 1) * info.display_period_ns;
            info.fractions[i] = static_cast<float>(i + 1) / multiplier;
        }
        {
            gate.arm(prime);
            gate.failFull = failTail;
            VDXRFG_TicketV1 pair{};
            const auto submitted = api.Submit(context, &info, &pair);
            if (failTail) {
                gate.release();
                const auto drained = gpu.nextFence++;
                success(gate.moduleQueue->Signal(gpu.fence.Get(), drained),
                        "drain post-early failure before assertions");
                gpu.wait(gpu.fence.Get(), drained);
                check(gate.first && gate.failedFull && submitted == VDXRFG_E_POISONED && pair.token && !pair.role_mask,
                      "failure after early signal keeps owned poisoned ticket and publishes no output");
                check(query(pair.token, 0, ready) == VDXRFG_E_POISONED && !ready.fence && !ready.value,
                      "poisoned early pixels cannot be published by query");
                check(api.Retire(context, pair.token) == VDXRFG_E_POISONED &&
                          api.Reset(context, 0) == VDXRFG_E_POISONED && api.Destroy(context) == VDXRFG_E_POISONED,
                      "failed whole completion never releases resources based on S1");
                gpu.noErrors();
                // The module/context stay retained intentionally; destruction
                // is unsafe after its final completion signal failed.
                std::puts("PASS: post-S1 signal failure retains poisoned ownership without publishing pixels");
                return;
            }
            success(submitted, "submit first plus tail");
            success(query(pair.token, 0, ready), "query authoritative S1 readiness");
            const bool split = multiplier == 4 || (fidelityFx && multiplier == 3);
            check(bool(gate.first) == split, "split only independent multi-output compositions; preserve 3X MRT");
            if (split) {
                check(ready.fence == gate.first.Get() && ready.value == gate.firstValue,
                      "S1 returns the actual shared signal before tail");
                gpu.wait(static_cast<ID3D12Fence*>(ready.fence), ready.value);
                check(static_cast<ID3D12Fence*>(pair.output_ready_fence)->GetCompletedValue() < pair.output_ready_value,
                      "whole transaction remains incomplete while S1 exists");
                check(api.Retire(context, pair.token) == VDXRFG_E_BUSY, "early S1 cannot retire full ticket");
                check(api.Reset(context, 0) == VDXRFG_E_BUSY, "early S1 cannot reset ownership");
                check(api.Destroy(context) == VDXRFG_E_BUSY, "early S1 cannot destroy context");
                VDXRFG_OutputReadinessV4 tail{};
                success(query(pair.token, multiplier == 4 ? 3 : 1, tail), "query tail readiness");
                check(tail.fence == pair.output_ready_fence && tail.value == pair.output_ready_value,
                      "later outputs keep full completion");
                // Read S1 on an independent consumer, never behind the gated tail.
                const auto producer = gpu.queue;
                D3D12_COMMAND_QUEUE_DESC q{};
                q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                ComPtr<ID3D12CommandQueue> consumer;
                success(gpu.device->CreateCommandQueue(&q, IID_PPV_ARGS(&consumer)), "early pixel consumer");
                success(consumer->Wait(gate.first.Get(), gate.firstValue), "consumer joins early signal");
                gpu.queue = consumer;
                const auto earlyPixels = gpu.transfer(outputs[pair.bank][0].Get(), nullptr);
                gpu.queue = producer;
                check(earlyPixels != a && earlyPixels != b, "early stereo S1 contains generated pixels");
                gate.release();
                gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
                check(gpu.transfer(outputs[pair.bank][0].Get(), nullptr) == earlyPixels,
                      "tail never rewrites completed S1 in either eye");
            } else {
                check(ready.fence == pair.output_ready_fence && ready.value == pair.output_ready_value,
                      "single/joint output retains full ready fence");
                gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
            }
            check(query(pair.token, 4, ready) == VDXRFG_E_REJECTED && !ready.fence && !ready.value,
                  "invalid role clears readiness");
            success(api.Retire(context, pair.token), "retire completed pair");
            check(query(pair.token, 0, ready) == VDXRFG_E_REJECTED, "retired token cannot publish a readiness fence");
            ready = {sizeof(ready), 3, nullptr, 0};
            check(api.GetOutputReadiness(context, pair.token, 0, &ready) == VDXRFG_E_REJECTED,
                  "readiness uses an explicit V4 envelope");
            ready = {sizeof(ready) - 1, VDXRFG_API_VERSION_4, nullptr, 0};
            check(api.GetOutputReadiness(context, pair.token, 0, &ready) == VDXRFG_E_REJECTED,
                  "short readiness envelope is rejected");
            check(api.GetOutputReadiness(context, pair.token, 0, nullptr) == E_POINTER,
                  "null readiness output is rejected");
            check(query(0, 0, ready) == VDXRFG_E_REJECTED && !ready.fence && !ready.value,
                  "zero token cannot publish readiness");
        }
        success(api.Reset(context, 0), "reset after full retirement");
        success(api.Destroy(context), "destroy completed context");
        FreeLibrary(module);
        gpu.noErrors();
        std::printf("PASS: V4 %ux %s early S1 pixels, tail gate, full lifetime and legacy composition\n",
                    multiplier,
                    fidelityFx ? "FidelityFX" : "NVIDIA");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 4 || argc == 5, "module, multiplier, backend and optional post-S1 failure required");
        const unsigned multiplier = static_cast<unsigned>(std::stoul(argv[2]));
        check(multiplier >= 2 && multiplier <= 4, "supported multiplier");
        const std::string backend(argv[3]);
        check(backend == "nvidia" || backend == "fidelityfx-full", "supported backend");
        check(argc != 5 || std::string(argv[4]) == "fail-tail", "known post-S1 failure");
        check(argc != 5 || multiplier == 4, "post-S1 failure fixture uses a separately composed tail");
        run(argv[1], multiplier, backend == "fidelityfx-full", argc == 5);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
