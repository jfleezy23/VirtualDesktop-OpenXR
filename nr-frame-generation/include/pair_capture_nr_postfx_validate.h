#ifndef PAIR_CAPTURE_NR_POSTFX_VALIDATE_H
#define PAIR_CAPTURE_NR_POSTFX_VALIDATE_H
#include "pair_capture_nr_abi.h"
#include "pair_capture_nr_postfx_abi.h"
#include <string.h>
/* Pure scalar gate, after nr_validate_packet. No address is dereferenced.
   This proves recorded consistency, not execution or mathematical correctness. */
static inline int nr_postfx_zero(const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    size_t i;
    for (i = 0; i < bytes; ++i)
        if (p[i])
            return 0;
    return 1;
}
static inline int
nr_postfx_held_group(const NrPacketV1* p, uint64_t address, uint64_t life, uint64_t type, uint64_t group) {
    uint64_t i;
    int found = 0;
    if (!address || !life)
        return 0;
    for (i = 0; i < p->header.refCount; ++i) {
        const NrRefV1* r = &p->refs[i];
        if (r->borrowedIUnknownAddress == address && r->interfaceEnum == type && r->groupIndex == group) {
            if (r->owningSourceLifetimeEpoch != life)
                return 0;
            found = 1;
        }
    }
    return found;
}
static inline int nr_postfx_held(const NrPacketV1* p, uint64_t address, uint64_t life, uint64_t type) {
    return nr_postfx_held_group(p, address, life, type, 2);
}
static inline int nr_postfx_input(const NrPacketV1* p, const NrPostFxRowV1* r) {
    uint64_t i, j;
    unsigned matches = 0;
    if (nr_postfx_held(p, r->inputResourceAddress, r->inputResourceLifetime, NR_TEXTURE11))
        return 1;
    /* The actual retained D11 SRV owns its numeric underlying texture. API5
       joins that producer to the independently held NR D12 import. No extra
       hidden reference or callable D11 resource is introduced here. */
    for (i = 0; i < p->header.aliasCount; ++i) {
        const NrAliasV1* a = &p->aliases[i];
        if (a->apiKind == 5 && a->sourceAddress == r->inputResourceAddress &&
            a->sourceLifetime == r->inputResourceLifetime && a->sourceWriteEpoch == r->inputSourceWriteEpoch &&
            nr_postfx_held_group(p, a->targetAddress, a->targetLifetime, NR_RESOURCE12, 1)) {
            for (j = 0; j < p->header.roleCount; ++j) {
                const NrRoleV1* role = &p->roles[j];
                if (role->role == NR_POST_RESOLVE && role->sourceAddressObserved == a->targetAddress &&
                    role->sourceLifetimeEpoch == a->targetLifetime && role->sourceWriteEpoch == a->targetWriteEpoch)
                    ++matches;
            }
        }
    }
    return matches == 1;
}
static inline int nr_postfx_descriptor(const NrPacketV1* p,
                                       const NrDrawV1* d,
                                       uint64_t kind,
                                       uint64_t object,
                                       uint64_t life,
                                       uint64_t epoch,
                                       uint64_t resource,
                                       uint64_t resourceLife,
                                       uint64_t write) {
    uint64_t i;
    unsigned found = 0;
    for (i = d->descriptorFirst; i < d->descriptorFirst + d->descriptorCount; ++i) {
        const NrDescriptorV1* v = &p->descriptors[i];
        if (v->kind == kind && v->objectAddress == object && v->objectLifetimeEpoch == life && v->writeEpoch == epoch &&
            v->resourceAddress == resource && v->resourceLifetimeEpoch == resourceLife && v->sourceWriteEpoch == write)
            ++found;
    }
    return found == 1;
}
static inline int nr_validate_postfx(const NrPacketV1* p, const NrPostFxSidecarV1* s, size_t bytes) {
    uint64_t i, j, next = 0, stages, count = 0;
    const NrIdentityV1* x;
    if (!p || !s || bytes != sizeof(*s) || s->size != sizeof(*s) || s->version != 1 ||
        p->header.operation != NR_FRAME_TRANSFER || p->header.drawCount > 16 || p->header.roleCount > 64 ||
        p->header.descriptorCount > 64 || p->header.refCount > 128 || p->header.aliasCount > 64 ||
        p->header.constantsUsedBytes > sizeof(p->constants) || p->header.groupCount < 3 || p->header.groupCount > 8 ||
        s->rowCount > 4 || !nr_postfx_zero(s->reserved, sizeof(s->reserved)))
        return 0;
    x = &p->identity;
    if (s->attempt != p->header.attempt || s->transferId != p->header.transferId ||
        s->frameOrdinal != p->header.frameOrdinal || s->hostSerial != x->hostSerial ||
        s->applicationSubmissionId != x->applicationSubmissionId || s->corpusFrameIndex != x->corpusFrameIndex ||
        s->publicJournalWitness != x->publicJournalWitness || memcmp(s->nonce, x->nonce, sizeof(s->nonce)) ||
        s->pid != x->pid || s->creationFiletime != x->creationFiletime ||
        memcmp(s->sessionNonce, x->sessionNonce, sizeof(s->sessionNonce)) ||
        s->fgContextAddress != x->fgContextAddress || s->fgContextLifetimeEpoch != x->fgContextLifetimeEpoch ||
        s->fgHistoryGeneration != x->fgHistoryGeneration || s->actualNrMode != p->header.actualNrMode)
        return 0;
    stages = (x->settings.upscaling ? NR_POSTFX_EASU : 0u) | (x->settings.sharpening ? NR_POSTFX_CAS : 0u);
    if (s->stageFlags != stages || s->rowCount != 2 * ((stages & 1u) != 0) + 2 * ((stages & 2u) != 0))
        return 0;
    for (i = 0; i < p->header.drawCount; ++i)
        if (p->draws[i].kind == 3 || p->draws[i].kind == 4)
            ++count;
    if (count != s->rowCount)
        return 0;
    for (i = 0; i < 4; ++i) {
        const NrPostFxRowV1* r = &s->rows[i];
        const NrDrawV1* d = 0;
        unsigned matches = 0;
        uint64_t expectedKind = 3;
        if (i >= s->rowCount) {
            if (!nr_postfx_zero(r, sizeof(*r)))
                return 0;
            continue;
        }
        if (!(stages & 1u) || i >= 2)
            expectedKind = 4;
        if (r->kind != expectedKind || r->eye != (i % 2) || r->hostSerial != x->hostSerial ||
            r->stageFlags != (stages | NR_POSTFX_NEURAL_INPUT) || !nr_postfx_zero(r->reserved, sizeof(r->reserved)))
            return 0;
        for (j = 0; j < p->header.drawCount; ++j)
            if (p->draws[j].kind == r->kind && p->draws[j].eye == r->eye) {
                d = &p->draws[j];
                ++matches;
            }
        if (matches != 1 || !d)
            return 0;
        if (d->descriptorFirst > p->header.descriptorCount ||
            d->descriptorCount > p->header.descriptorCount - d->descriptorFirst ||
            d->constantsOffset > p->header.constantsUsedBytes ||
            d->constantsBytes > p->header.constantsUsedBytes - d->constantsOffset ||
            d->shaderHashOffsetInConstants > p->header.constantsUsedBytes ||
            32 > p->header.constantsUsedBytes - d->shaderHashOffsetInConstants)
            return 0;
        if (!r->contextAddress || r->contextAddress != p->groups[2].actualQueueOrContext ||
            r->contextAddress != d->listOrContext || r->bufferAddress != d->rootAddress ||
            r->shaderAddress != d->psoAddress || r->srvAddress != d->rootTable0 || r->uavAddress != d->rootTable1 ||
            !r->constantWriteSequence || r->constantWriteSequence != d->batchEpoch || !r->drawBindEpoch ||
            r->drawBindEpoch != d->bindEpoch || r->dispatchX != d->dispatchX || r->dispatchY != d->dispatchY ||
            r->dispatchZ != d->dispatchZ || r->outputSourceWriteEpoch != d->resourceWriteEpoch)
            return 0;
        if (!nr_postfx_held(p, r->bufferAddress, r->bufferLifetime, NR_BUFFER11) ||
            !nr_postfx_held(p, r->shaderAddress, r->shaderLifetime, NR_SHADER11) ||
            !nr_postfx_held(p, r->srvAddress, r->srvLifetime, NR_VIEW11) ||
            !nr_postfx_held(p, r->uavAddress, r->uavLifetime, NR_VIEW11) || !nr_postfx_input(p, r) ||
            !nr_postfx_held(p, r->outputResourceAddress, r->outputResourceLifetime, NR_TEXTURE11))
            return 0;
        if (r->mapResultBits > UINT32_MAX || (r->mapResultBits & UINT64_C(0x80000000)) || r->mapSubresource ||
            r->mapType != 4 || r->mapFlags || !r->mapEvent ||
            !(r->mapEvent < r->copyEvent && r->copyEvent < r->unmapEvent && r->unmapEvent < r->bindingEvents[1] &&
              r->bindingEvents[1] < r->dispatchEvent) ||
            !r->mapThread || r->mapThread != r->dispatchThread || !r->dispatchQpc)
            return 0;
        for (j = 0; j < (r->kind == 3 ? 5u : 4u); ++j)
            if (!r->bindingEpochs[j] || r->bindingEpochs[j] > r->drawBindEpoch || !r->bindingEvents[j] ||
                r->bindingEvents[j] >= r->dispatchEvent)
                return 0;
        if (r->kind == 4 && (r->bindingEpochs[4] || r->bindingEvents[4] || r->samplerAddress || r->samplerLifetime ||
                             r->samplerWriteEpoch))
            return 0;
        if (r->kind == 3 &&
            (!nr_postfx_held(p, r->samplerAddress, r->samplerLifetime, NR_SAMPLER11) ||
             !nr_postfx_descriptor(
                 p, d, NR_DESC_SAMPLER, r->samplerAddress, r->samplerLifetime, r->samplerWriteEpoch, 0, 0, 0)))
            return 0;
        if (!r->inputSourceWriteEpoch || !r->outputSourceWriteEpoch || !r->srvWriteEpoch || !r->uavWriteEpoch ||
            !nr_postfx_descriptor(p,
                                  d,
                                  NR_DESC_SRV,
                                  r->srvAddress,
                                  r->srvLifetime,
                                  r->srvWriteEpoch,
                                  r->inputResourceAddress,
                                  r->inputResourceLifetime,
                                  r->inputSourceWriteEpoch) ||
            !nr_postfx_descriptor(p,
                                  d,
                                  NR_DESC_UAV,
                                  r->uavAddress,
                                  r->uavLifetime,
                                  r->uavWriteEpoch,
                                  r->outputResourceAddress,
                                  r->outputResourceLifetime,
                                  r->outputSourceWriteEpoch))
            return 0;
        if (r->inputXbits > INT32_MAX || r->inputYbits > INT32_MAX || !r->inputWidth || !r->inputHeight ||
            r->sourceWidth > INT32_MAX || r->sourceHeight > INT32_MAX || r->outputWidth > INT32_MAX ||
            r->outputHeight > INT32_MAX || !r->outputWidth || !r->outputHeight || r->inputXbits > r->sourceWidth ||
            r->inputYbits > r->sourceHeight || r->inputWidth > r->sourceWidth - r->inputXbits ||
            r->inputHeight > r->sourceHeight - r->inputYbits || r->dispatchX != (r->outputWidth + 15) / 16 ||
            r->dispatchY != (r->outputHeight + 15) / 16 || r->dispatchZ != 1)
            return 0;
        if (r->constantsBytes != (r->kind == 3 ? 112u : 80u) || r->constantsBytes != d->constantsBytes ||
            memcmp(r->constants, p->constants + d->constantsOffset, (size_t)r->constantsBytes) ||
            !nr_postfx_zero(r->constants + r->constantsBytes, sizeof(r->constants) - (size_t)r->constantsBytes) ||
            nr_postfx_zero(r->shaderSha256, sizeof(r->shaderSha256)) ||
            memcmp(r->shaderSha256, p->constants + d->shaderHashOffsetInConstants, 32))
            return 0;
        for (j = 0; j < i; ++j)
            if (s->rows[j].constantWriteSequence == r->constantWriteSequence || s->rows[j].copyEvent == r->copyEvent ||
                s->rows[j].dispatchEvent == r->dispatchEvent)
                return 0;
        ++next;
    }
    return next == s->rowCount;
}
#endif
