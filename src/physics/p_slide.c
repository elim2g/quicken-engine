/*
 * QUICKEN Engine - SlideMove and StepSlideMove
 *
 * Collision response: clip velocity against planes, slide along surfaces,
 * step up stairs/ledges.
 */

#include "p_internal.h"

/* ---- Clip velocity off a collision plane ---- */

vec3_t p_clip_velocity(vec3_t velocity, vec3_t normal, f32 overbounce) {
    f32 backoff = vec3_dot(velocity, normal) * overbounce;

    vec3_t result;
    result.x = velocity.x - normal.x * backoff;
    result.y = velocity.y - normal.y * backoff;
    result.z = velocity.z - normal.z * backoff;

    /* Ensure no tiny residual into-plane velocity (numerical cleanup) */
    f32 adjust = vec3_dot(result, normal);
    if (adjust < 0.0f) {
        result.x -= normal.x * adjust;
        result.y -= normal.y * adjust;
        result.z -= normal.z * adjust;
    }

    return result;
}

/* ---- SlideMove: move and clip against collision planes ---- */

bool p_slide_move(qk_player_state_t *ps, const qk_phys_world_t *world,
                  f32 dt, i32 max_bumps) {
    vec3_t planes[P_MAX_CLIP_PLANES];
    i32 num_planes = 0;
    vec3_t primal_velocity = ps->velocity;
    f32 time_left = dt;

    for (i32 bump = 0; bump < max_bumps; bump++) {
        /* Where we want to go this sub-step */
        vec3_t end;
        end.x = ps->origin.x + ps->velocity.x * time_left;
        end.y = ps->origin.y + ps->velocity.y * time_left;
        end.z = ps->origin.z + ps->velocity.z * time_left;

        qk_trace_result_t trace = p_trace_world(world, ps->origin, end,
                                                 ps->mins, ps->maxs);

        if (trace.all_solid) {
            /* Stuck in solid -- kill velocity */
            ps->velocity = (vec3_t){0.0f, 0.0f, 0.0f};
            return true;
        }

        if (trace.fraction > 0.0f) {
            /* Move to the contact point */
            ps->origin = trace.end_pos;
        }

        if (trace.fraction == 1.0f) {
            break; /* Moved the full distance without hitting anything */
        }

        /* Reduce remaining time */
        time_left -= time_left * trace.fraction;

        /* Record this clip plane, but skip if nearly duplicate of an
           existing plane. Curved surfaces decomposed into many small
           brushes produce near-identical normals per frame; accumulating
           them as separate clip planes causes the velocity to be killed
           (classic "corner trap" between two nearly-parallel planes). */
        {
            bool duplicate = false;
            for (i32 k = 0; k < num_planes; k++) {
                if (vec3_dot(trace.hit_normal, planes[k]) > 0.99f) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                /* Re-clip velocity against the existing (nearly identical)
                   plane and continue rather than adding a redundant entry */
                ps->velocity = p_clip_velocity(ps->velocity,
                                                trace.hit_normal,
                                                QK_PM_OVERCLIP);
                continue;
            }
        }

        if (num_planes >= P_MAX_CLIP_PLANES) {
            ps->velocity = (vec3_t){0.0f, 0.0f, 0.0f};
            return true;
        }
        planes[num_planes] = trace.hit_normal;
        num_planes++;

        /* Try to clip velocity against all accumulated planes */
        i32 i, j;
        for (i = 0; i < num_planes; i++) {
            vec3_t clipped = p_clip_velocity(ps->velocity, planes[i],
                                             QK_PM_OVERCLIP);

            /* Check that the clipped velocity doesn't re-enter any
               previously recorded plane */
            for (j = 0; j < num_planes; j++) {
                if (j == i) continue;
                if (vec3_dot(clipped, planes[j]) < 0.0f) {
                    break; /* clips into another plane */
                }
            }

            if (j == num_planes) {
                /* This clip works against all planes */
                ps->velocity = clipped;
                break;
            }
        }

        if (i == num_planes) {
            /* Could not find a valid clip against a single plane.
               Try sliding along the crease between the last two planes. */
            if (num_planes == 2) {
                vec3_t dir = vec3_cross(planes[0], planes[1]);
                dir = vec3_normalize(dir);
                f32 d = vec3_dot(dir, ps->velocity);
                ps->velocity = vec3_scale(dir, d);
            } else {
                /* Cornered by 3+ planes -- stop */
                ps->velocity = (vec3_t){0.0f, 0.0f, 0.0f};
                return true;
            }
        }

        /* Don't accelerate past original speed (no speed gain from
           bouncing off walls) */
        if (vec3_dot(ps->velocity, primal_velocity) <= 0.0f) {
            ps->velocity = (vec3_t){0.0f, 0.0f, 0.0f};
            return true;
        }
    }

    return (num_planes > 0);
}

/* ---- StepSlideMove: handle stepping up stairs/ledges ---- */

void p_step_slide_move(qk_player_state_t *ps,
                       const qk_phys_world_t *world, f32 dt) {
    /* Save starting state */
    vec3_t start_origin = ps->origin;
    vec3_t start_velocity = ps->velocity;

    /* First, try a normal slide move */
    p_slide_move(ps, world, dt, 4);

    /* Save the result of the non-stepped move */
    vec3_t down_origin = ps->origin;
    vec3_t down_velocity = ps->velocity;

    /* Restore and try the stepped move */
    ps->origin = start_origin;
    ps->velocity = start_velocity;

    /* Trace up by step height */
    vec3_t up_dest = start_origin;
    up_dest.z += QK_PM_STEP_HEIGHT;
    qk_trace_result_t trace = p_trace_world(world, ps->origin, up_dest,
                                             ps->mins, ps->maxs);
    if (!trace.all_solid) {
        ps->origin = trace.end_pos;
    }

    /* Slide move from the stepped-up position */
    p_slide_move(ps, world, dt, 4);

    /* Step back down */
    vec3_t down_dest = ps->origin;
    down_dest.z -= QK_PM_STEP_HEIGHT;
    trace = p_trace_world(world, ps->origin, down_dest,
                          ps->mins, ps->maxs);
    if (!trace.all_solid) {
        ps->origin = trace.end_pos;
    }

    /* Snap to ground if we landed on a walkable surface */
    if (trace.fraction < 1.0f &&
        trace.hit_normal.z >= QK_PM_MIN_WALK_NORMAL) {
        /* Use the stepped result if it got further horizontally */
        f32 step_dist_sq = (ps->origin.x - start_origin.x) *
                           (ps->origin.x - start_origin.x) +
                           (ps->origin.y - start_origin.y) *
                           (ps->origin.y - start_origin.y);
        f32 down_dist_sq = (down_origin.x - start_origin.x) *
                           (down_origin.x - start_origin.x) +
                           (down_origin.y - start_origin.y) *
                           (down_origin.y - start_origin.y);

        if (step_dist_sq > down_dist_sq) {
            /* Stepped move was better */
            return;
        }
    }

    /* Non-stepped move was better (or stepped move didn't land) */
    ps->origin = down_origin;
    ps->velocity = down_velocity;
}
