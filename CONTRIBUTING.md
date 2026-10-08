# Contributing

Keep changes small and explain the failure or behavior they address. Target `stable` for general runtime fixes and
`experimental-nr` for NR work. Preserve upstream attribution, `LICENSE`, and `THIRD_PARTY`. Do not mix unrelated cleanup
with a behavior change.

Before merging, provide meaningful regression evidence: a behavior-changing fix should have a test that fails for the
original failure and passes with the fix, including relevant positive controls. Rebuild runtime headers, objects, and
linked fixtures together. Old object cohorts are not evidence for current source. Have an independent read-only reviewer
challenge correctness, resource ownership, error paths, and test blind spots; resolve or explicitly adjudicate each finding.
This is a review gate, not a requirement for a particular review service.

## Source checks

Use Python 3.13 and clang-format **22.1.1**. The upstream `.clang-format` remains authoritative for first-party C/C++.
Format complete new first-party C/C++ files and changed hunks in existing files; review any surrounding construct adjusted
by the formatter. Do not reformat inherited vendor code or whole inherited files to satisfy a changed-line check.
The tooling regressions also require Windows PowerShell 5.1 and the v143 C++ tools described below, to exercise real native
argument handling and a small MSBuild project in paths containing spaces.

```powershell
python -m pip install clang-format==22.1.1
python -m unittest discover -s scripts/tests -p test_hygiene.py -v
python -m compileall -q scripts/check-hygiene.py scripts/tests
python scripts/check-hygiene.py
git diff --check 1a83fec8b5c565b14b06ffa8e1eb7e4768057573
```

The fork's public contribution branches are `stable` and `experimental-nr`. Source checks do not require a `main` branch.
The CLI defaults to stable's pinned upstream commit `1a83fec8b5c565b14b06ffa8e1eb7e4768057573` and fails if that commit
is unavailable. A full-history clone includes the baseline through stable's history; in a shallow checkout, fetch sufficient
history before running checks (for example, `git fetch --unshallow origin`). Use `--base <commit-or-ref>` for another actual
review baseline, including a local upstream ref if present. The commands above use stable's baseline. For experimental NR,
use `--base 925dc598c524cb48b37fa4b0a2bff471f75fc5c4`, the retained upstream NR baseline;
its vendor imports are inherited NR content. CI uses those branch baselines
for source hygiene and the PR base or previous push commit for diagnostic comparisons (branch baseline for manual/new-branch runs).
`--clang-format <executable>` selects an installed formatter. The check reads current working-tree text, normalizes
line endings to LF, and includes untracked, unignored files. It checks all tracked paths for case collisions, added text
for whitespace, common credential patterns and personal machine paths, and new or changed binary/generated artifacts.
Unchanged inherited binaries are allowed. Vendor paths are excluded only from formatting. This heuristic credential screen
does not replace review of the patch and history or a dedicated secret scan when publication warrants one.

Keep generated objects, DLLs, installers, symbols, traces, reports, local settings, model data, credentials, and personal
machine paths out of commits. Never add a proprietary NR DLL to the repository. Use ignored `bin/` for build evidence.
Review `git status`, staged paths, and `git diff --cached --check` before committing.

## Core builds and diagnostics

Install Visual Studio 2022 or its Build Tools with the v143 C++ tools and Windows SDK, plus Git, Python, and NuGet.
The build helper finds MSBuild through PATH or `vswhere`; it accepts `-MsBuildPath` for an explicit tool.

```powershell
git submodule update --init --recursive
nuget restore VirtualDesktop-OpenXR.sln
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Build-CommunityRuntime.ps1 -Configuration ReleaseBundle -Platform x64
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Build-CommunityRuntime.ps1 -Configuration ReleaseBundle -Platform x64 -Analyze
```

The helper builds only `virtualdesktop-openxr.vcxproj`, with explicit `SolutionDir`, isolated output and intermediate
folders under `bin/community/`, and `/fp:precise`. It disables inherited pre/post build version editing and signing,
generates `commit.h` in the intermediate folder, and exits nonzero on MSBuild failure. Analyzer builds use `/analyze`
without whole-program optimization. Do not use a bare solution build as a substitute: the upstream solution also requires
installer tooling. A core project build does not validate an installer.

For `stable`, check `Release` and `ReleaseBundle` on Win32 and x64. For `experimental-nr`, check both configurations on x64;
Win32 NR is unsupported until its inherited NGX architecture gap is fixed and verified. Compile and analyze fresh baseline
and candidate sources with the same tools and configuration. Require no newly introduced compiler/analyzer diagnostics;
review inherited diagnostics separately. CI compares diagnostic identities and occurrence counts while ignoring shifted
line numbers. An analyzer result is additional evidence, not a substitute for regression tests or source review.

The community workflow runs source checks, supported core builds, an analyzer comparison, and CPU input, watcher, and
actual-DLL redirect negotiation regressions on both stable and experimental NR. Those fixtures use each branch's fresh x64
ReleaseBundle objects and matching headers. It does not upload runtime,
test, or vendor DLLs, invoke installer signing, or select the system runtime. The upstream workflow is retained separately.

The linked CPU fixtures and runtime DLL import the Vulkan loader even though these tests do not create a Vulkan session.
Headless CI stages the x64 loader and its license from [LunarG's runtime components](https://vulkan.lunarg.com/sdk/home/),
pinned to version 1.4.341.0 and verified by SHA256, beside the test executables under ignored `bin/` paths. This does not
install a driver or replace a system DLL. For local CPU tests, provide an appropriate Vulkan loader too.
The watcher fixture uses a temporary key under HKCU and a process-private registry mapping, restores that mapping, and
removes its owned key after watcher teardown; it does not require an installed Streamer or change production settings.

## Hardware tests

Follow [tests/README.md](tests/README.md) and [tests/vr_input_README.md](tests/vr_input_README.md) for each fixture's requirements.
Run real-GPU and NR tests locally on compatible hardware with separately supplied vendor components, in bounded sequential
processes, and require the documented `PASS:` markers as well as exit success. Preserve and restore settings if a test needs
them. Record the source revision, build cohort, tool versions, backend, test case, and relevant limitations.

GitHub-hosted CPU tests do not establish native GPU/backend coverage, headset image quality, streaming latency, or Khronos
conformance. Keep those claims tied to the actual scope of fresh evidence.
