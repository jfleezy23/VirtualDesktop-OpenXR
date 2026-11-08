#include "PairCaptureNrSources.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrEngineFacts.h"
#include "PairCaptureNrWrites.h"
#include <array>
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct SwapchainRow {
            uint64_t address{}, lifetime{};
            bool live{};
        };
        struct TextureRow {
            SourceTextureFacts facts{};
            D3D11_TEXTURE2D_DESC description{};
        };
        struct SourceJournal {
            std::mutex mutex;
            std::array<SwapchainRow, 32> swapchains{};
            std::array<TextureRow, 256> textures{};
            uint64_t nextLifetime{1}, nextRelease{1};
            size_t swapchainCount{}, textureCount{};
            bool exhausted{};
        } journal;
        static_assert(sizeof(SourceJournal) <= hostMetadataSource);
        uint64_t next(uint64_t& v) noexcept {
            if (!v || v == UINT64_MAX) {
                journal.exhausted = true;
                return 0;
            }
            return v++;
        }
        SwapchainRow* swapchain(const void* object) noexcept {
            const auto address = reinterpret_cast<uint64_t>(object);
            for (size_t i = journal.swapchainCount; i; --i)
                if (journal.swapchains[i - 1].address == address && journal.swapchains[i - 1].live)
                    return &journal.swapchains[i - 1];
            return nullptr;
        }
        TextureRow* texture(const void* object, uint32_t index) noexcept {
            const auto* s = swapchain(object);
            if (!s)
                return nullptr;
            for (size_t i = journal.textureCount; i; --i) {
                auto& t = journal.textures[i - 1];
                if (t.facts.swapchainLifetime == s->lifetime && t.facts.imageIndex == index && t.facts.live)
                    return &t;
            }
            return nullptr;
        }
    } // namespace
    uint64_t sourceClock() noexcept {
        if (!hostRequest().valid)
            return 0;
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        return static_cast<uint64_t>(q.QuadPart);
    }
    void sourceSwapchainCreated(const void* object) noexcept {
        if (!hostRequest().valid || !object)
            return;
        std::lock_guard lock(journal.mutex);
        if (swapchain(object) || journal.swapchainCount == journal.swapchains.size()) {
            journal.exhausted = true;
            return;
        }
        journal.swapchains[journal.swapchainCount++] = {
            reinterpret_cast<uint64_t>(object), next(journal.nextLifetime), true};
    }
    void sourceSwapchainDestroyed(const void* object) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(journal.mutex);
        auto* s = swapchain(object);
        if (!s)
            return;
        s->live = false;
        for (auto& t : journal.textures)
            if (t.facts.swapchainLifetime == s->lifetime)
                t.facts.live = false;
    }
    void sourceTextureCreated(const void* object,
                              uint32_t index,
                              ID3D11Texture2D* resource,
                              ID3D11Device* device,
                              const D3D11_TEXTURE2D_DESC& description) noexcept {
        if (!hostRequest().valid || !resource || !device)
            return;
        const auto boundLifetime = nrBankTextureLifetime(resource);
        std::lock_guard lock(journal.mutex);
        if (!boundLifetime) {
            journal.exhausted = true;
            return;
        }
        const auto* s = swapchain(object);
        if (!s || texture(object, index) || journal.textureCount == journal.textures.size()) {
            journal.exhausted = true;
            return;
        }
        auto& t = journal.textures[journal.textureCount++];
        t.description = description;
        t.facts.swapchain = s->address;
        t.facts.swapchainLifetime = s->lifetime;
        t.facts.imageIndex = index;
        t.facts.producer = reinterpret_cast<uint64_t>(resource);
        t.facts.producerDevice = reinterpret_cast<uint64_t>(device);
        t.facts.producerLifetime = boundLifetime;
        t.facts.live = true;
    }
    void sourceQueryObserved(const void* object,
                             uint32_t index,
                             const void* resource,
                             const void* iface,
                             int32_t result,
                             uint64_t begin,
                             uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(journal.mutex);
        auto* t = texture(object, index);
        if (!t || t->facts.producer != reinterpret_cast<uint64_t>(resource)) {
            journal.exhausted = true;
            return;
        }
        auto& f = t->facts;
        if (f.exportObserved) {
            // A cached NT handle retains the QI facts from its actual export.
            if (result < 0)
                journal.exhausted = true;
            return;
        }
        f.queryObserved = true;
        f.queryResult = result;
        f.queryInterfaceThis = reinterpret_cast<uint64_t>(iface);
        f.queryBegin = begin;
        f.queryEnd = end;
        f.thread = GetCurrentThreadId();
    }
    void sourceExportObserved(const void* object,
                              uint32_t index,
                              HANDLE handle,
                              bool nt,
                              uint32_t access,
                              int32_t result,
                              uint64_t begin,
                              uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(journal.mutex);
        auto* t = texture(object, index);
        if (!t) {
            journal.exhausted = true;
            return;
        }
        auto& f = t->facts;
        f.sharedHandle = reinterpret_cast<uint64_t>(handle);
        f.exportNt = nt;
        f.exportAccess = access;
        f.exportResult = result;
        f.exportObserved = true;
        f.exportBegin = begin;
        f.exportEnd = end;
        f.thread = GetCurrentThreadId();
    }
    void sourceImportObserved(const void* object,
                              uint32_t index,
                              ID3D11Device* device,
                              const void* iface,
                              ID3D11Texture2D* output,
                              HANDLE handle,
                              bool nt,
                              int32_t result,
                              uint64_t begin,
                              uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(journal.mutex);
        auto* t = texture(object, index);
        if (!t || t->facts.sharedHandle != reinterpret_cast<uint64_t>(handle)) {
            journal.exhausted = true;
            return;
        }
        auto& f = t->facts;
        f.applicationDevice = reinterpret_cast<uint64_t>(device);
        f.importInterfaceThis = reinterpret_cast<uint64_t>(iface);
        f.application = reinterpret_cast<uint64_t>(output);
        f.applicationLifetime = output ? next(journal.nextLifetime) : 0;
        f.importNt = nt;
        f.importResult = result;
        f.importObserved = true;
        f.importBegin = begin;
        f.importEnd = end;
        f.thread = GetCurrentThreadId();
    }
    void sourceImageReleased(const void* object, uint32_t index) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(journal.mutex);
        auto* t = texture(object, index);
        if (!t) {
            journal.exhausted = true;
            return;
        }
        t->facts.releaseEpoch = next(journal.nextRelease);
        t->facts.releasedIndex = index;
    }
    bool sourceGet(const void* object,
                   uint32_t index,
                   SourceTextureFacts& facts,
                   D3D11_TEXTURE2D_DESC& description) noexcept {
        facts = {};
        description = {};
        if (!hostRequest().valid)
            return false;
        std::lock_guard lock(journal.mutex);
        const auto* t = texture(object, index);
        if (!t || journal.exhausted)
            return false;
        facts = t->facts;
        description = t->description;
        return true;
    }
    bool sourceApplicationWrite11(uint64_t producer,
                                  uint64_t producerLifetime,
                                  ResourceWriteFact& out,
                                  SourceTextureFacts* applicationAlias) noexcept {
        out = {};
        if (applicationAlias)
            *applicationAlias = {};
        if (!hostRequest().valid || !producer || !producerLifetime)
            return false;
        ResourceWriteFact ignored{};
        if (originalWriteStatus(producer, producerLifetime, ignored) == OriginalWriteStatus::invalid)
            return false;
        SourceTextureFacts fact{};
        D3D11_TEXTURE2D_DESC description{};
        {
            std::lock_guard lock(journal.mutex);
            if (journal.exhausted)
                return false;
            const TextureRow* found{};
            for (size_t i = 0; i < journal.textureCount; ++i) {
                const auto& row = journal.textures[i];
                if (!row.facts.live || row.facts.producer != producer || row.facts.producerLifetime != producerLifetime)
                    continue;
                if (found || texture(reinterpret_cast<const void*>(row.facts.swapchain),
                                     static_cast<uint32_t>(row.facts.imageIndex)) != &row)
                    return false;
                found = &row;
            }
            if (!found)
                return false;
            fact = found->facts;
            description = found->description;
        }
        // No source-registry lock crosses the separately locked control snapshot.
        NrSourceWitnessV1 source{};
        NrError error{};
        const auto& request = hostRequest();
        if (!sourceAliasComplete(fact, producer, fact.application, fact.imageIndex) || !hostLatestSource(source) ||
            !nr_validate_source(&source, sizeof source, &error) || source.header.requestNonceLo != request.nonce[0] ||
            source.header.requestNonceHi != request.nonce[1] || source.header.pid != request.pid ||
            source.header.creationFiletime != request.creation || source.header.endThread != GetCurrentThreadId() ||
            source.header.sourceModuleIndex != 4)
            return false;
        uint32_t index{};
        if (!selectRawSourceCopy(source, fact.application, 0, description.ArraySize, description.MipLevels, index))
            return false;
        const auto& copy = source.copies[index];
        if (copy.qpcBefore < fact.importEnd)
            return false;
        out.resource = copy.destinationAddress;
        out.lifetime = fact.applicationLifetime;
        out.sequence = copy.destinationWriteObservationEpoch;
        out.serial = source.header.applicationSubmissionId;
        out.contextOrList = copy.actualContextAddress;
        out.api = 11;
        out.kind = NR_COPY;
        out.actualCallSequence = copy.actualCopySequence;
        out.sourceAddress = copy.sourceAddress;
        out.thread = source.header.sourceThread;
        out.qpc = copy.qpcAfter;
        if (applicationAlias)
            *applicationAlias = fact;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
