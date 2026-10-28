// CPU-only real Win32 registry test; all writes stay in a unique HKCU fixture.
#include <windows.h>
#include <iostream>
#if __has_include("../virtualdesktop-openxr/game_profiles.h")
#include "../virtualdesktop-openxr/game_profiles.h"
#include <stdexcept>

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    using namespace vdxr_profiles;
    const auto base = L"Software\\VDXRProfileNativeTests\\" + std::to_wstring(GetCurrentProcessId());
    const auto exe = L"C:\\ProfileFixtures\\One\\fixture.exe";
    try {
        require(profileId(exe) == profileId(L"c:/profilefixtures/one/sub/../FIXTURE.EXE"), "identity normalization");
        require(profileId(exe) == L"40d1fe279f0e8a92", "Controls/runtime UTF16 hash contract");
        require(profileId(L"\\\\?\\unc\\SERVER\\SHARE\\FIXTURE.EXE") == profileId(L"\\\\server\\share\\fixture.exe"),
                "extended UNC case normalization");
        require(profileId(exe) != profileId(L"C:\\ProfileFixtures\\Two\\fixture.exe"), "basename collision");
        HKEY global{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER,
                                base.c_str(),
                                0,
                                nullptr,
                                0,
                                KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                                nullptr,
                                &global,
                                nullptr) == ERROR_SUCCESS,
                "fixture create");
        DWORD enabled = 1, mode = 3, intensity = 83;
        RegSetValueExW(global, L"DLSSNR_Enabled", 0, REG_DWORD, reinterpret_cast<BYTE*>(&enabled), sizeof(enabled));
        RegSetValueExW(global, L"FrameGenerationMode", 0, REG_DWORD, reinterpret_cast<BYTE*>(&mode), sizeof(mode));
        RegSetValueExW(
            global, L"DLSSNR_Intensity", 0, REG_DWORD, reinterpret_cast<BYTE*>(&intensity), sizeof(intensity));
        RegCloseKey(global);
        auto values = readEffective(HKEY_CURRENT_USER, base, exe);
        require(values.nrValid && values.fgValid && values.get("DLSSNR_Intensity") == 83, "global inheritance");
        require(values.get("FoveationSize") == 100, "missing global/profile coverage must use full coverage");
        require(RegOpenKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &global) ==
                    ERROR_SUCCESS,
                "global coverage fixture");
        DWORD coverage = 90;
        RegSetValueExW(global, L"FoveationSize", 0, REG_DWORD, reinterpret_cast<BYTE*>(&coverage), sizeof(coverage));
        RegCloseKey(global);
        const auto path = base + L"\\Profiles\\" + profileId(exe);
        HKEY profile{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER,
                                path.c_str(),
                                0,
                                nullptr,
                                0,
                                KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                                nullptr,
                                &profile,
                                nullptr) == ERROR_SUCCESS,
                "profile create");
        auto identity = normalizeExecutable(exe);
        RegSetValueExW(profile,
                       L"ExecutablePath",
                       0,
                       REG_SZ,
                       reinterpret_cast<const BYTE*>(identity.c_str()),
                       static_cast<DWORD>((identity.size() + 1) * sizeof(wchar_t)));
        mode = 4;
        RegSetValueExW(profile, L"FrameGenerationMode", 0, REG_DWORD, reinterpret_cast<BYTE*>(&mode), sizeof(mode));
        values = readEffective(HKEY_CURRENT_USER, base, exe);
        require(values.get("FrameGenerationMode") == 4 && values.get("DLSSNR_Intensity") == 83 &&
                    values.get("FoveationSize") == 90,
                "profile mode and individual inheritance");
        const wchar_t bad[] = L"100";
        RegSetValueExW(profile, L"DLSSNR_Intensity", 0, REG_SZ, reinterpret_cast<const BYTE*>(bad), sizeof(bad));
        values = readEffective(HKEY_CURRENT_USER, base, exe);
        require(!values.nrValid && values.get("DLSSNR_Enabled") == 0 && values.get("FrameGenerationMode") == 4,
                "bad NR value must reject NR without changing FG");
        RegSetValueExW(profile, L"FrameGenerationBackend", 0, REG_SZ, reinterpret_cast<const BYTE*>(bad), sizeof(bad));
        values = readEffective(HKEY_CURRENT_USER, base, exe);
        require(!values.fgValid && values.get("FrameGenerationMode") == 0, "bad backend disables FG");
        RegDeleteValueW(profile, L"DLSSNR_Intensity");
        RegDeleteValueW(profile, L"FrameGenerationBackend");
        const wchar_t other[] = L"c:\\other\\fixture.exe";
        RegSetValueExW(profile, L"ExecutablePath", 0, REG_SZ, reinterpret_cast<const BYTE*>(other), sizeof(other));
        values = readEffective(HKEY_CURRENT_USER, base, exe);
        require(!values.nrValid && !values.fgValid && values.get("DLSSNR_Enabled") == 0 &&
                    values.get("FrameGenerationMode") == 0,
                "metadata collision rejection");
        RegCloseKey(profile);
        RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
        std::cout << "game profile native policy PASS\n";
        return 0;
    } catch (const std::exception& error) {
        RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#else
int main() {
    std::cerr << "FAIL: executable profile policy is missing\n";
    return 1;
}
#endif
