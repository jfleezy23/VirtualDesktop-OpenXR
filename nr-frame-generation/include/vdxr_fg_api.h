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

#ifdef __cplusplus
}
#endif
