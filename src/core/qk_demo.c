/*
 * QUICKEN Engine - Demo Recording/Playback
 *
 * State machine: IDLE / RECORDING / PLAYING.
 * Recording: fwrite record header + payload to .qkdm file.
 * Playback: peek-ahead reader, processes records whose tick <= current_tick,
 *           injects snapshots via qk_net_client_inject_demo_snapshot().
 */

#include "core/qk_demo.h"
#include "gameplay/qk_gameplay.h"
#include "netcode/qk_netcode.h"

#include <stdio.h>
#include <string.h>

#ifdef QK_PLATFORM_WINDOWS
#include <direct.h>
#define DEMO_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define DEMO_MKDIR(p) mkdir(p, 0755)
#endif

#define DEMO_DIR        "demos"
#define DEMO_EXT        ".qkdm"
#define DEMO_MAX_ENTITIES 256
#define DEMO_PAYLOAD_MAX  8192

/* ---- State machine ---- */

typedef enum {
    DEMO_IDLE,
    DEMO_RECORDING,
    DEMO_PLAYING
} demo_mode_t;

static struct {
    demo_mode_t         mode;
    FILE               *file;
    qk_demo_header_t    header;

    /* Playback peek-ahead */
    qk_demo_record_t    next_record;
    bool                 has_peeked;
    qk_usercmd_t         last_cmd;
    qk_ca_state_t        last_ca_state;
} s_demo;

/* ---- Helpers ---- */

static u32 count_mask_bits(const u64 *mask, u32 count) {
    u32 total = 0;
    for (u32 i = 0; i < count; i++) {
        u64 x = mask[i];
        while (x) { total++; x &= x - 1; }
    }
    return total;
}

static void demo_ensure_dir(void) {
    DEMO_MKDIR(DEMO_DIR);
}

static void demo_build_path(char *out, u32 out_size, const char *name) {
    snprintf(out, out_size, "%s/%s%s", DEMO_DIR, name, DEMO_EXT);
}

/* ---- Lifecycle ---- */

void qk_demo_init(void) {
    memset(&s_demo, 0, sizeof(s_demo));
    s_demo.mode = DEMO_IDLE;
}

void qk_demo_shutdown(void) {
    if (s_demo.mode == DEMO_RECORDING) {
        qk_demo_record_stop();
    } else if (s_demo.mode == DEMO_PLAYING) {
        qk_demo_play_stop();
    }
}

/* ---- Recording ---- */

bool qk_demo_record_start(const char *name, u8 client_id,
                           u32 start_tick, const char *map_name) {
    if (s_demo.mode != DEMO_IDLE) return false;

    demo_ensure_dir();

    char path[256];
    demo_build_path(path, sizeof(path), name);

    s_demo.file = fopen(path, "wb");
    if (!s_demo.file) return false;

    memset(&s_demo.header, 0, sizeof(s_demo.header));
    s_demo.header.magic = QK_DEMO_MAGIC;
    s_demo.header.version = QK_DEMO_VERSION;
    s_demo.header.tick_rate = QK_TICK_RATE;
    s_demo.header.start_tick = start_tick;
    s_demo.header.local_client_id = client_id;
    if (map_name) {
        strncpy(s_demo.header.map_name, map_name,
                QK_DEMO_MAP_NAME_LEN - 1);
    }

    fwrite(&s_demo.header, sizeof(s_demo.header), 1, s_demo.file);
    s_demo.mode = DEMO_RECORDING;
    return true;
}

void qk_demo_record_stop(void) {
    if (s_demo.mode != DEMO_RECORDING) return;

    /* Write END record */
    qk_demo_record_t end_rec = {0};
    end_rec.type = QK_DEMO_RECORD_END;
    fwrite(&end_rec, sizeof(end_rec), 1, s_demo.file);

    fclose(s_demo.file);
    s_demo.file = NULL;
    s_demo.mode = DEMO_IDLE;
}

void qk_demo_record_snapshot(u32 tick, u32 entity_count,
                              const u64 *entity_mask,
                              const n_entity_state_t *entities) {
    if (s_demo.mode != DEMO_RECORDING) return;

    u32 active = count_mask_bits(entity_mask, QK_DEMO_MASK_U64S);
    u16 payload_len = (u16)(4 + 4 + QK_DEMO_MASK_U64S * 8 +
                            active * sizeof(n_entity_state_t));

    qk_demo_record_t rec = {0};
    rec.type = QK_DEMO_RECORD_SNAPSHOT;
    rec.payload_len = payload_len;
    rec.server_tick = tick;
    fwrite(&rec, sizeof(rec), 1, s_demo.file);

    fwrite(&tick, 4, 1, s_demo.file);
    fwrite(&entity_count, 4, 1, s_demo.file);
    fwrite(entity_mask, 8, QK_DEMO_MASK_U64S, s_demo.file);

    for (u32 i = 0; i < DEMO_MAX_ENTITIES; i++) {
        u32 word = i / 64;
        u32 bit = i % 64;
        if (entity_mask[word] & ((u64)1 << bit)) {
            fwrite(&entities[i], sizeof(n_entity_state_t), 1, s_demo.file);
        }
    }
}

void qk_demo_record_usercmd(u32 tick, const qk_usercmd_t *cmd) {
    if (s_demo.mode != DEMO_RECORDING || !cmd) return;

    qk_demo_record_t rec = {0};
    rec.type = QK_DEMO_RECORD_USERCMD;
    rec.payload_len = (u16)sizeof(qk_usercmd_t);
    rec.server_tick = tick;
    fwrite(&rec, sizeof(rec), 1, s_demo.file);
    fwrite(cmd, sizeof(qk_usercmd_t), 1, s_demo.file);
}

void qk_demo_record_event(u32 tick, const void *event_blob, u16 event_size) {
    if (s_demo.mode != DEMO_RECORDING || !event_blob) return;

    qk_demo_record_t rec = {0};
    rec.type = QK_DEMO_RECORD_EVENT;
    rec.payload_len = event_size;
    rec.server_tick = tick;
    fwrite(&rec, sizeof(rec), 1, s_demo.file);
    fwrite(event_blob, event_size, 1, s_demo.file);
}

void qk_demo_record_gamestate(u32 tick, const struct qk_ca_state *ca_state) {
    if (s_demo.mode != DEMO_RECORDING || !ca_state) return;

    qk_demo_record_t rec = {0};
    rec.type = QK_DEMO_RECORD_GAMESTATE;
    rec.payload_len = (u16)sizeof(qk_ca_state_t);
    rec.server_tick = tick;
    fwrite(&rec, sizeof(rec), 1, s_demo.file);
    fwrite(ca_state, sizeof(qk_ca_state_t), 1, s_demo.file);
}

/* ---- Playback ---- */

bool qk_demo_play_start(const char *name) {
    if (s_demo.mode != DEMO_IDLE) return false;

    char path[256];
    demo_build_path(path, sizeof(path), name);

    s_demo.file = fopen(path, "rb");
    if (!s_demo.file) return false;

    if (fread(&s_demo.header, sizeof(s_demo.header), 1, s_demo.file) != 1) {
        fclose(s_demo.file);
        s_demo.file = NULL;
        return false;
    }

    if (s_demo.header.magic != QK_DEMO_MAGIC ||
        s_demo.header.version != QK_DEMO_VERSION) {
        fclose(s_demo.file);
        s_demo.file = NULL;
        return false;
    }

    s_demo.has_peeked = false;
    memset(&s_demo.last_cmd, 0, sizeof(s_demo.last_cmd));
    memset(&s_demo.last_ca_state, 0, sizeof(s_demo.last_ca_state));
    s_demo.mode = DEMO_PLAYING;
    return true;
}

void qk_demo_play_stop(void) {
    if (s_demo.mode != DEMO_PLAYING) return;

    fclose(s_demo.file);
    s_demo.file = NULL;
    s_demo.has_peeked = false;
    s_demo.mode = DEMO_IDLE;
}

bool qk_demo_play_tick(u32 current_tick) {
    if (s_demo.mode != DEMO_PLAYING) return false;

    /* Ensure we have a peeked record */
    if (!s_demo.has_peeked) {
        if (fread(&s_demo.next_record, sizeof(qk_demo_record_t), 1,
                  s_demo.file) != 1) {
            return false;
        }
        s_demo.has_peeked = true;
    }

    /* Process all records whose tick <= current_tick */
    while (s_demo.has_peeked &&
           s_demo.next_record.server_tick <= current_tick) {
        qk_demo_record_t *rec = &s_demo.next_record;

        if (rec->type == QK_DEMO_RECORD_END) {
            return false;
        }

        /* Read payload */
        u8 payload[DEMO_PAYLOAD_MAX];
        u16 plen = rec->payload_len;
        if (plen > sizeof(payload)) plen = (u16)sizeof(payload);

        if (plen > 0) {
            if (fread(payload, plen, 1, s_demo.file) != 1) {
                return false;
            }
            /* Skip excess bytes if payload was truncated */
            if (rec->payload_len > plen) {
                fseek(s_demo.file, rec->payload_len - plen, SEEK_CUR);
            }
        }

        switch (rec->type) {
        case QK_DEMO_RECORD_SNAPSHOT: {
            if (plen < 40) break; /* 4+4+32 minimum */

            u32 snap_tick, snap_entity_count;
            u64 snap_mask[QK_DEMO_MASK_U64S];
            memcpy(&snap_tick, payload, 4);
            memcpy(&snap_entity_count, payload + 4, 4);
            memcpy(snap_mask, payload + 8, QK_DEMO_MASK_U64S * 8);

            /* Reconstruct full entity array from sparse data */
            n_entity_state_t entities[DEMO_MAX_ENTITIES];
            memset(entities, 0, sizeof(entities));

            u32 offset = 40;
            for (u32 i = 0; i < DEMO_MAX_ENTITIES; i++) {
                u32 word = i / 64;
                u32 bit = i % 64;
                if (snap_mask[word] & ((u64)1 << bit)) {
                    if (offset + sizeof(n_entity_state_t) <= plen) {
                        memcpy(&entities[i], payload + offset,
                               sizeof(n_entity_state_t));
                    }
                    offset += sizeof(n_entity_state_t);
                }
            }

            qk_net_client_inject_demo_snapshot(
                snap_tick, snap_entity_count, snap_mask, entities);
            break;
        }
        case QK_DEMO_RECORD_USERCMD:
            if (plen >= sizeof(qk_usercmd_t)) {
                memcpy(&s_demo.last_cmd, payload, sizeof(qk_usercmd_t));
            }
            break;
        case QK_DEMO_RECORD_EVENT:
            break;
        case QK_DEMO_RECORD_GAMESTATE:
            if (plen >= sizeof(qk_ca_state_t)) {
                memcpy(&s_demo.last_ca_state, payload, sizeof(qk_ca_state_t));
            }
            break;
        default:
            break;
        }

        /* Peek next record */
        if (fread(&s_demo.next_record, sizeof(qk_demo_record_t), 1,
                  s_demo.file) != 1) {
            s_demo.has_peeked = false;
            return true; /* last record processed, no more data */
        }
    }

    return true;
}

/* ---- State queries ---- */

bool qk_demo_is_recording(void) {
    return s_demo.mode == DEMO_RECORDING;
}

bool qk_demo_is_playing(void) {
    return s_demo.mode == DEMO_PLAYING;
}

u8 qk_demo_get_pov_client_id(void) {
    return s_demo.header.local_client_id;
}

u32 qk_demo_get_start_tick(void) {
    return s_demo.header.start_tick;
}

const qk_usercmd_t *qk_demo_get_last_usercmd(void) {
    return &s_demo.last_cmd;
}

const struct qk_ca_state *qk_demo_get_last_ca_state(void) {
    return &s_demo.last_ca_state;
}
