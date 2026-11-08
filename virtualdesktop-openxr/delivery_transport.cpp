#include "pch.h"
#include "delivery_transport.h"
#include "utils.h"
#include "log.h"

#include <map>
#include <set>
#include <atomic>

namespace virtualdesktop_openxr {
    using Microsoft::WRL::ComPtr;

    bool DeliverySdkTraceObserver::enabled() noexcept {
        return TraceLoggingProviderEnabled(log::g_traceProvider, 0, 0);
    }
    int64_t DeliverySdkTraceObserver::now() noexcept {
        LARGE_INTEGER ticks{};
        QueryPerformanceCounter(&ticks);
        return ticks.QuadPart;
    }
    void DeliverySdkTraceObserver::emit(const detail::SdkGateTiming& timing) noexcept {
        static const int64_t frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        TraceLoggingWrite(log::g_traceProvider,
                          "Delivery_SdkGate",
                          TLArg(timing.staticOp, "Operation"),
                          TLArg(timing.nativeId, "NativeId"),
                          TLArg(static_cast<uint64_t>(timing.gate), "Gate"),
                          TLArg(frequency, "QpcFrequency"),
                          TLArg(timing.gateEnter, "GateEnter"),
                          TLArg(timing.gateAcquired, "GateAcquired"),
                          TLArg(timing.callEnter, "CallEnter"),
                          TLArg(timing.callReturn, "CallReturn"),
                          TLArg(timing.releaseEnter, "ReleaseEnter"),
                          TLArg(timing.released, "Released"),
                          TLArg(timing.callEntered, "CallEntered"),
                          TLArg(timing.callCompleted, "CallCompleted"));
    }

    namespace {
        ComPtr<ID3D11Fence> openFence(ID3D11Device5* device, ID3D11Fence* fence) {
            wil::unique_handle handle;
            CHECK_HRCMD(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, handle.put()));
            ComPtr<ID3D11Fence> result;
            CHECK_HRCMD(device->OpenSharedFence(handle.get(), IID_PPV_ARGS(&result)));
            return result;
        }
        void awaitFence(ID3D11Fence* fence, uint64_t value) {
            const auto completed = fence->GetCompletedValue();
            CHECK_MSG(completed != UINT64_MAX, "Delivery fence device removed");
            if (completed >= value)
                return;
            wil::unique_handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            CHECK_MSG(event.get(), "Delivery completion event creation failed");
            CHECK_HRCMD(fence->SetEventOnCompletion(value, event.get()));
            CHECK_MSG(WaitForSingleObject(event.get(), 5000) == WAIT_OBJECT_0,
                      "Delivery completion did not settle within shutdown bound");
            const auto settled = fence->GetCompletedValue();
            CHECK_MSG(settled != UINT64_MAX && settled >= value,
                      "Delivery completion event did not certify its fence value");
        }
    } // namespace

    struct DeliveryTransport::Chain {
        ovrTextureSwapChain native{};
        ovrTextureSwapChainDesc desc{};
        bool nativeOnly{}, pending{};
        int producerIndex{}, producedIndex{-1}, nativeTarget{-1};
        uint64_t reservedReady{};
        bool copied{};
        std::vector<ComPtr<ID3D11Texture2D>> nativeImages, producerImages, presentationAliases;
        std::vector<wil::unique_handle> handles;
    };

    struct DeliveryTransport::State {
        ovrSession session{};
        ComPtr<ID3D11Device5> producer, presentation;
        ComPtr<ID3D11DeviceContext4> sourceContext, targetContext;
        ComPtr<ID3D11Fence> sourceReady, sourceOnTarget, readDone, readOnSource;
        uint64_t sourceValue{}, readValue{};
        std::atomic<bool> unknown{};
        bool closed{};
        mutable std::recursive_mutex sdkMutex, metadata;
        std::map<ovrTextureSwapChain, std::shared_ptr<Chain>> chains;
        std::vector<std::shared_ptr<Chain>> retired;
        Statistics statistics;
    };

    DeliveryTransport::DeliveryTransport(ovrSession session,
                                         ID3D11Device5* producer,
                                         ID3D11DeviceContext4* producerContext,
                                         ID3D11Device5* presentation,
                                         ID3D11DeviceContext4* presentationContext)
        : m_state(std::make_unique<State>()) {
        auto& s = *m_state;
        CHECK_MSG(session && producer && producerContext && presentation && presentationContext &&
                      producer != presentation,
                  "Independent delivery domains required");
        s.session = session;
        s.producer = producer;
        s.presentation = presentation;
        s.sourceContext = producerContext;
        s.targetContext = presentationContext;
        CHECK_HRCMD(producer->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.sourceReady)));
        s.sourceOnTarget = openFence(presentation, s.sourceReady.Get());
        CHECK_HRCMD(presentation->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.readDone)));
        s.readOnSource = openFence(producer, s.readDone.Get());
    }

    DeliveryTransport::~DeliveryTransport() {
        try {
            shutdown();
        } catch (...) {
            // The runtime must also retain its SDK session on unknown work.
            (void)m_state.release();
        }
    }

    DeliveryTransport::SdkLease DeliveryTransport::sdkLease(const char* operation, long long nativeId) {
        return SdkLease(m_state->sdkMutex, operation, nativeId);
    }

    ovrResult DeliveryTransport::create(const ovrTextureSwapChainDesc& desc,
                                        ovrTextureSwapChain* output,
                                        bool nativeOnly) {
        auto sdk = sdkLease("Create");
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        CHECK_MSG(!s.unknown && !s.closed && output, "Cannot allocate on an uncertain delivery graph");
        auto chain = std::make_shared<Chain>();
        chain->desc = desc;
        chain->nativeOnly = nativeOnly;
        const auto result = ovr_CreateTextureSwapChainDX(s.session, s.presentation.Get(), &desc, &chain->native);
        if (OVR_FAILURE(result))
            return result;
        auto rollback = MakeScopeGuard([&] { ovr_DestroyTextureSwapChain(s.session, chain->native); });
        CHECK_OVRCMD(ovr_GetTextureSwapChainDesc(s.session, chain->native, &chain->desc));
        int count{};
        CHECK_OVRCMD(ovr_GetTextureSwapChainLength(s.session, chain->native, &count));
        CHECK_MSG(count > 0 && count < 256, "Invalid native delivery image count");
        CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(s.session, chain->native, &chain->producerIndex));
        CHECK_MSG(chain->producerIndex >= 0 && chain->producerIndex < count, "Invalid native delivery index");
        for (int index = 0; index < count; ++index) {
            ComPtr<ID3D11Texture2D> native;
            CHECK_OVRCMD(ovr_GetTextureSwapChainBufferDX(s.session, chain->native, index, IID_PPV_ARGS(&native)));
            ComPtr<ID3D11Device> owner;
            native->GetDevice(&owner);
            ComPtr<IUnknown> actual, expected;
            CHECK_HRCMD(owner.As(&actual));
            CHECK_HRCMD(s.presentation.As(&expected));
            CHECK_MSG(actual == expected, "SDK returned a foreign presentation-device image");
            chain->nativeImages.push_back(native);
            if (nativeOnly)
                continue;
            D3D11_TEXTURE2D_DESC texture{};
            native->GetDesc(&texture);
            texture.Usage = D3D11_USAGE_DEFAULT;
            texture.CPUAccessFlags = 0;
            // Stencil-bearing resources are not NT-shareable on supported D3D11
            // drivers. Keep their complete format via the established KMT path.
            const bool nt =
                texture.Format != DXGI_FORMAT_R32G8X24_TYPELESS && texture.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
                texture.Format != DXGI_FORMAT_R24G8_TYPELESS && texture.Format != DXGI_FORMAT_D24_UNORM_S8_UINT;
            texture.MiscFlags = (texture.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) | D3D11_RESOURCE_MISC_SHARED |
                                (nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
            ComPtr<ID3D11Texture2D> privateImage, alias;
            CHECK_HRCMD(s.producer->CreateTexture2D(&texture, nullptr, &privateImage));
            ComPtr<IDXGIResource1> resource;
            CHECK_HRCMD(privateImage.As(&resource));
            wil::unique_handle handle;
            if (nt) {
                CHECK_HRCMD(resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, handle.put()));
                CHECK_HRCMD(s.presentation->OpenSharedResource1(handle.get(), IID_PPV_ARGS(&alias)));
            } else {
                HANDLE borrowed{};
                CHECK_HRCMD(resource->GetSharedHandle(&borrowed));
                CHECK_HRCMD(s.presentation->OpenSharedResource(borrowed, IID_PPV_ARGS(&alias)));
                // KMT handles borrow the producer texture lifetime. Never close
                // or duplicate them; the chain retains that texture until drain.
            }
            chain->producerImages.push_back(std::move(privateImage));
            chain->presentationAliases.push_back(std::move(alias));
            chain->handles.push_back(std::move(handle));
            ++s.statistics.privateExports;
        }
        s.chains.emplace(chain->native, chain);
        *output = chain->native;
        rollback.Deactivate();
        return result;
    }

    void DeliveryTransport::destroy(ovrTextureSwapChain chain) {
        if (!chain)
            return;
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        const auto found = s.chains.find(chain);
        CHECK_MSG(found != s.chains.end(), "Unknown delivery chain destruction");
        // Actual native deletion follows a complete two-domain drain. Batch
        // references retain source allocations between publication and End.
        s.retired.push_back(std::move(found->second));
        s.chains.erase(found);
    }

    ovrResult DeliveryTransport::length(ovrTextureSwapChain chain, int* value) {
        std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
        const auto found = m_state->chains.find(chain);
        if (found == m_state->chains.end() || !value)
            return ovrError_InvalidParameter;
        *value = static_cast<int>(found->second->nativeImages.size());
        return ovrSuccess;
    }
    ovrResult DeliveryTransport::description(ovrTextureSwapChain chain, ovrTextureSwapChainDesc* desc) {
        std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
        const auto found = m_state->chains.find(chain);
        if (found == m_state->chains.end() || !desc)
            return ovrError_InvalidParameter;
        *desc = found->second->desc;
        return ovrSuccess;
    }
    ovrResult DeliveryTransport::current(ovrTextureSwapChain chain, int* index) {
        {
            std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
            if (m_state->unknown || m_state->closed)
                return ovrError_RuntimeException;
            const auto found = m_state->chains.find(chain);
            if (found == m_state->chains.end() || !index)
                return ovrError_InvalidParameter;
            if (!found->second->nativeOnly) {
                if (found->second->reservedReady)
                    return ovrError_InvalidParameter;
                *index = found->second->producerIndex;
                return ovrSuccess;
            }
        }
        auto sdk = sdkLease("Current");
        std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
        if (m_state->unknown || m_state->closed)
            return ovrError_RuntimeException;
        const auto found = m_state->chains.find(chain);
        if (found == m_state->chains.end() || !index)
            return ovrError_InvalidParameter;
        if (found->second->nativeOnly)
            return ovr_GetTextureSwapChainCurrentIndex(m_state->session, chain, index);
        *index = found->second->producerIndex;
        return ovrSuccess;
    }
    ovrResult DeliveryTransport::buffer(ovrTextureSwapChain chain, int index, REFIID iid, void** output) {
        std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
        const auto found = m_state->chains.find(chain);
        if (found == m_state->chains.end() || !output || index < 0 ||
            static_cast<size_t>(index) >= found->second->nativeImages.size())
            return ovrError_InvalidParameter;
        const auto& c = *found->second;
        const auto result =
            (c.nativeOnly ? c.nativeImages[index] : c.producerImages[index])->QueryInterface(iid, output);
        CHECK_HRCMD(result);
        return ovrSuccess;
    }
    ovrResult DeliveryTransport::commit(ovrTextureSwapChain chain) {
        {
            std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
            if (m_state->unknown || m_state->closed)
                return ovrError_RuntimeException;
            const auto found = m_state->chains.find(chain);
            if (found == m_state->chains.end())
                return ovrError_InvalidParameter;
            auto& c = *found->second;
            if (!c.nativeOnly) {
                if (c.reservedReady)
                    return ovrError_InvalidParameter;
                c.producedIndex = c.producerIndex;
                c.pending = true;
                return ovrSuccess;
            }
        }
        auto sdk = sdkLease("Commit");
        std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
        const auto found = m_state->chains.find(chain);
        if (found == m_state->chains.end())
            return ovrError_InvalidParameter;
        if (m_state->unknown || m_state->closed)
            return ovrError_RuntimeException;
        auto& c = *found->second;
        if (c.nativeOnly) {
            const auto result = ovr_CommitTextureSwapChain(m_state->session, chain);
            if (OVR_FAILURE(result))
                m_state->unknown = true;
            return result;
        }
        c.producedIndex = c.producerIndex;
        c.pending = true;
        return ovrSuccess;
    }

    DeliveryTransport::Batch DeliveryTransport::prepare(const std::vector<ovrTextureSwapChain>& selected,
                                                        uint64_t serial) {
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        CHECK_MSG(!s.unknown && !s.closed, "Cannot publish uncertain producer images");
        Batch batch;
        batch.serial = serial;
        std::set<ovrTextureSwapChain> seen;
        for (const auto handle : selected) {
            if (!handle || !seen.insert(handle).second)
                continue;
            const auto found = s.chains.find(handle);
            CHECK_MSG(found != s.chains.end(), "Submitted delivery chain has no owner");
            auto& c = *found->second;
            if (c.nativeOnly || !c.pending)
                continue;
            CHECK_MSG(!c.reservedReady, "Producer image already belongs to a published batch");
            CHECK_MSG(c.producedIndex >= 0 && static_cast<size_t>(c.producedIndex) < c.producerImages.size(),
                      "Producer image has no finalized index");
            batch.images.push_back({found->second, c.producedIndex});
        }
        if (!batch.images.empty()) {
            CHECK_MSG(s.sourceValue < UINT64_MAX - 1, "Delivery source fence exhausted");
            batch.sourceReady = ++s.sourceValue;
            const auto signaled = s.sourceContext->Signal(s.sourceReady.Get(), batch.sourceReady);
            if (FAILED(signaled))
                s.unknown = true;
            CHECK_HRCMD(signaled);
            s.sourceContext->Flush();
            for (const auto& image : batch.images)
                image.owner->reservedReady = batch.sourceReady;
        }
        return batch;
    }

    void DeliveryTransport::copy(const Batch& batch) {
        if (batch.images.empty())
            return;
        auto sdk = sdkLease("Copy");
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        std::set<const Chain*> seen;
        CHECK_MSG(!s.unknown && !s.closed && batch.sourceReady, "Invalid native transport batch");
        for (const auto& image : batch.images) {
            CHECK_MSG(image.owner && seen.insert(image.owner.get()).second,
                      "Null or duplicated native transport owner");
            const auto& c = *image.owner;
            const auto found = s.chains.find(c.native);
            const bool retained = found != s.chains.end()
                                      ? found->second == image.owner
                                      : std::find(s.retired.begin(), s.retired.end(), image.owner) != s.retired.end();
            CHECK_MSG(retained && c.reservedReady == batch.sourceReady && !c.copied && c.pending &&
                          c.producedIndex == image.sourceIndex,
                      "Replayed, foreign or changed native transport receipt");
        }
        try {
            CHECK_HRCMD(s.targetContext->Wait(s.sourceOnTarget.Get(), batch.sourceReady));
            for (const auto& image : batch.images) {
                auto& c = *image.owner;
                CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(s.session, c.native, &c.nativeTarget));
                CHECK_MSG(c.nativeTarget >= 0 && static_cast<size_t>(c.nativeTarget) < c.nativeImages.size() &&
                              image.sourceIndex >= 0 &&
                              static_cast<size_t>(image.sourceIndex) < c.presentationAliases.size(),
                          "Native transport index outside owned images");
                s.targetContext->CopyResource(c.nativeImages[c.nativeTarget].Get(),
                                              c.presentationAliases[image.sourceIndex].Get());
                c.copied = true;
                ++s.statistics.nativeCopies;
            }
            CHECK_MSG(s.readValue < UINT64_MAX - 1, "Delivery read fence exhausted");
            CHECK_HRCMD(s.targetContext->Signal(s.readDone.Get(), ++s.readValue));
            s.targetContext->Flush();
        } catch (...) {
            s.unknown = true;
            throw;
        }
    }
    void DeliveryTransport::commitCopied(const Batch& batch) {
        if (batch.images.empty())
            return;
        auto sdk = sdkLease("CommitCopied");
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        CHECK_MSG(!s.unknown && !s.closed, "Cannot commit uncertain delivery graph");
        std::set<const Chain*> seen;
        for (const auto& image : batch.images) {
            CHECK_MSG(image.owner && seen.insert(image.owner.get()).second, "Null or duplicated native commit owner");
            const auto found = s.chains.find(image.owner->native);
            const bool retained = found != s.chains.end()
                                      ? found->second == image.owner
                                      : std::find(s.retired.begin(), s.retired.end(), image.owner) != s.retired.end();
            CHECK_MSG(retained && image.owner->reservedReady == batch.sourceReady && image.owner->copied,
                      "Foreign, replayed or uncopied native commit receipt");
        }
        try {
            for (const auto& image : batch.images) {
                auto& c = *image.owner;
                int target{};
                CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(s.session, c.native, &target));
                CHECK_MSG(target == c.nativeTarget, "Native delivery index changed before Commit");
                CHECK_OVRCMD(ovr_CommitTextureSwapChain(s.session, c.native));
                CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(s.session, c.native, &c.producerIndex));
                CHECK_MSG(c.producerIndex >= 0 && static_cast<size_t>(c.producerIndex) < c.producerImages.size(),
                          "Native SDK returned an invalid next image index");
                c.pending = c.copied = false;
                c.reservedReady = 0;
                c.nativeTarget = -1;
                ++s.statistics.nativeCommits;
            }
            ++s.statistics.batches;
        } catch (...) {
            s.unknown = true;
            throw;
        }
    }
    void DeliveryTransport::execute(const Batch& batch) {
        copy(batch);
        commitCopied(batch);
    }

    void DeliveryTransport::orderProducerAfterNativeReads() {
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        CHECK_MSG(!s.unknown, "Producer cannot overwrite uncertain native reads");
        if (!s.readValue)
            return;
        const auto done = s.readOnSource->GetCompletedValue();
        if (done == UINT64_MAX)
            s.unknown = true;
        CHECK_MSG(done != UINT64_MAX, "Native-read fence device removed");
        if (done < s.readValue) {
            const auto result = s.sourceContext->Wait(s.readOnSource.Get(), s.readValue);
            if (FAILED(result))
                s.unknown = true;
            CHECK_HRCMD(result);
        }
    }

    HRESULT DeliveryTransport::duplicateSharedHandle(ID3D11Texture2D* image, HANDLE* output) {
        std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
        if (!output)
            return E_POINTER;
        for (const auto& entry : m_state->chains) {
            const auto& c = *entry.second;
            for (size_t index = 0; index < c.producerImages.size(); ++index) {
                if (c.producerImages[index].Get() != image)
                    continue;
                if (!c.handles[index])
                    return S_FALSE;
                return DuplicateHandle(GetCurrentProcess(),
                                       c.handles[index].get(),
                                       GetCurrentProcess(),
                                       output,
                                       0,
                                       FALSE,
                                       DUPLICATE_SAME_ACCESS)
                           ? S_OK
                           : HRESULT_FROM_WIN32(GetLastError());
            }
        }
        return S_FALSE;
    }

    void DeliveryTransport::drain() {
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        if (s.closed)
            return;
        CHECK_MSG(!s.unknown, "Cannot certify an uncertain delivery graph");
        try {
            CHECK_HRCMD(s.sourceContext->Signal(s.sourceReady.Get(), ++s.sourceValue));
            s.sourceContext->Flush();
            CHECK_HRCMD(s.targetContext->Signal(s.readDone.Get(), ++s.readValue));
            s.targetContext->Flush();
            awaitFence(s.sourceReady.Get(), s.sourceValue);
            awaitFence(s.readDone.Get(), s.readValue);
        } catch (...) {
            s.unknown = true;
            throw;
        }
    }
    void DeliveryTransport::collectRetired() {
        const auto hasReclaimable = [&] {
            return std::any_of(m_state->retired.begin(), m_state->retired.end(), [](const auto& item) {
                return item.use_count() == 1;
            });
        };
        {
            std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
            if (m_state->retired.empty())
                return;
            CHECK_MSG(!m_state->unknown, "Cannot collect an uncertain delivery graph");
            // A published batch still owns every image. A full GPU drain could
            // reclaim nothing and needlessly block native presentation.
            if (!hasReclaimable())
                return;
        }
        auto sdk = sdkLease("CollectRetired");
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        CHECK_MSG(!s.unknown, "Cannot collect an uncertain delivery graph");
        if (!hasReclaimable())
            return;
        drain();
        auto item = s.retired.begin();
        while (item != s.retired.end()) {
            if (item->use_count() != 1) {
                ++item;
                continue;
            }
            ovr_DestroyTextureSwapChain(s.session, (*item)->native);
            item = s.retired.erase(item);
        }
    }
    void DeliveryTransport::shutdown() {
        if (!m_state)
            return;
        auto sdk = sdkLease("Shutdown");
        if (m_state->closed)
            return;
        drain();
        auto& s = *m_state;
        std::lock_guard<std::recursive_mutex> lock(s.metadata);
        // Shutdown is terminal cancellation after both queues drain. Outstanding
        // receipts may keep private images alive, but cannot execute after closed.
        for (const auto& entry : s.chains)
            ovr_DestroyTextureSwapChain(s.session, entry.first);
        for (const auto& entry : s.retired)
            ovr_DestroyTextureSwapChain(s.session, entry->native);
        s.chains.clear();
        s.retired.clear();
        s.closed = true;
    }
    bool DeliveryTransport::completionUnknown() const noexcept {
        return m_state && m_state->unknown;
    }
    DeliveryTransport::Statistics DeliveryTransport::statistics() const {
        std::lock_guard<std::recursive_mutex> lock(m_state->metadata);
        return m_state->statistics;
    }
    ID3D11Device5* DeliveryTransport::presentationDevice() const noexcept {
        return m_state->presentation.Get();
    }
    ID3D11DeviceContext4* DeliveryTransport::presentationContext() const noexcept {
        return m_state->targetContext.Get();
    }
} // namespace virtualdesktop_openxr
