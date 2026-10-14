"""Acquire pinned source; validate OFXR files against Git object identities.

SDK outputs remain ignored. Never updates an existing nonmatching source tree.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile

OFXR = "07449429c139e5d2070b6f0855ed5323de7cee00"
FFX = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55"
OFXR_REPO = "djules75/OFXR-Bridge"
FFX_REPO = "GPUOpen-LibrariesAndSDKs/FidelityFX-SDK"


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "VDXR-FG-source-build"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def verified_blob(data: bytes, sha: str) -> bool:
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest() == sha


def acquire(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    tree = json.loads(fetch(f"https://api.github.com/repos/{OFXR_REPO}/git/trees/{OFXR}?recursive=1"))
    if tree.get("truncated"):
        raise RuntimeError("Incomplete source tree")
    prefixes = ("include/xrfg/", "src/core/", "src/d3d12/", "src/common/dlss_motion_vectors.", "src/layer/bridge_flight_logger.",
                "external/NVIDIA-Optical-Flow-SDK-5.0.7/")
    exact = {"LICENSE", "THIRD_PARTY.md", "CMakeLists.txt", "docs/BUILDING.md", "licenses/AMD-FidelityFX-MIT.txt"}
    entries = [item for item in tree["tree"] if item["type"] == "blob" and
               (item["path"] in exact or item["path"].startswith(prefixes))]
    manifest = {"repository": OFXR_REPO, "commit": OFXR, "files": {}}
    for entry in entries:
        relative = entry["path"]
        destination = output / "ofxr" / relative
        data = destination.read_bytes() if destination.exists() else fetch(
            f"https://raw.githubusercontent.com/{OFXR_REPO}/{OFXR}/{relative}")
        if not verified_blob(data, entry["sha"]):
            raise RuntimeError(f"Source identity mismatch: {relative}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        manifest["files"][relative] = hashlib.sha256(data).hexdigest()
    (output / "ofxr-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    ffx = output / "fidelityfx"
    stamp = ffx / "vdxr-source-pin.json"
    if ffx.exists():
        if not stamp.exists() or json.loads(stamp.read_text())["commit"] != FFX:
            raise RuntimeError("Existing FidelityFX tree has no matching acquisition receipt")
        receipt = json.loads(stamp.read_text())
        if "files" not in receipt:
            raise RuntimeError("Existing FidelityFX receipt lacks complete source hashes; use fresh output")
        for name, digest in receipt["files"].items():
            if hashlib.sha256((ffx / name).read_bytes()).hexdigest() != digest:
                raise RuntimeError(f"Modified FidelityFX dependency: {name}")
    else:
        data = fetch(f"https://codeload.github.com/{FFX_REPO}/zip/{FFX}")
        archive = zipfile.ZipFile(io.BytesIO(data))
        prefix = f"FidelityFX-SDK-{FFX}/"
        hashes = {}
        for member in archive.infolist():
            if not member.filename.startswith(prefix):
                raise RuntimeError("Unexpected source archive root")
            relative = Path(member.filename[len(prefix):])
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError("Unsafe archive path")
            if member.is_dir():
                continue
            destination = ffx / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            payload = archive.read(member)
            destination.write_bytes(payload)
            hashes[relative.as_posix()] = hashlib.sha256(payload).hexdigest()
        stamp.write_text(json.dumps({"repository": FFX_REPO, "commit": FFX,
                                     "archive_sha256": hashlib.sha256(data).hexdigest(), "files": hashes}, indent=2) + "\n")
    print(f"PASS: pinned OFXR ({len(entries)} verified files) and FidelityFX source acquired")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    acquire(parser.parse_args().output.resolve())
