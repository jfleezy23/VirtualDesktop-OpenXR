"""Bounded hardware checks against one explicit module/runtime build cohort."""

import argparse
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def successful(code, output):
    return code == 0 and bool(re.search(r"^PASS:", output, re.M)) and not re.search(
        r"^(?:FAIL|ERROR):|ASSERTION FAILED:|ERROR: AddressSanitizer", output, re.M
    )


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["gpu", "integration", "presenter", "lifecycle", "all"], default="all")
    parser.add_argument("--module-directory", required=True, type=Path)
    parser.add_argument("--runtime-cohort", type=Path)
    parser.add_argument("--backend-template", type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("fg_provenance", repo / "scripts/fg-build-provenance.py")
    provenance_tools = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(provenance_tools)
    output = args.output_directory.resolve()
    if not output.is_relative_to(repo / "bin") or output.exists():
        parser.error("Use a fresh output directory inside the repository's ignored bin directory.")
    module_directory = args.module_directory.resolve()
    module = module_directory / "vdxr-frame-generation.dll"
    if not module.is_file():
        parser.error("The requested module build is missing.")
    try:
        provenance_tools.verify_receipt(module_directory.parent / "build-source.json", repo, "module")
    except (OSError, ValueError, KeyError) as error:
        parser.error(f"Module cohort provenance: {error}")
    cases = []

    def module_case(name, exe, *arguments):
        cases.append((name, [str(module_directory / (exe + ".exe")), *map(str, arguments)]))

    if args.suite in {"gpu", "all"}:
        module_case("c-abi", "fg_c_abi_regression", module)
        module_case("cpp-abi", "fg_abi_regression", module)
        for multiplier in (2, 3):
            module_case(f"gpu-{multiplier}x", "fg_gpu_regression", module, multiplier)
        for scenario in ("gamma-alpha", "cropped-eyes"):
            module_case("gpu-" + scenario, "fg_gpu_regression", module, 3, scenario)
        for scenario in ("stationary", "motion"):
            module_case("stereo-" + scenario, "fg_gpu_regression", module, 3, scenario)
    if args.suite in {"integration", "lifecycle", "all"}:
        for scenario in ("interop", "invalid-plan", "stale-ticket", "blocked-consumer", "invalid-context", "copy-identity", "copy-alias", "event-stale"):
            module_case("host-" + scenario, "nr_fg_integration_regression", module, scenario)
    if args.suite in {"lifecycle", "all"}:
        for scenario in ("removed", "event-failure", "wait-failure", "stale-wake"):
            module_case("event-" + scenario, "nr_fg_event_regression", module, scenario)
        module_case("initialization", "fg_initialization_regression", module)
        for signal in range(1, 5):
            module_case(f"signal-{signal}", "fg_initialization_regression", module, signal)
    if args.suite in {"presenter", "all"}:
        module_case("cadence", "nr_fg_presenter_regression")

    manifest = {"suite": args.suite, "module_sha256": sha256(module), "results": []}
    if args.suite != "gpu":
        if not args.runtime_cohort or not args.backend_template:
            parser.error("Runtime suites need a matched runtime cohort and an OVRNull/NR backend template.")
        cohort = args.runtime_cohort.resolve()
        build = json.loads((cohort / "result.json").read_text())
        runtime = cohort / "virtualdesktop-openxr.dll"
        try:
            provenance_tools.verify_receipt(cohort / "build-source.json", repo, "runtime")
        except (OSError, ValueError, KeyError) as error:
            parser.error(f"Runtime cohort provenance: {error}")
        if build["exit_code"] or sha256(runtime) != build["runtime_sha256"]:
            parser.error("The runtime DLL does not match a successful cohort build.")
        manifest.update(runtime_sha256=sha256(runtime), compiler_version=build["compiler_version"])
        backend = output / "backend"
        ordinary_backend = output / "backend-nr-off"
        tests = cohort / "tests"

        def runtime_case(name, exe, *arguments):
            provenance = json.loads((tests / (exe + "-provenance.json")).read_text())
            try:
                provenance_tools.verify_receipt(tests / (exe + "-source.json"), repo, "fixture")
            except (OSError, ValueError, KeyError) as error:
                parser.error(f"Fixture {exe} source provenance: {error}")
            binary = tests / (exe + ".exe")
            if provenance["compiler"] != build["compiler_version"] or sha256(binary) != provenance["exe_sha256"]:
                parser.error(f"Fixture {exe} does not match its build receipt.")
            if sha256(repo / "tests" / (exe + ".cpp")) != provenance["source_sha256"]:
                parser.error(f"Fixture {exe} needs rebuilding after source changes.")
            objects = {p.name: sha256(p) for p in (cohort / "obj").glob("*.obj")}
            if not objects or objects != provenance["runtime_objects"]:
                parser.error(f"Fixture {exe} was linked against another runtime cohort.")
            cases.append((name, [str(binary), *map(str, arguments)]))

        if args.suite in {"integration", "all"}:
            for scenario in ("native", "upscaled", "discard", "nr-toggle"):
                runtime_case("nr-" + scenario, "nr_fg_runtime_regression", backend, output / ("state-" + scenario), scenario)
            for scenario in ("nr-off-2", "nr-off-3", "nr-off-upscaled", "nr-off-cropped", "nr-off-packed",
                             "nr-off-reuse", "nr-off-bypass", "nr-off-reuse-bypass",
                             "nr-off-d3d12", "nr-off-d3d12-reuse", "nr-off-d3d12-reuse-bypass"):
                runtime_case(scenario, "nr_fg_runtime_regression", ordinary_backend,
                             output / ("state-" + scenario), scenario)
        if args.suite in {"presenter", "all"}:
            for multiplier, scenario in ((2, "normal"), (3, "normal"), (3, "late"), (3, "capacity")):
                runtime_case(f"worker-{multiplier}-{scenario}", "nr_fg_worker_regression", backend, module, multiplier, scenario)
            for scenario in ("control", "late-fg", "future-input", "late-nr"):
                runtime_case("delay-" + scenario, "nr_fg_delay_regression", backend, module, scenario)
        if args.suite in {"lifecycle", "all"}:
            scenarios = ["failed-pending", "stop-before-begin", "cleanup-producer", "bgra", "unequal-eyes", "status-snapshots",
                         "startup-retry", "startup-timeout", "startup-stop",
                         "allocation-create", "allocation-length", "allocation-empty", "allocation-buffer",
                         "static", "protected", "nr-off", "low-coverage", "app-submission", "synchronous",
                         "depth", "overlay", "second-projection"]
            scenarios += ["history-" + s for s in ("recenter", "fov", "space", "sharpen", "visibility", "pipelined", "visibility-gap", "busy")]
            scenarios += [f"fail-{operation}-{slot}" for operation in ("wait", "begin", "end") for slot in ("s1", "s2", "b")]
            for scenario in scenarios:
                runtime_case("worker-" + scenario, "nr_fg_worker_regression", backend, module, 3, scenario)

    for _, command in cases:
        if not Path(command[0]).is_file():
            parser.error("A required fixture executable is missing.")
    output.mkdir(parents=True)
    if args.suite != "gpu":
        backend.mkdir()
        for name in ("LibOVRRT64_1.dll", "nvngx_dlssnr.dll"):
            source = args.backend_template / name
            shutil.copy2(source, backend / name)
            manifest[name + "_sha256"] = sha256(source)
        shutil.copy2(runtime, backend / runtime.name)
        shutil.copy2(module, backend / module.name)
        ordinary_backend.mkdir()
        for name in ("LibOVRRT64_1.dll", runtime.name, module.name):
            shutil.copy2(backend / name, ordinary_backend / name)
        manifest["nr_off_backend_excludes_vendor"] = not (ordinary_backend / "nvngx_dlssnr.dll").exists()
    manifest["review_time_source_sha256"] = {
        p.relative_to(repo).as_posix(): sha256(p) for folder in ("virtualdesktop-openxr", "tests", "nr-frame-generation")
        for p in (repo / folder).rglob("*") if p.is_file() and p.suffix in {".cpp", ".c", ".h", ".hpp", ".hlsl", ".py"}
    }
    for name, command in cases:
        started = time.monotonic()
        try:
            environment = os.environ.copy()
            if name.startswith("stereo-"):
                environment["VDXR_FG_TEST_OUTPUT"] = str(output)
            run = subprocess.run(command, cwd=repo, env=environment, capture_output=True, text=True, timeout=60)
            text, code = run.stdout + run.stderr, run.returncode
        except subprocess.TimeoutExpired:
            text, code = "FAIL: child process timed out\n", -999
        (output / (name + ".log")).write_text(text, encoding="utf-8")
        passed = successful(code, text)
        manifest["results"].append({"name": name, "passed": passed, "exit_code": code,
                                    "seconds": time.monotonic() - started, "argv": command,
                                    "exe_sha256": sha256(Path(command[0]))})
        (output / "results.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(("PASS: " if passed else "FAIL: ") + name, flush=True)
        if not passed:
            print(text[-2000:], flush=True)
    if args.suite in {"gpu", "all"}:
        stationary = (output / "stationary-s1.rgba").read_bytes()
        moving = (output / "moving-s1.rgba").read_bytes()
        size_ok = len(stationary) == len(moving) == 1024 * 256 * 4
        # Only left-eye motion differs between these two executions. The right
        # eye's generated image must remain identical, including its alpha.
        differences = sum(
            a != b for row in range(256)
            for a, b in zip(stationary[row * 4096 + 2048:(row + 1) * 4096],
                            moving[row * 4096 + 2048:(row + 1) * 4096])
        )
        passed = size_ok and differences == 0
        manifest["results"].append({"name": "eye-isolation", "passed": passed,
                                    "right_eye_changed_bytes": differences,
                                    "stationary_sha256": sha256(output / "stationary-s1.rgba"),
                                    "moving_sha256": sha256(output / "moving-s1.rgba")})
        (output / "results.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(("PASS: " if passed else "FAIL: ") + "eye-isolation", flush=True)
    return 0 if all(r["passed"] for r in manifest["results"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
