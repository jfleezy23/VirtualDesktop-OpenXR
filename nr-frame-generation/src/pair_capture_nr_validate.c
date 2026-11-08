#include "pair_capture_nr_control.h"
#include <limits.h>
#include <string.h>
static int fail(NrError* e, uint32_t c, uint64_t i) {
    if (e) {
        e->code = c;
        e->index = (uint32_t)i;
    }
    return 0;
}
static int zero(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; ++i)
        if (b[i])
            return 0;
    return 1;
}
static int epoch(uint64_t n) {
    return n && n != UINT64_MAX;
}
static int range(uint64_t first, uint64_t count, uint64_t limit) {
    return first <= limit && count <= limit - first;
}
static int u32(uint64_t n) {
    return n <= UINT32_MAX;
}
static int hr_ok(uint64_t n) {
    return u32(n) && !(n & UINT64_C(0x80000000));
}
static int finite32(uint64_t n) {
    return u32(n) && (n & UINT64_C(0x7f800000)) != UINT64_C(0x7f800000);
}
static uint64_t role_mask(const NrPacketV1* p) {
    uint64_t m = NR_ROLE_BIT(1) | NR_ROLE_BIT(2) | NR_ROLE_BIT(3) | NR_ROLE_BIT(4) | NR_ROLE_BIT(9) | NR_ROLE_BIT(10) |
                 NR_ROLE_BIT(12) | NR_ROLE_BIT(15) | NR_ROLE_BIT(16);
    if (p->header.actualNrMode == NR_NATIVE100)
        m |= NR_ROLE_BIT(7);
    else
        m |= NR_ROLE_BIT(5) | NR_ROLE_BIT(6) | NR_ROLE_BIT(8) | NR_ROLE_BIT(11);
    if (p->identity.settings.upscaling)
        m |= NR_ROLE_BIT(13);
    if (p->identity.settings.sharpening)
        m |= NR_ROLE_BIT(14);
    return m;
}
static const NrRoleV1* role(const NrPacketV1* p, uint64_t r, uint64_t eye) {
    uint64_t i;
    for (i = 0; i < p->header.roleCount; ++i)
        if (p->roles[i].role == r && p->roles[i].eye == eye)
            return &p->roles[i];
    return NULL;
}
static int has_ref(const NrPacketV1* p, uint64_t a, uint64_t g, uint64_t mask, uint64_t type) {
    uint64_t i;
    for (i = 0; i < p->header.refCount; ++i)
        if (p->refs[i].borrowedIUnknownAddress == a && p->refs[i].groupIndex == g && p->refs[i].interfaceEnum == type &&
            (p->refs[i].roleMask & mask) == mask)
            return 1;
    return 0;
}
static int has_call(const NrPacketV1* p, uint64_t kind, uint64_t fence, uint64_t value) {
    uint64_t i;
    for (i = 0; i < p->header.callCount; ++i) {
        const NrCallV1* c = &p->calls[i];
        if (c->kind == kind && c->fenceAddress == fence && c->signalValue == value && c->endObserved &&
            (c->resultType == NR_VOID || c->classifiedSuccess))
            return 1;
    }
    return 0;
}

static int dispatch_in_module(const NrPacketV1* p, const NrCallV1* c) {
    const NrModuleV1* m;
    if (c->moduleIndex >= p->header.moduleCount)
        return 0;
    m = &p->modules[c->moduleIndex];
    return c->dispatchAddressObserved >= m->baseObserved && c->dispatchAddressObserved - m->baseObserved < m->imageSize;
}
static uint64_t parameter_type(uint64_t key) {
    if (key == 4 || key == 5)
        return NR_U32;
    if (key == 8 || key == 9 || key == 18 || key == 31)
        return NR_RESOURCE_ID;
    if ((key >= 26 && key <= 29) || key == 36 || key == 37)
        return NR_F32;
    if (key >= 38)
        return NR_MATRIX_ABSENT;
    return NR_I32;
}
static int valid_resource_description(const NrDescriptorV1* d) {
    const uint64_t* w = d->canonicalDescriptionWords;
    uint64_t i;
    if (d->kind != NR_DESC_RESOURCE_DESCRIPTION || !d->resourceAddress || d->objectAddress != d->resourceAddress ||
        !epoch(d->resourceLifetimeEpoch) || d->objectLifetimeEpoch != d->resourceLifetimeEpoch ||
        !epoch(d->sourceWriteEpoch) || !epoch(d->writeEpoch) || d->bindEpoch || d->heapAddress || d->heapSlot ||
        d->descriptorIncrement || d->cpuHandle || d->gpuHandle || d->observedMask != 15 ||
        !zero(w + 11, 5 * sizeof(*w)))
        return 0;
    for (i = 0; i < 11; ++i)
        if (!u32(w[i]) && !(d->descriptionKind == NR_D12_RESOURCE_DESCRIPTION && (i == 1 || i == 2)))
            return 0;
    if (d->descriptionKind == NR_D11_TEXTURE2D_DESCRIPTION)
        return d->apiDescriptionBytes == 44 && w[0] && w[0] <= 16384 && w[1] && w[1] <= 16384 && w[2] == 1 && w[3] &&
               w[3] <= 2 && nr_capture_pixel_bytes(w[4]) && w[5] == 1 && !w[6];
    if (d->descriptionKind == NR_D12_RESOURCE_DESCRIPTION)
        return d->apiDescriptionBytes == 56 && w[0] == 3 && w[2] && w[2] <= 16384 && w[3] && w[3] <= 16384 && w[4] &&
               w[4] <= 2 && w[5] == 1 && w[7] == 1 && !w[8] && nr_capture_pixel_bytes(w[6]) && w[9] <= 3 &&
               (w[1] == 0 || w[1] == 4096 || w[1] == 65536 || w[1] == 4194304);
    return 0;
}
static int description_matches_role(const NrDescriptorV1* d, const NrRoleV1* r) {
    const uint64_t* w = d->canonicalDescriptionWords;
    if (d->kind != NR_DESC_RESOURCE_DESCRIPTION) {
        int d12 = r->groupIndex == 1 || r->groupIndex == NR_ABSENT;
        int desc12 =
            d->descriptionKind == NR_D12_SRV || d->descriptionKind == NR_D12_UAV || d->descriptionKind == NR_D12_RTV;
        uint64_t first, count;
        if ((d->kind != NR_DESC_SRV && d->kind != NR_DESC_UAV && d->kind != NR_DESC_RTV) || d12 != desc12 ||
            w[0] != r->viewFormat || w[1] != r->viewDimension ||
            !nr_capture_view_family_compatible(r->format, r->viewFormat))
            return 0;
        if (w[1] == 4)
            return r->depthOrArray == 1;
        first = d->kind == NR_DESC_SRV ? w[5] : w[3];
        count = d->kind == NR_DESC_SRV ? w[6] : w[4];
        return range(first, count, r->depthOrArray) && count && r->selectedArraySlice >= first &&
               r->selectedArraySlice - first < count;
    }
    if (d->descriptionKind == NR_D11_TEXTURE2D_DESCRIPTION)
        return r->groupIndex != 1 && r->groupIndex != NR_ABSENT && w[0] == r->width && w[1] == r->height &&
               w[2] == r->mipLevels && w[3] == r->depthOrArray && w[4] == r->format && w[5] == r->sampleCount &&
               w[6] == r->sampleQuality && w[7] == r->resourceUsage && w[8] == r->bindFlags &&
               w[9] == r->cpuAccessFlags && w[10] == r->miscFlags;
    if (d->descriptionKind == NR_D12_RESOURCE_DESCRIPTION)
        return (r->groupIndex == 1 || r->groupIndex == NR_ABSENT) && w[0] == r->resourceDimension && w[2] == r->width &&
               w[3] == r->height && w[4] == r->depthOrArray && w[5] == r->mipLevels && w[6] == r->format &&
               w[7] == r->sampleCount && w[8] == r->sampleQuality && w[9] == r->layout && w[10] == r->resourceFlags;
    return 0;
}
static int valid_descriptor(const NrDescriptorV1* d) {
    const uint64_t* w = d->canonicalDescriptionWords;
    uint64_t used = 0, size = 0, kind = 0, i;
    if (d->kind == NR_DESC_RESOURCE_DESCRIPTION)
        return valid_resource_description(d);
    switch (d->descriptionKind) {
    case NR_D11_SRV:
        kind = NR_DESC_SRV;
        size = 24;
        break;
    case NR_D12_SRV:
        kind = NR_DESC_SRV;
        size = 40;
        break;
    case NR_D11_UAV:
        kind = NR_DESC_UAV;
        size = 20;
        break;
    case NR_D12_UAV:
        kind = NR_DESC_UAV;
        size = 40;
        break;
    case NR_D11_RTV:
        kind = NR_DESC_RTV;
        size = 20;
        break;
    case NR_D12_RTV:
        kind = NR_DESC_RTV;
        size = 24;
        break;
    case NR_D12_CBV:
        kind = NR_DESC_CBV;
        size = 16;
        break;
    case NR_D11_SAMPLER:
    case NR_D12_SAMPLER:
        kind = NR_DESC_SAMPLER;
        size = 52;
        break;
    default:
        return 0;
    }
    if (d->kind != kind || d->apiDescriptionBytes != size || !d->objectAddress || !epoch(d->objectLifetimeEpoch) ||
        !epoch(d->bindEpoch) || !epoch(d->writeEpoch) || d->observedMask != 15)
        return 0;
    if (kind == NR_DESC_SAMPLER) {
        used = 13;
        if (d->resourceAddress || d->resourceLifetimeEpoch || d->sourceWriteEpoch || w[0] != 21 || w[1] != 3 ||
            w[2] != 3 || w[3] != 3 || !finite32(w[4]) || w[5] > 16 ||
            (w[6] < 1 && !(d->descriptionKind == NR_D11_SAMPLER && w[0] == 21)) || w[6] > 8)
            return 0;
        for (i = 7; i < 13; ++i)
            if (!finite32(w[i]))
                return 0;
    } else {
        if (!d->resourceAddress) {
            // These are real explicit null D12 descriptors, not D11 view objects
            // or evidence that a resource was written.
            if ((d->descriptionKind != NR_D12_SRV && d->descriptionKind != NR_D12_UAV) || d->resourceLifetimeEpoch ||
                d->sourceWriteEpoch)
                return 0;
        } else if (!epoch(d->resourceLifetimeEpoch) || !epoch(d->sourceWriteEpoch))
            return 0;
        if (kind == NR_DESC_CBV) {
            used = 2;
            if (!w[0] || w[0] % 256 || !w[1] || w[1] > 65536 || w[1] % 256)
                return 0;
        } else {
            if ((w[0] != 10 && w[0] != 28 && w[0] != 29 && w[0] != 41 && w[0] != 87 && w[0] != 88 && w[0] != 91 &&
                 w[0] != 93) ||
                (w[1] != 4 && w[1] != 5))
                return 0;
            if (kind == NR_DESC_SRV) {
                used = 13;
                if (w[2] != (d->descriptionKind == NR_D12_SRV ? 5768U : 0U) || w[3] ||
                    (w[4] != 1 && !(d->descriptionKind == NR_D11_SRV && w[4] == UINT32_MAX)) || w[7] ||
                    !finite32(w[8]) || w[9] || w[10] || w[11] || w[12])
                    return 0;
                if (w[1] == 4 && (w[5] || w[6]))
                    return 0;
                if (w[1] == 5 && (!w[6] || w[5] > 1 || w[6] > 2 || w[5] + w[6] > 2))
                    return 0;
            } else {
                used = kind == NR_DESC_UAV ? 11 : 8;
                if (w[2] || w[5] || w[6] || w[7])
                    return 0;
                if (w[1] == 4 && (w[3] || w[4]))
                    return 0;
                if (w[1] == 5 && (!w[4] || w[3] > 1 || w[4] > 2 || w[3] + w[4] > 2))
                    return 0;
                if (kind == NR_DESC_UAV && (w[8] || w[9] || w[10]))
                    return 0;
            }
        }
    }
    return zero(w + used, (size_t)(16 - used) * 8);
}

static uint64_t param_value(const NrPacketV1* p, const NrCallV1* c, uint64_t key) {
    uint64_t i;
    for (i = c->paramFirst; i < c->paramFirst + c->paramCount; ++i)
        if (p->params[i].parameterEnum == key)
            return p->params[i].valueLo;
    return UINT64_MAX;
}
static int eval_rects(const NrPacketV1* p, const NrCallV1* c) {
    const NrRoleV1* colour = role(p, p->header.actualNrMode == NR_NATIVE100 ? 3 : 5, c->eye);
    const NrRoleV1* depth = role(p, p->header.actualNrMode == NR_NATIVE100 ? 4 : 6, c->eye);
    const NrRoleV1* output =
        role(p, p->header.actualNrMode == NR_NATIVE100 ? 7 : 8, p->header.actualNrMode == NR_NATIVE100 ? 2 : c->eye);
    uint64_t w = param_value(p, c, NR_WIDTH), h = param_value(p, c, NR_HEIGHT);
    if (!colour || !depth || !output || w != colour->cropWidth || h != colour->cropHeight || !w || !h)
        return 0;
    if (param_value(p, c, NR_COLOR_X) != colour->cropXbits || param_value(p, c, NR_COLOR_Y) != colour->cropYbits ||
        param_value(p, c, NR_COLOR_WIDTH) != w || param_value(p, c, NR_COLOR_HEIGHT) != h)
        return 0;
    if (!range(param_value(p, c, NR_OUTPUT_X), param_value(p, c, NR_OUTPUT_WIDTH), output->width) ||
        !range(param_value(p, c, NR_OUTPUT_Y), param_value(p, c, NR_OUTPUT_HEIGHT), output->height) ||
        param_value(p, c, NR_OUTPUT_WIDTH) != w || param_value(p, c, NR_OUTPUT_HEIGHT) != h)
        return 0;
    if (!range(param_value(p, c, NR_DEPTH_X), param_value(p, c, NR_DEPTH_WIDTH), depth->width) ||
        !range(param_value(p, c, NR_DEPTH_Y), param_value(p, c, NR_DEPTH_HEIGHT), depth->height) ||
        !param_value(p, c, NR_DEPTH_WIDTH) || !param_value(p, c, NR_DEPTH_HEIGHT))
        return 0;
    if (param_value(p, c, NR_DEPTH_INVERTED) > 1 || param_value(p, c, NR_RESET) > 1 ||
        (p->header.actualNrMode == NR_NATIVE100 && param_value(p, c, NR_RESET)))
        return 0;
    return 1;
}
static int nr_return_alias(const NrPacketV1* p) {
    uint64_t i, j;
    const NrIdentityV1* x = &p->identity;
    for (i = 0; i < p->header.aliasCount; ++i) {
        const NrAliasV1* a = &p->aliases[i];
        if (a->sourceAddress != x->nrFenceAddress || a->sourceDevice != x->nrDeviceAddress ||
            a->targetDevice != x->producerDeviceAddress || a->apiKind != 1)
            continue;
        if (!has_ref(p, a->targetAddress, 1, 128, NR_FENCE11))
            continue;
        for (j = 0; j < p->header.callCount; ++j) {
            const NrCallV1* c = &p->calls[j];
            if (c->kind == NR_WAIT && c->fenceAddress == a->targetAddress &&
                c->signalValue == x->originalNrSignalValue && c->deviceAddress == x->producerDeviceAddress &&
                c->queueAddress == x->producerContextAddress && c->contextAddress == x->producerContextAddress &&
                c->classifiedSuccess && c->endObserved)
                return 1;
        }
    }
    return 0;
}
static int held_type(const NrPacketV1* p, uint64_t address, uint64_t type) {
    uint64_t i;
    int found = 0;
    for (i = 0; i < p->header.refCount; ++i)
        if (p->refs[i].borrowedIUnknownAddress == address) {
            uint64_t observed = p->refs[i].interfaceEnum;
            if (observed != type && observed != NR_IUNKNOWN)
                return 0;
            found |= observed == type;
        }
    return found;
}
static int resource_alias_shape(const NrPacketV1* p, const NrAliasV1* a) {
    const NrCallV1* ex = &p->calls[a->exportCallIndex];
    const NrCallV1* im = &p->calls[a->importCallIndex];
    uint64_t i;
    int sourceKnown = 0, sourceTexture11 = 0, targetKnown = 0, targetTexture11 = 0, targetResource12 = 0;
    const uint64_t preparationResult = ex->actual.actualCountArgument >> 32;
    const uint64_t access = ex->actual.actualCountArgument & UINT32_MAX;
    // For resource witnesses queueAddress is the actual interface this, not a
    // command queue; the old fenceAddress word carries shared-resource identity.
    if (a->sourceDevice != p->identity.producerDeviceAddress || !held_type(p, a->sourceDevice, NR_DEVICE11) ||
        !ex->queueAddress || im->queueAddress != im->deviceAddress || ex->flags != im->flags ||
        !hr_ok(preparationResult) || access != (ex->flags ? UINT64_C(0x10000000) : 0) || im->actual.actualCountArgument)
        return 0;
    for (i = 0; i < p->header.refCount; ++i) {
        const NrRefV1* r = &p->refs[i];
        if (r->borrowedIUnknownAddress == a->sourceAddress) {
            if (r->interfaceEnum != NR_TEXTURE11 && r->interfaceEnum != NR_IUNKNOWN)
                return 0;
            sourceKnown = 1;
            sourceTexture11 |= r->interfaceEnum == NR_TEXTURE11;
        }
        if (r->borrowedIUnknownAddress == a->targetAddress) {
            uint64_t expected = a->apiKind == NR_TEXTURE11_TO_TEXTURE11 ? NR_TEXTURE11 : NR_RESOURCE12;
            if (r->interfaceEnum != expected && r->interfaceEnum != NR_IUNKNOWN)
                return 0;
            targetKnown = 1;
            targetTexture11 |= r->interfaceEnum == NR_TEXTURE11;
            targetResource12 |= r->interfaceEnum == NR_RESOURCE12;
        }
    }
    if (a->apiKind == NR_TEXTURE11_TO_TEXTURE11)
        // The selected actual sharing branch skips all export/import calls when
        // the application and submission devices are the same.
        return a->sourceDevice != a->targetDevice && a->sourceAddress != a->targetAddress &&
               held_type(p, a->targetDevice, NR_DEVICE11) && sourceKnown && sourceTexture11 &&
               (!targetKnown || targetTexture11);
    // The original Texture11 is runtime-leased; the imported NR Resource12 is
    // already held by the audited owner. Do not add a synthetic source AddRef.
    return a->apiKind == NR_TEXTURE11_TO_RESOURCE12 && a->targetDevice == p->identity.nrDeviceAddress &&
           held_type(p, a->targetDevice, NR_DEVICE12) && (!sourceKnown || sourceTexture11) && targetKnown &&
           targetResource12;
}

static int control_request_shape(const NrPacketV1* p) {
    const NrHeaderV1* h = &p->header;
    const NrIdentityV1* x = &p->identity;
    if (h->flags || !epoch(h->attempt) || !zero(&h->frameOrdinal, sizeof(*h) - offsetof(NrHeaderV1, frameOrdinal)) ||
        !(x->nonce[0] | x->nonce[1]) || !x->pid || !x->creationFiletime || !(x->sessionNonce[0] | x->sessionNonce[1]) ||
        !x->fgContextAddress || !epoch(x->fgContextLifetimeEpoch) || !epoch(x->fgHistoryGeneration))
        return 0;
    if (!zero(&x->hostInstanceAddress,
              offsetof(NrIdentityV1, fgContextAddress) - offsetof(NrIdentityV1, hostInstanceAddress)) ||
        !zero(&x->ticketGeneration, sizeof(*x) - offsetof(NrIdentityV1, ticketGeneration)) ||
        !zero(&p->budget, sizeof(*p) - offsetof(NrPacketV1, budget)))
        return 0;
    return 1;
}

static int validate(const NrPacketV1* p, size_t bytes, int final, NrError* e) {
    uint64_t i, j, mask, counts[20] = {0}, eyes[20] = {0}, draws[6] = {0}, sdk[2] = {0}, creates[2] = {0},
                         usedParams[256] = {0}, charged = 0;
    const NrHeaderV1* h;
    const NrIdentityV1* x;
    const NrBudgetV1* b;
    if (e) {
        e->code = 0;
        e->index = 0;
    }
    if (!p || bytes != sizeof(*p) || ((uintptr_t)p % 8))
        return fail(e, NR_BAD_BUFFER, 0);
    h = &p->header;
    x = &p->identity;
    b = &p->budget;
    if (h->magic != NR_MAGIC || h->size != 156160 || h->version != 1)
        return fail(e, NR_BAD_HEADER, 0);
    if (h->operation == NR_NEGOTIATE) {
        if (final || !zero((const unsigned char*)p + 32, sizeof(*p) - 32))
            return fail(e, NR_BAD_OPERATION, 0);
        return 1;
    }
    if (h->operation == NR_QUERY || h->operation == NR_ABORT) {
        if (final || !control_request_shape(p))
            return fail(e, NR_BAD_OPERATION, 0);
        return 1;
    }
    if (h->operation != NR_FRAME_TRANSFER || h->flags != (uint64_t) final)
        return fail(e, NR_BAD_OPERATION, 0);
    if (!epoch(h->attempt) || !epoch(h->transferId) || h->frameOrdinal > 1 || h->frameOutcome != 1 ||
        !epoch(h->packetMutationEpoch) || h->packetFrozen != 1 || h->guardCoverage != 2 ||
        !zero(h->reserved, sizeof h->reserved))
        return fail(e, NR_BAD_HEADER, 0);
    if (h->actualNrMode != NR_NATIVE100 && h->actualNrMode != NR_SPATIAL85)
        return fail(e, NR_BAD_HEADER, 0);
    if (h->roleCount > 64 || h->refCount > 128 || h->groupCount != 3 || h->paramCount > 256 || h->callCount > 160 ||
        h->descriptorCount > 64 || h->drawCount > 16 || h->aliasCount > 64 || h->writeCount > 128 ||
        h->footprintCount > 128 || h->constantsUsedBytes > 4096 || h->moduleCount > 8 ||
        h->sourceObservationCount < 4 || h->sourceObservationCount > 8192)
        return fail(e, NR_BAD_COUNT, 0);
#define UNUSED(member, count)                                                                                          \
    if (!zero(p->member + (count), sizeof(p->member) - sizeof(p->member[0]) * (size_t)(count)))                        \
    return fail(e, NR_NONZERO_UNUSED, 0)
    UNUSED(roles, h->roleCount);
    UNUSED(refs, h->refCount);
    UNUSED(groups, h->groupCount);
    UNUSED(params, h->paramCount);
    UNUSED(calls, h->callCount);
    UNUSED(descriptors, h->descriptorCount);
    UNUSED(draws, h->drawCount);
    UNUSED(aliases, h->aliasCount);
    UNUSED(writes, h->writeCount);
    UNUSED(footprints, h->footprintCount);
    UNUSED(constants, h->constantsUsedBytes);
    UNUSED(modules, h->moduleCount);
#undef UNUSED
    if (!(x->nonce[0] | x->nonce[1]) || !(x->sessionNonce[0] | x->sessionNonce[1]) || !x->pid || !x->creationFiletime ||
        !x->hostInstanceAddress || !epoch(x->hostLifetimeEpoch) || x->ownerHostEpoch != x->hostLifetimeEpoch ||
        !x->sessionHandleObserved || !epoch(x->sessionLifetimeEpoch) || !epoch(x->hostSerial) || !x->displayTimeBits ||
        !epoch(x->sourceOccurrence) || !x->applicationSubmissionId || !x->publicJournalWitness || x->projectionIndex ||
        x->eyeMask != 3 || !x->frameThread || !x->xrEntryQpc || x->producerLeaseQpc <= x->xrEntryQpc ||
        !epoch(x->sourceGeneration) || !epoch(x->sourceWriteEpoch))
        return fail(e, NR_BAD_IDENTITY, 0);
    if (!x->fgContextAddress || !epoch(x->fgContextLifetimeEpoch) || !epoch(x->fgHistoryGeneration) ||
        (!final && (x->ticketGeneration || x->previousSerial || x->currentSerial)) || !x->nrContextAddress ||
        !epoch(x->nrContextLifetimeEpoch) || !x->pipelineAddress || !epoch(x->pipelineLifetimeEpoch) ||
        !epoch(x->pipelineGenerationEpoch) || !epoch(x->batchObjectEpoch) || !epoch(x->descriptorBindEpoch) ||
        !epoch(x->listObjectEpoch) || !epoch(x->allocatorObjectEpoch) || !epoch(x->recordingEpoch) ||
        !x->producerContextAddress || !x->producerDeviceAddress || !x->nrDeviceAddress || !x->nrQueueAddress ||
        !x->nrFenceAddress || !epoch(x->originalNrSignalValue) || !x->privateProducerFenceAddress ||
        !epoch(x->privateProducerValue) || !x->importedAppFenceAddress || !x->importedFgFenceAddress)
        return fail(e, NR_BAD_IDENTITY, 1);
    if (x->actualProcessingPercent != (h->actualNrMode == NR_NATIVE100 ? 100U : 85U) ||
        x->actualCoveragePercent != 100 || x->settingsObservedMask != 65535 || x->pipelineEpochObservedMask != 15 ||
        (x->pipelinePointerPresence & ~UINT64_C(0x7ffff)) || !x->pipelinePointerPresence ||
        !epoch(x->featureLifetimeLeft) || !epoch(x->featureLifetimeRight) || !epoch(x->parameterTableLifetime) ||
        !epoch(x->warmFeatureWitnessEpoch) || x->framePolicyBits || !x->stackCount || x->stackCount > 48 ||
        x->observedIdentityMask != (final ? 255U : 127U) ||
        !zero(x->entryStackAddresses + x->stackCount, (48 - (size_t)x->stackCount) * 8))
        return fail(e, NR_BAD_IDENTITY, 2);
    if (x->settings.enabled != 1 || x->settings.processing != x->actualProcessingPercent ||
        x->settings.coverage != 100 || x->settings.upscaling > 1 || x->settings.sharpening > 1 ||
        x->settings.srgb > 1 || !x->settings.inputWidth || !x->settings.inputHeight || !x->settings.outputWidth ||
        !x->settings.outputHeight)
        return fail(e, NR_BAD_IDENTITY, 3);
    if (b->capBytes != 134217728 || b->cpuMetadataReservation != 8388608 ||
        b->privatePayloadReservation > b->privateAllocationReservation ||
        b->privateAllocationReservation > b->capBytes || b->physicalPrivatePeakBytes > b->capBytes ||
        b->senderRefCountA > 128 || b->senderRefCountB > 128 || b->receiverRefCountA > 128 ||
        b->receiverRefCountB > 128 || b->pairOnlyRoleCount != 5 || b->groupCountA != 3 || b->groupCountB != 3 ||
        b->descriptorCapacity != 64 || b->sourceObservationCapacity != 8192 || b->allArithmeticChecked != 1 ||
        b->preflightComplete != 1 || b->pinHostComplete != 1 || b->pinFgComplete != 1 ||
        !epoch(b->firstExposureSequence) || b->reserved)
        return fail(e, NR_BAD_BUDGET, 0);
    if (!range(b->cpuMetadataReservation, b->bytesA, b->physicalPrivatePeakBytes) ||
        !range(b->cpuMetadataReservation + b->bytesA, b->bytesB, b->physicalPrivatePeakBytes) ||
        !range(b->cpuMetadataReservation + b->bytesA + b->bytesB, b->pairOnlyBytes, b->physicalPrivatePeakBytes) ||
        !range(b->cpuMetadataReservation, b->privateAllocationReservation, b->physicalPrivatePeakBytes))
        return fail(e, NR_BAD_BUDGET, 1);
    for (i = 0; i < h->refCount; ++i) {
        const NrRefV1* r = &p->refs[i];
        int duplicate = 0;
        if (!r->borrowedIUnknownAddress || r->interfaceEnum < 1 || r->interfaceEnum > 19 || !r->roleMask ||
            (r->roleMask & ~UINT64_C(255)) || (r->groupIndex >= 3 && !(final && r->groupIndex == NR_ABSENT)) ||
            !epoch(r->owningSourceLifetimeEpoch) || r->observedNonNull != 1 ||
            (!r->physicalAllocationId && r->chargedPrivateBytes))
            return fail(e, NR_BAD_REF, i);
        if (r->chargedPrivateBytes) {
            for (j = 0; j < i; ++j)
                if (p->refs[j].physicalAllocationId == r->physicalAllocationId) {
                    if (p->refs[j].chargedPrivateBytes != r->chargedPrivateBytes ||
                        p->refs[j].borrowedIUnknownAddress != r->borrowedIUnknownAddress)
                        return fail(e, NR_BAD_REF, i);
                    duplicate = 1;
                }
            if (!duplicate) {
                if (!range(charged, r->chargedPrivateBytes, b->privateAllocationReservation))
                    return fail(e, NR_BAD_BUDGET, i);
                charged += r->chargedPrivateBytes;
            }
        }
    }
    mask = role_mask(p);
    if (final && h->frameOrdinal == 1)
        mask |= NR_ROLE_BIT(17) | NR_ROLE_BIT(18) | NR_ROLE_BIT(19);
    if (h->requiredRoleMaskLo != mask || h->presentRoleMaskLo != mask || h->requiredRoleMaskHi || h->presentRoleMaskHi)
        return fail(e, NR_BAD_ROLE, 0);
    for (i = 0; i < h->roleCount; ++i) {
        const NrRoleV1* r = &p->roles[i];
        const NrRefV1 *s, *t;
        const uint64_t role = r->role;
        const unsigned roleIndex = (unsigned)role;
        if (roleIndex < 1 || roleIndex > 19 || role != roleIndex)
            return fail(e, NR_BAD_ROLE, i);
        uint64_t group = r->role <= 2 ? 0 : r->role <= 12 ? 1 : r->role <= 16 ? 2 : NR_ABSENT;
        int packed = r->role == 7 || r->role == 12 || r->role == 16 || r->role >= 17;
        int resourceOnly = r->descriptorCount == 1 && r->descriptorFirst < h->descriptorCount &&
                           p->descriptors[r->descriptorFirst].kind == NR_DESC_RESOURCE_DESCRIPTION;
        if (r->role < 1 || r->role > 19 || !(mask & NR_ROLE_BIT(r->role)) || r->eye > 2 || (!packed && r->eye > 1) ||
            (packed && r->eye != 2) || r->groupIndex != group || r->reserved || r->observedMask != 15)
            return fail(e, NR_BAD_ROLE, i);
        ++counts[roleIndex];
        if (r->role >= 17) {
            uint64_t bit;
            if ((r->role < 19 && r->phase > 1) || (r->role == 19 && r->phase != 1))
                return fail(e, NR_BAD_ROLE, i);
            bit = UINT64_C(1) << r->phase;
#if defined(_MSC_VER) && defined(_PREFAST_)
            /* The role bound above still holds after the phase checks. */
            __analysis_assume(roleIndex < 20);
#endif
            if (eyes[roleIndex] & bit)
                return fail(e, NR_BAD_ROLE, i);
            eyes[roleIndex] |= bit;
        }
        if (r->role < 17) {
            if (eyes[roleIndex] & (UINT64_C(1) << r->eye))
                return fail(e, NR_BAD_ROLE, i);
            eyes[roleIndex] |= UINT64_C(1) << r->eye;
        }
        if (r->sourceRefIndex >= h->refCount || r->snapshotRefIndex >= h->refCount)
            return fail(e, NR_BAD_RANGE, i);
        s = &p->refs[r->sourceRefIndex];
        t = &p->refs[r->snapshotRefIndex];
        if (s->borrowedIUnknownAddress != r->sourceAddressObserved ||
            t->borrowedIUnknownAddress != r->snapshotAddressObserved || s->groupIndex != group ||
            t->groupIndex != group ||
            s->interfaceEnum != ((group == 1 || group == NR_ABSENT) ? NR_RESOURCE12 : NR_TEXTURE11) ||
            t->interfaceEnum != s->interfaceEnum || !(s->roleMask & 1) || !(t->roleMask & 2) ||
            s->owningSourceLifetimeEpoch != r->sourceLifetimeEpoch || !epoch(r->sourceWriteEpoch) ||
            !epoch(r->snapshotWriteEpoch) || !epoch(r->descriptorEpoch) || !epoch(r->sourceOccurrence) ||
            !epoch(r->hostSerial) || !r->copyListAddress || r->chargedBytes != t->chargedPrivateBytes ||
            r->payloadBytes > r->chargedBytes)
            return fail(e, NR_BAD_ROLE, i);
        if (r->role < 17 &&
            (r->sourceOccurrence != x->sourceOccurrence || r->hostSerial != x->hostSerial || r->projectionIndex))
            return fail(e, NR_BAD_ROLE, i);
        if (r->layout || r->resourceDimension != 3 || !r->width || r->width > 16384 || !r->height ||
            r->height > 16384 || !r->depthOrArray || r->depthOrArray > 2 || r->mipLevels != 1 || r->sampleCount != 1 ||
            r->sampleQuality || r->selectedMip || r->selectedArraySlice >= r->depthOrArray || r->firstSubresource ||
            r->subresourceCount != r->depthOrArray || !nr_capture_pixel_bytes(r->format) ||
            (resourceOnly ? (r->viewDimension || r->viewFormat)
                          : ((r->viewDimension != 4 && r->viewDimension != 5) || !r->viewFormat)) ||
            !r->rectWidth || !r->rectHeight || !range(r->rectXbits, r->rectWidth, r->width) ||
            !range(r->rectYbits, r->rectHeight, r->height) || !range(r->cropXbits, r->cropWidth, r->width) ||
            !range(r->cropYbits, r->cropHeight, r->height) ||
            r->copyMethod != ((group == 1 || group == NR_ABSENT) ? 2U : 1U) || !u32(r->copyBeforeState) ||
            !u32(r->copyAfterState))
            return fail(e, NR_BAD_ROLE, i);
        if (!r->descriptorCount || !range(r->descriptorFirst, r->descriptorCount, h->descriptorCount) ||
            r->footprintCount != r->subresourceCount ||
            !range(r->footprintFirst, r->footprintCount, h->footprintCount) ||
            (r->aliasIndexOrAbsent != NR_ABSENT && r->aliasIndexOrAbsent >= h->aliasCount) ||
            (r->callIndexOrAbsent != NR_ABSENT && r->callIndexOrAbsent >= h->callCount))
            return fail(e, NR_BAD_RANGE, i);
    }
    for (i = 1; i <= 19; ++i)
        if (mask & NR_ROLE_BIT(i)) {
            uint64_t want = (i == 7 || i == 12 || i == 16 || i == 19) ? 1 : 2;
            if (counts[i] != want)
                return fail(e, NR_BAD_ROLE, i);
        }
    for (i = 0; i < h->roleCount; ++i) {
        const NrRoleV1* r = &p->roles[i];
        int described = 0;
        for (j = r->descriptorFirst; j < r->descriptorFirst + r->descriptorCount; ++j) {
            const NrDescriptorV1* d = &p->descriptors[j];
            if (d->kind == NR_DESC_RESOURCE_DESCRIPTION &&
                (r->descriptorCount != 1 || r->viewDimension || r->viewFormat))
                return fail(e, NR_BAD_DESCRIPTOR, i);
            if (d->resourceAddress == r->sourceAddressObserved) {
                if (d->resourceLifetimeEpoch != r->sourceLifetimeEpoch || d->sourceWriteEpoch != r->sourceWriteEpoch)
                    return fail(e, NR_BAD_DESCRIPTOR, i);
                if (d->writeEpoch == r->descriptorEpoch && description_matches_role(d, r))
                    described = 1;
            }
        }
        if (!described)
            return fail(e, NR_BAD_DESCRIPTOR, i);
        for (j = 0; j < r->footprintCount; ++j)
            if (p->footprints[r->footprintFirst + j].roleIndex != i ||
                p->footprints[r->footprintFirst + j].subresourceIndex != j)
                return fail(e, NR_BAD_FOOTPRINT, i);
    }
    for (i = 0; i < 3; ++i) {
        const NrGroupV1* g = &p->groups[i];
        uint64_t need = i == 1 ? 127 : 79;
        uint64_t roles = 0;
        for (j = 0; j < h->refCount; ++j)
            if (p->refs[j].groupIndex == i)
                roles |= p->refs[j].roleMask;
        if (g->kind != i || !g->senderOwnerNumericId || g->senderAttempt != h->attempt ||
            g->frameOrdinal != h->frameOrdinal || g->senderSlot != i || !epoch(g->senderGeneration) ||
            !epoch(g->transferId) || g->sourceOccurrence != x->sourceOccurrence || !epoch(g->sourceGeneration) ||
            !epoch(g->descriptorEpoch) || (roles & need) != need ||
            g->nrContextLifetimeEpoch != x->nrContextLifetimeEpoch || g->originalSignalObserved != 1 ||
            g->coveringSignalObserved != 1 || g->sourceReturnWaitObserved != 1 ||
            !hr_ok(g->sourceReturnWaitResultBits) || !hr_ok(g->originalSignalResultBits) ||
            !hr_ok(g->coveringSignalResultBits) || !epoch(g->expectedSignalValue) ||
            g->originalSignalValue != g->expectedSignalValue || g->coveringSignalValue != g->expectedSignalValue ||
            (final && g->completedObserved != 1) || g->completedObserved > 1 ||
            (!g->completedObserved && g->completedValue) ||
            (g->completedObserved && (!epoch(g->completedValue) || g->completedValue < g->coveringSignalValue)))
            return fail(e, NR_BAD_GROUP, i);
        for (j = 0; j < i; ++j)
            if (g->transferId == p->groups[j].transferId)
                return fail(e, NR_BAD_GROUP, i);
        if (i == 1) {
            if (g->deviceAddress != x->nrDeviceAddress || g->actualQueueOrContext != x->nrQueueAddress ||
                g->actualFence != x->nrFenceAddress || g->expectedSignalValue != x->originalNrSignalValue ||
                !g->actualList || !g->actualAllocator || g->recordingEpoch != x->recordingEpoch ||
                g->closeObserved != 1 || !hr_ok(g->closeResultBits) || g->executeObservationCount != 1 ||
                g->qualificationMask != 31)
                return fail(e, NR_BAD_GROUP, i);
        } else if (g->deviceAddress != x->producerDeviceAddress ||
                   g->actualQueueOrContext != x->producerContextAddress ||
                   g->actualFence != x->privateProducerFenceAddress ||
                   g->expectedSignalValue != x->privateProducerValue || g->actualList || g->actualAllocator ||
                   g->recordingEpoch || g->closeObserved || g->closeResultBits || g->executeObservationCount ||
                   g->qualificationMask != 28)
            return fail(e, NR_BAD_GROUP, i);
        if (!has_ref(p, g->deviceAddress, i, 4, i == 1 ? NR_DEVICE12 : NR_DEVICE11) ||
            !has_ref(p, g->actualQueueOrContext, i, 8, i == 1 ? NR_QUEUE12 : NR_CONTEXT11) ||
            !has_ref(p, g->actualFence, i, 64, i == 1 ? NR_FENCE12 : NR_FENCE11) ||
            (i == 1 &&
             (!has_ref(p, g->actualList, i, 16, NR_LIST12) || !has_ref(p, g->actualAllocator, i, 32, NR_ALLOCATOR12))))
            return fail(e, NR_BAD_GROUP, i);
    }
    for (i = 0; i < h->moduleCount; ++i) {
        const NrModuleV1* m = &p->modules[i];
        if (m->moduleKind < 1 || m->moduleKind > 8 || !m->baseObserved || !m->imageSize || !m->fileSize ||
            m->addressRva >= m->imageSize || !epoch(m->loadLifetimeEpoch) || zero(m->sha256Words, 32) ||
            m->pinObserved > 1 || m->pinResult > 1 || m->observedMask != 7 ||
            (m->moduleKind <= 2 && (!m->pinObserved || !m->pinResult)))
            return fail(e, NR_BAD_MODULE, i);
        for (j = 0; j < i; ++j)
            if (m->moduleKind == p->modules[j].moduleKind)
                return fail(e, NR_BAD_MODULE, i);
    }
    if (h->moduleCount < 3 || p->modules[0].moduleKind != 1 || p->modules[1].moduleKind != 2 ||
        p->modules[2].moduleKind != 3)
        return fail(e, NR_BAD_MODULE, 0);
    for (i = 0; i < h->callCount; ++i) {
        const NrCallV1* c = &p->calls[i];
        if (c->kind < 1 || c->kind > 19 || c->beginObserved != 1 || c->endObserved != 1 || c->resultType > 3 ||
            !u32(c->resultBits) || c->classifiedSuccess > 1 || !c->qpcBegin || c->qpcEnd < c->qpcBegin || !c->thread ||
            c->eye > 2 || c->moduleIndex >= h->moduleCount || c->observedMask != 7 || (c->flags & ~UINT64_C(1)))
            return fail(e, NR_BAD_CALL, i);
        if ((c->kind == NR_SIGNAL || c->kind == NR_WAIT || c->kind == NR_EXPORT_FENCE || c->kind == NR_IMPORT_FENCE) &&
            c->resultType != NR_HRESULT)
            return fail(e, NR_BAD_CALL, i);
        if ((c->resultType == NR_VOID && (c->resultBits || c->classifiedSuccess)) ||
            (c->resultType == NR_HRESULT && c->classifiedSuccess != (uint64_t)hr_ok(c->resultBits)) ||
            (c->resultType == NR_NGX &&
             c->classifiedSuccess != (uint64_t)(nr_ngx_not_failed((uint32_t)c->resultBits))) ||
            (c->resultType == NR_BOOLEAN && (c->resultBits > 1 || c->classifiedSuccess != c->resultBits)))
            return fail(e, NR_BAD_CALL, i);

        if (c->kind == NR_CLOSE || c->kind == NR_EXECUTE ||
            (c->kind == NR_SIGNAL && c->fenceAddress == x->nrFenceAddress)) {
            const NrGroupV1* g = &p->groups[1];
            if (c->deviceAddress != x->nrDeviceAddress || c->queueAddress != g->actualQueueOrContext ||
                c->fenceAddress != g->actualFence || c->listAddress != g->actualList ||
                c->allocatorAddress != g->actualAllocator || c->recordingEpoch != g->recordingEpoch ||
                c->contextAddress != x->nrContextAddress || c->contextLifetimeEpoch != x->nrContextLifetimeEpoch ||
                (c->kind == NR_EXECUTE && (c->resultType != NR_VOID || c->actual.actualCountArgument != 1)) ||
                (c->kind == NR_CLOSE && (c->resultType != NR_HRESULT || c->resultBits != g->closeResultBits)))
                return fail(e, NR_BAD_CALL, i);
        }
        if (c->kind == NR_SIGNAL && c->fenceAddress == x->privateProducerFenceAddress &&
            (c->queueAddress != x->producerContextAddress || c->deviceAddress != x->producerDeviceAddress ||
             c->listAddress || c->allocatorAddress || c->recordingEpoch))
            return fail(e, NR_BAD_CALL, i);
        if (c->kind == NR_CREATE_SDK || c->kind == NR_CREATE_MANUAL || c->kind == NR_EVALUATE) {
            uint64_t seen = 0;
            if (c->eye > 1 || c->resultType != NR_NGX || !c->parameterTableAddressObserved ||
                c->parameterTableLifetime != x->parameterTableLifetime || !c->dispatchAddressObserved ||
                !epoch(c->dispatchTableEpoch) || !dispatch_in_module(p, c) || !c->listAddress ||
                !epoch(c->recordingEpoch) || c->contextAddress != x->nrContextAddress ||
                c->contextLifetimeEpoch != x->nrContextLifetimeEpoch || c->paramCount != 39 ||
                !range(c->paramFirst, c->paramCount, h->paramCount))
                return fail(e, NR_BAD_CALL, i);
            for (j = c->paramFirst; j < c->paramFirst + c->paramCount; ++j) {
                const NrParamV1* q = &p->params[j];
                uint64_t key = q->parameterEnum;
                if (usedParams[j]++ || q->callIndex != i || key < 1 || key > 39 || (seen & (UINT64_C(1) << key)) ||
                    q->tableLifetime != x->parameterTableLifetime || q->observationStatus > 3 ||
                    q->typeEnum != parameter_type(key))
                    return fail(e, NR_BAD_PARAM, j);
                seen |= UINT64_C(1) << key;
                if (key > 30) {
                    if (q->observationStatus != NR_NOT_HOST_WRITTEN || q->valueLo || q->valueHi || q->writeEpoch)
                        return fail(e, NR_BAD_PARAM, j);
                    continue;
                }
                if (c->kind == NR_EVALUATE) {
                    uint64_t expected = 0;
                    int checked = 1;
                    switch (key) {
                    case NR_PRESET:
                        expected = 2;
                        break;
                    case NR_ENABLED:
                    case NR_AUTO_MASK:
                    case NR_CREATION_NODE:
                    case NR_VISIBILITY_NODE:
                        expected = 1;
                        break;
                    case NR_UI:
                        expected = 0;
                        break;
                    case NR_STYLE:
                        expected = x->settings.style;
                        break;
                    case NR_INTENSITY:
                        expected = x->settings.intensity;
                        break;
                    case NR_TONE:
                        expected = x->settings.tone;
                        break;
                    case NR_STRUCTURE:
                        expected = x->settings.structure;
                        break;
                    case NR_SKIN:
                        expected = x->settings.skin;
                        break;
                    default:
                        checked = 0;
                        break;
                    }
                    if (checked && q->valueLo != expected)
                        return fail(e, NR_BAD_PARAM, j);
                }
                if (q->observationStatus == NR_OBSERVED_VALUE) {
                    if (!epoch(q->writeEpoch))
                        return fail(e, NR_BAD_PARAM, j);
                    if (key == 8 || key == 9 || key == 18) {
                        if (q->typeEnum != NR_RESOURCE_ID || !q->valueLo || !epoch(q->valueHi))
                            return fail(e, NR_BAD_PARAM, j);
                        if (c->kind == NR_EVALUATE) {
                            uint64_t wantRole = key == 8   ? (h->actualNrMode == NR_NATIVE100 ? 3 : 5)
                                                : key == 9 ? (h->actualNrMode == NR_NATIVE100 ? 7 : 8)
                                                           : (h->actualNrMode == NR_NATIVE100 ? 4 : 6);
                            const NrRoleV1* rr = role(p, wantRole, wantRole == 7 ? 2 : c->eye);
                            if (!rr || rr->sourceAddressObserved != q->valueLo || rr->sourceLifetimeEpoch != q->valueHi)
                                return fail(e, NR_BAD_PARAM, j);
                        }
                    } else if (key >= 26 && key <= 29) {
                        if (q->typeEnum != NR_F32 || !finite32(q->valueLo) || q->valueHi)
                            return fail(e, NR_BAD_PARAM, j);
                    } else if (q->typeEnum != parameter_type(key) || !u32(q->valueLo) || q->valueHi)
                        return fail(e, NR_BAD_PARAM, j);
                } else if (q->observationStatus == NR_OBSERVED_NULL) {
                    // An executed null-resource Set still has a real write epoch.
                    if (q->typeEnum != NR_RESOURCE_ID || (key != NR_COLOR && key != NR_OUTPUT && key != NR_DEPTH) ||
                        q->valueLo || q->valueHi || !epoch(q->writeEpoch) || c->kind == NR_EVALUATE)
                        return fail(e, NR_BAD_PARAM, j);
                } else if (q->valueLo || q->valueHi || q->writeEpoch || c->kind == NR_EVALUATE)
                    return fail(e, NR_BAD_PARAM, j);
            }
            if (c->classifiedSuccess &&
                (c->resultBits != 1 || !c->featureAddressObserved ||
                 c->featureLifetimeEpoch != (c->eye ? x->featureLifetimeRight : x->featureLifetimeLeft)))
                return fail(e, NR_BAD_CALL, i);
            if (c->kind == NR_EVALUATE) {
                if (!eval_rects(p, c) || !c->classifiedSuccess || c->resultBits != 1 ||
                    c->recordingEpoch != x->recordingEpoch || c->listAddress != p->groups[1].actualList ||
                    c->allocatorAddress != p->groups[1].actualAllocator || c->queueAddress != x->nrQueueAddress ||
                    c->fenceAddress != x->nrFenceAddress)
                    return fail(e, NR_BAD_CALL, i);
                ++sdk[c->eye];
            } else if (c->classifiedSuccess) {
                if (!c->actual.pairedEvaluateAddress || !c->aux.pairedReleaseAddress)
                    return fail(e, NR_BAD_CALL, i);
                ++creates[c->eye];
            }
        } else if (c->paramFirst || c->paramCount)
            return fail(e, NR_BAD_PARAM, i);
    }

    for (i = 0; i < h->callCount; ++i)
        if (p->calls[i].kind == NR_EVALUATE) {
            const NrCallV1* c = &p->calls[i];
            int paired = 0;
            for (j = 0; j < h->callCount; ++j) {
                const NrCallV1* create = &p->calls[j];
                if ((create->kind == NR_CREATE_SDK || create->kind == NR_CREATE_MANUAL) && create->classifiedSuccess &&
                    create->eye == c->eye && create->featureAddressObserved == c->featureAddressObserved &&
                    create->featureLifetimeEpoch == c->featureLifetimeEpoch &&
                    create->dispatchTableEpoch == c->dispatchTableEpoch &&
                    create->actual.pairedEvaluateAddress == c->dispatchAddressObserved &&
                    create->parameterTableAddressObserved == c->parameterTableAddressObserved)
                    paired = 1;
            }
            if (!paired)
                return fail(e, NR_BAD_CALL, i);
        }
    for (i = 0; i < h->paramCount; ++i)
        if (usedParams[i] != 1)
            return fail(e, NR_BAD_PARAM, i);
    if (sdk[0] != 1 || sdk[1] != 1 || creates[0] != 1 || creates[1] != 1)
        return fail(e, NR_BAD_CALL, 0);
    if (!has_call(p, NR_CLOSE, x->nrFenceAddress, 0) || !has_call(p, NR_EXECUTE, x->nrFenceAddress, 0) ||
        !has_call(p, NR_SIGNAL, x->nrFenceAddress, x->originalNrSignalValue) ||
        !has_call(p, NR_SIGNAL, x->privateProducerFenceAddress, x->privateProducerValue) ||
        !has_call(p, NR_WAIT, x->importedAppFenceAddress, x->privateProducerValue) || !has_call(p, NR_FLUSH, 0, 0) ||
        !nr_return_alias(p))
        return fail(e, NR_BAD_CALL, 0);
    for (i = 0; i < h->descriptorCount; ++i)
        if (!valid_descriptor(&p->descriptors[i]))
            return fail(e, NR_BAD_DESCRIPTOR, i);
    for (i = 0; i < h->drawCount; ++i) {
        const NrDrawV1* d = &p->draws[i];
        // FG has 248 bytes of B0 root constants and a 256-byte B1 slice;
        // its payload is not the NR resolve shader's 80-byte constants.
        uint64_t expected = d->kind == 3 ? 112 : d->kind == 5 ? 504 : 80;
        if (d->kind < 1 || d->kind > 5 || d->eye > 1 || !d->listOrContext || !d->psoAddress || !d->rootAddress ||
            d->shaderModuleIndex >= h->moduleCount ||
            !range(d->shaderHashOffsetInConstants, 32, h->constantsUsedBytes) ||
            !range(d->constantsOffset, d->constantsBytes, h->constantsUsedBytes) || d->constantsBytes != expected ||
            !d->descriptorCount || !range(d->descriptorFirst, d->descriptorCount, h->descriptorCount) ||
            !d->rootTable0 || d->observedMask != 15 || !zero(d->reserved, sizeof d->reserved))
            return fail(e, NR_BAD_DRAW, i);
        if (draws[d->kind] & (UINT64_C(1) << d->eye))
            return fail(e, NR_BAD_DRAW, i);
        for (j = d->descriptorFirst; j < d->descriptorFirst + d->descriptorCount; ++j)
            if (p->descriptors[j].kind == NR_DESC_RESOURCE_DESCRIPTION)
                return fail(e, NR_BAD_DRAW, i);
        draws[d->kind] |= UINT64_C(1) << d->eye;
    }
    if (draws[1] != (h->actualNrMode == NR_SPATIAL85 ? 3U : 0U) || draws[2] != 3 ||
        draws[3] != (x->settings.upscaling ? 3U : 0U) || draws[4] != (x->settings.sharpening ? 3U : 0U) ||
        draws[5] != ((final && h->frameOrdinal == 1) ? 3U : 0U))
        return fail(e, NR_BAD_DRAW, 0);
    for (i = 0; i < h->aliasCount; ++i) {
        const NrAliasV1* a = &p->aliases[i];
        if (!a->sourceAddress || !a->targetAddress || !epoch(a->sourceLifetime) || !epoch(a->targetLifetime) ||
            !a->sourceDevice || !a->targetDevice || !a->exportedHandleObserved || a->exportCallIndex >= h->callCount ||
            a->importCallIndex >= h->callCount || p->calls[a->exportCallIndex].kind != NR_EXPORT_FENCE ||
            p->calls[a->importCallIndex].kind != NR_IMPORT_FENCE || !p->calls[a->exportCallIndex].classifiedSuccess ||
            !p->calls[a->importCallIndex].classifiedSuccess || a->apiKind < 1 ||
            a->apiKind > NR_TEXTURE11_TO_RESOURCE12 || a->observedMask != 3 || a->reserved)
            return fail(e, NR_BAD_ALIAS, i);
    }

    for (i = 0; i < h->aliasCount; ++i) {
        const NrAliasV1* a = &p->aliases[i];
        if (p->calls[a->exportCallIndex].deviceAddress != a->sourceDevice ||
            p->calls[a->exportCallIndex].fenceAddress != a->sourceAddress ||
            p->calls[a->importCallIndex].deviceAddress != a->targetDevice ||
            p->calls[a->importCallIndex].fenceAddress != a->targetAddress)
            return fail(e, NR_BAD_ALIAS, i);
        if (a->apiKind >= NR_TEXTURE11_TO_TEXTURE11 && !resource_alias_shape(p, a))
            return fail(e, NR_BAD_ALIAS, i);
        if (p->calls[a->exportCallIndex].aux.auxiliaryValue != a->exportedHandleObserved ||
            p->calls[a->importCallIndex].aux.auxiliaryValue != a->exportedHandleObserved)
            return fail(e, NR_BAD_ALIAS, i);
        for (j = 0; j < h->refCount; ++j) {
            const NrRefV1* r = &p->refs[j];
            if ((r->borrowedIUnknownAddress == a->sourceAddress && r->owningSourceLifetimeEpoch != a->sourceLifetime) ||
                (r->borrowedIUnknownAddress == a->targetAddress && r->owningSourceLifetimeEpoch != a->targetLifetime))
                return fail(e, NR_BAD_ALIAS, i);
        }
    }
    uint64_t coveredRoles = 0;
    for (i = 0; i < h->writeCount; ++i) {
        const NrWriteV1* w = &p->writes[i];
        const NrRoleV1* r;
        if (!w->resourceAddress || !epoch(w->resourceLifetime) || !epoch(w->writeEpoch) ||
            !epoch(w->sourceOccurrence) || !w->listOrContextAddress || w->callOrRoleIndex >= h->roleCount)
            return fail(e, NR_BAD_WRITE, i);
        // In this table callOrRoleIndex names the role association. Actual call
        // provenance is separate; a writer need not be the diagnostic copier.
        r = &p->roles[w->callOrRoleIndex];
        if (r->sourceAddressObserved != w->resourceAddress || r->sourceWriteEpoch != w->writeEpoch ||
            r->sourceLifetimeEpoch != w->resourceLifetime || r->sourceOccurrence != w->sourceOccurrence)
            return fail(e, NR_BAD_WRITE, i);
        coveredRoles |= UINT64_C(1) << w->callOrRoleIndex;
    }
    for (i = 0; i < h->roleCount; ++i)
        if (!(coveredRoles & (UINT64_C(1) << i)))
            return fail(e, NR_BAD_WRITE, i);
    for (i = 0; i < h->footprintCount; ++i) {
        const NrFootprintV1* f = &p->footprints[i];
        const NrRoleV1* r;
        uint64_t n;
        if (f->roleIndex >= h->roleCount)
            return fail(e, NR_BAD_FOOTPRINT, i);
        r = &p->roles[f->roleIndex];
        if (i < r->footprintFirst || i - r->footprintFirst >= r->footprintCount ||
            f->subresourceIndex >= r->subresourceCount || !f->rowPitch || (r->copyMethod == 2 && f->rowPitch % 256) ||
            f->rowBytes != r->width * nr_capture_pixel_bytes(r->format) || f->rowBytes > f->rowPitch ||
            (r->copyMethod == 1 && f->rowPitch > r->mapPitchBound) || f->rowCount != r->height || f->depth != 1 ||
            f->rowPitch > UINT64_MAX / f->rowCount)
            return fail(e, NR_BAD_FOOTPRINT, i);
        n = f->rowPitch * f->rowCount;
        if (f->totalBytes != n || !range(f->offset, n, r->payloadBytes))
            return fail(e, NR_BAD_FOOTPRINT, i);
        for (j = 0; j < i; ++j) {
            const NrFootprintV1* other = &p->footprints[j];
            if (other->roleIndex == f->roleIndex && f->offset < other->offset + other->totalBytes &&
                other->offset < f->offset + f->totalBytes)
                return fail(e, NR_BAD_FOOTPRINT, i);
        }
    }
    return 1;
}
int nr_validate_packet(const NrPacketV1* p, size_t n, NrError* e) {
    return validate(p, n, 0, e);
}
int nr_validate_ack(const NrPacketV1* p, const NrAckV1* a, size_t n, const uint64_t digest[2], NrError* e) {
    if (!a || n != sizeof(*a) || !digest || !nr_validate_packet(p, sizeof(*p), e) ||
        p->header.operation != NR_FRAME_TRANSFER)
        return fail(e, NR_BAD_ACK, 0);
    if (a->size != 128 || a->version != 1 || a->status != NR_DEEP_OWNED || a->reasonEnum ||
        a->requestAttempt != p->header.attempt || a->frameOrdinal != p->header.frameOrdinal ||
        a->transferId != p->header.transferId || !epoch(a->receiverCommitEpoch) ||
        a->roleCount != p->header.roleCount || a->refCount != p->header.refCount ||
        a->groupCount != p->header.groupCount || a->packetDigestLo != digest[0] || a->packetDigestHi != digest[1] ||
        a->acceptedBudgetBytes != p->budget.physicalPrivatePeakBytes || a->observedFlags != 1 || a->reserved)
        return fail(e, NR_BAD_ACK, 0);
    return 1;
}
int nr_validate_control_ack(
    const NrPacketV1* request, uint32_t returned_status, const NrAckV1* ack, size_t ack_bytes, NrError* e) {
    if (!ack || ack_bytes != sizeof(*ack) || ((uintptr_t)ack % 8) ||
        !nr_validate_packet(request, sizeof(*request), e) ||
        (request->header.operation != NR_QUERY && request->header.operation != NR_ABORT))
        return fail(e, NR_BAD_ACK, 0);
    if (ack->size != sizeof(*ack) || ack->version != 1 || ack->status != returned_status || ack->reasonEnum ||
        ack->requestAttempt != request->header.attempt || ack->observedFlags != 1 || ack->reserved ||
        !zero(&ack->frameOrdinal, offsetof(NrAckV1, observedFlags) - offsetof(NrAckV1, frameOrdinal)))
        return fail(e, NR_BAD_ACK, 0);
    if (request->header.operation == NR_ABORT) {
        if (ack->status != NR_REJECTED && ack->status != NR_UNCERTAIN)
            return fail(e, NR_BAD_ACK, 0);
    } else if (ack->status != NR_REJECTED && ack->status != NR_DEEP_OWNED && ack->status != NR_UNCERTAIN &&
               ack->status != NR_SETTLED)
        return fail(e, NR_BAD_ACK, 0);
    return 1;
}

int nr_validate_final_pair(const NrPacketV1* a, const NrPacketV1* b, NrError* e) {
    uint64_t i;
    if (!validate(a, sizeof(*a), 1, e) || !validate(b, sizeof(*b), 1, e))
        return 0;
    if (a->header.operation != NR_FRAME_TRANSFER || a->header.frameOrdinal != 0 || b->header.frameOrdinal != 1 ||
        a->header.transferId == b->header.transferId ||
        a->identity.ticketGeneration != a->identity.fgHistoryGeneration ||
        a->identity.currentSerial != a->identity.hostSerial ||
        a->identity.previousSerial >= a->identity.currentSerial || a->identity.hostSerial == UINT64_MAX ||
        b->identity.hostSerial != a->identity.hostSerial + 1 || b->identity.previousSerial != a->identity.hostSerial ||
        b->identity.currentSerial != b->identity.hostSerial ||
        b->identity.ticketGeneration != b->identity.fgHistoryGeneration ||
        a->identity.fgHistoryGeneration != b->identity.fgHistoryGeneration ||
        a->identity.fgContextAddress != b->identity.fgContextAddress ||
        a->identity.fgContextLifetimeEpoch != b->identity.fgContextLifetimeEpoch ||
        a->identity.nrContextAddress != b->identity.nrContextAddress ||
        a->identity.nrContextLifetimeEpoch != b->identity.nrContextLifetimeEpoch ||
        a->identity.pid != b->identity.pid || a->identity.creationFiletime != b->identity.creationFiletime ||
        a->identity.hostInstanceAddress != b->identity.hostInstanceAddress ||
        a->identity.hostLifetimeEpoch != b->identity.hostLifetimeEpoch ||
        a->identity.ownerHostEpoch != b->identity.ownerHostEpoch ||
        a->identity.sessionHandleObserved != b->identity.sessionHandleObserved ||
        a->identity.sessionLifetimeEpoch != b->identity.sessionLifetimeEpoch ||
        memcmp(a->identity.sessionNonce, b->identity.sessionNonce, 16) ||
        a->identity.pipelineLifetimeEpoch != b->identity.pipelineLifetimeEpoch ||
        a->identity.featureLifetimeLeft != b->identity.featureLifetimeLeft ||
        a->identity.featureLifetimeRight != b->identity.featureLifetimeRight ||
        a->header.attempt != b->header.attempt || a->header.actualNrMode != b->header.actualNrMode ||
        memcmp(a->identity.nonce, b->identity.nonce, 16) ||
        memcmp(&a->identity.settings, &b->identity.settings, sizeof(NrRawSettingsV1)) ||
        !has_call(b, NR_ADMISSION, 0, 0) || !has_call(b, NR_RETIRE, 0, 0))
        return fail(e, NR_BAD_PAIR, 0);
    for (i = 0; i < b->header.roleCount; ++i) {
        const NrRoleV1* r = &b->roles[i];
        const NrPacketV1* source;
        if (r->role < NR_FG_COLOUR)
            continue;
        source = r->role != NR_GENERATED && r->phase == 0 ? a : b;
        if (r->hostSerial != source->identity.hostSerial || r->sourceOccurrence != source->identity.sourceOccurrence)
            return fail(e, NR_BAD_PAIR, i);
    }
    return 1;
}
int nr_validate_source(const NrSourceWitnessV1* s, size_t n, NrError* e) {
    uint64_t i, j;
    if (!s || n != sizeof(*s))
        return fail(e, NR_BAD_BUFFER, 0);
    if (s->header.size != 640 || s->header.version != 1 || !(s->header.requestNonceLo | s->header.requestNonceHi) ||
        !s->header.pid || !s->header.creationFiletime || !s->header.applicationSubmissionId ||
        !s->header.publicWitnessId || !s->header.sourceThread || s->header.sourceThread != s->header.endThread ||
        s->header.corpusFrameIndex >= 5 || s->header.copyCount != 4 || s->header.sourceModuleIndex >= 8 ||
        s->header.observationFlags != 1 || !zero(s->header.reserved, sizeof s->header.reserved))
        return fail(e, NR_BAD_SOURCE, 0);
    for (i = 0; i < 4; ++i) {
        const NrSourceCopyV1* c = &s->copies[i];
        uint64_t present = c->destinationZ_and_boxPresent_packed32 >> 32;
        if (c->actualContextAddress != s->copies[0].actualContextAddress || c->destinationXYpacked32 ||
            (c->destinationZ_and_boxPresent_packed32 & UINT32_MAX) ||
            (present == 0 && (c->boxLeftTop_packed32 || c->boxFrontRight_packed32 || c->boxBottomBack_packed32)) ||
            (present == 1 && (c->boxLeftTop_packed32 || c->boxFrontRight_packed32 != (UINT64_C(512) << 32) ||
                              c->boxBottomBack_packed32 != ((UINT64_C(1) << 32) | 256))) ||
            (i && (c->qpcBefore < s->copies[i - 1].qpcAfter ||
                   c->actualCopySequence <= s->copies[i - 1].actualCopySequence)))
            return fail(e, NR_BAD_SOURCE, i);
        if (!c->actualContextAddress || !c->sourceAddress || !c->destinationAddress || !c->qpcBefore ||
            c->qpcAfter < c->qpcBefore || !c->callerRva || !epoch(c->actualCopySequence) ||
            !epoch(c->destinationWriteObservationEpoch) || c->observedMask != 7 || c->sourceSubresource ||
            c->destinationSubresource || (c->destinationZ_and_boxPresent_packed32 >> 32) > 1)
            return fail(e, NR_BAD_SOURCE, i);
        for (j = 0; j < i; ++j)
            if (c->actualCopySequence == s->copies[j].actualCopySequence ||
                c->destinationAddress == s->copies[j].destinationAddress)
                return fail(e, NR_BAD_SOURCE, i);
    }
    return 1;
}

int nr_match_replay(const NrPacketV1* a, const NrPacketV1* b, const uint64_t ad[2], const uint64_t bd[2], NrError* e) {
    if (!ad || !bd || !nr_validate_packet(a, sizeof(*a), e) || !nr_validate_packet(b, sizeof(*b), e) ||
        a->header.operation != NR_FRAME_TRANSFER || b->header.operation != NR_FRAME_TRANSFER)
        return fail(e, NR_BAD_ACK, 0);
    /* Addresses are compared as bytes; no encoded address is dereferenced.
     * Digest comparison alone is insufficient because borrowed refs are zeroed. */
    if (ad[0] != bd[0] || ad[1] != bd[1] || memcmp(a, b, sizeof(*a)))
        return fail(e, NR_BAD_ACK, 0);
    return 1;
}

int nr_ngx_not_failed(uint32_t value) {
    return (value & UINT32_C(0xfff00000)) != UINT32_C(0xbad00000);
}
