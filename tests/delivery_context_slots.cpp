// Derive test-hook slots from the installed SDK's C vtable declarations.
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11_4.h>
#include <cstddef>

extern "C" size_t deliveryContextSlot(unsigned method) {
    switch (method) {
    case 0:
        return offsetof(ID3D11DeviceContext4Vtbl, CopyResource) / sizeof(void*);
    case 1:
        return offsetof(ID3D11DeviceContext4Vtbl, CopySubresourceRegion) / sizeof(void*);
    case 2:
        return offsetof(ID3D11DeviceContext4Vtbl, Dispatch) / sizeof(void*);
    case 4:
        return offsetof(ID3D11DeviceContext4Vtbl, Signal) / sizeof(void*);
    default:
        return offsetof(ID3D11DeviceContext4Vtbl, Wait) / sizeof(void*);
    }
}

extern "C" size_t deliveryFenceImportSlot() {
    return offsetof(ID3D11Device5Vtbl, OpenSharedFence) / sizeof(void*);
}

extern "C" size_t deliveryFenceCompletedSlot() {
    return offsetof(ID3D11FenceVtbl, GetCompletedValue) / sizeof(void*);
}
