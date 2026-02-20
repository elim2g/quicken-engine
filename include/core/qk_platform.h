/*
 * QUICKEN Engine - Platform Abstraction
 *
 * Monotonic time, sleep. Thin wrappers over OS/SDL3 calls.
 */

#ifndef QK_PLATFORM_H
#define QK_PLATFORM_H

#include "quicken.h"

f64  qk_platform_time_now(void);    // monotonic time in seconds
void qk_platform_sleep(u32 ms);

#endif // QK_PLATFORM_H
