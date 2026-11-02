#define CINTERFACE
#define D3D11_NO_HELPERS
#define D3D11_VIDEO_NO_HELPERS
#include <d3d11_3.h>
extern "C" void* contextMethodAddress(void* pointer, unsigned method) {
    auto* context = static_cast<ID3D11DeviceContext4*>(pointer);
    if (method == 0)
        return reinterpret_cast<void*>(context->lpVtbl->Flush);
    if (method == 1)
        return reinterpret_cast<void*>(context->lpVtbl->Signal);
    if (method == 2)
        return reinterpret_cast<void*>(context->lpVtbl->Wait);
    return nullptr;
}
