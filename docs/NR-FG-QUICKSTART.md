# VDXR NR and OFXR frame generation

This experimental Windows x64 package adds neural rendering and optional PC-side **OFXR 2×/3×** generation to the
Virtual Desktop OpenXR runtime. It includes the runtime, optional synthesis DLL and NR Controls. It uses Virtual Desktop
Streamer and the headset's existing Virtual Desktop connection. It does not provide a Steam Frame streaming runtime.

The package was tested locally on an RTX 4090. Compatible NVIDIA hardware and a separately supplied, licensed
`nvngx_dlssnr.dll` are required for NR. The proprietary NR DLL is not included in this repository or release.

## Install

1. Install Virtual Desktop Streamer normally and connect the headset. Close all VR games before replacing runtime files.
2. Download the Windows x64 NR/FG package from [Releases](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases)
   and extract it. Keep the matching runtime and synthesis DLL together.
3. Back up the existing files with these names from `%ProgramFiles%\Virtual Desktop Streamer\OpenXR`:
   `virtualdesktop-openxr.dll`, `vdxr-frame-generation.dll` and `vdxr_registry_gui.py`. Record any that were absent.
4. Copy the package's three files into that OpenXR folder, accepting Windows administrator access when prompted.
   Leave the runtime JSON, stock 32-bit DLL and other files intact. Keep your licensed `nvngx_dlssnr.dll` beside the runtime.
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
2. In NR Controls, enable NR and set **FoveationSize / NR coverage to 100%**. Keep your preferred NR style and face/hair settings.
3. Select **Off**, **OFXR 2×** or **OFXR 3×** under PC frame generation. The selection saves immediately.
4. Close and relaunch the game after changing FG mode. NR strength/detail controls can be adjusted during play.

Off retains ordinary NR. OFXR 2× adds one synthetic frame between real frames; 3× adds two when the runtime's eligibility
and deadline checks pass. These are PC-generated frames; VD's separate headset-side SSW should remain off for this test.
The selector shows the saved request, not proof that FG is active in the current game.

FG currently requires NR, at least 90% coverage, and an eligible full color-only stereo projection through VDXR.
Use 100% coverage for this configuration. Unsupported depth, overlay or format/layout submissions retain the ordinary path.
Games running through native Oculus or SteamVR do not acquire these features simply because the files are installed;
those titles need a compatible OpenXR translation path. Turning NR off also bypasses this post-NR FG integration.

## First check

Use fresh Off / 2× / 3× sessions at the same refresh rate and resolution. Check stereo alignment, hair and faces in motion,
occlusions and controller response. Keep NR coverage at 100%. If an artifact or startup issue occurs, select FG Off and
restart the game. The maintainer reported smooth play at 120 Hz with Godlike quality and increased pixel density;
that observation is not a controlled latency benchmark or a guarantee for every game and headset.

## Restore the original files

With games closed, restore the backed-up three files. Remove only a package file that was absent before installation.
Set FG Off before reverting if you want the saved setting cleared; restoring files by itself preserves registry settings.
Do not replace the stock 32-bit runtime or proprietary NR DLL as part of this rollback.

## Source and licenses

The runtime retains its original MIT license. The optional synthesis module includes the pinned OFXR core under LGPL-3.0,
with FidelityFX and NVIDIA interface notices retained. Each binary release includes licenses and a matching corresponding
source archive. See [the module notice](../nr-frame-generation/NOTICE.md) and [the regression/build guide](../tests/frame-generation.md).
