"""Build a self-contained Windows x64 controls folder; never collect VR DLLs."""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[1]
NAME = "VDXR-NR-FG-Controls"
PYINSTALLER_VERSION = "6.22.3"
ALLOWED_DLLS = {"python3.dll", "python313.dll", "tcl86t.dll", "tk86t.dll", "libffi-8.dll",
                "vcruntime140.dll", "vcruntime140_1.dll", "libcrypto-3.dll", "zlib1.dll"}
DOC_IMAGES = ("docs/images/controls-overview.png", "docs/images/controls-fg-only.png",
              "docs/images/controls-game-profile.png")
SOURCE_FILES = (
    "vdxr_registry_gui.py", "vdxr_runtime_selection.py", "vdxr_game_profiles.py",
    "scripts/controls_entry.py", "scripts/controls_self_test.py",
    "scripts/build-controls.py", "scripts/controls-build-requirements.txt",
    "scripts/tests/test_fg_controls.py", "scripts/tests/test_runtime_selection.py", "scripts/tests/test_controls_package.py",
    "scripts/tests/test_nr_spatial_controls.py",
    "scripts/tests/test_game_profiles.py",
    "scripts/licenses/Tcl-8.6.15.txt", "docs/NR-FG-CONTROLS.md", "docs/SIDE-BY-SIDE-RUNTIME.md",
    "docs/GAME-COMPATIBILITY.md", "LICENSE",
    "scripts/licenses/OpenSSL-3.0.16.txt", "scripts/licenses/zlib-1.3.1.txt",
) + DOC_IMAGES


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_payload(folder):
    for path in folder.rglob("*"):
        if path.is_symlink():
            raise ValueError("Controls package cannot contain symbolic links")
        if path.is_file():
            if path.suffix.lower() == ".dll" and path.name.lower() not in ALLOWED_DLLS:
                raise ValueError(f"Unapproved DLL in controls package: {path.name}")
            if path.name.lower().startswith("nvngx") or path.suffix.lower() in {".onnx", ".safetensors", ".pt", ".pth", ".gguf"}:
                raise ValueError(f"NR/model content is forbidden: {path.name}")


def archive_bundle(bundle, destination):
    verify_payload(bundle)
    hashes = {path.relative_to(bundle).as_posix(): sha(path) for path in sorted(bundle.rglob("*")) if path.is_file()}
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in hashes:
            archive.write(bundle / name, NAME + "/" + name)
    with zipfile.ZipFile(destination) as archive:
        names = archive.namelist()
        if len({name.casefold() for name in names}) != len(names):
            raise ValueError("Case-colliding or duplicate ZIP members")
        for name, digest in hashes.items():
            if hashlib.sha256(archive.read(NAME + "/" + name)).hexdigest() != digest:
                raise ValueError(f"ZIP member changed: {name}")
    return hashes


def verify_recorded_payload(output, bundle):
    recorded = json.loads((output / "payload-sha256.json").read_text())
    current = {path.relative_to(bundle).as_posix(): sha(path) for path in bundle.rglob("*") if path.is_file()}
    if current != recorded:
        raise ValueError("Tested package contents changed; do not publish this build")


def copy_documentation(snapshot, bundle):
    """Keep the extracted guide and its relative screenshot links usable offline."""
    shutil.copyfile(snapshot / "docs/NR-FG-CONTROLS.md", bundle / "README.md")
    shutil.copyfile(snapshot / "docs/SIDE-BY-SIDE-RUNTIME.md", bundle / "SIDE-BY-SIDE-RUNTIME.md")
    shutil.copyfile(snapshot / "docs/GAME-COMPATIBILITY.md", bundle / "GAME-COMPATIBILITY.md")
    images = bundle / "images"
    images.mkdir()
    for name in DOC_IMAGES:
        shutil.copyfile(snapshot / name, images / Path(name).name)


def build(output):
    if sys.platform != "win32" or struct.calcsize("P") != 8 or sys.version_info[:2] != (3, 13):
        raise RuntimeError("Build with Windows x64 Python 3.13")
    if importlib.metadata.version("pyinstaller") != PYINSTALLER_VERSION:
        raise RuntimeError("Install the pinned controls-build-requirements.txt first")
    output.mkdir(parents=True, exist_ok=False)
    # Compile and publish one immutable source cohort, even if the checkout changes.
    snapshot = output / "source-snapshot"
    inputs = {name: sha(ROOT / name) for name in SOURCE_FILES}
    for name, digest in inputs.items():
        destination = snapshot / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, destination)
        if sha(destination) != digest:
            raise ValueError(f"Source changed while capturing snapshot: {name}")
    build_env = os.environ.copy()
    for name in ("PYTHONPATH", "PYTHONHOME", "TCL_LIBRARY", "TK_LIBRARY"):
        build_env.pop(name, None)
    # Prevent unrelated tools on PATH from supplying DLL dependencies.
    build_env["PATH"] = os.pathsep.join([sys.base_prefix, str(Path(sys.base_prefix) / "DLLs"),
                                         str(Path(os.environ["SystemRoot"]) / "System32"), os.environ["SystemRoot"]])
    subprocess.run([
        sys.executable, "-m", "PyInstaller", "--onedir", "--windowed", "--noupx", "--uac-admin",
        "--name", NAME, "--paths", str(snapshot), "--distpath", str(output / "dist"),
        "--workpath", str(output / "work"), "--specpath", str(output),
        str(snapshot / "scripts/controls_entry.py"),
    ], cwd=snapshot, env=build_env, check=True)
    bundle = output / "dist" / NAME
    verify_payload(bundle)
    source = bundle / "source"
    for name in SOURCE_FILES:
        destination = source / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if sha(snapshot / name) != inputs[name]:
            raise ValueError(f"Frozen source changed during compilation: {name}")
        shutil.copyfile(snapshot / name, destination)
    licenses = bundle / "licenses"
    licenses.mkdir()
    shutil.copyfile(snapshot / "LICENSE", licenses / "VDXR-MIT.txt")
    shutil.copyfile(Path(sys.base_prefix) / "LICENSE.txt", licenses / "Python.txt")
    shutil.copyfile(Path(sys.base_prefix) / "tcl/tk8.6/license.terms", licenses / "Tk-8.6.15.txt")
    shutil.copyfile(snapshot / "scripts/licenses/Tcl-8.6.15.txt", licenses / "Tcl-8.6.15.txt")
    shutil.copyfile(snapshot / "scripts/licenses/OpenSSL-3.0.16.txt", licenses / "OpenSSL-3.0.16.txt")
    shutil.copyfile(snapshot / "scripts/licenses/zlib-1.3.1.txt", licenses / "zlib-1.3.1.txt")
    copy_documentation(snapshot, bundle)
    metadata = {"name": NAME, "source_revision": None,
                "build_base_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "source_sha256": inputs, "exe_sha256": sha(bundle / (NAME + ".exe")),
                "python": sys.version.split()[0], "pyinstaller": PYINSTALLER_VERSION,
                "architecture": "x64", "mode": "onedir", "requested_execution_level": "requireAdministrator",
                "nvidia_nr_dlls_included": False, "requires_python_install": False}
    (bundle / "BUILD.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    hashes = archive_bundle(bundle, output / (NAME + "-Windows-x64.zip"))
    (output / "payload-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"bundle": str(bundle), "files": len(hashes), "exe_sha256": sha(bundle / (NAME + ".exe"))}))


def bind_revision(output, revision):
    revision = subprocess.check_output(["git", "rev-parse", "--verify", revision + "^{commit}"], cwd=ROOT, text=True).strip()
    bundle = output / "dist" / NAME
    verify_recorded_payload(output, bundle)
    metadata = json.loads((bundle / "BUILD.json").read_text())
    if sha(bundle / (NAME + ".exe")) != metadata["exe_sha256"]:
        raise ValueError("Tested executable has changed")
    for name, digest in metadata["source_sha256"].items():
        source = bundle / "source" / name
        committed = subprocess.check_output(["git", "show", f"{revision}:{name}"], cwd=ROOT)
        data = source.read_bytes()
        matches = data == committed if name.endswith(".png") else data.replace(b"\r\n", b"\n") == committed.replace(b"\r\n", b"\n")
        if sha(source) != digest or not matches:
            raise ValueError(f"Compiled source does not match revision: {name}")
    metadata["source_revision"] = revision
    (bundle / "BUILD.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    hashes = archive_bundle(bundle, output / (NAME + "-Windows-x64.zip"))
    (output / "payload-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: unchanged executable bound to {revision}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New build directory (must not exist)")
    parser.add_argument("--bind-revision", help="Bind an already tested build to matching committed source, without rebuilding")
    args = parser.parse_args()
    if args.bind_revision:
        bind_revision(args.output.resolve(), args.bind_revision)
    else:
        build(args.output.resolve())
