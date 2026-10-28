#define main existingInteropMain
#include "nr_fg_integration_regression.cpp"
#undef main

#include "fg_early_ready_test_gate.h"

void earlyHost(const char* path, unsigned multiplier, bool fidelityFx) {
    const bool split = multiplier == 4 || (fidelityFx && multiplier == 3);
    Interop hw;
    EarlyOutputGate gate(hw.gpu.device.Get());
    FrameGeneration fg;
    FrameGeneration::InitInfo init{hw.device.Get(),
                                   hw.context.Get(),
                                   hw.gpu.device.Get(),
                                   std::filesystem::absolute(path),
                                   eyeWidth,
                                   height,
                                   multiplier,
                                   DXGI_FORMAT_R8G8B8A8_UNORM};
    init.useFidelityFxFull = fidelityFx;
    success(fg.initialize(init), "initialize early-ready interop host");
    FrameGenerationCadence cadence;
    FinalProjectionInput input;
    input.serial = 1;
    input.time = 1000000000;
    input.period = 8333333;
    const auto a = scene(0), b = scene(24);
    input.colour[0] = input.colour[1] = hw.texture(&a);
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& view = input.views[eye];
        view.rect = {eye * eyeWidth, 0, eyeWidth, height};
        view.orientation[3] = 1;
        view.position[0] = eye ? .032f : -.032f;
        view.fov[0] = view.fov[3] = -.8f;
        view.fov[1] = view.fov[2] = .8f;
    }
    input.slots = cadence.plan(
        cadence.reserve(0, input.time - (multiplier - 1) * input.period, input.period, multiplier, 0), 0, false);
    OwnedFrameTransaction prime;
    success(fg.capture(input, prime), "capture early host prime");
    hw.await(fg, prime);
    gate.arm(prime.ticket);
    hw.retire(fg, prime);
    const auto previous = input.time;
    input.time += multiplier * input.period;
    input.serial = 2;
    input.colour[0] = input.colour[1] = hw.texture(&b);
    input.slots = cadence.plan(
        cadence.reserve(previous, previous + input.period, input.period, multiplier, multiplier), previous, true);
    gate.holdFirst = split;
    OwnedFrameTransaction pair;
    success(fg.capture(input, pair), "capture early host pair");
    check(bool(gate.first) == split, "host negotiates early readiness without replacing joint composition");
    HANDLE firstEvent{}, fullEvent{};
    if (split) {
        check(fg.prepareOutputReadyWait(pair, firstEvent, FrameGenerationRole::synthetic1) == VDXRFG_E_BUSY &&
                  firstEvent,
              "register first readiness before its signal");
        check(fg.prepareOutputReadyWait(pair, fullEvent) == VDXRFG_E_BUSY && fullEvent,
              "register full readiness with the same numeric token on another fence");
        gate.releaseFirst();
        check(WaitForSingleObject(firstEvent, 500) == WAIT_OBJECT_0, "first signal wakes its registered event");
        check(fg.prepareOutputReadyWait(pair, fullEvent) == VDXRFG_E_BUSY,
              "first wake cannot manufacture full completion");
    }
    const auto outputB = hw.texture(), outputS1 = hw.texture();
    success(fg.copyOutput(pair, FrameGenerationRole::real, outputB.Get()), "protect real fallback independently");
    check(hw.read(outputB.Get()) == b, "real fallback is independent of pending synthesis tail");
    auto forged = pair;
    forged.ticket.output_ready_fence = nullptr;
    forged.ticket.output_ready_value = 0;
    forged.ticket.role_mask = 0;
    success(fg.copyOutput(forged, FrameGenerationRole::synthetic1, outputS1.Get()),
            "S1 uses stored authoritative readiness");
    success(fg.waitForCopy(pair, FrameGenerationRole::synthetic1, 500), "S1 consumer completes before tail release");
    const auto earlyPixels = hw.read(outputS1.Get());
    check(earlyPixels != a && earlyPixels != b, "copied S1 contains generated pixels");
    if (split) {
        check(!fg.ready(pair), "full ticket remains pending after S1 copy");
        check(fg.retire(pair) == VDXRFG_E_BUSY && fg.reset(0) == VDXRFG_E_BUSY && fg.shutdown() == VDXRFG_E_BUSY,
              "early host copy cannot release whole ownership");
    }
    ComPtr<ID3D11Texture2D> outputLast;
    if (multiplier > 2) {
        const auto last = multiplier == 4 ? FrameGenerationRole::synthetic3 : FrameGenerationRole::synthetic2;
        outputLast = hw.texture();
        success(fg.copyOutput(pair, last, outputLast.Get()), "tail role retains its full wait after early S1");
        if (split)
            check(fg.waitForCopy(pair, last, 50) == VDXRFG_E_BUSY,
                  "an early S1 wait cannot suppress the pending tail's own full wait");
    }
    gate.release();
    if (split)
        check(WaitForSingleObject(fullEvent, 500) == WAIT_OBJECT_0,
              "full fence has its own registration despite the equal first/full values");
    hw.await(fg, pair);
    if (multiplier > 2) {
        const auto last = multiplier == 4 ? FrameGenerationRole::synthetic3 : FrameGenerationRole::synthetic2;
        success(fg.waitForCopy(pair, last, 500), "tail consumer completes after full signal");
        check(hw.read(outputLast.Get()) != earlyPixels, "tail role is a distinct temporal output");
    }
    hw.retire(fg, pair);
    success(fg.shutdown(), "shutdown after all early/tail consumers");
    hw.gpu.noErrors();
    std::printf("PASS: actual interop %ux %s S1 copy precedes tail and full ownership is retained\n",
                multiplier,
                fidelityFx ? "FidelityFX" : "NVIDIA");
}

int main(int argc, char** argv) {
    try {
        check(argc == 4, "module, multiplier and backend required");
        const auto multiplier = static_cast<unsigned>(std::stoul(argv[2]));
        check(multiplier >= 2 && multiplier <= 4, "supported multiplier");
        const std::string backend(argv[3]);
        check(backend == "nvidia" || backend == "fidelityfx-full", "supported backend");
        earlyHost(argv[1], multiplier, backend == "fidelityfx-full");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
