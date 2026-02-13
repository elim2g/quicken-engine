/*
 * QUICKEN Engine - Input Sampling
 *
 * SDL3 event polling abstraction. Produces raw input state and usercmds.
 */

#ifndef QK_INPUT_H
#define QK_INPUT_H

#include "quicken.h"
#include "qk_types.h"

typedef struct {
    bool    keys[512];
    i32     mouse_dx;
    i32     mouse_dy;
    bool    mouse_buttons[5];
    bool    quit_requested;
} qk_input_state_t;

void            qk_input_poll(qk_input_state_t *state);
qk_usercmd_t   qk_input_build_usercmd(const qk_input_state_t *state, u32 server_time);

#endif /* QK_INPUT_H */
