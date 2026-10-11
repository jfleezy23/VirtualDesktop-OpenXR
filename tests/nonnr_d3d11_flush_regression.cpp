// Native D3D11 completion/failure controls; no VR session, headset, or settings writes.
#include "pch.h"
#include "runtime.h"

namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }
    using FenceEvent = HRESULT(STDMETHODCALLTYPE*)(ID3D11Fence*, UINT64, HANDLE);
    FenceEvent originalEvent;
    decltype(&WaitForSingleObject) originalWait = WaitForSingleObject;
    ID3D11Fence* watchedFence;
    HANDLE watchedEvent, entered;
    unsigned eventCalls, nullCalls, waitCalls;
    UINT64 watchedValue;
    bool failWait, failEvent;
    thread_local bool watching;
    HRESULT STDMETHODCALLTYPE event(ID3D11Fence* fence, UINT64 value, HANDLE handle) {
        if (!watching || fence != watchedFence)
            return originalEvent(fence, value, handle);
        ++eventCalls;
        watchedValue = value;
        nullCalls += !handle;
        watchedEvent = handle;
        if (entered)
            SetEvent(entered);
        return failEvent ? E_OUTOFMEMORY : originalEvent(fence, value, handle);
    }
    DWORD WINAPI wait(HANDLE handle, DWORD timeout) {
        if (!watching || !watchedEvent || handle != watchedEvent)
            return originalWait(handle, timeout);
        ++waitCalls;
        if (failWait) {
            SetLastError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        return originalWait(handle, std::min<DWORD>(timeout, 5000));
    }
    class Hooks {
      public:
        explicit Hooks(ID3D11Fence* fence) : retained(fence) {
            watchedFence = fence;
            watchedEvent = entered = nullptr;
            eventCalls = nullCalls = waitCalls = 0;
            watchedValue = 0;
            failWait = failEvent = false;
            originalEvent = reinterpret_cast<FenceEvent>((*reinterpret_cast<void***>(fence))[9]);
            require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
            LONG result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            require(result == NO_ERROR, "Fence hook installation failed");
            watching = true;
        }
        ~Hooks() {
            watching = false;
            LONG result = DetourTransactionBegin();
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (result == NO_ERROR)
                result = DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            if (result != NO_ERROR)
                std::terminate();
            watchedFence = nullptr;
        }
        Hooks(const Hooks&) = delete;
        Hooks& operator=(const Hooks&) = delete;

      private:
        ComPtr<ID3D11Fence> retained;
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static void run(const std::wstring& mode) {
            OpenXrRuntime runtime;
            runtime.stopRegistryWatcher();
            ComPtr<ID3D11Device> base;
            ComPtr<ID3D11DeviceContext> baseContext;
            CHECK_HRCMD(D3D11CreateDevice(nullptr,
                                          D3D_DRIVER_TYPE_HARDWARE,
                                          nullptr,
                                          0,
                                          nullptr,
                                          0,
                                          D3D11_SDK_VERSION,
                                          base.GetAddressOf(),
                                          nullptr,
                                          baseContext.GetAddressOf()));
            ComPtr<ID3D11Device5> device;
            ComPtr<ID3D11DeviceContext4> context;
            CHECK_HRCMD(base.As(&device));
            CHECK_HRCMD(baseContext.As(&context));
            ComPtr<ID3D11Fence> fence;
            CHECK_HRCMD(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
            runtime.m_d3d11Device = runtime.m_ovrSubmissionDevice = device;
            runtime.m_d3d11Context = runtime.m_ovrSubmissionContext = context;
            runtime.m_d3d11Fence = fence;
            const bool crossDevice = mode == L"cross-cpu" || mode == L"cross-gpu";
            if (crossDevice) {
                ComPtr<ID3D11Device> submissionBase;
                ComPtr<ID3D11DeviceContext> submissionContext;
                CHECK_HRCMD(D3D11CreateDevice(nullptr,
                                              D3D_DRIVER_TYPE_HARDWARE,
                                              nullptr,
                                              0,
                                              nullptr,
                                              0,
                                              D3D11_SDK_VERSION,
                                              submissionBase.GetAddressOf(),
                                              nullptr,
                                              submissionContext.GetAddressOf()));
                CHECK_HRCMD(submissionBase.As(&runtime.m_ovrSubmissionDevice));
                CHECK_HRCMD(submissionContext.As(&runtime.m_ovrSubmissionContext));
            }
            CHECK_HRCMD(runtime.m_ovrSubmissionDevice->CreateFence(
                0,
                crossDevice ? D3D11_FENCE_FLAG_SHARED : D3D11_FENCE_FLAG_NONE,
                IID_PPV_ARGS(runtime.m_ovrSubmissionFence.GetAddressOf())));
            if (crossDevice) {
                wil::unique_handle handle;
                CHECK_HRCMD(
                    runtime.m_ovrSubmissionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, handle.put()));
                CHECK_HRCMD(
                    device->OpenSharedFence(handle.get(), IID_PPV_ARGS(runtime.m_d3d11Fence.ReleaseAndGetAddressOf())));
            }
            CHECK_HRCMD(runtime.m_ovrSubmissionDevice->CreateFence(
                0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(runtime.m_ovrSubmissionCompletionFence.GetAddressOf())));
            if (mode.find(L"submission") == 0)
                fence = runtime.m_ovrSubmissionCompletionFence;
            else if (mode.find(L"cpu") == 0 || mode == L"gpu" || crossDevice)
                fence = runtime.m_ovrSubmissionFence;
#ifdef VDXR_LEGACY_HANDOFF_EVENT
            // Only the matching pre-change cohort has this private member.
            runtime.m_eventForSubmissionFence.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            require(!!runtime.m_eventForSubmissionFence, "Cannot create baseline handoff event");
#endif
            Hooks hooks(fence.Get());
            if (mode == L"app-wait" || mode == L"submission-wait") {
                const bool app = mode == L"app-wait";
                failWait = true;
                bool threw = false;
                try {
                    if (app)
                        runtime.cleanupD3D11();
                    else
                        runtime.cleanupSubmissionDevice();
                } catch (const std::exception&) {
                    threw = true;
                }
                const bool retained =
                    app ? runtime.m_d3d11Device == device && runtime.m_d3d11Context == context
                        : runtime.m_ovrSubmissionDevice == device && runtime.m_ovrSubmissionContext == context;
                const auto waits = waitCalls;
                const auto failedValue = watchedValue;
                failWait = false;
                if (app)
                    runtime.cleanupD3D11();
                else
                    runtime.cleanupSubmissionDevice();
                // Drain local owners even if a faulty baseline already dropped runtime references.
                watching = false;
                CHECK_HRCMD(fence->SetEventOnCompletion(failedValue, nullptr));
                watching = true;
                require(failedValue != 0 && fence->GetCompletedValue() >= failedValue,
                        "Fixture did not drain the failed cleanup's submitted fence value");
                require(threw && retained && waits == 1, "Failed wait released resources or reported completion");
            } else if (crossDevice) {
                runtime.m_syncGpuWorkInEndFrame = mode == L"cross-cpu";
                runtime.serializeD3D11Frame();
                watching = false;
                CHECK_HRCMD(fence->SetEventOnCompletion(runtime.m_fenceValue, nullptr));
                watching = true;
                require(runtime.m_fenceValue == 1 && fence->GetCompletedValue() >= 1,
                        "Cross-device consumer did not observe the producer's shared fence");
                require(mode == L"cross-cpu" ? nullCalls == 1 && eventCalls == 1 && waitCalls == 0
                                             : eventCalls == 0 && waitCalls == 0,
                        "Cross-device handoff used unexpected CPU synchronization");
                CHECK_HRCMD(runtime.m_ovrSubmissionDevice->GetDeviceRemovedReason());
            } else if (mode == L"app" || mode == L"submission") {
                for (unsigned i = 0; i < 2; ++i) {
                    if (mode == L"app")
                        runtime.flushD3D11Context();
                    else
                        runtime.flushSubmissionContext();
                }
                require(eventCalls == 2 && waitCalls == 2 && fence->GetCompletedValue() >= 2,
                        "Repeated drain did not establish GPU completion");
            } else if (mode == L"cpu" || mode == L"cpu-event" || mode == L"cpu-blocked" || mode == L"gpu") {
                runtime.m_syncGpuWorkInEndFrame = mode != L"gpu";
                runtime.m_fenceValue = 1;
                ComPtr<ID3D12Fence> gate;
                ComPtr<ID3D11Fence> importedGate;
                wil::unique_handle started, done;
                std::thread worker;
                if (mode == L"cpu-blocked") {
                    started.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                    done.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                    require(!!started && !!done, "Cannot create observer events");
                    ComPtr<ID3D12Device> gateDevice;
                    CHECK_HRCMD(
                        D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(gateDevice.GetAddressOf())));
                    CHECK_HRCMD(gateDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(gate.GetAddressOf())));
                    wil::unique_handle handle;
                    CHECK_HRCMD(
                        gateDevice->CreateSharedHandle(gate.Get(), nullptr, GENERIC_ALL, nullptr, handle.put()));
                    CHECK_HRCMD(device->OpenSharedFence(handle.get(), IID_PPV_ARGS(importedGate.GetAddressOf())));
                    CHECK_HRCMD(context->Wait(importedGate.Get(), 1));
                }
                auto releaseGate = MakeScopeGuard([&] {
                    if (gate)
                        gate->Signal(1);
                    if (worker.joinable())
                        worker.join();
                    entered = nullptr;
                });
                CHECK_HRCMD(context->Signal(fence.Get(), 1));
                bool completionThrew = false;
                if (mode == L"cpu-blocked") {
                    entered = started.get();
                    std::exception_ptr error;
                    worker = std::thread([&] {
                        watching = true;
                        try {
                            runtime.waitOnSubmissionDevice();
                        } catch (...) {
                            error = std::current_exception();
                        }
                        SetEvent(done.get());
                        watching = false;
                    });
                    const bool startedWait = originalWait(started.get(), 3000) == WAIT_OBJECT_0;
                    const bool blocked = originalWait(done.get(), 100) == WAIT_TIMEOUT;
                    const auto released = gate->Signal(1);
                    worker.join();
                    entered = nullptr;
                    CHECK_HRCMD(released);
                    if (error)
                        std::rethrow_exception(error);
                    require(startedWait && blocked && fence->GetCompletedValue() >= 1,
                            "CPU handoff returned before GPU producer completion");
                } else {
                    failEvent = mode == L"cpu-event";
                    try {
                        runtime.waitOnSubmissionDevice();
                    } catch (const std::exception&) {
                        completionThrew = true;
                    }
                    failEvent = false;
                    if (completionThrew)
                        runtime.waitOnSubmissionDevice();
                }
                watching = false;
                CHECK_HRCMD(fence->SetEventOnCompletion(runtime.m_fenceValue, nullptr));
                watching = true;
                require(completionThrew == (mode == L"cpu-event"),
                        "Completion registration failure was not propagated");
                if (mode == L"gpu")
                    require(eventCalls == 0 && waitCalls == 0, "Default GPU handoff introduced a CPU wait");
                else
                    require(nullCalls == eventCalls && eventCalls != 0 && waitCalls == 0,
                            "CPU handoff still uses a separately registered persistent event/Win32 wait");
            } else
                throw std::runtime_error("Unknown completion mode");
            CHECK_HRCMD(device->GetDeviceRemovedReason());
            std::cout << "PASS: D3D11 completion " << xr::wide_to_utf8(mode) << '\n';
        }
    };
} // namespace virtualdesktop_openxr
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(argc == 2,
                "usage: nonnr_d3d11_flush_regression "
                "app|submission|app-wait|submission-wait|cpu|cpu-event|cpu-blocked|gpu|cross-cpu|cross-gpu");
        virtualdesktop_openxr::RuntimeInputRegression::run(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
