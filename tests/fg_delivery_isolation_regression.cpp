// Actual two-device colour-delivery proof. No SDK/headset behavior is inferred.
#define main retainedDeliveryInteropMain
#include "nr_fg_integration_regression.cpp"
#undef main

#include <iostream>
#include <detours.h>

extern "C" size_t deliveryContextSlot(unsigned method);
extern "C" size_t deliveryFenceImportSlot();
extern "C" size_t deliveryFenceCompletedSlot();

namespace {
    struct Counts {
        unsigned copyResource{}, copyRegion{}, dispatch{};
        unsigned copies() const {
            return copyResource + copyRegion;
        }
    };
    struct ContextHooks {
        using Copy = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
        using Region = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
        using Dispatch = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
        using Wait = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
        using Import = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device5*, HANDLE, REFIID, void**);
        using Completed = UINT64(STDMETHODCALLTYPE*)(ID3D11Fence*);
        static inline Copy originalCopy{};
        static inline Region originalRegion{};
        static inline Dispatch originalDispatch{};
        static inline Wait originalWait{};
        static inline Import originalImport{};
        static inline Completed originalCompleted{};
        static inline thread_local ContextHooks* active{};
        ID3D11DeviceContext4* producer{};
        ID3D11DeviceContext4* delivery{};
        ID3D11Device5* deliveryDevice{};
        Counts counts;
        bool counting{}, omitPendingProducerWait{}, failDeliveryWait{};
        unsigned omitted{}, injected{};
        unsigned failImportAt{}, importAttempts{}, importFailures{};
        bool recordFence{};
        ID3D11Fence* observedFence{};
        ID3D11Fence* removedFence{};

        static bool watches(ID3D11DeviceContext* context) {
            return active && (context == active->producer || context == active->delivery);
        }
        static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* context, ID3D11Resource* dst, ID3D11Resource* src) {
            if (watches(context) && active->counting)
                ++active->counts.copyResource;
            originalCopy(context, dst, src);
        }
        static void STDMETHODCALLTYPE region(ID3D11DeviceContext* context,
                                             ID3D11Resource* dst,
                                             UINT sub,
                                             UINT x,
                                             UINT y,
                                             UINT z,
                                             ID3D11Resource* src,
                                             UINT srcSub,
                                             const D3D11_BOX* box) {
            if (watches(context) && active->counting)
                ++active->counts.copyRegion;
            originalRegion(context, dst, sub, x, y, z, src, srcSub, box);
        }
        static void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z) {
            if (watches(context) && active->counting)
                ++active->counts.dispatch;
            originalDispatch(context, x, y, z);
        }
        static HRESULT STDMETHODCALLTYPE wait(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
            if (active && context == active->delivery && active->failDeliveryWait) {
                ++active->injected;
                return E_FAIL;
            }
            if (active && context == active->producer && active->omitPendingProducerWait &&
                fence->GetCompletedValue() < value) {
                ++active->omitted;
                return S_OK; // Deliberately compiled causal negative control.
            }
            return originalWait(context, fence, value);
        }
        static HRESULT STDMETHODCALLTYPE importFence(ID3D11Device5* device, HANDLE handle, REFIID iid, void** result) {
            if (active && device == active->deliveryDevice && active->failImportAt &&
                ++active->importAttempts == active->failImportAt) {
                ++active->importFailures;
                *result = nullptr;
                return E_FAIL;
            }
            return originalImport(device, handle, iid, result);
        }
        static UINT64 STDMETHODCALLTYPE fenceCompleted(ID3D11Fence* fence) {
            if (active && active->recordFence) {
                active->observedFence = fence;
                active->recordFence = false;
            }
            if (active && active->removedFence == fence)
                return UINT64_MAX;
            return originalCompleted(fence);
        }
        ContextHooks(ID3D11DeviceContext4* from, ID3D11DeviceContext4* to, ID3D11Device5* device, ID3D11Fence* sample)
            : producer(from), delivery(to), deliveryDevice(device) {
            check(!active, "one owned context hook set");
            const auto producerVtable = *reinterpret_cast<void***>(producer);
            const auto deliveryVtable = *reinterpret_cast<void***>(delivery);
            for (unsigned method = 0; method < 4; ++method)
                check(producerVtable[deliveryContextSlot(method)] == deliveryVtable[deliveryContextSlot(method)],
                      "test hooks cover both immediate context implementations");
            originalCopy = reinterpret_cast<Copy>(producerVtable[deliveryContextSlot(0)]);
            originalRegion = reinterpret_cast<Region>(producerVtable[deliveryContextSlot(1)]);
            originalDispatch = reinterpret_cast<Dispatch>(producerVtable[deliveryContextSlot(2)]);
            originalWait = reinterpret_cast<Wait>(producerVtable[deliveryContextSlot(3)]);
            originalImport = reinterpret_cast<Import>((*reinterpret_cast<void***>(device))[deliveryFenceImportSlot()]);
            originalCompleted =
                reinterpret_cast<Completed>((*reinterpret_cast<void***>(sample))[deliveryFenceCompletedSlot()]);
            check(DetourTransactionBegin() == NO_ERROR, "begin context hooks");
            if (DetourUpdateThread(GetCurrentThread()) || DetourAttach(reinterpret_cast<PVOID*>(&originalCopy), copy) ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalRegion), region) ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalDispatch), dispatch) ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait) ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalImport), importFence) ||
                DetourAttach(reinterpret_cast<PVOID*>(&originalCompleted), fenceCompleted)) {
                DetourTransactionAbort();
                throw std::runtime_error("attach context hooks");
            }
            check(DetourTransactionCommit() == NO_ERROR, "commit context hooks");
            active = this;
        }
        ~ContextHooks() {
            active = nullptr;
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalCopy), copy) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalRegion), region) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalDispatch), dispatch) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalImport), importFence) ||
                DetourDetach(reinterpret_cast<PVOID*>(&originalCompleted), fenceCompleted) || DetourTransactionCommit())
                std::terminate();
        }
        void begin() {
            counts = {};
            counting = true;
        }
        Counts end() {
            counting = false;
            return counts;
        }
    };
    struct Event {
        HANDLE value{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        Event() {
            check(value != nullptr, "completion event");
        }
        ~Event() {
            CloseHandle(value);
        }
        Event(const Event&) = delete;
        Event& operator=(const Event&) = delete;
    };

    bool completed(ID3D11Fence* fence, uint64_t value, DWORD timeout = 5000) {
        const auto before = fence->GetCompletedValue();
        check(before != UINT64_MAX, "completion device not removed");
        if (before >= value)
            return true;
        Event event;
        success(fence->SetEventOnCompletion(value, event.value), "register exact completion");
        const auto result = WaitForSingleObject(event.value, timeout);
        check(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, "bounded completion wait");
        if (result == WAIT_TIMEOUT)
            return false;
        const auto after = fence->GetCompletedValue();
        check(after != UINT64_MAX && after >= value, "event corresponds to exact fence value");
        return true;
    }

    struct DeliveryDevice {
        ComPtr<ID3D11Device5> device;
        ComPtr<ID3D11DeviceContext4> context;
        ComPtr<ID3D11Fence> fence;
        uint64_t value{};

        explicit DeliveryDevice(ID3D12Device* gpu) {
            ComPtr<IDXGIFactory4> factory;
            success(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "delivery factory");
            ComPtr<IDXGIAdapter> adapter;
            success(factory->EnumAdapterByLuid(gpu->GetAdapterLuid(), IID_PPV_ARGS(&adapter)),
                    "delivery uses same adapter");
            ComPtr<ID3D11Device> base;
            ComPtr<ID3D11DeviceContext> immediate;
            success(D3D11CreateDevice(adapter.Get(),
                                      D3D_DRIVER_TYPE_UNKNOWN,
                                      nullptr,
                                      0,
                                      nullptr,
                                      0,
                                      D3D11_SDK_VERSION,
                                      &base,
                                      nullptr,
                                      &immediate),
                    "independent delivery D3D11 device");
            success(base.As(&device), "delivery device5");
            success(immediate.As(&context), "delivery context4");
            success(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "delivery test fence");
        }

        ComPtr<ID3D11Texture2D> texture() {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> result;
            success(device->CreateTexture2D(&desc, nullptr, &result), "delivery destination");
            return result;
        }

        void drain() {
            success(context->Signal(fence.Get(), ++value), "delivery completion marker");
            context->Flush();
            check(completed(fence.Get(), value), "delivery completes without producer release");
        }

        std::vector<uint8_t> read(ID3D11Texture2D* source) {
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.BindFlags = desc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            success(device->CreateTexture2D(&desc, nullptr, &staging), "delivery readback staging");
            context->CopyResource(staging.Get(), source);
            drain();
            D3D11_MAPPED_SUBRESOURCE map{};
            success(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "delivery map after completion");
            std::vector<uint8_t> pixels(width * height * 4);
            for (UINT y = 0; y < height; ++y)
                std::memcpy(pixels.data() + y * width * 4,
                            static_cast<const uint8_t*>(map.pData) + y * map.RowPitch,
                            width * 4);
            context->Unmap(staging.Get(), 0);
            return pixels;
        }
    };

    // Releases its own gate on every exception path before FG objects unwind.
    struct ProducerHold {
        ComPtr<ID3D11Fence> gate, after;
        ComPtr<ID3D12Fence> release;
        bool armed{};
        explicit ProducerHold(Interop& hw) : ProducerHold(hw.device.Get(), hw.context.Get(), hw.gpu.device.Get()) {
        }
        ProducerHold(ID3D11Device5* device, ID3D11DeviceContext4* context, ID3D12Device* gpu) {
            success(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)), "queue hold gate");
            success(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&after)), "queue hold marker");
            HANDLE shared{};
            success(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared), "export test gate");
            const auto imported = gpu->OpenSharedHandle(shared, IID_PPV_ARGS(&release));
            CloseHandle(shared);
            success(imported, "import CPU-release gate");
            success(context->Wait(gate.Get(), 1), "hold queue commands");
            armed = true;
            try {
                success(context->Signal(after.Get(), 1), "marker after owned queue wait");
                context->Flush();
            } catch (...) {
                release->Signal(1);
                throw;
            }
        }
        ~ProducerHold() {
            if (armed)
                release->Signal(1);
        }
        void finish() {
            success(release->Signal(1), "release only the fixture-owned producer gate");
            armed = false;
            check(completed(after.Get(), 1), "held queue resumes");
        }
    };

    template <class Info>
    auto selectDelivery(Info& info, DeliveryDevice& delivery, int)
        -> decltype(info.deliveryDevice11 = delivery.device.Get(),
                    info.deliveryContext11 = delivery.context.Get(),
                    bool{}) {
        info.deliveryDevice11 = delivery.device.Get();
        info.deliveryContext11 = delivery.context.Get();
        return true;
    }
    template <class Info>
    bool selectDelivery(Info&, DeliveryDevice&, long) {
        return false;
    }

    template <class Info>
    auto rejectInvalidDelivery(const Info& good, Interop& hw, DeliveryDevice& delivery, int)
        -> decltype(good.deliveryDevice11, bool{}) {
        std::array<Info, 4> invalid{good, good, good, good};
        invalid[0].deliveryContext11 = nullptr;
        invalid[1].deliveryDevice11 = nullptr;
        invalid[2].deliveryContext11 = hw.context.Get();
        invalid[3].deliveryDevice11 = hw.device.Get();
        invalid[3].deliveryContext11 = hw.context.Get();
        for (const auto& info : invalid) {
            FrameGeneration rejected;
            check(rejected.initialize(info) == VDXRFG_E_REJECTED && !rejected.initialized(),
                  "incomplete/mismatched/nonindependent delivery configuration rejected");
        }
        ComPtr<ID3D11DeviceContext3> deferred;
        ComPtr<ID3D11DeviceContext4> deferred4;
        success(delivery.device->CreateDeferredContext3(0, &deferred), "negative deferred delivery context");
        success(deferred.As(&deferred4), "negative deferred delivery context4");
        auto info = good;
        info.deliveryContext11 = deferred4.Get();
        FrameGeneration rejected;
        check(rejected.initialize(info) == VDXRFG_E_REJECTED && !rejected.initialized(),
              "deferred delivery context rejected");
        return true;
    }
    template <class Info>
    bool rejectInvalidDelivery(const Info&, Interop&, DeliveryDevice&, long) {
        return false;
    }

    ComPtr<ID3D11Texture2D> sourcePixels(
        Interop& hw, const std::vector<uint8_t>& rgba, const std::wstring& scenario, uint32_t& x, uint32_t& y) {
        x = scenario == L"crop" ? 7 : 0;
        y = scenario == L"crop" ? 5 : 0;
        const bool bgra = scenario == L"bgra" || scenario == L"bgra-srgb";
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width + 2 * x;
        desc.Height = height + 2 * y;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = scenario == L"bgra-srgb" ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                      : bgra                   ? DXGI_FORMAT_B8G8R8A8_UNORM
                                               : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        std::vector<uint8_t> pixels(size_t(desc.Width) * desc.Height * 4, 0x19);
        for (uint32_t row = 0; row < height; ++row) {
            auto* target = pixels.data() + (size_t(row + y) * desc.Width + x) * 4;
            std::memcpy(target, rgba.data() + size_t(row) * width * 4, width * 4);
            if (bgra)
                for (uint32_t col = 0; col < width; ++col)
                    std::swap(target[col * 4], target[col * 4 + 2]);
        }
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = desc.Width * 4;
        ComPtr<ID3D11Texture2D> result;
        success(hw.device->CreateTexture2D(&desc, &data, &result), "finalized source format/rectangle");
        return result;
    }

    void qualify(const std::filesystem::path& module, unsigned multiplier, const std::wstring& scenario) {
        Interop hw;
        DeliveryDevice delivery(hw.gpu.device.Get());
        ContextHooks hooks(hw.context.Get(), delivery.context.Get(), delivery.device.Get(), delivery.fence.Get());
        check(delivery.device.Get() != hw.device.Get() && delivery.context.Get() != hw.context.Get(),
              "two independent same-adapter D3D11 domains");
        FrameGeneration legacy, isolated;
        FrameGeneration::InitInfo info{hw.device.Get(),
                                       hw.context.Get(),
                                       hw.gpu.device.Get(),
                                       module,
                                       eyeWidth,
                                       height,
                                       multiplier,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
        success(legacy.initialize(info), "legacy reference initialization");
        const bool configured = selectDelivery(info, delivery, 0);
        if (scenario == L"invalid-init") {
            check(configured && rejectInvalidDelivery(info, hw, delivery, 0), "validated delivery API exists");
            success(legacy.shutdown(), "invalid delivery reference shutdown");
            std::puts("PASS: delivery initialization contracts");
            return;
        }
        success(isolated.initialize(info), "delivery prototype initialization");
        FrameGenerationCadence cadence;
        FinalProjectionInput input;
        input.serial = 1;
        input.period = 10000000;
        input.time = 1000000000;
        input.slots = cadence.plan(
            cadence.reserve(0, input.time - (multiplier - 1) * input.period, input.period, multiplier, 0), 0, false);
        const auto a = scene(0), b = scene(24);
        uint32_t sourceX{}, sourceY{};
        input.colour[0] = input.colour[1] = sourcePixels(hw, a, scenario, sourceX, sourceY);
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& view = input.views[eye];
            view.rect = {eye * eyeWidth + sourceX, sourceY, eyeWidth, height};
            view.orientation[3] = 1;
            view.position[0] = eye ? .032f : -.032f;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
        }
        OwnedFrameTransaction prime[2];
        success(legacy.capture(input, prime[0]), "legacy prime");
        if (scenario == L"consumed-import-failure" || scenario == L"ready-import-failure") {
            hooks.failImportAt = scenario == L"consumed-import-failure" ? 1u : 2u;
            check(isolated.capture(input, prime[1]) == VDXRFG_E_POISONED && prime[1].ticket.token,
                  "post-submit delivery import failure retains an accepted ticket");
            hooks.failImportAt = 0;
            check(hooks.importFailures == 1, "exact requested delivery fence import failed");
            hw.gpu.wait(static_cast<ID3D12Fence*>(prime[1].ticket.output_ready_fence),
                        prime[1].ticket.output_ready_value);
            check(isolated.shutdown() == VDXRFG_E_POISONED,
                  "post-submit import failure cannot remain temporary BUSY forever");
            hw.gpu.wait(static_cast<ID3D12Fence*>(prime[0].ticket.output_ready_fence),
                        prime[0].ticket.output_ready_value);
            hw.retire(legacy, prime[0]);
            success(legacy.shutdown(), "import failure reference shutdown");
            std::puts("PASS: post-submit delivery import failure is terminal");
            return;
        }
        success(isolated.capture(input, prime[1]), "isolated prime");
        hw.gpu.wait(static_cast<ID3D12Fence*>(prime[0].ticket.output_ready_fence), prime[0].ticket.output_ready_value);
        hw.gpu.wait(static_cast<ID3D12Fence*>(prime[1].ticket.output_ready_fence), prime[1].ticket.output_ready_value);
        hw.retire(legacy, prime[0]);
        hw.retire(isolated, prime[1]);
        const auto previous = input.time;
        input.serial = 2;
        input.time += multiplier * input.period;
        input.slots = cadence.plan(
            cadence.reserve(0, previous + input.period, input.period, multiplier, multiplier), previous, true);
        input.colour[0] = input.colour[1] = sourcePixels(hw, b, scenario, sourceX, sourceY);
        OwnedFrameTransaction pair[2];
        success(legacy.capture(input, pair[0]), "legacy pair");
        success(isolated.capture(input, pair[1]), "isolated pair");
        hw.gpu.wait(static_cast<ID3D12Fence*>(pair[0].ticket.output_ready_fence), pair[0].ticket.output_ready_value);
        hw.gpu.wait(static_cast<ID3D12Fence*>(pair[1].ticket.output_ready_fence), pair[1].ticket.output_ready_value);
        if (scenario == L"consumer-removal") {
            auto real = delivery.texture();
            success(isolated.copyOutput(pair[1], FrameGenerationRole::real, real.Get()),
                    "real copy before consumer fault");
            delivery.drain();
            hooks.recordFence = true;
            check(isolated.copyReady(pair[1], FrameGenerationRole::real) && hooks.observedFence,
                  "observe actual delivery consumer fence identity");
            hooks.removedFence = hooks.observedFence;
            check(isolated.retire(pair[1]) == VDXRFG_E_POISONED, "consumer removal poisons retirement");
            check(isolated.shutdown() == VDXRFG_E_POISONED,
                  "consumer device removal is terminal despite a healthy producer device");
            hooks.removedFence = nullptr;
            hw.retire(legacy, pair[0]);
            success(legacy.shutdown(), "consumer removal reference shutdown");
            std::puts("PASS: independent consumer removal is terminal");
            return;
        }
        if (scenario == L"wait-failure") {
            auto destination = delivery.texture();
            hooks.failDeliveryWait = true;
            check(isolated.copyOutput(pair[1], FrameGenerationRole::real, destination.Get()) == VDXRFG_E_POISONED,
                  "failed real delivery wait poisons owned graph");
            hooks.failDeliveryWait = false;
            check(hooks.injected == 1, "one actual delivery dependency failure");
            auto next = input;
            ++next.serial;
            next.time += multiplier * next.period;
            OwnedFrameTransaction rejected;
            check(isolated.capture(next, rejected) == VDXRFG_E_REJECTED,
                  "poisoned graph cannot be hidden as temporary atlas pressure");
            check(isolated.shutdown() == VDXRFG_E_POISONED, "failed delivery retains uncertain graph");
            hw.retire(legacy, pair[0]);
            success(legacy.shutdown(), "failure reference shutdown");
            std::puts("PASS: poisoned delivery keeps terminal precedence");
            return;
        }
        if (scenario == L"legacy-blocked") {
            auto legacyReal = hw.texture();
            auto isolatedReal = delivery.texture();
            ProducerHold hold(hw);
            success(legacy.copyOutput(pair[0], FrameGenerationRole::real, legacyReal.Get()),
                    "queue original shared-context real copy behind producer");
            check(legacy.waitForCopy(pair[0], FrameGenerationRole::real, 10) == VDXRFG_E_BUSY &&
                      !legacy.copyReady(pair[0], FrameGenerationRole::real),
                  "original final-copy path really blocks on the same producer hold");
            success(isolated.copyOutput(pair[1], FrameGenerationRole::real, isolatedReal.Get()),
                    "queue independent real copy under identical producer hold");
            delivery.drain();
            check(delivery.read(isolatedReal.Get()) == b && hold.after->GetCompletedValue() == 0 &&
                      !legacy.copyReady(pair[0], FrameGenerationRole::real),
                  "isolated exact B completes while original B and producer stay pending");
            hold.finish();
            success(legacy.waitForCopy(pair[0], FrameGenerationRole::real, 1000),
                    "original B completes after producer release");
            check(hw.read(legacyReal.Get()) == b, "released original B matches exact delivery pixels");
            hw.retire(legacy, pair[0]);
            hw.retire(isolated, pair[1]);
            success(legacy.shutdown(), "blocked legacy reference shutdown");
            success(isolated.shutdown(), "blocked legacy delivery shutdown");
            hw.gpu.noErrors();
            std::puts("PASS: original delivery blocks while isolated delivery completes under the same producer hold");
            return;
        }
        std::vector<FrameGenerationRole> roles{FrameGenerationRole::real, FrameGenerationRole::synthetic1};
        if (multiplier >= 3)
            roles.push_back(FrameGenerationRole::synthetic2);
        if (multiplier == 4)
            roles.push_back(FrameGenerationRole::synthetic3);
        std::vector<std::vector<uint8_t>> reference;
        std::vector<ComPtr<ID3D11Texture2D>> retainedReference;
        std::vector<Counts> referenceCounts;
        for (auto role : roles) {
            auto destination = hw.texture();
            hooks.begin();
            success(legacy.copyOutput(pair[0], role, destination.Get()), "legacy reference copy");
            referenceCounts.push_back(hooks.end());
            reference.push_back(hw.read(destination.Get()));
            if (role == FrameGenerationRole::real)
                check(reference.back() == b, "legacy real frame preserves finalized encoded colour bytes");
            retainedReference.push_back(destination);
        }
        if (scenario == L"reuse" || scenario == L"omit-reuse-control" || scenario == L"unprepared") {
            auto next = input;
            ++next.serial;
            next.time += multiplier * next.period;
            next.slots =
                cadence.plan(cadence.reserve(0, input.time + input.period, next.period, multiplier, 2 * multiplier),
                             input.time,
                             true);
            const auto c = scene(48);
            next.colour[0] = next.colour[1] = hw.texture(&c);
            if (scenario == L"unprepared") {
                OwnedFrameTransaction refused;
                check(isolated.capture(next, refused) == VDXRFG_E_BUSY && !refused.ticket.token,
                      "live unprepared canonical B blocks only the next atlas pack");
                check(isolated.reset(0) == VDXRFG_E_BUSY, "busy reset preserves unprepared real owner");
                auto real = delivery.texture();
                success(isolated.copyOutput(pair[1], FrameGenerationRole::real, real.Get()),
                        "B remains available after nonwriting capture/reset refusal");
                delivery.drain();
                check(delivery.read(real.Get()) == reference[0], "refused reuse preserves original B pixels");
                hw.retire(legacy, pair[0]);
                hw.retire(isolated, pair[1]);
            } else {
                ProducerHold deliveryHold(delivery.device.Get(), delivery.context.Get(), hw.gpu.device.Get());
                auto real = delivery.texture();
                success(isolated.copyOutput(pair[1], FrameGenerationRole::real, real.Get()),
                        "queue held real delivery");
                check(!isolated.copyReady(pair[1], FrameGenerationRole::real), "held real copy is not complete");
                check(isolated.retire(pair[1]) == VDXRFG_E_BUSY && isolated.shutdown() == VDXRFG_E_BUSY &&
                          isolated.reset(0) == VDXRFG_E_BUSY,
                      "pending delivery retains graph across retire/shutdown/reset");
                hooks.omitPendingProducerWait = scenario == L"omit-reuse-control";
                OwnedFrameTransaction newer;
                success(isolated.capture(next, newer), "next producer is GPU-ordered behind real delivery");
                hooks.omitPendingProducerWait = false;
                check(hooks.omitted == (scenario == L"omit-reuse-control" ? 1u : 0u),
                      "negative control removes exactly one pending producer reuse dependency");
                ComPtr<ID3D11Fence> afterPack;
                success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&afterPack)),
                        "after-pack marker");
                success(hw.context->Signal(afterPack.Get(), 1), "mark next packed atlas");
                hw.context->Flush();
                check(!completed(afterPack.Get(), 1, 10), "canonical atlas reuse cannot pass pending B delivery");
                deliveryHold.finish();
                check(completed(afterPack.Get(), 1), "canonical atlas reuse resumes on real-copy completion");
                delivery.drain();
                check(delivery.read(real.Get()) == reference[0], "next pack cannot overwrite prior B delivery");
                success(isolated.copyOutput(pair[1], FrameGenerationRole::real, real.Get()),
                        "prepared old B stays reusable");
                auto redirect = delivery.texture();
                check(isolated.copyOutput(pair[1], FrameGenerationRole::real, redirect.Get()) == VDXRFG_E_REJECTED,
                      "prepared old B cannot redirect to another destination");
                check(delivery.read(real.Get()) == reference[0],
                      "prepared B is not recopied from newer canonical atlas");
                hw.gpu.wait(static_cast<ID3D12Fence*>(newer.ticket.output_ready_fence),
                            newer.ticket.output_ready_value);
                hw.retire(legacy, pair[0]);
                hw.retire(isolated, pair[1]);
                hw.retire(isolated, newer);
            }
            success(legacy.shutdown(), "reuse reference shutdown");
            success(isolated.shutdown(), "reuse prototype shutdown");
            hw.gpu.noErrors();
            std::puts("PASS: canonical B ownership and GPU reuse ordering");
            return;
        }
        if (scenario == L"future-capture") {
            auto real = delivery.texture();
            hooks.begin();
            success(isolated.copyOutput(pair[1], FrameGenerationRole::real, real.Get()),
                    "prepare current B before future capture");
            const auto realCopies = hooks.end();
            delivery.drain();
            check(delivery.read(real.Get()) == reference[0] && realCopies.copies() == referenceCounts[0].copies(),
                  "current B uses the existing final copy exactly once");
            const auto preparedValue = pair[1].consumerValue;
            auto next = input;
            ++next.serial;
            next.time += multiplier * next.period;
            next.slots =
                cadence.plan(cadence.reserve(0, input.time + input.period, next.period, multiplier, 2 * multiplier),
                             input.time,
                             true);
            const auto c = scene(48);
            next.colour[0] = next.colour[1] = hw.texture(&c);
            ProducerHold hold(hw);
            OwnedFrameTransaction newer;
            success(isolated.capture(next, newer), "accept actual next FG ticket behind held producer");
            const auto futureReady = static_cast<ID3D12Fence*>(newer.ticket.output_ready_fence);
            check(newer.ticket.token && newer.ticket.bank != pair[1].ticket.bank &&
                      futureReady->GetCompletedValue() < newer.ticket.output_ready_value,
                  "future FG ticket exists and is genuinely incomplete");
            std::vector<ComPtr<ID3D11Texture2D>> destinations;
            for (size_t i = 1; i < roles.size(); ++i) {
                auto destination = delivery.texture();
                hooks.begin();
                success(isolated.copyOutput(pair[1], roles[i], destination.Get()),
                        "current synthetic delivery passes actual pending future capture");
                const auto count = hooks.end();
                check(count.copies() == referenceCounts[i].copies() && count.dispatch == referenceCounts[i].dispatch,
                      "actual future capture adds no current-output copy or conversion");
                delivery.drain();
                check(delivery.read(destination.Get()) == reference[i],
                      "current synthetic pixels remain exact with actual future capture pending");
                check(isolated.copyReady(pair[1], roles[i]) && hold.after->GetCompletedValue() == 0 &&
                          futureReady->GetCompletedValue() < newer.ticket.output_ready_value,
                      "current delivery completes without draining actual next FG work");
                destinations.push_back(destination);
            }
            hooks.begin();
            success(isolated.copyOutput(pair[1], FrameGenerationRole::real, real.Get()),
                    "current protected B survives actual future capture");
            const auto repeated = hooks.end();
            check(!repeated.copies() && !repeated.dispatch && pair[1].consumerValue == preparedValue &&
                      delivery.read(real.Get()) == reference[0],
                  "old B reuses its prepared destination without reading the future atlas");
            hold.finish();
            hw.gpu.wait(futureReady, newer.ticket.output_ready_value);
            hw.retire(legacy, pair[0]);
            hw.retire(isolated, pair[1]);
            hw.retire(isolated, newer);
            success(legacy.shutdown(), "future capture reference shutdown");
            success(isolated.shutdown(), "future capture delivery shutdown");
            hw.gpu.noErrors();
            std::cout << "FUTURE_CAPTURE_PROOF {\"multiplier\":" << multiplier
                      << ",\"actual_future_ticket_pending\":true,\"current_pixel_roles\":" << roles.size()
                      << ",\"extra_final_copies\":0,\"sdk_tested\":false,\"headset_tested\":false}\n";
            return;
        }
        ProducerHold hold(hw);
        check(!completed(hold.after.Get(), 1, 5), "negative control really holds producer GPU queue");
        std::vector<ComPtr<ID3D11Texture2D>> destinations;
        std::vector<Counts> deliveryCounts;
        for (size_t i = 0; i < roles.size(); ++i) {
            auto destination = delivery.texture();
            hooks.begin();
            // This is the intended baseline RED, not a missing-field assertion.
            success(isolated.copyOutput(pair[1], roles[i], destination.Get()),
                    "ready output must copy on independent delivery device");
            const auto count = hooks.end();
            deliveryCounts.push_back(count);
            check(count.copies() > 0 && count.copies() <= referenceCounts[i].copies() &&
                      count.dispatch <= referenceCounts[i].dispatch,
                  "independent delivery adds no host colour copy or conversion pass");
            delivery.drain();
            check(isolated.copyReady(pair[1], roles[i]), "actual delivery consumer fence completed");
            check(hold.after->GetCompletedValue() == 0, "delivery did not release or drain future producer work");
            check(delivery.read(destination.Get()) == reference[i], "independent delivery pixels match legacy exactly");
            destinations.push_back(destination);
        }
        check(configured, "candidate exposes validated delivery initialization");
        auto stale = pair[1];
        hold.finish();
        hw.retire(legacy, pair[0]);
        hw.retire(isolated, pair[1]);
        check(isolated.copyOutput(stale, FrameGenerationRole::real, destinations[0].Get()) == VDXRFG_E_REJECTED,
              "retired delivery rejects stale owner");
        success(legacy.shutdown(), "legacy shutdown");
        success(isolated.shutdown(), "isolated shutdown");
        hw.gpu.noErrors();
        std::cout << "DELIVERY_PROOF {\"multiplier\":" << multiplier << ",\"pixel_roles\":" << roles.size()
                  << ",\"producer_pending_during_delivery\":true,\"host_copy_count_checked\":true,"
                     "\"sdk_tested\":false,\"headset_tested\":false,\"host_copies\":[";
        for (size_t i = 0; i < roles.size(); ++i) {
            if (i)
                std::cout << ',';
            std::cout << "{\"role\":" << static_cast<unsigned>(roles[i])
                      << ",\"legacy\":" << referenceCounts[i].copies() << ",\"delivery\":" << deliveryCounts[i].copies()
                      << '}';
        }
        std::cout << "]}\n";
    }
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3 || argc == 4, "usage: module multiplier [scenario]");
        const unsigned multiplier = std::stoul(argv[2]);
        check(multiplier >= 2 && multiplier <= 4, "known multiplier");
        const std::wstring scenario = argc == 4 ? argv[3] : L"pending-producer";
        check(scenario == L"pending-producer" || scenario == L"wait-failure" || scenario == L"reuse" ||
                  scenario == L"unprepared" || scenario == L"omit-reuse-control" || scenario == L"invalid-init" ||
                  scenario == L"bgra" || scenario == L"bgra-srgb" || scenario == L"crop" ||
                  scenario == L"consumed-import-failure" || scenario == L"ready-import-failure" ||
                  scenario == L"consumer-removal" || scenario == L"future-capture" || scenario == L"legacy-blocked",
              "known isolation scenario");
        qualify(std::filesystem::absolute(argv[1]), multiplier, scenario);
        std::puts("PASS: ready colour delivery is independent of future producer GPU work");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
