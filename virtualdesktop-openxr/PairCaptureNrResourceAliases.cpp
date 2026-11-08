#include "PairCaptureNrSources.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrWrites.h"
#include <array>
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct BankJournal {
            std::mutex mutex;
            std::array<SourceTextureFacts, 256> rows{};
            size_t count{};
            uint64_t nextLifetime{1};
            bool invalid{};
        } banks;
        static_assert(sizeof(BankJournal) <= hostMetadataBank);
        SourceTextureFacts* bankTexture(const void* resource) noexcept {
            const auto address = reinterpret_cast<uint64_t>(resource);
            for (size_t i = banks.count; i; --i)
                if (banks.rows[i - 1].producer == address && banks.rows[i - 1].live)
                    return &banks.rows[i - 1];
            return nullptr;
        }
    } // namespace
    void nrBankTextureObserved(ID3D11Texture2D* resource, ID3D11Device* device) noexcept {
        if (!hostRequest().valid || !resource || !device)
            return;
        std::lock_guard lock(banks.mutex);
        if (banks.count == banks.rows.size() || !banks.nextLifetime || banks.nextLifetime == UINT64_MAX) {
            banks.invalid = true;
            return;
        }
        // Called only at original successful creation/buffer acquisition. A new
        // owned bank binding gets a fresh epoch, even if an address is reused.
        if (auto* old = bankTexture(resource))
            old->live = false;
        auto& row = banks.rows[banks.count++];
        row.producer = reinterpret_cast<uint64_t>(resource);
        row.producerDevice = reinterpret_cast<uint64_t>(device);
        row.producerLifetime = banks.nextLifetime++;
        row.live = true;
    }
    uint64_t nrBankTextureLifetime(ID3D11Texture2D* resource) noexcept {
        if (!hostRequest().valid)
            return 0;
        std::lock_guard lock(banks.mutex);
        const auto* row = bankTexture(resource);
        return !banks.invalid && row ? row->producerLifetime : 0;
    }
    bool sourceNrOriginalWrite12(ID3D12Resource* resource, uint64_t lifetime, ResourceWriteFact& out) noexcept {
        out = {};
        SourceTextureFacts alias{};
        if (!resource || !lifetime || !sourceNrGet(resource, alias) || alias.applicationLifetime != lifetime ||
            !nrAliasComplete(alias,
                             alias.producer,
                             reinterpret_cast<uint64_t>(resource),
                             alias.producerDevice,
                             alias.applicationDevice))
            return false;
        // This is the exact original producer write observed at bind, through an
        // existing successful storage import. It does not invent an imported write.
        const auto status = originalWriteStatus(alias.producer, alias.producerLifetime, out);
        return status == OriginalWriteStatus::observed ||
               (status == OriginalWriteStatus::absent &&
                sourceApplicationWrite11(alias.producer, alias.producerLifetime, out));
    }
    bool sourceProducerWrite11(ID3D11Texture2D* resource, ResourceWriteFact& out) noexcept {
        out = {};
        if (!hostRequest().valid)
            return false;
        SourceTextureFacts alias{};
        {
            std::lock_guard lock(banks.mutex);
            const auto* row = bankTexture(resource);
            if (!row || banks.invalid)
                return false;
            alias = *row;
        }
        ResourceWriteFact direct{}, imported{};
        const auto ownStatus = originalWriteStatus(alias.producer, alias.producerLifetime, direct);
        if (ownStatus == OriginalWriteStatus::invalid)
            return false;
        const bool own = ownStatus == OriginalWriteStatus::observed;
        bool shared = false;
        if (nrAliasComplete(alias, alias.producer, alias.application, alias.producerDevice, alias.applicationDevice) &&
            sdkObservedResourceLifetime(reinterpret_cast<ID3D12Resource*>(alias.application)) ==
                alias.applicationLifetime) {
            const auto sharedStatus = originalWriteStatus(alias.application, alias.applicationLifetime, imported);
            if (sharedStatus == OriginalWriteStatus::invalid)
                return false;
            shared = sharedStatus == OriginalWriteStatus::observed;
        }
        if (!own && !shared)
            return sourceApplicationWrite11(alias.producer, alias.producerLifetime, out);
        // Both sequences originate in the same actual producer-command journal.
        out = shared && (!own || imported.sequence > direct.sequence) ? imported : direct;
        return true;
    }
    void sourceBankWriteObserved(ID3D11Texture2D* resource,
                                 ID3D11DeviceContext4* context,
                                 uint64_t serial,
                                 uint64_t kind,
                                 ID3D11Texture2D* source) noexcept {
        if (!hostRequest().valid)
            return;
        const auto lifetime = nrBankTextureLifetime(resource);
        if (!observeOriginalWrite(reinterpret_cast<uint64_t>(resource),
                                  lifetime,
                                  serial,
                                  reinterpret_cast<uint64_t>(context),
                                  0,
                                  11,
                                  kind,
                                  0,
                                  reinterpret_cast<uint64_t>(source))) {
            std::lock_guard lock(banks.mutex);
            banks.invalid = true;
        }
    }
    void sourceNrQueryObserved(
        ID3D11Texture2D* resource, const void* iface, int32_t result, uint64_t begin, uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(banks.mutex);
        auto* row = bankTexture(resource);
        if (!row) {
            banks.invalid = true;
            return;
        }
        // A cached NT export keeps the exact QI/interface used by that export.
        if (row->exportObserved) {
            if (result < 0)
                banks.invalid = true;
            return;
        }
        row->queryObserved = true;
        row->queryResult = result;
        row->queryInterfaceThis = reinterpret_cast<uint64_t>(iface);
        row->queryBegin = begin;
        row->queryEnd = end;
        row->thread = GetCurrentThreadId();
    }
    void sourceNrExportObserved(ID3D11Texture2D* resource,
                                HANDLE handle,
                                bool nt,
                                uint32_t access,
                                int32_t result,
                                uint64_t begin,
                                uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(banks.mutex);
        auto* row = bankTexture(resource);
        if (!row) {
            banks.invalid = true;
            return;
        }
        row->sharedHandle = reinterpret_cast<uint64_t>(handle);
        row->exportNt = nt;
        row->exportAccess = access;
        row->exportObserved = true;
        row->exportResult = result;
        row->exportBegin = begin;
        row->exportEnd = end;
        row->thread = GetCurrentThreadId();
    }
    void sourceNrImportObserved(ID3D11Texture2D* source,
                                ID3D12Device* device,
                                ID3D12Resource* target,
                                uint64_t targetLifetime,
                                HANDLE handle,
                                bool nt,
                                int32_t result,
                                uint64_t begin,
                                uint64_t end) noexcept {
        if (!hostRequest().valid)
            return;
        std::lock_guard lock(banks.mutex);
        auto* row = bankTexture(source);
        if (!row || row->sharedHandle != reinterpret_cast<uint64_t>(handle)) {
            banks.invalid = true;
            return;
        }
        // application* is the generic imported side of SourceTextureFacts here:
        // API5 explicitly tags these words Resource12/D12Device, never Texture11.
        row->application = reinterpret_cast<uint64_t>(target);
        row->applicationLifetime = targetLifetime;
        row->applicationDevice = reinterpret_cast<uint64_t>(device);
        row->importInterfaceThis = reinterpret_cast<uint64_t>(device);
        row->importNt = nt;
        row->importResult = result;
        row->importObserved = true;
        row->importBegin = begin;
        row->importEnd = end;
        row->thread = GetCurrentThreadId();
    }
    bool sourceNrGet(ID3D12Resource* target, SourceTextureFacts& out) noexcept {
        out = {};
        if (!hostRequest().valid || !target)
            return false;
        std::lock_guard lock(banks.mutex);
        if (banks.invalid)
            return false;
        for (size_t i = banks.count; i; --i) {
            const auto& row = banks.rows[i - 1];
            if (row.live && row.application == reinterpret_cast<uint64_t>(target)) {
                out = row;
                return true;
            }
        }
        return false;
    }
} // namespace virtualdesktop_openxr::nr_capture
