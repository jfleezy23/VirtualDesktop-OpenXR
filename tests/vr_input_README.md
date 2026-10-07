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

The native settings watcher has a separate CPU regression:

```powershell
.\tests\vr_input_build.cmd -TestSource vr_input_watcher_regression.cpp
.\bin\vr-input-tests\vr_input_watcher_regression.exe
```

It signals the production notification event, pauses an actual registry read with
Detours, and verifies owner teardown waits for the callback to finish. A callback
delivered after detachment must leave the owner untouched. This test reads the
existing settings key and changes no registry values. The baseline-only build flag
uses the original WIL watcher from a captured old header/object set to demonstrate
its two failing lifecycle checks.

The same executable also injects an allocation failure immediately after the native
wait is created. It observes the actual wait API and requires the constructor to
throw before arming the wait, so constructor unwinding cannot free a live callback
context. `--constructor-only` runs just that case. The historical regression was
verified against the captured pre-deferred-arm instance object, then against a
fresh complete runtime build.

Live controller settings and asymmetric visibility queries have another CPU case:

```powershell
.\tests\vr_input_build.cmd -TestSource vr_input_settings_regression.cpp
.\bin\vr-input-tests\vr_input_settings_regression.exe 'C:\path\to\OVRNull\'
```

The real settings refresh is paused at its final controller pose registry read
while a consumer holds the production actions/spaces mutex. Publishing all pose
offsets and cache invalidations must wait for that reader to release its lock. The
test also calls the real visibility-mask API with a nonzero vertex capacity and
zero index capacity, both with and without an index pointer. Both capacities must
be treated as zero: only counts are returned, and caller buffers stay untouched.
Only the external OVR stencil function is replaced with a fixed count fixture.
OVRNull is explicitly loaded for that CPU API; no OVR session or graphics device is
created and no registry value is written.

Two more CPU regressions exercise Vulkan rollback and visibility event lifetime:

```powershell
.\tests\vr_input_build.cmd -TestSource vr_input_vulkan_cleanup_regression.cpp
.\bin\vr-input-tests\vr_input_vulkan_cleanup_regression.exe
.\tests\vr_input_build.cmd -TestSource vr_input_visibility_events_regression.cpp
.\bin\vr-input-tests\vr_input_visibility_events_regression.exe
```

The Vulkan case invokes actual initialization with local dispatch functions that
report an invalid LUID before device ownership is published. Its subsequent real
cleanup must make no device calls. It also covers device-only and pool-only partial
initialization, independent fence cleanup, full resource cleanup, and idempotent
repeat cleanup. Local spies replace all Vulkan APIs reached by the fixture, so no
driver or graphics device is used.

The visibility case calls real event polling before session creation, without the
extension, and after actual empty CPU-only session destruction. It requires no
invalid or unsupported visibility events, cleared pending events at destruction,
and exactly one event per eye for an enabled live session after a quiescent change.

`nonnr_resolution_regression.cpp` runs the production FSR-plus-sharpen upscaler on
D3D11 WARP with intercepted OVR output allocation. Build with
`vr_input_build.cmd -TestSource nonnr_resolution_regression.cpp`, then pass a mode
and an OVRNull directory. Modes `resize` and `reuse` check changing dimensions,
stable resource reuse and distinct-eye pixels; `texture`, `srv`, and `uav` (plus
their `-right` variants) inject creation failures and require the prior complete
intermediate entry to survive, followed by complete same-resolution recovery.
These are resource correctness tests, not headset latency measurements.
