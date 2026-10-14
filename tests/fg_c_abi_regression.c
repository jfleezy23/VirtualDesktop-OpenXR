#include "vdxr_fg_api.h"

#include <stddef.h>
#include <stdio.h>

_Static_assert(sizeof(void*) == 8, "V1 module is x64 only");
_Static_assert(sizeof(VDXRFG_ViewV1) == 60, "view layout");
_Static_assert(offsetof(VDXRFG_ViewV1, rect) == 44, "rectangle offset");
_Static_assert(sizeof(VDXRFG_CreateInfoV1) == 112, "create layout");
_Static_assert(sizeof(VDXRFG_SubmitInfoV1) == 208, "submit layout");
_Static_assert(offsetof(VDXRFG_SubmitInfoV1, views) == 88, "views offset");
_Static_assert(sizeof(VDXRFG_TicketV1) == 80, "ticket layout");
_Static_assert(sizeof(VDXRFG_StatisticsV1) == 80, "statistics layout");
_Static_assert(sizeof(VDXRFG_ApiV1) == 56, "function table layout");

int main(int argc, char** argv) {
    HMODULE module;
    VDXRFG_GetApiV1Fn get_api;
    VDXRFG_ApiV1 api = {0};
    if (argc != 2 ||
        !(module = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))) {
        fprintf(stderr, "FAIL: C consumer cannot load module\n");
        return 1;
    }
    get_api = (VDXRFG_GetApiV1Fn)GetProcAddress(module, "VDXRFG_GetApiV1");
    api.struct_size = sizeof(api);
    api.api_version = VDXRFG_API_VERSION;
    if (!get_api || FAILED(get_api(&api)) || !api.Create || !api.Submit || !api.Retire || !api.Reset ||
        !api.GetStatistics || !api.Destroy) {
        fprintf(stderr, "FAIL: C function-table negotiation\n");
        FreeLibrary(module);
        return 1;
    }
    FreeLibrary(module);
    puts("PASS: C11 x64 ABI sizes, offsets and real DLL calling convention");
    return 0;
}
