# Getting a game onto VDXR

Installing the custom runtime is only one part of setup. Some games reach VDXR directly; others need a compatible
translation layer or a title-specific fix. A Steam or Oculus store label does not tell you which VR interface
a game uses. **A working VDXR route does not guarantee an NR visual benefit or artifact-free frame generation.**

## Start with a working baseline

Connect through Virtual Desktop and launch the game using its normal supported route first. Check stereo and
controllers with **Stock VDXR**, NR off and FG off before adding features. Keep a working SteamVR/native Oculus
launch available so you can return to it if an alternate route fails.

Virtual Desktop recommends launching supported titles from its headset **Games** tab; see its
[official FAQ and compatibility link](https://www.vrdesktop.net/). Use the game's own documented launch choices
where available. Neither this package nor its Controls app supplies games or translation layers.

## Match the game's interface

| Game interface | What to try |
| --- | --- |
| Native OpenXR | Select **VDXR** in Streamer, choose the game's supported OpenXR mode, and relaunch. |
| OpenVR / SteamVR | A compatible OpenComposite OpenXR build may translate the game to VDXR. Check its title/version support first. Keep the original SteamVR route. |
| Native Oculus / LibOVR | Prefer a supported native OpenXR option. Otherwise a compatible OpenXR-capable ReviveXR build, including title-specific patches where required, may be needed. Ordinary Revive configured for OpenVR still routes through SteamVR. |

**OpenXR is an application interface; VDXR is a runtime that implements it.** Selecting VDXR changes where OpenXR
applications run. It does not convert native OpenVR or Oculus calls. The upstream
[VDXR compatibility guide](https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki/Application-Compatibility)
lists title-specific choices; consult the [VDXR setup guide](https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki)
for the original runtime.

Steam launch menus vary between titles and versions. Choose an explicit supported OpenXR option when one exists.
An option named "Oculus" can refer to different interfaces in different games; confirm the resulting runtime rather
than inferring it from the label. Do not apply a generic forced launch flag to every game.

For translations, use the relevant project's documentation and compatible build:
[OpenComposite](https://gitlab.com/znixian/OpenOVR) translates OpenVR to OpenXR, and
[Revive](https://github.com/LibreVR/Revive) supports Oculus SDK translation through OpenVR/OpenXR paths.
There is no universal recipe for every title. This release does not bundle either bridge, a patched ReviveXR
distribution, or licensed game/NR components. Avoid blanket game-DLL replacements; follow the applicable
title/build instructions and retain a way to restore the original files.

## Confirm the route, then enable the features

1. In the Virtual Desktop performance overlay, check **Runtime: VDXR**. A SteamVR or native Oculus indication
   means you have not yet reached this runtime's NR/FG path. VDXR alone does not identify stock versus custom.
2. With the game closed, choose **Custom VDXR** in Controls using the verified
   [side-by-side installation](SIDE-BY-SIDE-RUNTIME.md), then launch again. A saved selection does not replace
   the runtime in a process that is already running.
3. Check stereo, controller poses/buttons and stability before enabling NR or FG. If these fail with NR and FG
   off, resolve the game's route/input compatibility first.
4. Try the [80% processing / 100% coverage setup](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/docs/NR-FG-QUICKSTART.md). Keep game pixel density fixed when
   comparing. Enable OFXR separately, with SSW off, and restart after changing FG mode/backend.

The Controls selectors show requests, not proof that NR/FG executed or that every synthetic frame reached the
headset. For a new-game report, record the game/store/version, launch route, translation build if used, runtime
selection, NR processing/coverage and FG backend/mode. Logs under `%ProgramData%\Virtual Desktop\OpenXR.log`
can help establish the loaded path; remove personal information before sharing them.

## Current limits

Compatibility can change between store editions, game updates and translation-layer builds. **Sniper Elite VR's fusion/stereo problem remains unresolved.** Use a working original route
for affected games rather than assuming the new NR slider fixes routing or stereo issues.

Frame Launcher's separate SteamVR route remains unchanged. This release is a Windows Virtual Desktop runtime,
not a Steam Frame streaming runtime or an ALVR setup.
