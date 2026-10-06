// CPU regressions against the real runtime methods. Friend access is confined to this test.
#include "pch.h"
#include "accessibility.h"
#include "utils.h"
#include "BodyState.h"
#include "trackers.h"
#include <hand_simulation.h>
#include <RuntimeConfiguration.h>
#include "framework/dispatch.gen.h"
#include "runtime.h"
#include <array>
#include <cstring>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

using virtualdesktop_openxr::OpenXrRuntime;
namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
inline static int failures = 0;
static void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

static void actionSetStartsWithNoInput() {
    alignas(OpenXrRuntime::ActionSet) std::array<unsigned char, sizeof(OpenXrRuntime::ActionSet)> storage;
    storage.fill(0x5a);
    auto* set = new (storage.data()) OpenXrRuntime::ActionSet;
    // xrSyncActions increments this on its first successful sync.
    ++set->generation;
    expect(set->generation == 1, "first action-set sync must have generation 1");
    expect(set->cachedInputState.Buttons == 0 && set->cachedInputState.Touches == 0 &&
               set->cachedInputState.IndexTrigger[0] == 0.f && set->cachedInputState.IndexTrigger[1] == 0.f,
           "an unsynchronized action set must not retain allocator input bytes");
    set->~ActionSet();
}

static void runtimeStartsWithNoInput() {
    alignas(OpenXrRuntime) std::array<unsigned char, sizeof(OpenXrRuntime)> storage;
    storage.fill(0x5a);
    auto* runtime = new (storage.data()) OpenXrRuntime;
    runtime->m_registryWatcher.reset();
    expect(runtime->m_cachedInputState.Buttons == 0 && runtime->m_cachedInputState.Touches == 0 &&
               runtime->m_cachedInputState.IndexTrigger[0] == 0.f &&
               runtime->m_cachedInputState.IndexTrigger[1] == 0.f,
           "hand simulation before the first sync must start with no controller input");
    runtime->~OpenXrRuntime();
}

static void unfocusedSyncDeactivatesAllActionSets() {
    OpenXrRuntime runtime;
    runtime.m_registryWatcher.reset();
    OpenXrRuntime::ActionSet sets[2]{};
    OpenXrRuntime::Action actions[2]{};
    runtime.m_sessionCreated = true;
    runtime.m_sessionState = XR_SESSION_STATE_VISIBLE;
    runtime.m_isControllerActive[xr::Side::Left] = true;
    for (int i = 0; i < 2; ++i) {
        auto& set = sets[i];
        set.priority = set.effectivePriority = 0;
        set.cachedInputState.Buttons = ovrButton_X;
        const auto setHandle = reinterpret_cast<XrActionSet>(&set);
        auto& action = actions[i];
        action.type = XR_ACTION_TYPE_BOOLEAN_INPUT;
        action.actionSet = setHandle;
        OpenXrRuntime::ActionSource source{};
        source.buttonMap = &set.cachedInputState.Buttons;
        source.buttonType = ovrButton_X;
        action.actionSources["/user/hand/left/input/x/click"] = source;
        runtime.m_attachedActionSets.insert(setHandle);
        runtime.m_actions.insert(reinterpret_cast<XrAction>(&action));
    }
    // A was active at the previous sync; the next sync selects B after focus is lost.
    runtime.m_activeActionSets.insert(reinterpret_cast<XrActionSet>(&sets[0]));
    const XrActiveActionSet active{reinterpret_cast<XrActionSet>(&sets[1]), XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    expect(runtime.xrSyncActions(reinterpret_cast<XrSession>(1), &sync) == XR_SESSION_NOT_FOCUSED,
           "unfocused sync must return XR_SESSION_NOT_FOCUSED");
    for (auto& action : actions) {
        XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO};
        query.action = reinterpret_cast<XrAction>(&action);
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        expect(runtime.xrGetActionStateBoolean(reinterpret_cast<XrSession>(1), &query, &state) == XR_SUCCESS,
               "action state query after an unfocused sync must succeed");
        expect(state.isActive == XR_FALSE && state.currentState == XR_FALSE &&
                   state.changedSinceLastSync == XR_FALSE && state.lastChangeTime == 0,
               "all action sets must report inactive after an unfocused sync");
    }
    runtime.m_sessionCreated = false;
    runtime.m_actions.clear();
    runtime.m_attachedActionSets.clear();
    runtime.m_activeActionSets.clear();
}

static void inactiveHandsCannotGenerateInput() {
    OpenXrRuntime runtime;
    runtime.m_registryWatcher.reset();
    virtualdesktop_openxr::BodyTracking::BodyStateV2 body{};
    runtime.m_bodyState = &body;
    for (uint32_t side = 0; side < xr::Side::Count; ++side) {
        runtime.m_cachedInputState = {};
        runtime.m_cachedBodyState = {};
        runtime.m_cachedBodyState.LeftHandActive = side == xr::Side::Right;
        runtime.m_cachedBodyState.RightHandActive = side == xr::Side::Left;
        runtime.m_cachedBodyState.LeftAimState.PinchStrengthIndex = 0.75f;
        runtime.m_cachedBodyState.RightAimState.PinchStrengthIndex = 0.75f;
        runtime.processHandGestures(side);
        expect(runtime.m_cachedInputState.IndexTrigger[side] == 0.f && runtime.m_cachedInputState.Buttons == 0,
               "an inactive hand must not consume stale joints or pinch strength from the active opposite hand");
        auto& aim = side == xr::Side::Left ? runtime.m_cachedBodyState.LeftAimState
                                          : runtime.m_cachedBodyState.RightAimState;
        aim.AimStatus = XR_HAND_TRACKING_AIM_VALID_BIT_FB;
        aim.AimPose.orientation.w = 1.f;
        XrPosef pose = xr::math::Pose::Identity();
        expect(!runtime.getPinchPose(static_cast<int>(side), pose, pose),
               "an inactive hand must not return its stale valid aim pose");
    }
    runtime.m_bodyState = nullptr;
}

static void poseStateAlwaysWritesActivity() {
    OpenXrRuntime runtime;
    runtime.m_registryWatcher.reset();
    runtime.m_sessionCreated = true;
    runtime.m_isControllerActive[xr::Side::Left] = true;
    OpenXrRuntime::ActionSet set{};
    set.priority = set.effectivePriority = 0;
    const auto setHandle = reinterpret_cast<XrActionSet>(&set);
    runtime.m_attachedActionSets.insert(setHandle);
    runtime.m_activeActionSets.insert(setHandle);
    OpenXrRuntime::Action action{};
    action.type = XR_ACTION_TYPE_POSE_INPUT;
    action.actionSet = setHandle;
    const auto actionHandle = reinterpret_cast<XrAction>(&action);
    runtime.m_actions.insert(actionHandle);
    XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO};
    query.action = actionHandle;
    XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
    state.isActive = XR_TRUE;
    expect(runtime.xrGetActionStatePose(reinterpret_cast<XrSession>(1), &query, &state) == XR_SUCCESS,
           "unbound attached pose action query must succeed");
    expect(state.isActive == XR_FALSE, "an unbound pose action must overwrite caller activity with FALSE");

    XrPath leftPath{};
    expect(runtime.xrStringToPath(XR_NULL_HANDLE, "/user/hand/left", &leftPath) == XR_SUCCESS,
           "left hand path must be created");
    action.subactionPaths.insert(leftPath);
    query.subactionPath = leftPath;
    action.actionSources["/user/hand/right/input/aim/pose"] = {};
    state.isActive = XR_TRUE;
    expect(runtime.xrGetActionStatePose(reinterpret_cast<XrSession>(1), &query, &state) == XR_SUCCESS,
           "pose action query with no matching source must succeed");
    expect(state.isActive == XR_FALSE, "a pose action with no matching source must overwrite activity with FALSE");

    action.actionSources["/user/hand/left/input/aim/pose"] = {};
    state.isActive = XR_FALSE;
    expect(runtime.xrGetActionStatePose(reinterpret_cast<XrSession>(1), &query, &state) == XR_SUCCESS &&
               state.isActive == XR_TRUE,
           "a bound active controller pose must still report active");
    runtime.m_sessionCreated = false;
    runtime.m_actions.clear();
    runtime.m_attachedActionSets.clear();
    runtime.m_activeActionSets.clear();
}

static void crossHandButtonsRequireBothHands() {
    OpenXrRuntime runtime;
    runtime.m_registryWatcher.reset();
    virtualdesktop_openxr::BodyTracking::BodyStateV2 body{};
    runtime.m_bodyState = &body;
    for (uint32_t side = 0; side < xr::Side::Count; ++side) {
        runtime.m_cachedInputState = {};
        runtime.m_cachedBodyState = {};
        runtime.m_cachedBodyState.LeftHandActive = side == xr::Side::Left;
        runtime.m_cachedBodyState.RightHandActive = side == xr::Side::Right;
        // The stale opposite index tip coincides with the active palm.
        runtime.processHandGestures(side);
        expect(runtime.m_cachedInputState.Buttons == 0,
               "stale joints from an inactive opposite hand must not produce a cross-hand button");
        runtime.m_cachedBodyState.LeftHandActive = runtime.m_cachedBodyState.RightHandActive = true;
        runtime.processHandGestures(side);
        expect(runtime.m_cachedInputState.Buttons == (side == xr::Side::Left ? ovrButton_Y : ovrButton_B),
               "two tracked hands in contact must still produce the appropriate cross-hand button");
    }
    runtime.m_bodyState = nullptr;
}

// Optional integration case for the parent to run sequentially with the other GPU tests.
// The CPU-only invocation does not load LibOVR or initialize a graphics device.
static void sessionRecreationClearsCachedInput(const wchar_t* nullRuntimeDirectory) {
    ovrInitParams init{};
    init.Flags = ovrInit_RequestVersion;
    init.RequestedMinorVersion = OVR_MINOR_VERSION;
    const auto result = ovr_InitializeWithPathOverride(&init, nullRuntimeDirectory);
    if (OVR_FAILURE(result)) throw std::runtime_error("OVRNull initialization failed");
    OpenXrRuntime runtime;
    runtime.m_registryWatcher.reset();
    runtime.m_isOVRLoaded = true;
    runtime.m_instanceCreated = runtime.m_systemCreated = true;
    runtime.has_XR_MND_headless = true;
    if (OVR_FAILURE(ovr_Create(&runtime.m_ovrSession, reinterpret_cast<ovrGraphicsLuid*>(&runtime.m_adapterLuid))))
        throw std::runtime_error("OVRNull session creation failed");
    for (int repeat = 0; repeat < 2; ++repeat) {
        runtime.m_cachedInputState.Buttons = ovrButton_X;
        runtime.m_cachedInputState.Touches = ovrTouch_A;
        runtime.m_cachedInputState.IndexTrigger[0] = 0.75f;
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        info.systemId = 1;
        XrSession session{};
        expect(runtime.xrCreateSession(reinterpret_cast<XrInstance>(1), &info, &session) == XR_SUCCESS,
               "OVRNull headless session must be created");
        expect(runtime.m_cachedInputState.Buttons == 0 && runtime.m_cachedInputState.Touches == 0 &&
                   runtime.m_cachedInputState.IndexTrigger[0] == 0.f,
               "new sessions must not reuse previous controller input before their first sync");
        expect(runtime.xrDestroySession(session) == XR_SUCCESS, "OVRNull headless session must be destroyed");
    }
}
static int run(int argc, wchar_t** argv) {
    try {
        actionSetStartsWithNoInput();
        runtimeStartsWithNoInput();
        unfocusedSyncDeactivatesAllActionSets();
        inactiveHandsCannotGenerateInput();
        poseStateAlwaysWritesActivity();
        crossHandButtonsRequireBothHands();
        if (argc == 3 && std::wstring_view(argv[1]) == L"--session-reset")
            sessionRecreationClearsCachedInput(argv[2]);
        else if (argc != 1)
            throw std::runtime_error("usage: vr_input_regression [--session-reset OVRNull-directory]");
        if (failures) return 1;
        std::cout << "PASS: VR input initialization, focus loss, and hand validity regressions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
};
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    return virtualdesktop_openxr::RuntimeInputRegression::run(argc, argv);
}
