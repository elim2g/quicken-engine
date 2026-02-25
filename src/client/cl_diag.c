/*
 * QUICKEN Engine - Client Diagnostics
 */

#include "client/cl_diag.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "qk_types.h"
#include "netcode/qk_netcode.h"
#include "gameplay/qk_gameplay.h"
#include "physics/qk_physics.h"
#include "client/cl_fx.h"
#include "ui/qk_console.h"

static FILE *s_diag_file;
static u32   s_diag_frame;
static u32   s_diag_last_phys_tick;

void cl_diag_init(void) {
    s_diag_file = NULL;
    s_diag_frame = 0;
    s_diag_last_phys_tick = 0;
}

void cl_diag_shutdown(void) {
    if (s_diag_file) {
        fclose(s_diag_file);
        s_diag_file = NULL;
    }
}

void cl_diag_cmd(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: diag start|stop");
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        if (s_diag_file) {
            qk_console_print("diag: already running");
            return;
        }
        s_diag_file = fopen("diag_trace.log", "w");
        s_diag_frame = 0;
        if (s_diag_file) {
            fprintf(s_diag_file, "# QUICKEN diagnostic trace\n");
            fprintf(s_diag_file, "# Columns vary by line prefix. Greppable.\n\n");
            qk_console_print("diag: trace started -> diag_trace.log");
        } else {
            qk_console_print("diag: failed to open file");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        if (s_diag_file) {
            fclose(s_diag_file);
            s_diag_file = NULL;
            qk_console_printf("diag: stopped (%u frames written)", s_diag_frame);
        }
    }
}

void cl_diag_frame(f64 now, f32 real_dt, f32 server_accumulator,
                    f32 pred_accumulator, u8 local_client_id,
                    const qk_interp_state_t *interp,
                    bool has_prediction,
                    const qk_player_state_t *predicted_ps) {
    if (!s_diag_file) return;
    const qk_interp_diag_t *idiag = qk_net_client_get_interp_diag();

    // Frame header
    fprintf(s_diag_file,
        "FRAME %u now=%.6f dt=%.6f srv_tick=%u srv_acc=%.6f cl_acc=%.6f"
        " interp=[%s a=%u b=%u t=%.4f rt=%.2f cnt=%u]\n",
        s_diag_frame, now, (double)real_dt,
        qk_net_server_get_tick(),
        (double)server_accumulator, (double)pred_accumulator,
        (idiag && idiag->valid) ? "OK" : "NONE",
        idiag ? idiag->snap_a_tick : 0,
        idiag ? idiag->snap_b_tick : 0,
        idiag ? (double)idiag->t : 0.0,
        idiag ? idiag->render_tick : 0.0,
        idiag ? idiag->interp_count : 0);

    // Projectile entities: server f32 vs packed i16 vs interp f32
    for (u32 di = 0; di < qk_game_get_entity_count(); di++) {
        n_entity_state_t packed;
        qk_game_pack_entity((u8)di, &packed);
        if (packed.entity_type != 2) continue;

        f32 sx, sy, sz;
        if (!qk_game_get_entity_origin((u8)di, &sx, &sy, &sz)) continue;

        const qk_interp_entity_t *die = interp ? &interp->entities[di] : NULL;
        bool di_active = die && die->active;
        fprintf(s_diag_file,
            "  PROJ[%u] srv=(%.2f,%.2f,%.2f) i16=(%d,%d,%d) "
            "interp=(%s%.2f,%.2f,%.2f) vel=(%d,%d,%d)\n",
            di, (double)sx, (double)sy, (double)sz,
            packed.pos_x, packed.pos_y, packed.pos_z,
            di_active ? "" : "INACTIVE ",
            di_active ? (double)die->pos_x : 0.0,
            di_active ? (double)die->pos_y : 0.0,
            di_active ? (double)die->pos_z : 0.0,
            packed.vel_x, packed.vel_y, packed.vel_z);
    }

    // Local player weapon/beam state
    {
        const qk_player_state_t *dps =
            qk_game_get_player_state(local_client_id);
        const qk_interp_entity_t *dle =
            interp ? &interp->entities[local_client_id] : NULL;
        if (dps) {
            fprintf(s_diag_file,
                "  LOCAL srv_weapon=%u srv_wtime=%u "
                "interp_flags=0x%02x prev_flags=0x%02x "
                "interp_weapon=%u\n",
                dps->weapon, dps->weapon_time,
                dle ? dle->flags : 0,
                cl_fx_get_prev_flags(local_client_id),
                dle ? dle->weapon : 0);
        }
    }

    // Movement physics state (from client prediction, one line per physics tick)
    if (has_prediction && predicted_ps &&
        predicted_ps->command_time != s_diag_last_phys_tick) {
        s_diag_last_phys_tick = predicted_ps->command_time;
        f32 hspeed = sqrtf(predicted_ps->velocity.x * predicted_ps->velocity.x +
                           predicted_ps->velocity.y * predicted_ps->velocity.y);
        u32 since_jump = predicted_ps->command_time - predicted_ps->last_jump_tick;
        fprintf(s_diag_file,
            "  PHYS pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f) "
            "hspeed=%.1f vz=%.1f ground=%d gnorm=(%.3f,%.3f,%.3f) "
            "skim=%u hop_cd=%u jump_held=%d last_jump=%u since_jump=%u "
            "djwin=%u cmd_time=%u\n",
            (double)predicted_ps->origin.x,
            (double)predicted_ps->origin.y,
            (double)predicted_ps->origin.z,
            (double)predicted_ps->velocity.x,
            (double)predicted_ps->velocity.y,
            (double)predicted_ps->velocity.z,
            (double)hspeed,
            (double)predicted_ps->velocity.z,
            predicted_ps->on_ground,
            (double)predicted_ps->ground_normal.x,
            (double)predicted_ps->ground_normal.y,
            (double)predicted_ps->ground_normal.z,
            predicted_ps->skim_ticks,
            predicted_ps->autohop_cooldown,
            predicted_ps->jump_held,
            predicted_ps->last_jump_tick,
            since_jump,
            (u32)((u32)(QK_PM_CPM_DOUBLE_JUMP_WINDOW * QK_TICK_RATE / 1000)),
            predicted_ps->command_time);
    }

    // Physics collision debug trace (per tick, only when something happened)
    if (has_prediction && predicted_ps &&
        predicted_ps->command_time == s_diag_last_phys_tick) {
        const qk_phys_dbg_t *pdbg = &g_phys_dbg;
        if (pdbg->bump_count > 0 || pdbg->all_solid_hit ||
            pdbg->cornered || pdbg->primal_reject ||
            pdbg->step_attempted || pdbg->depenetrate_fired) {
            fprintf(s_diag_file,
                "  SLIDE bumps=%u planes=%u all_solid=%d cornered=%d "
                "primal_rej=%d step=[%s ndist=%.3f sdist=%.3f used_normal=%d] "
                "depenetrate=[%d off=(%.3f,%.3f,%.3f)]\n",
                pdbg->bump_count, pdbg->plane_count,
                pdbg->all_solid_hit, pdbg->cornered,
                pdbg->primal_reject,
                pdbg->step_attempted ? "Y" : "N",
                (double)pdbg->step_normal_dist_sq,
                (double)pdbg->step_step_dist_sq,
                pdbg->step_used_normal,
                pdbg->depenetrate_fired,
                (double)pdbg->depenetrate_offset.x,
                (double)pdbg->depenetrate_offset.y,
                (double)pdbg->depenetrate_offset.z);
            for (u32 bi = 0; bi < pdbg->bump_count; bi++) {
                const qk_phys_dbg_bump_t *b = &pdbg->bumps[bi];
                fprintf(s_diag_file,
                    "    BUMP[%u] n=(%.4f,%.4f,%.4f) frac=%.6f "
                    "all_solid=%d dup=%d\n",
                    bi,
                    (double)b->hit_normal.x,
                    (double)b->hit_normal.y,
                    (double)b->hit_normal.z,
                    (double)b->fraction,
                    b->all_solid, b->duplicate);
            }
        }
    }

    s_diag_frame++;
    if (s_diag_frame % 128 == 0) fflush(s_diag_file);
}
