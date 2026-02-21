/*
 * QUICKEN Engine - Client Diagnostics
 *
 * Per-frame diagnostic trace to diag_trace.log.
 * Console command: diag start|stop
 */

#ifndef CL_DIAG_H
#define CL_DIAG_H

#include "quicken.h"
#include "qk_types.h"
#include "netcode/qk_netcode.h"

void cl_diag_init(void);
void cl_diag_shutdown(void);

/* Console command handler (registered as "diag"). */
void cl_diag_cmd(i32 argc, const char **argv);

/* Write one frame of diagnostic data (call every client frame). */
void cl_diag_frame(f64 now, f32 real_dt, f32 server_accumulator,
                    f32 pred_accumulator, u8 local_client_id,
                    const qk_interp_state_t *interp,
                    bool has_prediction,
                    const qk_player_state_t *predicted_ps);

#endif /* CL_DIAG_H */
