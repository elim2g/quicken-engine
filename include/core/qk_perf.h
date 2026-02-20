/*
 * QUICKEN Engine - Frame Profiling System
 *
 * File-based profiler that writes CSV data to quicken_perf.csv.
 * Controlled via the r_perflog cvar. Never writes to console/stderr.
 */

#ifndef QK_PERF_H
#define QK_PERF_H

#include "quicken.h"

void qk_perf_init(void);
void qk_perf_shutdown(void);
void qk_perf_begin_frame(void);
void qk_perf_end_frame(f32 cpu_frame_ms, f32 gpu_frame_ms, f32 world_ms,
                        f32 compose_ms, u32 draw_calls, u32 tris,
                        u32 swapchain_w, u32 swapchain_h,
                        f32 fence_wait_ms, f32 acquire_ms);
void qk_perf_set_enabled(bool enabled);
void qk_perf_log_event(const char *fmt, ...);

#endif // QK_PERF_H
