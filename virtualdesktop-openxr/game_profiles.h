#pragma once
// CPU-only profile policy shared by the runtime's startup and live NR reads.
// Controls peer: vdxr_game_profiles.py. No global settings are rewritten.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace vdxr_profiles {
    inline std::wstring normalizeExecutable(std::wstring path) {
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (path.size() >= 8 && _wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0)
            path = L"\\\\" + path.substr(8);
        else if (path.compare(0, 4, L"\\\\?\\") == 0)
            path = path.substr(4);
        if (!(path.size() >= 3 && path[1] == L':' && path[2] == L'\\') && path.compare(0, 2, L"\\\\") != 0)
            throw std::runtime_error("Executable identity requires an absolute path");
        const auto size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (!size)
            throw std::runtime_error("Executable identity normalization failed");
        std::wstring full(size, L'\0');
        const auto written = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
        if (!written || written >= size)
            throw std::runtime_error("Executable identity normalization failed");
        full.resize(written);
        const auto length =
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, full.c_str(), -1, nullptr, 0, nullptr, nullptr, 0);
        if (!length)
            throw std::runtime_error("Executable identity case normalization failed");
        std::wstring lower(length, L'\0');
        if (!LCMapStringEx(
                LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, full.c_str(), -1, lower.data(), length, nullptr, nullptr, 0))
            throw std::runtime_error("Executable identity case normalization failed");
        lower.resize(length - 1);
        return lower;
    }

    inline std::wstring profileId(const std::wstring& executable) {
        const auto normalized = normalizeExecutable(executable);
        uint64_t hash = 14695981039346656037ull;
        for (const auto character : normalized) {
            hash = (hash ^ (character & 0xff)) * 1099511628211ull;
            hash = (hash ^ (character >> 8)) * 1099511628211ull;
        }
        const wchar_t digits[] = L"0123456789abcdef";
        std::wstring result(16, L'0');
        for (int i = 15; i >= 0; --i, hash >>= 4)
            result[i] = digits[hash & 15];
        return result;
    }

    inline const std::wstring& currentExecutable() {
        static const auto path = [] {
            std::wstring buffer(32768, L'\0');
            const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!size || size >= buffer.size())
                throw std::runtime_error("Cannot identify executable for profiles");
            buffer.resize(size);
            return normalizeExecutable(buffer);
        }();
        return path;
    }

    struct Rule {
        const char* name;
        const wchar_t* wideName;
        int fallback;
        int minimum;
        int maximum;
    };
    inline constexpr std::array<Rule, 10> Rules{{
        {"DLSSNR_Enabled", L"DLSSNR_Enabled", 0, 0, 1},
        {"DLSSNR_Style", L"DLSSNR_Style", 0, 0, 2},
        {"DLSSNR_Intensity", L"DLSSNR_Intensity", 100, 0, 100},
        {"DLSSNR_LocalToneStrength", L"DLSSNR_LocalToneStrength", 20, 0, 100},
        {"DLSSNR_LocalStructureStrength", L"DLSSNR_LocalStructureStrength", 70, 0, 100},
        {"DLSSNR_SkinStructureStrength", L"DLSSNR_SkinStructureStrength", 50, 0, 100},
        {"FoveationSize", L"FoveationSize", 100, 0, 100},
        {"DLSSNR_ProcessingResolution", L"DLSSNR_ProcessingResolution", 100, 50, 100},
        {"FrameGenerationMode", L"FrameGenerationMode", 0, 0, 4},
        {"FrameGenerationBackend", L"FrameGenerationBackend", 0, 0, 1},
    }};

    inline bool isProfileSetting(const std::string& name) {
        return std::any_of(Rules.begin(), Rules.end(), [&](const auto& rule) { return name == rule.name; });
    }

    struct Settings {
        std::array<int, Rules.size()> values{};
        bool nrValid{true}, fgValid{true};
        std::string invalidName;
        std::optional<int> get(const std::string& name) const {
            for (size_t i = 0; i < Rules.size(); ++i)
                if (name == Rules[i].name)
                    return values[i];
            return {};
        }
    };

    inline bool missing(LSTATUS status) {
        return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    }

    inline Settings readEffective(HKEY root, const std::wstring& base, const std::wstring& executable) {
        Settings result;
        for (size_t i = 0; i < Rules.size(); ++i)
            result.values[i] = Rules[i].fallback;
        const auto profilePath = base + L"\\Profiles\\" + profileId(executable);
        HKEY profile{};
        const auto opened = RegOpenKeyExW(root, profilePath.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &profile);
        bool identityValid = missing(opened);
        if (opened == ERROR_SUCCESS) {
            std::wstring stored(32768, L'\0');
            DWORD bytes = static_cast<DWORD>(stored.size() * sizeof(wchar_t)), type{};
            const auto status = RegQueryValueExW(
                profile, L"ExecutablePath", nullptr, &type, reinterpret_cast<BYTE*>(stored.data()), &bytes);
            if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t) && bytes % sizeof(wchar_t) == 0 &&
                stored[bytes / sizeof(wchar_t) - 1] == L'\0') {
                stored.resize(bytes / sizeof(wchar_t) - 1);
                identityValid = stored == normalizeExecutable(executable);
            }
        }
        if (!identityValid) {
            result.nrValid = result.fgValid = false;
            result.invalidName = "ExecutablePath";
        } else {
            for (size_t i = 0; i < Rules.size(); ++i) {
                const auto& rule = Rules[i];
                DWORD value{}, bytes = sizeof(value), type{};
                auto status = profile
                                  ? RegQueryValueExW(
                                        profile, rule.wideName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes)
                                  : ERROR_FILE_NOT_FOUND;
                if (missing(status)) {
                    bytes = sizeof(value);
                    status = RegGetValueW(
                        root, base.c_str(), rule.wideName, RRF_SUBKEY_WOW6464KEY | RRF_RT_ANY, &type, &value, &bytes);
                }
                if (missing(status))
                    continue;
                const bool valid = status == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(value) &&
                                   value >= static_cast<DWORD>(rule.minimum) &&
                                   value <= static_cast<DWORD>(rule.maximum) && (i != 8 || value != 1);
                if (valid)
                    result.values[i] = static_cast<int>(value);
                else {
                    if (i < 8)
                        result.nrValid = false;
                    else
                        result.fgValid = false;
                    if (result.invalidName.empty())
                        result.invalidName = rule.name;
                }
            }
        }
        if (profile)
            RegCloseKey(profile);
        if (!result.nrValid)
            result.values[0] = 0;
        if (!result.fgValid)
            result.values[8] = 0;
        return result;
    }

    inline Settings readCurrent(const std::wstring& base) {
        return readEffective(HKEY_LOCAL_MACHINE, base, currentExecutable());
    }
} // namespace vdxr_profiles
