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
    (output / path.name).write_text(modified, encoding="utf-8")
    (output / "initialization-ownership.patch").write_text("".join(difflib.unified_diff(
        original.splitlines(keepends=True), modified.splitlines(keepends=True),
        fromfile="upstream/src/d3d12/d3d12_frame_synthesizer.cpp",
        tofile="compiled/src/d3d12/d3d12_frame_synthesizer.cpp")), encoding="utf-8")
    shader_path = source / "src/d3d12/frame_synthesis.hlsl"
    original_shader = shader_path.read_text(encoding="utf-8")
    shader = original_shader
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
