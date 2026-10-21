#define main retainedV2RegressionMain
#include "fg_v2_regression.cpp"
#undef main

// Observe the actual packed color allocation passed to FidelityFX. Distinct
// output pixels alone would not prove that the backend uses full resolution.
class FullInputProbe {
    using Create = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
                                               const D3D12_HEAP_PROPERTIES*,
                                               D3D12_HEAP_FLAGS,
                                               const D3D12_RESOURCE_DESC*,
                                               D3D12_RESOURCE_STATES,
                                               const D3D12_CLEAR_VALUE*,
                                               REFIID,
                                               void**);
    inline static FullInputProbe* active{};
    Gpu gpu{false}; // Match the native-operation fixture before any device enables validation.
    Create original{};
    const DWORD thread = GetCurrentThreadId();
    unsigned packed{}, halfPacked{};
    static HRESULT STDMETHODCALLTYPE create(ID3D12Device* device,
                                            const D3D12_HEAP_PROPERTIES* heap,
                                            D3D12_HEAP_FLAGS flags,
                                            const D3D12_RESOURCE_DESC* desc,
                                            D3D12_RESOURCE_STATES state,
                                            const D3D12_CLEAR_VALUE* clear,
                                            REFIID iid,
                                            void** result) {
        const auto hr = active->original(device, heap, flags, desc, state, clear, iid, result);
        if (GetCurrentThreadId() == active->thread && SUCCEEDED(hr) && desc && desc->Width == eyeWidth &&
            desc->Height == 2 * height + 64 && desc->DepthOrArraySize == 1 &&
            desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM && (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            ++active->packed;
        if (GetCurrentThreadId() == active->thread && SUCCEEDED(hr) && desc && desc->Width == eyeWidth / 2 &&
            desc->Height == height + 64 && desc->DepthOrArraySize == 1 && desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            ++active->halfPacked;
        return hr;
    }
    void attach(bool enable) {
        auto hr = DetourTransactionBegin();
        const bool started = hr == NO_ERROR;
        if (!hr)
            hr = DetourUpdateThread(GetCurrentThread());
        if (!hr)
            hr = (enable ? DetourAttach : DetourDetach)(reinterpret_cast<PVOID*>(&original), create);
        if (!hr)
            hr = DetourTransactionCommit();
        else if (started)
            DetourTransactionAbort();
        success(HRESULT_FROM_WIN32(hr), "full input allocation observer");
    }

  public:
    FullInputProbe() {
        original = reinterpret_cast<Create>((*reinterpret_cast<void***>(gpu.device.Get()))[27]);
        check(!active, "one full input allocation observer");
        active = this;
        try {
            attach(true);
        } catch (...) {
            active = nullptr;
            throw;
        }
    }
    ~FullInputProbe() {
        try {
            attach(false);
        } catch (...) {
            std::_Exit(2);
        }
        active = nullptr;
    }
    unsigned count() const {
        return packed;
    }
    unsigned halfCount() const {
        return halfPacked;
    }
};

int main(int argc, char** argv) {
    try {
        check(argc == 2 || argc == 3, "module and optional negative-control scenario");
        auto module = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load synthesis module");
        const std::string mode = argc == 3 ? argv[2] : "full";
        if (mode == "prime-baseline") {
            generated(module, 3, false, VDXRFG_CREATE_GPU_TIMING, true);
        } else if (mode == "timing-baseline") {
            // Use only existing flags against the isolated old FFX prototype.
            // Query observer intercepts invalid resolves before GPU execution.
            generated(module, 3, false, VDXRFG_CREATE_GPU_TIMING, false, true);
        } else {
            FullInputProbe inputResolution;
            constexpr uint32_t fidelityFxFull = VDXRFG_CREATE_FIDELITYFX_FULL;
            const auto generateFull = [&](uint32_t multiplier, uint32_t flags, bool audit = false) {
                const auto before = inputResolution.count();
                const auto halfBefore = inputResolution.halfCount();
                auto result = generated(module, multiplier, false, flags, audit);
                check(inputResolution.count() > before,
                      "this FidelityFX context allocates full per-eye flow input, not half");
                check(inputResolution.halfCount() == halfBefore, "full-resolution mode never allocates the half input");
                return result;
            };
            for (uint32_t multiplier : {2u, 3u}) {
                const auto untimed = generateFull(multiplier, fidelityFxFull);
                const auto timed = generateFull(multiplier, fidelityFxFull | VDXRFG_CREATE_GPU_TIMING, true);
                const auto owned = generateFull(multiplier, fidelityFxFull | VDXRFG_CREATE_HOST_OWNS_REAL);
                check(untimed.pixels == timed.pixels, "FidelityFX timing preserves exact real and synthetic pixels");
                check(owned.pixels[0] == untimed.pixels[0] && owned.pixels[1] == untimed.pixels[1],
                      "FidelityFX host-owned real preserves exact generated images");
                check(timed.statistics.last_gpu_us > 0, "FidelityFX publishes a real completed pair timing");
                check(!timed.statistics.pack_gpu_us && !timed.statistics.flow_gpu_us &&
                          !timed.statistics.composition_gpu_us,
                      "FidelityFX unavailable substage timings remain zero");
                check(!untimed.statistics.last_gpu_us && !untimed.operations.queries && !untimed.operations.resolves,
                      "regular FidelityFX gameplay has no timing queries or resolves");
                std::printf("%uX FFX full completed GPU span=%llu us\n", multiplier, timed.statistics.last_gpu_us);
            }
            const auto beforeDefault = inputResolution.count();
            generated(module, 3, false, 0);
            check(inputResolution.count() == beforeDefault,
                  "NVIDIA default is a negative control for the full FFX pack");
        }
        FreeLibrary(module);
        std::puts("PASS: full-resolution FidelityFX, exact timing invariance and initialized timing samples");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
