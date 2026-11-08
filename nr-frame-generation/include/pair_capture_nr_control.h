/* Candidate-local operations; the frozen wire layout/header stays unchanged. */
#ifndef PAIR_CAPTURE_NR_CONTROL_H
#define PAIR_CAPTURE_NR_CONTROL_H
#include "pair_capture_nr_abi.h"

enum NrSharedHandleCallAlias { NR_EXPORT_SHARED_HANDLE = NR_EXPORT_FENCE, NR_IMPORT_SHARED_HANDLE = NR_IMPORT_FENCE };

/* Resource GetDesc observations, not shader views or descriptor bindings. */
enum NrResourceDescriptionExtension {
    NR_DESC_RESOURCE_DESCRIPTION = 6,
    NR_D11_TEXTURE2D_DESCRIPTION = 10,
    NR_D12_RESOURCE_DESCRIPTION = 11
};
enum NrResourceAliasExtension { NR_TEXTURE11_TO_TEXTURE11 = 4, NR_TEXTURE11_TO_RESOURCE12 = 5 };
/* D11 draw kinds 3 (EASU) / 4 (CAS) use API-specific legacy words:
   psoAddress = actual compute shader; rootAddress = actual B0 buffer;
   rootTable0/1 = actual SRV0/UAV0 objects. They are not D12 PSO/root/tables.
   Graphics-only viewport/scissor/vertex fields are inapplicable zero; dispatch
   counts are observed. Constants are actual mapped writes:112/80 bytes.
   D12 kinds1/2/5 retain their actual PSO/root/table meanings. */

/* Snapshot storage/view families only; not NR/HDR or GPU-view capability proof. */
static inline uint64_t nr_capture_pixel_bytes(uint64_t format) {
    switch (format) {
    case 9:
    case 10:
        return 8;
    case 27:
    case 28:
    case 29:
    case 39:
    case 40:
    case 41:
    case 87:
    case 88:
    case 90:
    case 91:
    case 92:
    case 93:
        return 4;
    default:
        return 0;
    }
}
static inline int nr_capture_view_family_compatible(uint64_t resource, uint64_t view) {
    switch (resource) {
    case 9:
    case 10:
        return view == 10;
    case 27:
    case 28:
    case 29:
        return view == 28 || view == 29;
    case 39:
    case 40:
    case 41:
        return view == 41;
    case 87:
    case 90:
    case 91:
        return view == 87 || view == 91;
    case 88:
    case 92:
    case 93:
        return view == 88 || view == 93;
    default:
        return 0;
    }
}

#ifdef __cplusplus
extern "C" {
#endif
/* Shape/correlation only. Receiver state must separately prove settlement. */
int nr_validate_control_ack(
    const NrPacketV1* request, uint32_t returned_status, const NrAckV1* ack, size_t ack_bytes, NrError* error);
#ifdef __cplusplus
}
#endif
#endif
