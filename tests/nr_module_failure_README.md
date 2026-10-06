# Isolated NR module failures

`build-module-failure-regression.cmd` compiles a black-box OpenXR dispatch harness
and four **test-only** unsigned NGX facades. The facade produces no rendered image.
Never install it in Virtual Desktop Streamer or use it to evaluate image quality.

Copy a chosen facade as `nvngx_dlssnr.dll` beside an isolated VDXR runtime and
prepare the existing OVRNull/NR-enabled test environment. Run GPU cases sequentially:

```powershell
.\tests\build-module-failure-regression.cmd
.\bin\tests\nr_module_failure_regression.exe 'C:\isolated\virtualdesktop-openxr.dll' partial-create
```

The executable rejects a facade whose exported variant does not match its mode:

| Mode | Facade folder | Checks |
| --- | --- | --- |
| `missing-export` | `bin/tests/module-missing` | Two failed frames cannot publish an incomplete NGX dispatch table; teardown and fresh instance creation succeed. |
| `shutdown-retry` | `bin/tests/module-shutdown` | One vendor shutdown failure returns an API error; dropping the test's VDXR reference leaves its code pinned, a second instance is rejected, and direct same-handle cleanup retry succeeds. |
| `partial-create` | `bin/tests/module-partial` | After one rendered stub frame and session teardown, an actual main-thread registry read throws once after manual NR reinitialization. Failed session creation must immediately pair initialization/shutdown, return a null session, and permit a clean retry. |
| `foreign-import` | `bin/tests/module-import` | The fake module replaces its own actual `GetModuleFileNameW` import with a foreign function while retaining VDXR's shim. Teardown must preserve that foreign pointer and report failure, pin VDXR, then succeed after the fixture restores the owned shim. |

The settings failure uses Detours and changes no registry values. Its thread and
value filter prevents the asynchronous registry watcher from consuming the fault.
The harness records the initialization count inside the fault to prove it ran after
the NR reinitialization, rather than during unrelated setup.

All facades track live feature membership and marker values before dereferencing a
release/evaluation handle. They reject duplicate initialization, device-mismatched
shutdown, shutdown with live features, and stale/foreign handles. Final assertions
require every successful initialization to have one successful shutdown and every
created feature to be released. The facade's intentionally rejected first shutdown
is counted separately from successful shutdowns.

Direct negotiated runtime dispatch preserves a failed instance for retry. The
upstream loader can discard its own instance even when runtime teardown returns an
error; this harness deliberately observes the underlying runtime's behavior.

Generated DLLs, executables, objects, logs, and copied isolated runtime folders stay
under ignored `bin/` and are not source evidence to commit. These tests cover API
ownership and failure handling, not vendor NR rendering or headset behavior.
