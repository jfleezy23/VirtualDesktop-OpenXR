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


def prepare(source: Path, output: Path) -> None:
    manifest = json.loads((source / "manifest.json").read_text())
    if manifest["commit"] != "07449429c139e5d2070b6f0855ed5323de7cee00":
        raise ValueError("Unsupported OFXR source pin")
    for name, digest in manifest["files"].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"Modified immutable vendor source: {name}")
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
    output.mkdir(parents=True, exist_ok=True)
    (output / path.name).write_text(modified, encoding="utf-8")
    (output / "initialization-ownership.patch").write_text("".join(difflib.unified_diff(
        original.splitlines(keepends=True), modified.splitlines(keepends=True),
        fromfile="upstream/src/d3d12/d3d12_frame_synthesizer.cpp",
        tofile="compiled/src/d3d12/d3d12_frame_synthesizer.cpp")), encoding="utf-8")
    print("PASS: verified OFXR ownership and timing derivative prepared")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
