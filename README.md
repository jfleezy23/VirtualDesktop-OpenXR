# VirtualDesktop-OpenXR community fork

This is [jfleezy23's community fork](https://github.com/jfleezy23/VirtualDesktop-OpenXR) of
[Matthieu Bucchianeri's VirtualDesktop-OpenXR](https://github.com/mbucchia/VirtualDesktop-OpenXR).
The upstream README, contributor attribution, license, and third-party notices are retained below.

`stable` is the general runtime branch, beginning at the upstream `main` baseline. Independently reviewed general
runtime fixes will be ported there with regression evidence. `experimental-nr` preserves the upstream NR development
line and the fork's NR and general runtime fixes; use it for experimental NR work. The experimental branch's core build
target is x64: inherited Win32 NR code references NGX functions while the retained NGX libraries are x64 only.
`main` retains the upstream main baseline.

These branch names describe development scope, not a guarantee of headset compatibility or release readiness.
The fork makes no Khronos conformance claim or measured image-quality, streaming-latency, or performance claim.
See [CONTRIBUTING.md](CONTRIBUTING.md) for source checks, supported build targets, and the distinction between CPU
regressions and local hardware tests. The separate community workflow builds core projects without installer signing
or distributing NR vendor DLLs.

# Upstream README: an implementation of the OpenXR 1.0 and 1.1 standard for Virtual Desktop

This program is an implementation of the OpenXR 1.0 and 1.1 standard for Virtual Desktop on Windows. It allows you to run OpenXR applications without SteamVR.

DISCLAIMER: This runtime is not officially conformant per Khronos standards: it cannot be called "conformant" nor use the OpenXR trademark and logo. However, VDXR implements all features in the core OpenXR 1.0 and 1.1 specification and passes the majority of OpenXR conformance tests and the reason for not seeking official conformance is the required adopter fee (several thousand dollars).

DISCLAIMER: This software is distributed as-is, without any warranties or conditions of any kind. Use at your own risks.

# Details and instructions on the Wiki: https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki

## Contributors

- Matthieu Bucchianeri
- Guy Godin (Virtual Desktop)
- Kyle Hendry (Virtual Desktop)

Additional contributions:

- Vladimir Dranyonkov (bug fixes, on original PimaxXR)
- Sarah Heinzen  (Accessibility project, sponsored by Microsoft)
- Jonas Holderman (Accessibility project, sponsored by Microsoft)
- Heather Kemp (Accessibility project, sponsored by Microsoft)
- Mathias Peter Nordskog (bug fixes)
- Pavel Skakov (bug fixes, on original PimaxXR)

## Donate

Donations are welcome and totally optional. Please use [my GitHub sponsorship page](https://github.com/sponsors/mbucchia) to make one-time or recurring donations!

Thank you!
