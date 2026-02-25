/*
 * QUICKEN Engine - CPU Feature Detection
 *
 * Detects CPU SIMD capabilities at runtime using cpuid.
 * Call qk_cpuid_detect() once at boot before any SIMD dispatch.
 */

#ifndef QK_CPUID_H
#define QK_CPUID_H

#include "quicken.h"

/* CPU feature flags (bitfield) */
typedef enum {
    QK_CPU_SSE2   = (1 << 0),
    QK_CPU_SSE41  = (1 << 1),
    QK_CPU_AVX2   = (1 << 2),
    QK_CPU_FMA    = (1 << 3),
    QK_CPU_POPCNT = (1 << 4),
} qk_cpu_feature_t;

/* Run cpuid, cache the result, extract brand string.
 * Must be called before any qk_cpuid_has() or qk_cpuid_get_features(). */
void    qk_cpuid_detect(void);

/* Test whether a specific feature is available. */
bool    qk_cpuid_has(qk_cpu_feature_t feature);

/* Return the full bitmask of detected features. */
u32     qk_cpuid_get_features(void);

/* Return the CPU brand string (e.g. "AMD Ryzen 9 7950X").
 * Returns "" if called before qk_cpuid_detect(). */
const char *qk_cpuid_get_brand(void);

/* Print detected CPU info to stdout (brand + feature flags). */
void    qk_cpuid_print(void);

#endif /* QK_CPUID_H */
