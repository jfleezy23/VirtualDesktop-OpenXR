#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11_4.h>
#include <d3d12.h>
extern "C" void* fgTailQueueSignal(void* queue) noexcept {
    return reinterpret_cast<void*>(static_cast<ID3D12CommandQueue*>(queue)->lpVtbl->Signal);
}
extern "C" void* prefixFenceEventAddress(void* fence) noexcept {
    return reinterpret_cast<void*>(static_cast<ID3D11Fence*>(fence)->lpVtbl->SetEventOnCompletion);
}
extern "C" void* recoveryFenceCompletedAddress(void* fence) noexcept {
    return reinterpret_cast<void*>(static_cast<ID3D11Fence*>(fence)->lpVtbl->GetCompletedValue);
}
extern "C" void* recoveryConsumerSignalAddress(void* context) noexcept {
    return reinterpret_cast<void*>(static_cast<ID3D11DeviceContext4*>(context)->lpVtbl->Signal);
}
