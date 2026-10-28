#include <d3d12sdklayers.h>
#include "fg_atlas_gpu_helpers.h"
#include "xrfg/d3d12_frame_synthesizer.hpp"

#include <algorithm>

namespace {
    constexpr UINT monoWidth = 513, monoHeight = 257;
    using Results = std::vector<AtlasPixels>;

    bool gpuValidationRequested() {
        char mode[2]{};
        return GetEnvironmentVariableA("VDXR_FG_GPU_VALIDATION", mode, sizeof(mode)) == 1 && mode[0] == '1';
    }

    void checkValidationSupport(Gpu& gpu) {
        if (!gpuValidationRequested())
            return;
        check(gpu.debugEnabled && gpu.diagnostics, "requested GPU validation has an active debug info queue");
        for (UINT64 index = 0; index < gpu.diagnostics->GetNumStoredMessages(); ++index) {
            SIZE_T bytes{};
            success(gpu.diagnostics->GetMessage(index, nullptr, &bytes), "GPU validation diagnostic size");
            std::vector<uint8_t> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            success(gpu.diagnostics->GetMessage(index, message, &bytes), "GPU validation diagnostic");
            check(message->ID != D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_UNSUPPORTED,
                  "requested GPU validation is supported by this device");
        }
    }

    AtlasPixels image(UINT step) {
        AtlasPixels pixels(monoWidth * monoHeight * 2 * 4);
        for (UINT eye = 0; eye < 2; ++eye)
            for (UINT y = 0; y < monoHeight; ++y)
                for (UINT x = 0; x < monoWidth; ++x) {
                    auto* p = pixels.data() + ((eye * monoHeight + y) * monoWidth + x) * 4;
                    p[0] = static_cast<uint8_t>(17 + (x * 3 + y * 5 + eye * 61) % 113);
                    p[1] = static_cast<uint8_t>(23 + ((x / 7 + y / 11 + eye) % 2) * 49);
                    p[2] = static_cast<uint8_t>(31 + (x + y * 2 + eye * 37) % 97);
                    p[3] = static_cast<uint8_t>(39 + (x * 5 + y * 3 + eye * 71) % 187);
                    const UINT center = eye ? 331 - step : 113 + step;
                    if (x >= center && x < center + 37 && y >= 89 && y < 163) {
                        p[eye ? 0 : 1] = 239;
                        p[2] = static_cast<uint8_t>(eye ? 51 : 197);
                        p[3] = static_cast<uint8_t>(eye ? 213 : 83);
                    }
                }
        return pixels;
    }

    AtlasPixels wide(const AtlasPixels& array) {
        AtlasPixels pixels(array.size());
        for (UINT eye = 0; eye < 2; ++eye)
            for (UINT y = 0; y < monoHeight; ++y)
                std::memcpy(pixels.data() + (y * monoWidth * 2 + eye * monoWidth) * 4,
                            array.data() + (eye * monoHeight + y) * monoWidth * 4,
                            monoWidth * 4);
        return pixels;
    }

    void rejectInvalidLayouts() {
        AtlasGpu gpu;
        auto input = gpu.texture(monoWidth, monoHeight, 2);
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* inputs[]{input.Get()};
        success(history->initialize(gpu.device.Get(), gpu.queue.Get(), inputs, D3D12_RESOURCE_STATE_COMMON),
                "initialize layout-validation history");
        auto current = gpu.texture(monoWidth, monoHeight, 2, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto array = gpu.texture(monoWidth, monoHeight, 2, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto atlas = gpu.texture(monoWidth * 2, monoHeight);
        ID3D12Resource* currents[]{current.Get()};
        ID3D12Resource* arrays[]{array.Get()};
        ID3D12Resource* atlases[]{atlas.Get()};
        using Layout = xrfg::D3D12SyntheticOutputLayout;
        const auto rejected =
            [&](std::span<ID3D12Resource* const> real, std::span<ID3D12Resource* const> synthetic, Layout layout) {
                xrfg::D3D12FrameSynthesizer value;
                check(value.initialize(gpu.device.Get(),
                                       gpu.queue.Get(),
                                       history,
                                       real,
                                       synthetic,
                                       DXGI_FORMAT_R8G8B8A8_UNORM,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       xrfg::D3D12OpticalFlowBackend::fidelity_fx,
                                       {},
                                       false,
                                       true,
                                       layout) == E_INVALIDARG,
                      "invalid synthetic layout is rejected before GPU initialization");
            };
        // Ordinary callers do not silently accept the new wide geometry.
        xrfg::D3D12FrameSynthesizer native;
        check(native.initialize(gpu.device.Get(),
                                gpu.queue.Get(),
                                history,
                                currents,
                                atlases,
                                DXGI_FORMAT_R8G8B8A8_UNORM,
                                D3D12_RESOURCE_STATE_RENDER_TARGET) == E_INVALIDARG,
              "omitted layout argument preserves native destination validation");
        rejected(currents, arrays, Layout::shared_stereo_atlas);
        rejected(currents, atlases, static_cast<Layout>(99));
        auto mono = gpu.texture(monoWidth, monoHeight, 1, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* monoCurrent[]{mono.Get()};
        rejected(monoCurrent, atlases, Layout::shared_stereo_atlas);
        auto oversized =
            gpu.texture(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / 2 + 1, 16, 2, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* oversizedCurrent[]{oversized.Get()};
        rejected(oversizedCurrent, atlases, Layout::shared_stereo_atlas);
        ID3D12Resource* duplicates[]{atlas.Get(), atlas.Get()};
        rejected(currents, duplicates, Layout::shared_stereo_atlas);
        gpu.noErrors();
        checkValidationSupport(gpu);
    }

    Results run(bool atlas,
                xrfg::D3D12OpticalFlowBackend backend,
                UINT multiplier,
                bool srgb,
                bool cropped,
                bool reversedSlices,
                bool markerEnabled,
                const std::string& sharedMode = {},
                bool enableShared3x = false) {
        AtlasGpu gpu;
        auto input = gpu.texture(monoWidth, monoHeight, 2);
        std::array<ComPtr<ID3D12Resource>, 2> current;
        std::array<ComPtr<ID3D12Resource>, 3> synthetic;
        std::array<ID3D12Resource*, 2> currentRaw{};
        std::array<ID3D12Resource*, 3> syntheticRaw{};
        const auto syntheticState = atlas ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_RENDER_TARGET;
        const auto arraySeed = image(72), atlasSeed = wide(arraySeed);
        for (UINT i = 0; i < 2; ++i) {
            current[i] = gpu.texture(monoWidth, monoHeight, 2, D3D12_RESOURCE_STATE_RENDER_TARGET);
            currentRaw[i] = current[i].Get();
            if (i < multiplier - 1) {
                synthetic[i] =
                    gpu.texture(atlas ? monoWidth * 2 : monoWidth, monoHeight, atlas ? 1 : 2, syntheticState);
                syntheticRaw[i] = synthetic[i].Get();
                gpu.transfer(synthetic[i].Get(), atlas ? &atlasSeed : &arraySeed, syntheticState);
            }
        }
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* inputs[]{input.Get()};
        success(history->initialize(gpu.device.Get(), gpu.queue.Get(), inputs, D3D12_RESOURCE_STATE_COMMON),
                "initialize array history for backend differential");
        xrfg::D3D12FrameSynthesizer synthesizer;
        xrfg::D3D12NvidiaOpticalFlowOptions options{};
        if (sharedMode == "shared-3x-slow")
            options.preset = xrfg::D3D12NvidiaPerformancePreset::slow;
        if (sharedMode == "shared-3x-fast")
            options.preset = xrfg::D3D12NvidiaPerformancePreset::fast;
        options.bidirectional = sharedMode == "shared-3x-bidir";
        if (backend == xrfg::D3D12OpticalFlowBackend::fidelity_fx)
            options.input_scale = xrfg::D3D12OpticalFlowInputScale::full;
        if (multiplier == 4) {
            synthetic[2] = gpu.texture(atlas ? monoWidth * 2 : monoWidth, monoHeight, atlas ? 1 : 2, syntheticState);
            syntheticRaw[2] = synthetic[2].Get();
            gpu.transfer(synthetic[2].Get(), atlas ? &atlasSeed : &arraySeed, syntheticState);
        }
        const auto syntheticSpan = std::span<ID3D12Resource* const>(syntheticRaw.data(), multiplier - 1);
        if (atlas)
            success(synthesizer.initialize(gpu.device.Get(),
                                           gpu.queue.Get(),
                                           history,
                                           currentRaw,
                                           syntheticSpan,
                                           srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           backend,
                                           options,
                                           false,
                                           true,
                                           xrfg::D3D12SyntheticOutputLayout::shared_stereo_atlas,
                                           enableShared3x),
                    "initialize opt-in shared synthetic atlases");
        else
            success(synthesizer.initialize(gpu.device.Get(),
                                           gpu.queue.Get(),
                                           history,
                                           currentRaw,
                                           syntheticSpan,
                                           srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           backend,
                                           options),
                    "initialize unchanged default-native arrays");
        std::array<xrfg::D3D12ReprojectionView, 2> views{};
        for (UINT view = 0; view < 2; ++view) {
            auto& value = views[view];
            const UINT eye = reversedSlices ? 1 - view : view;
            value.array_slice = eye;
            value.pose.orientation.w = 1;
            value.pose.position.x = eye ? .032f : -.032f;
            value.fov = {-.8f, .8f, .8f, -.8f};
            if (cropped)
                value.image_rect = eye ? xrfg::D3D12ImageRect{23, 5, monoWidth - 41, monoHeight - 17}
                                       : xrfg::D3D12ImageRect{7, 11, monoWidth - 19, monoHeight - 29};
        }
        const auto covered = [&](UINT eye, UINT x, UINT y) {
            for (const auto& view : views) {
                if (view.array_slice != eye)
                    continue;
                const auto rect =
                    view.image_rect.width ? view.image_rect : xrfg::D3D12ImageRect{0, 0, monoWidth, monoHeight};
                return x >= rect.offset_x && x < rect.offset_x + rect.width && y >= rect.offset_y &&
                       y < rect.offset_y + rect.height;
            }
            return false;
        };
        Results result;
        const auto snapshot = [&] {
            for (UINT phase = 0; phase < multiplier - 1; ++phase) {
                auto pixels = gpu.transfer(synthetic[phase].Get(), nullptr, syntheticState);
                if (!atlas)
                    pixels = wide(pixels);
                AtlasPixels roi;
                for (UINT eye = 0; eye < 2; ++eye)
                    for (UINT y = 0; y < monoHeight; ++y)
                        for (UINT x = 0; x < monoWidth; ++x) {
                            const size_t index = (y * monoWidth * 2 + eye * monoWidth + x) * 4;
                            if (covered(eye, x, y))
                                roi.insert(roi.end(), pixels.begin() + index, pixels.begin() + index + 4);
                            else
                                check(std::equal(pixels.begin() + index,
                                                 pixels.begin() + index + 4,
                                                 atlasSeed.begin() + index),
                                      "backend output preserves each undrawn seeded pixel");
                        }
                result.push_back(std::move(roi));
            }
            result.push_back(gpu.transfer(current[1].Get(), nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET));
        };
        const auto capture = [&](UINT step) {
            const auto pixels = image(step);
            gpu.transfer(input.Get(), &pixels);
            xrfg::D3D12HistoryCaptureTicket ticket{};
            success(history->capture(0, &ticket), "capture backend differential input");
            success(history->commit(ticket), "commit backend differential input");
            return ticket;
        };
        const auto completed = [&](const xrfg::D3D12FrameSynthesisTicket& ticket) {
            ComPtr<ID3D12Fence> fence;
            success(synthesizer.completion_fence(&fence), "backend differential completion fence");
            gpu.wait(fence.Get(), ticket.fence_value);
            success(synthesizer.wait_for_previous_submission(0), "completed backend differential work");
            gpu.noErrors();
        };
        const std::optional<xrfg::OverlayPlacement> marker =
            markerEnabled ? std::optional<xrfg::OverlayPlacement>(xrfg::OverlayPlacement{.07f, .03f, -2.0f, .23f, .17f})
                          : std::nullopt;
        const auto cycle = [&] {
            const auto a = capture(0);
            xrfg::D3D12FrameSynthesisTicket prime{};
            success(synthesizer.submit_prime(a, views, 0, &prime), "prime backend differential");
            completed(prime);
            check(gpu.transfer(current[0].Get(), nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET) == image(0),
                  "backend prime current remains bit-exact");
            const auto b = capture(24);
            std::array<xrfg::D3D12ExtraSynthetic, 2> extraValues{};
            for (unsigned i = 0; i < multiplier - 2; ++i)
                extraValues[i] = {i + 1, static_cast<float>(i + 2) / multiplier};
            const std::span<const xrfg::D3D12ExtraSynthetic> extra(extraValues.data(), multiplier - 2);
            if (multiplier == 4) {
                auto alias = extraValues;
                alias[1].destination_index = alias[0].destination_index;
                xrfg::D3D12FrameSynthesisTicket rejected{};
                check(synthesizer.submit_pair_outputs(
                          b, views, views, 0, 1, &rejected, marker, {}, false, .25f, alias) == E_INVALIDARG &&
                          !rejected.fence_value,
                      "bounded third output rejects duplicate destinations before GPU submission");
                const std::array<xrfg::D3D12ExtraSynthetic, 3> overflow{};
                check(synthesizer.submit_pair_outputs(
                          b, views, views, 0, 1, &rejected, marker, {}, false, .25f, overflow) == E_INVALIDARG &&
                          !rejected.fence_value,
                      "bounded extra span rejects a fourth synthetic");
            }
            xrfg::D3D12FrameSynthesisTicket pair{};
            success(synthesizer.submit_pair_outputs(
                        b, views, views, 0, 1, &pair, marker, {}, false, 1.0f / multiplier, extra),
                    "generate backend differential pair");
            completed(pair);
            snapshot();
            // This is a true repeated history ticket, with camera metadata
            // advancing while the captured image and slice identities remain.
            auto repeatedViews = views;
            for (auto& view : repeatedViews) {
                view.pose.orientation.y = std::sin(.006f);
                view.pose.orientation.w = std::cos(.006f);
            }
            xrfg::D3D12FrameSynthesisTicket repeated{};
            success(synthesizer.submit_pair_outputs(
                        b, repeatedViews, repeatedViews, 0, 1, &repeated, marker, {}, false, 1.0f / multiplier, extra),
                    "generate true repeated-capture backend pair");
            completed(repeated);
            check(repeated.previous_serial == b.serial && repeated.current_serial == b.serial,
                  "backend repeated capture preserves capture identity");
            snapshot();
        };
        cycle();
        success(synthesizer.reconfigure_gpu_timing(true), "retain output layout through timing rebuild");
        success(history->invalidate(), "fresh history after timing rebuild");
        cycle();
        const auto other = backend == xrfg::D3D12OpticalFlowBackend::nvidia ? xrfg::D3D12OpticalFlowBackend::fidelity_fx
                                                                            : xrfg::D3D12OpticalFlowBackend::nvidia;
        success(synthesizer.reconfigure(other, options), "retain output layout through backend rebuild");
        success(history->invalidate(), "fresh history after backend rebuild");
        cycle();
        if (!sharedMode.empty()) {
            success(synthesizer.reconfigure(backend, options),
                    "retain shared-3x opt-in when returning to original backend");
            success(history->invalidate(), "fresh history after restoring original backend");
            cycle();
        }
        success(synthesizer.wait_for_idle(), "settle backend differential");
        gpu.noErrors();
        checkValidationSupport(gpu);
        return result;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        ComPtr<ID3D12Debug3> gpuValidation;
        if (gpuValidationRequested()) {
            success(D3D12GetDebugInterface(IID_PPV_ARGS(&gpuValidation)), "required GPU validation interface");
            gpuValidation->EnableDebugLayer();
            gpuValidation->SetEnableGPUBasedValidation(TRUE);
            gpuValidation->SetEnableSynchronizedCommandQueueValidation(TRUE);
            std::puts("GPU-based and synchronized queue validation enabled before device creation");
        }
        check(argc == 4 || argc == 5,
              "nvidia|fidelityfx-full 2|3 full|srgb-crop-marker|reversed-crop-marker [shared-3x[-slow|-fast|-bidir]] "
              "required");
        const std::string backendName = argv[1], scenario = argv[3];
        const std::string sharedMode = argc == 5 ? argv[4] : "";
        check(sharedMode.empty() || sharedMode == "shared-3x" || sharedMode == "shared-3x-slow" ||
                  sharedMode == "shared-3x-fast" || sharedMode == "shared-3x-bidir",
              "known shared-3x opt-in/fallback control");
        check(backendName == "nvidia" || backendName == "fidelityfx-full", "known differential backend");
        const UINT multiplier = static_cast<UINT>(std::stoul(argv[2]));
        check(multiplier >= 2 && multiplier <= 4, "known differential multiplier");
        check(scenario == "full" || scenario == "srgb-crop-marker" || scenario == "reversed-crop-marker",
              "known direct backend differential scenario");
        const auto backend = backendName == "nvidia" ? xrfg::D3D12OpticalFlowBackend::nvidia
                                                     : xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        rejectInvalidLayouts();
        const bool srgb = scenario == "srgb-crop-marker", cropped = scenario != "full";
        // Shared-mode controls use the same atlas geometry on both sides;
        // only the private MRT opt-in differs inside this one executable.
        const auto native = run(!sharedMode.empty(),
                                backend,
                                multiplier,
                                srgb,
                                cropped,
                                scenario == "reversed-crop-marker",
                                cropped,
                                sharedMode);
        const auto atlas = run(true,
                               backend,
                               multiplier,
                               srgb,
                               cropped,
                               scenario == "reversed-crop-marker",
                               cropped,
                               sharedMode,
                               !sharedMode.empty());
        check(native == atlas,
              "default-native and opted atlas outputs match exactly across repeated/marker/rebuild paths");
        std::puts("PASS: native/atlas backend byte identity, repeated captures, markers and rebuilds");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
