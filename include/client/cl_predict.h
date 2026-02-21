/*
 * QUICKEN Engine - Client Prediction
 *
 * Fixed-rate client-side prediction, input buffering, server reconciliation.
 */

#ifndef CL_PREDICT_H
#define CL_PREDICT_H

#include "quicken.h"
#include "qk_types.h"
#include "core/qk_input.h"

/* Forward declaration */
typedef struct qk_phys_world qk_phys_world_t;

void cl_predict_init(void);
void cl_predict_reset(void);

/* Run prediction ticks for this frame's dt.
 * is_remote: true = remote server (server_time derived from local sequence),
 *            false = local loopback (server_time from qk_net_server_get_tick). */
void cl_predict_tick(const qk_input_state_t *input,
                      qk_phys_world_t *world, f32 dt, bool is_remote);

/* Check for server acknowledgement and reconcile if mispredicted. */
void cl_predict_reconcile(qk_phys_world_t *world);

/* Predicted player state (NULL if no prediction yet). */
const qk_player_state_t *cl_predict_get_state(void);

/* Mutable access (for restoring gameplay state on map load). */
qk_player_state_t *cl_predict_get_state_mut(void);

bool cl_predict_has_state(void);
f32  cl_predict_get_accumulator(void);
u32  cl_predict_get_cmd_sequence(void);

#endif /* CL_PREDICT_H */
