/*
 * QUICKEN Engine - Snapshot Capture and Delta Compression
 *
 * Snapshots hold the complete entity state at a given tick.
 * Delta encoding compares two snapshots field-by-field and only
 * transmits changed fields using the bitpacker.
 *
 * Delta format:
 *   [base_tick: u32]
 *   [current_tick: u32]
 *   [entity_mask delta: 4 words, each 1-bit changed flag + 64 bits if changed]
 *   [per-entity deltas for entities in either snapshot]
 *     If spawned (in current only): full entity state
 *     If despawned (in baseline only): nothing (mask already encodes removal)
 *     If in both: 1-bit changed + 12-bit field bitmask + changed field values
 */

#include "netcode/n_internal.h"

void n_snapshot_init(n_snapshot_t *snap) {
    memset(snap, 0, sizeof(*snap));
}

void n_snapshot_set_entity(n_snapshot_t *snap, u8 id, const n_entity_state_t *state) {
    u32 word = id / 64;
    u32 bit = id % 64;

    bool was_present = (snap->entity_mask[word] & ((u64)1 << bit)) != 0;
    snap->entity_mask[word] |= ((u64)1 << bit);
    snap->entities[id] = *state;

    if (!was_present) {
        snap->entity_count++;
    }
}

void n_snapshot_remove_entity(n_snapshot_t *snap, u8 id) {
    u32 word = id / 64;
    u32 bit = id % 64;

    bool was_present = (snap->entity_mask[word] & ((u64)1 << bit)) != 0;
    snap->entity_mask[word] &= ~((u64)1 << bit);
    memset(&snap->entities[id], 0, sizeof(n_entity_state_t));

    if (was_present && snap->entity_count > 0) {
        snap->entity_count--;
    }
}

bool n_snapshot_has_entity(const n_snapshot_t *snap, u8 id) {
    u32 word = id / 64;
    u32 bit = id % 64;
    return (snap->entity_mask[word] & ((u64)1 << bit)) != 0;
}

/* Write full entity state to bitwriter */
static void write_full_entity(n_bitwriter_t *w, const n_entity_state_t *e) {
    n_write_u8(w, e->entity_type);
    n_write_u8(w, e->flags);
    n_write_i16(w, e->pos_x);
    n_write_i16(w, e->pos_y);
    n_write_i16(w, e->pos_z);
    n_write_i16(w, e->vel_x);
    n_write_i16(w, e->vel_y);
    n_write_i16(w, e->vel_z);
    n_write_u16(w, e->yaw);
    n_write_u16(w, e->pitch);
    n_write_u8(w, e->health);
    n_write_u8(w, e->armor);
    n_write_u8(w, e->weapon);
    n_write_u8(w, e->ammo);
}

/* Read full entity state from bitreader */
static void read_full_entity(n_bitreader_t *r, n_entity_state_t *e) {
    e->entity_type = n_read_u8(r);
    e->flags = n_read_u8(r);
    e->pos_x = n_read_i16(r);
    e->pos_y = n_read_i16(r);
    e->pos_z = n_read_i16(r);
    e->vel_x = n_read_i16(r);
    e->vel_y = n_read_i16(r);
    e->vel_z = n_read_i16(r);
    e->yaw = n_read_u16(r);
    e->pitch = n_read_u16(r);
    e->health = n_read_u8(r);
    e->armor = n_read_u8(r);
    e->weapon = n_read_u8(r);
    e->ammo = n_read_u8(r);
}

/* Build 12-bit field bitmask: which fields differ between two entities */
static u16 entity_field_diff(const n_entity_state_t *a, const n_entity_state_t *b) {
    u16 mask = 0;
    if (a->pos_x != b->pos_x)                      mask |= (1 << 0);
    if (a->pos_y != b->pos_y)                      mask |= (1 << 1);
    if (a->pos_z != b->pos_z)                      mask |= (1 << 2);
    if (a->vel_x != b->vel_x)                      mask |= (1 << 3);
    if (a->vel_y != b->vel_y)                      mask |= (1 << 4);
    if (a->vel_z != b->vel_z)                      mask |= (1 << 5);
    if (a->yaw != b->yaw)                          mask |= (1 << 6);
    if (a->pitch != b->pitch)                      mask |= (1 << 7);
    if (a->flags != b->flags)                      mask |= (1 << 8);
    if (a->health != b->health)                    mask |= (1 << 9);
    if (a->armor != b->armor)                      mask |= (1 << 10);
    if (a->weapon != b->weapon || a->ammo != b->ammo) mask |= (1 << 11);
    return mask;
}

/* Write changed fields based on bitmask */
static void write_delta_fields(n_bitwriter_t *w, const n_entity_state_t *e, u16 mask) {
    if (mask & (1 << 0))  n_write_i16(w, e->pos_x);
    if (mask & (1 << 1))  n_write_i16(w, e->pos_y);
    if (mask & (1 << 2))  n_write_i16(w, e->pos_z);
    if (mask & (1 << 3))  n_write_i16(w, e->vel_x);
    if (mask & (1 << 4))  n_write_i16(w, e->vel_y);
    if (mask & (1 << 5))  n_write_i16(w, e->vel_z);
    if (mask & (1 << 6))  n_write_u16(w, e->yaw);
    if (mask & (1 << 7))  n_write_u16(w, e->pitch);
    if (mask & (1 << 8))  n_write_u8(w, e->flags);
    if (mask & (1 << 9))  n_write_u8(w, e->health);
    if (mask & (1 << 10)) n_write_u8(w, e->armor);
    if (mask & (1 << 11)) {
        n_write_u8(w, e->weapon);
        n_write_u8(w, e->ammo);
    }
}

/* Read changed fields based on bitmask, applying to existing entity */
static void read_delta_fields(n_bitreader_t *r, n_entity_state_t *e, u16 mask) {
    if (mask & (1 << 0))  e->pos_x = n_read_i16(r);
    if (mask & (1 << 1))  e->pos_y = n_read_i16(r);
    if (mask & (1 << 2))  e->pos_z = n_read_i16(r);
    if (mask & (1 << 3))  e->vel_x = n_read_i16(r);
    if (mask & (1 << 4))  e->vel_y = n_read_i16(r);
    if (mask & (1 << 5))  e->vel_z = n_read_i16(r);
    if (mask & (1 << 6))  e->yaw = n_read_u16(r);
    if (mask & (1 << 7))  e->pitch = n_read_u16(r);
    if (mask & (1 << 8))  e->flags = n_read_u8(r);
    if (mask & (1 << 9))  e->health = n_read_u8(r);
    if (mask & (1 << 10)) e->armor = n_read_u8(r);
    if (mask & (1 << 11)) {
        e->weapon = n_read_u8(r);
        e->ammo = n_read_u8(r);
    }
}

u32 n_snapshot_delta_encode(const n_snapshot_t *baseline, const n_snapshot_t *current,
                            u8 *out_buf, u32 max_bytes) {
    n_bitwriter_t w;
    n_bitwriter_init(&w, out_buf, max_bytes);

    /* Write entity mask delta (4 x 64-bit words) */
    for (u32 word = 0; word < N_MAX_ENTITIES / 64; word++) {
        u64 base_mask = baseline ? baseline->entity_mask[word] : 0;
        u64 cur_mask = current->entity_mask[word];

        bool changed = (base_mask != cur_mask);
        n_write_bool(&w, changed);
        if (changed) {
            /* Write the new mask word as two 32-bit halves */
            n_write_u32(&w, (u32)(cur_mask & 0xFFFFFFFFu));
            n_write_u32(&w, (u32)(cur_mask >> 32));
        }
    }

    /* Per-entity deltas */
    for (u32 id = 0; id < N_MAX_ENTITIES; id++) {
        bool in_base = baseline ? n_snapshot_has_entity(baseline, (u8)id) : false;
        bool in_cur = n_snapshot_has_entity(current, (u8)id);

        if (!in_base && !in_cur) continue;

        if (in_cur && !in_base) {
            /* Entity spawned: write full state */
            write_full_entity(&w, &current->entities[id]);
        } else if (in_base && !in_cur) {
            /* Entity despawned: mask already encodes removal, nothing to write */
        } else {
            /* Entity in both: delta encode */
            u16 field_mask = entity_field_diff(&baseline->entities[id],
                                               &current->entities[id]);
            bool entity_changed = (field_mask != 0);
            n_write_bool(&w, entity_changed);
            if (entity_changed) {
                n_write_bits(&w, field_mask, N_ENTITY_FIELD_COUNT);
                write_delta_fields(&w, &current->entities[id], field_mask);
            }
        }
    }

    return n_bitwriter_bytes_written(&w);
}

bool n_snapshot_delta_decode(const n_snapshot_t *baseline, n_snapshot_t *out,
                             const u8 *data, u32 data_len,
                             u32 current_tick) {
    n_bitreader_t r;
    n_bitreader_init(&r, data, data_len);

    /* Start from baseline (or empty) */
    if (baseline) {
        *out = *baseline;
    } else {
        memset(out, 0, sizeof(*out));
    }
    out->tick = current_tick;

    /* Read entity mask delta */
    for (u32 word = 0; word < N_MAX_ENTITIES / 64; word++) {
        bool changed = n_read_bool(&r);
        if (changed) {
            u32 lo = n_read_u32(&r);
            u32 hi = n_read_u32(&r);
            out->entity_mask[word] = (u64)lo | ((u64)hi << 32);
        }
    }

    if (n_bitreader_overflowed(&r)) return false;

    /* Recount entities */
    out->entity_count = 0;
    for (u32 word = 0; word < N_MAX_ENTITIES / 64; word++) {
        u64 mask = out->entity_mask[word];
        /* Count set bits */
        while (mask) {
            out->entity_count++;
            mask &= mask - 1;
        }
    }

    /* Read per-entity deltas */
    for (u32 id = 0; id < N_MAX_ENTITIES; id++) {
        bool in_base = baseline ? n_snapshot_has_entity(baseline, (u8)id) : false;
        bool in_cur = n_snapshot_has_entity(out, (u8)id);

        if (!in_base && !in_cur) continue;

        if (in_cur && !in_base) {
            /* Entity spawned: read full state */
            read_full_entity(&r, &out->entities[id]);
        } else if (in_base && !in_cur) {
            /* Entity despawned: clear */
            memset(&out->entities[id], 0, sizeof(n_entity_state_t));
        } else {
            /* Entity in both: read delta */
            bool entity_changed = n_read_bool(&r);
            if (entity_changed) {
                u16 field_mask = (u16)n_read_bits(&r, N_ENTITY_FIELD_COUNT);
                read_delta_fields(&r, &out->entities[id], field_mask);
            }
        }
    }

    return !n_bitreader_overflowed(&r);
}
