#include "fg_gpu_helpers.h"
#include "fg_early_ready_test_gate.h"

class PollProbe {
    using Event = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
    using Wait = DWORD(WINAPI*)(HANDLE, DWORD);
    using WaitEx = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
    inline static PollProbe* active{};
    Event event{};
    Wait wait = WaitForSingleObject;
    WaitEx waitEx = WaitForSingleObjectEx;
    const DWORD owner = GetCurrentThreadId();
    bool watches() const {
        return armed && GetCurrentThreadId() == owner;
    }
    static HRESULT STDMETHODCALLTYPE onEvent(ID3D12Fence* f, UINT64 v, HANDLE e) {
        auto& s = *active;
        if (s.watches())
            ++s.registrations;
        return s.event(f, v, e);
    }
    static DWORD WINAPI onWait(HANDLE e, DWORD t) {
        auto& s = *active;
        if (s.watches())
            ++s.waits;
        return s.wait(e, t);
    }
    static DWORD WINAPI onWaitEx(HANDLE e, DWORD t, BOOL a) {
        auto& s = *active;
        if (s.watches())
            ++s.waits;
        return s.waitEx(e, t, a);
    }
    void attach(bool enable) {
        auto op = enable ? DetourAttach : DetourDetach;
        LONG status = DetourTransactionBegin();
        const bool started = !status;
        if (!status)
            status = DetourUpdateThread(GetCurrentThread());
        if (!status)
            status = op(reinterpret_cast<PVOID*>(&event), onEvent);
        if (!status)
            status = op(reinterpret_cast<PVOID*>(&wait), onWait);
        if (!status)
            status = op(reinterpret_cast<PVOID*>(&waitEx), onWaitEx);
        if (!status)
            status = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(status), "native poll observer");
    }

  public:
    bool armed{};
    unsigned registrations{}, waits{};
    explicit PollProbe(ID3D12Fence* f) {
        event = reinterpret_cast<Event>((*reinterpret_cast<void***>(f))[9]);
        check(!active, "one poll observer");
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }
    ~PollProbe() {
        armed = false;
        try {
            attach(false);
        } catch (...) {
            std::terminate();
        }
        active = nullptr;
    }
};

void run(const char* path, bool fidelityFx) {
    constexpr unsigned multiplier = 4;
    HMODULE module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(module != nullptr, "load synthesis module");
    const auto get = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
    check(get != nullptr, "module must negotiate V4 role readiness");
    VDXRFG_ApiV4 api{};
    api.struct_size = sizeof(api);
    api.api_version = VDXRFG_API_VERSION_4;
    success(get(&api), "negotiate role readiness");
    check(api.GetOutputReadiness && api.max_multiplier >= 4, "complete V4 table");
    Gpu gpu(false);
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

    gate.arm(prime);
    VDXRFG_TicketV1 pair{};
    success(api.Submit(context, &info, &pair), "submit gated ordinary pair");
    check(gate.first != nullptr, "4x pair has an independent S1 signal");
    gpu.wait(gate.first.Get(), gate.firstValue);
    check(static_cast<ID3D12Fence*>(pair.output_ready_fence)->GetCompletedValue() < pair.output_ready_value,
          "previous full submission is incomplete and other lease is free");
    ++info.serial;
    for (unsigned i = 0; i < multiplier - 1; ++i)
        info.target_time_ns[i] = info.display_time_ns + (i + 1) * info.display_period_ns;
    info.display_time_ns += multiplier * info.display_period_ns;
    unsigned events{}, waits{};
    {
        PollProbe probe(static_cast<ID3D12Fence*>(pair.output_ready_fence));
        probe.armed = true;
        bool busy = true, cleared = true;
        for (unsigned retry = 0; retry < 32; ++retry) {
            VDXRFG_TicketV1 pending;
            std::memset(&pending, 0x5a, sizeof(pending));
            busy &= api.Submit(context, &info, &pending) == VDXRFG_E_BUSY;
            const VDXRFG_TicketV1 empty{};
            cleared &= std::memcmp(&pending, &empty, sizeof(pending)) == 0;
        }
        probe.armed = false;
        events = probe.registrations;
        waits = probe.waits;
        gate.release();
        gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
        check(busy && cleared, "all retries return BUSY and clear the entire output ticket");
    }
    success(api.Retire(context, pair.token), "retire gated pair after full completion");
    VDXRFG_TicketV1 next{};
    success(api.Submit(context, &info, &next), "same submission progresses after gate release");
    gpu.wait(static_cast<ID3D12Fence*>(next.output_ready_fence), next.output_ready_value);
    success(api.Retire(context, next.token), "retire next pair");
    success(api.Reset(context, 0), "reset completed context");
    success(api.Destroy(context), "destroy completed context");
    FreeLibrary(module);
    gpu.noErrors();
    std::printf("poll retries=32 registrations=%u waits=%u backend=%s\n",
                events,
                waits,
                fidelityFx ? "fidelityfx-full" : "nvidia");
    check(events == 0 && waits == 0, "zero-timeout Submit must not register kernel events or wait");
}
int main(int argc, char** argv) {
    try {
        check(argc == 3, "module and backend required");
        const std::string backend(argv[2]);
        check(backend == "nvidia" || backend == "fidelityfx-full", "supported backend");
        run(argv[1], backend == "fidelityfx-full");
        std::puts("PASS: nonblocking BUSY polling and post-gate progress");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
