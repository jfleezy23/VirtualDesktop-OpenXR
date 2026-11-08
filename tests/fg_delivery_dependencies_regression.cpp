// Test completion dependencies only; artificial gates do not measure gameplay latency.
#define wmain retainedDeliveryProofMain
#include "fg_delivery_isolation_regression.cpp"
#undef wmain

extern "C" size_t deliveryQueueSignalSlot();

namespace {
    struct QueueSignalGate {
        using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
        static inline Signal original{};
        static inline thread_local QueueSignalGate* active{};
        ComPtr<ID3D12Fence> gate;
        ComPtr<ID3D12Fence> target;
        uint64_t afterValue{};
        unsigned hits{};
        static HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
            if (active && active->target.Get() == fence && value > active->afterValue && !active->hits) {
                ++active->hits;
                const auto result = queue->Wait(active->gate.Get(), 1);
                if (FAILED(result))
                    return result;
            }
            return original(queue, fence, value);
        }
        explicit QueueSignalGate(Gpu& gpu) {
            check(!active, "one queue-signal gate");
            success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "owned signal gate");
            original =
                reinterpret_cast<Signal>((*reinterpret_cast<void***>(gpu.queue.Get()))[deliveryQueueSignalSlot()]);
            check(DetourTransactionBegin() == NO_ERROR, "begin signal hook");
            if (DetourUpdateThread(GetCurrentThread()) || DetourAttach(reinterpret_cast<PVOID*>(&original), signal)) {
                DetourTransactionAbort();
                throw std::runtime_error("attach signal hook");
            }
            check(DetourTransactionCommit() == NO_ERROR, "commit signal hook");
            active = this;
        }
        void release() {
            success(gate->Signal(1), "CPU release never queues behind held work");
        }
        ~QueueSignalGate() {
            // Release even when a tested assertion fails, before the FG owners unwind.
            (void)gate->Signal(1);
            active = nullptr;
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), signal) || DetourTransactionCommit())
                std::terminate();
        }
    };

    void qualifyDependencies(const std::filesystem::path& module, unsigned multiplier, bool consumed) {
        Interop hw;
        DeliveryDevice delivery(hw.gpu.device.Get());
        FrameGeneration legacy, isolated;
        FrameGeneration::InitInfo info{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       module,
                                       eyeWidth,
                                       height,
                                       multiplier,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        success(legacy.initialize(info), "legacy dependency reference");
        check(selectDelivery(info, delivery, 0), "optional delivery fields exist");
        success(isolated.initialize(info), "isolated dependency reference");
        QueueSignalGate gate(hw.gpu);
        FrameGenerationCadence cadence;
        FinalProjectionInput input;
        input.serial = 1;
        input.period = 10000000;
        input.time = 1000000000;
        input.slots = cadence.plan(
            cadence.reserve(0, input.time - (multiplier - 1) * input.period, input.period, multiplier, 0), 0, false);
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
        OwnedFrameTransaction primes[2];
        success(legacy.capture(input, primes[0]), "legacy dependency prime");
        success(isolated.capture(input, primes[1]), "isolated dependency prime");
        hw.gpu.wait(static_cast<ID3D12Fence*>(primes[0].ticket.output_ready_fence),
                    primes[0].ticket.output_ready_value);
        hw.gpu.wait(static_cast<ID3D12Fence*>(primes[1].ticket.output_ready_fence),
                    primes[1].ticket.output_ready_value);
        gate.target = static_cast<ID3D12Fence*>(consumed ? primes[1].ticket.input_consumed_fence
                                                         : primes[1].ticket.output_ready_fence);
        gate.afterValue = consumed ? primes[1].ticket.input_consumed_value : primes[1].ticket.output_ready_value;
        hw.retire(legacy, primes[0]);
        hw.retire(isolated, primes[1]);
        const auto previous = input.time;
        ++input.serial;
        input.time += multiplier * input.period;
        input.slots = cadence.plan(
            cadence.reserve(0, previous + input.period, input.period, multiplier, multiplier), previous, true);
        input.colour[0] = input.colour[1] = hw.texture(&b);
        OwnedFrameTransaction pairs[2];
        success(legacy.capture(input, pairs[0]), "legacy dependency pair");
        hw.gpu.wait(static_cast<ID3D12Fence*>(pairs[0].ticket.output_ready_fence), pairs[0].ticket.output_ready_value);
        auto legacyS1 = hw.texture();
        success(legacy.copyOutput(pairs[0], FrameGenerationRole::synthetic1, legacyS1.Get()),
                "legacy exact S1 reference before artificial hold");
        const auto expectedS1 = hw.read(legacyS1.Get());
        success(isolated.capture(input, pairs[1]), "accepted isolated pair before gated signal");
        check(gate.hits == 1, "gate intercepts exact module-owned completion signal once");
        check(gate.target.Get() == static_cast<ID3D12Fence*>(consumed ? pairs[1].ticket.input_consumed_fence
                                                                      : pairs[1].ticket.output_ready_fence) &&
                  gate.afterValue <
                      (consumed ? pairs[1].ticket.input_consumed_value : pairs[1].ticket.output_ready_value),
              "accepted ticket identifies the exact gated fence and later value");
        const auto full = static_cast<ID3D12Fence*>(pairs[1].ticket.output_ready_fence);
        check(full->GetCompletedValue() < pairs[1].ticket.output_ready_value, "full-ready signal genuinely pending");
        auto real = delivery.texture();
        success(isolated.copyOutput(pairs[1], FrameGenerationRole::real, real.Get()), "queue isolated B dependency");
        if (consumed) {
            check(isolated.waitForCopy(pairs[1], FrameGenerationRole::real, 10) == VDXRFG_E_BUSY,
                  "canonical B must wait for pending COMMON input-consumed handoff");
            auto legacyReal = hw.texture();
            success(legacy.copyOutput(pairs[0], FrameGenerationRole::real, legacyReal.Get()),
                    "legacy direct B fallback");
            check(hw.read(legacyReal.Get()) == b && !isolated.copyReady(pairs[1], FrameGenerationRole::real),
                  "legacy direct B can finish while isolated canonical B remains FG-input-dependent");
        } else {
            success(isolated.waitForCopy(pairs[1], FrameGenerationRole::real, 1000), "B does not wait for full output");
            check(delivery.read(real.Get()) == b && full->GetCompletedValue() < pairs[1].ticket.output_ready_value,
                  "exact B delivered while complete-batch signal stays held");
            auto s1 = delivery.texture();
            success(isolated.copyOutput(pairs[1], FrameGenerationRole::synthetic1, s1.Get()),
                    "S1 uses first-ready fence");
            if (multiplier != 4) {
                // This default NVIDIA profile uses full readiness for 2x
                // and jointly composed 3x. Only its 4x path has split S1.
                check(isolated.waitForCopy(pairs[1], FrameGenerationRole::synthetic1, 10) == VDXRFG_E_BUSY,
                      "NVIDIA 2x/3x correctly retain whole-batch synthetic readiness");
            } else {
                success(isolated.waitForCopy(pairs[1], FrameGenerationRole::synthetic1, 1000),
                        "split-ready S1 bypasses full-ready marker");
                const auto pixels = delivery.read(s1.Get());
                check(pixels == expectedS1 && pixels != a && pixels != b &&
                          full->GetCompletedValue() < pairs[1].ticket.output_ready_value,
                      "byte-exact generated S1 delivered before whole-batch marker");
            }
        }
        gate.release();
        success(isolated.waitForCopy(pairs[1], FrameGenerationRole::real, 1000), "B completes after required handoff");
        check(delivery.read(real.Get()) == b, "completion preserves exact B");
        hw.gpu.wait(full, pairs[1].ticket.output_ready_value);
        hw.retire(legacy, pairs[0]);
        hw.retire(isolated, pairs[1]);
        success(legacy.shutdown(), "legacy dependency shutdown");
        success(isolated.shutdown(), "isolated dependency shutdown");
        hw.gpu.noErrors();
        std::cout << "DEPENDENCY_PROOF {\"multiplier\":" << multiplier << ",\"gate\":\""
                  << (consumed ? "input-consumed" : "full-ready")
                  << "\",\"artificial_gate\":true,\"gameplay_latency_measured\":false}\n";
    }
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4, "usage: module multiplier consumed|full");
        const unsigned multiplier = std::stoul(argv[2]);
        const std::wstring mode = argv[3];
        check(multiplier >= 2 && multiplier <= 4 && (mode == L"consumed" || mode == L"full"), "dependency case");
        qualifyDependencies(std::filesystem::absolute(argv[1]), multiplier, mode == L"consumed");
        std::puts("PASS: isolated output completion dependencies characterized");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
