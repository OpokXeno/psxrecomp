#ifndef PSX_NATIVE_RENDER_BASELINE_RUNTIME_H
#define PSX_NATIVE_RENDER_BASELINE_RUNTIME_H

#include "native_render_baseline.h"

void native_render_baseline_runtime_reset(void);
void native_render_baseline_runtime_arm(void);
NativeRenderBaselineReason native_render_baseline_runtime_observe(
    NativeRenderBaselineSnapshot *snapshot);

#endif
