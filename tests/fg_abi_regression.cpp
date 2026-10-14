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
    FreeLibrary(module);
    std::puts("PASS: fg_abi negotiation and rejection");
}
