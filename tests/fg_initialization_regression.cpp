#include "fg_gpu_helpers.h"
#include <detours.h>
#include <nvOpticalFlowD3D12.h>

namespace {
    using RegisterFn = decltype(NV_OF_D3D12_API_FUNCTION_LIST::nvOFRegisterResourceD3D12);
    using UnregisterFn = decltype(NV_OF_D3D12_API_FUNCTION_LIST::nvOFUnregisterResourceD3D12);
    using EventFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
    RegisterFn originalRegister{};
    UnregisterFn originalUnregister{};
    EventFn originalEvent{};
    ComPtr<ID3D12Fence> gate, registration;
    UINT64 registrationValue{};
    bool armed = true;
    uint32_t unsafeUnregister{};
    using SignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    SignalFn originalSignal{};
    ID3D12CommandQueue* faultQueue{};
    uint32_t signalCount{}, failedSignal{};

    HRESULT STDMETHODCALLTYPE signalFailure(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
        return queue == faultQueue && ++signalCount == failedSignal ? E_FAIL : originalSignal(queue, fence, value);
    }

    NV_OF_STATUS NVOFAPI registerResource(NvOFHandle handle, NV_OF_REGISTER_RESOURCE_PARAMS_D3D12* params) {
        if (!armed || registration) {
            return originalRegister(handle, params);
        }
        auto blocked = *params;
        blocked.inputFencePoint.fence = gate.Get();
        blocked.inputFencePoint.value = 1;
        registration = params->outputFencePoint.fence;
        registrationValue = params->outputFencePoint.value;
        return originalRegister(handle, &blocked);
    }

    HRESULT STDMETHODCALLTYPE eventFailure(ID3D12Fence* fence, UINT64 value, HANDLE event) {
        return armed && fence == registration.Get() ? E_FAIL : originalEvent(fence, value, event);
    }

    void releaseRegistration() {
        armed = false;
        success(gate->Signal(1), "release registration gate");
        if (registration && registration->GetCompletedValue() < registrationValue) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(event != nullptr, "registration event");
            const auto hr = originalEvent(registration.Get(), registrationValue, event);
            const auto status = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
            CloseHandle(event);
            success(hr, "registration drain");
            check(status == WAIT_OBJECT_0, "bounded registration drain");
        }
    }

    NV_OF_STATUS NVOFAPI unregisterResource(NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12* params) {
        if (armed && registration && registration->GetCompletedValue() < registrationValue) {
            ++unsafeUnregister;
            // Red control records the unsafe attempt but releases the gate before
            // allowing driver teardown. The fixture must never create an actual UAF.
            releaseRegistration();
        }
        return originalUnregister(params);
    }

    void attach(bool enable) {
        check(DetourTransactionBegin() == NO_ERROR, "detour transaction");
        check(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "detour thread");
        const auto operation = enable ? DetourAttach : DetourDetach;
        check(operation(reinterpret_cast<PVOID*>(&originalRegister), registerResource) == NO_ERROR,
              "registration detour");
        check(operation(reinterpret_cast<PVOID*>(&originalUnregister), unregisterResource) == NO_ERROR,
              "unregister detour");
        check(operation(reinterpret_cast<PVOID*>(&originalEvent), eventFailure) == NO_ERROR, "event detour");
        check(DetourTransactionCommit() == NO_ERROR, "detour commit");
    }

    void attachSignal(bool enable) {
        check(DetourTransactionBegin() == NO_ERROR, "signal detour transaction");
        check(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "signal detour thread");
        const auto operation = enable ? DetourAttach : DetourDetach;
        check(operation(reinterpret_cast<PVOID*>(&originalSignal), signalFailure) == NO_ERROR, "signal detour");
        check(DetourTransactionCommit() == NO_ERROR, "signal detour commit");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2 || argc == 3, "absolute module path and optional signal-failure index required");
        Gpu gpu;
        success(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "initialization gate");
        HMODULE driver = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(driver != nullptr, "NVIDIA driver API");
        using GetDriverApi = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);
        auto getDriver = reinterpret_cast<GetDriverApi>(GetProcAddress(driver, "NvOFAPICreateInstanceD3D12"));
        check(getDriver != nullptr, "driver API export");
        NV_OF_D3D12_API_FUNCTION_LIST driverApi{};
        check(getDriver(NV_OF_API_VERSION, &driverApi) == NV_OF_SUCCESS, "driver API table");
        originalRegister = driverApi.nvOFRegisterResourceD3D12;
        originalUnregister = driverApi.nvOFUnregisterResourceD3D12;
        originalEvent = reinterpret_cast<EventFn>((*reinterpret_cast<void***>(gate.Get()))[9]);
        HMODULE module =
            LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "FG module");
        auto getApi = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
        check(getApi != nullptr, "FG API export");
        VDXRFG_ApiV1 api{};
        api.struct_size = sizeof(api);
        api.api_version = 1;
        success(getApi(&api), "negotiate module");
        auto input = gpu.texture();
        std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs;
        VDXRFG_CreateInfoV1 info{};
        info.struct_size = sizeof(info);
        info.api_version = 1;
        info.device = gpu.device.Get();
        info.queue = gpu.queue.Get();
        info.input = input.Get();
        info.width = width;
        info.height = height;
        info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.multiplier = 3;
        for (UINT bank = 0; bank < 2; ++bank) {
            for (UINT role = 0; role < 3; ++role) {
                outputs[bank][role] = gpu.texture();
                info.outputs[bank][role] = outputs[bank][role].Get();
            }
        }
        const bool signalTest = argc == 3;
        if (!signalTest) {
            attach(true);
        }
        void* context{};
        const HRESULT hr = api.Create(&info, &context);
        if (signalTest) {
            success(hr, "initialize before signal fault");
            const auto pixels = scene(0);
            gpu.transfer(input.Get(), &pixels);
            faultQueue = gpu.queue.Get();
            originalSignal = reinterpret_cast<SignalFn>((*reinterpret_cast<void***>(faultQueue))[14]);
            failedSignal = static_cast<uint32_t>(std::stoul(argv[2]));
            attachSignal(true);
            VDXRFG_SubmitInfoV1 submit{};
            submit.struct_size = sizeof(submit);
            submit.api_version = 1;
            submit.input = input.Get();
            submit.serial = 1;
            submit.display_time_ns = 1000000000;
            submit.display_period_ns = 10000000;
            for (UINT eye = 0; eye < 2; ++eye) {
                submit.views[eye].orientation[3] = 1;
                submit.views[eye].fov[0] = submit.views[eye].fov[3] = -0.8f;
                submit.views[eye].fov[1] = submit.views[eye].fov[2] = 0.8f;
                submit.views[eye].rect = {eye * eyeWidth, 0, eyeWidth, height};
            }
            VDXRFG_TicketV1 ticket{};
            const auto faultResult = api.Submit(context, &submit, &ticket);
            attachSignal(false);
            const uint64_t drain = gpu.nextFence++;
            success(originalSignal(gpu.queue.Get(), gpu.fence.Get(), drain), "fixture drains faulted GPU work");
            gpu.wait(gpu.fence.Get(), drain);
            check(signalCount >= failedSignal && faultResult == VDXRFG_E_POISONED && ticket.token && !ticket.role_mask,
                  "post-submission signal failure retains recovery identity without publishing outputs");
            check(api.Retire(context, ticket.token) == VDXRFG_E_POISONED && api.Destroy(context) == VDXRFG_E_POISONED,
                  "uncertain context stays owned, even after fixture-specific drain");
            gpu.noErrors();
            // Intentionally retain module/context until process exit: production
            // has no authority to infer completion from our private fixture fence.
            std::printf("PASS: signal fault %u retains unpublished recovery transaction\n", failedSignal);
            return 0;
        }
        const bool retained = hr == VDXRFG_E_POISONED && context && unsafeUnregister == 0;
        if (context) {
            VDXRFG_StatisticsV1 statistics{};
            statistics.struct_size = sizeof(statistics);
            statistics.api_version = VDXRFG_API_VERSION;
            success(api.GetStatistics(context, &statistics), "statistics tolerate incomplete initialization");
            check(statistics.poisoned && !statistics.captures && !statistics.pairs && !statistics.live_tickets &&
                      !statistics.last_gpu_us,
                  "incomplete initialization exposes poison without reading unavailable GPU timing");
            check(api.Destroy(context) == VDXRFG_E_POISONED, "pending initialization cannot be destroyed");
        }
        releaseRegistration();
        if (context) {
            success(api.Destroy(context), "settled initialization can be destroyed");
        }
        attach(false);
        FreeLibrary(module);
        FreeLibrary(driver);
        check(retained, "uncertain initialization retains context before unregistering driver resources");
        gpu.noErrors();
        std::puts("PASS: asynchronous NVIDIA initialization failure retains ownership then drains");
        return 0;
    } catch (const std::exception& error) {
        if (gate) {
            gate->Signal(1);
        }
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
