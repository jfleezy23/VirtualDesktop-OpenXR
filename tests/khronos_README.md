# Targeted Khronos checks

Use the approved [OpenXR CTS 1.1.63.0 x64 release](https://github.com/KhronosGroup/OpenXR-CTS/releases/tag/openxr-cts-1.1.63.0).
Keep the complete bundle together and run from its directory: CTS sets `XR_API_LAYER_PATH` to that directory.
Verify the release artifact checksum before execution. These selected tests do not constitute full conformance.

Run with a normal user token. Set `XR_RUNTIME_JSON` only in the test process environment to the manifest for the candidate
runtime. Check the runtime log to confirm the actual DLL directory, backend, adapter and graphics API. An elevated loader
may ignore process-local runtime selection. Keep NR off and close games before testing.

For a native headset pass, connect Virtual Desktop and wear the headset. The following commands assume the correct runtime
has already been selected for this process and that `results/` exists. Repeat each command with `d3d12` and distinct output
filenames. Do not run two graphics tests concurrently.

```text
conformance_cli.exe "ProjectionArraySwapchain,ProjectionWideSwapchain" -G d3d11 --minApiVersion 1.0 -V Stereo -L XR_APILAYER_KHRONOS_runtime_conformance --reporter ctsxml::out=results/stereo-d3d11.xml --reporter console
conformance_cli.exe "Timed_Pipelined_Frame_Submission" -G d3d11 --minApiVersion 1.0 -L XR_APILAYER_KHRONOS_runtime_conformance --reporter ctsxml::out=results/pipeline-d3d11.xml --reporter console
conformance_cli.exe "SwapchainsRender,SwapchainsUnorderedAccess" -G d3d11 --minApiVersion 1.0 -L XR_APILAYER_KHRONOS_runtime_conformance --reporter ctsxml::out=results/swapchains-d3d11.xml --reporter console
```

Allow 180 seconds for visual projection checks and 60 seconds for pipeline checks. A watchdog must terminate only its own
test process. A timeout is inconclusive. Follow CTS's headset prompts to judge stereo, array/wide layouts and depth cases.
Adding `--autoSkipTimeout 3000` exercises projection paths without requiring a visual judgment; an auto-skipped prompt is
not a stereo pass. Record frame-pacing numbers as measurements for that backend, not end-to-end streaming latency.

For direct invalid-handle/type checks and the focused sync regression:

```text
conformance_cli.exe "FrameSubmission,Swapchains,SwapchainsAcquire,xrLocateViews" -G d3d11 --minApiVersion 1.0 -H -T --reporter ctsxml::out=results/contracts-direct-d3d11.xml --reporter console
conformance_cli.exe "xrSyncActions" -G d3d11 --minApiVersion 1.0 -H -T -c Focus -c "subaction path rules" --reporter ctsxml::out=results/scope-direct-d3d11.xml --reporter console
```

CTS intentionally disables its conformance layer when `-H` is selected. Keep this direct-runtime pass separate from a
layer-assisted pass: repeat without `-H` and with `-L XR_APILAYER_KHRONOS_runtime_conformance`. Remove the
`KHRONOS_runtime_conformance_disabled` variable from the child environment. Disable unrelated optional layers only in
that environment if necessary; preserve the global runtime selection and registry settings.

Check both the exit status and XML assertions, failed/error cases, test counts, layer report and warnings. A focus warning
can reflect a queued session transition that the layer has not yet observed; investigate its trace and the CTS source before
patching runtime state. Private-backend tests can exercise real GPU resources, but cannot establish native Virtual Desktop
streaming, controller interaction or visual correctness. A full interactive action test also needs controller input.
