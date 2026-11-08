#include "pair_capture_nr_fg.hpp"
#include "pair_capture_private.hpp"
#include "pair_capture_core.hpp"
#include "pair_capture_read_state.hpp"
#include "pair_capture_nr_private.hpp"
#include <wrl/client.h>
#include <dxgi1_2.h>
#include <bcrypt.h>
#include <intrin.h>
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <vector>

namespace vdxrfg::pair_capture {
    namespace {
        using Microsoft::WRL::ComPtr;
        static_assert(readPixel == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
                      readNonPixel == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
                      readCopySource == D3D12_RESOURCE_STATE_COPY_SOURCE &&
                      readCopyDest == D3D12_RESOURCE_STATE_COPY_DEST);
        struct Image11 {
            std::string role;
            Eye eye{};
            uintptr_t source{}, context{};
            uint64_t serial{};
            D3D11_TEXTURE2D_DESC original{};
            std::vector<ComPtr<ID3D11Texture2D>> staging;
        };
        struct Image12 {
            std::string role;
            ComPtr<ID3D12Resource> source, readback;
            D3D12_RESOURCE_DESC desc{};
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
            uint64_t bytes{};
            uintptr_t copyCommands{};
        };
        struct State {
            std::mutex mutex, debugMutex;
            Selector select;
            bool checked{}, requested{}, bound{}, armed{}, drawn{}, workerStarted{}, finished{};
            std::atomic<bool> gpuUnknown{};
            std::string nonce, session, failure, expectedCaller;
            std::filesystem::path output, armFile;
            uint64_t cap{}, minimumSerial{}, minimumTime{}, sequence{}, gpuBudget{}, executeSequence{};
            void* context{};
            void* slot{};
            ComPtr<ID3D12Device> device;
            ComPtr<ID3D12CommandQueue> queue;
            std::array<ComPtr<ID3D12Resource>, 8> outputs;
            std::array<ComPtr<ID3D12Resource>, 4> consumers;
            std::array<D3D12_SHADER_RESOURCE_VIEW_DESC, 4> srvs{};
            ComPtr<ID3D12DescriptorHeap> heap;
            uint32_t descriptorIncrement{};
            uint64_t guideA{}, guideB{}, guideEpoch{};
            std::array<D3D11_TEXTURE2D_DESC, 2> originalRaw{};
            std::vector<Image11> host;
            std::vector<Image12> gpu;
            std::ofstream events;
            VDXRFG_TicketV1 ticketA{}, ticketB{};
            ComPtr<ID3D12Fence> copyFence;
            ComPtr<ID3D11Fence> copyFence11;
            ComPtr<ID3D12CommandAllocator> copyAllocator;
            ComPtr<ID3D12GraphicsCommandList> copyList;
            struct Initial {
                std::string name;
                uintptr_t object{}, other{};
                uint64_t value{}, qpc{};
                HRESULT result{};
                uintptr_t context{};
                uint64_t serial{};
                DWORD thread{};
            };
            std::vector<Initial> initial;
            std::array<Initial, 4> early;
            UINT earlyCount{};
            uint64_t earlySerial{};
            struct Rtv {
                uintptr_t handle{};
                ID3D12Resource* resource{};
                D3D12_RENDER_TARGET_VIEW_DESC description{};
            };
            struct Targets {
                ID3D12GraphicsCommandList* commands{};
                UINT count{};
                D3D12_CPU_DESCRIPTOR_HANDLE handles[2]{};
                D3D12_VIEWPORT viewport{};
                D3D12_RECT scissor{};
                D3D12_GPU_DESCRIPTOR_HANDLE rootTables[2]{};
            };
            struct B1View {
                void* slot{};
                uintptr_t resource{}, heap{};
                UINT eye{}, index{}, offset{}, increment{};
                uint64_t qpc{}, cpu{}, gpu{};
                D3D12_CONSTANT_BUFFER_VIEW_DESC description{};
            };
            std::array<B1View, 64> b1Views{};
            std::array<Rtv, 64> rtvs{};
            std::array<Targets, 16> renderTargets{};
            VDXRFG_TicketV1 prospective{};
            uint32_t selectedEyeMask{};
            VDXRFG_TicketV1 inputTicket{};
            std::array<ID3D12Resource*, 2> colourHistory{};
            struct ResourceState {
                ID3D12Resource* resource{};
                TextureReadState read;
            };
            std::array<ResourceState, 256> resourceStates{};
            ComPtr<ID3D11Device> submissionDevice;
            ComPtr<ID3D11InfoQueue> info11;
            ComPtr<ID3D12InfoQueue> info12;
            HRESULT info11Result{E_NOINTERFACE}, info12Result{E_NOINTERFACE};
            uint64_t debugStart11{}, debugStart12{};
            bool coreSignalA{};
            bool retireReady{}, retireHeldReported{};
            ComPtr<ID3D12Fence> fullFenceB;
            ArmIdentity ready{};
            std::string readyNonce, armRecord;
            std::filesystem::path readyPath;
            bool workerSettled{}, released{};
            CopyOwnership ownership;
            std::array<HANDLE, 2> pendingWaitEvents{};
            bool effectiveNr[2]{};
            struct OutputRow {
                OutputRecord value{};
                VDXRFG_TicketV1 ticket{};
                uintptr_t context{};
                HRESULT result{};
                uint64_t serial{};
            };
            std::array<OutputRow, 2048> outputRows{};
            size_t outputCount{};
            bool outputOverflow{};

        } state;
        std::string jsonQuote(const std::string&);

        std::wstring environment(const wchar_t* name) {
            wchar_t value[4096]{};
            const DWORD size = GetEnvironmentVariableW(name, value, 4096);
            return size && size < 4096 ? std::wstring(value, size) : std::wstring{};
        }
        uint64_t number(const std::wstring& value) {
            if (value.empty())
                return 0;
            uint64_t result{};
            for (wchar_t c : value) {
                if (c < L'0' || c > L'9' || result > (UINT64_MAX - (c - L'0')) / 10)
                    return 0;
                result = result * 10 + c - L'0';
            }
            return result;
        }
        std::string ascii(const std::wstring& value) {
            std::string answer;
            for (wchar_t c : value) {
                if (c < 32 || c > 126)
                    return {};
                answer.push_back(static_cast<char>(c));
            }
            return answer;
        }
        void checkRequest() {
            if (state.checked)
                return;
            state.checked = true;
            const auto nonce = ascii(environment(L"VDXR_PAIR_NONCE"));
            const auto output = environment(L"VDXR_PAIR_OUTPUT"), arm = environment(L"VDXR_PAIR_ARM_FILE");
            const auto pidText = environment(L"VDXR_PAIR_PID");
            const auto pid = number(pidText);
            if (nonce.size() != 32 || !pidText.empty() && pid != GetCurrentProcessId() || output.empty() || arm.empty())
                return;
            for (char c : nonce)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return;
            state.cap = number(environment(L"VDXR_PAIR_MAX_BYTES"));
            state.minimumSerial = number(environment(L"VDXR_PAIR_MIN_SERIAL"));
            state.minimumTime = number(environment(L"VDXR_PAIR_MIN_TIME"));
            state.session = ascii(environment(L"VDXR_PAIR_SESSION"));
            state.output = output;
            state.armFile = arm;
            state.expectedCaller = ascii(environment(L"VDXR_PAIR_CALLER_SHA256"));
            if (!state.cap || state.cap > (uint64_t{4} << 30) || !state.minimumSerial || !state.minimumTime ||
                state.session.empty() || state.expectedCaller.size() != 64 || !state.output.is_absolute() ||
                !state.armFile.is_absolute() || std::filesystem::exists(state.output))
                return;
            for (char c : state.expectedCaller)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return;
            state.nonce = nonce;
            state.requested = true;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                state.requested = false;
                return;
            }
            state.ready.pid = GetCurrentProcessId();
            state.ready.creation = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            state.readyPath = state.armFile;
            state.readyPath +=
                L".ready." + std::to_wstring(state.ready.pid) + L"." + std::to_wstring(state.ready.creation) + L".json";
        }
        void texture2dDescription(std::ostream& out, const D3D11_TEXTURE2D_DESC& d) {
            out << "{\"width\":" << d.Width << ",\"height\":" << d.Height << ",\"mip_levels\":" << d.MipLevels
                << ",\"array_size\":" << d.ArraySize << ",\"format\":" << d.Format
                << ",\"sample_count\":" << d.SampleDesc.Count << ",\"sample_quality\":" << d.SampleDesc.Quality
                << ",\"usage\":" << d.Usage << ",\"bind_flags\":" << d.BindFlags
                << ",\"cpu_access_flags\":" << d.CPUAccessFlags << ",\"misc_flags\":" << d.MiscFlags << '}';
        }
        void writeOutputLineage() {
            std::ofstream out(state.output / "output-lineage.json", std::ios::binary);
            out << "{\"schema\":1,\"private_host_version\":2,\"bounded_capacity\":2048,\"overflow\":"
                << (state.outputOverflow ? "true" : "false") << ",\"rows\":[";
            for (size_t i = 0; i < state.outputCount; ++i) {
                const auto& row = state.outputRows[i];
                const auto& v = row.value;
                out << (i ? "," : "") << "{\"call_id\":" << v.callId << ",\"context\":" << row.context
                    << ",\"host_generation\":" << v.hostGeneration << ",\"owner_generation\":" << v.ownerGeneration
                    << ",\"ticket_token\":" << row.ticket.token << ",\"ticket_generation\":" << row.ticket.generation
                    << ",\"ticket_bank\":" << row.ticket.bank << ",\"serial\":" << row.serial << ",\"role\":" << v.role
                    << ",\"host_context11\":" << v.hostContext11 << ",\"source11\":" << v.source11
                    << ",\"source12\":" << v.source12 << ",\"destination11\":" << v.destination11
                    << ",\"source_path\":" << jsonQuote(v.sourcePath) << ",\"source11_eyes\":[" << v.source11Eyes[0]
                    << ',' << v.source11Eyes[1] << "],\"consumer_fence11\":" << v.fence11
                    << ",\"consumer_value\":" << v.consumerValue << ",\"role_value_before\":" << v.roleValueBefore
                    << ",\"retained_destination11_before\":" << v.retainedDestination11Before
                    << ",\"prepared\":" << (v.prepared ? "true" : "false")
                    << ",\"prepared_reference_valid\":" << (v.preparedReferenceValid ? "true" : "false")
                    << ",\"copy_id\":" << v.copyId << ",\"destination_retention_epoch\":" << v.destinationRetentionEpoch
                    << ",\"copy_called\":" << (v.copyCalled ? "true" : "false")
                    << ",\"signal_called\":" << (v.signalCalled ? "true" : "false")
                    << ",\"signal_hresult\":" << (v.signalCalled ? std::to_string(v.signalResult) : "null")
                    << ",\"flush_called\":" << (v.flushCalled ? "true" : "false") << ",\"result\":" << row.result
                    << ",\"call_qpc\":" << v.callQpc << ",\"copy_qpc\":" << v.copyQpc
                    << ",\"signal_qpc\":" << v.signalQpc << ",\"flush_qpc\":" << v.flushQpc
                    << ",\"return_qpc\":" << v.returnQpc
                    << ",\"destination_description_observed\":" << (v.descriptionObserved ? "true" : "false")
                    << ",\"identity_namespace\":\"D3D11.ID3D11Texture2D\",\"identity_origin\":\"already-owned-typed-"
                       "interfaces\",\"destination_desc\":";
                if (v.descriptionObserved)
                    texture2dDescription(out, v.destinationDescription);
                else
                    out << "null";
                out << '}';
            }
            out << "]}\n";
            out.close();
            if (!out.good())
                throw std::runtime_error("output-lineage-write-failed");
        }
        void event(const char* name, uintptr_t a = 0, uint64_t b = 0, uint64_t c = 0) {
            if (!state.events)
                return;
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << qpc.QuadPart
                         << ",\"event\":" << jsonQuote(name) << ",\"object\":" << a << ",\"value\":" << b
                         << ",\"extra\":" << c << "}\n";
        }
        void fail(const char* reason) {
            state.select.abort();
            if (state.failure.empty())
                state.failure = reason;
            event(reason);
        }
        void abortException() noexcept {
            try {
                std::lock_guard lock(state.mutex);
                state.select.abort();
                if (state.failure.empty())
                    state.failure = "observer-error";
            } catch (...) {
            }
        }
        bool arm() {
            if (state.armed)
                return true;
            std::ifstream stream(state.armFile, std::ios::binary);
            char data[512]{};
            stream.read(data, sizeof(data));
            if (!stream.eof())
                return false;
            const std::string expected = state.nonce + " " + std::to_string(state.ready.pid) + " " +
                                         std::to_string(state.ready.creation) + " " + state.readyNonce + " " +
                                         std::to_string(state.ready.context) + " " +
                                         std::to_string(state.ready.generation) + "\n";
            const std::string body(data, static_cast<size_t>(stream.gcount()));
            std::istringstream parsed(body);
            std::string nonce, session;
            ArmIdentity identity;
            if (!(parsed >> nonce >> identity.pid >> identity.creation >> session >> identity.context >>
                  identity.generation) ||
                session.size() != 32)
                return false;
            std::istringstream low(session.substr(0, 16)), high(session.substr(16));
            low >> std::hex >> identity.sessionLow;
            high >> std::hex >> identity.sessionHigh;
            if (!sameArm(identity, state.ready) || nonce != state.nonce || body != expected)
                return false;
            state.armRecord = body;
            if (!std::filesystem::create_directory(state.output)) {
                fail("output-directory-not-fresh");
                return false;
            }
            state.events.open(state.output / "native-events.jsonl", std::ios::binary);
            if (!state.events) {
                fail("event-open-failed");
                return false;
            }
            state.select.arm(state.minimumSerial, state.minimumTime, state.cap);
            state.armed = true;
            for (const auto& row : state.initial)
                state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << row.qpc << ",\"event\":\""
                             << row.name << "\",\"object\":" << row.object << ",\"other\":" << row.other
                             << ",\"value\":" << row.value << ",\"result\":" << row.result << "}\n";
            event("explicit-arm", GetCurrentProcessId(), state.minimumSerial, state.minimumTime);
            return true;
        }
        DXGI_FORMAT stagingFormat(DXGI_FORMAT value) {
            switch (value) {
            case DXGI_FORMAT_D16_UNORM:
                return DXGI_FORMAT_R16_TYPELESS;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
                return DXGI_FORMAT_R24G8_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT:
                return DXGI_FORMAT_R32_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                return DXGI_FORMAT_R32G8X24_TYPELESS;
            default:
                return value;
            }
        }
        uint32_t bytesPerPixel(DXGI_FORMAT value) {
            switch (value) {
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_R16_TYPELESS:
                return 2;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32G8X24_TYPELESS:
                return 8;
            default:
                return 4;
            }
        }
        uint64_t footprintBytes(const D3D12_RESOURCE_DESC& desc, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* footprints) {
            uint64_t required{};
            state.device->GetCopyableFootprints(
                &desc, 0, desc.DepthOrArraySize, 0, footprints, nullptr, nullptr, &required);
            if (!required || required == UINT64_MAX)
                return 0;
            const uint64_t stride = desc.DepthOrArraySize > 1
                                        ? footprints[1].Offset
                                        : uint64_t(footprints[0].Footprint.RowPitch) * desc.Height;
            return stride * desc.DepthOrArraySize;
        }
        bool preflight(const Host& packet) {
            uint64_t total{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& d = packet.eyes[eye].description;
                if (!d.Width || !d.Height || d.Width > 16384 || d.Height > 16384 || !d.ArraySize || d.ArraySize > 16 ||
                    !d.MipLevels || d.SampleDesc.Count != 1)
                    return false;
                // Conservative payload reservation before staging allocation; actual mapped
                // pitches are retained and checked again without allocating whole CPU images.
                total += 2 * ((uint64_t(d.Width) * bytesPerPixel(d.Format) + 4095) & ~uint64_t(4095)) * d.Height *
                         d.ArraySize;
                state.originalRaw[eye] = d;
            }
            D3D11_TEXTURE2D_DESC canonical{};
            packet.canonical->GetDesc(&canonical);
            if (!canonical.Width || !canonical.Height || canonical.ArraySize != 1 || canonical.MipLevels != 1 ||
                canonical.Format != DXGI_FORMAT_R32_FLOAT)
                return false;
            total += 2 * ((uint64_t(canonical.Width) * 4 + 4095) & ~uint64_t(4095)) * canonical.Height;
            D3D12_RESOURCE_DESC depth{};
            depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depth.Width = canonical.Width;
            depth.Height = canonical.Height;
            depth.MipLevels = depth.DepthOrArraySize = 1;
            depth.SampleDesc.Count = 1;
            depth.Format = DXGI_FORMAT_R32_FLOAT;
            auto colour = depth;
            colour.Width /= 2;
            colour.DepthOrArraySize = 2;
            colour.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
            if (!state.outputs[0])
                return false;
            const auto output = state.outputs[0]->GetDesc();
            state.gpuBudget = 2 * footprintBytes(depth, footprints) + 2 * footprintBytes(colour, footprints) +
                              footprintBytes(output, footprints);
            total += state.gpuBudget;
            return state.select.reserve(total);
        }
        bool
        stage11(const char* role, ID3D11DeviceContext4* context, ID3D11Texture2D* source, const Eye* eye = nullptr) {
            Image11 image;
            image.role = role;
            if (eye)
                image.eye = *eye;
            image.source = reinterpret_cast<uintptr_t>(source);
            image.context = reinterpret_cast<uintptr_t>(context);
            image.serial = state.select.phase == Phase::a_provisional ? state.select.serialA : state.select.serialB;
            source->GetDesc(&image.original);
            auto desc = image.original;
            if (!desc.ArraySize || desc.ArraySize > 16)
                return false;
            desc.Format = stagingFormat(desc.Format);
            desc.ArraySize = desc.MipLevels = 1;
            desc.BindFlags = desc.MiscFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Device> device;
            source->GetDevice(&device);
            image.staging.resize(image.original.ArraySize);
            for (UINT slice = 0; slice < image.original.ArraySize; ++slice) {
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &image.staging[slice])))
                    return false;
            }
            state.host.push_back(std::move(image));
            auto& owned = state.host.back();
            for (UINT slice = 0; slice < owned.staging.size(); ++slice)
                context->CopySubresourceRegion(owned.staging[slice].Get(),
                                               0,
                                               0,
                                               0,
                                               0,
                                               source,
                                               D3D11CalcSubresource(0, slice, owned.original.MipLevels),
                                               nullptr);
            event(role,
                  reinterpret_cast<uintptr_t>(source),
                  reinterpret_cast<uintptr_t>(context),
                  owned.original.ArraySize);
            return true;
        }
        bool plan12(const char* role, ID3D12Resource* source, Image12& image) {
            image.role = role;
            image.source = source;
            image.desc = source->GetDesc();
            const auto& desc = image.desc;
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
                desc.DepthOrArraySize > 2 || desc.SampleDesc.Count != 1)
                return false;
            image.footprints.resize(desc.DepthOrArraySize);
            image.bytes = footprintBytes(desc, image.footprints.data());
            if (!image.bytes || image.bytes > state.cap)
                return false;
            return true;
        }
        bool allocate12(Image12& image) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = image.bytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return SUCCEEDED(state.device->CreateCommittedResource(&heap,
                                                                   D3D12_HEAP_FLAG_NONE,
                                                                   &buffer,
                                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   nullptr,
                                                                   IID_PPV_ARGS(&image.readback)));
        }
        void copy12(ID3D12GraphicsCommandList* commands, Image12& image, uint32_t before, bool transition = true) {
            image.copyCommands = reinterpret_cast<uintptr_t>(commands);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = image.source.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            if (transition)
                commands->ResourceBarrier(1, &barrier);
            for (UINT slice = 0; slice < image.footprints.size(); ++slice) {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = image.source.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = slice;
                dst.pResource = image.readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = image.footprints[slice];
                commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            if (transition) {
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                commands->ResourceBarrier(1, &barrier);
            }
        }
        struct Hash {
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE hash{};
            Hash() {
                if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
                    BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
            }
            ~Hash() {
                if (hash)
                    BCryptDestroyHash(hash);
                if (algorithm)
                    BCryptCloseAlgorithmProvider(algorithm, 0);
            }
            bool update(const void* data, size_t count) {
                return hash && count <= ULONG_MAX &&
                       BCryptHashData(hash, (PUCHAR)data, static_cast<ULONG>(count), 0) >= 0;
            }
            std::string finish() {
                unsigned char bytes[32]{};
                if (!hash || BCryptFinishHash(hash, bytes, 32, 0) < 0)
                    return {};
                const char* hex = "0123456789abcdef";
                std::string answer;
                for (auto b : bytes) {
                    answer += hex[b >> 4];
                    answer += hex[b & 15];
                }
                return answer;
            }
        };
        std::string jsonQuote(const std::string& input) {
            std::string safe = input;
            const char title[] = {86, 82, 80, 97, 114, 97, 100, 105, 115, 101};
            const std::string privateTitle(title, sizeof(title));
            for (size_t at = safe.find(privateTitle); at != std::string::npos; at = safe.find(privateTitle))
                safe.replace(at, privateTitle.size(), "[redacted]");
            std::string result = "\"";
            const char* hex = "0123456789abcdef";
            for (unsigned char c : safe) {
                switch (c) {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\b':
                    result += "\\b";
                    break;
                case '\f':
                    result += "\\f";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    if (c < 32) {
                        result += "\\u00";
                        result += hex[c >> 4];
                        result += hex[c & 15];
                    } else
                        result += static_cast<char>(c);
                    break;
                }
            }
            return result + '"';
        }
        std::string utf8(const wchar_t* value) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
            if (size <= 1)
                return {};
            std::string result(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
            result.pop_back();
            return result;
        }
        void readyRecord(uint64_t generation) {
            state.earlyCount = 0;
            state.earlySerial = 0;
            state.ready.context = reinterpret_cast<uintptr_t>(state.context);
            state.ready.generation = generation;
            uint64_t random[2]{};
            if (BCryptGenRandom(
                    nullptr, reinterpret_cast<PUCHAR>(random), sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
                !(random[0] || random[1])) {
                fail("ready-session-nonce-failed");
                return;
            }
            state.ready.sessionLow = random[0];
            state.ready.sessionHigh = random[1];
            std::ostringstream nonce;
            nonce << std::hex << std::setfill('0') << std::setw(16) << random[0] << std::setw(16) << random[1];
            state.readyNonce = nonce.str();
            auto temporary = state.readyPath;
            temporary += L".partial";
            std::ofstream stream(temporary, std::ios::binary);
            const auto luid = state.device->GetAdapterLuid();
            const auto output = state.outputs[0]->GetDesc();
            stream << "{\"status\":\"graphics-context-ready-requires-explicit-scene-arm\",\"nonce\":\"" << state.nonce
                   << "\",\"pid\":" << state.ready.pid << ",\"creation_filetime\":" << state.ready.creation
                   << ",\"session_nonce\":\"" << state.readyNonce << "\",\"context\":" << state.ready.context
                   << ",\"generation\":" << state.ready.generation << ",\"adapter_luid\":[" << luid.LowPart << ','
                   << luid.HighPart << "],\"full_width\":" << output.Width << ",\"height\":" << output.Height
                   << ",\"caller_scope\":\"per-call-stack-required-at-selected-xrEndFrame-not-module-presence\"}\n";
            stream.close();
            if (!stream.good() || !MoveFileExW(temporary.c_str(),
                                               state.readyPath.c_str(),
                                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                fail("ready-record-write-failed");
        }
        bool callerWitness(const Host& packet, void* boundaryCaller) {
            const auto& entry = packet.entry;
            if (entry.serial != packet.serial || entry.displayTime != packet.displayTime || !entry.qpc ||
                !entry.leaseQpc || entry.qpc >= entry.leaseQpc || entry.thread != GetCurrentThreadId() ||
                entry.producerContext != packet.producer || !entry.count || entry.count > 48)
                return false;
            const uint64_t serial = packet.serial;
            void* hostAddress = boundaryCaller;
            HMODULE hostModule{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(hostAddress),
                               &hostModule);
            HMODULE own{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&callerWitness),
                               &own);
            void* const* stack = entry.addresses;
            const USHORT count = entry.count;
            bool matched = false;
            for (USHORT index = 0; index < count; ++index) {
                HMODULE module{};
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(stack[index]),
                                        &module))
                    return false;
                wchar_t path[32768]{};
                const DWORD length = GetModuleFileNameW(module, path, 32768);
                if (!length || length == 32768)
                    return false;
                Hash hash;
                std::ifstream file(std::filesystem::path(path), std::ios::binary);
                char chunk[65536]{};
                uint64_t total{};
                while (file) {
                    file.read(chunk, sizeof(chunk));
                    const auto got = file.gcount();
                    if (got > 0) {
                        total += static_cast<uint64_t>(got);
                        if (total > state.cap || !hash.update(chunk, static_cast<size_t>(got)))
                            return false;
                    }
                }
                if (!file.eof())
                    return false;
                const auto digest = hash.finish();
                if (digest.size() != 64)
                    return false;
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE)
                    return false;
                state.events << "{\"sequence\":" << ++state.sequence
                             << ",\"event\":\"actual-host-call-module\",\"serial\":" << serial
                             << ",\"display_time_ns\":" << entry.displayTime << ",\"thread\":" << entry.thread
                             << ",\"xr_entry_qpc\":" << entry.qpc << ",\"producer_lease_qpc\":" << entry.leaseQpc
                             << ",\"frame_index\":" << index
                             << ",\"address\":" << reinterpret_cast<uintptr_t>(stack[index])
                             << ",\"module\":" << reinterpret_cast<uintptr_t>(module) << ",\"rva\":"
                             << reinterpret_cast<uintptr_t>(stack[index]) - reinterpret_cast<uintptr_t>(module)
                             << ",\"image_size\":" << nt->OptionalHeader.SizeOfImage
                             << ",\"timestamp\":" << nt->FileHeader.TimeDateStamp
                             << ",\"path\":" << jsonQuote(utf8(path)) << ",\"disk_sha256\":\"" << digest << "\"}\n";
                if (module != hostModule && module != own && digest == state.expectedCaller)
                    matched = true;
            }
            return matched;
        }
        void rectangle(std::ostream& stream, const VDXRFG_RectV1& rect) {
            stream << '[' << rect.x << ',' << rect.y << ',' << rect.width << ',' << rect.height << ']';
        }
        uint32_t bits(float value) {
            uint32_t result{};
            std::memcpy(&result, &value, sizeof(result));
            return result;
        }
        void debugReceipt() {
            std::lock_guard debugLock(state.debugMutex);
            std::ofstream stream(state.output / "actual-device-debug.json", std::ios::binary);
            stream << "{\"scope\":\"actual-private-submission11-and-FG12\",\"calibration_debug_requested\":"
                   << (debugRequested() ? "true" : "false")
                   << ",\"device11\":" << reinterpret_cast<uintptr_t>(state.submissionDevice.Get())
                   << ",\"device12\":" << reinterpret_cast<uintptr_t>(state.device.Get()) << ",\"creation_flags11\":"
                   << (state.submissionDevice ? state.submissionDevice->GetCreationFlags() : 0)
                   << ",\"infoqueue_qi11\":" << state.info11Result << ",\"infoqueue_qi12\":" << state.info12Result
                   << ",\"device_removed11\":"
                   << (state.submissionDevice ? state.submissionDevice->GetDeviceRemovedReason() : E_POINTER)
                   << ",\"device_removed12\":" << state.device->GetDeviceRemovedReason()
                   << ",\"capture_start_messages11\":" << state.debugStart11
                   << ",\"capture_start_messages12\":" << state.debugStart12 << ",\"messages\":[";
            bool first = true, complete = true;
            uint64_t errors{};
            if (state.info11) {
                const uint64_t count = state.info11->GetNumStoredMessagesAllowedByRetrievalFilter();
                if (count > 4096)
                    complete = false;
                for (uint64_t index = 0; index < count && index < 4096; ++index) {
                    SIZE_T size{};
                    if (FAILED(state.info11->GetMessage(index, nullptr, &size)) || size > 65536) {
                        complete = false;
                        break;
                    }
                    std::vector<unsigned char> bytes(size);
                    auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                    if (FAILED(state.info11->GetMessage(index, message, &size))) {
                        complete = false;
                        break;
                    }
                    if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
                        message->Severity == D3D11_MESSAGE_SEVERITY_ERROR)
                        ++errors;
                    stream << (first ? "" : ",") << "{\"api\":11,\"index\":" << index
                           << ",\"severity\":" << message->Severity << ",\"id\":" << message->ID << ",\"description\":"
                           << jsonQuote(
                                  std::string(message->pDescription,
                                              message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0))
                           << "}";
                    first = false;
                }
            }
            if (state.info12) {
                const uint64_t count = state.info12->GetNumStoredMessagesAllowedByRetrievalFilter();
                if (count > 4096)
                    complete = false;
                for (uint64_t index = 0; index < count && index < 4096; ++index) {
                    SIZE_T size{};
                    if (FAILED(state.info12->GetMessage(index, nullptr, &size)) || size > 65536) {
                        complete = false;
                        break;
                    }
                    std::vector<unsigned char> bytes(size);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                    if (FAILED(state.info12->GetMessage(index, message, &size))) {
                        complete = false;
                        break;
                    }
                    if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                        message->Severity == D3D12_MESSAGE_SEVERITY_ERROR)
                        ++errors;
                    stream << (first ? "" : ",") << "{\"api\":12,\"index\":" << index
                           << ",\"severity\":" << message->Severity << ",\"id\":" << message->ID << ",\"description\":"
                           << jsonQuote(
                                  std::string(message->pDescription,
                                              message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0))
                           << "}";
                    first = false;
                }
            }
            const uint64_t lost11 = state.info11 ? state.info11->GetNumMessagesDiscardedByMessageCountLimit() : 0,
                           lost12 = state.info12 ? state.info12->GetNumMessagesDiscardedByMessageCountLimit() : 0;
            stream << "],\"messages_discarded11\":" << lost11 << ",\"messages_discarded12\":" << lost12
                   << ",\"corruption_or_errors\":" << errors << ",\"inspection_complete\":"
                   << (complete && state.info11 && state.info12 && !lost11 && !lost12 ? "true" : "false")
                   << ",\"qualification\":\"raw-debug-observations-no-clean-claim-if-unavailable-incomplete-or-"
                      "errors\"}\n";
            stream.close();
            if (!stream.good())
                throw std::runtime_error("debug-receipt-write-failed");
        }
        bool wait(ID3D12Fence* fence, uint64_t value) {
            const uint64_t completed = fence->GetCompletedValue();
            if (completed == UINT64_MAX)
                return false;
            if (completed >= value)
                return true;
            HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ready)
                return false;
            if (FAILED(fence->SetEventOnCompletion(value, ready))) {
                CloseHandle(ready);
                return false;
            }
            const bool result = WaitForSingleObject(ready, 15000) == WAIT_OBJECT_0 &&
                                fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value;
            if (result)
                CloseHandle(ready);
            else { // A live completion registration also remains owned on timeout.
                std::lock_guard lock(state.mutex);
                for (auto& pending : state.pendingWaitEvents)
                    if (!pending) {
                        pending = ready;
                        break;
                    }
            }
            return result;
        }
        DWORD workerBody() {
            try {
                ComPtr<ID3D12Fence> full = state.fullFenceB;
                state.gpuUnknown = true;
                if (!full || !wait(full.Get(), state.ticketB.output_ready_value)) {
                    std::lock_guard lock(state.mutex);
                    fail("full-product-fence-incomplete");
                    return 0;
                }
                {
                    std::lock_guard lock(state.mutex);
                    state.ownership.productComplete = true;
                    state.gpuUnknown = false;
                    event("full-product-completed",
                          reinterpret_cast<uintptr_t>(full.Get()),
                          state.ticketB.output_ready_value,
                          full->GetCompletedValue());
                }
                Image12 output;
                if (!plan12("generated", state.outputs[state.ticketB.bank * 4].Get(), output) || !allocate12(output) ||
                    FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                IID_PPV_ARGS(&state.copyAllocator))) ||
                    FAILED(state.device->CreateCommandList(0,
                                                           D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           state.copyAllocator.Get(),
                                                           nullptr,
                                                           IID_PPV_ARGS(&state.copyList)))) {
                    std::lock_guard lock(state.mutex);
                    fail("private-copy-allocation-failed");
                    return 0;
                }
                D3D12_RESOURCE_STATES outputBefore{};
                bool observed = false;
                {
                    std::lock_guard lock(state.mutex);
                    for (const auto& item : state.resourceStates)
                        if (item.resource == output.source.Get() && item.read.known && !item.read.invalid) {
                            outputBefore = static_cast<D3D12_RESOURCE_STATES>(item.read.value);
                            observed = true;
                            break;
                        }
                    if (!observed) {
                        fail("product-output-AFTER-state-not-observed");
                        return 0;
                    }
                    event("private-output-copy-before-state",
                          reinterpret_cast<uintptr_t>(output.source.Get()),
                          outputBefore);
                }
                state.gpu.push_back(std::move(output));
                copy12(state.copyList.Get(), state.gpu.back(), outputBefore);
                if (FAILED(state.copyList->Close())) {
                    std::lock_guard lock(state.mutex);
                    fail("private-copy-close-failed");
                    return 0;
                }
                ID3D12CommandList* lists[] = {state.copyList.Get()};
                {
                    std::lock_guard lock(state.mutex);
                    state.ownership.copyQueued = true;
                    state.gpuUnknown = true;
                }
                state.queue->ExecuteCommandLists(1, lists);
                {
                    std::lock_guard lock(state.mutex);
                    event("private-copy-executed",
                          reinterpret_cast<uintptr_t>(state.copyList.Get()),
                          reinterpret_cast<uintptr_t>(state.queue.Get()));
                }
                const HRESULT privateSignal = state.queue->Signal(state.copyFence.Get(), 1);
                {
                    std::lock_guard lock(state.mutex);
                    LARGE_INTEGER qpc{};
                    QueryPerformanceCounter(&qpc);
                    state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << qpc.QuadPart
                                 << ",\"event\":\"private-copy-Signal-result\",\"queue\":"
                                 << reinterpret_cast<uintptr_t>(state.queue.Get())
                                 << ",\"fence\":" << reinterpret_cast<uintptr_t>(state.copyFence.Get())
                                 << ",\"value\":1,\"hresult\":" << privateSignal << "}\n";
                }
                if (FAILED(privateSignal) || !wait(state.copyFence.Get(), 1)) {
                    std::lock_guard lock(state.mutex);
                    fail("private-copy-fence-incomplete");
                    return 0;
                }
                {
                    std::lock_guard lock(state.mutex);
                    state.ownership.copyComplete = true;
                    state.gpuUnknown = false;
                    state.retireReady = state.ownership.canRetire();
                    event("private-copy-completed",
                          reinterpret_cast<uintptr_t>(state.copyFence.Get()),
                          1,
                          state.copyFence->GetCompletedValue());
                }
                debugReceipt();
                std::ofstream manifest(state.output / "native-buffers.partial", std::ios::binary);
                manifest << "{\"schema\":1,\"status\":\"buffers-captured-unadjudicated\",\"nonce\":\"" << state.nonce
                         << "\",\"session\":" << jsonQuote(state.session) << ",\"pid\":" << GetCurrentProcessId()
                         << ",\"serial_a\":" << state.select.serialA << ",\"serial_b\":" << state.select.serialB
                         << ",\"generation\":" << state.select.generation
                         << ",\"creation_filetime\":" << state.ready.creation << ",\"ready_session_nonce\":\""
                         << state.readyNonce << "\",\"arm_record\":" << jsonQuote(state.armRecord)
                         << ",\"guide_epoch\":" << state.select.epoch
                         << ",\"context\":" << reinterpret_cast<uintptr_t>(state.context)
                         << ",\"ticket_a\":" << state.ticketA.token << ",\"ticket_b\":" << state.ticketB.token
                         << ",\"a_prime\":" << (state.ticketA.prime ? "true" : "false")
                         << ",\"a_core_signal_observed\":" << (state.coreSignalA ? "true" : "false")
                         << ",\"prime_no_full_synth_fence\":"
                         << (state.ticketA.prime && !state.coreSignalA ? "true" : "false")
                         << ",\"a_input_consumed_fence\":"
                         << reinterpret_cast<uintptr_t>(state.ticketA.input_consumed_fence)
                         << ",\"a_input_consumed_value\":" << state.ticketA.input_consumed_value
                         << ",\"a_public_ready_scope\":\"original-module-ready-not-an-invented-A-synthesis-fence\",\"a_"
                            "public_ready_fence\":"
                         << reinterpret_cast<uintptr_t>(state.ticketA.output_ready_fence)
                         << ",\"a_public_ready_value\":" << state.ticketA.output_ready_value
                         << ",\"full_product_fence\":" << reinterpret_cast<uintptr_t>(state.ticketB.output_ready_fence)
                         << ",\"full_product_value\":" << state.ticketB.output_ready_value
                         << ",\"actual_effective_nr_A\":" << (state.effectiveNr[0] ? "true" : "false")
                         << ",\"actual_effective_nr_B\":" << (state.effectiveNr[1] ? "true" : "false")
                         << ",\"NR_scope\":\"actual-per-frame-precompositor-snapshot; child-only-NRoff-calibration; "
                            "not-global-profile-inference\""
                         << ",\"private_copy_fence\":" << reinterpret_cast<uintptr_t>(state.copyFence.Get())
                         << ",\"private_copy_value\":1,\"artifacts\":[";
                bool first = true;
                uint64_t written{};
                for (const auto& image : state.host) {
                    Hash hash;
                    std::ofstream bytes(state.output / (image.role + ".bin"), std::ios::binary);
                    uint64_t size{};
                    uint32_t pitch{};
                    for (const auto& texture : image.staging) {
                        ComPtr<IDXGISurface> surface;
                        if (FAILED(texture.As(&surface)))
                            throw std::runtime_error("staging-surface-unavailable");
                        DXGI_MAPPED_RECT mapped{};
                        if (FAILED(surface->Map(&mapped, DXGI_MAP_READ)))
                            throw std::runtime_error("staging-map-failed");
                        const uint64_t count = uint64_t(mapped.Pitch) * image.original.Height;
                        if (mapped.Pitch <= 0 || count > state.cap - written ||
                            pitch && pitch != static_cast<uint32_t>(mapped.Pitch)) {
                            surface->Unmap();
                            throw std::runtime_error("staging-pitch-budget");
                        }
                        pitch = static_cast<uint32_t>(mapped.Pitch);
                        bytes.write(reinterpret_cast<const char*>(mapped.pBits), static_cast<std::streamsize>(count));
                        const bool hashed = hash.update(mapped.pBits, static_cast<size_t>(count));
                        surface->Unmap();
                        if (!hashed)
                            throw std::runtime_error("hash-failed");
                        written += count;
                        size += count;
                    }
                    bytes.close();
                    if (!bytes.good())
                        throw std::runtime_error("payload-write-failed");
                    manifest << (first ? "" : ",") << "{\"role\":\"" << image.role << "\",\"sha256\":\""
                             << hash.finish() << "\",\"size\":" << size << ",\"row_pitch\":" << pitch
                             << ",\"slice_pitch\":" << uint64_t(pitch) * image.original.Height
                             << ",\"width\":" << image.original.Width << ",\"height\":" << image.original.Height
                             << ",\"array_size\":" << image.original.ArraySize
                             << ",\"source_mip_levels\":" << image.original.MipLevels
                             << ",\"selected_mip\":0,\"resource_format\":" << image.original.Format
                             << ",\"data_scope\":\"full-array-mip0\",\"map_row_pitch\":" << pitch
                             << ",\"file_row_pitch\":" << pitch
                             << ",\"file_slice_pitch\":" << uint64_t(pitch) * image.original.Height
                             << ",\"descriptor_origin\":\"existing-ID3D11Texture2D-GetDesc\",\"texture2d_desc\":";
                    texture2dDescription(manifest, image.original);
                    manifest << ",\"staging_resources\":[";
                    for (size_t index = 0; index < image.staging.size(); ++index)
                        manifest << (index ? "," : "") << reinterpret_cast<uintptr_t>(image.staging[index].Get());
                    manifest << "]"
                             << ",\"source\":" << image.source << ",\"context\":" << image.context
                             << ",\"serial\":" << image.serial << ",\"released_index\":" << image.eye.releasedIndex
                             << ",\"selected_array_slice\":" << image.eye.arraySlice << ",\"colour_rect\":";
                    rectangle(manifest, image.eye.colour);
                    manifest << ",\"crop_rect\":";
                    rectangle(manifest, image.eye.crop);
                    manifest << ",\"depth_rect\":";
                    rectangle(manifest, image.eye.depth);
                    manifest << ",\"depth_range_bits\":[" << bits(image.eye.minDepth) << ',' << bits(image.eye.maxDepth)
                             << ',' << bits(image.eye.nearZ) << ',' << bits(image.eye.farZ)
                             << "],\"capture_view\":{\"position\":[";
                    for (unsigned i = 0; i < 3; ++i)
                        manifest << (i ? "," : "") << bits(image.eye.view.position[i]);
                    manifest << "],\"orientation\":[";
                    for (unsigned i = 0; i < 4; ++i)
                        manifest << (i ? "," : "") << bits(image.eye.view.orientation[i]);
                    manifest << "],\"fov\":[";
                    for (unsigned i = 0; i < 4; ++i)
                        manifest << (i ? "," : "") << bits(image.eye.view.fov[i]);
                    manifest << "]}}";
                    first = false;
                }
                for (const auto& image : state.gpu) {
                    if (image.bytes > state.cap - written)
                        throw std::runtime_error("payload-budget");
                    void* mapped{};
                    D3D12_RANGE read{0, static_cast<SIZE_T>(image.bytes)};
                    if (FAILED(image.readback->Map(0, &read, &mapped)))
                        throw std::runtime_error("readback-map-failed");
                    Hash hash;
                    const bool hashed = hash.update(mapped, static_cast<size_t>(image.bytes));
                    std::ofstream bytes(state.output / (image.role + ".bin"), std::ios::binary);
                    bytes.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(image.bytes));
                    D3D12_RANGE noWrite{0, 0};
                    image.readback->Unmap(0, &noWrite);
                    bytes.close();
                    if (!hashed || !bytes.good())
                        throw std::runtime_error("readback-write-failed");
                    written += image.bytes;
                    manifest << (first ? "" : ",") << "{\"role\":\"" << image.role << "\",\"sha256\":\""
                             << hash.finish() << "\",\"size\":" << image.bytes << ",\"width\":" << image.desc.Width
                             << ",\"height\":" << image.desc.Height << ",\"array_size\":" << image.desc.DepthOrArraySize
                             << ",\"resource_format\":" << image.desc.Format
                             << ",\"source\":" << reinterpret_cast<uintptr_t>(image.source.Get())
                             << ",\"readback_resource\":" << reinterpret_cast<uintptr_t>(image.readback.Get())
                             << ",\"copy_command_list\":" << image.copyCommands << ",\"footprints\":[";
                    bool firstSlice = true;
                    for (const auto& footprint : image.footprints) {
                        manifest << (firstSlice ? "" : ",") << "[" << footprint.Offset << ","
                                 << footprint.Footprint.RowPitch << "," << footprint.Footprint.Width << ","
                                 << footprint.Footprint.Height << "]";
                        firstSlice = false;
                    }
                    manifest << "]}";
                    first = false;
                }
                manifest
                    << "],\"bytes\":" << written
                    << ",\"qualification\":\"native-buffers-only-requires-independent-whole-pair-adjudication\"}\n";
                manifest.close();
                if (!manifest.good() || !MoveFileExW((state.output / "native-buffers.partial").c_str(),
                                                     (state.output / "native-buffers.json").c_str(),
                                                     MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("manifest-write-failed");
                {
                    std::lock_guard lock(state.mutex);
                    event("buffers-written-unadjudicated", 0, written);
                    state.events.flush();
                    if (!state.events.good()) {
                        fail("event-log-write-incomplete");
                        return 0;
                    }
                    state.finished = true;
                }
            } catch (const std::exception& error) {
                std::lock_guard lock(state.mutex);
                fail(error.what());
            } catch (...) {
                std::lock_guard lock(state.mutex);
                fail("observer-worker-exception");
            }
            return 0;
        }
        DWORD WINAPI worker(void* module) {
            workerBody();
            bool release = false;
            bool failed = false;
            {
                std::lock_guard lock(state.mutex);
                failed = !state.failure.empty();
            }
            try {
                if (failed && state.device)
                    debugReceipt();
            } catch (...) {
                std::lock_guard lock(state.mutex);
                event("failed-capture-debug-snapshot-unavailable");
            }
            {
                std::lock_guard lock(state.mutex);
                state.ownership.workerExited = true;
                release = state.ownership.canRelease();
                if (release && !state.failure.empty())
                    state.retireReady = state.ownership.canRetire();
                state.workerSettled = true;
                event(release ? "observer-worker-finished-safe" : "observer-module-retained-GPU-uncertain");
                state.events.flush();
            }
            // Keep this DLL loaded while its detached observer thread is executing.
            // Uncertain GPU ownership intentionally retains the private module/resources
            // until the bounded test child exits; never release them on a timeout.
            if (release)
                FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
            return 0;
        }
    } // namespace

    void prepare(void* context) noexcept {
        if (nr::requested()) {
            nr::prepare(context);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            checkRequest();
            if (!state.requested)
                return;
            if (state.finished)
                return;
            if (state.armed) {
                fail("graphics-context-recreated-after-arm");
                return;
            }
            state.bound = false;
            state.rtvs = {};
            state.b1Views = {};
            state.renderTargets = {};
            state.resourceStates = {};
            state.colourHistory = {};
            (void)context;
        } catch (...) {
            abortException();
        }
    }
    void bind(void* context,
              ID3D12Device* device,
              ID3D12CommandQueue* queue,
              ID3D12Resource* const outputs[8],
              uint64_t generation) noexcept {
        if (nr::requested()) {
            nr::bind(context, device, queue, outputs, generation);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            checkRequest();
            if (!state.requested)
                return;
            if (state.finished)
                return;
            if (state.armed && state.context != context) {
                fail("context-replaced");
                return;
            }
            state.context = context;
            state.device = device;
            state.queue = queue;
            for (unsigned i = 0; i < 8; ++i)
                state.outputs[i] = outputs[i];
            state.bound = true;
            if (!state.armed)
                readyRecord(generation);
        } catch (...) {
            abortException();
        }
    }
    void generation(void* context, uint64_t generation) noexcept {
        if (nr::requested()) {
            nr::generation(context, generation);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (state.requested && state.bound && state.context == context && !state.finished) {
                if (state.armed)
                    fail("context-generation-reset-after-arm");
                else
                    readyRecord(generation);
            }
        } catch (...) {
            abortException();
        }
    }
    void depthCbv(void* slot,
                  ID3D12Resource* resource,
                  ID3D12DescriptorHeap* heap,
                  UINT eye,
                  UINT index,
                  UINT offset,
                  UINT increment,
                  const D3D12_CONSTANT_BUFFER_VIEW_DESC& description,
                  D3D12_CPU_DESCRIPTOR_HANDLE cpu) noexcept {
        if (nr::requested()) {
            nr::fgCbv(slot, resource, heap, eye, index, offset, increment, description, cpu);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            checkRequest();
            if (!state.requested || state.finished)
                return;
            for (auto& item : state.b1Views)
                if (!item.slot || item.slot == slot && item.eye == eye) {
                    LARGE_INTEGER qpc{};
                    QueryPerformanceCounter(&qpc);
                    item = {slot,
                            reinterpret_cast<uintptr_t>(resource),
                            reinterpret_cast<uintptr_t>(heap),
                            eye,
                            index,
                            offset,
                            increment,
                            static_cast<uint64_t>(qpc.QuadPart),
                            cpu.ptr,
                            heap->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(index) * increment,
                            description};
                    return;
                }
            fail("B1-descriptor-observation-capacity");
        } catch (...) {
            abortException();
        }
    }
    void rootTable(ID3D12GraphicsCommandList* commands, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
        if (nr::requested()) {
            nr::fgRootTable(commands, index, handle);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.finished || index > 1)
                return;
            for (auto& item : state.renderTargets)
                if (!item.commands || item.commands == commands) {
                    item.commands = commands;
                    item.rootTables[index] = handle;
                    return;
                }
            fail("root-table-observation-capacity");
        } catch (...) {
            abortException();
        }
    }
    void configure(void* slot,
                   ID3D12Resource* a,
                   ID3D12Resource* b,
                   uint64_t serialA,
                   uint64_t serialB,
                   uint64_t epoch,
                   const D3D12_SHADER_RESOURCE_VIEW_DESC& srv,
                   const void*) noexcept {
        if (nr::requested()) {
            nr::fgConfigure(slot, a, b, serialA, serialB, epoch, srv);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.finished || state.context == nullptr)
                return;
            state.slot = slot;
            state.consumers[2] = a;
            state.consumers[3] = b;
            state.srvs[2] = state.srvs[3] = srv;
            state.guideA = serialA;
            state.guideB = serialB;
            state.guideEpoch = epoch;
        } catch (...) {
            abortException();
        }
    }
    void sources(void* slot,
                 ID3D12Resource* a,
                 ID3D12Resource* b,
                 const D3D12_SHADER_RESOURCE_VIEW_DESC& srv,
                 ID3D12DescriptorHeap* heap,
                 uint32_t increment) noexcept {
        if (nr::requested()) {
            nr::fgSources(slot, a, b, srv, heap, increment);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.finished || state.slot != slot)
                return;
            state.consumers[0] = a;
            state.consumers[1] = b;
            state.srvs[0] = state.srvs[1] = srv;
            state.heap = heap;
            state.descriptorIncrement = increment;
        } catch (...) {
            abortException();
        }
    }
    void draw(void* slot,
              ID3D12GraphicsCommandList* commands,
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
        if (nr::requested()) {
            nr::draw(
                slot, commands, pso, eye, flags, second, b1, b0, b0Size, viewIndex, b1Resource, shader, shaderSize);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.select.phase != Phase::b_provisional || state.slot != slot)
                return;
            if (state.guideA != state.select.serialA || state.guideB != state.select.serialB ||
                state.guideEpoch != state.select.epoch || !b1 || !shader || !shaderSize) {
                fail("draw-lineage-missing");
                return;
            }
            Hash hash;
            if (!hash.update(shader, shaderSize)) {
                fail("shader-hash-failed");
                return;
            }
            event("actual-depth-draw", reinterpret_cast<uintptr_t>(commands), eye, flags);
            event("actual-draw-pso", reinterpret_cast<uintptr_t>(pso), second);
            const State::Targets* bound = nullptr;
            for (const auto& item : state.renderTargets)
                if (item.commands == commands)
                    bound = &item;
            if (!bound || !bound->count || bound->count > 2) {
                fail("actual-render-target-binding-missing");
                return;
            }
            const State::B1View* b1View = nullptr;
            for (const auto& item : state.b1Views)
                if (item.slot == slot && item.eye == eye && item.resource == reinterpret_cast<uintptr_t>(b1Resource) &&
                    item.heap == reinterpret_cast<uintptr_t>(state.heap.Get()))
                    b1View = &item;
            if (!b0 || b0Size != 248 || !b1Resource || !b1View || b1View->description.SizeInBytes != 256 ||
                b1View->description.BufferLocation != b1Resource->GetGPUVirtualAddress() + uint64_t(eye) * 256 ||
                !bound->rootTables[0].ptr ||
                bound->rootTables[0].ptr + uint64_t(b1View->offset) * b1View->increment != b1View->gpu) {
                fail("actual-b0-b1-CBV-root-binding-unproved");
                return;
            }
            bool role0 = false;
            for (UINT index = 0; index < bound->count; ++index) {
                const State::Rtv* view = nullptr;
                for (const auto& item : state.rtvs)
                    if (item.handle == bound->handles[index].ptr)
                        view = &item;
                if (!view) {
                    fail("actual-render-target-view-missing");
                    return;
                }
                int mappedRole = -1;
                for (unsigned role = 0; role < 8; ++role)
                    if (state.outputs[role].Get() == view->resource)
                        mappedRole = static_cast<int>(role);
                if (mappedRole >= 0 && static_cast<uint32_t>(mappedRole) == state.prospective.bank * 4)
                    role0 = true;
                state.events << "{\"sequence\":" << ++state.sequence
                             << ",\"event\":\"actual-draw-MRT-output\",\"command_list\":"
                             << reinterpret_cast<uintptr_t>(commands) << ",\"eye\":" << eye
                             << ",\"mrt_index\":" << index << ",\"mrt_count\":" << bound->count
                             << ",\"rtv\":" << view->handle
                             << ",\"resource\":" << reinterpret_cast<uintptr_t>(view->resource)
                             << ",\"registered_bank_role\":" << mappedRole
                             << ",\"view_format\":" << view->description.Format
                             << ",\"view_dimension\":" << view->description.ViewDimension << ",\"slice\":"
                             << (view->description.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY
                                     ? view->description.Texture2DArray.FirstArraySlice
                                     : 0)
                             << ",\"packed_phase\":" << flags << ",\"packed_second_phase\":" << second
                             << ",\"viewport\":[" << bound->viewport.TopLeftX << ',' << bound->viewport.TopLeftY << ','
                             << bound->viewport.Width << ',' << bound->viewport.Height << "],\"scissor\":["
                             << bound->scissor.left << ',' << bound->scissor.top << ',' << bound->scissor.right << ','
                             << bound->scissor.bottom << "]}\n";
            }
            if (!role0)
                return;
            if (eye > 1 || state.selectedEyeMask & (1u << eye)) {
                fail("duplicate-selected-role-eye-draw");
                return;
            }
            state.selectedEyeMask |= 1u << eye;
            state.events << "{\"sequence\":" << ++state.sequence
                         << ",\"event\":\"bound-shader\",\"command_list\":" << reinterpret_cast<uintptr_t>(commands)
                         << ",\"eye\":" << eye << ",\"pipeline\":" << reinterpret_cast<uintptr_t>(pso)
                         << ",\"ticket\":" << state.prospective.token << ",\"generation\":" << state.select.generation
                         << ",\"colour_serial_a\":" << state.prospective.previous_serial
                         << ",\"colour_serial_b\":" << state.prospective.current_serial
                         << ",\"guide_serial_a\":" << state.guideA << ",\"guide_serial_b\":" << state.guideB
                         << ",\"guide_epoch\":" << state.guideEpoch << ",\"sha256\":\"" << hash.finish()
                         << "\",\"view_index\":" << viewIndex << ",\"b0_size\":" << b0Size
                         << ",\"b1_resource\":" << b1View->resource
                         << ",\"b1_gpu_address\":" << b1View->description.BufferLocation
                         << ",\"b1_size\":" << b1View->description.SizeInBytes
                         << ",\"b1_CBV_creation_qpc\":" << b1View->qpc << ",\"b1_descriptor_cpu\":" << b1View->cpu
                         << ",\"b1_descriptor_gpu\":" << b1View->gpu << ",\"b1_heap_index\":" << b1View->index
                         << ",\"b1_root_table_offset\":" << b1View->offset
                         << ",\"bound_root_table0_gpu\":" << bound->rootTables[0].ptr << ",\"b0_hex\":\"";
            const char* b0Hex = "0123456789abcdef";
            for (size_t i = 0; i < b0Size; ++i) {
                const auto value = static_cast<const unsigned char*>(b0)[i];
                state.events << b0Hex[value >> 4] << b0Hex[value & 15];
            }
            state.events << "\",\"b1_hex\":\"";
            const auto* bytes = static_cast<const unsigned char*>(b1);
            const char* hex = "0123456789abcdef";
            for (size_t i = 0; i < 256; ++i)
                state.events << hex[bytes[i] >> 4] << hex[bytes[i] & 15];
            state.events << "\"}\n";
            for (unsigned i = 0; i < 4; ++i) {
                const auto& srv = state.srvs[i];
                const bool array = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                state.events << "{\"sequence\":" << ++state.sequence
                             << ",\"event\":\"actual-selected-binding\",\"slot\":" << reinterpret_cast<uintptr_t>(slot)
                             << ",\"heap\":" << reinterpret_cast<uintptr_t>(state.heap.Get())
                             << ",\"command_list\":" << reinterpret_cast<uintptr_t>(commands) << ",\"eye\":" << eye
                             << ",\"register\":" << (i < 2 ? i : i + 5)
                             << ",\"resource\":" << reinterpret_cast<uintptr_t>(state.consumers[i].Get())
                             << ",\"descriptor_increment\":" << state.descriptorIncrement
                             << ",\"format\":" << srv.Format << ",\"dimension\":" << srv.ViewDimension
                             << ",\"component_mapping\":" << srv.Shader4ComponentMapping << ",\"most_detailed_mip\":"
                             << (array ? srv.Texture2DArray.MostDetailedMip : srv.Texture2D.MostDetailedMip)
                             << ",\"mip_levels\":" << (array ? srv.Texture2DArray.MipLevels : srv.Texture2D.MipLevels)
                             << ",\"first_slice\":" << (array ? srv.Texture2DArray.FirstArraySlice : 0)
                             << ",\"slice_count\":" << (array ? srv.Texture2DArray.ArraySize : 1) << ",\"plane_slice\":"
                             << (array ? srv.Texture2DArray.PlaneSlice : srv.Texture2D.PlaneSlice)
                             << ",\"min_lod_bits\":"
                             << bits(array ? srv.Texture2DArray.ResourceMinLODClamp : srv.Texture2D.ResourceMinLODClamp)
                             << "}\n";
            }
            // Actual Draw just consumed all four bound SRVs. Record that GPU
            // read use before choosing any diagnostic copy state or allocation.
            if (!distinctGuidePair(reinterpret_cast<uintptr_t>(state.consumers[2].Get()),
                                   reinterpret_cast<uintptr_t>(state.consumers[3].Get()))) {
                fail("consumer-guide-histories-alias-or-missing");
                return;
            }
            std::array<ReadCopyPlan, 4> copyPlans{};
            for (unsigned i = 0; i < 4; ++i) {
                State::ResourceState* observed = nullptr;
                for (auto& item : state.resourceStates)
                    if (item.resource == state.consumers[i].Get())
                        observed = &item;
                if (!state.consumers[i] || !observed) {
                    fail("consumer-draw-state-provenance-missing");
                    return;
                }
                const auto desc = state.consumers[i]->GetDesc();
                // Current atlas colour histories and guide atlases are ordinary
                // non-depth textures. Other resource kinds need separate proof.
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
                    (desc.Flags &
                     (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                      D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))) {
                    fail("consumer-state-resource-kind-unqualified");
                    return;
                }
                if (!observed->read.drawRead(reinterpret_cast<uintptr_t>(commands), true)) {
                    fail("consumer-original-Draw-read-state-unproved");
                    return;
                }
                event("actual-consumer-Draw-read-state",
                      reinterpret_cast<uintptr_t>(state.consumers[i].Get()),
                      observed->read.value,
                      observed->read.implicit);
                if (!state.drawn) {
                    copyPlans[i] = observed->read.copyRead(reinterpret_cast<uintptr_t>(commands));
                    if (!copyPlans[i].valid) {
                        fail("consumer-copy-read-policy-unproved");
                        return;
                    }
                }
            }
            if (state.drawn)
                return;
            static const char* roles[] = {
                "consumer-colour-A", "consumer-colour-B", "consumer-depth-A", "consumer-depth-B"};
            if (state.consumers[0].Get() != state.colourHistory[0] ||
                state.consumers[1].Get() != state.colourHistory[1] ||
                state.colourHistory[0] == state.colourHistory[1]) {
                fail("actual-t0-t1-selected-history-mismatch");
                return;
            }
            state.gpu.reserve(5);
            uint64_t required{};
            std::array<Image12, 4> planned;
            for (unsigned i = 0; i < 4; ++i) {
                if (!state.consumers[i] || !plan12(roles[i], state.consumers[i].Get(), planned[i])) {
                    fail("consumer-binding-layout");
                    return;
                }
                required += planned[i].bytes;
            }
            Image12 plannedOutput;
            if (!state.outputs[0] || !plan12("generated", state.outputs[0].Get(), plannedOutput) ||
                required + plannedOutput.bytes > state.gpuBudget) {
                fail("complete-GPU-readback-budget");
                return;
            }
            for (auto& image : planned) {
                if (!allocate12(image)) {
                    fail("consumer-readback-allocation");
                    return;
                }
                state.gpu.push_back(std::move(image));
            }
            for (unsigned i = 0; i < state.gpu.size(); ++i) {
                auto& image = state.gpu[i];
                const auto& plan = copyPlans[i];
                event(plan.implicit ? "consumer-copy-implicit-read-accumulation" : "consumer-copy-explicit-read-policy",
                      reinterpret_cast<uintptr_t>(image.source.Get()),
                      plan.before,
                      plan.transition);
                // Implicit read promotion permits accumulating COPY_SOURCE. Do
                // not introduce an explicit COMMON barrier or suppress decay.
                copy12(commands, image, plan.before, plan.transition);
            }
            state.drawn = true;
        } catch (...) {
            abortException();
        }
    }
    bool holdRetire(void* context, uint64_t token) noexcept {
        if (nr::requested()) {
            return nr::fgHoldRetire(context, token);
        }
        try {
            std::lock_guard lock(state.mutex);
            if (context != state.context || token != state.select.tokenB || !token)
                return false;
            if (!state.workerStarted && !state.failure.empty() && state.fullFenceB) {
                const auto completed = state.fullFenceB->GetCompletedValue();
                if (completed != UINT64_MAX && completed >= state.ticketB.output_ready_value) {
                    state.ownership.productComplete = state.ownership.workerExited = true;
                    state.retireReady = state.ownership.canRetire();
                }
            }
            if (state.select.hold(token, state.retireReady)) {
                if (!state.retireHeldReported) {
                    event("private-retire-held", reinterpret_cast<uintptr_t>(context), token);
                    state.retireHeldReported = true;
                }
                return true;
            }
            event("private-retire-gate-open", reinterpret_cast<uintptr_t>(context), token);
            return false;
        } catch (...) {
            return true;
        }
    }
    void abort(void* context, const char* reason) noexcept {
        if (nr::requested()) {
            nr::fgAbort(context);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (state.armed && context == state.context && !state.finished)
                fail(reason);
        } catch (...) {
            abortException();
        }
    }
    void queued(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands) noexcept {
        if (nr::requested()) {
            nr::fgQueued(queue, count, commands);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.finished || queue != state.queue.Get())
                return;
            if (!commands || !count || count > 16) {
                fail("original-Execute-batch-unqualified");
                return;
            }
            const uint64_t batch = ++state.executeSequence;
            std::array<uintptr_t, 16> listIds{};
            event("original-Execute-batch", reinterpret_cast<uintptr_t>(queue), count, batch);
            for (UINT i = 0; i < count; ++i) {
                if (!commands[i]) {
                    fail("null-original-Execute-list");
                    return;
                }
                listIds[i] = reinterpret_cast<uintptr_t>(commands[i]);
                event("original-queue-ExecuteCommandLists", reinterpret_cast<uintptr_t>(queue), listIds[i], batch);
            }
            // One update for the complete actual API batch. This is the predicted
            // GPU state at that ordered batch's completion, never CPU completion.
            for (auto& item : state.resourceStates) {
                if (item.resource && item.read.executeBatch(listIds.data(), count))
                    event("predicted-implicit-read-decay-at-Execute-completion",
                          reinterpret_cast<uintptr_t>(item.resource),
                          item.read.decayedReadMask,
                          batch);
            }
        } catch (...) {
            abortException();
        }
    }
    void signalled(
        const char* name, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value, HRESULT result) noexcept {
        if (nr::requested()) {
            nr::fgSignalled(name, queue, fence, value, result);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (state.armed && !state.finished && queue == state.queue.Get()) {
                if (state.select.phase == Phase::a_provisional && std::strcmp(name, "original-core-signal") == 0)
                    state.coreSignalA = true;
                LARGE_INTEGER qpc{};
                QueryPerformanceCounter(&qpc);
                state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << qpc.QuadPart << ",\"event\":\""
                             << name << "\",\"queue\":" << reinterpret_cast<uintptr_t>(queue)
                             << ",\"fence\":" << reinterpret_cast<uintptr_t>(fence) << ",\"value\":" << value
                             << ",\"original_hresult\":" << result << "}\n";
            }
        } catch (...) {
            abortException();
        }
    }
    void rtv(ID3D12Resource* resource,
             const D3D12_RENDER_TARGET_VIEW_DESC& desc,
             D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
        if (nr::requested()) {
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            checkRequest();
            if (!state.requested || state.finished)
                return;
            for (auto& item : state.rtvs)
                if (!item.handle || item.handle == handle.ptr) {
                    item = {handle.ptr, resource, desc};
                    return;
                }
            fail("RTV-observation-capacity");
        } catch (...) {
            abortException();
        }
    }
    void targets(ID3D12GraphicsCommandList* commands, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles) noexcept {
        if (nr::requested()) {
            nr::fgTargets(commands, count, handles);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.select.phase != Phase::b_provisional)
                return;
            if (!handles || !count || count > 2) {
                fail("MRT-count-unqualified");
                return;
            }
            for (auto& item : state.renderTargets)
                if (!item.commands || item.commands == commands) {
                    item.commands = commands;
                    item.count = count;
                    for (UINT i = 0; i < count; ++i)
                        item.handles[i] = handles[i];
                    return;
                }
            fail("draw-list-observation-capacity");
        } catch (...) {
            abortException();
        }
    }
    void viewport(ID3D12GraphicsCommandList* commands,
                  const D3D12_VIEWPORT& viewport,
                  const D3D12_RECT& scissor) noexcept {
        if (nr::requested()) {
            nr::fgViewport(commands, viewport, scissor);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.select.phase != Phase::b_provisional)
                return;
            for (auto& item : state.renderTargets)
                if (item.commands == commands) {
                    item.viewport = viewport;
                    item.scissor = scissor;
                    return;
                }
            fail("viewport-without-render-targets");
        } catch (...) {
            abortException();
        }
    }
    void ticket(void* context, const VDXRFG_TicketV1& ticket) noexcept {
        if (nr::requested()) {
            nr::fgTicket(context, ticket);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.context != context || !state.select.accepts(ticket.current_serial))
                return;
            state.inputTicket = ticket;
            if (state.select.phase == Phase::a_provisional)
                return;
            if (ticket.previous_serial != state.select.serialA || ticket.current_serial != state.select.serialB ||
                ticket.generation != state.select.generation || ticket.bank > 1) {
                fail("prospective-ticket-lineage");
                return;
            }
            state.prospective = ticket;
            event("original-prospective-B-ticket", reinterpret_cast<uintptr_t>(context), ticket.token, ticket.bank);
        } catch (...) {
            abortException();
        }
    }
    void history(ID3D12Resource* source,
                 ID3D12Resource* target,
                 ID3D12GraphicsCommandList* commands,
                 uint64_t serial,
                 uint32_t slot) noexcept {
        if (nr::requested()) {
            nr::fgHistory(source, target, commands, serial, slot);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || !state.select.accepts(state.inputTicket.current_serial))
                return;
            const unsigned frame = state.select.phase == Phase::a_provisional ? 0u : 1u;
            state.colourHistory[frame] = target;
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            const auto desc = target->GetDesc();
            state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << qpc.QuadPart
                         << ",\"event\":\"actual-selected-history-copy\",\"host_serial\":"
                         << state.inputTicket.current_serial << ",\"host_ticket\":" << state.inputTicket.token
                         << ",\"generation\":" << state.inputTicket.generation << ",\"history_serial\":" << serial
                         << ",\"history_slot\":" << slot << ",\"source\":" << reinterpret_cast<uintptr_t>(source)
                         << ",\"target\":" << reinterpret_cast<uintptr_t>(target)
                         << ",\"command_list\":" << reinterpret_cast<uintptr_t>(commands) << ",\"width\":" << desc.Width
                         << ",\"height\":" << desc.Height << ",\"array_size\":" << desc.DepthOrArraySize
                         << ",\"format\":" << desc.Format << "}\n";
        } catch (...) {
            abortException();
        }
    }
    void barriers(ID3D12GraphicsCommandList* commands, UINT count, const D3D12_RESOURCE_BARRIER* barriers) noexcept {
        if (nr::requested()) {
            nr::fgBarriers(commands, count, barriers);
            return;
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.finished)
                return;
            if (!barriers || count > 64) {
                fail("original-barrier-count-unqualified");
                return;
            }
            for (UINT i = 0; i < count; ++i) {
                const auto& barrier = barriers[i];
                if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                    continue;
                auto* resource = barrier.Transition.pResource;
                if (!resource) {
                    fail("null-original-transition-resource");
                    return;
                }
                State::ResourceState* tracked = nullptr;
                for (auto& item : state.resourceStates)
                    if (item.resource == resource) {
                        tracked = &item;
                        break;
                    }
                if (!tracked)
                    for (auto& item : state.resourceStates)
                        if (!item.resource) {
                            tracked = &item;
                            item.resource = resource;
                            break;
                        }
                if (!tracked) {
                    fail("resource-state-observation-capacity");
                    return;
                }
                const auto description = resource->GetDesc();
                const bool full = barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
                                  description.MipLevels == 1 && description.DepthOrArraySize == 1 &&
                                      barrier.Transition.Subresource == 0;
                tracked->read.transition(reinterpret_cast<uintptr_t>(commands),
                                         barrier.Transition.StateBefore,
                                         barrier.Transition.StateAfter,
                                         barrier.Flags,
                                         full);
                state.events << "{\"sequence\":" << ++state.sequence
                             << ",\"event\":\"original-resource-transition\",\"command_list\":"
                             << reinterpret_cast<uintptr_t>(commands)
                             << ",\"resource\":" << reinterpret_cast<uintptr_t>(resource)
                             << ",\"subresource\":" << barrier.Transition.Subresource
                             << ",\"before\":" << barrier.Transition.StateBefore
                             << ",\"after\":" << barrier.Transition.StateAfter << ",\"flags\":" << barrier.Flags
                             << "}\n";
            }
        } catch (...) {
            abortException();
        }
    }

    void host(const Host* packet, void* boundaryCaller) noexcept {
        if (nr::requested()) {
            if (packet && packet->size == sizeof(Host) && packet->version == 2)
                nr::fgHost(*packet);
            return;
        }
        try {
            if (!packet || packet->size != sizeof(Host) || packet->version != 2)
                return;
            std::lock_guard lock(state.mutex);
            checkRequest();
            if (packet->operation == 7) {
                // Bounded scalar copy only: no COM calls, GPU work, file writes or borrowed pointer dereferences.
                if (state.requested && state.bound && packet->context == state.context) {
                    if (state.outputCount == state.outputRows.size()) {
                        state.outputOverflow = true;
                        return;
                    }
                    state.outputRows[state.outputCount++] = {packet->output,
                                                             packet->ticket,
                                                             reinterpret_cast<uintptr_t>(packet->context),
                                                             packet->originalResult,
                                                             packet->serial};
                }
                return;
            }
            if (state.requested && packet->operation == 4 && packet->eventName) {
                LARGE_INTEGER qpc{};
                QueryPerformanceCounter(&qpc);
                const std::string name = packet->eventName;
                const bool early = name == "producer-lease-begin" || name == "prior-input-consumed-Wait-result";
                if (early && packet->entry.count &&
                    (!state.armed || state.select.phase == Phase::armed || state.select.phase == Phase::a_admitted) &&
                    packet->serial != state.select.serialA) {
                    if (state.earlySerial != packet->serial) {
                        state.earlySerial = packet->serial;
                        state.earlyCount = 0;
                    }
                    if (state.earlyCount >= state.early.size()) {
                        fail("early-selected-boundary-capacity");
                        return;
                    }
                    state.early[state.earlyCount++] = {name,
                                                       reinterpret_cast<uintptr_t>(packet->eventObject),
                                                       reinterpret_cast<uintptr_t>(packet->eventOther),
                                                       packet->eventValue,
                                                       static_cast<uint64_t>(qpc.QuadPart),
                                                       packet->originalResult,
                                                       reinterpret_cast<uintptr_t>(packet->context),
                                                       packet->serial,
                                                       GetCurrentThreadId()};
                    return;
                }
                if (!state.armed) {
                    if (name.find("export") != std::string::npos || name.find("import") != std::string::npos) {
                        if (state.initial.size() >= 2048) {
                            fail("initial-observation-overflow");
                            return;
                        }
                        state.initial.push_back({name,
                                                 reinterpret_cast<uintptr_t>(packet->eventObject),
                                                 reinterpret_cast<uintptr_t>(packet->eventOther),
                                                 packet->eventValue,
                                                 static_cast<uint64_t>(qpc.QuadPart),
                                                 packet->originalResult});
                    }
                    return;
                }
                if (packet->serial == state.select.serialA || packet->serial == state.select.serialB ||
                    !state.finished &&
                        (name.find("export") != std::string::npos || name.find("import") != std::string::npos)) {
                    const bool hasResult =
                        name.find("-void") == std::string::npos && name.find("lease") == std::string::npos &&
                        name.find("device") == std::string::npos && name.find("xrEndFrame") == std::string::npos;
                    state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << qpc.QuadPart
                                 << ",\"thread\":" << GetCurrentThreadId()
                                 << ",\"event\":" << jsonQuote(packet->eventName)
                                 << ",\"context\":" << reinterpret_cast<uintptr_t>(packet->context)
                                 << ",\"serial\":" << packet->serial
                                 << ",\"object\":" << reinterpret_cast<uintptr_t>(packet->eventObject)
                                 << ",\"other\":" << reinterpret_cast<uintptr_t>(packet->eventOther)
                                 << ",\"value\":" << packet->eventValue
                                 << ",\"result\":" << (hasResult ? std::to_string(packet->originalResult) : "null")
                                 << ",\"ticket_token\":" << packet->ticket.token
                                 << ",\"ticket_bank\":" << packet->ticket.bank
                                 << ",\"ticket_generation\":" << packet->ticket.generation << "}\n";
                }
                return;
            }
            if (!state.requested || !state.bound || packet->context != state.context)
                return;
            if (packet->operation != 0 && (!state.armed || !state.select.accepts(packet->serial)) &&
                !(packet->operation == 2 &&
                  (packet->serial == state.select.serialA || packet->serial == state.select.serialB)))
                return;
            if (packet->operation == 0) {
                if (!arm())
                    return;
                if (state.select.phase == Phase::armed && !packet->entry.count)
                    return;
                if (!state.select.begin(packet->serial,
                                        static_cast<uint64_t>(packet->displayTime),
                                        reinterpret_cast<uintptr_t>(packet->context),
                                        reinterpret_cast<uintptr_t>(packet->producer)))
                    return;
                if (state.earlySerial == packet->serial) {
                    for (UINT i = 0; i < state.earlyCount; ++i) {
                        const auto& row = state.early[i];
                        const bool result = row.name == "prior-input-consumed-Wait-result";
                        state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << row.qpc
                                     << ",\"thread\":" << row.thread << ",\"event\":" << jsonQuote(row.name)
                                     << ",\"context\":" << row.context << ",\"serial\":" << row.serial
                                     << ",\"object\":" << row.object << ",\"other\":" << row.other
                                     << ",\"value\":" << row.value
                                     << ",\"result\":" << (result ? std::to_string(row.result) : "null")
                                     << ",\"buffered_before_internal_arm\":true}\n";
                    }
                    state.earlyCount = 0;
                }
                state.effectiveNr[state.select.phase == Phase::a_provisional ? 0 : 1] = packet->nrEnabled;
                if (packet->nrEnabled || !packet->hasGuide || !packet->canonical) {
                    fail("NRoff-complete-guide-required");
                    return;
                }
                if (!callerWitness(*packet, boundaryCaller)) {
                    fail("actual-expected-caller-not-in-selected-prelease-call-stack");
                    return;
                }
                if (state.select.phase == Phase::a_provisional && !preflight(*packet)) {
                    fail("pair-preflight-budget");
                    return;
                }
                if (state.select.phase == Phase::a_provisional) {
                    packet->producer->GetDevice(&state.submissionDevice);
                    state.info11Result = state.submissionDevice.As(&state.info11);
                    state.info12Result = state.device.As(&state.info12);
                    state.debugStart11 =
                        state.info11 ? state.info11->GetNumStoredMessagesAllowedByRetrievalFilter() : 0;
                    state.debugStart12 =
                        state.info12 ? state.info12->GetNumStoredMessagesAllowedByRetrievalFilter() : 0;
                }
                const char* label = state.select.phase == Phase::a_provisional ? "A" : "B";
                for (unsigned eye = 0; eye < 2; ++eye) {
                    const auto& d = packet->eyes[eye].description;
                    const auto& original = state.originalRaw[eye];
                    if (d.Width != original.Width || d.Height != original.Height || d.ArraySize != original.ArraySize ||
                        d.MipLevels != original.MipLevels || d.Format != original.Format) {
                        fail("raw-layout-changed-within-pair");
                        return;
                    }
                    const std::string role = std::string("raw-") + label + (eye ? "-right" : "-left");
                    if (!stage11(role.c_str(), packet->producer, packet->eyes[eye].source, &packet->eyes[eye])) {
                        fail("raw-staging-failed");
                        return;
                    }
                }
                event("selected-prior-consumed-dependency-metadata",
                      reinterpret_cast<uintptr_t>(packet->priorConsumedFence),
                      packet->priorConsumedValue);
            } else if (packet->operation == 1) {
                if (state.select.phase != Phase::a_provisional && state.select.phase != Phase::b_provisional)
                    return;
                if (!stage11(state.select.phase == Phase::a_provisional ? "host-depth-A" : "host-depth-B",
                             packet->producer,
                             packet->canonical)) {
                    fail("canonical-staging-failed");
                    return;
                }
            } else if (packet->operation == 2) {
                LARGE_INTEGER qpc{};
                QueryPerformanceCounter(&qpc);
                state.events << "{\"sequence\":" << ++state.sequence << ",\"qpc\":" << qpc.QuadPart
                             << ",\"thread\":" << GetCurrentThreadId()
                             << ",\"event\":\"original-Submit-result-ticket\",\"context\":"
                             << reinterpret_cast<uintptr_t>(packet->context) << ",\"serial\":" << packet->serial
                             << ",\"hresult\":" << packet->originalResult << ",\"token\":" << packet->ticket.token
                             << ",\"bank\":" << packet->ticket.bank << ",\"generation\":" << packet->ticket.generation
                             << ",\"previous_serial\":" << packet->ticket.previous_serial
                             << ",\"current_serial\":" << packet->ticket.current_serial << ",\"output_ready_fence\":"
                             << reinterpret_cast<uintptr_t>(packet->ticket.output_ready_fence)
                             << ",\"output_ready_value\":" << packet->ticket.output_ready_value << "}\n";
                if (packet->ticket.generation != state.ready.generation ||
                    !state.select.admit(packet->originalResult,
                                        packet->ticket.token,
                                        packet->ticket.previous_serial,
                                        packet->ticket.current_serial,
                                        packet->ticket.generation,
                                        packet->guideEpoch,
                                        reinterpret_cast<uintptr_t>(packet->context))) {
                    fail("original-admission-failed");
                    return;
                }
                event("original-admission",
                      reinterpret_cast<uintptr_t>(packet->context),
                      packet->ticket.token,
                      packet->ticket.current_serial);
                if (state.select.phase == Phase::a_admitted)
                    state.ticketA = packet->ticket;
                else {
                    state.ticketB = packet->ticket;
                    state.fullFenceB = static_cast<ID3D12Fence*>(packet->ticket.output_ready_fence);
                    if (!state.drawn || state.selectedEyeMask != 3 || state.prospective.token != packet->ticket.token ||
                        state.prospective.bank != packet->ticket.bank) {
                        fail("selected-consumer-draw-or-MRT-ticket-missing");
                        return;
                    }
                    // Create before returning B admission so every host role0 GPU
                    // read can wait on this exact extra-copy completion timeline.
                    if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state.copyFence)))) {
                        fail("private-copy-shared-fence-allocation-failed");
                        return;
                    }
                    HMODULE self{};
                    if (!GetModuleHandleExW(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &self)) {
                        fail("observer-module-self-reference-failed");
                        return;
                    }
                    state.workerStarted = true;
                    HANDLE thread = CreateThread(nullptr, 0, worker, self, 0, nullptr);
                    if (thread)
                        CloseHandle(thread);
                    else {
                        state.workerStarted = false;
                        FreeLibrary(self);
                        fail("observer-worker-start-failed");
                    }
                }
            } else if (packet->operation == 3) {
                event("producer-signal-flush-observed",
                      reinterpret_cast<uintptr_t>(packet->producerFence),
                      packet->producerValue,
                      reinterpret_cast<uintptr_t>(packet->importedProducerFence));
            } else if (packet->operation == 5) {
                const auto& srv = packet->normalizerSrv;
                event("original-normalizer-Dispatch",
                      reinterpret_cast<uintptr_t>(packet->eventObject),
                      packet->serial,
                      packet->eventValue);
                state.events << "{\"sequence\":" << ++state.sequence
                             << ",\"event\":\"original-normalizer-view\",\"serial\":" << packet->serial
                             << ",\"eye\":" << packet->eventValue
                             << ",\"view\":" << reinterpret_cast<uintptr_t>(packet->eventOther)
                             << ",\"viewed_resource\":" << reinterpret_cast<uintptr_t>(packet->normalizerTexture)
                             << ",\"format\":" << srv.Format << ",\"dimension\":" << srv.ViewDimension
                             << ",\"first_slice\":" << srv.Texture2DArray.FirstArraySlice
                             << ",\"slice_count\":" << srv.Texture2DArray.ArraySize
                             << ",\"mip\":" << srv.Texture2DArray.MostDetailedMip
                             << ",\"mip_levels\":" << srv.Texture2DArray.MipLevels << "}\n";
            } else if (packet->operation == 6) {
                for (unsigned eye = 0; eye < 2; ++eye) {
                    state.events << "{\"sequence\":" << ++state.sequence
                                 << ",\"event\":\"actual-final-colour-input-copy\",\"host_serial\":" << packet->serial
                                 << ",\"eye\":" << eye
                                 << ",\"context\":" << reinterpret_cast<uintptr_t>(packet->producer)
                                 << ",\"source\":" << reinterpret_cast<uintptr_t>(packet->colourSources[eye])
                                 << ",\"target11\":" << reinterpret_cast<uintptr_t>(packet->eventObject)
                                 << ",\"target12\":" << reinterpret_cast<uintptr_t>(packet->eventOther)
                                 << ",\"bgra_swap\":"
                                 << ((packet->eventValue & (uint64_t{1} << eye)) ? "true" : "false")
                                 << ",\"source_rect\":";
                    rectangle(state.events, packet->colourViews[eye].rect);
                    state.events << "}\n";
                }
            }
        } catch (...) {
            abortException();
        }
    }
    HRESULT outputWait(void* context, uint64_t token, uint32_t role, ID3D11DeviceContext4* consumer) noexcept {
        if (nr::requested()) {
            return nr::fgOutputWait(context, token, role, consumer);
        }
        try {
            std::lock_guard lock(state.mutex);
            if (!state.armed || state.context != context ||
                !state.select.outputWait(token, role, state.ownership.copyComplete))
                return S_OK;
            if (!consumer || !state.copyFence || !state.workerStarted || !state.failure.empty())
                return E_FAIL;
            if (!state.copyFence11) {
                ComPtr<ID3D11Device> base;
                consumer->GetDevice(&base);
                ComPtr<ID3D11Device5> device11;
                if (FAILED(base.As(&device11)))
                    return E_NOINTERFACE;
                HANDLE shared{};
                HRESULT result =
                    state.device->CreateSharedHandle(state.copyFence.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
                event("private-copy-fence-export12",
                      reinterpret_cast<uintptr_t>(state.copyFence.Get()),
                      reinterpret_cast<uintptr_t>(shared),
                      static_cast<uint32_t>(result));
                if (FAILED(result))
                    return result;
                result = device11->OpenSharedFence(shared, IID_PPV_ARGS(&state.copyFence11));
                CloseHandle(shared);
                event("private-copy-fence-import11",
                      reinterpret_cast<uintptr_t>(state.copyFence11.Get()),
                      reinterpret_cast<uintptr_t>(device11.Get()),
                      static_cast<uint32_t>(result));
                if (FAILED(result))
                    return result;
            }
            const HRESULT result = consumer->Wait(state.copyFence11.Get(), 1);
            event("selected-output-host-GPU-wait-private-copy",
                  reinterpret_cast<uintptr_t>(consumer),
                  reinterpret_cast<uintptr_t>(state.copyFence11.Get()),
                  static_cast<uint32_t>(result));
            if (FAILED(result))
                fail("selected-output-host-private-wait-failed");
            else
                state.ownership.hostWaitQueued = true;
            return result;
        } catch (...) {
            abortException();
            return E_FAIL;
        }
    }
    uint32_t status(uint32_t flags) noexcept {
        if (nr::requested()) {
            return nr::fgStatus(flags);
        }
        try {
            std::lock_guard lock(state.mutex);
            checkRequest();
            if (!state.requested)
                return 0;
            if ((flags & 1u) && state.device)
                debugReceipt();
            if ((flags & 2u) && state.ownership.canRelease() && state.retireReady && !state.released) {
                writeOutputLineage();
                state.gpu.clear();
                state.host.clear();
                state.outputs = {};
                state.consumers = {};
                state.heap.Reset();
                state.copyList.Reset();
                state.copyAllocator.Reset();
                state.copyFence.Reset();
                state.copyFence11.Reset();
                state.fullFenceB.Reset();
                state.info11.Reset();
                state.info12.Reset();
                state.submissionDevice.Reset();
                state.queue.Reset();
                state.device.Reset();
                state.released = true;
                event("observer-owned-GPU-refs-released");
                state.events.flush();
            }
            return 16u | (state.armed ? 32u : 0u) | (state.workerStarted ? 64u : 0u) | (state.finished ? 1u : 0u) |
                   (state.failure.empty() ? 0u : 2u) | (state.gpuUnknown ? 4u : 0u) | (state.retireReady ? 8u : 0u) |
                   (state.workerSettled ? 128u : 0u) | (state.released ? 256u : 0u);
        } catch (...) {
            return 2u;
        }
    }
} // namespace vdxrfg::pair_capture
extern "C" __declspec(dllexport) void WINAPI
VDXRFG_PrivatePairCaptureV1(const vdxrfg::pair_capture::Host* packet) noexcept {
    vdxrfg::pair_capture::host(packet, _ReturnAddress());
}
extern "C" __declspec(dllexport) uint32_t WINAPI VDXRFG_PrivatePairCaptureStatusV1(uint32_t flags) noexcept {
    return vdxrfg::pair_capture::status(flags);
}

extern "C" __declspec(dllexport) HRESULT WINAPI
VDXRFG_PrivatePairOutputWaitV1(void* context, uint64_t token, uint32_t role, ID3D11DeviceContext4* consumer) noexcept {
    return vdxrfg::pair_capture::outputWait(context, token, role, consumer);
}
