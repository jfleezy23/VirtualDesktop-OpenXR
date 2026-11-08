// Actual host shader/interop transport test. The observer DLL never synthesizes frames.
#include <d3d11_4.h>
#define main retainedInteropFixtureMain
#include "nr_fg_integration_regression.cpp"
#undef main
#include <algorithm>
#include <limits>

namespace {
    struct ComputeState {
        ID3D11DeviceContext4* context;
        ComPtr<ID3D11Texture2D> image, storage;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11Buffer> constants;
        explicit ComputeState(Interop& hw) : context(hw.context.Get()), image(hw.texture()) {
            success(hw.device->CreateShaderResourceView(image.Get(), nullptr, &srv), "sentinel SRV");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 16;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            success(hw.device->CreateTexture2D(&desc, nullptr, &storage), "sentinel UAV image");
            success(hw.device->CreateUnorderedAccessView(storage.Get(), nullptr, &uav), "sentinel UAV");
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 16;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            success(hw.device->CreateBuffer(&buffer, nullptr, &constants), "sentinel constant buffer");
            context->CSSetShader(nullptr, nullptr, 0);
            context->CSSetShaderResources(0, 1, srv.GetAddressOf());
            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        }
        void verify() const {
            ComPtr<ID3D11ComputeShader> shader;
            ComPtr<ID3D11ShaderResourceView> actualSrv;
            ComPtr<ID3D11UnorderedAccessView> actualUav;
            ComPtr<ID3D11Buffer> actualConstants;
            context->CSGetShader(&shader, nullptr, nullptr);
            context->CSGetShaderResources(0, 1, &actualSrv);
            context->CSGetUnorderedAccessViews(0, 1, &actualUav);
            context->CSGetConstantBuffers(0, 1, &actualConstants);
            check(!shader && actualSrv == srv && actualUav == uav && actualConstants == constants,
                  "depth normalization restores caller compute bindings");
        }
        ~ComputeState() {
            context->ClearState();
        }
    };

    struct DepthImage {
        FrameGenerationDepthSource metadata;
        std::vector<float> raw;
    };

    DepthImage depthImage(Interop& hw, DXGI_FORMAT format, unsigned eye, unsigned seed, bool direct) {
        DepthImage image;
        auto& metadata = image.metadata;
        auto& desc = metadata.description;
        desc.Width = 96;
        desc.Height = 80;
        desc.ArraySize = desc.MipLevels = 2;
        desc.SampleDesc.Count = 1;
        desc.Format = format;
        const auto srvFormat = frameGenerationDepthSrvFormat(format);
        const bool d16 = srvFormat == DXGI_FORMAT_R16_UNORM;
        const bool d24 = srvFormat == DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        const bool d32s8 = srvFormat == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        const bool typed = format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                           format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        desc.BindFlags = typed ? D3D11_BIND_DEPTH_STENCIL : direct ? D3D11_BIND_SHADER_RESOURCE : 0;
        metadata.colourRect = {23, 17, 1024, 512};
        metadata.colourCrop = {151, 81, 512, 256};
        metadata.depthRect = {13, 11, 64, 48};
        metadata.arraySlice = 1;
        metadata.releasedIndex = seed;
        metadata.minDepth = 0;
        metadata.maxDepth = 1;
        metadata.nearZ = eye ? 40.f : .25f;
        metadata.farZ = eye ? .5f : 80.f; // Opposite ordering independently per eye.
        metadata.affineVerified = true;
        const UINT stride = d16 ? 2 : d32s8 ? 8 : 4;
        std::vector<std::vector<uint8_t>> storage(4);
        D3D11_SUBRESOURCE_DATA initial[4]{};
        image.raw.resize(size_t(desc.Width) * desc.Height);
        const float values[]{0,
                             1,
                             .25f,
                             .5f,
                             .75f,
                             std::numeric_limits<float>::quiet_NaN(),
                             -.5f,
                             std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity(),
                             .125f,
                             .875f};
        for (UINT slice = 0; slice < 2; ++slice) {
            for (UINT mip = 0; mip < 2; ++mip) {
                const UINT sub = D3D11CalcSubresource(mip, slice, 2);
                const UINT w = desc.Width >> mip, h = desc.Height >> mip;
                storage[sub].resize(size_t(w) * h * stride);
                initial[sub] = {storage[sub].data(), w * stride, 0};
                for (UINT y = 0; y < h; ++y)
                    for (UINT x = 0; x < w; ++x) {
                        float raw = slice == 1 && mip == 0 ? values[(x + 3 * y + eye * 5 + seed) % 11] : .375f;
                        auto* bytes = storage[sub].data() + (size_t(y) * w + x) * stride;
                        if (d16 || d24) {
                            if (!std::isfinite(raw) || raw < 0)
                                raw = .625f;
                            const uint32_t maximum = d16 ? 65535 : 16777215;
                            const auto code = static_cast<uint32_t>(std::llround(double(raw) * maximum));
                            const uint32_t packed = code | (d24 ? 0xa5000000u : 0);
                            std::memcpy(bytes, &packed, stride);
                            raw = static_cast<float>(double(code) / maximum);
                        } else {
                            std::memcpy(bytes, &raw, sizeof(raw));
                            if (d32s8) {
                                const uint32_t stencil = 0x000000a5;
                                std::memcpy(bytes + 4, &stencil, 4);
                            }
                        }
                        if (slice == 1 && mip == 0)
                            image.raw[size_t(y) * w + x] = raw;
                    }
            }
        }
        success(hw.device->CreateTexture2D(&desc, initial, &metadata.source), "depth source with mips and slices");
        return image;
    }

    // Both views share one original packed allocation and slice zero. Nonzero,
    // eye-distinct spatial patterns expose wrong-eye, wrong-origin and zero reads.
    std::array<DepthImage, 2> packedDepthImage(Interop& hw, bool direct) {
        std::array<DepthImage, 2> images;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 192;
        desc.Height = 80;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = direct ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
        desc.BindFlags = direct ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_DEPTH_STENCIL;
        std::vector<float> raw(size_t(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
            for (UINT x = 0; x < desc.Width; ++x) {
                const UINT eye = x / 96;
                raw[size_t(y) * desc.Width + x] = float(16 + eye * 128 + ((x % 96) + 3 * y) % 96) / 256.f;
            }
        D3D11_SUBRESOURCE_DATA initial{raw.data(), desc.Width * UINT(sizeof(float)), 0};
        ComPtr<ID3D11Texture2D> shared;
        success(hw.device->CreateTexture2D(&desc, &initial, &shared), "shared packed original depth slice zero");
        for (UINT eye = 0; eye < 2; ++eye) {
            auto& image = images[eye];
            auto& metadata = image.metadata;
            metadata.source = shared;
            metadata.description = desc;
            metadata.arraySlice = 0;
            metadata.colourRect = {23 + eye * 1024, 17, 1024, 512};
            metadata.colourCrop = {151 + eye * 1024, 81, 512, 256};
            metadata.depthRect = {13 + eye * 96, 11, 64, 48};
            metadata.minDepth = 0;
            metadata.maxDepth = 1;
            metadata.nearZ = std::numeric_limits<float>::infinity();
            metadata.farZ = .1f; // Same reversed/infinite encoding as the packed application route.
            metadata.affineVerified = true;
            image.raw = raw; // Independent oracle addresses the full packed source.
        }
        check(images[0].metadata.source == images[1].metadata.source && images[0].metadata.arraySlice == 0 &&
                  images[1].metadata.arraySlice == 0,
              "packed views share allocation and slice zero");
        return images;
    }

    // Independent scalar specification uses original color/depth rectangles, not host helpers.
    float expected(const DepthImage& image, UINT x, UINT y) {
        const auto& s = image.metadata;
        const double colorX = s.colourCrop.x + (double(x) + .5) * s.colourCrop.width / eyeWidth;
        const double colorY = s.colourCrop.y + (double(y) + .5) * s.colourCrop.height / height;
        const double edgeX = s.depthRect.x + (colorX - s.colourRect.x) * s.depthRect.width / s.colourRect.width;
        const double edgeY = s.depthRect.y + (colorY - s.colourRect.y) * s.depthRect.height / s.colourRect.height;
        // Centers nearest to the mapped center; exact half-way ties select the upper texel.
        const auto px = static_cast<UINT>(std::floor(edgeX));
        const auto py = static_cast<UINT>(std::floor(edgeY));
        const float raw = image.raw[size_t(py) * s.description.Width + px];
        if (!std::isfinite(raw) || raw < s.minDepth || raw > s.maxDepth)
            return -1;
        const double u = (double(raw) - s.minDepth) / (double(s.maxDepth) - s.minDepth);
        const double inverse = (1.0 - u) / s.nearZ + u / s.farZ;
        const bool infinity = (raw == s.minDepth && std::isinf(s.nearZ)) || (raw == s.maxDepth && std::isinf(s.farZ));
        if (infinity)
            return 0;
        // SM5 float operations flush denormal inputs/results; finite reciprocal
        // underflow must remain unavailable and cannot masquerade as true infinity.
        if (!std::isfinite(inverse) || inverse < std::numeric_limits<float>::min())
            return -1;
        return static_cast<float>(inverse);
    }

    void verifyGuide(const std::vector<uint8_t>& bytes, const std::array<DepthImage, 2>& images) {
        check(bytes.size() == size_t(width) * height * sizeof(float), "complete R32 guide readback");
        unsigned valid{}, invalid{}, infinity{};
        for (UINT eye = 0; eye < 2; ++eye)
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < eyeWidth; ++x) {
                    float actual{};
                    std::memcpy(&actual, bytes.data() + (size_t(y) * width + eye * eyeWidth + x) * 4, 4);
                    const float reference = expected(images[eye], x, y);
                    if (reference == -1 || reference == 0) {
                        if (actual != reference) {
                            std::fprintf(stderr,
                                         "guide mismatch eye=%u x=%u y=%u actual=%.9g expected=%.9g\n",
                                         eye,
                                         x,
                                         y,
                                         actual,
                                         reference);
                            check(false, "invalid/infinite guide sentinel is exact");
                        }
                        reference == -1 ? ++invalid : ++infinity;
                    } else {
                        // CPU double interpolation and shader float32/fused operations
                        // round differently. 8 float32 eps relative bounds those operations;
                        // no absolute tolerance can turn unavailable or infinity into valid.
                        const float tolerance = 8 * std::numeric_limits<float>::epsilon() * reference;
                        if (!std::isfinite(actual) || actual <= 0 || std::abs(actual - reference) > tolerance) {
                            std::fprintf(stderr,
                                         "guide mismatch eye=%u x=%u y=%u actual=%.9g expected=%.9g tol=%.9g\n",
                                         eye,
                                         x,
                                         y,
                                         actual,
                                         reference,
                                         tolerance);
                            check(false, "every canonical inverse-Z pixel matches scalar reference");
                        }
                        ++valid;
                    }
                }
        std::printf("pixels: valid=%u unavailable=%u infinity=%u\n", valid, invalid, infinity);
    }

    // Upload exact binary32 depth bits to mip zero of slice one. Other mips and
    // slices retain depthImage's nonzero .375 sentinel. A typeless D32-family
    // allocation without depth-stencil binding permits UpdateSubresource and
    // exercises the production compatible-snapshot path (not a fake GPU object).
    std::array<DepthImage, 2> narrowRangeImages(Interop& hw, const std::string& mode) {
        const auto fromBits = [](uint32_t bits) {
            float value{};
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };
        const bool boundary = mode == "range-normal-boundary";
        const bool wide = mode == "range-subnormal-wide";
        const bool weighted = mode == "range-subnormal-weight";
        const bool bitsContract = mode == "range-bits";
        const bool smallReciprocals = mode == "range-small-reciprocals";
        const bool smallProducts = mode == "range-small-products";
        const uint32_t minimum = boundary ? 0x007ffffeu : wide ? 0x00400000u : 0u;
        const uint32_t maximum = boundary                                                        ? 0x00800002u
                                 : wide                                                          ? 0x01800000u
                                 : weighted || bitsContract || smallReciprocals || smallProducts ? 0x3f800000u
                                                                                                 : 0x000116c2u;
        const uint32_t middle = smallReciprocals   ? 0x3f000000u
                                : smallProducts    ? 0x3e800000u
                                : boundary || wide ? 0x00800000u
                                : weighted         ? 0x000116c2u
                                                   : 0x00008b61u;
        std::array<DepthImage, 2> images;
        for (unsigned eye = 0; eye < 2; ++eye) {
            images[eye] = depthImage(hw, DXGI_FORMAT_R32_TYPELESS, eye, eye + 1, false);
            auto& image = images[eye];
            auto& s = image.metadata;
            s.minDepth = fromBits(minimum);
            s.maxDepth = fromBits(maximum);
            s.nearZ = eye ? .1f : std::numeric_limits<float>::infinity();
            s.farZ = eye ? std::numeric_limits<float>::infinity() : .1f;
            if (weighted) {
                s.nearZ = eye ? 1e-38f : std::numeric_limits<float>::infinity();
                s.farZ = eye ? std::numeric_limits<float>::infinity() : 1e-38f;
            }
            if (smallReciprocals || smallProducts) {
                s.nearZ = std::ldexp(1.f, smallReciprocals ? 127 : 126);
                s.farZ = std::ldexp(1.f, 125);
            }
            check(frameGenerationDepthMetadataValid(s), "narrow raw range admitted by production metadata contract");
            check(frameGenerationDepthSourceValid(hw.device.Get(), s),
                  "narrow raw range source admitted by actual device contract");
            std::vector<uint32_t> bits(image.raw.size());
            for (UINT y = 0; y < s.description.Height; ++y)
                for (UINT x = 0; x < s.description.Width; ++x) {
                    const uint32_t choices[]{minimum, middle, maximum};
                    const uint32_t special[]{0u, 0x80000000u, 0x80000001u, 0x3f800001u, 0x7fc00001u, 0x7f800000u};
                    const uint32_t value = bitsContract                    ? special[(x + y) % 6]
                                           : boundary                      ? choices[(x + y) % 3]
                                           : mode == "range-subnormal-min" ? minimum
                                           : mode == "range-subnormal-max" ? maximum
                                                                           : middle;
                    const size_t index = size_t(y) * s.description.Width + x;
                    bits[index] = value;
                    std::memcpy(&image.raw[index], &bits[index], sizeof(float));
                }
            hw.context->UpdateSubresource(s.source.Get(),
                                          D3D11CalcSubresource(0, s.arraySlice, s.description.MipLevels),
                                          nullptr,
                                          bits.data(),
                                          s.description.Width * UINT(sizeof(uint32_t)),
                                          0);
            std::printf("ADMITTED %s eye=%u minBits=%08x maxBits=%08x near=%.9g far=%.9g slice=1 mip=0\n",
                        mode.c_str(),
                        eye,
                        minimum,
                        maximum,
                        s.nearZ,
                        s.farZ);
        }
        return images;
    }

    void runGuide(const char* modulePath, const std::string& mode) {
        Interop hw;
        const auto path = std::filesystem::absolute(modulePath);
        HMODULE module = LoadLibraryW(path.c_str());
        check(module != nullptr, "load test-only capture observer");
        struct Library {
            HMODULE value;
            ~Library() {
                FreeLibrary(value);
            }
        } library{module};
        const auto snapshot = reinterpret_cast<HRESULT(WINAPI*)(unsigned, ID3D12Resource**)>(
            GetProcAddress(module, "FG_DEPTH_TEST_GetSnapshot"));
        const auto lastSubmit = reinterpret_cast<HRESULT(WINAPI*)(VDXRFG_SubmitInfoV5*)>(
            GetProcAddress(module, "FG_DEPTH_TEST_LastSubmit"));
        check(snapshot && lastSubmit, "observer exports available");
        FrameGeneration fg;
        FrameGeneration::InitInfo init{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       path,
                                       eyeWidth,
                                       height,
                                       3,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        init.enableDepthGuidance = true;
        success(fg.initialize(init), "initialize actual host canonical-depth producer");
        check(fg.depthGuidanceEnabled(), "host negotiated V5 guide consumer");
        FinalProjectionInput input;
        auto color = scene(0), replacementColor = scene(17);
        input.colour[0] = input.colour[1] = hw.texture(&color);
        input.serial = 0;
        input.time = 1000000000;
        input.period = 10000000;
        input.slots.count = 1; // Observer publishes owned prime work, never fake synthesis.
        input.slots.roles[0] = FrameGenerationRole::real;
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& view = input.views[eye];
            view.rect = {eye * eyeWidth, 0, eyeWidth, height};
            view.orientation[3] = 1;
            view.position[0] = eye ? .032f : -.032f;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
        }
        ComputeState state(hw);
        uint64_t priorEpoch{};
        const auto capture = [&](const char* label,
                                 const std::array<DepthImage, 2>& images,
                                 bool available,
                                 int epochRelation,
                                 bool overwrite,
                                 bool legacyCapture = false) {
            for (unsigned eye = 0; eye < 2; ++eye)
                input.depth[eye] = images[eye].metadata;
            ++input.serial;
            input.time += 3 * input.period;
            input.slots.targets[0] = input.time;
            OwnedFrameTransaction tx;
            FrameGeneration::CaptureDiagnostics diagnostics{};
            success(legacyCapture ? fg.capture(input, tx) : fg.capture(input, tx, &diagnostics),
                    "host capture reaches test observer");
            state.verify();
            check(tx.ticket.prime && tx.ticket.role_mask == 0, "test observer claims no synthesized output");
            hw.await(fg, tx);
            hw.gpu.wait(static_cast<ID3D12Fence*>(tx.ticket.input_consumed_fence), tx.ticket.input_consumed_value);
            VDXRFG_SubmitInfoV5 submitted{};
            success(lastSubmit(&submitted), "observe actual V5 submission");
            if (!legacyCapture) {
                check(diagnostics.submitCalled && diagnostics.submitResult == S_OK &&
                          diagnostics.apiVersion == submitted.api_version &&
                          diagnostics.guideMask == submitted.guide_mask &&
                          diagnostics.guideSerial == submitted.guide_serial &&
                          diagnostics.guideEpoch == submitted.guide_epoch &&
                          diagnostics.guideEncoding == submitted.guide_encoding &&
                          diagnostics.previousSerial == tx.ticket.previous_serial &&
                          diagnostics.currentSerial == tx.ticket.current_serial &&
                          diagnostics.ticketToken == tx.ticket.token,
                      "capture diagnostics match independently received V5 packet and Submit result");
            }
            check(submitted.base.serial == input.serial && submitted.base.views[0].rect.x == 0 &&
                      submitted.base.views[1].rect.x == eyeWidth,
                  "guide paired with normalized color projection and serial");
            ComPtr<ID3D12Resource> guide, capturedColor;
            if (available) {
                check(submitted.guide_mask == 3 && submitted.guide_serial == input.serial && submitted.guide_epoch &&
                          submitted.guide_encoding == VDXRFG_GUIDE_ENCODING_INVERSE_Z,
                      "valid guide publishes complete canonical envelope");
                if (priorEpoch)
                    check(epochRelation == 0 ? submitted.guide_epoch == priorEpoch : submitted.guide_epoch > priorEpoch,
                          "guide epoch changes only with projection/depth semantics");
                priorEpoch = submitted.guide_epoch;
                success(snapshot(0, &guide), "private canonical guide snapshot");
                const auto desc = guide->GetDesc();
                check(desc.Format == DXGI_FORMAT_R32_FLOAT && desc.Width == width && desc.Height == height &&
                          desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1,
                      "actual guide is canonical single-plane R32_FLOAT");
            } else {
                check(!submitted.guide_mask && !submitted.guide_serial && !submitted.guide_epoch &&
                          !submitted.guide_encoding,
                      "partial or unsupported guide submits a zeroed envelope");
                check(snapshot(0, &guide) == VDXRFG_E_REJECTED && !guide,
                      "guide gap cannot expose previous private guide through observer");
            }
            success(snapshot(1, &capturedColor), "private color snapshot");
            if (overwrite) {
                // This proves private-copy isolation only after real GPU consumed
                // completion. Runtime pre-consumption admission is tested separately.
                auto newColor = hw.texture(&replacementColor);
                hw.context->CopyResource(input.colour[0].Get(), newColor.Get());
                for (unsigned eye = 0; eye < 2; ++eye) {
                    auto changed = depthImage(hw, images[eye].metadata.description.Format, eye, 9, true);
                    hw.context->CopyResource(input.depth[eye].source.Get(), changed.metadata.source.Get());
                }
                hw.context->Flush();
            }
            check(hw.gpu.transfer(capturedColor.Get(), nullptr) == color, "observer captured current color exactly");
            if (available)
                verifyGuide(hw.gpu.transfer(guide.Get(), nullptr), images);
            if (!legacyCapture) {
                // Reuse a populated receipt to detect stale diagnostic data on an
                // early host rejection. An occupied transaction never calls Submit.
                check(fg.capture(input, tx, &diagnostics) == VDXRFG_E_REJECTED,
                      "occupied transaction rejects before Submit");
                const FrameGeneration::CaptureDiagnostics empty{};
                check(!diagnostics.submitCalled && diagnostics.apiVersion == empty.apiVersion &&
                          diagnostics.guideMask == empty.guideMask && diagnostics.guideSerial == empty.guideSerial &&
                          diagnostics.guideEpoch == empty.guideEpoch &&
                          diagnostics.guideEncoding == empty.guideEncoding &&
                          diagnostics.previousSerial == empty.previousSerial &&
                          diagnostics.currentSerial == empty.currentSerial &&
                          diagnostics.ticketToken == empty.ticketToken &&
                          diagnostics.submitResult == empty.submitResult,
                      "early rejection clears every field of a previously populated diagnostic receipt");
                VDXRFG_SubmitInfoV5 unchanged{};
                success(lastSubmit(&unchanged), "observer retains original packet after early rejection");
                check(unchanged.base.serial == submitted.base.serial && unchanged.guide_mask == submitted.guide_mask &&
                          unchanged.guide_serial == submitted.guide_serial &&
                          unchanged.guide_epoch == submitted.guide_epoch,
                      "early rejection leaves observer packet unchanged");
            }
            hw.retire(fg, tx);
            if (overwrite)
                hw.context->UpdateSubresource(input.colour[0].Get(), 0, nullptr, color.data(), width * 4, 0);
            std::printf("PASS host guide: %s serial=%llu epoch=%llu mask=%u\n",
                        label,
                        static_cast<unsigned long long>(input.serial),
                        static_cast<unsigned long long>(submitted.guide_epoch),
                        submitted.guide_mask);
        };
        if (!mode.empty()) {
            const auto images = narrowRangeImages(hw, mode);
            capture(mode.c_str(), images, true, 1, false);
            state.verify();
            success(fg.shutdown(), "settled narrow-range host guide fixture shutdown");
            hw.gpu.noErrors();
            return;
        }
        const DXGI_FORMAT formats[]{DXGI_FORMAT_R16_TYPELESS,
                                    DXGI_FORMAT_D16_UNORM,
                                    DXGI_FORMAT_R24G8_TYPELESS,
                                    DXGI_FORMAT_D24_UNORM_S8_UINT,
                                    DXGI_FORMAT_R32_TYPELESS,
                                    DXGI_FORMAT_D32_FLOAT,
                                    DXGI_FORMAT_R32G8X24_TYPELESS,
                                    DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
        for (auto format : formats) {
            std::array<DepthImage, 2> images{depthImage(hw, format, 0, 1, true), depthImage(hw, format, 1, 2, true)};
            char label[80];
            sprintf_s(label, "typed/typeless format %u slice=1 mip=0 affine crop", unsigned(format));
            capture(label, images, true, 1, false);
        }
        capture("shared packed slice-zero typed depth snapshot with offset crop",
                packedDepthImage(hw, false),
                true,
                1,
                false);
        capture("shared packed slice-zero direct typeless depth with offset crop",
                packedDepthImage(hw, true),
                true,
                1,
                false);
        std::array<DepthImage, 2> images{depthImage(hw, DXGI_FORMAT_R32_TYPELESS, 0, 3, false),
                                         depthImage(hw, DXGI_FORMAT_R32_TYPELESS, 1, 4, false)};
        capture("source without SRV binding uses compatible snapshot", images, true, 1, false);
        auto replaced = std::array<DepthImage, 2>{depthImage(hw, DXGI_FORMAT_R32_TYPELESS, 0, 7, false),
                                                  depthImage(hw, DXGI_FORMAT_R32_TYPELESS, 1, 8, false)};
        capture("source allocation replacement preserves semantic epoch", replaced, true, 0, true);
        auto ties = images;
        for (auto& image : ties) {
            image.metadata.colourRect = {23, 17, 1101, 567};
            image.metadata.colourCrop = {23, 17, 1024, 512};
            image.metadata.depthRect = {13, 11, 63, 63};
        }
        // At output (183,31), original-color center (390,80) maps exactly to
        // depth edge (34,18), a nearest-center tie in both axes. Pixel (34,18)
        // must win over (33,17); the per-pixel pattern makes these distinguishable.
        capture("exact point-sampling ties select upper texel", ties, true, 1, false);
        images[0].metadata.nearZ = std::numeric_limits<float>::infinity();
        images[1].metadata.farZ = std::numeric_limits<float>::infinity();
        capture("infinite near/far with exact infinity zero", images, true, 1, false);
        for (auto& image : images) {
            image.metadata.minDepth = .25f;
            image.metadata.maxDepth = .75f;
        }
        capture("nondefault raw range and invalid raw sentinels", images, true, 1, false);
        for (auto& image : images) {
            image.metadata.nearZ = std::numeric_limits<float>::max();
            image.metadata.farZ = std::numeric_limits<float>::max() / 2;
        }
        capture("finite reciprocal denormal remains unavailable", images, true, 1, false);
        auto partial = images;
        partial[1].metadata.source.Reset();
        capture("partial eye guide gap", partial, false, 0, false);
        capture("guide return after gap still uses new submission pixels", images, true, 0, false);
        auto unsupported = images;
        unsupported[1].metadata.description.Format = DXGI_FORMAT_R8_UNORM;
        capture("unsupported depth format metadata", unsupported, false, 0, false);
        auto unverified = images;
        unverified[0].metadata.affineVerified = false;
        capture("unverified affine crop", unverified, false, 0, false);
        capture("two-argument capture retains depth packet and pixels", images, true, 0, false, true);
        state.verify();
        success(fg.shutdown(), "settled host guide fixture shutdown");
        hw.gpu.noErrors();
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2 || argc == 3, "test capture module path and optional narrow-range mode required");
        const std::string mode = argc == 3 ? argv[2] : "";
        check(mode.empty() || mode == "range-subnormal-min" || mode == "range-subnormal-max" ||
                  mode == "range-subnormal-mid" || mode == "range-normal-boundary" || mode == "range-subnormal-wide" ||
                  mode == "range-subnormal-weight" || mode == "range-bits" || mode == "range-small-reciprocals" ||
                  mode == "range-small-products",
              "unknown narrow-range mode");
        runGuide(argv[1], mode);
        std::puts("PASS: actual host GPU depth normalization and transport; no synthesis claim");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
