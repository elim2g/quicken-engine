/*
 * QUICKEN Engine - Netcode Wire Format Types
 *
 * Compact, quantized types for network transmission.
 * n_entity_state_t is the on-the-wire entity representation.
 * n_input_t is the on-the-wire input representation.
 */

#ifndef N_TYPES_H
#define N_TYPES_H

#include "quicken.h"

/* Entity state for network transmission (22 bytes, quantized) */
typedef struct {
    i16     pos_x, pos_y, pos_z;    /* fixed-point 13.3: +/-4096 at 0.125 precision */
    i16     vel_x, vel_y, vel_z;    /* 1 unit/sec precision */
    u16     yaw;                     /* 0..65535 -> 0..360 degrees */
    u16     pitch;                   /* 0..65535 -> 0..360 degrees */
    u8      entity_type;
    u8      flags;
    u8      health;                  /* 0..255, clamped from i16 */
    u8      armor;                   /* 0..255 */
    u8      weapon;
    u8      ammo;                    /* current weapon ammo only */
} n_entity_state_t;

/* Input for network transmission (compact) */
typedef struct {
    i8      forward_move;            /* -127..127 */
    i8      side_move;               /* -127..127 */
    u16     yaw;                     /* quantized angle */
    u16     pitch;                   /* quantized angle */
    u16     buttons;
    u8      weapon_select;
} n_input_t;

#endif /* N_TYPES_H */
