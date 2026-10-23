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

The local installation uses:

```text
C:\Program Files\VDXR-NR-FG\OpenXR\
    virtualdesktop-openxr.dll
    vdxr-frame-generation.dll
    nvngx_dlssnr.dll            (separately supplied, licensed NR component)
```

Keep the matching host and FG module together. NR requires the compatible licensed
NR component beside them; it is not distributed by this project. Keep the folder
protected by normal Program Files permissions. Do not redirect to a build directory
that unprivileged programs can modify.

## Selection

The 64-bit registry value is:

```text
Key:   HKEY_LOCAL_MACHINE\SOFTWARE\VirtualDesktop-OpenXR
Name:  redirect_to
Type:  REG_SZ
Value: C:\Program Files\VDXR-NR-FG\OpenXR\virtualdesktop-openxr.dll
```

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
