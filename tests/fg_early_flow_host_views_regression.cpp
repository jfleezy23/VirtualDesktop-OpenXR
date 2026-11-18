// Pure production view construction. No runtime instance, native SDK or GPU.
#include "pch.h"
#include "runtime.h"

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        using Space = OpenXrRuntime::Space;
        using AppViews = XrCompositionLayerProjectionView[2];
        using Output = std::array<VDXRFG_ViewV1, 2>;

        static bool make(const Space& layer,
                         const Space& origin,
                         const AppViews& views,
                         const ovrLayerEyeFov& colour,
                         Output& result) {
            return OpenXrRuntime::makeEarlyFrameGenerationViews(layer, origin, views, colour, result);
        }

        static bool closeEnough(float a, float b) {
            return std::abs(a - b) < 1e-6f;
        }
        static unsigned run() {
            unsigned failed{};
            const auto check = [&](bool pass, const char* name) {
                std::printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
                failed += !pass;
            };
            Space layer{}, origin{};
            layer.referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            origin.referenceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            layer.poseInSpace.orientation.w = origin.poseInSpace.orientation.w = 1.f;
            AppViews views{};
            ovrLayerEyeFov colour{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye].pose.orientation.w = 1.f;
                views[eye].fov = {-.7f, .8f, .9f, -.6f};
                colour.Viewport[eye] = {{int(eye) * 80 + 3, 5}, {64, 48}};
            }
            Output local{}, stage{}, translated{}, rotated{}, fov{}, crop{};
            const bool localOk = make(layer, origin, views, colour, local);
            check(localOk && closeEnough(local[0].orientation[3], 1.f) && closeEnough(local[1].orientation[3], 1.f) &&
                      closeEnough(local[0].fov[0], -.7f) && closeEnough(local[1].fov[3], -.6f) &&
                      local[0].rect.x == 3 && local[1].rect.x == 83 && local[0].rect.y == 5 &&
                      local[0].rect.width == 64 && local[1].rect.height == 48,
                  "LOCAL to STAGE creates exact provisional stereo rays and finalized crops");
            layer.referenceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            const bool stageOk = make(layer, origin, views, colour, stage);
            check(stageOk && closeEnough(stage[0].orientation[3], 1.f), "STAGE to STAGE is eligible");
            layer.referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
            layer.poseInSpace.position = {12.f, 1.75f, -4.f};
            origin.poseInSpace.position = {-2.f, -.5f, 8.f};
            const bool translationOk = make(layer, origin, views, colour, translated);
            check(localOk && translationOk && closeEnough(translated[0].orientation[3], local[0].orientation[3]) &&
                      closeEnough(translated[1].orientation[3], local[1].orientation[3]),
                  "LOCAL_FLOOR and floor-height translation preserve pack rotation");
            constexpr float half = .7071067811865475f;
            layer.poseInSpace.orientation = {0.f, 0.f, half, half};
            const bool rotateOk = make(layer, origin, views, colour, rotated);
            check(rotateOk && closeEnough(rotated[0].orientation[2], half) &&
                      closeEnough(rotated[1].orientation[3], half),
                  "reference rotation changes both provisional pack mappings");
            views[1].fov.angleRight = .5f;
            const bool fovOk = make(layer, origin, views, colour, fov);
            check(fovOk && closeEnough(fov[1].fov[1], .5f) && closeEnough(fov[0].fov[1], .8f),
                  "one-eye FOV mutation reaches exactly that provisional mapping");
            colour.Viewport[1].Pos = {11, 17};
            const bool cropOk = make(layer, origin, views, colour, crop);
            check(cropOk && crop[1].rect.x == 11 && crop[1].rect.y == 17 && crop[0].rect.x == 3,
                  "one-eye final crop mutation reaches exactly that provisional mapping");
            for (const auto type : {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_MAX_ENUM}) {
                layer.referenceType = type;
                Output declined{};
                check(!make(layer, origin, views, colour, declined), "VIEW or action reference declines early");
            }
            layer.referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            layer.action = reinterpret_cast<XrAction>(uintptr_t(1));
            Output declined{};
            check(!make(layer, origin, views, colour, declined), "action-bound fixed reference declines early");
            layer.action = XR_NULL_HANDLE;
            origin.referenceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            check(!make(layer, origin, views, colour, declined), "dynamic origin reference declines early");
            origin.referenceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            views[1].pose.orientation = {0.f, 0.f, 0.f, 0.f};
            check(!make(layer, origin, views, colour, declined), "invalid right-eye rotation declines early");
            views[1].pose.orientation.w = 1.f;
            colour.Viewport[1].Pos.x = -1;
            check(!make(layer, origin, views, colour, declined), "negative finalized right-eye crop declines early");
            colour.Viewport[1].Pos.x = 11;
            origin.poseInSpace.orientation = {half, 0.f, 0.f, half};
            Output originRotation{};
            check(make(layer, origin, views, colour, originRotation) &&
                      closeEnough(originRotation[0].orientation[0], -.5f) &&
                      closeEnough(originRotation[0].orientation[1], .5f) &&
                      closeEnough(originRotation[0].orientation[2], .5f) &&
                      closeEnough(originRotation[0].orientation[3], .5f),
                  "noncommuting origin rotation is inverted before layer rotation");
            return failed;
        }
    };
} // namespace virtualdesktop_openxr

int main() {
    const auto failures = virtualdesktop_openxr::RuntimeFrameGenerationRegression::run();
    std::printf("HOST_VIEWS failures=%u\n", failures);
    return failures ? 1 : 0;
}
