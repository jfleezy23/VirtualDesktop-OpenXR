// Exercise the production watcher callback in a process-private registry sandbox.
#include "pch.h"
#include "runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
    class PrivateSettingsRegistry {
      public:
        PrivateSettingsRegistry() {
            try {
                GUID id{};
                if (FAILED(CoCreateGuid(&id)))
                    throw std::runtime_error("Could not generate private registry identity");
                wchar_t identity[39]{};
                if (!StringFromGUID2(id, identity, static_cast<int>(std::size(identity))))
                    throw std::runtime_error("Could not format private registry identity");
                m_path = std::wstring(L"Software\\VDXRWatcherRegression-") + identity;
                m_probe = std::wstring(L"VDXRWatcherProbe-") + identity;
                require(probeAbsent(), "Default HKLM probe must be absent");
                DWORD disposition = 0;
                require(RegCreateKeyExW(HKEY_CURRENT_USER,
                                        m_path.c_str(),
                                        0,
                                        nullptr,
                                        REG_OPTION_VOLATILE,
                                        KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                                        nullptr,
                                        &m_root,
                                        &disposition),
                        "Create private HKCU root");
                m_owned = disposition == REG_CREATED_NEW_KEY;
                if (!m_owned)
                    throw std::runtime_error("Refusing to use a preexisting private registry root");
                createKey(m_probe.c_str());
                createKey(xr::utf8_to_wide(virtualdesktop_openxr::RegPrefix).c_str());
                require(RegOverridePredefKey(HKEY_LOCAL_MACHINE, m_root), "Override process HKLM");
                m_overridden = true;
                HKEY probe = nullptr;
                require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, m_probe.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &probe),
                        "Mapped HKLM probe must be present");
                require(RegCloseKey(probe), "Close mapped HKLM probe");
                std::cout << "PASS: process HKLM reaches its unique private registry probe\n";
            } catch (...) {
                cleanupOrExit();
                throw;
            }
        }
        PrivateSettingsRegistry(const PrivateSettingsRegistry&) = delete;
        PrivateSettingsRegistry& operator=(const PrivateSettingsRegistry&) = delete;
        ~PrivateSettingsRegistry() {
            cleanupOrExit();
        }
        void finish() {
            require(cleanup(), "Restore registry mapping and remove owned root");
            std::cout << "PASS: default HKLM mapping restored and owned HKCU root removed\n";
        }

      private:
        static void require(LSTATUS status, const char* operation) {
            if (status != ERROR_SUCCESS)
                throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(status));
        }
        void createKey(const wchar_t* path) {
            HKEY key = nullptr;
            DWORD disposition = 0;
            require(RegCreateKeyExW(m_root,
                                    path,
                                    0,
                                    nullptr,
                                    REG_OPTION_VOLATILE,
                                    KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                                    nullptr,
                                    &key,
                                    &disposition),
                    "Create private registry subkey");
            require(RegCloseKey(key), "Close private registry subkey");
            if (disposition != REG_CREATED_NEW_KEY)
                throw std::runtime_error("Private registry subkey unexpectedly existed");
        }
        LSTATUS probeAbsent() const noexcept {
            HKEY probe = nullptr;
            const auto status =
                RegOpenKeyExW(HKEY_LOCAL_MACHINE, m_probe.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &probe);
            if (status == ERROR_FILE_NOT_FOUND)
                return ERROR_SUCCESS;
            if (status != ERROR_SUCCESS)
                return status;
            const auto closeStatus = RegCloseKey(probe);
            return closeStatus == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : closeStatus;
        }
        LSTATUS cleanup() noexcept {
            if (m_overridden) {
                const auto status = RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
                if (status != ERROR_SUCCESS)
                    return status;
                m_overridden = false;
                const auto probeStatus = probeAbsent();
                if (probeStatus != ERROR_SUCCESS)
                    return probeStatus;
            }
            if (m_root) {
                // Only a root created by this instance may have its contents removed.
                if (m_owned) {
                    const auto status = RegDeleteTreeW(m_root, nullptr);
                    if (status != ERROR_SUCCESS)
                        return status;
                }
                const auto status = RegCloseKey(m_root);
                if (status != ERROR_SUCCESS)
                    return status;
                m_root = nullptr;
            }
            if (m_owned) {
                const auto status = RegDeleteKeyExW(HKEY_CURRENT_USER, m_path.c_str(), KEY_WOW64_64KEY, 0);
                if (status != ERROR_SUCCESS)
                    return status;
                m_owned = false;
                HKEY leftover = nullptr;
                const auto openStatus =
                    RegOpenKeyExW(HKEY_CURRENT_USER, m_path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &leftover);
                if (openStatus == ERROR_FILE_NOT_FOUND)
                    return ERROR_SUCCESS;
                if (openStatus != ERROR_SUCCESS)
                    return openStatus;
                const auto closeStatus = RegCloseKey(leftover);
                return closeStatus == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : closeStatus;
            }
            return ERROR_SUCCESS;
        }
        void cleanupOrExit() noexcept {
            const auto status = cleanup();
            if (status != ERROR_SUCCESS) {
                std::fprintf(stderr, "FAIL: private registry cleanup failed: %ld\n", status);
                std::_Exit(2);
            }
        }
        HKEY m_root = nullptr;
        std::wstring m_path;
        std::wstring m_probe;
        bool m_owned = false;
        bool m_overridden = false;
    };

    std::atomic<DWORD> allocationFaultThread{0};
    std::atomic<bool> failNextAllocation{false};
    std::atomic<bool> sawAllocationFault{false};
    std::atomic<bool> armedBeforeAllocationFault{false};
} // namespace

void* operator new(std::size_t size) {
    if (allocationFaultThread.load() == GetCurrentThreadId() && failNextAllocation.exchange(false)) {
        sawAllocationFault.store(true);
        throw std::bad_alloc();
    }
    if (auto* memory = std::malloc(std::max(size, std::size_t{1})))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
    decltype(&RegGetValueW) originalRegGetValue = RegGetValueW;
    std::atomic<bool> blockRegistryRead{false};
    wil::unique_handle readEntered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    wil::unique_handle allowRead{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    decltype(&CreateThreadpoolWait) originalCreateWait = CreateThreadpoolWait;
    decltype(&SetThreadpoolWait) originalSetWait = SetThreadpoolWait;
    PTP_WAIT WINAPI observeCreateWait(PTP_WAIT_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON environment) {
        const auto wait = originalCreateWait(callback, context, environment);
        if (wait && allocationFaultThread.load() == GetCurrentThreadId())
            failNextAllocation.store(true);
        return wait;
    }
    void WINAPI observeSetWait(PTP_WAIT wait, HANDLE event, PFILETIME timeout) {
        if (event && allocationFaultThread.load() == GetCurrentThreadId() && !sawAllocationFault.load())
            armedBeforeAllocationFault.store(true);
        originalSetWait(wait, event, timeout);
    }
    LSTATUS WINAPI
    observeRegGetValue(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
        if (blockRegistryRead.load()) {
            SetEvent(readEntered.get());
            WaitForSingleObject(allowRead.get(), INFINITE);
        }
        return originalRegGetValue(key, subkey, value, flags, type, data, size);
    }
    void checkDetour(LONG result) {
        if (result != NO_ERROR)
            throw std::runtime_error("Detours setup failed");
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeInputRegression {
        static int constructorUnwind() {
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalCreateWait), observeCreateWait));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalSetWait), observeSetWait));
            checkDetour(DetourTransactionCommit());
            allocationFaultThread.store(GetCurrentThreadId());
            bool threw = false;
            try {
                OpenXrRuntime runtime;
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            allocationFaultThread.store(0);
            failNextAllocation.store(false);
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourDetach(reinterpret_cast<PVOID*>(&originalCreateWait), observeCreateWait));
            checkDetour(DetourDetach(reinterpret_cast<PVOID*>(&originalSetWait), observeSetWait));
            checkDetour(DetourTransactionCommit());
            if (!threw || !sawAllocationFault.load())
                throw std::runtime_error("Constructor allocation fault was not exercised");
            if (armedBeforeAllocationFault.load()) {
                std::cerr << "FAIL: native settings wait armed before fallible constructor initialization completed\n";
                return 1;
            }
            std::cout << "PASS: constructor allocation failure leaves native settings wait unarmed\n";
            return 0;
        }
        static int run() {
            OpenXrRuntime runtime;
            int failures = 0;
#ifdef VDXR_WATCHER_BASELINE
            using Storage = wil::details::unique_storage<wil::details::registry_watcher_state_resource_policy>;
            auto* state = static_cast<Storage&>(runtime.m_registryWatcher).get();
            if (!state || !state->TryAddRef())
                throw std::runtime_error("Existing settings registry key/watcher required");
            const auto event = state->m_eventHandle.get();
            const auto wait = state->m_threadPoolWait.get();
            auto lateCallback = state->m_callback;
#else
            const auto state = runtime.m_settingsWatcherState;
            if (!state || !runtime.m_registryWatcher)
                throw std::runtime_error("Existing settings registry key/watcher required");
            const auto event = state->event.get();
#endif
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourAttach(reinterpret_cast<PVOID*>(&originalRegGetValue), observeRegGetValue));
            checkDetour(DetourTransactionCommit());
            blockRegistryRead.store(true);
            SetEvent(event);
            if (WaitForSingleObject(readEntered.get(), 5000) != WAIT_OBJECT_0) {
                blockRegistryRead.store(false);
                SetEvent(allowRead.get());
                throw std::runtime_error("Production watcher callback did not enter settings refresh");
            }
            wil::unique_handle stopped{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            std::thread stop([&] {
#ifdef VDXR_WATCHER_BASELINE
                // This is the original owner's implicit WIL watcher cleanup operation.
                runtime.m_registryWatcher.reset();
#else
                runtime.stopRegistryWatcher();
#endif
                SetEvent(stopped.get());
            });
            if (WaitForSingleObject(stopped.get(), 150) != WAIT_TIMEOUT) {
                ++failures;
                std::cerr << "FAIL: owner stop returned while its production settings callback was blocked\n";
            }
            blockRegistryRead.store(false);
            SetEvent(allowRead.get());
            stop.join();
#ifdef VDXR_WATCHER_BASELINE
            // Our extra WIL reference keeps its native wait alive for safe test cleanup.
            SetThreadpoolWait(wait, nullptr, nullptr);
            WaitForThreadpoolWaitCallbacks(wait, TRUE);
            state->Release();
#endif
            checkDetour(DetourTransactionBegin());
            checkDetour(DetourUpdateThread(GetCurrentThread()));
            checkDetour(DetourDetach(reinterpret_cast<PVOID*>(&originalRegGetValue), observeRegGetValue));
            checkDetour(DetourTransactionCommit());
            runtime.m_controllerLingerTimeout = -123;
#ifdef VDXR_WATCHER_BASELINE
            lateCallback(wil::RegistryChangeKind::Modify);
#else
            OpenXrRuntime::SettingsWatcherState::callback(nullptr, state.get(), nullptr, WAIT_OBJECT_0);
#endif
            if (runtime.m_controllerLingerTimeout != -123) {
                ++failures;
                std::cerr << "FAIL: callback after owner detach still touched runtime settings\n";
            }
            if (failures)
                return 1;
            std::cout << "PASS: settings watcher stop drains callbacks and late callbacks cannot access owner\n";
            return 0;
        }
    };
} // namespace virtualdesktop_openxr

int main(int argc, char** argv) {
    try {
        PrivateSettingsRegistry registry;
        int result = 0;
        if (argc == 2 && std::string_view(argv[1]) == "--constructor-only") {
            result = virtualdesktop_openxr::RuntimeInputRegression::constructorUnwind();
        } else {
            const auto lifetimeResult = virtualdesktop_openxr::RuntimeInputRegression::run();
            const auto constructorResult = virtualdesktop_openxr::RuntimeInputRegression::constructorUnwind();
            result = lifetimeResult || constructorResult ? 1 : 0;
        }
        registry.finish();
        return result;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
