#ifndef PAIR_CAPTURE_NR_POSTFX_ABI_H
#define PAIR_CAPTURE_NR_POSTFX_ABI_H
#include <stdint.h>
#include <stddef.h>
/* Private candidate sidecar; original NrPacketV1 layout is unchanged.
   All address fields are scalar observations, never ownership or callable pointers.
   Canonical transfer bytes include every field, zero unused rows/reserved bytes.
   SHA256 ACK digest covers NrPacketV1 with ONLY refs.borrowedIUnknownAddress
   zeroed, followed by every exact fixed sidecar byte (no sidecar sanitization). Exact idempotence compares ORIGINAL
   packet plus ORIGINAL sidecar bytes, including every address field.
   A successful ACK covers immutable packet+sidecar acquisition in one transaction. */
enum NrPostFxFlagsV1 { NR_POSTFX_EASU = 1, NR_POSTFX_CAS = 2, NR_POSTFX_NEURAL_INPUT = 4 };
typedef struct NrPostFxRowV1 {
    uint64_t eye;
    uint64_t kind;
    uint64_t hostSerial;
    uint64_t contextAddress;
    uint64_t bufferAddress;
    uint64_t bufferLifetime;
    uint64_t constantWriteSequence;
    uint64_t mapResultBits;
    uint64_t mapSubresource;
    uint64_t mapType;
    uint64_t mapFlags;
    uint64_t mapEvent;
    uint64_t copyEvent;
    uint64_t unmapEvent;
    uint64_t dispatchEvent;
    uint64_t mapThread;
    uint64_t shaderAddress;
    uint64_t shaderLifetime;
    uint64_t srvAddress;
    uint64_t uavAddress;
    uint64_t samplerAddress;
    uint64_t bindingEpochs[5];
    uint64_t bindingEvents[5];
    uint64_t drawBindEpoch;
    uint64_t dispatchX;
    uint64_t dispatchY;
    uint64_t dispatchZ;
    uint64_t dispatchThread;
    uint64_t dispatchQpc;
    uint64_t inputXbits;
    uint64_t inputYbits;
    uint64_t inputWidth;
    uint64_t inputHeight;
    uint64_t sourceWidth;
    uint64_t sourceHeight;
    uint64_t outputWidth;
    uint64_t outputHeight;
    uint64_t stageFlags;
    uint64_t constantsBytes;
    uint64_t srvLifetime;
    uint64_t uavLifetime;
    uint64_t samplerLifetime;
    uint64_t inputResourceAddress;
    uint64_t inputResourceLifetime;
    uint64_t inputSourceWriteEpoch;
    uint64_t outputResourceAddress;
    uint64_t outputResourceLifetime;
    uint64_t outputSourceWriteEpoch;
    uint64_t srvWriteEpoch;
    uint64_t uavWriteEpoch;
    uint64_t samplerWriteEpoch;
    uint64_t reserved[3];
    uint8_t shaderSha256[32];
    uint8_t constants[112];
} NrPostFxRowV1;
/* D11-only kind3/4:
   constantWriteSequence == packet.draw.batchEpoch is observed memcpy-after-Map,
   independent from drawBindEpoch / bindingEpochs (actual CSSet observations).
   map/copy/unmap/binding/dispatch Event fields are a separate monotonic actual
   hook-order domain, with Map < copy < Unmap < B0 bind < Dispatch.
   binding array order: shader, B0 buffer, SRV0, UAV0, sampler0; CAS sampler entries
   and samplerAddress are zero. Each required binding has its own observed stamp.
   Map is original subresource0 / WRITE_DISCARD4 / flags0. Result is low32 HRESULT,
   with high32 zero. No result is synthesized for void Unmap/CSSet/Dispatch.
   Coordinates use zero-extended low32 signed bits, with nonnegative valid input.
   inputRect/inputSize/outputResolution are original per-eye values before layer
   mutation; allocation size is not asserted equal to per-eye outputResolution.
   EASU constants are112 original mapped bytes; CAS80 plus zero trailing32.
   Shader hash is exact original CreateComputeShader bytecode SHA256.
   No stamp or result claims GPU completion. */
typedef struct NrPostFxSidecarV1 {
    uint64_t size;
    uint64_t version;
    uint64_t attempt;
    uint64_t transferId;
    uint64_t frameOrdinal;
    uint64_t hostSerial;
    uint64_t applicationSubmissionId;
    uint64_t corpusFrameIndex;
    uint64_t publicJournalWitness;
    uint64_t nonce[2];
    uint64_t pid;
    uint64_t creationFiletime;
    uint64_t sessionNonce[2];
    uint64_t fgContextAddress;
    uint64_t fgContextLifetimeEpoch;
    uint64_t fgHistoryGeneration;
    uint64_t actualNrMode;
    uint64_t rowCount;
    uint64_t stageFlags;
    uint64_t reserved[3];
    NrPostFxRowV1 rows[4];
} NrPostFxSidecarV1;
/* Header fields join exact transfer packet identity. stageFlags permits EASU/CAS
   only; rows in (kind3 eye0,eye1, kind4 eye0,eye1) order, omitting disabled kinds.
   No postFX: complete cohort header, stageFlags0, rowCount0, all rows zero.
   Nonempty rows require NEURAL_INPUT plus exact common EASU/CAS flags.
   Receiver hashes/copies outside observer locks; it never dereferences an address. */
#if defined(__cplusplus)
static_assert(sizeof(NrPostFxRowV1) == 640 && sizeof(NrPostFxSidecarV1) == 2752);
static_assert(offsetof(NrPostFxSidecarV1, rows) == 192);
#else
_Static_assert(sizeof(NrPostFxRowV1) == 640 && sizeof(NrPostFxSidecarV1) == 2752, "postFX ABI size");
_Static_assert(offsetof(NrPostFxSidecarV1, rows) == 192, "postFX ABI rows");
#endif
#endif
