#include "fg_gpu_helpers.h"
#include "../virtualdesktop-openxr/frame_generation.h"

#include <d3d11_4.h>
#include <chrono>
#include <thread>

using namespace virtualdesktop_openxr;

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
    if (scenario == "invalid-plan") {
        input.slots.count = 0;
        check(fg.capture(input, prime) == VDXRFG_E_REJECTED && !prime.ticket.token,
              "invalid slot count must reject before accepting a GPU transaction");
        success(fg.shutdown(), "shutdown after rejected plan");
        return;
    }
    success(fg.capture(input, prime), "capture finalized D3D11 stereo pixels");
    hw.await(fg, prime);
    check(fg.shutdown() == VDXRFG_E_BUSY && fg.reset(0) == VDXRFG_E_BUSY, "live interop ticket retains graph");
    auto output = hw.texture();
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
