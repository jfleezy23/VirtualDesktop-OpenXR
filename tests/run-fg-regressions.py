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


def select_clock(scenario, inherited=None, override=None):
    if override is not None and override not in (30, 60, 120):
        raise ValueError("Supported qualification clocks are30,60,120Hz")
    profile = "steady-fixed" if scenario.startswith("spatial-steady-") else (
        "cold-transition" if scenario.startswith("spatial-") else "runtime-default"
    )
    rate = 30 if profile == "cold-transition" else 120
    return {"profile": profile, "profile_hz": rate,
            "selected_hz": override if override is not None else rate,
            "selection": "cli-override" if override is not None else "profile", "inherited_env": inherited}


def clock_receipt(selected, output):
    rates = [int(rate) for rate in re.findall(r"^private backend test clock=(\d+)Hz$", output, re.M)]
    return {**selected, "reported_hz": rates[0] if len(rates) == 1 else None,
            "reported_count": len(rates), "matched": len(rates) == 1 and rates[0] == selected["selected_hz"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["gpu", "integration", "presenter", "lifecycle", "all"], default="all")
    parser.add_argument("--module-directory", required=True, type=Path)
    parser.add_argument("--runtime-cohort", type=Path)
    parser.add_argument("--backend-template", type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    parser.add_argument("--refresh-rate", type=int, choices=[30, 60, 120],
                        help="Explicitly override per-case runtime clocks (cold spatial30Hz, steady120Hz). "
                             "Inherited VDXR_FG_TEST_REFRESH_RATE is recorded but does not select clocks.")
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
        cases.append((name, [str(module_directory / (exe + ".exe")), *map(str, arguments)], None))

    if args.suite in {"gpu", "all"}:
        module_case("c-abi", "fg_c_abi_regression", module)
        module_case("cpp-abi", "fg_abi_regression", module)
        module_case("v2-timing-and-real", "fg_v2_regression", module)
        for multiplier in (2, 3):
            module_case(f"gpu-{multiplier}x", "fg_gpu_regression", module, multiplier)
        for scenario in ("gamma-alpha", "cropped-eyes"):
            module_case("gpu-" + scenario, "fg_gpu_regression", module, 3, scenario)
        for scenario in ("stationary", "motion"):
            module_case("stereo-" + scenario, "fg_gpu_regression", module, 3, scenario)
    if args.suite in {"integration", "lifecycle", "all"}:
        for scenario in ("typeless", "unorm", "srgb", "rgba", "rgba-srgb", "motion", "motion-srgb", "separate", "array-mip", "source-reuse", "two-live", "mixed", "mixed-reverse", "unsupported-right", "invalid-right"):
            module_case("bgra-" + scenario, "nr_fg_bgra_regression", module, scenario)
        for scenario in ("interop", "invalid-plan", "stale-ticket", "blocked-consumer", "invalid-context", "copy-identity", "copy-alias", "event-stale", "source-overwrite"):
            module_case("host-" + scenario, "nr_fg_integration_regression", module, scenario)
        for mode in range(8):
            module_case(f"v2-host-{mode}", "fg_v2_host_regression", module_directory / "fg_v2_host_test_module.dll", mode)
        module_case("v2-host-diagnostics", "fg_v2_host_regression", module_directory / "fg_v2_host_test_module.dll", 5, "diagnostics")
        for scenario in ("packed", "offset", "separate", "gap", "overlap", "reverse", "different-y", "array-mip",
                         "invalid-rect", "invalid-pose", "wrong-device"):
            module_case("pack-" + scenario, "nr_fg_pack_regression", module, scenario)
    if args.suite in {"lifecycle", "all"}:
        for scenario in ("all", "wait-failure"):
            module_case("ready-wait-" + scenario, "nr_fg_ready_wait_regression", module, scenario)
        for scenario in ("removed", "event-failure", "wait-failure", "stale-wake"):
            module_case("event-" + scenario, "nr_fg_event_regression", module, scenario)
        for scenario in ("timeout", "event-failure", "stale-wake", "removed", "invalid", "next"):
            module_case("ready-" + scenario, "nr_fg_ready_regression", module, scenario)
        module_case("initialization", "fg_initialization_regression", module)
        for signal in range(1, 5):
            module_case(f"signal-{signal}", "fg_initialization_regression", module, signal)
            for multiplier in (2, 3):
                module_case(f"signal-host-{multiplier}x-{signal}", "fg_initialization_regression",
                            module, signal, "host", multiplier)
    if args.suite in {"presenter", "all"}:
        module_case("cadence", "nr_fg_presenter_regression")

    inherited_clock = os.environ.get("VDXR_FG_TEST_REFRESH_RATE")
    manifest = {"suite": args.suite, "module_sha256": sha256(module), "results": [],
                "clock_override_hz": args.refresh_rate, "inherited_clock_env": inherited_clock}
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
            clock = select_clock(str(arguments[-1]), inherited_clock, args.refresh_rate) if exe == "nr_fg_runtime_regression" else None
            cases.append((name, [str(binary), *map(str, arguments)], clock))

        if args.suite in {"integration", "all"}:
            runtime_case("cpu-query-allocations", "nonnr_input_regression", "query-allocations")
            runtime_case("cpu-resolve-allocations", "nonnr_copy_preprocess_regression", "allocations", backend)
            for scenario in ("native", "upscaled", "sharpen", "discard", "nr-toggle", "bgra", "bgra-srgb", "bgra-srgb-jitter", "nr-off-reset-busy", "nr-off-capture-busy"):
                runtime_case("nr-" + scenario, "nr_fg_runtime_regression", backend, output / ("state-" + scenario), scenario)
            for scenario in ("spatial-bgra-off", "spatial-bgra-srgb-off", "spatial-bgra-2", "spatial-bgra-srgb-2",
                             "spatial-bgra-3", "spatial-bgra-srgb-3",
                             "spatial-combined-bgra-off", "spatial-combined-bgra-srgb-off",
                             "spatial-combined-bgra-2", "spatial-combined-bgra-srgb-2",
                             "spatial-combined-bgra-3", "spatial-combined-bgra-srgb-3"):
                runtime_case(scenario, "nr_fg_runtime_regression", backend, output / ("state-" + scenario), scenario)
            for coverage, processing in ((95, 80), (90, 80), (90, 85), (100, 80)):
                for encoding in ("bgra", "bgra-srgb"):
                    for multiplier in (2, 3):
                        scenario = f"spatial-steady-{coverage}-{processing}-{encoding}-{multiplier}"
                        runtime_case(scenario, "nr_fg_runtime_regression", backend, output / ("state-" + scenario), scenario)
            for scenario in ("color", "depth", "ignored", "reuse", "upscaled", "direct", "static", "static-array"):
                runtime_case("commits-" + scenario, "nr_intermediate_commit_regression", backend,
                             output / ("state-commits-" + scenario), scenario)
            for scenario in ("nr-off-2", "nr-off-3", "nr-off-upscaled", "nr-off-cropped", "nr-off-packed",
                             "nr-off-reuse", "nr-off-bypass", "nr-off-reuse-bypass",
                             "nr-off-d3d12", "nr-off-d3d12-reuse", "nr-off-d3d12-reuse-bypass"):
                runtime_case(scenario, "nr_fg_runtime_regression", ordinary_backend,
                             output / ("state-" + scenario), scenario)
        if args.suite in {"presenter", "all"}:
            for multiplier, scenario in ((2, "normal"), (3, "normal"), (3, "late"), (3, "capacity")):
                runtime_case(f"worker-{multiplier}-{scenario}", "nr_fg_worker_regression", backend, module, multiplier, scenario)
            for scenario in ("control", "late-fg", "future-input", "late-nr", "expired-ready",
                             "ready-event-failure", "ready-wait-failure", "ready-cancel"):
                runtime_case("delay-" + scenario, "nr_fg_delay_regression", backend, module, scenario)
        if args.suite in {"lifecycle", "all"}:
            scenarios = ["failed-pending", "stop-before-begin", "cleanup-producer", "bgrx", "unequal-eyes", "status-snapshots",
                         "startup-retry", "startup-timeout", "startup-stop",
                         "allocation-create", "allocation-length", "allocation-empty", "allocation-buffer",
                         "static", "protected", "nr-off", "low-coverage", "app-submission", "synchronous",
                         "depth", "overlay", "second-projection"]
            scenarios += ["history-" + s for s in ("recenter", "fov", "space", "sharpen", "visibility", "pipelined", "visibility-gap", "busy")]
            scenarios += [f"fail-{operation}-{slot}" for operation in ("wait", "begin", "end") for slot in ("s1", "s2", "b")]
            for scenario in scenarios:
                runtime_case("worker-" + scenario, "nr_fg_worker_regression", backend, module, 3, scenario)

    for _, command, _ in cases:
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
    for name, command, selected_clock in cases:
        started = time.monotonic()
        try:
            environment = os.environ.copy()
            if selected_clock:
                environment["VDXR_FG_TEST_REFRESH_RATE"] = str(selected_clock["selected_hz"])
            if name.startswith("stereo-"):
                environment["VDXR_FG_TEST_OUTPUT"] = str(output)
            run = subprocess.run(command, cwd=repo, env=environment, capture_output=True, text=True, timeout=60)
            text, code = run.stdout + run.stderr, run.returncode
        except subprocess.TimeoutExpired:
            text, code = "FAIL: child process timed out\n", -999
        receipt = clock_receipt(selected_clock, text) if selected_clock else None
        if receipt and not receipt["matched"]:
            text += "\nFAIL: fixture clock does not match its explicit qualification profile/override\n"
        (output / (name + ".log")).write_text(text, encoding="utf-8")
        passed = successful(code, text)
        manifest["results"].append({"name": name, "passed": passed, "exit_code": code,
                                    "seconds": time.monotonic() - started, "argv": command,
                                    "exe_sha256": sha256(Path(command[0])), "qualification_clock": receipt})
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
