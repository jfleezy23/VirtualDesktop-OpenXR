# VR input regressions

`vr_input_regression.cpp` calls the real runtime's action and hand methods. A single
friend declaration gives this test access to seed state; no algorithms or runtime
methods are replaced by mocks. Its default invocation performs CPU tests and reads
no headset input. Registry watchers are stopped immediately after each runtime
construction.

Build a fresh `ReleaseBundle|x64` runtime and link the harness against that build's
object directory. The runtime header and all objects must belong to the same build,
because this harness accesses runtime fields directly. For example, after building
the runtime into `bin/code-analysis/obj`:

```powershell
.\tests\vr_input_build.cmd
.\bin\vr-input-tests\vr_input_regression.exe
```

For another fresh build, pass `-RuntimeObjectDirectory <object-directory>`. The
`-SourceDirectory` and `-RecompileInputSources` options support a captured baseline:
the latter recompiles only action, hand tracking, instance, and session sources.
Use those options only when the captured header matches the other runtime objects.
Generated objects and executables remain under ignored `bin/vr-input-tests`.

The tests cover first-sync generation, initially empty input caches, all action
sets becoming inactive after an unfocused sync, stale data from an inactive hand,
cross-hand buttons requiring two active hands, and unbound or unmatched pose
actions explicitly reporting inactive. Positive cross-hand contact is checked for
both hands, and a bound active controller pose must still report active.

An optional integration case requires the OVRNull test runtime and a graphics
adapter. Run it sequentially with other GPU tests:

```powershell
.\bin\vr-input-tests\vr_input_regression.exe --session-reset 'C:\path\to\OVRNull\'
```

This explicitly loads OVRNull from the supplied directory, poisons the cached input,
and creates/destroys two headless sessions. Each new session must expose an empty
input cache before its first sync. The optional case does not change the selected
system OpenXR runtime or registry settings.

The focus-loss expectation follows the normative OpenXR input specification:
[`xrSyncActions`](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#xrSyncActions)
requires all session action states to be inactive when it returns
`XR_SESSION_NOT_FOCUSED`.
