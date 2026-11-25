#include "pch.h"
#include "NrBeforeSrHooks.h"

#include <atomic>
#include <cstring>
#include <intrin.h>

namespace virtualdesktop_openxr::nr_before_sr {
    namespace {
        constexpr size_t executeSlot = 10;       // ID3D12CommandQueue::ExecuteCommandLists
        constexpr size_t resetSlot = 10;         // ID3D12GraphicsCommandList::Reset
        constexpr size_t allocatorResetSlot = 8; // ID3D12CommandAllocator::Reset

        std::mutex installMutex;
        std::atomic<HookSink*> activeSink{};
        void* executeTarget{};
        void* resetTarget{};
        void* allocatorResetTarget{};
        decltype(&NVSDK_NGX_D3D12_CreateFeature) nativeCreate{};
        decltype(&NVSDK_NGX_D3D12_EvaluateFeature_C) nativeEvaluate{};
        decltype(&NVSDK_NGX_D3D12_ReleaseFeature) nativeRelease{};
        // The SDK exposes this prototype only with NGX_ENABLE_DEPRECATED_SHUTDOWN.
        using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)();
        ShutdownFn nativeShutdown{};
        decltype(&NVSDK_NGX_D3D12_Shutdown1) nativeShutdown1{};
        ExecuteFn nativeExecute{};
        ResetFn nativeReset{};
        AllocatorResetFn nativeAllocatorReset{};
        thread_local bool insideGlobalShutdown{};

        using List = ID3D12GraphicsCommandList;
        using Pipeline = ID3D12PipelineState;
        using Tracker = PipelineStateTracker<List, Pipeline>;
        std::atomic<Tracker*> pipelineTracker{};
        using SetPipelineFn = void(STDMETHODCALLTYPE*)(List*, Pipeline*);
        SetPipelineFn nativeClearState{}, nativeSetPipeline{};
        void(STDMETHODCALLTYPE* nativeBundle)(List*, List*){};
        void(STDMETHODCALLTYPE* nativeProtected)(ID3D12GraphicsCommandList3*, ID3D12ProtectedResourceSession*){};
        using MetaFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, ID3D12MetaCommand*, const void*, SIZE_T);
        MetaFn nativeInitializeMeta{}, nativeExecuteMeta{};
        void(STDMETHODCALLTYPE* nativeSetStateObject)(ID3D12GraphicsCommandList4*, ID3D12StateObject*){};
        void(STDMETHODCALLTYPE* nativeSetProgram)(ID3D12GraphicsCommandList10*, const D3D12_SET_PROGRAM_DESC*){};

        void ObserveState(List* list, Pipeline* state = nullptr) noexcept {
            if (auto* tracker = pipelineTracker.load(std::memory_order_acquire))
                tracker->set(list, state);
        }

        void STDMETHODCALLTYPE ClearState(List* list, Pipeline* state) {
            nativeClearState(list, state);
            ObserveState(list, state);
        }
        void STDMETHODCALLTYPE SetPipeline(List* list, Pipeline* state) {
            nativeSetPipeline(list, state);
            ObserveState(list, state);
        }
        void STDMETHODCALLTYPE ExecuteBundle(List* list, List* bundle) {
            nativeBundle(list, bundle);
            ObserveState(list);
        }
        void STDMETHODCALLTYPE SetProtected(ID3D12GraphicsCommandList3* list, ID3D12ProtectedResourceSession* session) {
            nativeProtected(list, session);
            ObserveState(list);
        }
        void STDMETHODCALLTYPE InitializeMeta(ID3D12GraphicsCommandList4* list,
                                              ID3D12MetaCommand* command,
                                              const void* data,
                                              SIZE_T size) {
            nativeInitializeMeta(list, command, data, size);
            ObserveState(list);
        }
        void STDMETHODCALLTYPE ExecuteMeta(ID3D12GraphicsCommandList4* list,
                                           ID3D12MetaCommand* command,
                                           const void* data,
                                           SIZE_T size) {
            nativeExecuteMeta(list, command, data, size);
            ObserveState(list);
        }
        void STDMETHODCALLTYPE SetStateObject(ID3D12GraphicsCommandList4* list, ID3D12StateObject* state) {
            nativeSetStateObject(list, state);
            ObserveState(list);
        }
        void STDMETHODCALLTYPE SetProgram(ID3D12GraphicsCommandList10* list, const D3D12_SET_PROGRAM_DESC* desc) {
            nativeSetProgram(list, desc);
            ObserveState(list);
        }

        // SDK 10.0.26100 ID3D12GraphicsCommandList[0..10] vtables. Require *all* exposed
        // graphics aliases to have the same address. A tear-off could otherwise miss a state
        // setter even when the base Reset and SetPipelineState methods were intercepted.
        constexpr std::array<size_t, 8> stateSlots{11, 25, 27, 67, 70, 71, 75, 84};
        constexpr std::array<unsigned, 8> stateRevisions{0, 0, 0, 3, 4, 4, 4, 10};
        using StateEntries = std::array<void*, stateSlots.size()>;
        StateEntries installedStateEntries{};

        bool ResolveStateEntries(List* list, StateEntries& entries) noexcept {
            const IID* const revisions[] = {&__uuidof(List),
                                            &__uuidof(ID3D12GraphicsCommandList1),
                                            &__uuidof(ID3D12GraphicsCommandList2),
                                            &__uuidof(ID3D12GraphicsCommandList3),
                                            &__uuidof(ID3D12GraphicsCommandList4),
                                            &__uuidof(ID3D12GraphicsCommandList5),
                                            &__uuidof(ID3D12GraphicsCommandList6),
                                            &__uuidof(ID3D12GraphicsCommandList7),
                                            &__uuidof(ID3D12GraphicsCommandList8),
                                            &__uuidof(ID3D12GraphicsCommandList9),
                                            &__uuidof(ID3D12GraphicsCommandList10)};
            unsigned highest = 0;
            for (unsigned i = 0; i != std::size(revisions); ++i) {
                ComPtr<IUnknown> alias;
                const auto hr = list->QueryInterface(*revisions[i], reinterpret_cast<void**>(alias.GetAddressOf()));
                if (hr == E_NOINTERFACE && i != 0)
                    continue;
                if (FAILED(hr) || alias.Get() != static_cast<IUnknown*>(list))
                    return false;
                highest = i;
            }
            auto* table = *reinterpret_cast<void***>(list);
            for (size_t i = 0; i != entries.size(); ++i) {
                entries[i] = stateRevisions[i] <= highest ? table[stateSlots[i]] : nullptr;
                if (stateRevisions[i] <= highest && !entries[i])
                    return false;
            }
            return true;
        }

        NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* list,
                                           NVSDK_NGX_Feature feature,
                                           NVSDK_NGX_Parameter* params,
                                           NVSDK_NGX_Handle** handle) {
            const auto result = nativeCreate(list, feature, params, handle);
            ObserveState(list); // Opaque vendor work can change state outside public list methods.
            if (NVSDK_NGX_SUCCEED(result) && handle && *handle) {
                if (auto* sink = activeSink.load(std::memory_order_acquire))
                    sink->featureCreated(list, feature, params, *handle);
            }
            return result;
        }

        NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list,
                                             const NVSDK_NGX_Handle* handle,
                                             const NVSDK_NGX_Parameter* params,
                                             PFN_NVSDK_NGX_ProgressCallback_C callback) {
            auto* sink = activeSink.load(std::memory_order_acquire);
            if (!sink)
                return nativeEvaluate(list, handle, params, callback);
            EvaluationReceipt receipt{};
            // The hash-pinned Hubris SR helper tail-calls Evaluate_C after setting the mutable per-feature map.
            // The caller's return address is the instruction following FNGXD3D12RHI::ExecuteDLSS's helper call.
            static const auto qualifiedReturn = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x131e9ae;
            receipt.qualifiedColorCaller =
                !callback && reinterpret_cast<uintptr_t>(_ReturnAddress()) == qualifiedReturn;
            sink->beforeEvaluate(list, handle, params, receipt);
            bool succeeded = false;
            auto finish = wil::scope_exit([&] { sink->afterEvaluate(handle, receipt, succeeded); });
            const auto result = EvaluateWithResolvedColor(params, receipt.color, [&] {
                const auto nativeResult = nativeEvaluate(list, handle, params, callback);
                ObserveState(list);
                return nativeResult;
            });
            succeeded = NVSDK_NGX_SUCCEED(result);
            return result;
        }

        NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* handle) {
            auto* sink = activeSink.load(std::memory_order_acquire);
            const unsigned id = sink ? HubrisHandleId(handle) : 0;
            const auto result = nativeRelease(handle);
            if (sink && handle && NVSDK_NGX_SUCCEED(result))
                sink->featureReleased(handle, id);
            return result;
        }

        NVSDK_NGX_Result NVSDK_CONV Shutdown() {
            // Hubris's no-argument wrapper calls Shutdown1(nullptr). A single global
            // notification closes every device before either native function executes.
            if (auto* sink = activeSink.load(std::memory_order_acquire); sink && !insideGlobalShutdown) {
                insideGlobalShutdown = true;
                auto restore = wil::scope_exit([] { insideGlobalShutdown = false; });
                sink->beforeNgxShutdown(nullptr);
                return nativeShutdown();
            }
            return nativeShutdown();
        }

        NVSDK_NGX_Result NVSDK_CONV Shutdown1(ID3D12Device* device) {
            if (!insideGlobalShutdown) {
                if (auto* sink = activeSink.load(std::memory_order_acquire))
                    sink->beforeNgxShutdown(device);
            }
            return nativeShutdown1(device);
        }

        void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
            if (auto* sink = activeSink.load(std::memory_order_acquire))
                sink->execute(queue, count, lists, nativeExecute);
            else
                nativeExecute(queue, count, lists);
        }

        HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* list,
                                        ID3D12CommandAllocator* allocator,
                                        ID3D12PipelineState* pso) {
            if (auto* sink = activeSink.load(std::memory_order_acquire))
                return sink->reset(list, allocator, pso, nativeReset);
            return nativeReset(list, allocator, pso);
        }

        HRESULT STDMETHODCALLTYPE ResetAllocator(ID3D12CommandAllocator* allocator) {
            if (auto* sink = activeSink.load(std::memory_order_acquire))
                return sink->resetAllocator(allocator, nativeAllocatorReset);
            return nativeAllocatorReset(allocator);
        }

        bool IsExecutableImageCode(const void* address, HMODULE owner = nullptr) {
            MEMORY_BASIC_INFORMATION page{};
            if (!address || !VirtualQuery(address, &page, sizeof(page)) || page.Type != MEM_IMAGE ||
                page.State != MEM_COMMIT || (page.Protect & PAGE_GUARD) || (owner && page.AllocationBase != owner))
                return false;
            constexpr DWORD executable =
                PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            return (page.Protect & executable) != 0;
        }

        // The caller already matched the build. Resolve the named EAT entry in that
        // mapped executable, rejecting forwarder strings, including same-image aliases.
        void* DirectExport(HMODULE image, const char* name) {
            const auto base = reinterpret_cast<const unsigned char*>(image);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
                return nullptr;
            const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
            const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
            const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
            for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
                if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name))
                    continue;
                if (ordinals[i] >= exports->NumberOfFunctions)
                    return nullptr;
                const DWORD rva = functions[ordinals[i]];
                if (!rva || rva >= nt->OptionalHeader.SizeOfImage ||
                    (rva >= directory.VirtualAddress && rva - directory.VirtualAddress < directory.Size))
                    return nullptr;
                auto* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(image) + rva);
                return IsExecutableImageCode(target, image) ? target : nullptr;
            }
            return nullptr;
        }

        struct HookMethod {
            void** original;
            void* hook;
            void* target;
            template <class Fn>
            HookMethod(Fn& trampoline, Fn replacement, void* entry)
                : original(reinterpret_cast<void**>(&trampoline)), hook(reinterpret_cast<void*>(replacement)),
                  target(entry ? DetourCodeFromPointer(entry, nullptr) : nullptr) {
                trampoline = reinterpret_cast<Fn>(entry);
            }
            bool attach() const noexcept {
                void* realTarget{};
                return DetourAttachEx(original, hook, nullptr, &realTarget, nullptr) == NO_ERROR &&
                       realTarget == target;
            }
        };

        // Collect handles before suspending any threads. Detours can then relocate
        // instruction pointers already inside the instructions it is about to replace.
        bool CollectThreads(std::vector<wil::unique_handle>& threads) {
            wil::unique_hfile snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            THREADENTRY32 entry{sizeof(entry)};
            if (!snapshot || !Thread32First(snapshot.get(), &entry))
                return false;
            const auto process = GetCurrentProcessId();
            const auto self = GetCurrentThreadId();
            do {
                if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
                    constexpr DWORD rights = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION;
                    wil::unique_handle thread(OpenThread(rights, FALSE, entry.th32ThreadID));
                    if (thread) {
                        const auto owner = GetProcessIdOfThread(thread.get());
                        if (!owner)
                            return false;
                        if (owner == process) // the thread ID could have been recycled
                            threads.push_back(std::move(thread));
                    } else if (GetLastError() != ERROR_INVALID_PARAMETER) // exited after the snapshot
                        return false;
                }
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snapshot.get(), &entry));
            return GetLastError() == ERROR_NO_MORE_FILES;
        }
    } // namespace

    unsigned HubrisHandleId(const NVSDK_NGX_Handle* handle) noexcept {
        // Public headers leave it opaque. The admitted Hubris PDB type 0x1C1512
        // describes one unsigned Id at offset zero. Never read it after release.
        unsigned id{};
        if (handle)
            std::memcpy(&id, handle, sizeof(id));
        return id;
    }

    bool ObservesListReset(ID3D12GraphicsCommandList* list) noexcept {
        return activeSink.load(std::memory_order_acquire) && list &&
               (*reinterpret_cast<void***>(list))[resetSlot] == resetTarget;
    }

    bool ObservesAllocatorReset(ID3D12CommandAllocator* allocator) noexcept {
        return activeSink.load(std::memory_order_acquire) && allocator &&
               (*reinterpret_cast<void***>(allocator))[allocatorResetSlot] == allocatorResetTarget;
    }

    void TrackPipelineReset(List* list, std::uint64_t generation, Pipeline* state) noexcept {
        if (auto* tracker = pipelineTracker.load(std::memory_order_acquire)) {
            tracker->reset(list, generation, state, [](List* candidate) {
                StateEntries entries{};
                return candidate->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && ObservesListReset(candidate) &&
                       ResolveStateEntries(candidate, entries) && entries == installedStateEntries;
            });
        }
    }

    ComPtr<Pipeline> CapturePipelineState(List* list, std::uint64_t generation) noexcept {
        auto* tracker = pipelineTracker.load(std::memory_order_acquire);
        return tracker ? tracker->capture(list, generation) : ComPtr<Pipeline>{};
    }

    bool InstallHooks(ID3D12CommandQueue* queue,
                      ID3D12GraphicsCommandList* probeList,
                      ID3D12CommandAllocator* probeAllocator,
                      HookSink& sink,
                      bool trackPipelineState) noexcept {
        try {
            if (!queue || !probeList || !probeAllocator)
                return false;
            std::scoped_lock lock(installMutex);
            auto* execute = (*reinterpret_cast<void***>(queue))[executeSlot];
            auto* reset = (*reinterpret_cast<void***>(probeList))[resetSlot];
            auto* allocatorReset = (*reinterpret_cast<void***>(probeAllocator))[allocatorResetSlot];
            StateEntries states{};
            if (trackPipelineState && !ResolveStateEntries(probeList, states))
                return false;
            if (auto* installed = activeSink.load(std::memory_order_acquire))
                return installed == &sink && execute == executeTarget && reset == resetTarget &&
                       allocatorReset == allocatorResetTarget && states == installedStateEntries;

            const auto image = GetModuleHandleW(nullptr);
            if (!image || !IsExecutableImageCode(execute) || !IsExecutableImageCode(reset) ||
                !IsExecutableImageCode(allocatorReset) || std::any_of(states.begin(), states.end(), [](void* entry) {
                    return entry && !IsExecutableImageCode(entry);
                }))
                return false;
            std::vector<HookMethod> methods{
                {nativeCreate, Create, DirectExport(image, "NVSDK_NGX_D3D12_CreateFeature")},
                {nativeEvaluate, Evaluate, DirectExport(image, "NVSDK_NGX_D3D12_EvaluateFeature_C")},
                {nativeRelease, Release, DirectExport(image, "NVSDK_NGX_D3D12_ReleaseFeature")},
                {nativeShutdown, Shutdown, DirectExport(image, "NVSDK_NGX_D3D12_Shutdown")},
                {nativeShutdown1, Shutdown1, DirectExport(image, "NVSDK_NGX_D3D12_Shutdown1")},
                {nativeExecute, Execute, execute},
                {nativeReset, Reset, reset},
                {nativeAllocatorReset, ResetAllocator, allocatorReset}};
            if (trackPipelineState) {
                methods.emplace_back(nativeClearState, ClearState, states[0]);
                methods.emplace_back(nativeSetPipeline, SetPipeline, states[1]);
                methods.emplace_back(nativeBundle, ExecuteBundle, states[2]);
                if (states[3])
                    methods.emplace_back(nativeProtected, SetProtected, states[3]);
                if (states[4]) {
                    methods.emplace_back(nativeInitializeMeta, InitializeMeta, states[4]);
                    methods.emplace_back(nativeExecuteMeta, ExecuteMeta, states[5]);
                    methods.emplace_back(nativeSetStateObject, SetStateObject, states[6]);
                }
                if (states[7])
                    methods.emplace_back(nativeSetProgram, SetProgram, states[7]);
            }
            // A forwarding layer can use the same stub for several COM methods.
            // Installing two different ABIs on one entry would corrupt the transaction.
            for (size_t i = 0; i < methods.size(); ++i) {
                const auto target = methods[i].target;
                if (!IsExecutableImageCode(target, i < 5 ? image : nullptr) ||
                    std::any_of(methods.begin(), methods.begin() + i, [=](auto& m) { return m.target == target; }))
                    return false;
            }

            // Keep the consumer DLL and the COM implementations resident for the
            // lifetime of committed trampolines. Failed attempts release all refs.
            std::vector<wil::unique_hmodule> modules(methods.size() - 4);
            for (size_t i = 0; i < modules.size(); ++i) {
                const auto address = i ? methods[i + 4].target : reinterpret_cast<void*>(&InstallHooks);
                if (!GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), modules[i].put()))
                    return false;
            }
            auto tracker = trackPipelineState ? std::make_unique<Tracker>() : nullptr;
            std::vector<wil::unique_handle> threads;
            const auto begin = DetourTransactionBegin();
            if (begin != NO_ERROR) {
                // A protection failure may have claimed the transaction, unlike the busy return.
                if (begin != ERROR_INVALID_OPERATION)
                    DetourTransactionAbort();
                return false;
            }
            auto abort = wil::scope_exit([] { DetourTransactionAbort(); });
            if (!std::all_of(methods.begin(), methods.end(), [](auto& method) { return method.attach(); }) ||
                !CollectThreads(threads))
                return false;
            for (const auto& thread : threads) {
                if (DetourUpdateThread(thread.get()) != NO_ERROR)
                    return false;
            }
            // Detours atomically commits the whole transaction or aborts it. Until
            // publication any already-resumed thread simply calls its trampoline.
            const auto commit = DetourTransactionCommit();
            abort.release();
            if (commit != NO_ERROR)
                return false;
            for (auto& module : modules)
                (void)module.release();
            executeTarget = execute;
            resetTarget = reset;
            allocatorResetTarget = allocatorReset;
            installedStateEntries = states;
            pipelineTracker.store(tracker.release(), std::memory_order_release);
            activeSink.store(&sink, std::memory_order_release);
            return true;
        } catch (...) {
            // Any started transaction was aborted before releasing the thread handles.
            return false;
        }
    }
} // namespace virtualdesktop_openxr::nr_before_sr
