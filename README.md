# VirtualDesktop-OpenXR — NR and frame generation

An independent Windows runtime fork maintained by [jfleezy23](https://github.com/jfleezy23), built on
[VirtualDesktop-OpenXR](https://github.com/mbucchia/VirtualDesktop-OpenXR). Run compatible OpenXR games through
Virtual Desktop with optional neural rendering (NR) and **OFXR 2×/3× PC frame generation**.

**[Download the October 25 NR/FG release](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/tag/nr-fg-2026.10.25)** ·
[Install and try it](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/docs/NR-FG-QUICKSTART.md) ·
[Controls guide](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/docs/NR-FG-CONTROLS.md) ·
[Game routing help](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/docs/GAME-COMPATIBILITY.md)

[Runtime ZIP](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/download/nr-fg-2026.10.25/VDXR-NR-FG-Windows-x64-2026.10.25.zip) · [Controls ZIP](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/download/nr-fg-2026.10.25/VDXR-NR-FG-Controls-Windows-x64-2026.10.25.zip)

## What's new

**NR processing resolution** runs whole-eye NR at a smaller working resolution, then reconstructs the result
at the original output size. Start at **80% processing with 100% coverage**. Your game pixel density and saved
face/hair/detail controls remain separate. Set processing back to **100%** for the native path.

On an RTX 4090, isolated two-eye NR GPU time fell from **6.615 to 5.050 ms** at 2048 × 2048 per eye, and from
**14.374 to 9.643 ms** at 3072 × 3072 per eye, including reconstruction: about **24% and 33% less NR GPU time**.
These are not game-FPS or headset-latency measurements. Quality and performance depend on the game and settings.

Frame generation works with **NR on or off**. NVIDIA is the default optical-flow backend; FidelityFX at full
resolution remains experimental. See the [release notes](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/docs/releases/2026.10.25.md).

![The actual VDXR NR/FG Controls app](https://raw.githubusercontent.com/jfleezy23/VirtualDesktop-OpenXR/nr-fg-2026.10.25/docs/images/controls-overview.png)

*Actual app with example settings; installation preserves existing preferences.*

## What you need

- A Windows x64 PC, the headset version of [Virtual Desktop](https://www.vrdesktop.net/) and its installed Streamer.
- A game that reaches VDXR through OpenXR. Steam or Oculus ownership alone does not establish compatibility.
- For NR: compatible NVIDIA hardware and a separately supplied, compatible licensed `nvngx_dlssnr.dll`.
  **No proprietary NR DLL or neural model is included or downloaded.** NR was tested on an RTX 4090;
  support for every NVIDIA GPU is not established. FG with NR off does not need the NR component.
- Current graphics drivers and the [Microsoft Visual C++ v14 x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/).
  The standalone Controls package needs no Python installation.

This uses Virtual Desktop's Windows streaming connection. It does not install a Steam Frame or ALVR streaming
runtime; Frame Launcher's existing SteamVR route remains separate.

## Try it

1. Download the dated **runtime** and **Controls** ZIPs from the
   [release](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/tag/nr-fg-2026.10.25).
   Follow the [side-by-side installation](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/docs/SIDE-BY-SIDE-RUNTIME.md)
   to install the matching runtime DLLs and metadata in `%ProgramFiles%\VDXR-NR-FG\OpenXR`.
   Streamer's original files stay in place.
2. Extract the **entire** Controls ZIP, keep `_internal` beside the EXE, and run **VDXR-NR-FG-Controls.exe**.
   Select **Custom VDXR** there and **VDXR** in Streamer. Turn Virtual Desktop's **SSW Off** for OFXR.
3. Enable NR, set **NR processing resolution to 80%**, keep **NR coverage at 100%**, and select **OFXR 3× / NVIDIA**.
   Existing preferences survive installation; make these choices explicitly. Fresh settings start with NR and FG
   off, processing 100% and coverage 100%.
4. Relaunch the game after changing runtime, FG mode or backend. Controls save automatically; there is no Apply button.
   Check both eyes, controllers, faces/hair and head motion. Compare processing 100% versus 80% in the same scene.

If the game still uses SteamVR or native Oculus, follow the
[game routing guide](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/docs/GAME-COMPATIBILITY.md).
Some titles need a compatible translation layer or a title-specific fix; routing to VDXR does not guarantee an NR
visual benefit. If interpolation is unsuitable, choose **FG Off** and restart. To return to the official runtime,
close the game, select **Stock VDXR**, then launch again.

## Branches and development

| Branch | Purpose |
| --- | --- |
| [stable](https://github.com/jfleezy23/VirtualDesktop-OpenXR/tree/stable) — default | General runtime fixes. No NR/NGX integration. |
| [experimental-nr](https://github.com/jfleezy23/VirtualDesktop-OpenXR/tree/experimental-nr) | NR and optional OFXR development; Windows x64. NR/FG downloads come from this branch. |

This README is shared between the branches; the dated links above open the NR release's own guides and images.
`stable` describes the general runtime branch, not universal game compatibility. There is no separate public `main` branch.

For source builds, start with [CONTRIBUTING.md](CONTRIBUTING.md). NR/FG build requirements and regression scope are
in the [dated FG development guide](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/tests/frame-generation.md).
Binary downloads include matching corresponding source and license notices; branch source may advance later.

## Credits and licenses

Thanks to **Matthieu Bucchianeri**, **Guy Godin**, **Kyle Hendry**, and the Virtual Desktop team for the original runtime
and platform. Their authorship, the runtime's [MIT license](LICENSE), and inherited [third-party notices](THIRD_PARTY)
are retained. Additional upstream contributors include Vladimir Dranyonkov, Sarah Heinzen, Jonas Holderman,
Heather Kemp, Mathias Peter Nordskog and Pavel Skakov. The accessibility contributions were sponsored by Microsoft.

Thanks to **janblade** for [F5 Superior Scaling in OptiScaler DLSSNR Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass),
which inspired the spatial reconstruction approach. This fork uses an independent spatial implementation,
not F5's full temporal pipeline.

The optional synthesis module builds on [djules75/OFXR-Bridge](https://github.com/djules75/OFXR-Bridge) and includes
**LGPL-3.0** code. AMD FidelityFX and NVIDIA interface notices accompany it. See the
[module notice](https://github.com/jfleezy23/VirtualDesktop-OpenXR/blob/nr-fg-2026.10.25/nr-frame-generation/NOTICE.md)
for component terms and corresponding source. The runtime's MIT license does not replace those terms.

This is an independent community project, provided as-is, with no Khronos conformance claim. For original runtime
documentation, visit the [upstream wiki](https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki).
Support its author through [Matthieu's GitHub sponsorship page](https://github.com/sponsors/mbucchia).
