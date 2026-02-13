/*
 * QUICKEN Engine - Platform Socket Abstraction
 *
 * Winsock on Windows, BSD sockets on Linux.
 * Provides init/shutdown and monotonic time.
 */

#include "n_internal.h"

#ifdef QK_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

static bool s_wsa_initialized = false;

bool n_platform_init(void) {
    if (s_wsa_initialized) return true;

    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) return false;

    s_wsa_initialized = true;
    return true;
}

void n_platform_shutdown(void) {
    if (s_wsa_initialized) {
        WSACleanup();
        s_wsa_initialized = false;
    }
}

#include <windows.h>

static bool   s_timer_initialized = false;
static LARGE_INTEGER s_timer_freq;
static LARGE_INTEGER s_timer_start;

f64 n_platform_time(void) {
    if (!s_timer_initialized) {
        QueryPerformanceFrequency(&s_timer_freq);
        QueryPerformanceCounter(&s_timer_start);
        s_timer_initialized = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (f64)(now.QuadPart - s_timer_start.QuadPart) / (f64)s_timer_freq.QuadPart;
}

#else /* Linux */

#include <time.h>

bool n_platform_init(void) {
    return true;
}

void n_platform_shutdown(void) {
}

static bool            s_linux_timer_initialized = false;
static struct timespec s_linux_timer_start;

f64 n_platform_time(void) {
    if (!s_linux_timer_initialized) {
        clock_gettime(CLOCK_MONOTONIC, &s_linux_timer_start);
        s_linux_timer_initialized = true;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    f64 sec  = (f64)(ts.tv_sec  - s_linux_timer_start.tv_sec);
    f64 nsec = (f64)(ts.tv_nsec - s_linux_timer_start.tv_nsec);
    return sec + nsec * 1e-9;
}

#endif

/* ---- PRNG (shared state across all TUs) ---- */

static u32 s_rng_state = 2166136261u;

u32 n_random_u32(void) {
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 17;
    s_rng_state ^= s_rng_state << 5;
    s_rng_state *= 16777619u;
    return s_rng_state;
}
