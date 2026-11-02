// Actual xrEndFrame dispatch ordering on real WARP D3D11 contexts; no headset.
#define wmain retainedAsyncFixtureMain
#include "nonnr_async_regression.cpp"
#undef wmain
#include <future>
extern "C" void* contextMethodAddress(void*, unsigned);

namespace {
    struct Devices {
        ComPtr<ID3D11Device5> app, submission;
        ComPtr<ID3D11DeviceContext4> appContext, submissionContext;
        ComPtr<ID3D12Device> device12;
        ComPtr<ID3D12CommandQueue> queue12;
        Devices() {
            for (unsigned i = 0; i < 2; ++i) {
                ComPtr<ID3D11Device> device;
                ComPtr<ID3D11DeviceContext> context;
                CHECK_HRCMD(D3D11CreateDevice(nullptr,
                                              D3D_DRIVER_TYPE_WARP,
                                              nullptr,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                              nullptr,
                                              0,
                                              D3D11_SDK_VERSION,
                                              &device,
                                              nullptr,
                                              &context));
                CHECK_HRCMD(device.As(i ? &submission : &app));
                CHECK_HRCMD(context.As(i ? &submissionContext : &appContext));
            }
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> adapter;
            CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            CHECK_HRCMD(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
            CHECK_HRCMD(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)));
            D3D12_COMMAND_QUEUE_DESC queue{};
            CHECK_HRCMD(device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue12)));
        }
    };
    struct Observation {
        ID3D11DeviceContext4* app{};
        ID3D11DeviceContext4* submission{};
        std::atomic<DWORD> thread{};
        std::atomic<unsigned> appFlush{}, submissionFlush{}, appSignal{}, submissionSignal{}, appWait{},
            submissionWait{};
        std::atomic<bool> rejectSignal{};
    };
    Observation* observed{};
    using FlushFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*);
    using SyncFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
    FlushFn originalFlush{};
    SyncFn originalSignal{}, originalGpuWait{};
    bool selected(ID3D11DeviceContext4* context) {
        return observed && observed->thread.load() == GetCurrentThreadId() &&
               (context == observed->app || context == observed->submission);
    }
    void STDMETHODCALLTYPE observeFlush(ID3D11DeviceContext4* context) {
        if (selected(context)) {
            if (context == observed->app)
                ++observed->appFlush;
            if (context == observed->submission)
                ++observed->submissionFlush;
        }
        originalFlush(context);
    }
    HRESULT STDMETHODCALLTYPE observeSignal(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
        if (selected(context)) {
            if (context == observed->app)
                ++observed->appSignal;
            if (context == observed->submission)
                ++observed->submissionSignal;
            if (observed->rejectSignal.load())
                return E_ABORT;
        }
        return originalSignal(context, fence, value);
    }
    HRESULT STDMETHODCALLTYPE observeWait(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
        if (selected(context)) {
            if (context == observed->app)
                ++observed->appWait;
            if (context == observed->submission)
                ++observed->submissionWait;
        }
        return originalGpuWait(context, fence, value);
    }
    struct Spy {
        explicit Spy(const Devices& devices) {
            for (unsigned method = 0; method < 3; ++method)
                require(contextMethodAddress(devices.appContext.Get(), method) ==
                            contextMethodAddress(devices.submissionContext.Get(), method),
                        "shared WARP method implementations");
            originalFlush = reinterpret_cast<FlushFn>(contextMethodAddress(devices.appContext.Get(), 0));
            originalSignal = reinterpret_cast<SyncFn>(contextMethodAddress(devices.appContext.Get(), 1));
            originalGpuWait = reinterpret_cast<SyncFn>(contextMethodAddress(devices.appContext.Get(), 2));
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalFlush), observeFlush));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), observeSignal));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalGpuWait), observeWait));
            checkDetour(DetourTransactionCommit());
        }
        ~Spy() {
            observed = nullptr;
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalFlush), observeFlush) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), observeSignal) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalGpuWait), observeWait) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR)
                std::_Exit(2);
        }
    };
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void
        run(const wchar_t* backend, Devices& devices, unsigned multiplier, bool nr, unsigned exclusion, bool cancel) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(backend, state);
            RuntimeInputRegression::seed(runtime);
            runtime.m_isHeadless = exclusion == 5;
            runtime.m_useMirrorWindow = false;
            runtime.m_useOculusRuntime = true;
            runtime.m_frameGenerationMode = exclusion == 1 ? 0 : multiplier;
            runtime.m_useAsyncSubmission = exclusion != 2;
            runtime.m_useApplicationDeviceForSubmission = exclusion == 3;
            runtime.m_d3d11Device = devices.app;
            runtime.m_d3d11Context = (exclusion == 4 || exclusion == 6) ? nullptr : devices.appContext;
            if (exclusion >= 6) {
                runtime.m_d3d12Device = devices.device12;
                runtime.m_d3d12CommandQueue = devices.queue12;
            }
            runtime.m_ovrSubmissionDevice = exclusion == 3 ? devices.app : devices.submission;
            runtime.m_ovrSubmissionContext = exclusion == 3 ? devices.appContext : devices.submissionContext;
            runtime.m_dlssnrSettings.enabled = nr;
            runtime.m_fenceValue = 17;
            runtime.m_submissionFenceValue = 23;
            runtime.m_precompositor.displayTime = -123;
            runtime.m_asyncSubmissionReady = false;
            runtime.m_fgActiveSubmission = runtime.m_fgActiveOwnedPixels = true;
            Observation observation;
            observation.app = devices.appContext.Get();
            observation.submission = runtime.m_ovrSubmissionContext.Get();
            observation.rejectSignal = exclusion == 2;
            observed = &observation;
            std::promise<void> entered, finished;
            auto entering = entered.get_future();
            auto completion = finished.get_future();
            bool threw{};
            auto info = emptyFrame();
            std::thread producer([&] {
                observation.thread = GetCurrentThreadId();
                entered.set_value();
                try {
                    runtime.xrEndFrame((XrSession)1, &info);
                } catch (...) {
                    threw = true;
                }
                finished.set_value();
            });
            entering.wait();
            bool entryObserved{};
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (!runtime.m_swapchainsMutex.try_lock()) {
                    entryObserved = true;
                    break;
                }
                runtime.m_swapchainsMutex.unlock();
                if (completion.wait_for(0ms) == std::future_status::ready) {
                    entryObserved = true;
                    break;
                }
                std::this_thread::yield();
            }
            const bool premature = completion.wait_for(50ms) == std::future_status::ready;
            const auto appFlush = observation.appFlush.load();
            const auto subFlush = observation.submissionFlush.load();
            const auto appSignal = observation.appSignal.load();
            const auto subSignal = observation.submissionSignal.load();
            const auto appWait = observation.appWait.load(), subWait = observation.submissionWait.load();
            const bool frameUnlocked = runtime.m_frameMutex.try_lock();
            const bool countersUnchanged =
                frameUnlocked && runtime.m_fenceValue == 17 && runtime.m_submissionFenceValue == 23;
            const bool pristine = frameUnlocked && runtime.m_precompositor.displayTime == -123;
            if (frameUnlocked)
                runtime.m_frameMutex.unlock();
            const bool contextUnlocked = runtime.m_fgContextMutex.try_lock();
            if (contextUnlocked)
                runtime.m_fgContextMutex.unlock();
            {
                std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                if (cancel)
                    runtime.m_terminateAsyncThread = true;
                else
                    runtime.m_asyncSubmissionError = std::make_exception_ptr(std::runtime_error("dispatch test error"));
                runtime.m_asyncSubmissionCondVar.notify_all();
            }
            const bool exitedPromptly = completion.wait_for(2s) == std::future_status::ready;
            if (!exitedPromptly) {
                observation.rejectSignal = true;
                std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                runtime.m_terminateAsyncThread = true;
                runtime.m_asyncSubmissionReady = true;
                runtime.m_fgActiveOwnedPixels = true;
                runtime.m_asyncSubmissionCondVar.notify_all();
            }
            if (completion.wait_for(2s) != std::future_status::ready) {
                std::fprintf(stderr, "FAIL: producer remained stuck after bounded cleanup rescue\n");
                std::fflush(stderr);
                std::_Exit(3);
            }
            producer.join();
            const bool noSynchronizationAfterFailure =
                observation.appFlush.load() == appFlush && observation.submissionFlush.load() == subFlush &&
                observation.appSignal.load() == appSignal && observation.submissionSignal.load() == subSignal &&
                observation.appWait.load() == appWait && observation.submissionWait.load() == subWait;
            const bool stateAfterFailure = runtime.m_precompositor.displayTime == -123 &&
                                           runtime.m_submissionFenceValue == 23 &&
                                           (exclusion == 2 || runtime.m_fenceValue == 17);
            observed = nullptr;
            const bool published = !runtime.m_layersForAsyncSubmission.empty();
            runtime.m_asyncSubmissionError = nullptr;
            runtime.m_fgActiveSubmission = runtime.m_fgActiveOwnedPixels = false;
            runtime.m_d3d11Context.Reset();
            runtime.m_d3d11Device.Reset();
            runtime.m_d3d12CommandQueue.Reset();
            runtime.m_d3d12Device.Reset();
            runtime.m_ovrSubmissionContext.Reset();
            runtime.m_ovrSubmissionDevice.Reset();
            RuntimeInputRegression::stop(runtime, state);
            require(entryObserved && exitedPromptly && threw && !published,
                    "actual EndFrame error/cancel exits without publication");
            require(contextUnlocked && noSynchronizationAfterFailure && stateAfterFailure,
                    "blocked admission releases context and error/cancel cannot perform later synchronization or "
                    "preprocessing");
            require(
                appFlush == (exclusion == 0 ? 1u : 0u),
                "pending app commands must dispatch before closed producer admission only on eligible D3D11 FG path");
            require(subFlush == 0 && subSignal == 0 && appWait == 0 && subWait == 0,
                    "no early submission operation or moved queue Wait");
            if (exclusion == 2)
                require(premature && appSignal == 1,
                        "sync control reaches its unchanged late Signal and injected failure");
            else
                require(!premature && frameUnlocked && appSignal == 0 && countersUnchanged && pristine,
                        "blocked admission retains counters/preprocessing and requires neither early Signal nor GPU "
                        "completion");
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2 || argc == 3, "backend [controls]");
        Devices devices;
        Spy spy(devices);
        unsigned count{};
        const bool controlsOnly = argc == 3;
        for (unsigned m : {2u, 3u, 4u})
            for (bool nr : {false, true})
                for (unsigned exclusion = controlsOnly ? 1u : 0u; exclusion <= 7; ++exclusion)
                    for (bool cancel : {false, true}) {
                        virtualdesktop_openxr::RuntimeFrameGenerationRegression::run(
                            argv[1], devices, m, nr, exclusion, cancel);
                        ++count;
                    }
        std::printf(
            "PASS: %u actual EndFrame early-dispatch/exclusion/error/cancel cases; WARP contexts,2x/3x/4x,NRon/off\n",
            count);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
