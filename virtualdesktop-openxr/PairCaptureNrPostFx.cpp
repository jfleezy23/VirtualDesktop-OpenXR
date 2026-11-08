#include "PairCaptureNrPostFx.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrSources.h"
#include "PairCaptureNrWrites.h"
#include <bcrypt.h>
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        enum class ObjectKind { shader, buffer, srv, uav, sampler };
        struct Object {
            uint64_t address{}, lifetime{};
            ObjectKind kind{};
            NrDescriptorV1 descriptor{};
            std::array<uint8_t, 32> shaderHash{};
        };
        struct Frame {
            uint64_t serial{};
            std::array<PostFxRecord, 4> rows{};
            std::array<bool, 4> present{};
        };
        struct Journal {
            std::mutex mutex;
            std::array<Object, 256> objects{};
            std::array<Frame, 8> frames{};
            size_t count{};
            uint64_t nextObject{1}, nextBinding{1}, nextMapWrite{1}, nextEvent{1};
            bool invalid{};
        } journal;
        static_assert(sizeof(Journal) <= hostMetadataPostFx);
        uint64_t clock() noexcept {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            return static_cast<uint64_t>(q.QuadPart);
        }
        void invalid() noexcept {
            std::lock_guard lock(journal.mutex);
            journal.invalid = true;
        }
        void remember(Object row) noexcept {
            std::lock_guard lock(journal.mutex);
            if (!row.address || journal.count == journal.objects.size() || !journal.nextObject ||
                journal.nextObject == UINT64_MAX) {
                journal.invalid = true;
                return;
            }
            row.lifetime = journal.nextObject++;
            if (row.descriptor.kind) {
                row.descriptor.objectAddress = row.address;
                row.descriptor.objectLifetimeEpoch = row.lifetime;
                row.descriptor.writeEpoch = row.lifetime;
                row.descriptor.observedMask = 15;
            }
            journal.objects[journal.count++] = row;
        }
        bool object(uint64_t address, ObjectKind kind, Object& out) noexcept {
            for (size_t i = journal.count; i; --i)
                if (journal.objects[i - 1].address == address && journal.objects[i - 1].kind == kind) {
                    out = journal.objects[i - 1];
                    return true;
                }
            return false;
        }
        void rememberView(ID3D11Texture2D* resource,
                          const void* view,
                          ObjectKind kind,
                          NrDescriptorV1 description) noexcept {
            const auto lifetime = nrBankTextureLifetime(resource);
            if (!view || !resource || !lifetime || !description.kind) {
                invalid();
                return;
            }
            Object row{};
            row.address = reinterpret_cast<uint64_t>(view);
            row.kind = kind;
            row.descriptor = description;
            row.descriptor.resourceAddress = reinterpret_cast<uint64_t>(resource);
            row.descriptor.resourceLifetimeEpoch = lifetime;
            remember(row);
        }
    } // namespace
    void postFxShaderCreated(ID3D11ComputeShader* shader, const void* bytes, size_t count) noexcept {
        if (!hostRequest().valid)
            return;
        Object row{};
        row.address = reinterpret_cast<uint64_t>(shader);
        row.kind = ObjectKind::shader;
        // Windows CNG hashes the exact bytecode pointer/count passed to original
        // CreateComputeShader. No observer lock spans CNG or any original API.
        if (!shader || !bytes || !count || count > 1048576 ||
            BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                       nullptr,
                       0,
                       const_cast<PUCHAR>(static_cast<const unsigned char*>(bytes)),
                       static_cast<ULONG>(count),
                       row.shaderHash.data(),
                       static_cast<ULONG>(row.shaderHash.size())) < 0) {
            invalid();
            return;
        }
        remember(row);
    }
    void postFxBufferCreated(ID3D11Buffer* buffer) noexcept {
        if (!hostRequest().valid)
            return;
        Object row{};
        row.address = reinterpret_cast<uint64_t>(buffer);
        row.kind = ObjectKind::buffer;
        remember(row);
    }
    void postFxSrvCreated(ID3D11Texture2D* resource,
                          ID3D11ShaderResourceView* view,
                          const D3D11_SHADER_RESOURCE_VIEW_DESC& description) noexcept {
        if (!hostRequest().valid)
            return;
        NrDescriptorV1 row{};
        if (!viewDescription(description, row)) {
            invalid();
            return;
        }
        rememberView(resource, view, ObjectKind::srv, row);
    }
    void postFxUavCreated(ID3D11Texture2D* resource,
                          ID3D11UnorderedAccessView* view,
                          const D3D11_UNORDERED_ACCESS_VIEW_DESC& description) noexcept {
        if (!hostRequest().valid)
            return;
        NrDescriptorV1 row{};
        if (!viewDescription(description, row)) {
            invalid();
            return;
        }
        rememberView(resource, view, ObjectKind::uav, row);
    }
    void postFxSamplerCreated(ID3D11SamplerState* sampler, const D3D11_SAMPLER_DESC& description) noexcept {
        if (!hostRequest().valid)
            return;
        Object row{};
        row.address = reinterpret_cast<uint64_t>(sampler);
        row.kind = ObjectKind::sampler;
        if (!viewDescription(description, row.descriptor)) {
            invalid();
            return;
        }
        remember(row);
    }
    void postFxMapReturned(PostFxRecord& record,
                           ID3D11DeviceContext4* context,
                           ID3D11Buffer* buffer,
                           HRESULT result) noexcept {
        if (!hostRequest().valid)
            return;
        const auto thread = GetCurrentThreadId();
        std::lock_guard lock(journal.mutex);
        Object actualBuffer{};
        if (record.mapObserved || !context || !buffer || journal.invalid ||
            !object(reinterpret_cast<uint64_t>(buffer), ObjectKind::buffer, actualBuffer) || !journal.nextEvent ||
            journal.nextEvent == UINT64_MAX) {
            journal.invalid = true;
            return;
        }
        record.mapObserved = true;
        record.mapResultBits = static_cast<uint32_t>(result);
        record.mapEvent = journal.nextEvent++;
        record.mapThread = thread;
        record.constantContextAddress = reinterpret_cast<uint64_t>(context);
        record.constantBufferAddress = reinterpret_cast<uint64_t>(buffer);
        record.constantBufferLifetime = actualBuffer.lifetime;
    }
    void postFxUnmapReturned(PostFxRecord& record, ID3D11DeviceContext4* context, ID3D11Buffer* buffer) noexcept {
        if (!hostRequest().valid)
            return;
        const auto thread = GetCurrentThreadId();
        std::lock_guard lock(journal.mutex);
        if (journal.invalid || !record.constantsObserved || record.unmapEvent || record.mapThread != thread ||
            record.constantContextAddress != reinterpret_cast<uint64_t>(context) ||
            record.constantBufferAddress != reinterpret_cast<uint64_t>(buffer) || !journal.nextEvent ||
            journal.nextEvent == UINT64_MAX) {
            journal.invalid = true;
            return;
        }
        record.unmapEvent = journal.nextEvent++;
    }
    void postFxConstants(PostFxRecord& record,
                         ID3D11DeviceContext4* context,
                         ID3D11Buffer* buffer,
                         const void* mapped,
                         size_t count) noexcept {
        if (!hostRequest().valid)
            return;
        if (!context || !buffer || !mapped || (count != 80 && count != 112) || record.constantsObserved ||
            !record.mapObserved || (record.mapResultBits & 0x80000000u) || record.mapThread != GetCurrentThreadId() ||
            record.constantContextAddress != reinterpret_cast<uint64_t>(context) ||
            record.constantBufferAddress != reinterpret_cast<uint64_t>(buffer)) {
            invalid();
            return;
        }
        // Called immediately after the original memcpy and before original Unmap.
        // This is the observed host write version, not a bind or GPU completion epoch.
        std::memcpy(record.constants.data(), mapped, count);
        std::lock_guard lock(journal.mutex);
        Object actualBuffer{};
        if (journal.invalid || !object(reinterpret_cast<uint64_t>(buffer), ObjectKind::buffer, actualBuffer) ||
            record.constantBufferLifetime != actualBuffer.lifetime || !journal.nextMapWrite ||
            journal.nextMapWrite == UINT64_MAX || !journal.nextEvent || journal.nextEvent == UINT64_MAX) {
            journal.invalid = true;
            return;
        }
        const auto sequence = journal.nextMapWrite++;
        record.constantContextAddress = reinterpret_cast<uint64_t>(context);
        record.constantBufferAddress = reinterpret_cast<uint64_t>(buffer);
        record.constantBufferLifetime = actualBuffer.lifetime;
        record.constantWriteSequence = sequence;
        record.draw.batchEpoch = sequence;
        record.copyEvent = journal.nextEvent++;
        record.constantsObserved = true;
        record.draw.constantsBytes = count;
    }
    void postFxGeometry(PostFxRecord& record,
                        int32_t x,
                        int32_t y,
                        int32_t width,
                        int32_t height,
                        int32_t sourceWidth,
                        int32_t sourceHeight,
                        int32_t outputWidth,
                        int32_t outputHeight,
                        bool upscaling,
                        bool sharpening,
                        bool neuralInput) noexcept {
        if (!hostRequest().valid)
            return;
        if (record.geometryObserved || x < 0 || y < 0 || width <= 0 || height <= 0 || sourceWidth <= 0 ||
            sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0 || x > sourceWidth - width ||
            y > sourceHeight - height) {
            invalid();
            return;
        }
        record.inputX = x;
        record.inputY = y;
        record.inputWidth = width;
        record.inputHeight = height;
        record.sourceWidth = sourceWidth;
        record.sourceHeight = sourceHeight;
        record.outputWidth = outputWidth;
        record.outputHeight = outputHeight;
        record.upscaling = upscaling;
        record.sharpening = sharpening;
        record.neuralInput = neuralInput;
        record.geometryObserved = true;
    }
    void postFxBindingObserved(PostFxRecord& record, PostFxBinding binding, const void* actual) noexcept {
        if (!hostRequest().valid)
            return;
        const auto thread = GetCurrentThreadId();
        const auto index = static_cast<size_t>(binding);
        const auto address = reinterpret_cast<uint64_t>(actual);
        std::lock_guard lock(journal.mutex);
        if (index >= record.bindingEpochs.size() || !address || record.bindingEpochs[index] || !journal.nextBinding ||
            journal.nextBinding == UINT64_MAX || !journal.nextEvent || journal.nextEvent == UINT64_MAX) {
            journal.invalid = true;
            return;
        }
        if (binding == PostFxBinding::buffer &&
            (!record.constantsObserved || !record.unmapEvent || record.mapThread != thread ||
             record.constantBufferAddress != address)) {
            journal.invalid = true;
            return;
        }
        const auto epoch = journal.nextBinding++;
        record.bindingEpochs[index] = epoch;
        record.bindingEvents[index] = journal.nextEvent++;
        record.draw.bindEpoch = epoch;
        switch (binding) {
        case PostFxBinding::shader:
            record.draw.psoAddress = address;
            break;
        case PostFxBinding::buffer:
            record.draw.rootAddress = address;
            break;
        case PostFxBinding::srv:
            record.draw.rootTable0 = address;
            break;
        case PostFxBinding::uav:
            record.draw.rootTable1 = address;
            break;
        case PostFxBinding::sampler:
            record.samplerAddress = address;
            break;
        }
    }
    bool observePostFxDispatch(PostFxRecord& record,
                               uint64_t serial,
                               uint32_t eye,
                               uint32_t kind,
                               ID3D11DeviceContext4* context,
                               uint32_t x,
                               uint32_t y,
                               uint32_t z) noexcept {
        if (!hostRequest().valid)
            return false;
        if (!record.constantsObserved || record.constantContextAddress != reinterpret_cast<uint64_t>(context) ||
            !serial || eye > 1 || (kind != 3 && kind != 4) || !context) {
            invalid();
            return false;
        }
        Object shader{}, buffer{}, srv{}, uav{}, sampler{};
        {
            std::lock_guard lock(journal.mutex);
            if (!journal.nextEvent || journal.nextEvent == UINT64_MAX) {
                journal.invalid = true;
                return false;
            }
            record.dispatchEvent = journal.nextEvent++;
            if (journal.invalid || !object(record.draw.psoAddress, ObjectKind::shader, shader) ||
                !object(record.draw.rootAddress, ObjectKind::buffer, buffer) ||
                !object(record.draw.rootTable0, ObjectKind::srv, srv) ||
                !object(record.draw.rootTable1, ObjectKind::uav, uav) ||
                (kind == 3 && !object(record.samplerAddress, ObjectKind::sampler, sampler)))
                return false;
        }
        if (record.constantBufferLifetime != buffer.lifetime) {
            invalid();
            return false;
        }
        // These lookups consume only fixed scalar journals; no view/resource COM
        // query or Runtime callback occurs while the observer lock is held.
        ResourceWriteFact input{}, output{};
        if (!sourceProducerWrite11(reinterpret_cast<ID3D11Texture2D*>(srv.descriptor.resourceAddress), input) ||
            !sourceProducerWrite11(reinterpret_cast<ID3D11Texture2D*>(uav.descriptor.resourceAddress), output))
            return false;
        record.serial = serial;
        record.shaderLifetime = shader.lifetime;
        record.bufferLifetime = buffer.lifetime;
        record.shaderHash = shader.shaderHash;
        record.dispatchThread = GetCurrentThreadId();
        record.dispatchQpc = clock();
        record.draw.kind = kind;
        record.draw.eye = eye;
        record.draw.listOrContext = reinterpret_cast<uint64_t>(context);
        record.draw.dispatchX = x;
        record.draw.dispatchY = y;
        record.draw.dispatchZ = z;
        record.draw.resourceWriteEpoch = output.sequence;
        record.draw.observedMask = 15;
        record.descriptorCount = kind == 3 ? 3u : 2u;
        record.draw.descriptorCount = record.descriptorCount;
        record.descriptors[0] = srv.descriptor;
        record.descriptors[1] = uav.descriptor;
        record.descriptors[0].sourceWriteEpoch = input.sequence;
        record.descriptors[1].sourceWriteEpoch = output.sequence;
        record.descriptors[0].bindEpoch = record.bindingEpochs[static_cast<size_t>(PostFxBinding::srv)];
        record.descriptors[1].bindEpoch = record.bindingEpochs[static_cast<size_t>(PostFxBinding::uav)];
        if (kind == 3) {
            record.descriptors[2] = sampler.descriptor;
            record.descriptors[2].bindEpoch = record.bindingEpochs[static_cast<size_t>(PostFxBinding::sampler)];
        }
        if (!postFxRecordComplete(record)) {
            invalid();
            return false;
        }
        std::lock_guard lock(journal.mutex);
        if (journal.invalid)
            return false;
        auto& frame = journal.frames[serial % journal.frames.size()];
        if (frame.serial != serial) {
            frame = {};
            frame.serial = serial;
        }
        const auto slot = (kind - 3) * 2 + eye;
        if (frame.present[slot]) {
            journal.invalid = true;
            return false;
        }
        frame.rows[slot] = record;
        frame.present[slot] = true;
        return true;
    }
    bool postFxFrameComplete(uint64_t serial, bool upscaling, bool sharpening) noexcept {
        if (!upscaling && !sharpening)
            return true;
        std::lock_guard lock(journal.mutex);
        const auto& frame = journal.frames[serial % journal.frames.size()];
        if (journal.invalid || frame.serial != serial)
            return false;
        for (size_t i = 0; i < 4; ++i)
            if ((i < 2 ? upscaling : sharpening) && (!frame.present[i] || !postFxRecordComplete(frame.rows[i])))
                return false;
        return true;
    }
    bool copyPostFxRecord(uint64_t serial, uint32_t eye, uint32_t kind, PostFxRecord& out) noexcept {
        out = {};
        if (eye > 1 || (kind != 3 && kind != 4))
            return false;
        std::lock_guard lock(journal.mutex);
        const auto& frame = journal.frames[serial % journal.frames.size()];
        const auto slot = (kind - 3) * 2 + eye;
        if (journal.invalid || frame.serial != serial || !frame.present[slot])
            return false;
        out = frame.rows[slot];
        return postFxRecordComplete(out);
    }
    bool copyPostFxSidecar(const NrPacketV1& packet, bool upscaling, bool sharpening, NrPostFxSidecarV1& out) noexcept {
        out = {};
        const auto& h = packet.header;
        const auto& id = packet.identity;
        if (h.operation != NR_FRAME_TRANSFER || !h.attempt || !h.transferId || h.frameOrdinal > 1 || !id.hostSerial ||
            !id.applicationSubmissionId || !id.publicJournalWitness ||
            !postFxFrameComplete(id.hostSerial, upscaling, sharpening))
            return false;
        NrPostFxSidecarV1 result{};
        result.size = sizeof result;
        result.version = 1;
        result.attempt = h.attempt;
        result.transferId = h.transferId;
        result.frameOrdinal = h.frameOrdinal;
        result.hostSerial = id.hostSerial;
        result.applicationSubmissionId = id.applicationSubmissionId;
        result.corpusFrameIndex = id.corpusFrameIndex;
        result.publicJournalWitness = id.publicJournalWitness;
        for (size_t i = 0; i < 2; ++i) {
            result.nonce[i] = id.nonce[i];
            result.sessionNonce[i] = id.sessionNonce[i];
        }
        result.pid = id.pid;
        result.creationFiletime = id.creationFiletime;
        result.fgContextAddress = id.fgContextAddress;
        result.fgContextLifetimeEpoch = id.fgContextLifetimeEpoch;
        result.fgHistoryGeneration = id.fgHistoryGeneration;
        result.actualNrMode = h.actualNrMode;
        result.stageFlags = (upscaling ? NR_POSTFX_EASU : 0u) | (sharpening ? NR_POSTFX_CAS : 0u);
        for (uint32_t kind = 3; kind <= 4; ++kind) {
            if (!(kind == 3 ? upscaling : sharpening))
                continue;
            for (uint32_t eye = 0; eye < 2; ++eye) {
                PostFxRecord record{};
                if (!copyPostFxRecord(id.hostSerial, eye, kind, record) || record.upscaling != upscaling ||
                    record.sharpening != sharpening || !postFxExportRow(record, result.rows[result.rowCount]))
                    return false;
                ++result.rowCount;
            }
        }
        out = result;
        return true;
    }

} // namespace virtualdesktop_openxr::nr_capture
