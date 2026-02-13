/*
 * QUICKEN Engine - Shared Input Types
 *
 * Canonical input command and movement state types used by physics,
 * netcode, and gameplay. No module owns these — they are engine-wide.
 */

#ifndef QUICKEN_CORE_INPUT_H
#define QUICKEN_CORE_INPUT_H

#include "quicken.h"

/* Button bitfield */
#define QUICKEN_BUTTON_FIRE     (1 << 0)
#define QUICKEN_BUTTON_JUMP     (1 << 1)
#define QUICKEN_BUTTON_CROUCH   (1 << 2)
#define QUICKEN_BUTTON_USE      (1 << 3)
#define QUICKEN_BUTTON_RELOAD   (1 << 4)
#define QUICKEN_BUTTON_WALK     (1 << 5)

typedef enum quicken_move_state {
    QUICKEN_MOVE_GROUNDED = 0,
    QUICKEN_MOVE_AIRBORNE,      /* jumped */
    QUICKEN_MOVE_CROUCHSLIDE,   /* TPM crouchslide */
    QUICKEN_MOVE_FALLING,       /* walked off edge, no jump */
    QUICKEN_MOVE_COUNT
} quicken_move_state_t;

typedef struct quicken_input {
    u32 tick;
    i8  forward;            /* -127 to 127 */
    i8  side;               /* -127 to 127 */
    i16 angles[3];          /* quantized pitch / yaw / roll */
    u16 buttons;            /* QUICKEN_BUTTON_* bitfield */
} quicken_input_t;

#endif /* QUICKEN_CORE_INPUT_H */
