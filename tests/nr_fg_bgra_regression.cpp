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
        inline static CaptureProbe* active{};
        CopyFn original{};
        RegionFn originalRegion{};
        ComPtr<ID3D11Texture2D> packed;
        ID3D11DeviceContext* context{};
        bool armed{};
        static void observe(ID3D11DeviceContext* ctx, ID3D11Resource* dst) {
            if (active->armed && ctx == active->context) {
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
        explicit CaptureProbe(ID3D11DeviceContext* ctx) : context(ctx) {
            check(!active, "one capture probe");
            active = this;
            original = reinterpret_cast<CopyFn>((*reinterpret_cast<void***>(ctx))[47]);
            originalRegion = reinterpret_cast<RegionFn>((*reinterpret_cast<void***>(ctx))[46]);
            success(HRESULT_FROM_WIN32(DetourTransactionBegin()), "copy probe begin");
            success(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())), "copy probe thread");
            success(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&original), copy)), "copy probe attach");
            success(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&originalRegion), region)),
                    "region probe attach");
            success(HRESULT_FROM_WIN32(DetourTransactionCommit()), "copy probe commit");
        }
        ~CaptureProbe() {
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), copy) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalRegion), region) || DetourTransactionCommit())
                std::_Exit(2);
            active = nullptr;
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
        check(mode == "typeless" || mode == "unorm" || srgb || motion || rgba || separate || mixed || invalid ||
                  unsupported || mode == "array-mip" || mode == "source-reuse" || twoLive,
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
        CaptureProbe probe(hw.context.Get());
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
        if (!twoLive) {
            success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "BGRA real copy");
            sentinel.verify();
            check(hw.read(output.Get()) == a, "independent real B preserves every canonical RGBA byte");
        }
        if (mode == "source-reuse") {
            auto replacement = source(hw, format, false, b);
            hw.context->CopyResource(input.colour[0].Get(), replacement.Get());
            success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "reuse already prepared B");
            check(hw.read(output.Get()) == a, "prepared real output survives source reuse");
        }
        if (!twoLive)
            hw.retire(fg, prime);
        input.colour[0] = input.colour[1] = source(hw, format, mode == "array-mip", b);
        for (unsigned eye = 0; eye < 2; ++eye)
            input.views[eye].rect = {16 + eye * eyeWidth, 8, eyeWidth, height};
        ++input.serial;
        const auto previous = input.time;
        input.time += input.period * 3;
        input.slots = cadence.plan(
            cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 1), previous, true);
        OwnedFrameTransaction pair;
        success(fg.capture(input, pair), "BGRA pair capture");
        hw.await(fg, pair);
        check(!pair.ticket.prime && pair.slots.count == 3, "BGRA pair produces genuine three-role generation");
        auto s1 = outputTexture(), s2 = outputTexture(), real = outputTexture();
        success(fg.copyOutput(pair, FrameGenerationRole::synthetic1, s1.Get()), "BGRA S1 output");
        success(fg.copyOutput(pair, FrameGenerationRole::synthetic2, s2.Get()), "BGRA S2 output");
        success(fg.copyOutput(pair, FrameGenerationRole::real, real.Get()), "BGRA B output");
        sentinel.verify();
        check(hw.read(real.Get()) == b, "current BGRA B stays exact after both synthetic copies");
        const auto first = hw.read(s1.Get()), second = hw.read(s2.Get());
        check(first.size() == a.size() && second.size() == a.size(), "both GPU role images cover both eyes");
        if (motion)
            check(first != a && first != b && second != a && second != b && first != second,
                  "known moving scene produces two intermediates distinct from both endpoints and each other");
        if (twoLive) {
            success(fg.copyOutput(prime, FrameGenerationRole::real, output.Get()), "older live bank real copy");
            check(hw.read(output.Get()) == a, "older retained bank survives newer BGRA scratch writes");
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
