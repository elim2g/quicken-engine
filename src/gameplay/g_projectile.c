/*
 * QUICKEN Engine - Projectile System
 *
 * Spawn, tick (movement + collision), explosion.
 */

#include "g_internal.h"
#include "physics/qk_physics.h"

/* Projectile trace extents (small box, essentially a point trace) */
static const vec3_t PROJ_MINS = {-1.0f, -1.0f, -1.0f};
static const vec3_t PROJ_MAXS = { 1.0f,  1.0f,  1.0f};

entity_t *g_projectile_spawn(qk_game_state_t *gs, entity_t *owner,
                              qk_weapon_id_t weapon, vec3_t origin,
                              vec3_t direction) {
    const g_weapon_def_t *wdef = &g_weapon_defs[weapon];

    entity_t *proj = g_entity_alloc(&gs->entities, ENTITY_PROJECTILE);
    if (!proj) return NULL;

    /* small forward offset to avoid self-collision */
    vec3_t spawn_origin = vec3_add(origin, vec3_scale(direction, 16.0f));

    projectile_t *p = &proj->data.projectile;
    p->origin = spawn_origin;
    p->velocity = vec3_scale(direction, wdef->speed);
    p->owner = owner->id;
    p->weapon = weapon;
    p->spawn_time = gs->server_time_ms;
    p->damage = wdef->damage;
    p->splash_radius = wdef->splash_radius;
    p->splash_damage = wdef->splash_damage;

    return proj;
}

void g_projectile_tick(qk_game_state_t *gs, f32 dt,
                       const qk_phys_world_t *world) {
    entity_t *e = g_entity_first(&gs->entities, ENTITY_PROJECTILE);
    while (e) {
        entity_t *next = g_entity_next(&gs->entities, e, ENTITY_PROJECTILE);
        projectile_t *p = &e->data.projectile;
        const g_weapon_def_t *wdef = &g_weapon_defs[p->weapon];

        /* check lifetime */
        f32 elapsed = (f32)(gs->server_time_ms - p->spawn_time) / 1000.0f;
        if (elapsed >= wdef->projectile_lifetime) {
            g_entity_free(&gs->entities, e);
            e = next;
            continue;
        }

        /* desired new position */
        vec3_t new_origin = vec3_add(p->origin, vec3_scale(p->velocity, dt));

        /* trace against world geometry */
        f32 world_frac = 1.0f;
        bool hit_world = false;
        if (world) {
            qk_trace_result_t tr = qk_physics_trace(world, p->origin,
                                                      new_origin,
                                                      PROJ_MINS, PROJ_MAXS);
            if (tr.fraction < 1.0f) {
                world_frac = tr.fraction;
                hit_world = true;
            }
        }

        /* trace against player entities */
        bool hit_player = false;
        entity_t *hit_ent = NULL;
        f32 best_player_frac = 1.0f;
        vec3_t ray = vec3_sub(new_origin, p->origin);

        for (entity_t *pe = g_entity_first(&gs->entities, ENTITY_PLAYER);
             pe; pe = g_entity_next(&gs->entities, pe, ENTITY_PLAYER)) {
            if (pe->id == p->owner) continue;
            if (pe->data.player.alive_state != QK_PSTATE_ALIVE) continue;

            vec3_t pmin = vec3_add(pe->data.player.origin, pe->data.player.mins);
            vec3_t pmax = vec3_add(pe->data.player.origin, pe->data.player.maxs);

            f32 t;
            if (ray_aabb_intersect(p->origin, ray, 1.0f, pmin, pmax, &t) &&
                t < best_player_frac) {
                best_player_frac = t;
                hit_ent = pe;
                hit_player = true;
            }
        }

        /* determine which hit is closer: world or player */
        bool player_hit_first = hit_player && (!hit_world ||
                                                best_player_frac <= world_frac);

        /* Normalized velocity for explosion direction */
        f32 vel_len = sqrtf(p->velocity.x * p->velocity.x +
                            p->velocity.y * p->velocity.y +
                            p->velocity.z * p->velocity.z);
        vec3_t vel_dir = (vel_len > 0.1f)
            ? vec3_scale(p->velocity, 1.0f / vel_len)
            : (vec3_t){0.0f, 1.0f, 0.0f};

        if (player_hit_first && hit_ent) {
            /* direct hit on player */
            vec3_t hit_point = vec3_add(p->origin,
                                         vec3_scale(ray, best_player_frac));
            vec3_t hit_dir = vec3_normalize(
                vec3_sub(hit_ent->data.player.origin, hit_point));

            damage_event_t dmg = {0};
            dmg.attacker_id = p->owner;
            dmg.victim_id = hit_ent->id;
            dmg.damage = (i16)p->damage;
            dmg.dir = hit_dir;
            dmg.knockback = wdef->knockback;
            dmg.weapon = p->weapon;
            dmg.is_self = false;
            g_combat_apply_damage(gs, &dmg);

            /* splash at hit point (skip direct-hit target) */
            if (p->splash_radius > 0.0f) {
                g_combat_splash_damage(gs, hit_point, p->splash_radius,
                                        p->splash_damage, wdef->knockback,
                                        p->owner, p->weapon, hit_ent->id);
            }

            game_event_t evt = {0};
            evt.type = GEVT_EXPLOSION;
            evt.server_time = gs->server_time_ms;
            evt.data.explosion.pos[0] = hit_point.x;
            evt.data.explosion.pos[1] = hit_point.y;
            evt.data.explosion.pos[2] = hit_point.z;
            evt.data.explosion.dir[0] = vel_dir.x;
            evt.data.explosion.dir[1] = vel_dir.y;
            evt.data.explosion.dir[2] = vel_dir.z;
            evt.data.explosion.radius = p->splash_radius;
            g_event_push(&gs->events, &evt);

            g_entity_free(&gs->entities, e);
            e = next;
            continue;
        }

        if (hit_world) {
            /* explode on world surface */
            vec3_t hit_point = vec3_add(p->origin,
                                         vec3_scale(ray, world_frac));

            if (p->splash_radius > 0.0f) {
                /* splash damage includes self-damage to owner */
                g_combat_splash_damage(gs, hit_point, p->splash_radius,
                                        p->splash_damage, wdef->knockback,
                                        p->owner, p->weapon, 0xFF);
            }

            game_event_t evt = {0};
            evt.type = GEVT_EXPLOSION;
            evt.server_time = gs->server_time_ms;
            evt.data.explosion.pos[0] = hit_point.x;
            evt.data.explosion.pos[1] = hit_point.y;
            evt.data.explosion.pos[2] = hit_point.z;
            evt.data.explosion.dir[0] = vel_dir.x;
            evt.data.explosion.dir[1] = vel_dir.y;
            evt.data.explosion.dir[2] = vel_dir.z;
            evt.data.explosion.radius = p->splash_radius;
            g_event_push(&gs->events, &evt);

            g_entity_free(&gs->entities, e);
            e = next;
            continue;
        }

        /* no collision, advance position */
        p->origin = new_origin;
        e = next;
    }
}
