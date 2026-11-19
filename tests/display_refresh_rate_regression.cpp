// CPU-only checks of the public refresh-rate extension and its live timing state.
#define wmain refreshFixtureOriginalMain
#include "nonnr_async_regression.cpp"
#undef wmain
#include <limits>

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static unsigned run(const wchar_t* directory) {
            OpenXrRuntime runtime;
            BackendState state;
            BackendHooks hooks(directory, state);
            RuntimeInputRegression::seed(runtime);
            runtime.has_XR_FB_display_refresh_rate = true;
            runtime.m_displayRefreshRate = 90.f;
            unsigned failures{};
            const auto check = [&](const char* name, bool passed) {
                std::cout << (passed ? "PASS: " : "FAIL: ") << name << '\n';
                failures += !passed;
            };
            const auto session = reinterpret_cast<XrSession>(1);
            const auto request = [&](float rate) { return runtime.xrRequestDisplayRefreshRateFB(session, rate); };
            check("no preference accepts zero", request(0.f) == XR_SUCCESS);
            check("NaN is unsupported",
                  request(std::numeric_limits<float>::quiet_NaN()) == XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);
            check("current supported rate", request(90.f) == XR_SUCCESS);
            for (const auto rate :
                 {120.f, -90.f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
                check("unsupported rate rejected", request(rate) == XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);
            float rate{};
            check("get current rate", runtime.xrGetDisplayRefreshRateFB(session, &rate) == XR_SUCCESS && rate == 90.f);
            uint32_t count{};
            check("enumeration count query",
                  runtime.xrEnumerateDisplayRefreshRatesFB(session, 0, &count, nullptr) == XR_SUCCESS && count == 1);
            check("enumeration supported rate",
                  runtime.xrEnumerateDisplayRefreshRatesFB(session, 1, &count, &rate) == XR_SUCCESS && count == 1 &&
                      rate == 90.f);
            // Mirror the production writer: changing a native display rate must
            // be reflected by all three public queries under the same lock.
            {
                std::lock_guard lock(runtime.m_frameMutex);
                runtime.m_displayRefreshRate = 72.f;
            }
            check("get updated native rate",
                  runtime.xrGetDisplayRefreshRateFB(session, &rate) == XR_SUCCESS && rate == 72.f);
            check("enumerate updated rate",
                  runtime.xrEnumerateDisplayRefreshRatesFB(session, 1, &count, &rate) == XR_SUCCESS && rate == 72.f);
            check("request uses updated rate",
                  request(72.f) == XR_SUCCESS && request(90.f) == XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);
            check("invalid session",
                  runtime.xrGetDisplayRefreshRateFB(XR_NULL_HANDLE, &rate) == XR_ERROR_HANDLE_INVALID &&
                      runtime.xrRequestDisplayRefreshRateFB(XR_NULL_HANDLE, 0.f) == XR_ERROR_HANDLE_INVALID &&
                      runtime.xrEnumerateDisplayRefreshRatesFB(XR_NULL_HANDLE, 0, &count, nullptr) ==
                          XR_ERROR_HANDLE_INVALID);
            runtime.has_XR_FB_display_refresh_rate = false;
            check("extension not enabled",
                  runtime.xrGetDisplayRefreshRateFB(session, &rate) == XR_ERROR_FUNCTION_UNSUPPORTED &&
                      request(0.f) == XR_ERROR_FUNCTION_UNSUPPORTED &&
                      runtime.xrEnumerateDisplayRefreshRatesFB(session, 0, &count, nullptr) ==
                          XR_ERROR_FUNCTION_UNSUPPORTED);
            return failures;
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "usage: display_refresh_rate_regression OVRNull-directory");
        return virtualdesktop_openxr::RuntimeFrameGenerationRegression::run(argv[1]) ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
