# Spatial NR developer regressions

These tests cover the optional whole-eye NR working grid and native-guided reconstruction. They do not establish proprietary NR inference, headset image quality, streaming latency, or a performance improvement.

## Toolchain and inputs

Use the MSVC **14.51.36231** tools directory and Windows SDK **10.0.26100.0**. Both spatial runners accept `-VCToolsDirectory` and otherwise check `VCToolsInstallDir`, the current compiler, the existing local custom tools location, and Visual Studio 18.x installations reported by `vswhere`. The directory version must match; a serviced compiler binary must report version 14.51 with servicing build at least 36231. Each invocation prints the actual binary version. An older compiler is rejected.

The SDK comes from `WindowsSdkDir` or the standard Windows Kits directory. No runner installs dependencies or changes installed runtime registration or product settings. The linked fixture requires the repository's usual external headers, libraries, and restored packages, as for a normal C++ runtime build.

## Standalone policy and shader tests

From the repository root:

```powershell
# CPU policy only; suitable after native toolchain setup in CI.
.\tests\build-nr-spatial-regression.ps1 -PolicyOnly `
    -OutputDirectory bin/nr-spatial-policy-01

# D3D12 WARP: scalar-oracle comparisons and resource lifetime checks.
.\tests\build-nr-spatial-regression.ps1 `
    -OutputDirectory bin/nr-spatial-warp-01

# Actual hardware correctness checks; run separately from games or GPU timing.
.\tests\build-nr-spatial-regression.ps1 -Hardware `
    -OutputDirectory bin/nr-spatial-hardware-01
```

An explicit compiler selection, when needed, is the directory ending in `VC\Tools\MSVC\14.51.36231`, supplied through `-VCToolsDirectory` on either runner. The standalone default output is `bin/nr-spatial-standalone`; distinct output directories make comparisons easier to retain.

The policy test covers percentage bounds, rounding/alignment, small or ineffective reductions, and format eligibility. The shader test uses an independent scalar oracle for area filtering, storage encoding, signed edits, and reconstruction. It exercises RGBA/BGRA, UNORM/sRGB, unequal/offset eyes, depth, array-slice-zero imports, changed inputs, descriptor reuse, delayed completion, and state round trips. Synthetic identity edits are not vendor qualification.

Standalone negative controls compile changed shader copies under their output directory. `-ShaderControl ZeroResidual` and `-ShaderControl EncodedArea` must fail the relevant oracle comparisons. They do not modify production shaders.

## Runtime-linked six-mode fixture

Build a fresh ReleaseBundle runtime cohort first, then use its `obj` directory with the exact matching sources and headers. For example:

```powershell
.\scripts\Build-CommunityRuntime.ps1 -Toolset v145 `
    -Configuration ReleaseBundle -OutputDirectory bin/nr-spatial-host-01

# Compile/link only: does not run a GPU workload.
.\tests\build-nr-spatial-runtime-regression.ps1 `
    -RuntimeObjectDirectory bin/nr-spatial-host-01/obj `
    -CaseName compile-01 -CandidateSpatial

# Explicitly select a source-built x64 OVRNull DLL before running.
$ovrNull = (Resolve-Path '<path-to-OVRNull-LibOVRRT64_1.dll>').Path
.\tests\build-nr-spatial-runtime-regression.ps1 `
    -RuntimeObjectDirectory bin/nr-spatial-host-01/obj `
    -CaseName six-modes-01 -CandidateSpatial -Run -OvrNull $ovrNull
```

`SourceDirectory` defaults to `virtualdesktop-openxr` in this checkout. `RuntimeObjectDirectory` and a fresh `CaseName` are required. Never link changed `runtime.h` layouts against old runtime objects. Outputs, logs, a private backend, and input SHA-256 records go to ignored `bin/nr-spatial-runtime/<CaseName>`; an existing case directory is rejected.

The runner reuses the retained runtime fixture's compiler and linker options. It privately recompiles only `system.cpp` with `LOAD_LOCAL_LIBOVR`, substituting that object for backend selection. It compiles `nr_test_module.cpp` as a private NGX facade and copies only the explicitly supplied OVRNull DLL into that backend. The facade supplies handle validation; the fixture records a test-only identity GPU copy for NR evaluation. No vendor NR DLL is used or distributed by this test.

| Mode | Main checks |
| --- | --- |
| `native100` | Native backend geometry and lazy spatial bypass; unchanged final eye packing and pixels. |
| `sequence` | Native/reduced switches, 80/75/90% geometry, unequal eyes and independent depth rectangles, source replacement reset, changing image banks, and native-size final output. |
| `evaluate-failure` | Second-eye evaluation error propagates without publishing a partial frame; feature ownership balances. |
| `create-failure` | Both normal and manual-loader creation attempts fail; no stale output is published. |
| `signal-failure` | Real execution precedes a rejected completion signal; uncertain ownership survives until a blocked native drain completes. |
| `wait-failure` | Accepted submission precedes a rejected D3D11 consumer wait; resources survive until completion and final output is not published. |

Every child has a 90-second bound. Success requires exit zero, exactly one complete `PASS: public runtime <mode>; ...` marker, and no assertion failures or unexpected `FAIL:` lines. Each of the four injected-failure modes intentionally emits exactly one inherited `FAIL: xrEndFrame(session, &end): -2` before its final PASS. The runner preserves and checks that expected diagnostic; it does not suppress it or treat an unrelated failure as success.

For an old frozen baseline, omit `-CandidateSpatial` and pass its matching frozen `SourceDirectory` and object cohort. The two-mode run should retain a native100 pass and a reduced-sequence failure when the setting is ignored. This is a negative baseline, not a successful candidate run.

## Runtime negative controls

With matching current candidate objects, add either `-NegativeControl DropUncertainOwnership` or `-NegativeControl IgnoreWaitFailure` to a fresh linked `-CandidateSpatial -Run` invocation. The runner compiles a changed private copy of `dlssnr_spatial.cpp` and substitutes only that object. The first control runs `signal-failure`; the second runs `wait-failure`. Both must exit nonzero with behavioral assertion/readback evidence. Production sources remain unchanged.

These controls check that the ownership and consumer-wait assertions can detect regressions through actual queued spatial work. They do not replace the normal six-mode pass, real vendor qualification, or separate headset and timing evaluation.
