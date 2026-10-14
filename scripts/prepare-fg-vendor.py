"""Apply the audited initialization-ownership change to verified OFXR source.

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
    output.mkdir(parents=True, exist_ok=True)
    (output / path.name).write_text(modified, encoding="utf-8")
    (output / "initialization-ownership.patch").write_text("".join(difflib.unified_diff(
        original.splitlines(keepends=True), modified.splitlines(keepends=True),
        fromfile="upstream/src/d3d12/d3d12_frame_synthesizer.cpp",
        tofile="compiled/src/d3d12/d3d12_frame_synthesizer.cpp")), encoding="utf-8")
    print("PASS: verified OFXR initialization ownership derivative prepared")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
