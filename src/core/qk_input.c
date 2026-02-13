/*
 * QUICKEN Input Sampling - Stub Implementation
 *
 * Will be replaced with real SDL3 input polling by the Principal Engineer.
 */

#include "core/qk_input.h"
#include <string.h>

void qk_input_poll(qk_input_state_t *state) {
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

qk_usercmd_t qk_input_build_usercmd(const qk_input_state_t *state, u32 server_time) {
    QK_UNUSED(state);
    qk_usercmd_t cmd = {0};
    cmd.server_time = server_time;
    return cmd;
}
