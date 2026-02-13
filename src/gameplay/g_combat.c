/*
 * QUICKEN Engine - Damage and Combat
 *
 * Single damage pipeline, hitscan trace, beam trace, splash damage, kill processing.
 */

#include "g_internal.h"

/* ---- Apply Damage (central damage pipeline) ---- */
void g_combat_apply_damage(qk_game_state_t *gs, const damage_event_t *dmg) {
    entity_t *victim = g_entity_find(&gs->entities, dmg->victim_id);
    if (!victim || victim->type != ENTITY_PLAYER) return;

    qk_player_state_t *vps = &victim->data.player;

    /* only damage alive players */
    if (vps->alive_state != QK_PSTATE_ALIVE) return;

    /* only deal damage during PLAYING state */
    if (gs->ca.state != CA_STATE_PLAYING) return;

    i16 raw_damage = dmg->damage;

    /* self damage scaling */
    if (dmg->is_self) {
        const g_weapon_def_t *wdef = &g_weapon_defs[dmg->weapon];
        raw_damage = (i16)((f32)raw_damage * wdef->self_damage_mult);
        if (raw_damage <= 0) return;
    }

    /* armor absorption */
    i16 health_dmg, armor_dmg;
    g_player_apply_armor(vps, raw_damage, &health_dmg, &armor_dmg);

    vps->health -= health_dmg;
    vps->armor -= armor_dmg;

    i16 actual_damage = health_dmg + armor_dmg;

    /* knockback */
    f32 kb = dmg->knockback;
    if (dmg->is_self) {
        const g_weapon_def_t *wdef = &g_weapon_defs[dmg->weapon];
        kb = wdef->self_knockback;
        /* self-knockback: invert direction (push player away from explosion) */
        vec3_t inv_dir = vec3_scale(dmg->dir, -1.0f);
        vps->velocity = vec3_add(vps->velocity, vec3_scale(inv_dir, kb * (f32)actual_damage));
    } else {
        vps->velocity = vec3_add(vps->velocity, vec3_scale(dmg->dir, kb * (f32)actual_damage));
    }

    /* update stats */
    entity_t *attacker = g_entity_find(&gs->entities, dmg->attacker_id);
    if (attacker && attacker->type == ENTITY_PLAYER && !dmg->is_self) {
        attacker->data.player.damage_given += (u16)actual_damage;
    }
    vps->damage_taken += (u16)actual_damage;

    /* push hit event for attacker's client */
    if (!dmg->is_self) {
        game_event_t evt = {0};
        evt.type = GEVT_HIT;
        evt.server_time = gs->server_time_ms;
        evt.data.hit.target = dmg->victim_id;
        evt.data.hit.damage = actual_damage;
        g_event_push(&gs->events, &evt);
    }

    /* check for kill */
    if (vps->health <= 0) {
        g_combat_kill(gs, dmg->attacker_id, dmg->victim_id, dmg->weapon);
    }
}

/* ---- Kill Processing ---- */
void g_combat_kill(qk_game_state_t *gs, u8 attacker_id, u8 victim_id,
                    qk_weapon_id_t weapon) {
    entity_t *victim = g_entity_find(&gs->entities, victim_id);
    if (!victim || victim->type != ENTITY_PLAYER) return;

    qk_player_state_t *vps = &victim->data.player;
    vps->alive_state = QK_PSTATE_DEAD;
    vps->deaths++;

    /* increment attacker frags (if not self-kill) */
    if (attacker_id != victim_id) {
        entity_t *attacker = g_entity_find(&gs->entities, attacker_id);
        if (attacker && attacker->type == ENTITY_PLAYER) {
            attacker->data.player.frags++;
        }
    }

    /* push kill event */
    game_event_t evt = {0};
    evt.type = GEVT_KILL;
    evt.server_time = gs->server_time_ms;
    evt.data.kill.attacker = attacker_id;
    evt.data.kill.victim = victim_id;
    evt.data.kill.weapon = weapon;
    g_event_push(&gs->events, &evt);

    /* recount alive players */
    g_ca_count_alive(gs);
}

/* ---- Hitscan Trace (Railgun) ---- */
void g_combat_hitscan_trace(qk_game_state_t *gs, entity_t *attacker,
                             vec3_t start, vec3_t dir, f32 range,
                             qk_weapon_id_t weapon) {
    const g_weapon_def_t *wdef = &g_weapon_defs[weapon];
    vec3_t end = vec3_add(start, vec3_scale(dir, range));

    /*
     * For now, trace against all enemy player entities using simple
     * AABB ray intersection. When physics_trace_line is available,
     * this will also trace against world geometry.
     */
    f32 best_frac = 1.0f;
    entity_t *hit_ent = NULL;

    for (entity_t *e = g_entity_first(&gs->entities, ENTITY_PLAYER);
         e; e = g_entity_next(&gs->entities, e, ENTITY_PLAYER)) {
        if (e == attacker) continue;
        if (e->data.player.alive_state != QK_PSTATE_ALIVE) continue;

        /* simple AABB test: check if ray passes through player bbox */
        vec3_t pmin = vec3_add(e->data.player.origin, e->data.player.mins);
        vec3_t pmax = vec3_add(e->data.player.origin, e->data.player.maxs);

        /* ray-AABB intersection (slab method) */
        f32 tmin = 0.0f;
        f32 tmax = 1.0f;

        f32 ray_dx = end.x - start.x;
        f32 ray_dy = end.y - start.y;
        f32 ray_dz = end.z - start.z;

        /* X slab */
        if (ray_dx != 0.0f) {
            f32 inv = 1.0f / ray_dx;
            f32 t1 = (pmin.x - start.x) * inv;
            f32 t2 = (pmax.x - start.x) * inv;
            if (t1 > t2) { f32 tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) continue;
        } else {
            if (start.x < pmin.x || start.x > pmax.x) continue;
        }

        /* Y slab */
        if (ray_dy != 0.0f) {
            f32 inv = 1.0f / ray_dy;
            f32 t1 = (pmin.y - start.y) * inv;
            f32 t2 = (pmax.y - start.y) * inv;
            if (t1 > t2) { f32 tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) continue;
        } else {
            if (start.y < pmin.y || start.y > pmax.y) continue;
        }

        /* Z slab */
        if (ray_dz != 0.0f) {
            f32 inv = 1.0f / ray_dz;
            f32 t1 = (pmin.z - start.z) * inv;
            f32 t2 = (pmax.z - start.z) * inv;
            if (t1 > t2) { f32 tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) continue;
        } else {
            if (start.z < pmin.z || start.z > pmax.z) continue;
        }

        if (tmin >= 0.0f && tmin < best_frac) {
            best_frac = tmin;
            hit_ent = e;
        }
    }

    /* apply damage if we hit a player */
    if (hit_ent) {
        vec3_t hit_dir = vec3_normalize(vec3_sub(hit_ent->data.player.origin, start));
        damage_event_t dmg = {0};
        dmg.attacker_id = attacker->id;
        dmg.victim_id = hit_ent->id;
        dmg.damage = (i16)wdef->damage;
        dmg.dir = hit_dir;
        dmg.knockback = wdef->knockback;
        dmg.weapon = weapon;
        dmg.is_self = false;
        g_combat_apply_damage(gs, &dmg);
    }
}

/* ---- Beam Trace (Lightning Gun) ---- */
void g_combat_beam_trace(qk_game_state_t *gs, entity_t *attacker,
                          vec3_t start, vec3_t dir, f32 range,
                          qk_weapon_id_t weapon) {
    /* Beam is mechanically identical to hitscan, just fires more frequently */
    g_combat_hitscan_trace(gs, attacker, start, dir, range, weapon);
}

/* ---- Splash Damage (Rocket Explosion) ---- */
void g_combat_splash_damage(qk_game_state_t *gs, vec3_t origin,
                             f32 radius, f32 max_damage, f32 knockback,
                             u8 attacker_id, qk_weapon_id_t weapon) {
    for (entity_t *e = g_entity_first(&gs->entities, ENTITY_PLAYER);
         e; e = g_entity_next(&gs->entities, e, ENTITY_PLAYER)) {
        if (e->data.player.alive_state != QK_PSTATE_ALIVE) continue;

        vec3_t diff = vec3_sub(e->data.player.origin, origin);
        f32 dist = vec3_length(diff);
        if (dist >= radius) continue;

        f32 damage_frac = 1.0f - (dist / radius);
        i16 damage = (i16)(max_damage * damage_frac);
        f32 kb = knockback * damage_frac;
        vec3_t dir = (dist > 0.001f) ? vec3_normalize(diff) : (vec3_t){0, 0, 1.0f};
        bool is_self = (e->id == attacker_id);

        damage_event_t dmg = {0};
        dmg.attacker_id = attacker_id;
        dmg.victim_id = e->id;
        dmg.damage = damage;
        dmg.dir = dir;
        dmg.knockback = kb;
        dmg.weapon = weapon;
        dmg.is_self = is_self;
        g_combat_apply_damage(gs, &dmg);
    }
}
