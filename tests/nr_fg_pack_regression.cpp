// Native packed-eye fast paths must preserve exact pixels and all rejection checks.
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main
#include <detours.h>

namespace {
    struct PackProbe {
        using DeviceFn = void(STDMETHODCALLTYPE*)(ID3D11Texture2D*, ID3D11Device**);
        using DescFn = void(STDMETHODCALLTYPE*)(ID3D11Texture2D*, D3D11_TEXTURE2D_DESC*);
        using QueryFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
        using CopyFn = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
        inline static PackProbe* active{};
        DeviceFn originalDevice{};
        DescFn originalDesc{};
        CopyFn originalCopy{};
        QueryFn originalQuery{};
        std::array<ComPtr<ID3D11Texture2D>, 2> sources;
        ComPtr<ID3D11Resource> copiedDestination;
        ID3D11DeviceContext* context{};
        ID3D11Device* expectedDevice{};
        DWORD thread = GetCurrentThreadId();
        unsigned devices{}, descriptions{}, copies{}, queries{};
        bool armed{};

        bool watches(ID3D11Resource* source) const {
            return GetCurrentThreadId() == thread && armed &&
                   (source == sources[0].Get() || source == sources[1].Get());
        }
        static void STDMETHODCALLTYPE device(ID3D11Texture2D* source, ID3D11Device** result) {
            if (active->watches(source))
                ++active->devices;
            active->originalDevice(source, result);
        }
        static void STDMETHODCALLTYPE description(ID3D11Texture2D* source, D3D11_TEXTURE2D_DESC* result) {
            if (active->watches(source))
                ++active->descriptions;
            active->originalDesc(source, result);
        }
        static HRESULT STDMETHODCALLTYPE query(IUnknown* object, REFIID id, void** result) {
            if (GetCurrentThreadId() == active->thread && active->armed && object == active->expectedDevice &&
                id == __uuidof(IUnknown))
                ++active->queries;
            return active->originalQuery(object, id, result);
        }
        static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* context,
                                           ID3D11Resource* destination,
                                           UINT subresource,
                                           UINT x,
                                           UINT y,
                                           UINT z,
                                           ID3D11Resource* source,
                                           UINT sourceSubresource,
                                           const D3D11_BOX* box) {
            if (context == active->context && active->watches(source)) {
                ++active->copies;
                active->copiedDestination = destination;
            }
            active->originalCopy(context, destination, subresource, x, y, z, source, sourceSubresource, box);
        }
        void attach(bool enable) {
            const auto operation = enable ? DetourAttach : DetourDetach;
            LONG result = DetourTransactionBegin();
            const bool started = result == NO_ERROR;
            if (result == NO_ERROR)
                result = DetourUpdateThread(GetCurrentThread());
            if (result == NO_ERROR)
                result = operation(reinterpret_cast<PVOID*>(&originalDevice), device);
            if (result == NO_ERROR)
                result = operation(reinterpret_cast<PVOID*>(&originalDesc), description);
            if (result == NO_ERROR)
                result = operation(reinterpret_cast<PVOID*>(&originalCopy), copy);
            if (result == NO_ERROR)
                result = operation(reinterpret_cast<PVOID*>(&originalQuery), query);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else if (started)
                DetourTransactionAbort();
            success(HRESULT_FROM_WIN32(result), "pack probe transaction");
        }
        PackProbe(const FinalProjectionInput& input, ID3D11DeviceContext* immediate, ID3D11Device* device)
            : sources(input.colour), context(immediate), expectedDevice(device) {
            check(!active, "one native pack probe at a time");
            originalDevice = reinterpret_cast<DeviceFn>((*reinterpret_cast<void***>(sources[0].Get()))[3]);
            originalDesc = reinterpret_cast<DescFn>((*reinterpret_cast<void***>(sources[0].Get()))[10]);
            originalCopy = reinterpret_cast<CopyFn>((*reinterpret_cast<void***>(context))[46]);
            originalQuery = reinterpret_cast<QueryFn>((*reinterpret_cast<void***>(expectedDevice))[0]);
            active = this;
            try {
                attach(true);
            } catch (...) {
                active = nullptr;
                throw;
            }
        }
        ~PackProbe() {
            armed = false;
            try {
                attach(false);
            } catch (...) {
                std::_Exit(2);
            }
            active = nullptr;
        }
        void clear() {
            devices = descriptions = copies = queries = 0;
            copiedDestination.Reset();
        }
    };

    ComPtr<ID3D11Texture2D> sourceTexture(
        ID3D11Device* device, UINT w, UINT h, UINT mips, UINT arrays, unsigned seed, std::vector<uint8_t>& top) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = mips;
        desc.ArraySize = arrays;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<std::vector<uint8_t>> storage(mips * arrays);
        std::vector<D3D11_SUBRESOURCE_DATA> data(mips * arrays);
        for (UINT array = 0; array < arrays; ++array) {
            for (UINT mip = 0; mip < mips; ++mip) {
                const auto index = D3D11CalcSubresource(mip, array, mips);
                const UINT mw = std::max(1u, w >> mip), mh = std::max(1u, h >> mip);
                auto& pixels = storage[index];
                pixels.resize(mw * mh * 4);
                for (UINT y = 0; y < mh; ++y) {
                    for (UINT x = 0; x < mw; ++x) {
                        const size_t i = (size_t(y) * mw + x) * 4;
                        pixels[i] = uint8_t(seed + x * 13 + array * 31);
                        pixels[i + 1] = uint8_t(seed + y * 7 + mip * 43);
                        pixels[i + 2] = uint8_t(x / 7 + y / 11 + seed);
                        pixels[i + 3] = 255;
                    }
                }
                data[index] = {pixels.data(), mw * 4, mw * mh * 4};
            }
        }
        top = storage[0];
        ComPtr<ID3D11Texture2D> texture;
        success(device->CreateTexture2D(&desc, data.data(), &texture), "native source layout");
        return texture;
    }

    void runPack(const char* module, const std::string& scenario) {
        const bool separate = scenario == "separate" || scenario == "wrong-device";
        const bool offset = scenario == "offset";
        const bool gap = scenario == "gap", overlap = scenario == "overlap", reverse = scenario == "reverse";
        const bool differentY = scenario == "different-y", arrayMip = scenario == "array-mip";
        const bool invalid = scenario == "invalid-rect" || scenario == "invalid-pose" || scenario == "wrong-device";
        check(scenario == "packed" || separate || offset || gap || overlap || reverse || differentY || arrayMip ||
                  invalid,
              "known pack scenario");
        Interop hw;
        FrameGeneration fg;
        FrameGeneration::InitInfo init{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       std::filesystem::absolute(module),
                                       eyeWidth,
                                       height,
                                       3,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        success(fg.initialize(init), "pack host");
        const UINT sourceWidth = width + 64, sourceHeight = height + 32;
        FinalProjectionInput input;
        std::array<std::vector<uint8_t>, 2> sourcePixels;
        input.colour[0] = sourceTexture(
            hw.device.Get(), sourceWidth, sourceHeight, arrayMip ? 2 : 1, arrayMip ? 2 : 1, 19, sourcePixels[0]);
        std::unique_ptr<Interop> other;
        if (scenario == "wrong-device")
            other = std::make_unique<Interop>();
        input.colour[1] = separate ? sourceTexture(other ? other->device.Get() : hw.device.Get(),
                                                   sourceWidth,
                                                   sourceHeight,
                                                   1,
                                                   1,
                                                   97,
                                                   sourcePixels[1])
                                   : input.colour[0];
        if (!separate)
            sourcePixels[1] = sourcePixels[0];
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& view = input.views[eye];
            view.rect = {separate ? 0 : eye * eyeWidth, 0, eyeWidth, height};
            if (offset) {
                view.rect.x += 16;
                view.rect.y = 8;
            }
            if (gap && eye)
                view.rect.x += 16;
            if (overlap && eye)
                view.rect.x -= 16;
            if (reverse)
                view.rect.x = (1 - eye) * eyeWidth;
            if (differentY && eye)
                view.rect.y = 8;
            view.orientation[3] = 1.f;
            view.position[0] = eye ? .032f : -.032f;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
        }
        if (scenario == "invalid-rect")
            input.views[1].rect.x = UINT32_MAX;
        if (scenario == "invalid-pose")
            input.views[1].orientation[3] = 2.f;
        input.serial = 1;
        input.time = 1000000000;
        input.period = 10000000;
        FrameGenerationCadence cadence;
        input.slots =
            cadence.plan(cadence.reserve(input.time - 30000000, input.time - 20000000, input.period, 3, 0), 0, false);
        PackProbe probe(input, hw.context.Get(), hw.device.Get());
        // Verify both actual COM dispatches and copies are observable before measuring production.
        auto output = hw.texture();
        probe.armed = true;
        ComPtr<IUnknown> identity;
        success(hw.device->QueryInterface(IID_PPV_ARGS(&identity)), "identity positive control");
        for (const auto& texture : input.colour) {
            D3D11_TEXTURE2D_DESC desc{};
            ComPtr<ID3D11Device> owner;
            texture->GetDesc(&desc);
            texture->GetDevice(&owner);
            const D3D11_BOX box{0, 0, 0, 1, 1, 1};
            hw.context->CopySubresourceRegion(
                output.Get(), 0, 0, 0, 0, other ? input.colour[0].Get() : texture.Get(), 0, &box);
        }
        probe.armed = false;
        // A resource from another device is never copied in the positive control.
        check(probe.devices == 2 && probe.descriptions == 2 && probe.copies == 2 && probe.queries >= 1,
              "native probe positive controls");
        probe.clear();
        probe.armed = true;
        OwnedFrameTransaction transaction;
        const auto result = fg.capture(input, transaction);
        probe.armed = false;
        const auto queried = probe.devices, described = probe.descriptions, captured = probe.copies;
        const auto captureQueries = probe.queries;
        if (invalid) {
            check(result == VDXRFG_E_REJECTED && !transaction.ticket.token && !captured,
                  "right-eye metadata and foreign-device rejection precede all copies");
            success(fg.shutdown(), "shutdown rejected pack");
            return;
        }
        success(result, "capture pack");
        ComPtr<ID3D11Texture2D> packedInput;
        success(probe.copiedDestination.As(&packedInput), "actual capture destination");
        hw.await(fg, transaction);
        const auto capturedPixels = hw.read(packedInput.Get());
        probe.clear();
        probe.armed = true;
        success(fg.copyOutput(transaction, FrameGenerationRole::real, output.Get()), "copy real packed output");
        probe.armed = false;
        const auto presented = probe.copies;
        const auto presentationQueries = probe.queries;
        const auto actual = hw.read(output.Get());
        std::vector<uint8_t> expected(width * height * 4);
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& rect = input.views[eye].rect;
            for (UINT y = 0; y < height; ++y)
                std::memcpy(expected.data() + (size_t(y) * width + eye * eyeWidth) * 4,
                            sourcePixels[eye].data() + (size_t(y + rect.y) * sourceWidth + rect.x) * 4,
                            eyeWidth * 4);
        }
        hw.retire(fg, transaction);
        success(fg.shutdown(), "shutdown packed host");
        hw.gpu.noErrors();
        const unsigned expectedCopies = separate || gap || overlap || reverse || differentY ? 2 : 1;
        std::printf("pack calls: device=%u desc=%u capture=%u real=%u expectedCopies=%u captureQI=%u realQI=%u\n",
                    queried,
                    described,
                    captured,
                    presented,
                    expectedCopies,
                    captureQueries,
                    presentationQueries);
        check(actual == expected, "packed/gapped/cropped/array-mip stereo pixels remain bit-exact");
        check(capturedPixels == expected, "actual FG input pack preserves both eye rectangles bit-exact");
        check(queried == (separate ? 2u : 1u) && described == (separate ? 2u : 1u), "query each distinct source once");
        check(captured == expectedCopies && presented == expectedCopies, "combine only adjacent packed-eye copies");
        check(captureQueries == 0 && presentationQueries == 0, "direct device identity needs no QueryInterface calls");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "module and pack scenario required");
        runPack(argv[1], argv[2]);
        std::puts("PASS: packed-eye API costs, exact GPU pixels and independent eye validation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
