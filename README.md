# VirtualDesktop-OpenXR

An independent development fork of [VirtualDesktop-OpenXR](https://github.com/mbucchia/VirtualDesktop-OpenXR),
maintained by [jfleezy23](https://github.com/jfleezy23). The runtime supports OpenXR applications through
Virtual Desktop on Windows without requiring SteamVR.

Development focuses on runtime correctness, resource efficiency, and experimental neural rendering. General runtime
changes and NR development are maintained on separate branches, with regression tests and independent code review.

## Two branches, two purposes

| Branch | What belongs here |
|---|---|
| [stable](https://github.com/jfleezy23/VirtualDesktop-OpenXR/tree/stable) (default) | General runtime fixes and small, testable reductions in CPU work. No NR/NGX integration. |
| [experimental-nr](https://github.com/jfleezy23/VirtualDesktop-OpenXR/tree/experimental-nr) | The NR development line, including shared runtime fixes and experimental NR resource handling. x64 builds only. |

The upstream references are recorded by commit in the contribution guide; this fork has no separate public `main` branch.
`stable` describes the branch's purpose, not a release or a promise of compatibility with every game.

## What's different in this fork

The general runtime work includes corrections to frame/session and swapchain lifetimes, interop cleanup and retries,
depth alignment, input state, buffer bounds, and runtime loading. It also caches preprocessing shaders and constant
buffers, replaces per-frame layer-pointer heap storage with bounded stack storage, and removes unused precompositor
render-target views.

On the NR branch, inputs are prepared only when that frame enables NR. Imports become visible only after a complete
generation is ready, and failed NT imports retain the owned export needed for retry. NR coverage is configurable.

These changes have local regression and build evidence. That evidence covers specific correctness and resource-creation
behavior; it does not establish a measured headset latency, FPS, or image-quality improvement.

## Building and testing

Start with [CONTRIBUTING.md](CONTRIBUTING.md) for build prerequisites, source checks, and the supported configurations.
The [runtime regression guide](tests/README.md) and [input regression guide](tests/vr_input_README.md) describe the fixtures
and their requirements.

The community CI workflow checks source hygiene, builds the unsigned core runtime, compares compiler/analyzer diagnostics,
and runs CPU regressions against matching fresh objects and headers. GPU and headset testing are separate local steps.
Core builds do not validate an installer. Proprietary NR vendor DLLs are supplied separately and are not distributed here.

This is an independent community project. For the original runtime's documentation, see the
[upstream wiki](https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki). This fork makes no Khronos conformance claim and
is provided as-is under the [MIT license](LICENSE); third-party notices remain in [THIRD_PARTY](THIRD_PARTY).

## Upstream credit

This work builds on Matthieu Bucchianeri's runtime and the contributions of the Virtual Desktop team. Their authorship,
license, and third-party notices are retained.

Original contributors:

- Matthieu Bucchianeri
- Guy Godin (Virtual Desktop)
- Kyle Hendry (Virtual Desktop)

Additional upstream contributions:

- Vladimir Dranyonkov (bug fixes, on original PimaxXR)
- Sarah Heinzen (Accessibility project, sponsored by Microsoft)
- Jonas Holderman (Accessibility project, sponsored by Microsoft)
- Heather Kemp (Accessibility project, sponsored by Microsoft)
- Mathias Peter Nordskog (bug fixes)
- Pavel Skakov (bug fixes, on original PimaxXR)

To support the original runtime author, visit [Matthieu's GitHub sponsorship page](https://github.com/sponsors/mbucchia).
