#include "fg_gpu_helpers.h"
#include "../virtualdesktop-openxr/frame_generation.h"

#include <d3d11_4.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <detours.h>

using namespace virtualdesktop_openxr;

namespace {
    // Fault injection at the actual COM boundary, including LTCG-inlined host
    // methods. Real queue signals and event registrations are forwarded. Only
    // the reported completion snapshots are scripted; this is not a physical
    // device-removal or GPU-latency reproduction.
    struct FenceCompletionScript {
        enum class Mode {
            Complete,
            PendingReady,
            StaleWake,
            SplitSentinel,
            RemovedInitial,
            RemovedAfterWake,
            CapturePending,
            CapturePendingReady,
            CapturePendingRemoved
        };
        using Completed = UINT64(STDMETHODCALLTYPE*)(ID3D11Fence*);
        using Event = HRESULT(STDMETHODCALLTYPE*)(ID3D11Fence*, UINT64, HANDLE);
        using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
        inline static Completed originalCompleted{};
        inline static Event originalEvent{};
        inline static Signal originalSignal{};
        inline static thread_local FenceCompletionScript* active{};
        inline static thread_local bool forwarding{};
        ID3D11DeviceContext4* context{};
        ID3D11Fence* fence{};
        UINT64 signaledValue{};
        void* completedEntry{};
        void* eventEntry{};
        Mode mode{Mode::Complete};
        bool measuring{}, registered{}, covered{};
        unsigned initialReads{}, postReads{}, eventCalls{}, signals{};

        static HRESULT STDMETHODCALLTYPE signal(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
            auto* self = active;
            if (self && context == self->context) {
                ++self->signals;
                if (!self->fence) {
                    self->fence = fence;
                    self->signaledValue = value;
                    const auto table = *reinterpret_cast<void***>(fence);
                    self->covered = table[8] == self->completedEntry && table[9] == self->eventEntry;
                }
            }
            const auto wasForwarding = forwarding;
            forwarding = true;
            const auto result = originalSignal(context, fence, value);
            forwarding = wasForwarding;
            return result;
        }
        static HRESULT STDMETHODCALLTYPE event(ID3D11Fence* fence, UINT64 value, HANDLE handle) {
            auto* self = active;
            const bool observed = self && self->measuring && fence == self->fence && !forwarding;
            if (observed)
                ++self->eventCalls;
            const auto wasForwarding = forwarding;
            forwarding = true;
            const auto result = originalEvent(fence, value, handle);
            forwarding = wasForwarding;
            if (observed && SUCCEEDED(result))
                self->registered = true;
            return result;
        }
        static UINT64 STDMETHODCALLTYPE completed(ID3D11Fence* fence) {
            auto* self = active;
            if (!self || !self->measuring || forwarding || fence != self->fence)
                return originalCompleted(fence);
            if (!self->registered) {
                ++self->initialReads;
                if (self->mode == Mode::RemovedInitial)
                    return UINT64_MAX;
                if (self->mode == Mode::CapturePendingReady && self->initialReads > 1)
                    return originalCompleted(fence);
                if (self->mode == Mode::CapturePendingRemoved && self->initialReads > 1)
                    return UINT64_MAX;
                return self->mode == Mode::Complete ? originalCompleted(fence) : 0;
            }
            ++self->postReads;
            if (self->mode == Mode::RemovedAfterWake)
                return UINT64_MAX;
            if (self->mode == Mode::SplitSentinel)
                return self->postReads == 1 ? 0 : UINT64_MAX;
            if (self->mode == Mode::StaleWake)
                return 0;
            return originalCompleted(fence);
        }
        FenceCompletionScript(ID3D11Device5* device, ID3D11DeviceContext4* source) : context(source) {
            check(!active, "one COM fence completion script");
            ComPtr<ID3D11Fence> sample;
            success(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&sample)),
                    "completion script sample fence");
            const auto table = *reinterpret_cast<void***>(sample.Get());
            completedEntry = table[8];
            eventEntry = table[9];
            originalCompleted = reinterpret_cast<Completed>(completedEntry);
            originalEvent = reinterpret_cast<Event>(eventEntry);
            originalSignal = reinterpret_cast<Signal>((*reinterpret_cast<void***>(source))[147]);
            active = this;
            check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalCompleted), completed) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalEvent), event) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal) == NO_ERROR &&
                      DetourTransactionCommit() == NO_ERROR,
                  "install real COM fence completion script");
        }
        ~FenceCompletionScript() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalCompleted), completed) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalEvent), event) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal) || DetourTransactionCommit())
                std::_Exit(3);
            active = nullptr;
        }
        void begin(Mode selected) {
            mode = selected;
            registered = false;
            initialReads = postReads = eventCalls = 0;
            measuring = true;
        }
        void end() noexcept {
            measuring = false;
        }
        void settleRealSignal() const {
            check(fence && signaledValue && covered, "observer selected the actual queue-signaled COM fence");
            const auto handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(handle != nullptr, "completion observer real settling event");
            const auto registered = originalEvent(fence, signaledValue, handle);
            const auto waited = SUCCEEDED(registered) ? WaitForSingleObject(handle, 5000) : WAIT_FAILED;
            CloseHandle(handle);
            success(registered, "register actual copy completion before fault injection");
            check(waited == WAIT_OBJECT_0, "actual copied GPU work settles before scripted observations");
        }
        void report(const char* scope) const {
            std::printf("COM_FENCE_FAULT_OBSERVER scope=%s initial=%u postwake=%u events=%u signals=%u covered=%u\n",
                        scope,
                        initialReads,
                        postReads,
                        eventCalls,
                        signals,
                        unsigned(covered));
        }
    };
} // namespace

struct Interop {
    Gpu gpu;
    ComPtr<ID3D11Device5> device;
    ComPtr<ID3D11DeviceContext4> context;

    Interop() {
        ComPtr<IDXGIFactory4> factory;
        success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "interop factory");
        ComPtr<IDXGIAdapter> adapter;
        success(factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)),
                "same NVIDIA adapter");
        ComPtr<ID3D11Device> base;
        ComPtr<ID3D11DeviceContext> immediate;
        success(D3D11CreateDevice(adapter.Get(),
                                  D3D_DRIVER_TYPE_UNKNOWN,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  D3D11_SDK_VERSION,
                                  &base,
                                  nullptr,
                                  &immediate),
                "D3D11 interop device");
        success(base.As(&device), "D3D11 device5");
        success(immediate.As(&context), "D3D11 context4");
    }

    ComPtr<ID3D11Texture2D> texture(const std::vector<uint8_t>* pixels = nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data{};
        if (pixels) {
            data.pSysMem = pixels->data();
            data.SysMemPitch = width * 4;
        }
        ComPtr<ID3D11Texture2D> result;
        success(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &result), "D3D11 eye pixels");
        return result;
    }

    std::vector<uint8_t> read(ID3D11Texture2D* source) {
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        success(device->CreateTexture2D(&desc, nullptr, &staging), "D3D11 readback");
        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        success(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "D3D11 map");
        std::vector<uint8_t> result(width * height * 4);
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(
                result.data() + y * width * 4, static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, width * 4);
        }
        context->Unmap(staging.Get(), 0);
        return result;
    }

    void await(FrameGeneration& fg, OwnedFrameTransaction& tx) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fg.ready(tx) && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(fg.ready(tx), "bounded D3D11/D3D12 FG readiness");
    }

    void retire(FrameGeneration& fg, OwnedFrameTransaction& tx) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        HRESULT result;
        do {
            result = fg.retire(tx);
            if (result != VDXRFG_E_BUSY) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        // Map waits for the copy, but GetCompletedValue publication can lag it.
        // Retire is expressly a nonblocking completion poll, as in the worker.
        success(result, "bounded retirement after actual consumer readback");
    }
};

void run(const char* path, const std::string& scenario) {
    const bool copyWaitProbe = scenario.rfind("copy-wait-", 0) == 0;
    Interop hw;
    FrameGeneration fg;
    FrameGeneration::InitInfo init{hw.device.Get(),
                                   hw.context.Get(),
                                   hw.gpu.device.Get(),
                                   std::filesystem::absolute(path),
                                   eyeWidth,
                                   height,
                                   3,
                                   DXGI_FORMAT_R8G8B8A8_UNORM};
    auto missing = init;
    missing.modulePath = init.modulePath.parent_path() / L"missing-fg-module.dll";
    check(FAILED(fg.initialize(missing)) && !fg.initialized(), "missing module leaves host retryable");
    if (scenario == "invalid-context") {
        ComPtr<ID3D11DeviceContext3> deferred;
        success(hw.device->CreateDeferredContext3(0, &deferred), "deferred context negative control");
        ComPtr<ID3D11DeviceContext4> context;
        success(deferred.As(&context), "deferred context4");
        auto invalid = init;
        invalid.context11 = context.Get();
        check(fg.initialize(invalid) == VDXRFG_E_REJECTED && !fg.initialized(),
              "deferred context rejected before registering GPU resources");
        return;
    }
    success(fg.initialize(init), "initialize real interop host");
    FrameGenerationCadence cadence;
    FinalProjectionInput input;
    input.serial = 1;
    input.time = 1000000000;
    input.period = 10000000;
    input.slots =
        cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 0), 0, false);
    const auto a = scene(0), b = scene(24);
    input.colour[0] = input.colour[1] = hw.texture(&a);
    for (UINT eye = 0; eye < 2; ++eye) {
        input.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
        input.views[eye].orientation[3] = 1;
        input.views[eye].position[0] = eye ? .032f : -.032f;
        input.views[eye].fov[0] = input.views[eye].fov[3] = -.8f;
        input.views[eye].fov[1] = input.views[eye].fov[2] = .8f;
    }
    OwnedFrameTransaction prime;
    if (scenario.rfind("capture-query-", 0) == 0) {
        using Mode = FenceCompletionScript::Mode;
        const bool complete = scenario == "capture-query-complete";
        const bool pending = scenario == "capture-query-pending";
        const bool pendingReady = scenario == "capture-query-pending-ready";
        const bool removedInitial = scenario == "capture-query-removed-initial";
        const bool removedPending = scenario == "capture-query-pending-removed";
        check(complete || pending || pendingReady || removedInitial || removedPending,
              "known capture producer completion observation");
        FenceCompletionScript observed(hw.device.Get(), hw.context.Get());
        success(fg.capture(input, prime), "capture selects the actual producer fence");
        observed.settleRealSignal();
        hw.await(fg, prime);
        ++input.serial;
        input.time += 3 * input.period;
        input.colour[0] = input.colour[1] = hw.texture(&b);
        input.slots = cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 3),
                                   input.time - 30000000,
                                   true);
        OwnedFrameTransaction next;
        observed.begin(complete         ? Mode::Complete
                       : pendingReady   ? Mode::CapturePendingReady
                       : removedInitial ? Mode::RemovedInitial
                       : removedPending ? Mode::CapturePendingRemoved
                                        : Mode::CapturePending);
        const auto result = fg.capture(input, next);
        observed.end();
        observed.report(scenario.c_str());
        std::printf("CAPTURE_QUERY_RESULT hr=0x%08lx accepted=%u\n",
                    static_cast<unsigned long>(result),
                    unsigned(bool(next.ticket.token)));
        const auto expected = removedInitial || removedPending ? VDXRFG_E_POISONED : pending ? VDXRFG_E_BUSY : S_OK;
        check(result == expected, "capture preserves pending second chance and poisons either removal snapshot");
        check(observed.covered && observed.initialReads == (complete || removedInitial ? 1u : 2u) &&
                  observed.postReads == 0 && observed.eventCalls == 0,
              "capture reads a completed producer once and preserves a pending producer's second chance");
        if (removedInitial || removedPending) {
            check(prime.ticket.token && !next.ticket.token && !fg.initialized() && fg.shutdown() == VDXRFG_E_POISONED,
                  "producer removal retains the owned graph and accepts no new transaction");
            std::puts("PASS: COM removal observation; no physical GPU reset is claimed");
            return;
        }
        check(bool(next.ticket.token) == !pending, "only completed producer snapshots accept a new transaction");
        if (next.ticket.token) {
            hw.await(fg, next);
            hw.retire(fg, next);
        }
        hw.retire(fg, prime);
        success(fg.shutdown(), "shutdown settled capture producer observation host");
        hw.gpu.noErrors();
        return;
    }
    if (scenario == "invalid-plan") {
        input.slots.count = 0;
        check(fg.capture(input, prime) == VDXRFG_E_REJECTED && !prime.ticket.token,
              "invalid slot count must reject before accepting a GPU transaction");
        success(fg.shutdown(), "shutdown after rejected plan");
        return;
    }
    success(fg.capture(input, prime), "capture finalized D3D11 stereo pixels");
    if (copyWaitProbe) {
        HANDLE event{};
        const auto prepared = fg.prepareOutputReadyWait(prime, event);
        if (prepared == VDXRFG_E_BUSY) {
            check(event && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0,
                  "bounded real output-ready event before copy wait probe");
            success(fg.prepareOutputReadyWait(prime, event), "authoritative output readiness after event");
        } else {
            success(prepared, "completed real output before copy wait probe");
        }
    } else {
        hw.await(fg, prime);
    }
    check(fg.shutdown() == VDXRFG_E_BUSY && fg.reset(0) == VDXRFG_E_BUSY, "live interop ticket retains graph");
    auto output = hw.texture();
    if (copyWaitProbe) {
        using Mode = FenceCompletionScript::Mode;
        check(scenario == "copy-wait-complete" || scenario == "copy-wait-pending-ready" ||
                  scenario == "copy-wait-stale-wake" || scenario == "copy-wait-removed-initial" ||
                  scenario == "copy-wait-removed-postwake",
              "known copy-wait COM observation");
        FenceCompletionScript observed(hw.device.Get(), hw.context.Get());
        success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "queue actual observed real-role copy");
        check(observed.signals == 1 && observed.signaledValue == prime.consumerValue,
              "observer selected this validated role's actual consumer fence and value");
        observed.settleRealSignal();
        check(hw.read(output.Get()) == a, "actual copied pixels precede scripted fence observations");
        const bool complete = scenario == "copy-wait-complete";
        const bool removedInitial = scenario == "copy-wait-removed-initial";
        const bool removedPost = scenario == "copy-wait-removed-postwake";
        const bool stale = scenario == "copy-wait-stale-wake";
        observed.begin(complete         ? Mode::Complete
                       : removedInitial ? Mode::RemovedInitial
                       : removedPost    ? Mode::RemovedAfterWake
                       : stale          ? Mode::StaleWake
                                        : Mode::PendingReady);
        const auto result = fg.waitForCopy(prime, FrameGenerationRole::real, 1000);
        observed.end();
        observed.report(scenario.c_str());
        const auto expected = removedInitial || removedPost ? VDXRFG_E_POISONED : stale ? VDXRFG_E_BUSY : S_OK;
        check(result == expected, "copy wait honors the authoritative current role's completion snapshot");
        check(observed.covered && observed.initialReads == 1 &&
                  observed.postReads == (complete || removedInitial ? 0u : 1u) &&
                  observed.eventCalls == (complete || removedInitial ? 0u : 1u),
              "copy wait uses one validated initial snapshot and at most one authoritative post-event snapshot");
        if (removedInitial || removedPost) {
            check(prime.ticket.token && !fg.initialized() && fg.shutdown() == VDXRFG_E_POISONED &&
                      fg.shutdown() == VDXRFG_E_POISONED,
                  "injected removal keeps the owned transaction and poisoned graph retained");
            std::puts("PASS: COM removal fault observation retains the graph; no physical GPU reset is claimed");
            return;
        }
        success(fg.waitForCopy(prime, FrameGenerationRole::real, 1000),
                "removing the observer restores actual completed-copy readiness");
        success(fg.retire(prime), "retire the actual settled copy after bounded observer case");
        success(fg.shutdown(), "shutdown settled copy-wait observation host");
        hw.gpu.noErrors();
        return;
    }
    if (scenario == "source-overwrite") {
        success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "protect real pixels before reuse");
        hw.context->UpdateSubresource(input.colour[0].Get(), 0, nullptr, b.data(), width * 4, 0);
        check(hw.read(input.colour[0].Get()) == b, "overwrite control changes the same source object");
        check(hw.read(output.Get()) == a, "prepared real image preserves original pixels after source overwrite");
        success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "late fallback reuses protected real");
        check(hw.read(output.Get()) == a, "late fallback cannot recopy overwritten source pixels");
        hw.retire(fg, prime);
        success(fg.reset(0), "settled source reuse resets safely");
        success(fg.shutdown(), "settled source overwrite host");
        hw.gpu.noErrors();
        return;
    }
    if (scenario == "event-stale") {
        ComPtr<ID3D11Fence> gate;
        success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "stale event gate");
        HANDLE handle{};
        success(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "export stale event gate");
        ComPtr<ID3D12Fence> release;
        const auto opened = hw.gpu.device->OpenSharedHandle(handle, IID_PPV_ARGS(&release));
        CloseHandle(handle);
        success(opened, "import stale event gate");
        struct Release {
            ID3D12Fence* fence;
            uint64_t value{1};
            ~Release() {
                (void)fence->Signal(value);
            }
        } releaseOnError{release.Get()};
        success(hw.context->Wait(gate.Get(), 1), "park first consumer");
        success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "queue first event consumer");
        const auto firstWait = fg.waitForCopy(prime, FrameGenerationRole::real, 1);
        const auto repeatedWait = fg.waitForCopy(prime, FrameGenerationRole::real, 1);
        const auto earlyRetire = fg.retire(prime);
        success(release->Signal(1), "release first event before assertions");
        check(firstWait == VDXRFG_E_BUSY && repeatedWait == VDXRFG_E_BUSY && earlyRetire == VDXRFG_E_BUSY,
              "repeated finite event timeouts cannot retire a blocked copy");
        check(hw.read(output.Get()) == a, "first registered copy completes exactly");
        hw.retire(fg, prime); // Leave its auto-reset event signaled for the next ticket.
        input.serial = 2;
        input.time += 3 * input.period;
        input.colour[0] = input.colour[1] = hw.texture(&b);
        input.slots = cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 3),
                                   input.time - 30000000,
                                   true);
        OwnedFrameTransaction next;
        success(fg.capture(input, next), "capture later role with prior event notification");
        hw.await(fg, next);
        releaseOnError.value = 2;
        success(hw.context->Wait(gate.Get(), 2), "park later synthetic consumer");
        success(fg.copyOutput(next, FrameGenerationRole::synthetic1, output.Get()), "queue later role");
        const auto staleWake = fg.waitForCopy(next, FrameGenerationRole::synthetic1, 0);
        const auto secondWait = fg.waitForCopy(next, FrameGenerationRole::synthetic1, 1);
        const auto nextRetire = fg.retire(next);
        success(release->Signal(2), "release later event before assertions");
        check(staleWake == VDXRFG_E_BUSY && secondWait == VDXRFG_E_BUSY && nextRetire == VDXRFG_E_BUSY,
              "old auto-reset event cannot invent later-role completion or permit bank reuse");
        success(fg.waitForCopy(next, FrameGenerationRole::synthetic1, 100), "new role's own GPU signal wakes its wait");
        const auto synthetic = hw.read(output.Get());
        check(synthetic != a && synthetic != b, "later role retains actual generated pixels");
        hw.retire(fg, next);
        success(fg.shutdown(), "settled reusable event lifetime");
        hw.gpu.noErrors();
        return;
    }
    if (scenario == "copy-identity") {
        auto clone = prime;
        clone.ticket.output_ready_fence = nullptr;
        clone.ticket.output_ready_value = 0;
        clone.ticket.role_mask = 0;
        clone.consumerValue = UINT64_MAX;
        check(!fg.copyReady(clone, FrameGenerationRole::real), "forged clone cannot invent prepared B completion");
        check(fg.waitForCopy(clone, FrameGenerationRole::real, 0) == VDXRFG_E_REJECTED,
              "unprepared role cannot register a completion event from forged clone fields");
        input.colour = {};
        success(fg.copyOutput(clone, FrameGenerationRole::real, output.Get()), "retained captured sources prepare B");
        const auto prepared = clone.consumerValue;
        success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "same SDK image reuses preparation");
        check(prime.consumerValue == prepared, "duplicate preparation queues no new consumer");
        auto other = hw.texture();
        check(fg.copyOutput(clone, FrameGenerationRole::real, other.Get()) == VDXRFG_E_REJECTED,
              "cloned transaction cannot replace the prepared SDK destination");
        check(hw.read(output.Get()) == a, "captured final NR sources remain owned after caller releases them");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!fg.copyReady(clone, FrameGenerationRole::real) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(fg.copyReady(clone, FrameGenerationRole::real) && !fg.copyReady(clone, FrameGenerationRole::synthetic1),
              "only an actually prepared role can report GPU completion");
        success(fg.waitForCopy(clone, FrameGenerationRole::real, 1), "completed role event wait is immediate");
        check(fg.waitForCopy(clone, FrameGenerationRole::real, INFINITE) == VDXRFG_E_REJECTED,
              "copy completion never accepts an unbounded wait");
        hw.retire(fg, prime);
        check(!fg.copyReady(clone, FrameGenerationRole::real), "retirement rejects stale prepared-role clones");
        check(fg.waitForCopy(clone, FrameGenerationRole::real, 0) == VDXRFG_E_REJECTED,
              "retired role cannot register a stale GPU event");
        success(fg.shutdown(), "settled prepared-role identity host");
        hw.gpu.noErrors();
        return;
    }
    if (scenario == "blocked-consumer") {
        ComPtr<ID3D11Fence> gate;
        success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "consumer gate");
        HANDLE handle{};
        success(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "export consumer gate");
        ComPtr<ID3D12Fence> release;
        const auto opened = hw.gpu.device->OpenSharedHandle(handle, IID_PPV_ARGS(&release));
        CloseHandle(handle);
        success(opened, "import consumer gate");
        auto earlyCopy = prime;
        success(hw.context->Wait(gate.Get(), 1), "park D3D11 output reader");
        success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "queue blocked output read");
        earlyCopy.ticket.output_ready_value = 0;
        earlyCopy.consumerValue = 0;
        const bool incorrectlyReady = fg.copyReady(earlyCopy, FrameGenerationRole::real);
        const auto eventWait = fg.waitForCopy(earlyCopy, FrameGenerationRole::real, 0);
        const auto retired = fg.retire(earlyCopy);
        // Always release parked GPU work before checking a failing control.
        success(release->Signal(1), "release parked output reader");
        check(hw.read(output.Get()) == a, "blocked read retained exact pixels");
        check(retired == VDXRFG_E_BUSY, "copied transaction cannot hide a pending host consumer");
        check(!incorrectlyReady, "real-copy readiness does not trust clone fence fields");
        check(eventWait == VDXRFG_E_BUSY, "finite completion wait times out on the actual blocked copy");
        success(fg.waitForCopy(earlyCopy, FrameGenerationRole::real, 1), "released copy completes authoritative wait");
        hw.retire(fg, prime);
        success(fg.shutdown(), "shutdown after blocked reader");
        return;
    }
    success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "copy prime B to compositor-owned texture");
    check(hw.read(output.Get()) == a, "real prime survives cross-API pack and copy bit-exact");
    const auto stale = prime;
    hw.retire(fg, prime);
    if (scenario == "stale-ticket") {
        check(!fg.ready(stale), "retired ticket cannot access borrowed module fences");
        success(fg.shutdown(), "shutdown after stale rejection");
        return;
    }
    input.serial = 2;
    input.time += 3 * input.period;
    input.colour[0] = input.colour[1] = hw.texture(&b);
    input.slots = cadence.plan(
        cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 3), input.time - 30000000, true);
    OwnedFrameTransaction pair;
    success(fg.capture(input, pair), "capture moving final pixels");
    hw.await(fg, pair);
    if (scenario == "copy-alias") {
        const auto real = hw.texture();
        success(fg.copyOutput(pair, FrameGenerationRole::real, real.Get()), "prepare independent B");
        check(fg.copyOutput(pair, FrameGenerationRole::synthetic1, real.Get()) == VDXRFG_E_REJECTED,
              "synthetic role cannot overwrite an already prepared real image");
        check(fg.copyOutput(pair, FrameGenerationRole::synthetic1, input.colour[0].Get()) == VDXRFG_E_REJECTED,
              "SDK destination cannot overwrite retained finalized input");
        check(hw.read(real.Get()) == b, "alias rejection preserves exact B");
        hw.retire(fg, pair);
        success(fg.shutdown(), "settled alias control");
        hw.gpu.noErrors();
        return;
    }
    success(fg.copyOutput(pair, FrameGenerationRole::synthetic1, output.Get()), "copy S1");
    const auto s1 = hw.read(output.Get());
    const auto outputS2 = hw.texture(), outputB = hw.texture();
    success(fg.copyOutput(pair, FrameGenerationRole::synthetic2, outputS2.Get()), "copy S2");
    const auto s2 = hw.read(outputS2.Get());
    success(fg.copyOutput(pair, FrameGenerationRole::real, outputB.Get()), "copy B");
    check(hw.read(outputB.Get()) == b, "postprocessing input B remains bit-exact");
    check(s1 != s2 && s1 != a && s2 != b, "real GPU interop yields distinct synthetic outputs");
    hw.retire(fg, pair);
    success(fg.reset(0), "reset settled interop host");
    success(fg.shutdown(), "unload settled host");
    check(!fg.ready(stale), "unloaded host rejects stale borrowed fences");
    hw.gpu.noErrors();
}

int main(int argc, char** argv) {
    try {
        check(argc >= 2, "module path required");
        run(argv[1], argc > 2 ? argv[2] : "interop");
        std::puts("PASS: real D3D11/D3D12 host capture, copies and retained retirement");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
