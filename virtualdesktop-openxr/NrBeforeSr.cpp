#include "pch.h"

#include "NrBeforeSr.h"
#include "NrBeforeSrColor.h"
#include "NrBeforeSrHistoryBank.h"
#include "NrBeforeSrHooks.h"
#include "NrBeforeSrModel.h"
#include "NrBeforeSrRecording.h"
#include "NrBeforeSrStereo.h"
#include "log.h"

#include <array>
#include <atomic>
#include <bcrypt.h>
#include <limits>
#include <mutex>

namespace virtualdesktop_openxr::nr_before_sr {
    namespace {
        using log::g_traceProvider;
        using Microsoft::WRL::ComPtr;

        std::uintptr_t Identity(IUnknown* object) noexcept {
            ComPtr<IUnknown> identity;
            if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))))
                return 0;
            return reinterpret_cast<std::uintptr_t>(identity.Get());
        }

        std::uintptr_t DeviceIdentity(ID3D12DeviceChild* child) noexcept {
            ComPtr<ID3D12Device> device;
            if (!child || FAILED(child->GetDevice(IID_PPV_ARGS(&device))))
                return 0;
            return Identity(device.Get());
        }

        // The Hubris build is admitted only after its native D3D12 SDK call path has been inspected.
        // If Steam updates the executable, the usual VDXR NR path remains available.
        bool HasQualifiedImageHash(const wchar_t* path) noexcept {
            constexpr std::array<UCHAR, 32> expected{0x18, 0x0a, 0xd1, 0xb1, 0xa3, 0x5d, 0x1e, 0xe6, 0x64, 0x0d, 0x2c,
                                                     0x16, 0x1d, 0x86, 0x46, 0x0f, 0x12, 0x65, 0x73, 0x0d, 0x2e, 0x51,
                                                     0x05, 0x15, 0x6b, 0xd3, 0xe8, 0xd0, 0xec, 0x64, 0x5c, 0xf5};
            wil::unique_handle file(CreateFileW(
                path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!file || file.get() == INVALID_HANDLE_VALUE)
                return false;

            BCRYPT_HASH_HANDLE digest{};
            if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &digest, nullptr, 0, nullptr, 0, 0) < 0)
                return false;
            auto cleanup = wil::scope_exit([&] { BCryptDestroyHash(digest); });
            std::array<UCHAR, 32768> buffer{};
            DWORD used{};
            do {
                if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &used, nullptr) ||
                    (used && BCryptHashData(digest, buffer.data(), used, 0) < 0))
                    return false;
            } while (used);
            std::array<UCHAR, 32> actual{};
            return BCryptFinishHash(digest, actual.data(), static_cast<ULONG>(actual.size()), 0) >= 0 &&
                   actual == expected;
        }

        bool QualifiedLaunch() noexcept {
            std::array<wchar_t, 32768> actual{};
            const auto capacity = static_cast<DWORD>(actual.size());
            const DWORD length = GetModuleFileNameW(nullptr, actual.data(), capacity);
            return length && length < capacity && IsHubrisShippingImage({actual.data(), length}) &&
                   HasQualifiedImageHash(actual.data());
        }

        bool HubrisDisabledDlss() noexcept try {
            wil::unique_cotaskmem_string localAppData;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, localAppData.put())))
                return false;
            const auto file =
                std::filesystem::path(localAppData.get()) / L"HubrisVR/Saved/Config/WindowsNoEditor/Graphics.ini";
            wchar_t value[32]{};
            const auto length = GetPrivateProfileStringW(L"/Script/HubrisVR.HubrisGraphicsSettings",
                                                         L"bDLSSEnabled",
                                                         L"",
                                                         value,
                                                         ARRAYSIZE(value),
                                                         file.c_str());
            return length < ARRAYSIZE(value) - 1 && DlssExplicitlyDisabled({value, length});
        } catch (...) {
            return false;
        }

        TextureRegion Surface(const NgxTextureRegion& region) noexcept {
            const auto desc = region.texture->GetDesc();
            return {Identity(region.texture),
                    DeviceIdentity(region.texture),
                    desc.Width,
                    desc.Height,
                    desc.DepthOrArraySize,
                    desc.MipLevels,
                    desc.SampleDesc.Count,
                    desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                    region.active};
        }

        struct ModelHistory {
            ModelParameters createParameters;
            ModelParameters evaluateParameters;
            decltype(BeforeSrModelApi::release) release{};
            NVSDK_NGX_Handle* handle{};
            Extent extent{};

            ~ModelHistory() {
                if (handle)
                    release(handle);
            }
        };

        struct RecordingOwner {
            std::shared_ptr<ModelHistory> history;
            std::shared_ptr<ColorPipeline> color;
            std::shared_ptr<ColorScratch> scratch;
            ComPtr<ID3D12Resource> depth, motion, output;
            ComPtr<ID3D12PipelineState> hostPipeline;
        };

        struct RecordingIdentity {
            ComPtr<ID3D12GraphicsCommandList> list;
            ComPtr<ID3D12CommandAllocator> allocator;
        };

        struct HistoryBank {
            bool resetPending{true};
            bool regionKnown{};
            Rect lastColor{}, lastOutput{};
            std::shared_ptr<ModelHistory> history;
            std::shared_ptr<ColorScratch> scratch;
            RecordingLedger::Generation previous;
            std::uint64_t lastRecordedEpoch{};
        };

        struct Feature {
            const NVSDK_NGX_Handle* handle{};
            NVSDK_NGX_Parameter* parameters{}; // Borrowed from native Create; only used on a matching live evaluation.
            unsigned id{};
            std::uint64_t generation{};
            NgxSrFeature contract{};
            std::array<HistoryBank, kStereoHistoryBankCount> banks{};

            void resetHistories() noexcept {
                for (auto& bank : banks)
                    bank.resetPending = true;
            }
        };

        bool SameRegion(Rect a, Rect b) noexcept {
            return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        }

        class Controller final : public HookSink {
          public:
            bool start(ID3D12Device* device,
                       ID3D12CommandQueue* queue,
                       ID3D12GraphicsCommandList* list,
                       ID3D12CommandAllocator* allocator) {
                std::unique_lock lock(m_modelMutex);
                if (m_ledger || !device || !queue || !list)
                    return false;
                m_binding = {Identity(device),
                             Identity(queue),
                             DeviceIdentity(queue),
                             queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT};
                if (!m_binding.device || !m_binding.queue || m_binding.queueDevice != m_binding.device ||
                    !m_binding.d3d12Direct)
                    return false;
                m_color = std::make_shared<ColorPipeline>();
                if (!m_color->initialize(device) ||
                    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
                    return false;
                m_device = device;
                m_queue = queue;
                m_ledger = std::make_unique<RecordingLedger>(m_binding.queue);
                if (!InstallHooks(queue, list, allocator, *this, true)) {
                    m_ledger.reset();
                    return false;
                }
                m_active = true;
                return true;
            }

            void stop() noexcept {
                std::unique_lock lock(m_modelMutex);
                m_active = false;
                m_ledger->stop();
                discardCachedScratch();
                m_stereo.complete(false);
                for (auto& feature : m_features)
                    feature.resetHistories();
                // Outstanding application lists may still execute after the session ends.
            }

            bool resume(ID3D12Device* device,
                        ID3D12CommandQueue* queue,
                        ID3D12GraphicsCommandList* list,
                        ID3D12CommandAllocator* allocator) noexcept {
                std::unique_lock lock(m_modelMutex);
                if (m_active || m_terminal || m_modelUnavailable || m_stereo.evaluationInFlight() ||
                    Identity(device) != m_binding.device || Identity(queue) != m_binding.queue ||
                    !InstallHooks(queue, list, allocator, *this, true) || !m_ledger->resume())
                    return false;
                m_enablement.update(false, false);
                m_frameEnabled = false;
                m_active = true;
                return true;
            }

            bool ownsPlacementOnRestart() noexcept {
                std::unique_lock lock(m_modelMutex);
                return m_model.create != nullptr;
            }

            bool permanentlyUnavailable() noexcept {
                std::unique_lock lock(m_modelMutex);
                return (m_modelUnavailable || m_terminal || m_ledger->quarantined()) && !m_model.create;
            }

            void updateAppearance(ModelAppearance appearance, bool enabled, bool validProfile) noexcept {
                std::unique_lock lock(m_modelMutex);
                m_appearance = appearance;
                m_enablement.update(enabled, validProfile);
            }

            void beginFrame() noexcept {
                std::unique_lock lock(m_modelMutex);
                if (!m_active)
                    return;
                const auto enablement = m_enablement.beginFrame();
                if (m_stereo.open() || enablement.resetHistory) {
                    for (auto& feature : m_features)
                        feature.resetHistories();
                }
                m_stereo.begin();
                m_frameAppearance = m_appearance;
                m_frameEnabled = enablement.enabled;
                m_frameChecked = m_frameCanRecord = false;
                m_frameHistory.reset();
            }

            std::optional<ModelAppearance> endFrame(bool stereoProjection) noexcept {
                std::unique_lock lock(m_modelMutex);
                if (!m_active)
                    return {};
                const bool valid = m_stereo.complete(stereoProjection);
                const unsigned recorded = m_stereo.modelsRecorded();
                if (!valid || recorded != 2) {
                    for (auto& feature : m_features)
                        feature.resetHistories();
                }
                TraceLoggingWrite(g_traceProvider,
                                  "BeforeSrFrame",
                                  TLArg(m_stereo.epoch(), "FrameEpoch"),
                                  TLArg(stereoProjection, "StereoProjection"),
                                  TLArg(valid, "CompletePair"),
                                  TLArg(recorded, "ModelsRecorded"));
                if (valid && recorded == 2)
                    return m_frameAppearance;
                return {};
            }

            void featureCreated(ID3D12GraphicsCommandList* list,
                                NVSDK_NGX_Feature id,
                                NVSDK_NGX_Parameter* params,
                                const NVSDK_NGX_Handle* handle) noexcept override try {
                if (id != NVSDK_NGX_Feature_SuperSampling || !params || !handle ||
                    DeviceIdentity(list) != m_binding.device)
                    return;
                const auto contract = ReadNgxSrFeature(*params);
                if (!contract)
                    return;
                const auto handleId = HubrisHandleId(handle);
                std::unique_lock lock(m_modelMutex);
                if (m_terminal || m_nextGeneration == std::numeric_limits<std::uint64_t>::max())
                    return;
                auto* reusable = find(handle, handleId);
                if (!reusable) {
                    const auto free = std::find_if(
                        m_features.begin(), m_features.end(), [](const Feature& feature) { return !feature.handle; });
                    if (free == m_features.end())
                        return;
                    reusable = &*free;
                }
                if (!retireFeature(*reusable))
                    return;
                reusable->handle = handle;
                reusable->parameters = params;
                reusable->id = handleId;
                reusable->generation = ++m_nextGeneration;
                reusable->contract = *contract;
            } catch (...) {
                // Native SR continues on all optional-provider errors.
            }

            void beforeEvaluate(ID3D12GraphicsCommandList* list,
                                const NVSDK_NGX_Handle* handle,
                                const NVSDK_NGX_Parameter* params,
                                EvaluationReceipt& receipt) noexcept override try {
                std::unique_lock lock(m_modelMutex);
                if (!m_active)
                    return;
                const bool evaluationInFlight = m_stereo.evaluationInFlight();
                receipt.epoch = m_stereo.enterEvaluation();
                if (!handle || !params || !list)
                    return;
                const auto id = HubrisHandleId(handle);
                auto* feature = find(handle, id);
                if (!feature)
                    return;
                receipt.handleId = id;
                receipt.view.generation = feature->generation;
                const auto sdkThread = GetCurrentThreadId();
                std::uint64_t completedFence{};
                // Retire on the SDK lane outside native Evaluate, even when the next eye is ineligible.
                if (m_model.create && m_sdkThread == sdkThread && !evaluationInFlight) {
                    completedFence = m_fence->GetCompletedValue();
                    m_ledger->poll(completedFence);
                    m_ledger->drain();
                    for (auto& history : m_retiredHistory)
                        history.reset();
                }
                if (!m_stereo.open() || !m_frameEnabled || m_modelUnavailable)
                    return;
                const auto evaluation = ReadNgxSrEvaluation(*params, feature->contract);
                if (!evaluation) {
                    feature->resetHistories();
                    return;
                }
                if (evaluation->reset)
                    feature->resetHistories();
                const auto& frame = *evaluation;
                const auto listIdentity = Identity(list);
                const auto handleIdentity = reinterpret_cast<std::uintptr_t>(handle);
                const TrackedFeature tracked{handleIdentity,
                                             feature->generation,
                                             m_binding.device,
                                             NVSDK_NGX_Feature_SuperSampling,
                                             feature->contract.createFlags,
                                             feature->contract.maxRender,
                                             feature->contract.output,
                                             feature->contract.outputSubrects};
                const Evaluation call{handleIdentity,
                                      feature->generation,
                                      listIdentity,
                                      DeviceIdentity(list),
                                      m_binding.queue,
                                      list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      Surface(frame.color),
                                      Surface(frame.depth),
                                      Surface(frame.motion),
                                      Surface(frame.output),
                                      frame.motionScaleX,
                                      frame.motionScaleY};
                const auto binding = ObservesListReset(list) ? m_ledger->current(listIdentity) : nullptr;
                const bool allocatorBound = bool(binding);
                const bool layout = allocatorBound && m_device->GetNodeCount() == 1 && !m_ledger->quarantined() &&
                                    CanRunBeforeSr(Placement::BeforeSrOnly, m_binding, tracked, call);
                const auto color = frame.color.texture->GetDesc();
                const auto depth = frame.depth.texture->GetDesc();
                const auto motion = frame.motion.texture->GetDesc();
                const bool formats =
                    PlanColorViews(color, frame.color.active, depth, frame.depth.active) &&
                    std::isnormal(frame.preExposure) &&
                    (motion.Format == DXGI_FORMAT_R16G16_FLOAT || motion.Format == DXGI_FORMAT_R32G32_FLOAT) &&
                    !(motion.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
                const bool nativeRenderMotion = frame.motion.active.width == frame.color.active.width &&
                                                frame.motion.active.height == frame.color.active.height;
                const bool compactMotion = call.motion.allocationWidth == frame.motion.active.width &&
                                           call.motion.allocationHeight == frame.motion.active.height;
                const bool modelMotionExtentEligible =
                    ModelMotionExtentEligible(frame, motion.Format) && (nativeRenderMotion || compactMotion);
                if (!layout || !formats) {
                    m_frameCanRecord = false;
                    feature->resetHistories();
                    return;
                }
                if (!modelMotionExtentEligible) {
                    feature->resetHistories();
                    m_frameChecked = true;
                    m_frameCanRecord = false;
                    return;
                }
                // The qualified caller passes a dedicated, mutable SDK map through the immediate RHI command.
                if (!receipt.qualifiedColorCaller || feature->parameters != params ||
                    std::any_of(
                        m_features.begin(),
                        m_features.end(),
                        [&](const Feature& other) { return &other != feature && other.parameters == params; }) ||
                    !ScopedNgxColor::matches(*params, frame.color)) {
                    feature->resetHistories();
                    m_frameChecked = true;
                    m_frameCanRecord = false;
                    return;
                }

                if (m_sdkThread && m_sdkThread != sdkThread) {
                    feature->resetHistories();
                    m_frameCanRecord = false;
                    return;
                }
                // Calibration must see the same prerequisites that enable recording, including the original PSO.
                auto hostPipeline = CapturePipelineState(list, binding->generation);
                if (!hostPipeline) {
                    TraceLoggingWrite(g_traceProvider,
                                      "BeforeSrMissingHostPso",
                                      TLArg(m_stereo.epoch(), "FrameEpoch"),
                                      TLArg(feature->generation, "HandleGeneration"),
                                      TLArg(binding->generation, "ListGeneration"));
                    m_frameCanRecord = false;
                    return;
                }
                receipt.view.output = frame.output.active;
                receipt.view.sdkThread = sdkThread;
                if (!m_stereo.canApply(feature->generation, frame.output.active, sdkThread))
                    return;
                m_sdkThread = sdkThread;
                if (!m_frameChecked) {
                    m_frameChecked = true;
                    m_frameHistory = selectHistory();
                    m_frameCanRecord = m_frameHistory.has_value();
                    TraceLoggingWrite(
                        g_traceProvider,
                        "BeforeSrPairReady",
                        TLArg(m_stereo.epoch(), "FrameEpoch"),
                        TLArg(completedFence, "CompletedFence"),
                        TLArg(m_frameCanRecord, "Ready"),
                        TLArg(m_frameHistory ? static_cast<int>(m_frameHistory->bank) : -1, "HistoryBank"),
                        TLArg(m_frameHistory && m_frameHistory->reset, "PairHistoryReset"));
                }
                if (!m_frameCanRecord || !m_frameHistory || m_ledger->quarantined())
                    return;
                auto& bank = feature->banks[m_frameHistory->bank];
                if (bank.previous) {
                    const auto outcome = bank.previous->outcome();
                    if (outcome == RecordingOutcome::Abandoned) {
                        bank.history.reset();
                        bank.resetPending = true;
                    }
                }
                if (bank.regionKnown &&
                    (bank.lastColor.x != frame.color.active.x || bank.lastColor.y != frame.color.active.y ||
                     !SameRegion(bank.lastOutput, frame.output.active))) {
                    // A handle reused for another packed eye must not share temporal history.
                    bank.history.reset();
                    bank.resetPending = true;
                }
                if (!m_model.create && !OpenBeforeSrModel(m_device.Get(), m_model)) {
                    // This controller is tied to a single native device and model for its lifetime.
                    m_modelUnavailable = true;
                    TraceLoggingWrite(g_traceProvider, "BeforeSrModelOpenFailed");
                    log::Log(
                        "Before-DLSS NR: model initialization failed. Returning to compositor NR on the next frame.\n");
                    m_frameCanRecord = false;
                    return;
                }
                m_frameCanRecord =
                    record(*feature, bank, list, listIdentity, frame, std::move(hostPipeline), receipt.color);
            } catch (...) {
                // Preserve the entry epoch so native return invalidates this frame and releases the in-flight count.
                receipt.view.output = {};
            }

            void afterEvaluate(const NVSDK_NGX_Handle* handle,
                               const EvaluationReceipt& seen,
                               bool succeeded) noexcept override {
                if (!seen.epoch)
                    return;
                std::unique_lock lock(m_modelMutex);
                const bool failedBinding = seen.color.resolved && (!seen.color.substituted || !seen.color.restored);
                const bool recorded = succeeded && seen.color.substituted && seen.color.restored;
                const bool sameFrame = m_stereo.leaveEvaluation(
                    *seen.epoch, succeeded && !failedBinding ? seen.view : StereoViewKey{}, recorded);
                if (!m_active)
                    return;
                if (!seen.color.restored) {
                    m_active = false;
                    m_terminal = true;
                    m_ledger->stop();
                    discardCachedScratch();
                    log::Log(
                        "Before-DLSS NR: could not verify restoration of the native SR color parameters; stopped.\n");
                    return;
                }
                if (!succeeded || !seen.view.output.width || failedBinding) {
                    if (sameFrame)
                        m_frameCanRecord = false;
                    if (seen.view.generation) {
                        auto* feature = find(handle, seen.handleId);
                        if (feature && feature->generation == seen.view.generation)
                            feature->resetHistories();
                    }
                } else if (sameFrame && recorded && m_frameHistory) {
                    auto* feature = find(handle, seen.handleId);
                    std::uint64_t submittedGeneration{};
                    if (feature && feature->generation == seen.view.generation) {
                        auto& bank = feature->banks[m_frameHistory->bank];
                        bank.lastRecordedEpoch = *seen.epoch;
                        if (bank.previous)
                            submittedGeneration = bank.previous->generation;
                    }
                    TraceLoggingWrite(g_traceProvider,
                                      "BeforeSrRecorded",
                                      TLArg(*seen.epoch, "FrameEpoch"),
                                      TLArg(submittedGeneration, "ListGeneration"),
                                      TLArg(seen.handleId, "Handle"),
                                      TLArg(seen.view.generation, "HandleGeneration"));
                    if (m_recorded.fetch_add(1, std::memory_order_relaxed) < 4)
                        log::Log("Before-DLSS NR: recorded model and native SR for handle %u.\n", seen.handleId);
                }
            }

            void featureReleased(const NVSDK_NGX_Handle* handle, unsigned id) noexcept override {
                std::unique_lock lock(m_modelMutex);
                if (auto* feature = find(handle, id))
                    retireFeature(*feature);
            }

            void beforeNgxShutdown(ID3D12Device* device) noexcept override {
                if (device && Identity(device) != m_binding.device)
                    return;
                std::unique_lock lock(m_modelMutex);
                // Outstanding SDK owners are held for the process lifetime once the game shuts NGX down.
                m_terminal = true;
                m_active = false;
                m_ledger->stop();
                discardCachedScratch();
            }

            void execute(ID3D12CommandQueue* queue,
                         UINT count,
                         ID3D12CommandList* const* lists,
                         ExecuteFn native) noexcept override {
                if (count && !lists) {
                    m_ledger->quarantine();
                    native(queue, count, lists);
                    return;
                }
                const auto queueIdentity = Identity(queue);
                std::unique_lock lock(m_submitMutex, std::defer_lock);
                if (queueIdentity == m_binding.queue)
                    lock.lock();
                auto submitted =
                    m_ledger->beforeExecute(queueIdentity, count, [&](std::size_t i) { return Identity(lists[i]); });
                native(queue, count, lists);
                if (submitted) {
                    const auto value = ++m_lastFence;
                    const bool signaled = value && SUCCEEDED(queue->Signal(m_fence.Get(), value));
                    m_ledger->afterSignal(std::move(submitted), value, signaled);
                    TraceLoggingWrite(g_traceProvider,
                                      "BeforeSrSubmission",
                                      TLArg(value, "FenceValue"),
                                      TLArg(signaled, "SignalSucceeded"));
                }
            }

            HRESULT reset(ID3D12GraphicsCommandList* list,
                          ID3D12CommandAllocator* allocator,
                          ID3D12PipelineState* state,
                          ResetFn native) noexcept override {
                const auto listId = Identity(list);
                const auto generation = m_ledger->current(listId);
                const HRESULT result = native(list, allocator, state);
                m_ledger->afterReset(generation, SUCCEEDED(result));
                if (SUCCEEDED(result) && allocator && m_binding.device == DeviceIdentity(list) &&
                    m_binding.device == DeviceIdentity(allocator) && ObservesAllocatorReset(allocator)) {
                    try {
                        auto identity = std::make_shared<RecordingIdentity>();
                        identity->list = list;
                        identity->allocator = allocator;
                        if (m_ledger->bind(listId, Identity(allocator), identity)) {
                            if (const auto binding = m_ledger->current(listId))
                                TrackPipelineReset(list, binding->generation, state);
                        }
                    } catch (...) {
                        m_ledger->quarantine();
                    }
                }
                return result;
            }

            HRESULT resetAllocator(ID3D12CommandAllocator* allocator, AllocatorResetFn native) noexcept override {
                const auto identity = Identity(allocator);
                const auto epoch = m_ledger->beforeAllocatorReset(identity);
                const HRESULT result = native(allocator);
                m_ledger->afterAllocatorReset(identity, epoch, SUCCEEDED(result));
                return result;
            }

          private:
            void discardCachedScratch() noexcept {
                for (auto& feature : m_features)
                    for (auto& bank : feature.banks)
                        bank.scratch.reset();
            }

            std::optional<StereoHistorySelection> selectHistory() const noexcept {
                const auto* expected = m_stereo.expected();
                if (!expected)
                    return std::nullopt;
                StereoHistoryBanks banks{};
                for (std::size_t eye = 0; eye < expected->size(); ++eye) {
                    const auto feature =
                        std::find_if(m_features.begin(), m_features.end(), [&](const Feature& candidate) {
                            return candidate.generation == (*expected)[eye].generation;
                        });
                    if (feature == m_features.end())
                        return std::nullopt;
                    for (std::size_t i = 0; i < banks.size(); ++i) {
                        const auto& bank = feature->banks[i];
                        const auto outcome = bank.previous ? bank.previous->outcome() : RecordingOutcome::Abandoned;
                        banks[i][eye].ready =
                            outcome == RecordingOutcome::Executed || outcome == RecordingOutcome::Abandoned;
                        if (outcome == RecordingOutcome::Executed && bank.history && !bank.resetPending)
                            banks[i][eye].lastSuccessfulEpoch = bank.lastRecordedEpoch;
                    }
                }
                return SelectStereoHistoryBank(m_stereo.epoch(), banks);
            }

            bool retireFeature(Feature& feature) noexcept {
                const auto histories = std::count_if(feature.banks.begin(),
                                                     feature.banks.end(),
                                                     [](const HistoryBank& bank) { return bool(bank.history); });
                if (histories > std::count(m_retiredHistory.begin(), m_retiredHistory.end(), nullptr)) {
                    m_active = false;
                    m_ledger->quarantine();
                    discardCachedScratch();
                    return false;
                }
                for (auto& bank : feature.banks) {
                    if (bank.history)
                        *std::find(m_retiredHistory.begin(), m_retiredHistory.end(), nullptr) = std::move(bank.history);
                }
                feature = {};
                return true;
            }

            Feature* find(const NVSDK_NGX_Handle* handle, unsigned id) noexcept {
                const auto match = std::find_if(m_features.begin(), m_features.end(), [&](const Feature& feature) {
                    return feature.handle == handle && feature.id == id;
                });
                return match == m_features.end() ? nullptr : &*match;
            }

            bool record(Feature& feature,
                        HistoryBank& bank,
                        ID3D12GraphicsCommandList* list,
                        std::uintptr_t listIdentity,
                        const NgxSrEvaluation& frame,
                        ComPtr<ID3D12PipelineState> hostPipeline,
                        NgxColorSubstitution& color) {
                const Extent extent{frame.color.active.width, frame.color.active.height};
                if (bank.history &&
                    (bank.history->extent.width != extent.width || bank.history->extent.height != extent.height))
                    bank.history.reset();
                auto owner = std::make_shared<RecordingOwner>();
                owner->history = bank.history ? bank.history : std::make_shared<ModelHistory>();
                auto& history = *owner->history;
                owner->color = m_color;
                owner->depth = frame.depth.texture;
                owner->motion = frame.motion.texture;
                owner->output = frame.output.texture;
                owner->hostPipeline = std::move(hostPipeline);
                // A terminal execution and sole ownership mean drain() released the old list's lease.
                // An incomplete or failed eye leaves resetPending set and must use fresh private storage.
                auto scratch = std::move(bank.scratch);
                if (!scratch || !bank.previous || bank.previous->outcome() != RecordingOutcome::Executed ||
                    bank.resetPending || scratch.use_count() != 1 ||
                    !scratch->reuseRetired(m_device.Get(),
                                           frame.color.texture,
                                           frame.color.active,
                                           frame.depth.texture,
                                           frame.depth.active)) {
                    scratch.reset();
                    scratch = std::make_shared<ColorScratch>();
                    if (!scratch->initialize(m_device.Get(),
                                             frame.color.texture,
                                             frame.color.active,
                                             frame.depth.texture,
                                             frame.depth.active))
                        return false;
                }
                owner->scratch = std::move(scratch);
                const bool newHistory = !history.handle;
                // The bank was retired before selection. Its recording owners retain the map
                // through fence completion and allocator reset, including when the history is invalidated.
                if (newHistory &&
                    (!history.createParameters.acquire(m_model) || !history.evaluateParameters.acquire(m_model)))
                    return false;
                ConfigureModelEvaluate(*history.evaluateParameters.get(),
                                       frame,
                                       owner->scratch->modelInput(),
                                       owner->scratch->modelOutput(),
                                       owner->scratch->extractedDepth(),
                                       (feature.contract.createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0,
                                       m_frameHistory->reset || bank.resetPending || newHistory,
                                       m_frameAppearance);
                bank.previous = m_ledger->attach(listIdentity, owner);
                if (!bank.previous)
                    return false;

                // Unreal restores heaps and invalidates its compute root bindings after native SR.
                // Restore the real pre-call PSO now, even if recording exits through a failure.
                auto restoreHostPipeline = RestorePipelineOnExit(list, owner->hostPipeline.Get());
                if (newHistory) {
                    history.release = m_model.release;
                    history.extent = extent;
                    ConfigureModelCreate(*history.createParameters.get(), extent);
                    if (NVSDK_NGX_FAILED(m_model.create(
                            list, NVSDK_NGX_Feature_Reserved18, history.createParameters.get(), &history.handle)) ||
                        !history.handle)
                        return false;
                }
                if (!owner->color->encode(list, *owner->scratch, frame.preExposure))
                    return false;

                if (NVSDK_NGX_FAILED(
                        m_model.evaluate(list, history.handle, history.evaluateParameters.get(), nullptr)) ||
                    !owner->color->resolve(list, *owner->scratch, frame.preExposure)) {
                    owner->color->recover(list, *owner->scratch);
                    bank.history.reset();
                    bank.resetPending = true;
                    return false;
                }

                bank.regionKnown = true;
                bank.lastColor = frame.color.active;
                bank.lastOutput = frame.output.active;
                bank.history = owner->history;
                bank.scratch = owner->scratch;
                // Abandonment re-arms reset before this model history can be reused.
                bank.resetPending = false;
                color = {feature.parameters, frame.color, owner->scratch->resolvedColor()};
                return color.resolved != nullptr;
            }

            DeviceBinding m_binding;
            ComPtr<ID3D12Device> m_device;
            ComPtr<ID3D12CommandQueue> m_queue;
            ComPtr<ID3D12Fence> m_fence;
            std::shared_ptr<ColorPipeline> m_color;
            std::unique_ptr<RecordingLedger> m_ledger;
            std::mutex m_modelMutex, m_submitMutex;
            std::array<Feature, 8> m_features{};
            std::array<std::shared_ptr<ModelHistory>, std::tuple_size_v<decltype(m_features)> * kStereoHistoryBankCount>
                m_retiredHistory{};
            std::uint64_t m_nextGeneration{}, m_lastFence{};
            std::atomic_uint64_t m_recorded{};
            DWORD m_sdkThread{};
            BeforeSrModelApi m_model;
            ModelAppearance m_appearance, m_frameAppearance;
            StereoGate m_stereo;
            std::optional<StereoHistorySelection> m_frameHistory;
            bool m_frameChecked{}, m_frameCanRecord{};
            BeforeSrEnablement m_enablement;
            bool m_frameEnabled{}, m_modelUnavailable{};
            bool m_active{}, m_terminal{};
        };

        std::atomic<Controller*> g_controller{};
        std::mutex g_startMutex;
    } // namespace

    BeforeSrStartup StartBeforeSr(ID3D12Device* device,
                                  ID3D12CommandQueue* queue,
                                  ID3D12GraphicsCommandList* list,
                                  ID3D12CommandAllocator* allocator) noexcept try {
        static const bool qualified = QualifiedLaunch();
        if (!qualified)
            return {};
        std::lock_guard startLock(g_startMutex);
        auto* existing = g_controller.load(std::memory_order_acquire);
        if (HubrisDisabledDlss()) {
            const bool retained = existing && existing->ownsPlacementOnRestart();
            const auto deviceId = Identity(device);
            const bool directQueue = deviceId && queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
                                     DeviceIdentity(queue) == deviceId;
            if (retained)
                log::Log("Before-DLSS NR: restart Hubris to use compositor NR after disabling native DLSS.\n");
            else
                log::Log("Before-DLSS NR: native DLSS is disabled in Hubris. Using compositor NR.\n");
            return {false, retained, directQueue};
        }
        if (existing) {
            if (existing->resume(device, queue, list, allocator))
                return {true, true, true};
            const bool retained = existing->ownsPlacementOnRestart();
            if (retained)
                log::Log(
                    "Before-DLSS NR: application model retained. Restart the process to start another NR session.\n");
            return {false, retained, false};
        }
        auto controller = std::make_unique<Controller>();
        if (!controller->start(device, queue, list, allocator)) {
            log::Log("Before-DLSS NR: could not initialize the Hubris route. Using compositor NR.\n");
            return {};
        }
        // The hook sink, its detours and uncertain/replayable GPU recordings share process lifetime.
        g_controller.store(controller.release(), std::memory_order_release);
        log::Log("Before-DLSS NR: selected before-upscale placement for Hubris.\n");
        return {true, true, true};
    } catch (...) {
        return {};
    }

    void StopBeforeSr() noexcept {
        if (auto* controller = g_controller.load(std::memory_order_acquire))
            controller->stop();
    }

    bool HasBeforeSrModel() noexcept {
        auto* controller = g_controller.load(std::memory_order_acquire);
        return controller && controller->ownsPlacementOnRestart();
    }

    bool BeforeSrModelUnavailable() noexcept {
        auto* controller = g_controller.load(std::memory_order_acquire);
        return controller && controller->permanentlyUnavailable();
    }

    void UpdateBeforeSrAppearance(
        int style, float intensity, float tone, float detail, float skin, bool enabled, bool validProfile) noexcept {
        if (auto* controller = g_controller.load(std::memory_order_acquire))
            controller->updateAppearance({style, intensity, tone, detail, skin}, enabled, validProfile);
    }

    void BeginBeforeSrFrame() noexcept {
        if (auto* controller = g_controller.load(std::memory_order_acquire))
            controller->beginFrame();
    }

    std::optional<ModelAppearance> EndBeforeSrFrame(bool stereoProjection) noexcept {
        if (auto* controller = g_controller.load(std::memory_order_acquire))
            return controller->endFrame(stereoProjection);
        return {};
    }
} // namespace virtualdesktop_openxr::nr_before_sr
