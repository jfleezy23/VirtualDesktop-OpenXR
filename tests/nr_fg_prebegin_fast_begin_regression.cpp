// Negative control: release current Begin while the actual future Submit still
// owns the producer lease, then require EVERY current SDK End to return.
#define VDXR_PREBEGIN_FAST_REGRESSION 1
#include "nr_fg_prebegin_overlap_regression.cpp"
