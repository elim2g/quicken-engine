/*
 * QUICKEN Engine - SIMD Tier Dispatch
 *
 * Provides a simple tier enum for choosing implementations at runtime.
 * Reads the cached cpuid result from qk_cpuid.
 */

#ifndef QK_SIMD_DISPATCH_H
#define QK_SIMD_DISPATCH_H

#include "quicken.h"
#include "core/qk_cpuid.h"

typedef enum {
    QK_SIMD_SCALAR = 0,   /* No SIMD (shouldn't happen on x64, but safe fallback) */
    QK_SIMD_SSE2   = 1,   /* SSE2 baseline (guaranteed on all x86_64) */
    QK_SIMD_SSE41  = 2,   /* SSE4.1 */
    QK_SIMD_AVX2   = 3,   /* AVX2 + FMA */
} qk_simd_tier_t;

/* Returns the highest SIMD tier the CPU supports.
 * Must be called after qk_cpuid_detect(). */
static inline qk_simd_tier_t qk_simd_get_tier(void) {
    u32 features = qk_cpuid_get_features();

    if ((features & QK_CPU_AVX2) && (features & QK_CPU_FMA))
        return QK_SIMD_AVX2;
    if (features & QK_CPU_SSE41)
        return QK_SIMD_SSE41;
    if (features & QK_CPU_SSE2)
        return QK_SIMD_SSE2;

    return QK_SIMD_SCALAR;
}

/* Human-readable tier name for logging. */
static inline const char *qk_simd_tier_name(qk_simd_tier_t tier) {
    switch (tier) {
        case QK_SIMD_SCALAR: return "Scalar";
        case QK_SIMD_SSE2:   return "SSE2";
        case QK_SIMD_SSE41:  return "SSE4.1";
        case QK_SIMD_AVX2:   return "AVX2";
    }
    return "Unknown";
}

#endif /* QK_SIMD_DISPATCH_H */
