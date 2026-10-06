// CPU-only visibility event lifecycle checks using the real poll and destroy methods.
#include "pch.h"
#include "runtime.h"

namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
static int run() {
    OpenXrRuntime runtime;
    runtime.stopRegistryWatcher();
    runtime.m_instanceCreated = true;
    int failures = 0;
    const auto expectNoEvents = [&](const char* label) {
        bool passed = true;
        for (uint32_t attempt = 0; attempt < 3; ++attempt) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            const auto result = runtime.xrPollEvent(reinterpret_cast<XrInstance>(1), &event);
            if (result != XR_EVENT_UNAVAILABLE) {
                passed = false;
                std::cerr << "FAIL: " << label << " result=" << result << " event=" << event.type << '\n';
            }
        }
        if (passed) std::cout << "PASS: " << label << '\n';
        else ++failures;
    };
    runtime.has_XR_KHR_visibility_mask = true;
    runtime.m_visibilityMaskDirty = 2;
    expectNoEvents("visibility changes before session creation cannot emit a session event");

    runtime.m_sessionCreated = true;
    runtime.has_XR_KHR_visibility_mask = false;
    runtime.m_visibilityMaskDirty = 2;
    expectNoEvents("disabled visibility extension cannot emit its event");

    runtime.has_XR_KHR_visibility_mask = true;
    runtime.m_visibilityMaskDirty = 2;
    bool positive = true;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const auto result = runtime.xrPollEvent(reinterpret_cast<XrInstance>(1), &event);
        const auto& visibility = reinterpret_cast<const XrEventDataVisibilityMaskChangedKHR&>(event);
        if (result != XR_SUCCESS || event.type != XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR ||
            visibility.session != reinterpret_cast<XrSession>(1) || visibility.viewIndex != eye ||
            visibility.viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
            positive = false;
    }
    if (!positive) {
        ++failures;
        std::cerr << "FAIL: enabled live session must emit one visibility change per eye\n";
    } else std::cout << "PASS: enabled live session emits one visibility change per eye\n";
    expectNoEvents("quiescent visibility changes are consumed exactly once");

    // No graphics binding/OVR session is installed: actual DestroySession performs CPU-only empty cleanup.
    runtime.m_visibilityMaskDirty = 2;
    const auto destroyed = runtime.xrDestroySession(reinterpret_cast<XrSession>(1));
    if (destroyed != XR_SUCCESS || runtime.m_sessionCreated)
        throw std::runtime_error("Empty real session cleanup did not finish");
    if (runtime.m_visibilityMaskDirty.load() != 0) {
        ++failures;
        std::cerr << "FAIL: destroyed session retained pending visibility events\n";
    } else std::cout << "PASS: session destruction clears pending visibility events\n";
    expectNoEvents("destroyed session cannot emit pending visibility events");
    // Model a later settings change while no session exists.
    runtime.m_visibilityMaskDirty = 2;
    expectNoEvents("settings changes after destruction cannot emit an invalid session event");
    return failures ? 1 : 0;
}
};
}

int main() {
    try { return virtualdesktop_openxr::RuntimeInputRegression::run(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
