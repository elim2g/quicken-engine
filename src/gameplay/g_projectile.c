/*
 * QUICKEN Engine - Projectile System
 *
 * Spawn, tick (movement + collision), explosion.
 */

#include "g_internal.h"

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

void g_projectile_tick(qk_game_state_t *gs, f32 dt) {
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

        /* move */
        vec3_t new_origin = vec3_add(p->origin, vec3_scale(p->velocity, dt));

        /*
         * Trace from old to new position against player entities.
         * World geometry tracing will be added when physics_trace_line is available.
         */
        bool hit_player = false;
        entity_t *hit_ent = NULL;
        f32 best_frac = 1.0f;
        vec3_t ray = vec3_sub(new_origin, p->origin);

        for (entity_t *pe = g_entity_first(&gs->entities, ENTITY_PLAYER);
             pe; pe = g_entity_next(&gs->entities, pe, ENTITY_PLAYER)) {
            if (pe->id == p->owner) continue;
            if (pe->data.player.alive_state != QK_PSTATE_ALIVE) continue;

            vec3_t pmin = vec3_add(pe->data.player.origin, pe->data.player.mins);
            vec3_t pmax = vec3_add(pe->data.player.origin, pe->data.player.maxs);

            f32 t;
            if (ray_aabb_intersect(p->origin, ray, 1.0f, pmin, pmax, &t) && t < best_frac) {
                best_frac = t;
                hit_ent = pe;
                hit_player = true;
            }
        }

        if (hit_player && hit_ent) {
            /* direct hit: apply direct damage + splash */
            vec3_t hit_point = vec3_add(p->origin, vec3_scale(ray, best_frac));
            vec3_t hit_dir = vec3_normalize(vec3_sub(hit_ent->data.player.origin, hit_point));

            damage_event_t dmg = {0};
            dmg.attacker_id = p->owner;
            dmg.victim_id = hit_ent->id;
            dmg.damage = (i16)p->damage;
            dmg.dir = hit_dir;
            dmg.knockback = wdef->knockback;
            dmg.weapon = p->weapon;
            dmg.is_self = false;
            g_combat_apply_damage(gs, &dmg);

            /* splash damage at hit point (skip direct-hit target to avoid double damage) */
            if (p->splash_radius > 0.0f) {
                g_combat_splash_damage(gs, hit_point, p->splash_radius,
                                        p->splash_damage, wdef->knockback,
                                        p->owner, p->weapon, hit_ent->id);
            }

            g_entity_free(&gs->entities, e);
            e = next;
            continue;
        }

        /*
         * TODO: world geometry collision would go here.
         * For now, just advance the projectile position.
         * When physics is integrated, a world hit triggers explosion + splash.
         */

        p->origin = new_origin;
        e = next;
    }
}
