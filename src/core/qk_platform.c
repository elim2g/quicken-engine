/*
 * QUICKEN Platform Abstraction - Real Implementation
 *
 * Monotonic time via SDL3 (client) or OS APIs (headless server).
 * Sleep via SDL3 or OS APIs.
 */

#include "core/qk_platform.h"

#ifdef QK_HEADLESS

/* ---- Headless (no SDL3) ---- */

#ifdef QK_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>

    static i64 s_qpc_freq = 0;
    static i64 s_qpc_start = 0;

    f64 qk_platform_time_now(void) {
        if (s_qpc_freq == 0) {
            LARGE_INTEGER freq, now;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            s_qpc_freq = freq.QuadPart;
            s_qpc_start = now.QuadPart;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (f64)(now.QuadPart - s_qpc_start) / (f64)s_qpc_freq;
    }

    void qk_platform_sleep(u32 ms) {
        Sleep(ms);
    }

#else /* Linux */
    #include <time.h>

    static struct timespec s_start = {0, 0};

    f64 qk_platform_time_now(void) {
        if (s_start.tv_sec == 0 && s_start.tv_nsec == 0) {
            clock_gettime(CLOCK_MONOTONIC, &s_start);
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (f64)(now.tv_sec - s_start.tv_sec) +
               (f64)(now.tv_nsec - s_start.tv_nsec) * 1e-9;
    }

    void qk_platform_sleep(u32 ms) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

#endif /* QK_PLATFORM_WINDOWS */

#else /* !QK_HEADLESS -- use SDL3 */

#include <SDL3/SDL_timer.h>

static u64 s_sdl_start = 0;

f64 qk_platform_time_now(void) {
    if (s_sdl_start == 0) {
        s_sdl_start = SDL_GetPerformanceCounter();
    }
    u64 now = SDL_GetPerformanceCounter();
    return (f64)(now - s_sdl_start) / (f64)SDL_GetPerformanceFrequency();
}

void qk_platform_sleep(u32 ms) {
    SDL_Delay(ms);
}

#endif /* QK_HEADLESS */
