#include "PairCaptureNrSdk.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrWrites.h"
#include <windows.h>
#include <algorithm>
#include <cwchar>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct ResourceLifetime {
            uint64_t address{}, lifetime{};
        };
        struct Journal {
            ParameterMirror mirror;
            SdkWarmFacts facts;
            std::array<ResourceLifetime, 128> resources{};
            std::array<SdkObservation, 6> rows{};
            size_t rowCount{}, createCount{}, resourceCount{}, activeIndex{};
            std::array<bool, 6> present{};
            struct Dispatch {
                uint64_t evaluate{}, release{}, epoch{};
            };
            std::array<Dispatch, 64> dispatches{};
            size_t dispatchCount{};
            uint64_t nextDispatchEpoch{1};
            std::array<SdkObservation, 64> lifecycle{};
            size_t lifecycleCount{};
            bool resetCreateCycle{};
            uint64_t nextResourceLifetime{1};
            uint64_t nextLifetime{1}, nextCall{1}, activeToken{}, frameSerial{};
            const void* context{};
            uint32_t recordingThread{};
            vdxrfg::pair_capture::nr::NrRecordingOwnership recording{};
        };
        Journal journal;
        static_assert(sizeof(Journal) <= hostMetadataSdk);
        bool enabled() noexcept {
            static const bool selected = []() noexcept {
                wchar_t abi[8]{}, mode[16]{}, pid[32]{};
                if (GetEnvironmentVariableW(L"VDXR_PAIR_NR_ABI", abi, 8) != 1 || abi[0] != L'1' ||
                    GetEnvironmentVariableW(L"VDXR_PAIR_NR_MODE", mode, 16) != 9 ||
                    (std::wcscmp(mode, L"native100") && std::wcscmp(mode, L"spatial85")))
                    return false;
                const auto length = GetEnvironmentVariableW(L"VDXR_PAIR_PID", pid, 32);
                if (!length || length >= 32)
                    return false;
                uint64_t value = 0;
                for (DWORD i = 0; i < length; ++i) {
                    if (pid[i] < L'0' || pid[i] > L'9' ||
                        value > (UINT32_MAX - static_cast<unsigned>(pid[i] - L'0')) / 10)
                        return false;
                    value = value * 10 + static_cast<unsigned>(pid[i] - L'0');
                }
                return value == GetCurrentProcessId();
            }();
            return selected;
        }
        uint64_t next(uint64_t& v) noexcept {
            if (!v || v == UINT64_MAX)
                return 0;
            return v++;
        }
        uint64_t qpc() noexcept {
            LARGE_INTEGER v{};
            return QueryPerformanceCounter(&v) ? static_cast<uint64_t>(v.QuadPart) : 0;
        }
        uint64_t lifetime(uint64_t address) noexcept {
            for (size_t i = 0; i < journal.resourceCount; ++i)
                if (journal.resources[i].address == address)
                    return journal.resources[i].lifetime;
            return 0;
        }
        uint64_t dispatchEpoch(uint64_t evaluate, uint64_t release) noexcept {
            if (!evaluate || !release)
                return 0;
            for (size_t i = 0; i < journal.dispatchCount; ++i)
                if (journal.dispatches[i].evaluate == evaluate && journal.dispatches[i].release == release)
                    return journal.dispatches[i].epoch;
            if (journal.dispatchCount == journal.dispatches.size())
                return 0;
            const auto epoch = next(journal.nextDispatchEpoch);
            journal.dispatches[journal.dispatchCount++] = {evaluate, release, epoch};
            return epoch;
        }
        SdkObservation& active() noexcept {
            return journal.activeIndex < 6 ? journal.rows[journal.activeIndex]
                                           : journal.lifecycle[journal.activeIndex - 6];
        }
        bool qualifiedMap(const SdkObservation& row) noexcept {
            if (!row.parametersObserved)
                return false;
            for (size_t i = 0; i < 30; ++i)
                if (row.parameters[i].observationStatus != NR_OBSERVED_VALUE || !row.parameters[i].writeEpoch)
                    return false;
            return true;
        }
        uint64_t key(const char* name) noexcept {
            static const char* names[30] = {"DLSSNR.Width",
                                            "DLSSNR.Height",
                                            "DLSSNR.Hint.Render.Preset",
                                            "CreationNodeMask",
                                            "VisibilityNodeMask",
                                            "DLSSNR.DepthInverted",
                                            "DLSSNR.Enabled",
                                            "DLSSNR.Color",
                                            "DLSSNR.Output",
                                            "DLSSNR.ColorSubrectBaseX",
                                            "DLSSNR.ColorSubrectBaseY",
                                            "DLSSNR.ColorSubrectWidth",
                                            "DLSSNR.ColorSubrectHeight",
                                            "DLSSNR.OutputSubrectBaseX",
                                            "DLSSNR.OutputSubrectBaseY",
                                            "DLSSNR.OutputSubrectWidth",
                                            "DLSSNR.OutputSubrectHeight",
                                            "DLSSNR.Depth",
                                            "DLSSNR.DepthSubrectBaseX",
                                            "DLSSNR.DepthSubrectBaseY",
                                            "DLSSNR.DepthSubrectWidth",
                                            "DLSSNR.DepthSubrectHeight",
                                            "DLSSNR.Reset",
                                            "DLSSNR.UseAutoMask",
                                            "DLSSNR.Style",
                                            "DLSSNR.Intensity",
                                            "DLSSNR.LocalToneStrength",
                                            "DLSSNR.LocalStructureStrength",
                                            "DLSSNR.SkinStructureStrength",
                                            "DLSSNR.UICorrection"};
            if (!name)
                return 0;
            for (size_t i = 0; i < 30; ++i)
                if (std::strcmp(name, names[i]) == 0)
                    return i + 1;
            return 0;
        }
    } // namespace
    void sdkTableCreated(NVSDK_NGX_Parameter* table) noexcept {
        if (!enabled())
            return;
        if (!table)
            return;
        const auto epoch = next(journal.nextLifetime);
        journal.facts = {};
        journal.facts.tableAddress = reinterpret_cast<uint64_t>(table);
        journal.facts.tableLifetime = epoch;
        journal.facts.tableLive = true;
        journal.facts.invalid = !journal.mirror.bind(journal.facts.tableAddress, epoch);
        journal.rowCount = journal.createCount = 0;
        journal.present = {};
        journal.resetCreateCycle = false;
    }
    void sdkTableDestroyed(NVSDK_NGX_Parameter* table) noexcept {
        if (!enabled())
            return;
        if (reinterpret_cast<uint64_t>(table) == journal.facts.tableAddress)
            journal.facts.tableLive = false;
    }
    void sdkRecording(uint64_t serial,
                      const void* context,
                      const vdxrfg::pair_capture::nr::NrRecordingOwnership& actual) noexcept {
        if (!enabled())
            return;
        if (journal.frameSerial != serial) {
            journal.rowCount = journal.createCount;
            journal.present[4] = journal.present[5] = false;
            journal.facts.evaluateSequence[0] = journal.facts.evaluateSequence[1] = 0;
            journal.facts.completeFrame = false;
        }
        journal.frameSerial = serial;
        journal.facts.frameSerial = serial;
        journal.context = context;
        journal.recording = actual;
        journal.recordingThread = GetCurrentThreadId();
    }
    bool sdkCurrentRecording(ID3D12GraphicsCommandList* list,
                             uint64_t& serial,
                             vdxrfg::pair_capture::nr::NrRecordingOwnership& out) noexcept {
        serial = 0;
        out = {};
        if (!enabled() || !list || journal.recordingThread != GetCurrentThreadId() || !journal.frameSerial ||
            list != journal.recording.list || !journal.recording.device || !journal.recording.queue ||
            !journal.recording.fence || !journal.recording.allocator)
            return false;
        vdxrfg::pair_capture::nr::NrRecordingEpochFacts facts{};
        if (!vdxrfg::pair_capture::nr::TryBorrowRecordingEpochFacts(
                journal.context, journal.recording.facts.nrCommandContextLifetimeEpoch, journal.recording.facts, facts))
            return false;
        serial = journal.frameSerial;
        out = journal.recording;
        return true;
    }
    void sdkResourceCreated(ID3D12Resource* resource) noexcept {
        if (!enabled())
            return;
        sdkResourceLifetime(resource, next(journal.nextResourceLifetime));
    }
    uint64_t sdkObservedResourceLifetime(ID3D12Resource* resource) noexcept {
        if (!enabled())
            return 0;
        return lifetime(reinterpret_cast<uint64_t>(resource));
    }
    void sdkResourceLifetime(ID3D12Resource* resource, uint64_t epoch) noexcept {
        if (!enabled())
            return;
        const auto address = reinterpret_cast<uint64_t>(resource);
        if (!address || !epoch || epoch == UINT64_MAX) {
            journal.facts.invalid = true;
            return;
        }
        for (size_t i = 0; i < journal.resourceCount; ++i)
            if (journal.resources[i].address == address) {
                journal.resources[i].lifetime = epoch;
                return;
            }
        if (journal.resourceCount == journal.resources.size()) {
            journal.facts.invalid = true;
            return;
        }
        journal.resources[journal.resourceCount++] = {address, epoch};
    }
    void sdkResourceWrite(ID3D12Resource* resource, ID3D12GraphicsCommandList* list, uint64_t kind) noexcept {
        if (!enabled())
            return;
        if (!resource || list != journal.recording.list ||
            !observeOriginalWrite(reinterpret_cast<uint64_t>(resource),
                                  lifetime(reinterpret_cast<uint64_t>(resource)),
                                  journal.frameSerial,
                                  reinterpret_cast<uint64_t>(list),
                                  journal.recording.facts.nrRecordingEpoch,
                                  12,
                                  kind))
            journal.facts.invalid = true;
    }
    void sdkSetBits(NVSDK_NGX_Parameter* table, const char* name, uint64_t type, uint64_t bits) noexcept {
        if (!enabled())
            return;
        if (!journal.facts.tableLive || reinterpret_cast<uint64_t>(table) != journal.facts.tableAddress)
            return;
        const auto parameter = key(name);
        if (!parameter) {
            if (name && std::strncmp(name, "DLSSNR.", 7) == 0)
                journal.facts.invalid = true;
            return;
        }
        const auto resourceEpoch = type == NR_RESOURCE_ID && bits ? lifetime(bits) : 0;
        if (!journal.mirror.set(parameter, type, bits, resourceEpoch))
            journal.facts.invalid = true;
    }
    uint64_t sdkCallBegin(uint64_t kind,
                          uint32_t eye,
                          uint64_t dispatch,
                          ID3D12GraphicsCommandList* list,
                          NVSDK_NGX_Parameter* table,
                          uint64_t feature,
                          uint64_t evaluate,
                          uint64_t release,
                          ID3D12Device* device) noexcept {
        if (!enabled())
            return 0;
        if (kind != NR_INIT_MANUAL &&
            (!journal.facts.tableLive || reinterpret_cast<uint64_t>(table) != journal.facts.tableAddress || eye > 1))
            return 0;
        if (kind == NR_RELEASE_FEATURE) {
            if (feature != journal.facts.featureAddress[eye])
                journal.facts.invalid = true;
            // Original Release still runs. Its result is observed by the returned marker.
        }
        if ((kind == NR_CREATE_SDK || kind == NR_CREATE_MANUAL) && journal.resetCreateCycle) {
            journal.createCount = journal.rowCount = 0;
            journal.present = {};
            journal.resetCreateCycle = false;
        }
        if (kind != NR_CREATE_SDK && kind != NR_CREATE_MANUAL && kind != NR_EVALUATE && kind != NR_RELEASE_FEATURE &&
            kind != NR_INIT_MANUAL)
            return 0;
        if (journal.activeToken) {
            journal.facts.invalid = true;
            return 0;
        }
        size_t index{};
        if (kind == NR_RELEASE_FEATURE || kind == NR_INIT_MANUAL) {
            if (journal.lifecycleCount == journal.lifecycle.size()) {
                journal.facts.invalid = true;
                return 0;
            }
            index = 6 + journal.lifecycleCount;
        } else if (kind == NR_EVALUATE)
            index = 4 + eye;
        else {
            if (journal.createCount >= 4) {
                journal.facts.invalid = true;
                return 0;
            }
            index = journal.createCount;
        }
        if (index < 6 && journal.present[index]) {
            journal.facts.invalid = true;
            return 0;
        }
        journal.activeIndex = index;
        const auto token = next(journal.nextCall);
        if (!token) {
            journal.facts.invalid = true;
            return 0;
        }
        auto& observation = active();
        observation = {};
        auto& c = observation.call;
        c.kind = kind;
        c.eye = eye;
        c.dispatchAddressObserved = dispatch;
        const bool hasRecordingArguments = kind == NR_CREATE_SDK || kind == NR_CREATE_MANUAL || kind == NR_EVALUATE;
        if (hasRecordingArguments) {
            c.parameterTableAddressObserved = journal.facts.tableAddress;
            c.parameterTableLifetime = journal.facts.tableLifetime;
            c.listAddress = reinterpret_cast<uint64_t>(list);
            c.contextAddress = reinterpret_cast<uint64_t>(journal.context);
            c.contextLifetimeEpoch = journal.recording.facts.nrCommandContextLifetimeEpoch;
            c.queueAddress = reinterpret_cast<uint64_t>(journal.recording.queue);
            c.fenceAddress = reinterpret_cast<uint64_t>(journal.recording.fence);
            c.allocatorAddress = reinterpret_cast<uint64_t>(journal.recording.allocator);
            c.deviceAddress = reinterpret_cast<uint64_t>(journal.recording.device);
            c.recordingEpoch = journal.recording.facts.nrRecordingEpoch;
        } else if (kind == NR_INIT_MANUAL) {
            c.deviceAddress = reinterpret_cast<uint64_t>(device);
        }
        c.featureAddressObserved = feature;
        c.actual.pairedEvaluateAddress = evaluate;
        c.aux.pairedReleaseAddress = release;
        c.dispatchTableEpoch = dispatchEpoch(evaluate, release);
        c.resultType = NR_NGX;
        c.beginObserved = 1;
        c.qpcBegin = qpc();
        c.thread = GetCurrentThreadId();
        if (kind != NR_RELEASE_FEATURE && kind != NR_INIT_MANUAL) {
            observation.parametersObserved = journal.mirror.snapshot(index, observation.parameters.data(), 39);
            c.paramCount = 39;
        }
        journal.activeToken = token;
        journal.facts.totalSequence = token;
        return token;
    }
    void sdkCallEnd(uint64_t token, uint32_t result, uint64_t feature) noexcept {
        if (!token)
            return;
        if (journal.activeToken != token) {
            journal.facts.invalid = true;
            return;
        }
        auto& o = active();
        auto& c = o.call;
        c.resultBits = result;
        c.classifiedSuccess = (result & UINT32_C(0xfff00000)) != UINT32_C(0xbad00000);
        c.endObserved = 1;
        c.qpcEnd = qpc();
        c.featureAddressObserved = feature;
        o.ended = true;
        if (c.kind == NR_INIT_MANUAL) {
            ++journal.lifecycleCount;
            journal.activeToken = 0;
            return;
        }
        const auto eye = static_cast<size_t>(c.eye);
        if (c.kind == NR_CREATE_SDK || c.kind == NR_CREATE_MANUAL) {
            if (journal.activeIndex != journal.createCount || journal.createCount >= 4)
                journal.facts.invalid = true;
            if (result == 1 && feature) {
                journal.facts.featureAddress[eye] = feature;
                journal.facts.featureLifetime[eye] = next(journal.nextLifetime);
                journal.facts.evaluateAddress[eye] = c.actual.pairedEvaluateAddress;
                journal.facts.releaseAddress[eye] = c.aux.pairedReleaseAddress;
                journal.facts.featureLive[eye] = true;
            }
            ++journal.createCount;
        }
        if (c.kind == NR_RELEASE_FEATURE) {
            if (result == 1) {
                journal.facts.featureLive[eye] = false;
                if (!journal.facts.featureLive[0] && !journal.facts.featureLive[1])
                    journal.resetCreateCycle = true;
            } else
                journal.facts.invalid = true;
        }
        // A failed Create did not establish a new feature lifetime. Do not attach
        // the previous released feature's epoch to a null or unqualified result.
        const bool createsFeature = c.kind == NR_CREATE_SDK || c.kind == NR_CREATE_MANUAL;
        c.featureLifetimeEpoch =
            feature && feature == journal.facts.featureAddress[eye] && (!createsFeature || result == 1)
                ? journal.facts.featureLifetime[eye]
                : 0;
        if (c.kind == NR_EVALUATE) {
            if (result == 1 && o.parametersObserved) {
                for (const auto& parameter : o.parameters) {
                    if (parameter.parameterEnum == NR_OUTPUT && parameter.typeEnum == NR_RESOURCE_ID &&
                        parameter.observationStatus == NR_OBSERVED_VALUE && parameter.valueLo && parameter.valueHi) {
                        if (!observeOriginalWrite(parameter.valueLo,
                                                  parameter.valueHi,
                                                  journal.frameSerial,
                                                  c.listAddress,
                                                  c.recordingEpoch,
                                                  12,
                                                  NR_EVALUATE,
                                                  token))
                            journal.facts.invalid = true;
                    }
                }
            }
            if (result == 1 && journal.facts.featureLive[eye] && feature == journal.facts.featureAddress[eye] &&
                c.dispatchAddressObserved == journal.facts.evaluateAddress[eye]) {
                journal.facts.evaluateSequence[eye] = token;
                journal.facts.completeFrame = journal.facts.evaluateSequence[0] && journal.facts.evaluateSequence[1] &&
                                              qualifiedMap(journal.rows[4]) && qualifiedMap(journal.rows[5]);
            } else
                journal.facts.invalid = true;
        }
        if (journal.activeIndex < 6) {
            journal.present[journal.activeIndex] = true;
            ++journal.rowCount;
        } else
            ++journal.lifecycleCount;
        journal.activeToken = 0;
    }
    void sdkCallThrew(uint64_t token) noexcept {
        if (token && journal.activeToken == token) {
            active().threw = true;
            journal.facts.invalid = true;
            journal.activeToken = 0;
            if (journal.activeIndex < 6) {
                journal.present[journal.activeIndex] = true;
                ++journal.rowCount;
            } else
                ++journal.lifecycleCount;
        }
    }
    bool sdkWarmFacts(SdkWarmFacts& out) noexcept {
        out = {};
        if (!enabled())
            return false;
        out = journal.facts;
        return out.tableLive && !out.invalid && out.completeFrame;
    }
    bool sdkCopyLifecycle(SdkObservation* out, size_t capacity, size_t& count) noexcept {
        count = 0;
        if (!out || capacity < journal.lifecycleCount || journal.activeToken)
            return false;
        std::copy_n(journal.lifecycle.data(), journal.lifecycleCount, out);
        count = journal.lifecycleCount;
        return true;
    }
    bool sdkCopyObservations(SdkObservation* out, size_t capacity, size_t& count) noexcept {
        count = 0;
        if (!out || capacity < journal.rowCount || journal.activeToken || journal.facts.invalid)
            return false;
        for (size_t i = 0; i < journal.rows.size(); ++i)
            if (journal.present[i]) {
                out[count] = journal.rows[i];
                for (auto& parameter : out[count].parameters)
                    parameter.callIndex = count;
                ++count;
            }
        return count == journal.rowCount;
    }
} // namespace virtualdesktop_openxr::nr_capture
