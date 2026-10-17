// Actual vendor GPU output is parked before capture. Test fence-driven host wakeup ownership.
#define VDXR_FG_EVENT_HELPERS_ONLY
#include "nr_fg_event_regression.cpp"
#undef VDXR_FG_EVENT_HELPERS_ONLY
#include <type_traits>

template <typename T, typename = void>
struct HasReadyWait : std::false_type {};
template <typename T>
struct HasReadyWait<T,
                    std::void_t<decltype(std::declval<T&>().prepareOutputReadyWait(
                        std::declval<const OwnedFrameTransaction&>(), std::declval<HANDLE&>()))>> : std::true_type {};
template <typename T>
HRESULT prepareReady(T& fg, const OwnedFrameTransaction& tx, HANDLE& event) {
    if constexpr (HasReadyWait<T>::value) {
        return fg.prepareOutputReadyWait(tx, event);
    } else {
        // Retained baseline can only poll; this makes the missing wakeup fail
        // against a real pending GPU fence rather than failing to compile.
        event = nullptr;
        return fg.ready(tx) ? S_OK : VDXRFG_E_BUSY;
    }
}

int main(int argc, char** argv) {
    try {
        check(argc == 3,
              "Usage: nr_fg_ready_regression <module> timeout|event-failure|stale-wake|removed|invalid|next");
        const std::string scenario = argv[2];
        check(scenario == "timeout" || scenario == "event-failure" || scenario == "stale-wake" ||
                  scenario == "removed" || scenario == "invalid" || scenario == "next",
              "known ready scenario");
        Interop hw;
        FrameGeneration fg;
        FrameGeneration::InitInfo init{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       std::filesystem::absolute(argv[1]),
                                       eyeWidth,
                                       height,
                                       3,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        success(fg.initialize(init), "ready graph");
        ComPtr<ID3D11Fence> gate;
        success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "ready GPU gate");
        HANDLE shared{};
        success(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared), "share ready GPU gate");
        ComPtr<ID3D12Fence> release;
        const auto imported = hw.gpu.device->OpenSharedHandle(shared, IID_PPV_ARGS(&release));
        CloseHandle(shared);
        success(imported, "import ready GPU gate");
        struct Release {
            ID3D12Fence* fence;
            ~Release() {
                (void)fence->Signal(2);
            }
        } releaseOnError{release.Get()};
        FinalProjectionInput input;
        FrameGenerationCadence cadence;
        input.serial = 1;
        input.time = 1000000000;
        input.period = 10000000;
        input.slots =
            cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 0), 0, false);
        const auto pixels = scene(0);
        input.colour[0] = input.colour[1] = hw.texture(&pixels);
        for (unsigned eye = 0; eye < 2; ++eye) {
            input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
            input.views[eye].orientation[3] = 1;
            input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
            input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
        }
        success(hw.context->Wait(gate.Get(), 1), "park producer before output can finish");
        OwnedFrameTransaction tx;
        success(fg.capture(input, tx), "capture pending actual GPU output");
        check(!fg.ready(tx), "GPU gate proves output is pending");
        HANDLE event{};
        {
            EventFaults hooks(gate.Get(), scenario);
            armed = true;
            const auto first = prepareReady(fg, tx, event);
            if (scenario == "event-failure") {
                check(first == E_OUTOFMEMORY && registrations == 1, "registration failure propagates and is observed");
                fault = "timeout";
                armed = true;
                check(prepareReady(fg, tx, event) == VDXRFG_E_BUSY && registrations == 2,
                      "failed registration can retry without a poisoned ticket");
            } else {
                check(first == VDXRFG_E_BUSY && event, "Pending GPU output must expose a fence-driven wakeup");
            }
            const auto registered = registrations;
            check(event != nullptr, "pending output borrows a live cached event");
            const auto waited = waitEvent(event, 0);
            if (scenario == "removed") {
                check(prepareReady(fg, tx, event) == VDXRFG_E_POISONED,
                      "authoritative post-wake removal poisons instead of declaring readiness");
            } else {
                check(waited == (scenario == "stale-wake" ? WAIT_OBJECT_0 : WAIT_TIMEOUT),
                      "real wait or injected stale wake");
                check(prepareReady(fg, tx, event) == VDXRFG_E_BUSY && registrations == registered,
                      "timeout or stale wake keeps pending registration without duplicate kernel entries");
                if (scenario == "invalid") {
                    auto forged = tx;
                    forged.ticket.bank = 2;
                    check(prepareReady(fg, forged, event) == VDXRFG_E_REJECTED, "invalid bank rejected");
                    forged = tx;
                    ++forged.ticket.token;
                    check(prepareReady(fg, forged, event) == VDXRFG_E_REJECTED,
                          "wrong nonzero live-bank token rejected");
                    forged = tx;
                    ++forged.ownerGeneration;
                    check(prepareReady(fg, forged, event) == VDXRFG_E_REJECTED, "wrong owner rejected");
                    forged = tx;
                    forged.ticket.output_ready_value = 0;
                    forged.ticket.output_ready_fence = reinterpret_cast<void*>(1);
                    check(prepareReady(fg, forged, event) == VDXRFG_E_BUSY,
                          "caller cannot forge readiness by altering fence or target");
                }
            }
            armed = false;
        }
        success(release->Signal(1), "release real pending GPU work");
        if (scenario == "removed") {
            check(fg.shutdown() == VDXRFG_E_POISONED, "removed output retains complete graph");
        } else {
            if (scenario != "next") {
                check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "GPU completion wakes its cached event");
            }
            hw.await(fg, tx);
            check(prepareReady(fg, tx, event) == S_OK, "only actual fence completion proves ready");
            auto output = hw.texture();
            success(fg.copyOutput(tx, FrameGenerationRole::real, output.Get()), "copy exact real frame after wake");
            check(hw.read(output.Get()) == pixels, "ready path preserves exact real pixels");
            auto retired = tx;
            hw.retire(fg, tx);
            check(prepareReady(fg, retired, event) == VDXRFG_E_REJECTED, "retired token rejected");
            if (scenario == "next") {
                input.serial = 2;
                input.time += 30000000;
                input.slots =
                    cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 3),
                                 input.time - 30000000,
                                 true);
                success(hw.context->Wait(gate.Get(), 2), "park second GPU output");
                success(fg.capture(input, tx), "capture second pending ticket");
                check(prepareReady(fg, retired, event) == VDXRFG_E_REJECTED,
                      "retired token rejected while another ticket is live");
                check(prepareReady(fg, tx, event) == VDXRFG_E_BUSY && event, "new ticket owns a new target");
                check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
                      "prior unconsumed event notification remains live");
                check(prepareReady(fg, tx, event) == VDXRFG_E_BUSY, "stale old ticket wake cannot complete new target");
                success(release->Signal(2), "release second GPU output");
                check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "second target gets its genuine wake");
                check(prepareReady(fg, tx, event) == S_OK, "second target proves completion");
                success(fg.copyOutput(tx, FrameGenerationRole::real, output.Get()), "copy second frame");
                hw.read(output.Get());
                hw.retire(fg, tx);
            }
            success(fg.shutdown(), "ready registrations settle before release");
            success(fg.initialize(init), "new owner generation");
            input.serial = 1;
            input.time += 30000000;
            FrameGenerationCadence freshCadence;
            input.slots = freshCadence.plan(
                freshCadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 0), 0, false);
            success(fg.capture(input, tx), "fresh live ticket after generation restart");
            check(tx.ticket.bank == retired.ticket.bank && tx.ticket.token == retired.ticket.token,
                  "cross-generation rejection isolates a reused live bank/token");
            check(prepareReady(fg, retired, event) == VDXRFG_E_REJECTED,
                  "previous graph ticket rejected after reinitialize");
            hw.await(fg, tx);
            hw.retire(fg, tx);
            success(fg.shutdown(), "release fresh graph");
        }
        hw.gpu.noErrors();
        std::printf("PASS: actual GPU output-ready event ownership; mode=%s\n", scenario.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
