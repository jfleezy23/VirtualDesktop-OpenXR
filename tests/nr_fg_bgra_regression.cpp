// Exercise the real BGRA capture and independent B output, including encoded sRGB bytes.
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main
#include <detours.h>

namespace {
    struct CaptureProbe {
        using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
        using RegionFn = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
        using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
        using WaitFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
        using CompletedFn = UINT64(STDMETHODCALLTYPE*)(ID3D11Fence*);
        inline static CaptureProbe* active{};
        CopyFn original{};
        RegionFn originalRegion{};
        DispatchFn originalDispatch{};
        WaitFn originalWait{};
        CompletedFn originalCompleted{};
        ComPtr<ID3D11Fence> sampleFence;
        ComPtr<ID3D11Texture2D> packed;
        ID3D11DeviceContext* context{};
        bool armed{};
        unsigned copies{}, dispatches{}, waits{};
        unsigned completionReads{};
        bool forceCompletion{};
        UINT64 completionValue{};
        DWORD thread = GetCurrentThreadId();
        static void observe(ID3D11DeviceContext* ctx, ID3D11Resource* dst) {
            if (GetCurrentThreadId() == active->thread && ctx == active->context && active->armed) {
                ++active->copies;
                ComPtr<ID3D11Texture2D> image;
                if (SUCCEEDED(dst->QueryInterface(IID_PPV_ARGS(&image)))) {
                    D3D11_TEXTURE2D_DESC desc{};
                    image->GetDesc(&desc);
                    if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                        (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                        active->packed = image;
                    }
                }
            }
        }
        static void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
            if (GetCurrentThreadId() == active->thread && ctx == active->context && active->armed)
                ++active->dispatches;
            active->originalDispatch(ctx, x, y, z);
        }
        static HRESULT STDMETHODCALLTYPE wait(ID3D11DeviceContext4* ctx, ID3D11Fence* fence, UINT64 value) {
            if (GetCurrentThreadId() == active->thread && ctx == active->context && active->armed)
                ++active->waits;
            return active->originalWait(ctx, fence, value);
        }
        static UINT64 STDMETHODCALLTYPE completed(ID3D11Fence* fence) {
            if (GetCurrentThreadId() == active->thread && active->armed && fence != active->sampleFence.Get()) {
                ++active->completionReads;
                if (active->forceCompletion)
                    return active->completionValue;
            }
            return active->originalCompleted(fence);
        }
        static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
            observe(ctx, dst);
            active->original(ctx, dst, src);
        }
        static void STDMETHODCALLTYPE region(ID3D11DeviceContext* ctx,
                                             ID3D11Resource* dst,
                                             UINT sub,
                                             UINT x,
                                             UINT y,
                                             UINT z,
                                             ID3D11Resource* src,
                                             UINT srcSub,
                                             const D3D11_BOX* box) {
            observe(ctx, dst);
            active->originalRegion(ctx, dst, sub, x, y, z, src, srcSub, box);
        }
        explicit CaptureProbe(Interop& hw) : context(hw.context.Get()) {
            check(!active, "one capture probe");
            auto ctx = hw.context.Get();
            success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&sampleFence)), "probe fence");
            active = this;
            original = reinterpret_cast<CopyFn>((*reinterpret_cast<void***>(ctx))[47]);
            originalRegion = reinterpret_cast<RegionFn>((*reinterpret_cast<void***>(ctx))[46]);
            originalDispatch = reinterpret_cast<DispatchFn>((*reinterpret_cast<void***>(ctx))[41]);
            originalWait = reinterpret_cast<WaitFn>((*reinterpret_cast<void***>(ctx))[148]);
            originalCompleted = reinterpret_cast<CompletedFn>((*reinterpret_cast<void***>(sampleFence.Get()))[8]);
            success(HRESULT_FROM_WIN32(DetourTransactionBegin()), "copy probe begin");
            success(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())), "copy probe thread");
            success(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&original), copy)), "copy probe attach");
            success(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalRegion), region)),
                    "region probe attach");
            success(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalDispatch), dispatch)),
                    "dispatch probe attach");
            success(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait)),
                    "wait probe attach");
            success(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalCompleted), completed)),
                    "fence completion probe attach");
            success(HRESULT_FROM_WIN32(DetourTransactionCommit()), "copy probe commit");
        }
        ~CaptureProbe() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), copy) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalRegion), region) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDispatch), dispatch) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalCompleted), completed) || DetourTransactionCommit())
                std::_Exit(2);
            active = nullptr;
        }
        void begin(bool force = false, UINT64 value = 0) {
            copies = dispatches = waits = completionReads = 0;
            forceCompletion = force;
            completionValue = value;
            armed = true;
        }
        void verify(const char* label, unsigned expectedCopies, unsigned expectedDispatches) const {
            std::printf("%s: copies=%u dispatches=%u waits=%u fenceReads=%u expected=%u/%u/0\n",
                        label,
                        copies,
                        dispatches,
                        waits,
                        completionReads,
                        expectedCopies,
                        expectedDispatches);
            check(copies == expectedCopies && dispatches == expectedDispatches && waits == 0,
                  "BGRA real-copy operation budget preserves the no-added-wait contract");
        }
    };

    struct ContextSentinel {
        ID3D11DeviceContext4* context;
        ComPtr<ID3D11Texture2D> image, storage;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11Buffer> constants;
        explicit ContextSentinel(Interop& hw) : context(hw.context.Get()), image(hw.texture()) {
            success(hw.device->CreateShaderResourceView(image.Get(), nullptr, &srv), "sentinel SRV");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 16;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            success(hw.device->CreateTexture2D(&desc, nullptr, &storage), "sentinel texture");
            success(hw.device->CreateUnorderedAccessView(storage.Get(), nullptr, &uav), "sentinel UAV");
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 16;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            success(hw.device->CreateBuffer(&buffer, nullptr, &constants), "sentinel constants");
            context->CSSetShader(nullptr, nullptr, 0);
            context->CSSetShaderResources(0, 1, srv.GetAddressOf());
            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        }
        void verify() {
            ComPtr<ID3D11ComputeShader> shader;
            ComPtr<ID3D11ShaderResourceView> actualSrv;
            ComPtr<ID3D11UnorderedAccessView> actualUav;
            ComPtr<ID3D11Buffer> actualConstants;
            context->CSGetShader(&shader, nullptr, nullptr);
            context->CSGetShaderResources(0, 1, &actualSrv);
            context->CSGetUnorderedAccessViews(0, 1, &actualUav);
            context->CSGetConstantBuffers(0, 1, &actualConstants);
            check(!shader && actualSrv == srv && actualUav == uav && actualConstants == constants,
                  "conversion restores nondefault compute bindings and shader state");
        }
        ~ContextSentinel() {
            context->ClearState();
        }
    };

    std::vector<uint8_t> pixels(unsigned seed) {
        std::vector<uint8_t> result(width * height * 4);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x) {
                const auto i = (size_t(y) * width + x) * 4;
                const auto eye = x / eyeWidth;
                result[i] = static_cast<uint8_t>(x + seed + eye * 43);
                result[i + 1] = static_cast<uint8_t>(y + seed * 3 + eye * 71);
                result[i + 2] = static_cast<uint8_t>(x + y * 7 + seed * 5 + eye * 97);
                result[i + 3] = static_cast<uint8_t>(x * 3 + y * 5 + seed + eye * 19);
            }
        return result;
    }

    ComPtr<ID3D11Texture2D> source(Interop& hw, DXGI_FORMAT format, bool arrayMip, const std::vector<uint8_t>& rgba) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width + 32;
        desc.Height = height + 16;
        desc.ArraySize = desc.MipLevels = arrayMip ? 2 : 1;
        desc.SampleDesc.Count = 1;
        desc.Format = format;
        // No SRV binding: conversion must not assume app or SDK bind flags.
        ComPtr<ID3D11Texture2D> result;
        success(hw.device->CreateTexture2D(&desc, nullptr, &result), "BGRA source");
        std::vector<uint8_t> storage(size_t(desc.Width) * desc.Height * 4, 0x71);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x) {
                const auto in = (size_t(y) * width + x) * 4;
                const auto out = (size_t(y + 8) * desc.Width + x + 16) * 4;
                const bool rgbaFormat = format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                                        format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                storage[out] = rgba[in + (rgbaFormat ? 0 : 2)];
                storage[out + 1] = rgba[in + 1];
                storage[out + 2] = rgba[in + (rgbaFormat ? 2 : 0)];
                storage[out + 3] = rgba[in + 3];
            }
        hw.context->UpdateSubresource(result.Get(), 0, nullptr, storage.data(), desc.Width * 4, 0);
        return result;
    }

    void runBgra(const char* module, const std::string& mode) {
        const bool motion = mode == "motion" || mode == "motion-srgb";
        const bool srgb = mode == "srgb" || mode == "rgba-srgb" || mode == "motion-srgb";
        const bool rgba = mode == "rgba" || mode == "rgba-srgb";
        const bool separate = mode == "separate";
        const bool mixed = mode == "mixed" || mode == "mixed-reverse";
        const bool unsupported = mode == "unsupported-right";
        const bool invalid = mode == "invalid-right";
        const bool twoLive = mode == "two-live";
        const bool staleOwner = mode == "reuse-stale-submit";
        const bool reset = mode == "reuse-reset";
        const bool pending = mode == "reuse-pending";
        const bool removed = mode == "reuse-removed";
        const bool poisoned = mode == "reuse-poisoned-capture";
        const bool reuse = mode == "reuse-completed" || pending || removed || reset;
        check(mode == "typeless" || mode == "unorm" || srgb || motion || rgba || separate || mixed || invalid ||
                  unsupported || mode == "array-mip" || mode == "source-reuse" || reuse || staleOwner || poisoned ||
                  twoLive,
              "known BGRA mode");
        Interop hw;
        FrameGeneration fg;
        FrameGeneration::InitInfo init{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       std::filesystem::absolute(module),
                                       eyeWidth,
                                       height,
                                       3,
                                       srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM};
        if (const auto backend = std::getenv("VDXR_FG_TEST_BACKEND")) {
            check(!strcmp(backend, "nvidia") || !strcmp(backend, "fidelityfx-full"), "known BGRA test backend");
            init.useFidelityFxFull = !strcmp(backend, "fidelityfx-full");
        }
        success(fg.initialize(init), "initialize BGRA host");
        const auto format = rgba   ? (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_TYPELESS)
                            : srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                                   : (mode == "unorm" ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8A8_TYPELESS);
        const auto a = motion ? scene(0) : pixels(11), b = motion ? scene(16) : pixels(37);
        FinalProjectionInput input;
        input.colour[0] = source(hw, format, mode == "array-mip", a);
        input.colour[1] = separate ? source(hw, format, false, a) : input.colour[0];
        if (mixed)
            input.colour[mode == "mixed-reverse" ? 0 : 1] = hw.texture(&a);
        if (unsupported)
            input.colour[1] = source(hw, DXGI_FORMAT_B8G8R8X8_UNORM, false, a);
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& view = input.views[eye];
            view.rect = {16 + eye * eyeWidth, 8, eyeWidth, height};
            view.orientation[3] = 1;
            view.position[0] = eye ? .032f : -.032f;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
        }
        if (invalid)
            input.views[1].rect.x = UINT32_MAX;
        if (mixed) {
            const unsigned rgbaEye = mode == "mixed-reverse" ? 0 : 1;
            input.views[rgbaEye].rect = {rgbaEye * eyeWidth, 0, eyeWidth, height};
        }
        input.serial = 1;
        input.time = 1000000000;
        input.period = 10000000;
        FrameGenerationCadence cadence;
        input.slots =
            cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 0), 0, false);
        CaptureProbe probe(hw);
        ContextSentinel sentinel(hw);
        OwnedFrameTransaction prime;
        probe.armed = true;
        const auto result = fg.capture(input, prime);
        probe.armed = false;
        sentinel.verify();
        if (unsupported || invalid) {
            check(result == VDXRFG_E_REJECTED && !prime.ticket.token && !probe.packed,
                  "unsupported format or invalid right-eye bounds rejected before packing");
            success(fg.shutdown(), "shutdown negative BGRA host");
            return;
        }
        success(result, "BGRA capture must reach the actual FG module");
        check(probe.packed != nullptr, "observe actual shared RGBA FG input");
        hw.await(fg, prime);
        check(hw.read(probe.packed.Get()) == a, "all BGRA capture bytes normalize exactly, including alpha and sRGB");
        const auto outputTexture = [&] {
            auto image = hw.texture();
            if (srgb) {
                D3D11_TEXTURE2D_DESC desc{};
                image->GetDesc(&desc);
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                success(hw.device->CreateTexture2D(&desc, nullptr, image.ReleaseAndGetAddressOf()),
                        "typed sRGB output");
            }
            return image;
        };
        auto output = outputTexture();
        if (poisoned) {
            OwnedFrameTransaction rejected;
            probe.begin(true, UINT64_MAX);
            const auto poisonedCapture = fg.capture(input, rejected);
            probe.armed = false;
            check(poisonedCapture == VDXRFG_E_POISONED && !rejected.ticket.token && probe.completionReads == 1,
                  "actual producer-fence device failure poisons a capture before any new packing");
            probe.verify("poisoned capture writes no new input", 0, 0);
            check(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()) == VDXRFG_E_REJECTED,
                  "poisoned input ownership cannot prepare a retained real ticket");
            check(fg.shutdown() == VDXRFG_E_POISONED, "unknown host completion retains the poisoned graph");
            return;
        }
        if (reset)
            check(fg.reset(0) == VDXRFG_E_BUSY, "busy reset retains the live ticket but invalidates input reuse");
        if (!twoLive && !staleOwner) {
            if (reuse)
                probe.begin(pending || removed, removed ? UINT64_MAX : 0);
            success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "BGRA real copy");
            probe.armed = false;
            sentinel.verify();
            check(hw.read(output.Get()) == a, "independent real B preserves every canonical RGBA byte");
            if (reuse) {
                probe.verify(mode.c_str(), pending || removed || reset ? 4 : 1, pending || removed || reset ? 2 : 0);
                if (pending || removed)
                    check(probe.completionReads == 1, "one actual consumed-fence query selects immediate fallback");
            }
        }
        if (mode == "source-reuse") {
            auto replacement = source(hw, format, false, b);
            hw.context->CopyResource(input.colour[0].Get(), replacement.Get());
            success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "reuse already prepared B");
            check(hw.read(output.Get()) == a, "prepared real output survives source reuse");
        }
        auto stale = prime;
        if (!twoLive && !staleOwner)
            hw.retire(fg, prime);
        if (reset)
            success(fg.reset(0), "settled reset clears input ownership and history");
        input.colour[0] = input.colour[1] = source(hw, format, mode == "array-mip", b);
        for (unsigned eye = 0; eye < 2; ++eye)
            input.views[eye].rect = {16 + eye * eyeWidth, 8, eyeWidth, height};
        if (!staleOwner)
            ++input.serial;
        const auto previous = input.time;
        input.time += input.period * 3;
        input.slots = cadence.plan(
            cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 1), previous, true);
        OwnedFrameTransaction pair;
        const auto pairResult = fg.capture(input, pair);
        if (staleOwner) {
            check(pairResult == VDXRFG_E_REJECTED && !pair.ticket.token,
                  "duplicate serial rejects after the new BGRA input has been packed");
            check(hw.read(probe.packed.Get()) == b, "failed Submit really overwrote the shared input with newer B");
            probe.begin();
            success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "late A after rejected B submit");
            probe.armed = false;
            check(hw.read(output.Get()) == a, "failed newer capture cannot make old A reuse newer B pixels");
            probe.verify("rejected-newer-submit old A fallback", 4, 2);
            sentinel.verify();
            hw.retire(fg, prime);
            success(fg.shutdown(), "settled rejected-submit ownership");
            return;
        }
        success(pairResult, "BGRA pair capture");
        hw.await(fg, pair);
        if (reset) {
            check(pair.ticket.prime, "successful reset makes the next ticket a genuine prime");
            probe.begin();
            success(fg.copyOutput(pair, FrameGenerationRole::real, output.Get()), "new owner after reset");
            probe.armed = false;
            check(hw.read(output.Get()) == b, "reset reuse preserves new B bytes");
            probe.verify("completed new owner after reset", 1, 0);
            hw.retire(fg, pair);
            success(fg.shutdown(), "shutdown before host reinitialization");
            success(fg.initialize(init), "reinitialize independent input-owner generation");
            check(fg.copyOutput(stale, FrameGenerationRole::real, output.Get()) == VDXRFG_E_REJECTED,
                  "retired prior host generation cannot reuse the new input atlas");
            input.serial = 1;
            OwnedFrameTransaction fresh;
            success(fg.capture(input, fresh), "capture first ticket after reinitialization");
            hw.await(fg, fresh);
            probe.begin();
            success(fg.copyOutput(fresh, FrameGenerationRole::real, output.Get()), "fresh owner after reinit");
            probe.armed = false;
            check(hw.read(output.Get()) == b, "new host input ownership preserves canonical bytes");
            probe.verify("completed owner after host reinit", 1, 0);
            hw.retire(fg, fresh);
            success(fg.shutdown(), "settled reinitialized host");
            return;
        }
        check(!pair.ticket.prime && pair.slots.count == 3, "BGRA pair produces genuine three-role generation");
        auto s1 = outputTexture(), s2 = outputTexture(), real = outputTexture();
        success(fg.copyOutput(pair, FrameGenerationRole::synthetic1, s1.Get()), "BGRA S1 output");
        success(fg.copyOutput(pair, FrameGenerationRole::synthetic2, s2.Get()), "BGRA S2 output");
        if (twoLive)
            probe.begin();
        success(fg.copyOutput(pair, FrameGenerationRole::real, real.Get()), "BGRA B output");
        probe.armed = false;
        sentinel.verify();
        check(hw.read(real.Get()) == b, "current BGRA B stays exact after both synthetic copies");
        if (twoLive)
            probe.verify("newer live B reuses current input", 1, 0);
        const auto first = hw.read(s1.Get()), second = hw.read(s2.Get());
        check(first.size() == a.size() && second.size() == a.size(), "both GPU role images cover both eyes");
        if (motion)
            check(first != a && first != b && second != a && second != b && first != second,
                  "known moving scene produces two intermediates distinct from both endpoints and each other");
        if (twoLive) {
            probe.begin();
            success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "older live bank real copy");
            probe.armed = false;
            check(hw.read(output.Get()) == a, "older retained bank survives newer BGRA scratch writes");
            probe.verify("older live A retains original-source fallback", 4, 2);
            check(hw.read(real.Get()) == b, "newer prepared real image survives older bank conversion");
            hw.retire(fg, prime);
        }
        hw.retire(fg, pair);
        success(fg.shutdown(), "shutdown BGRA host");
        hw.gpu.noErrors();
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "module and BGRA mode required");
        runBgra(argv[1], argv[2]);
        std::puts("PASS: exact color handoff, real-frame preservation and actual three-role GPU output");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
