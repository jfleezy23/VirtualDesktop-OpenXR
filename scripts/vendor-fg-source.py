"""Retain the verified compilation subset and license notices for the FG DLL."""

import argparse
import hashlib
import json
from pathlib import Path

PIN = "07449429c139e5d2070b6f0855ed5323de7cee00"
FILES = [
    "src/d3d12/d3d12_history.cpp", "src/d3d12/d3d12_frame_synthesizer.cpp",
    "src/common/dlss_motion_vectors.cpp", "src/core/pose.cpp", "src/layer/bridge_flight_logger.cpp",
    "src/d3d12/frame_synthesis.hlsl", "LICENSE", "THIRD_PARTY.md", "licenses/AMD-FidelityFX-MIT.txt",
] + ["include/xrfg/" + name + ".hpp" for name in [
    "d3d12_history", "d3d12_frame_synthesizer", "dlss_motion_vectors", "pose",
    "fps_overlay_model", "bridge_flight_logger",
]] + ["external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface/" + name for name in [
    "nvOpticalFlowCommon.h", "nvOpticalFlowD3D12.h",
]]


def retain(deps: Path, output: Path) -> None:
    source = json.loads((deps / "ofxr-manifest.json").read_text())
    if source["commit"] != PIN or source["repository"] != "djules75/OFXR-Bridge":
        raise ValueError("Unsupported OFXR pin")
    manifest = {"repository": source["repository"], "commit": PIN, "files": {}, "upstream_files": {}}
    for name in FILES:
        data = (deps / "ofxr" / name).read_bytes()
        upstream_hash = hashlib.sha256(data).hexdigest()
        if upstream_hash != source["files"][name]:
            raise ValueError(f"Modified acquired source: {name}")
        if "NvOFInterface/" in name:
            # Only trailing horizontal whitespace is normalized in the two
            # NVIDIA interfaces. Preserve every token and complete notice.
            data = b"\n".join(line.rstrip(b" \t\r") for line in data.split(b"\n"))
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        manifest["files"][name] = hashlib.sha256(data).hexdigest()
        manifest["upstream_files"][name] = upstream_hash
    manifest["normalization"] = "NVIDIA interface headers: trailing horizontal whitespace only"
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("PASS: retained OFXR compilation subset, source identities and notices")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    retain(args.deps, args.output)
