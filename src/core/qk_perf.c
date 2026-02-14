/*
 * QUICKEN Engine - Frame Profiling System
 *
 * Writes CSV data to quicken_perf.csv when enabled.
 * Logs every 64th frame to minimize I/O. Tracks rolling averages.
 * At shutdown, writes summary stats.
 *
 * Stubbed out for headless builds (no SDL3, no renderer).
 */

#include "core/qk_perf.h"

#ifdef QK_HEADLESS

void qk_perf_init(void) {}
void qk_perf_shutdown(void) {}
void qk_perf_begin_frame(void) {}
void qk_perf_end_frame(f32 cpu_frame_ms, f32 gpu_frame_ms, f32 world_ms,
                        f32 compose_ms, u32 draw_calls, u32 tris,
                        u32 swapchain_w, u32 swapchain_h,
                        f32 fence_wait_ms, f32 acquire_ms)
{
    QK_UNUSED(cpu_frame_ms); QK_UNUSED(gpu_frame_ms);
    QK_UNUSED(world_ms); QK_UNUSED(compose_ms);
    QK_UNUSED(draw_calls); QK_UNUSED(tris);
    QK_UNUSED(swapchain_w); QK_UNUSED(swapchain_h);
    QK_UNUSED(fence_wait_ms); QK_UNUSED(acquire_ms);
}
void qk_perf_set_enabled(bool enabled) { QK_UNUSED(enabled); }
void qk_perf_log_event(const char *fmt, ...) { QK_UNUSED(fmt); }

#else /* !QK_HEADLESS */

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define PERF_LOG_INTERVAL   64
#define PERF_ROLLING_SIZE   256

typedef struct {
    FILE   *file;
    bool    enabled;
    u64     frame_start_ticks;
    u64     perf_freq;

    u64     total_frames;
    u32     interval_counter;

    /* Rolling averages */
    f32     cpu_ms_history[PERF_ROLLING_SIZE];
    f32     gpu_ms_history[PERF_ROLLING_SIZE];
    f32     fence_ms_history[PERF_ROLLING_SIZE];
    f32     acquire_ms_history[PERF_ROLLING_SIZE];
    u32     rolling_index;
    u32     rolling_count;

    /* Lifetime stats */
    f64     sum_cpu_ms;
    f32     min_cpu_ms;
    f32     max_cpu_ms;
    f64     sum_gpu_ms;
    f32     min_gpu_ms;
    f32     max_gpu_ms;
} perf_state_t;

static perf_state_t s_perf;

void qk_perf_init(void)
{
    memset(&s_perf, 0, sizeof(s_perf));
    s_perf.perf_freq = SDL_GetPerformanceFrequency();
    s_perf.min_cpu_ms = 9999.0f;
    s_perf.min_gpu_ms = 9999.0f;
}

void qk_perf_shutdown(void)
{
    if (s_perf.file && s_perf.total_frames > 0) {
        f64 avg_cpu = s_perf.sum_cpu_ms / (f64)s_perf.total_frames;
        f64 avg_gpu = s_perf.sum_gpu_ms / (f64)s_perf.total_frames;
        f64 avg_fps = (avg_cpu > 0.001) ? 1000.0 / avg_cpu : 0.0;

        fprintf(s_perf.file, "# SUMMARY: frames=%llu avg_fps=%.1f "
                "cpu_avg=%.3f cpu_min=%.3f cpu_max=%.3f "
                "gpu_avg=%.3f gpu_min=%.3f gpu_max=%.3f\n",
                (unsigned long long)s_perf.total_frames, avg_fps,
                avg_cpu, (double)s_perf.min_cpu_ms, (double)s_perf.max_cpu_ms,
                avg_gpu, (double)s_perf.min_gpu_ms, (double)s_perf.max_gpu_ms);
    }

    if (s_perf.file) {
        fclose(s_perf.file);
        s_perf.file = NULL;
    }
    s_perf.enabled = false;
}

void qk_perf_set_enabled(bool enabled)
{
    if (enabled == s_perf.enabled) return;

    if (enabled) {
        s_perf.file = fopen("quicken_perf.csv", "w");
        if (!s_perf.file) return;

        fprintf(s_perf.file, "frame,cpu_ms,gpu_ms,world_ms,compose_ms,"
                "draw_calls,tris,swap_w,swap_h,fence_ms,acquire_ms,"
                "avg_cpu_ms,avg_gpu_ms,avg_fence_ms,avg_acquire_ms\n");

        s_perf.total_frames = 0;
        s_perf.interval_counter = 0;
        s_perf.rolling_index = 0;
        s_perf.rolling_count = 0;
        s_perf.sum_cpu_ms = 0.0;
        s_perf.sum_gpu_ms = 0.0;
        s_perf.min_cpu_ms = 9999.0f;
        s_perf.max_cpu_ms = 0.0f;
        s_perf.min_gpu_ms = 9999.0f;
        s_perf.max_gpu_ms = 0.0f;
        s_perf.enabled = true;
    } else {
        qk_perf_shutdown();
    }
}

void qk_perf_begin_frame(void)
{
    if (!s_perf.enabled) return;
    s_perf.frame_start_ticks = SDL_GetPerformanceCounter();
}

void qk_perf_end_frame(f32 cpu_frame_ms, f32 gpu_frame_ms, f32 world_ms,
                        f32 compose_ms, u32 draw_calls, u32 tris,
                        u32 swapchain_w, u32 swapchain_h,
                        f32 fence_wait_ms, f32 acquire_ms)
{
    if (!s_perf.enabled || !s_perf.file) return;

    s_perf.total_frames++;

    /* Update lifetime stats */
    s_perf.sum_cpu_ms += (f64)cpu_frame_ms;
    s_perf.sum_gpu_ms += (f64)gpu_frame_ms;
    if (cpu_frame_ms < s_perf.min_cpu_ms) s_perf.min_cpu_ms = cpu_frame_ms;
    if (cpu_frame_ms > s_perf.max_cpu_ms) s_perf.max_cpu_ms = cpu_frame_ms;
    if (gpu_frame_ms < s_perf.min_gpu_ms) s_perf.min_gpu_ms = gpu_frame_ms;
    if (gpu_frame_ms > s_perf.max_gpu_ms) s_perf.max_gpu_ms = gpu_frame_ms;

    /* Update rolling buffers */
    u32 ri = s_perf.rolling_index;
    s_perf.cpu_ms_history[ri] = cpu_frame_ms;
    s_perf.gpu_ms_history[ri] = gpu_frame_ms;
    s_perf.fence_ms_history[ri] = fence_wait_ms;
    s_perf.acquire_ms_history[ri] = acquire_ms;
    s_perf.rolling_index = (ri + 1) % PERF_ROLLING_SIZE;
    if (s_perf.rolling_count < PERF_ROLLING_SIZE) s_perf.rolling_count++;

    /* Only write every Nth frame */
    s_perf.interval_counter++;
    if (s_perf.interval_counter < PERF_LOG_INTERVAL) return;
    s_perf.interval_counter = 0;

    /* Compute rolling averages */
    f64 avg_cpu = 0, avg_gpu = 0, avg_fence = 0, avg_acquire = 0;
    u32 count = s_perf.rolling_count;
    for (u32 i = 0; i < count; i++) {
        avg_cpu     += (f64)s_perf.cpu_ms_history[i];
        avg_gpu     += (f64)s_perf.gpu_ms_history[i];
        avg_fence   += (f64)s_perf.fence_ms_history[i];
        avg_acquire += (f64)s_perf.acquire_ms_history[i];
    }
    avg_cpu     /= (f64)count;
    avg_gpu     /= (f64)count;
    avg_fence   /= (f64)count;
    avg_acquire /= (f64)count;

    fprintf(s_perf.file, "%llu,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%.3f,%.3f,"
            "%.3f,%.3f,%.3f,%.3f\n",
            (unsigned long long)s_perf.total_frames,
            (double)cpu_frame_ms, (double)gpu_frame_ms,
            (double)world_ms, (double)compose_ms,
            draw_calls, tris, swapchain_w, swapchain_h,
            (double)fence_wait_ms, (double)acquire_ms,
            avg_cpu, avg_gpu, avg_fence, avg_acquire);
}

void qk_perf_log_event(const char *fmt, ...)
{
    if (!s_perf.enabled || !s_perf.file) return;

    u64 now = SDL_GetPerformanceCounter();
    f64 elapsed_sec = (f64)(now - s_perf.frame_start_ticks) / (f64)s_perf.perf_freq;

    fprintf(s_perf.file, "# EVENT [%.3fs]: ", elapsed_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(s_perf.file, fmt, args);
    va_end(args);

    fprintf(s_perf.file, "\n");
    fflush(s_perf.file);
}

#endif /* QK_HEADLESS */
