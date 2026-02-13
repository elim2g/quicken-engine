/*
 * QUICKEN Engine - Platform Socket Abstraction
 *
 * Winsock on Windows, BSD sockets on Linux.
 * Provides init/shutdown and monotonic time.
 */

#include "netcode/n_internal.h"

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

f64 n_platform_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

#endif
