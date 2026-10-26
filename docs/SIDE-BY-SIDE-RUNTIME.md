# Keep a custom runtime alongside Virtual Desktop Streamer

The bundled VDXR runtime has an upstream development hook that can forward OpenXR
loader negotiation to another runtime DLL. This keeps Streamer's official runtime
and manifest in place while a custom build lives in a separate protected folder.
It does not disable Streamer updates or replace the streaming backend.

## Required build and layout

Use an x64 **ReleaseBundle** build containing the self-redirect guard in
`virtualdesktop-openxr/framework/entry.cpp`. An older ReleaseBundle without this
guard will recursively follow its own redirect. Do not enable the hook for an
unguarded build. Switching to the standalone `Release` configuration also changes
the settings registry key and is not equivalent to this setup.

The installation uses:

```text
C:\Program Files\VDXR-NR-FG\OpenXR\
    virtualdesktop-openxr.dll
    vdxr-frame-generation.dll
    CUSTOM-RUNTIME.json
    nvngx_dlssnr.dll            (separately supplied, licensed NR component)
```

Keep the matching host and FG module together. NR requires the compatible licensed
NR component beside them; it is not distributed by this project. Keep the folder
protected by normal Program Files permissions. Do not redirect to a build directory
that unprivileged programs can modify.

## Install from the release

The [**2026.10.25 release**](https://github.com/jfleezy23/VirtualDesktop-OpenXR/releases/tag/nr-fg-2026.10.25) supplies the guarded runtime and
`CUSTOM-RUNTIME.json` together. Older runtime packages without this metadata are
not the switcher package. Download the dated runtime ZIP and dated Controls ZIP
from that release, and verify them against `SHA256SUMS-2026.10.25.txt` before continuing.
Use `VDXR-NR-FG-Windows-x64-2026.10.25.zip` and
`VDXR-NR-FG-Controls-Windows-x64-2026.10.25.zip` together.

1. Close VR games. Keep Virtual Desktop Streamer's **official** runtime installed.
   If you previously replaced its DLL with a community build, restore the original
   vendor backup or reinstall/update Streamer first. Otherwise the Stock choice
   would still run that replacement. Do not copy the new DLLs over Streamer's files.
2. Extract the runtime ZIP. Its two DLLs and `CUSTOM-RUNTIME.json` are at the ZIP
   root. Using File Explorer, create `%ProgramFiles%\VDXR-NR-FG\OpenXR` and copy
   those **three files** into it, accepting administrator access. Keep the normal
   Program Files permissions. For an existing managed installation, use the upgrade
   procedure below instead of overwriting it while a game is running.
3. If using NR, copy your separately supplied, compatible licensed
   `nvngx_dlssnr.dll` beside them. No NR DLL is included or downloaded. FG with NR
   off does not require it.
4. Open **64-bit PowerShell as Administrator** and run the following block. It
   verifies the copied host against the supplied metadata, then records its
   location for Controls. It does not activate the redirect or change NR/FG
   settings. Stop on any error.

```powershell
$ErrorActionPreference = 'Stop'
$hostPath = Join-Path $env:ProgramFiles 'VDXR-NR-FG\OpenXR\virtualdesktop-openxr.dll'
$manifestPath = Join-Path (Split-Path $hostPath) 'CUSTOM-RUNTIME.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$hostHash = (Get-FileHash -LiteralPath $hostPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($manifest.schema -ne 1 -or $manifest.self_redirect_guard -ne $true -or
    $manifest.host_sha256 -cne $hostHash) {
    throw 'The host and guarded-runtime metadata do not match. Recheck the download.'
}
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
$key = $null
try {
    $key = $base.CreateSubKey('SOFTWARE\VirtualDesktop-OpenXR')
    $existing = $key.GetValue('custom_runtime_path')
    if ($existing -and $existing -ine $hostPath) {
        throw 'Another managed runtime is registered. Preserve it before changing this installation.'
    }
    $key.SetValue('custom_runtime_path', $hostPath, [Microsoft.Win32.RegistryValueKind]::String)
} finally {
    if ($key) { $key.Dispose() }
    $base.Dispose()
}
```

5. Extract the **whole** Controls ZIP and open `VDXR-NR-FG-Controls.exe`, keeping
   `_internal` beside it. Accept the administrator prompt. Choose **Custom VDXR**
   in the Runtime card, keep **VDXR** selected in Streamer, then launch a game
   through its supported VDXR route. For OFXR, turn VD's SSW off. For the first NR
   comparison, enable NR, set processing resolution to **80%**, keep coverage at
   **100%**, and choose **OFXR 3× / NVIDIA**. Fresh NR settings use native 100%
   processing and NR off; installation preserves existing preferences.
6. To compare with the official runtime, choose **Stock VDXR** and relaunch the
   game. Neither Streamer nor Controls needs to restart.

The packaged metadata has schema `1`, `self_redirect_guard: true`, the exact host
`host_sha256`, and `source_revision`. It binds the guarded DLL to the supplied
metadata; it is not a digital signature. Do not manufacture this marker for an
older DLL or copy it from a different build. The Controls app checks the hash
without loading runtime code. The source revision identifies the compiled host;
the Controls app can have a later documentation/application revision.

### Upgrade an existing managed installation

Close all VR games and select **Stock VDXR** in Controls. Back up the existing
external host, FG DLL and `CUSTOM-RUNTIME.json`. Replace only those three files
with the matching new runtime package. Preserve the separately licensed NR DLL
and the folder's permissions. The new spatial NR option keeps processing at native
100% until explicitly changed when no prior processing preference exists. Extract the new Controls app into a fresh folder
and update your shortcut to its EXE. The saved `custom_runtime_path` remains valid
when the installation path stays the same; rerunning the registration block is
safe for that same path. Select Custom and launch a game again.

## Selection

The 64-bit registry value is:

```text
Key:   HKEY_LOCAL_MACHINE\SOFTWARE\VirtualDesktop-OpenXR
Name:  redirect_to
Type:  REG_SZ
Value: C:\Program Files\VDXR-NR-FG\OpenXR\virtualdesktop-openxr.dll
```

The setup above records `custom_runtime_path` at that key and places
`CUSTOM-RUNTIME.json` beside the host DLL. The updated NR/FG Controls app uses
that metadata to verify the guarded installation before enabling Custom VDXR.
It retains this installation information when Stock VDXR removes `redirect_to`.

For normal use, select **Stock VDXR** or **Custom VDXR** in the Controls app's
Runtime card. Changes save immediately. Streamer and the Controls app can stay
open; an already running game keeps its loaded runtime until it exits. The
dropdown in Streamer remains Automatic / SteamVR / VDXR; no fourth menu entry is
registered by this arrangement.

Setting it requires administrator access. Close VR games before changing the
selection. Keep **VDXR** selected in Streamer. Leave the official OpenXR DLLs,
runtime JSON files, Streamer installation `Path`, and Khronos `ActiveRuntime`
registration unchanged. Do not run `Install-Runtime.ps1` for this arrangement.

Normal launches that already reach VDXR enter the official bundled runtime, which
hands off to the external DLL. The external build detects its own negotiation
export and continues locally instead of redirecting again. Its streaming backend
is still loaded from the installed Virtual Desktop Streamer.

ReleaseBundle continues to read NR, FG and controller settings from the existing
`HKEY_LOCAL_MACHINE\SOFTWARE\Virtual Desktop, Inc.\OpenXR` key. The NR/FG Controls
app needs no settings migration. Existing native Oculus or SteamVR routes are not
automatically converted into OpenXR by this hook.

## Updates and verification

Streamer can update its own files without overwriting the external folder. This is
a source-implemented development hook, not a promise that every future Streamer
version will support every older custom runtime. The official runtime forwards
only when the target exports the required entry points and its `getVersion()` is
at least the official runtime's version. A newer official runtime can therefore
decline the redirect and use stock VDXR.

After an update, verify the selected dispatch module and actual game process use
the external DLL. File presence or a saved registry value alone does not establish
that the custom runtime loaded. Check that the FG module and NR component are
also available before judging their output. Preserve NR coverage at 100% and the
existing appearance settings when testing this configuration.

## Return to stock

Close VR games. Remove only the `redirect_to` value from the 64-bit key above, or
restore its prior value if one existed. The next VDXR launch uses the official
runtime. No runtime DLL replacement or NR/FG settings reset is required. Keep the
external files until no process is using them.
