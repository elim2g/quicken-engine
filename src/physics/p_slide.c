/*
 * QUICKEN Engine - SlideMove and StepSlideMove
 *
 * Collision response: clip velocity against planes, slide along surfaces,
 * step up stairs/ledges.
 *
 * SIMD used for: clip velocity (dot + scale + subtract), plane dot checks.
 */

#include "p_internal.h"
#include "p_simd.h"

/* ---- Clip velocity off a collision plane ---- */

vec3_t p_clip_velocity(vec3_t velocity, vec3_t normal, f32 overbounce) {
#if P_USE_SSE2
    __m128 v_vel = p_simd_load_vec3(velocity);
    __m128 v_norm = p_simd_load_vec3(normal);
    __m128 v_result = p_simd_clip_velocity(v_vel, v_norm, overbounce);
    return p_simd_store_vec3(v_result);
#else
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
#endif
}

/* ---- SlideMove: move and clip against collision planes ---- */

bool p_slide_move(qk_player_state_t *ps, const qk_phys_world_t *world,
                  f32 dt, i32 max_bumps) {
    vec3_t planes[P_MAX_CLIP_PLANES];
    i32 num_planes = 0;
    vec3_t primal_velocity = ps->velocity;
    f32 time_left = dt;

#if P_USE_SSE2
    /* Keep SIMD versions of planes for fast dot products in inner loops */
    __m128 simd_planes[P_MAX_CLIP_PLANES];
#endif

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
#if P_USE_SSE2
            __m128 v_hit = p_simd_load_vec3(trace.hit_normal);
            for (i32 k = 0; k < num_planes; k++) {
                if (p_simd_dot3(v_hit, simd_planes[k]) > 0.99f) {
                    duplicate = true;
                    break;
                }
            }
#else
            for (i32 k = 0; k < num_planes; k++) {
                if (vec3_dot(trace.hit_normal, planes[k]) > 0.99f) {
                    duplicate = true;
                    break;
                }
            }
#endif
            if (duplicate) {
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
#if P_USE_SSE2
        simd_planes[num_planes] = p_simd_load_vec3(trace.hit_normal);
#endif
        num_planes++;

        /* Try to clip velocity against all accumulated planes */
        i32 i, j;
        for (i = 0; i < num_planes; i++) {
            vec3_t clipped = p_clip_velocity(ps->velocity, planes[i],
                                             QK_PM_OVERCLIP);

            /* Check that the clipped velocity doesn't re-enter any
               previously recorded plane */
#if P_USE_SSE2
            __m128 v_clipped = p_simd_load_vec3(clipped);
            for (j = 0; j < num_planes; j++) {
                if (j == i) continue;
                if (p_simd_dot3(v_clipped, simd_planes[j]) < 0.0f) {
                    break; /* clips into another plane */
                }
            }
#else
            for (j = 0; j < num_planes; j++) {
                if (j == i) continue;
                if (vec3_dot(clipped, planes[j]) < 0.0f) {
                    break;
                }
            }
#endif

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

/*
 * Q3-style step-slide: try a normal slide first, then attempt a
 * step-up if blocked. Key behaviors matching Q3's PM_StepSlideMove:
 *
 * 1. Skip step-up when velocity.z > 0 AND not on walkable ground.
 *    This lets the player's upward trajectory do the work of climbing
 *    stairs during skim-gliding -- step-up only fires when coming DOWN
 *    onto a step or standing on ground, producing the "pop" effect at
 *    the apex of a jump.
 *
 * 2. Step-down uses the actual distance we managed to step up (which
 *    may be less than STEP_HEIGHT if a ceiling is in the way), not the
 *    full constant.
 *
 * 3. Clip velocity against the landing surface after step-down, so
 *    the player doesn't retain a downward velocity component that
 *    fights the ground.
 */

void p_step_slide_move(qk_player_state_t *ps,
                       const qk_phys_world_t *world, f32 dt) {
    vec3_t start_origin = ps->origin;
    vec3_t start_velocity = ps->velocity;

    /* Try normal slide first. If no collision, we're done --
       no step-up needed. This avoids vertical oscillation on
       flat ground that "step-up first" would cause. */
    bool hit_wall = p_slide_move(ps, world, dt, 4);

    if (!hit_wall) {
        return;
    }

    /* Never step up when still moving upward, UNLESS the player is
       standing on walkable ground. */
    if (start_velocity.z > 0.0f) {
        vec3_t down_check = start_origin;
        down_check.z -= QK_PM_STEP_HEIGHT;
        qk_trace_result_t ground_trace = p_trace_world(
            world, start_origin, down_check, ps->mins, ps->maxs);

        if (ground_trace.fraction == 1.0f ||
            ground_trace.hit_normal.z < QK_PM_MIN_WALK_NORMAL) {
            /* Airborne with upward velocity -- don't step up.
               Use the normal slide result. */
            return;
        }
    }

    /* Hit something and step-up is allowed. Save the normal-slide
       result for comparison. */
    vec3_t normal_origin = ps->origin;
    vec3_t normal_velocity = ps->velocity;

    /* Restore to start for step-up attempt */
    ps->origin = start_origin;
    ps->velocity = start_velocity;

    /* Trace up by step height. Record actual distance achieved
       (ceiling may limit it). */
    vec3_t up_dest = start_origin;
    up_dest.z += QK_PM_STEP_HEIGHT;
    qk_trace_result_t trace = p_trace_world(world, ps->origin, up_dest,
                                             ps->mins, ps->maxs);
    if (trace.all_solid) {
        /* Can't step up at all -- use normal slide result */
        ps->origin = normal_origin;
        ps->velocity = normal_velocity;
        return;
    }

    f32 step_size = trace.end_pos.z - start_origin.z;
    ps->origin = trace.end_pos;

    /* Slide from the stepped-up position */
    p_slide_move(ps, world, dt, 4);

    /* Step back down by the actual step distance (not the full
       constant). This prevents pushing through floors when a
       ceiling limited the upward trace. */
    vec3_t down_dest = ps->origin;
    down_dest.z -= step_size;
    trace = p_trace_world(world, ps->origin, down_dest,
                          ps->mins, ps->maxs);
    if (!trace.all_solid) {
        ps->origin = trace.end_pos;
    }

    /* Clip velocity against the landing surface (Q3 behavior).
       Without this, a downward velocity component persists and
       fights the ground plane on the next tick. */
    if (trace.fraction < 1.0f) {
        ps->velocity = p_clip_velocity(ps->velocity, trace.hit_normal,
                                        QK_PM_OVERCLIP);
    }
}
