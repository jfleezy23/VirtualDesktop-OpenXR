// Process-private accessibility provider regressions; no headset or application settings changes.
#include "pch.h"
#include "accessibility.h"

namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }

    struct InputGate {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned entries = 0;
        unsigned buttons = 0;
        bool pending = false;
        bool stopping = false;

        void step(unsigned value) {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 5s, [&] { return entries != 0; }), "Input worker did not start");
            const auto previous = entries;
            buttons = value;
            pending = true;
            changed.notify_all();
            require(changed.wait_for(lock, 5s, [&] { return entries > previous; }),
                    "Input worker did not publish the requested state");
        }

        void release() {
            std::unique_lock lock(mutex);
            stopping = true;
            changed.notify_all();
        }
    };

    InputGate* inputGate = nullptr;
    ovrResult headResult = ovrSuccess;
    bool writeFailedPose = false;
    ovrVector3f headPosition = {10.f, 20.f, 30.f};
    unsigned headCalls = 0;
    decltype(&ovr_GetInputState) originalInput = ovr_GetInputState;
    decltype(&ovr_GetDevicePoses) originalPoses = ovr_GetDevicePoses;

    ovrResult OVR_CDECL readInput(ovrSession, ovrControllerType, ovrInputState* state) {
        std::unique_lock lock(inputGate->mutex);
        ++inputGate->entries;
        inputGate->changed.notify_all();
        inputGate->changed.wait(lock, [] { return inputGate->pending || inputGate->stopping; });
        *state = {};
        state->Buttons = inputGate->buttons;
        state->IndexTrigger[0] = 0.25f;
        state->IndexTrigger[1] = 0.75f;
        inputGate->pending = false;
        return ovrSuccess;
    }

    ovrResult OVR_CDECL
    readPoses(ovrSession, ovrTrackedDeviceType* devices, int count, double time, ovrPoseStatef* states) {
        require(count == 1 && devices[0] == ovrTrackedDevice_HMD, "Unexpected tracked device request");
        ++headCalls;
        if (headResult == ovrSuccess || writeFailedPose) {
            states[0] = {};
            states[0].ThePose.Orientation.w = 1.f;
            states[0].TimeInSeconds = time;
            if (headResult == ovrSuccess)
                states[0].ThePose.Position = headPosition;
        }
        return headResult;
    }

    class Providers {
      public:
        explicit Providers(InputGate& gate) {
            inputGate = &gate;
            require(DetourTransactionBegin() == NO_ERROR, "Provider hook transaction failed");
            if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalInput), readInput) != NO_ERROR ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalPoses), readPoses) != NO_ERROR) {
                DetourTransactionAbort();
                throw std::runtime_error("Provider hook attachment failed");
            }
            require(DetourTransactionCommit() == NO_ERROR, "Provider hook commit failed");
        }

        ~Providers() {
            const auto begun = DetourTransactionBegin();
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto poses = DetourDetach(reinterpret_cast<PVOID*>(&originalPoses), readPoses);
            const auto input = DetourDetach(reinterpret_cast<PVOID*>(&originalInput), readInput);
            const auto committed = DetourTransactionCommit();
            if (begun || updated || poses || input || committed)
                std::terminate();
            inputGate = nullptr;
        }
    };

    struct HelperOwner {
        InputGate& gate;
        std::unique_ptr<virtualdesktop_openxr::AccessibilityHelper> helper;
        ~HelperOwner() {
            gate.release();
            helper.reset();
        }
    };

    bool equal(const ovrPosef& a, const ovrPosef& b) {
        return a.Position.x == b.Position.x && a.Position.y == b.Position.y && a.Position.z == b.Position.z &&
               a.Orientation.x == b.Orientation.x && a.Orientation.y == b.Orientation.y &&
               a.Orientation.z == b.Orientation.z && a.Orientation.w == b.Orientation.w;
    }

    void run(const std::string& mode, const std::filesystem::path& directory) {
        const auto config = directory / ("accessibility-" + std::to_string(GetCurrentProcessId()) + ".json");
        const auto dominant = mode == "negative"  ? -1
                              : mode == "minimum" ? INT_MIN
                              : mode == "right"   ? 1
                              : mode == "large"   ? INT_MAX
                                                  : 0;
        {
            std::ofstream output(config);
            require(output.is_open(), "Could not create private accessibility configuration");
            output << "{\"default\":{\"emulate_left\":true,\"emulate_right\":true,"
                      "\"debug_use_touch_controller_buttons\":true,\"dominant_hand\":"
                   << dominant
                   << ",\"left_pose_relative_to_head\":{\"x\":-0.15,\"y\":-0.1,\"z\":-0.35},"
                      "\"right_pose_relative_to_head\":{\"x\":0.15,\"y\":-0.1,\"z\":-0.35}}}";
            require(bool(output), "Could not write private accessibility configuration");
        }
        const auto cleanup = wil::scope_exit([&] { std::filesystem::remove(config); });
        InputGate gate;
        Providers providers(gate);
        HelperOwner owner{
            gate, virtualdesktop_openxr::CreateAccessibilityHelper(nullptr, config.wstring(), "fixture", "fixture")};
        require(bool(owner.helper), "Accessibility factory rejected the private configuration");
        gate.step(0);
        gate.step(ovrButton_X | ovrButton_Y | ovrButton_A | ovrButton_B);
        ovrInputState input{};
        require(owner.helper->GetEmulatedInputState(0, &input), "Controller did not enter gaze-follow mode");
        const auto expectedTrigger = dominant < 1 ? 0.25f : 0.75f;
        require(input.IndexTrigger[0] == expectedTrigger, "Dominant-hand input mapping did not clamp to a valid hand");

        if (mode != "lost" && mode != "unavailable")
            return;
        ovrPoseStatef good{};
        require(owner.helper->GetEmulatedDevicePose(0, 1., &good), "Successful head pose was rejected");
        require(good.ThePose.Position.x > 9.f && good.ThePose.Position.y > 19.f && good.ThePose.Position.z > 29.f,
                "Successful controller pose did not follow the translated head");
        headResult = mode == "lost" ? ovrError_LostTracking : ovrSuccess_DeviceUnavailable;
        for (bool writes : {false, true}) {
            writeFailedPose = writes;
            ovrPoseStatef invalid{};
            require(!owner.helper->GetEmulatedDevicePose(0, 2., &invalid),
                    "Unavailable head pose was reported as a tracked controller");
        }
        gate.step(ovrButton_Y);
        require(!owner.helper->GetEmulatedInputState(0, &input), "Left controller did not stop following gaze");
        const auto before = headCalls;
        ovrPoseStatef cached{};
        require(owner.helper->GetEmulatedDevicePose(0, 3., &cached), "Last valid stationary controller pose was lost");
        require(headCalls == before, "Stationary controller unexpectedly queried the unavailable head");
        require(equal(cached.ThePose, good.ThePose), "Unavailable head pose overwrote the last valid controller pose");
        gate.step(ovrButton_X);
        headResult = ovrSuccess;
        headPosition.x += 1.f;
        const auto beforeRecovery = headCalls;
        require(owner.helper->GetEmulatedDevicePose(0, 4., &cached), "Head tracking did not recover");
        require(headCalls == beforeRecovery + 1, "Recovery did not request a fresh head pose");
        auto recovered = good.ThePose;
        recovered.Position.x += 1.f;
        require(equal(cached.ThePose, recovered), "Recovered controller did not follow the new head position");
    }
} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        require(argc == 3,
                "Usage: accessibility_pose_regression left|right|negative|minimum|large|lost|unavailable "
                "<private-directory>");
        const std::string mode = argv[1];
        require(mode == "left" || mode == "right" || mode == "negative" || mode == "minimum" || mode == "large" ||
                    mode == "lost" || mode == "unavailable",
                "Unknown accessibility case");
        run(mode, argv[2]);
        std::cout << "PASS: accessibility pose/input " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
