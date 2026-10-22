# Optional OFXR synthesis module

`vdxr-frame-generation.dll` dynamically supplies frame synthesis to the experimental VDXR runtime through the retained
C ABI. It includes code from [djules75/OFXR-Bridge](https://github.com/djules75/OFXR-Bridge), pinned to
`07449429c139e5d2070b6f0855ed5323de7cee00` (V443 / 0.2.13.2). Upstream authorship and notices remain in the retained source.

The OFXR-derived module contains **LGPL-3.0** code. Its license is retained in [vendor/ofxr/LICENSE](vendor/ofxr/LICENSE),
with the companion full [GPL-3.0 text](vendor/LICENSE.GPL-3.0.txt). The runtime's top-level MIT license does not replace
these module terms. The optional module can be replaced separately from the runtime.

Builds apply direct stereo-atlas history capture, initialization/lifetime ownership, GPU-state, diagnostic timing, FidelityFX endpoint-validation and
camera-space motion corrections, active-view sampling bounds, nominal optical-flow scaling and optional host-owned real-output copy elision
in `scripts/prepare-fg-vendor.py`, using `src/ffx_endpoint_validation.hlsl`. The script generates corresponding
patches and modified C++/HLSL sources. The unmodified source and upstream/retained hashes are recorded in
[vendor/ofxr/manifest.json](vendor/ofxr/manifest.json).

FidelityFX source is pinned to `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` (1.1.4). Its MIT license, the OFXR LGPL/GPL texts,
and NVIDIA optical-flow interface notices accompany binary releases. The NVIDIA headers retain their original notices;
only trailing horizontal whitespace is normalized in the retained copies. Additional inherited component information is
in [vendor/ofxr/THIRD_PARTY.md](vendor/ofxr/THIRD_PARTY.md).

Binary releases include matching corresponding source for the module, generated derivative and linked FidelityFX code.
Use the release's source archive for the exact binary snapshot; the GitHub branch may advance after publication.
Build instructions are in [tests/frame-generation.md](../tests/frame-generation.md).

The proprietary neural-rendering DLL `nvngx_dlssnr.dll` is supplied separately and is not part of this release.
