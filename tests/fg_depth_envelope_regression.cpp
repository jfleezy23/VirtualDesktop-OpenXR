#include "vdxr_fg_depth_validation.hpp"

#include <cstdlib>
#include <cstdio>

static void require(bool condition) {
    if (!condition)
        std::abort();
}

int main() {
    VDXRFG_SubmitInfoV5 info{};
    info.struct_size = sizeof(info);
    info.api_version = VDXRFG_API_VERSION_5;
    info.base.struct_size = sizeof(info.base);
    info.base.api_version = VDXRFG_API_VERSION_3;
    info.base.serial = 10;
    require(vdxrfg::validDepthEnvelope(info, false));
    info.guide_serial = 10;
    require(!vdxrfg::validDepthEnvelope(info, true));
    info.guide_mask = 3;
    info.guide_epoch = 1;
    info.guide_encoding = VDXRFG_GUIDE_ENCODING_INVERSE_Z;
    require(vdxrfg::validDepthEnvelope(info, true));
    require(!vdxrfg::validDepthEnvelope(info, false));
    info.guide_mask = 1;
    require(!vdxrfg::validDepthEnvelope(info, true));
    info.guide_mask = 3;
    info.guide_serial = 9;
    require(!vdxrfg::validDepthEnvelope(info, true));
    info.guide_serial = 10;
    info.guide_epoch = 0;
    require(!vdxrfg::validDepthEnvelope(info, true));
    info.guide_epoch = 1;
    info.guide_encoding = 2;
    require(!vdxrfg::validDepthEnvelope(info, true));

    require(vdxrfg::compatibleDepthPair(10, 10, 1, 11, 1));
    require(!vdxrfg::compatibleDepthPair(10, 0, 1, 11, 1));
    require(!vdxrfg::compatibleDepthPair(10, 10, 1, 12, 1));
    require(!vdxrfg::compatibleDepthPair(10, 10, 1, 11, 2));
    require(!vdxrfg::compatibleDepthPair(0, 0, 0, 1, 1));
    std::puts("PASS: V5 depth envelope and consecutive guide identities");
}
