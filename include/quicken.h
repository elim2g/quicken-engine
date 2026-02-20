/*
 * QUICKEN Engine - Core Header
 *
 * Platform detection, base types (u8..f64), unified result codes, assert macro.
 * Include this in all engine source files.
 */

#ifndef QUICKEN_H
#define QUICKEN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Version
#define QUICKEN_VERSION_MAJOR 0
#define QUICKEN_VERSION_MINOR 1
#define QUICKEN_VERSION_PATCH 0

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #define QK_PLATFORM_WINDOWS
#elif defined(__linux__)
    #define QK_PLATFORM_LINUX
#else
    #error "Unsupported platform"
#endif

// Common types
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

typedef float    f32;
typedef double   f64;

// Unified result codes (all modules return this)
typedef enum {
    QK_SUCCESS = 0,

    // General errors
    QK_ERROR_INIT_FAILED,
    QK_ERROR_OUT_OF_MEMORY,
    QK_ERROR_INVALID_PARAM,
    QK_ERROR_NOT_FOUND,
    QK_ERROR_FULL,

    // Renderer errors
    QK_ERROR_VULKAN_INIT,
    QK_ERROR_NO_SUITABLE_GPU,
    QK_ERROR_SWAPCHAIN,
    QK_ERROR_PIPELINE,

    // Netcode errors
    QK_ERROR_SOCKET,
    QK_ERROR_TIMEOUT,
    QK_ERROR_REJECTED,

    QK_RESULT_COUNT
} qk_result_t;

// Assert macro
#ifdef QUICKEN_DEBUG
    #include <stdio.h>
    #include <stdlib.h>
    #define QK_ASSERT(expr) \
        do { \
            if (!(expr)) { \
                fprintf(stderr, "QK_ASSERT failed: %s (%s:%d)\n", \
                        #expr, __FILE__, __LINE__); \
                abort(); \
            } \
        } while (0)
#else
    #define QK_ASSERT(expr) ((void)0)
#endif

// Utility
#define QK_UNUSED(x) ((void)(x))
static const u32 QK_TARGET_FPS = 1000;

#endif // QUICKEN_H
