/*
 * QUICKEN Engine - Core Header
 *
 * Main engine header file. Include this in all engine source files.
 */

#ifndef QUICKEN_H
#define QUICKEN_H

#include <stdint.h>
#include <stdbool.h>

/* Version information */
#define QUICKEN_VERSION_MAJOR 0
#define QUICKEN_VERSION_MINOR 1
#define QUICKEN_VERSION_PATCH 0

/* Build configuration */
#ifdef QUICKEN_DEBUG
    #define QUICKEN_ASSERT(expr) \
        do { \
            if (!(expr)) { \
                fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", \
                        #expr, __FILE__, __LINE__); \
                abort(); \
            } \
        } while (0)
#else
    #define QUICKEN_ASSERT(expr) ((void)0)
#endif

/* Platform detection */
#if defined(_WIN32) || defined(_WIN64)
    #define QUICKEN_PLATFORM_WINDOWS
#elif defined(__linux__)
    #define QUICKEN_PLATFORM_LINUX
#else
    #error "Unsupported platform"
#endif

/* Common types */
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

/* Performance targets */
#define QUICKEN_TARGET_FPS 1000
#define QUICKEN_TARGET_FRAMETIME_MS (1000.0 / QUICKEN_TARGET_FPS)

#endif /* QUICKEN_H */
