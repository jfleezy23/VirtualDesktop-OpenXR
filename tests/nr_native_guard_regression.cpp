// Reuse the established D3D12 fixture; its original suite remains independently runnable.
#define main nr_spatial_fixture_main
#include "nr_spatial_shader_regression.cpp"
#undef main

void nativeLightEncodingContract(Gpu& gpu, bool bgra) {
    const auto render = [&](bool srgb, uint8_t sourceCode, uint8_t answerGreen) {
        const auto format = bgra ? (srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM)
                                 : (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
        const auto storage = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        auto source = gpu.texture(2, 1, storage);
        auto output = gpu.texture(2, 1, storage, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const Bytes pixels{sourceCode, sourceCode, sourceCode, 153, sourceCode, sourceCode, sourceCode, 153};
        const Bytes answer{sourceCode, answerGreen, sourceCode, 255, sourceCode, answerGreen, sourceCode, 255};
        gpu.transfer(source.Get(), &pixels);
        FrameInput frame{};
        frame.nativeAnswer = true;
        frame.output = output.Get();
        frame.outputFormat = format;
        frame.outputEyeStride = 1;
        for (UINT eye = 0; eye < 2; ++eye)
            frame.eyes[eye] = {source.Get(), format, {eye, 0, 1, 1}};
        Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
        hr(pipeline.Prepare(frame), "native matched-light prepare");
        gpu.transfer(pipeline.GetNativeAnswer(), &answer);
        auto commands = gpu.commands();
        pipeline.RecordResolve(commands.list.Get());
        const auto fence = gpu.submit(commands);
        pipeline.MarkSubmitted(fence);
        gpu.wait(fence);
        const auto result = gpu.transfer(output.Get());
        check(result[3] == 153 && result[7] == 153, "native matched-light source alpha");
        check(std::memcmp(result.data(), result.data() + 4, 4) == 0, "native matched-light packed eye equality");
        return std::array<uint8_t, 4>{result[0], result[1], result[2], result[3]};
    };
    for (const auto pair : {std::array<uint8_t, 4>{1, 9, 13, 53}, {2, 10, 22, 56}}) {
        // These are matched physical-light samples, deliberately different encoded bytes.
        // UNORM output quantization dominates the <=1/255 linear-light tolerance.
        const auto linear = render(false, pair[0], pair[1]);
        const auto srgb = render(true, pair[2], pair[3]);
        double maximumDifference{}, linearLuminance{}, srgbLuminance{};
        for (UINT c = 0; c < 3; ++c) {
            const double a = decode(linear[c] / 255.0, false);
            const double b = decode(srgb[c] / 255.0, true);
            maximumDifference = std::max(maximumDifference, std::abs(a - b));
            check(std::abs(a - b) <= 1.0 / 255.0,
                  "native matched physical-light encoding pair must agree within one linear UNORM code");
            const double weight = c == 1 ? 0.7152 : (c == (bgra ? 2u : 0u) ? 0.2126 : 0.0722);
            linearLuminance += weight * a;
            srgbLuminance += weight * b;
        }
        check(linearLuminance - decode(pair[0] / 255.0, false) > 0.005 &&
                  srgbLuminance - decode(pair[2] / 255.0, true) > 0.005,
              "native matched-light pair must retain nonzero NR scalar structure; identity is insufficient");
        if (pair[0] == 2) {
            check(linear[0] == pair[0] && linear[1] == pair[1] && linear[2] == pair[0] && srgb[0] == pair[2] &&
                      srgb[1] == pair[3] && srgb[2] == pair[2],
                  "native above-toe matched-light pair retains complete vendor answer");
        }
        std::printf("NATIVE_MATCHED_LIGHT bgra=%u sourceLinear=%u sourceSrgb=%u resultLinear=(%u,%u,%u) "
                    "resultSrgb=(%u,%u,%u) maxLinearDifference=%.9f\n",
                    bgra,
                    pair[0],
                    pair[2],
                    linear[0],
                    linear[1],
                    linear[2],
                    srgb[0],
                    srgb[1],
                    srgb[2],
                    maximumDifference);
    }
    gpu.noErrors();
}

void nativeNearBlackContract(Gpu& gpu, DXGI_FORMAT format) {
    // Full code ramp challenges quantized continuity, with named samples below.
    constexpr UINT eyeWidth = 65, width = eyeWidth * 2, height = 4;
    const bool bgra = RawColorFormat(format) == DXGI_FORMAT_B8G8R8A8_UNORM;
    const auto resourceFormat = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    auto source = gpu.texture(width, height, resourceFormat);
    auto output = gpu.texture(width, height, resourceFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    Bytes pixels(size_t(width) * height * 4), edited(pixels.size());
    for (UINT eye = 0; eye < 2; ++eye)
        for (UINT y = 0; y < height; ++y)
            for (UINT code = 0; code < eyeWidth; ++code) {
                const size_t i = (size_t(y) * width + eye * eyeWidth + code) * 4;
                auto* s = &pixels[i];
                auto* a = &edited[i];
                s[0] = s[1] = s[2] = uint8_t(code);
                if (y == 2) { // Legitimate dark source green, unchanged vendor answer.
                    s[0] = uint8_t(code / 4);
                    s[1] = uint8_t(code);
                    s[2] = uint8_t(code / 2);
                }
                s[3] = uint8_t(7 + code + eye * 70 + y * 9);
                std::memcpy(a, s, 4);
                a[3] = 255;
                if (y == 0)
                    a[1] = uint8_t(code + 8); // Chromatic model contamination.
                if (y == 1)
                    a[0] = a[1] = a[2] = uint8_t(code + 2); // Scalar structure edit.
                if (y == 3) {                               // Signed residual followed by a clipped vendor answer.
                    a[bgra ? 2 : 0] = uint8_t(code > 6 ? code - 6 : 0);
                    a[1] = uint8_t(code + 8);
                    a[bgra ? 0 : 2] = uint8_t(code > 3 ? code - 3 : 0);
                }
            }
    gpu.transfer(source.Get(), &pixels);
    FrameInput frame{};
    frame.nativeAnswer = true;
    frame.output = output.Get();
    frame.outputFormat = format;
    frame.outputEyeStride = eyeWidth;
    for (UINT eye = 0; eye < 2; ++eye)
        frame.eyes[eye] = {source.Get(), format, {eye * eyeWidth, 0, eyeWidth, height}};
    Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
    hr(pipeline.Prepare(frame), "native near-black prepare");
    gpu.transfer(pipeline.GetNativeAnswer(), &edited);
    auto commands = gpu.commands();
    pipeline.RecordResolve(commands.list.Get());
    const auto fence = gpu.submit(commands);
    pipeline.MarkSubmitted(fence);
    gpu.wait(fence);
    const auto result = gpu.transfer(output.Get());
    int maximumStep{}, maximumToeSpread{};
    for (UINT eye = 0; eye < 2; ++eye)
        for (UINT y = 0; y < height; ++y)
            for (UINT code = 0; code < eyeWidth; ++code) {
                const size_t i = (size_t(y) * width + eye * eyeWidth + code) * 4;
                const auto* s = &pixels[i];
                const auto* a = &edited[i];
                const auto* r = &result[i];
                check(r[3] == s[3], "native near-black original alpha");
                if (code == 0 || y == 2)
                    check(std::memcmp(r, s, 4) == 0, "native exact black and legitimate source-green identity");
                if (code >= 22)
                    check(std::memcmp(r, a, 3) == 0, "native full nonzero color edit retained above shadow range");
                if (y == 1 && code >= 4)
                    check(std::memcmp(r, a, 3) == 0, "native neutral structure retained outside narrow black toe");
                if (y == 0) {
                    const int spread = int(std::max({r[0], r[1], r[2]})) - int(std::min({r[0], r[1], r[2]}));
                    if (IsSrgb(format) && code >= 1 && code <= 4) {
                        maximumToeSpread = std::max(maximumToeSpread, spread);
                        check(spread <= 1, "native near-black sRGB chroma protection: codes1..4 spread must be<=1");
                    }
                    check(r[0] >= code && r[1] >= code && r[2] >= code && r[0] <= code + 8 && r[1] <= code + 8 &&
                              r[2] <= code + 8,
                          "native positive residual does not overshoot source/answer envelope");
                    if (code) {
                        const auto* previous = &result[i - 4];
                        for (UINT c = 0; c < 3; ++c) {
                            maximumStep = std::max(maximumStep, std::abs(int(r[c]) - int(previous[c])));
                            if (IsSrgb(format))
                                check(r[c] >= previous[c], "native positive sRGB shadow ramp remains monotone");
                            // Linear8 code1 already exceeds the narrow luminance toe.
                            // Its quantization can span the complete policy transition;
                            // do not claim the same encoded continuity as sRGB storage.
                            check(std::abs(int(r[c]) - int(previous[c])) <= (IsSrgb(format) ? 4 : 8),
                                  "native quantized shadow ramp respects encoding-specific adjacent bound");
                        }
                    }
                }
                if (y == 3 && IsSrgb(format) && code >= 1 && code <= 4) {
                    const int spread = int(std::max({r[0], r[1], r[2]})) - int(std::min({r[0], r[1], r[2]}));
                    const int originalSpread = int(std::max({a[0], a[1], a[2]})) - int(std::min({a[0], a[1], a[2]}));
                    check(spread < originalSpread,
                          "native signed clipped residual chroma is reduced in deepest nonzero shadows");
                }
            }
    for (UINT code : {1u, 2u, 4u, 8u, 16u, 22u, 32u}) {
        const auto* r = &result[size_t(code) * 4];
        std::printf("NATIVE_SHADOW format=%u source=%u G8answer=(%u,%u,%u) resolved=(%u,%u,%u)\n",
                    unsigned(format),
                    code,
                    code,
                    code + 8,
                    code,
                    r[0],
                    r[1],
                    r[2]);
    }
    std::printf("NATIVE_SHADOW_BOUNDS format=%u maxAdjacentStep=%d maxSrgbToeSpread=%d\n",
                unsigned(format),
                maximumStep,
                maximumToeSpread);
    gpu.noErrors();
}

void nativeContract(Gpu& gpu, DXGI_FORMAT sourceFormat, DXGI_FORMAT outputFormat, bool tiny) {
    const bool sourceBgra = RawColorFormat(sourceFormat) == DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool outputBgra = RawColorFormat(outputFormat) == DXGI_FORMAT_B8G8R8A8_UNORM;
    const UINT width = tiny ? 11 : 83, height = tiny ? 7 : 45, stride = width + 5;
    auto source = gpu.texture(
        width * 2 + 9, height + 8, sourceBgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS);
    auto output = gpu.texture(stride * 2,
                              height,
                              outputBgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const UINT sw = width * 2 + 9;
    Bytes pixels(size_t(sw) * (height + 8) * 4, 0);
    for (UINT y = 0; y < height + 8; ++y)
        for (UINT x = 0; x < sw; ++x) {
            auto* p = &pixels[(size_t(y) * sw + x) * 4];
            p[0] = uint8_t(64 + x % 80);
            p[1] = uint8_t(70 + y % 80);
            p[2] = 120;
            if (x % 7 == 0)
                p[0] = p[1] = p[2] = 0;
            p[3] = uint8_t((x + y * 3) % 256);
        }
    gpu.transfer(source.Get(), &pixels);
    FrameInput frame{};
    frame.nativeAnswer = true;
    frame.output = output.Get();
    frame.outputFormat = outputFormat;
    frame.outputEyeStride = stride;
    frame.processingPercent = 100;
    for (UINT e = 0; e < 2; ++e)
        frame.eyes[e] = {source.Get(),
                         sourceFormat,
                         {3 + e * width, 4, width - e, height - e},
                         nullptr,
                         DXGI_FORMAT_UNKNOWN,
                         {},
                         {1, 1, width - e - 2, height - e - 2},
                         0};
    Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
    hr(pipeline.Prepare(frame), "native direct prepare, including tiny extent");
    check(pipeline.GetWorkingEye(0).proxy == nullptr && pipeline.GetWorkingEye(0).answer == nullptr,
          "native mode must not allocate proxy/per-eye answers");
    auto* answer = pipeline.GetNativeAnswer();
    check(answer != nullptr && answer != frame.output, "native vendor answer is separate packed scratch");
    check(answer->GetDesc().Format == output->GetDesc().Format, "vendor output format contract retained");
    Bytes edited(size_t(stride * 2) * height * 4, 255); // Poison every unwritten vendor border.
    for (UINT e = 0; e < 2; ++e) {
        const auto& eye = frame.eyes[e];
        for (UINT y = 1; y < eye.colorRect.height - 1; ++y)
            for (UINT x = 1; x < eye.colorRect.width - 1; ++x) {
                auto* a = &edited[(size_t(y) * stride * 2 + e * stride + x) * 4];
                a[0] = uint8_t(160 + e * 20);
                a[1] = 100;
                a[2] = 150;
                a[3] = 255;
            }
    }
    gpu.transfer(answer, &edited);
    auto commands = gpu.commands();
#if defined(NR_SPATIAL_TEST_OBSERVER)
    namespace trace = virtualdesktop_openxr::nr_spatial::test_observer;
    trace::Reset();
#endif
    pipeline.RecordResolve(commands.list.Get());
#if defined(NR_SPATIAL_TEST_OBSERVER)
    size_t draws{}, dispatches{}, answerTransitions{};
    for (size_t i = 0; i < trace::count; ++i) {
        draws += trace::events[i].kind == trace::Kind::Draw;
        dispatches += trace::events[i].kind == trace::Kind::Dispatch;
        answerTransitions += trace::events[i].kind == trace::Kind::Transition && trace::events[i].resource == answer;
    }
    check(draws == 2 && dispatches == 0 && answerTransitions == 2, "native direct operation budget");
    trace::enabled = false;
#endif
    const auto fence = gpu.submit(commands);
    pipeline.MarkSubmitted(fence);
    gpu.wait(fence);
    const auto result = gpu.transfer(output.Get());
    for (UINT e = 0; e < 2; ++e) {
        const auto& eye = frame.eyes[e];
        for (UINT y = 0; y < eye.colorRect.height; ++y)
            for (UINT x = 0; x < eye.colorRect.width; ++x) {
                const auto* s = &pixels[(size_t(y + 4) * sw + 3 + e * width + x) * 4];
                const size_t destination = (size_t(y) * stride * 2 + e * stride + x) * 4;
                const auto* a = &edited[destination];
                const auto* r = &result[destination];
                const bool outer = x == 0 || y == 0 || x + 1 == eye.colorRect.width || y + 1 == eye.colorRect.height;
                const bool black = s[0] == 0 && s[1] == 0 && s[2] == 0;
                for (UINT c = 0; c < 3; ++c) {
                    const UINT sc = sourceBgra != outputBgra ? 2 - c : c;
                    const uint8_t expected = outer || black ? s[sc] : a[c];
                    check(r[c] == expected, "native crop/raw black/packing/channel/answer contract");
                }
                check(r[3] == s[3], "native source alpha preserved");
            }
    }
    // Completed banks reuse scratch and descriptor bindings without a new generation.
    for (int i = 0; i < 3; ++i) {
        hr(pipeline.Prepare(frame), "native repeat prepare");
        check(pipeline.GetNativeAnswer() == answer, "native cached answer");
        pipeline.DiscardRecording();
    }
    hr(pipeline.Prepare(frame), "native uncertain submission prepare");
    pipeline.MarkSubmitted(0);
    check(pipeline.Prepare(frame) == DXGI_ERROR_WAS_STILL_DRAWING, "zero fence retains native active batch");
    pipeline.MarkSubmitted(UINT64_MAX);
    check(pipeline.Prepare(frame) == DXGI_ERROR_WAS_STILL_DRAWING, "invalid fence retains native active batch");
    // No commands were submitted in this test; discard is therefore safe.
    pipeline.DiscardRecording();
    auto wrong = frame;
    wrong.processingPercent = 80;
    check(FAILED(pipeline.Prepare(wrong)), "native direct mode requires 100 percent");
    gpu.noErrors();
}

void nativeDelayedBatches(Gpu& gpu) {
    constexpr UINT width = 38, height = 13;
    auto a = gpu.texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto b = gpu.texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto outA = gpu.texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto outB = gpu.texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    Bytes pa(size_t(width) * height * 4), pb(pa.size());
    for (size_t i = 0; i < pa.size(); ++i) {
        pa[i] = uint8_t(40 + i % 180);
        pb[i] = uint8_t(50 + (i * 3) % 170);
    }
    gpu.transfer(a.Get(), &pa);
    gpu.transfer(b.Get(), &pb);
    FrameInput input{};
    input.nativeAnswer = true;
    input.output = outA.Get();
    input.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    input.outputEyeStride = width / 2;
    for (UINT eye = 0; eye < 2; ++eye)
        input.eyes[eye] = {a.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, {eye * width / 2, 0, width / 2, height}};
    Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
    ComPtr<ID3D12Fence> gate;
    hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "native gate");
    hr(gpu.queue->Wait(gate.Get(), 1), "native queue hold");
    std::vector<Gpu::Commands> pending;
    pending.reserve(3);
    uint64_t last{};
    ID3D12Resource* scratch{};
    for (UINT n = 0; n < 3; ++n) {
        input.output = n == 1 ? outB.Get() : outA.Get();
        for (auto& eye : input.eyes)
            eye.color = n == 1 ? b.Get() : a.Get();
        hr(pipeline.Prepare(input), "native blocked prepare");
        auto* answer = pipeline.GetNativeAnswer();
        check(n == 0 || answer == scratch, "one native answer shared in queue order");
        scratch = answer;
        pending.push_back(gpu.commands());
        auto* list = pending.back().list.Get();
        auto* source = input.eyes[0].color;
        Gpu::barrier(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Gpu::barrier(list, answer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(answer, source);
        Gpu::barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Gpu::barrier(list, answer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        pipeline.RecordResolve(list);
        last = gpu.submit(pending.back());
        check(gpu.fence->GetCompletedValue() < last, "native completion genuinely held");
        pipeline.MarkSubmitted(last);
    }
    // Keep batches immutable and own their source COM references while execution is held.
    a.Reset();
    b.Reset();
    hr(gate->Signal(1), "native release hold");
    gpu.wait(last);
    compare(gpu.transfer(outA.Get()), pa, 0, "native delayed first bank identity");
    compare(gpu.transfer(outB.Get()), pb, 0, "native delayed second bank identity");
    auto replacement = gpu.texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (auto& eye : input.eyes)
        eye.color = replacement.Get();
    const auto before = allocationCount.load();
    hr(pipeline.Prepare(input), "native completed bank rebind");
    check(allocationCount.load() == before, "native completed descriptor bank reused without allocation");
    pipeline.DiscardRecording();
    // Rejected geometry cannot replace the valid cached native generation.
    auto spatial = input;
    spatial.nativeAnswer = false;
    spatial.processingPercent = 80;
    check(!pipeline.Matches(spatial), "mode participates in geometry identity");
    check(FAILED(pipeline.Prepare(spatial)) && pipeline.GetNativeAnswer() == scratch,
          "rejected mode preserves existing generation");
    pipeline.Clear();
    check(pipeline.GetNativeAnswer() == nullptr, "native clear after drain");
    gpu.noErrors();
}

int main(int argc, char** argv) {
    try {
        Gpu gpu(argc > 1 && std::strcmp(argv[1], "--hardware") == 0);
        colorChannelContract(gpu, true);
        nativeLightEncodingContract(gpu, false);
        nativeLightEncodingContract(gpu, true);
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            DXGI_FORMAT_B8G8R8A8_UNORM})
            nativeNearBlackContract(gpu, format);
        nativeDelayedBatches(gpu);
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
            for (bool tiny : {false, true}) {
                nativeContract(gpu, format, format, tiny);
                nativeContract(
                    gpu, format, IsSrgb(format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, tiny);
            }
        std::puts("PASS native direct guard crop, black, alpha, formats, tiny extents and operation budget");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
