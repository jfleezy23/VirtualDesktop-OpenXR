// Public runtime integration with a private source-built NGX facade. The facade owns
// real feature handles; this observer supplies an identity copy, not vendor inference.
// Breaks caught: ignored processing setting, crop instead of whole-eye reduction,
// wrong-eye dimensions/depth, stale feature capacity after shrink, and scaled output.
#define VDXR_NR_TEST_ENTRYPOINT retainedInputMain
#include "nr_input_toggle_regression.cpp"
#undef VDXR_NR_TEST_ENTRYPOINT
#include <fstream>
#include <set>

namespace virtualdesktop_openxr::log {
    extern std::ofstream logStream;
}

namespace {
#ifdef VDXR_NR_SPATIAL_CANDIDATE
    // Actual queue/consumer failure boundaries. A real queue gate keeps submitted
    // spatial work pending until teardown enters its native completion drain.
    struct SpatialSubmissionFailure {
        using Boundary = virtualdesktop_openxr::RuntimeInputRegression::SpatialSubmissionBoundary;
        using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
        using Signal = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
        using Wait = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
        using Event = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
        struct Retired {
            std::atomic<unsigned> resources{}, lists{};
        };
        struct Lifetime final : IUnknown {
            std::atomic<ULONG> references{1};
            std::shared_ptr<Retired> retired;
            bool list;
            Lifetime(std::shared_ptr<Retired> value, bool isList) : retired(std::move(value)), list(isList) {
            }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
                if (!out)
                    return E_POINTER;
                *out = nullptr;
                if (iid != __uuidof(IUnknown))
                    return E_NOINTERFACE;
                *out = this;
                AddRef();
                return S_OK;
            }
            ULONG STDMETHODCALLTYPE AddRef() override {
                return ++references;
            }
            ULONG STDMETHODCALLTYPE Release() override {
                const auto count = --references;
                if (!count) {
                    if (list)
                        ++retired->lists;
                    else
                        ++retired->resources;
                    delete this;
                }
                return count;
            }
        };
        inline static SpatialSubmissionFailure* active{};
        inline static const GUID lifetimeId{
            0xe771a2c3, 0x6641, 0x4cea, {0x83, 0x45, 0xb9, 0x62, 0x71, 0x08, 0x58, 0x24}};
        Boundary target{};
        Execute originalExecute{};
        Signal originalSignal{};
        Wait originalWait{};
        Event originalEvent{};
        ComPtr<ID3D12Fence> gate;
        std::shared_ptr<Retired> retired = std::make_shared<Retired>();
        std::set<ID3D12Object*> watchedResources, watchedLists;
        std::thread releaseThread;
        std::atomic<bool> injected{}, enteredDrain{}, cleanupStarted{}, gateReleased{}, timedOut{}, retainedAtDrain{};
        bool signalFailure{}, native{}, installed{}, retainedAtFailure{}, cleanupSucceeded{};
        unsigned executions{}, acceptedSignals{}, consumerWaits{}, failures{};
        UINT64 lastAcceptedSignal{}, rejectedSignal{};
        explicit SpatialSubmissionFailure(bool failSignal, bool nativeRoute = false)
            : signalFailure(failSignal), native(nativeRoute) {
        }
        void check(bool condition, const char* message) {
            if (!condition) {
                ++failures;
                std::cerr << "ASSERTION FAILED: native submission " << message << '\n';
            }
        }
        static void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
            auto& self = *active;
            if (queue == self.target.queue)
                ++self.executions;
            self.originalExecute(queue, count, lists);
        }
        static HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
            auto& self = *active;
            if (queue == self.target.queue && fence == self.target.completion) {
                if (self.signalFailure && !self.injected.exchange(true)) {
                    self.check(self.executions == 1 && self.consumerWaits == 0,
                               "Signal failure must follow actual Execute without publishing a consumer wait");
                    self.rejectedSignal = value;
                    return E_FAIL;
                }
                const auto result = self.originalSignal(queue, fence, value);
                if (SUCCEEDED(result)) {
                    ++self.acceptedSignals;
                    self.lastAcceptedSignal = value;
                }
                return result;
            }
            return self.originalSignal(queue, fence, value);
        }
        static HRESULT STDMETHODCALLTYPE wait(ID3D11DeviceContext4* context, ID3D11Fence* fence, UINT64 value) {
            auto& self = *active;
            if (context == self.target.consumer && fence == self.target.consumerFence) {
                ++self.consumerWaits;
                if (!self.signalFailure && !self.injected.exchange(true)) {
                    self.check(self.executions == 1 && self.acceptedSignals == 1 && value == self.lastAcceptedSignal,
                               "consumer Wait failure must follow positive native submission registration");
                    return E_FAIL;
                }
            }
            return self.originalWait(context, fence, value);
        }
        static HRESULT STDMETHODCALLTYPE event(ID3D12Fence* fence, UINT64 value, HANDLE handle) {
            auto& self = *active;
            if (fence == self.target.completion && self.injected && self.cleanupStarted)
                self.enteredDrain = true;
            return self.originalEvent(fence, value, handle);
        }
        void observe(ID3D12Object* object, bool list) {
            auto& set = list ? watchedLists : watchedResources;
            if (!set.insert(object).second)
                return;
            auto* owner = new Lifetime(retired, list);
            const auto result = object->SetPrivateDataInterface(lifetimeId, owner);
            owner->Release();
            checkHr(result);
        }
        void arm() {
            target = virtualdesktop_openxr::RuntimeInputRegression::spatialSubmissionBoundary(native);
            originalExecute = reinterpret_cast<Execute>((*reinterpret_cast<void***>(target.queue))[10]);
            originalSignal = reinterpret_cast<Signal>((*reinterpret_cast<void***>(target.queue))[14]);
            // ID3D11DeviceContext4 vtable slots147/148 are Signal/Wait in the retained Windows SDK.
            originalWait = reinterpret_cast<Wait>((*reinterpret_cast<void***>(target.consumer))[148]);
            originalEvent = reinterpret_cast<Event>((*reinterpret_cast<void***>(target.completion))[9]);
            active = this;
            require(DetourTransactionBegin() == NO_ERROR, "native boundary hook transaction begin failed");
            LONG status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalExecute), execute);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalSignal), signal);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (status == NO_ERROR)
                status = DetourAttach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            require(status == NO_ERROR, "pointer-filtered native boundary hook installation failed");
            installed = true;
            ComPtr<ID3D12Device> device;
            checkHr(target.completion->GetDevice(IID_PPV_ARGS(&device)));
            checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
            checkHr(target.queue->Wait(gate.Get(), 1));
            releaseThread = std::thread([this] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!enteredDrain && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                timedOut = !enteredDrain;
                retainedAtDrain = !retired->resources && !retired->lists;
                // Always unblock GPU work, including a failing baseline control.
                gateReleased = SUCCEEDED(gate->Signal(1));
            });
        }
        void beforeCleanup() {
            retainedAtFailure =
                virtualdesktop_openxr::RuntimeInputRegression::spatialFailureOwnsResources(signalFailure, native) &&
                !retired->resources && !retired->lists;
            cleanupStarted = true;
        }
        void finish() {
            if (releaseThread.joinable())
                releaseThread.join();
            check(injected && executions == 1, "fault must be injected into exactly one actual NR submission");
            check(cleanupSucceeded && retainedAtFailure,
                  "failed frame must retain resources and correct recovery flags");
            check(enteredDrain && retainedAtDrain && gateReleased && !timedOut,
                  "teardown must enter the real blocked completion drain before resource retirement");
            const unsigned expectedResources = native ? 5 : 6;
            check(watchedResources.size() == expectedResources && retired->resources == expectedResources &&
                      watchedLists.size() == 1 && retired->lists == 1,
                  "all exact route-specific resources and submitted list must retire after proven completion");
            check(signalFailure ? consumerWaits == 0 && acceptedSignals >= 1 && lastAcceptedSignal > rejectedSignal
                                : consumerWaits == 1 && acceptedSignals == 1,
                  "submission recovery must use the correct accepted fence contract");
            std::cout << "native submission failure route=" << (native ? "native100" : "spatial")
                      << " signal=" << signalFailure << " executes=" << executions
                      << " acceptedSignals=" << acceptedSignals << " consumerWaits=" << consumerWaits
                      << " resourcesRetired=" << retired->resources << " listsRetired=" << retired->lists
                      << " retainedAtFailure=" << retainedAtFailure << " retainedAtDrain=" << retainedAtDrain << '\n';
        }
        ~SpatialSubmissionFailure() {
            if (releaseThread.joinable()) {
                if (gate)
                    gate->Signal(1);
                releaseThread.join();
            }
            if (!installed)
                return;
            LONG status = DetourTransactionBegin();
            if (status == NO_ERROR)
                status = DetourUpdateThread(GetCurrentThread());
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalExecute), execute);
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalSignal), signal);
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalWait), wait);
            if (status == NO_ERROR)
                status = DetourDetach(reinterpret_cast<PVOID*>(&originalEvent), event);
            if (status == NO_ERROR)
                status = DetourTransactionCommit();
            else
                DetourTransactionAbort();
            if (status != NO_ERROR)
                std::_Exit(2);
            active = nullptr;
        }
    };

    // Create/evaluate failures occur before Execute, so their unsubmitted list
    // may retire immediately. The prepared scratch must survive until teardown.
    struct NativePreparationFailure {
        inline static NativePreparationFailure* active{};
        std::shared_ptr<SpatialSubmissionFailure::Retired> retired =
            std::make_shared<SpatialSubmissionFailure::Retired>();
        bool observed{}, retainedAtFailure{}, cleanupSucceeded{};
        NativePreparationFailure() {
            active = this;
        }
        ~NativePreparationFailure() {
            active = nullptr;
        }
        void observe(ID3D12GraphicsCommandList* commands) {
            require(commands && commands->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT,
                    "native preparation fault must cross an actual DIRECT vendor call");
            if (observed)
                return;
            auto* scratch = virtualdesktop_openxr::RuntimeInputRegression::nativeAnswerResource();
            auto* owner = new SpatialSubmissionFailure::Lifetime(retired, false);
            const auto result = scratch->SetPrivateDataInterface(SpatialSubmissionFailure::lifetimeId, owner);
            owner->Release();
            checkHr(result);
            observed = true;
        }
        void beforeCleanup() {
            retainedAtFailure = observed && !retired->resources &&
                                virtualdesktop_openxr::RuntimeInputRegression::spatialFailureOwnsResources(false, true);
        }
        void finish() {
            require(observed && retainedAtFailure && cleanupSucceeded && retired->resources == 1,
                    "native preparation failure must retain scratch/flags until successful cleanup");
            std::cout << "native preparation failure retainedAtFailure=" << retainedAtFailure
                      << " resourcesRetired=" << retired->resources << " cleanupSucceeded=" << cleanupSucceeded << '\n';
        }
    };
#endif
    struct SpatialRuntimeProbe {
        struct Feature {
            int width{}, height{};
            unsigned generation{};
        };
        HMODULE module{};
        decltype(&NVSDK_NGX_D3D12_CreateFeature) create{};
        decltype(&NVSDK_NGX_D3D12_EvaluateFeature) evaluate{};
        decltype(&NVSDK_NGX_D3D12_ReleaseFeature) release{};
        std::map<const NVSDK_NGX_Handle*, Feature> features;
        std::array<unsigned, 2> previousGeneration{};
        std::set<ID3D12Resource*> nativeBanks;
        std::set<ID3D12Resource*> publishedBanks;
        unsigned frame{}, evaluations{}, creations{}, releases{}, failures{}, beforeEvaluations{};
        unsigned previousPercent{100};
        bool nativeOnly{};
        bool failEvaluation{}, failCreation{}, injectedFailure{};
        inline static SpatialRuntimeProbe* active{};

        void check(bool condition, const char* message) {
            if (!condition) {
                ++failures;
                std::cerr << "ASSERTION FAILED: frame=" << frame << " processing=" << nrProcessingResolution << ' '
                          << message << '\n';
            }
        }
        int integer(const NVSDK_NGX_Parameter* parameters, const char* name) {
            int result = -999;
            check(NVSDK_NGX_SUCCEED(parameters->Get(name, &result)), name);
            return result;
        }
        ID3D12Resource* resource(const NVSDK_NGX_Parameter* parameters, const char* name) {
            ID3D12Resource* result{};
            check(NVSDK_NGX_SUCCEED(parameters->Get(name, &result)) && result, name);
            return result;
        }
        std::array<int, 4> rect(const NVSDK_NGX_Parameter* parameters, const char* prefix) {
            std::array<int, 4> result{};
            const char* fields[]{"SubrectBaseX", "SubrectBaseY", "SubrectWidth", "SubrectHeight"};
            for (unsigned i = 0; i < result.size(); ++i)
                result[i] = integer(parameters, (std::string(prefix) + fields[i]).c_str());
            return result;
        }
        static NVSDK_NGX_Result NVSDK_CONV onCreate(ID3D12GraphicsCommandList* commands,
                                                    NVSDK_NGX_Feature kind,
                                                    NVSDK_NGX_Parameter* parameters,
                                                    NVSDK_NGX_Handle** handle) {
            auto& self = *active;
            if (self.failCreation && self.frame == (self.nativeOnly ? 0u : 5u)) {
#ifdef VDXR_NR_SPATIAL_CANDIDATE
                if (NativePreparationFailure::active)
                    NativePreparationFailure::active->observe(commands);
#endif
                self.injectedFailure = true;
                *handle = nullptr;
                return NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
            }
            // Capture creation arguments before the other eye overwrites the shared parameter map.
            Feature value{self.integer(parameters, "DLSSNR.Width"),
                          self.integer(parameters, "DLSSNR.Height"),
                          self.creations + 1};
            const auto result = self.create(commands, kind, parameters, handle);
            if (NVSDK_NGX_SUCCEED(result) && handle && *handle) {
                self.check(!self.features.count(*handle), "create reused a live feature handle");
                self.features[*handle] = value;
                ++self.creations;
                std::cout << "create generation=" << value.generation << " size=" << value.width << 'x' << value.height
                          << '\n';
            }
            return result;
        }
        static NVSDK_NGX_Result NVSDK_CONV onRelease(NVSDK_NGX_Handle* handle) {
            auto& self = *active;
            self.check(self.features.count(handle) == 1, "release received unknown/stale feature");
            const auto result = self.release(handle);
            if (NVSDK_NGX_SUCCEED(result)) {
                self.features.erase(handle);
                ++self.releases;
            }
            return result;
        }
        static NVSDK_NGX_Result NVSDK_CONV onEvaluate(ID3D12GraphicsCommandList* commands,
                                                      const NVSDK_NGX_Handle* handle,
                                                      const NVSDK_NGX_Parameter* parameters,
                                                      PFN_NVSDK_NGX_ProgressCallback callback) {
            auto& self = *active;
            const auto eye = (self.evaluations - self.beforeEvaluations) % 2;
            ++self.evaluations;
            // Preserve the source facade's live-feature/error checks before recording pixels.
            const auto result = self.evaluate(commands, handle, parameters, callback);
            if (NVSDK_NGX_FAILED(result))
                return result;
            if (self.failEvaluation && self.frame == 7 && eye == 1) {
#ifdef VDXR_NR_SPATIAL_CANDIDATE
                if (NativePreparationFailure::active)
                    NativePreparationFailure::active->observe(commands);
#endif
                self.injectedFailure = true;
                return NVSDK_NGX_Result_FAIL_PlatformError;
            }
            const auto color = self.resource(parameters, "DLSSNR.Color");
            const auto answer = self.resource(parameters, "DLSSNR.Output");
            const auto depth = self.resource(parameters, "DLSSNR.Depth");
#ifdef VDXR_NR_SPATIAL_CANDIDATE
            if (self.frame == 8 && SpatialSubmissionFailure::active && SpatialSubmissionFailure::active->installed) {
                auto& submission = *SpatialSubmissionFailure::active;
                submission.observe(commands, true);
                for (auto* resource : {color, answer, depth})
                    if (resource)
                        submission.observe(resource, false);
            }
#endif
            const auto cr = self.rect(parameters, "DLSSNR.Color");
            const auto ar = self.rect(parameters, "DLSSNR.Output");
            const auto dr = self.rect(parameters, "DLSSNR.Depth");
            const bool reduced = nrProcessingResolution != 100;
            if (reduced && self.frame == 3) {
                self.check(self.integer(parameters, "DLSSNR.Reset") != 0,
                           "same-size source replacement must reset NR history without resetting rotating banks");
            }
            const int width = !reduced                       ? (eye ? 480 : 512)
                              : nrProcessingResolution == 90 ? (eye ? 432 : 464)
                              : nrProcessingResolution == 80 ? (eye ? 384 : 412)
                                                             : (eye ? 360 : 384);
            const int height = !reduced                       ? (eye ? 496 : 512)
                               : nrProcessingResolution == 90 ? (eye ? 448 : 464)
                               : nrProcessingResolution == 80 ? (eye ? 400 : 412)
                                                              : (eye ? 372 : 384);
            self.check(cr == std::array<int, 4>{reduced || !eye ? 0 : 16, reduced || !eye ? 0 : 8, width, height},
                       "NGX color rectangle must be whole eye at expected independent working grid");
            self.check(ar == std::array<int, 4>{reduced ? 0 : int(eye * 512), 0, width, height},
                       "NGX answer rectangle must match active eye working grid");
            self.check(dr == std::array<int, 4>{reduced || !eye ? 0 : 8,
                                                reduced || !eye ? 0 : 12,
                                                reduced ? width : (eye ? 496 : 512),
                                                reduced ? height : (eye ? 480 : 512)},
                       "paired depth rectangle must match working grid using its own valid rectangle");
            self.check(self.integer(parameters, "DLSSNR.DepthInverted") == int(eye),
                       "paired depth inversion must remain per eye");
            self.check(color != answer, "proxy and answer must be separate resources");
            const auto found = self.features.find(handle);
            self.check(found != self.features.end(), "evaluate received released/unobserved feature");
            if (found != self.features.end()) {
                const auto& feature = found->second;
                self.check(reduced ? feature.width == width && feature.height == height
                                   : feature.width >= width && feature.height >= height,
                           "actual feature creation dimensions do not match active geometry");
                if (frameChanged(self) && self.previousGeneration[eye])
                    self.check(feature.generation != self.previousGeneration[eye],
                               "processing change reused stale feature instead of recreation");
                self.previousGeneration[eye] = feature.generation;
            }
            if (color && answer && depth) {
                const auto cd = color->GetDesc(), ad = answer->GetDesc(), dd = depth->GetDesc();
                if (self.frame == 2) {
                    const auto describe = [&](const char* role, const D3D12_RESOURCE_DESC& desc) {
                        std::cout << "spatial resource eye=" << eye << " role=" << role << " format=" << desc.Format
                                  << " flags=" << desc.Flags << " extent=" << desc.Width << 'x' << desc.Height
                                  << " slices=" << desc.DepthOrArraySize << " mips=" << desc.MipLevels
                                  << " samples=" << desc.SampleDesc.Count << '\n';
                    };
                    describe("color", cd);
                    describe("answer", ad);
                    describe("depth", dd);
                }
                if (reduced) {
                    self.check(cd.Width == width && cd.Height == height && ad.Width == width && ad.Height == height &&
                                   dd.Width == width && dd.Height == height,
                               "proxy, answer and paired depth allocations must use active per-eye extent");
                    self.check(cd.Format == DXGI_FORMAT_R8G8B8A8_UNORM && ad.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                                   dd.Format == DXGI_FORMAT_R32_FLOAT,
                               "spatial backend must receive canonical color and typed R32F depth");
                } else {
                    self.check(cd.Width == 512 && cd.Height == 512 && ad.Width == 1024 && ad.Height == 512,
                               "native100 must keep existing imported input and double-wide output contract");
                    self.nativeBanks.insert(answer);
                }
                // Test-only identity NR: exact stored proxy -> answer, preserving COMMON interop.
                D3D12_RESOURCE_BARRIER barriers[]{
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        answer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
                commands->ResourceBarrier(2, barriers);
                D3D12_TEXTURE_COPY_LOCATION src{color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                    dst{answer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                const D3D12_BOX box{UINT(cr[0]), UINT(cr[1]), 0, UINT(cr[0] + cr[2]), UINT(cr[1] + cr[3]), 1};
                commands->CopyTextureRegion(&dst, ar[0], ar[1], 0, &src, &box);
                for (auto& barrier : barriers)
                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                commands->ResourceBarrier(2, barriers);
            }
            std::cout << "evaluate frame=" << self.frame << " eye=" << eye << " color=" << cr[0] << ',' << cr[1] << ','
                      << cr[2] << ',' << cr[3] << " depth=" << dr[0] << ',' << dr[1] << ',' << dr[2] << ',' << dr[3]
                      << '\n';
            return result;
        }
        static bool frameChanged(const SpatialRuntimeProbe& self) {
            return self.previousPercent != nrProcessingResolution;
        }
        explicit SpatialRuntimeProbe(const char* backend, const std::string& mode)
            : nativeOnly(mode == "native100" || mode.rfind("native-", 0) == 0),
              failEvaluation(mode == "evaluate-failure" || mode == "native-evaluate-failure"),
              failCreation(mode == "create-failure" || mode == "native-create-failure") {
            const auto path = std::filesystem::absolute(backend) / L"nvngx_dlssnr.dll";
            module = LoadLibraryW(path.c_str());
            require(module && GetProcAddress(module, "NR_Test_GetVariant"),
                    "Use a private source-built NR_Test facade, never an installed/vendor module");
            create = reinterpret_cast<decltype(create)>(GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
            evaluate = reinterpret_cast<decltype(evaluate)>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
            release = reinterpret_cast<decltype(release)>(GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
            require(create && evaluate && release, "Private module observation exports missing");
            active = this;
            checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
            checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&create), onCreate)));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&evaluate), onEvaluate)));
            checkHr(HRESULT_FROM_WIN32(DetourAttach(reinterpret_cast<PVOID*>(&release), onRelease)));
            checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        }
        ~SpatialRuntimeProbe() {
            if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&create), onCreate) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&evaluate), onEvaluate) != NO_ERROR ||
                DetourDetach(reinterpret_cast<PVOID*>(&release), onRelease) != NO_ERROR ||
                DetourTransactionCommit() != NO_ERROR)
                std::_Exit(2);
            active = nullptr;
            FreeLibrary(module);
        }
        void before(int index) {
            frame = index;
            beforeEvaluations = evaluations;
            constexpr DWORD percent[]{100, 100, 80, 80, 100, 75, 90, 80};
            nrProcessingResolution = nativeOnly ? 100 : percent[std::min<unsigned>(index, 7)];
            // Alternating imports/banks must use this frame, not a cached earlier color.
            expectedOffColour = {{{29u + unsigned(index), 61, 101, 255}, {173, 83u + unsigned(index), 37, 255}}};
        }
        void after(int) {
            check(evaluations == beforeEvaluations + 2, "each frame must run both NGX eyes exactly once");
            check(features.size() == 2, "retain only one current feature per eye after a completed frame");
#ifdef VDXR_NR_SPATIAL_CANDIDATE
            virtualdesktop_openxr::RuntimeInputRegression::checkSpatialPipeline(nrProcessingResolution != 100);
            auto* output = virtualdesktop_openxr::RuntimeInputRegression::publishedNrOutputResource();
            publishedBanks.insert(output);
            if (nativeOnly)
                check(output != virtualdesktop_openxr::RuntimeInputRegression::nativeAnswerResource(),
                      "native answer scratch must remain separate from published output bank");
#endif
            previousPercent = nrProcessingResolution;
        }
    };
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4,
                "Usage: nr_spatial_runtime_regression <private-backend-folder> <state> "
                "<native100|sequence|evaluate-failure|create-failure|signal-failure|wait-failure|"
                "native-create-failure|native-evaluate-failure|native-signal-failure|native-wait-failure>");
        const std::string mode = argv[3];
        const bool nativeFault = mode.rfind("native-", 0) == 0;
        const std::string fault = nativeFault ? mode.substr(7) : mode;
        const bool failSubmission = fault == "signal-failure" || fault == "wait-failure";
        require(mode == "native100" || mode == "sequence" || mode == "evaluate-failure" || mode == "create-failure" ||
                    failSubmission || mode == "native-create-failure" || mode == "native-evaluate-failure",
                "Unknown spatial runtime mode");
        std::filesystem::create_directories(std::filesystem::absolute(argv[2]));
        virtualdesktop_openxr::log::logStream.open(std::filesystem::absolute(argv[2]) / "runtime-test.log");
        require(virtualdesktop_openxr::log::logStream.is_open(), "Private runtime diagnostic log could not be opened");
        SpatialRuntimeProbe probe(argv[1], mode);
#ifdef VDXR_NR_SPATIAL_CANDIDATE
        std::unique_ptr<SpatialSubmissionFailure> submission;
        std::unique_ptr<NativePreparationFailure> preparation;
        if (failSubmission) {
            submission = std::make_unique<SpatialSubmissionFailure>(fault == "signal-failure", nativeFault);
            beforeFailureCleanup = [&] { submission->beforeCleanup(); };
            afterFailureCleanup = [&](XrResult result) { submission->cleanupSucceeded = result == XR_SUCCESS; };
        } else if (nativeFault) {
            preparation = std::make_unique<NativePreparationFailure>();
            beforeFailureCleanup = [&] { preparation->beforeCleanup(); };
            afterFailureCleanup = [&](XrResult result) { preparation->cleanupSucceeded = result == XR_SUCCESS; };
        }
#else
        require(!failSubmission && !nativeFault, "NR fault modes require a matching candidate build");
#endif
        additionalSteadyFrames = 12;
        checkOffColourEdges = checkEnabledColourIdentity = true;
        refreshColourEachFrame = true;
        expectedOffColour = {{{29, 61, 101, 255}, {173, 83, 37, 255}}};
        inputFrameNrEnabled = [](int) { return true; };
        beforeInputFrame = [&](int frame) { probe.before(frame); };
        afterInputFrame = [&](int frame) {
            probe.after(frame);
#ifdef VDXR_NR_SPATIAL_CANDIDATE
            if (submission && frame == 7)
                submission->arm();
#endif
        };
        std::array<XrCompositionLayerProjectionView, 2> views;
        std::array<XrCompositionLayerDepthInfoKHR, 2> depths;
        XrCompositionLayerProjection adjusted{};
        const XrCompositionLayerBaseHeader* header{};
        adjustFrameLayers = [&](auto& end, const auto& layer) {
            adjusted = layer;
            for (unsigned eye = 0; eye < 2; ++eye) {
                views[eye] = layer.views[eye];
                depths[eye] = *static_cast<const XrCompositionLayerDepthInfoKHR*>(views[eye].next);
                views[eye].next = &depths[eye];
                if (eye) {
                    views[eye].subImage.imageRect = {{16, 8}, {480, 496}};
                    depths[eye].subImage.imageRect = {{8, 12}, {496, 480}};
                    depths[eye].nearZ = 100;
                    depths[eye].farZ = .1f;
                }
            }
            adjusted.views = views.data();
            header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&adjusted);
            end.layers = &header;
        };
        observeProjection = [&](const auto& layer) {
            for (unsigned eye = 0; eye < 2; ++eye) {
                require(layer.Viewport[eye].Size.w == (eye ? 480 : 512) &&
                            layer.Viewport[eye].Size.h == (eye ? 496 : 512),
                        "spatial processing changed native final viewport dimensions");
                require(layer.Viewport[eye].Pos.x == int(eye * 512) && layer.Viewport[eye].Pos.y == 0,
                        "spatial processing changed native final eye packing");
            }
        };
        observeSubmittedColour = [](const auto& desc, unsigned) {
            require(desc.Width == 1024 && desc.Height == 512,
                    "final compositor resource must remain the native double-wide bank");
        };
        char toggle[] = "toggle", full[] = "100";
        char* arguments[]{argv[0], argv[1], argv[2], toggle, full};
        const auto result = retainedInputMain(5, arguments);
        beforeInputFrame = afterInputFrame = {};
        inputFrameNrEnabled = {};
        observeProjection = {};
        observeSubmittedColour = {};
        adjustFrameLayers = {};
        beforeFailureCleanup = {};
        afterFailureCleanup = {};
#ifdef VDXR_NR_SPATIAL_CANDIDATE
        if (submission) {
            submission->finish();
            probe.failures += submission->failures;
        }
        if (preparation)
            preparation->finish();
#endif
        require(result == (probe.failEvaluation || probe.failCreation || failSubmission ? 1 : 0),
                "public spatial runtime fixture returned unexpected status");
        require(probe.features.empty() && probe.creations == probe.releases,
                "normal teardown must release every created feature exactly once");
        require(probe.evaluations == (probe.failEvaluation ? 16
                                      : probe.failCreation ? (nativeFault ? 0 : 10)
                                      : failSubmission     ? 18
                                                           : 38),
                "bounded run must observe the exact expected stereo evaluation count");
        if (probe.failEvaluation) {
            require(probe.injectedFailure && captured == 7,
                    "failed second-eye evaluation must propagate without publishing partial native output");
        }
        if (probe.failCreation) {
            require(probe.injectedFailure && captured == (nativeFault ? 0 : 5),
                    "failed feature creation must propagate without publishing a stale native output");
        }
        if (failSubmission) {
            require(captured == 8, "failed native submission must not publish partial or stale final output");
        }
        if (!(nativeFault && probe.failCreation)) {
#ifdef VDXR_NR_SPATIAL_CANDIDATE
            if (probe.nativeOnly) {
                require(probe.nativeBanks.size() == 1, "native100 must reuse one cached packed answer scratch");
                require(probe.publishedBanks.size() > 1, "native100 must still rotate final published output banks");
            } else {
                require(probe.nativeBanks.size() > 1, "test must exercise rotating native output banks");
            }
#else
            require(probe.nativeBanks.size() > 1, "test must exercise rotating native output banks");
#endif
        }
        require(!probe.failures, "spatial backend geometry/lifecycle assertions failed");
        std::cout << "PASS: public runtime " << mode
                  << "; native output/identity pixels/depth intact; creates=" << probe.creations
                  << " releases=" << probe.releases << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
