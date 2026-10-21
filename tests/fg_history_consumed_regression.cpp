#include "fg_gpu_helpers.h"

#include <detours.h>

#include <atomic>
#include <cstdlib>
#include <thread>

// Stop the module queue immediately after its public input-consumed signal.
// The independent producer can then legally reuse the input while synthesis
// remains parked. Native entry-point interception must not be combined with GBV.
class ConsumedGate {
    using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    inline static ConsumedGate* active{};
    Signal signal{};
    ID3D12CommandQueue* queue{};
    ID3D12Fence* consumed{};
    const DWORD owner = GetCurrentThreadId();
    bool armed{};
    HANDLE cancel{};
    std::thread watchdog;

    static HRESULT STDMETHODCALLTYPE onSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
        auto& probe = *active;
        const HRESULT result = probe.signal(queue, fence, value);
        if (SUCCEEDED(result) && GetCurrentThreadId() == probe.owner && queue == probe.queue &&
            fence == probe.consumed && probe.armed) {
            ++probe.hits;
            if (probe.hits == 1) {
                probe.waitResult = queue->Wait(probe.gate.Get(), 1);
                if (FAILED(probe.waitResult))
                    return probe.waitResult;
            }
        }
        return result;
    }

    void attach(bool enable) {
        LONG error = DetourTransactionBegin();
        const bool started = error == NO_ERROR;
        if (!error)
            error = DetourUpdateThread(GetCurrentThread());
        if (!error)
            error = (enable ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&signal), onSignal);
        if (!error)
            error = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(error), "consumed signal observer transaction");
    }

  public:
    ComPtr<ID3D12Fence> gate;
    unsigned hits{};
    HRESULT waitResult{S_OK};
    std::atomic_bool expired{};

    ConsumedGate(Gpu& gpu, ID3D12Fence* consumedFence) : queue(gpu.queue.Get()), consumed(consumedFence) {
        check(!gpu.debugEnabled && !gpu.diagnostics, "native consumed observer requires validation disabled");
        success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "synthesis gate");
        cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(cancel != nullptr, "watchdog cancellation event");
        signal = reinterpret_cast<Signal>((*reinterpret_cast<void***>(queue))[14]);
        check(!active, "one consumed observer");
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            CloseHandle(cancel);
            throw;
        }
    }
    ~ConsumedGate() {
        release();
        try {
            attach(false);
        } catch (...) {
            std::_Exit(2);
        }
        active = nullptr;
        CloseHandle(cancel);
    }
    void begin() {
        watchdog = std::thread([this] {
            if (WaitForSingleObject(cancel, 2000) != WAIT_OBJECT_0) {
                expired = true;
                gate->Signal(1);
            }
        });
        armed = true;
    }
    void stopObserving() {
        armed = false;
    }
    void release() noexcept {
        armed = false;
        gate->Signal(1);
        SetEvent(cancel);
        if (watchdog.joinable())
            watchdog.join();
    }
};

void boundedWait(ID3D12Fence* fence, UINT64 value) {
    check(fence && fence->GetCompletedValue() != UINT64_MAX, "valid overwrite completion fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event != nullptr, "overwrite completion event");
    const HRESULT result = fence->SetEventOnCompletion(value, event);
    const DWORD wait = SUCCEEDED(result) ? WaitForSingleObject(event, 2000) : WAIT_FAILED;
    CloseHandle(event);
    success(result, "overwrite completion registration");
    check(wait == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX,
          "producer overwrite completes within two seconds while synthesis is parked");
}

struct Fixture {
    Gpu gpu{false};
    HMODULE module{};
    VDXRFG_ApiV2 api{};
    void* context{};
    ComPtr<ID3D12Resource> input, poison;
    std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs;
    ComPtr<ID3D12CommandQueue> producer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> copy;
    ComPtr<ID3D12Fence> overwritten;
    const uint32_t multiplier;
    const bool hostOwnsReal;

    Fixture(const char* path, const std::string& backend, uint32_t multiplier, bool hostOwnsReal)
        : multiplier(multiplier), hostOwnsReal(hostOwnsReal) {
        module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load real frame-generation module");
        auto get = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
        check(get != nullptr, "V2 API export");
        api.struct_size = sizeof(api);
        api.api_version = VDXRFG_API_VERSION_2;
        success(get(&api), "negotiate real module API");
        input = gpu.texture();
        poison = gpu.texture();
        auto poisoned = scene(91);
        for (size_t i = 0; i < poisoned.size(); ++i)
            poisoned[i] ^= static_cast<uint8_t>(i % 4 == 3 ? 0x7f : 0xe7);
        gpu.transfer(poison.Get(), &poisoned);
        VDXRFG_CreateInfoV2 create{};
        create.struct_size = sizeof(create);
        create.api_version = VDXRFG_API_VERSION_2;
        auto& base = create.base;
        base.struct_size = sizeof(base);
        base.api_version = VDXRFG_API_VERSION;
        base.device = gpu.device.Get();
        base.queue = gpu.queue.Get();
        base.input = input.Get();
        base.width = width;
        base.height = height;
        base.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        base.multiplier = multiplier;
        create.flags = (hostOwnsReal ? VDXRFG_CREATE_HOST_OWNS_REAL : 0u) |
                       (backend == "fidelityfx-full" ? VDXRFG_CREATE_FIDELITYFX_FULL : 0u);
        check((api.supported_create_flags & create.flags) == create.flags, "requested capabilities supported");
        for (UINT bank = 0; bank < 2; ++bank)
            for (UINT role = 0; role < 3; ++role)
                if (role != 1 || multiplier == 3) {
                    outputs[bank][role] = gpu.texture();
                    base.outputs[bank][role] = outputs[bank][role].Get();
                }
        success(api.Create(&create, &context), "create synthesis context");
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        success(gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&producer)), "independent producer queue");
        success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&overwritten)), "overwrite fence");
        success(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "overwrite allocator");
        success(gpu.device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&copy)),
                "overwrite list");
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
        for (auto& barrier : barriers)
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition = {input.Get(),
                                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_COMMON,
                                  D3D12_RESOURCE_STATE_COPY_DEST};
        barriers[1].Transition = {poison.Get(),
                                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_COMMON,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE};
        copy->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        copy->CopyResource(input.Get(), poison.Get());
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        copy->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        success(copy->Close(), "close overwrite list");
    }

    std::array<std::vector<uint8_t>, 4> run(bool overwrite) {
        const auto a = scene(0), b = scene(24), sentinel = scene(57);
        for (auto& bank : outputs)
            for (auto& output : bank)
                if (output)
                    gpu.transfer(output.Get(), &sentinel);
        VDXRFG_SubmitInfoV1 info{};
        info.struct_size = sizeof(info);
        info.api_version = VDXRFG_API_VERSION;
        info.input = input.Get();
        info.serial = 1;
        info.display_time_ns = 1000000000;
        info.display_period_ns = 10000000;
        for (UINT eye = 0; eye < 2; ++eye) {
            info.views[eye].orientation[3] = 1;
            info.views[eye].position[0] = eye ? 0.032f : -0.032f;
            info.views[eye].fov[0] = info.views[eye].fov[3] = -0.8f;
            info.views[eye].fov[1] = info.views[eye].fov[2] = 0.8f;
            info.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
        }
        gpu.transfer(input.Get(), &a);
        VDXRFG_TicketV1 prime{};
        success(api.Submit(context, &info, &prime), "prime capture");
        check(prime.prime && prime.role_mask == (hostOwnsReal ? 0u : 4u), "prime role ownership");
        gpu.wait(static_cast<ID3D12Fence*>(prime.output_ready_fence), prime.output_ready_value);
        gpu.transfer(input.Get(), &b);
        info.serial = 2;
        info.display_time_ns += multiplier * info.display_period_ns;
        for (UINT i = 0; i < multiplier - 1; ++i) {
            info.target_time_ns[i] = 1000000000 + (i + 1) * info.display_period_ns;
            info.fractions[i] = static_cast<float>(i + 1) / multiplier;
        }
        VDXRFG_TicketV1 pair{};
        if (overwrite) {
            ConsumedGate probe(gpu, static_cast<ID3D12Fence*>(prime.input_consumed_fence));
            probe.begin();
            const auto start = GetTickCount64();
            success(api.Submit(context, &info, &pair), "parked pair Submit");
            const auto elapsed = GetTickCount64() - start;
            probe.stopObserving();
            success(probe.waitResult, "park synthesis after consumed signal");
            check(elapsed < 1000 && probe.hits == 1 && !probe.expired, "Submit is nonblocking and parks once");
            auto ready = static_cast<ID3D12Fence*>(pair.output_ready_fence);
            check(ready && ready->GetCompletedValue() < pair.output_ready_value, "synthesis remains pending");
            success(producer->Wait(static_cast<ID3D12Fence*>(pair.input_consumed_fence), pair.input_consumed_value),
                    "producer observes public consumed boundary");
            ID3D12CommandList* commands[] = {copy.Get()};
            producer->ExecuteCommandLists(1, commands);
            success(producer->Signal(overwritten.Get(), 1), "producer overwrite completion");
            boundedWait(overwritten.Get(), 1);
            check(!probe.expired && ready->GetCompletedValue() < pair.output_ready_value,
                  "input was overwritten before synthesis could finish");
            probe.release();
        } else {
            success(api.Submit(context, &info, &pair), "reference pair Submit");
        }
        check(!pair.prime && pair.bank != prime.bank &&
                  pair.role_mask == ((multiplier == 3 ? 3u : 1u) | (hostOwnsReal ? 0u : 4u)),
              "pair owns the expected distinct output bank");
        gpu.wait(static_cast<ID3D12Fence*>(pair.output_ready_fence), pair.output_ready_value);
        if (overwrite)
            check(gpu.transfer(input.Get(), nullptr) == gpu.transfer(poison.Get(), nullptr),
                  "independent producer actually replaced every input pixel");
        std::array<std::vector<uint8_t>, 4> pixels;
        pixels[0] = gpu.transfer(outputs[pair.bank][0].Get(), nullptr);
        if (multiplier == 3)
            pixels[1] = gpu.transfer(outputs[pair.bank][1].Get(), nullptr);
        pixels[2] = gpu.transfer(outputs[pair.bank][2].Get(), nullptr);
        pixels[3] = gpu.transfer(outputs[prime.bank][2].Get(), nullptr);
        check(pixels[0] != a && pixels[0] != b, "S1 is not an endpoint repeat");
        check(multiplier != 3 || (pixels[1] != pixels[0] && pixels[1] != a && pixels[1] != b),
              "S2 is a distinct intermediate");
        check(pixels[2] == (hostOwnsReal ? sentinel : b), "B preserves exact pixels or host-owned sentinel");
        check(pixels[3] == (hostOwnsReal ? sentinel : a), "held prime survives capture and input reuse");
        success(api.Retire(context, prime.token), "retire held prime");
        success(api.Retire(context, pair.token), "retire pair");
        success(api.Reset(context, 1), "reset drained context for identical replay");
        return pixels;
    }
};

int main(int argc, char** argv) {
    try {
        check(argc == 5, "absolute module path, backend, multiplier and real-frame owner required");
        check(std::filesystem::path(argv[1]).is_absolute(), "module path must be absolute");
        const std::string backend = argv[2], owner = argv[4];
        check(backend == "nvidia" || backend == "fidelityfx-full", "known backend");
        const auto multiplier = static_cast<uint32_t>(std::stoul(argv[3]));
        check(multiplier == 2 || multiplier == 3, "2X or 3X required");
        check(owner == "host" || owner == "module", "host or module real-frame ownership required");
        // On failure retain this fixture, its live context and loaded DLL until
        // process exit; never unload code or resources still owned by the GPU.
        auto* fixture = new Fixture(argv[1], backend, multiplier, owner == "host");
        const auto reference = fixture->run(false);
        const auto overwritten = fixture->run(true);
        check(reference == overwritten, "all synthetic and real outputs match despite early input reuse");
        success(fixture->api.Destroy(fixture->context), "destroy drained context");
        fixture->context = nullptr;
        FreeLibrary(fixture->module);
        delete fixture;
        std::printf("PASS: %s %uX %s-owned real frame survives input reuse at consumed boundary\n",
                    backend.c_str(),
                    multiplier,
                    owner.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
