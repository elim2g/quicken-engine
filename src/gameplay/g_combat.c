/*
 * QUICKEN Engine - Damage and Combat
 *
 * Single damage pipeline, hitscan trace, beam trace, splash damage, kill processing.
 */

#include "g_internal.h"

// --- Apply Damage (central damage pipeline) ---
void g_combat_apply_damage(qk_game_state_t *gs, const damage_event_t *dmg) {
    entity_t *victim = g_entity_find(&gs->entities, dmg->victim_id);
    if (!victim || victim->type != ENTITY_PLAYER) return;

    qk_player_state_t *vps = &victim->data.player;

    // only damage alive players
    if (vps->alive_state != QK_PSTATE_ALIVE) return;

    i16 raw_damage = dmg->damage;

    // self damage: apply knockback in ALL game states (rocket jumping)
    if (dmg->is_self) {
        const g_weapon_def_t *wdef = &g_weapon_defs[dmg->weapon];
        raw_damage = (i16)((f32)raw_damage * wdef->self_damage_mult);
        if (raw_damage <= 0) return;

        f32 knockback = wdef->self_knockback;
        vps->velocity = vec3_add(vps->velocity, vec3_scale(dmg->dir, knockback * (f32)raw_damage));
        vps->splash_slick_ticks = 8; // ~62ms at 128Hz
        return;
    }

    // only deal damage to other players during PLAYING state
    if (gs->ca.state != CA_STATE_PLAYING) return;

    // armor absorption
    i16 health_dmg, armor_dmg;
    g_player_apply_armor(vps, raw_damage, &health_dmg, &armor_dmg);

    vps->health -= health_dmg;
    vps->armor -= armor_dmg;

    i16 actual_damage = health_dmg + armor_dmg;

    // knockback (non-self only; self-knockback handled above)
    vps->velocity = vec3_add(vps->velocity, vec3_scale(dmg->dir, dmg->knockback * (f32)actual_damage));

    // update stats
    entity_t *attacker = g_entity_find(&gs->entities, dmg->attacker_id);
    if (attacker && attacker->type == ENTITY_PLAYER && !dmg->is_self) {
        attacker->data.player.damage_given += (u16)actual_damage;
    }
    vps->damage_taken += (u16)actual_damage;

    // push hit event for attacker's client
    if (!dmg->is_self) {
        game_event_t evt = {
            .type = GEVT_HIT,
            .server_time = gs->server_time_ms,
            .data.hit = { .target = dmg->victim_id, .damage = actual_damage },
        };
        g_event_push(&gs->events, &evt);
    }

    // check for kill
    if (vps->health <= 0) {
        g_combat_kill(gs, dmg->attacker_id, dmg->victim_id, dmg->weapon);
    }
}

// --- Kill Processing ---
void g_combat_kill(qk_game_state_t *gs, u8 attacker_id, u8 victim_id,
                    qk_weapon_id_t weapon) {
    entity_t *victim = g_entity_find(&gs->entities, victim_id);
    if (!victim || victim->type != ENTITY_PLAYER) return;

    qk_player_state_t *vps = &victim->data.player;
    vps->alive_state = QK_PSTATE_DEAD;
    vps->deaths++;

    // increment attacker frags (if not self-kill)
    if (attacker_id != victim_id) {
        entity_t *attacker = g_entity_find(&gs->entities, attacker_id);
        if (attacker && attacker->type == ENTITY_PLAYER) {
            attacker->data.player.frags++;
        }
    }

    // push kill event
    game_event_t evt = {
        .type = GEVT_KILL,
        .server_time = gs->server_time_ms,
        .data.kill = { .attacker = attacker_id, .victim = victim_id, .weapon = weapon },
    };
    g_event_push(&gs->events, &evt);

    // recount alive players
    g_ca_count_alive(gs);
}

// --- Hitscan Trace (Railgun) ---
void g_combat_hitscan_trace(qk_game_state_t *gs, entity_t *attacker,
                             vec3_t start, vec3_t dir, f32 range,
                             qk_weapon_id_t weapon) {
    const g_weapon_def_t *wdef = &g_weapon_defs[weapon];
    vec3_t ray_dir = vec3_scale(dir, range);

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

        vec3_t pmin = vec3_add(e->data.player.origin, e->data.player.mins);
        vec3_t pmax = vec3_add(e->data.player.origin, e->data.player.maxs);

        f32 t;
        if (ray_aabb_intersect(start, ray_dir, 1.0f, pmin, pmax, &t) && t < best_frac) {
            best_frac = t;
            hit_ent = e;
        }
    }

    // apply damage if we hit a player
    if (hit_ent) {
        vec3_t hit_dir = vec3_normalize(vec3_sub(hit_ent->data.player.origin, start));
        damage_event_t dmg = {
            .attacker_id = attacker->id,
            .victim_id = hit_ent->id,
            .damage = (i16)wdef->damage,
            .dir = hit_dir,
            .knockback = wdef->knockback,
            .weapon = weapon,
            .is_self = false,
        };
        g_combat_apply_damage(gs, &dmg);
    }
}

// --- Beam Trace (Lightning Gun) ---
void g_combat_beam_trace(qk_game_state_t *gs, entity_t *attacker,
                          vec3_t start, vec3_t dir, f32 range,
                          qk_weapon_id_t weapon) {
    // Beam is mechanically identical to hitscan, just fires more frequently
    g_combat_hitscan_trace(gs, attacker, start, dir, range, weapon);
}

// --- Splash Damage (Rocket Explosion) ---
void g_combat_splash_damage(qk_game_state_t *gs, vec3_t origin,
                             f32 radius, f32 max_damage, f32 knockback,
                             u8 attacker_id, qk_weapon_id_t weapon,
                             u8 skip_id) {
    for (entity_t *e = g_entity_first(&gs->entities, ENTITY_PLAYER);
         e; e = g_entity_next(&gs->entities, e, ENTITY_PLAYER)) {
        if (e->id == skip_id) continue;
        if (e->data.player.alive_state != QK_PSTATE_ALIVE) continue;

        // Find nearest point on player AABB to explosion origin.
        // This ensures rockets at feet produce upward impulse (not horizontal).
        vec3_t pmin = vec3_add(e->data.player.origin, e->data.player.mins);
        vec3_t pmax = vec3_add(e->data.player.origin, e->data.player.maxs);
        vec3_t nearest = {
            origin.x < pmin.x ? pmin.x : (origin.x > pmax.x ? pmax.x : origin.x),
            origin.y < pmin.y ? pmin.y : (origin.y > pmax.y ? pmax.y : origin.y),
            origin.z < pmin.z ? pmin.z : (origin.z > pmax.z ? pmax.z : origin.z),
        };
        vec3_t diff = vec3_sub(nearest, origin);
        f32 dist = vec3_length(diff);
        if (dist >= radius) continue;

        f32 damage_frac = 1.0f - (dist / radius);
        i16 damage = (i16)(max_damage * damage_frac);
        f32 kb = knockback * damage_frac;
        vec3_t dir = (dist > 0.001f) ? vec3_normalize(diff) : (vec3_t){0, 0, 1.0f};
        bool is_self = (e->id == attacker_id);

        damage_event_t dmg = {
            .attacker_id = attacker_id,
            .victim_id = e->id,
            .damage = damage,
            .dir = dir,
            .knockback = kb,
            .weapon = weapon,
            .is_self = is_self,
        };
        g_combat_apply_damage(gs, &dmg);
    }
}
