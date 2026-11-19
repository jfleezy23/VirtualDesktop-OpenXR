// CPU regressions against real tracking caches and public locate methods.
// Only the process-private external pose/origin/config provider is replaced.
#include "pch.h"
#include "runtime.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }

    bool close(float actual, float expected) {
        return std::isfinite(actual) && std::abs(actual - expected) < .0001f;
    }

    bool positionIs(const XrPosef& pose, float x, float y, float z) {
        return close(pose.position.x, x) && close(pose.position.y, y) && close(pose.position.z, z);
    }

    ovrPoseStatef sample(float x = 1.f, float y = 2.f, float z = 3.f) {
        ovrPoseStatef state{};
        state.ThePose.Orientation.w = 1.f;
        state.ThePose.Position = {x, y, z};
        state.LinearVelocity = {4.f, 5.f, 6.f};
        state.AngularVelocity = {7.f, 8.f, 9.f};
        return state;
    }

    struct Provider {
        inline static Provider* active{};
        decltype(&ovr_GetDevicePoses) poses{};
        decltype(&ovr_GetTrackingOriginType) origin{};
        decltype(&ovr_GetFloat) configFloat{};
        std::mutex mutex;
        std::condition_variable changed;
        ovrPoseStatef state = sample();
        ovrResult result = ovrSuccess;
        bool writeOutput = true;
        bool pairGate{}, gateTimedOut{}, timePosition{};
        unsigned gateEntries{};

        static ovrResult OVR_CDECL
        readPoses(ovrSession, ovrTrackedDeviceType* devices, int count, double time, ovrPoseStatef* output) {
            auto& self = *active;
            std::unique_lock lock(self.mutex);
            if (count != 1 || (devices[0] != ovrTrackedDevice_HMD && devices[0] != ovrTrackedDevice_LTouch &&
                               devices[0] != ovrTrackedDevice_RTouch))
                return ovrError_DeviceUnavailable;
            if (self.pairGate) {
                ++self.gateEntries;
                self.changed.notify_all();
                if (!self.changed.wait_for(lock, 2s, [&] { return self.gateEntries >= 2; }))
                    self.gateTimedOut = true;
            }
            if (self.writeOutput) {
                *output = self.state;
                output->TimeInSeconds = time;
                if (self.timePosition)
                    output->ThePose.Position.x = static_cast<float>(time);
            }
            return self.result;
        }

        static ovrTrackingOrigin OVR_CDECL readOrigin(ovrSession) {
            return ovrTrackingOrigin_FloorLevel;
        }

        static float OVR_CDECL readFloat(ovrSession, const char* name, float fallback) {
            return !strcmp(name, OVR_KEY_EYE_HEIGHT) ? 0.f : fallback;
        }

        explicit Provider(const wchar_t* directory) {
            require(!active, "One process-private pose provider is allowed");
            ovrInitParams init{};
            init.Flags = ovrInit_RequestVersion;
            init.RequestedMinorVersion = OVR_MINOR_VERSION;
            const auto path = std::filesystem::absolute(directory).wstring() + L"\\";
            require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, path.c_str())), "Initialize explicit OVRNull");
            const auto module = GetModuleHandleW(L"LibOVRRT64_1.dll");
            require(module != nullptr, "Explicit OVRNull module is loaded");
            poses = reinterpret_cast<decltype(poses)>(GetProcAddress(module, "ovr_GetDevicePoses"));
            origin = reinterpret_cast<decltype(origin)>(GetProcAddress(module, "ovr_GetTrackingOriginType"));
            configFloat = reinterpret_cast<decltype(configFloat)>(GetProcAddress(module, "ovr_GetFloat"));
            require(poses && origin && configFloat, "OVRNull pose/origin/config exports exist");
            active = this;
            require(DetourTransactionBegin() == NO_ERROR, "Begin pose provider hook transaction");
            if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourAttach(reinterpret_cast<PVOID*>(&poses), readPoses) != NO_ERROR ||
                DetourAttach(reinterpret_cast<PVOID*>(&origin), readOrigin) != NO_ERROR ||
                DetourAttach(reinterpret_cast<PVOID*>(&configFloat), readFloat) != NO_ERROR) {
                DetourTransactionAbort();
                active = nullptr;
                throw std::runtime_error("Attach pose provider hooks");
            }
            require(DetourTransactionCommit() == NO_ERROR, "Commit pose provider hook transaction");
        }

        ~Provider() {
            const auto begun = DetourTransactionBegin();
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto detachedPoses = DetourDetach(reinterpret_cast<PVOID*>(&poses), readPoses);
            const auto detachedOrigin = DetourDetach(reinterpret_cast<PVOID*>(&origin), readOrigin);
            const auto detachedFloat = DetourDetach(reinterpret_cast<PVOID*>(&configFloat), readFloat);
            const auto committed = DetourTransactionCommit();
            if (begun || updated || detachedPoses || detachedOrigin || detachedFloat || committed)
                std::terminate();
            active = nullptr;
        }

        void publish(ovrResult status, const ovrPoseStatef& value, bool writes = true) {
            std::scoped_lock lock(mutex);
            result = status;
            state = value;
            writeOutput = writes;
        }
    };

    constexpr XrSpaceLocationFlags orientation = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    constexpr XrSpaceLocationFlags position = XR_SPACE_LOCATION_POSITION_VALID_BIT;
    constexpr XrSpaceLocationFlags orientationTracked = XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    constexpr XrSpaceLocationFlags positionTracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    constexpr XrSpaceLocationFlags valid = orientation | position;
    constexpr XrSpaceLocationFlags tracked = valid | orientationTracked | positionTracked;
} // namespace

namespace virtualdesktop_openxr {
    // This existing friend keeps test setup and private getter checks out of production.
    struct RuntimeInputRegression {
        struct Fixture {
            OpenXrRuntime runtime;
            OpenXrRuntime::Space view{}, stage{}, local{};

            Fixture() {
                runtime.stopRegistryWatcher();
                runtime.m_instanceCreated = runtime.m_sessionCreated = true;
                view.referenceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                stage.referenceType = XR_REFERENCE_SPACE_TYPE_STAGE;
                local.referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                view.poseInSpace = stage.poseInSpace = local.poseInSpace = xr::math::Pose::Identity();
                runtime.m_viewSpace = &view;
                runtime.m_originSpace = &stage;
                runtime.m_spaces.insert(reinterpret_cast<XrSpace>(&view));
                runtime.m_spaces.insert(reinterpret_cast<XrSpace>(&stage));
                runtime.m_spaces.insert(reinterpret_cast<XrSpace>(&local));
                runtime.m_jiggleViewRotations = false;
                runtime.m_overrideWorldScale = 1.f;
                for (unsigned eye = 0; eye < 2; ++eye) {
                    runtime.m_cachedEyeInfo[eye].HmdToEyePose.Orientation.w = 1.f;
                    runtime.m_cachedEyeInfo[eye].HmdToEyePose.Position.x = eye ? .032f : -.032f;
                    runtime.m_cachedEyeFov[eye] = {-.8f, .8f, .8f, -.8f};
                }
            }

            XrSpaceLocationFlags get(int device, XrTime time, XrPosef& pose, XrSpaceVelocity* velocity = nullptr) {
                return device < 0 ? runtime.getHmdPose(time, pose, velocity)
                                  : runtime.getControllerPose(device, time, pose, velocity);
            }

            XrSpaceLocation locate(bool floor = false) {
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                require(runtime.xrLocateSpace(reinterpret_cast<XrSpace>(&view),
                                              reinterpret_cast<XrSpace>(floor ? &local : &stage),
                                              10'000'000'000,
                                              &location) == XR_SUCCESS,
                        "Public space locate succeeds");
                return location;
            }

            std::array<XrView, 2> locateViews(XrTime time, XrViewState& state) {
                std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
                XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
                info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                info.displayTime = time;
                info.space = reinterpret_cast<XrSpace>(&stage);
                uint32_t count{};
                require(runtime.xrLocateViews(reinterpret_cast<XrSession>(1), &info, &state, 2, &count, views.data()) ==
                                XR_SUCCESS &&
                            count == 2,
                        "Public view locate succeeds");
                return views;
            }
        };

        // An identity fallback from a failed query must never become observed tracking history.
        static void cold(const wchar_t* directory) {
            const ovrResult statuses[] = {ovrError_LostTracking, ovrSuccess_DeviceUnavailable};
            for (const auto status : statuses) {
                for (const bool writes : {false, true}) {
                    for (int device = -1; device < 2; ++device) {
                        Fixture fixture;
                        Provider provider(directory);
                        provider.publish(status, sample(0.f, 0.f, 0.f), writes);
                        for (unsigned attempt = 0; attempt < 2; ++attempt) {
                            XrPosef pose{};
                            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
                            const auto flags = fixture.get(device, 10'000'000'000, pose, &velocity);
                            std::cout << "cold device=" << device << " status=" << status << " writes=" << writes
                                      << " attempt=" << attempt << " flags=" << flags << '\n';
                            require(flags == 0 && velocity.velocityFlags == 0,
                                    "Missing cold pose remains invalid on every query");
                        }
                        if (device < 0)
                            require(fixture.locate().locationFlags == 0, "Public locate cannot promote missing pose");
                    }
                }
            }
        }

        // Partial samples retain only components that were actually valid; infinities are not valid samples.
        static void partial(const wchar_t* directory) {
            const auto nan = std::numeric_limits<float>::quiet_NaN();
            const auto infinity = std::numeric_limits<float>::infinity();
            for (int device = -1; device < 2; ++device) {
                Fixture fixture;
                Provider provider(directory);
                auto state = sample();
                state.ThePose.Position.x = nan;
                provider.publish(ovrSuccess, state);
                XrPosef pose{};
                auto flags = fixture.get(device, 10'000'000'000, pose);
                require(flags == (orientation | orientationTracked), "Cold bad position does not gain validity");
                require(close(pose.orientation.w, 1.f), "Good orientation survives missing position");

                state = sample(7.f, 8.f, 9.f);
                state.ThePose.Orientation.x = infinity;
                provider.publish(ovrSuccess, state);
                flags = fixture.get(device, 11'000'000'000, pose);
                require(flags == (valid | positionTracked) && positionIs(pose, 7.f, 8.f, 9.f) &&
                            close(pose.orientation.x, 0.f) && close(pose.orientation.w, 1.f),
                        "Good position combines with observed orientation fallback");

                provider.publish(ovrError_LostTracking, sample(0.f, 0.f, 0.f));
                flags = fixture.get(device, 12'000'000'000, pose);
                require(flags == valid && positionIs(pose, 7.f, 8.f, 9.f) && close(pose.orientation.w, 1.f),
                        "Lost tracking preserves valid components without tracked bits");

                state = sample();
                state.ThePose.Position.y = infinity;
                state.ThePose.Orientation = {0.f, .70710678f, 0.f, .70710678f};
                state.AngularVelocity.x = infinity;
                state.LinearVelocity.x = nan;
                provider.publish(ovrSuccess, state);
                XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
                flags = fixture.get(device, 13'000'000'000, pose, &velocity);
                require(flags == (valid | orientationTracked) && positionIs(pose, 7.f, 8.f, 9.f) &&
                            close(pose.orientation.y, .70710678f) && velocity.velocityFlags == 0,
                        "Nonfinite position and velocities preserve independent orientation tracking");

                provider.publish(ovrSuccess, sample(4.f, 5.f, 6.f));
                flags = fixture.get(device, 14'000'000'000, pose, &velocity);
                require(flags == tracked && positionIs(pose, 4.f, 5.f, 6.f) &&
                            velocity.velocityFlags ==
                                (XR_SPACE_VELOCITY_ANGULAR_VALID_BIT | XR_SPACE_VELOCITY_LINEAR_VALID_BIT),
                        "A complete finite sample restores tracking and velocity validity");
            }
        }

        // The Unity same-time compatibility path must never return another query timestamp's pose.
        static void unity(const wchar_t* directory) {
            Fixture fixture;
            Provider provider(directory);
            fixture.runtime.m_isUnity = fixture.runtime.m_isOculusXrPlugin = true;
            XrPosef generic{};
            provider.publish(ovrSuccess, sample(-7.f));
            require(fixture.get(-1, 10'000'000'000, generic) == tracked && positionIs(generic, -7.f, 2.f, 3.f),
                    "Generic HMD control observes its first sample");
            provider.publish(ovrSuccess, sample(-8.f));
            require(fixture.get(-1, 10'000'000'000, generic) == tracked && positionIs(generic, -8.f, 2.f, 3.f),
                    "Generic same-time HMD queries refine before any public view request");
            XrViewState state{XR_TYPE_VIEW_STATE};
            provider.publish(ovrSuccess, sample(1.f));
            const auto first = fixture.locateViews(10'000'000'000, state);
            provider.publish(ovrSuccess, sample(2.f));
            const auto repeated = fixture.locateViews(10'000'000'000, state);
            require((state.viewStateFlags & valid) == valid &&
                        close(repeated[0].pose.position.x, first[0].pose.position.x),
                    "Consecutive same-time Unity views retain their coherent snapshot");
            provider.publish(ovrSuccess, sample(9.f));
            XrPosef other{};
            require(fixture.get(-1, 20'000'000'000, other) == tracked && positionIs(other, 9.f, 2.f, 3.f),
                    "Mixed-time control observes a distinct HMD pose");
            provider.publish(ovrSuccess, sample(3.f));
            const auto resumed = fixture.locateViews(10'000'000'000, state);
            const float center = (resumed[0].pose.position.x + resumed[1].pose.position.x) * .5f;
            require((state.viewStateFlags & valid) == valid && (close(center, 1.f) || close(center, 3.f)),
                    "Unity views never replay the intervening different-time pose");

            auto partial = sample(4.f);
            partial.ThePose.Orientation.x = std::numeric_limits<float>::quiet_NaN();
            provider.publish(ovrSuccess, partial);
            require(fixture.get(-1, 10'000'000'000, generic) == (valid | positionTracked) &&
                        positionIs(generic, 4.f, 2.f, 3.f) && close(generic.orientation.w, 1.f),
                    "Partial HMD update retains only observed component tracking");
            provider.publish(ovrSuccess, sample(5.f));
            require(fixture.get(-1, 10'000'000'000, generic) == tracked && positionIs(generic, 5.f, 2.f, 3.f),
                    "A partial fallback composite cannot become a frozen complete timestamp");
        }

        // A cache lock around SDK access would prevent the two legal public queries entering together.
        static void concurrent(const wchar_t* directory) {
            Fixture fixture;
            Provider provider(directory);
            provider.pairGate = provider.timePosition = true;
            std::array<std::exception_ptr, 2> errors{};
            XrViewState state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> views{};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            std::thread first([&] {
                try {
                    views = fixture.locateViews(10'000'000'000, state);
                } catch (...) {
                    errors[0] = std::current_exception();
                }
            });
            std::thread second([&] {
                try {
                    require(fixture.runtime.xrLocateSpace(reinterpret_cast<XrSpace>(&fixture.view),
                                                          reinterpret_cast<XrSpace>(&fixture.stage),
                                                          20'000'000'000,
                                                          &location) == XR_SUCCESS,
                            "Concurrent public space locate succeeds");
                } catch (...) {
                    errors[1] = std::current_exception();
                }
            });
            first.join();
            second.join();
            for (const auto& error : errors)
                if (error)
                    std::rethrow_exception(error);
            require(!provider.gateTimedOut && provider.gateEntries == 2 && (state.viewStateFlags & valid) == valid &&
                        close((views[0].pose.position.x + views[1].pose.position.x) * .5f, 10.f) &&
                        (location.locationFlags & valid) == valid && positionIs(location.pose, 20.f, 2.f, 3.f),
                    "Concurrent locates enter the SDK independently and return their own coherent poses");
        }

        // LOCAL height inference recursively requests HMD tracking and must not retain a leaf cache lock.
        static void floor(const wchar_t* directory) {
            Fixture fixture;
            Provider provider(directory);
            provider.publish(ovrSuccess, sample(1.f, 1.6f, 3.f));
            const auto location = fixture.locate(true);
            require((location.locationFlags & valid) == valid && positionIs(location.pose, 1.f, 0.f, 3.f),
                    "Recursive LOCAL floor inference completes with the observed eye height");
        }

        static int run(const std::wstring& mode, const wchar_t* directory) {
            if (mode == L"cold")
                cold(directory);
            else if (mode == L"partial")
                partial(directory);
            else if (mode == L"unity")
                unity(directory);
            else if (mode == L"concurrent")
                concurrent(directory);
            else if (mode == L"floor")
                floor(directory);
            else
                throw std::runtime_error("Unknown mode: cold, partial, unity, concurrent, floor");
            std::wcout << L"PASS: tracking cache " << mode << L'\n';
            return 0;
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    // A broken lock scope must fail the process instead of stranding a join or future destructor.
    const HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!finished)
        return 2;
    std::thread watchdog([&] {
        if (WaitForSingleObject(finished, 15000) != WAIT_OBJECT_0) {
            std::fputs("FAIL: tracking cache deadlock watchdog\n", stderr);
            std::_Exit(3);
        }
    });
    int result = 1;
    try {
        require(argc == 3, "Usage: tracking_cache_regression cold|partial|unity|concurrent|floor <OVRNull-directory>");
        result = virtualdesktop_openxr::RuntimeInputRegression::run(argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
    return result;
}
