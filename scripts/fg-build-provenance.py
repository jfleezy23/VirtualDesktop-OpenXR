"""Bind a completed build to source inputs captured before compilation."""

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(repo, kind):
    repo = repo.resolve()
    paths = set()
    folders = {"runtime": ["virtualdesktop-openxr", "RuntimeConfiguration", "nr-frame-generation/include"],
               "module": ["nr-frame-generation"],
               "fixture": ["virtualdesktop-openxr", "RuntimeConfiguration", "nr-frame-generation/include", "tests"]}
    suffixes = {".c", ".cpp", ".h", ".hpp", ".hlsl", ".props", ".vcxproj", ".filters"}
    for folder in folders[kind]:
        paths.update(p for p in (repo / folder).rglob("*") if p.is_file() and
                     (p.suffix in suffixes or p.name == "CMakeLists.txt"))
    if kind == "module":
        for name in ("frame_generation", "frame_generation_pacing"):
            paths.update((repo / "virtualdesktop-openxr" / (name + suffix)) for suffix in (".cpp", ".h"))
        paths.update((repo / "tests").glob("fg_*.*"))
        paths.update(repo / "tests" / (name + ".cpp") for name in ("nr_fg_integration_regression", "nr_fg_presenter_regression", "nr_fg_event_regression", "nr_fg_ready_regression", "nr_fg_ready_wait_regression", "nr_fg_pack_regression", "nr_fg_bgra_regression"))
        paths.add(repo / "virtualdesktop-openxr/FrameGenerationCopyCS.hlsl")
    for name in ("Experimental-MSVC145.props", "prepare-fg-vendor.py", "fg-build-provenance.py"):
        if (repo / "scripts" / name).is_file():
            paths.add(repo / "scripts" / name)
    return {p.relative_to(repo).as_posix(): sha256(p) for p in sorted(paths) if p.is_file()}


def write_receipt(path, repo, kind, before, artifacts):
    if before != snapshot(repo, kind):
        raise ValueError("Source changed during the build; discard this cohort.")
    repo = repo.resolve()
    artifacts = [p.resolve() for p in artifacts]
    if not artifacts or any(not p.is_relative_to(repo) for p in artifacts):
        raise ValueError("Build artifacts must be nonempty and inside the source workspace.")
    data = {"kind": kind, "source_sha256": before,
            "artifact_sha256": {p.relative_to(repo).as_posix(): sha256(p) for p in artifacts}}
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def verify_receipt(path, repo, kind):
    data = json.loads(path.read_text())
    if data["kind"] != kind or data["source_sha256"] != snapshot(repo, kind) or not data["artifact_sha256"]:
        raise ValueError("Build receipt is unfinished or its source inputs are stale.")
    for name, digest in data["artifact_sha256"].items():
        artifact = (repo / name).resolve()
        if not artifact.is_relative_to(repo.resolve()) or not artifact.is_file() or sha256(artifact) != digest:
            raise ValueError("Build artifact differs from its receipt.")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--kind", choices=["runtime", "module", "fixture"], required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--artifact-directory", type=Path)
    args = parser.parse_args()
    if args.capture:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps({"kind": args.kind, "source_sha256": snapshot(args.repo, args.kind),
                                            "artifact_sha256": {}}, indent=2), encoding="utf-8")
    else:
        if not args.artifact_directory:
            parser.error("Finalizing requires an artifact directory.")
        before = json.loads(args.receipt.read_text())
        if before["kind"] != args.kind:
            parser.error("Receipt kind changed during compilation.")
        artifacts = [p for p in args.artifact_directory.rglob("*") if p.is_file() and p.suffix in {".dll", ".exe", ".obj"}]
        write_receipt(args.receipt, args.repo, args.kind, before["source_sha256"], artifacts)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
