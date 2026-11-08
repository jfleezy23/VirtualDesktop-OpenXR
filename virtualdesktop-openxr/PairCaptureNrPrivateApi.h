#pragma once
// Private candidate-only control ABI. Packet/source/ACK layout stays in the frozen header.
#include "../nr-frame-generation/include/pair_capture_nr_abi.h"
#include "../nr-frame-generation/include/pair_capture_nr_postfx_abi.h"
#include <stdint.h>
#if defined(_WIN32)
#define NR_PRIVATE_CALL __stdcall
#else
#define NR_PRIVATE_CALL
#endif

// Scalar observations only. Address words are never ownership transfer.
typedef struct NrReadyV1 {
    uint64_t size, version, status, observedFlags;
    uint64_t attempt, nonceLo, nonceHi, mode;
    uint64_t pid, creationFiletime, sessionNonceLo, sessionNonceHi;
    uint64_t fgContextAddress, fgContextLifetimeEpoch, fgHistoryGeneration;
    uint64_t deviceAddress, queueAddress, outputWidth, outputHeight, format;
    uint64_t moduleBase, moduleLifetimeEpoch, prepareSequence, bindSequence;
    uint64_t reserved[8];
} NrReadyV1;
// Warm source allocation observations only; no ownership or future ticket binding.
// Source resource addresses remain under the original Context owner lease.
typedef struct NrFgBudgetFootprintV1 {
    uint64_t offset, format, width, height, depth, rowPitch, rowCount, rowBytes;
} NrFgBudgetFootprintV1;
typedef struct NrFgBudgetRowV1 {
    uint64_t resourceAddress;
    uint64_t sourceIndex; // planned consumer category0..3; generated actual representative bank index
    uint64_t sourceKind;  // 1: observed history/guide representative for category0..3, 2: generated output
    uint64_t ownerContextLifetimeEpoch;
    uint64_t lifetimeBasis; // 1: actual Context owner lease; not an invented resource epoch
    uint64_t subresourceCount;
    uint64_t textureDescription[11]; // exact canonical D12 GetDesc words, same kind11 order
    uint64_t apiTotalBytes;
    uint64_t payloadBound;
    uint64_t allocationBytes; // actual allocation query for planned READBACK BUFFER
    uint64_t allocationAlignment;
    NrFgBudgetFootprintV1 footprints[2];
    uint64_t reserved[27];
} NrFgBudgetRowV1;
typedef struct NrFgReadbackBudgetV1 {
    uint64_t size, version, attempt, nonce[2], pid, creationFiletime, sessionNonce[2];
    uint64_t fgContextAddress, fgContextLifetimeEpoch, fgHistoryGeneration, actualNrMode;
    uint64_t status, rowCount, totalPayload, totalAllocation, observedFlags;
    uint64_t queryThread, qpcBegin, qpcEnd;
    uint64_t reserved[11];
    NrFgBudgetRowV1 rows[5];
} NrFgReadbackBudgetV1;
// Request: exact size/version/cohort, every response field/row/reserved byte zero.
// Return1/status1/flags1: five actual source descriptions, footprints and intended
// readback-buffer allocation queries completed. No allocation, command, AddRef,
// implicit arm, deep ACK or GPU completion is implied. Unused footprints zero.
// All applicable generated banks must match the observed representative fully;
// later selected allocations must match these full planned layouts, not just sums.

typedef enum NrHostState {
    NR_HOST_UNARMED = 0,
    NR_HOST_WARM = 1,
    NR_HOST_ARMED = 2,
    NR_HOST_ACTIVE = 3,
    NR_HOST_TRANSFERRED = 4,
    NR_HOST_SETTLED = 5,
    NR_HOST_ABORTED = 6,
    NR_HOST_UNCERTAIN = 7
} NrHostState;
typedef enum NrHostFlags {
    NR_HOST_REQUEST_VALID = 1,
    NR_HOST_RECEIVER_NEGOTIATED = 2,
    NR_HOST_NATIVE_WARM = 4,
    NR_HOST_SPATIAL_WARM = 8,
    NR_HOST_TWO_EYE_EVALUATE = 16,
    NR_HOST_NR_OUTPUT_WAIT = 32,
    NR_HOST_FG_ADMITTED = 64,
    NR_HOST_SOURCE_BOUND = 128,
    NR_HOST_PRIVATE_ARM = 256,
    NR_HOST_DUAL_PIN = 512,
    NR_HOST_ALL_REFS_RELEASED = 1024,
    NR_HOST_NO_READERS = 2048,
    NR_HOST_FEATURE_CREATED = 4096,
    NR_HOST_ORIGINAL_RETIRED = 8192
} NrHostFlags;
typedef struct NrHostStatusV1 {
    uint64_t size, version, state, flags;
    uint64_t attempt, nonceLo, nonceHi, mode;
    uint64_t pid, creationFiletime, sessionNonceLo, sessionNonceHi;
    uint64_t hostLifetimeEpoch, nrContextLifetimeEpoch, fgContextLifetimeEpoch, fgHistoryGeneration;
    uint64_t warmSerial, lastSerial, sourceWitnessId, sourceSubmissionId;
    uint64_t evaluateLeftSequence, evaluateRightSequence, originalNrSignalValue, outputWaitResultBits;
    uint64_t transferredFrames, releasedGroups, failureReason, guardCoverage;
    uint64_t reserved[4];
} NrHostStatusV1;
typedef struct NrHostReleaseV1 {
    uint64_t size, version, attempt, nonceLo, nonceHi, pid, creationFiletime, reserved;
} NrHostReleaseV1;
#ifdef __cplusplus
#define NR_PRIVATE_NOEXCEPT noexcept
static_assert(sizeof(NrFgBudgetFootprintV1) == 64 && sizeof(NrFgBudgetRowV1) == 512 &&
              sizeof(NrFgReadbackBudgetV1) == 2816 && offsetof(NrFgReadbackBudgetV1, rows) == 256);
static_assert(sizeof(NrReadyV1) == 256 && sizeof(NrHostStatusV1) == 256 && sizeof(NrHostReleaseV1) == 64);
extern "C" {
#else
#define NR_PRIVATE_NOEXCEPT
#endif
// Implemented by FG: return 1 only for a complete actual prepared/bound context
// matching the child request; otherwise zero and cleared response except size/version.
// Caller initializes size/version; successful flags are 1 (prepared+bound).
uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRReadyV1(NrReadyV1* ready) NR_PRIVATE_NOEXCEPT;
uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRBudgetV1(NrFgReadbackBudgetV1* budget) NR_PRIVATE_NOEXCEPT;
uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRV1(const NrPacketV1* packet, NrAckV1* ack) NR_PRIVATE_NOEXCEPT;
// Selected transfer atomically binds packet + complete actual postFX sidecar.
uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRSubmitWithPostFxV1(const NrPacketV1* packet,
                                                                const NrPostFxSidecarV1* postFx,
                                                                NrAckV1* ack) NR_PRIVATE_NOEXCEPT;
// Receiver copies exact source metadata only; this is not a deep ownership ACK.
uint32_t NR_PRIVATE_CALL VDXRFG_PrivatePairNRSourceV1(const NrPacketV1* packet,
                                                      const NrSourceWitnessV1* source) NR_PRIVATE_NOEXCEPT;
// Host: source witness accepted1/rejected0; no deep ownership implied.
uint32_t NR_PRIVATE_CALL VDXR_PrivatePairNRSourceV1(const NrSourceWitnessV1* witness) NR_PRIVATE_NOEXCEPT;
// Query requires caller size256/version1; returns1 when exact response written.
// Flags are observed facts; only WARM+required flags allow scene-ready publication.
uint32_t NR_PRIVATE_CALL VDXR_PrivatePairNRStatusV1(NrHostStatusV1* status) NR_PRIVATE_NOEXCEPT;
// Returns1 only after all sender groups settled, readers closed and FG settled.
// Return0 may be pending; query status and retry boundedly. ABORTED/UNCERTAIN
// or timeout require owned-child fail-closed termination, never ordinary teardown.
uint32_t NR_PRIVATE_CALL VDXR_PrivatePairNRReleaseV1(const NrHostReleaseV1* request) NR_PRIVATE_NOEXCEPT;
#ifdef __cplusplus
}
#endif
