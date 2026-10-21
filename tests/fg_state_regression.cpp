#include <d3d12sdklayers.h>
#include "fg_gpu_helpers.h"

#include <cstdlib>

namespace {
    // This must precede every device creation, including the helper's Gpu.
    struct RequiredValidation {
        ComPtr<ID3D12Debug1> debug;

        RequiredValidation() {
            success(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),
                    "D3D12 debug layer with GPU validation is required; install Windows Graphics Tools");
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
            debug->SetEnableSynchronizedCommandQueueValidation(TRUE);
            std::puts("D3D12 debug layer, GPU-based validation and synchronized queue validation enabled");
        }
    };

    // Match runtime message IDs, never driver-specific wording or source text.
    bool stateMessage(D3D12_MESSAGE_ID id) {
        switch (id) {
        case D3D12_MESSAGE_ID_INVALID_SUBRESOURCE_STATE:
        case D3D12_MESSAGE_ID_RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH:
        case D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INCOMPATIBLE_RESOURCE_STATE:
            return true;
        default:
            return false;
        }
    }

    class StateDiagnostics {
        Gpu& gpu;
        std::ofstream transcript;
        uint64_t nextMessage{}, stateErrors{}, otherErrors{}, warnings{}, validationUnavailable{};
        uint64_t initialDeniedMessages{};

      public:
        explicit StateDiagnostics(Gpu& value) : gpu(value) {
            check(gpu.debugEnabled && gpu.diagnostics, "debug validation and ID3D12InfoQueue are mandatory");
            gpu.diagnostics->ClearStorageFilter();
            gpu.diagnostics->ClearRetrievalFilter();
            initialDeniedMessages = gpu.diagnostics->GetNumMessagesDeniedByStorageFilter();
            success(gpu.diagnostics->SetMessageCountLimit(65536), "retain complete debug diagnostics");
            if (const char* path = std::getenv("VDXR_FG_STATE_LOG")) {
                transcript.open(path, std::ios::binary);
                check(transcript.good(), "open complete state-validation transcript");
            }
        }

        void scan(const char* phase) {
            const auto count = gpu.diagnostics->GetNumStoredMessages();
            for (; nextMessage < count; ++nextMessage) {
                SIZE_T bytes{};
                success(gpu.diagnostics->GetMessage(nextMessage, nullptr, &bytes), "debug message size");
                std::vector<uint8_t> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                success(gpu.diagnostics->GetMessage(nextMessage, message, &bytes), "debug message");
                const bool state = stateMessage(message->ID);
                const bool error = message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
                // Full messages are emitted separately from the state verdict. In
                // particular, FFX SDK warnings cannot masquerade as a RED result.
                std::fprintf(stderr,
                             "D3D12[%s] id=%u severity=%u class=%s: %s\n",
                             phase,
                             static_cast<unsigned>(message->ID),
                             static_cast<unsigned>(message->Severity),
                             state ? "resource-state" : "other",
                             message->pDescription);
                if (transcript.is_open()) {
                    transcript << "D3D12[" << phase << "] id=" << static_cast<unsigned>(message->ID)
                               << " severity=" << static_cast<unsigned>(message->Severity)
                               << " class=" << (state ? "resource-state" : "other") << ": " << message->pDescription
                               << '\n';
                }
                stateErrors += state;
                otherErrors += error && !state;
                warnings += message->Severity == D3D12_MESSAGE_SEVERITY_WARNING && !state;
                validationUnavailable += message->ID == D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_UNSUPPORTED;
            }
            if (transcript.is_open()) {
                transcript.flush();
                check(transcript.good(), "write complete state-validation transcript");
            }
            check(!gpu.diagnostics->GetNumMessagesDiscardedByMessageCountLimit(),
                  "debug messages were discarded; state-validation evidence is incomplete");
            SIZE_T filterBytes{};
            success(gpu.diagnostics->GetStorageFilter(nullptr, &filterBytes), "active storage filter size");
            std::vector<uint8_t> filterStorage(filterBytes);
            auto* filter = reinterpret_cast<D3D12_INFO_QUEUE_FILTER*>(filterStorage.data());
            success(gpu.diagnostics->GetStorageFilter(filter, &filterBytes), "active storage filter");
            check(!filter->DenyList.NumCategories && !filter->DenyList.NumSeverities && !filter->DenyList.NumIDs,
                  "active debug storage filter denies messages; state-validation evidence is incomplete");
            // The counter is cumulative: bootstrap may have filtered INFO before
            // the test cleared the filter. Only new denials invalidate this run.
            check(gpu.diagnostics->GetNumMessagesDeniedByStorageFilter() == initialDeniedMessages,
                  "debug messages were filtered; state-validation evidence is incomplete");
        }

        void clean(const char* phase) {
            scan(phase);
            std::printf("STATE_CHECK[%s]: state=%llu other-errors=%llu warnings=%llu\n",
                        phase,
                        stateErrors,
                        otherErrors,
                        warnings);
            check(!validationUnavailable,
                  "GPU-based validation is unsupported; state-validation evidence is unavailable");
            check(!stateErrors, "STATE_REGRESSION: completed GPU work reported an invalid resource state");
            check(!otherErrors, "NON_STATE_VALIDATION_ERROR: inspect complete D3D12 diagnostics");
        }

        void drain() {
            const auto value = gpu.nextFence++;
            success(gpu.queue->Signal(gpu.fence.Get(), value), "state diagnostic drain signal");
            gpu.wait(gpu.fence.Get(), value);
        }
    };

    // An expected RED must still settle live tickets before unloading the DLL.
    struct ContextScope {
        StateDiagnostics& diagnostics;
        const VDXRFG_ApiV2& api;
        void* handle{};
        std::array<VDXRFG_TicketV1, 2> live{};

        void retire(UINT bank) {
            success(api.Retire(handle, live[bank].token), "retire completed state fixture ticket");
            live[bank] = {};
        }

        void close() {
            if (!handle)
                return;
            diagnostics.drain();
            for (UINT bank = 0; bank < live.size(); ++bank)
                if (live[bank].token)
                    retire(bank);
            success(api.Destroy(handle), "destroy settled state fixture context");
            handle = nullptr;
        }

        ~ContextScope() {
            try {
                close();
                diagnostics.scan("context-cleanup");
            } catch (const std::exception& error) {
                std::fprintf(stderr, "FAIL: safe context cleanup failed: %s\n", error.what());
                try {
                    diagnostics.scan("cleanup-failed");
                } catch (const std::exception& diagnosticError) {
                    std::fprintf(stderr, "FAIL: cannot retrieve cleanup diagnostics: %s\n", diagnosticError.what());
                }
                // Do not unload a DLL or release allocations with uncertain work.
                std::_Exit(2);
            }
        }
    };

    using Snapshot = std::array<std::vector<uint8_t>, 3>;

    void run(HMODULE module, const char* backend, UINT multiplier, bool hostOwned, bool uniform) {
        Gpu gpu;
        StateDiagnostics diagnostics(gpu);
        VDXRFG_ApiV2 api{};
        api.struct_size = sizeof(api);
        api.api_version = VDXRFG_API_VERSION_2;
        const auto getApi = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
        check(getApi != nullptr, "real module exports V2 creation");
        success(getApi(&api), "negotiate state fixture API");

        const bool fidelityFx = std::string(backend) == "fidelityfx-full";
        const UINT flags =
            (fidelityFx ? VDXRFG_CREATE_FIDELITYFX_FULL : 0u) | (hostOwned ? VDXRFG_CREATE_HOST_OWNS_REAL : 0u);
        check((api.supported_create_flags & flags) == flags, "requested backend and B ownership are supported");
        auto input = gpu.texture();
        std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs;
        const std::vector<uint8_t> sentinel(width * height * 4, 0x5a);
        VDXRFG_CreateInfoV2 create{};
        create.struct_size = sizeof(create);
        create.api_version = VDXRFG_API_VERSION_2;
        create.flags = flags;
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
        // All public resources stay COMMON. The regression concerns the module's
        // private render targets, which are exercised through its real draws.
        for (UINT bank = 0; bank < 2; ++bank) {
            for (UINT role = 0; role < 3; ++role) {
                if (role == 1 && multiplier == 2)
                    continue;
                outputs[bank][role] = gpu.texture();
                gpu.transfer(outputs[bank][role].Get(), &sentinel);
                base.outputs[bank][role] = outputs[bank][role].Get();
            }
        }
        ContextScope context{diagnostics, api};
        const auto created = api.Create(&create, &context.handle);
        diagnostics.drain();
        diagnostics.clean("create-complete");
        success(created, "create real backend for GPU state regression");
        check(context.handle != nullptr, "successful create owns a context");

        VDXRFG_SubmitInfoV1 info{};
        info.struct_size = sizeof(info);
        info.api_version = VDXRFG_API_VERSION;
        info.input = input.Get();
        info.display_period_ns = 10000000;
        for (UINT eye = 0; eye < 2; ++eye) {
            auto& view = info.views[eye];
            view.orientation[3] = 1;
            view.position[0] = eye ? .032f : -.032f;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
            view.rect = {eye * eyeWidth, 0, eyeWidth, height};
        }
        std::array<Snapshot, 2> snapshots;
        const auto prepare = [&](uint64_t serial) {
            info.serial = serial;
            info.display_time_ns = 1000000000 + (serial - 1) * multiplier * info.display_period_ns;
            for (UINT role = 0; role < multiplier - 1; ++role) {
                info.fractions[role] = static_cast<float>(role + 1) / multiplier;
                info.target_time_ns[role] = info.display_time_ns - (multiplier - role - 1) * info.display_period_ns;
            }
        };
        const auto submit = [&](uint64_t serial, bool prime) {
            prepare(serial);
            auto pixels = scene(static_cast<UINT>((serial - 1) * 24));
            if (uniform) {
                for (UINT y = 0; y < height; ++y) {
                    for (UINT x = 0; x < width; ++x) {
                        auto* pixel = pixels.data() + (y * width + x) * 4;
                        pixel[0] = pixel[1] = pixel[2] = x < eyeWidth ? 255 : 237;
                        pixel[3] = x < eyeWidth ? 91 : 193;
                    }
                }
            }
            gpu.transfer(input.Get(), &pixels);
            diagnostics.clean("host-input-upload-complete");
            VDXRFG_TicketV1 ticket{};
            const auto submitted = api.Submit(context.handle, &info, &ticket);
            if (ticket.token && ticket.bank < 2)
                context.live[ticket.bank] = ticket;
            if (ticket.output_ready_fence && ticket.output_ready_value)
                gpu.wait(static_cast<ID3D12Fence*>(ticket.output_ready_fence), ticket.output_ready_value);
            else
                diagnostics.drain();
            // GBV messages are asynchronous: inspect only after the output fence.
            diagnostics.clean(prime ? "prime-output-fence" : "pair-output-fence");
            success(submitted, "submit real backend GPU work");
            check(ticket.token && ticket.bank < 2 && ticket.output_ready_fence && ticket.output_ready_value,
                  "completed submit returns a valid output ticket");
            const UINT expectedMask = (hostOwned ? 0u : 4u) | (prime ? 0u : multiplier == 3 ? 3u : 1u);
            check(ticket.prime == static_cast<UINT>(prime) && ticket.role_mask == expectedMask,
                  "prime and pair publish the exact ownership role mask");
            for (UINT role = 0; role < 3; ++role) {
                if (role == 1 && multiplier == 2)
                    continue;
                const auto actual = gpu.transfer(outputs[ticket.bank][role].Get(), nullptr);
                if (role == 2)
                    check(actual == (hostOwned ? sentinel : pixels),
                          "B is exact or host-owned sentinel stays untouched");
                else if (!prime) {
                    if (uniform)
                        check(actual == pixels,
                              "uniform synthesis preserves every RGBA byte, including eye boundaries");
                    else
                        check(actual != sentinel, "real synthetic work replaced the host output sentinel");
                }
                snapshots[ticket.bank][role] = actual;
            }
            check(gpu.transfer(input.Get(), nullptr) == pixels, "input returns to COMMON without pixel changes");
            diagnostics.clean("host-readback-complete");
            return ticket;
        };
        const auto retained = [&](UINT bank) {
            for (UINT role = 0; role < 3; ++role) {
                if ((role == 1 && multiplier == 2) || (!context.live[bank].role_mask && role != 2))
                    continue;
                check(gpu.transfer(outputs[bank][role].Get(), nullptr) == snapshots[bank][role],
                      "retained output bank survives a later submission and remains COMMON");
            }
            diagnostics.clean("retained-bank-readback-complete");
        };

        // Reset packed inputs before any pair has consumed them. This differs
        // from the later reset, where flow outputs have already been sampled.
        for (uint64_t serial = 1; serial <= 2; ++serial) {
            const auto purePrime = submit(serial, true);
            context.retire(purePrime.bank);
            success(api.Reset(context.handle, 0), "reset prime without consuming packed flow input");
        }
        auto oldest = submit(3, true);
        auto newest = submit(4, false);
        check(oldest.bank != newest.bank, "two retained tickets use both output banks");
        retained(oldest.bank);
        prepare(5);
        VDXRFG_TicketV1 busy{};
        check(api.Submit(context.handle, &info, &busy) == VDXRFG_E_BUSY && !busy.token,
              "both retained banks reject work before modifying GPU state");
        diagnostics.drain();
        diagnostics.clean("both-banks-retained");
        for (uint64_t serial = 5; serial <= 10; ++serial) {
            const auto reusedBank = oldest.bank;
            context.retire(reusedBank);
            oldest = newest;
            newest = submit(serial, false);
            check(newest.bank == reusedBank, "new pair reuses the retired output bank");
            retained(oldest.bank);
        }
        context.retire(oldest.bank);
        context.retire(newest.bank);
        success(api.Reset(context.handle, 0), "reset after repeated bank reuse");
        oldest = submit(11, true);
        context.retire(oldest.bank);
        newest = submit(12, false);
        check(newest.bank == oldest.bank, "post-reset pair reuses the prime's private render targets");
        context.retire(newest.bank);
        context.close();
        diagnostics.clean("destroy-complete");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 5 || argc == 6,
              "usage: fg_state_regression MODULE nvidia|fidelityfx-full 2|3 module|host [uniform]");
        check(std::string(argv[2]) == "nvidia" || std::string(argv[2]) == "fidelityfx-full", "known backend");
        check(std::string(argv[3]) == "2" || std::string(argv[3]) == "3", "2X or 3X multiplier");
        check(std::string(argv[4]) == "module" || std::string(argv[4]) == "host", "known real-output owner");
        check(argc == 5 || std::string(argv[5]) == "uniform", "known optional uniform-frame mode");
        RequiredValidation validation;
        const auto module =
            LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load the requested real synthesis DLL");
        // On failure the process retains the DLL until context cleanup has run.
        run(module, argv[2], static_cast<UINT>(std::stoul(argv[3])), std::string(argv[4]) == "host", argc == 6);
        FreeLibrary(module);
        std::printf("PASS: actual GPU resource states %s %sX %s-owned-B %s; prime, both banks, reuse and reset\n",
                    argv[2],
                    argv[3],
                    argv[4],
                    argc == 6 ? "uniform-RGBA" : "motion");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
