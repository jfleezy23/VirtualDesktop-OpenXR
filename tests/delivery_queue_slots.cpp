#define CINTERFACE
#include <d3d12.h>
#include <cstddef>

extern "C" size_t deliveryQueueSignalSlot() {
    return offsetof(ID3D12CommandQueueVtbl, Signal) / sizeof(void*);
}
