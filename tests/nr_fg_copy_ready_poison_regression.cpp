// Private, isolated-process fault test; never edits the frozen source tree.
// Reuse real two-device FG capture/copy/readback helpers, not a scheduler model.
#define wmain retainedPoisonDeliveryMain
#include "fg_delivery_isolation_regression.cpp"
#undef wmain
#include <condition_variable>
#include <mutex>

namespace {
    struct InvalidBankSubmit {
        inline static VDXRFG_GetApiV4Fn originalApi{};
        inline static decltype(VDXRFG_ApiV4::Submit) originalSubmit{};
        inline static InvalidBankSubmit* active{};
        HMODULE module{};
        std::mutex mutex;
        std::condition_variable changed;
        uint64_t armedSerial{};
        unsigned currentBank{};
        bool reached{}, release{}, genuineOpposite{};
        VDXRFG_TicketV1 genuineTicket{};
        HRESULT genuineResult{E_PENDING};

        static HRESULT WINAPI submit(void* context, const VDXRFG_SubmitInfoV3* info, VDXRFG_TicketV1* ticket) noexcept {
            const auto result = originalSubmit(context, info, ticket);
            auto& self = *active;
            if (!info || info->serial != self.armedSerial)
                return result;
            std::unique_lock lock(self.mutex);
            self.genuineResult = result;
            if (SUCCEEDED(result) && ticket) {
                self.genuineTicket = *ticket;
                self.genuineOpposite = ticket->token && !ticket->prime && ticket->bank < 2 &&
                                       ticket->bank != self.currentBank && ticket->current_serial == self.armedSerial;
            }
            self.reached = true;
            self.changed.notify_all();
            self.changed.wait(lock, [&] { return self.release; });
            // The real module accepted the opposite ticket. Corrupt only its
            // returned bank so capture's guard must poison BEFORE indexing or
            // writing any bank metadata. Keep the genuine token/fence alive.
            if (self.genuineOpposite)
                ticket->bank = 2;
            return result;
        }
        static HRESULT WINAPI api(VDXRFG_ApiV4* value) noexcept {
            const auto result = originalApi(value);
            if (SUCCEEDED(result) && value && value->Submit) {
                originalSubmit = value->Submit;
                value->Submit = submit;
            }
            return result;
        }
        explicit InvalidBankSubmit(const std::filesystem::path& path) {
            module = LoadLibraryExW(
                path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            check(module != nullptr, "load actual FG module for bank fault");
            originalApi = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(module, "VDXRFG_GetApiV4"));
            check(originalApi != nullptr, "actual V4 export for narrow Submit fault");
            active = this;
            check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                      DetourAttach(reinterpret_cast<PVOID*>(&originalApi), api) == NO_ERROR &&
                      DetourTransactionCommit() == NO_ERROR,
                  "attach actual V4 negotiation observer");
        }
        void unblock() {
            std::lock_guard lock(mutex);
            release = true;
            changed.notify_all();
        }
        ~InvalidBankSubmit() {
            unblock();
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalApi), api) || DetourTransactionCommit())
                std::_Exit(3);
            active = nullptr;
            FreeLibrary(module);
        }
    };
    struct JoinProducer {
        InvalidBankSubmit& fault;
        std::thread& producer;
        ~JoinProducer() {
            fault.unblock();
            if (producer.joinable())
                producer.join();
        }
    };

    void poisonCurrentReadiness(const std::filesystem::path& module, unsigned multiplier) {
        Interop producer;
        DeliveryDevice presentation(producer.gpu.device.Get());
        InvalidBankSubmit fault(module);
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
        check(!current.ticket.prime && current.slots.count == multiplier, "current group has every requested role");
        std::array<ComPtr<ID3D11Texture2D>, 4> outputs;
        std::array<std::vector<uint8_t>, 4> retainedPixels;
        for (const unsigned role : {2u, 0u, 1u, 3u}) {
            if ((role == 1 && multiplier == 2) || (role == 3 && multiplier != 4))
                continue;
            outputs[role] = presentation.texture();
            success(fg.copyOutput(current, static_cast<FrameGenerationRole>(role), outputs[role].Get()),
                    "stage an actual current presentation copy");
        }
        presentation.drain();
        const auto last = multiplier == 4   ? FrameGenerationRole::synthetic3
                          : multiplier == 3 ? FrameGenerationRole::synthetic2
                                            : FrameGenerationRole::synthetic1;
        check(fg.copyReady(current, last), "last queued actual current S is physically complete before fault");
        for (unsigned role = 0; role < 4; ++role)
            if (outputs[role])
                retainedPixels[role] = presentation.read(outputs[role].Get());
        check(retainedPixels[2] == b, "current real B presentation pixels are bit-exact before fault");

        const auto currentTicket = current.ticket;
        const auto currentTime = input.time;
        ++input.serial;
        input.time += multiplier * input.period;
        input.slots =
            cadence.plan(cadence.reserve(0, currentTime + input.period, input.period, multiplier, 2 * multiplier),
                         currentTime,
                         true);
        input.colour[0] = input.colour[1] = producer.texture(&c);
        fault.armedSerial = input.serial;
        fault.currentBank = current.ticket.bank;
        OwnedFrameTransaction opposite;
        HRESULT captured{E_PENDING};
        std::thread nextProducer([&] { captured = fg.capture(input, opposite); });
        JoinProducer join{fault, nextProducer};
        {
            std::unique_lock lock(fault.mutex);
            check(fault.changed.wait_for(lock, std::chrono::seconds(5), [&] { return fault.reached; }),
                  "real opposite Submit reaches fault boundary");
            check(fault.genuineResult == S_OK && fault.genuineOpposite,
                  "real module accepted a distinct genuine opposite-bank ticket before bank corruption");
        }
        check(fg.copyReady(current, last), "completed current copies remain observable during opposite-bank capture");
        fault.unblock();
        nextProducer.join();
        check(captured == VDXRFG_E_POISONED && opposite.ticket.bank == 2 && opposite.ticket.token,
              "capture rejects the invalid returned bank through its pre-write poison guard");
        const auto completed = static_cast<ID3D12Fence*>(currentTicket.output_ready_fence)->GetCompletedValue();
        check(completed != UINT64_MAX && completed >= currentTicket.output_ready_value,
              "current genuine output fence remains complete after host poison");
        check(!fg.copyReady(current, last) && !fg.initialized(),
              "poisoned graph fails current copy readiness closed despite completed real copies");
        check(current.ticket.token == currentTicket.token && current.ticket.bank == currentTicket.bank &&
                  current.ticket.generation == currentTicket.generation,
              "invalid opposite bank never mutates current ownership identity");
        for (unsigned role = 0; role < 4; ++role)
            if (outputs[role])
                check(presentation.read(outputs[role].Get()) == retainedPixels[role],
                      "poison leaves every physically copied current role unchanged");
        check(fg.shutdown() == VDXRFG_E_POISONED, "unknown-completion poison intentionally retains the actual graph");
        std::cout << "POISON_FAIL_CLOSED multiplier=" << multiplier
                  << " genuine_opposite_bank=" << fault.genuineTicket.bank
                  << " invalid_returned_bank=" << opposite.ticket.bank
                  << " ready_before=1 ready_during_capture=1 ready_after=0 completed_current_fence=1 retained_pixels=1"
                  << " graph_retention_expected=1\n";
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "actual module DLL and multiplier 2|3|4 required");
        const auto multiplier = static_cast<unsigned>(std::stoul(argv[2]));
        check(multiplier >= 2 && multiplier <= 4, "known multiplier");
        poisonCurrentReadiness(std::filesystem::absolute(argv[1]), multiplier);
        std::cout << "PASS: actual completed current copies fail closed after opposite-capture poison\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
