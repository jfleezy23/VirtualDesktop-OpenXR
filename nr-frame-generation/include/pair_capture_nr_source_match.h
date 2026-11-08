#pragma once
#include "pair_capture_nr_control.h"
/* Call only after nr_validate_packet and nr_validate_source succeed. No pointer
   is dereferenced: this joins the exact public copy to the held raw resource.
   API4's imported application endpoint is an explicit reverse storage alias;
   its write epoch is not assumed equal to the producer endpoint's epoch. */
static inline int nr_capture_source_matches(const NrPacketV1* p, const NrSourceWitnessV1* source) {
    uint64_t i, j;
    unsigned matched = 0, used = 0;
    const NrIdentityV1* x;
    const NrSourceHeaderV1* h;
    if (!p || !source || p->header.roleCount > 64 || p->header.aliasCount > 64 || source->header.copyCount != 4 ||
        p->header.moduleCount <= 4 || p->header.moduleCount > 8 || source->header.sourceModuleIndex != 4 ||
        p->modules[4].moduleKind != 5)
        return 0;
    /* Controlled candidate source-author slot, not a generic SDK module index.
       C5 validates the module record; external file/hash binding remains required. */
    for (j = 0; j < 4; ++j)
        if (source->copies[j].callerRva >= p->modules[4].imageSize)
            return 0;
    x = &p->identity;
    h = &source->header;
    if (x->nonce[0] != h->requestNonceLo || x->nonce[1] != h->requestNonceHi || x->pid != h->pid ||
        x->creationFiletime != h->creationFiletime || x->applicationSubmissionId != h->applicationSubmissionId ||
        x->corpusFrameIndex != h->corpusFrameIndex || x->publicJournalWitness != h->publicWitnessId)
        return 0;
    for (i = 0; i < p->header.roleCount; ++i)
        if (p->roles[i].role == NR_RAW_COLOUR || p->roles[i].role == NR_RAW_DEPTH) {
            const NrRoleV1* role = &p->roles[i];
            unsigned count = 0, copyBit = 0;
            if (role->aliasIndexOrAbsent != NR_ABSENT) {
                const NrAliasV1* alias;
                uint64_t api;
                int endpoint;
                if (role->aliasIndexOrAbsent >= p->header.aliasCount || role->sourceRefIndex >= p->header.refCount ||
                    role->sourceRefIndex >= 128)
                    return 0;
                alias = &p->aliases[role->aliasIndexOrAbsent];
                api = p->refs[role->sourceRefIndex].interfaceEnum;
                if ((alias->apiKind != NR_TEXTURE11_TO_TEXTURE11 && alias->apiKind != NR_TEXTURE11_TO_RESOURCE12) ||
                    alias->observedMask != 3 || alias->reserved || alias->format != role->format ||
                    alias->subresource != role->selectedArraySlice)
                    return 0;
                endpoint = api == NR_TEXTURE11 && alias->sourceAddress == role->sourceAddressObserved &&
                           alias->sourceLifetime == role->sourceLifetimeEpoch &&
                           alias->sourceWriteEpoch == role->sourceWriteEpoch;
                endpoint |= api == (alias->apiKind == NR_TEXTURE11_TO_TEXTURE11 ? NR_TEXTURE11 : NR_RESOURCE12) &&
                            alias->targetAddress == role->sourceAddressObserved &&
                            alias->targetLifetime == role->sourceLifetimeEpoch &&
                            alias->targetWriteEpoch == role->sourceWriteEpoch;
                if (!endpoint)
                    return 0;
            }
            for (j = 0; j < 4; ++j) {
                const NrSourceCopyV1* copy = &source->copies[j];
                int related = 0;
                if (copy->destinationSubresource != role->selectedArraySlice)
                    continue;
                related = copy->destinationAddress == role->sourceAddressObserved &&
                          copy->destinationWriteObservationEpoch == role->sourceWriteEpoch;
                if (!related && role->aliasIndexOrAbsent < p->header.aliasCount) {
                    const NrAliasV1* alias = &p->aliases[role->aliasIndexOrAbsent];
                    related = alias->apiKind == NR_TEXTURE11_TO_TEXTURE11 && alias->observedMask == 3 &&
                              !alias->reserved && alias->sourceAddress == role->sourceAddressObserved &&
                              alias->sourceLifetime == role->sourceLifetimeEpoch &&
                              alias->sourceWriteEpoch == role->sourceWriteEpoch &&
                              alias->targetAddress == copy->destinationAddress &&
                              alias->targetWriteEpoch == copy->destinationWriteObservationEpoch &&
                              alias->sourceDevice == x->producerDeviceAddress &&
                              alias->targetDevice != alias->sourceDevice &&
                              alias->subresource == copy->destinationSubresource && alias->format == role->format;
                }
                if (related) {
                    ++count;
                    copyBit = 1u << (unsigned)j;
                }
            }
            if (count != 1 || (used & copyBit))
                return 0;
            used |= copyBit;
            ++matched;
        }
    return matched == 4 && used == 15;
}
