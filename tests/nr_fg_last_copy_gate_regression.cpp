// Public worker + actual FG module + real independent delivery GPU queue.
// The future Submit retains its real producer lease while only the final
// current-copy GPU gate is released. Require actual End GPU completion.
#define VDXR_LAST_COPY_GATE_REGRESSION 1
#include "nr_fg_prebegin_fast_begin_regression.cpp"
