# VDXR NR/FG Controls — Windows x64

Extract the entire ZIP to a local folder, then open **VDXR-NR-FG-Controls.exe**.
Keep `_internal` beside the EXE. Python is included; no Python installation is needed.
Windows requests administrator access because Virtual Desktop saves these settings in
the machine-wide, 64-bit OpenXR registry key. The app is an unsigned community build.

Use this with the experimental NR/FG runtime from this repository and an existing
Virtual Desktop Streamer installation. The controls do not install, replace or select
a runtime. No DLSSNR DLLs, models, VR runtime DLLs or games are included or downloaded.
NR requires a separately supplied, compatible licensed NR component. FG can run with NR off.

## Settings

- **NR enable and sliders:** save automatically and apply during play. Coverage defaults
  to **100%**. Existing saved values are preserved when the window opens.
- **NR coverage:** saves after a short pause while dragging. Closing the app flushes the
  last pending change. A failed save is reported rather than silently discarded.
- **Frame generation:** choose Off, OFXR 2× or OFXR 3× and NVIDIA or FidelityFX.
  Restart the game after changing either selector. A saved request does not prove FG is
  active. NVIDIA is the default; FidelityFX full resolution remains experimental.
- **Reset NR settings:** disables NR and immediately saves the NR defaults, including
  100% coverage. It preserves both FG selections. This replaces your saved NR appearance
  choices; simply opening or closing the window does not reset them.

Use 100% coverage for NR and turn Virtual Desktop's SSW off when testing OFXR. The app
does not control codecs, headset settings or Frame Launcher's separate SteamVR route.
The window can be closed during gameplay. Uninstall by deleting its extracted folder;
saved runtime settings remain in place. NR hair/face rendering and runtime binaries are
unchanged by installing this controls package.

## Build and local verification

From a full source checkout on Windows x64 with Python 3.13 and Tcl/Tk:

```powershell
python -m venv bin/controls-build-env
bin/controls-build-env/Scripts/python -m pip install --require-hashes -r scripts/controls-build-requirements.txt
bin/controls-build-env/Scripts/python scripts/build-controls.py --output bin/controls-build
python -m unittest discover -s scripts/tests -p test_fg_controls.py -v
```

The build uses pinned PyInstaller dependencies and creates a folder bundle, its ZIP,
source copies, license notices and a build manifest. No installed NR/VR DLL is collected.
`BUILD.json` identifies the exact source files and interpreter used. Python 3.13.3 and
Tcl/Tk 8.6.15 were used for this release; building with another patch version produces
a different dependency inventory. A fresh output directory is required for every build.

The actual EXE also supports a bounded integration check, from an elevated terminal:

```powershell
.\VDXR-NR-FG-Controls.exe --self-test C:\Temp\vdxr-controls-test.json
```

The result path must not exist. Require both process exit 0 and a nonempty JSON result
with `passed: true`. The check creates a unique owned HKCU test key, maps HKLM only
inside that process, exercises the actual Tk controls and Windows registry APIs, then
restores the mapping and removes its test key. It fails closed if isolation fails.
It does not load a graphics backend or change the real runtime settings.

The application is MIT licensed. Included Python, Tcl and Tk notices cover the bundled
interpreter and GUI components. Tcl notice source: the upstream `core-8-6-15` tag at
https://github.com/tcltk/tcl/blob/core-8-6-15/license.terms.
