#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
#define VDXRFG_NOEXCEPT noexcept
extern "C" {
#else
#define VDXRFG_NOEXCEPT
#endif

#define VDXRFG_API_VERSION 1u
#define VDXRFG_E_BUSY ((HRESULT)0x80040201L)
#define VDXRFG_E_REJECTED ((HRESULT)0x80040202L)
#define VDXRFG_E_POISONED ((HRESULT)0x80040203L)

/* All resources belong to one D3D12 device. The host owns allocations; the
 * module retains COM references. Handoffs use COMMON. Calls are serialized.
 * No borrowed fence or module may be released while its ticket is live.
 * Create returning POISONED with nonnull context retains uncertain GPU
 * initialization. The caller must retain the context/module until Destroy
 * succeeds; all other failed Create results clear the context. */
typedef struct VDXRFG_RectV1 {
    uint32_t x, y, width, height;
} VDXRFG_RectV1;

typedef struct VDXRFG_ViewV1 {
    float position[3];
    float orientation[4]; /* x, y, z, w */
    float fov[4];         /* left, right, up, down */
    VDXRFG_RectV1 rect;
} VDXRFG_ViewV1;

typedef struct VDXRFG_CreateInfoV1 {
    uint32_t struct_size, api_version;
    void* device;
    void* queue;
    void* input;
    void* outputs[2][3]; /* S1, S2, B; S2 must be null in 2X. */
    uint32_t width, height, format, multiplier;
    uint32_t release_state; /* COMMON only in V1 */
    uint32_t reserved[3];
} VDXRFG_CreateInfoV1;

typedef struct VDXRFG_SubmitInfoV1 {
    uint32_t struct_size, api_version;
    void* input;
    void* producer_fence;
    uint64_t producer_value, serial;
    int64_t display_time_ns, display_period_ns;
    int64_t target_time_ns[3];
    float fractions[2];
    VDXRFG_ViewV1 views[2];
} VDXRFG_SubmitInfoV1;

/* Output fences are borrowed. Retire is allowed only after the host settles
 * its copy/presentation consumers. The module checks its own GPU completion.
 * POISONED keeps an owned token but publishes no outputs. REJECTED/BUSY clears
 * the whole ticket before accepting work. Role bits: S1=1, S2=2, B=4. */
typedef struct VDXRFG_TicketV1 {
    uint64_t token, generation, previous_serial, current_serial;
    void* input_consumed_fence;
    void* output_ready_fence;
    uint64_t input_consumed_value, output_ready_value;
    uint32_t bank, role_mask, prime, reserved;
} VDXRFG_TicketV1;

typedef struct VDXRFG_StatisticsV1 {
    uint32_t struct_size, api_version;
    uint64_t captures, pairs, synthetic_frames, busy_rejections;
    uint64_t last_gpu_us, pack_gpu_us, flow_gpu_us, composition_gpu_us;
    uint32_t live_tickets, poisoned;
} VDXRFG_StatisticsV1;

typedef struct VDXRFG_ApiV1 {
    uint32_t struct_size, api_version;
    HRESULT(WINAPI* Create)(const VDXRFG_CreateInfoV1*, void**) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Submit)(void*, const VDXRFG_SubmitInfoV1*, VDXRFG_TicketV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Retire)(void*, uint64_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Reset)(void*, uint32_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* GetStatistics)(void*, VDXRFG_StatisticsV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Destroy)(void*) VDXRFG_NOEXCEPT;
} VDXRFG_ApiV1;

typedef HRESULT(WINAPI* VDXRFG_GetApiV1Fn)(VDXRFG_ApiV1*) VDXRFG_NOEXCEPT;

/* Additive creation negotiation. V1 remains unchanged, including GPU timing.
 * All lifecycle payloads below still use their V1 size/version envelopes.
 * A successful V2 Create accepts every requested flag. Timing fields are
 * unavailable (zero), not measured zero, when GPU_TIMING is not requested. */
#define VDXRFG_API_VERSION_2 2u
#define VDXRFG_CREATE_GPU_TIMING 1u
/* Keep B allocations, but leave them untouched and omit bit 4. The host
 * presents its independently protected real source. Prime still owns work
 * and fences with mask 0; 2X/3X pairs publish masks 1/3 respectively. */
#define VDXRFG_CREATE_HOST_OWNS_REAL 2u
/* Explicit alternative: FidelityFX optical flow at full per-eye input
 * resolution. Absence keeps NVIDIA Medium/50%. Hosts must require this
 * capability when selected; never silently mask it away or retry NVIDIA. */
#define VDXRFG_CREATE_FIDELITYFX_FULL 4u

typedef struct VDXRFG_CreateInfoV2 {
    uint32_t struct_size, api_version;
    VDXRFG_CreateInfoV1 base;
    uint32_t flags, reserved;
} VDXRFG_CreateInfoV2;

typedef struct VDXRFG_ApiV2 {
    uint32_t struct_size, api_version;
    uint32_t supported_create_flags, reserved;
    HRESULT(WINAPI* Create)(const VDXRFG_CreateInfoV2*, void**) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Submit)(void*, const VDXRFG_SubmitInfoV1*, VDXRFG_TicketV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Retire)(void*, uint64_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Reset)(void*, uint32_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* GetStatistics)(void*, VDXRFG_StatisticsV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Destroy)(void*) VDXRFG_NOEXCEPT;
} VDXRFG_ApiV2;

typedef HRESULT(WINAPI* VDXRFG_GetApiV2Fn)(VDXRFG_ApiV2*) VDXRFG_NOEXCEPT;

/* V3 is a separate negotiated envelope. V1/V2 sizes and semantics stay fixed.
 * Roles retain their legacy indices/bits: S1=0/1, S2=1/2, B=2/4, S3=3/8.
 * Presentation order for 4X is S1,S2,S3,B. Every published role shares the
 * full completion fence; retirement still requires all host consumers. */
#define VDXRFG_API_VERSION_3 3u
#define VDXRFG_CREATE_OMIT_REAL_OUTPUTS 8u

typedef struct VDXRFG_CreateInfoV3 {
    uint32_t struct_size, api_version;
    void* device;
    void* queue;
    void* input;
    void* outputs[2][4]; /* S1, S2, B, S3. Unused synthetic roles must be null. */
    uint32_t width, height, format, multiplier;
    uint32_t release_state;
    uint32_t flags;
    /* OMIT_REAL_OUTPUTS requires HOST_OWNS_REAL and null B in both banks.
     * Without it, B remains required even under HOST_OWNS_REAL. */
} VDXRFG_CreateInfoV3;

typedef struct VDXRFG_SubmitInfoV3 {
    uint32_t struct_size, api_version;
    void* input;
    void* producer_fence;
    uint64_t producer_value, serial;
    int64_t display_time_ns, display_period_ns;
    int64_t target_time_ns[4]; /* Presentation order, ending in real B. */
    float fractions[3];
    VDXRFG_ViewV1 views[2];
} VDXRFG_SubmitInfoV3;

typedef struct VDXRFG_ApiV3 {
    uint32_t struct_size, api_version;
    uint32_t supported_create_flags, max_multiplier;
    HRESULT(WINAPI* Create)(const VDXRFG_CreateInfoV3*, void**) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Submit)(void*, const VDXRFG_SubmitInfoV3*, VDXRFG_TicketV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Retire)(void*, uint64_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Reset)(void*, uint32_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* GetStatistics)(void*, VDXRFG_StatisticsV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Destroy)(void*) VDXRFG_NOEXCEPT;
} VDXRFG_ApiV3;

typedef HRESULT(WINAPI* VDXRFG_GetApiV3Fn)(VDXRFG_ApiV3*) VDXRFG_NOEXCEPT;

/* V4 explicitly reuses V3 Create/Submit envelopes and TicketV1. The ticket's
 * output_ready fence still covers the complete transaction and retirement.
 * Only GetOutputReadiness may return an earlier borrowed S1 fence. Query by
 * live context/token/role; unavailable roles are rejected. 2X, primes, repeats
 * and jointly composed outputs retain their full-completion readiness. */
#define VDXRFG_API_VERSION_4 4u

typedef struct VDXRFG_OutputReadinessV4 {
    uint32_t struct_size, api_version;
    void* fence;
    uint64_t value;
} VDXRFG_OutputReadinessV4;

typedef struct VDXRFG_ApiV4 {
    uint32_t struct_size, api_version;
    uint32_t supported_create_flags, max_multiplier;
    HRESULT(WINAPI* Create)(const VDXRFG_CreateInfoV3*, void**) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Submit)(void*, const VDXRFG_SubmitInfoV3*, VDXRFG_TicketV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Retire)(void*, uint64_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Reset)(void*, uint32_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* GetStatistics)(void*, VDXRFG_StatisticsV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Destroy)(void*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* GetOutputReadiness)(void*, uint64_t, uint32_t, VDXRFG_OutputReadinessV4*) VDXRFG_NOEXCEPT;
} VDXRFG_ApiV4;

typedef HRESULT(WINAPI* VDXRFG_GetApiV4Fn)(VDXRFG_ApiV4*) VDXRFG_NOEXCEPT;

/* V5 retains all V3 color semantics and optionally snapshots a canonical
 * R32_FLOAT stereo atlas. It is one mip/slice/sample, exactly color width /
 * height, same device, COMMON at handoff. Base views describe its normalized
 * final-color projection. Old envelopes and exports remain unchanged. */
#define VDXRFG_API_VERSION_5 5u
#define VDXRFG_DEPTH_CANONICAL_TRANSPORT 1u
/* Advertised only when the NVIDIA Medium endpoint preference consumer is
 * linked. V5 Create rejects a guide with FIDELITYFX_FULL. */
#define VDXRFG_DEPTH_ENDPOINT_PREFERENCE 2u
/* V5 Reset can change semantic opacity on guided NVIDIA contexts after all
 * tickets retire. Advertised only with the implemented endpoint consumer. */
#define VDXRFG_DEPTH_OPACITY_RESET 4u
/* Optional V5 NVIDIA camera reconstruction consumer. Advertised only when
 * that implementation is compiled; no public structure or table is extended. */
#define VDXRFG_DEPTH_RECONSTRUCTION 8u
#define VDXRFG_GUIDE_ENCODING_INVERSE_Z 1u
/* V5 only, with a non-null guide_input and endpoint-preference support.
 * Color is compositionally opaque regardless of stored alpha. This bypasses
 * only depth-preference alpha eligibility; output alpha remains unchanged.
 * Unsupported with FidelityFX. Initial mode; changing it requires negotiated
 * DEPTH_OPACITY_RESET and a successful V5 Reset with one mode flag below. */
#define VDXRFG_CREATE_COLOR_OPAQUE 16u
/* V5 only: requires DEPTH_RECONSTRUCTION, this supported create bit, a
 * non-null canonical guide_input and NVIDIA. Immutable for the context's
 * lifetime, including history and opacity resets. Missing depth on a Submit
 * preserves ordinary color FG rather than fabricating a guide. */
#define VDXRFG_CREATE_DEPTH_RECONSTRUCTION 32u
/* Reset reasons 0/1 retain their existing history-reset behavior and preserve
 * opacity. V5 with DEPTH_OPACITY_RESET additionally accepts exactly one of
 * these mode bits, optionally combined with reason 1. Unknown or conflicting
 * bits, legacy entrypoints/contexts, and no-guide contexts are rejected.
 * BUSY/rejection/failure does not change opacity or publish fresh lineage.
 * Success clears color/guide pair history, then changes opacity; the next
 * Submit primes a fresh pair. Submitted work retains its captured mode. */
#define VDXRFG_RESET_COLOR_OPAQUE 0x100u
#define VDXRFG_RESET_COLOR_ALPHA 0x200u

typedef struct VDXRFG_CreateInfoV5 {
    uint32_t struct_size, api_version;
    VDXRFG_CreateInfoV3 base;
    void* guide_input; /* Optional, retained until context destruction. */
} VDXRFG_CreateInfoV5;

typedef struct VDXRFG_SubmitInfoV5 {
    uint32_t struct_size, api_version;
    VDXRFG_SubmitInfoV3 base;
    uint64_t guide_serial, guide_epoch;
    /* Mask is 0 or 3. Mask 0 requires zero serial/epoch/encoding. Mask 3
     * requires retained guide_input, base.serial and nonzero epoch. Invalid
     * samples are -1; positive inverse view-axis meters include infinity=0. */
    uint32_t guide_mask, guide_encoding;
} VDXRFG_SubmitInfoV5;

typedef struct VDXRFG_ApiV5 {
    uint32_t struct_size, api_version;
    uint32_t supported_create_flags, max_multiplier;
    uint32_t supported_depth_capabilities, reserved;
    HRESULT(WINAPI* Create)(const VDXRFG_CreateInfoV5*, void**) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Submit)(void*, const VDXRFG_SubmitInfoV5*, VDXRFG_TicketV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Retire)(void*, uint64_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Reset)(void*, uint32_t) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* GetStatistics)(void*, VDXRFG_StatisticsV1*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* Destroy)(void*) VDXRFG_NOEXCEPT;
    HRESULT(WINAPI* GetOutputReadiness)(void*, uint64_t, uint32_t, VDXRFG_OutputReadinessV4*) VDXRFG_NOEXCEPT;
} VDXRFG_ApiV5;

typedef HRESULT(WINAPI* VDXRFG_GetApiV5Fn)(VDXRFG_ApiV5*) VDXRFG_NOEXCEPT;

#ifdef __cplusplus
}
#endif
