/*
 * QUICKEN Netcode - Tunable Profile System
 *
 * All netcode parameters live in a single struct that can be modified
 * at runtime via key-value setters. Systems read from the active profile
 * pointer — changes take effect immediately on the next tick.
 *
 * Built-in presets cover common scenarios. Custom profiles can be
 * constructed by loading a preset and tweaking individual values.
 */

#ifndef QUICKEN_NETCODE_PROFILE_H
#define QUICKEN_NETCODE_PROFILE_H

#include "quicken.h"

#define NETCODE_PROFILE_NAME_MAX 32

typedef struct netcode_profile {
    char name[NETCODE_PROFILE_NAME_MAX];

    /* --- Jitter buffer --- */
    u32 jitter_buf_min;         /* minimum buffer depth (ticks) */
    u32 jitter_buf_max;         /* maximum buffer depth (ticks) */
    f32 jitter_adapt_rate;      /* adaptation speed (0 = static, 1 = instant) */

    /* --- Server-side input prediction --- */
    u32 predict_grace_ticks;    /* ticks of empty buffer before predicting */
    u32 predict_decel_start;    /* predicted ticks before deceleration kicks in */
    f32 predict_decel_rate;     /* speed fraction removed per tick (0.0 - 1.0) */
    u32 predict_max_ticks;      /* freeze player beyond this many predicted ticks */

    /* --- Correction blending --- */
    f32 correct_small_dist;     /* threshold for "small" correction (units) */
    f32 correct_large_dist;     /* threshold for "snap" correction (units) */
    u32 correct_small_ticks;    /* blend duration for small corrections (ticks) */
    u32 correct_medium_ticks;   /* blend duration for medium corrections (ticks) */
    f32 correct_air_mult;       /* blend duration multiplier while airborne */

    /* --- Snapshot interpolation --- */
    f32 interp_delay_ms;        /* interpolation buffer delay (ms) */
    f32 extrap_max_ms;          /* maximum extrapolation time (ms) */

    /* --- Input redundancy --- */
    u32 input_redundancy;       /* past inputs to include per packet */
} netcode_profile_t;

typedef enum netcode_profile_preset {
    NETCODE_PROFILE_COMPETITIVE,    /* tight, for <35ms players */
    NETCODE_PROFILE_LENIENT,        /* relaxed, for 80-100ms+ players */
    NETCODE_PROFILE_LAN,            /* minimal buffering */
    NETCODE_PROFILE_COUNT
} netcode_profile_preset_t;

void                    netcode_profile_init(void);
void                    netcode_profile_load_preset(netcode_profile_t *profile,
                                                    netcode_profile_preset_t preset);
const netcode_profile_t *netcode_profile_active(void);
void                    netcode_profile_activate_preset(netcode_profile_preset_t preset);
void                    netcode_profile_activate_custom(const netcode_profile_t *profile);

/* Runtime tuning — set/get individual values by field name.
 * Operates on the provided profile, not a hidden global.
 * Returns true if key was found and type matched. */
bool netcode_profile_set_u32(netcode_profile_t *profile, const char *key, u32 value);
bool netcode_profile_set_f32(netcode_profile_t *profile, const char *key, f32 value);
bool netcode_profile_get_u32(const netcode_profile_t *profile, const char *key, u32 *out);
bool netcode_profile_get_f32(const netcode_profile_t *profile, const char *key, f32 *out);

void netcode_profile_dump(const netcode_profile_t *profile);

#endif /* QUICKEN_NETCODE_PROFILE_H */
