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
        info.enableGpuTiming = argc == 4;
        const auto result = fg.initialize(info);
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
