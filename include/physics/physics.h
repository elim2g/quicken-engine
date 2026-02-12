/*
 * QUICKEN Physics Module - Public API
 *
 * Player movement, collision detection, and game simulation.
 * Uses precise floating-point for cross-platform determinism.
 */

#ifndef QUICKEN_PHYSICS_H
#define QUICKEN_PHYSICS_H

#include "quicken.h"

/* Initialize physics system */
void physics_init(void);

/* Update physics simulation */
void physics_update(f32 delta_time);

/* Cleanup physics system */
void physics_shutdown(void);

#endif /* QUICKEN_PHYSICS_H */
