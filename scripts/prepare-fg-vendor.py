"""Apply audited ownership and timing fixes to verified OFXR source.

Immutable upstream files stay intact. The compiled derivative and a unified
diff are emitted into the build directory; this script is corresponding source
for that derivative. Only the fixed pinned input is accepted.
"""

import argparse
import difflib
import hashlib
import json
from pathlib import Path

from fg_early_readiness import prepare_early_readiness


def prepare_history(source: Path, output: Path) -> None:
    """Capture a stereo atlas into the existing owned history slots."""
    header_path = source / "include/xrfg/d3d12_history.hpp"
    source_path = source / "src/d3d12/d3d12_history.cpp"
    original_header = header_path.read_text(encoding="utf-8")
    original_source = source_path.read_text(encoding="utf-8")
    header = original_header
    implementation = original_source
    header_changes = [
        ("enum class D3D12HistoryInitializationStage : std::uint32_t {",
         "// Internal source layout; the public module ABI is unchanged.\n"
         "enum class D3D12HistorySourceLayout { native, stereo_atlas };\n\n"
         "enum class D3D12HistoryInitializationStage : std::uint32_t {"),
        ("        D3D12HistoryInitializationStage* failure_stage = nullptr) noexcept;",
         "        D3D12HistoryInitializationStage* failure_stage = nullptr,\n"
         "        D3D12HistorySourceLayout source_layout = D3D12HistorySourceLayout::native) noexcept;"),
    ]
    source_changes = [
        ("    D3D12_RESOURCE_STATES release_state{};",
         "    D3D12_RESOURCE_STATES release_state{};\n"
         "    D3D12HistorySourceLayout source_layout{};"),
        ("        D3D12HistoryInitializationStage* failure_stage) {",
         "        D3D12HistoryInitializationStage* failure_stage,\n"
         "        D3D12HistorySourceLayout input_layout) {"),
        ("        if (input_device == nullptr || input_queue == nullptr || inputs.empty() ||",
         "        if ((input_layout != D3D12HistorySourceLayout::native &&\n"
         "             input_layout != D3D12HistorySourceLayout::stereo_atlas) ||\n"
         "            input_device == nullptr || input_queue == nullptr || inputs.empty() ||"),
        ("        source_images.reserve(inputs.size());",
         "        if (input_layout == D3D12HistorySourceLayout::stereo_atlas &&\n"
         "            ((source_description.Width & 1) != 0 ||\n"
         "             source_description.Width > std::numeric_limits<UINT>::max() ||\n"
         "             source_description.DepthOrArraySize != 1 || source_description.MipLevels != 1 ||\n"
         "             source_description.SampleDesc.Count != 1)) {\n"
         "            if (failure_stage != nullptr) {\n"
         "                *failure_stage = D3D12HistoryInitializationStage::source_description;\n"
         "            }\n"
         "            return E_INVALIDARG;\n"
         "        }\n\n"
         "        source_images.reserve(inputs.size());"),
        ("        release_state = input_release_state;",
         "        release_state = input_release_state;\n"
         "        source_layout = input_layout;"),
        ("        for (HistorySlot& slot : slots) {\n            HRESULT result = device->CreateCommittedResource(",
         "        if (source_layout == D3D12HistorySourceLayout::stereo_atlas) {\n"
         "            history_description.Width /= 2;\n"
         "            history_description.DepthOrArraySize = 2;\n"
         "            history_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;\n"
         "        }\n"
         "        for (HistorySlot& slot : slots) {\n            HRESULT result = device->CreateCommittedResource("),
        ("        slot.capture_list->CopyResource(slot.resource.Get(), source);",
         "        if (source_layout == D3D12HistorySourceLayout::stereo_atlas) {\n"
         "            const auto description = source->GetDesc();\n"
         "            const UINT eye_width = static_cast<UINT>(description.Width / 2);\n"
         "            for (UINT eye = 0; eye < 2; ++eye) {\n"
         "                D3D12_TEXTURE_COPY_LOCATION atlas{};\n"
         "                atlas.pResource = source;\n"
         "                atlas.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;\n"
         "                D3D12_TEXTURE_COPY_LOCATION isolated{};\n"
         "                isolated.pResource = slot.resource.Get();\n"
         "                isolated.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;\n"
         "                isolated.SubresourceIndex = eye;\n"
         "                const D3D12_BOX box{eye * eye_width, 0, 0,\n"
         "                    (eye + 1) * eye_width, description.Height, 1};\n"
         "                slot.capture_list->CopyTextureRegion(&isolated, 0, 0, 0, &atlas, &box);\n"
         "            }\n"
         "        } else {\n"
         "            slot.capture_list->CopyResource(slot.resource.Get(), source);\n"
         "        }"),
        ("    D3D12_RESOURCE_STATES release_state,\n"
         "    D3D12HistoryInitializationStage* failure_stage) noexcept {",
         "    D3D12_RESOURCE_STATES release_state,\n"
         "    D3D12HistoryInitializationStage* failure_stage,\n"
         "    D3D12HistorySourceLayout source_layout) noexcept {"),
        ("            device, queue, source_images, release_state, failure_stage);",
         "            device, queue, source_images, release_state, failure_stage, source_layout);"),
    ]
    for old, new in header_changes:
        if header.count(old) != 1:
            raise ValueError("Pinned history header context does not match uniquely")
        header = header.replace(old, new)
    for old, new in source_changes:
        if implementation.count(old) != 1:
            raise ValueError(f"Pinned history capture context does not match uniquely: {old!r}")
        implementation = implementation.replace(old, new)
    (output / "xrfg").mkdir(parents=True, exist_ok=True)
    (output / "xrfg/d3d12_history.hpp").write_text(header, encoding="utf-8")
    (output / "d3d12_history.cpp").write_text(implementation, encoding="utf-8")
    patch = "".join(difflib.unified_diff(
        original_header.splitlines(keepends=True), header.splitlines(keepends=True),
        fromfile="upstream/include/xrfg/d3d12_history.hpp", tofile="compiled/include/xrfg/d3d12_history.hpp"))
    patch += "".join(difflib.unified_diff(
        original_source.splitlines(keepends=True), implementation.splitlines(keepends=True),
        fromfile="upstream/src/d3d12/d3d12_history.cpp", tofile="compiled/src/d3d12/d3d12_history.cpp"))
    (output / "history-capture.patch").write_text(patch, encoding="utf-8")


def prepare_pair_real_output(source: Path, output: Path, modified: str) -> str:
    """Keep legacy real outputs; host-owned pairs only restore input state."""
    header_path = source / "include/xrfg/d3d12_frame_synthesizer.hpp"
    original_header = header_path.read_text(encoding="utf-8")
    header_old = "        bool enable_nvidia_gpu_timing = false) noexcept;"
    header_new = (
        "        bool enable_nvidia_gpu_timing = false,\n"
        "        // False skips the unused real output of ordinary pairs only.\n"
        "        // Prime and repeated-capture output semantics stay unchanged.\n"
        "        bool copy_current_output = true) noexcept;")
    if original_header.count(header_old) != 1:
        raise ValueError("Pinned current-output header context does not match uniquely")
    header = original_header.replace(header_old, header_new)

    changes = [
        ("    bool nvidia_gpu_timing_enabled{};",
         "    bool nvidia_gpu_timing_enabled{};\n    bool copy_current_output{true};"),
        ("        bool enable_nvidia_gpu_timing) {",
         "        bool enable_nvidia_gpu_timing,\n        bool input_copy_current_output) {"),
        ("        backend = input_backend;\n        nvidia_options = input_nvidia_options;",
         "        backend = input_backend;\n        nvidia_options = input_nvidia_options;\n"
         "        copy_current_output = input_copy_current_output;"),
        ("    bool enable_nvidia_gpu_timing) noexcept {",
         "    bool enable_nvidia_gpu_timing,\n    bool copy_current_output) noexcept {"),
        ("            enable_nvidia_gpu_timing);\n        if (FAILED(result)) {",
         "            enable_nvidia_gpu_timing,\n            copy_current_output);\n        if (FAILED(result)) {"),
        ("            backend, options, impl_->nvidia_gpu_timing_enabled);",
         "            backend, options, impl_->nvidia_gpu_timing_enabled, impl_->copy_current_output);"),
        ("            impl_->backend, impl_->nvidia_options, enabled);",
         "            impl_->backend, impl_->nvidia_options, enabled, impl_->copy_current_output);"),
    ]
    for old, new in changes:
        if modified.count(old) != 1:
            raise ValueError(f"Pinned current-output context does not match uniquely: {old!r}")
        modified = modified.replace(old, new)

    # Preserve the current-copy command list and its submission/fence identity.
    # Host-owned B is read directly by the host, so only its history source
    # needs restoring; its private destination must remain untouched.
    start_marker = (
        "        // The copy's own transitions travel with the copy, so the resources\n"
        "        // are left in their normal states while it waits to be submitted.\n")
    end_marker = (
        "        slot.current_copy_command_list->ResourceBarrier(\n"
        "            static_cast<UINT>(after_current_copy.size()),\n"
        "            after_current_copy.data());\n")
    if modified.count(start_marker) != 2:
        raise ValueError("Pinned pair-current-output block count differs")
    offset = 0
    for backend in ("NVIDIA", "FidelityFX"):
        start = modified.index(start_marker, offset)
        end = modified.index(end_marker, start) + len(end_marker)
        block = modified[start:end]
        if block.count("->CopyResource(") != 1 or block.count(f'"OFXR {backend} current copy"') != 1:
            raise ValueError("Pinned backend current-output copy block differs")
        wrapped = (
            "        if (copy_current_output) {\n"
            + "".join("    " + line if line.strip() else line for line in block.splitlines(keepends=True))
            + "        } else {\n"
            "            const auto restore_current = transition_barrier(\n"
            "                current_resource,\n"
            "                kShaderReadState,\n"
            "                D3D12_RESOURCE_STATE_COMMON);\n"
            "            slot.current_copy_command_list->ResourceBarrier(1, &restore_current);\n"
            "        }\n")
        modified = modified[:start] + wrapped + modified[end:]
        offset = start + len(wrapped)

    (output / "xrfg").mkdir(parents=True, exist_ok=True)
    (output / "xrfg/d3d12_frame_synthesizer.hpp").write_text(header, encoding="utf-8")
    (output / "current-output-header.patch").write_text("".join(difflib.unified_diff(
        original_header.splitlines(keepends=True), header.splitlines(keepends=True),
        fromfile="upstream/include/xrfg/d3d12_frame_synthesizer.hpp",
        tofile="compiled/include/xrfg/d3d12_frame_synthesizer.hpp")), encoding="utf-8")
    return modified


def prepare_synthetic_atlas_output(output: Path, modified: str) -> str:
    """Opt in to shared wide synthetic destinations; keep current arrays native."""
    header_path = output / "xrfg/d3d12_frame_synthesizer.hpp"
    original_header = header_path.read_text(encoding="utf-8")
    header = original_header
    header_changes = [
        ("class D3D12FrameSynthesizer final {",
         "// Shared atlases require two logical array slices and 2W x H x 1\n"
         "// synthetic destinations. Only those destinations enter/leave COMMON;\n"
         "// current destinations retain the explicit release_state contract.\n"
         "enum class D3D12SyntheticOutputLayout : std::uint32_t {\n"
         "    native,\n"
         "    shared_stereo_atlas,\n"
         "};\n\nclass D3D12FrameSynthesizer final {"),
        ("        bool copy_current_output = true) noexcept;",
         "        bool copy_current_output = true,\n"
         "        D3D12SyntheticOutputLayout synthetic_output_layout =\n"
         "            D3D12SyntheticOutputLayout::native) noexcept;"),
    ]
    for old, new in header_changes:
        if header.count(old) != 1:
            raise ValueError("Pinned synthetic-atlas header context does not match uniquely")
        header = header.replace(old, new)
    changes = [
        ("    bool copy_current_output{true};",
         "    bool copy_current_output{true};\n"
         "    D3D12SyntheticOutputLayout synthetic_output_layout{};"),
        ("        bool input_copy_current_output) {",
         "        bool input_copy_current_output,\n"
         "        D3D12SyntheticOutputLayout input_synthetic_output_layout) {"),
        ("        if (input_device == nullptr || input_queue == nullptr ||",
         "        if ((input_synthetic_output_layout != D3D12SyntheticOutputLayout::native &&\n"
         "             input_synthetic_output_layout != D3D12SyntheticOutputLayout::shared_stereo_atlas) ||\n"
         "            input_device == nullptr || input_queue == nullptr ||"),
        ("        synthetic_destinations.reserve(input_synthetic_images.size());",
         "        auto synthetic_description = description;\n"
         "        if (input_synthetic_output_layout == D3D12SyntheticOutputLayout::shared_stereo_atlas) {\n"
         "            if (description.DepthOrArraySize != 2 ||\n"
         "                description.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / 2U) {\n"
         "                return E_INVALIDARG;\n"
         "            }\n"
         "            synthetic_description.Width *= 2;\n"
         "            synthetic_description.DepthOrArraySize = 1;\n"
         "        }\n"
         "        synthetic_destinations.reserve(input_synthetic_images.size());"),
        ("        copy_current_output = input_copy_current_output;",
         "        copy_current_output = input_copy_current_output;\n"
         "        synthetic_output_layout = input_synthetic_output_layout;"),
        ("    bool copy_current_output) noexcept {",
         "    bool copy_current_output,\n"
         "    D3D12SyntheticOutputLayout synthetic_output_layout) noexcept {"),
        ("            copy_current_output);\n        if (FAILED(result)) {",
         "            copy_current_output,\n            synthetic_output_layout);\n        if (FAILED(result)) {"),
        ("            backend, options, impl_->nvidia_gpu_timing_enabled, impl_->copy_current_output);",
         "            backend, options, impl_->nvidia_gpu_timing_enabled, impl_->copy_current_output,\n"
         "            impl_->synthetic_output_layout);"),
        ("            impl_->backend, impl_->nvidia_options, enabled, impl_->copy_current_output);",
         "            impl_->backend, impl_->nvidia_options, enabled, impl_->copy_current_output,\n"
         "            impl_->synthetic_output_layout);"),
    ]
    for old, new in changes:
        if modified.count(old) != 1:
            raise ValueError(f"Pinned synthetic-atlas context does not match uniquely: {old!r}")
        modified = modified.replace(old, new)
    start = modified.index("        synthetic_destinations.reserve(input_synthetic_images.size());")
    end = modified.index("        const UINT width =", start)
    block = modified[start:end]
    old = "!copy_compatible(description, image->GetDesc())"
    if block.count(old) != 1:
        raise ValueError("Pinned synthetic-atlas destination validation differs")
    modified = modified[:start] + block.replace(old, "!copy_compatible(synthetic_description, image->GetDesc())") + modified[end:]

    marker = "    [[nodiscard]] HRESULT create_rtv_heap(UINT node_mask) noexcept {"
    helpers = """    [[nodiscard]] bool shared_synthetic_atlas() const noexcept {
        return synthetic_output_layout == D3D12SyntheticOutputLayout::shared_stereo_atlas;
    }

    void transition_synthetic_atlas(ID3D12GraphicsCommandList* commands,
        UINT destination_index, bool begin) const noexcept {
        if (!shared_synthetic_atlas()) return;
        const auto barrier = transition_barrier(
            synthetic_destinations[destination_index].resource.Get(),
            begin ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_RENDER_TARGET,
            begin ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON);
        commands->ResourceBarrier(1, &barrier);
    }

    [[nodiscard]] D3D12ReprojectionView synthetic_output_view(
        const D3D12ReprojectionView& view, UINT slice) const noexcept {
        if (!shared_synthetic_atlas()) return view;
        auto output = view;
        output.image_rect = resolved_rect(view.image_rect,
            static_cast<UINT>(image_description.Width), image_description.Height);
        output.image_rect.offset_x += slice * static_cast<UINT>(image_description.Width);
        return output;
    }

"""
    if modified.count(marker) != 1:
        raise ValueError("Pinned synthetic-atlas helper location differs")
    modified = modified.replace(marker, helpers + marker)
    start = modified.index("        for (const Destination& destination : synthetic_destinations) {")
    end = modified.index("        current_rtv_base = descriptor_index;", start)
    block = modified[start:end]
    old = "                rtv_description.Texture2DArray.FirstArraySlice = slice;"
    if block.count(old) != 1:
        raise ValueError("Pinned synthetic-atlas RTV context differs")
    block = block.replace("        for (const Destination& destination : synthetic_destinations) {",
                          "        if (shared_synthetic_atlas()) {\n"
                          "            rtv_description = {};\n"
                          "            rtv_description.Format = view_format;\n"
                          "            rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;\n"
                          "        }\n"
                          "        for (const Destination& destination : synthetic_destinations) {")
    block = block.replace(old, "                if (!shared_synthetic_atlas())\n" + old + "\n")
    block += ("        rtv_description = {};\n"
              "        rtv_description.Format = view_format;\n"
              "        rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;\n"
              "        rtv_description.Texture2DArray.ArraySize = 1;\n")
    modified = modified[:start] + block + modified[end:]

    marker = "        for (UINT output = 0; output < synthetic_output_count(); ++output) {"
    if modified.count(marker) != 4:
        raise ValueError("Pinned synthetic-atlas draw loop count differs")
    offset = 0
    for _ in range(4):
        start = modified.index(marker, offset)
        opening = modified.index("{", start)
        end, depth = opening + 1, 1
        while depth:
            depth += (modified[end] == "{") - (modified[end] == "}")
            end += 1
        block = modified[start:end]
        commands = "synthesis" if "synthesis->OMSetRenderTargets" in block else "slot.command_list.Get()"
        block = block.replace(marker, marker + "\n"
                              "            const UINT atlas_destination =\n"
                              "                synthetic_output_destination(output, synthetic_destination_index);\n"
                              f"            transition_synthetic_atlas({commands}, atlas_destination, true);")
        old = "packed_synthesis_fraction(synthetic_output_fraction(output));"
        if block.count(old) != 1 or block.count("                parameters.slice = slice;") != 1:
            raise ValueError("Pinned synthetic-atlas draw parameters differ")
        block = block.replace(old, "packed_synthesis_fraction(synthetic_output_fraction(output)) |\n"
                              "                (shared_synthetic_atlas() ? 0x10U : 0U);")
        block = block.replace("                parameters.slice = slice;",
                              "                const auto output_view = synthetic_output_view(target_views[view_index], slice);\n"
                              "                parameters.slice = slice;")
        block = block.replace("target_views[view_index].image_rect", "output_view.image_rect")
        block = block.replace("rtv, target_views[view_index]", "rtv, output_view")
        block = block[:-1] + f"    transition_synthetic_atlas({commands}, atlas_destination, false);\n        }}"
        modified = modified[:start] + block + modified[end:]
        offset = start + len(block)
    header_path.write_text(header, encoding="utf-8")
    (output / "synthetic-atlas-header.patch").write_text("".join(difflib.unified_diff(
        original_header.splitlines(keepends=True), header.splitlines(keepends=True),
        fromfile="current-output/include/xrfg/d3d12_frame_synthesizer.hpp",
        tofile="compiled/include/xrfg/d3d12_frame_synthesizer.hpp")), encoding="utf-8")
    return modified


def prepare_shared_nvidia_3x(output: Path, modified: str) -> str:
    """Private medium-NVIDIA atlas opt-in; every legacy composition stays available."""
    header_path = output / "xrfg/d3d12_frame_synthesizer.hpp"
    original_header = header_path.read_text(encoding="utf-8")
    old = "            D3D12SyntheticOutputLayout::native) noexcept;"
    if original_header.count(old) != 1:
        raise ValueError("Pinned shared-NVIDIA header context differs")
    header = original_header.replace(old,
        "            D3D12SyntheticOutputLayout::native,\n"
        "        // Private derivative opt-in; medium NVIDIA 3x atlases only.\n"
        "        bool enable_shared_nvidia_3x = false) noexcept;")
    changes = [
        ('#include "nvidia_synthesize_midpoint_pixel_shader.hpp"',
         '#include "nvidia_synthesize_midpoint_pixel_shader.hpp"\n'
         '#include "nvidia_shared_3x_synthesize_pixel_shader.hpp"'),
        ("    ComPtr<ID3D12PipelineState> nvidia_graphics_pipeline;",
         "    ComPtr<ID3D12PipelineState> nvidia_graphics_pipeline;\n"
         "    ComPtr<ID3D12PipelineState> nvidia_shared_3x_graphics_pipeline;"),
        ("    D3D12SyntheticOutputLayout synthetic_output_layout{};",
         "    D3D12SyntheticOutputLayout synthetic_output_layout{};\n"
         "    bool enable_shared_nvidia_3x{};"),
        ("        D3D12SyntheticOutputLayout input_synthetic_output_layout) {",
         "        D3D12SyntheticOutputLayout input_synthetic_output_layout,\n"
         "        bool input_enable_shared_nvidia_3x) {"),
        ("        synthetic_output_layout = input_synthetic_output_layout;",
         "        synthetic_output_layout = input_synthetic_output_layout;\n"
         "        enable_shared_nvidia_3x = input_enable_shared_nvidia_3x;"),
        ("    D3D12SyntheticOutputLayout synthetic_output_layout) noexcept {",
         "    D3D12SyntheticOutputLayout synthetic_output_layout,\n"
         "    bool enable_shared_nvidia_3x) noexcept {"),
        ("            synthetic_output_layout);\n        if (FAILED(result)) {",
         "            synthetic_output_layout,\n            enable_shared_nvidia_3x);\n        if (FAILED(result)) {"),
    ]
    for old, new in changes:
        if modified.count(old) != 1:
            raise ValueError(f"Pinned shared-NVIDIA context differs: {old!r}")
        modified = modified.replace(old, new)
    old = "            impl_->synthetic_output_layout);"
    if modified.count(old) != 2:
        raise ValueError("Pinned shared-NVIDIA reconfiguration contexts differ")
    modified = modified.replace(old,
        "            impl_->synthetic_output_layout, impl_->enable_shared_nvidia_3x);")
    old = """        return device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(
                nvidia_bidirectional_graphics_pipeline.GetAddressOf()));"""
    new = """        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(
                nvidia_bidirectional_graphics_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;
        if (enable_shared_nvidia_3x && shared_synthetic_atlas() &&
            backend == D3D12OpticalFlowBackend::nvidia &&
            nvidia_options.preset == D3D12NvidiaPerformancePreset::medium &&
            !nvidia_options.bidirectional) {
            graphics_description.PS = {
                g_xrfg_nvidia_shared_3x_synthesize_pixel_shader,
                sizeof(g_xrfg_nvidia_shared_3x_synthesize_pixel_shader),
            };
            graphics_description.NumRenderTargets = 2;
            graphics_description.RTVFormats[1] = view_format;
            graphics_description.BlendState.RenderTarget[1] = target_blend;
            // This is optional. Unsupported MRT creation keeps the already
            // validated single-output PSO and the ordinary submission path.
            if (FAILED(device->CreateGraphicsPipelineState(
                    &graphics_description,
                    IID_PPV_ARGS(nvidia_shared_3x_graphics_pipeline.GetAddressOf())))) {
                nvidia_shared_3x_graphics_pipeline.Reset();
            }
        }
        return S_OK;"""
    if modified.count(old) != 1:
        raise ValueError("Pinned shared-NVIDIA PSO context differs")
    modified = modified.replace(old, new)
    old = """        synthesis->SetPipelineState(
            nvidia_options.bidirectional
                ? nvidia_bidirectional_graphics_pipeline.Get()
                : nvidia_graphics_pipeline.Get());"""
    new = """        const bool shared_3x = enable_shared_nvidia_3x && shared_synthetic_atlas() &&
            backend == D3D12OpticalFlowBackend::nvidia &&
            nvidia_options.preset == D3D12NvidiaPerformancePreset::medium &&
            !nvidia_options.bidirectional && nvidia_shared_3x_graphics_pipeline &&
            (parameters.synthesis_flags & 1U) == 0U && parameters.use_game_motion == 0U &&
            synthetic_output_count() == 2U &&
            synthetic_destinations[synthetic_destination_index].resource.Get() !=
                synthetic_destinations[*extra_synthetic_destination].resource.Get();
        synthesis->SetPipelineState(shared_3x
            ? nvidia_shared_3x_graphics_pipeline.Get()
            : (nvidia_options.bidirectional
                ? nvidia_bidirectional_graphics_pipeline.Get()
                : nvidia_graphics_pipeline.Get()));"""
    if modified.count(old) != 1:
        raise ValueError("Pinned shared-NVIDIA pipeline selection differs")
    modified = modified.replace(old, new)
    start = modified.index('        dred_marker(synthesis, "OFXR NVIDIA composition draw");')
    start = modified.index("        for (UINT output = 0; output < synthetic_output_count(); ++output) {", start)
    end = modified.index("\n\n        std::array<D3D12_RESOURCE_BARRIER,", start)
    legacy = modified[start:end]
    shared = """        if (shared_3x) {
            const UINT destinations[2] = {
                synthetic_destination_index, *extra_synthetic_destination,
            };
            // Both atlases must be RT before either eye writes either target.
            transition_synthetic_atlas(synthesis, destinations[0], true);
            transition_synthetic_atlas(synthesis, destinations[1], true);
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(0)) | 0x10U;
            // This slot is unused by normal NVIDIA. Preserve the exact
            // independent packing/zero-sentinel rules for the second phase.
            parameters.game_motion_padding = packed_synthesis_fraction(synthetic_output_fraction(1));
            for (UINT view_index = 0;
                 view_index < static_cast<UINT>(target_views.size()); ++view_index) {
                const UINT slice = resolved_array_slice(target_views[view_index], view_index,
                    target_views.size(), image_description.DepthOrArraySize);
                const auto output_view = synthetic_output_view(target_views[view_index], slice);
                parameters.slice = slice;
                parameters.view_index = view_index;
                const auto block = offset_gpu_handle(gpu_start,
                    slice * kDescriptorBlockSize, descriptor_increment);
                synthesis->SetGraphicsRootDescriptorTable(0, block);
                synthesis->SetGraphicsRootDescriptorTable(1,
                    offset_gpu_handle(block, kSrvDescriptorCount, descriptor_increment));
                synthesis->SetGraphicsRoot32BitConstants(2, kSynthesisConstantCount, &parameters, 0);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {
                    offset_cpu_handle(rtv_start,
                        destinations[0] * image_description.DepthOrArraySize + slice, rtv_increment),
                    offset_cpu_handle(rtv_start,
                        destinations[1] * image_description.DepthOrArraySize + slice, rtv_increment),
                };
                synthesis->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
                set_viewport_and_scissor(synthesis, output_view.image_rect,
                    static_cast<UINT>(image_description.Width), image_description.Height);
                synthesis->DrawInstanced(3, 1, 0, 0);
                for (const auto rtv : rtvs) {
                    clear_synthetic_marker(synthesis, rtv, output_view,
                        static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
                }
            }
            transition_synthetic_atlas(synthesis, destinations[0], false);
            transition_synthetic_atlas(synthesis, destinations[1], false);
        } else {
""" + "\n".join("    " + line for line in legacy.split("\n")) + "\n        }"
    modified = modified[:start] + shared + modified[end:]
    header_path.write_text(header, encoding="utf-8")
    (output / "shared-nvidia-3x-header.patch").write_text("".join(difflib.unified_diff(
        original_header.splitlines(keepends=True), header.splitlines(keepends=True),
        fromfile="synthetic-atlas/include/xrfg/d3d12_frame_synthesizer.hpp",
        tofile="compiled/include/xrfg/d3d12_frame_synthesizer.hpp")), encoding="utf-8")
    return modified


def shared_nvidia_3x_shader() -> str:
    """Separate entry point: retain the trusted single-output shader verbatim."""
    return """
// Only the private medium-NVIDIA, nonrepeated atlas path selects this PS.
// Keep the single-output entry points above unchanged for differential replay.
struct NvidiaSharedSynthesisCommon {
    float2 pixel;
    CameraSample current_fallback;
    CameraSample previous_fallback;
    float2 raw_backward;
    float backward_flow_support;
    float camera_only_error;
    bool valid;
};

NvidiaSharedSynthesisCommon prepare_nvidia_shared_synthesis(FullscreenVertex input) {
    NvidiaSharedSynthesisCommon common = (NvidiaSharedSynthesisCommon)0;
    common.current_fallback.color = float4(0.0, 0.0, 0.0, 1.0);
    uint2 integer_pixel = uint2(input.position.xy);
    if ((SynthesisFlags & 0x10u) != 0u)
        integer_pixel.x -= Slice * Width;
    bool in_bounds = integer_pixel.x < Width && integer_pixel.y < Height && Slice < ArraySize;
    if (in_bounds) {
        common.pixel = float2(integer_pixel);
        common.current_fallback = sample_current_target(common.pixel, Slice, ViewIndex);
        MappedCoordinate previous_coverage = map_target_to_source(
            common.pixel, PreviousMappings[ViewIndex]);
        if (previous_coverage.valid >= 0.5) {
            NvidiaFlowSample reconstructed = nvidia_confidence_flow_for_pixel(
                common.pixel, Slice, ViewIndex, 1.0 / 32.0);
            common.raw_backward = reconstructed.motion;
            common.backward_flow_support = reconstructed.support;
            common.previous_fallback = sample_previous_target(common.pixel, Slice, ViewIndex);
            common.camera_only_error = rgb_error(
                pack_flow_input_color(common.previous_fallback.color),
                pack_flow_input_color(common.current_fallback.color));
            common.valid = true;
        }
    }
    return common;
}

float4 synthesize_nvidia_shared_phase(NvidiaSharedSynthesisCommon common, float fraction) {
    float4 output_color = saturate(common.current_fallback.color);
    if (common.valid) {
        CameraSample previous_sample = sample_previous_target(
            common.pixel + common.raw_backward * fraction, Slice, ViewIndex);
        CameraSample current_sample = sample_current_target(
            common.pixel - common.raw_backward * (1.0 - fraction), Slice, ViewIndex);
        if (previous_sample.valid >= 0.5 && current_sample.valid >= 0.5) {
            float4 flow_midpoint = lerp(previous_sample.color, current_sample.color, fraction);
            float disagreement = max(abs(previous_sample.color.r - current_sample.color.r),
                max(abs(previous_sample.color.g - current_sample.color.g),
                    abs(previous_sample.color.b - current_sample.color.b)));
            float confidence = saturate(1.0 - disagreement * 6.0);
            float4 stable_midpoint = lerp(
                common.previous_fallback.color, common.current_fallback.color, fraction);
            float consistency_confidence = 1.0;
            float cost_confidence = common.backward_flow_support;
            confidence *= consistency_confidence * cost_confidence;
            // Alpha retains the original stable/flow policy for each phase.
            output_color = saturate(lerp(stable_midpoint, flow_midpoint, confidence));
            float warped_error = rgb_error(
                pack_flow_input_color(previous_sample.color), pack_flow_input_color(current_sample.color));
            float rgb_confidence = saturate(1.0 + (common.camera_only_error - warped_error) * 8.0);
            rgb_confidence *= cost_confidence;
            output_color.rgb = saturate(lerp(
                common.current_fallback.color.rgb, current_sample.color.rgb, rgb_confidence));
        }
    }
    return output_color;
}

struct NvidiaShared3xTargets {
    float4 first : SV_Target0;
    float4 second : SV_Target1;
};

NvidiaShared3xTargets SynthesizeNvidiaShared3xPS(FullscreenVertex input) {
    NvidiaSharedSynthesisCommon common = prepare_nvidia_shared_synthesis(input);
    uint second_packed = (GameMotionPadding >> 8) & 0xffffu;
    float second_fraction = second_packed == 0u ? 0.5 : float(second_packed) / 65535.0;
    NvidiaShared3xTargets output;
    output.first = synthesize_nvidia_shared_phase(common, synthesis_fraction());
    output.second = synthesize_nvidia_shared_phase(common, second_fraction);
    return output;
}
"""


def prepare_four_frame_outputs(output: Path, modified: str) -> str:
    """Bounded V558 extra outputs on the retained derivative, with 3X MRT intact."""
    header_path = output / "xrfg/d3d12_frame_synthesizer.hpp"
    header = header_path.read_text(encoding="utf-8")
    start = header.index("    [[nodiscard]] HRESULT submit_pair(")
    end = header.index(") noexcept;", start) + len(") noexcept;")
    declaration = header[start:end]
    # Keep the optional one-extra source contract used by retained tests.
    # The new explicitly named overload carries up to two bounded outputs.
    span_declaration = declaration.replace("submit_pair(", "submit_pair_outputs(")
    span_declaration = span_declaration.replace(" = std::nullopt", "").replace(" = {}", "")
    span_declaration = span_declaration.replace(" = false", "").replace(" = 0.5F", "")
    span_declaration = span_declaration.replace("std::optional<D3D12ExtraSynthetic> extra_synthetic",
                                              "std::span<const D3D12ExtraSynthetic> extra_synthetics")
    header = header[:end] + "\n\n" + span_declaration + header[end:]
    header_path.write_text(header, encoding="utf-8")
    replacements = [
        ("    std::optional<std::uint32_t> extra_synthetic_destination;\n    float extra_synthetic_fraction{0.5F};",
         "    static constexpr UINT kMaxExtraSynthetics = 2;\n"
         "    std::array<std::uint32_t, kMaxExtraSynthetics> extra_synthetic_destinations{};\n"
         "    std::array<float, kMaxExtraSynthetics> extra_synthetic_fractions{};\n"
         "    UINT extra_synthetic_count{};"),
        ("return extra_synthetic_destination ? 2U : 1U;", "return 1U + extra_synthetic_count;"),
        ("return output == 0U ? synthetic_fraction : extra_synthetic_fraction;",
         "return output == 0U ? synthetic_fraction : extra_synthetic_fractions[output - 1U];"),
        ("return output == 0U ? first_destination : *extra_synthetic_destination;",
         "return output == 0U ? first_destination : extra_synthetic_destinations[output - 1U];"),
        ("const std::optional<D3D12ExtraSynthetic>& extra_synthetic) noexcept {",
         "std::span<const D3D12ExtraSynthetic> extra_synthetics) noexcept {"),
        ("        extra_synthetic_destination.reset();", "        extra_synthetic_count = 0;"),
        ("""        if (extra_synthetic) {
            if (extra_synthetic->destination_index >= synthetic_destinations.size() ||
                extra_synthetic->destination_index == synthetic_destination_index) {
                return E_INVALIDARG;
            }
            extra_synthetic_destination = extra_synthetic->destination_index;
            extra_synthetic_fraction =
                usable_fraction(extra_synthetic->interpolation_fraction);
        }""", """        if (extra_synthetics.size() > kMaxExtraSynthetics) {
            return E_INVALIDARG;
        }
        for (const auto& extra : extra_synthetics) {
            if (extra.destination_index >= synthetic_destinations.size() ||
                extra.destination_index == synthetic_destination_index) {
                return E_INVALIDARG;
            }
            for (UINT earlier = 0; earlier < extra_synthetic_count; ++earlier) {
                if (extra_synthetic_destinations[earlier] == extra.destination_index) {
                    return E_INVALIDARG;
                }
            }
            extra_synthetic_destinations[extra_synthetic_count] = extra.destination_index;
            extra_synthetic_fractions[extra_synthetic_count] = usable_fraction(extra.interpolation_fraction);
            ++extra_synthetic_count;
        }"""),
        ("""        if (extra_synthetic_destination) {
            result = destination_available(
                synthetic_destinations[*extra_synthetic_destination]);
            if (FAILED(result)) {
                return result;
            }
        }
        ID3D12Resource* const extra_destination = extra_synthetic_destination
            ? synthetic_destinations[*extra_synthetic_destination].resource.Get()
            : nullptr;""", """        for (UINT extra = 0; extra < extra_synthetic_count; ++extra) {
            result = destination_available(synthetic_destinations[extra_synthetic_destinations[extra]]);
            if (FAILED(result)) return result;
        }
        const auto aliases_extra = [&](ID3D12Resource* resource) {
            for (UINT extra = 0; extra < extra_synthetic_count; ++extra)
                if (resource == synthetic_destinations[extra_synthetic_destinations[extra]].resource.Get())
                    return true;
            return false;
        };"""),
        ("repeated_resource == extra_destination", "aliases_extra(repeated_resource)"),
        ("""(extra_destination != nullptr &&
             (previous.resource.Get() == extra_destination ||
              next.resource.Get() == extra_destination))""",
         "(aliases_extra(previous.resource.Get()) || aliases_extra(next.resource.Get()))"),
    ]
    for old, new in replacements:
        if modified.count(old) != 1:
            raise ValueError(f"Pinned 4X context differs: {old!r}")
        modified = modified.replace(old, new)
    for indent in ("        ", "            "):
        old = (indent + "if (extra_synthetic_destination) {\n" + indent +
               "    synthetic_destinations[*extra_synthetic_destination].fence_value =\n" + indent +
               "        fence_value;\n" + indent + "}")
        if modified.count(old) != 1:
            raise ValueError("Pinned 4X completion context differs")
        modified = modified.replace(old, indent + "for (UINT extra = 0; extra < extra_synthetic_count; ++extra) {\n" +
            indent + "    synthetic_destinations[extra_synthetic_destinations[extra]].fence_value = fence_value;\n" + indent + "}")
    # MRT accesses the first extra only behind synthetic_output_count()==2.
    modified = modified.replace("*extra_synthetic_destination", "extra_synthetic_destinations[0]")
    start = modified.index("HRESULT D3D12FrameSynthesizer::submit_pair(")
    end = modified.index("\nHRESULT D3D12FrameSynthesizer::flush_current_copy", start)
    original = modified[start:end]
    bounded = original.replace("::submit_pair(", "::submit_pair_outputs(", 1).replace(
        "std::optional<D3D12ExtraSynthetic> extra_synthetic)",
        "std::span<const D3D12ExtraSynthetic> extra_synthetics)").replace(
        "            extra_synthetic);", "            extra_synthetics);")
    signature = original[:original.index("    try {")]
    wrapper = signature + """    const std::span<const D3D12ExtraSynthetic> extras = extra_synthetic
        ? std::span<const D3D12ExtraSynthetic>(&*extra_synthetic, 1)
        : std::span<const D3D12ExtraSynthetic>{};
    return submit_pair_outputs(current, current_source_views, synthetic_target_views,
        synthetic_destination_index, current_destination_index, ticket, debug_marker,
        std::move(motion_vectors), defer_current_copy, interpolation_fraction, extras);
}
"""
    modified = modified[:start] + wrapper + "\n" + bounded + modified[end:]
    return modified


def prepare_linear_clamp_root(modified: str) -> str:
    """Backport only V477's zero-root-DWORD linear-clamp static sampler."""
    old = ("        root_description.NumStaticSamplers = 0;\n"
           "        root_description.pStaticSamplers = nullptr;")
    new = """        // Static samplers do not consume the full 64-DWORD root budget.
        D3D12_STATIC_SAMPLER_DESC linear_clamp{};
        linear_clamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        linear_clamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linear_clamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linear_clamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linear_clamp.MaxAnisotropy = 1;
        linear_clamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        linear_clamp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        linear_clamp.MinLOD = 0.0F;
        linear_clamp.MaxLOD = 0.0F;
        linear_clamp.ShaderRegister = 0;
        linear_clamp.RegisterSpace = 0;
        linear_clamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_description.NumStaticSamplers = 1;
        root_description.pStaticSamplers = &linear_clamp;"""
    if modified.count(old) != 1:
        raise ValueError("Pinned static sampler root context does not match uniquely")
    return modified.replace(old, new)


def prepare_linear_clamp_shader(shader: str) -> str:
    """Replace manual source taps while retaining caller crop and slice policy."""
    declaration = "Texture2DArray<float4> PreviousFrame : register(t0);"
    if shader.count(declaration) != 1:
        raise ValueError("Pinned sampler declaration context does not match uniquely")
    shader = shader.replace(declaration, declaration + "\nSamplerState LinearClamp : register(s0);")
    for endpoint, resource in (("previous", "PreviousFrame"), ("current", "CurrentFrame")):
        signature = f"float4 bilinear_{endpoint}_source(float2 coordinate, uint slice) {{"
        if shader.count(signature) != 1:
            raise ValueError("Pinned bilinear helper does not match uniquely")
        start = shader.index(signature)
        end = shader.index("\n}", start) + len("\n}")
        if shader[start:end].count(f"{resource}.Load(") != 4:
            raise ValueError("Pinned bilinear helper no longer has four manual taps")
        replacement = signature + f"""
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    // Source coordinates are pixel centers in an eye-local array slice.
    // Caller clamps to active SourceRect remain in force before normalization.
    float2 uv = (bounded + 0.5) / float2(float(Width), float(Height));
    return {resource}.SampleLevel(LinearClamp, float3(uv, float(slice)), 0.0);
}}"""
        shader = shader[:start] + replacement + shader[end:]
    return shader


def prepare(source: Path, output: Path) -> None:
    manifest = json.loads((source / "manifest.json").read_text())
    if manifest["commit"] != "07449429c139e5d2070b6f0855ed5323de7cee00":
        raise ValueError("Unsupported OFXR source pin")
    for name, digest in manifest["files"].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"Modified immutable vendor source: {name}")
    prepare_history(source, output)
    path = source / "src/d3d12/d3d12_frame_synthesizer.cpp"
    original = path.read_text(encoding="utf-8")
    modified = original
    changes = [
        ("    bool completion_unknown{};", "    bool completion_unknown{};\n    bool initialization_quarantined{};"),
        ("    [[nodiscard]] HRESULT wait_for_idle() noexcept {\n        // last_submitted_fence_value",
         "    [[nodiscard]] HRESULT wait_for_idle() noexcept {\n"
         "        // A failed initialization may still own asynchronous OFA registration.\n"
         "        // Poll it without entering the ordinary unbounded shutdown wait.\n"
         "        if (initialization_quarantined && nvidia_fence && last_nvidia_fence_value &&\n"
         "            fence_status(nvidia_fence.Get(), last_nvidia_fence_value) != S_OK) {\n"
         "            return HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE);\n"
         "        }\n        // last_submitted_fence_value"),
        ("            enable_nvidia_gpu_timing);\n        if (FAILED(result)) {\n            return result;\n        }\n        impl_ = std::move(candidate);",
         "            enable_nvidia_gpu_timing);\n        if (FAILED(result)) {\n"
         "            // Do not destroy an initialization candidate while the driver owns\n"
         "            // a pending registration, even when its completion event failed.\n"
         "            if (candidate->nvidia_fence && candidate->last_nvidia_fence_value &&\n"
         "                candidate->fence_status(candidate->nvidia_fence.Get(), candidate->last_nvidia_fence_value) != S_OK) {\n"
         "                candidate->initialization_quarantined = true;\n"
         "                impl_ = std::move(candidate);\n"
         "                return HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE);\n"
         "            }\n            return result;\n        }\n        impl_ = std::move(candidate);"),
    ]
    for old, new in changes:
        if modified.count(old) != 1:
            raise ValueError("Pinned ownership patch context does not match uniquely")
        modified = modified.replace(old, new)

    # History invalidation does not recreate GPU resources. Keep the packed
    # input in the same read state after allocation and every dispatch, and
    # tell the SDK the actual resting state of previously consumed outputs.
    state_changes = [
        ("            &packed_description,\n"
         "            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,",
         "            &packed_description,\n"
         "            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"),
        ("        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{\n"
         "            transition_barrier(\n"
         "                source.resource.Get(),\n"
         "                D3D12_RESOURCE_STATE_COMMON,\n"
         "                kShaderReadState),",
         "        const std::array<D3D12_RESOURCE_BARRIER, 3> before_copy{\n"
         "            transition_barrier(\n"
         "                packed_color.Get(),\n"
         "                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,\n"
         "                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),\n"
         "            transition_barrier(\n"
         "                source.resource.Get(),\n"
         "                D3D12_RESOURCE_STATE_COMMON,\n"
         "                kShaderReadState),"),
        ("            const HRESULT seed_result = dispatch_fidelityfx(\n"
         "                slot.command_list.Get(),\n"
         "                FFX_RESOURCE_STATE_UNORDERED_ACCESS);",
         "            const HRESULT seed_result = dispatch_fidelityfx(\n"
         "                slot.command_list.Get(),\n"
         "                flow_output_consumed ? FFX_RESOURCE_STATE_PIXEL_READ\n"
         "                                     : FFX_RESOURCE_STATE_UNORDERED_ACCESS);"),
    ]
    for old, new in state_changes:
        if modified.count(old) != 1:
            raise ValueError("Pinned FidelityFX state patch context does not match uniquely")
        modified = modified.replace(old, new)

    # A work slot may retain an older completed cache as well as a new pending
    # sample. Only complete, resolved samples may publish timing metadata.
    timing_changes = [
        ("    [[nodiscard]] HRESULT reset_work_slot(WorkSlot& slot) noexcept {\n",
         "    [[nodiscard]] HRESULT reset_work_slot(WorkSlot& slot) noexcept {\n"
         "        slot.timing_metadata = {};\n"
         "        slot.timing_pending = false;\n"),
        ("        slot.timing_pending = nvidia_gpu_timing_enabled;\n"
         "        last_submitted_fence_value = value;",
         "        slot.timing_pending = nvidia_gpu_timing_enabled &&\n"
         "            slot.timing_metadata.previous_serial != 0;\n"
         "        last_submitted_fence_value = value;"),
        ("        slot.timing_pending = nvidia_gpu_timing_enabled;\n"
         "        last_submitted_fence_value = completion_value;",
         "        slot.timing_pending = nvidia_gpu_timing_enabled &&\n"
         "            slot.timing_metadata.previous_serial != 0;\n"
         "        last_submitted_fence_value = completion_value;"),
        ("                debug_marker);\n        }\n"
         "        ID3D12Resource* const previous_resource = previous_source.resource.Get();",
         "                debug_marker);\n        }\n"
         "        if (nvidia_gpu_timing_enabled) {\n"
         "            slot.timing_metadata.previous_serial = previous_source.ticket.serial;\n"
         "            slot.timing_metadata.current_serial = current_source.ticket.serial;\n"
         "            slot.timing_metadata.eye_count = image_description.DepthOrArraySize;\n"
         "        }\n"
         "        ID3D12Resource* const previous_resource = previous_source.resource.Get();"),
        ("            // Only this path records a resolve of its own; the NVIDIA path\n"
         "            // resolves on its synthesis list. Without one the span marks\n"
         "            // never reach the readback buffer at all.\n"
         "            slot.command_list->ResolveQueryData(\n"
         "                nvidia_timestamp_heap.Get(),\n"
         "                D3D12_QUERY_TYPE_TIMESTAMP,\n"
         "                slot.timing_query_base,\n"
         "                kTimestampCount,\n"
         "                nvidia_timestamp_readback.Get(),\n"
         "                static_cast<UINT64>(slot.timing_query_base) *\n"
         "                    sizeof(std::uint64_t));",
         "            // FidelityFX writes only the two span timestamps. Resolving\n"
         "            // the unwritten NVIDIA stage queries is undefined behavior.\n"
         "            slot.command_list->ResolveQueryData(\n"
         "                nvidia_timestamp_heap.Get(),\n"
         "                D3D12_QUERY_TYPE_TIMESTAMP,\n"
         "                slot.timing_query_base + kSpanBegin,\n"
         "                2,\n"
         "                nvidia_timestamp_readback.Get(),\n"
         "                static_cast<UINT64>(slot.timing_query_base + kSpanBegin) *\n"
         "                    sizeof(std::uint64_t));"),
    ]
    for old, new in timing_changes:
        if modified.count(old) != 1:
            raise ValueError("Pinned timing patch context does not match uniquely")
        modified = modified.replace(old, new)

    start = modified.index("    [[nodiscard]] HRESULT harvest_nvidia_gpu_timing(")
    end = modified.index("    [[nodiscard]] HRESULT consume_nvidia_gpu_timing(", start)
    harvest = modified[start:end]
    harvest_changes = [
        ("        if (status != S_OK) {\n            return status;\n        }",
         "        if (status != S_OK) {\n"
         "            if (FAILED(status)) slot.timing_pending = false;\n"
         "            return status;\n        }"),
        ("        const SIZE_T byte_begin =\n"
         "            static_cast<SIZE_T>(slot.timing_query_base) *\n"
         "            sizeof(std::uint64_t);\n"
         "        const SIZE_T byte_end = byte_begin +\n"
         "            kTimestampCount * sizeof(std::uint64_t);",
         "        const UINT first = backend == D3D12OpticalFlowBackend::nvidia ? 0 : kSpanBegin;\n"
         "        const UINT count = backend == D3D12OpticalFlowBackend::nvidia ? kTimestampCount : 2;\n"
         "        const SIZE_T byte_begin =\n"
         "            static_cast<SIZE_T>(slot.timing_query_base + first) * sizeof(std::uint64_t);\n"
         "        const SIZE_T byte_end = byte_begin + count * sizeof(std::uint64_t);"),
        ("        std::copy_n(timestamps, values.size(), values.begin());",
         "        std::copy_n(timestamps + first, count, values.begin() + first);"),
        ("        slot.cached_timing = slot.timing_metadata;",
         "        if (values[kSpanBegin] > values[kSpanEnd]) {\n"
         "            slot.timing_pending = false;\n"
         "            return E_FAIL;\n        }\n"
         "        // Publish only after validation, preserving any older valid cache\n"
         "        // if this pending sample has failed.\n"
         "        auto timing = slot.timing_metadata;"),
    ]
    for old, new in harvest_changes:
        if harvest.count(old) != 1:
            raise ValueError("Pinned timing harvest context does not match uniquely")
        harvest = harvest.replace(old, new)
    harvest = harvest.replace("slot.cached_timing.", "timing.")
    marker = "            slot.timing_pending = false;\n            slot.timing_cached = true;"
    if harvest.count(marker) != 1:
        raise ValueError("Pinned FidelityFX timing publication context does not match uniquely")
    harvest = harvest.replace(marker, "            slot.cached_timing = timing;\n" + marker)
    marker = "        slot.timing_pending = false;\n        slot.timing_cached = true;"
    if harvest.count(marker) != 1:
        raise ValueError("Pinned NVIDIA timing publication context does not match uniquely")
    harvest = harvest.replace(marker, "        slot.cached_timing = timing;\n" + marker)
    modified = modified[:start] + harvest + modified[end:]
    # Allocation padding is not the optical-flow sampling scale. Carry the
    # nominal ratio independently in unused low flag bits, including extra S2
    # draws (which already preserve the low byte when replacing the fraction).
    marker = "// Packs the synthetic's position between the two captures into the spare"
    scale_helper = (
        "// Bits 2-3 carry the nominal scale; zero retains legacy shader fixtures.\n"
        "[[nodiscard]] constexpr UINT packed_flow_input_scale(D3D12OpticalFlowInputScale scale) noexcept {\n"
        "    switch (scale) {\n"
        "    case D3D12OpticalFlowInputScale::half: return 3U << 2U;\n"
        "    case D3D12OpticalFlowInputScale::three_quarter: return 2U << 2U;\n"
        "    default: return 1U << 2U;\n"
        "    }\n"
        "}\n\n")
    if modified.count(marker) != 1:
        raise ValueError("Pinned flow scale helper context does not match uniquely")
    modified = modified.replace(marker, scale_helper + marker)
    marker = "parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction)"
    if modified.count(marker) != 5:
        raise ValueError("Pinned flow scale producers do not match")
    modified = modified.replace(marker, marker + " |\n            packed_flow_input_scale(nvidia_options.input_scale)")
    output.mkdir(parents=True, exist_ok=True)
    modified = prepare_pair_real_output(source, output, modified)
    modified = prepare_synthetic_atlas_output(output, modified)
    modified = prepare_shared_nvidia_3x(output, modified)
    modified = prepare_four_frame_outputs(output, modified)
    modified = prepare_early_readiness(output, modified)
    modified = prepare_linear_clamp_root(modified)
    (output / path.name).write_text(modified, encoding="utf-8")
    (output / "initialization-ownership.patch").write_text("".join(difflib.unified_diff(
        original.splitlines(keepends=True), modified.splitlines(keepends=True),
        fromfile="upstream/src/d3d12/d3d12_frame_synthesizer.cpp",
        tofile="compiled/src/d3d12/d3d12_frame_synthesizer.cpp")), encoding="utf-8")
    shader_path = source / "src/d3d12/frame_synthesis.hlsl"
    original_shader = shader_path.read_text(encoding="utf-8")
    shader = original_shader
    old = "    uint2 integer_pixel = uint2(input.position.xy);"
    new = (old + "\n"
           "    // Shared synthetic atlases retain the array history's eye-local domain.\n"
           "    // Default/native layouts keep their original pixel calculation.\n"
           "    if ((SynthesisFlags & 0x10u) != 0u)\n"
           "        integer_pixel.x -= Slice * Width;")
    if shader.count(old) != 1:
        raise ValueError("Pinned synthetic-atlas shader coordinate context differs")
    shader = shader.replace(old, new)
    helper = (Path(__file__).resolve().parents[1] /
              "nr-frame-generation/src/ffx_endpoint_validation.hlsl").read_text(encoding="utf-8")
    marker = "// Preserve a stationary neighbourhood after the existing XR camera mapping."
    if shader.count(marker) != 1:
        raise ValueError("Pinned endpoint helper context does not match uniquely")
    shader = shader.replace(marker, helper + "\n" + marker)
    flow_start = shader.index("float2 flow_for_pixel(")
    flow_end = shader.index("float2 forward_flow_for_pixel(", flow_start)
    flow = shader[flow_start:flow_end]
    old_limit = "    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;"
    new_limit = (
        "    // FFX raw flow still includes head rotation. Limit only residual\n"
        "    // scene motion after converting it into the target camera below.\n"
        "    return use_nvidia_input_scale && magnitude > 512.0\n"
        "        ? result * (512.0 / magnitude) : result;")
    if flow.count(old_limit) != 1:
        raise ValueError("Pinned FidelityFX raw-flow limit context does not match uniquely")
    shader = shader[:flow_start] + flow.replace(old_limit, new_limit) + shader[flow_end:]
    old_residual = (
        "                    float2 residual_backward = input_pose_compensated\n"
        "                        ? raw_backward\n"
        "                        : raw_backward - (previous_coverage.coordinate - pixel);")
    new_residual = (
        "                    float2 residual_backward = input_pose_compensated\n"
        "                        ? raw_backward\n"
        "                        : raw_backward - (previous_coverage.coordinate - pixel);\n"
        "                    if (!input_pose_compensated && repeated_capture_flag() == 0) {\n"
        "                        // FFX flow ends in A's source camera. Convert the\n"
        "                        // endpoint to B's camera before subtracting pixels.\n"
        "                        MappedCoordinate endpoint = map_source_to_target(\n"
        "                            pixel + raw_backward, PreviousMappings[ViewIndex]);\n"
        "                        endpoints_valid = endpoint.valid >= 0.5;\n"
        "                        residual_backward = endpoint.coordinate - pixel;\n"
        "                        float magnitude = length(residual_backward);\n"
        "                        residual_backward = magnitude > 512.0\n"
        "                            ? residual_backward * (512.0 / magnitude) : residual_backward;\n"
        "                    }")
    if shader.count(old_residual) != 1:
        raise ValueError("Pinned FidelityFX camera conversion context does not match uniquely")
    shader = shader.replace(old_residual, new_residual)
    # Only normal NVIDIA reconstructs motion from cell reliability. Keep the
    # original functions for FidelityFX, Fast and bidirectional entry points.
    confidence_helper = """struct NvidiaFlowSample {
    float2 motion;
    float support;
};

NvidiaFlowSample nvidia_confidence_flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale) {
    float2 scaled = flow_input_coordinate(pixel);
    float2 packed_coordinate = float2(
        scaled.x, scaled.y + float(slice * PackedEyeStride));
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect, slice);
    float2 bounded = clamp(
        flow_coordinate, float2(bounds.minimum), float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 v00 = load_backward_flow_clamped(top_left, bounds.minimum, bounds.maximum);
    float2 v10 = load_backward_flow_clamped(
        int2(bottom_right.x, top_left.y), bounds.minimum, bounds.maximum);
    float2 v01 = load_backward_flow_clamped(
        int2(top_left.x, bottom_right.y), bounds.minimum, bounds.maximum);
    float2 v11 = load_backward_flow_clamped(bottom_right, bounds.minimum, bounds.maximum);
    uint c00 = FlowAuxiliary.Load(int3(top_left, 0)) & 0xffU;
    // Exact-zero spatial contributors do not affect reliability or uniformity.
    uint c10 = fraction.x > 0.0
        ? FlowAuxiliary.Load(int3(bottom_right.x, top_left.y, 0)) & 0xffU : c00;
    uint c01 = fraction.y > 0.0
        ? FlowAuxiliary.Load(int3(top_left.x, bottom_right.y, 0)) & 0xffU : c00;
    uint c11 = fraction.x > 0.0 && fraction.y > 0.0
        ? FlowAuxiliary.Load(int3(bottom_right, 0)) & 0xffU : c00;
    float2 top = lerp(v00, v10, fraction.x);
    float2 bottom = lerp(v01, v11, fraction.x);
    float2 result = lerp(top, bottom, fraction.y);
    NvidiaFlowSample sample;
    sample.support = saturate(1.0 - float(c00) / 255.0);
    // Retain AAF7 arithmetic and endpoint validity for every uniform cost,
    // including zero support: zeroing that vector would change alpha fallback.
    if (c00 != c10 || c00 != c01 || c00 != c11) {
        float q00 = sample.support;
        float q10 = saturate(1.0 - float(c10) / 255.0);
        float q01 = saturate(1.0 - float(c01) / 255.0);
        float q11 = saturate(1.0 - float(c11) / 255.0);
        sample.support = lerp(
            lerp(q00, q10, fraction.x), lerp(q01, q11, fraction.x), fraction.y);
        if (sample.support > 0.0) {
            result = lerp(
                lerp(v00 * q00, v10 * q10, fraction.x),
                lerp(v01 * q01, v11 * q11, fraction.x), fraction.y) / sample.support;
        }
    }
    result *= value_scale;
    result /= max(flow_input_scale(), float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    sample.motion = magnitude > 512.0 ? result * (512.0 / magnitude) : result;
    return sample;
}

"""
    marker = "float2 flow_for_pixel("
    if shader.count(marker) != 1:
        raise ValueError("Pinned NVIDIA confidence helper context does not match uniquely")
    shader = shader.replace(marker, confidence_helper + marker)
    old = "                float2 raw_backward = float2(0.0, 0.0);"
    new = (old + "\n"
           "                bool use_nvidia_vector_confidence = use_nvidia_cost && !validate_fast && !use_nvidia_bidirectional;\n"
           "                float backward_flow_support = 1.0;")
    if shader.count(old) != 1:
        raise ValueError("Pinned NVIDIA confidence state context does not match uniquely")
    shader = shader.replace(old, new)
    old = ("                    raw_backward = repeated_capture_flag() != 0\n"
           "                        ? float2(0.0, 0.0)\n"
           "                        : flow_for_pixel(\n"
           "                            pixel,\n"
           "                            Slice,\n"
           "                            ViewIndex,\n"
           "                            flow_value_scale,\n"
           "                            use_nvidia_cost);")
    new = ("                    if (use_nvidia_vector_confidence && repeated_capture_flag() == 0) {\n"
           "                        NvidiaFlowSample reconstructed = nvidia_confidence_flow_for_pixel(\n"
           "                            pixel, Slice, ViewIndex, flow_value_scale);\n"
           "                        raw_backward = reconstructed.motion;\n"
           "                        backward_flow_support = reconstructed.support;\n"
           "                    } else {\n" + old + "\n                    }")
    if shader.count(old) != 1:
        raise ValueError("Pinned NVIDIA reconstruction context does not match uniquely")
    shader = shader.replace(old, new)
    old = ("                                cost_confidence = saturate(\n"
           "                                    1.0 - nvidia_cost_for_pixel(\n"
           "                                        pixel,\n"
           "                                        Slice,\n"
           "                                        ViewIndex) / 255.0);")
    new = old.replace("cost_confidence = saturate(",
                      "cost_confidence = use_nvidia_vector_confidence ? backward_flow_support : saturate(")
    if shader.count(old) != 1:
        raise ValueError("Pinned NVIDIA support confidence context does not match uniquely")
    shader = shader.replace(old, new)
    # Reuse the camera-only A sample already read for the original alpha
    # blend. Compare the camera-only and motion-warped RGB errors without
    # adding source samples or changing that alpha calculation.
    old_stable = ("                    float4 stable_midpoint = lerp(\n"
                  "                        sample_previous_target(pixel, Slice, ViewIndex).color,\n"
                  "                        current_fallback.color,\n"
                  "                        synthesis_fraction());")
    new_stable = ("                    CameraSample previous_fallback = sample_previous_target(pixel, Slice, ViewIndex);\n"
                  "                    float4 stable_midpoint = lerp(\n"
                  "                        previous_fallback.color,\n"
                  "                        current_fallback.color,\n"
                  "                        synthesis_fraction());")
    if shader.count(old_stable) != 1:
        raise ValueError("Pinned camera-only sample context does not match uniquely")
    shader = shader.replace(old_stable, new_stable)
    # Normal NVIDIA RGB keeps motion when it explains A/B at least as well
    # as the camera alone, then applies the existing hardware cost guard.
    # Compare in the packed flow color space so dark sRGB errors retain
    # their encoded contrast. Alpha and other branches retain their policy.
    old = ("                        output_color = saturate(lerp(\n"
           "                            stable_midpoint,\n"
           "                            flow_midpoint,\n"
           "                            confidence));")
    new = (old + "\n"
           "                        if (!validate_fast && !use_nvidia_bidirectional && repeated_capture_flag() == 0) {\n"
           "                            float camera_only_error = rgb_error(\n"
           "                                pack_flow_input_color(previous_fallback.color), pack_flow_input_color(current_fallback.color));\n"
           "                            float warped_error = rgb_error(\n"
           "                                pack_flow_input_color(previous_sample.color), pack_flow_input_color(current_sample.color));\n"
           "                            float rgb_confidence = saturate(1.0 + (camera_only_error - warped_error) * 8.0);\n"
           "                            rgb_confidence *= cost_confidence;\n"
           "                            output_color.rgb = saturate(lerp(\n"
           "                                current_fallback.color.rgb, current_sample.color.rgb, rgb_confidence));\n"
           "                        }")
    start = shader.index("                    if (use_nvidia_cost) {", shader.index("float4 synthesize_midpoint("))
    end = shader.index("                    } else {\n                        output_color", start)
    section = shader[start:end]
    if section.count(old) != 1:
        raise ValueError("Pinned NVIDIA newest-image output context does not match uniquely")
    shader = shader[:start] + section.replace(old, new) + shader[end:]
    old = ("                    } else {\n"
           "                        output_color = saturate(lerp(\n"
           "                            stable_midpoint,\n"
           "                            flow_midpoint,\n"
           "                            confidence));\n"
           "                    }")
    new = old.replace(
        "                            confidence));\n",
        "                            confidence));\n"
        "                        if (!use_game_motion_pipeline && repeated_capture_flag() == 0) {\n"
        "                            // A rejected original endpoint uses current B, rather than\n"
        "                            // crossfading unrelated history into disoccluded pixels.\n"
        "                            output_color = saturate(lerp(\n"
        "                                current_fallback.color, output_color,\n"
        "                                fidelityfx_endpoint_confidence(pixel, raw_backward, Slice)));\n"
        "                        }\n")
    if shader.count(old) != 1:
        raise ValueError("Pinned FidelityFX endpoint output context does not match uniquely")
    shader = shader.replace(old, new)
    # Both temporal endpoints must use the same pixel-center domain. The old
    # current sampler admitted an extra half-pixel strip, reading view padding
    # where the previous sampler could not supply a matching endpoint.
    current_start = shader.index("CameraSample sample_current_target(")
    current_end = shader.index("[numthreads", current_start)
    current_sampler = shader[current_start:current_end]
    for old, new in (
            ("target_coordinate.x >= target_minimum.x - 0.5", "target_coordinate.x >= target_minimum.x"),
            ("target_coordinate.y >= target_minimum.y - 0.5", "target_coordinate.y >= target_minimum.y"),
            ("target_coordinate.x <= target_maximum.x - 0.5", "target_coordinate.x <= target_maximum.x - 1.0"),
            ("target_coordinate.y <= target_maximum.y - 0.5", "target_coordinate.y <= target_maximum.y - 1.0")):
        if current_sampler.count(old) != 1:
            raise ValueError("Pinned current endpoint bounds do not match uniquely")
        current_sampler = current_sampler.replace(old, new)
    shader = shader[:current_start] + current_sampler + shader[current_end:]
    # A mapped endpoint can be geometrically valid in the half-pixel edge
    # footprint. Clamp its bilinear sample to this view's active pixel centers,
    # rather than mixing neighboring padding into a valid camera sample.
    old = "        output.color = bilinear_previous_source(mapped.coordinate, slice);"
    new = (
        "        float4 source_rect = PreviousMappings[view_index].SourceRect;\n"
        "        float2 source_coordinate = clamp(mapped.coordinate,\n"
        "            source_rect.xy, source_rect.xy + source_rect.zw - 1.0);\n"
        "        output.color = bilinear_previous_source(source_coordinate, slice);")
    if shader.count(old) != 1:
        raise ValueError("Pinned previous endpoint sample does not match uniquely")
    shader = shader.replace(old, new)
    old = (
        "// The ratio the flow input was packed at, relative to the source image.\n"
        "// One when the input is full resolution. Derived from the width alone and\n"
        "// applied to both axes: the scale is uniform, and the packed height carries\n"
        "// the stacked eyes on the FidelityFX path, so a height ratio would not\n"
        "// describe it.\n"
        "float2 flow_input_scale() {\n"
        "    float ratio = min(\n"
        "        1.0,\n"
        "        float(PackedWidth) / max(float(Width), 1.0));\n"
        "    return float2(ratio, ratio);\n"
        "}")
    new = (
        "// The nominal uniform scale excludes backend block/minimum-size padding.\n"
        "// Zero keeps legacy injected-flow fixtures compatible; production packs\n"
        "// and composition always carry an explicit scale in bits 2-3.\n"
        "float2 flow_input_scale() {\n"
        "    uint nominal = (SynthesisFlags >> 2u) & 3u;\n"
        "    float ratio = nominal == 3u ? 0.5 : nominal == 2u ? 0.75 :\n"
        "        nominal == 1u ? 1.0 : min(1.0, float(PackedWidth) / max(float(Width), 1.0));\n"
        "    return float2(ratio, ratio);\n"
        "}")
    shader_changes = [
        (old, new),
        ("    if (PackedWidth < Width) {", "    if (any(input_scale < 1.0)) {"),
        ("        if (source_coordinate.y < float(Height)) {",
         "        if (source_coordinate.x < float(Width) && source_coordinate.y < float(Height)) {"),
        ("    bool downscaled = PackedWidth < Width || PackedHeight < Height;",
         "    bool downscaled = any(flow_input_scale() < 1.0);"),
    ]
    for old, new in shader_changes:
        if shader.count(old) != 1:
            raise ValueError("Pinned nominal flow scale shader context does not match uniquely")
        shader = shader.replace(old, new)
    old = (
        "            current_color = downscaled\n"
        "                ? bilinear_current_source(target_coordinate, slice)\n"
        "                : CurrentFrame.Load(int4(int2(thread_id.xy), int(slice), 0));")
    new = (
        "            bool cropped_target = false;\n"
        "            if (view_index < 2u) {\n"
        "                cropped_target = any(PreviousMappings[view_index].TargetRect !=\n"
        "                    float4(0.0, 0.0, float(Width), float(Height)));\n"
        "            }\n"
        "            if (downscaled && cropped_target) {\n"
        "                // Match previous target-center coverage inside a crop,\n"
        "                // preserving resource-edge replication for full images.\n"
        "                current_color = sample_current_target(\n"
        "                    target_coordinate, slice, view_index).color;\n"
        "            } else {\n"
        "                current_color = downscaled\n"
        "                    ? bilinear_current_source(target_coordinate, slice)\n"
        "                    : CurrentFrame.Load(int4(int2(thread_id.xy), int(slice), 0));\n"
        "            }")
    if shader.count(old) != 1:
        raise ValueError("Pinned NVIDIA current pack context does not match uniquely")
    shader = shader.replace(old, new)
    shader += shared_nvidia_3x_shader()
    shader = prepare_linear_clamp_shader(shader)
    (output / shader_path.name).write_text(shader, encoding="utf-8")
    (output / "endpoint-validation.patch").write_text("".join(difflib.unified_diff(
        original_shader.splitlines(keepends=True), shader.splitlines(keepends=True),
        fromfile="upstream/src/d3d12/frame_synthesis.hlsl",
        tofile="compiled/src/d3d12/frame_synthesis.hlsl")), encoding="utf-8")
    print("PASS: verified OFXR ownership, state, timing and endpoint derivatives prepared")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
