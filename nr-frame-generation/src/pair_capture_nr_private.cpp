#include "pair_capture_nr_fg.hpp"
#include "pair_capture_nr_private.hpp"
#include "pair_capture_nr_budget.hpp"
#include "../../virtualdesktop-openxr/PairCaptureNrPrivateApi.h"
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace vdxrfg::pair_capture::nr {
    namespace {
        struct Request {
            bool selected{}, valid{};
            RequestBinding binding{};
            std::wstring nonce, arm, output;
        };
        struct Runtime {
            std::mutex mutex;
            NrReadyV1 ready{};
            uint64_t sequence{}, lifetime{}, preparedContext{};
            bool boundReceiver{}, terminal{};
            BudgetProvider budgetProvider{};
            NrFgReadbackBudgetV1 budget{};
            bool budgetValid{}, budgetQuery{};
            std::array<FgDrawObservation, 2> draws{};
            uint32_t drawMask{};
        } runtime;
        static_assert(sizeof(Runtime) <= 8192, "private scalar state exceeds its prepaid scratch allowance");
        Receiver custody;
        std::wstring env(const wchar_t* name) {
            std::array<wchar_t, 4096> value{};
            const auto n = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
            return n && n < value.size() ? std::wstring(value.data(), n) : std::wstring{};
        }
        bool decimal(const std::wstring& text, uint64_t& value) noexcept {
            value = 0;
            if (text.empty())
                return false;
            for (auto c : text) {
                if (c < L'0' || c > L'9' || value > (UINT64_MAX - static_cast<uint64_t>(c - L'0')) / 10)
                    return false;
                value = value * 10 + static_cast<uint64_t>(c - L'0');
            }
            return value && value != UINT64_MAX;
        }
        bool nonce(const std::wstring& text, uint64_t words[2]) noexcept {
            if (text.size() != 32)
                return false;
            words[0] = words[1] = 0;
            for (size_t i = 0; i < 32; ++i) {
                const auto c = text[i];
                const unsigned digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : 16;
                if (digit == 16)
                    return false;
                words[i / 16] = (words[i / 16] << 4) | digit;
            }
            return (words[0] | words[1]) != 0;
        }
        Request loadRequest() {
            Request r;
            const auto abi = env(L"VDXR_PAIR_NR_ABI");
            r.selected = !abi.empty();
            if (abi != L"1")
                return r;
            const auto mode = env(L"VDXR_PAIR_NR_MODE");
            r.binding.mode = mode == L"native100" ? NR_NATIVE100 : mode == L"spatial85" ? NR_SPATIAL85 : 0;
            r.nonce = env(L"VDXR_PAIR_NONCE");
            r.arm = env(L"VDXR_PAIR_ARM_FILE");
            r.output = env(L"VDXR_PAIR_OUTPUT");
            if (!r.binding.mode || !nonce(r.nonce, r.binding.nonce) ||
                !decimal(env(L"VDXR_PAIR_ATTEMPT"), r.binding.attempt) ||
                !decimal(env(L"VDXR_PAIR_PID"), r.binding.pid) || r.binding.pid != GetCurrentProcessId() ||
                !decimal(env(L"VDXR_PAIR_MIN_SERIAL"), r.binding.minimumSerial) ||
                !decimal(env(L"VDXR_PAIR_MIN_TIME"), r.binding.minimumTime) ||
                !decimal(env(L"VDXR_PAIR_MAX_BYTES"), r.binding.budget) || r.binding.budget != 134217728 ||
                env(L"VDXR_PAIR_SESSION").empty() || !std::filesystem::path(r.arm).is_absolute() ||
                !std::filesystem::path(r.output).is_absolute() || std::filesystem::exists(r.output))
                return r;
            FILETIME created{}, end{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &end, &kernel, &user))
                return r;
            r.binding.creation = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            r.valid = true;
            return r;
        }
        const Request& request() {
            static const Request value = loadRequest();
            return value;
        }
        std::string hexNonce(uint64_t lo, uint64_t hi) {
            std::ostringstream s;
            s << std::hex << std::setfill('0') << std::setw(16) << lo << std::setw(16) << hi;
            return s.str();
        }
        bool readyFile(const NrReadyV1& r) {
            auto path = std::filesystem::path(request().arm);
            path += L".ready." + std::to_wstring(r.pid) + L"." + std::to_wstring(r.creationFiletime) + L".json";
            auto partial = path;
            partial += L".partial";
            std::ofstream stream(partial, std::ios::binary);
            stream << "{\"status\":\"nr-graphics-context-ready-requires-host-warm-and-explicit-arm\",\"nr_abi\":1,\"nr_"
                      "mode\":"
                   << r.mode << ",\"attempt\":" << r.attempt << ",\"nonce\":\"" << hexNonce(r.nonceLo, r.nonceHi)
                   << "\",\"pid\":" << r.pid << ",\"creation_filetime\":" << r.creationFiletime
                   << ",\"session_nonce\":\"" << hexNonce(r.sessionNonceLo, r.sessionNonceHi)
                   << "\",\"context\":" << r.fgContextAddress << ",\"context_lifetime\":" << r.fgContextLifetimeEpoch
                   << ",\"generation\":" << r.fgHistoryGeneration << ",\"device\":" << r.deviceAddress
                   << ",\"queue\":" << r.queueAddress << ",\"full_width\":" << r.outputWidth
                   << ",\"height\":" << r.outputHeight << ",\"format\":" << r.format << "}\n";
            stream.close();
            return stream.good() &&
                   MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        void removeReadyFile() noexcept {
            try {
                const auto& r = request();
                if (!r.valid)
                    return;
                auto path = std::filesystem::path(r.arm);
                path +=
                    L".ready." + std::to_wstring(r.binding.pid) + L"." + std::to_wstring(r.binding.creation) + L".json";
                DeleteFileW(path.c_str()); // Live exports remain authoritative if removal fails.
            } catch (...) {
            }
        }
        bool randomSession(NrReadyV1& r) noexcept {
            uint64_t words[2]{};
            if (BCryptGenRandom(
                    nullptr, reinterpret_cast<PUCHAR>(words), sizeof words, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
                !(words[0] | words[1]))
                return false;
            r.sessionNonceLo = words[0];
            r.sessionNonceHi = words[1];
            return true;
        }
        void failRuntime() noexcept {
            std::lock_guard lock(runtime.mutex);
            runtime.terminal = true;
            runtime.ready.status = 0;
            runtime.ready.observedFlags = 0;
        }
        bool armReceiver() {
            NrReadyV1 ready{};
            {
                std::lock_guard lock(runtime.mutex);
                if (runtime.terminal || runtime.budgetQuery || !runtime.budgetValid)
                    return false;
                if (runtime.boundReceiver)
                    return true;
                ready = runtime.ready;
                if (!nr_fg_budget_matches_ready(&runtime.budget, &ready))
                    return false;
            }
            if (ready.status != 1)
                return false;
            const auto& r = request();
            std::ifstream file(r.arm, std::ios::binary);
            std::array<char, 512> bytes{};
            file.read(bytes.data(), bytes.size());
            if (!file.eof())
                return false;
            const auto expected =
                hexNonce(ready.nonceLo, ready.nonceHi) + " " + std::to_string(ready.pid) + " " +
                std::to_string(ready.creationFiletime) + " " + hexNonce(ready.sessionNonceLo, ready.sessionNonceHi) +
                " " + std::to_string(ready.fgContextAddress) + " " + std::to_string(ready.fgHistoryGeneration) + "\n";
            if (std::string(bytes.data(), static_cast<size_t>(file.gcount())) != expected)
                return false;
            auto binding = r.binding;
            binding.session[0] = ready.sessionNonceLo;
            binding.session[1] = ready.sessionNonceHi;
            binding.context = ready.fgContextAddress;
            binding.lifetime = ready.fgContextLifetimeEpoch;
            binding.history = ready.fgHistoryGeneration;
            if (!custody.bind(binding))
                return false;
            {
                std::lock_guard lock(runtime.mutex);
                if (runtime.ready.bindSequence != ready.bindSequence || runtime.terminal)
                    return false;
                runtime.boundReceiver = true;
            }
            return true;
        }
    } // namespace
    Receiver& receiver() noexcept {
        return custody;
    }
    bool requested() noexcept {
        try {
            return request().selected;
        } catch (...) {
            return true;
        }
    }
    void prepare(void* context) noexcept {
        try {
            if (!request().valid)
                return;
            {
                std::lock_guard lock(runtime.mutex);
                if (runtime.terminal || runtime.boundReceiver || runtime.budgetQuery || !context)
                    return;
                if (runtime.lifetime == UINT64_MAX || runtime.sequence == UINT64_MAX) {
                    runtime.terminal = true;
                    runtime.ready = {};
                    return;
                }
                runtime.preparedContext = reinterpret_cast<uint64_t>(context);
                ++runtime.lifetime;
                runtime.budgetProvider = nullptr;
                runtime.budgetValid = false;
                runtime.budget = {};
                runtime.ready = {};
                runtime.ready.prepareSequence = ++runtime.sequence;
            }
            removeReadyFile();
            fgPrepare(context);
        } catch (...) {
            failRuntime();
        }
    }
    void bind(void* context,
              ID3D12Device* device,
              ID3D12CommandQueue* queue,
              ID3D12Resource* const outputs[8],
              uint64_t history) noexcept {
        try {
            const auto& r = request();
            if (!r.valid || !device || !queue || !outputs || !history)
                return;
            NrReadyV1 ready{};
            {
                std::lock_guard lock(runtime.mutex);
                if (runtime.terminal || runtime.boundReceiver ||
                    runtime.preparedContext != reinterpret_cast<uint64_t>(context))
                    return;
                ready.fgContextLifetimeEpoch = runtime.lifetime;
                ready.prepareSequence = runtime.ready.prepareSequence;
            }
            D3D12_RESOURCE_DESC description{};
            if (!outputs[0] || !outputs[4])
                return;
            for (unsigned i = 0; i < 8; ++i) {
                // The original create path requires null in inapplicable multiplier
                // slots and optional host-owned real-output slots. Do not invent refs.
                if (!outputs[i])
                    continue;
                const auto d = outputs[i]->GetDesc();
                if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 || d.DepthOrArraySize != 1 ||
                    d.SampleDesc.Count != 1 || !d.Width || !d.Height ||
                    !nr_capture_view_family_compatible(d.Format, 28))
                    return;
                if (i &&
                    (d.Width != description.Width || d.Height != description.Height || d.Format != description.Format))
                    return;
                if (!i)
                    description = d;
            }
            HMODULE module{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&VDXRFG_PrivatePairNRV1),
                                    &module) ||
                !randomSession(ready))
                return;
            ready.size = sizeof ready;
            ready.version = 1;
            ready.status = ready.observedFlags = 1;
            ready.attempt = r.binding.attempt;
            ready.nonceLo = r.binding.nonce[0];
            ready.nonceHi = r.binding.nonce[1];
            ready.mode = r.binding.mode;
            ready.pid = r.binding.pid;
            ready.creationFiletime = r.binding.creation;
            ready.fgContextAddress = reinterpret_cast<uint64_t>(context);
            ready.fgHistoryGeneration = history;
            ready.deviceAddress = reinterpret_cast<uint64_t>(device);
            ready.queueAddress = reinterpret_cast<uint64_t>(queue);
            ready.outputWidth = description.Width;
            ready.outputHeight = description.Height;
            ready.format = description.Format;
            ready.moduleBase = reinterpret_cast<uint64_t>(module);
            ready.moduleLifetimeEpoch = 1;
            if (!fgBound(context, device, queue, outputs, request().output.c_str())) {
                invalidateContext(context);
                return;
            }
            {
                std::lock_guard lock(runtime.mutex);
                if (runtime.terminal || runtime.boundReceiver || runtime.lifetime != ready.fgContextLifetimeEpoch)
                    return;
                ready.bindSequence = ++runtime.sequence;
                runtime.ready = ready;
                runtime.budgetValid = false;
            }
            if (!readyFile(ready))
                failRuntime();
        } catch (...) {
            failRuntime();
        }
    }
    void generation(void* context, uint64_t history) noexcept {
        try {
            NrReadyV1 ready{};
            bool armed = false;
            {
                std::lock_guard lock(runtime.mutex);
                if (runtime.terminal || runtime.ready.status != 1 ||
                    runtime.ready.fgContextAddress != reinterpret_cast<uint64_t>(context))
                    return;
                ready = runtime.ready;
                armed = runtime.boundReceiver;
                runtime.ready.status = runtime.ready.observedFlags = 0;
                runtime.budgetValid = false;
                runtime.budget = {};
            }
            removeReadyFile();
            if (armed || !fgInvalidateBudget(context) || !history || !randomSession(ready)) {
                fgAbort(context);
                return;
            }
            ready.fgHistoryGeneration = history;
            {
                std::lock_guard lock(runtime.mutex);
                if (runtime.boundReceiver || runtime.terminal || runtime.ready.bindSequence != ready.bindSequence)
                    return;
                ready.bindSequence = ++runtime.sequence;
                runtime.ready = ready;
                runtime.budgetValid = false;
            }
            if (!readyFile(ready))
                failRuntime();
        } catch (...) {
            failRuntime();
        }
    }
    void invalidateContext(void* context) noexcept {
        {
            std::lock_guard lock(runtime.mutex);
            if (!context || runtime.preparedContext != reinterpret_cast<uint64_t>(context))
                return;
            runtime.terminal = true;
            runtime.ready = {};
            runtime.budgetProvider = nullptr;
            runtime.budgetValid = false;
            runtime.budget = {};
        }
        removeReadyFile();
    }
    void destroyed(void* context) noexcept {
        {
            std::lock_guard lock(runtime.mutex);
            if (!context || runtime.preparedContext != reinterpret_cast<uint64_t>(context))
                return;
            runtime.terminal = runtime.terminal || runtime.boundReceiver || runtime.budgetQuery;
            runtime.preparedContext = 0;
            runtime.ready = {};
            runtime.budgetProvider = nullptr;
            runtime.budgetValid = false;
            runtime.budget = {};
        }
        removeReadyFile();
        if (!fgForgetContext(context))
            fgAbort(context);
    }
    void draw(void* slot,
              ID3D12GraphicsCommandList* list,
              ID3D12PipelineState* pso,
              uint32_t eye,
              uint32_t flags,
              uint32_t second,
              const void* b1,
              const void* b0,
              size_t b0Size,
              uint32_t viewIndex,
              ID3D12Resource* b1Resource,
              const unsigned char* shader,
              size_t shaderSize) noexcept {
        fgDraw(slot, list, pso, eye, flags, second, b1, b0, b0Size, viewIndex, b1Resource, shader, shaderSize);
    }
    uint32_t readyExport(NrReadyV1* target) noexcept {
        if (!target)
            return 0;
        const bool valid = target->size == sizeof(*target) && target->version == 1;
        *target = {};
        target->size = sizeof(*target);
        target->version = 1;
        if (!valid)
            return 0;
        std::lock_guard lock(runtime.mutex);
        if (runtime.terminal || runtime.ready.status != 1)
            return 0;
        *target = runtime.ready;
        return 1;
    }
    void installBudgetProvider(void* context, BudgetProvider provider) noexcept {
        std::lock_guard lock(runtime.mutex);
        if (runtime.boundReceiver || runtime.terminal || runtime.preparedContext != reinterpret_cast<uint64_t>(context))
            return;
        runtime.budgetProvider = provider;
        runtime.budgetValid = false;
    }
    bool observeBudgetResource(ID3D12Device* device,
                               ID3D12Resource* source,
                               uint64_t index,
                               uint64_t kind,
                               uint64_t lifetime,
                               NrFgBudgetRowV1& row) noexcept {
        row = {};
        if (!device || !source)
            return false;
        const auto d = source->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.DepthOrArraySize || d.DepthOrArraySize > 2 ||
            d.MipLevels != 1 || !d.Width || d.Width > UINT32_MAX || !d.Height || d.SampleDesc.Count != 1 ||
            d.SampleDesc.Quality || d.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
            !nr_fg_budget_texture_flags(kind, static_cast<uint64_t>(d.Flags)) || (kind != 1 && kind != 2) ||
            (kind == 1 ? index > 3 : index > 7) ||
            (kind == 1 && index >= 2 ? d.Format != DXGI_FORMAT_R32_FLOAT
                                     : !nr_capture_view_family_compatible(d.Format, 28)) ||
            d.Width > UINT32_MAX / nr_capture_pixel_bytes(d.Format))
            return false;
        row.resourceAddress = reinterpret_cast<uint64_t>(source);
        row.sourceIndex = index;
        row.sourceKind = kind;
        row.ownerContextLifetimeEpoch = lifetime;
        row.lifetimeBasis = 1;
        row.subresourceCount = d.DepthOrArraySize;
        const uint64_t words[11] = {static_cast<uint64_t>(d.Dimension),
                                    d.Alignment,
                                    d.Width,
                                    d.Height,
                                    d.DepthOrArraySize,
                                    d.MipLevels,
                                    static_cast<uint64_t>(d.Format),
                                    d.SampleDesc.Count,
                                    d.SampleDesc.Quality,
                                    static_cast<uint64_t>(d.Layout),
                                    static_cast<uint64_t>(d.Flags)};
        memcpy(row.textureDescription, words, sizeof words);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
        UINT rows[2]{};
        UINT64 rowBytes[2]{}, total{};
        device->GetCopyableFootprints(&d, 0, d.DepthOrArraySize, 0, footprints, rows, rowBytes, &total);
        row.apiTotalBytes = total;
        for (unsigned i = 0; i < d.DepthOrArraySize; ++i) {
            const auto& f = footprints[i];
            auto& out = row.footprints[i];
            out = {f.Offset,
                   static_cast<uint64_t>(f.Footprint.Format),
                   f.Footprint.Width,
                   f.Footprint.Height,
                   f.Footprint.Depth,
                   f.Footprint.RowPitch,
                   rows[i],
                   rowBytes[i]};
            const uint64_t bytes = uint64_t(rows[i]) * f.Footprint.RowPitch;
            if (!bytes || f.Offset > UINT64_MAX - bytes)
                return false;
            if (f.Offset + bytes > row.payloadBound)
                row.payloadBound = f.Offset + bytes;
        }
        if (!row.payloadBound || row.payloadBound > 134217728)
            return false;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = row.payloadBound;
        buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &buffer);
        row.allocationBytes = allocation.SizeInBytes;
        row.allocationAlignment = allocation.Alignment;
        return nr_fg_budget_row(&row) != 0;
    }
    uint32_t budgetExport(NrFgReadbackBudgetV1* target) noexcept {
        if (!target)
            return 0;
        NrFgReadbackBudgetV1 result = *target;
        *target = {};
        target->size = sizeof(*target);
        target->version = 1;
        NrReadyV1 ready{};
        BudgetProvider provider{};
        {
            std::lock_guard lock(runtime.mutex);
            if (runtime.terminal || runtime.boundReceiver || runtime.budgetQuery || !runtime.budgetProvider ||
                !nr_fg_budget_request(&result, &runtime.ready))
                return 0;
            ready = runtime.ready;
            provider = runtime.budgetProvider;
            runtime.budgetQuery = true;
            runtime.budgetValid = false;
        }
        LARGE_INTEGER begin{}, end{};
        QueryPerformanceCounter(&begin);
        result.queryThread = GetCurrentThreadId();
        result.qpcBegin = static_cast<uint64_t>(begin.QuadPart);
        const bool observed = provider(reinterpret_cast<void*>(ready.fgContextAddress), ready, result);
        QueryPerformanceCounter(&end);
        result.qpcEnd = static_cast<uint64_t>(end.QuadPart);
        result.status = result.observedFlags = 1;
        result.rowCount = 5;
        std::lock_guard lock(runtime.mutex);
        runtime.budgetQuery = false;
        if (!observed || runtime.terminal || runtime.boundReceiver ||
            runtime.ready.bindSequence != ready.bindSequence || !nr_fg_budget_matches_ready(&result, &runtime.ready) ||
            !nr_validate_fg_budget(&result, sizeof result))
            return 0;
        // Only POD copy/locks below, never COM or callback under observer mutex.
        if (!fgInstallBudget(result))
            return 0;
        runtime.budget = result;
        runtime.budgetValid = true;
        *target = result;
        return 1;
    }
    uint32_t packetExport(const NrPacketV1* packet, NrAckV1* ack) noexcept {
        try {
            std::unique_lock<std::recursive_mutex> publication(fgPublicationGate(), std::defer_lock);
            if (packet && (packet->header.operation == NR_QUERY || packet->header.operation == NR_ABORT))
                publication.lock();
            // Validate the control's actual cohort before it can publish anything.
            // A rejected/foreign query and ABORT cannot trigger finalization.
            if (packet && packet->header.operation == NR_QUERY) {
                const auto before = custody.transact(packet, ack);
                if (before != NR_DEEP_OWNED)
                    return before;
                fgQuery();
                return custody.transact(packet, ack);
            }
            if (packet && packet->header.operation == NR_FRAME_TRANSFER && !armReceiver()) {
                if (ack) {
                    *ack = {};
                    ack->size = sizeof(*ack);
                    ack->version = 1;
                    ack->status = NR_REJECTED;
                }
                return NR_REJECTED;
            }
            const auto result = custody.transact(packet, ack);
            if (packet && packet->header.operation == NR_ABORT && result == NR_UNCERTAIN)
                fgAbort(reinterpret_cast<void*>(packet->identity.fgContextAddress));
            return result;
        } catch (...) {
            failRuntime();
            if (ack) {
                *ack = {};
                ack->size = sizeof(*ack);
                ack->version = 1;
                ack->status = NR_UNCERTAIN;
            }
            return NR_UNCERTAIN;
        }
    }
    uint32_t submitWithPostFx(const NrPacketV1* packet, const NrPostFxSidecarV1* facts, NrAckV1* ack) noexcept {
        try {
            if (!packet || packet->header.operation != NR_FRAME_TRANSFER || !facts || !armReceiver() ||
                !fgBudgetMatches(*packet)) {
                if (ack) {
                    *ack = {};
                    ack->size = sizeof(*ack);
                    ack->version = 1;
                    ack->status = NR_REJECTED;
                }
                return NR_REJECTED;
            }
            return custody.transact(packet, ack, facts);
        } catch (...) {
            failRuntime();
            if (ack) {
                *ack = {};
                ack->size = sizeof(*ack);
                ack->version = 1;
                ack->status = NR_UNCERTAIN;
            }
            return NR_UNCERTAIN;
        }
    }
    uint32_t sourceExport(const NrPacketV1* packet, const NrSourceWitnessV1* source) noexcept {
        try {
            if (!packet || !source || !armReceiver() || !fgBudgetMatches(*packet))
                return 0;
            const NrPacketV1 copy = *packet;
            const NrSourceWitnessV1 witness = *source;
            return custody.attachSource(copy, witness) ? 1u : 0u;
        } catch (...) {
            failRuntime();
            return 0;
        }
    }
} // namespace vdxrfg::pair_capture::nr
#pragma comment(linker, "/EXPORT:VDXRFG_PrivatePairNRReadyV1")
#pragma comment(linker, "/EXPORT:VDXRFG_PrivatePairNRBudgetV1")
extern "C" uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRBudgetV1(NrFgReadbackBudgetV1* budget) noexcept {
    return vdxrfg::pair_capture::nr::budgetExport(budget);
}
#pragma comment(linker, "/EXPORT:VDXRFG_PrivatePairNRV1")
#pragma comment(linker, "/EXPORT:VDXRFG_PrivatePairNRSourceV1")
extern "C" uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRSourceV1(const NrPacketV1* packet,
                                                                 const NrSourceWitnessV1* source) noexcept {
    return vdxrfg::pair_capture::nr::sourceExport(packet, source);
}
extern "C" uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRReadyV1(NrReadyV1* ready) noexcept {
    return vdxrfg::pair_capture::nr::readyExport(ready);
}
extern "C" uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRV1(const NrPacketV1* packet, NrAckV1* ack) noexcept {
    return vdxrfg::pair_capture::nr::packetExport(packet, ack);
}

#pragma comment(linker, "/EXPORT:VDXRFG_PrivatePairNRSubmitWithPostFxV1")
extern "C" uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRSubmitWithPostFxV1(const NrPacketV1* packet,
                                                                           const NrPostFxSidecarV1* facts,
                                                                           NrAckV1* ack) noexcept {
    return vdxrfg::pair_capture::nr::submitWithPostFx(packet, facts, ack);
}
