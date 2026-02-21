/*
 * QUICKEN Engine - Client Visual Effects
 */

#include "client/cl_fx.h"

#include <math.h>
#include <string.h>

#include "qk_types.h"
#include "core/qk_platform.h"
#include "renderer/qk_renderer.h"
#include "physics/qk_physics.h"
#include "netcode/qk_netcode.h"
#include "gameplay/qk_gameplay.h"

// --- Rail Beams ---

#define MAX_RAIL_BEAMS 16
static const f64 RAIL_BEAM_LIFETIME = 1.5;

typedef struct {
    f32     start[3];
    f32     end[3];
    f64     birth_time;
    u32     color;
    bool    active;
} rail_beam_t;

static rail_beam_t s_rail_beams[MAX_RAIL_BEAMS];
static u32         s_rail_beam_next;

// --- Rail Impacts ---

#define MAX_RAIL_IMPACTS 16
static const f64 RAIL_IMPACT_LIFETIME = 1.5;

typedef struct {
    f32     pos[3];
    f32     normal[3];
    f32     in_dir[3];
    f64     birth_time;
    u32     color;
    bool    active;
} rail_impact_t;

static rail_impact_t s_rail_impacts[MAX_RAIL_IMPACTS];
static u32           s_rail_impact_next;

// --- Smoke Particles ---

#define SMOKE_POOL_SIZE       1024
static const f32 SMOKE_MAX_AGE       = 0.5f;
static const f32 SMOKE_SPAWN_SPACING = 8.0f;
#define MAX_TRACKED_ROCKETS   32

typedef struct {
    f32 pos[3];
    f64 birth_time;
    f32 angle;  // random billboard rotation (radians)
} smoke_particle_t;

static smoke_particle_t s_smoke_pool[SMOKE_POOL_SIZE];
static u32 s_smoke_pool_head;

typedef struct {
    u32  entity_id;
    bool active;
    f32  last_pos[3];
    f32  last_dir[3];
    u32  last_tick;
} rocket_smoke_tracker_t;

static rocket_smoke_tracker_t s_rocket_trackers[MAX_TRACKED_ROCKETS];

// --- Explosions ---

#define MAX_EXPLOSIONS        16
static const f32 EXPLOSION_LIFETIME = 1.0f;

typedef struct {
    f32  pos[3];
    f32  dir[3];
    f32  radius;
    f64  birth_time;
    bool active;
} explosion_t;

static explosion_t s_explosions[MAX_EXPLOSIONS];
static u32         s_explosion_next;

// --- Entity Flag Tracking (for beam edge detection) ---

static u8 s_prev_flags[QK_MAX_ENTITIES];

// --- Public API ---

void cl_fx_init(void) {
    cl_fx_reset();
}

void cl_fx_reset(void) {
    memset(s_rail_beams, 0, sizeof(s_rail_beams));
    memset(s_rail_impacts, 0, sizeof(s_rail_impacts));
    memset(s_prev_flags, 0, sizeof(s_prev_flags));
    s_rail_beam_next = 0;
    s_rail_impact_next = 0;
    memset(s_smoke_pool, 0, sizeof(s_smoke_pool));
    s_smoke_pool_head = 0;
    memset(s_rocket_trackers, 0, sizeof(s_rocket_trackers));
    memset(s_explosions, 0, sizeof(s_explosions));
    s_explosion_next = 0;
}

void cl_fx_add_explosions(const qk_explosion_event_t *events, u32 count,
                           f64 now) {
    for (u32 i = 0; i < count; i++) {
        explosion_t *expl = &s_explosions[s_explosion_next % MAX_EXPLOSIONS];
        s_explosion_next++;
        expl->active = true;
        expl->pos[0] = events[i].pos[0];
        expl->pos[1] = events[i].pos[1];
        expl->pos[2] = events[i].pos[2];
        expl->dir[0] = events[i].dir[0];
        expl->dir[1] = events[i].dir[1];
        expl->dir[2] = events[i].dir[2];
        expl->radius = events[i].radius;
        expl->birth_time = now;
    }
}

u8 cl_fx_get_prev_flags(u8 entity_id) {
    return s_prev_flags[entity_id];
}

// --- Static helpers ---

static void draw_entities(const cl_fx_frame_t *frame) {
    const qk_interp_state_t *interp = frame->interp;
    if (!interp) return;

    for (u32 i = 0; i < QK_MAX_ENTITIES; i++) {
        const qk_interp_entity_t *ent = &interp->entities[i];
        if (!ent->active) continue;

        // Skip local player capsule (first person)
        if (i == (u32)frame->local_client_id && ent->entity_type == 1) continue;

        if (ent->entity_type == 1) {
            u32 color = 0x00FF00FF;
            qk_renderer_draw_capsule(ent->pos_x, ent->pos_y, ent->pos_z,
                                      15.0f, 28.0f, ent->yaw, color);
        } else if (ent->entity_type == 2) {
            qk_renderer_draw_sphere(ent->pos_x, ent->pos_y, ent->pos_z,
                                     4.0f, 0xFF8800FF);

            // Rocket projectile emits an orange point light
            qk_dynamic_light_t rocket_light = {
                .position  = { ent->pos_x, ent->pos_y, ent->pos_z },
                .radius    = 200.0f,
                .color     = { 1.0f, 0.5f, 0.1f },
                .intensity = 1.5f
            };
            qk_renderer_submit_light(&rocket_light);

            // Smoke trail: spawn particles when tick advances
            u32 cur_tick = qk_net_server_get_tick();
            rocket_smoke_tracker_t *tracker = NULL;
            for (u32 t = 0; t < MAX_TRACKED_ROCKETS; t++) {
                if (s_rocket_trackers[t].active &&
                    s_rocket_trackers[t].entity_id == i) {
                    tracker = &s_rocket_trackers[t];
                    break;
                }
            }
            if (!tracker) {
                for (u32 t = 0; t < MAX_TRACKED_ROCKETS; t++) {
                    if (!s_rocket_trackers[t].active) {
                        tracker = &s_rocket_trackers[t];
                        tracker->active = true;
                        tracker->entity_id = i;
                        tracker->last_pos[0] = ent->pos_x;
                        tracker->last_pos[1] = ent->pos_y;
                        tracker->last_pos[2] = ent->pos_z;
                        tracker->last_dir[0] = 0.0f;
                        tracker->last_dir[1] = 1.0f;
                        tracker->last_dir[2] = 0.0f;
                        tracker->last_tick = cur_tick;
                        break;
                    }
                }
            }
            if (tracker && cur_tick > tracker->last_tick) {
                f32 delta_x = ent->pos_x - tracker->last_pos[0];
                f32 delta_y = ent->pos_y - tracker->last_pos[1];
                f32 delta_z = ent->pos_z - tracker->last_pos[2];
                f32 dist = sqrtf(delta_x * delta_x +
                                 delta_y * delta_y +
                                 delta_z * delta_z);

                if (dist > 0.1f) {
                    f32 inv = 1.0f / dist;
                    tracker->last_dir[0] = delta_x * inv;
                    tracker->last_dir[1] = delta_y * inv;
                    tracker->last_dir[2] = delta_z * inv;
                }

                u32 num_puffs = (dist > 0.1f)
                    ? (u32)(dist / SMOKE_SPAWN_SPACING) + 1 : 1;

                for (u32 p = 0; p < num_puffs; p++) {
                    f32 frac = (num_puffs > 1)
                        ? (f32)(p + 1) / (f32)num_puffs : 1.0f;
                    u32 slot = s_smoke_pool_head % SMOKE_POOL_SIZE;
                    smoke_particle_t *puff = &s_smoke_pool[slot];
                    s_smoke_pool_head++;
                    puff->pos[0] = tracker->last_pos[0] + delta_x * frac;
                    puff->pos[1] = tracker->last_pos[1] + delta_y * frac;
                    puff->pos[2] = tracker->last_pos[2] + delta_z * frac;
                    puff->birth_time = frame->now;
                    // Random rotation: hash the slot index
                    u32 hash = slot * 0x45d9f3bu;
                    hash = ((hash >> 16) ^ hash) * 0x45d9f3bu;
                    hash = (hash >> 16) ^ hash;
                    puff->angle = (f32)(hash & 0xFFFF) / 65535.0f
                                  * 6.28318530f;
                }

                tracker->last_pos[0] = ent->pos_x;
                tracker->last_pos[1] = ent->pos_y;
                tracker->last_pos[2] = ent->pos_z;
                tracker->last_tick = cur_tick;
            }
        }
    }

    // Expire rocket trackers
    for (u32 t = 0; t < MAX_TRACKED_ROCKETS; t++) {
        if (!s_rocket_trackers[t].active) continue;
        u32 eid = s_rocket_trackers[t].entity_id;
        const qk_interp_entity_t *ent = &interp->entities[eid];
        if (!ent->active || ent->entity_type != 2) {
            s_rocket_trackers[t].active = false;
        }
    }
}

static void draw_smoke(f64 now) {
    qk_renderer_begin_smoke();
    for (u32 i = 0; i < SMOKE_POOL_SIZE; i++) {
        smoke_particle_t *particle = &s_smoke_pool[i];
        if (particle->birth_time == 0.0) continue;
        f32 age = (f32)(now - particle->birth_time);
        if (age > SMOKE_MAX_AGE) {
            particle->birth_time = 0.0;
            continue;
        }
        f32 frac = age / SMOKE_MAX_AGE;
        f32 fade = 1.0f - frac;
        fade = fade * fade;  // quadratic falloff
        f32 half_size = 1.5f + frac * 5.0f;
        u8 grey = (u8)(80.0f * fade);
        u8 alpha = (u8)(180.0f * fade);
        u32 color = ((u32)grey << 24) | ((u32)grey << 16)
                   | ((u32)grey << 8) | alpha;
        qk_renderer_emit_smoke_puff(particle->pos[0], particle->pos[1],
                                     particle->pos[2],
                                     half_size, color, particle->angle);
    }
    qk_renderer_end_smoke();
}

static void draw_explosions(f64 now) {
    for (u32 i = 0; i < MAX_EXPLOSIONS; i++) {
        explosion_t *expl = &s_explosions[i];
        if (!expl->active) continue;
        f32 age = (f32)(now - expl->birth_time);
        if (age > EXPLOSION_LIFETIME) {
            expl->active = false;
            continue;
        }
        f32 fade = 1.0f - (age / EXPLOSION_LIFETIME);
        // Offset explosion origin back along travel direction so the
        // dynamic light isn't coplanar with the impacted surface.
        const f32 OFFSET = 8.0f;
        f32 offset_x = expl->pos[0] - expl->dir[0] * OFFSET;
        f32 offset_y = expl->pos[1] - expl->dir[1] * OFFSET;
        f32 offset_z = expl->pos[2] - expl->dir[2] * OFFSET;
        qk_renderer_draw_explosion(offset_x, offset_y, offset_z,
                                    expl->radius * 0.5f, age,
                                    1.0f, 0.6f, 0.1f, fade);
    }
}

static void draw_viewmodel(const cl_fx_frame_t *frame) {
    if (!frame->has_prediction) return;
    if (frame->predicted_ps->alive_state != QK_PSTATE_ALIVE) return;

    qk_renderer_draw_viewmodel(
        frame->predicted_ps->weapon,
        frame->cam_pitch, frame->cam_yaw,
        (f32)frame->now,
        frame->input->mouse_buttons[0] && !frame->input->console_active);
}

static void draw_beams(const cl_fx_frame_t *frame) {
    const qk_interp_state_t *interp = frame->interp;
    if (!interp) return;

    vec3_t zero_ext = {0, 0, 0};

    for (u32 i = 0; i < QK_MAX_ENTITIES; i++) {
        const qk_interp_entity_t *ent = &interp->entities[i];
        if (!ent->active || ent->entity_type != 1) {
            s_prev_flags[i] = 0;
            continue;
        }

        u8 cur_flags = ent->flags;
        u8 prev = s_prev_flags[i];
        bool firing_now = (cur_flags & QK_ENT_FLAG_FIRING) != 0;
        bool firing_prev = (prev & QK_ENT_FLAG_FIRING) != 0;

        // Determine eye position and forward direction
        f32 eye_x, eye_y, eye_z, fwd_pitch, fwd_yaw;
        bool is_local = (i == (u32)frame->local_client_id);

        if (is_local && frame->has_prediction) {
            eye_x = frame->camera->position[0];
            eye_y = frame->camera->position[1];
            eye_z = frame->camera->position[2];
            fwd_pitch = frame->cam_pitch;
            fwd_yaw = frame->cam_yaw;
        } else {
            eye_x = ent->pos_x;
            eye_y = ent->pos_y;
            eye_z = ent->pos_z + 26.0f;
            fwd_pitch = ent->pitch;
            fwd_yaw = ent->yaw;
        }

        // Rail beam: detect rising edge of FIRING flag
        if (ent->weapon == QK_WEAPON_RAIL && firing_now && !firing_prev) {
            f32 pitch_rad = fwd_pitch * (3.14159265f / 180.0f);
            f32 yaw_rad   = fwd_yaw * (3.14159265f / 180.0f);
            f32 cos_pitch = cosf(pitch_rad), sin_pitch = sinf(pitch_rad);
            f32 cos_yaw   = cosf(yaw_rad),   sin_yaw   = sinf(yaw_rad);
            f32 dir_x = cos_pitch * cos_yaw;
            f32 dir_y = cos_pitch * sin_yaw;
            f32 dir_z = sin_pitch;

            f32 range = 8192.0f;
            vec3_t start = {eye_x, eye_y, eye_z};
            vec3_t end_pt = {eye_x + dir_x * range,
                             eye_y + dir_y * range,
                             eye_z + dir_z * range};

            qk_trace_result_t trace = qk_physics_trace(frame->world,
                                                         start, end_pt,
                                                         zero_ext, zero_ext);

            rail_beam_t *beam = &s_rail_beams[
                s_rail_beam_next % MAX_RAIL_BEAMS];
            s_rail_beam_next++;
            beam->active = true;
            beam->birth_time = frame->now;
            if (is_local) {
                // Muzzle offset: 8 units right, 4 units below eye.
                // right = (sin_yaw, -cos_yaw, 0) in QUAKE yaw convention.
                beam->start[0] = frame->camera->position[0] + sin_yaw * 8.0f;
                beam->start[1] = frame->camera->position[1] + (-cos_yaw) * 8.0f;
                beam->start[2] = frame->camera->position[2] - 4.0f;
            } else {
                beam->start[0] = eye_x;
                beam->start[1] = eye_y;
                beam->start[2] = eye_z;
            }
            beam->end[0] = trace.end_pos.x;
            beam->end[1] = trace.end_pos.y;
            beam->end[2] = trace.end_pos.z;
            beam->color = is_local ? 0x00FF00FF : 0xFF0000FF;

            // Spawn impact particles at the wall hit point
            if (trace.fraction < 1.0f) {
                rail_impact_t *impact = &s_rail_impacts[
                    s_rail_impact_next % MAX_RAIL_IMPACTS];
                s_rail_impact_next++;
                impact->active = true;
                impact->birth_time = frame->now;
                impact->pos[0] = trace.end_pos.x;
                impact->pos[1] = trace.end_pos.y;
                impact->pos[2] = trace.end_pos.z;
                impact->normal[0] = trace.hit_normal.x;
                impact->normal[1] = trace.hit_normal.y;
                impact->normal[2] = trace.hit_normal.z;
                impact->in_dir[0] = dir_x;
                impact->in_dir[1] = dir_y;
                impact->in_dir[2] = dir_z;
                impact->color = beam->color;
            }
        }

        // LG beam: remote players only (local handled below)
        if (ent->weapon == QK_WEAPON_LG && firing_now && !is_local) {
            f32 pitch_rad = fwd_pitch * (3.14159265f / 180.0f);
            f32 yaw_rad   = fwd_yaw * (3.14159265f / 180.0f);
            f32 cos_pitch = cosf(pitch_rad), sin_pitch = sinf(pitch_rad);
            f32 cos_yaw   = cosf(yaw_rad),   sin_yaw   = sinf(yaw_rad);
            f32 dir_x = cos_pitch * cos_yaw;
            f32 dir_y = cos_pitch * sin_yaw;
            f32 dir_z = sin_pitch;

            f32 range = 768.0f;
            vec3_t start = {eye_x, eye_y, eye_z};
            vec3_t end_pt = {eye_x + dir_x * range,
                             eye_y + dir_y * range,
                             eye_z + dir_z * range};

            qk_trace_result_t trace = qk_physics_trace(frame->world,
                                                         start, end_pt,
                                                         zero_ext, zero_ext);

            qk_renderer_draw_lg_beam(eye_x, eye_y, eye_z,
                                      trace.end_pos.x, trace.end_pos.y,
                                      trace.end_pos.z,
                                      (f32)frame->now);
        }

        s_prev_flags[i] = cur_flags;
    }

    // Draw active rail beams (persistent with decay)
    for (u32 i = 0; i < MAX_RAIL_BEAMS; i++) {
        rail_beam_t *beam = &s_rail_beams[i];
        if (!beam->active) continue;

        f32 age = (f32)(frame->now - beam->birth_time);
        if (age > RAIL_BEAM_LIFETIME) {
            beam->active = false;
            continue;
        }

        qk_renderer_draw_rail_beam(beam->start[0], beam->start[1],
                                    beam->start[2],
                                    beam->end[0], beam->end[1], beam->end[2],
                                    age, beam->color);
    }

    // Draw active rail impact particles (persistent with decay)
    for (u32 i = 0; i < MAX_RAIL_IMPACTS; i++) {
        rail_impact_t *impact = &s_rail_impacts[i];
        if (!impact->active) continue;

        f32 age = (f32)(frame->now - impact->birth_time);
        if (age > RAIL_IMPACT_LIFETIME) {
            impact->active = false;
            continue;
        }

        qk_renderer_draw_rail_impact(
            impact->pos[0], impact->pos[1], impact->pos[2],
            impact->normal[0], impact->normal[1], impact->normal[2],
            impact->in_dir[0], impact->in_dir[1], impact->in_dir[2],
            age, impact->color);
    }

    // Local player LG beam: input-driven, per-frame, muzzle offset
    if (frame->has_prediction &&
        frame->predicted_ps->weapon == QK_WEAPON_LG &&
        frame->predicted_ps->alive_state == QK_PSTATE_ALIVE &&
        frame->predicted_ps->pending_weapon == QK_WEAPON_NONE &&
        frame->predicted_ps->switch_time == 0 &&
        frame->predicted_ps->ammo[QK_WEAPON_LG] > 0 &&
        frame->input->mouse_buttons[0] &&
        !frame->input->console_active) {
        f32 pitch_rad = frame->cam_pitch * (3.14159265f / 180.0f);
        f32 yaw_rad   = frame->cam_yaw * (3.14159265f / 180.0f);
        f32 cos_pitch = cosf(pitch_rad), sin_pitch = sinf(pitch_rad);
        f32 cos_yaw   = cosf(yaw_rad),   sin_yaw   = sinf(yaw_rad);
        f32 dir_x = cos_pitch * cos_yaw;
        f32 dir_y = cos_pitch * sin_yaw;
        f32 dir_z = sin_pitch;

        f32 range = 768.0f;
        vec3_t trace_start = {frame->camera->position[0],
                              frame->camera->position[1],
                              frame->camera->position[2]};
        vec3_t trace_end = {frame->camera->position[0] + dir_x * range,
                            frame->camera->position[1] + dir_y * range,
                            frame->camera->position[2] + dir_z * range};

        qk_trace_result_t trace = qk_physics_trace(frame->world,
            trace_start, trace_end, zero_ext, zero_ext);

        // Muzzle offset: 8 units right, 4 units below eye.
        // right = (sin_yaw, -cos_yaw, 0) in QUAKE yaw convention.
        f32 muzzle_x = frame->camera->position[0] + sin_yaw * 8.0f;
        f32 muzzle_y = frame->camera->position[1] + (-cos_yaw) * 8.0f;
        f32 muzzle_z = frame->camera->position[2] - 4.0f;

        qk_renderer_draw_lg_beam(muzzle_x, muzzle_y, muzzle_z,
                                  trace.end_pos.x, trace.end_pos.y,
                                  trace.end_pos.z, (f32)frame->now);
    }
}

// --- Main draw entry point ---

void cl_fx_draw(const cl_fx_frame_t *frame) {
    draw_entities(frame);
    draw_smoke(frame->now);
    draw_explosions(frame->now);
    draw_viewmodel(frame);
    draw_beams(frame);
}
