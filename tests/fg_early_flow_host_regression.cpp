// Actual FrameGeneration + real NVIDIA module boundary. No SDK/headset claim.
#define main retainedEarlyFlowInteropMain
#include "nr_fg_integration_regression.cpp"
#undef main
#include <nvOpticalFlowD3D12.h>

namespace {
    using Submit = decltype(VDXRFG_ApiV3{}.Submit);
    using Commit = decltype(VDXRFG_ApiV6{}.CommitPrepared);
    Submit originalSubmit{};
    Commit originalCommit{};
    VDXRFG_ApiV6 api6{};
    unsigned ordinaryCalls{}, commitCalls{}, busyDiscards{};
    bool scriptedBusy{};
    HRESULT WINAPI observeSubmit(void* context, const VDXRFG_SubmitInfoV3* input, VDXRFG_TicketV1* out) noexcept {
        ++ordinaryCalls;
        return originalSubmit(context, input, out);
    }
    HRESULT WINAPI observeCommit(void* context,
                                 const VDXRFG_PreparedV6* receipt,
                                 uint64_t epoch,
                                 const VDXRFG_SubmitInfoV5* input,
                                 VDXRFG_TicketV1* out) noexcept {
        ++commitCalls;
        if (scriptedBusy) {
            // Explicit C API contract fault case, not natural output-bank BUSY:
            // consume the real queued preparation using its real exact Cancel.
            *out = {};
            const auto discarded = api6.Cancel(context, receipt);
            if (discarded != S_OK)
                return discarded;
            ++busyDiscards;
            return VDXRFG_E_BUSY;
        }
        return originalCommit(context, receipt, epoch, input, out);
    }
    void dispatchHooks(HMODULE module, bool install) {
        if (install) {
            VDXRFG_ApiV4 api4{sizeof(api4), 4};
            auto get4 = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
            auto get6 = reinterpret_cast<VDXRFG_GetApiV6Fn>(GetProcAddress(module, "VDXRFG_GetApiV6"));
            api6 = {sizeof(api6), 6};
            check(get4 && get6, "real module V4/V6 tables");
            success(get4(&api4), "get actual ordinary dispatch");
            success(get6(&api6), "get actual prepared dispatch");
            originalSubmit = api4.Submit;
            originalCommit = api6.CommitPrepared;
        }
        check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
              "begin actual dispatch hooks");
        const auto op = install ? DetourAttach : DetourDetach;
        check(op(reinterpret_cast<PVOID*>(&originalSubmit), reinterpret_cast<PVOID>(observeSubmit)) == NO_ERROR &&
                  op(reinterpret_cast<PVOID*>(&originalCommit), reinterpret_cast<PVOID>(observeCommit)) == NO_ERROR &&
                  DetourTransactionCommit() == NO_ERROR,
              "apply actual dispatch hooks");
    }
    PFNNVOFEXECUTED3D12 originalExecute{};
    unsigned nvCalls{};
    ComPtr<ID3D12Fence> failedOutput;
    uint64_t failedValue{};
    NV_OF_STATUS NVOFAPI failSecondEye(NvOFHandle handle,
                                       const NV_OF_EXECUTE_INPUT_PARAMS_D3D12* input,
                                       NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12* output) {
        const auto status = originalExecute(handle, input, output);
        if (++nvCalls == 2 && status == NV_OF_SUCCESS) {
            failedOutput = output->fencePoint->fence;
            failedValue = output->fencePoint->value;
            return NV_OF_ERR_GENERIC;
        }
        return status;
    }
    void failureHook(bool install) {
        if (install) {
            auto driver = GetModuleHandleW(L"nvofapi64.dll");
            auto get = reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(
                GetProcAddress(driver, "NvOFAPICreateInstanceD3D12"));
            NV_OF_D3D12_API_FUNCTION_LIST table{};
            check(get && get(NV_OF_API_VERSION, &table) == NV_OF_SUCCESS, "actual NVIDIA execution table");
            originalExecute = table.nvOFExecuteD3D12;
        }
        check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                  (install ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&originalExecute),
                                                          reinterpret_cast<PVOID>(failSecondEye)) == NO_ERROR &&
                  DetourTransactionCommit() == NO_ERROR,
              "apply actual second-eye failure hook");
    }

    FinalProjectionInput inputFor(Interop& hw, unsigned serial) {
        FinalProjectionInput input;
        input.serial = input.contentEpoch = serial;
        input.period = 10000000;
        input.time = 1000000000 + int64_t(serial - 1) * 3 * input.period;
        FrameGenerationCadence cadence;
        input.slots = cadence.plan(
            cadence.reserve(
                input.time - 3 * input.period, input.time - 2 * input.period, input.period, 3, (serial - 1) * 3),
            serial == 1 ? 0 : input.time - 3 * input.period,
            serial != 1);
        const auto pixels = scene((serial - 1) * 4);
        input.colour[0] = input.colour[1] = hw.texture(&pixels);
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& view = input.views[eye];
            view.rect = {eye * eyeWidth, 0, eyeWidth, height};
            view.orientation[3] = 1;
            view.position[0] = eye ? .032f : -.032f;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
        }
        return input;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "actual candidate module and mismatch|zero-epoch|busy-contract|prepare-poison|match");
        const std::string scenario = argv[2];
        const auto path = std::filesystem::absolute(argv[1]);
        HMODULE module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load actual module");
        Interop hw;
        FrameGeneration fg;
        FrameGeneration::InitInfo init{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       path,
                                       eyeWidth,
                                       height,
                                       3,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        success(fg.initialize(init), "initialize actual host");
        auto a = inputFor(hw, 1), b = inputFor(hw, 2), c = inputFor(hw, 3);
        OwnedFrameTransaction first, previous, current;
        success(fg.capture(a, first), "prime actual host");
        hw.await(fg, first);
        success(fg.capture(b, previous), "establish actual previous endpoint");
        hw.await(fg, previous);
        hw.retire(fg, first); // Keep B live while analysis C is prepared.
        if (scenario == "prepare-poison") {
            ComPtr<ID3D12Fence> gate;
            ComPtr<ID3D11Fence> gate11;
            success(hw.gpu.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "producer test gate");
            struct ReleaseGate {
                ID3D12Fence* gate;
                ~ReleaseGate() {
                    gate->Signal(1);
                }
            } release{gate.Get()};
            HANDLE handle{};
            success(hw.gpu.device->CreateSharedHandle(gate.Get(), nullptr, GENERIC_ALL, nullptr, &handle),
                    "share test gate");
            const auto imported = hw.device->OpenSharedFence(handle, IID_PPV_ARGS(&gate11));
            CloseHandle(handle);
            success(imported, "import test gate");
            success(hw.context->Wait(gate11.Get(), 1), "hold only new private producer work");
            failureHook(true);
            const auto prepared = fg.prepareFlow(c);
            failureHook(false);
            check(prepared == VDXRFG_E_POISONED && nvCalls == 2 && failedOutput && failedValue &&
                      failedOutput->GetCompletedValue() < failedValue,
                  "real queued second eye is held and reports failure");
            check(fg.ready(previous), "previous verified output survives speculative module failure");
            check(fg.shutdown() == VDXRFG_E_BUSY, "known host producer remains waitable rather than quarantined");
            success(gate->Signal(1), "release real queued failed preparation");
            hw.gpu.wait(failedOutput.Get(), failedValue);
            hw.retire(fg, previous);
            success(fg.shutdown(), "module attests safe destruction after real completion and old retirement");
            failedOutput.Reset();
        } else {
            check(scenario == "mismatch" || scenario == "zero-epoch" || scenario == "busy-contract" ||
                      scenario == "match",
                  "known boundary scenario");
            check(fg.prepareFlow(c) == S_OK, "real preparation accepted");
            if (scenario == "mismatch")
                ++c.contentEpoch;
            if (scenario == "zero-epoch")
                c.contentEpoch = 0;
            scriptedBusy = scenario == "busy-contract";
            dispatchHooks(module, true);
            FrameGeneration::CaptureDiagnostics diagnostics{};
            const auto result = fg.capture(c, current, &diagnostics);
            dispatchHooks(module, false);
            std::printf("BOUNDARY scenario=%s result=%08lx ordinary=%u commit=%u busy_discard=%u prepared=%08lx\n",
                        scenario.c_str(),
                        static_cast<unsigned long>(result),
                        ordinaryCalls,
                        commitCalls,
                        busyDiscards,
                        static_cast<unsigned long>(diagnostics.preparedResult));
            if (scriptedBusy) {
                check(result == VDXRFG_E_BUSY && !current.ticket.token && ordinaryCalls == 0 && commitCalls == 1 &&
                          busyDiscards == 1 && fg.initialized(),
                      "consumed scripted BUSY preserves healthy host and owns no ticket");
                hw.read(c.colour[0].Get()); // Drain the real host copy before a subsequent ordinary capture.
                c.contentEpoch = 0;
                success(fg.capture(c, current), "ordinary fallback remains available after consumed BUSY");
            } else {
                check(result == S_OK && current.ticket.token,
                      "prepared match or clean mismatch returns a real output transaction");
                const bool match = scenario == "match";
                check(ordinaryCalls == (match ? 0u : 1u) && commitCalls == (scenario == "zero-epoch" ? 0u : 1u) &&
                          diagnostics.preparedUsed == match,
                      "actual dispatch performs exactly one required ordinary fallback and never double consumes");
            }
            hw.await(fg, current);
            hw.retire(fg, previous);
            hw.retire(fg, current);
            success(fg.shutdown(), "settled actual host shutdown");
        }
        hw.gpu.noErrors();
        FreeLibrary(module);
        std::printf("PASS: actual host early-flow boundary %s\n", scenario.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
