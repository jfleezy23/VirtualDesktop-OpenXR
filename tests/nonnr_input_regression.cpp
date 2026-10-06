// CPU regressions against real action, space and event methods. No registry writes or graphics device.
#include "pch.h"
#include "runtime.h"
#include <array>
#include <cstring>
#include <future>
#include <thread>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
bool invalidControllerAngularVelocity = false;
ovrTrackedDeviceType velocityOverrideDevice = ovrTrackedDevice_HMD;
std::optional<float> velocityAngularOverride;
std::optional<float> velocityLinearOverride;
unsigned inputStateCalls = 0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Replace only the external pose/origin provider. xrLocateSpace and all runtime math remain real.
ovrResult OVR_CDECL fixtureDevicePoses(ovrSession, ovrTrackedDeviceType* devices, int count,
                                     double time, ovrPoseStatef* poses) {
    for (int i = 0; i < count; ++i) {
        if (devices[i] != ovrTrackedDevice_HMD && devices[i] != ovrTrackedDevice_LTouch &&
            devices[i] != ovrTrackedDevice_RTouch) return ovrError_DeviceUnavailable;
        poses[i] = {};
        poses[i].ThePose.Orientation.w = 1.f;
        poses[i].ThePose.Position = {1.f, 2.f, 3.f};
        poses[i].LinearVelocity = {1.f, 2.f, 3.f};
        poses[i].AngularVelocity = {4.f, 5.f, 6.f};
        if (invalidControllerAngularVelocity && devices[i] != ovrTrackedDevice_HMD)
            poses[i].AngularVelocity.x = std::numeric_limits<float>::quiet_NaN();
        if (devices[i] == velocityOverrideDevice) {
            if (velocityAngularOverride) poses[i].AngularVelocity.x = *velocityAngularOverride;
            if (velocityLinearOverride) poses[i].LinearVelocity.x = *velocityLinearOverride;
        }
        poses[i].TimeInSeconds = time;
    }
    return ovrSuccess;
}
ovrTrackingOrigin OVR_CDECL fixtureTrackingOrigin(ovrSession) { return ovrTrackingOrigin_FloorLevel; }
float OVR_CDECL fixtureFloat(ovrSession, const char* property, float fallback) {
    return !strcmp(property, OVR_KEY_EYE_HEIGHT) ? 0.f : fallback;
}

struct PoseProvider {
    decltype(&ovr_GetDevicePoses) poses{};
    decltype(&ovr_GetTrackingOriginType) origin{};
    decltype(&ovr_GetFloat) configFloat{};
    PoseProvider(const wchar_t* backendDirectory) {
        ovrInitParams init{};
        init.Flags = ovrInit_RequestVersion;
        init.RequestedMinorVersion = OVR_MINOR_VERSION;
        // The shim requires a directory separator after the override directory.
        const auto directory = std::filesystem::absolute(backendDirectory).wstring() + L"\\";
        require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, directory.c_str())),
                "OVRNull initialization failed");
        const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
        require(module != nullptr, "Explicit OVR backend was not loaded");
        poses = reinterpret_cast<decltype(poses)>(GetProcAddress(module, "ovr_GetDevicePoses"));
        origin = reinterpret_cast<decltype(origin)>(GetProcAddress(module, "ovr_GetTrackingOriginType"));
        configFloat = reinterpret_cast<decltype(configFloat)>(GetProcAddress(module, "ovr_GetFloat"));
        require(poses && origin && configFloat, "OVR backend pose/origin/config exports missing");
        require(DetourTransactionBegin() == NO_ERROR, "Pose detour begin failed");
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto attachedPoses = DetourAttach(reinterpret_cast<PVOID*>(&poses), fixtureDevicePoses);
        const auto attachedOrigin = DetourAttach(reinterpret_cast<PVOID*>(&origin), fixtureTrackingOrigin);
        const auto attachedConfig = DetourAttach(reinterpret_cast<PVOID*>(&configFloat), fixtureFloat);
        if (updated || attachedPoses || attachedOrigin || attachedConfig) {
            DetourTransactionAbort();
            throw std::runtime_error("Pose detour attach failed");
        }
        require(DetourTransactionCommit() == NO_ERROR, "Pose detour commit failed");
    }
    ~PoseProvider() {
        const auto begun = DetourTransactionBegin();
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto detachedPoses = DetourDetach(reinterpret_cast<PVOID*>(&poses), fixtureDevicePoses);
        const auto detachedOrigin = DetourDetach(reinterpret_cast<PVOID*>(&origin), fixtureTrackingOrigin);
        const auto detachedConfig = DetourDetach(reinterpret_cast<PVOID*>(&configFloat), fixtureFloat);
        const auto committed = DetourTransactionCommit();
        // Returning with an installed detour would invalidate the fixture function pointers.
        if (begun || updated || detachedPoses || detachedOrigin || detachedConfig || committed) {
            std::cerr << "FAIL: pose provider detachment failed\n";
            std::terminate();
        }
    }
};

ovrResult OVR_CDECL fixtureInputState(ovrSession, ovrControllerType, ovrInputState* state) {
    ++inputStateCalls;
    *state = {};
    state->TimeInSeconds = 10.;
    return ovrSuccess;
}
unsigned int OVR_CDECL fixtureConnectedControllers(ovrSession) { return ovrControllerType_Touch; }
struct InputProvider {
    decltype(&ovr_GetInputState) input{};
    decltype(&ovr_GetConnectedControllerTypes) connected{};
    InputProvider() {
        const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
        input = reinterpret_cast<decltype(input)>(GetProcAddress(module, "ovr_GetInputState"));
        connected = reinterpret_cast<decltype(connected)>(GetProcAddress(module, "ovr_GetConnectedControllerTypes"));
        require(input && connected, "OVRNull input exports missing");
        require(DetourTransactionBegin() == NO_ERROR, "Input detour begin failed");
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto attachedInput = DetourAttach(reinterpret_cast<PVOID*>(&input), fixtureInputState);
        const auto attachedConnected = DetourAttach(reinterpret_cast<PVOID*>(&connected), fixtureConnectedControllers);
        if (updated || attachedInput || attachedConnected) {
            DetourTransactionAbort();
            throw std::runtime_error("Input detour attach failed");
        }
        require(DetourTransactionCommit() == NO_ERROR, "Input detour commit failed");
    }
    ~InputProvider() {
        const auto begun = DetourTransactionBegin();
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto detachedInput = DetourDetach(reinterpret_cast<PVOID*>(&input), fixtureInputState);
        const auto detachedConnected = DetourDetach(reinterpret_cast<PVOID*>(&connected), fixtureConnectedControllers);
        const auto committed = DetourTransactionCommit();
        if (begun || updated || detachedInput || detachedConnected || committed) std::terminate();
    }
};

decltype(&HeapFree) originalRetirementFree = HeapFree;
void* watchedAllocation{};
size_t watchedAllocationSize{};
unsigned retirementCalls{};
HANDLE retiredHeap{};
DWORD retiredFlags{};
void* retiredAllocation{};
BOOL WINAPI quarantineRetiredAllocation(HANDLE heap, DWORD flags, void* memory) {
    if (memory == watchedAllocation) {
        ++retirementCalls;
        retiredHeap = heap;
        retiredFlags = flags;
        retiredAllocation = memory;
        // Simulate immediate heap reuse, while keeping the block mapped for deterministic runtime evidence.
        memset(memory, 0xdd, watchedAllocationSize);
        return TRUE;
    }
    return originalRetirementFree(heap, flags, memory);
}
struct RetirementObserver {
    RetirementObserver() {
        require(DetourTransactionBegin() == NO_ERROR, "Heap retirement detour begin failed");
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto attached = DetourAttach(reinterpret_cast<PVOID*>(&originalRetirementFree), quarantineRetiredAllocation);
        if (updated || attached) {
            DetourTransactionAbort();
            throw std::runtime_error("Heap retirement detour attach failed");
        }
        require(DetourTransactionCommit() == NO_ERROR, "Heap retirement detour commit failed");
    }
    void watch(void* allocation, size_t size) {
        require(!retiredAllocation, "Previous quarantined block was not released");
        watchedAllocation = allocation;
        watchedAllocationSize = size;
        retirementCalls = 0;
    }
    void releaseRetired() {
        watchedAllocation = nullptr;
        if (retiredAllocation) {
            require(originalRetirementFree(retiredHeap, retiredFlags, retiredAllocation) != FALSE,
                    "Quarantined heap block cleanup failed");
            retiredAllocation = nullptr;
        }
    }
    ~RetirementObserver() {
        watchedAllocation = nullptr;
        if (retiredAllocation) {
            originalRetirementFree(retiredHeap, retiredFlags, retiredAllocation);
            retiredAllocation = nullptr;
        }
        const auto begun = DetourTransactionBegin();
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto detached = DetourDetach(reinterpret_cast<PVOID*>(&originalRetirementFree), quarantineRetiredAllocation);
        const auto committed = DetourTransactionCommit();
        if (begun || updated || detached || committed) std::terminate();
    }
};
bool closeVector(const XrVector3f& actual, const XrVector3f& expected) {
    return std::abs(actual.x - expected.x) < 0.0001f && std::abs(actual.y - expected.y) < 0.0001f &&
           std::abs(actual.z - expected.z) < 0.0001f;
}
}

namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
    inline static int failures = 0;
    static XrSession session() { return reinterpret_cast<XrSession>(1); }
    static XrInstance instance() { return reinterpret_cast<XrInstance>(1); }
    static void expect(bool passed, const char* label) {
        if (passed) std::cout << "PASS: " << label << '\n';
        else { ++failures; std::cout << "FAIL: " << label << '\n'; }
    }
    static void seed(OpenXrRuntime& runtime) {
        runtime.stopRegistryWatcher();
        runtime.m_instanceCreated = true;
        runtime.m_sessionCreated = true;
    }
    static XrPath path(OpenXrRuntime& runtime, const char* text) {
        XrPath result{};
        require(runtime.xrStringToPath(instance(), text, &result) == XR_SUCCESS, "Fixture path creation failed");
        return result;
    }
    static XrActionSet createSet(OpenXrRuntime& runtime, const char* name = nullptr) {
        XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(info.actionSetName, name ? name : "regression");
        strcpy_s(info.localizedActionSetName, name ? name : "Regression");
        XrActionSet result{};
        require(runtime.xrCreateActionSet(instance(), &info, &result) == XR_SUCCESS, "Fixture action set failed");
        return result;
    }
    static XrAction createAction(OpenXrRuntime& runtime, XrActionSet set, XrActionType type, XrPath left,
                                 XrPath right = XR_NULL_PATH) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(info.actionName, "input");
        strcpy_s(info.localizedActionName, "Input");
        info.actionType = type;
        const XrPath paths[] = {left, right};
        info.countSubactionPaths = right == XR_NULL_PATH ? 1 : 2;
        info.subactionPaths = paths;
        XrAction result{};
        require(runtime.xrCreateAction(set, &info, &result) == XR_SUCCESS, "Fixture action creation failed");
        return result;
    }

    static void floats(bool controlsOnly) {
        OpenXrRuntime runtime;
        seed(runtime);
        const auto left = path(runtime, "/user/hand/left");
        const auto right = path(runtime, "/user/hand/right");
        const auto setHandle = createSet(runtime);
        const auto actionHandle = createAction(runtime, setHandle, XR_ACTION_TYPE_FLOAT_INPUT, left, right);
        auto& set = *reinterpret_cast<OpenXrRuntime::ActionSet*>(setHandle);
        auto& action = *reinterpret_cast<OpenXrRuntime::Action*>(actionHandle);
        runtime.m_attachedActionSets.insert(setHandle);
        runtime.m_activeActionSets.insert(setHandle);
        runtime.m_isControllerActive[0] = true;
        runtime.m_isControllerActive[1] = true;
        runtime.m_actionSourcePriority[0] = set.effectivePriority;
        set.cachedInputState.TimeInSeconds = 10.;
        float values[2]{};
        OpenXrRuntime::ActionSource source{};
        source.floatValue = values;
        action.actionSources["/user/hand/left/input/thumbstick/x"] = source;
        action.actionSources["/user/hand/right/input/thumbstick/x"] = source;
        XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO};
        query.action = actionHandle;
        struct Case { const char* label; float left; float right; float expected; bool control; };
        const Case cases[] = {
            {"positive float maximum", 0.2f, 0.8f, 0.8f, true},
            {"zero float remains active", 0.f, 0.f, 0.f, true},
            {"mixed signed float chooses greatest magnitude", -0.9f, 0.2f, -0.9f, false},
            {"two negative floats choose greatest magnitude", -0.8f, -0.2f, -0.8f, false},
            {"negative float beats zero by magnitude", -0.7f, 0.f, -0.7f, false},
        };
        for (const auto& item : cases) {
            if (controlsOnly && !item.control) continue;
            values[0] = item.left;
            values[1] = item.right;
            ++set.generation;
            XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
            const auto result = runtime.xrGetActionStateFloat(session(), &query, &state);
            std::cout << item.label << " result=" << result << " active=" << state.isActive
                      << " actual=" << state.currentState << " expected=" << item.expected << '\n';
            expect(result == XR_SUCCESS && state.isActive == XR_TRUE && state.currentState == item.expected,
                   item.label);
        }
        values[0] = -0.6f;
        values[1] = 0.6f;
        ++set.generation;
        XrActionStateFloat first{XR_TYPE_ACTION_STATE_FLOAT};
        require(runtime.xrGetActionStateFloat(session(), &query, &first) == XR_SUCCESS, "Tie query failed");
        bool stable = first.isActive && std::abs(first.currentState) == 0.6f;
        // Either source may win a magnitude tie, but unchanged sources must produce a stable selection.
        for (unsigned i = 0; i < 4; ++i) {
            XrActionStateFloat next{XR_TYPE_ACTION_STATE_FLOAT};
            stable &= runtime.xrGetActionStateFloat(session(), &query, &next) == XR_SUCCESS &&
                      next.isActive == XR_TRUE && next.currentState == first.currentState;
        }
        expect(stable, "equal-magnitude signed float ties remain stable");
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Float session cleanup failed");
    }

    static void actionPaths(bool controlsOnly) {
        OpenXrRuntime runtime;
        seed(runtime);
        const auto left = path(runtime, "/user/hand/left");
        const auto right = path(runtime, "/user/hand/right");
        const auto gamepad = path(runtime, "/user/gamepad");
        const auto action = createAction(runtime, createSet(runtime), XR_ACTION_TYPE_POSE_INPUT, left);
        struct Case { const char* label; XrPath path; XrResult expected; bool control; };
        const Case cases[] = {
            {"declared action-space path is supported", left, XR_SUCCESS, true},
            {"null action-space path is accepted", XR_NULL_PATH, XR_SUCCESS, true},
            {"valid undeclared hand action-space path is unsupported", right, XR_ERROR_PATH_UNSUPPORTED, false},
            {"valid undeclared gamepad action-space path is unsupported", gamepad, XR_ERROR_PATH_UNSUPPORTED, false},
            {"unknown action-space path is invalid", XrPath(0xdeadbeef), XR_ERROR_PATH_INVALID, false},
        };
        for (const auto& item : cases) {
            if (controlsOnly && !item.control) continue;
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = item.path;
            info.poseInActionSpace.orientation.w = 1.f;
            XrSpace space{};
            const auto result = runtime.xrCreateActionSpace(session(), &info, &space);
            std::cout << item.label << " result=" << result << " expected=" << item.expected << '\n';
            expect(result == item.expected, item.label);
            if (result == XR_SUCCESS)
                require(runtime.xrDestroySpace(space) == XR_SUCCESS, "Action-space fixture cleanup failed");
        }
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Action-path session cleanup failed");
    }

    static void actionSetLifetime(const wchar_t* backendDirectory) {
        RetirementObserver observer;
        auto* control = HeapAlloc(GetProcessHeap(), 0, 32);
        require(control != nullptr, "Heap observer positive-control allocation failed");
        observer.watch(control, 32);
        require(HeapFree(GetProcessHeap(), 0, control) != FALSE && retirementCalls == 1,
                "Heap observer positive control did not reach the detour");
        observer.releaseRetired();
        auto runtime = std::make_unique<OpenXrRuntime>();
        seed(*runtime);
        {
            PoseProvider poses(backendDirectory);
            InputProvider inputs;
            runtime->m_sessionState = XR_SESSION_STATE_FOCUSED;
            runtime->m_supportsHandTracking = false;
            runtime->m_cachedControllerType[0] = runtime->m_cachedControllerType[1] = "touch_controller";
            runtime->m_controllerGripPose[0] = xr::math::Pose::Identity();
            const auto left = path(*runtime, "/user/hand/left");
            const auto setHandle = createSet(*runtime);
            const auto actionHandle = createAction(*runtime, setHandle, XR_ACTION_TYPE_POSE_INPUT, left);
            auto& set = *reinterpret_cast<OpenXrRuntime::ActionSet*>(setHandle);
            set.priority = set.effectivePriority = 7;
            OpenXrRuntime::ActionSource source{};
            source.sourceIndex = OpenXrRuntime::ActionSourceIndex::Grip;
            source.realPath = "/user/hand/left/input/grip/pose";
            reinterpret_cast<OpenXrRuntime::Action*>(actionHandle)->actionSources[source.realPath] = source;
            XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attach.countActionSets = 1;
            attach.actionSets = &setHandle;
            require(runtime->xrAttachSessionActionSets(session(), &attach) == XR_SUCCESS, "Lifetime fixture attach failed");
            const XrActiveActionSet active{setHandle, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            require(runtime->xrSyncActions(session(), &sync) == XR_SUCCESS, "Lifetime fixture initial real sync failed");
            XrActionSpaceCreateInfo actionInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            actionInfo.action = actionHandle;
            actionInfo.subactionPath = left;
            actionInfo.poseInActionSpace.orientation.w = 1.f;
            XrSpace actionSpace{};
            require(runtime->xrCreateActionSpace(session(), &actionInfo, &actionSpace) == XR_SUCCESS,
                    "Lifetime action-space creation failed");
            XrReferenceSpaceCreateInfo baseInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            baseInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            baseInfo.poseInReferenceSpace.orientation.w = 1.f;
            XrSpace base{};
            require(runtime->xrCreateReferenceSpace(session(), &baseInfo, &base) == XR_SUCCESS,
                    "Lifetime base-space creation failed");
            const auto locate = [&](bool tracked, const char* label) {
                XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
                if (!tracked)
                    velocity.velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                location.next = &velocity;
                const auto result = runtime->xrLocateSpace(actionSpace, base, 1000000000, &location);
                const auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                std::cout << label << " result=" << result << " flags=" << location.locationFlags
                          << " velocityFlags=" << velocity.velocityFlags << '\n';
                if (tracked)
                    expect(result == XR_SUCCESS && (location.locationFlags & valid) == valid &&
                           closeVector(location.pose.position, {1.f, 2.f, 3.f}), label);
                else expect(result == XR_SUCCESS && !(location.locationFlags & valid) && !velocity.velocityFlags, label);
            };
            locate(true, "live pose space is locatable after real active sync");
            observer.watch(reinterpret_cast<void*>(setHandle), sizeof(OpenXrRuntime::ActionSet));
            require(runtime->xrDestroyActionSet(setHandle) == XR_SUCCESS, "Lifetime action-set destruction failed");
            std::cout << "action-set resource retirements with surviving space=" << retirementCalls << '\n';
            expect(retirementCalls == 0, "destroyed action-set handle retains resources needed by its surviving space");
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.action = actionHandle;
            get.subactionPath = left;
            XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
            expect(runtime->xrGetActionStatePose(session(), &get, &poseState) == XR_ERROR_HANDLE_INVALID &&
                   runtime->xrDestroyActionSet(setHandle) == XR_ERROR_HANDLE_INVALID,
                   "destroyed action and action-set handles are invalidated");
            // Only valid surviving space handles are located after destruction; no dead ActionSet is dereferenced by the fixture.
            locate(true, "surviving pose space remains locatable under the most recent active sync");
            sync.countActiveActionSets = 0;
            sync.activeActionSets = nullptr;
            require(runtime->xrSyncActions(session(), &sync) == XR_SUCCESS, "Lifetime fixture inactive real sync failed");
            locate(false, "surviving pose space becomes unlocatable after the next inactive sync");
            require(runtime->xrDestroySpace(actionSpace) == XR_SUCCESS && runtime->xrDestroySpace(base) == XR_SUCCESS,
                    "Lifetime spaces cleanup failed");
            require(runtime->xrDestroySession(session()) == XR_SUCCESS, "Lifetime session cleanup failed");
        }
        // Detach the backend spies before runtime destruction unloads OVR; observe its final resource retirement.
        runtime.reset();
        expect(retirementCalls == 1, "action-set allocation is retired exactly once by final runtime teardown");
        observer.releaseRetired();
    }

    static void queryCache() {
        const XrActionType types[] = {
            XR_ACTION_TYPE_FLOAT_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_VECTOR2F_INPUT};
        for (const auto type : types) {
            OpenXrRuntime runtime;
            seed(runtime);
            const auto left = path(runtime, "/user/hand/left");
            const auto right = path(runtime, "/user/hand/right");
            const auto setHandle = createSet(runtime);
            const auto actionHandle = createAction(runtime, setHandle, type, left, right);
            auto& set = *reinterpret_cast<OpenXrRuntime::ActionSet*>(setHandle);
            auto& action = *reinterpret_cast<OpenXrRuntime::Action*>(actionHandle);
            runtime.m_attachedActionSets.insert(setHandle);
            runtime.m_activeActionSets.insert(setHandle);
            runtime.m_isControllerActive[0] = runtime.m_isControllerActive[1] = true;
            runtime.m_actionSourcePriority[0] = set.effectivePriority;
            set.generation = 1;
            set.cachedInputState.TimeInSeconds = 10.;
            set.cachedInputState.IndexTrigger[0] = 0.2f;
            set.cachedInputState.IndexTrigger[1] = 0.9f;
            set.cachedInputState.Thumbstick[0] = {0.2f, 0.f};
            set.cachedInputState.Thumbstick[1] = {0.9f, 0.f};
            set.cachedInputState.Buttons = ovrButton_A;
            for (unsigned side = 0; side < 2; ++side) {
                OpenXrRuntime::ActionSource source{};
                const std::string hand = side ? "/user/hand/right" : "/user/hand/left";
                if (type == XR_ACTION_TYPE_FLOAT_INPUT) {
                    source.floatValue = set.cachedInputState.IndexTrigger;
                    source.realPath = hand + "/input/trigger/value";
                } else if (type == XR_ACTION_TYPE_BOOLEAN_INPUT) {
                    source.buttonMap = &set.cachedInputState.Buttons;
                    source.buttonType = side ? ovrButton_A : ovrButton_X;
                    source.realPath = hand + (side ? "/input/a/click" : "/input/x/click");
                } else {
                    source.vector2fValue = set.cachedInputState.Thumbstick;
                    source.realPath = hand + "/input/thumbstick";
                }
                action.actionSources[source.realPath] = source;
            }
            struct Snapshot { XrResult result; XrBool32 active; XrBool32 changed; XrTime time; float x; float y; };
            const auto query = [&](XrPath subaction) -> Snapshot {
                XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
                info.action = actionHandle;
                info.subactionPath = subaction;
                if (type == XR_ACTION_TYPE_FLOAT_INPUT) {
                    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
                    const auto result = runtime.xrGetActionStateFloat(session(), &info, &state);
                    return {result, state.isActive, state.changedSinceLastSync, state.lastChangeTime, state.currentState, 0.f};
                } else if (type == XR_ACTION_TYPE_BOOLEAN_INPUT) {
                    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                    const auto result = runtime.xrGetActionStateBoolean(session(), &info, &state);
                    return {result, state.isActive, state.changedSinceLastSync, state.lastChangeTime, float(state.currentState), 0.f};
                } else {
                    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
                    const auto result = runtime.xrGetActionStateVector2f(session(), &info, &state);
                    return {result, state.isActive, state.changedSinceLastSync, state.lastChangeTime,
                            state.currentState.x, state.currentState.y};
                }
            };
            const char* label = type == XR_ACTION_TYPE_FLOAT_INPUT ? "float" :
                                type == XR_ACTION_TYPE_BOOLEAN_INPUT ? "boolean" : "vector2f";
            const XrPath initialOrder[] = {left, right, XR_NULL_PATH};
            XrTime priorTimes[3]{};
            const float leftValue = type == XR_ACTION_TYPE_BOOLEAN_INPUT ? 0.f : 0.2f;
            const float combinedValue = type == XR_ACTION_TYPE_BOOLEAN_INPUT ? 1.f : 0.9f;
            bool initialValues = true;
            for (unsigned i = 0; i < 3; ++i) {
                const auto state = query(initialOrder[i]);
                priorTimes[i] = state.time;
                initialValues &= state.result == XR_SUCCESS && state.active == XR_TRUE &&
                                 state.x == (i == 0 ? leftValue : combinedValue) && state.y == 0.f;
            }
            std::cout << label << " generation1 lastChangeTimes=" << priorTimes[0] << ',' << priorTimes[1]
                      << ',' << priorTimes[2] << '\n';
            expect(initialValues, "each cache query exposes its distinct bound hand or aggregate input");
            // Represent the next successful sync with unchanged input, but a later input sample timestamp.
            set.generation = 2;
            set.cachedInputState.TimeInSeconds = 20.;
            const XrPath nextOrder[] = {left, XR_NULL_PATH, right};
            const char* sideLabels[] = {"left", "aggregate", "right"};
            const unsigned priorIndex[] = {0, 2, 1};
            for (unsigned round = 0; round < 4; ++round) {
                for (unsigned i = 0; i < 3; ++i) {
                    const auto state = query(nextOrder[i]);
                    const bool unchanged = state.result == XR_SUCCESS && state.active == XR_TRUE &&
                        state.x == (i == 0 ? leftValue : combinedValue) && state.y == 0.f &&
                        state.changed == XR_FALSE && state.time == priorTimes[priorIndex[i]];
                    std::cout << (unchanged ? "PASS: " : "FAIL: ") << label << ' ' << sideLabels[i]
                              << " unchanged generation2 round=" << round << " changed=" << state.changed
                              << " time=" << state.time << " expectedTime=" << priorTimes[priorIndex[i]] << '\n';
                    if (!unchanged) ++failures;
                }
            }
            require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Query-cache session cleanup failed");
        }
    }

    static void actionPose() {
        OpenXrRuntime runtime;
        seed(runtime);
        const auto left = path(runtime, "/user/hand/left");
        const auto action = createAction(runtime, createSet(runtime), XR_ACTION_TYPE_POSE_INPUT, left);
        struct Case { const char* label; XrQuaternionf orientation; XrResult expected; };
        const Case cases[] = {
            {"zero action-space quaternion is invalid", {0.f, 0.f, 0.f, 0.f}, XR_ERROR_POSE_INVALID},
            {"nonunit action-space quaternion is invalid", {0.f, 0.f, 0.f, 2.f}, XR_ERROR_POSE_INVALID},
            {"negative identity action-space quaternion is normalized", {0.f, 0.f, 0.f, -1.f}, XR_SUCCESS},
            {"yaw action-space quaternion is normalized", {0.f, 0.70710678f, 0.f, 0.70710678f}, XR_SUCCESS},
        };
        for (const auto& item : cases) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = left;
            info.poseInActionSpace.orientation = item.orientation;
            XrSpace space{};
            const auto before = runtime.m_spaces.size();
            const auto result = runtime.xrCreateActionSpace(session(), &info, &space);
            std::cout << item.label << " result=" << result << " expected=" << item.expected << '\n';
            expect(result == item.expected, item.label);
            if (item.expected != XR_SUCCESS)
                expect(runtime.m_spaces.size() == before && space == XR_NULL_HANDLE,
                       "invalid action-space pose does not allocate a space");
            if (result == XR_SUCCESS)
                require(runtime.xrDestroySpace(space) == XR_SUCCESS, "Action-pose space cleanup failed");
        }
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Action-pose session cleanup failed");
    }

    static void events(bool controlsOnly) {
        OpenXrRuntime runtime;
        seed(runtime);
        runtime.has_XR_EXT_local_floor = true;
        runtime.m_currentInteractionProfileDirty = true;
        runtime.m_shouldRecenter = 3;
        runtime.m_recenterTime = 123456;
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const auto profileResult = runtime.xrPollEvent(instance(), &event);
        const auto& profile = reinterpret_cast<const XrEventDataInteractionProfileChanged&>(event);
        expect(profileResult == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED &&
               profile.session == session(), "live interaction-profile event names the session");
        const XrReferenceSpaceType expectedTypes[] = {
            XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE};
        bool recentered = true;
        for (const auto type : expectedTypes) {
            event = {XR_TYPE_EVENT_DATA_BUFFER};
            const auto result = runtime.xrPollEvent(instance(), &event);
            const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
            recentered &= result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING &&
                          change.session == session() && change.referenceSpaceType == type && change.changeTime == 123456;
        }
        expect(recentered, "live recenter emits each pending reference-space change");
        event = {XR_TYPE_EVENT_DATA_BUFFER};
        expect(runtime.xrPollEvent(instance(), &event) == XR_EVENT_UNAVAILABLE,
               "live interaction and recenter events are consumed exactly once");
        if (!controlsOnly) {
            runtime.m_currentInteractionProfileDirty = true;
            runtime.m_shouldRecenter = 3;
            runtime.m_sessionEventQueue.push_back({XR_SESSION_STATE_READY, 1.});
        }
        // No OVR session or graphics bindings: exercise real empty session cleanup.
        require(runtime.xrDestroySession(session()) == XR_SUCCESS && !runtime.m_sessionCreated,
                "Real empty session destruction did not finish");
        require(runtime.m_instanceCreated, "Session destruction incorrectly destroyed the instance");
        bool unavailable = true;
        for (unsigned attempt = 0; attempt < 5; ++attempt) {
            event = {XR_TYPE_EVENT_DATA_BUFFER};
            const auto result = runtime.xrPollEvent(instance(), &event);
            if (result != XR_EVENT_UNAVAILABLE) {
                unavailable = false;
                std::cerr << "post-destroy poll=" << attempt << " result=" << result << " event=" << event.type;
                if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
                    std::cerr << " session=" << reinterpret_cast<const XrEventDataInteractionProfileChanged&>(event).session;
                else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
                    std::cerr << " session=" << reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event).session;
                std::cerr << '\n';
            }
        }
        expect(unavailable, "destroyed session cannot emit pending interaction or recenter events");
    }

    static void pollLock() {
        OpenXrRuntime runtime;
        seed(runtime);
        // Keep the state machine stable so polling needs neither an OVR session nor an OVR time query.
        runtime.m_isHeadless = true;
        runtime.m_sessionBegun = true;
        runtime.m_sessionState = XR_SESSION_STATE_FOCUSED;
        runtime.m_currentInteractionProfileDirty = true;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::promise<XrResult> completed;
        auto completedFuture = completed.get_future();
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        std::unique_lock frameLock(runtime.m_frameMutex);
        std::thread poller([&] {
            started.set_value();
            try { completed.set_value(runtime.xrPollEvent(instance(), &event)); }
            catch (...) { completed.set_exception(std::current_exception()); }
        });
        startedFuture.wait();
        const bool blocked = completedFuture.wait_for(250ms) == std::future_status::timeout;
        // Release and join before assertions, including the early-completion failure on the baseline.
        frameLock.unlock();
        poller.join();
        const auto result = completedFuture.get();
        expect(blocked, "event polling waits while the frame mutex is held");
        const auto& profile = reinterpret_cast<const XrEventDataInteractionProfileChanged&>(event);
        expect(result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED &&
               profile.session == session(), "event polling delivers its pending event after frame mutex release");
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Poll-lock session cleanup failed");
    }

    static void syncValidation(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        seed(runtime);
        PoseProvider poses(backendDirectory);
        InputProvider inputs;
        runtime.m_sessionState = XR_SESSION_STATE_FOCUSED;
        const auto left = path(runtime, "/user/hand/left");
        const auto right = path(runtime, "/user/hand/right");
        const XrActionSet sets[] = {createSet(runtime, "set_a"), createSet(runtime, "set_b"), createSet(runtime, "set_c")};
        createAction(runtime, sets[0], XR_ACTION_TYPE_FLOAT_INPUT, left);
        createAction(runtime, sets[1], XR_ACTION_TYPE_FLOAT_INPUT, left);
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 2;
        attach.actionSets = sets;
        require(runtime.xrAttachSessionActionSets(session(), &attach) == XR_SUCCESS, "Sync validation fixture attach failed");
        struct Case { const char* label; XrActiveActionSet active[2]; uint32_t count; XrResult expected; };
        // All action-set handles are live. C is unattached; right is an interned but undeclared subaction path.
        const Case cases[] = {
            {"late live unattached set", {{sets[1], XR_NULL_PATH}, {sets[2], XR_NULL_PATH}}, 2, XR_ERROR_ACTIONSET_NOT_ATTACHED},
            {"early unsupported live subaction path", {{sets[1], right}, {}}, 1, XR_ERROR_PATH_UNSUPPORTED},
            {"late unsupported live subaction path", {{sets[1], XR_NULL_PATH}, {sets[0], right}}, 2, XR_ERROR_PATH_UNSUPPORTED},
        };
        for (const auto& item : cases) {
            runtime.m_activeActionSets.clear();
            runtime.m_activeActionSets.insert(sets[0]);
            const uint32_t priorities[] = {3, 7, 11};
            const uint32_t effective[] = {31, 79, 11};
            const uint64_t generations[] = {5, 7, 0};
            std::array<std::array<unsigned char, sizeof(ovrInputState)>, 3> cached{};
            for (unsigned i = 0; i < 3; ++i) {
                auto& set = *reinterpret_cast<OpenXrRuntime::ActionSet*>(sets[i]);
                set.priority = priorities[i];
                set.effectivePriority = effective[i];
                set.generation = generations[i];
                set.cachedInputState = {};
                set.cachedInputState.TimeInSeconds = i == 2 ? 0. : 10. * (i + 1);
                set.cachedInputState.Buttons = i == 0 ? ovrButton_X : i == 1 ? ovrButton_A : 0;
                memcpy(cached[i].data(), &set.cachedInputState, sizeof(set.cachedInputState));
            }
            for (auto& priority : runtime.m_actionSourcePriority) priority = 31;
            inputStateCalls = 0;
            const auto previousActive = runtime.m_activeActionSets;
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = item.count;
            sync.activeActionSets = item.active;
            const auto result = runtime.xrSyncActions(session(), &sync);
            bool effectiveUnchanged = true;
            bool cachedUnchanged = true;
            for (unsigned i = 0; i < 3; ++i) {
                const auto& set = *reinterpret_cast<OpenXrRuntime::ActionSet*>(sets[i]);
                effectiveUnchanged &= set.effectivePriority == effective[i] && set.priority == priorities[i];
                cachedUnchanged &= set.generation == generations[i] &&
                    !memcmp(cached[i].data(), &set.cachedInputState, sizeof(set.cachedInputState));
            }
            bool sourcePrioritiesUnchanged = true;
            for (const auto priority : runtime.m_actionSourcePriority) sourcePrioritiesUnchanged &= priority == 31;
            std::cout << item.label << " result=" << result << " activeCount=" << runtime.m_activeActionSets.size()
                      << " inputCalls=" << inputStateCalls << '\n';
            expect(result == item.expected, (std::string(item.label) + " returns the validation error").c_str());
            expect(runtime.m_activeActionSets == previousActive,
                   (std::string(item.label) + " preserves prior activation").c_str());
            expect(effectiveUnchanged && sourcePrioritiesUnchanged,
                   (std::string(item.label) + " preserves prior effective priorities").c_str());
            expect(cachedUnchanged && inputStateCalls == 0,
                   (std::string(item.label) + " preserves cached input and generations without sampling the backend").c_str());
        }
        XrActionsSyncInfo empty{XR_TYPE_ACTIONS_SYNC_INFO};
        inputStateCalls = 0;
        const auto result = runtime.xrSyncActions(session(), &empty);
        expect(result == XR_SUCCESS && runtime.m_activeActionSets.empty() && inputStateCalls == 1,
               "successful zero-active-set sync still clears activation and samples input");
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Sync validation fixture cleanup failed");
    }

    static void pinchVelocity(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        seed(runtime);
        PoseProvider provider(backendDirectory);
        const auto left = path(runtime, "/user/hand/left");
        const auto set = createSet(runtime);
        const auto action = createAction(runtime, set, XR_ACTION_TYPE_POSE_INPUT, left);
        runtime.m_attachedActionSets.insert(set);
        runtime.m_activeActionSets.insert(set);
        OpenXrRuntime::ActionSource source{};
        source.realPath = "/user/hand/left/input/aim/pose";
        reinterpret_cast<OpenXrRuntime::Action*>(action)->actionSources[source.realPath] = source;
        XrActionSpaceCreateInfo actionInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        actionInfo.action = action;
        actionInfo.subactionPath = left;
        actionInfo.poseInActionSpace.orientation.w = 1.f;
        XrSpace aim{};
        require(runtime.xrCreateActionSpace(session(), &actionInfo, &aim) == XR_SUCCESS, "Pinch velocity action-space fixture failed");
        XrReferenceSpaceCreateInfo baseInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        baseInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        baseInfo.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace stage{};
        require(runtime.xrCreateReferenceSpace(session(), &baseInfo, &stage) == XR_SUCCESS, "Pinch velocity base fixture failed");
        BodyTracking::BodyStateV2 body{};
        runtime.m_bodyState = &body;
        runtime.m_supportsHandTracking = true;
        runtime.m_cachedBodyState.LeftHandActive = true;
        runtime.m_cachedBodyState.LeftAimState.AimStatus = XR_HAND_TRACKING_AIM_VALID_BIT_FB;
        runtime.m_cachedBodyState.LeftAimState.AimPose.orientation.w = 1.f;
        runtime.m_cachedBodyState.LeftAimState.AimPose.position = {7.f, 8.f, 9.f};
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next = &velocity;
        XrResult result;
        try { result = runtime.xrLocateSpace(aim, stage, 1000000000, &location); }
        catch (...) { runtime.m_bodyState = nullptr; throw; }
        // The fixture body is stack-owned, not a mapped production view.
        runtime.m_bodyState = nullptr;
        std::cout << "pinch pose flags=" << location.locationFlags << " velocityFlags=" << velocity.velocityFlags << '\n';
        expect(result == XR_SUCCESS && (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
               closeVector(location.pose.position, {7.f, 8.f, 9.f}), "hand pinch pose replaces the controller pose");
        expect(velocity.velocityFlags == 0, "hand pinch pose does not inherit unrelated controller velocities");
        require(runtime.xrDestroySpace(aim) == XR_SUCCESS && runtime.xrDestroySpace(stage) == XR_SUCCESS,
                "Pinch velocity spaces cleanup failed");
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Pinch velocity session cleanup failed");
    }

    static void velocityInvalid(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        seed(runtime);
        PoseProvider provider(backendDirectory);
        invalidControllerAngularVelocity = false;
        runtime.m_supportsHandTracking = false;
        runtime.m_controllerGripPose[0] = xr::math::Pose::Identity();
        const auto left = path(runtime, "/user/hand/left");
        const auto set = createSet(runtime);
        const auto action = createAction(runtime, set, XR_ACTION_TYPE_POSE_INPUT, left);
        runtime.m_attachedActionSets.insert(set);
        runtime.m_activeActionSets.insert(set);
        OpenXrRuntime::ActionSource source{};
        source.realPath = "/user/hand/left/input/grip/pose";
        reinterpret_cast<OpenXrRuntime::Action*>(action)->actionSources[source.realPath] = source;
        XrReferenceSpaceCreateInfo stageInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        stageInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        stageInfo.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace stage{};
        require(runtime.xrCreateReferenceSpace(session(), &stageInfo, &stage) == XR_SUCCESS,
                "Invalid velocity stage fixture failed");
        const auto check = [&](const std::string& label, XrSpace space, XrSpace base, XrSpaceVelocityFlags expected) {
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            const auto result = runtime.xrLocateSpace(space, base, 1000000000, &location);
            const auto poseFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            std::cout << label << " velocityFlags=" << velocity.velocityFlags << " expected=" << expected << '\n';
            expect(result == XR_SUCCESS && (location.locationFlags & poseFlags) == poseFlags,
                   (label + " finite pose control").c_str());
            bool valid = velocity.velocityFlags == expected;
            if (expected & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
                valid &= closeVector(velocity.linearVelocity, {1.f, 2.f, 3.f});
            if (expected & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
                valid &= closeVector(velocity.angularVelocity, {4.f, 5.f, 6.f});
            expect(valid, (label + " component validity").c_str());
        };
        for (const bool controller : {false, true}) {
            const std::string device = controller ? "controller" : "HMD";
            velocityOverrideDevice = controller ? ovrTrackedDevice_LTouch : ovrTrackedDevice_HMD;
            XrSpace spaces[2]{};
            for (unsigned translated = 0; translated < 2; ++translated) {
                if (controller) {
                    XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                    info.action = action;
                    info.subactionPath = left;
                    info.poseInActionSpace.orientation.w = 1.f;
                    if (translated) info.poseInActionSpace.position = {1.f, 0.f, 0.f};
                    require(runtime.xrCreateActionSpace(session(), &info, &spaces[translated]) == XR_SUCCESS,
                            "Invalid velocity controller fixture failed");
                } else {
                    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                    info.poseInReferenceSpace.orientation.w = 1.f;
                    if (translated) info.poseInReferenceSpace.position = {1.f, 0.f, 0.f};
                    require(runtime.xrCreateReferenceSpace(session(), &info, &spaces[translated]) == XR_SUCCESS,
                            "Invalid velocity VIEW fixture failed");
                }
            }
            check(device + " finite velocity", spaces[0], stage,
                  XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
            struct Invalid { const char* name; float value; };
            const Invalid invalid[] = {{"NaN", std::numeric_limits<float>::quiet_NaN()},
                                       {"infinity", std::numeric_limits<float>::infinity()}};
            for (const auto& item : invalid) {
                velocityAngularOverride = item.value;
                const std::string angular = device + " " + item.name + " angular";
                check(angular + " zero offset", spaces[0], stage, XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
                check(angular + " translated origin", spaces[1], stage, 0);
                check(angular + " rotating base", stage, spaces[0], 0);
                velocityAngularOverride.reset();
                velocityLinearOverride = item.value;
                check(device + " " + item.name + " linear zero offset", spaces[0], stage, XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
                velocityLinearOverride.reset();
            }
            require(runtime.xrDestroySpace(spaces[0]) == XR_SUCCESS && runtime.xrDestroySpace(spaces[1]) == XR_SUCCESS,
                    "Invalid velocity source spaces cleanup failed");
        }
        require(runtime.xrDestroySpace(stage) == XR_SUCCESS, "Invalid velocity stage cleanup failed");
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Invalid velocity session cleanup failed");
    }

    static void velocityOffsets(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        seed(runtime);
        PoseProvider provider(backendDirectory);
        const auto reference = [&](XrReferenceSpaceType type, const XrVector3f& offset) {
            XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            info.referenceSpaceType = type;
            info.poseInReferenceSpace.orientation.w = 1.f;
            info.poseInReferenceSpace.position = offset;
            XrSpace space{};
            require(runtime.xrCreateReferenceSpace(session(), &info, &space) == XR_SUCCESS,
                    "Velocity-offset reference-space fixture failed");
            return space;
        };
        const auto check = [&](const char* label, XrSpace space, XrSpace base, const XrVector3f& position,
                               const XrVector3f& linear, const XrVector3f& angular) {
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            const auto result = runtime.xrLocateSpace(space, base, 1000000000, &location);
            std::cout << label << " actual linear=" << velocity.linearVelocity.x << ',' << velocity.linearVelocity.y
                      << ',' << velocity.linearVelocity.z << " expected=" << linear.x << ',' << linear.y << ',' << linear.z << '\n';
            const auto poseFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            expect(result == XR_SUCCESS && (location.locationFlags & poseFlags) == poseFlags &&
                   closeVector(location.pose.position, position), (std::string(label) + " pose control").c_str());
            expect((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) && closeVector(velocity.linearVelocity, linear),
                   (std::string(label) + " linear velocity").c_str());
            expect((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) && closeVector(velocity.angularVelocity, angular),
                   (std::string(label) + " angular velocity control").c_str());
        };
        struct Case {
            const char* label; XrReferenceSpaceType spaceType; XrVector3f spaceOffset;
            XrReferenceSpaceType baseType; XrVector3f baseOffset;
            XrVector3f position; XrVector3f linear; XrVector3f angular;
        };
        // Hand-derived from p=(1,2,3), v=(1,2,3), omega=(4,5,6), and identity tracked orientation.
        const Case cases[] = {
            {"zero VIEW offset in stationary STAGE", XR_REFERENCE_SPACE_TYPE_VIEW, {}, XR_REFERENCE_SPACE_TYPE_STAGE, {},
             {1.f, 2.f, 3.f}, {1.f, 2.f, 3.f}, {4.f, 5.f, 6.f}},
            {"VIEW in translated static STAGE base", XR_REFERENCE_SPACE_TYPE_VIEW, {}, XR_REFERENCE_SPACE_TYPE_STAGE, {2.f, -1.f, 4.f},
             {-1.f, 3.f, -1.f}, {1.f, 2.f, 3.f}, {4.f, 5.f, 6.f}},
            // omega x (1,0,0)=(0,6,-5), so the offset point's world velocity is (1,8,-2).
            {"translated VIEW offset in stationary STAGE", XR_REFERENCE_SPACE_TYPE_VIEW, {1.f, 0.f, 0.f}, XR_REFERENCE_SPACE_TYPE_STAGE, {},
             {2.f, 2.f, 3.f}, {1.f, 8.f, -2.f}, {4.f, 5.f, 6.f}},
            // -v - omega x (-p) = -(1,2,3)+(3,-6,3) = (2,-8,0).
            {"STAGE origin in rotating VIEW base", XR_REFERENCE_SPACE_TYPE_STAGE, {}, XR_REFERENCE_SPACE_TYPE_VIEW, {},
             {-1.f, -2.f, -3.f}, {2.f, -8.f, 0.f}, {-4.f, -5.f, -6.f}},
        };
        for (const auto& item : cases) {
            const auto space = reference(item.spaceType, item.spaceOffset);
            const auto base = reference(item.baseType, item.baseOffset);
            check(item.label, space, base, item.position, item.linear, item.angular);
            require(runtime.xrDestroySpace(space) == XR_SUCCESS && runtime.xrDestroySpace(base) == XR_SUCCESS,
                    "Velocity-offset reference-space cleanup failed");
        }
        const auto left = path(runtime, "/user/hand/left");
        const auto set = createSet(runtime);
        const auto action = createAction(runtime, set, XR_ACTION_TYPE_POSE_INPUT, left);
        runtime.m_attachedActionSets.insert(set);
        runtime.m_activeActionSets.insert(set);
        runtime.m_controllerGripPose[0] = runtime.m_controllerAimPose[0] = xr::math::Pose::Translation({1.f, 0.f, 0.f});
        runtime.m_supportsHandTracking = false;
        const auto stage = reference(XR_REFERENCE_SPACE_TYPE_STAGE, {});
        for (const bool aim : {false, true}) {
            auto& internal = *reinterpret_cast<OpenXrRuntime::Action*>(action);
            internal.actionSources.clear();
            OpenXrRuntime::ActionSource source{};
            source.realPath = aim ? "/user/hand/left/input/aim/pose" : "/user/hand/left/input/grip/pose";
            internal.actionSources[source.realPath] = source;
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = left;
            info.poseInActionSpace.orientation.w = 1.f;
            info.poseInActionSpace.position = {0.f, 1.f, 0.f};
            XrSpace space{};
            require(runtime.xrCreateActionSpace(session(), &info, &space) == XR_SUCCESS,
                    "Velocity-offset controller space fixture failed");
            // Total world offset (1,1,0); omega x offset=(-6,6,-1), giving v=(-5,8,2).
            check(aim ? "controller aim and action-space translated offsets" : "controller grip and action-space translated offsets",
                  space, stage, {2.f, 3.f, 3.f}, {-5.f, 8.f, 2.f}, {4.f, 5.f, 6.f});
            require(runtime.xrDestroySpace(space) == XR_SUCCESS, "Velocity-offset controller space cleanup failed");
        }
        auto& internal = *reinterpret_cast<OpenXrRuntime::Action*>(action);
        internal.actionSources.clear();
        OpenXrRuntime::ActionSource source{};
        source.realPath = "/user/hand/left/input/grip/pose";
        internal.actionSources[source.realPath] = source;
        invalidControllerAngularVelocity = true;
        const auto checkPartialVelocity = [&](XrSpace space, XrSpace base, bool linearValid, const char* label) {
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            const auto result = runtime.xrLocateSpace(space, base, 1000000000, &location);
            const XrSpaceVelocityFlags expected = linearValid ? XR_SPACE_VELOCITY_LINEAR_VALID_BIT : 0;
            std::cout << label << " velocityFlags=" << velocity.velocityFlags << " expected=" << expected << '\n';
            expect(result == XR_SUCCESS && (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                   velocity.velocityFlags == expected && (!linearValid || closeVector(velocity.linearVelocity, {1.f, 2.f, 3.f})), label);
        };
        for (const bool translated : {false, true}) {
            runtime.m_controllerGripPose[0] = translated ? xr::math::Pose::Translation({1.f, 0.f, 0.f}) : xr::math::Pose::Identity();
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = left;
            info.poseInActionSpace.orientation.w = 1.f;
            if (translated) info.poseInActionSpace.position = {0.f, 1.f, 0.f};
            XrSpace space{};
            require(runtime.xrCreateActionSpace(session(), &info, &space) == XR_SUCCESS,
                    "Partial velocity controller fixture failed");
            checkPartialVelocity(space, stage, !translated,
                translated ? "translated controller origin requires valid angular velocity" : "zero-offset controller retains independent linear validity");
            if (!translated)
                checkPartialVelocity(stage, space, false, "displaced STAGE origin relative to controller base requires base angular velocity");
            require(runtime.xrDestroySpace(space) == XR_SUCCESS, "Partial velocity controller cleanup failed");
        }
        invalidControllerAngularVelocity = false;
        require(runtime.xrDestroySpace(stage) == XR_SUCCESS, "Velocity-offset stage cleanup failed");
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Velocity-offset session cleanup failed");
    }

    static void velocities(const wchar_t* backendDirectory, bool controlsOnly) {
        OpenXrRuntime runtime;
        seed(runtime);
        PoseProvider provider(backendDirectory);
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        info.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace view{};
        require(runtime.xrCreateReferenceSpace(session(), &info, &view) == XR_SUCCESS, "View fixture failed");
        for (unsigned rotated = 0; rotated < (controlsOnly ? 1u : 2u); ++rotated) {
            info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            info.poseInReferenceSpace = {};
            info.poseInReferenceSpace.orientation.w = 1.f;
            // Static base, +90 degrees around Y, no offsets: only the inverse basis rotation is required.
            if (rotated) info.poseInReferenceSpace.orientation = {0.f, 0.70710678f, 0.f, 0.70710678f};
            XrSpace base{};
            require(runtime.xrCreateReferenceSpace(session(), &info, &base) == XR_SUCCESS, "Base fixture failed");
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            const auto result = runtime.xrLocateSpace(view, base, 1000000000, &location);
            const XrVector3f expectedLinear = rotated ? XrVector3f{-3.f, 2.f, 1.f} : XrVector3f{1.f, 2.f, 3.f};
            const XrVector3f expectedAngular = rotated ? XrVector3f{-6.f, 5.f, 4.f} : XrVector3f{4.f, 5.f, 6.f};
            const auto requiredFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            std::cout << "base=" << (rotated ? "yaw90" : "identity") << " linear=" << velocity.linearVelocity.x << ','
                      << velocity.linearVelocity.y << ',' << velocity.linearVelocity.z << " angular="
                      << velocity.angularVelocity.x << ',' << velocity.angularVelocity.y << ','
                      << velocity.angularVelocity.z << '\n';
            expect(result == XR_SUCCESS && (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                   closeVector(location.pose.position, expectedLinear), "located pose uses the base-space orientation");
            expect((velocity.velocityFlags & requiredFlags) == requiredFlags &&
                   closeVector(velocity.linearVelocity, expectedLinear),
                   rotated ? "linear velocity uses inverse yaw base orientation" : "identity base preserves linear velocity");
            expect((velocity.velocityFlags & requiredFlags) == requiredFlags &&
                   closeVector(velocity.angularVelocity, expectedAngular),
                   rotated ? "angular velocity uses inverse yaw base orientation" : "identity base preserves angular velocity");
            require(runtime.xrDestroySpace(base) == XR_SUCCESS, "Base-space fixture cleanup failed");
        }
        require(runtime.xrDestroySpace(view) == XR_SUCCESS, "View-space fixture cleanup failed");
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Velocity session cleanup failed");
    }

    static int run(int argc, wchar_t** argv) {
        const std::wstring mode = argc > 1 ? argv[1] : L"cpu";
        if (mode == L"cpu" || mode == L"controls") {
            floats(mode == L"controls");
            actionPaths(mode == L"controls");
            events(mode == L"controls");
        } else if (mode == L"float") floats(false);
        else if (mode == L"action-paths") actionPaths(false);
        else if (mode == L"action-pose") actionPose();
        else if (mode == L"query-cache") queryCache();
        else if (mode == L"action-set-lifetime") {
            require(argc == 3, "action-set-lifetime requires the explicit OVRNull directory");
            actionSetLifetime(argv[2]);
        }
        else if (mode == L"events") events(false);
        else if (mode == L"poll-lock") pollLock();
        else if (mode == L"sync-validation" || mode == L"pinch-velocity") {
            require(argc == 3, "sync-validation and pinch-velocity require the explicit OVRNull directory");
            if (mode == L"sync-validation") syncValidation(argv[2]);
            else pinchVelocity(argv[2]);
        }
        else if (mode == L"velocity-offsets") {
            require(argc == 3, "velocity-offsets requires the explicit OVRNull directory");
            velocityOffsets(argv[2]);
        }
        else if (mode == L"velocity-invalid") {
            require(argc == 3, "velocity-invalid requires the explicit OVRNull directory");
            velocityInvalid(argv[2]);
        }
        else if (mode == L"velocity" || mode == L"velocity-controls") {
            require(argc == 3, "velocity mode requires the explicit OVRNull directory");
            velocities(argv[2], mode == L"velocity-controls");
        } else throw std::runtime_error("Unknown mode: cpu, controls, float, action-paths, action-pose, query-cache, action-set-lifetime, events, poll-lock, sync-validation, pinch-velocity, velocity-offsets, velocity-invalid, velocity, velocity-controls");
        std::cout << "failures=" << failures << '\n';
        return failures ? 1 : 0;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try { return virtualdesktop_openxr::RuntimeInputRegression::run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
