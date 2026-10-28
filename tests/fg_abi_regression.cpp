#include "vdxr_fg_api.h"

#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    check(argc == 2, "module path required");
    HMODULE module = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(module != nullptr, "module loads from absolute path");
    check(GetProcAddress(module, "VDXRFG_GetApiV3") != nullptr, "V3 four-frame negotiation export exists");
    auto getApi = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(module, "VDXRFG_GetApiV1"));
    check(getApi != nullptr, "versioned API export exists");
    check(FAILED(getApi(nullptr)), "null negotiation output rejected");
    VDXRFG_ApiV1 api{};
    api.struct_size = sizeof(api) - 1;
    api.api_version = 1;
    check(FAILED(getApi(&api)), "undersized table rejected");
    api.struct_size = sizeof(api);
    api.api_version = 2;
    check(FAILED(getApi(&api)), "unknown version rejected");
    api.api_version = 1;
    check(SUCCEEDED(getApi(&api)), "API negotiation does not initialize GPU");
    check(api.Create && api.Submit && api.Retire && api.Reset && api.GetStatistics && api.Destroy,
          "all lifetime functions published");
    void* context = reinterpret_cast<void*>(1);
    VDXRFG_CreateInfoV1 create{};
    create.struct_size = sizeof(create);
    create.api_version = 1;
    check(FAILED(api.Create(&create, &context)) && !context, "invalid create clears context");
    check(FAILED(api.Create(&create, nullptr)), "null create output rejected");
    VDXRFG_SubmitInfoV1 submit{};
    submit.struct_size = sizeof(submit);
    submit.api_version = 1;
    VDXRFG_TicketV1 ticket{};
    ticket.token = 123;
    check(FAILED(api.Submit(nullptr, &submit, &ticket)) && !ticket.token,
          "rejected submission cannot leave a live ticket");
    check(FAILED(api.Retire(nullptr, 123)), "null retire rejected");
    check(FAILED(api.Reset(nullptr, 0)), "null reset rejected");
    check(FAILED(api.Destroy(nullptr)), "null destroy rejected");
    auto get3 = reinterpret_cast<VDXRFG_GetApiV3Fn>(GetProcAddress(module, "VDXRFG_GetApiV3"));
    VDXRFG_ApiV3 api3{};
    api3.struct_size = sizeof(api3);
    api3.api_version = VDXRFG_API_VERSION_3;
    check(SUCCEEDED(get3(&api3)) && api3.max_multiplier == 4, "V3 explicitly advertises four-frame support");
    check((api3.supported_create_flags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS) != 0,
          "real allocation omission requires a separate negotiated capability");
    check(FAILED(get3(nullptr)), "V3 null negotiation rejected");
    auto malformed = api3;
    --malformed.struct_size;
    check(FAILED(get3(&malformed)), "V3 undersized negotiation rejected");
    malformed = api3;
    malformed.api_version = 1;
    check(FAILED(get3(&malformed)), "V3 cannot reinterpret a legacy envelope");
    VDXRFG_CreateInfoV3 create3{};
    create3.struct_size = sizeof(create3);
    create3.api_version = VDXRFG_API_VERSION_3;
    create3.flags = VDXRFG_CREATE_OMIT_REAL_OUTPUTS;
    context = reinterpret_cast<void*>(1);
    check(api3.Create(&create3, &context) == VDXRFG_E_REJECTED && !context,
          "omission without host ownership rejects before touching device pointers");
    VDXRFG_SubmitInfoV3 submit3{};
    submit3.struct_size = sizeof(submit3);
    submit3.api_version = VDXRFG_API_VERSION_3;
    ticket.token = 1;
    check(api3.Submit(nullptr, &submit3, &ticket) == VDXRFG_E_REJECTED && !ticket.token,
          "V3 rejects without publishing stale ownership");
    create.multiplier = 4;
    context = reinterpret_cast<void*>(1);
    check(api.Create(&create, &context) == VDXRFG_E_REJECTED && !context,
          "legacy creation never enables four-frame payloads");
    FreeLibrary(module);
    std::puts("PASS: fg_abi negotiation and rejection");
}
