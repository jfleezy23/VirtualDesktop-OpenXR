// Same full lifecycle/overlap assertions with a private SDK and explicit test clock.
// This executable provides no real VD SDK or physical headset qualification.
#define VDXR_NATIVE_CONTROLLED_SDK 1
#define VDXR_DELIVERY_REGRESSION 1
#include "native_runtime_delivery_probe.cpp"
