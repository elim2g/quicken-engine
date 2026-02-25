/*
 * QUICKEN Engine - CPU Feature Detection Implementation
 *
 * Uses __cpuid/__cpuidex (MSVC) or __get_cpuid/__get_cpuid_count (GCC/Clang)
 * plus xgetbv to verify OS XSAVE support for AVX registers.
 */

#include "core/qk_cpuid.h"
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
    #include <intrin.h>
#else
    #include <cpuid.h>
#endif

/* Cached detection results */
static u32  s_features;
static char s_brand[64];
static bool s_detected;

/* Read XCR0 via xgetbv (needed to confirm OS saves AVX state). */
static u64 qk_xgetbv(u32 xcr) {
#ifdef _MSC_VER
    return _xgetbv(xcr);
#else
    u32 lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(xcr));
    return ((u64)hi << 32) | lo;
#endif
}

void qk_cpuid_detect(void) {
    if (s_detected) return;
    s_detected = true;
    s_features = 0;
    memset(s_brand, 0, sizeof(s_brand));

    /* --- Basic cpuid leaf 1: SSE2, SSE4.1, POPCNT, OSXSAVE, AVX, FMA --- */
    u32 eax, ebx, ecx, edx;

#ifdef _MSC_VER
    int regs[4];
    __cpuid(regs, 0);
    u32 max_leaf = (u32)regs[0];

    if (max_leaf >= 1) {
        __cpuid(regs, 1);
        eax = (u32)regs[0]; ebx = (u32)regs[1];
        ecx = (u32)regs[2]; edx = (u32)regs[3];
    } else {
        eax = ebx = ecx = edx = 0;
    }
#else
    u32 max_leaf = __get_cpuid_max(0, NULL);

    if (max_leaf >= 1) {
        __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    } else {
        eax = ebx = ecx = edx = 0;
    }
#endif

    /* SSE2: EDX bit 26 */
    if (edx & (1u << 26))
        s_features |= QK_CPU_SSE2;

    /* SSE4.1: ECX bit 19 */
    if (ecx & (1u << 19))
        s_features |= QK_CPU_SSE41;

    /* POPCNT: ECX bit 23 */
    if (ecx & (1u << 23))
        s_features |= QK_CPU_POPCNT;

    /* FMA: ECX bit 12 (checked below with AVX OS support) */
    bool cpu_fma = (ecx & (1u << 12)) != 0;

    /* OSXSAVE: ECX bit 27 — indicates OS supports xgetbv */
    bool osxsave = (ecx & (1u << 27)) != 0;

    /* AVX: ECX bit 28 (checked below with OS support) */
    bool cpu_avx = (ecx & (1u << 28)) != 0;

    /* --- Verify OS XSAVE support for YMM registers --- */
    bool os_avx = false;
    if (osxsave && cpu_avx) {
        u64 xcr0 = qk_xgetbv(0);
        /* Bits 1 (SSE state) and 2 (AVX state) must both be set */
        os_avx = ((xcr0 & 0x6) == 0x6);
    }

    if (os_avx && cpu_fma)
        s_features |= QK_CPU_FMA;

    /* --- Extended leaf 7: AVX2 --- */
    if (max_leaf >= 7) {
#ifdef _MSC_VER
        __cpuidex(regs, 7, 0);
        ebx = (u32)regs[1];
#else
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
#endif
        /* AVX2: EBX bit 5 */
        if (os_avx && (ebx & (1u << 5)))
            s_features |= QK_CPU_AVX2;
    }

    /* --- Brand string (leaves 0x80000002..0x80000004) --- */
    u32 ext_max;
#ifdef _MSC_VER
    __cpuid(regs, (int)0x80000000u);
    ext_max = (u32)regs[0];
#else
    ext_max = __get_cpuid_max(0x80000000u, NULL);
#endif

    if (ext_max >= 0x80000004u) {
        u32 brand_regs[12];
        for (u32 i = 0; i < 3; i++) {
#ifdef _MSC_VER
            __cpuid(regs, (int)(0x80000002u + i));
            brand_regs[i * 4 + 0] = (u32)regs[0];
            brand_regs[i * 4 + 1] = (u32)regs[1];
            brand_regs[i * 4 + 2] = (u32)regs[2];
            brand_regs[i * 4 + 3] = (u32)regs[3];
#else
            __get_cpuid(0x80000002u + i,
                        &brand_regs[i * 4 + 0], &brand_regs[i * 4 + 1],
                        &brand_regs[i * 4 + 2], &brand_regs[i * 4 + 3]);
#endif
        }
        memcpy(s_brand, brand_regs, 48);
        s_brand[48] = '\0';

        /* Trim leading spaces */
        char *p = s_brand;
        while (*p == ' ') p++;
        if (p != s_brand) memmove(s_brand, p, strlen(p) + 1);
    }
}

bool qk_cpuid_has(qk_cpu_feature_t feature) {
    return (s_features & (u32)feature) != 0;
}

u32 qk_cpuid_get_features(void) {
    return s_features;
}

const char *qk_cpuid_get_brand(void) {
    return s_brand;
}

void qk_cpuid_print(void) {
    printf("CPU: %s\n", s_brand[0] ? s_brand : "(unknown)");
    printf("SIMD: SSE2=%s SSE4.1=%s AVX2=%s FMA=%s POPCNT=%s\n",
           (s_features & QK_CPU_SSE2)   ? "yes" : "no",
           (s_features & QK_CPU_SSE41)  ? "yes" : "no",
           (s_features & QK_CPU_AVX2)   ? "yes" : "no",
           (s_features & QK_CPU_FMA)    ? "yes" : "no",
           (s_features & QK_CPU_POPCNT) ? "yes" : "no");
}
