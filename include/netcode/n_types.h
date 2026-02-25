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

// Entity state for network transmission (22 bytes, quantized)
typedef struct {
    i16     pos_x, pos_y, pos_z;    // fixed-point 15.1: +/-16383 at 0.5 precision
    i16     vel_x, vel_y, vel_z;    // 1 unit/sec precision
    u16     yaw;                     // 0..65535 -> 0..360 degrees
    u16     pitch;                   // 0..65535 -> 0..360 degrees
    u8      entity_type;
    u8      flags;                   // QK_ENT_FLAG_* bits (see qk_types.h)
    u8      health;                  // 0..255, clamped from i16
    u8      armor;                   // 0..255
    u8      weapon;
    u8      ammo;                    // current weapon ammo only
} n_entity_state_t;

_Static_assert(sizeof(n_entity_state_t) == 22,
               "entity state must be exactly 22 bytes for wire format");

// Full-precision player state for local client reconciliation.
// Sent only to the owning client, NOT to other players.
typedef struct {
    f32     pos_x, pos_y, pos_z;        // full float precision
    f32     vel_x, vel_y, vel_z;        // full float precision
    f32     yaw, pitch;                 // full float precision
    u32     command_time;               // tick counter for replay
    u32     last_jump_tick;             // double-jump timing
    u32     weapon_time;                // weapon cooldown
    u32     switch_time;                // weapon switch cooldown
    u8      splash_slick_ticks;         // rocket jump slick
    u8      skim_ticks;                 // ground skim
    u8      autohop_cooldown;           // autohop gating
    u8      jump_buffer_ticks;          // jump input buffer
    u8      flags;                      // on_ground, jump_held, teleport_bit
    u8      weapon;                     // current weapon
    u8      pending_weapon;             // weapon being switched to
    u8      queued_weapon;              // weapon queued after switch
    i16     health;
    i16     armor;
    u16     ammo[QK_WEAPON_COUNT];      // QK_WEAPON_COUNT = 4
} n_player_state_t;

_Static_assert(sizeof(n_player_state_t) == 68,
               "n_player_state_t size changed — update wire format");

// Input for network transmission (compact)
typedef struct {
    i8      forward_move;            // -127..127
    i8      side_move;               // -127..127
    u16     yaw;                     // quantized angle
    u16     pitch;                   // quantized angle
    u16     buttons;
    u8      weapon_select;
} n_input_t;

_Static_assert(sizeof(n_input_t) == 10,
               "input struct size changed — update wire format");

#endif /* N_TYPES_H */
