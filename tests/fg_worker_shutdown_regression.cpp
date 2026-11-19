// CPU-only lifecycle checks for the production FG worker and its native SDK calls.
#include "pch.h"
#include "runtime.h"
#define wmain retainedShutdownFixtureMain
#include "nonnr_async_regression.cpp"
#undef wmain

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        enum class StopPoint {
            duringWait,
            failedBegin,
            emptyMailbox,
            queuedTicket,
            afterEnd,
            failedEnd,
            failedClose,
            queuedFailedClose,
            producerError
        };

        static void run(const wchar_t* directory, StopPoint point) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(directory, state);
            RuntimeInputRegression::seed(runtime);
            runtime.m_frameGenerationMode = 2;
            state.releaseWait = point != StopPoint::duringWait;
            if (point == StopPoint::failedBegin)
                state.fault = Fault::Begin;
            const bool queued = point == StopPoint::queuedTicket || point == StopPoint::queuedFailedClose;
            const bool failedClose = point == StopPoint::failedClose || point == StopPoint::queuedFailedClose;
            const bool ordinary = point == StopPoint::afterEnd || point == StopPoint::failedEnd;
            if (point == StopPoint::failedEnd || failedClose)
                state.fault = Fault::End;

            constexpr uint64_t queuedToken = 37;
            auto stop = [&] {
                std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                runtime.m_terminateAsyncThread = true;
                runtime.m_asyncSubmissionCondVar.notify_all();
            };
            auto cleanup = MakeScopeGuard([&] {
                stop();
                state.release();
                if (runtime.m_asyncSubmissionThread.joinable())
                    runtime.m_asyncSubmissionThread.join();
                // The queued marker represents an owner but has no module allocation.
                runtime.m_fgTransactionForSubmission = {};
                runtime.cleanupSessionResources();
            });
            RuntimeInputRegression::start(runtime);
            auto await = [&](auto condition, const char* failure) {
                std::unique_lock lock(runtime.m_asyncSubmissionMutex);
                require(runtime.m_asyncSubmissionCondVar.wait_for(lock, 2s, condition), failure);
            };
            if (point == StopPoint::duringWait) {
                require(state.waitFor([&] { return state.waitCalls == 1; }), "native Wait never entered");
                stop();
                state.release();
            } else if (point == StopPoint::failedBegin) {
                await([&] { return bool(runtime.m_asyncSubmissionError); }, "native Begin failure never reported");
            } else {
                require(state.waitFor([&] { return state.begunIds.size() == 1; }), "native Begin never entered");
                await([&] { return runtime.m_asyncSubmissionReady; }, "worker never opened its startup mailbox");
                if (ordinary) {
                    {
                        std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                        runtime.m_layersForAsyncSubmission.emplace_back().Header.Type = ovrLayerType_Quad;
                        runtime.m_asyncNextFrameId = 1;
                        runtime.m_asyncSubmissionReady = false;
                        runtime.m_asyncSubmissionCondVar.notify_all();
                    }
                    await(
                        [&] {
                            return (runtime.m_asyncSubmissionReady && !runtime.m_fgActiveSubmission) ||
                                   runtime.m_asyncSubmissionError;
                        },
                        "worker never finished the submitted native cycle");
                }
                // Stop and a pending owner can become visible in the same mailbox wake.
                std::lock_guard lock(runtime.m_asyncSubmissionMutex);
                if (queued) {
                    runtime.m_layersForAsyncSubmission.emplace_back().Header.Type = ovrLayerType_Quad;
                    runtime.m_fgTransactionForSubmission.ticket.token = queuedToken;
                    runtime.m_asyncSubmissionReady = false;
                }
                if (point == StopPoint::producerError)
                    runtime.m_asyncSubmissionError = std::make_exception_ptr(std::runtime_error("producer failed"));
                runtime.m_terminateAsyncThread = true;
                runtime.m_asyncSubmissionCondVar.notify_all();
            }
            runtime.m_asyncSubmissionThread.join();
            require(state.waitedIds == std::vector<long long>{0}, "unexpected native Wait sequence");
            const bool attemptedBegin = point != StopPoint::duringWait;
            require(state.begunIds == (attemptedBegin ? std::vector<long long>{0} : std::vector<long long>{}),
                    "unexpected native Begin sequence");
            const bool neededEnd =
                attemptedBegin && point != StopPoint::failedBegin && point != StopPoint::producerError;
            require(state.frames.size() == (neededEnd ? 1u : 0u), "wrong number of native Ends at worker shutdown");
            if (neededEnd) {
                const auto& frame = state.frames.front();
                require(frame.id == 0 && frame.layerCount == 1 && frame.layers.size() == 1 &&
                            frame.layers.front().Type == (ordinary ? ovrLayerType_Quad : ovrLayerType_Disabled) &&
                            frame.layers.front().Flags == 0 && frame.hasViewScale == ordinary,
                        "shutdown used the wrong native ID or submitted the wrong layer");
            }
            const bool failed = point == StopPoint::failedBegin || point == StopPoint::failedEnd || failedClose;
            require(bool(runtime.m_asyncSubmissionError) == (failed || point == StopPoint::producerError) &&
                        state.faultCalls == (failed ? 1u : 0u),
                    "native failure was lost or retried");
            if (!ordinary)
                require(!runtime.m_fgActiveSubmission && !runtime.m_fgActiveOwnedPixels &&
                            runtime.m_fgWorkerGroupNativeId == 0 && !runtime.m_fgFailedTransaction.ticket.token &&
                            runtime.m_fgFailedLayers.empty(),
                        "shutdown accounted for a group that never started");
            if (queued)
                require(runtime.m_fgTransactionForSubmission.ticket.token == queuedToken &&
                            runtime.m_layersForAsyncSubmission.size() == 1 &&
                            runtime.m_layersForAsyncSubmission.front().Header.Type == ovrLayerType_Quad,
                        "shutdown consumed an unpublished owner");
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(argc == 2 || argc == 3, "usage: fg_worker_shutdown_regression OVRNull-directory [case]");
        using Regression = virtualdesktop_openxr::RuntimeFrameGenerationRegression;
        using Stop = Regression::StopPoint;
        const std::pair<const wchar_t*, Stop> cases[] = {{L"during-wait", Stop::duringWait},
                                                         {L"failed-begin", Stop::failedBegin},
                                                         {L"after-end", Stop::afterEnd},
                                                         {L"failed-end", Stop::failedEnd},
                                                         {L"empty-mailbox", Stop::emptyMailbox},
                                                         {L"queued-ticket", Stop::queuedTicket},
                                                         {L"failed-close", Stop::failedClose},
                                                         {L"queued-failed-close", Stop::queuedFailedClose},
                                                         {L"producer-error", Stop::producerError}};
        unsigned ran{}, failures{};
        for (const auto& [name, point] : cases) {
            if (argc == 3 && std::wstring_view(argv[2]) != name)
                continue;
            ++ran;
            try {
                Regression::run(argv[1], point);
                std::wcout << L"PASS: " << name << L'\n';
            } catch (const std::exception& error) {
                ++failures;
                std::wcerr << L"FAIL: " << name << L": ";
                std::cerr << error.what() << '\n';
            }
        }
        require(ran > 0, "unknown worker shutdown case");
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
