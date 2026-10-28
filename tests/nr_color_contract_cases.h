#pragma once

// Test model requests independently of the shadow-protection implementation.
// Uniform tiles keep each inspected pixel and its actual downsampled guides in
// the same color patch, so a zero channel request cannot come from a neighbor.
void colorChannelContract(Gpu& gpu, bool nativeAnswer) {
    struct Sample {
        std::array<uint8_t, 3> source, answer;
        bool sharedRedAnchor{};
    };
    std::vector<Sample> samples;
    for (UINT peak : {0u, 1u, 2u, 4u, 8u, 12u, 16u, 21u, 22u, 32u, 64u, 128u, 240u, 255u})
        for (UINT hue = 1; hue < 8; ++hue)
            for (UINT edit = 0; edit < 11; ++edit) {
                if (edit == 10 && hue != 7)
                    continue;
                Sample sample{};
                sample.sharedRedAnchor = hue == 7 && (edit == 7 || edit == 8 || edit == 10);
                for (UINT c = 0; c < 3; ++c) {
                    const int value = hue & (1u << c) ? int(peak) : 0;
                    int delta = 0;
                    if (edit >= 1 && edit <= 3 && c == edit - 1)
                        delta = 8;
                    if (edit >= 4 && edit <= 6 && c == edit - 4)
                        delta = -8;
                    if (edit == 7)
                        delta = 2;
                    if (edit == 8)
                        delta = -2;
                    if (edit == 9)
                        delta = c == 0 ? 8 : c == 1 ? -8 : 4;
                    if (edit == 10)
                        delta = c == 0 ? -2 : c == 1 ? -8 : -4;
                    sample.source[c] = uint8_t(value);
                    sample.answer[c] = uint8_t(std::clamp(value + delta, 0, 255));
                }
                samples.push_back(sample);
            }
    constexpr UINT size = 512, tile = 16, columns = size / tile, width = size * 2;
    check(samples.size() < size_t(columns) * columns, "color atlas capacity");
    size_t inspected = 0, nonzeroRequests = 0;
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                        DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_B8G8R8A8_UNORM}) {
        const bool bgra = RawColorFormat(format) == DXGI_FORMAT_B8G8R8A8_UNORM;
        const auto outputFormat = nativeAnswer     ? format
                                  : IsSrgb(format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                   : DXGI_FORMAT_R8G8B8A8_UNORM;
        auto source = gpu.texture(width, size, bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS);
        auto output = gpu.texture(width,
                                  size,
                                  nativeAnswer && bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        Bytes pixels(size_t(width) * size * 4), answers(pixels.size());
        for (UINT e = 0; e < 2; ++e)
            for (UINT y = 0; y < size; ++y)
                for (UINT x = 0; x < size; ++x) {
                    const size_t sampleIndex = size_t(y / tile) * columns + x / tile;
                    const auto sample = sampleIndex < samples.size() ? samples[sampleIndex] : Sample{};
                    for (UINT c = 0; c < 4; ++c) {
                        const auto at = offset(e * size + x, y, width, c, bgra);
                        pixels[at] = c == 3 ? uint8_t((sampleIndex + e * 73) % 256) : sample.source[c];
                        answers[at] = c == 3 ? 255 : sample.answer[c];
                    }
                }
        gpu.transfer(source.Get(), &pixels);
        FrameInput frame{};
        frame.output = output.Get();
        frame.outputFormat = outputFormat;
        frame.outputEyeStride = size;
        frame.nativeAnswer = nativeAnswer;
        frame.processingPercent = nativeAnswer ? 100 : 80;
        for (UINT e = 0; e < 2; ++e)
            frame.eyes[e] = {source.Get(), format, {e * size, 0, size, size}};
        Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
        hr(pipeline.Prepare(frame), "color contract prepare");
        if (nativeAnswer) {
            gpu.transfer(pipeline.GetNativeAnswer(), &answers);
        } else {
            auto commands = gpu.commands();
            pipeline.RecordInputs(commands.list.Get());
            gpu.wait(gpu.submit(commands));
            for (UINT e = 0; e < 2; ++e) {
                const auto eye = pipeline.GetWorkingEye(e);
                Bytes model(size_t(eye.extent.width) * eye.extent.height * 4);
                for (UINT y = 0; y < eye.extent.height; ++y)
                    for (UINT x = 0; x < eye.extent.width; ++x) {
                        const UINT sx = UINT((double(x) + .5) * size / eye.extent.width);
                        const UINT sy = UINT((double(y) + .5) * size / eye.extent.height);
                        const size_t sampleIndex = size_t(sy / tile) * columns + sx / tile;
                        const auto sample = sampleIndex < samples.size() ? samples[sampleIndex] : Sample{};
                        for (UINT c = 0; c < 4; ++c)
                            model[offset(x, y, eye.extent.width, c)] = c == 3 ? 255 : sample.answer[c];
                    }
                gpu.transfer(eye.answer, &model);
            }
        }
        auto commands = gpu.commands();
        pipeline.RecordResolve(commands.list.Get());
        const auto serial = gpu.submit(commands);
        pipeline.MarkSubmitted(serial);
        gpu.wait(serial);
        const auto result = gpu.transfer(output.Get());
        for (UINT e = 0; e < 2; ++e)
            for (size_t i = 0; i < samples.size(); ++i) {
                const UINT x = UINT(i % columns) * tile + tile / 2;
                const UINT y = UINT(i / columns) * tile + tile / 2;
                const auto& sample = samples[i];
                for (UINT c = 0; c < 3; ++c) {
                    const int actual = result[offset(e * size + x, y, width, c, nativeAnswer && bgra)];
                    const int before = sample.source[c], requested = sample.answer[c];
                    if (actual < std::min(before, requested) || actual > std::max(before, requested)) {
                        std::fprintf(stderr,
                                     "COLOR_CONTRACT native=%u format=%u eye=%u sample=%zu channel=%u source=%d "
                                     "answer=%d result=%d\n",
                                     unsigned(nativeAnswer),
                                     unsigned(format),
                                     e,
                                     i,
                                     c,
                                     before,
                                     requested,
                                     actual);
                        check(false, "NR protection must not invent, reverse or amplify a channel edit");
                    }
                    if (before == requested)
                        check(actual == before, "unchanged model channel remains exact");
                    if (std::max({sample.source[0], sample.source[1], sample.source[2]}) >= 22)
                        check(std::abs(actual - requested) <= 1, "full above-toe NR edit survives color protection");
                    if (c == 0 && sample.sharedRedAnchor && before >= (IsSrgb(format) ? 4 : 1))
                        check(std::abs(actual - requested) <= 1,
                              "positive and negative shared RGB detail survives outside the narrow black toe");
                    nonzeroRequests += actual != before;
                    ++inspected;
                }
                check(result[offset(e * size + x, y, width, 3)] == pixels[offset(e * size + x, y, width, 3)],
                      "color contract preserves original alpha");
            }
    }
    check(nonzeroRequests > samples.size(), "color protection must retain substantial real model requests");
    gpu.noErrors();
    std::printf(
        "PASS color channel contract native=%u backgroundCases=98 uniqueColors=92 requests=%zu inspectedChannels=%zu "
        "retainedChanges=%zu\n",
        unsigned(nativeAnswer),
        samples.size(),
        inspected,
        nonzeroRequests);
}
