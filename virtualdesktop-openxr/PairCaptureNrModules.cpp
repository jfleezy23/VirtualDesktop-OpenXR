#include "PairCaptureNrModules.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        uint64_t clock() noexcept {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            return static_cast<uint64_t>(q.QuadPart);
        }
        bool range(uint64_t first, uint64_t bytes, uint64_t total) noexcept {
            return first <= total && bytes <= total - first;
        }
        bool fileIdentity(HANDLE file, ModuleFileLease& out) noexcept {
            FILE_ID_INFO id{};
            if (!GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof id))
                return false;
            out.volume = id.VolumeSerialNumber;
            std::memcpy(out.fileId, id.FileId.Identifier, 16);
            return true;
        }
        bool fileDigest(const wchar_t* path, uint8_t digest[32], uint64_t& bytes, ModuleFileLease* lease) noexcept {
            bytes = 0;
            const std::unique_ptr<std::array<unsigned char, 65536>> chunk(new (std::nothrow)
                                                                              std::array<unsigned char, 65536>);
            if (!chunk)
                return false;
            HANDLE file = CreateFileW(path,
                                      GENERIC_READ,
                                      lease ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_SEQUENTIAL_SCAN,
                                      nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE hash{};
            bool ok = false;
            std::array<unsigned char, 1024> object{};
            LARGE_INTEGER size{};
            ULONG objectBytes{}, returned{};
            do {
                if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0)
                    break;
                bytes = static_cast<uint64_t>(size.QuadPart);
                if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                    break;
                if (BCryptGetProperty(algorithm,
                                      BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&objectBytes),
                                      sizeof objectBytes,
                                      &returned,
                                      0) < 0 ||
                    returned != sizeof objectBytes || objectBytes > object.size())
                    break;
                if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0)
                    break;
                uint64_t consumed = 0;
                bool readOk = true;
                while (consumed < bytes) {
                    DWORD got{};
                    const auto remaining = bytes - consumed;
                    const auto wanted = static_cast<DWORD>(remaining < chunk->size() ? remaining : chunk->size());
                    if (!ReadFile(file, chunk->data(), wanted, &got, nullptr) || !got || got > wanted ||
                        BCryptHashData(hash, chunk->data(), got, 0) < 0) {
                        readOk = false;
                        break;
                    }
                    consumed += got;
                }
                if (!readOk || consumed != bytes)
                    break;
                LARGE_INTEGER after{};
                if (!GetFileSizeEx(file, &after) || after.QuadPart != size.QuadPart)
                    break;
                if (BCryptFinishHash(hash, digest, 32, 0) < 0 || (lease && !fileIdentity(file, *lease)))
                    break;
                ok = true;
            } while (false);
            if (hash)
                BCryptDestroyHash(hash);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
            if (ok && lease)
                lease->handle = file;
            else
                CloseHandle(file);
            return ok;
        }
        bool imageFacts(HMODULE module, NrModuleV1& out) noexcept {
            const auto* image = reinterpret_cast<const uint8_t*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
                dos->e_lfanew > 1048576)
                return false;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || !nt->OptionalHeader.SizeOfImage)
                return false;
            out.baseObserved = reinterpret_cast<uint64_t>(module);
            out.imageSize = nt->OptionalHeader.SizeOfImage;
            if (!range(static_cast<uint64_t>(dos->e_lfanew), sizeof(*nt), out.imageSize))
                return false;
            if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
                return true;
            const auto& debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            if (!debug.Size)
                return true;
            if (!range(debug.VirtualAddress, debug.Size, out.imageSize) || debug.Size % sizeof(IMAGE_DEBUG_DIRECTORY))
                return false;
            const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(image + debug.VirtualAddress);
            for (size_t i = 0; i < debug.Size / sizeof(*entries); ++i) {
                const auto& d = entries[i];
                if (d.Type != IMAGE_DEBUG_TYPE_CODEVIEW)
                    continue;
                if (!range(d.AddressOfRawData, d.SizeOfData, out.imageSize) || d.SizeOfData < 24)
                    return false;
                const auto* cv = image + d.AddressOfRawData;
                uint32_t signature{}, age{};
                std::memcpy(&signature, cv, 4);
                if (signature != 0x53445352u)
                    continue;
                std::memcpy(out.codeViewGuidWords, cv + 4, 16);
                std::memcpy(&age, cv + 20, 4);
                out.codeViewAge = age;
                break;
            }
            return true;
        }
    } // namespace
    bool observeModule(const void* address,
                       uint64_t kind,
                       const uint8_t expected[32],
                       bool pin,
                       NrModuleV1& observed,
                       NrCallV1& pinCall,
                       uint64_t lifetime,
                       ModuleFileLease* lease) noexcept {
        observed = {};
        pinCall = {};
        if (!address || !expected || kind < 1 || kind > 8 || !lifetime || lifetime == UINT64_MAX)
            return false;
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(address),
                                &module))
            return false;
        NrModuleV1 actual{};
        if (!imageFacts(module, actual))
            return false;
        const auto word = reinterpret_cast<uint64_t>(address);
        if (word < actual.baseObserved || word - actual.baseObserved >= actual.imageSize)
            return false;
        actual.addressRva = word - actual.baseObserved;
        const std::unique_ptr<std::array<wchar_t, 32768>> path(new (std::nothrow) std::array<wchar_t, 32768>{});
        if (!path)
            return false;
        const auto chars = GetModuleFileNameW(module, path->data(), static_cast<DWORD>(path->size()));
        if (!chars || chars >= path->size())
            return false;
        uint8_t digest[32]{};
        if (!fileDigest(path->data(), digest, actual.fileSize, lease) || std::memcmp(digest, expected, 32))
            return false;
        actual.moduleKind = kind;
        actual.loadLifetimeEpoch = lifetime;
        actual.observedMask = 7;
        std::memcpy(actual.sha256Words, digest, 32);
        if (pin) {
            pinCall.kind = NR_PIN;
            pinCall.beginObserved = 1;
            pinCall.resultType = NR_BOOLEAN;
            pinCall.qpcBegin = clock();
            pinCall.thread = GetCurrentThreadId();
            pinCall.eye = 2;
            HMODULE pinned{};
            const BOOL result =
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(address),
                                   &pinned);
            pinCall.qpcEnd = clock();
            pinCall.endObserved = 1;
            pinCall.resultBits = result ? 1 : 0;
            pinCall.classifiedSuccess = result ? 1 : 0;
            pinCall.observedMask = NR_CALL_OBS;
            pinCall.dispatchAddressObserved = word;
            actual.pinObserved = 1;
            actual.pinResult = result ? 1 : 0;
            observed = actual;
            if (!result || pinned != module)
                return false;
        }
        observed = actual;
        return true;
    }
    ModuleFileLease::~ModuleFileLease() noexcept {
        reset();
    }
    void ModuleFileLease::reset() noexcept {
        if (handle)
            CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
        volume = fileId[0] = fileId[1] = 0;
    }
    void ModuleFileLease::take(ModuleFileLease& from) noexcept {
        if (this == &from)
            return;
        reset();
        handle = from.handle;
        volume = from.volume;
        fileId[0] = from.fileId[0];
        fileId[1] = from.fileId[1];
        from.handle = nullptr;
        from.volume = from.fileId[0] = from.fileId[1] = 0;
    }
    bool observePrivateModule(const void* address,
                              uint64_t kind,
                              const uint8_t expected[32],
                              bool pin,
                              NrModuleV1& observed,
                              NrCallV1& pinCall,
                              uint64_t lifetime) noexcept {
        return observeModule(address, kind, expected, pin, observed, pinCall, lifetime, nullptr);
    }
    bool observeLeasedPrivateModule(const void* address,
                                    uint64_t kind,
                                    const uint8_t expected[32],
                                    bool pin,
                                    NrModuleV1& observed,
                                    NrCallV1& pinCall,
                                    uint64_t lifetime,
                                    ModuleFileLease& lease) noexcept {
        lease.reset();
        if (observeModule(address, kind, expected, pin, observed, pinCall, lifetime, &lease))
            return true;
        lease.reset();
        return false;
    }
    bool validateLeasedPrivateModule(const void* address,
                                     uint64_t kind,
                                     const uint8_t expected[32],
                                     uint64_t lifetime,
                                     const NrModuleV1& cached,
                                     const ModuleFileLease& lease) noexcept {
        if (!address || !expected || !lease.handle || kind < 1 || kind > 8 || !lifetime || lifetime == UINT64_MAX ||
            cached.moduleKind != kind || cached.loadLifetimeEpoch != lifetime || cached.observedMask != 7 ||
            std::memcmp(cached.sha256Words, expected, 32))
            return false;
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(address),
                                &module))
            return false;
        NrModuleV1 actual{};
        if (!imageFacts(module, actual) || actual.baseObserved != cached.baseObserved ||
            actual.imageSize != cached.imageSize ||
            std::memcmp(actual.codeViewGuidWords, cached.codeViewGuidWords, 16) ||
            actual.codeViewAge != cached.codeViewAge)
            return false;
        const auto word = reinterpret_cast<uint64_t>(address);
        if (word < actual.baseObserved || word - actual.baseObserved >= actual.imageSize ||
            word - actual.baseObserved != cached.addressRva)
            return false;
        const std::unique_ptr<std::array<wchar_t, 32768>> path(new (std::nothrow) std::array<wchar_t, 32768>{});
        if (!path)
            return false;
        const auto chars = GetModuleFileNameW(module, path->data(), static_cast<DWORD>(path->size()));
        if (!chars || chars >= path->size())
            return false;
        HANDLE file = CreateFileW(
            path->data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        ModuleFileLease current;
        LARGE_INTEGER size{};
        const bool same = fileIdentity(file, current) && GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
                          static_cast<uint64_t>(size.QuadPart) == cached.fileSize && current.volume == lease.volume &&
                          current.fileId[0] == lease.fileId[0] && current.fileId[1] == lease.fileId[1];
        CloseHandle(file);
        return same;
    }
} // namespace virtualdesktop_openxr::nr_capture
