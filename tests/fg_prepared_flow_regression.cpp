#include "fg_gpu_helpers.h"
#include <algorithm>
#include <detours.h>
#include <nvOpticalFlowD3D12.h>

using Images = std::vector<std::vector<uint8_t>>;

// Forward both real submissions unchanged but report a failure on the second.
// The deliberately held producer prevents a successful retirement by accident.
static PFNNVOFEXECUTED3D12 originalExecute{};
static unsigned observedCalls{};
static ComPtr<ID3D12Fence> observedOutput;
static uint64_t observedValue{};
static NV_OF_STATUS NVOFAPI failSecondEye(NvOFHandle handle,
                                          const NV_OF_EXECUTE_INPUT_PARAMS_D3D12* input,
                                          NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12* output) {
    const auto status = originalExecute(handle, input, output);
    if (++observedCalls == 2 && status == NV_OF_SUCCESS) {
        observedOutput = output->fencePoint->fence;
        observedValue = output->fencePoint->value;
        return NV_OF_ERR_GENERIC;
    }
    return status;
}

static void faultHook(bool install) {
    if (install) {
        auto driver = GetModuleHandleW(L"nvofapi64.dll");
        auto getter = reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(
            GetProcAddress(driver, "NvOFAPICreateInstanceD3D12"));
        NV_OF_D3D12_API_FUNCTION_LIST table{};
        check(getter && getter(NV_OF_API_VERSION, &table) == NV_OF_SUCCESS, "real NVIDIA API table");
        originalExecute = table.nvOFExecuteD3D12;
    }
    check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
              (install ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&originalExecute),
                                                      reinterpret_cast<PVOID>(failSecondEye)) == NO_ERROR &&
              DetourTransactionCommit() == NO_ERROR,
          "install or remove test-only NV failure");
}

Images exercise(Gpu& gpu, HMODULE module, unsigned multiplier, bool staged, bool fault = false) {
    auto get4 = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
    auto get6 = reinterpret_cast<VDXRFG_GetApiV6Fn>(GetProcAddress(module, "VDXRFG_GetApiV6"));
    check(get4 && (!staged || get6), "required API exports");
    VDXRFG_ApiV4 base{};
    base.struct_size = sizeof(base);
    base.api_version = 4;
    success(get4(&base), "get V4");
    VDXRFG_ApiV6 api{};
    if (staged) {
        api.struct_size = sizeof(api);
        api.api_version = 6;
        success(get6(&api), "get V6");
    }
    auto input = gpu.texture(), snapshot = gpu.texture();
    std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> images;
    VDXRFG_CreateInfoV3 create{};
    create.struct_size = sizeof(create);
    create.api_version = 3;
    create.device = gpu.device.Get();
    create.queue = gpu.queue.Get();
    create.input = input.Get();
    create.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    create.width = width;
    create.height = height;
    create.multiplier = multiplier;
    create.flags = VDXRFG_CREATE_GPU_TIMING;
    for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned role = 0; role < 4; ++role) {
            if ((role == 1 && multiplier == 2) || (role == 3 && multiplier != 4))
                continue;
            images[bank][role] = gpu.texture();
            create.outputs[bank][role] = images[bank][role].Get();
        }
    }
    void* context{};
    success(base.Create(&create, &context), "create V4");
    VDXRFG_SubmitInfoV5 final{};
    final.struct_size = sizeof(final);
    final.api_version = 5;
    auto& info = final.base;
    info.struct_size = sizeof(info);
    info.api_version = 3;
    info.input = input.Get();
    info.display_period_ns = 10000000;
    std::vector<VDXRFG_TicketV1> live;
    Images result;
    ComPtr<ID3D12Fence> producer;
    success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer)), "private producer");
    for (unsigned frame = 0; frame < 10; ++frame) {
        const auto pixels = scene(frame * 4);
        gpu.transfer(input.Get(), &pixels);
        if (staged)
            gpu.transfer(snapshot.Get(), &pixels);
        info.serial = 101 + frame;
        info.display_time_ns = 1000000000 + frame * multiplier * info.display_period_ns;
        for (unsigned i = 0; i < multiplier - 1; ++i) {
            info.target_time_ns[i] = info.display_time_ns - (multiplier - 1 - i) * info.display_period_ns;
            info.fractions[i] = static_cast<float>(i + 1) / multiplier;
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& view = info.views[eye];
            view = {};
            view.orientation[3] = 1;
            view.position[0] = eye ? .033f : -.033f;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
            view.rect = {eye * eyeWidth, 0, eyeWidth, height};
        }
        VDXRFG_PreparedV6 receipt{sizeof(receipt), 6};
        const uint64_t epoch = 200 + frame;
        const bool prepare = staged && frame >= 2;
        if (prepare) {
            VDXRFG_PrepareInfoV6 p{};
            p.struct_size = sizeof(p);
            p.api_version = 6;
            p.previous_serial = info.serial - 1;
            p.current_serial = info.serial;
            p.content_epoch = epoch;
            p.producer_fence = producer.Get();
            p.producer_value = frame;
            p.current_atlas = snapshot.Get();
            std::copy_n(info.views, 2, p.views);
            // Normal synthesis requires the final source layout to equal A's.
            // Change the provisional one-eye crop and restore it at finalization.
            if (frame == 5)
                p.views[1].rect.height -= 4;
            // The last cancellation exercises logically dropping pending GPU reads.
            if (frame != 8 && !fault)
                success(producer->Signal(frame), "publish private source");
            check(live.size() == 2, "both previous external banks held");
            if (fault) {
                faultHook(true);
                const auto hr = api.Prepare(context, &p, &receipt);
                faultHook(false);
                check(hr == VDXRFG_E_POISONED && receipt.token && receipt.input_read_fence && observedCalls == 2 &&
                          observedOutput && observedValue && observedOutput->GetCompletedValue() < observedValue,
                      "failed second real eye retained");
                VDXRFG_TicketV1 rejected{};
                check(api.Cancel(context, &receipt) == VDXRFG_E_POISONED &&
                          api.CommitPrepared(context, &receipt, epoch, &final, &rejected) == VDXRFG_E_POISONED &&
                          base.Submit(context, &info, &rejected) == VDXRFG_E_POISONED && !rejected.token &&
                          base.Reset(context, 0) == VDXRFG_E_POISONED && base.Destroy(context) == VDXRFG_E_POISONED,
                      "quarantine survives retries before real GPU completion");
                // Prepare's failure owns no ordinary output. The earlier pair's actual
                // per-role fence identities and values must remain available for presentation.
                for (const auto& prior : live) {
                    for (unsigned role = 0; role < 4; ++role) {
                        VDXRFG_OutputReadinessV4 ready{sizeof(ready), 4};
                        const auto readiness = base.GetOutputReadiness(context, prior.token, role, &ready);
                        if (!(prior.role_mask & (1u << role))) {
                            check(readiness == VDXRFG_E_REJECTED && !ready.fence && !ready.value,
                                  "unowned role rejected");
                            continue;
                        }
                        check(readiness == S_OK && ready.fence && ready.value,
                              "earlier leased output remains queryable");
                        auto* fence = static_cast<ID3D12Fence*>(ready.fence);
                        const auto complete = fence->GetCompletedValue();
                        check(complete != UINT64_MAX && complete >= ready.value, "earlier output physically complete");
                        if (role != 0)
                            check(fence == prior.output_ready_fence && ready.value == prior.output_ready_value,
                                  "ordinary output retains original full fence");
                    }
                }
                VDXRFG_OutputReadinessV4 invalid{sizeof(invalid), 4};
                check(base.GetOutputReadiness(context, UINT64_MAX, 0, &invalid) == VDXRFG_E_REJECTED &&
                          !invalid.fence && !invalid.value,
                      "unowned ticket rejected during quarantine");
                success(producer->Signal(frame), "release failed GPU job");
                gpu.wait(observedOutput.Get(), observedValue);
                check(base.Destroy(context) == VDXRFG_E_BUSY, "old normal output owners survive prepare failure");
                for (const auto& t : live)
                    success(base.Retire(context, t.token), "retire surviving normal ticket");
                success(base.Destroy(context), "destroy after both real GPU signals and external retirement");
                observedOutput.Reset();
                gpu.noErrors();
                return {};
            }
            check(api.Prepare(context, &p, &receipt) == S_OK && receipt.token && receipt.input_read_fence &&
                      receipt.input_read_value,
                  "prepare while both banks owned");
            auto wrong = receipt;
            ++wrong.context_id;
            check(api.Cancel(context, &wrong) == VDXRFG_E_REJECTED, "foreign receipt leaves owned job active");
            auto duplicate = VDXRFG_PreparedV6{sizeof(receipt), 6};
            check(api.Prepare(context, &p, &duplicate) == S_FALSE && !duplicate.token, "one preparable job");
            VDXRFG_TicketV1 empty{};
            if (frame == 2) {
                auto malformed = final;
                malformed.base.display_time_ns = -1;
                check(api.CommitPrepared(context, &receipt, epoch + 1, &malformed, &empty) == VDXRFG_E_REJECTED &&
                          !empty.token,
                      "structural reject leaves the authentic receipt active");
            }
            if (frame == 9) {
                check(api.CommitPrepared(context, &receipt, epoch, &final, &empty) == VDXRFG_E_BUSY && !empty.token &&
                          api.Cancel(context, &receipt) == VDXRFG_E_REJECTED,
                      "ordinary two-bank BUSY consumes preparation");
            }
        }
        if (live.size() == 2) {
            success(base.Retire(context, live.front().token), "release older bank for final submit");
            live.erase(live.begin());
        }
        // Position is excluded from Standard packing. Each other late mutation
        // below changes exactly one effective key and must run the inline fallback.
        info.views[1].position[1] = .03f;
        if (frame == 3) {
            info.views[1].orientation[1] = std::sin(.04f);
            info.views[1].orientation[3] = std::cos(.04f);
        }
        if (frame == 4)
            info.views[1].fov[2] += .06f;
        VDXRFG_TicketV1 ticket{};
        bool inlineSubmit = true;
        if (prepare && frame < 7) {
            const auto hr = api.CommitPrepared(context, &receipt, frame == 6 ? epoch + 1 : epoch, &final, &ticket);
            if (frame == 2) {
                check(hr == S_OK, "translation-only change reuses prepared analysis");
                inlineSubmit = false;
            } else
                check(hr == S_FALSE && !ticket.token, "mismatching preparation has no output ownership");
        } else if (prepare && frame == 8) {
            check(api.Cancel(context, &receipt) == S_OK, "cancel before input producer completes");
        }
        if (inlineSubmit) {
            const HRESULT hr = base.Submit(context, &info, &ticket);
            if (hr != S_OK)
                std::fprintf(stderr, "ordinary submit frame %u staged %u HRESULT %08lx\n", frame, staged, hr);
            check(hr == S_OK, "ordinary submit without extra BUSY");
        }
        if (prepare && frame == 8)
            success(producer->Signal(frame), "release delayed producer after inline submit");
        if (prepare)
            check(api.Cancel(context, &receipt) == VDXRFG_E_REJECTED, "already consumed receipt");
        check(ticket.token && ticket.current_serial == info.serial && ticket.prime == (frame == 0),
              "authoritative ordinary output ticket");
        gpu.wait(static_cast<ID3D12Fence*>(ticket.output_ready_fence), ticket.output_ready_value);
        for (unsigned role = 0; role < 4; ++role) {
            if (!(ticket.role_mask & (1u << role)))
                continue;
            result.emplace_back(gpu.transfer(images[ticket.bank][role].Get(), nullptr));
            if (role == 2)
                check(result.back() == pixels, "current real pixels are exact");
        }
        live.push_back(ticket);
    }
    for (const auto& t : live)
        success(base.Retire(context, t.token), "retire final tickets");
    success(base.Reset(context, 0), "reset preparation context");
    success(base.Destroy(context), "destroy preparation context");
    gpu.noErrors();
    return result;
}

int main(int argc, char** argv) {
    try {
        check(argc == 3 || (argc == 4 && std::string(argv[3]) == "fault"), "module, multiplier and optional fault");
        const auto n = std::stoul(argv[2]);
        check(n >= 2 && n <= 4, "2X/3X/4X multiplier");
        const auto dll =
            LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(dll != nullptr, "load synthesis DLL");
        Gpu gpu;
        if (argc == 4) {
            exercise(gpu, dll, n, true, true);
            FreeLibrary(dll);
            std::puts("PASS poisoned second-eye submission retained until actual completion");
            return 0;
        }
        const auto normal = exercise(gpu, dll, n, false);
        check(exercise(gpu, dll, n, true) == normal, "every synthetic and real output equals full inline control");
        FreeLibrary(dll);
        std::printf("PASS V6 held banks, late pose/FOV/rect/epoch, pending cancellation and exact %luX pixels\n", n);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
