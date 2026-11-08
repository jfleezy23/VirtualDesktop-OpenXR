// Controlled SDK only: real public frames, NR/FG, delivery copies and worker.
// The harness parks one forwarded S1 Begin while the next public producer runs.
#define VDXR_NATIVE_CONTROLLED_SDK 1
#define VDXR_DELIVERY_REGRESSION 1
#define VDXR_PREBEGIN_OVERLAP_REGRESSION 1
#include "native_runtime_delivery_probe.cpp"
