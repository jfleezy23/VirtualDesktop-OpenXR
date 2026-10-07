// Exercise the installed production watcher callback without changing registry values.
#include "pch.h"
#include "runtime.h"
#include <atomic>
#include <cstdlib>

namespace {
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
        if (argc == 2 && std::string_view(argv[1]) == "--constructor-only")
            return virtualdesktop_openxr::RuntimeInputRegression::constructorUnwind();
        const auto lifetimeResult = virtualdesktop_openxr::RuntimeInputRegression::run();
        const auto constructorResult = virtualdesktop_openxr::RuntimeInputRegression::constructorUnwind();
        return lifetimeResult || constructorResult ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
