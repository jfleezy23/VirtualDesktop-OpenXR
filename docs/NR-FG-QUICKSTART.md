# Try VDXR NR and OFXR frame generation

The [October 27 Windows x64 release](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/tag/nr-fg-2026.10.27)
adds **OFXR 4× PC frame generation** and automatic per-game NR/FG profiles to Virtual Desktop's OpenXR runtime.
NR processing resolution and coverage are independent, including **95% coverage / 80% processing** and
**90% coverage / 85% processing**. FG offers Off, 2×, 3× and 4× and also works with NR off.

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

1. Close VR games. Download `VDXR-NR-FG-Windows-x64-2026.10.27.zip` and
   `VDXR-NR-FG-Controls-Windows-x64-2026.10.27.zip` from the
   [release](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/tag/nr-fg-2026.10.27).
   Check them against `SHA256SUMS-2026.10.27.txt`.
2. Follow the [complete side-by-side setup](SIDE-BY-SIDE-RUNTIME.md). It installs the matching host DLL, FG DLL
   and `CUSTOM-RUNTIME.json` under `%ProgramFiles%\VDXR-NR-FG\OpenXR`, then registers the managed path.
   Keep this release's host, FG module and metadata together: 4× requires the matching new host and module.
   Leave Streamer's official files in place. Restore them first if an older installation replaced them.
3. Extract the **entire** Controls ZIP. Run **VDXR-NR-FG-Controls.exe**, keeping `_internal` beside it.
   Accept the administrator prompt to save settings. This community app is unsigned; opening it preserves
   existing preferences. It does not install runtime DLLs.
4. Select **Custom VDXR** in Controls and **VDXR** in Streamer. The switch takes effect on the next game launch.

For upgrades, use the side-by-side guide's backup and replacement steps with all VR games closed. The new Controls
app can live in its own extracted folder. Existing NR, appearance and FG settings are preserved.

## First comparison

1. Turn Virtual Desktop's **SSW Off**, and disable any separate external FG layer.
2. Choose **Globals** for defaults, or select the actual game EXE with **Add game EXE…** for a game profile.
   Enable NR, set **NR coverage (%) to 95** and **NR processing resolution (%) to 80**.
   Keep your face, hair, tone and detail choices as they are. NR may also remain off for FG-only use.
3. Select **OFXR 4×** and **NVIDIA (default)**. Globals save automatically; in game scope, click
   **Save game profile**. Close and relaunch the game after changing FG mode, backend or runtime.
4. Check that both eyes align, controllers work, and faces, hair, dark scenes and head motion look acceptable.
   Compare processing **100%** against **80%** in the same scene, with game resolution and other settings held fixed.

![The actual Controls app with the spatial NR controls](images/controls-overview.png)

*Actual app with example settings, not installation defaults.*

Globals save automatically; game-profile edits remain drafts until **Save game profile**. Saved NR enable,
processing, coverage and appearance changes can apply during play. FG mode and backend are captured at session
start and require a game restart. Fresh settings start with NR off, processing **100%**, coverage **100%**, FG off
and NVIDIA selected. Installation does not enable the comparison settings for you.

Processing resolution (**50–100%**) scales NR's working width and height within the selected centered coverage
rectangle. Effective coverage is **10–100%** per eye; the slider's lower requests clamp to the 10% runtime minimum.
Both percentages describe linear dimensions. Reduced processing reconstructs NR within that rectangle and blends
its crop edge smoothly; outside coverage, the original image and alpha remain. At 100% coverage there is no crop
edge. Game pixel density and final output dimensions do not change. Processing **100%** uses the native NR path;
zero NR strength preserves the original RGB. Your appearance settings remain separate. See the
[Controls guide](NR-FG-CONTROLS.md) for the transition and mixed-setting details.

## Save an automatic game profile

Use the executable that actually runs the game, rather than its launcher or a shortcut. Adding or selecting an EXE
writes nothing. A new profile inherits effective settings from Globals; edit them and click **Save game profile**.
Save stores all ten displayed values: NR enable, style, strength, local tone, structure detail, skin detail, processing
resolution, coverage, FG mode and optical-flow backend. Later global changes do not alter that saved setup.
You can also save without edits to lock a working global setup for one game.

**Reset game profile** removes the overrides and immediately restores global inheritance. Unsaved game edits remain
drafts; switching scope or closing offers Save, Discard or Cancel. Global **Reset NR settings** resets only NR and
preserves the FG selections. The separate Runtime selector applies to the installation, rather than an individual game.

The custom runtime selects the matching profile automatically when that EXE launches. Controls can be closed, and
you do not need to select the profile before each launch. There is no automatic game-library scan. Executables in
different folders have separate profiles, even when their filenames match. Moving a game EXE needs a new profile;
updating it at the same path preserves the profile. Install the matching new runtime before relying on profiles.

## Choose your combination

| NR | FG | Result |
| --- | --- | --- |
| Off | Off | Ordinary custom-runtime rendering |
| On | Off | NR without PC frame generation |
| Off | OFXR 2×, 3× or 4× | PC frame generation from the ordinary stereo images |
| On | OFXR 2×, 3× or 4× | NR followed by PC frame generation; NR coverage must be at least 90% for FG eligibility |

OFXR 2× can add one generated frame between real frames; 3× can add two and 4× can add three when eligibility and timing allow.
Unsupported submissions and missed deadlines retain real frames. The selector shows the saved request, not proof
that every frame was generated or reached the headset. This is separate from Virtual Desktop's headset-side SSW.
The 4× mode is not a measured fourfold game-FPS improvement.

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

- For ghosting or interpolation artifacts, choose **FG Off**, save if editing a game profile, and relaunch.
  NR can remain on independently.
- To compare NR quality, retain your coverage and return processing to **100%**, or disable NR.
- If a global change does not affect a game, check whether its saved profile overrides that setting. Save the changed
  game profile, or use **Reset game profile** to inherit Globals again.
- If settings fail to save, read the Controls status line and check administrator access.
- If custom loading fails after a Streamer update, return to Stock and check the
  [side-by-side compatibility notes](SIDE-BY-SIDE-RUNTIME.md#updates-and-verification).
- To return to the official runtime, close the game, select **Stock VDXR**, then launch again. Streamer may stay open.
  If an older install overwrote vendor DLLs, restore their backup or reinstall/update Streamer first.

Rapid head turns, ghosting and exposure-change artifacts can remain. This release does not establish universal
game compatibility, artifact-free motion or identical perceived detail at every processing percentage.
The recorded Moss check used NVIDIA 4× with NR at **90% coverage / 85% processing** and passed stereo, controller
and motion checks. Both backends also passed 4× fixtures at **4096 × 4096 per eye**; that resolution result is a
fixture check, not a headset performance result. See the [release notes](releases/2026.10.27.md) for validation scope.

## Source and licenses

The host retains its MIT license. The optional synthesis module includes OFXR code under LGPL-3.0, with AMD
FidelityFX and NVIDIA interface notices retained. Matching corresponding source and rebuild instructions accompany
the release; see the [module notice](../nr-frame-generation/NOTICE.md). Thanks to
[janblade's F5 project](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass) for the spatial reconstruction
inspiration. This is an independent spatial implementation, not F5's full temporal pipeline or copied F5 GPL source.
