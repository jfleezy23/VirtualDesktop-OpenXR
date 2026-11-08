// Actual isolated S/P component test. This does not exercise worker B selection.
#define wmain retainedPendingCopyDeliveryMain
#include "fg_delivery_isolation_regression.cpp"
#undef wmain

namespace {
    void pendingCurrentCopies(const std::filesystem::path& module, unsigned multiplier) {
        Interop producer;
        DeliveryDevice presentation(producer.gpu.device.Get());
        FrameGeneration fg;
        FrameGeneration::InitInfo init{producer.device.Get(),
                                       producer.context.Get(),
                                       producer.gpu.device.Get(),
                                       module,
                                       eyeWidth,
                                       height,
                                       multiplier,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        check(selectDelivery(init, presentation, 0), "independent actual S/P domains are required");
        success(fg.initialize(init), "initialize actual isolated FG graph");
        FrameGenerationCadence cadence;
        FinalProjectionInput input;
        input.serial = 1;
        input.time = 1000000000;
        input.period = 10000000;
        const auto a = scene(0), b = scene(24), c = scene(48);
        input.colour[0] = input.colour[1] = producer.texture(&a);
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
        success(fg.capture(input, prime), "real prime capture");
        producer.gpu.wait(static_cast<ID3D12Fence*>(prime.ticket.output_ready_fence), prime.ticket.output_ready_value);
        producer.retire(fg, prime);
        const auto previous = input.time;
        input.serial = 2;
        input.time += multiplier * input.period;
        input.slots = cadence.plan(
            cadence.reserve(0, previous + input.period, input.period, multiplier, multiplier), previous, true);
        input.colour[0] = input.colour[1] = producer.texture(&b);
        OwnedFrameTransaction current;
        success(fg.capture(input, current), "actual nonprime current ticket");
        producer.gpu.wait(static_cast<ID3D12Fence*>(current.ticket.output_ready_fence),
                          current.ticket.output_ready_value);
        check(!current.ticket.prime && fg.ready(current) && current.slots.count == multiplier,
              "every current module output is physically ready before gating presentation copies");
        const auto currentTicket = current.ticket;
        std::array<ComPtr<ID3D11Texture2D>, 4> outputs;
        outputs[2] = presentation.texture();
        success(fg.copyOutput(current, FrameGenerationRole::real, outputs[2].Get()), "copy finalized current B first");
        presentation.drain();
        check(fg.copyReady(current, FrameGenerationRole::real), "current B consumer is complete before P gate");
        const auto originalB = presentation.read(outputs[2].Get());
        check(originalB == b, "current finalized B pixels are bit-exact before P gate");
        const auto bConsumer = current.consumerValue;

        // This existing helper inserts a real P GPU Wait and exposes a CPU
        // release fence. Never drain or Map P while the gate remains held.
        ProducerHold pending(presentation.device.Get(), presentation.context.Get(), producer.gpu.device.Get());
        for (const unsigned role : {0u, 1u, 3u}) {
            if ((role == 1 && multiplier == 2) || (role == 3 && multiplier != 4))
                continue;
            outputs[role] = presentation.texture();
            success(fg.copyOutput(current, static_cast<FrameGenerationRole>(role), outputs[role].Get()),
                    "queue actual current synthetic copy behind P gate");
        }
        const auto highestConsumer = current.consumerValue;
        const auto last = multiplier == 4   ? FrameGenerationRole::synthetic3
                          : multiplier == 3 ? FrameGenerationRole::synthetic2
                                            : FrameGenerationRole::synthetic1;
        check(highestConsumer > bConsumer && pending.after->GetCompletedValue() == 0 &&
                  fg.copyReady(current, FrameGenerationRole::real) && !fg.copyReady(current, last),
              "B stays complete while the last current synthetic consumer is genuinely pending");

        const auto currentTime = input.time;
        ++input.serial;
        input.time += multiplier * input.period;
        input.slots =
            cadence.plan(cadence.reserve(0, currentTime + input.period, input.period, multiplier, 2 * multiplier),
                         currentTime,
                         true);
        input.colour[0] = input.colour[1] = producer.texture(&c);
        OwnedFrameTransaction next;
        success(fg.capture(input, next), "actual opposite producer capture proceeds while P synthetic copies are held");
        const auto nextTicket = next.ticket;
        check(next.ticket.token && next.ticket.token != currentTicket.token && next.ticket.bank != currentTicket.bank &&
                  next.ticket.previous_serial == currentTicket.current_serial && !next.ticket.prime,
              "actual next pair retains a distinct opposite-bank owner and consecutive lineage");
        check(current.ticket.token == currentTicket.token && current.ticket.bank == currentTicket.bank &&
                  current.consumerValue == highestConsumer && fg.copyReady(current, FrameGenerationRole::real) &&
                  !fg.copyReady(current, last) && pending.after->GetCompletedValue() == 0,
              "opposite capture preserves complete B and pending current synthetic ownership");
        check(fg.retire(current) == VDXRFG_E_BUSY && current.ticket.token == currentTicket.token &&
                  next.ticket.token == nextTicket.token,
              "retirement remains BUSY on the highest current consumer instead of using completed B alone");
        for (unsigned role = 0; role < 4; ++role)
            if (outputs[role])
                check(current.destinations[role] == outputs[role],
                      "all current destination owners survive pending retirement");

        pending.finish(); // CPU signal, independent of the held P GPU queue.
        success(fg.waitForCopy(current, last, 5000), "last actual synthetic consumer completes after CPU release");
        for (unsigned role = 0; role < 4; ++role)
            if (outputs[role])
                check(fg.copyReady(current, static_cast<FrameGenerationRole>(role)),
                      "every current consumer completes after gate release");
        check(presentation.read(outputs[2].Get()) == originalB,
              "opposite capture and pending synthetic copies preserve exact B");
        check(fg.retire(current) == S_OK && !current.ticket.token && next.ticket.token == nextTicket.token,
              "current retires only after all synthetic consumers complete, retaining the next owner");
        producer.gpu.wait(static_cast<ID3D12Fence*>(next.ticket.output_ready_fence), next.ticket.output_ready_value);
        producer.retire(fg, next);
        success(fg.shutdown(), "settled isolated graph shuts down normally");
        producer.gpu.noErrors();
        std::cout << "PENDING_CURRENT_COPY multiplier=" << multiplier
                  << " current_outputs_full_ready=1 B_ready_before_gate=1 last_S_ready_while_gate_held=0"
                  << " opposite_capture_accepted=1 distinct_bank_owners=1 retire_while_gate_held_BUSY=1"
                  << " all_S_ready_after_release=1 retire_after_release_S_OK=1 retained_B_pixels=1"
                  << " component_only=1 worker_B_selection_tested=0\n";
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "actual module DLL and multiplier 2|3|4 required");
        const auto multiplier = static_cast<unsigned>(std::stoul(argv[2]));
        check(multiplier >= 2 && multiplier <= 4, "known multiplier");
        pendingCurrentCopies(std::filesystem::absolute(argv[1]), multiplier);
        std::cout << "PASS: completed B cannot retire pending current synthetic copies\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
