#pragma once
// Private diagnostic ABI. Never embedded in a public V5 reserved field.
#include "vdxr_fg_api.h"
#include "output_lineage_core.hpp"
#include <d3d11_4.h>
#include <d3d12.h>
#include <cstdint>
#include <intrin.h>
#include <string>
#include <cstring>
#include <sstream>
namespace vdxrfg::pair_capture {
    struct Entry {
        uint64_t serial{}, qpc{}, leaseQpc{};
        int64_t displayTime{};
        DWORD thread{};
        USHORT count{};
        void* hostAddress{};
        void* addresses[48]{};
        void* producerDevice{};
        void* producerContext{};
    };
    inline thread_local Entry currentEntry{};
    inline bool debugRequested() noexcept {
        wchar_t target[32]{}, nonce[64]{}, debug[8]{};
        return GetEnvironmentVariableW(L"VDXR_PAIR_NONCE", nonce, 64) == 32 &&
               GetEnvironmentVariableW(L"VDXR_PAIR_PID", target, 32) &&
               _wcstoui64(target, nullptr, 10) == GetCurrentProcessId() &&
               GetEnvironmentVariableW(L"VDXR_PAIR_CALIBRATION_DEBUG", debug, 8) == 1 && debug[0] == L'1';
    }
    inline void entry(uint64_t serial, int64_t displayTime) noexcept {
        currentEntry = {};
        wchar_t target[32]{}, arm[4096]{}, nonce[64]{};
        const DWORD nonceSize = GetEnvironmentVariableW(L"VDXR_PAIR_NONCE", nonce, 64),
                    targetSize = GetEnvironmentVariableW(L"VDXR_PAIR_PID", target, 32);
        if (nonceSize != 32 || targetSize && _wcstoui64(target, nullptr, 10) != GetCurrentProcessId() ||
            !GetEnvironmentVariableW(L"VDXR_PAIR_ARM_FILE", arm, 4096) ||
            GetFileAttributesW(arm) == INVALID_FILE_ATTRIBUTES)
            return;
        std::string expected;
        for (DWORD i = 0; i < nonceSize; ++i) {
            const auto c = nonce[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
                return;
            expected += static_cast<char>(c);
        }
        const std::string requestNonce = expected;
        HANDLE file = CreateFileW(arm,
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        char contents[512]{};
        DWORD read{};
        const bool readOk = ReadFile(file, contents, sizeof(contents), &read, nullptr) != FALSE;
        CloseHandle(file);
        if (!readOk || read == sizeof(contents))
            return;
        std::istringstream armRecord(std::string(contents, read));
        std::string recordNonce, sessionNonce, extra;
        uint64_t pid{}, creation{}, context{}, generation{};
        if (!(armRecord >> recordNonce >> pid >> creation >> sessionNonce >> context >> generation) ||
            armRecord >> extra || recordNonce != requestNonce || pid != GetCurrentProcessId() ||
            sessionNonce.size() != 32 || !context || !generation)
            return;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return;
        const uint64_t actualCreation = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (creation != actualCreation)
            return;
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        currentEntry.serial = serial;
        currentEntry.displayTime = displayTime;
        currentEntry.thread = GetCurrentThreadId();
        currentEntry.qpc = qpc.QuadPart;
        currentEntry.hostAddress = _ReturnAddress();
        currentEntry.count = CaptureStackBackTrace(0, 48, currentEntry.addresses, nullptr);
    }
    inline void lease(void* device, void* context) noexcept {
        if (!currentEntry.count)
            return;
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        currentEntry.leaseQpc = qpc.QuadPart;
        currentEntry.producerDevice = device;
        currentEntry.producerContext = context;
    }
    struct Eye {
        ID3D11Texture2D* source{};
        D3D11_TEXTURE2D_DESC description{};
        VDXRFG_RectV1 colour{}, crop{}, depth{};
        VDXRFG_ViewV1 view{};
        uint32_t releasedIndex{}, arraySlice{};
        float minDepth{}, maxDepth{}, nearZ{}, farZ{};
    };
    struct OutputRecord {
        uint64_t callId{}, hostGeneration{}, ownerGeneration{}, copyId{}, destinationRetentionEpoch{};
        uint64_t roleValueBefore{}, consumerValue{}, callQpc{}, copyQpc{}, signalQpc{}, flushQpc{}, returnQpc{};
        uintptr_t hostContext11{}, source11{}, source12{}, destination11{}, fence11{}, retainedDestination11Before{},
            source11Eyes[2]{};
        uint32_t role{};
        const char* sourcePath{"none"};
        bool prepared{}, preparedReferenceValid{}, copyCalled{}, signalCalled{}, flushCalled{}, descriptionObserved{};
        HRESULT signalResult{E_PENDING};
        D3D11_TEXTURE2D_DESC destinationDescription{};
    };
    struct Host {
        uint32_t size{sizeof(Host)}, version{2}, operation{};
        void* context{};
        ID3D11DeviceContext4* producer{};
        uint64_t serial{}, guideEpoch{}, producerValue{}, priorConsumedValue{};
        int64_t displayTime{};
        ID3D11Fence* producerFence{};
        ID3D11Fence* priorConsumedFence{};
        ID3D12Fence* importedProducerFence{};
        ID3D11Texture2D* canonical{};
        Eye eyes[2]{};
        Entry entry{};
        VDXRFG_TicketV1 ticket{};
        HRESULT originalResult{E_PENDING};
        bool nrEnabled{}, hasGuide{};
        const char* eventName{};
        void* eventObject{};
        void* eventOther{};
        uint64_t eventValue{};
        D3D11_SHADER_RESOURCE_VIEW_DESC normalizerSrv{};
        void* normalizerTexture{};
        ID3D11Texture2D* colourSources[2]{};
        VDXRFG_ViewV1 colourViews[2]{};
        OutputRecord output{};
    };
    using HostHook = void(WINAPI*)(const Host*) noexcept;
    inline void emit(HostHook hook,
                     const char* name,
                     void* context,
                     uint64_t serial,
                     void* object,
                     void* other,
                     uint64_t value,
                     HRESULT result = S_OK) noexcept {
        if (!hook)
            return;
        Host packet;
        packet.operation = 4;
        packet.context = context;
        packet.serial = serial;
        packet.eventName = name;
        packet.eventObject = object;
        packet.eventOther = other;
        packet.eventValue = value;
        packet.originalResult = result;
        packet.entry = currentEntry;
        hook(&packet);
    }
    inline void
    emitTicket(HostHook hook, const char* name, void* context, const VDXRFG_TicketV1& ticket, HRESULT result) noexcept {
        if (!hook)
            return;
        Host packet;
        packet.operation = 4;
        packet.context = context;
        packet.serial = ticket.current_serial;
        packet.eventName = name;
        packet.ticket = ticket;
        packet.originalResult = result;
        packet.entry = currentEntry;
        hook(&packet);
    }
    void prepare(void*) noexcept;
    void bind(void*, ID3D12Device*, ID3D12CommandQueue*, ID3D12Resource* const outputs[8], uint64_t) noexcept;
    void configure(void*,
                   ID3D12Resource*,
                   ID3D12Resource*,
                   uint64_t,
                   uint64_t,
                   uint64_t,
                   const D3D12_SHADER_RESOURCE_VIEW_DESC&,
                   const void*) noexcept;
    void sources(void*,
                 ID3D12Resource*,
                 ID3D12Resource*,
                 const D3D12_SHADER_RESOURCE_VIEW_DESC&,
                 ID3D12DescriptorHeap*,
                 uint32_t) noexcept;
    void draw(void*,
              ID3D12GraphicsCommandList*,
              ID3D12PipelineState*,
              uint32_t,
              uint32_t,
              uint32_t,
              const void*,
              const void*,
              size_t,
              uint32_t,
              ID3D12Resource*,
              const unsigned char*,
              size_t) noexcept;
    void depthCbv(void*,
                  ID3D12Resource*,
                  ID3D12DescriptorHeap*,
                  UINT,
                  UINT,
                  UINT,
                  UINT,
                  const D3D12_CONSTANT_BUFFER_VIEW_DESC&,
                  D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
    void rootTable(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) noexcept;
    bool holdRetire(void*, uint64_t) noexcept;
    void abort(void*, const char*) noexcept;
    void queued(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
    void signalled(const char*, ID3D12CommandQueue*, ID3D12Fence*, uint64_t, HRESULT) noexcept;
    void rtv(ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC&, D3D12_CPU_DESCRIPTOR_HANDLE) noexcept;
    void targets(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*) noexcept;
    void viewport(ID3D12GraphicsCommandList*, const D3D12_VIEWPORT&, const D3D12_RECT&) noexcept;
    void ticket(void*, const VDXRFG_TicketV1&) noexcept;
    void history(ID3D12Resource*, ID3D12Resource*, ID3D12GraphicsCommandList*, uint64_t, uint32_t) noexcept;
    void barriers(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*) noexcept;
    void generation(void*, uint64_t) noexcept;
} // namespace vdxrfg::pair_capture
