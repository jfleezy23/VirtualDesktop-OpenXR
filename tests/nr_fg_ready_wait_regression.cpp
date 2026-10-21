#define main existingInteropMain
#include "nr_fg_integration_regression.cpp"
#undef main

#include <detours.h>
#include <cstdlib>

// Observe the actual nondebug D3D11 immediate context, not a mock host.
class ReadyWaitProbe {
    using Wait = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
    inline static ReadyWaitProbe* active{};
    Wait original{};
    ID3D11DeviceContext4* context{};
    const DWORD owner = GetCurrentThreadId();
    bool armed{}, failNext{};

    static HRESULT STDMETHODCALLTYPE onWait(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
        auto& probe = *active;
        // Other threads do not inspect or mutate this thread's trace state.
        if (GetCurrentThreadId() == probe.owner && context == probe.context && probe.armed) {
            ++probe.count;
            probe.lastFence = fence;
            probe.lastValue = value;
            if (probe.failNext) {
                probe.failNext = false;
                return E_FAIL;
            }
        }
        return probe.original(context, fence, value);
    }
    void attach(bool enable) {
        LONG error = DetourTransactionBegin();
        const bool started = error == NO_ERROR;
        if (!error)
            error = DetourUpdateThread(GetCurrentThread());
        if (!error)
            error = (enable ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&original), onWait);
        if (!error)
            error = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(error), "D3D11 ready-wait observer transaction");
    }

  public:
    unsigned count{};
    ID3D11Fence* lastFence{};
    uint64_t lastValue{};

    explicit ReadyWaitProbe(ID3D11DeviceContext4* observed) : context(observed) {
        check(!active, "one D3D11 ready-wait observer");
        // ID3D11DeviceContext4::Wait follows Signal in SDK's 149-slot vtable.
        original = reinterpret_cast<Wait>((*reinterpret_cast<void***>(context))[148]);
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }
    ~ReadyWaitProbe() {
        armed = false;
        try {
            attach(false);
        } catch (...) {
            std::_Exit(2);
        }
        active = nullptr;
    }
    void begin(bool failFirst = false) {
        count = 0;
        lastFence = nullptr;
        lastValue = 0;
        failNext = failFirst;
        armed = true;
    }
    void end() {
        armed = false;
    }
};

struct ReadyWaitFixture {
    Interop hw;
    FrameGeneration fg;
    FrameGeneration::InitInfo init;
    FrameGenerationCadence cadence;
    uint64_t serial{};
    int64_t time{1000000000};
    const std::vector<uint8_t> a = scene(0), b = scene(24);

    ReadyWaitFixture(const char* path, uint32_t multiplier)
        : init{hw.device.Get(),
               hw.context.Get(),
               hw.gpu.device.Get(),
               std::filesystem::absolute(path),
               eyeWidth,
               height,
               multiplier,
               DXGI_FORMAT_R8G8B8A8_UNORM} {
        const char* backend = std::getenv("VDXR_FG_TEST_BACKEND");
        init.useFidelityFxFull = backend && std::string(backend) == "fidelityfx-full";
        success(fg.initialize(init), "initialize native ready-wait fixture");
    }
    OwnedFrameTransaction capture(bool moving) {
        FinalProjectionInput input;
        input.serial = ++serial;
        input.time = time;
        input.period = 10000000;
        input.slots = cadence.plan(cadence.reserve(time - init.multiplier * input.period,
                                                   time - (init.multiplier - 1) * input.period,
                                                   input.period,
                                                   init.multiplier,
                                                   serial * init.multiplier),
                                   moving ? time - init.multiplier * input.period : 0,
                                   moving);
        input.colour[0] = input.colour[1] = hw.texture((serial & 1) ? &a : &b);
        for (UINT eye = 0; eye < 2; ++eye) {
            input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
            input.views[eye].orientation[3] = 1;
            input.views[eye].position[0] = eye ? .032f : -.032f;
            input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
            input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
        }
        OwnedFrameTransaction tx;
        success(fg.capture(input, tx), "capture independent native transaction");
        hw.await(fg, tx);
        time += init.multiplier * input.period;
        return tx;
    }
    void prime(ReadyWaitProbe& probe) {
        auto tx = capture(false);
        auto output = hw.texture();
        probe.begin();
        success(fg.copyOutput(tx, FrameGenerationRole::real, output.Get()), "prime real B");
        probe.end();
        check(!probe.count, "real prime does not queue a synthetic ready wait");
        check(hw.read(output.Get()) == ((serial & 1) ? a : b), "prime preserves exact native pixels");
        hw.retire(fg, tx);
    }
};

void checkPair(ReadyWaitFixture& f, ReadyWaitProbe& probe, OwnedFrameTransaction& tx, bool reverse) {
    const auto first = reverse ? FrameGenerationRole::synthetic2 : FrameGenerationRole::synthetic1;
    const auto second = reverse ? FrameGenerationRole::synthetic1 : FrameGenerationRole::synthetic2;
    const auto outputB = f.hw.texture(), outputFirst = f.hw.texture(), outputSecond = f.hw.texture();
    const auto alternate = f.hw.texture();
    const uint64_t readyValue = tx.ticket.output_ready_value;
    auto clone = tx;
    clone.ticket.output_ready_fence = nullptr;
    clone.ticket.output_ready_value = 0;
    clone.ticket.role_mask = 0;
    clone.consumerValue = UINT64_MAX;
    clone.destinations[0] = clone.destinations[1] = outputFirst;
    probe.begin();
    success(f.fg.copyOutput(tx, FrameGenerationRole::real, outputB.Get()), "prepare B before synthetic output");
    check(!probe.count, "real B preparation cannot satisfy the synthetic ready wait");
    check(f.fg.copyOutput(clone, first, outputB.Get()) == VDXRFG_E_REJECTED && !probe.count,
          "rejected destination alias neither waits nor marks synthesis prepared");
    check(f.fg.copyOutput(clone, first, nullptr) == VDXRFG_E_REJECTED && !probe.count,
          "rejected null destination neither waits nor marks synthesis prepared");
    success(f.fg.copyOutput(clone, first, outputFirst.Get()), "first synthetic copy through forged caller fields");
    check(probe.count == 1 && probe.lastFence && probe.lastValue == readyValue && readyValue,
          "first synthetic copy queues exactly one authoritative nonzero ready-fence wait");
    const auto firstConsumer = clone.consumerValue;
    clone.consumerValue = UINT64_MAX;
    success(f.fg.copyOutput(clone, first, outputFirst.Get()), "repeat first role reuses prepared destination");
    check(clone.consumerValue == firstConsumer && probe.count == 1,
          "idempotent first role has no extra wait or consumer signal");
    check(f.fg.copyOutput(tx, first, alternate.Get()) == VDXRFG_E_REJECTED && probe.count == 1,
          "duplicate role cannot redirect its prepared copy");
    if (f.init.multiplier == 3) {
        success(f.fg.copyOutput(tx, second, outputSecond.Get()), "second synthetic copy on same ticket");
        check(probe.count == 1, "S1/S2 share one ready wait for their live ticket");
    } else {
        check(f.fg.copyOutput(tx, FrameGenerationRole::synthetic2, outputSecond.Get()) == VDXRFG_E_REJECTED &&
                  probe.count == 1,
              "2x rejects unavailable S2 without adding a ready wait");
    }
    probe.end();
    const auto b = f.hw.read(outputB.Get());
    const auto s1 = f.hw.read(outputFirst.Get());
    check(b == ((tx.ticket.current_serial & 1) ? f.a : f.b), "real B remains byte-exact for this ticket");
    check(s1 != f.a && s1 != f.b, "first synthetic output contains actual generated pixels");
    check(f.fg.copyReady(tx, FrameGenerationRole::real) && f.fg.copyReady(tx, first),
          "actual consumer fences complete for real and first synthetic role");
    if (f.init.multiplier == 3) {
        check(f.hw.read(outputSecond.Get()) != s1, "S1/S2 remain distinct native outputs");
        check(f.fg.copyReady(tx, second), "second synthetic role retains its consumer fence");
    }
    const auto stale = tx;
    f.hw.retire(f.fg, tx);
    auto staleCopy = stale;
    probe.begin();
    check(f.fg.copyOutput(staleCopy, first, alternate.Get()) == VDXRFG_E_REJECTED && !probe.count,
          "retired ticket cannot reuse the ready-wait decision");
    probe.end();
}

void runReadyWait(const char* path, uint32_t multiplier) {
    ReadyWaitFixture f(path, multiplier);
    ReadyWaitProbe probe(f.hw.context.Get());
    f.prime(probe);
    for (unsigned phase = 0; phase < 3; ++phase) {
        // Keep both banks live. A decision from another bank must not suppress
        // this ticket's first wait, even when both output fences are complete.
        auto a = f.capture(true), b = f.capture(true);
        check(a.ticket.bank != b.ticket.bank, "native fixture holds two different banks live");
        checkPair(f, probe, a, multiplier == 3 && phase == 1);
        checkPair(f, probe, b, multiplier == 3 && phase != 1);
        if (phase == 0) {
            success(f.fg.reset(0), "reset re-arms first synthetic wait");
            f.prime(probe);
        } else if (phase == 1) {
            success(f.fg.shutdown(), "shutdown settled ready-wait host");
            success(f.fg.initialize(f.init), "reinitialize re-arms first synthetic wait");
            f.prime(probe);
        }
    }
    success(f.fg.shutdown(), "shutdown final ready-wait host");
    f.hw.gpu.noErrors();
}

void runFailedReadyWait(const char* path) {
    ReadyWaitFixture f(path, 3);
    ReadyWaitProbe probe(f.hw.context.Get());
    f.prime(probe);
    auto tx = f.capture(true);
    auto output = f.hw.texture();
    probe.begin(true);
    check(f.fg.copyOutput(tx, FrameGenerationRole::synthetic1, output.Get()) == VDXRFG_E_POISONED && probe.count == 1,
          "failed first ready wait poisons host before recording a consumer");
    check(!tx.consumerValue && !tx.destinations[0], "failed first wait does not publish a prepared role");
    check(f.fg.copyOutput(tx, FrameGenerationRole::synthetic2, output.Get()) == VDXRFG_E_REJECTED && probe.count == 1,
          "failed first wait cannot become an implicit successful dependency for S2");
    probe.end();
    check(!f.fg.copyReady(tx, FrameGenerationRole::synthetic1) && !f.fg.ready(tx),
          "failed first wait cannot report role or output completion");
    check(f.fg.shutdown() == VDXRFG_E_POISONED && f.fg.reset(0) == VDXRFG_E_REJECTED &&
              f.fg.initialize(f.init) == VDXRFG_E_REJECTED,
          "uncertain dependency retains the owned graph instead of reinitializing it");
    f.hw.gpu.noErrors();
    // FrameGeneration's existing destructor intentionally retains a graph whose
    // host completion is unknown. This subprocess owns that bounded fault test.
}

int main(int argc, char** argv) {
    try {
        check(argc >= 2 && argc <= 3, "usage: ready-wait-regression <module> [all|wait-failure]");
        const std::string mode = argc == 3 ? argv[2] : "all";
        check(mode == "all" || mode == "wait-failure", "known ready-wait regression mode");
        if (mode == "wait-failure") {
            runFailedReadyWait(argv[1]);
        } else {
            runReadyWait(argv[1], 3);
            runReadyWait(argv[1], 2);
        }
        const char* backend = std::getenv("VDXR_FG_TEST_BACKEND");
        std::printf("PASS: native S1/S2 ready waits, authoritative per-ticket ownership and consumer fences (%s)\n",
                    backend ? backend : "nvidia");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
