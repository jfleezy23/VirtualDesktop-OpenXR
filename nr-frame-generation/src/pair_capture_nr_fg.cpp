#include "pair_capture_nr_fg.hpp"
#include "pair_capture_nr_budget.hpp"
#include "pair_capture_nr_host_reservation.h"
#include "pair_capture_nr_control.h"
#include "pair_capture_nr_private.hpp"
#include "pair_capture_nr_worker.hpp"
#include "pair_capture_nr_fg_journal.hpp"
#include "pair_capture_nr_fg_ledger.hpp"
#include "pair_capture_private.hpp"
#include "pair_capture_read_state.hpp"
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <bcrypt.h>
#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include "../module-checkpoint/include/packet_extension.hpp"
namespace vdxrfg::pair_capture::nr {
    namespace {
        uint64_t address(const void* p) noexcept {
            return reinterpret_cast<uint64_t>(p);
        }
        uint64_t timestamp() noexcept {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return static_cast<uint64_t>(value.QuadPart);
        }
        struct Descriptor {
            uint64_t cpu{}, gpu{}, heap{}, resource{}, epoch{}, slot{}, increment{}, eye{}, offset{};
            uint64_t qpc{}, thread{};
            uint64_t kind{}, bytes{};
            std::array<uint64_t, 16> description{};
        };
        struct Target {
            uint64_t list{}, root{}, tables[2]{};
            UINT count{};
            D3D12_CPU_DESCRIPTOR_HANDLE handles[2]{};
            D3D12_VIEWPORT viewport{};
            D3D12_RECT scissor{};
        };
        struct Write {
            uint64_t source{}, target{}, list{}, allocator{}, recording{}, serial{}, epoch{}, hostOrigin{}, hostWrite{};
        };
        struct Image {
            D3D12_RESOURCE_DESC description{};
            std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
            std::array<UINT, 2> rows{};
            std::array<UINT64, 2> rowBytes{};
            uint64_t bytes{}, allocation{}, copyEpoch{}, copyList{}, recording{}, before{}, after{};
        };
        struct Draw {
            NrDrawV1 row{};
            std::array<uint8_t, fgConstantBytes> constants{};
            std::array<uint8_t, 32> shader{};
            std::array<Descriptor, 7> descriptors{};
            UINT descriptorCount{};
        };
        struct Graph {
            std::mutex mutex;
            std::condition_variable changed;
            FgJournal journal;
            FgLedger ledger;
            CopyOwnership ownership;
            void* context{};
            ID3D12Device* device{};
            ID3D12CommandQueue* queue{};
            std::array<ID3D12Resource*, 8> outputs{};
            std::array<ID3D12Resource*, 4> consumers{};
            std::array<D3D12_SHADER_RESOURCE_VIEW_DESC, 4> srvs{};
            ID3D12DescriptorHeap* heap{};
            UINT increment{};
            void* slot{};
            uint64_t guideA{}, guideB{}, guideEpoch{}, serialA{}, serialB{}, nextWrite{}, nextLifetime{},
                nextDescriptor{};
            std::array<uint64_t, 35> lifetimes{};
            std::array<uint64_t, 35> types{};
            std::array<uint64_t, 35> retainedAddresses{};
            std::array<Descriptor, 128> descriptors{};
            std::array<Target, 32> targets{};
            struct State {
                ID3D12Resource* resource{};
                TextureReadState read;
            };
            std::array<State, 256> states{};
            std::array<Write, 4> writes{};
            std::array<Image, 5> images{};
            std::array<Draw, 2> draws{};
            std::array<VDXRFG_TicketV1, 2> tickets{};
            VDXRFG_TicketV1 input{};
            std::array<NrPacketV1, 2> final{};
            std::array<std::array<PayloadRecord, 64>, 2> payloads{};
            std::array<Host, 16> native{};
            size_t nativeCount{};
            struct Origin {
                uint64_t serial{}, source[2]{}, target11{}, target12{}, qpc{}, producer{};
                VDXRFG_ViewV1 views[2]{};
            };
            std::array<Origin, 2> origins{};
            uint64_t written{}, fgBudget{}, selectedList{}, selectedRecording{}, selectedWrite{}, drawMask{};
            uint64_t b1Write[2]{};
            ID3D12Resource* b1{};
            struct Begin {
                uint64_t qpc{}, thread{};
            };
            std::array<Begin, 2> admissionBegin{}, retireBegin{};
            bool reserved{}, reserving{}, failed{}, copiesRecorded{}, workerStarted{}, workerFinished{}, retiredA{},
                retiredB{}, released{}, published{};
            HANDLE worker{}, waitEvents[8]{};
            std::array<wchar_t, 4096> output{};
            HRESULT info11Result{E_PENDING}, info12Result{E_PENDING};
            uint64_t info11Begin{}, info12Begin{}, nativeDependencySequence{};
            bool outputAlreadyComplete{};
            uint64_t productCompletedSequence{}, copyCompletedSequence{};
            bool journalClosed{}, publishing{};
            NrFgReadbackBudgetV1 preflight{};
        } graph;
        static_assert(sizeof(Graph) + sizeof(Receiver) + 2 * sizeof(NrPacketV1) + 2 * sizeof(NrPostFxSidecarV1) +
                              262144 <
                          3 * 1024 * 1024,
                      "FG fixed graph, custody and worst concurrent packet/hash scratch exceed prepaid 3MiB");
        void failed() noexcept {
            void* context{};
            {
                std::lock_guard lock(graph.mutex);
                graph.failed = true;
                context = graph.context;
                graph.changed.notify_all();
            }
            invalidateContext(context);
            receiver().fail();
        }
        Target* target(uint64_t list) noexcept {
            for (auto& row : graph.targets)
                if (row.list == list)
                    return &row;
            for (auto& row : graph.targets)
                if (!row.list) {
                    row.list = list;
                    return &row;
                }
            return nullptr;
        }
        Descriptor* descriptor(uint64_t cpu) noexcept {
            for (auto& row : graph.descriptors)
                if (row.cpu == cpu)
                    return &row;
            for (auto& row : graph.descriptors)
                if (!row.cpu) {
                    row.cpu = cpu;
                    return &row;
                }
            return nullptr;
        }
        FgEventRow event(FgEvent kind) noexcept {
            FgEventRow value{};
            value.kind = kind;
            value.qpcObserved = timestamp();
            value.thread = GetCurrentThreadId();
            return value;
        }
        bool retain(FgRef index, IUnknown* pointer, uint64_t type) noexcept {
            if (!pointer)
                return false;
            const auto i = static_cast<unsigned>(index);
            if (auto* held = graph.ledger.builder(index)) {
                std::lock_guard lock(graph.mutex);
                return held == pointer && graph.types[i] == type;
            }
            if (!graph.ledger.acquire(index, pointer, 0))
                return false;
            auto row = event(FgEvent::retain);
            row.source = address(pointer);
            row.aux[0] = i;
            row.aux[1] = type;
            std::lock_guard lock(graph.mutex);
            uint64_t lifetime = 0;
            for (unsigned j = 0; j < 35; ++j)
                if (graph.retainedAddresses[j] == address(pointer) && graph.types[j] == type)
                    lifetime = graph.lifetimes[j];
            if (!lifetime)
                lifetime = ++graph.nextLifetime;
            row.epoch = graph.lifetimes[i] = lifetime;
            graph.types[i] = type;
            graph.retainedAddresses[i] = address(pointer);
            return graph.journal.append(row) != nullptr;
        }
        bool reserve() noexcept {
            const auto* packet = receiver().inspect(0);
            if (!packet)
                return false;
            {
                std::lock_guard lock(graph.mutex);
                if (graph.reserved)
                    return !graph.failed;
                if (graph.reserving || graph.failed || !nr_fg_budget_matches_packet(&graph.preflight, packet))
                    return false;
                graph.reserving = true;
            }
            if (!graph.ledger.reserve(packet->budget.senderRefCountB, packet->budget.pairOnlyBytes))
                return false;
            if (!retain(FgRef::device, graph.device, NR_DEVICE12) || !retain(FgRef::queue, graph.queue, NR_QUEUE12))
                return false;
            for (unsigned i = 0; i < 8; ++i)
                if (graph.outputs[i] && !retain(static_cast<FgRef>(i), graph.outputs[i], NR_RESOURCE12))
                    return false;
            {
                std::lock_guard lock(graph.mutex);
                graph.serialA = packet->identity.hostSerial;
                graph.serialB = graph.serialA + 1;
                graph.fgBudget = packet->budget.pairOnlyBytes;
                graph.reserving = false;
                graph.reserved = true;
            }
            return true;
        }
    } // namespace
    void fgPrepare(void* context) noexcept {
        if (!requested())
            return;
        std::lock_guard lock(graph.mutex);
        if (graph.reserved || graph.reserving) {
            graph.failed = true;
            return;
        }
        graph.context = context;
        graph.descriptors = {};
        graph.targets = {};
        graph.states = {};
        graph.journal.recordings = {};
        graph.preflight = {};
    }
    bool fgInstallBudget(const NrFgReadbackBudgetV1& budget) noexcept {
        if (!nr_validate_fg_budget(&budget, sizeof budget))
            return false;
        std::lock_guard lock(graph.mutex);
        if (graph.reserved || graph.reserving || graph.failed ||
            graph.context != reinterpret_cast<void*>(budget.fgContextAddress))
            return false;
        graph.preflight = budget;
        return true;
    }
    bool fgBudgetMatches(const NrPacketV1& packet) noexcept {
        std::lock_guard lock(graph.mutex);
        return !graph.failed && nr_fg_budget_matches_packet(&graph.preflight, &packet);
    }
    bool fgBound(void* context,
                 ID3D12Device* device,
                 ID3D12CommandQueue* queue,
                 ID3D12Resource* const outputs[8],
                 const wchar_t* output) noexcept {
        if (!requested())
            return false;
        std::lock_guard lock(graph.mutex);
        if (graph.context != context || graph.reserved || graph.reserving || graph.failed)
            return false;
        if (!output || wcsnlen_s(output, graph.output.size()) >= graph.output.size()) {
            graph.failed = true;
            return false;
        }
        graph.device = device;
        graph.queue = queue;
        for (unsigned i = 0; i < 8; ++i)
            graph.outputs[i] = outputs[i];
        wcscpy_s(graph.output.data(), graph.output.size(), output);
        return true;
    }
    bool fgInvalidateBudget(void* context) noexcept {
        std::lock_guard lock(graph.mutex);
        if (graph.context != context)
            return false;
        graph.preflight = {};
        return !graph.reserved && !graph.reserving && !graph.failed;
    }
    bool fgForgetContext(void* context) noexcept {
        std::lock_guard lock(graph.mutex);
        if (graph.context != context)
            return true;
        graph.preflight = {};
        if (graph.reserved || graph.reserving)
            return false; // Held intervals remain immutable.
        graph.context = nullptr;
        graph.device = nullptr;
        graph.queue = nullptr;
        graph.outputs = {};
        graph.consumers = {};
        graph.descriptors = {};
        graph.targets = {};
        graph.states = {};
        graph.journal.recordings = {};
        graph.failed = false;
        return true;
    }
    void fgReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, HRESULT result) noexcept {
        if (!requested())
            return;
        const auto qpc = timestamp();
        const auto thread = GetCurrentThreadId();
        bool okay;
        {
            std::lock_guard lock(graph.mutex);
            if (!graph.reserved || graph.failed || graph.workerStarted)
                return;
            okay = graph.journal.reset(address(list), address(allocator), static_cast<uint32_t>(result), qpc, thread);
            if (okay) {
                auto* state = target(address(list));
                if (!state)
                    okay = false;
                else {
                    *state = {};
                    state->list = address(list);
                }
            }
        }
        if (!okay)
            failed();
    }
    void fgAllocatorReset(ID3D12CommandAllocator* allocator, HRESULT result) noexcept {
        if (!requested())
            return;
        const auto qpc = timestamp();
        const auto thread = GetCurrentThreadId();
        bool okay;
        {
            std::lock_guard lock(graph.mutex);
            if (!graph.reserved || graph.failed || graph.workerStarted)
                return;
            okay = graph.journal.allocatorReset(address(allocator), static_cast<uint32_t>(result), qpc, thread);
        }
        if (!okay)
            failed();
    }
    void fgClosed(ID3D12GraphicsCommandList* list, HRESULT result) noexcept {
        if (!requested())
            return;
        const auto qpc = timestamp();
        const auto thread = GetCurrentThreadId();
        bool okay;
        {
            std::lock_guard lock(graph.mutex);
            if (!graph.reserved || graph.failed || graph.workerStarted)
                return;
            if (!graph.journal.recording(address(list)))
                return;
            okay = graph.journal.closed(address(list), static_cast<uint32_t>(result), qpc, thread);
        }
        if (!okay)
            failed();
    }
    void fgRoot(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root) noexcept {
        if (!requested())
            return;
        bool okay;
        {
            std::lock_guard lock(graph.mutex);
            auto* row = target(address(list));
            okay = row && root;
            if (okay)
                row->root = address(root);
        }
        if (!okay)
            failed();
    }
    void fgRootTable(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
        if (!requested() || index > 1)
            return;
        bool okay;
        {
            std::lock_guard lock(graph.mutex);
            auto* row = target(address(list));
            okay = row && handle.ptr;
            if (okay)
                row->tables[index] = handle.ptr;
        }
        if (!okay)
            failed();
    }
    void fgTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles) noexcept {
        if (!requested())
            return;
        bool okay = false;
        {
            std::lock_guard lock(graph.mutex);
            auto* row = target(address(list));
            if (row && handles && count && count <= 2) {
                row->count = count;
                for (UINT i = 0; i < count; ++i)
                    row->handles[i] = handles[i];
                okay = true;
            }
        }
        if (!okay)
            failed();
    }
    void fgViewport(ID3D12GraphicsCommandList* list,
                    const D3D12_VIEWPORT& viewport,
                    const D3D12_RECT& scissor) noexcept {
        if (!requested())
            return;
        bool okay;
        {
            std::lock_guard lock(graph.mutex);
            auto* row = target(address(list));
            okay = row != nullptr;
            if (row) {
                row->viewport = viewport;
                row->scissor = scissor;
            }
        }
        if (!okay)
            failed();
    }
    void fgTicket(void* context, const VDXRFG_TicketV1& ticket) noexcept {
        if (!requested() || !receiver().inspect(0))
            return;
        if (!reserve()) {
            failed();
            return;
        }
        {
            std::lock_guard lock(graph.mutex);
            if (ticket.current_serial != graph.serialA && ticket.current_serial != graph.serialB)
                return;
        }
        const auto* packet = receiver().inspect(ticket.current_serial == graph.serialA ? 0 : 1);
        bool okay = packet && context == graph.context && ticket.token && ticket.bank < 2 &&
                    ticket.current_serial == packet->identity.hostSerial &&
                    ticket.generation == packet->identity.fgHistoryGeneration;
        {
            std::lock_guard lock(graph.mutex);
            okay = okay && !graph.failed && !graph.workerStarted &&
                   (!graph.input.token || graph.input.current_serial != ticket.current_serial);
            if (okay)
                graph.input = ticket;
        }
        if (!okay)
            failed();
    }
    void fgAdmissionBegin(void* context, uint64_t serial) noexcept {
        if (!requested())
            return;
        const auto* a = receiver().inspect(0);
        if (!a)
            return;
        if (serial != a->identity.hostSerial && serial != a->identity.hostSerial + 1)
            return;
        const Graph::Begin observed{timestamp(), GetCurrentThreadId()};
        std::lock_guard lock(graph.mutex);
        if (context != graph.context || graph.failed)
            return;
        graph.admissionBegin[serial == a->identity.hostSerial ? 0 : 1] = observed;
    }
    void fgRetireBegin(void* context, uint64_t token) noexcept {
        if (!requested())
            return;
        const Graph::Begin observed{timestamp(), GetCurrentThreadId()};
        std::lock_guard lock(graph.mutex);
        if (context != graph.context || graph.failed)
            return;
        for (unsigned i = 0; i < 2; ++i)
            if (token && token == graph.tickets[i].token)
                graph.retireBegin[i] = observed;
    }
    void fgConfigure(void* slot,
                     ID3D12Resource* a,
                     ID3D12Resource* b,
                     uint64_t serialA,
                     uint64_t serialB,
                     uint64_t epoch,
                     const D3D12_SHADER_RESOURCE_VIEW_DESC& srv) noexcept {
        if (!requested())
            return;
        std::lock_guard lock(graph.mutex);
        if (!graph.reserved || graph.failed || graph.workerStarted || graph.input.current_serial != graph.serialB)
            return;
        graph.slot = slot;
        graph.consumers[2] = a;
        graph.consumers[3] = b;
        graph.srvs[2] = graph.srvs[3] = srv;
        graph.guideA = serialA;
        graph.guideB = serialB;
        graph.guideEpoch = epoch;
    }
    void fgSources(void* slot,
                   ID3D12Resource* a,
                   ID3D12Resource* b,
                   const D3D12_SHADER_RESOURCE_VIEW_DESC& srv,
                   ID3D12DescriptorHeap* heap,
                   UINT increment) noexcept {
        if (!requested())
            return;
        {
            std::lock_guard lock(graph.mutex);
            if (!graph.reserved || graph.failed || graph.workerStarted || graph.input.current_serial != graph.serialB)
                return;
            if (graph.slot != slot) {
                graph.failed = true;
                return;
            }
            graph.consumers[0] = a;
            graph.consumers[1] = b;
            graph.srvs[0] = graph.srvs[1] = srv;
            graph.heap = heap;
            graph.increment = increment;
        }
        if (!retain(FgRef::srvHeap, heap, NR_HEAP12))
            failed();
    }
    namespace {
        void historyWrite(FgEvent kind,
                          ID3D12Resource* source,
                          ID3D12Resource* targetResource,
                          ID3D12GraphicsCommandList* list,
                          uint64_t serial) noexcept {
            if (!requested())
                return;
            unsigned index{};
            uint64_t hostSerial{};
            {
                std::lock_guard lock(graph.mutex);
                if (!graph.reserved || graph.failed || graph.workerStarted)
                    return;
                if (graph.input.current_serial != graph.serialA && graph.input.current_serial != graph.serialB)
                    return;
                hostSerial = graph.input.current_serial;
                index = (kind == FgEvent::guideCopy ? 2u : 0u) + (hostSerial == graph.serialB ? 1u : 0u);
            }
            if (!retain(
                    static_cast<FgRef>(static_cast<unsigned>(FgRef::colourA) + index), targetResource, NR_RESOURCE12)) {
                failed();
                return;
            }
            auto row = event(kind);
            row.source = address(source);
            row.target = address(targetResource);
            row.list = address(list);
            row.serial = hostSerial;
            row.aux[0] = serial; // Separate active host serial and original callback serial.
            bool okay = false;
            {
                std::lock_guard lock(graph.mutex);
                auto* recording = graph.journal.recording(address(list));
                if (recording && !graph.writes[index].epoch) {
                    row.allocator = recording->allocator;
                    row.recording = recording->epoch;
                    row.epoch = ++graph.nextWrite;
                    auto& write = graph.writes[index];
                    write = {
                        row.source, row.target, row.list, row.allocator, row.recording, hostSerial, row.epoch, 0, 0};
                    okay = graph.journal.append(row) != nullptr;
                }
            }
            if (!okay)
                failed();
        }
    } // namespace
    void fgHistory(ID3D12Resource* source,
                   ID3D12Resource* targetResource,
                   ID3D12GraphicsCommandList* list,
                   uint64_t serial,
                   uint32_t) noexcept {
        historyWrite(FgEvent::historyCopy, source, targetResource, list, serial);
    }
    void fgGuideCopy(ID3D12Resource* source,
                     ID3D12Resource* targetResource,
                     ID3D12GraphicsCommandList* list,
                     uint64_t serial) noexcept {
        historyWrite(FgEvent::guideCopy, source, targetResource, list, serial);
    }
    void fgBarriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) noexcept {
        if (!requested())
            return;
        {
            std::lock_guard lock(graph.mutex);
            if (!graph.reserved || graph.failed || graph.workerStarted)
                return;
        }
        if (!barriers || count > 64) {
            failed();
            return;
        }
        for (UINT i = 0; i < count; ++i) {
            const auto& barrier = barriers[i];
            if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                continue;
            auto* resource = barrier.Transition.pResource;
            if (!resource) {
                failed();
                return;
            }
            const auto desc = resource->GetDesc();
            const bool full =
                barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
                (desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && barrier.Transition.Subresource == 0);
            bool found = false;
            {
                std::lock_guard lock(graph.mutex);
                Graph::State* row = nullptr;
                for (auto& candidate : graph.states)
                    if (candidate.resource == resource) {
                        row = &candidate;
                        break;
                    }
                if (!row)
                    for (auto& candidate : graph.states)
                        if (!candidate.resource) {
                            row = &candidate;
                            row->resource = resource;
                            break;
                        }
                if (row) {
                    row->read.transition(address(list),
                                         barrier.Transition.StateBefore,
                                         barrier.Transition.StateAfter,
                                         barrier.Flags,
                                         full);
                    found = true;
                }
            }
            if (!found) {
                failed();
                return;
            }
        }
    }
    void fgQueued(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
        if (!requested())
            return;
        {
            std::lock_guard lock(graph.mutex);
            if (!graph.reserved || graph.failed || graph.workerStarted)
                return;
        }
        if (!lists || !count || count > 16) {
            failed();
            return;
        }
        std::array<uint64_t, 16> ids{};
        for (UINT i = 0; i < count; ++i)
            ids[i] = address(lists[i]);
        const auto qpc = timestamp();
        const auto thread = GetCurrentThreadId();
        bool okay;
        {
            std::lock_guard lock(graph.mutex);
            okay = queue == graph.queue && graph.journal.execute(address(queue), ids.data(), count, qpc, thread);
            for (auto& state : graph.states)
                if (state.resource)
                    state.read.executeBatch(ids.data(), count);
        }
        if (!okay)
            failed();
    }
    void fgSignalled(
        const char* name, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value, HRESULT result) noexcept {
        if (!requested() || !name)
            return;
        auto row = event(std::strcmp(name, "original-producer-Wait") == 0 ? FgEvent::wait : FgEvent::signal);
        row.queue = address(queue);
        row.fence = address(fence);
        row.value = value;
        row.result = static_cast<uint32_t>(result);
        std::lock_guard lock(graph.mutex);
        if (!graph.reserved || graph.failed || graph.workerStarted)
            return;
        row.serial = graph.input.current_serial;
        if (queue != graph.queue || !graph.journal.append(row) || FAILED(result))
            graph.failed = true;
    }
    namespace {
        uint64_t floatBits(float value) noexcept {
            uint32_t result{};
            std::memcpy(&result, &value, 4);
            return result;
        }
        void storeDescriptor(Descriptor value) noexcept {
            value.qpc = timestamp();
            value.thread = GetCurrentThreadId();
            std::lock_guard lock(graph.mutex);
            if (graph.failed || graph.workerStarted)
                return;
            auto* row = descriptor(value.cpu);
            if (!row) {
                graph.failed = true;
                return;
            }
            value.epoch = ++graph.nextDescriptor;
            *row = value;
        }
    } // namespace
    void fgSrvCreated(void* slot,
                      ID3D12Resource* resource,
                      const D3D12_SHADER_RESOURCE_VIEW_DESC& desc,
                      ID3D12DescriptorHeap* heap,
                      UINT index,
                      UINT increment,
                      D3D12_CPU_DESCRIPTOR_HANDLE cpu) noexcept {
        if (!requested() || !heap)
            return;
        Descriptor value{};
        value.cpu = cpu.ptr;
        value.gpu = heap->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(index) * increment;
        value.resource = address(resource);
        value.heap = address(heap);
        value.slot = index;
        value.increment = increment;
        value.kind = NR_D12_SRV;
        value.bytes = sizeof desc;
        auto& w = value.description;
        w[0] = desc.Format;
        w[1] = desc.ViewDimension;
        w[2] = desc.Shader4ComponentMapping;
        if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            w[3] = desc.Texture2D.MostDetailedMip;
            w[4] = desc.Texture2D.MipLevels;
            w[7] = desc.Texture2D.PlaneSlice;
            w[8] = floatBits(desc.Texture2D.ResourceMinLODClamp);
        } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
            w[3] = desc.Texture2DArray.MostDetailedMip;
            w[4] = desc.Texture2DArray.MipLevels;
            w[5] = desc.Texture2DArray.FirstArraySlice;
            w[6] = desc.Texture2DArray.ArraySize;
            w[7] = desc.Texture2DArray.PlaneSlice;
            w[8] = floatBits(desc.Texture2DArray.ResourceMinLODClamp);
        } else
            return;
        (void)slot;
        storeDescriptor(value);
    }
    void fgRtvCreated(ID3D12Resource* resource,
                      const D3D12_RENDER_TARGET_VIEW_DESC& desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                      ID3D12DescriptorHeap* heap,
                      UINT index,
                      UINT increment) noexcept {
        if (!requested())
            return;
        Descriptor value{};
        value.cpu = cpu.ptr;
        value.heap = address(heap);
        value.resource = address(resource);
        value.slot = index;
        value.increment = increment;
        value.kind = NR_D12_RTV;
        value.bytes = sizeof desc;
        value.description[0] = desc.Format;
        value.description[1] = desc.ViewDimension;
        if (desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
            value.description[2] = desc.Texture2D.MipSlice;
            value.description[5] = desc.Texture2D.PlaneSlice;
        } else if (desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
            value.description[2] = desc.Texture2DArray.MipSlice;
            value.description[3] = desc.Texture2DArray.FirstArraySlice;
            value.description[4] = desc.Texture2DArray.ArraySize;
            value.description[5] = desc.Texture2DArray.PlaneSlice;
        } else
            return;
        storeDescriptor(value);
    }
    void fgCbv(void*,
               ID3D12Resource* resource,
               ID3D12DescriptorHeap* heap,
               UINT eye,
               UINT index,
               UINT offset,
               UINT increment,
               const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc,
               D3D12_CPU_DESCRIPTOR_HANDLE cpu) noexcept {
        if (!requested() || !heap)
            return;
        Descriptor value{};
        value.cpu = cpu.ptr;
        value.gpu = heap->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(index) * increment;
        value.heap = address(heap);
        value.resource = address(resource);
        value.eye = eye;
        value.offset = offset;
        value.slot = index;
        value.increment = increment;
        value.kind = NR_D12_CBV;
        value.bytes = sizeof desc;
        value.description[0] = desc.BufferLocation;
        value.description[1] = desc.SizeInBytes;
        storeDescriptor(value);
    }
    void fgCbvWrite(ID3D12Resource* resource, UINT eye) noexcept {
        if (!requested() || eye > 1)
            return;
        std::lock_guard lock(graph.mutex);
        if (!graph.reserved || graph.failed || graph.workerStarted || graph.input.current_serial != graph.serialB)
            return;
        graph.b1 = resource;
        graph.b1Write[eye] = ++graph.nextWrite;
    }
    namespace {
        bool validSnapshotTexture(const D3D12_RESOURCE_DESC& d, uint64_t sourceKind) noexcept {
            const auto pixelBytes = nr_capture_pixel_bytes(d.Format);
            return pixelBytes && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width &&
                   d.Width <= UINT_MAX / pixelBytes && d.Height && d.DepthOrArraySize && d.DepthOrArraySize <= 2 &&
                   d.MipLevels == 1 && d.SampleDesc.Count == 1 && !d.SampleDesc.Quality &&
                   nr_fg_budget_texture_flags(sourceKind, static_cast<uint64_t>(d.Flags)) &&
                   (nr_capture_view_family_compatible(d.Format, DXGI_FORMAT_R8G8B8A8_UNORM) ||
                    d.Format == DXGI_FORMAT_R32_FLOAT);
        }
        bool planImages(std::array<Image, 5>& images) noexcept {
            uint64_t allocation = 0, payload = 0;
            for (unsigned i = 0; i < 5; ++i) {
                auto* source = i < 4 ? graph.consumers[i] : graph.outputs[graph.input.bank * 4];
                if (!source)
                    return false;
                NrFgBudgetRowV1 observed{};
                if (!observeBudgetResource(graph.device,
                                           source,
                                           i < 4 ? i : graph.input.bank * 4,
                                           i < 4 ? 1 : 2,
                                           graph.preflight.fgContextLifetimeEpoch,
                                           observed) ||
                    !nr_fg_budget_same_layout(&observed, &graph.preflight.rows[i]))
                    return false;
                auto& image = images[i];
                const auto* d = observed.textureDescription;
                image.description = {static_cast<D3D12_RESOURCE_DIMENSION>(d[0]),
                                     d[1],
                                     d[2],
                                     static_cast<UINT>(d[3]),
                                     static_cast<UINT16>(d[4]),
                                     static_cast<UINT16>(d[5]),
                                     static_cast<DXGI_FORMAT>(d[6]),
                                     {static_cast<UINT>(d[7]), static_cast<UINT>(d[8])},
                                     static_cast<D3D12_TEXTURE_LAYOUT>(d[9]),
                                     static_cast<D3D12_RESOURCE_FLAGS>(d[10])};
                if (!validSnapshotTexture(image.description, i == 4 ? 2 : 1))
                    return false;
                for (unsigned sub = 0; sub < observed.subresourceCount; ++sub) {
                    const auto& f = observed.footprints[sub];
                    image.footprints[sub] = {f.offset,
                                             {static_cast<DXGI_FORMAT>(f.format),
                                              static_cast<UINT>(f.width),
                                              static_cast<UINT>(f.height),
                                              static_cast<UINT>(f.depth),
                                              static_cast<UINT>(f.rowPitch)}};
                    image.rows[sub] = static_cast<UINT>(f.rowCount);
                    image.rowBytes[sub] = f.rowBytes;
                }
                image.bytes = observed.payloadBound;
                image.allocation = observed.allocationBytes;
                if (!image.allocation || image.allocation == UINT64_MAX ||
                    image.allocation > graph.fgBudget - allocation || image.bytes > graph.fgBudget - payload)
                    return false;
                allocation += image.allocation;
                payload += image.bytes;
            }
            return allocation == graph.preflight.totalAllocation && payload == graph.preflight.totalPayload;
        }
        bool created(FgRef slot,
                     IUnknown* pointer,
                     uint64_t type,
                     uint64_t bytes,
                     HRESULT result,
                     FgEvent kind = FgEvent::allocate,
                     uint64_t source = 0) noexcept {
            if (!graph.ledger.finishCreate(slot, pointer, SUCCEEDED(result) && pointer))
                return false;
            const auto i = static_cast<unsigned>(slot);
            auto row = event(kind);
            row.target = address(pointer);
            row.source = source;
            row.value = bytes;
            row.result = static_cast<uint32_t>(result);
            row.aux[0] = i;
            row.aux[1] = type;
            row.aux[2] = kind == FgEvent::getDevice ? NR_VOID : NR_HRESULT;
            std::lock_guard lock(graph.mutex);
            row.epoch = graph.lifetimes[i] = ++graph.nextLifetime;
            graph.types[i] = type;
            graph.retainedAddresses[i] = address(pointer);
            return graph.journal.append(row) != nullptr;
        }
        bool allocateImages(const std::array<Image, 5>& images) noexcept {
            for (unsigned i = 0; i < 5; ++i) {
                const auto slot = static_cast<FgRef>(static_cast<unsigned>(FgRef::readback0) + i);
                const auto& image = images[i];
                if (!graph.ledger.reserveCreate(slot, image.allocation))
                    return false;
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC buffer{};
                buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width = image.bytes;
                buffer.Height = 1;
                buffer.DepthOrArraySize = buffer.MipLevels = 1;
                buffer.SampleDesc.Count = 1;
                buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                ID3D12Resource* resource{};
                const auto result = graph.device->CreateCommittedResource(&heap,
                                                                          D3D12_HEAP_FLAG_NONE,
                                                                          &buffer,
                                                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                                                          nullptr,
                                                                          IID_PPV_ARGS(&resource));
                if (!created(slot, resource, NR_RESOURCE12, image.allocation, result))
                    return false;
            }
            return true;
        }
        bool copyImage(unsigned index, ID3D12GraphicsCommandList* list, const ReadCopyPlan& plan) noexcept {
            auto* source = index < 4 ? graph.consumers[index] : graph.outputs[graph.input.bank * 4];
            auto* readback = static_cast<ID3D12Resource*>(
                graph.ledger.builder(static_cast<FgRef>(static_cast<unsigned>(FgRef::readback0) + index)));
            if (!source || !readback || !plan.valid)
                return false;
            Image image;
            uint64_t recording{};
            {
                std::lock_guard lock(graph.mutex);
                const auto* row = graph.journal.recording(address(list));
                if (!row)
                    return false;
                recording = row->epoch;
                image = graph.images[index];
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = source;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(plan.before);
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            if (plan.transition)
                list->ResourceBarrier(1, &barrier);
            for (UINT sub = 0; sub < image.description.DepthOrArraySize; ++sub) {
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = source;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = sub;
                to.pResource = readback;
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = image.footprints[sub];
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            if (plan.transition) {
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                list->ResourceBarrier(1, &barrier);
            }
            auto row = event(index < 4 ? FgEvent::consumerCopy : FgEvent::outputCopy);
            row.source = address(source);
            row.target = address(readback);
            row.list = address(list);
            row.recording = recording;
            row.count = image.description.DepthOrArraySize;
            row.aux[0] = plan.before;
            row.aux[1] = plan.transition;
            row.aux[2] = plan.implicit;
            std::lock_guard lock(graph.mutex);
            row.epoch = ++graph.nextWrite;
            auto& saved = graph.images[index];
            saved.copyEpoch = row.epoch;
            saved.copyList = row.list;
            saved.recording = recording;
            saved.before = plan.before;
            saved.after = plan.before;
            return graph.journal.append(row) != nullptr;
        }
    } // namespace
    void fgDraw(void* slot,
                ID3D12GraphicsCommandList* list,
                ID3D12PipelineState* pso,
                UINT eye,
                UINT flags,
                UINT second,
                const void* b1,
                const void* b0,
                size_t b0Size,
                UINT viewIndex,
                ID3D12Resource* b1Resource,
                const unsigned char* shader,
                size_t shaderSize) noexcept {
        if (!requested() || !receiver().inspect(1))
            return;
        try {
            if (eye > 1 || !shader || !shaderSize || shaderSize > ULONG_MAX) {
                failed();
                return;
            }
            Draw observed{};
            Target bound{};
            std::array<ReadCopyPlan, 4> plans{};
            bool first = false;
            uint64_t allocator{};
            if (!copyFgConstants(observed.constants, b0, b0Size, b1, fgCbvBytes) ||
                BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                           nullptr,
                           0,
                           const_cast<PUCHAR>(shader),
                           static_cast<ULONG>(shaderSize),
                           observed.shader.data(),
                           32) < 0) {
                failed();
                return;
            }
            const auto qpc = timestamp();
            const auto thread = GetCurrentThreadId();
            {
                std::lock_guard lock(graph.mutex);
                if (graph.failed || !graph.reserved || graph.workerStarted)
                    return;
                auto* targetRow = target(address(list));
                if (!targetRow || !targetRow->count || targetRow->count > 2) {
                    graph.failed = true;
                    return;
                }
                bound = *targetRow;
                const Descriptor* selected = nullptr;
                for (unsigned i = 0; i < bound.count; ++i)
                    for (const auto& descriptor : graph.descriptors)
                        if (descriptor.cpu == bound.handles[i].ptr &&
                            descriptor.resource == address(graph.outputs[graph.input.bank * 4]))
                            selected = &descriptor;
                if (!selected)
                    return; // An observed other output draw is not selected role0.
                auto* recording = graph.journal.recording(address(list));
                if (!recording || recording->closeSequence || !bound.root || !pso || graph.slot != slot ||
                    graph.input.current_serial != graph.serialB || graph.guideA != graph.serialA ||
                    graph.guideB != graph.serialB || !graph.guideEpoch || graph.drawMask & (uint64_t{1} << eye) ||
                    graph.b1 != b1Resource || !graph.b1Write[eye]) {
                    graph.failed = true;
                    return;
                }
                allocator = recording->allocator;
                first = graph.drawMask == 0;
                for (unsigned i = 0; i < 4; ++i) {
                    if (graph.writes[i].target != address(graph.consumers[i]) || !graph.writes[i].epoch ||
                        graph.writes[i].serial != (i % 2 ? graph.serialB : graph.serialA)) {
                        graph.failed = true;
                        return;
                    }
                    const uint64_t gpu = bound.tables[0] + uint64_t(i < 2 ? i : i + 5) * graph.increment;
                    const Descriptor* matched = nullptr;
                    for (const auto& descriptor : graph.descriptors)
                        if (descriptor.gpu == gpu && descriptor.heap == address(graph.heap) &&
                            descriptor.resource == address(graph.consumers[i]) && descriptor.kind == NR_D12_SRV)
                            matched = &descriptor;
                    if (!matched) {
                        graph.failed = true;
                        return;
                    }
                    observed.descriptors[i] = *matched;
                    Graph::State* state = nullptr;
                    for (auto& entry : graph.states)
                        if (entry.resource == graph.consumers[i])
                            state = &entry;
                    if (!state || !state->read.drawRead(address(list), true)) {
                        graph.failed = true;
                        return;
                    }
                    if (first) {
                        plans[i] = state->read.copyRead(address(list));
                        if (!plans[i].valid) {
                            graph.failed = true;
                            return;
                        }
                    }
                }
                const Descriptor* cbv = nullptr;
                for (const auto& descriptor : graph.descriptors)
                    if (descriptor.kind == NR_D12_CBV && descriptor.eye == eye &&
                        descriptor.resource == address(b1Resource) && descriptor.heap == address(graph.heap) &&
                        descriptor.gpu == bound.tables[0] + descriptor.offset * descriptor.increment)
                        cbv = &descriptor;
                if (!cbv || cbv->description[1] != 256 || !bound.tables[1]) {
                    graph.failed = true;
                    return;
                }
                observed.descriptors[4] = *cbv;
                observed.descriptors[5] = *selected;
                observed.descriptorCount = 6;
                for (unsigned i = 0; i < bound.count; ++i)
                    if (bound.handles[i].ptr != selected->cpu) {
                        const Descriptor* other = nullptr;
                        for (const auto& descriptor : graph.descriptors)
                            if (descriptor.cpu == bound.handles[i].ptr)
                                other = &descriptor;
                        if (!other || observed.descriptorCount == 7) {
                            graph.failed = true;
                            return;
                        }
                        observed.descriptors[observed.descriptorCount++] = *other;
                    }
                auto& row = observed.row;
                row.kind = 5;
                row.eye = eye;
                row.listOrContext = address(list);
                row.recordingEpoch = recording->epoch;
                row.batchEpoch = graph.input.token;
                row.bindEpoch = cbv->epoch;
                row.psoAddress = address(pso);
                row.rootAddress = bound.root;
                row.rootTable0 = bound.tables[0];
                row.rootTable1 = bound.tables[1];
                row.viewportXbits = floatBits(bound.viewport.TopLeftX);
                row.viewportYbits = floatBits(bound.viewport.TopLeftY);
                row.viewportWbits = floatBits(bound.viewport.Width);
                row.viewportHbits = floatBits(bound.viewport.Height);
                row.minDepthBits = floatBits(bound.viewport.MinDepth);
                row.maxDepthBits = floatBits(bound.viewport.MaxDepth);
                row.scissorLeftBits = static_cast<uint32_t>(bound.scissor.left);
                row.scissorTopBits = static_cast<uint32_t>(bound.scissor.top);
                row.scissorRightBits = static_cast<uint32_t>(bound.scissor.right);
                row.scissorBottomBits = static_cast<uint32_t>(bound.scissor.bottom);
                row.vertexCount = 3;
                row.instanceCount = 1;
                row.resourceWriteEpoch = ++graph.nextWrite;
                row.observedMask = 15;
                FgEventRow draw{};
                draw.kind = FgEvent::draw;
                draw.qpcObserved = qpc;
                draw.thread = thread;
                draw.list = address(list);
                draw.allocator = allocator;
                draw.recording = recording->epoch;
                draw.target = selected->resource;
                draw.serial = graph.serialB;
                draw.epoch = row.resourceWriteEpoch;
                draw.aux[0] = eye;
                draw.aux[1] = flags;
                draw.aux[2] = second;
                draw.aux[3] = viewIndex;
                draw.aux[4] = address(pso);
                draw.aux[5] = bound.root;
                draw.count = 3;
                if (!graph.journal.append(draw)) {
                    graph.failed = true;
                    return;
                }
                graph.selectedWrite = row.resourceWriteEpoch;
            }
            if (first) {
                if (!retain(FgRef::originalList, list, NR_LIST12) ||
                    !retain(FgRef::originalAllocator,
                            reinterpret_cast<ID3D12CommandAllocator*>(allocator),
                            NR_ALLOCATOR12) ||
                    !retain(FgRef::root, reinterpret_cast<ID3D12RootSignature*>(bound.root), NR_ROOT12) ||
                    !retain(FgRef::pso, pso, NR_PSO12) || !retain(FgRef::b1, b1Resource, NR_RESOURCE12) ||
                    !retain(FgRef::rtvHeap,
                            reinterpret_cast<ID3D12DescriptorHeap*>(observed.descriptors[5].heap),
                            NR_HEAP12)) {
                    failed();
                    return;
                }
                std::array<Image, 5> planned{};
                if (!planImages(planned) || !allocateImages(planned)) {
                    failed();
                    return;
                }
                {
                    std::lock_guard lock(graph.mutex);
                    graph.images = planned;
                    graph.selectedList = address(list);
                    graph.selectedRecording = observed.row.recordingEpoch;
                }
                for (unsigned i = 0; i < 4; ++i)
                    if (!copyImage(i, list, plans[i])) {
                        failed();
                        return;
                    }
            }
            {
                std::lock_guard lock(graph.mutex);
                if (graph.selectedList != address(list) || graph.selectedRecording != observed.row.recordingEpoch) {
                    graph.failed = true;
                    return;
                }
                observed.row.pipelineLifetime = graph.lifetimes[static_cast<unsigned>(FgRef::pso)];
                observed.row.pipelineGeneration = graph.input.generation;
                graph.draws[eye] = observed;
                graph.drawMask |= uint64_t{1} << eye;
                graph.copiesRecorded = true;
            }
        } catch (...) {
            failed();
        }
    }
    namespace {
        DWORD WINAPI worker(void*) noexcept;
        bool setupWorker(const Host& host) noexcept {
            if (!host.producer || !host.ticket.output_ready_fence || !host.ticket.output_ready_value ||
                !retain(FgRef::productFence, static_cast<ID3D12Fence*>(host.ticket.output_ready_fence), NR_FENCE12))
                return false;
            if (!graph.ledger.reserveCreate(FgRef::copyFence, 0))
                return false;
            ID3D12Fence* fence{};
            auto result = graph.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
            if (!created(FgRef::copyFence, fence, NR_FENCE12, 0, result))
                return false;
            if (!graph.ledger.reserveCreate(FgRef::copyAllocator, 0))
                return false;
            ID3D12CommandAllocator* allocator{};
            result = graph.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            if (!created(FgRef::copyAllocator, allocator, NR_ALLOCATOR12, 0, result))
                return false;
            if (!graph.ledger.reserveCreate(FgRef::copyList, 0))
                return false;
            ID3D12GraphicsCommandList* commands{};
            result = graph.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&commands));
            if (!created(FgRef::copyList, commands, NR_LIST12, 0, result))
                return false;
            result = commands->Close();
            auto initial = event(FgEvent::initialClose);
            initial.list = address(commands);
            initial.allocator = address(allocator);
            initial.result = static_cast<uint32_t>(result);
            initial.aux[0] = NR_HRESULT;
            {
                std::lock_guard lock(graph.mutex);
                if (!graph.journal.append(initial))
                    return false;
            }
            if (FAILED(result))
                return false;
            if (!receiver().claimScratch(1, 2) || !graph.ledger.reserveCreate(FgRef::submissionDevice, 0))
                return false;
            ID3D11Device* device11{};
            host.producer->GetDevice(&device11);
            if (!created(FgRef::submissionDevice,
                         device11,
                         NR_DEVICE11,
                         0,
                         S_OK,
                         FgEvent::getDevice,
                         address(host.producer)))
                return false;
            ID3D11Device5* importer{};
            result = device11->QueryInterface(IID_PPV_ARGS(&importer));
            auto queried = event(FgEvent::queryInterface);
            queried.source = address(device11);
            queried.target = address(importer);
            queried.result = static_cast<uint32_t>(result);
            queried.aux[0] = 5;
            queried.aux[1] = 1;
            queried.aux[2] = NR_HRESULT;
            {
                std::lock_guard lock(graph.mutex);
                if (!graph.journal.append(queried))
                    graph.failed = true;
            }
            if (FAILED(result) || !importer) {
                receiver().returnScratch(1, 2);
                return false;
            }
            HANDLE shared{};
            result = graph.device->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &shared);
            auto exported = event(FgEvent::fenceExport);
            exported.source = address(fence);
            exported.queue = address(graph.device);
            exported.value = address(shared);
            exported.result = static_cast<uint32_t>(result);
            {
                std::lock_guard lock(graph.mutex);
                if (!graph.journal.append(exported))
                    graph.failed = true;
            }
            if (FAILED(result) || !shared) {
                importer->Release();
                receiver().returnScratch(1, 2);
                return false;
            }
            if (!graph.ledger.reserveCreate(FgRef::copyFence11, 0)) {
                CloseHandle(shared);
                importer->Release();
                receiver().returnScratch(1, 2);
                return false;
            }
            ID3D11Fence* imported{};
            result = importer->OpenSharedFence(shared, IID_PPV_ARGS(&imported));
            auto importedEvent = event(FgEvent::fenceImport);
            importedEvent.target = address(imported);
            importedEvent.queue = address(importer);
            importedEvent.value = address(shared);
            importedEvent.result = static_cast<uint32_t>(result);
            {
                std::lock_guard lock(graph.mutex);
                if (!graph.journal.append(importedEvent))
                    graph.failed = true;
            }
            CloseHandle(shared);
            importer->Release();
            receiver().returnScratch(1, 2);
            if (!created(FgRef::copyFence11, imported, NR_FENCE11, 0, result))
                return false;
            if (!graph.ledger.reserveCreate(FgRef::info11, 0))
                return false;
            ID3D11InfoQueue* info11{};
            graph.info11Result = device11->QueryInterface(IID_PPV_ARGS(&info11));
            if (SUCCEEDED(graph.info11Result)) {
                if (!created(FgRef::info11,
                             info11,
                             NR_INFO_QUEUE,
                             0,
                             graph.info11Result,
                             FgEvent::queryInterface,
                             address(device11)))
                    return false;
                graph.info11Begin = info11->GetNumStoredMessagesAllowedByRetrievalFilter();
            } else if (graph.info11Result != E_NOINTERFACE || info11 || !graph.ledger.observedAbsent(FgRef::info11))
                return false;
            if (!graph.ledger.reserveCreate(FgRef::info12, 0))
                return false;
            ID3D12InfoQueue* info12{};
            graph.info12Result = graph.device->QueryInterface(IID_PPV_ARGS(&info12));
            if (SUCCEEDED(graph.info12Result)) {
                if (!created(FgRef::info12,
                             info12,
                             NR_INFO_QUEUE,
                             0,
                             graph.info12Result,
                             FgEvent::queryInterface,
                             address(graph.device)))
                    return false;
                graph.info12Begin = info12->GetNumStoredMessagesAllowedByRetrievalFilter();
            } else if (graph.info12Result != E_NOINTERFACE || info12 || !graph.ledger.observedAbsent(FgRef::info12))
                return false;
            for (unsigned i = 8; i < 35; ++i)
                if (i != static_cast<unsigned>(FgRef::unusedB0) && i != static_cast<unsigned>(FgRef::info11) &&
                    i != static_cast<unsigned>(FgRef::info12) && !graph.ledger.builder(static_cast<FgRef>(i)))
                    return false;
            return graph.ledger.seal();
        }
    } // namespace
    void fgHost(const Host& host) noexcept {
        if (!requested())
            return;
        try {
            if (host.operation == 4 && host.eventName) {
                FgEvent kind{};
                if (!std::strcmp(host.eventName, "texture-export11"))
                    kind = FgEvent::textureExport;
                else if (!std::strcmp(host.eventName, "texture-import12"))
                    kind = FgEvent::textureImport;
                else if (!std::strcmp(host.eventName, "original-Retire-result"))
                    kind = FgEvent::retire;
                else
                    return;
                auto row = event(kind);
                row.source = address(host.eventObject);
                row.target = address(host.eventOther);
                row.value = host.eventValue;
                row.result = static_cast<uint32_t>(host.originalResult);
                row.serial = host.serial;
                row.aux[0] = host.ticket.token;
                row.aux[1] = host.ticket.bank;
                row.aux[2] = host.ticket.generation;
                std::lock_guard lock(graph.mutex);
                if (graph.failed || graph.published || graph.journalClosed)
                    return;
                if (kind == FgEvent::retire) {
                    if (host.context != graph.context ||
                        (host.ticket.token != graph.tickets[0].token && host.ticket.token != graph.tickets[1].token))
                        return;
                    const auto& begin = graph.retireBegin[host.ticket.token == graph.tickets[0].token ? 0 : 1];
                    row.aux[6] = begin.qpc;
                    row.aux[7] = begin.thread;
                    if (!begin.qpc || begin.thread != row.thread || begin.qpc > row.qpcObserved) {
                        graph.failed = true;
                        return;
                    }
                    if (SUCCEEDED(host.originalResult)) {
                        if (host.ticket.token == graph.tickets[0].token)
                            graph.retiredA = true;
                        if (host.ticket.token == graph.tickets[1].token)
                            graph.retiredB = true;
                    }
                }
                if (!graph.journal.append(row))
                    graph.failed = true;
                graph.changed.notify_all();
                return;
            }
            if (host.context != graph.context)
                return;
            if (host.operation == 6) {
                const auto qpc = timestamp();
                const auto thread = GetCurrentThreadId();
                std::lock_guard lock(graph.mutex);
                if (graph.failed || graph.journalClosed)
                    return;
                if (graph.reserved && host.serial != graph.serialA && host.serial != graph.serialB)
                    return;
                auto& origin = graph.origins[host.serial % 2];
                origin.serial = host.serial;
                origin.qpc = qpc;
                origin.producer = address(host.producer);
                origin.target11 = address(host.eventObject);
                origin.target12 = address(host.eventOther);
                for (unsigned eye = 0; eye < 2; ++eye) {
                    origin.source[eye] = address(host.colourSources[eye]);
                    origin.views[eye] = host.colourViews[eye];
                }
                FgEventRow row{};
                row.kind = FgEvent::inputCopy;
                row.qpcObserved = qpc;
                row.thread = thread;
                row.serial = host.serial;
                row.list = origin.producer;
                row.source = origin.source[0];
                row.target = origin.target12;
                row.aux[0] = origin.source[1];
                row.aux[1] = origin.target11;
                row.aux[2] = host.eventValue;
                row.count = 2;
                if (!graph.journal.append(row))
                    graph.failed = true;
                return;
            }
            if (host.operation == 7) {
                auto row = event(FgEvent::nativeOutput);
                row.source = host.output.source12;
                row.target = host.output.destination11;
                row.serial = host.serial;
                row.epoch = host.output.copyId;
                row.value = host.ticket.token;
                row.result = static_cast<uint32_t>(host.originalResult);
                row.aux[0] = host.output.role;
                row.aux[1] = host.ticket.bank;
                row.aux[2] = host.ticket.generation;
                row.aux[3] = host.output.destinationRetentionEpoch;
                row.aux[4] = host.output.callQpc;
                row.aux[5] = host.output.copyQpc;
                row.aux[6] = host.output.signalQpc;
                row.aux[7] = host.output.returnQpc;
                std::lock_guard lock(graph.mutex);
                if (!graph.workerStarted || graph.failed || graph.journalClosed ||
                    host.ticket.token != graph.tickets[1].token || host.output.role != 0)
                    return;
                if (graph.nativeCount == graph.native.size()) {
                    graph.failed = true;
                    return;
                }
                graph.native[graph.nativeCount++] = host;
                if (!graph.journal.append(row))
                    graph.failed = true;
                graph.changed.notify_all();
                return;
            }
            if (host.operation != 2 || !receiver().inspect(0))
                return;
            {
                std::lock_guard lock(graph.mutex);
                if (host.serial != graph.serialA && host.serial != graph.serialB)
                    return;
            }
            const auto* packet = receiver().inspect(host.serial == graph.serialA ? 0 : 1);
            if (!packet)
                return;
            const unsigned frame = host.serial == graph.serialA ? 0u : 1u;
            if (FAILED(host.originalResult) || !host.nrEnabled ||
                host.ticket.current_serial != packet->identity.hostSerial ||
                host.ticket.generation != packet->identity.fgHistoryGeneration || !host.ticket.token ||
                host.ticket.bank > 1 || (frame && host.ticket.previous_serial != graph.serialA)) {
                failed();
                return;
            }
            auto row = event(FgEvent::admission);
            row.serial = host.serial;
            row.value = host.ticket.token;
            row.result = static_cast<uint32_t>(host.originalResult);
            row.fence = address(host.ticket.output_ready_fence);
            row.aux[0] = host.ticket.output_ready_value;
            row.aux[1] = host.ticket.bank;
            row.aux[2] = host.ticket.generation;
            row.aux[3] = host.ticket.previous_serial;
            row.aux[4] = host.ticket.current_serial;
            row.aux[5] = host.ticket.prime;
            {
                std::lock_guard lock(graph.mutex);
                if (graph.failed || graph.tickets[frame].token)
                    return;
                row.aux[6] = graph.admissionBegin[frame].qpc;
                row.aux[7] = graph.admissionBegin[frame].thread;
                if (!row.aux[6] || row.aux[7] != row.thread || row.aux[6] > row.qpcObserved) {
                    graph.failed = true;
                    return;
                }
                graph.tickets[frame] = host.ticket;
                if (!graph.journal.append(row)) {
                    graph.failed = true;
                    return;
                }
                if (frame && (graph.drawMask != 3 || !graph.copiesRecorded || graph.input.token != host.ticket.token ||
                              graph.input.bank != host.ticket.bank)) {
                    graph.failed = true;
                    return;
                }
            }
            if (!frame)
                return;
            if (!setupWorker(host)) {
                failed();
                return;
            }
            {
                std::lock_guard lock(graph.mutex);
                if (graph.failed)
                    return;
                graph.workerStarted = true;
            }
            HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
            {
                std::lock_guard lock(graph.mutex);
                graph.worker = thread;
            }
            if (!thread)
                failed();
        } catch (...) {
            failed();
        }
    }
    bool fgHoldRetire(void* context, uint64_t token) noexcept {
        std::lock_guard lock(graph.mutex);
        return context == graph.context && token && token == graph.tickets[1].token && !graph.ownership.canRetire();
    }
    void fgAbort(void* context) noexcept {
        std::lock_guard publication(fgPublicationGate());
        {
            std::lock_guard lock(graph.mutex);
            if (context != graph.context || graph.published)
                return;
        }
        failed();
    }
    HRESULT fgOutputWait(void* context, uint64_t token, uint32_t role, ID3D11DeviceContext4* consumer) noexcept {
        bool complete = false;
        {
            std::lock_guard lock(graph.mutex);
            if (context != graph.context || role || !token || token != graph.tickets[1].token)
                return S_OK;
            if (graph.failed || !graph.workerStarted || !consumer)
                return E_FAIL;
            if (graph.journalClosed)
                return graph.ownership.copyComplete ? S_OK : E_FAIL;
            complete = graph.ownership.copyComplete;
        }
        auto row = event(FgEvent::wait);
        row.queue = address(consumer);
        row.value = 1;
        row.serial = graph.serialB;
        if (complete) {
            row.aux[0] = 1; // Observed copy completion precedes the native call; no API Wait invented.
            row.aux[1] = graph.copyCompletedSequence;
            std::lock_guard lock(graph.mutex);
            auto* saved = graph.journal.append(row);
            if (!saved) {
                graph.failed = true;
                return E_FAIL;
            }
            graph.outputAlreadyComplete = true;
            graph.nativeDependencySequence = saved->sequence;
            return S_OK;
        }
        auto* fence = static_cast<ID3D11Fence*>(graph.ledger.borrow(FgRef::copyFence11));
        if (!fence)
            return E_FAIL;
        const auto result = consumer->Wait(fence, 1);
        row.fence = address(fence);
        row.result = static_cast<uint32_t>(result);
        row.qpcObserved = timestamp();
        {
            std::lock_guard lock(graph.mutex);
            auto* saved = graph.journal.append(row);
            if (!saved || FAILED(result))
                graph.failed = true;
            else {
                graph.ownership.hostWaitQueued = true;
                graph.nativeDependencySequence = saved->sequence;
            }
        }
        return result;
    }
    namespace {
        bool observedTextureAlias(uint64_t source, uint64_t target) noexcept {
            for (uint64_t i = graph.journal.count; i > 0; --i) {
                const auto& imported = graph.journal.rows[i - 1];
                if (imported.kind != FgEvent::textureImport || imported.source != target ||
                    imported.result & 0x80000000u)
                    continue;
                for (uint64_t j = i - 1; j > 0; --j) {
                    const auto& exported = graph.journal.rows[j - 1];
                    if (exported.kind != FgEvent::textureExport || exported.value != imported.value)
                        continue;
                    return exported.source == source && !(exported.result & 0x80000000u) &&
                           exported.qpcObserved <= imported.qpcObserved;
                }
            }
            return false;
        }
        bool bindOrigins() noexcept {
            for (unsigned frame = 0; frame < 2; ++frame) {
                const auto& packet = graph.final[frame];
                const auto& origin = graph.origins[packet.identity.hostSerial % 2];
                if (origin.serial != packet.identity.hostSerial || origin.target12 != graph.writes[frame].source ||
                    !observedTextureAlias(origin.target11, origin.target12))
                    return false;
                unsigned matched = 0;
                const NrRoleV1* canonical = nullptr;
                for (uint64_t i = 0; i < packet.header.roleCount; ++i) {
                    const auto& role = packet.roles[i];
                    if (role.role == NR_CANONICAL_DEPTH)
                        canonical = &role;
                    if (role.role == NR_FINAL_COLOUR && role.eye < 2 &&
                        role.sourceAddressObserved == origin.source[role.eye])
                        ++matched;
                }
                if (matched != 2 || !canonical ||
                    !observedTextureAlias(canonical->sourceAddressObserved, graph.writes[2 + frame].source))
                    return false;
                graph.writes[2 + frame].hostOrigin = canonical->sourceAddressObserved;
                graph.writes[2 + frame].hostWrite = canonical->sourceWriteEpoch;
            }
            return true;
        }
        uint64_t moduleIndex(const NrPacketV1& packet, uint64_t pointer) noexcept {
            for (uint64_t i = 0; i < packet.header.moduleCount; ++i) {
                const auto& module = packet.modules[i];
                if (pointer >= module.baseObserved && pointer - module.baseObserved < module.imageSize)
                    return i;
            }
            return NR_ABSENT;
        }
        uint64_t sourceSlot(unsigned image) noexcept {
            return image < 4 ? static_cast<uint64_t>(FgRef::colourA) + image : graph.tickets[1].bank * 4;
        }
        bool appendMetadata() noexcept {
            auto& p = graph.final[1];
            auto& h = p.header;
            const auto module = moduleIndex(p, address(reinterpret_cast<const void*>(&fgDraw)));
            if (module == NR_ABSENT)
                return false;
            std::array<uint64_t, 35> refIndices{};
            refIndices.fill(NR_ABSENT);
            for (unsigned i = 0; i < 35; ++i)
                if (auto* pointer = graph.ledger.borrow(static_cast<FgRef>(i))) {
                    if (h.refCount == 128)
                        return false;
                    refIndices[i] = h.refCount;
                    auto& ref = p.refs[h.refCount++];
                    ref.borrowedIUnknownAddress = address(pointer);
                    ref.interfaceEnum = graph.types[i];
                    ref.roleMask =
                        (i >= static_cast<unsigned>(FgRef::readback0) && i <= static_cast<unsigned>(FgRef::readback4))
                            ? 2
                            : 1;
                    if (ref.interfaceEnum == NR_DEVICE12 || ref.interfaceEnum == NR_DEVICE11)
                        ref.roleMask = 4;
                    else if (ref.interfaceEnum == NR_QUEUE12)
                        ref.roleMask = 8;
                    else if (ref.interfaceEnum == NR_LIST12)
                        ref.roleMask = 16;
                    else if (ref.interfaceEnum == NR_ALLOCATOR12)
                        ref.roleMask = 32;
                    else if (ref.interfaceEnum == NR_FENCE12 || ref.interfaceEnum == NR_FENCE11)
                        ref.roleMask = 64;
                    else if (ref.interfaceEnum != NR_RESOURCE12)
                        ref.roleMask = 128;
                    ref.groupIndex = NR_ABSENT;
                    ref.owningSourceLifetimeEpoch = graph.lifetimes[i];
                    ref.observedNonNull = 1;
                    if (i >= static_cast<unsigned>(FgRef::readback0) && i <= static_cast<unsigned>(FgRef::readback4)) {
                        ref.chargedPrivateBytes = graph.images[i - static_cast<unsigned>(FgRef::readback0)].allocation;
                        ref.physicalAllocationId = address(pointer);
                    }
                }
            std::array<uint64_t, 5> roleDescriptors{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& observed = graph.draws[eye];
                if (h.drawCount == 16 || h.descriptorCount + observed.descriptorCount > 64 ||
                    h.constantsUsedBytes + 32 + fgConstantBytes > 4096)
                    return false;
                auto& draw = p.draws[h.drawCount++];
                draw = observed.row;
                draw.shaderModuleIndex = module;
                draw.shaderHashOffsetInConstants = h.constantsUsedBytes;
                std::memcpy(p.constants + h.constantsUsedBytes, observed.shader.data(), 32);
                h.constantsUsedBytes += 32;
                draw.constantsOffset = h.constantsUsedBytes;
                draw.constantsBytes = fgConstantBytes;
                std::memcpy(p.constants + h.constantsUsedBytes, observed.constants.data(), fgConstantBytes);
                h.constantsUsedBytes += fgConstantBytes;
                draw.descriptorFirst = h.descriptorCount;
                draw.descriptorCount = observed.descriptorCount;
                for (unsigned i = 0; i < observed.descriptorCount; ++i) {
                    const auto& observedDescriptor = observed.descriptors[i];
                    unsigned refSlot = i < 4    ? static_cast<unsigned>(sourceSlot(i))
                                       : i == 4 ? static_cast<unsigned>(FgRef::b1)
                                                : static_cast<unsigned>(sourceSlot(4));
                    if (i == 6) {
                        refSlot = 8;
                        for (unsigned output = 0; output < 8; ++output)
                            if (address(graph.outputs[output]) == observedDescriptor.resource)
                                refSlot = output;
                        if (refSlot == 8)
                            return false;
                    }
                    auto& descriptor = p.descriptors[h.descriptorCount];
                    if ((!eye && i < 4) || (eye == 1 && i == 5))
                        roleDescriptors[i < 4 ? i : 4] = h.descriptorCount;
                    ++h.descriptorCount;
                    descriptor.kind = i < 4 ? NR_DESC_SRV : i == 4 ? NR_DESC_CBV : NR_DESC_RTV;
                    descriptor.objectAddress = observedDescriptor.cpu;
                    descriptor.objectLifetimeEpoch = observedDescriptor.epoch;
                    descriptor.heapAddress = observedDescriptor.heap;
                    descriptor.heapSlot = observedDescriptor.slot;
                    descriptor.descriptorIncrement = observedDescriptor.increment;
                    descriptor.cpuHandle = observedDescriptor.cpu;
                    descriptor.gpuHandle = observedDescriptor.gpu;
                    descriptor.bindEpoch = observedDescriptor.epoch;
                    descriptor.writeEpoch = observedDescriptor.epoch;
                    descriptor.resourceAddress = observedDescriptor.resource;
                    descriptor.resourceLifetimeEpoch = graph.lifetimes[refSlot];
                    descriptor.sourceWriteEpoch = i < 4    ? graph.writes[i].epoch
                                                  : i == 4 ? graph.b1Write[eye]
                                                           : observed.row.resourceWriteEpoch;
                    descriptor.apiDescriptionBytes = observedDescriptor.bytes;
                    descriptor.descriptionKind = observedDescriptor.kind;
                    descriptor.observedMask = 15;
                    std::copy(observedDescriptor.description.begin(),
                              observedDescriptor.description.end(),
                              descriptor.canonicalDescriptionWords);
                }
            }
            for (unsigned i = 0; i < 5; ++i) {
                if (h.roleCount == 64 || h.writeCount == 128)
                    return false;
                const auto roleIndex = h.roleCount++;
                auto& role = p.roles[roleIndex];
                const auto& image = graph.images[i];
                const auto& d = image.description;
                const auto slot = sourceSlot(i);
                const auto frame = i < 4 ? i % 2 : 1;
                const auto& origin = graph.final[frame].identity;
                role.role = i < 2 ? NR_FG_COLOUR : i < 4 ? NR_FG_DEPTH : NR_GENERATED;
                role.eye = 2;
                role.phase = frame;
                role.groupIndex = NR_ABSENT;
                role.sourceRefIndex = refIndices[slot];
                role.snapshotRefIndex = refIndices[static_cast<unsigned>(FgRef::readback0) + i];
                if (role.sourceRefIndex == NR_ABSENT || role.snapshotRefIndex == NR_ABSENT)
                    return false;
                role.sourceAddressObserved = p.refs[role.sourceRefIndex].borrowedIUnknownAddress;
                role.snapshotAddressObserved = p.refs[role.snapshotRefIndex].borrowedIUnknownAddress;
                role.sourceLifetimeEpoch = graph.lifetimes[slot];
                role.sourceWriteEpoch = i < 4 ? graph.writes[i].epoch : graph.selectedWrite;
                role.snapshotWriteEpoch = image.copyEpoch;
                role.descriptorFirst = roleDescriptors[i];
                role.descriptorCount = 1;
                role.descriptorEpoch = p.descriptors[role.descriptorFirst].writeEpoch;
                role.sourceOccurrence = origin.sourceOccurrence;
                role.hostSerial = origin.hostSerial;
                role.subresourceCount = d.DepthOrArraySize;
                role.resourceDimension = d.Dimension;
                role.width = d.Width;
                role.height = d.Height;
                role.depthOrArray = d.DepthOrArraySize;
                role.mipLevels = d.MipLevels;
                role.format = d.Format;
                role.sampleCount = d.SampleDesc.Count;
                role.sampleQuality = d.SampleDesc.Quality;
                role.layout = d.Layout;
                role.resourceFlags = d.Flags;
                role.viewFormat = p.descriptors[role.descriptorFirst].canonicalDescriptionWords[0];
                role.viewDimension = p.descriptors[role.descriptorFirst].canonicalDescriptionWords[1];
                role.rectWidth = role.cropWidth = d.Width;
                role.rectHeight = role.cropHeight = d.Height;
                role.copyMethod = 2;
                role.copyBeforeState = image.before;
                role.copyAfterState = image.after;
                role.copyListAddress = image.copyList;
                role.recordingEpoch = image.recording;
                role.payloadBytes = image.bytes;
                role.chargedBytes = image.allocation;
                role.observedMask = 15;
                role.aliasIndexOrAbsent = role.callIndexOrAbsent = NR_ABSENT;
                role.footprintFirst = h.footprintCount;
                role.footprintCount = d.DepthOrArraySize;
                for (UINT sub = 0; sub < d.DepthOrArraySize; ++sub) {
                    if (h.footprintCount == 128)
                        return false;
                    auto& footprint = p.footprints[h.footprintCount++];
                    footprint.roleIndex = roleIndex;
                    footprint.subresourceIndex = sub;
                    footprint.offset = image.footprints[sub].Offset;
                    footprint.rowPitch = image.footprints[sub].Footprint.RowPitch;
                    footprint.rowBytes = image.rowBytes[sub];
                    footprint.rowCount = image.rows[sub];
                    footprint.depth = 1;
                    footprint.totalBytes = footprint.rowPitch * footprint.rowCount;
                }
                auto& write = p.writes[h.writeCount++];
                write.resourceAddress = role.sourceAddressObserved;
                write.resourceLifetime = role.sourceLifetimeEpoch;
                write.writeEpoch = role.sourceWriteEpoch;
                write.sourceOccurrence = role.sourceOccurrence;
                write.phase = role.phase;
                write.listOrContextAddress = i < 4 ? graph.writes[i].list : graph.selectedList;
                write.recordingEpoch = i < 4 ? graph.writes[i].recording : graph.selectedRecording;
                write.callOrRoleIndex = roleIndex;
                h.requiredRoleMaskLo |= NR_ROLE_BIT(role.role);
                h.presentRoleMaskLo |= NR_ROLE_BIT(role.role);
            }
            return true;
        }
        bool appendAdmissionAndRetire() noexcept {
            for (unsigned frame = 0; frame < 2; ++frame) {
                auto& packet = graph.final[frame];
                packet.header.flags = 1;
                auto& identity = packet.identity;
                identity.ticketGeneration = graph.tickets[frame].generation;
                identity.previousSerial = graph.tickets[frame].previous_serial;
                identity.currentSerial = graph.tickets[frame].current_serial;
                identity.observedIdentityMask |= 128;
                unsigned mask = 0;
                for (uint64_t i = 0; i < graph.journal.count; ++i) {
                    const auto& event = graph.journal.rows[i];
                    if (event.serial != identity.hostSerial ||
                        (event.kind != FgEvent::admission && event.kind != FgEvent::retire) ||
                        (event.result & 0x80000000u))
                        continue;
                    const auto bit = event.kind == FgEvent::admission ? 1u : 2u;
                    if (mask & bit)
                        return false;
                    mask |= bit;
                    if (packet.header.callCount == 160)
                        return false;
                    auto& call = packet.calls[packet.header.callCount++];
                    call.kind = event.kind == FgEvent::admission ? NR_ADMISSION : NR_RETIRE;
                    call.beginObserved = call.endObserved = 1;
                    call.resultType = NR_HRESULT;
                    call.resultBits = event.result;
                    call.classifiedSuccess = 1;
                    call.qpcBegin = event.aux[6];
                    call.qpcEnd = event.qpcObserved;
                    call.thread = event.thread;
                    call.eye = 2;
                    call.moduleIndex = moduleIndex(packet, address(reinterpret_cast<const void*>(&fgHost)));
                    call.contextAddress = address(graph.context);
                    call.contextLifetimeEpoch = identity.fgContextLifetimeEpoch;
                    call.aux.auxiliaryValue = graph.tickets[frame].token;
                    call.observedMask = 7;
                    if (!call.qpcBegin || call.moduleIndex == NR_ABSENT)
                        return false;
                }
                if (mask != 3)
                    return false;
            }
            return true;
        }
        bool waitOwned(FgRef slot, uint64_t target, unsigned eventIndex) noexcept {
            auto* fence = static_cast<ID3D12Fence*>(graph.ledger.borrow(slot));
            if (!fence || !target || target == UINT64_MAX)
                return false;
            auto value = fence->GetCompletedValue();
            if (value == UINT64_MAX)
                return false;
            if (value < target) {
                HANDLE completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!completion)
                    return false;
                if (FAILED(fence->SetEventOnCompletion(target, completion))) {
                    CloseHandle(completion);
                    return false;
                }
                const auto result = WaitForSingleObject(completion, 15000);
                value = fence->GetCompletedValue();
                if (result != WAIT_OBJECT_0 || value == UINT64_MAX || value < target) {
                    std::lock_guard lock(graph.mutex);
                    graph.waitEvents[eventIndex] = completion;
                    return false;
                }
                CloseHandle(completion);
            }
            auto row = event(FgEvent::completed);
            row.fence = address(fence);
            row.value = value;
            row.aux[0] = target;
            std::lock_guard lock(graph.mutex);
            auto* saved = graph.journal.append(row);
            if (!saved)
                return false;
            if (slot == FgRef::productFence) {
                graph.productCompletedSequence = saved->sequence;
                graph.ownership.productComplete = true;
            } else {
                graph.copyCompletedSequence = saved->sequence;
                graph.ownership.copyComplete = true;
                graph.changed.notify_all();
            }
            return true;
        }
        bool releaseSupplemental(const CopyOwnership& ownership, bool readersClosed) noexcept {
            uint64_t mask = 0, count = 0;
            for (unsigned slot = 0; slot < 35; ++slot)
                if (graph.ledger.borrow(static_cast<FgRef>(slot))) {
                    mask |= uint64_t{1} << slot;
                    ++count;
                }
            if (!count || !graph.ledger.close(ownership, readersClosed) || !graph.ledger.release())
                return false;
            auto observed = event(FgEvent::supplementalReleased);
            observed.serial = graph.serialB;
            observed.value = mask;
            observed.count = count;
            std::lock_guard lock(graph.mutex);
            return graph.journal.append(observed) != nullptr;
        }
        bool writeArtifactFiles(const std::filesystem::path&);
        DWORD WINAPI worker(void*) noexcept {
            try {
                if (!waitOwned(FgRef::productFence, graph.tickets[1].output_ready_value, 6)) {
                    failed();
                    return 1;
                }
                auto* commands = static_cast<ID3D12GraphicsCommandList*>(graph.ledger.borrow(FgRef::copyList));
                auto* allocator = static_cast<ID3D12CommandAllocator*>(graph.ledger.borrow(FgRef::copyAllocator));
                auto* fence = static_cast<ID3D12Fence*>(graph.ledger.borrow(FgRef::copyFence));
                if (!commands || !allocator || !fence) {
                    failed();
                    return 1;
                }
                auto result = commands->Reset(allocator, nullptr);
                const auto resetTime = timestamp();
                const auto thread = GetCurrentThreadId();
                {
                    std::lock_guard lock(graph.mutex);
                    if (!graph.journal.reset(
                            address(commands), address(allocator), static_cast<uint32_t>(result), resetTime, thread)) {
                        graph.failed = true;
                        return 1;
                    }
                }
                ReadCopyPlan plan{};
                {
                    std::lock_guard lock(graph.mutex);
                    for (const auto& state : graph.states)
                        if (state.resource == graph.outputs[graph.tickets[1].bank * 4] && state.read.known &&
                            !state.read.invalid)
                            plan = {
                                true, state.read.value != D3D12_RESOURCE_STATE_COPY_SOURCE, false, state.read.value};
                }
                if (!plan.valid || !copyImage(4, commands, plan)) {
                    failed();
                    return 1;
                }
                result = commands->Close();
                const auto closeTime = timestamp();
                {
                    std::lock_guard lock(graph.mutex);
                    if (!graph.journal.closed(address(commands), static_cast<uint32_t>(result), closeTime, thread)) {
                        graph.failed = true;
                        return 1;
                    }
                    graph.ownership.copyQueued = true;
                }
                ID3D12CommandList* lists[]{commands};
                graph.queue->ExecuteCommandLists(1, lists);
                const auto executeTime = timestamp();
                const uint64_t ids[]{address(commands)};
                {
                    std::lock_guard lock(graph.mutex);
                    if (!graph.journal.execute(address(graph.queue), ids, 1, executeTime, thread)) {
                        graph.failed = true;
                        return 1;
                    }
                }
                result = graph.queue->Signal(fence, 1);
                auto signal = event(FgEvent::signal);
                signal.queue = address(graph.queue);
                signal.fence = address(fence);
                signal.value = 1;
                signal.result = static_cast<uint32_t>(result);
                {
                    std::lock_guard lock(graph.mutex);
                    if (!graph.journal.append(signal)) {
                        graph.failed = true;
                        return 1;
                    }
                }
                if (FAILED(result) || !waitOwned(FgRef::copyFence, 1, 7)) {
                    failed();
                    return 1;
                }
                const std::filesystem::path output(graph.output.data());
                if (!std::filesystem::create_directory(output)) {
                    failed();
                    return 1;
                }
                for (unsigned frame = 0; frame < 2; ++frame)
                    if (!writeHostFrame(receiver(),
                                        frame,
                                        output,
                                        graph.final[frame],
                                        graph.payloads[frame],
                                        graph.written,
                                        graph.waitEvents + frame * 3)) {
                        failed();
                        return 1;
                    }
                {
                    std::unique_lock lock(graph.mutex);
                    if (!graph.changed.wait_for(lock,
                                                std::chrono::seconds(30),
                                                [] {
                                                    return graph.failed ||
                                                           (graph.retiredA && graph.retiredB && graph.nativeCount &&
                                                            graph.nativeDependencySequence);
                                                }) ||
                        graph.failed) {
                        lock.unlock();
                        failed();
                        return 1;
                    }
                    graph.journalClosed = true;
                }
                if (!bindOrigins() || !appendAdmissionAndRetire() || !appendMetadata()) {
                    failed();
                    return 1;
                }
                NrError error{};
                const auto* originalA = receiver().inspect(0);
                const auto* originalB = receiver().inspect(1);
                if (!originalA || !originalB || !nr_validate_final_pair(&graph.final[0], &graph.final[1], &error) ||
                    !nr_capture_reference_reservation(&graph.final[0]) ||
                    !nr_capture_reference_reservation(&graph.final[1]) || !hostExtension(*originalA, graph.final[0]) ||
                    !hostExtension(*originalB, graph.final[1])) {
                    failed();
                    return 1;
                }
                if (!writeArtifactFiles(output)) {
                    failed();
                    return 1;
                }
                {
                    std::lock_guard lock(graph.mutex);
                    graph.workerFinished = true;
                }
                return 0;
            } catch (...) {
                failed();
                return 1;
            }
        }
        struct FileRecord {
            std::array<char, 64> name{};
            uint64_t size{};
            std::array<uint8_t, 32> sha{};
        };
        void hashJson(std::ostream& stream, const std::array<uint8_t, 32>& hash) {
            constexpr char hex[] = "0123456789abcdef";
            stream << '"';
            for (auto value : hash)
                stream << hex[value >> 4] << hex[value & 15];
            stream << '"';
        }
        void fileJson(std::ostream& stream, const FileRecord& file) {
            stream << "{\"path\":\"" << file.name.data() << "\",\"size\":" << file.size << ",\"sha256\":";
            hashJson(stream, file.sha);
            stream << '}';
        }
        bool fileRecord(const std::filesystem::path& directory, const char* name, FileRecord& record) {
            if (strnlen_s(name, record.name.size()) >= record.name.size())
                return false;
            strcpy_s(record.name.data(), record.name.size(), name);
            record.size = std::filesystem::file_size(directory / name);
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE hash{};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                return false;
            if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return false;
            }
            std::array<char, 65536> buffer{};
            std::ifstream file(directory / name, std::ios::binary);
            bool okay = bool(file);
            uint64_t read = 0;
            while (okay && file) {
                file.read(buffer.data(), buffer.size());
                const auto count = file.gcount();
                if (count > 0) {
                    read += static_cast<uint64_t>(count);
                    okay = BCryptHashData(
                               hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) >= 0;
                }
            }
            okay = okay && file.eof() && read == record.size && BCryptFinishHash(hash, record.sha.data(), 32, 0) >= 0;
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return okay;
        }
        bool persist(const std::filesystem::path& directory,
                     const char* name,
                     const void* bytes,
                     size_t size,
                     FileRecord& record) {
            std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
            file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
            file.close();
            return file.good() && fileRecord(directory, name, record);
        }
        bool writeJournal(const std::filesystem::path& directory, FileRecord& record) {
            std::ofstream out(directory / "fg-journal.json", std::ios::binary);
            out << "{\"schema\":\"vdxr.nr.fg-journal.v2\",\"overflow\":" << (graph.journal.overflow ? "true" : "false")
                << ",\"source_lifetime_basis\":\"retained-reference-interval\",\"original_object_creation_epoch\":null,"
                   "\"rows\":[";
            for (uint64_t i = 0; i < graph.journal.count; ++i) {
                const auto& row = graph.journal.rows[i];
                out << (i ? "," : "") << "{\"kind\":" << static_cast<uint64_t>(row.kind)
                    << ",\"sequence\":" << row.sequence << ",\"qpc_observed\":" << row.qpcObserved
                    << ",\"thread\":" << row.thread << ",\"list\":" << row.list << ",\"allocator\":" << row.allocator
                    << ",\"recording\":" << row.recording << ",\"queue\":" << row.queue << ",\"fence\":" << row.fence
                    << ",\"value\":" << row.value << ",\"source\":" << row.source << ",\"target\":" << row.target
                    << ",\"serial\":" << row.serial << ",\"epoch\":" << row.epoch << ",\"result\":" << row.result
                    << ",\"count\":" << row.count << ",\"aux\":[";
                for (unsigned j = 0; j < 8; ++j)
                    out << (j ? "," : "") << row.aux[j];
                out << "]}";
            }
            out << "],\"producer_origins\":[";
            for (unsigned frame = 0; frame < 2; ++frame) {
                const auto& origin = graph.origins[graph.final[frame].identity.hostSerial % 2];
                const auto& write = graph.writes[frame + 2];
                out << (frame ? "," : "") << "{\"frame\":" << frame << ",\"serial\":" << origin.serial
                    << ",\"colour11\":" << origin.target11 << ",\"colour12\":" << origin.target12
                    << ",\"colour_sources\":[" << origin.source[0] << ',' << origin.source[1] << "]"
                    << ",\"canonical11\":" << write.hostOrigin << ",\"canonical_write\":" << write.hostWrite
                    << ",\"guide12\":" << write.source << ",\"guide_history\":" << write.target
                    << ",\"guide_write\":" << write.epoch << '}';
            }
            out << "],\"draw_bindings\":[";
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& draw = graph.draws[eye];
                out << (eye ? "," : "") << "{\"eye\":" << eye << ",\"list\":" << draw.row.listOrContext
                    << ",\"recording\":" << draw.row.recordingEpoch << ",\"pso\":" << draw.row.psoAddress
                    << ",\"root\":" << draw.row.rootAddress << ",\"write\":" << draw.row.resourceWriteEpoch
                    << ",\"root_table0\":" << draw.row.rootTable0 << ",\"root_table1\":" << draw.row.rootTable1
                    << ",\"vertex_count\":" << draw.row.vertexCount << ",\"instance_count\":" << draw.row.instanceCount
                    << ",\"start_vertex\":" << draw.row.startVertex << ",\"start_instance\":" << draw.row.startInstance
                    << ",\"viewport_bits\":[";
                const uint64_t viewport[]{draw.row.viewportXbits,
                                          draw.row.viewportYbits,
                                          draw.row.viewportWbits,
                                          draw.row.viewportHbits,
                                          draw.row.minDepthBits,
                                          draw.row.maxDepthBits};
                for (unsigned i = 0; i < 6; ++i)
                    out << (i ? "," : "") << viewport[i];
                out << "],\"scissor_bits\":[";
                const uint64_t scissor[]{draw.row.scissorLeftBits,
                                         draw.row.scissorTopBits,
                                         draw.row.scissorRightBits,
                                         draw.row.scissorBottomBits};
                for (unsigned i = 0; i < 4; ++i)
                    out << (i ? "," : "") << scissor[i];
                out << "],\"shader_sha256\":";
                hashJson(out, draw.shader);
                out << ",\"constants_hex\":\"";
                constexpr char hex[] = "0123456789abcdef";
                for (auto value : draw.constants)
                    out << hex[value >> 4] << hex[value & 15];
                out << "\",\"descriptors\":[";
                for (unsigned i = 0; i < draw.descriptorCount; ++i) {
                    const auto& descriptor = draw.descriptors[i];
                    out << (i ? "," : "") << "{\"kind\":" << descriptor.kind << ",\"resource\":" << descriptor.resource
                        << ",\"heap\":" << descriptor.heap << ",\"cpu\":" << descriptor.cpu
                        << ",\"gpu\":" << descriptor.gpu << ",\"epoch\":" << descriptor.epoch
                        << ",\"slot\":" << descriptor.slot << ",\"increment\":" << descriptor.increment
                        << ",\"eye\":" << descriptor.eye << ",\"offset\":" << descriptor.offset
                        << ",\"api_bytes\":" << descriptor.bytes << ",\"qpc_observed\":" << descriptor.qpc
                        << ",\"thread\":" << descriptor.thread << ",\"words\":[";
                    for (unsigned w = 0; w < 16; ++w)
                        out << (w ? "," : "") << descriptor.description[w];
                    out << "]}";
                }
                out << "]}";
            }
            out << "]}\n";
            out.close();
            return out.good() && fileRecord(directory, "fg-journal.json", record);
        }
        bool sameNativeIdentity(const Host& reuse, const Host& fresh) noexcept {
            const auto identity = [](const Host& h) noexcept {
                const auto& r = h.output;
                return OutputIdentity{address(h.context),
                                      r.hostGeneration,
                                      h.ticket.token,
                                      h.ticket.generation,
                                      h.ticket.bank,
                                      r.role,
                                      r.source11,
                                      r.source12,
                                      r.destination11,
                                      r.fence11};
            };
            const SuccessfulCopy prior{identity(fresh), fresh.output.copyId, fresh.output.consumerValue};
            return preparedMatches(identity(reuse),
                                   prior,
                                   reuse.output.roleValueBefore,
                                   reuse.output.retainedDestination11Before) &&
                   reuse.output.hostContext11 == fresh.output.hostContext11;
        }
        bool validNativeLineage(
            const Host* rows, size_t count, void* context, const VDXRFG_TicketV1& ticket, uint64_t source12) noexcept {
            if (!rows || !count || count > 16 || !context || !ticket.token || !ticket.generation || ticket.bank > 1 ||
                !ticket.current_serial || !source12)
                return false;
            size_t freshCount{};
            for (size_t i = 0; i < count; ++i) {
                const auto& host = rows[i];
                const auto& row = host.output;
                if (host.operation != 7 || host.context != context || host.originalResult != S_OK ||
                    host.serial != ticket.current_serial || host.ticket.current_serial != ticket.current_serial ||
                    host.ticket.token != ticket.token || host.ticket.generation != ticket.generation ||
                    host.ticket.bank != ticket.bank || row.role != 0 || row.source12 != source12 ||
                    !row.hostGeneration || row.ownerGeneration != row.hostGeneration || !row.hostContext11 ||
                    !row.fence11 || !row.source11 || !row.destination11 || !row.callId || !row.copyId ||
                    row.destinationRetentionEpoch != row.copyId || !row.consumerValue || !row.descriptionObserved ||
                    !row.callQpc || row.returnQpc < row.callQpc ||
                    (i && (row.callId <= rows[i - 1].output.callId || row.callQpc < rows[i - 1].output.returnQpc)))
                    return false;
                if (!row.prepared) {
                    if (freshCount || row.preparedReferenceValid || !row.copyCalled || !row.signalCalled ||
                        !row.flushCalled || FAILED(row.signalResult) || row.copyId != row.callId ||
                        row.roleValueBefore || row.copyQpc < row.callQpc || row.signalQpc < row.copyQpc ||
                        row.flushQpc < row.signalQpc || row.returnQpc < row.flushQpc)
                        return false;
                    for (size_t j = 0; j < i; ++j)
                        if (rows[j].output.copyId == row.copyId)
                            return false;
                    ++freshCount;
                } else {
                    if (!row.preparedReferenceValid || row.copyCalled || row.signalCalled || row.flushCalled ||
                        row.copyQpc || row.signalQpc || row.flushQpc || row.signalResult != E_PENDING)
                        return false;
                    const Host* fresh{};
                    for (size_t j = 0; j < i; ++j)
                        if (!rows[j].output.prepared && rows[j].output.copyId == row.copyId) {
                            if (fresh)
                                return false;
                            fresh = &rows[j];
                        }
                    if (!fresh || !sameNativeIdentity(host, *fresh) ||
                        row.consumerValue != fresh->output.consumerValue || row.callId <= fresh->output.callId ||
                        row.callQpc < fresh->output.returnQpc)
                        return false;
                }
            }
            // This graph contains only one selected ticket's role0 lifetime. Its first
            // successful copy sets roleConsumerValues; later calls must be prepared.
            return freshCount == 1;
        }
        bool writeNative(const std::filesystem::path& directory, FileRecord& record) {
            // A prepared call reuses one actual earlier copy. It does not issue another
            // Copy/Signal/Flush, so those two observed paths have distinct shapes.
            if (graph.tickets[1].bank > 1 || !validNativeLineage(graph.native.data(),
                                                                 graph.nativeCount,
                                                                 graph.context,
                                                                 graph.tickets[1],
                                                                 address(graph.outputs[graph.tickets[1].bank * 4])))
                return false;
            std::ofstream out(directory / "native-output-lineage.json", std::ios::binary);
            out << "{\"schema\":2,\"private_host_version\":2,\"overflow\":false,\"rows\":[";
            for (size_t i = 0; i < graph.nativeCount; ++i) {
                const auto& host = graph.native[i];
                const auto& row = host.output;
                out << (i ? "," : "") << "{\"ticket_token\":" << host.ticket.token
                    << ",\"ticket_bank\":" << host.ticket.bank << ",\"ticket_generation\":" << host.ticket.generation
                    << ",\"serial\":" << host.serial << ",\"role\":" << row.role << ",\"source12\":" << row.source12
                    << ",\"source11\":" << row.source11 << ",\"destination11\":" << row.destination11
                    << ",\"copy_id\":" << row.copyId
                    << ",\"destination_retention_epoch\":" << row.destinationRetentionEpoch
                    << ",\"context\":" << address(host.context) << ",\"host_generation\":" << row.hostGeneration
                    << ",\"owner_generation\":" << row.ownerGeneration << ",\"host_context11\":" << row.hostContext11
                    << ",\"fence11\":" << row.fence11 << ",\"call_id\":" << row.callId
                    << ",\"consumer_value\":" << row.consumerValue << ",\"role_value_before\":" << row.roleValueBefore
                    << ",\"retained_destination11_before\":" << row.retainedDestination11Before
                    << ",\"description_observed\":" << (row.descriptionObserved ? "true" : "false")
                    << ",\"prepared\":" << (row.prepared ? "true" : "false")
                    << ",\"prepared_reference_valid\":" << (row.preparedReferenceValid ? "true" : "false")
                    << ",\"copy_called\":" << (row.copyCalled ? "true" : "false")
                    << ",\"signal_called\":" << (row.signalCalled ? "true" : "false")
                    << ",\"flush_called\":" << (row.flushCalled ? "true" : "false")
                    << ",\"result\":" << host.originalResult << ",\"signal_hresult\":" << row.signalResult
                    << ",\"call_qpc\":" << row.callQpc << ",\"copy_qpc\":" << row.copyQpc
                    << ",\"signal_qpc\":" << row.signalQpc << ",\"flush_qpc\":" << row.flushQpc
                    << ",\"return_qpc\":" << row.returnQpc << '}';
            }
            out << "]}\n";
            out.close();
            return graph.nativeCount && out.good() && fileRecord(directory, "native-output-lineage.json", record);
        }
        bool writeDebug(const std::filesystem::path& directory, FileRecord& record) {
            std::ofstream out(directory / "debug-observations.json", std::ios::binary);
            out << "{\"schema\":1,\"description_policy\":\"hash-only\",\"queues\":[";
            for (unsigned api = 0; api < 2; ++api) {
                auto* q11 = api ? nullptr : static_cast<ID3D11InfoQueue*>(graph.ledger.borrow(FgRef::info11));
                auto* q12 = api ? static_cast<ID3D12InfoQueue*>(graph.ledger.borrow(FgRef::info12)) : nullptr;
                const auto begin = api ? graph.info12Begin : graph.info11Begin;
                const uint64_t end = q11   ? q11->GetNumStoredMessagesAllowedByRetrievalFilter()
                                     : q12 ? q12->GetNumStoredMessagesAllowedByRetrievalFilter()
                                           : 0;
                if (end < begin || end - begin > 512)
                    return false;
                out << (api ? "," : "") << "{\"api\":" << (api ? 12 : 11)
                    << ",\"query_hresult\":" << (api ? graph.info12Result : graph.info11Result)
                    << ",\"available\":" << ((q11 || q12) ? "true" : "false") << ",\"begin\":" << begin
                    << ",\"end\":" << end << ",\"messages\":[";
                alignas(D3D12_MESSAGE) std::array<unsigned char, 65536> bytes{};
                for (uint64_t i = begin; i < end; ++i) {
                    SIZE_T size = bytes.size();
                    HRESULT result;
                    uint64_t severity{}, category{}, id{};
                    const char* description{};
                    size_t length{};
                    if (q11) {
                        auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                        result = q11->GetMessage(i, message, &size);
                        if (SUCCEEDED(result)) {
                            severity = message->Severity;
                            category = message->Category;
                            id = message->ID;
                            description = message->pDescription;
                            length = message->DescriptionByteLength;
                        }
                    } else {
                        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                        result = q12->GetMessage(i, message, &size);
                        if (SUCCEEDED(result)) {
                            severity = message->Severity;
                            category = message->Category;
                            id = message->ID;
                            description = message->pDescription;
                            length = message->DescriptionByteLength;
                        }
                    }
                    std::array<uint8_t, 32> hash{};
                    if (FAILED(result) || size > bytes.size() || !description || length > bytes.size() ||
                        BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                                   nullptr,
                                   0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(description)),
                                   static_cast<ULONG>(length),
                                   hash.data(),
                                   32) < 0)
                        return false;
                    out << (i != begin ? "," : "") << "{\"index\":" << i << ",\"category\":" << category
                        << ",\"severity\":" << severity << ",\"id\":" << id << ",\"description_bytes\":" << length
                        << ",\"description_sha256\":";
                    hashJson(out, hash);
                    out << '}';
                }
                out << "]}";
            }
            out << "]}\n";
            out.close();
            return out.good() && fileRecord(directory, "debug-observations.json", record);
        }
        bool writeArtifactFiles(const std::filesystem::path& output) {
            for (unsigned i = 0; i < 5; ++i) {
                const auto index = graph.final[1].header.roleCount - 5 + i;
                const auto& role = graph.final[1].roles[index];
                auto* resource = static_cast<ID3D12Resource*>(
                    graph.ledger.borrow(static_cast<FgRef>(static_cast<unsigned>(FgRef::readback0) + i)));
                if (!resource || role.payloadBytes > graph.final[0].budget.privatePayloadReservation - graph.written)
                    return false;
                void* data{};
                const D3D12_RANGE range{0, static_cast<SIZE_T>(role.payloadBytes)};
                if (FAILED(resource->Map(0, &range, &data)))
                    return false;
                auto& payload = graph.payloads[1][index];
                payload.frame = 1;
                payload.roleIndex = index;
                payload.bytes = role.payloadBytes;
                payload.rowPitch = graph.images[i].footprints[0].Footprint.RowPitch;
                sprintf_s(payload.filename.data(), payload.filename.size(), "nr-B-fg-%u.bin", i);
                FileRecord record;
                bool okay = false;
                try {
                    okay = data &&
                           persist(output, payload.filename.data(), data, static_cast<size_t>(payload.bytes), record);
                } catch (...) {
                    const D3D12_RANGE none{};
                    resource->Unmap(0, &none);
                    throw;
                }
                const D3D12_RANGE none{};
                resource->Unmap(0, &none);
                if (!okay)
                    return false;
                payload.sha256 = record.sha;
                graph.written += payload.bytes;
            }
            std::array<FileRecord, 2> hosts{}, sources{}, finals{}, acks{}, postfx{};
            FileRecord journal, native, debug, preflight;
            if (!nr_fg_budget_matches_packet(&graph.preflight, receiver().inspect(0)) ||
                !nr_fg_budget_matches_packet(&graph.preflight, receiver().inspect(1)) ||
                !persist(output, "nr-fg-preflight.bin", &graph.preflight, sizeof(graph.preflight), preflight))
                return false;
            for (unsigned frame = 0; frame < 2; ++frame) {
                const auto* source = receiver().source(frame);
                const auto* facts = receiver().postFx(frame);
                const auto ack = receiver().acknowledgement(frame);
                if (!source || !facts || ack.status != NR_DEEP_OWNED ||
                    !fileRecord(output, frame ? "nr-host-B.packet" : "nr-host-A.packet", hosts[frame]) ||
                    !persist(output,
                             frame ? "nr-source-B.bin" : "nr-source-A.bin",
                             source,
                             sizeof(*source),
                             sources[frame]) ||
                    !persist(output,
                             frame ? "nr-final-B.packet" : "nr-final-A.packet",
                             &graph.final[frame],
                             sizeof(NrPacketV1),
                             finals[frame]) ||
                    !persist(output, frame ? "nr-ack-B.bin" : "nr-ack-A.bin", &ack, sizeof ack, acks[frame]) ||
                    !persist(
                        output, frame ? "nr-postfx-B.bin" : "nr-postfx-A.bin", facts, sizeof(*facts), postfx[frame]))
                    return false;
            }
            if (!writeNative(output, native) || !writeDebug(output, debug))
                return false;
            for (unsigned frame = 0; frame < 2; ++frame) {
                if (!receiver().closeReaders(frame))
                    return false;
                auto closed = event(FgEvent::readersClosed);
                closed.serial = graph.final[frame].identity.hostSerial;
                if (!graph.journal.append(closed) || !receiver().endWorker(frame))
                    return false;
                auto ended = event(FgEvent::workerEnd);
                ended.serial = closed.serial;
                if (!graph.journal.append(ended))
                    return false;
            }
            CopyOwnership ownership;
            {
                std::lock_guard lock(graph.mutex);
                graph.ownership.workerExited = true;
                ownership = graph.ownership;
            }
            if (!releaseSupplemental(ownership, true))
                return false;
            for (unsigned frame = 0; frame < 2; ++frame) {
                if (!receiver().release(frame))
                    return false;
                auto released = event(FgEvent::released);
                released.serial = graph.final[frame].identity.hostSerial;
                released.count = receiver().acknowledgement(frame).refCount;
                if (!graph.journal.append(released))
                    return false;
            }
            {
                std::lock_guard lock(graph.mutex);
                graph.released = true;
            }
            if (!writeJournal(output, journal))
                return false;
            std::ofstream out(output / "nr-capture.partial", std::ios::binary);
            out << "{\"schema\":\"vdxr.nr.capture.v1\",\"status\":\"buffers-captured-unadjudicated\",\"frames\":[";
            for (unsigned frame = 0; frame < 2; ++frame) {
                out << (frame ? "," : "") << "{\"host\":";
                fileJson(out, hosts[frame]);
                out << ",\"source\":";
                fileJson(out, sources[frame]);
                out << ",\"final\":";
                fileJson(out, finals[frame]);
                out << ",\"ack\":";
                fileJson(out, acks[frame]);
                out << ",\"postfx\":";
                fileJson(out, postfx[frame]);
                out << '}';
            }
            out << "],\"fg_preflight\":";
            fileJson(out, preflight);
            out << ",\"lifecycle\":[";
            for (unsigned frame = 0; frame < 2; ++frame) {
                const auto ack = receiver().acknowledgement(frame);
                out << (frame ? "," : "") << "{\"frame\":" << frame << ",\"commit_epoch\":" << ack.receiverCommitEpoch
                    << ",\"acquired_refs\":" << ack.refCount
                    << ",\"deep_ack\":true,\"worker_begin\":true,\"worker_end\":true,\"readers_closed\":true,"
                       "\"released_groups\":3,\"uncertain\":false}";
            }
            const auto& b = graph.tickets[1];
            out << "],\"fg\":{\"bank\":" << b.bank << ",\"token_a\":" << graph.tickets[0].token
                << ",\"token_b\":" << b.token << ",\"history\":" << b.generation << ",\"serial_a\":" << graph.serialA
                << ",\"serial_b\":" << graph.serialB
                << ",\"product_completed\":true,\"copy_completed\":true,\"host_output_wait\":"
                << (graph.ownership.hostWaitQueued ? "true" : "false")
                << ",\"copy_completed_before_native_call\":" << (graph.outputAlreadyComplete ? "true" : "false")
                << ",\"native_dependency_sequence\":" << graph.nativeDependencySequence
                << ",\"original_retired\":true,\"worker_exited\":true,\"supplemental_refs_released\":true,\"outputs\":"
                   "[";
            for (unsigned i = 0; i < 8; ++i)
                out << (i ? "," : "") << address(graph.outputs[i]);
            out << "],\"native_source12\":" << address(graph.outputs[b.bank * 4])
                << ",\"native_role\":0,\"native_ticket\":" << b.token << ",\"native_copy_called\":true},\"payloads\":[";
            bool first = true;
            for (unsigned frame = 0; frame < 2; ++frame)
                for (uint64_t i = 0; i < graph.final[frame].header.roleCount; ++i) {
                    const auto& payload = graph.payloads[frame][i];
                    out << (first ? "" : ",") << "{\"frame\":" << frame << ",\"role_index\":" << i << ",\"path\":\""
                        << payload.filename.data() << "\",\"size\":" << payload.bytes << ",\"sha256\":";
                    hashJson(out, payload.sha256);
                    out << '}';
                    first = false;
                }
            out << "],\"qualification\":{\"model\":\"captured-actual-answer-not-predicted\"";
            for (unsigned frame = 0; frame < 2; ++frame) {
                const auto& settings = graph.final[frame].identity.settings;
                out << ",\"easu_" << frame << "\":\""
                    << (settings.upscaling ? "captured-math-unadjudicated" : "not-applicable-observed-disabled") << '"';
                out << ",\"cas_" << frame << "\":\""
                    << (settings.sharpening ? "captured-math-unadjudicated" : "not-applicable-observed-disabled")
                    << '"';
            }
            out << "},\"fg_journal\":";
            fileJson(out, journal);
            out << ",\"native_output_lineage\":";
            fileJson(out, native);
            out << ",\"debug_observations\":";
            fileJson(out, debug);
            out << "}\n";
            out.close();
            return out.good();
        }
    } // namespace
    std::recursive_mutex& fgPublicationGate() noexcept {
        static std::recursive_mutex gate;
        return gate;
    }
    size_t fgFixedMetadataBytes() noexcept {
        return sizeof(Graph) + sizeof(Receiver);
    }
    void fgQuery() noexcept {
        std::lock_guard publication(fgPublicationGate());
        try {
            HANDLE workerHandle{};
            bool terminal;
            {
                std::lock_guard lock(graph.mutex);
                terminal = graph.failed;
            }
            if (terminal) {
                receiver().fail();
                return;
            }
            {
                std::lock_guard lock(graph.mutex);
                if (graph.published || graph.publishing || !graph.workerFinished || !graph.released ||
                    !graph.retiredA || !graph.retiredB || !graph.nativeCount)
                    return;
                graph.publishing = true;
                workerHandle = graph.worker;
            }
            if (!workerHandle || WaitForSingleObject(workerHandle, 0) != WAIT_OBJECT_0) {
                std::lock_guard lock(graph.mutex);
                graph.publishing = false;
                return;
            }
            DWORD code{};
            if (!GetExitCodeThread(workerHandle, &code) || code) {
                failed();
                return;
            }
            const std::filesystem::path output(graph.output.data());
            if (!MoveFileExW((output / "nr-capture.partial").c_str(),
                             (output / "nr-capture.json").c_str(),
                             MOVEFILE_WRITE_THROUGH) ||
                !receiver().artifactPublished()) {
                failed();
                return;
            }
            CloseHandle(workerHandle);
            {
                std::lock_guard lock(graph.mutex);
                graph.worker = nullptr;
                graph.published = true;
                graph.publishing = false;
            }
        } catch (...) {
            failed();
        }
    }
    uint32_t fgStatus(uint32_t) noexcept {
        fgQuery();
        std::lock_guard lock(graph.mutex);
        return 16u | (graph.reserved ? 32u : 0u) | (graph.workerStarted ? 64u : 0u) | (graph.published ? 1u : 0u) |
               (graph.failed ? 2u : 0u) | (graph.ownership.uncertain() ? 4u : 0u) |
               (graph.ownership.canRetire() ? 8u : 0u) | (graph.workerFinished ? 128u : 0u) |
               (graph.released ? 256u : 0u);
    }
} // namespace vdxrfg::pair_capture::nr
