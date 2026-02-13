/*
 * QUICKEN Netcode - Profile System Implementation
 *
 * Uses a field descriptor table with offsetof so that any field in
 * netcode_profile_t can be read/written by name at runtime. This powers
 * live tuning from a debug console, config files, or remote commands.
 */

#include "netcode/netcode_profile.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Field reflection table                                              */
/* ------------------------------------------------------------------ */

typedef enum { FIELD_TYPE_U32, FIELD_TYPE_F32 } field_type_t;

typedef struct {
    const char  *key;
    size_t       offset;
    field_type_t type;
} field_desc_t;

#define DESC_U32(field) { #field, offsetof(netcode_profile_t, field), FIELD_TYPE_U32 }
#define DESC_F32(field) { #field, offsetof(netcode_profile_t, field), FIELD_TYPE_F32 }

static const field_desc_t s_fields[] = {
    DESC_U32(jitter_buf_min),
    DESC_U32(jitter_buf_max),
    DESC_F32(jitter_adapt_rate),

    DESC_U32(predict_grace_ticks),
    DESC_U32(predict_decel_start),
    DESC_F32(predict_decel_rate),
    DESC_U32(predict_max_ticks),

    DESC_F32(correct_small_dist),
    DESC_F32(correct_large_dist),
    DESC_U32(correct_small_ticks),
    DESC_U32(correct_medium_ticks),
    DESC_F32(correct_air_mult),

    DESC_F32(interp_delay_ms),
    DESC_F32(extrap_max_ms),

    DESC_U32(input_redundancy),
};

#define FIELD_COUNT (sizeof(s_fields) / sizeof(s_fields[0]))

/* ------------------------------------------------------------------ */
/* Active profile storage                                              */
/* ------------------------------------------------------------------ */

static netcode_profile_t s_active;

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/* ------------------------------------------------------------------ */

/*
 * COMPETITIVE: Default. Optimized for players under ~35ms.
 * Tight jitter buffer, fast prediction onset, moderate deceleration.
 * Corrections blend quickly. Other players see smooth movement.
 *
 * At 128 Hz tick rate:
 *   2 ticks = ~15.6ms,  4 ticks = ~31.2ms
 *  10 ticks = ~78ms,   24 ticks = ~187ms
 */
static const netcode_profile_t s_preset_competitive = {
    .name               = "competitive",

    .jitter_buf_min     = 2,
    .jitter_buf_max     = 4,
    .jitter_adapt_rate  = 0.05f,

    .predict_grace_ticks = 1,
    .predict_decel_start = 10,
    .predict_decel_rate  = 0.15f,
    .predict_max_ticks   = 24,

    .correct_small_dist  = 1.0f,
    .correct_large_dist  = 3.0f,
    .correct_small_ticks = 5,
    .correct_medium_ticks = 10,
    .correct_air_mult    = 1.5f,

    .interp_delay_ms     = 15.625f,  /* 2 ticks at 128 Hz */
    .extrap_max_ms       = 50.0f,

    .input_redundancy    = 3,
};

/*
 * LENIENT: For mixed-region play, 80-100ms+ players.
 * Larger jitter buffer, more grace before prediction, gentler decel.
 * Longer correction blends. Tradeoff: more "shot around corners."
 */
static const netcode_profile_t s_preset_lenient = {
    .name               = "lenient",

    .jitter_buf_min     = 4,
    .jitter_buf_max     = 8,
    .jitter_adapt_rate  = 0.03f,

    .predict_grace_ticks = 3,
    .predict_decel_start = 16,
    .predict_decel_rate  = 0.08f,
    .predict_max_ticks   = 32,

    .correct_small_dist  = 2.0f,
    .correct_large_dist  = 5.0f,
    .correct_small_ticks = 8,
    .correct_medium_ticks = 16,
    .correct_air_mult    = 2.0f,

    .interp_delay_ms     = 31.25f,   /* 4 ticks at 128 Hz */
    .extrap_max_ms       = 100.0f,

    .input_redundancy    = 5,
};

/*
 * LAN: Minimal buffering for local play.
 * Nearly zero jitter expected. Predict aggressively on any gap
 * because it means something is actually wrong.
 */
static const netcode_profile_t s_preset_lan = {
    .name               = "lan",

    .jitter_buf_min     = 1,
    .jitter_buf_max     = 2,
    .jitter_adapt_rate  = 0.1f,

    .predict_grace_ticks = 0,
    .predict_decel_start = 8,
    .predict_decel_rate  = 0.2f,
    .predict_max_ticks   = 16,

    .correct_small_dist  = 0.5f,
    .correct_large_dist  = 2.0f,
    .correct_small_ticks = 4,
    .correct_medium_ticks = 8,
    .correct_air_mult    = 1.25f,

    .interp_delay_ms     = 7.8125f,  /* 1 tick at 128 Hz */
    .extrap_max_ms       = 25.0f,

    .input_redundancy    = 2,
};

static const netcode_profile_t *s_presets[NETCODE_PROFILE_COUNT] = {
    [NETCODE_PROFILE_COMPETITIVE] = &s_preset_competitive,
    [NETCODE_PROFILE_LENIENT]     = &s_preset_lenient,
    [NETCODE_PROFILE_LAN]         = &s_preset_lan,
};

/* ------------------------------------------------------------------ */
/* Field lookup helper                                                 */
/* ------------------------------------------------------------------ */

static const field_desc_t *find_field(const char *key)
{
    for (u32 i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(s_fields[i].key, key) == 0) {
            return &s_fields[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void netcode_profile_init(void)
{
    netcode_profile_load_preset(&s_active, NETCODE_PROFILE_COMPETITIVE);
}

void netcode_profile_load_preset(netcode_profile_t *profile,
                                 netcode_profile_preset_t preset)
{
    QUICKEN_ASSERT(preset < NETCODE_PROFILE_COUNT);
    memcpy(profile, s_presets[preset], sizeof(netcode_profile_t));
}

const netcode_profile_t *netcode_profile_active(void)
{
    return &s_active;
}

void netcode_profile_activate_preset(netcode_profile_preset_t preset)
{
    netcode_profile_load_preset(&s_active, preset);
}

void netcode_profile_activate_custom(const netcode_profile_t *profile)
{
    memcpy(&s_active, profile, sizeof(netcode_profile_t));
}

bool netcode_profile_set_u32(netcode_profile_t *profile, const char *key,
                             u32 value)
{
    const field_desc_t *f = find_field(key);
    if (!f || f->type != FIELD_TYPE_U32) return false;

    u32 *ptr = (u32 *)((u8 *)profile + f->offset);
    *ptr = value;
    return true;
}

bool netcode_profile_set_f32(netcode_profile_t *profile, const char *key,
                             f32 value)
{
    const field_desc_t *f = find_field(key);
    if (!f || f->type != FIELD_TYPE_F32) return false;

    f32 *ptr = (f32 *)((u8 *)profile + f->offset);
    *ptr = value;
    return true;
}

bool netcode_profile_get_u32(const netcode_profile_t *profile, const char *key,
                             u32 *out)
{
    const field_desc_t *f = find_field(key);
    if (!f || f->type != FIELD_TYPE_U32) return false;

    const u32 *ptr = (const u32 *)((const u8 *)profile + f->offset);
    *out = *ptr;
    return true;
}

bool netcode_profile_get_f32(const netcode_profile_t *profile, const char *key,
                             f32 *out)
{
    const field_desc_t *f = find_field(key);
    if (!f || f->type != FIELD_TYPE_F32) return false;

    const f32 *ptr = (const f32 *)((const u8 *)profile + f->offset);
    *out = *ptr;
    return true;
}

/* TODO: route through engine logging system when available */
void netcode_profile_dump(const netcode_profile_t *profile)
{
    fprintf(stderr, "--- netcode profile: %s ---\n", profile->name);

    for (u32 i = 0; i < FIELD_COUNT; i++) {
        const field_desc_t *f = &s_fields[i];
        const u8 *base = (const u8 *)profile;

        if (f->type == FIELD_TYPE_U32) {
            u32 v;
            memcpy(&v, base + f->offset, sizeof(u32));
            fprintf(stderr, "  %-24s = %u\n", f->key, v);
        } else {
            f32 v;
            memcpy(&v, base + f->offset, sizeof(f32));
            fprintf(stderr, "  %-24s = %.4f\n", f->key, (double)v);
        }
    }

    fprintf(stderr, "---\n");
}
