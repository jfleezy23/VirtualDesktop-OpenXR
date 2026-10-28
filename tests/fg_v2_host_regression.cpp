#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main

int main(int argc, char** argv) {
    try {
        check(argc == 3 || argc == 4, "test module, mode and optional diagnostic request required");
        const auto path = std::filesystem::absolute(argv[1]);
        const unsigned mode = static_cast<unsigned>(std::stoul(argv[2]));
        HMODULE module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load negotiation test module");
        auto setMode = reinterpret_cast<void(WINAPI*)(unsigned)>(GetProcAddress(module, "SetMode"));
        auto count = reinterpret_cast<unsigned(WINAPI*)(unsigned)>(GetProcAddress(module, "Count"));
        check(setMode && count, "test control exports");
        setMode(mode);
        Interop hw;
        FrameGeneration fg;
        FrameGeneration::InitInfo info{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       path,
                                       eyeWidth,
                                       height,
                                       3,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        const std::string option = argc == 4 ? argv[3] : "";
        info.enableGpuTiming = option == "diagnostics" || option == "ffx-diagnostics";
        info.useFidelityFxFull = option == "ffx" || option == "ffx-diagnostics" || option == "v1-ffx";
        const bool legacyOnly = option == "v1" || option == "v1-ffx";
        if (option == "v1-4x" || option == "v2-4x") {
            info.multiplier = 4;
            check(fg.initialize(info) == VDXRFG_E_REJECTED && !fg.initialized(), "legacy module rejects explicit 4X");
            check(count(0) == 0 && count(1) == 0, "4X rejection precedes any legacy Create");
            success(fg.shutdown(), "shutdown rejected 4X");
            FreeLibrary(module);
            std::puts("PASS: new host rejects 4X with old module before creation");
            return 0;
        }
        const auto result = fg.initialize(info);
        if (legacyOnly || info.useFidelityFxFull) {
            const bool unsupported = info.useFidelityFxFull && (legacyOnly || mode != 8);
            check(result == (unsupported ? VDXRFG_E_REJECTED : E_NOTIMPL) && !fg.initialized(),
                  "explicit backend fails closed or reaches the negotiated Create");
            check(count(0) == (!unsupported && legacyOnly ? 1u : 0u) &&
                      count(1) == (!unsupported && !legacyOnly ? 1u : 0u),
                  "unsupported backend never calls either Create or retries another backend");
            if (!unsupported && !legacyOnly)
                check(count(3) == (VDXRFG_CREATE_FIDELITYFX_FULL | VDXRFG_CREATE_HOST_OWNS_REAL |
                                   (info.enableGpuTiming ? VDXRFG_CREATE_GPU_TIMING : 0u)),
                      "required full-resolution FidelityFX flag survives optional capability negotiation");
            success(fg.shutdown(), "shutdown rejected backend request");
            FreeLibrary(module);
            std::puts("PASS: explicit backend negotiation and legacy compatibility");
            return 0;
        }
        const HRESULT expected = mode == 5 ? E_NOTIMPL : mode == 6 ? VDXRFG_E_POISONED : VDXRFG_E_REJECTED;
        check(result == expected && !fg.initialized(), "bad V2 fails closed without a legacy downgrade");
        check(count(0) == 0 && count(1) == (mode >= 5 ? 1u : 0u), "only accepted V2 invokes modern creation");
        if (mode >= 5) {
            check(count(3) == (info.enableGpuTiming ? VDXRFG_CREATE_GPU_TIMING : 0u),
                  "host diagnostics explicitly opt in; regular gameplay defaults off");
        }
        success(fg.shutdown(), "shutdown retains and resolves poisoned creation ownership");
        check(count(2) == (mode == 6 ? 1u : 0u), "only poisoned owned context requires destruction");
        FreeLibrary(module);
        std::puts("PASS: optional negotiation fails closed and preserves poison ownership");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
