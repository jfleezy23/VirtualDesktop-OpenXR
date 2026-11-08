#include "fg_gpu_helpers.h"
#include <d3d12sdklayers.h>
#include <detours.h>

#include <type_traits>
#include <atomic>

namespace {
    constexpr uint32_t depthOpacityReset = VDXRFG_DEPTH_OPACITY_RESET;
    constexpr uint32_t resetColorOpaque = VDXRFG_RESET_COLOR_OPAQUE;
    constexpr uint32_t resetColorAlpha = VDXRFG_RESET_COLOR_ALPHA;
    struct ProbeFault {
        using CreateResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
                                                           const D3D12_HEAP_PROPERTIES*,
                                                           D3D12_HEAP_FLAGS,
                                                           const D3D12_RESOURCE_DESC*,
                                                           D3D12_RESOURCE_STATES,
                                                           const D3D12_CLEAR_VALUE*,
                                                           REFIID,
                                                           void**);
        using MapResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
        inline static CreateResource originalCreate{};
        inline static MapResource originalMap{};
        inline static ID3D12Device* owner{};
        inline static std::atomic<unsigned> attempts{}, injected{};
        // Independent public-fixture layout: 1024 R32 pixels, 32 rows, two endpoints.
        static constexpr UINT64 probeBytes = 1024ull * sizeof(float) * 32 * 2;
        bool attached{}, allocation{};
        ComPtr<ID3D12Resource> borrowedReadback;

        static bool shape(const D3D12_RESOURCE_DESC& desc) {
            return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && desc.Width == probeBytes && desc.Height == 1 &&
                   desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.Format == DXGI_FORMAT_UNKNOWN &&
                   desc.SampleDesc.Count == 1 && desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        }
        static HRESULT STDMETHODCALLTYPE create(ID3D12Device* device,
                                                const D3D12_HEAP_PROPERTIES* heap,
                                                D3D12_HEAP_FLAGS flags,
                                                const D3D12_RESOURCE_DESC* desc,
                                                D3D12_RESOURCE_STATES state,
                                                const D3D12_CLEAR_VALUE* clear,
                                                REFIID iid,
                                                void** out) {
            if (device == owner && heap && heap->Type == D3D12_HEAP_TYPE_READBACK && desc && shape(*desc) &&
                state == D3D12_RESOURCE_STATE_COPY_DEST) {
                if (attempts.fetch_add(1) == 0) {
                    ++injected;
                    if (out)
                        *out = nullptr;
                    return E_OUTOFMEMORY;
                }
            }
            return originalCreate(device, heap, flags, desc, state, clear, iid, out);
        }
        static HRESULT STDMETHODCALLTYPE map(ID3D12Resource* resource,
                                             UINT subresource,
                                             const D3D12_RANGE* range,
                                             void** out) {
            D3D12_HEAP_PROPERTIES heap{};
            D3D12_HEAP_FLAGS flags{};
            ComPtr<ID3D12Device> device;
            if (shape(resource->GetDesc()) && SUCCEEDED(resource->GetHeapProperties(&heap, &flags)) &&
                heap.Type == D3D12_HEAP_TYPE_READBACK && SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device))) &&
                device.Get() == owner) {
                if (attempts.fetch_add(1) == 0) {
                    ++injected;
                    if (out)
                        *out = nullptr;
                    return E_FAIL;
                }
            }
            return originalMap(resource, subresource, range, out);
        }
        ProbeFault(ID3D12Device* device, const std::string& mode) {
            if (mode != "gpu-probe-alloc-fail" && mode != "gpu-probe-map-fail")
                return;
            owner = device;
            allocation = mode == "gpu-probe-alloc-fail";
            attempts = injected = 0;
            // Borrow only public vtables from fixture-owned GPU objects. The module
            // context, tickets, flags and actual probe resource are never forged.
            if (allocation) {
                originalCreate = reinterpret_cast<CreateResource>((*reinterpret_cast<void***>(device))[27]);
            } else {
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = probeBytes;
                desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                success(device->CreateCommittedResource(&heap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr,
                                                        IID_PPV_ARGS(&borrowedReadback)),
                        "borrow Map vtable");
                originalMap = reinterpret_cast<MapResource>((*reinterpret_cast<void***>(borrowedReadback.Get()))[8]);
            }
            change(true);
            attached = true;
        }
        void change(bool attach) {
            LONG result = DetourTransactionBegin();
            if (result != NO_ERROR)
                success(HRESULT_FROM_WIN32(result), "begin probe hook transaction");
            result = DetourUpdateThread(GetCurrentThread());
            auto target =
                allocation ? reinterpret_cast<PVOID*>(&originalCreate) : reinterpret_cast<PVOID*>(&originalMap);
            auto hook = allocation ? reinterpret_cast<PVOID>(create) : reinterpret_cast<PVOID>(map);
            if (result == NO_ERROR)
                result = attach ? DetourAttach(target, hook) : DetourDetach(target, hook);
            if (result == NO_ERROR)
                result = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            success(HRESULT_FROM_WIN32(result), "change probe fault hook");
        }
        void finish() {
            if (!attached)
                return;
            change(false);
            attached = false;
            check(attempts.load() == 1 && injected.load() == 1,
                  "exactly one diagnostic readback fault across all captures and Reset; no retry");
            std::printf("PROBE_FAULT stage=%s attempts=%u injected=%u\n",
                        allocation ? "prepare" : "map",
                        attempts.load(),
                        injected.load());
        }
        ~ProbeFault() {
            if (attached) {
                try {
                    change(false);
                } catch (...) {
                    std::terminate();
                }
            }
        }
    };
} // namespace

static_assert(std::is_standard_layout_v<VDXRFG_CreateInfoV5>);
static_assert(std::is_standard_layout_v<VDXRFG_SubmitInfoV5>);
static_assert(sizeof(VDXRFG_ApiV1) == 56 && sizeof(VDXRFG_ApiV4) == 72);
static_assert(sizeof(VDXRFG_ApiV5) == 80);
static_assert(offsetof(VDXRFG_CreateInfoV5, base) == 8);
static_assert(offsetof(VDXRFG_SubmitInfoV5, base) == 8);

int main(int argc, char** argv) {
    try {
        check(
            argc == 2 ||
                (argc == 3 &&
                 (std::string(argv[2]) == "gpu" || std::string(argv[2]) == "gpu-gbv" ||
                  std::string(argv[2]) == "gpu-probe-alloc-fail" || std::string(argv[2]) == "gpu-probe-map-fail" ||
                  std::string(argv[2]) == "gpu-opaque" || std::string(argv[2]) == "gpu-opacity-reset")),
            "module path [gpu|gpu-gbv|gpu-probe-alloc-fail|gpu-probe-map-fail|gpu-opaque|gpu-opacity-reset] required");
        const auto path = std::filesystem::absolute(argv[1]);
        HMODULE module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load module");
        const auto getApi = reinterpret_cast<VDXRFG_GetApiV5Fn>(GetProcAddress(module, "VDXRFG_GetApiV5"));
        check(getApi != nullptr, "V5 export");
        VDXRFG_ApiV5 api{};
        api.struct_size = sizeof(api);
        api.api_version = VDXRFG_API_VERSION_5;
        success(getApi(&api), "negotiate V5 without GPU initialization");
        check(api.max_multiplier == 4 && !api.reserved &&
                  (api.supported_depth_capabilities & VDXRFG_DEPTH_CANONICAL_TRANSPORT),
              "explicit transport capability");
        check(api.Create && api.Submit && api.Retire && api.Reset && api.GetStatistics && api.Destroy &&
                  api.GetOutputReadiness,
              "all V5 lifecycle functions");
        const bool endpointConsumer = (api.supported_depth_capabilities & VDXRFG_DEPTH_ENDPOINT_PREFERENCE) != 0;
        const bool opacityReset = argc == 3 && std::string(argv[2]) == "gpu-opacity-reset";
        if (!opacityReset)
            check(((api.supported_depth_capabilities & depthOpacityReset) != 0) == endpointConsumer,
                  "opacity reset advertised exactly when endpoint consumer is compiled");
        check(((api.supported_create_flags & VDXRFG_CREATE_COLOR_OPAQUE) != 0) == endpointConsumer,
              "V5 opaque-color flag advertised exactly when endpoint consumer is compiled");
        // Previous interfaces must not advertise or accept the additive V5 bit.
        VDXRFG_ApiV2 v2{};
        v2.struct_size = sizeof(v2);
        v2.api_version = VDXRFG_API_VERSION_2;
        VDXRFG_ApiV3 v3{};
        v3.struct_size = sizeof(v3);
        v3.api_version = VDXRFG_API_VERSION_3;
        VDXRFG_ApiV4 v4{};
        v4.struct_size = sizeof(v4);
        v4.api_version = VDXRFG_API_VERSION_4;
        const auto getV2 = reinterpret_cast<HRESULT(WINAPI*)(VDXRFG_ApiV2*)>(GetProcAddress(module, "VDXRFG_GetApiV2"));
        const auto getV3 = reinterpret_cast<HRESULT(WINAPI*)(VDXRFG_ApiV3*)>(GetProcAddress(module, "VDXRFG_GetApiV3"));
        const auto getV4 = reinterpret_cast<HRESULT(WINAPI*)(VDXRFG_ApiV4*)>(GetProcAddress(module, "VDXRFG_GetApiV4"));
        check(getV2 && getV3 && getV4, "legacy API exports retained");
        success(getV2(&v2), "negotiate V2");
        success(getV3(&v3), "negotiate V3");
        success(getV4(&v4), "negotiate V4");
        check(!((v2.supported_create_flags | v3.supported_create_flags | v4.supported_create_flags) &
                VDXRFG_CREATE_COLOR_OPAQUE),
              "opaque flag never advertised by legacy APIs");
        VDXRFG_CreateInfoV2 old2{};
        old2.struct_size = sizeof(old2);
        old2.api_version = VDXRFG_API_VERSION_2;
        old2.flags = VDXRFG_CREATE_COLOR_OPAQUE;
        old2.base.struct_size = sizeof(old2.base);
        old2.base.api_version = VDXRFG_API_VERSION;
        old2.base.device = old2.base.queue = old2.base.input = reinterpret_cast<void*>(1);
        old2.base.width = width;
        old2.base.height = height;
        old2.base.multiplier = 2;
        old2.base.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        VDXRFG_CreateInfoV3 old3{};
        old3.struct_size = sizeof(old3);
        old3.api_version = VDXRFG_API_VERSION_3;
        old3.flags = VDXRFG_CREATE_COLOR_OPAQUE;
        old3.device = old3.queue = old3.input = reinterpret_cast<void*>(1);
        old3.width = width;
        old3.height = height;
        old3.multiplier = 2;
        old3.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        void* legacyContext = reinterpret_cast<void*>(1);
        check(v2.Create(&old2, &legacyContext) == VDXRFG_E_REJECTED && !legacyContext, "V2 rejects opaque bit");
        legacyContext = reinterpret_cast<void*>(1);
        check(v3.Create(&old3, &legacyContext) == VDXRFG_E_REJECTED && !legacyContext, "V3 rejects opaque bit");
        legacyContext = reinterpret_cast<void*>(1);
        check(v4.Create(&old3, &legacyContext) == VDXRFG_E_REJECTED && !legacyContext, "V4 rejects opaque bit");
        check(getApi(nullptr) == VDXRFG_E_REJECTED, "null V5 negotiation");
        auto malformed = api;
        --malformed.struct_size;
        check(getApi(&malformed) == VDXRFG_E_REJECTED, "short V5 negotiation");
        malformed = api;
        malformed.api_version = 4;
        check(getApi(&malformed) == VDXRFG_E_REJECTED, "wrong V5 version");
        VDXRFG_CreateInfoV5 create{};
        create.struct_size = sizeof(create);
        create.api_version = VDXRFG_API_VERSION_5;
        create.base.struct_size = sizeof(create.base);
        create.base.api_version = VDXRFG_API_VERSION_3;
        void* context = reinterpret_cast<void*>(1);
        check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context, "invalid V5 clears context");
        // This unsupported backend combination must reject before dereferencing
        // any device/guide pointer or initializing the GPU.
        create.guide_input = reinterpret_cast<void*>(1);
        create.base.flags = VDXRFG_CREATE_FIDELITYFX_FULL;
        check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context, "FFX guide rejects before device work");
        // Complete the scalar envelope so a missing flag guard cannot pass
        // accidentally through the generic null-device check. These sentinel
        // pointers must never be dereferenced by unsupported requests.
        create.base.device = create.base.queue = create.base.input = reinterpret_cast<void*>(1);
        create.base.width = width;
        create.base.height = height;
        create.base.multiplier = 2;
        create.base.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        create.base.flags |= VDXRFG_CREATE_COLOR_OPAQUE;
        check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context,
              "FFX opaque guide rejects before device work");
        create.guide_input = nullptr;
        create.base.flags = VDXRFG_CREATE_COLOR_OPAQUE;
        check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context, "opaque flag requires guide input");
        create.guide_input = reinterpret_cast<void*>(1);
        create.base.flags = VDXRFG_CREATE_COLOR_OPAQUE | 32u;
        check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context, "unknown V5 flags reject");
        if (!endpointConsumer) {
            create.base.flags = VDXRFG_CREATE_COLOR_OPAQUE;
            check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context,
                  "transport-only build rejects opaque request before device work");
        }
        VDXRFG_SubmitInfoV5 info{};
        info.struct_size = sizeof(info);
        info.api_version = VDXRFG_API_VERSION_5;
        info.base.struct_size = sizeof(info.base);
        info.base.api_version = VDXRFG_API_VERSION_3;
        VDXRFG_TicketV1 ticket{};
        ticket.token = 99;
        check(api.Submit(nullptr, &info, &ticket) == VDXRFG_E_REJECTED && !ticket.token, "invalid V5 clears ticket");
        if (argc == 2) {
            FreeLibrary(module);
            std::puts("PASS: V5 additive negotiation and pre-GPU guards");
            return 0;
        }

        ComPtr<ID3D12Debug1> validation;
        if (std::string(argv[2]) == "gpu-gbv") {
            success(D3D12GetDebugInterface(IID_PPV_ARGS(&validation)), "required GPU validation interface");
            validation->EnableDebugLayer();
            validation->SetEnableGPUBasedValidation(TRUE);
            validation->SetEnableSynchronizedCommandQueueValidation(TRUE);
            std::puts("GPU-based and synchronized-queue validation required and enabled");
        }
        Gpu gpu;
        const bool opaque = std::string(argv[2]) == "gpu-opaque" || opacityReset;
        check(!opaque || endpointConsumer, "opaque lifecycle requires advertised endpoint consumer");
        auto input = gpu.texture();
        std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> outputs;
        create.guide_input = nullptr;
        create.base.flags = opaque ? VDXRFG_CREATE_COLOR_OPAQUE : 0;
        create.base.device = gpu.device.Get();
        create.base.queue = gpu.queue.Get();
        create.base.input = input.Get();
        create.base.width = width;
        create.base.height = height;
        create.base.multiplier = 2;
        create.base.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned role : {0u, 2u}) {
                outputs[bank][role] = gpu.texture();
                create.base.outputs[bank][role] = outputs[bank][role].Get();
            }
        if (opacityReset) {
            // Real, otherwise valid contexts prove Reset rejects unsupported
            // modes instead of falling through an invalid/null-handle guard.
            for (uint32_t backend : {0u, VDXRFG_CREATE_FIDELITYFX_FULL}) {
                create.base.flags = backend;
                success(api.Create(&create, &context), "create no-guide reset control");
                check(api.Reset(context, resetColorOpaque) == VDXRFG_E_REJECTED &&
                          api.Reset(context, resetColorAlpha) == VDXRFG_E_REJECTED,
                      "V5 no-guide NVIDIA/FFX context rejects opacity reset");
                success(api.Reset(context, 1), "legacy reason remains valid without guide");
                success(api.Destroy(context), "destroy no-guide reset control");
            }
            create.base.flags = 0;
            success(v4.Create(&create.base, &context), "create legacy reset control");
            check(api.Reset(context, resetColorOpaque) == VDXRFG_E_REJECTED &&
                      api.Reset(context, resetColorAlpha) == VDXRFG_E_REJECTED,
                  "V5 reset rejects opacity changes on legacy context");
            success(v4.Destroy(context), "destroy legacy reset control");
            create.base.flags = VDXRFG_CREATE_COLOR_OPAQUE;
        }
        if (opaque)
            check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context,
                  "otherwise valid GPU context rejects opaque mode without guide");
        create.guide_input = input.Get();
        check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context, "color cannot alias guide");
        create.guide_input = outputs[0][0].Get();
        check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context, "non-R32 guide rejects");
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto desc = input->GetDesc();
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> guide;
        success(gpu.device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&guide)),
                "create canonical atlas");
        create.guide_input = guide.Get();
        success(api.Create(&create, &context), "create V5 guide context");
        ProbeFault probeFault(gpu.device.Get(), argv[2]);
        info.base.input = input.Get();
        info.base.display_period_ns = 10000000;
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& view = info.base.views[eye];
            view.orientation[3] = 1;
            view.fov[0] = view.fov[3] = -.8f;
            view.fov[1] = view.fov[2] = .8f;
            view.rect = {eye * eyeWidth, 0, eyeWidth, height};
        }
        // The existing transfer helper copies four bytes per pixel, also R32.
        std::vector<uint8_t> depth(width * height * sizeof(float));
        const auto uploadGuide = [&](uint64_t serial) {
            for (unsigned eye = 0; eye < 2; ++eye)
                for (UINT y = 0; y < height; ++y)
                    for (UINT x = 0; x < eyeWidth; ++x) {
                        uint32_t bits{};
                        const float tag = static_cast<float>(serial * 10 + eye) + .25f;
                        switch (x % 5) {
                        case 0:
                            std::memcpy(&bits, &tag, sizeof(bits));
                            break;
                        case 1:
                            bits = 0x00000000u;
                            break; // Exact positive zero: infinite distance.
                        case 2:
                            bits = 0xbf800000u;
                            break; // Unavailable sample, -1.
                        case 3:
                            bits = 0x7fc00000u;
                            break; // Quiet NaN: unavailable/fallback.
                        case 4:
                            bits = 0x7f800000u;
                            break; // Positive infinity: unavailable/fallback.
                        }
                        const size_t offset = (size_t(y) * width + eye * eyeWidth + x) * sizeof(float);
                        std::memcpy(depth.data() + offset, &bits, sizeof(bits));
                    }
            gpu.transfer(guide.Get(), &depth);
        };
        // Expected receiver-side classification for the first compatible A/B
        // pair. This is input evidence, not an assertion of GPU consumption.
        constexpr UINT probeRows = 32;
        static_assert(height >= probeRows);
        UINT expectedCounts[5]{};
        for (UINT x = 0; x < eyeWidth; ++x)
            expectedCounts[x % 5] += probeRows;
        std::printf("GUIDE_EXPECTED {\"schema\":1,\"width\":%u,\"height\":%u,\"eye_width\":%u,"
                    "\"probe_rows\":%u,\"stripe_axis\":\"eye_local_x_mod_5\","
                    "\"class_order\":[\"finite_positive\",\"zero\",\"minus_one\",\"nan\",\"positive_infinity\"],"
                    "\"counts_per_eye\":[%u,%u,%u,%u,%u],\"epoch\":1,\"a_serial\":2,\"b_serial\":3,"
                    "\"a_finite_min_max\":[[20.25,20.25],[21.25,21.25]],"
                    "\"b_finite_min_max\":[[30.25,30.25],[31.25,31.25]]}\n",
                    width,
                    height,
                    eyeWidth,
                    probeRows,
                    expectedCounts[0],
                    expectedCounts[1],
                    expectedCounts[2],
                    expectedCounts[3],
                    expectedCounts[4]);
        auto pixels = scene(0);
        if (opaque) {
            // Compositionally opaque does not mean stored alpha is one.
            // Shader regression separately verifies identical RGB decisions
            // and preserved output alpha; this exercises the real API lifecycle.
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                    pixels[(size_t(y) * width + x) * 4 + 3] = x < eyeWidth ? 0 : 128;
            std::puts("OPAQUE_CONTEXT requested=1 stored_alpha_left=0 stored_alpha_right=128");
        }
        gpu.transfer(input.Get(), &pixels);
        const auto settle = [&](const VDXRFG_TicketV1& t) {
            gpu.wait(static_cast<ID3D12Fence*>(t.output_ready_fence), t.output_ready_value);
            success(api.Retire(context, t.token), "retire depth transaction");
        };
        uint64_t previousSerial = 0;
        const auto capture = [&](uint64_t serial, bool valid, uint64_t epoch, bool retire = true) {
            // Every prior capture has completed and retired before the shared
            // caller atlas is overwritten. Distinct A/B tags expose history
            // aliasing without fabricating module-private state or tickets.
            if (valid)
                uploadGuide(serial);
            info.base.serial = serial;
            info.base.display_time_ns = static_cast<int64_t>(serial) * 1000000000;
            info.base.target_time_ns[0] = static_cast<int64_t>(serial + previousSerial) * 500000000;
            info.base.fractions[0] = .5f;
            info.guide_mask = valid ? 3 : 0;
            info.guide_serial = valid ? serial : 0;
            info.guide_epoch = valid ? epoch : 0;
            info.guide_encoding = valid ? VDXRFG_GUIDE_ENCODING_INVERSE_Z : 0;
            VDXRFG_TicketV1 t{};
            success(api.Submit(context, &info, &t), "V5 lifecycle capture");
            check(t.current_serial == serial, "guide publication matches color serial");
            check(t.previous_serial == previousSerial && (t.prime != 0) == (previousSerial == 0),
                  "capture preserves lineage or freshly primes after successful reset");
            if (retire)
                settle(t);
            previousSerial = serial;
            return t;
        };
        if (opacityReset) {
            const auto prime = capture(1, true, 1);
            const auto held = capture(2, true, 1, false);
            gpu.wait(static_cast<ID3D12Fence*>(held.output_ready_fence), held.output_ready_value);
            check(api.Reset(context, resetColorAlpha) == VDXRFG_E_BUSY,
                  "opacity reset stays BUSY while completed ticket is host-owned");
            for (uint32_t invalid : {2u, 0x400u, resetColorOpaque | resetColorAlpha, resetColorAlpha | 2u})
                check(api.Reset(context, invalid) == VDXRFG_E_REJECTED,
                      "invalid reset flags reject before live-ticket state checks");
            settle(held);
            VDXRFG_ApiV1 v1{};
            v1.struct_size = sizeof(v1);
            v1.api_version = VDXRFG_API_VERSION;
            const auto getV1 = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
            check(getV1 != nullptr, "V1 export for reset compatibility");
            success(getV1(&v1), "negotiate V1 reset compatibility");
            for (const auto legacyReset : {v1.Reset, v2.Reset, v3.Reset, v4.Reset})
                check(legacyReset(context, resetColorOpaque) == VDXRFG_E_REJECTED &&
                          legacyReset(context, resetColorAlpha) == VDXRFG_E_REJECTED,
                      "V1-V4 Reset entrypoints reject opacity flags");
            const auto unchanged = capture(3, true, 1);
            check(unchanged.generation == prime.generation,
                  "BUSY and rejected resets leave generation and pair history unchanged");
            success(api.Reset(context, resetColorAlpha | 1u), "switch opaque context to alpha mode");
            previousSerial = 0;
            const auto alphaPrime = capture(1, true, 2);
            check(alphaPrime.generation == prime.generation + 1, "successful opacity reset advances generation once");
            capture(2, true, 2);
            success(api.Reset(context, 0), "legacy reset preserves alpha mode");
            previousSerial = 0;
            capture(1, true, 3);
            capture(2, true, 3);
            success(api.Reset(context, resetColorOpaque), "switch alpha context back to opaque mode");
            previousSerial = 0;
            capture(1, true, 4);
            capture(2, true, 4);
            check((api.supported_depth_capabilities & depthOpacityReset) != 0,
                  "working opacity reset is explicitly advertised");
            success(api.Destroy(context), "destroy opacity reset context");
            gpu.noErrors();
            FreeLibrary(module);
            std::puts("PASS: V5 opacity reset validation, host-held BUSY, unchanged lineage and fresh primes");
            return 0;
        }
        info.base.serial = 1;
        info.base.display_time_ns = 1000000000;
        info.guide_mask = 1;
        ticket.token = 99;
        check(api.Submit(context, &info, &ticket) == VDXRFG_E_REJECTED && !ticket.token, "partial guide rejected");
        info.guide_mask = 0;
        info.guide_epoch = 1;
        check(api.Submit(context, &info, &ticket) == VDXRFG_E_REJECTED && !ticket.token,
              "no-guide payload must be zero");
        capture(1, false, 0);
        capture(2, true, 1); // Valid B survives missing A.
        capture(3, true, 1); // First compatible pair.
        capture(4, false, 0);
        capture(5, true, 1);
        capture(6, true, 2); // New epoch primes a new pair.
        capture(7, true, 2);
        capture(9, true, 2); // A serial gap clears compatibility, keeps B.
        capture(10, true, 2);
        success(api.Reset(context, 0), "reset clears guide lineage");
        previousSerial = 0;
        capture(1, true, 3);
        capture(2, true, 3);
        success(api.Destroy(context), "destroy V5 guide context");
        probeFault.finish();
        gpu.noErrors();
        FreeLibrary(module);
        std::puts("PASS: V5 canonical resource rejection and guide lifecycle");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
