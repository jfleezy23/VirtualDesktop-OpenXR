# Build the retained frame-generation module

The module's retained headers live in `nr-frame-generation/module-checkpoint/include` and take
precedence for its build. The host retains its own checkpoint headers. The nine
prepared synthesis files live in `nr-frame-generation/prepared-ofxr`; the
existing `vendor/ofxr` baseline remains unchanged.

The retained qualified module SHA-256 is
`8206b8a8640cd3a2688b531afede29dbd0945ea7ad6e579c087d894f669f48a7`.
Its recorded variant is `dxc-grid`: MSVC 14.51.36231, Windows SDK 10.0.26100.0,
DXC 1.9.2609.5, HLSL 2016, optimization level 3, IEEE-strict mode off, and PGO
disabled. Packaging source paths does not establish that a new build has the
same binary hash or runtime qualification.

Provide the pinned FidelityFX dependency build directory as `VDXR_FG_DEPS`, the
Windows SDK FXC executable as `VDXR_FXC`, and the qualified DXC executable as
`VDXR_DXC`. Supply dependency binaries, licensed SDK files, and model files separately. Use the following from the repository root in Windows CMD:

```bat
cmake -S nr-frame-generation -B bin/fg-source-build ^
  -G "Visual Studio 18 2026" -A x64 -T v145,version=14.51.36231 ^
  -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DBUILD_TESTING=OFF ^
  -DFG_DEPS="%VDXR_FG_DEPS%" -DFXC="%VDXR_FXC%" -DDXC="%VDXR_DXC%" ^
  -DVDXR_FG_DEPTH_ENDPOINT_PREFERENCE=ON ^
  -DVDXR_FG_DEPTH_RECONSTRUCTION=ON ^
  -DVDXR_FG_DEPTH_RECONSTRUCTION_CELL_SUPPORT=ON ^
  -DVDXR_FG_A_OWNED_DEPTH_HISTORY=ON ^
  -DVDXR_FG_RECONSTRUCTION_REUSE=ON ^
  -DVDXR_FG_DEPTH_AVAILABILITY=ON -DVDXR_FG_NVIDIA_GRID4=ON ^
  -DVDXR_FG_SHADER_COMPILER=DXC -DVDXR_FG_DXC_IEEE_STRICT=OFF ^
  -DVDXR_FG_PATCH_MOTION_EXPERIMENT=OFF ^
  -DVDXR_FG_BLEND_PROMOTION_EXPERIMENT=OFF ^
  -DVDXR_FG_BOUNDED_MOTION_EXPERIMENT=OFF
cmake --build bin/fg-source-build --config Release ^
  --target vdxr_frame_generation -- /m:4 /nologo /p:UsePGO=false
```

Build receipts bind the compiled module to its source inputs and output artifacts.
The prepared synthesis sources, retained module headers, shared host contracts, and
validator are part of that source cohort.
