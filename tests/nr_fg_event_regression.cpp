// Faults at actual host event-wait boundaries, without removing the GPU.
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main
#include <detours.h>

namespace {
    using CompletedFn = UINT64(STDMETHODCALLTYPE*)(ID3D11Fence*);
    using EventFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Fence*, UINT64, HANDLE);
    using WaitFn = DWORD(WINAPI*)(HANDLE, DWORD);
    CompletedFn originalCompleted;
    EventFn originalEvent;
    WaitFn originalWait;
    ID3D11Fence* watchedFence;
    HANDLE watchedEvent;
    DWORD ownerThread;
    std::string fault;
    bool armed{}, afterWait{};
    unsigned injected{};

    UINT64 STDMETHODCALLTYPE completed(ID3D11Fence* fence) {
        if (armed && fault == "removed" && afterWait && fence == watchedFence && GetCurrentThreadId() == ownerThread) {
            armed = false;
            ++injected;
            return UINT64_MAX;
        }
        return originalCompleted(fence);
    }
    HRESULT STDMETHODCALLTYPE registerEvent(ID3D11Fence* fence, UINT64 value, HANDLE event) {
        if (armed && GetCurrentThreadId() == ownerThread) {
            watchedFence = fence;
            watchedEvent = event;
            if (fault == "event-failure") {
                armed = false;
                ++injected;
                return E_OUTOFMEMORY;
            }
        }
        return originalEvent(fence, value, event);
    }
    DWORD WINAPI waitEvent(HANDLE event, DWORD timeout) {
        if (armed && event == watchedEvent && GetCurrentThreadId() == ownerThread) {
            if (fault == "stale-wake") {
                armed = false;
                ++injected;
                check(SetEvent(event) != FALSE, "inject old event notification on the actual cached handle");
                return originalWait(event, timeout);
            }
            if (fault == "wait-failure") {
                armed = false;
                ++injected;
                SetLastError(ERROR_INVALID_HANDLE);
                return WAIT_FAILED;
            }
            const auto result = originalWait(event, timeout);
            afterWait = true;
            return result;
        }
        return originalWait(event, timeout);
    }
    struct EventFaults {
        EventFaults(ID3D11Fence* sample, const std::string& scenario) {
            originalCompleted = reinterpret_cast<CompletedFn>((*reinterpret_cast<void***>(sample))[8]);
            originalEvent = reinterpret_cast<EventFn>((*reinterpret_cast<void***>(sample))[9]);
            originalWait =
                reinterpret_cast<WaitFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WaitForSingleObject"));
            check(originalWait != nullptr, "native wait export");
            ownerThread = GetCurrentThreadId();
            fault = scenario;
            watchedFence = nullptr;
            watchedEvent = nullptr;
            injected = 0;
            armed = afterWait = false;
            attach(true);
        }
        static void attach(bool enable) {
            const auto operation = enable ? DetourAttach : DetourDetach;
            check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
                  "event fault transaction");
            check(operation(reinterpret_cast<PVOID*>(&originalCompleted), completed) == NO_ERROR &&
                      operation(reinterpret_cast<PVOID*>(&originalEvent), registerEvent) == NO_ERROR &&
                      operation(reinterpret_cast<PVOID*>(&originalWait), waitEvent) == NO_ERROR &&
                      DetourTransactionCommit() == NO_ERROR,
                  "event fault hooks");
        }
        ~EventFaults() {
            armed = false;
            try {
                attach(false);
            } catch (...) {
                std::_Exit(2);
            }
        }
    };
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "module and removed/event-failure/wait-failure required");
        const std::string scenario = argv[2];
        check(scenario == "removed" || scenario == "event-failure" || scenario == "wait-failure" ||
                  scenario == "stale-wake",
              "known event fault");
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
        success(fg.initialize(init), "host event graph");
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
            input.views[eye].orientation[3] = 1.f;
            input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
            input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
        }
        OwnedFrameTransaction tx;
        success(fg.capture(input, tx), "capture event test");
        hw.await(fg, tx);
        ComPtr<ID3D11Fence> gate;
        success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "event test gate");
        HANDLE handle{};
        success(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "export event gate");
        ComPtr<ID3D12Fence> release;
        const auto opened = hw.gpu.device->OpenSharedHandle(handle, IID_PPV_ARGS(&release));
        CloseHandle(handle);
        success(opened, "import event gate");
        struct Release {
            ID3D12Fence* fence;
            ~Release() {
                (void)fence->Signal(1);
            }
        } releaseOnError{release.Get()};
        auto output = hw.texture();
        success(hw.context->Wait(gate.Get(), 1), "park actual copied pixels");
        success(fg.copyOutput(tx, FrameGenerationRole::real, output.Get()), "queue event consumer");
        ComPtr<ID3D11Fence> sample;
        success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&sample)),
                "matching consumer implementation");
        HRESULT result;
        {
            EventFaults faults(sample.Get(), scenario);
            armed = true;
            result = fg.waitForCopy(tx, FrameGenerationRole::real, 0);
            armed = false;
        }
        success(release->Signal(1), "always release before checking injected faults");
        check(hw.read(output.Get()) == pixels, "event failure preserves owned exact final pixels");
        check(injected == 1, "fault reached its actual host event boundary exactly once");
        if (scenario == "removed") {
            check(result == VDXRFG_E_POISONED && fg.shutdown() == VDXRFG_E_POISONED,
                  "post-wait removal retains the complete graph instead of falsely retiring it");
            // Deliberate process-lifetime graph retention is the uncertainty
            // policy. This fixture never removes the actual graphics device.
        } else {
            check(result ==
                      (scenario == "stale-wake"
                           ? VDXRFG_E_BUSY
                           : (scenario == "event-failure" ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE))),
                  "native event/wait failure propagates without losing GPU consumers");
            success(fg.waitForCopy(tx, FrameGenerationRole::real, 1), "settled copy remains queryable after failure");
            hw.retire(fg, tx);
            success(fg.shutdown(), "known completion settles the graph");
        }
        hw.gpu.noErrors();
        std::puts("PASS: actual host event failure, post-wait removal and retained ownership");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
