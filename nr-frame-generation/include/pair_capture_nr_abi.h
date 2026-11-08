/* Private in-process x64 POD ABI. All address fields are numeric observations:
 * the metadata validator never dereferences them. Not a COM ownership proof. */
#ifndef PAIR_CAPTURE_NR_ABI_H
#define PAIR_CAPTURE_NR_ABI_H
#include <stddef.h>
#include <stdint.h>
#define NR_MAGIC UINT64_C(0x31524e5249415056)
#define NR_PACKET_BYTES UINT64_C(156160)
#define NR_ABSENT UINT64_MAX
#define NR_ROLE_BIT(n) (UINT64_C(1) << ((n) - 1))
#define NR_HOST_IDENTITY_OBS UINT64_C(127)
#define NR_ROLE_OBS UINT64_C(15)
#define NR_CALL_OBS UINT64_C(7)
#define NR_ABI_VERSION UINT64_C(1)
enum NrOperation { NR_NEGOTIATE = 1, NR_FRAME_TRANSFER = 2, NR_ABORT = 3, NR_QUERY = 4 };
enum NrMode { NR_NATIVE100 = 1, NR_SPATIAL85 = 2 };
enum NrStatus {
    NR_UNSUPPORTED = 0,
    NR_REJECTED = 1,
    NR_DEEP_OWNED = 2,
    NR_UNCERTAIN = 3,
    NR_NEGOTIATED = 4,
    NR_SETTLED = 5
};
enum NrRole {
    NR_RAW_COLOUR = 1,
    NR_RAW_DEPTH,
    NR_MODEL_COLOUR,
    NR_MODEL_DEPTH,
    NR_PROXY,
    NR_PROXY_DEPTH,
    NR_NATIVE_ANSWER,
    NR_SPATIAL_ANSWER,
    NR_POST_EVAL_COLOUR,
    NR_PRE_RESOLVE_COLOUR,
    NR_EXACT_PROXY,
    NR_POST_RESOLVE,
    NR_POST_EASU,
    NR_POST_CAS,
    NR_FINAL_COLOUR,
    NR_CANONICAL_DEPTH,
    NR_FG_COLOUR,
    NR_FG_DEPTH,
    NR_GENERATED
};
enum NrInterface {
    NR_IUNKNOWN = 1,
    NR_TEXTURE11,
    NR_RESOURCE12,
    NR_DEVICE11,
    NR_CONTEXT11,
    NR_DEVICE12,
    NR_QUEUE12,
    NR_LIST12,
    NR_ALLOCATOR12,
    NR_FENCE11,
    NR_FENCE12,
    NR_HEAP12,
    NR_ROOT12,
    NR_PSO12,
    NR_VIEW11,
    NR_BUFFER11,
    NR_SHADER11,
    NR_SAMPLER11,
    NR_INFO_QUEUE
};
enum NrDescriptorKind { NR_DESC_SRV = 1, NR_DESC_UAV, NR_DESC_RTV, NR_DESC_CBV, NR_DESC_SAMPLER };
enum NrDescriptorApi {
    NR_D11_SRV = 1,
    NR_D12_SRV,
    NR_D11_UAV,
    NR_D12_UAV,
    NR_D11_RTV,
    NR_D12_RTV,
    NR_D12_CBV,
    NR_D11_SAMPLER,
    NR_D12_SAMPLER
};
enum NrGroupKind { NR_RAW11 = 0, NR_LIST_GROUP12 = 1, NR_POSTFX11 = 2 };
enum NrObservation { NR_UNOBSERVED = 0, NR_OBSERVED_VALUE = 1, NR_OBSERVED_NULL = 2, NR_NOT_HOST_WRITTEN = 3 };
enum NrParamType { NR_U32 = 1, NR_F32 = 2, NR_RESOURCE_ID = 3, NR_MATRIX_ABSENT = 4, NR_I32 = 5 };
enum NrResultType { NR_VOID = 0, NR_HRESULT = 1, NR_NGX = 2, NR_BOOLEAN = 3 };
enum NrCallKind {
    NR_CREATE_SDK = 1,
    NR_CREATE_MANUAL,
    NR_INIT_MANUAL,
    NR_EVALUATE,
    NR_RELEASE_FEATURE,
    NR_CLOSE,
    NR_EXECUTE,
    NR_SIGNAL,
    NR_WAIT,
    NR_FLUSH,
    NR_COPY,
    NR_DISPATCH,
    NR_DRAW,
    NR_DEVICE_LUID,
    NR_PIN,
    NR_EXPORT_FENCE,
    NR_IMPORT_FENCE,
    NR_ADMISSION,
    NR_RETIRE
};
enum NrParameter {
    NR_WIDTH = 1,
    NR_HEIGHT,
    NR_PRESET,
    NR_CREATION_NODE,
    NR_VISIBILITY_NODE,
    NR_DEPTH_INVERTED,
    NR_ENABLED,
    NR_COLOR,
    NR_OUTPUT,
    NR_COLOR_X,
    NR_COLOR_Y,
    NR_COLOR_WIDTH,
    NR_COLOR_HEIGHT,
    NR_OUTPUT_X,
    NR_OUTPUT_Y,
    NR_OUTPUT_WIDTH,
    NR_OUTPUT_HEIGHT,
    NR_DEPTH,
    NR_DEPTH_X,
    NR_DEPTH_Y,
    NR_DEPTH_WIDTH,
    NR_DEPTH_HEIGHT,
    NR_RESET,
    NR_AUTO_MASK,
    NR_STYLE,
    NR_INTENSITY,
    NR_TONE,
    NR_STRUCTURE,
    NR_SKIN,
    NR_UI,
    NR_MVEC_ABSENT,
    NR_MVEC_X_ABSENT,
    NR_MVEC_Y_ABSENT,
    NR_MVEC_WIDTH_ABSENT,
    NR_MVEC_HEIGHT_ABSENT,
    NR_MVEC_SCALE_X_ABSENT,
    NR_MVEC_SCALE_Y_ABSENT,
    NR_WORLD_VIEW_ABSENT,
    NR_VIEW_CLIP_ABSENT
};
enum NrErrorCode {
    NR_VALID = 0,
    NR_BAD_BUFFER,
    NR_BAD_HEADER,
    NR_BAD_OPERATION,
    NR_BAD_COUNT,
    NR_NONZERO_UNUSED,
    NR_BAD_IDENTITY,
    NR_BAD_BUDGET,
    NR_BAD_ROLE,
    NR_BAD_REF,
    NR_BAD_GROUP,
    NR_BAD_RANGE,
    NR_BAD_PARAM,
    NR_BAD_CALL,
    NR_BAD_DESCRIPTOR,
    NR_BAD_DRAW,
    NR_BAD_ALIAS,
    NR_BAD_WRITE,
    NR_BAD_FOOTPRINT,
    NR_BAD_MODULE,
    NR_BAD_ACK,
    NR_BAD_PAIR,
    NR_BAD_SOURCE
};
typedef struct NrError {
    uint32_t code;
    uint32_t index;
} NrError;
#pragma pack(push, 8)
typedef struct NrHeaderV1 {
    uint64_t magic;
    uint64_t size;
    uint64_t version;
    uint64_t operation;
    uint64_t flags;
    uint64_t attempt;
    uint64_t frameOrdinal;
    uint64_t transferId;
    uint64_t roleCount;
    uint64_t refCount;
    uint64_t groupCount;
    uint64_t paramCount;
    uint64_t callCount;
    uint64_t descriptorCount;
    uint64_t drawCount;
    uint64_t aliasCount;
    uint64_t writeCount;
    uint64_t footprintCount;
    uint64_t constantsUsedBytes;
    uint64_t moduleCount;
    uint64_t sourceObservationCount;
    uint64_t frameOutcome;
    uint64_t actualNrMode;
    uint64_t requiredRoleMaskLo;
    uint64_t requiredRoleMaskHi;
    uint64_t presentRoleMaskLo;
    uint64_t presentRoleMaskHi;
    uint64_t packetMutationEpoch;
    uint64_t guardCoverage;
    uint64_t packetFrozen;
    uint64_t reserved[2];
} NrHeaderV1;
typedef struct NrRawSettingsV1 {
    uint64_t enabled;
    uint64_t style;
    uint64_t intensity;
    uint64_t tone;
    uint64_t structure;
    uint64_t skin;
    uint64_t processing;
    uint64_t coverage;
    uint64_t upscaling;
    uint64_t sharpening;
    uint64_t sharpenFactor;
    uint64_t srgb;
    uint64_t inputWidth;
    uint64_t inputHeight;
    uint64_t outputWidth;
    uint64_t outputHeight;
} NrRawSettingsV1;
typedef struct NrIdentityV1 {
    uint64_t nonce[2];
    uint64_t pid;
    uint64_t creationFiletime;
    uint64_t sessionNonce[2];
    uint64_t hostInstanceAddress;
    uint64_t hostLifetimeEpoch;
    uint64_t ownerHostEpoch;
    uint64_t sessionHandleObserved;
    uint64_t sessionLifetimeEpoch;
    uint64_t hostSerial;
    uint64_t displayTimeBits;
    uint64_t sourceOccurrence;
    uint64_t applicationSubmissionId;
    uint64_t corpusFrameIndex;
    uint64_t publicJournalWitness;
    uint64_t projectionIndex;
    uint64_t eyeMask;
    uint64_t frameThread;
    uint64_t xrEntryQpc;
    uint64_t producerLeaseQpc;
    uint64_t sourceGeneration;
    uint64_t sourceWriteEpoch;
    uint64_t fgContextAddress;
    uint64_t fgContextLifetimeEpoch;
    uint64_t fgHistoryGeneration;
    uint64_t ticketGeneration;
    uint64_t previousSerial;
    uint64_t currentSerial;
    uint64_t nrContextAddress;
    uint64_t nrContextLifetimeEpoch;
    uint64_t pipelineAddress;
    uint64_t pipelineLifetimeEpoch;
    uint64_t pipelineGenerationEpoch;
    uint64_t batchObjectEpoch;
    uint64_t descriptorBindEpoch;
    uint64_t listObjectEpoch;
    uint64_t allocatorObjectEpoch;
    uint64_t recordingEpoch;
    uint64_t producerContextAddress;
    uint64_t producerDeviceAddress;
    uint64_t nrDeviceAddress;
    uint64_t nrQueueAddress;
    uint64_t nrFenceAddress;
    uint64_t originalNrSignalValue;
    uint64_t privateProducerFenceAddress;
    uint64_t privateProducerValue;
    uint64_t importedAppFenceAddress;
    uint64_t importedFgFenceAddress;
    uint64_t luidLow;
    uint64_t luidHigh;
    uint64_t actualProcessingPercent;
    uint64_t actualCoveragePercent;
    uint64_t settingsObservedMask;
    uint64_t pipelinePointerPresence;
    uint64_t pipelineEpochObservedMask;
    uint64_t featureLifetimeLeft;
    uint64_t featureLifetimeRight;
    uint64_t parameterTableLifetime;
    uint64_t warmFeatureWitnessEpoch;
    uint64_t framePolicyBits;
    uint64_t stackCount;
    uint64_t observedIdentityMask;
    uint64_t entryStackAddresses[48];
    NrRawSettingsV1 settings;
} NrIdentityV1;
typedef struct NrBudgetV1 {
    uint64_t capBytes;
    uint64_t cpuMetadataReservation;
    uint64_t privatePayloadReservation;
    uint64_t privateAllocationReservation;
    uint64_t mappedPitchReservation;
    uint64_t retainedOriginalRefs;
    uint64_t senderRefCountA;
    uint64_t senderRefCountB;
    uint64_t receiverRefCountA;
    uint64_t receiverRefCountB;
    uint64_t roleCountA;
    uint64_t roleCountB;
    uint64_t pairOnlyRoleCount;
    uint64_t groupCountA;
    uint64_t groupCountB;
    uint64_t allocationCount;
    uint64_t handleCount;
    uint64_t featureWitnessCount;
    uint64_t descriptorCapacity;
    uint64_t sourceObservationCapacity;
    uint64_t bytesA;
    uint64_t bytesB;
    uint64_t pairOnlyBytes;
    uint64_t physicalPrivatePeakBytes;
    uint64_t logicalSenderCharge;
    uint64_t logicalReceiverCharge;
    uint64_t allArithmeticChecked;
    uint64_t preflightComplete;
    uint64_t pinHostComplete;
    uint64_t pinFgComplete;
    uint64_t firstExposureSequence;
    uint64_t reserved;
} NrBudgetV1;
typedef struct NrRoleV1 {
    uint64_t role;
    uint64_t eye;
    uint64_t phase;
    uint64_t groupIndex;
    uint64_t sourceRefIndex;
    uint64_t snapshotRefIndex;
    uint64_t sourceAddressObserved;
    uint64_t snapshotAddressObserved;
    uint64_t sourceLifetimeEpoch;
    uint64_t sourceWriteEpoch;
    uint64_t snapshotWriteEpoch;
    uint64_t descriptorEpoch;
    uint64_t sourceOccurrence;
    uint64_t hostSerial;
    uint64_t projectionIndex;
    uint64_t releasedIndex;
    uint64_t selectedArraySlice;
    uint64_t selectedMip;
    uint64_t firstSubresource;
    uint64_t subresourceCount;
    uint64_t resourceDimension;
    uint64_t width;
    uint64_t height;
    uint64_t depthOrArray;
    uint64_t mipLevels;
    uint64_t format;
    uint64_t sampleCount;
    uint64_t sampleQuality;
    uint64_t layout;
    uint64_t resourceFlags;
    uint64_t resourceUsage;
    uint64_t bindFlags;
    uint64_t cpuAccessFlags;
    uint64_t miscFlags;
    uint64_t viewFormat;
    uint64_t viewDimension;
    uint64_t rectXbits;
    uint64_t rectYbits;
    uint64_t rectWidth;
    uint64_t rectHeight;
    uint64_t cropXbits;
    uint64_t cropYbits;
    uint64_t cropWidth;
    uint64_t cropHeight;
    uint64_t minDepthBits;
    uint64_t maxDepthBits;
    uint64_t nearZBits;
    uint64_t farZBits;
    uint64_t copyMethod;
    uint64_t copyBeforeState;
    uint64_t copyAfterState;
    uint64_t copyListAddress;
    uint64_t recordingEpoch;
    uint64_t descriptorFirst;
    uint64_t descriptorCount;
    uint64_t footprintFirst;
    uint64_t footprintCount;
    uint64_t payloadBytes;
    uint64_t chargedBytes;
    uint64_t mapPitchBound;
    uint64_t observedMask;
    uint64_t aliasIndexOrAbsent;
    uint64_t callIndexOrAbsent;
    uint64_t reserved;
} NrRoleV1;
typedef struct NrRefV1 {
    uint64_t borrowedIUnknownAddress;
    uint64_t interfaceEnum;
    uint64_t roleMask;
    uint64_t groupIndex;
    uint64_t physicalAllocationId;
    uint64_t chargedPrivateBytes;
    uint64_t owningSourceLifetimeEpoch;
    uint64_t observedNonNull;
} NrRefV1;
typedef struct NrGroupV1 {
    uint64_t kind;
    uint64_t senderOwnerNumericId;
    uint64_t senderAttempt;
    uint64_t frameOrdinal;
    uint64_t senderSlot;
    uint64_t senderGeneration;
    uint64_t transferId;
    uint64_t sourceOccurrence;
    uint64_t sourceGeneration;
    uint64_t descriptorEpoch;
    uint64_t deviceAddress;
    uint64_t actualQueueOrContext;
    uint64_t actualFence;
    uint64_t actualList;
    uint64_t actualAllocator;
    uint64_t recordingEpoch;
    uint64_t nrContextLifetimeEpoch;
    uint64_t expectedSignalValue;
    uint64_t closeObserved;
    uint64_t closeResultBits;
    uint64_t executeObservationCount;
    uint64_t originalSignalObserved;
    uint64_t originalSignalValue;
    uint64_t originalSignalResultBits;
    uint64_t coveringSignalObserved;
    uint64_t coveringSignalValue;
    uint64_t coveringSignalResultBits;
    uint64_t completedObserved;
    uint64_t completedValue;
    uint64_t sourceReturnWaitObserved;
    uint64_t sourceReturnWaitResultBits;
    uint64_t qualificationMask;
} NrGroupV1;
typedef struct NrParamV1 {
    uint64_t callIndex;
    uint64_t parameterEnum;
    uint64_t typeEnum;
    uint64_t valueLo;
    uint64_t valueHi;
    uint64_t tableLifetime;
    uint64_t writeEpoch;
    uint64_t observationStatus;
} NrParamV1;
typedef union NrCallArgumentV1 {
    uint64_t actualCountArgument;
    uint64_t pairedEvaluateAddress;
} NrCallArgumentV1;
typedef union NrCallAuxV1 {
    uint64_t auxiliaryValue;
    uint64_t pairedReleaseAddress;
} NrCallAuxV1;
typedef struct NrCallV1 {
    uint64_t kind;
    uint64_t beginObserved;
    uint64_t endObserved;
    uint64_t resultType;
    uint64_t resultBits;
    uint64_t classifiedSuccess;
    uint64_t qpcBegin;
    uint64_t qpcEnd;
    uint64_t thread;
    uint64_t eye;
    uint64_t featureAddressObserved;
    uint64_t featureLifetimeEpoch;
    uint64_t parameterTableAddressObserved;
    uint64_t parameterTableLifetime;
    uint64_t dispatchAddressObserved;
    uint64_t dispatchTableEpoch;
    uint64_t moduleIndex;
    uint64_t deviceAddress;
    uint64_t queueAddress;
    uint64_t fenceAddress;
    uint64_t signalValue;
    uint64_t listAddress;
    uint64_t allocatorAddress;
    uint64_t recordingEpoch;
    uint64_t contextAddress;
    uint64_t contextLifetimeEpoch;
    uint64_t paramFirst;
    uint64_t paramCount;
    uint64_t flags;
    NrCallArgumentV1 actual;
    NrCallAuxV1 aux;
    uint64_t observedMask;
} NrCallV1;
typedef struct NrDescriptorV1 {
    uint64_t kind;
    uint64_t objectAddress;
    uint64_t objectLifetimeEpoch;
    uint64_t heapAddress;
    uint64_t heapSlot;
    uint64_t descriptorIncrement;
    uint64_t cpuHandle;
    uint64_t gpuHandle;
    uint64_t bindEpoch;
    uint64_t writeEpoch;
    uint64_t resourceAddress;
    uint64_t resourceLifetimeEpoch;
    uint64_t sourceWriteEpoch;
    uint64_t apiDescriptionBytes;
    uint64_t descriptionKind;
    uint64_t observedMask;
    uint64_t canonicalDescriptionWords[16];
} NrDescriptorV1;
typedef struct NrDrawV1 {
    uint64_t kind;
    uint64_t eye;
    uint64_t listOrContext;
    uint64_t recordingEpoch;
    uint64_t pipelineLifetime;
    uint64_t pipelineGeneration;
    uint64_t batchEpoch;
    uint64_t bindEpoch;
    uint64_t psoAddress;
    uint64_t rootAddress;
    uint64_t shaderModuleIndex;
    uint64_t shaderHashOffsetInConstants;
    uint64_t constantsOffset;
    uint64_t constantsBytes;
    uint64_t descriptorFirst;
    uint64_t descriptorCount;
    uint64_t rootTable0;
    uint64_t rootTable1;
    uint64_t viewportXbits;
    uint64_t viewportYbits;
    uint64_t viewportWbits;
    uint64_t viewportHbits;
    uint64_t minDepthBits;
    uint64_t maxDepthBits;
    uint64_t scissorLeftBits;
    uint64_t scissorTopBits;
    uint64_t scissorRightBits;
    uint64_t scissorBottomBits;
    uint64_t dispatchX;
    uint64_t dispatchY;
    uint64_t dispatchZ;
    uint64_t vertexCount;
    uint64_t instanceCount;
    uint64_t startVertex;
    uint64_t startInstance;
    uint64_t resourceWriteEpoch;
    uint64_t observedMask;
    uint64_t reserved[27];
} NrDrawV1;
typedef struct NrAliasV1 {
    uint64_t sourceAddress;
    uint64_t sourceLifetime;
    uint64_t targetAddress;
    uint64_t targetLifetime;
    uint64_t sourceDevice;
    uint64_t targetDevice;
    uint64_t exportedHandleObserved;
    uint64_t exportCallIndex;
    uint64_t importCallIndex;
    uint64_t apiKind;
    uint64_t sourceWriteEpoch;
    uint64_t targetWriteEpoch;
    uint64_t subresource;
    uint64_t format;
    uint64_t observedMask;
    uint64_t reserved;
} NrAliasV1;
typedef struct NrWriteV1 {
    uint64_t resourceAddress;
    uint64_t resourceLifetime;
    uint64_t writeEpoch;
    uint64_t sourceOccurrence;
    uint64_t phase;
    uint64_t listOrContextAddress;
    uint64_t recordingEpoch;
    uint64_t callOrRoleIndex;
} NrWriteV1;
typedef struct NrFootprintV1 {
    uint64_t roleIndex;
    uint64_t subresourceIndex;
    uint64_t offset;
    uint64_t rowPitch;
    uint64_t rowBytes;
    uint64_t rowCount;
    uint64_t depth;
    uint64_t totalBytes;
} NrFootprintV1;
typedef struct NrModuleV1 {
    uint64_t moduleKind;
    uint64_t baseObserved;
    uint64_t imageSize;
    uint64_t addressRva;
    uint64_t fileSize;
    uint64_t loadLifetimeEpoch;
    uint64_t pinObserved;
    uint64_t pinResult;
    uint64_t sha256Words[4];
    uint64_t codeViewGuidWords[2];
    uint64_t codeViewAge;
    uint64_t observedMask;
} NrModuleV1;
typedef struct NrAckV1 {
    uint64_t size;
    uint64_t version;
    uint64_t status;
    uint64_t reasonEnum;
    uint64_t requestAttempt;
    uint64_t frameOrdinal;
    uint64_t transferId;
    uint64_t receiverCommitEpoch;
    uint64_t roleCount;
    uint64_t refCount;
    uint64_t groupCount;
    uint64_t packetDigestLo;
    uint64_t packetDigestHi;
    uint64_t acceptedBudgetBytes;
    uint64_t observedFlags;
    uint64_t reserved;
} NrAckV1;
typedef struct NrSourceHeaderV1 {
    uint64_t size;
    uint64_t version;
    uint64_t requestNonceLo;
    uint64_t requestNonceHi;
    uint64_t pid;
    uint64_t creationFiletime;
    uint64_t applicationSubmissionId;
    uint64_t corpusFrameIndex;
    uint64_t sourceThread;
    uint64_t endThread;
    uint64_t publicWitnessId;
    uint64_t copyCount;
    uint64_t sourceModuleIndex;
    uint64_t observationFlags;
    uint64_t reserved[2];
} NrSourceHeaderV1;
typedef struct NrSourceCopyV1 {
    uint64_t actualContextAddress;
    uint64_t sourceAddress;
    uint64_t destinationAddress;
    uint64_t sourceSubresource;
    uint64_t destinationSubresource;
    uint64_t destinationXYpacked32;
    uint64_t destinationZ_and_boxPresent_packed32;
    uint64_t boxLeftTop_packed32;
    uint64_t boxFrontRight_packed32;
    uint64_t boxBottomBack_packed32;
    uint64_t qpcBefore;
    uint64_t qpcAfter;
    uint64_t callerRva;
    uint64_t actualCopySequence;
    uint64_t destinationWriteObservationEpoch;
    uint64_t observedMask;
} NrSourceCopyV1;
typedef struct NrPacketV1 {
    NrHeaderV1 header;
    NrIdentityV1 identity;
    NrBudgetV1 budget;
    NrRoleV1 roles[64];
    NrRefV1 refs[128];
    NrGroupV1 groups[8];
    NrParamV1 params[256];
    NrCallV1 calls[160];
    NrDescriptorV1 descriptors[64];
    NrDrawV1 draws[16];
    NrAliasV1 aliases[64];
    NrWriteV1 writes[128];
    NrFootprintV1 footprints[128];
    uint8_t constants[4096];
    NrModuleV1 modules[8];
} NrPacketV1;
typedef struct NrSourceWitnessV1 {
    NrSourceHeaderV1 header;
    NrSourceCopyV1 copies[4];
} NrSourceWitnessV1;
#pragma pack(pop)
#ifdef __cplusplus
extern "C" {
#endif
/* Input is a stable caller-owned CPU POD copy, byteCount exactly sizeof.
 * Return is metadata consistency only: no digest calculation or pointer probe.
 * Transfer validator accepts only complete host roles1..16, before FG admission.
 * Final validator requires separate final A and FG-augmented B CPU copies
 * (flags=1), both with completion observations, never ACKed packet mutation. */
int nr_validate_packet(const NrPacketV1*, size_t, NrError*);
int nr_validate_ack(const NrPacketV1*, const NrAckV1*, size_t, const uint64_t digest[2], NrError*);
int nr_validate_final_pair(const NrPacketV1*, const NrPacketV1*, NrError*);
int nr_ngx_not_failed(uint32_t);
int nr_match_replay(const NrPacketV1*, const NrPacketV1*, const uint64_t[2], const uint64_t[2], NrError*);
int nr_validate_source(const NrSourceWitnessV1*, size_t, NrError*);
#ifdef __cplusplus
}
#endif
#endif
