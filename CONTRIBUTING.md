# Contributing

Keep runtime corrections focused, preserve upstream attribution and third-party
notices, and explain the triggering failure and resulting behavior. Include a
regression that fails on the original code and passes with the correction, with
relevant positive controls. Independently review ownership and failure paths.

## Source checks

The existing `.clang-format` is authoritative for first-party C/C++. Format new
files and changed hunks without reformatting inherited vendor sources. The
additional workflow pins Python 3.13 and clang-format 22.1.1.

```powershell
python -m pip install clang-format==22.1.1
python -m unittest discover -s scripts/tests -p test_hygiene.py -v
python -m compileall -q scripts/check-hygiene.py scripts/tests
python scripts/check-hygiene.py --base <review-base-commit>
git diff --check <review-base-commit>
```

The checker defaults to retained upstream commit
`6da20fe478e331c05672243fd82a9a1500cd39f5`. Use `--base` for the actual comparison
commit; fetch full history if necessary. It checks current text, including
untracked/unignored files, for case collisions, whitespace, common credential
patterns, personal machine paths and new binary/generated artifacts. This
heuristic does not replace source review or a dedicated secret scan. Keep local
settings, traces, reports and build products out of commits.

## Unsigned core builds

Use Visual Studio 2022/v143 C++ tools, the Windows SDK, Git and NuGet. Initialize
the pinned submodules and restore packages before building:

```powershell
git submodule update --init --recursive
nuget restore VirtualDesktop-OpenXR.sln
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Build-CommunityRuntime.ps1 -Configuration ReleaseBundle -Platform x64
```

Check `Release` and `ReleaseBundle` on both Win32 and x64. The helper builds only
the runtime project, keeps fresh outputs/objects under ignored `bin/community/`,
disables inherited version-edit/signing events and sets `/fp:precise`. Use fresh
output directories and matching private headers/objects. The helper's argument
handling is tested with Windows PowerShell 5.1. Core builds do not validate the
installer or modify the selected system runtime.

Add `-Analyze` to compare baseline and candidate diagnostics with identical
tools and settings. The regression workflow rejects new diagnostic identities
or occurrence counts; inherited diagnostics still require separate consideration.
The original installer workflow remains unchanged.

## Runtime tests

Follow [tests/README.md](tests/README.md) and
[tests/vr_input_README.md](tests/vr_input_README.md). Link fixtures against a fresh
matching x64 ReleaseBundle object cohort. Run hardware/backend processes
sequentially with timeouts and require documented `PASS:` markers plus exit
success. Record the source revision, backend, case and coverage limits.

The CPU workflow stages the pinned LunarG Vulkan loader and license only beside
test executables; it does not install a driver or replace system files. The
watcher fixture uses a temporary HKCU key and process-private registry mapping.
GPU, native headset, image-quality and streaming-latency measurements are
separate from hosted CPU checks and selected Khronos assertions.
