#pragma once
#include "../../../virtualdesktop-openxr/PairCaptureNrPrivateApi.h"
#include "pair_capture_nr_control.h"
#include <string.h>
/* Pure scalar checks. These cannot prove an API was executed: the original
   Context-owned query supplies observations; offline source/module binding is
   independent. No address is ever dereferenced by these C-compatible helpers. */
static inline int nr_fg_budget_zero(const void* data, size_t bytes) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;
    for (i = 0; i < bytes; ++i)
        if (p[i])
            return 0;
    return 1;
}
/* Original generated outputs may be D11/D12 shared textures. This observes
   their layout only; history/guide sources and cross-adapter resources remain
   outside that exception. Private readbacks are separate flags-zero buffers. */
static inline int nr_fg_budget_texture_flags(uint64_t sourceKind, uint64_t flags) {
    return (sourceKind == 1 || sourceKind == 2) && flags <= UINT32_MAX && !(flags & (2u | 8u | 16u)) &&
           (sourceKind == 2 || !(flags & 32u));
}
static inline int nr_fg_budget_row(const NrFgBudgetRowV1* r) {
    const uint64_t* d = r->textureDescription;
    uint64_t i, end = 0, payload = 0, bpp = nr_capture_pixel_bytes(d[6]);
    if (!r->resourceAddress || !r->ownerContextLifetimeEpoch || r->lifetimeBasis != 1 || r->sourceKind < 1 ||
        r->sourceKind > 2 || (r->sourceKind == 1 ? r->sourceIndex > 3 : r->sourceIndex > 7) ||
        !nr_fg_budget_zero(r->reserved, sizeof(r->reserved)) || d[0] != 3 || !d[2] || d[2] > UINT32_MAX || !d[3] ||
        d[3] > UINT32_MAX || !d[4] || d[4] > 2 || d[5] != 1 || d[7] != 1 || d[8] || d[9] ||
        !nr_fg_budget_texture_flags(r->sourceKind, d[10]) || !bpp || d[2] > UINT32_MAX / bpp ||
        r->subresourceCount != d[4])
        return 0;
    if (r->sourceKind == 1 && r->sourceIndex >= 2) {
        if (d[6] != 41)
            return 0;
    } else if (!nr_capture_view_family_compatible(d[6], 28))
        return 0;
    for (i = 0; i < 2; ++i) {
        const NrFgBudgetFootprintV1* f = &r->footprints[i];
        uint64_t bytes;
        if (i >= r->subresourceCount) {
            if (!nr_fg_budget_zero(f, sizeof(*f)))
                return 0;
            continue;
        }
        if (f->format != d[6] || f->width != d[2] || f->height != d[3] || f->depth != 1 || f->rowCount != d[3] ||
            f->rowBytes != d[2] * bpp || !f->rowPitch || f->rowPitch > UINT32_MAX || f->rowPitch % 256 ||
            f->rowPitch < f->rowBytes || f->offset % 512 || f->rowCount > UINT64_MAX / f->rowPitch)
            return 0;
        bytes = f->rowCount * f->rowPitch;
        if (f->offset < end || f->offset > UINT64_MAX - bytes)
            return 0;
        end = f->offset + bytes;
        if (end > payload)
            payload = end;
    }
    if (!payload || payload != r->payloadBound || !r->apiTotalBytes || r->apiTotalBytes > payload ||
        r->apiTotalBytes < r->footprints[r->subresourceCount - 1].offset +
                               (r->footprints[r->subresourceCount - 1].rowCount - 1) *
                                   r->footprints[r->subresourceCount - 1].rowPitch +
                               r->footprints[r->subresourceCount - 1].rowBytes ||
        !r->allocationBytes || r->allocationBytes > 134217728 || r->allocationBytes < payload ||
        !r->allocationAlignment || (r->allocationAlignment & (r->allocationAlignment - 1)) ||
        r->allocationBytes % r->allocationAlignment)
        return 0;
    return 1;
}
static inline int nr_fg_budget_same_layout(const NrFgBudgetRowV1* a, const NrFgBudgetRowV1* b) {
    return a->subresourceCount == b->subresourceCount &&
           !memcmp(a->textureDescription, b->textureDescription, sizeof(a->textureDescription)) &&
           !memcmp(a->footprints, b->footprints, sizeof(a->footprints)) && a->apiTotalBytes == b->apiTotalBytes &&
           a->payloadBound == b->payloadBound && a->allocationBytes == b->allocationBytes &&
           a->allocationAlignment == b->allocationAlignment;
}
static inline int nr_validate_fg_budget(const NrFgReadbackBudgetV1* b, size_t bytes) {
    uint64_t i, payload = 0, allocation = 0;
    if (!b || bytes != sizeof(*b) || b->size != sizeof(*b) || b->version != 1 || b->status != 1 ||
        b->observedFlags != 1 || b->rowCount != 5 || !b->attempt || !(b->nonce[0] | b->nonce[1]) || !b->pid ||
        !b->creationFiletime || !(b->sessionNonce[0] | b->sessionNonce[1]) || !b->fgContextAddress ||
        !b->fgContextLifetimeEpoch || !b->fgHistoryGeneration || (b->actualNrMode != 1 && b->actualNrMode != 2) ||
        !b->queryThread || !b->qpcBegin || b->qpcEnd < b->qpcBegin ||
        !nr_fg_budget_zero(b->reserved, sizeof(b->reserved)))
        return 0;
    for (i = 0; i < 5; ++i) {
        const NrFgBudgetRowV1* r = &b->rows[i];
        if (!nr_fg_budget_row(r) || r->ownerContextLifetimeEpoch != b->fgContextLifetimeEpoch ||
            (i < 4 ? (r->sourceKind != 1 || r->sourceIndex != i)
                   : (r->sourceKind != 2 || (r->sourceIndex != 0 && r->sourceIndex != 4))) ||
            r->allocationBytes > 134217728 - allocation || r->payloadBound > 134217728 - payload)
            return 0;
        allocation += r->allocationBytes;
        payload += r->payloadBound;
    }
    return b->totalPayload == payload && b->totalAllocation == allocation &&
           nr_fg_budget_same_layout(&b->rows[0], &b->rows[1]) && nr_fg_budget_same_layout(&b->rows[2], &b->rows[3]);
}
static inline int nr_fg_budget_matches_packet(const NrFgReadbackBudgetV1* b, const NrPacketV1* p) {
    const NrIdentityV1* x;
    if (!b || !p)
        return 0;
    x = &p->identity;
    return nr_validate_fg_budget(b, sizeof(*b)) && b->attempt == p->header.attempt &&
           b->actualNrMode == p->header.actualNrMode && !memcmp(b->nonce, x->nonce, sizeof(b->nonce)) &&
           b->pid == x->pid && b->creationFiletime == x->creationFiletime &&
           !memcmp(b->sessionNonce, x->sessionNonce, sizeof(b->sessionNonce)) &&
           b->fgContextAddress == x->fgContextAddress && b->fgContextLifetimeEpoch == x->fgContextLifetimeEpoch &&
           b->fgHistoryGeneration == x->fgHistoryGeneration && b->totalAllocation == p->budget.pairOnlyBytes &&
           b->totalPayload <= p->budget.privatePayloadReservation;
}
static inline int nr_fg_budget_matches_ready(const NrFgReadbackBudgetV1* b, const NrReadyV1* r) {
    return r->status == 1 && r->observedFlags == 1 && b->size == sizeof(*b) && b->version == 1 &&
           b->attempt == r->attempt && b->nonce[0] == r->nonceLo && b->nonce[1] == r->nonceHi && b->pid == r->pid &&
           b->creationFiletime == r->creationFiletime && b->sessionNonce[0] == r->sessionNonceLo &&
           b->sessionNonce[1] == r->sessionNonceHi && b->fgContextAddress == r->fgContextAddress &&
           b->fgContextLifetimeEpoch == r->fgContextLifetimeEpoch && b->fgHistoryGeneration == r->fgHistoryGeneration &&
           b->actualNrMode == r->mode;
}
static inline int nr_fg_budget_request(const NrFgReadbackBudgetV1* b, const NrReadyV1* r) {
    return nr_fg_budget_matches_ready(b, r) &&
           nr_fg_budget_zero(&b->status, sizeof(*b) - offsetof(NrFgReadbackBudgetV1, status));
}
#ifdef __cplusplus
#include <d3d12.h>
namespace vdxrfg::pair_capture::nr {
    using BudgetProvider = bool (*)(void*, const NrReadyV1&, NrFgReadbackBudgetV1&) noexcept;
    void installBudgetProvider(void*, BudgetProvider) noexcept;
    bool observeBudgetResource(ID3D12Device*, ID3D12Resource*, uint64_t, uint64_t, uint64_t, NrFgBudgetRowV1&) noexcept;
    bool fgInstallBudget(const NrFgReadbackBudgetV1&) noexcept;
    bool fgBudgetMatches(const NrPacketV1&) noexcept;
} // namespace vdxrfg::pair_capture::nr
#endif
