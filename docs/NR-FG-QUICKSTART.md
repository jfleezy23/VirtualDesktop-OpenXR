# VDXR NR and OFXR frame generation

This experimental Windows x64 package adds neural rendering and optional PC-side **OFXR 2×/3×** generation to the
Virtual Desktop OpenXR runtime. It includes the runtime, optional synthesis DLL and NR Controls. It uses Virtual Desktop
Streamer and the headset's existing Virtual Desktop connection. It does not provide a Steam Frame streaming runtime.

The package was tested locally on an RTX 4090. Compatible NVIDIA hardware and a separately supplied, licensed
`nvngx_dlssnr.dll` are required for NR. The proprietary NR DLL is not included in this repository or release.

Use a current NVIDIA display driver and the latest
[Microsoft Visual C++ v14 x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/).
The synthesis DLL uses the Microsoft C++ runtime; the host also needs the Vulkan loader supplied by the graphics driver.
End users do not need Visual Studio or a compiler. Python is needed only for the controls window, as described below.

## Install

1. Install Virtual Desktop Streamer normally and connect the headset. Close all VR games before replacing runtime files.
2. Download the Windows x64 NR/FG package from [Releases](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases)
   and extract it. Keep the matching runtime and synthesis DLL together.
3. Back up the existing files with these names from `%ProgramFiles%\Virtual Desktop Streamer\OpenXR`:
   `virtualdesktop-openxr.dll`, `vdxr-frame-generation.dll` and `vdxr_registry_gui.py`. Record any that were absent.
4. Copy the package's three files into that OpenXR folder, accepting Windows administrator access when prompted.
   Leave the runtime JSON, stock 32-bit DLL and other files intact. Keep your licensed `nvngx_dlssnr.dll` beside the runtime if you want NR. FG with NR off does not initialize NR or require this vendor DLL.
5. In Streamer's Options, select **VDXR** as the OpenXR runtime. Fully restart the game after installation.

The files do not change runtime registration, existing NR settings, or the saved FG mode when copied.
Streamer updates may replace a custom runtime; retain the package and backups.

## Open NR Controls

Install **64-bit Python 3.13**, including Tcl/Tk and the Python launcher. From PowerShell in the OpenXR folder, run:

```powershell
pyw -3.13 .\vdxr_registry_gui.py
```

The window requests administrator access to save settings. The release UI targets VD Streamer's 64-bit runtime registry.
You can create a desktop shortcut to the same command. Opening the panel only reads settings; it does not reset them.

## Choose a mode

1. Turn **SSW Off** in Virtual Desktop. Disable any external OFXR frame-generation layer for this comparison.
2. In NR Controls, leave NR off for ordinary frame generation, or enable it for neural rendering plus FG. Keep **FoveationSize / NR coverage at 100%** when NR is on; FG does not lower coverage or change face/hair settings.
3. Select **Off**, **OFXR 2×** or **OFXR 3×** under PC frame generation. The selection saves immediately.
4. Close and relaunch the game after changing FG mode. NR strength/detail controls can be adjusted during play.

The **PC frame-generation backend** selector offers **NVIDIA (default)** and experimental
**FidelityFX (full resolution)**. It saves immediately; restart the game after changing it.
NVIDIA retains its existing Medium/50% optical-flow settings. FidelityFX uses the full per-eye image
for motion estimation. This setting does not change game resolution, NR coverage, or NR detail controls.
Keep NR coverage at 100%. Compare hair, faces and head motion before choosing a backend for regular play.
An explicit FidelityFX request requires a matching module; an older or unsupported module leaves FG
unavailable for that session and retains real frames, with the cause recorded in the runtime log.

Off disables OFXR and retains the selected ordinary or NR processing path. OFXR 2× adds one synthetic frame between real frames; 3× adds two when the runtime's eligibility
and deadline checks pass. These are PC-generated frames; VD's separate headset-side SSW should remain off for this test.
The selector shows the saved request, not proof that FG is active in the current game.

FG accepts an eligible full color-only stereo projection through VDXR. With NR off, the current independent path supports DirectX 11 and DirectX 12. When NR is on, coverage must be at least 90%; use 100% for this configuration. Unsupported depth, overlay or format/layout submissions retain the ordinary path.
Games running through native Oculus or SteamVR do not acquire these features simply because the files are installed;
those titles need a compatible OpenXR translation path. NR and OFXR are independent: NR off with OFXR 2× or 3× processes the ordinary final stereo images. Unsupported inputs or missed deadlines can still fall back to real frames.

## First check

Use fresh Off / 2× / 3× sessions at the same refresh rate and resolution. Check stereo alignment, hair and faces in motion,
occlusions and controller response. Keep NR coverage at 100%. If an artifact or startup issue occurs, select FG Off and
restart the game. The selected mode is a request, not confirmation that every frame was generated. Confirm actual
synthetic submissions when validating a new game; perceived smoothness and a displayed FPS counter alone do not establish
which component supplied the frames.

Rapid head turns can still produce interpolation artifacts. This remains an experimental frame-generation path;
the recovery and synchronization corrections do not establish artifact-free head motion in every title.

### Automatic color handoff

The shared runtime handoff supports 8-bit RGBA and BGRA, including linear/sRGB and typeless textures. Both eyes may use
different channel layouts. Packed, separate, cropped, and resolved array/mip images use the same path; no game profile or
launch flag is needed. BGRA conversion preserves encoded color and alpha bytes. NR output, coverage, and appearance settings
are unchanged, and the existing RGBA copy path remains available.

Unsupported higher-precision or X8 formats retain the original real-frame path rather than being silently reduced to
8-bit input. For development captures, `FG_Capture` records accepted slot plans and `FG_Present` identifies the source serial,
slot, and whether a native submission was synthetic or a fallback repeat. These are software submissions, not a measurement
of physical headset scanout. Regular gameplay telemetry remains off.

## Restore the original files

With games closed, restore the backed-up three files. Remove only a package file that was absent before installation.
Set FG Off before reverting if you want the saved setting cleared; restoring files by itself preserves registry settings.
Do not replace the stock 32-bit runtime or proprietary NR DLL as part of this rollback.

## Source and licenses

The runtime retains its original MIT license. The optional synthesis module includes the pinned OFXR core under LGPL-3.0,
with FidelityFX and NVIDIA interface notices retained. Each binary release includes licenses and a matching corresponding
source archive. See [the module notice](../nr-frame-generation/NOTICE.md) and [the regression/build guide](../tests/frame-generation.md).
