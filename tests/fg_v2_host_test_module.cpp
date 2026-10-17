#include "vdxr_fg_api.h"

namespace {
    unsigned mode{}, legacyCreates{}, modernCreates{}, destroyed{}, createFlags{};
    HRESULT WINAPI legacy(const VDXRFG_CreateInfoV1*, void** output) noexcept {
        ++legacyCreates;
        *output = nullptr;
        return E_NOTIMPL;
    }
    HRESULT WINAPI modern(const VDXRFG_CreateInfoV2* info, void** output) noexcept {
        ++modernCreates;
        createFlags = info->flags;
        *output = mode == 6 ? reinterpret_cast<void*>(1) : nullptr;
        return mode == 6 ? VDXRFG_E_POISONED : mode == 7 ? S_OK : E_NOTIMPL;
    }
    HRESULT WINAPI submit(void*, const VDXRFG_SubmitInfoV1*, VDXRFG_TicketV1* output) noexcept {
        *output = {};
        return E_NOTIMPL;
    }
    HRESULT WINAPI retire(void*, uint64_t) noexcept {
        return S_OK;
    }
    HRESULT WINAPI reset(void*, uint32_t) noexcept {
        return S_OK;
    }
    HRESULT WINAPI stats(void*, VDXRFG_StatisticsV1*) noexcept {
        return S_OK;
    }
    HRESULT WINAPI destroy(void*) noexcept {
        ++destroyed;
        return S_OK;
    }
} // namespace

extern "C" __declspec(dllexport) void WINAPI SetMode(unsigned value) {
    mode = value;
    legacyCreates = modernCreates = destroyed = 0;
    createFlags = 0;
}
extern "C" __declspec(dllexport) unsigned WINAPI Count(unsigned kind) {
    return kind == 0 ? legacyCreates : kind == 1 ? modernCreates : kind == 2 ? destroyed : createFlags;
}
extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV1(VDXRFG_ApiV1* output) noexcept {
    *output = {sizeof(*output), 1, legacy, submit, retire, reset, stats, destroy};
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI VDXRFG_GetApiV2(VDXRFG_ApiV2* output) noexcept {
    *output = {sizeof(*output), 2, VDXRFG_CREATE_GPU_TIMING, 0, modern, submit, retire, reset, stats, destroy};
    switch (mode) {
    case 0:
        return VDXRFG_E_REJECTED;
    case 1:
        --output->struct_size;
        break;
    case 2:
        output->api_version = 1;
        break;
    case 3:
        output->reserved = 1;
        break;
    case 4:
        output->Submit = nullptr;
        break;
    }
    return S_OK;
}
