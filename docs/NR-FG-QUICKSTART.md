# Try VDXR NR and OFXR frame generation

The [October 25 Windows x64 release](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/tag/nr-fg-2026.10.25)
adds adjustable NR processing resolution and optional **OFXR 2×/3× PC frame generation** to Virtual Desktop's
OpenXR runtime. Start with **80% processing / 100% coverage** to evaluate reduced NR GPU work at the same output size.
Frame generation also works with NR off.

## Before installing

Connect your headset through its Virtual Desktop app and Windows Streamer. You need a compatible OpenXR game,
current graphics drivers and the
[Microsoft Visual C++ v14 x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/).
The graphics driver supplies the Vulkan loader needed by the host.

NR requires compatible NVIDIA hardware and a separately supplied, compatible licensed `nvngx_dlssnr.dll`.
**No proprietary NR DLL or model is included or downloaded.** This build was tested on an RTX 4090; support for
every NVIDIA GPU is not established. FG with NR off does not require the NR component.

End users do not need Python, Visual Studio or a compiler. This package uses Virtual Desktop's Windows connection;
it does not provide Steam Frame or ALVR streaming. Frame Launcher's separate SteamVR route is unchanged.

## Install once

1. Close VR games. Download `VDXR-NR-FG-Windows-x64-2026.10.25.zip` and
   `VDXR-NR-FG-Controls-Windows-x64-2026.10.25.zip` from the
   [release](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/tag/nr-fg-2026.10.25).
   Check them against `SHA256SUMS-2026.10.25.txt`.
2. Follow the [complete side-by-side setup](SIDE-BY-SIDE-RUNTIME.md). It installs the matching host DLL, FG DLL
   and `CUSTOM-RUNTIME.json` under `%ProgramFiles%\VDXR-NR-FG\OpenXR`, then registers the managed path.
   Leave Streamer's official files in place. Restore them first if an older installation replaced them.
3. Extract the **entire** Controls ZIP. Run **VDXR-NR-FG-Controls.exe**, keeping `_internal` beside it.
   Accept the administrator prompt to save settings. This community app is unsigned; opening it preserves
   existing preferences. It does not install runtime DLLs.
4. Select **Custom VDXR** in Controls and **VDXR** in Streamer. The switch takes effect on the next game launch.

For upgrades, use the side-by-side guide's backup and replacement steps with all VR games closed. The new Controls
app can live in its own extracted folder. Existing NR, appearance and FG settings are preserved.

## First comparison

1. Turn Virtual Desktop's **SSW Off**, and disable any separate external FG layer.
2. Enable NR. Set **NR processing resolution (%) to 80** and **NR coverage (%) to 100**.
   Keep your face, hair, tone and detail choices as they are.
3. Select **OFXR 3×** and **NVIDIA (default)**. Close and relaunch the game after changing FG mode, backend or runtime.
4. Check that both eyes align, controllers work, and faces, hair, dark scenes and head motion look acceptable.
   Compare processing **100%** against **80%** in the same scene, with game resolution and other settings held fixed.

![The actual Controls app with the spatial NR control](images/controls-overview.png)

*Actual app with example settings, not installation defaults.*

Changes save automatically; there is **no Apply button**. NR processing, coverage and appearance controls can be
adjusted during play. Fresh settings start with NR off, processing **100%**, coverage **100%**, FG off and NVIDIA
selected. Installation does not enable the recommended configuration for you.

Processing resolution and coverage do different jobs: **80% processing** uses a smaller whole-eye NR working image
and reconstructs its result at native output size. **100% coverage** keeps NR across the whole eye. Game pixel
density and final output dimensions do not change. Processing **100%** bypasses the new spatial work; zero NR
strength preserves the original RGB. Saved appearance settings are not replaced by an 80% preset.

In an isolated RTX 4090 benchmark,
complete two-eye NR GPU time improved from 6.615 to 5.050 ms at 2048 × 2048 per eye, and from 14.374 to 9.643 ms
at 3072 × 3072, including reconstruction. These are NR-region timings, not game-FPS or latency measurements.
See the [release notes](releases/2026.10.25.md) for scope and remaining limits.

## Choose your combination

| NR | FG | Result |
| --- | --- | --- |
| Off | Off | Ordinary custom-runtime rendering |
| On | Off | NR without PC frame generation |
| Off | OFXR 2× or 3× | PC frame generation from the ordinary stereo images |
| On | OFXR 2× or 3× | NR followed by PC frame generation; keep coverage at 100% |

OFXR 2× can add one generated frame between real frames; 3× can add two when eligibility and timing allow.
Unsupported submissions and missed deadlines retain real frames. The selector shows the saved request, not proof
that every frame was generated. This is separate from Virtual Desktop's headset-side SSW.

Start with NVIDIA. **FidelityFX (full resolution)** remains experimental and may show interpolation artifacts.
Restart the game after changing backends. Backend selection does not change NR coverage, appearance controls or
game pixel density. An explicit unsupported/mismatched backend request can leave FG unavailable for that session.

## Check the game's route

Use the Virtual Desktop overlay's **Runtime: VDXR** indication to check that the game reaches VDXR. It does not
identify the custom build or confirm NR/FG activity. A saved Custom selection alone does not prove a running game
loaded it; restart the game after switching.

Steam and Oculus games may require different launch choices or compatible translation layers. Follow the
[game routing guide](GAME-COMPATIBILITY.md) before changing files or settings. Native Oculus and OpenVR games are
not automatically converted by selecting VDXR. Routing a game successfully does not guarantee an NR visual benefit.

## If something looks wrong

- For ghosting or interpolation artifacts, choose **FG Off** and relaunch. NR can remain on independently.
- To compare NR quality, keep coverage at **100%** and return processing to **100%**, or disable NR.
- If settings fail to save, read the Controls status line and check administrator access.
- If custom loading fails after a Streamer update, return to Stock and check the
  [side-by-side compatibility notes](SIDE-BY-SIDE-RUNTIME.md#updates-and-verification).
- To return to the official runtime, close the game, select **Stock VDXR**, then launch again. Streamer may stay open.
  If an older install overwrote vendor DLLs, restore their backup or reinstall/update Streamer first.

Rapid head turns, ghosting and exposure-change artifacts can remain. This release does not establish universal
game compatibility, artifact-free motion or identical perceived detail at every processing percentage.

## Source and licenses

The host retains its MIT license. The optional synthesis module includes OFXR code under LGPL-3.0, with AMD
FidelityFX and NVIDIA interface notices retained. Matching corresponding source and rebuild instructions accompany
the release; see the [module notice](../nr-frame-generation/NOTICE.md). Thanks to
[janblade's F5 project](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass) for the spatial reconstruction
inspiration. This is an independent spatial implementation, not F5's full temporal pipeline or copied F5 GPL source.
