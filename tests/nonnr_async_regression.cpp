// CPU-only regressions through the real asynchronous submission worker and session cleanup.
// The OVRNull exports are intercepted before they can reach a driver or GPU.
#include "pch.h"
#include "runtime.h"
#include <cstdlib>
#include <exception>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
enum class Fault { None, Wait, Begin, End, NotInitialized };

struct SubmittedFrame {
    long long id;
    unsigned layerCount;
    std::vector<ovrLayerHeader> layers;
    float worldScale;
};

struct BackendState {
    std::mutex mutex;
    std::condition_variable changed;
    bool releaseWait{false};
    unsigned waitCalls{0};
    unsigned beginCalls{0};
    unsigned faultCalls{0};
    std::vector<long long> waitedIds;
    std::vector<long long> begunIds;
    std::vector<SubmittedFrame> frames;
    std::vector<ovrTextureSwapChain> createdChains;
    std::vector<ovrTextureSwapChain> destroyedChains;
    Fault fault{Fault::None};

    template <typename Predicate>
    bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, predicate);
    }

    void release() {
        std::lock_guard lock(mutex);
        releaseWait = true;
        changed.notify_all();
    }
};

BackendState* backendState;
decltype(&ovr_WaitToBeginFrame) originalWait;
decltype(&ovr_BeginFrame) originalBegin;
decltype(&ovr_EndFrame) originalEnd;
decltype(&ovr_GetSessionStatus) originalStatus;
decltype(&ovr_GetHmdDesc) originalHmdDesc;
decltype(&ovr_CreateTextureSwapChainDX) originalCreateChain;
decltype(&ovr_DestroyTextureSwapChain) originalDestroyChain;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkDetour(LONG result) { require(result == NO_ERROR, "OVR export detour failed"); }

ovrResult OVR_CDECL interceptedWait(ovrSession, long long frameId) {
    auto& state = *backendState;
    std::unique_lock lock(state.mutex);
    ++state.waitCalls;
    state.waitedIds.push_back(frameId);
    state.changed.notify_all();
    state.changed.wait(lock, [&] { return state.releaseWait; });
    if (state.fault == Fault::Wait || state.fault == Fault::NotInitialized) {
        ++state.faultCalls;
        state.changed.notify_all();
        return state.fault == Fault::Wait ? ovrError_DisplayLost : ovrError_NotInitialized;
    }
    return ovrSuccess;
}

ovrResult OVR_CDECL interceptedBegin(ovrSession, long long frameId) {
    auto& state = *backendState;
    std::lock_guard lock(state.mutex);
    ++state.beginCalls;
    state.begunIds.push_back(frameId);
    if (state.fault == Fault::Begin) ++state.faultCalls;
    state.changed.notify_all();
    return state.fault == Fault::Begin ? ovrError_DisplayLost : ovrSuccess;
}

ovrResult OVR_CDECL interceptedEnd(ovrSession,
                                  long long frameId,
                                  const ovrViewScaleDesc* scale,
                                  const ovrLayerHeader* const* layers,
                                  unsigned layerCount) {
    auto& state = *backendState;
    std::lock_guard lock(state.mutex);
    SubmittedFrame frame{frameId, layerCount, {}, scale ? scale->HmdSpaceToWorldScaleInMeters : 0.f};
    for (unsigned i = 0; i < layerCount; ++i) {
        // Copy at the backend boundary: the real worker owns the payload lifetime.
        if (layers && layers[i]) frame.layers.push_back(*layers[i]);
    }
    state.frames.push_back(std::move(frame));
    if (state.fault == Fault::End) ++state.faultCalls;
    state.changed.notify_all();
    return state.fault == Fault::End ? ovrError_DisplayLost : ovrSuccess;
}

ovrResult OVR_CDECL interceptedStatus(ovrSession, ovrSessionStatus* status) {
    *status = {};
    status->HmdPresent = status->HmdMounted = status->IsVisible = ovrTrue;
    return ovrSuccess;
}

ovrHmdDesc OVR_CDECL interceptedHmdDesc(ovrSession) {
    ovrHmdDesc info{};
    info.DisplayRefreshRate = 90.f;
    return info;
}

ovrResult OVR_CDECL interceptedCreateChain(ovrSession,
                                          IUnknown*,
                                          const ovrTextureSwapChainDesc*,
                                          ovrTextureSwapChain* chain) {
    auto& state = *backendState;
    std::lock_guard lock(state.mutex);
    *chain = reinterpret_cast<ovrTextureSwapChain>(uintptr_t(0x1000 + state.createdChains.size()));
    state.createdChains.push_back(*chain);
    return ovrSuccess;
}

void OVR_CDECL interceptedDestroyChain(ovrSession, ovrTextureSwapChain chain) {
    auto& state = *backendState;
    std::lock_guard lock(state.mutex);
    state.destroyedChains.push_back(chain);
}

struct BackendHooks {
    explicit BackendHooks(const wchar_t* directory, BackendState& state) {
        ovrInitParams init{};
        init.Flags = ovrInit_RequestVersion;
        init.RequestedMinorVersion = OVR_MINOR_VERSION;
        auto path = std::filesystem::absolute(directory).wstring();
        if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
        require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, path.c_str())), "OVRNull initialization failed");
        const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
        require(module != nullptr, "OVRNull module missing");
        originalWait = reinterpret_cast<decltype(originalWait)>(GetProcAddress(module, "ovr_WaitToBeginFrame"));
        originalBegin = reinterpret_cast<decltype(originalBegin)>(GetProcAddress(module, "ovr_BeginFrame"));
        originalEnd = reinterpret_cast<decltype(originalEnd)>(GetProcAddress(module, "ovr_EndFrame"));
        originalStatus = reinterpret_cast<decltype(originalStatus)>(GetProcAddress(module, "ovr_GetSessionStatus"));
        originalHmdDesc = reinterpret_cast<decltype(originalHmdDesc)>(GetProcAddress(module, "ovr_GetHmdDesc"));
        originalCreateChain = reinterpret_cast<decltype(originalCreateChain)>(GetProcAddress(module, "ovr_CreateTextureSwapChainDX"));
        originalDestroyChain = reinterpret_cast<decltype(originalDestroyChain)>(GetProcAddress(module, "ovr_DestroyTextureSwapChain"));
        require(originalWait && originalBegin && originalEnd && originalStatus && originalHmdDesc &&
                    originalCreateChain && originalDestroyChain, "OVR fixture exports missing");
        backendState = &state;
        checkDetour(DetourTransactionBegin());
        checkDetour(DetourUpdateThread(GetCurrentThread()));
        checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalWait), interceptedWait));
        checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalBegin), interceptedBegin));
        checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalEnd), interceptedEnd));
        checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalStatus), interceptedStatus));
        checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalHmdDesc), interceptedHmdDesc));
        checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalCreateChain), interceptedCreateChain));
        checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalDestroyChain), interceptedDestroyChain));
        checkDetour(DetourTransactionCommit());
    }

    ~BackendHooks() {
        // Every worker is joined before these exports are restored.
        if (DetourTransactionBegin() != NO_ERROR ||
            DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalWait), interceptedWait) != NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalBegin), interceptedBegin) != NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalEnd), interceptedEnd) != NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalStatus), interceptedStatus) != NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalHmdDesc), interceptedHmdDesc) != NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalCreateChain), interceptedCreateChain) != NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&originalDestroyChain), interceptedDestroyChain) != NO_ERROR ||
            DetourTransactionCommit() != NO_ERROR) {
            std::cerr << "FAIL: OVR export detour cleanup failed\n";
            std::_Exit(2);
        }
        backendState = nullptr;
    }
};

XrFrameEndInfo emptyFrame() {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = 1;
    info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return info;
}
} // namespace

namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
    static void seed(OpenXrRuntime& runtime) {
        runtime.stopRegistryWatcher();
        runtime.m_sessionCreated = true;
        runtime.m_sessionBegun = true;
        runtime.m_sessionState = XR_SESSION_STATE_SYNCHRONIZED;
        runtime.m_isHeadless = true;
        runtime.m_useAsyncSubmission = true;
        runtime.m_needStartAsyncSubmissionThread = false;
        runtime.m_terminateAsyncThread = false;
        runtime.m_frameWaited = runtime.m_frameBegun = 1;
        runtime.m_frameCompleted = 0;
        runtime.m_renderTimerApp.start();
        // No OVR session, swapchain, graphics binding, or device is created.
    }

    static void start(OpenXrRuntime& runtime) {
        runtime.m_asyncSubmissionThread = std::thread([&] { runtime.asyncSubmissionThread(); });
    }

    static void stop(OpenXrRuntime& runtime, BackendState& state, std::thread* producer = nullptr) {
        state.release();
        {
            std::lock_guard lock(runtime.m_asyncSubmissionMutex);
            runtime.m_terminateAsyncThread = true;
            runtime.m_asyncSubmissionCondVar.notify_all();
        }
        if (producer && producer->joinable()) producer->join();
        runtime.cleanupSessionResources();
    }

    static void verifyDisabledFrame(const SubmittedFrame& frame, long long expectedFrameId = 0) {
        require(frame.id == expectedFrameId, "submitted backend frame has the wrong ID");
        require(frame.layerCount == 1 && frame.layers.size() == 1 && frame.layers[0].Type == ovrLayerType_Disabled &&
                    frame.layers[0].Flags == 0 && frame.worldScale == 1.f,
                "zero-layer xrEndFrame must submit exactly one disabled layer with world scale 1");
    }

    static void startup(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        BackendState state;
        BackendHooks hooks(backendDirectory, state);
        seed(runtime);
        std::thread producer;
        std::mutex producerMutex;
        std::condition_variable producerChanged;
        bool producerDone = false;
        XrResult producerResult = XR_ERROR_RUNTIME_FAILURE;
        std::exception_ptr producerError;
        const auto info = emptyFrame();
        auto cleanup = MakeScopeGuard([&] { stop(runtime, state, &producer); });
        start(runtime);
        require(state.waitFor([&] { return state.waitCalls == 1; }), "worker did not reach its first backend Wait");
        producer = std::thread([&] {
            try { producerResult = runtime.xrEndFrame(reinterpret_cast<XrSession>(1), &info); }
            catch (...) { producerError = std::current_exception(); }
            {
                std::lock_guard lock(producerMutex);
                producerDone = true;
                producerChanged.notify_all();
            }
        });

        // Observe entry into the real frame critical section, rather than just thread startup.
        bool producerEntered = false;
        const auto entryDeadline = std::chrono::steady_clock::now() + 2000ms;
        while (std::chrono::steady_clock::now() < entryDeadline) {
            {
                std::lock_guard lock(producerMutex);
                if (producerDone) { producerEntered = true; break; }
            }
            {
                std::unique_lock probe(runtime.m_swapchainsMutex, std::try_to_lock);
                if (!probe.owns_lock()) { producerEntered = true; break; }
            }
            std::this_thread::sleep_for(1ms);
        }
        require(producerEntered, "producer did not enter xrEndFrame before the first backend Begin");
        bool returnedBeforeBegin;
        {
            std::unique_lock lock(producerMutex);
            // The backend cannot Begin while Wait is gated. This is a negative ordering assertion.
            returnedBeforeBegin = producerChanged.wait_for(lock, 150ms, [&] { return producerDone; });
        }
        if (returnedBeforeBegin) std::cerr << "FAIL: xrEndFrame returned before the worker's first backend Begin\n";
        state.release();
        bool producerFinished;
        {
            std::unique_lock lock(producerMutex);
            producerFinished = producerChanged.wait_for(lock, 2000ms, [&] { return producerDone; });
        }
        const bool submitted = state.waitFor([&] { return !state.frames.empty(); });
        if (!submitted) std::cerr << "FAIL: first queued disabled payload was discarded; backend End calls=0\n";
        bool secondSubmitted = false;
        if (!returnedBeforeBegin && producerFinished && !producerError && producerResult == XR_SUCCESS && submitted) {
            producer.join();
            runtime.waitForAsyncSubmissionIdle();
            {
                std::lock_guard lock(runtime.m_frameMutex);
                runtime.m_frameWaited = runtime.m_frameBegun = 2;
                runtime.m_renderTimerApp.start();
            }
            auto secondInfo = emptyFrame();
            secondInfo.displayTime = 2;
            require(runtime.xrEndFrame(reinterpret_cast<XrSession>(1), &secondInfo) == XR_SUCCESS,
                    "second zero-layer xrEndFrame failed");
            secondSubmitted = state.waitFor([&] { return state.frames.size() == 2; });
        }
        // Always stop and join before assertions can unwind the runtime or the producer.
        stop(runtime, state, &producer);
        cleanup.Deactivate();
        require(producerFinished, "xrEndFrame did not finish after backend Wait was released");
        if (producerError) std::rethrow_exception(producerError);
        require(producerResult == XR_SUCCESS, "zero-layer xrEndFrame failed");
        require(!returnedBeforeBegin && submitted, "first frame readiness/payload ordering failed");
        require(secondSubmitted, "second queued disabled payload did not reach the backend");
        {
            std::lock_guard lock(state.mutex);
            require(state.frames.size() == 2 && state.begunIds.size() >= 2 && state.waitedIds.size() >= 2 &&
                        state.begunIds[0] == 0 && state.begunIds[1] == 1 &&
                        state.waitedIds[0] == 0 && state.waitedIds[1] == 1,
                    "successive backend Wait/Begin/End calls must use frame IDs 0 and 1 exactly once each");
            verifyDisabledFrame(state.frames.front());
            verifyDisabledFrame(state.frames[1], 1);
        }
        std::cout << "PASS: first xrEndFrame waits for backend Begin; two disabled payloads use frame IDs 0 and 1\n";
    }

    static void workerError(const wchar_t* backendDirectory, Fault fault) {
        OpenXrRuntime runtime;
        BackendState state;
        state.fault = fault;
        BackendHooks hooks(backendDirectory, state);
        seed(runtime);
        auto cleanup = MakeScopeGuard([&] { stop(runtime, state); });
        start(runtime);
        require(state.waitFor([&] { return state.waitCalls == 1; }), "worker did not reach its first backend Wait");
        state.release();
        if (fault == Fault::End) {
            require(state.waitFor([&] { return state.beginCalls == 1; }), "worker did not Begin before End error injection");
            runtime.waitForAsyncSubmissionIdle();
            const auto info = emptyFrame();
            require(runtime.xrEndFrame(reinterpret_cast<XrSession>(1), &info) == XR_SUCCESS,
                    "could not queue the frame for backend End error injection");
        }
        require(state.waitFor([&] { return state.faultCalls == 1; }), "backend failure was not injected");
        std::cout << "INFO: injected ovrError_DisplayLost; checking caller error propagation\n" << std::flush;
        bool propagated = false;
        try { runtime.waitForAsyncSubmissionIdle(); }
        catch (const std::exception&) { propagated = true; }
        // A caught worker failure must also remain safe to join during actual session cleanup.
        stop(runtime, state);
        cleanup.Deactivate();
        require(propagated, "worker backend failure must propagate to the waiting caller");
        require(!runtime.m_asyncSubmissionThread.joinable() && runtime.m_needStartAsyncSubmissionThread,
                "session cleanup did not join the failed worker");
        if (fault == Fault::End) {
            std::lock_guard lock(state.mutex);
            require(state.frames.size() == 1, "End failure must have exactly one submitted frame");
            verifyDisabledFrame(state.frames.front());
        }
        std::cout << "PASS: backend failure reaches the caller and actual session cleanup joins the worker\n";
    }

    static void shutdownNotInitialized(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        BackendState state;
        state.fault = Fault::NotInitialized;
        BackendHooks hooks(backendDirectory, state);
        seed(runtime);
        auto cleanup = MakeScopeGuard([&] { stop(runtime, state); });
        start(runtime);
        state.release();
        require(state.waitFor([&] { return state.faultCalls >= 2; }), "persistent NotInitialized retry was not reached");
        std::cout << "INFO: persistent ovrError_NotInitialized; invoking actual cleanupSessionResources\n" << std::flush;
        // Run each mode in its own process with an external timeout: the baseline never returns from this join.
        runtime.cleanupSessionResources();
        cleanup.Deactivate();
        require(!runtime.m_asyncSubmissionThread.joinable() && runtime.m_needStartAsyncSubmissionThread &&
                    !runtime.m_sessionCreated,
                "cleanup must join a worker retrying NotInitialized and clear session state");
        std::cout << "PASS: actual session cleanup interrupts persistent NotInitialized retry\n";
    }

    static void controlDuringWait(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        BackendState state;
        state.fault = Fault::NotInitialized;
        BackendHooks hooks(backendDirectory, state);
        seed(runtime);
        runtime.m_instanceCreated = true;
        runtime.m_sessionState = XR_SESSION_STATE_FOCUSED;
        std::thread producer, poll, requestExit;
        std::mutex controlMutex;
        std::condition_variable controlChanged;
        bool producerDone = false;
        bool pollStarted = false, pollDone = false;
        bool exitStarted = false, exitDone = false;
        XrResult pollResult = XR_ERROR_RUNTIME_FAILURE;
        XrResult exitResult = XR_ERROR_RUNTIME_FAILURE;
        std::exception_ptr pollError, exitError;
        const auto info = emptyFrame();
        const auto finish = [&] {
            state.release();
            {
                // Termination must not need the frame mutex held by the waiting producer.
                std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                runtime.m_terminateAsyncThread = true;
                runtime.m_asyncSubmissionCondVar.notify_all();
            }
            if (producer.joinable()) producer.join();
            if (poll.joinable()) poll.join();
            if (requestExit.joinable()) requestExit.join();
            runtime.cleanupSessionResources();
        };
        auto cleanup = MakeScopeGuard([&] { finish(); });
        start(runtime);
        state.release();
        require(state.waitFor([&] { return state.faultCalls >= 2; }), "persistent NotInitialized wait was not reached");
        producer = std::thread([&] {
            try { runtime.xrEndFrame(reinterpret_cast<XrSession>(1), &info); }
            catch (const std::exception&) { /* Expected when fixture cleanup stops its worker. */ }
            {
                std::lock_guard lock(controlMutex);
                producerDone = true;
                controlChanged.notify_all();
            }
        });
        bool producerEntered = false;
        const auto entryDeadline = std::chrono::steady_clock::now() + 2000ms;
        while (std::chrono::steady_clock::now() < entryDeadline) {
            {
                std::unique_lock probe(runtime.m_swapchainsMutex, std::try_to_lock);
                if (!probe.owns_lock()) { producerEntered = true; break; }
            }
            std::this_thread::sleep_for(1ms);
        }
        require(producerEntered, "producer did not enter the real xrEndFrame critical section");
        {
            std::lock_guard lock(controlMutex);
            require(!producerDone, "xrEndFrame unexpectedly completed during persistent NotInitialized");
        }
        poll = std::thread([&] {
            {
                std::lock_guard lock(controlMutex);
                pollStarted = true;
                controlChanged.notify_all();
            }
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            try { pollResult = runtime.xrPollEvent(reinterpret_cast<XrInstance>(1), &event); }
            catch (...) { pollError = std::current_exception(); }
            {
                std::lock_guard lock(controlMutex);
                pollDone = true;
                controlChanged.notify_all();
            }
        });
        requestExit = std::thread([&] {
            {
                std::lock_guard lock(controlMutex);
                exitStarted = true;
                controlChanged.notify_all();
            }
            try { exitResult = runtime.xrRequestExitSession(reinterpret_cast<XrSession>(1)); }
            catch (...) { exitError = std::current_exception(); }
            {
                std::lock_guard lock(controlMutex);
                exitDone = true;
                controlChanged.notify_all();
            }
        });
        bool pollResponsive, exitResponsive;
        {
            std::unique_lock lock(controlMutex);
            require(controlChanged.wait_for(lock, 2000ms, [&] { return pollStarted && exitStarted; }),
                    "control threads did not start");
            controlChanged.wait_for(lock, 250ms, [&] { return pollDone && exitDone; });
            pollResponsive = pollDone;
            exitResponsive = exitDone;
        }
        if (!pollResponsive) std::cerr << "FAIL: xrPollEvent blocked for 250ms while xrEndFrame waited for the backend\n";
        if (!exitResponsive) std::cerr << "FAIL: xrRequestExitSession blocked for 250ms while xrEndFrame waited for the backend\n";
        finish();
        cleanup.Deactivate();
        require(pollResponsive && exitResponsive, "session control calls must remain responsive during async frame wait");
        if (pollError) std::rethrow_exception(pollError);
        if (exitError) std::rethrow_exception(exitError);
        require(pollResult == XR_EVENT_UNAVAILABLE || pollResult == XR_SUCCESS, "live-session event polling failed");
        require(exitResult == XR_SUCCESS, "live-session exit request failed");
        std::cout << "PASS: event polling and exit request finish during persistent async backend wait\n";
    }

    static void waitRetry(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        BackendState state;
        state.fault = Fault::Wait;
        BackendHooks hooks(backendDirectory, state);
        seed(runtime);
        runtime.m_useAsyncSubmission = false;
        runtime.m_frameWaited = runtime.m_frameBegun = 0;
        runtime.m_frameTimerApp.start();
        auto cleanup = MakeScopeGuard([&] { runtime.cleanupSessionResources(); });
        state.release();
        unsigned failedWaits = 0;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            try { runtime.xrWaitFrame(reinterpret_cast<XrSession>(1), &waitInfo, &frameState); }
            catch (const std::exception&) { ++failedWaits; }
        }
        runtime.cleanupSessionResources();
        cleanup.Deactivate();
        require(failedWaits == 2 && state.faultCalls == 2, "both first-frame backend Wait failures must reach the caller");
        if (state.createdChains.size() != 1 || state.destroyedChains.size() != 1) {
            std::cerr << "FAIL: retrying failed first-frame wait created=" << state.createdChains.size()
                      << " destroyed=" << state.destroyedChains.size() << " expected one of each\n";
        }
        require(state.createdChains.size() == 1 && state.destroyedChains.size() == 1 &&
                    state.createdChains.front() == state.destroyedChains.front(),
                "first-frame wait retry must retain and destroy its original dummy swapchain exactly once");
        std::cout << "PASS: failed first-frame waits reuse one dummy swapchain and cleanup destroys it once\n";
    }
};
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    // Fault modes run as child processes: the caller detects a crash and bounds a stuck cleanup join.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(argc == 3, "usage: nonnr_async_regression startup/error-wait/error-begin/error-end/shutdown-not-initialized/control-during-wait/wait-retry OVRNull-directory");
        const std::wstring mode = argv[1];
        using virtualdesktop_openxr::RuntimeInputRegression;
        if (mode == L"startup") RuntimeInputRegression::startup(argv[2]);
        else if (mode == L"error-wait") RuntimeInputRegression::workerError(argv[2], Fault::Wait);
        else if (mode == L"error-begin") RuntimeInputRegression::workerError(argv[2], Fault::Begin);
        else if (mode == L"error-end") RuntimeInputRegression::workerError(argv[2], Fault::End);
        else if (mode == L"shutdown-not-initialized") RuntimeInputRegression::shutdownNotInitialized(argv[2]);
        else if (mode == L"control-during-wait") RuntimeInputRegression::controlDuringWait(argv[2]);
        else if (mode == L"wait-retry") RuntimeInputRegression::waitRetry(argv[2]);
        else throw std::runtime_error("Unknown async regression mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
