# Frame delivery and pacing

The experimental runtime lets the next real frame prepare while Virtual Desktop
waits to begin presenting the current frame group. A completed generated image
does not need to reacquire the next frame's producer-context lock before delivery.
This applies to 2×, 3× and 4×, with NR enabled or disabled.

The native SDK's Wait/Begin/End ordering is preserved. GPU completion, bank ownership
and image-retirement fences still protect every delivered image. Pending copies,
failed captures and shared-context paths retain their synchronization. There is no
new pacing sleep, image copy, synthesis pass or appearance control.

The source includes the retained NR/FG contracts and delivery implementation needed
to build this host together. Use matching host and module contracts rather than
mixing intermediate headers or object files. See the
[retained module recipe](build-retained-fg.md) and [build instructions](../CONTRIBUTING.md).
Licensed NR components must be supplied separately; none are included here.

## Local evidence

A steady-flight Aircar capture used NVIDIA 4× at 120 Hz with NR off. The earlier
build delivered 17,611 native handoffs over 147 seconds; this build delivered
10,505 over 88 seconds. After initial resolution adjustments settled:

| Native handoff interval | Earlier build | This build |
| --- | ---: | ---: |
| 99th percentile | 14.704 ms | 8.680 ms |
| Maximum | 31.759 ms | 9.453 ms |
| Intervals longer than 12.5 ms | 321 | 0 |
| Catch-up intervals shorter than 4.167 ms | 320 | 0 |

Both captures retained correct complete generated/real frame ordering, with no
steady-state fallback or padding. The later run rendered 44% more pixels per eye
and was reported smoother. Resolution, scene and connection were not controlled
between runs, so this is a gameplay observation, not a causal latency percentage
or proof of headset display cadence. Startup and deliberate resize hitches are
excluded from the table. Native Begin can still block even with regular delivery.

## Reproduce the synchronization checks

Build a fresh x64 ReleaseBundle runtime and record its source/artifact receipt with
`scripts/fg-build-provenance.py`. The fixture builder takes that cohort directory,
containing `result.json`, the completed `build-source.json` and `obj/`:

```powershell
python tests/build-fg-runtime-fixtures.py $cohort `
  nr_fg_prebegin_overlap_regression.cpp nr_fg_prebegin_fast_begin_regression.cpp `
  nr_fg_prebegin_failure_regression.cpp nr_fg_prebegin_cancel_regression.cpp `
  nr_fg_copy_ready_poison_regression.cpp nr_fg_pending_current_copy_regression.cpp `
  --installation $visualStudioInstallation
```

Build the repository's OVRNull backend separately. For controlled tests, put that
backend's `LibOVRRT64_1.dll`, the matching FG module and a separately supplied NR
component in an isolated asset directory. Do not use that test backend as your
installed gaming runtime. Run each lifecycle fixture in a bounded child process:

```powershell
& "$cohort/tests/nr_fg_prebegin_fast_begin_regression.exe" `
  $assets $uniqueStateDirectory 3 nr-on colour d3d11
```

Exercise `2`, `3`, `4`; `nr-on`, `nr-off`; and `d3d11`, `d3d12`. Overlap tests park
the real next producer and require every current SDK End to finish. Failure and
cancellation tests require both owners to survive until joined cleanup. The two
component tests instead take the FG module path and multiplier; they cover current
copy readiness during opposite-bank capture, pending-copy retention and poison.
Require exit success and the fixture's complete `PASS:` marker, retaining expected
injected failures separately.

The controlled fixtures exercise actual GPU resources and the public submission
worker with an isolated SDK. They do not qualify the real VD SDK or headset.
`native_runtime_delivery_probe` deliberately retains the real SDK loader; use a
connected headset and the documented asset, state, multiplier, NR, depth and API
arguments for native checks. Do not count an unavailable or sleeping headset as a
passed native test.
