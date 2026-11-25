#include <cstdio>
#include <limits>
#include <stdexcept>

#if __has_include("../virtualdesktop-openxr/NrBeforeSrPolicy.h")
#include "../virtualdesktop-openxr/NrBeforeSrPolicy.h"

using namespace virtualdesktop_openxr::nr_before_sr;

namespace {
    void require(bool ok, const char* description) {
        if (!ok)
            throw std::runtime_error(description);
    }

    void startupPlacement() {
        constexpr auto image = L"F:\\SteamLibrary\\Hubris\\HubrisVR-Win64-Shipping.exe";
        constexpr auto same = L"f:/steamlibrary/hubris/HUBRISVR-win64-shipping.EXE";
        require(IsHubrisShippingImage(image) && IsHubrisShippingImage(same), "shipping executable across libraries");
        for (const auto path : {L"HubrisVR-Win64-Shipping.exe",
                                L"F:\\SteamLibrary\\Hubris\\HubrisVR.exe",
                                L"F:\\SteamLibrary\\Hubris\\HubrisVR-Win64-Shipping.exe.bak",
                                L"F:\\SteamLibrary\\..\\HubrisVR-Win64-Shipping.exe",
                                L"C:\\Games\\RoboRecall-Win64-Shipping.exe"})
            require(!IsHubrisShippingImage(path), "other or invalid image");
    }

    void gamePreference() {
        for (const auto value : {L"False", L"false", L"FALSE", L" \tFalse ", L"\"False\"", L"'False'"})
            require(DlssExplicitlyDisabled(value), "explicit false selects compositor placement");
        for (const auto value : {L"", L"True", L"0", L" false!", L"'false", L"False extra", L" \t"})
            require(!DlssExplicitlyDisabled(value), "missing, enabled or malformed leaves native SR available");
    }

    void liveEnablement() {
        BeforeSrEnablement state;
        require(!state.beginFrame().enabled, "default off");
        state.update(true, true);
        auto frame = state.beginFrame();
        require(frame.enabled && frame.resetHistory, "enable resets history");
        state.update(false, true);
        state.update(true, true);
        frame = state.beginFrame();
        require(frame.enabled && frame.resetHistory, "off and on between frames invalidates history");
        state.update(true, true);
        require(!state.beginFrame().resetHistory, "unchanged effective enable preserves history");
        state.update(true, false);
        frame = state.beginFrame();
        require(!frame.enabled && frame.resetHistory, "invalid profile disables early NR");
        state.update(false, true);
        require(!state.beginFrame().enabled, "valid disabled profile cannot enable early NR");
    }

    struct Fixture {
        DeviceBinding binding{10, 20, 10, true};
        TrackedFeature feature{30, 50, 10, 1, 1 | 2 | 8, {1600, 1200}, {2000, 1500}, true};
        Evaluation call{30,
                        50,
                        40,
                        10,
                        20,
                        true,
                        {101, 10, 3200, 1250, 1, 1, 1, true, {3, 4, 1440, 1080}},
                        {102, 10, 3150, 1290, 1, 1, 1, true, {10, 21, 1440, 1080}},
                        {103, 10, 3120, 1220, 1, 1, 1, true, {60, 10, 1440, 1080}},
                        {104, 10, 4080, 1610, 1, 1, 1, true, {30, 50, 2000, 1500}},
                        -1440.f,
                        1080.f};

        bool admitted() const {
            return CanRunBeforeSr(Placement::BeforeSrOnly, binding, feature, call);
        }
    };

    void stereoLayouts() {
        Fixture f;
        for (bool right : {false, true}) {
            f.call.color.active.x = right ? 1504 : 3;
            f.call.depth.active.x = right ? 1488 : 10;
            f.call.output.active.x = right ? 2080 : 30;
            f.feature.createFlags = 1 | 2 | 8;
            f.call.motion.active = {right ? 1632u : 60u, 10, 1440, 1080};
            f.call.motion.allocationWidth = 3120;
            f.call.motion.allocationHeight = 1220;
            require(f.admitted(), "both eyes, independently padded guides, render-resolution motion");
            f.feature.createFlags = 1;
            f.call.motion.active = {right ? 2050u : 16u, 18, 2000, 1500};
            f.call.motion.allocationWidth = 4084;
            f.call.motion.allocationHeight = 1599;
            require(f.admitted(), "both eyes, display-resolution motion, only the HDR flag");
        }
        f.feature.generation++;
        require(!f.admitted(), "recycled handle cannot use the retired generation");
        f.call.generation++;
        require(f.admitted(), "newly tracked generation");
        require(!CanRunBeforeSr(Placement::Late, f.binding, f.feature, f.call), "late mode cannot admit an early call");
        f.feature.outputSubrects = false;
        f.call.output.active.x = f.call.output.active.y = 0;
        f.call.output.allocationWidth = f.feature.output.width;
        f.call.output.allocationHeight = f.feature.output.height;
        f.feature.createFlags = 1 | 8 | 32 | 64;
        require(f.admitted(), "full output without subrect support; HDR, inverted depth, sharpening and exposure");
    }

    template <class Change>
    void reject(Change change, const char* description) {
        Fixture f;
        change(f);
        require(!f.admitted(), description);
    }

    void ownership() {
        reject([](Fixture& f) { f.binding.d3d12Direct = false; }, "wrong API or queue type");
        reject([](Fixture& f) { f.binding.device = 0; }, "missing device");
        reject([](Fixture& f) { f.binding.queue = f.call.queue = 0; }, "missing queue");
        reject([](Fixture& f) { f.binding.queueDevice++; }, "queue on foreign device");
        reject([](Fixture& f) { f.call.listDevice++; }, "list on foreign device");
        reject([](Fixture& f) { f.call.directList = false; }, "wrong list type");
        reject([](Fixture& f) { f.call.list = 0; }, "missing list");
        reject([](Fixture& f) { f.call.queue++; }, "call on unqualified queue");
        reject([](Fixture& f) { f.feature.device++; }, "feature on foreign device");
        reject([](Fixture& f) { f.feature.handle = f.call.handle = 0; }, "null handle never tracked");
        reject([](Fixture& f) { f.call.handle++; }, "different feature handle");
        reject([](Fixture& f) { f.feature.generation = f.call.generation = 0; }, "untracked generation");
        for (auto featureId : {0u, 11u, 13u, 18u})
            reject([=](Fixture& f) { f.feature.featureId = featureId; }, "only SR feature id admitted");
        for (auto flags : {4u, 16u, 128u, 256u, 0x80000000u, 0xffffffffu})
            reject([=](Fixture& f) { f.feature.createFlags |= flags; }, "jittered, reserved or invalid SR flags");
        reject([](Fixture& f) { f.feature.createFlags &= ~1u; }, "the pre-exposed HDR codec requires the SR HDR flag");
        for (auto scale : {0.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            reject([=](Fixture& f) { f.call.mvScaleX = scale; }, "invalid horizontal MV scale");
            reject([=](Fixture& f) { f.call.mvScaleY = scale; }, "invalid vertical MV scale");
        }
    }

    void resourceChecks() {
        constexpr TextureRegion Evaluation::* views[] = {
            &Evaluation::color, &Evaluation::depth, &Evaluation::motion, &Evaluation::output};
        for (auto view : views) {
            reject([=](Fixture& f) { (f.call.*view).device++; }, "foreign resource device");
            reject([=](Fixture& f) { (f.call.*view).identity = 0; }, "missing resource");
            reject([=](Fixture& f) { (f.call.*view).arraySize = 2; }, "array view unsupported");
            reject([=](Fixture& f) { (f.call.*view).samples = 4; }, "multisampled view unsupported");
            reject([=](Fixture& f) { (f.call.*view).mipLevels = 2; }, "ambiguous mip");
            reject([=](Fixture& f) { (f.call.*view).isTexture2D = false; }, "wrong texture dimension");
            reject([=](Fixture& f) { (f.call.*view).active.width = 0; }, "zero active width");
            reject([=](Fixture& f) { (f.call.*view).active.height = 0; }, "zero active height");
            reject([=](Fixture& f) { (f.call.*view).allocationWidth = 0; }, "zero allocation");
            reject([=](Fixture& f) { (f.call.*view).allocationWidth = 1ull << 32; }, "unsafe narrowed width");
            reject([=](Fixture& f) { (f.call.*view).active.x = 0xfffffff0; }, "horizontal wrap");
            reject([=](Fixture& f) { (f.call.*view).active.y = 0xfffffff0; }, "vertical wrap");
            reject(
                [=](Fixture& f) {
                    auto& t = f.call.*view;
                    t.allocationHeight = t.active.y + t.active.height - 1;
                },
                "one row outside texture");
            reject(
                [=](Fixture& f) {
                    auto& t = f.call.*view;
                    t.allocationWidth = t.active.x + t.active.width - 1;
                },
                "one column outside texture");
            for (auto other : views) {
                if (other != view)
                    reject([=](Fixture& f) { (f.call.*view).identity = (f.call.*other).identity; },
                           "aliased resources");
            }
        }
        reject([](Fixture& f) { f.call.depth.active.height--; }, "depth does not cover render extent");
        reject([](Fixture& f) { f.call.motion.active.width--; }, "low-resolution motion width mismatch");
        reject([](Fixture& f) { f.feature.createFlags = 1; }, "display motion flag with low-resolution motion");
        reject([](Fixture& f) { f.call.output.active.width--; }, "output width disagrees with feature target");
        reject([](Fixture& f) { f.feature.maxRender.height = 1000; }, "render above feature bound");
        reject([](Fixture& f) { f.feature.outputSubrects = false; }, "unqualified nonzero output origin");
        reject([](Fixture& f) { f.feature.output.height = f.call.output.active.height = 1000; },
               "SR target below render size");
    }
} // namespace

int main() {
    try {
        startupPlacement();
        gamePreference();
        liveEnablement();
        stereoLayouts();
        ownership();
        resourceChecks();
        std::puts("PASS before-SR placement and admission");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
#else
int main() {
    std::fputs("FAIL: before-SR placement and admission policy unavailable\n", stderr);
    return 1;
}
#endif
