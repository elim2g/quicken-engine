/*
 * QUICKEN Engine - Entity Pool
 *
 * Flat array of tagged entities. alloc/free/find/iterate.
 */

#include "g_internal.h"

void g_entity_pool_init(entity_pool_t *pool) {
    memset(pool, 0, sizeof(*pool));
    pool->count = 0;
    pool->high_water = 0;
}

entity_t *g_entity_alloc(entity_pool_t *pool, entity_type_t type) {
    for (u32 i = 0; i < QK_MAX_ENTITIES; i++) {
        if (pool->entities[i].type == ENTITY_NONE) {
            entity_t *ent = &pool->entities[i];
            memset(ent, 0, sizeof(*ent));
            ent->type = type;
            ent->id = (u8)i;
            ent->active = true;
            pool->count++;
            if (i >= pool->high_water) {
                pool->high_water = i + 1;
            }
            return ent;
        }
    }
    return NULL;
}

void g_entity_free(entity_pool_t *pool, entity_t *ent) {
    if (!ent || ent->type == ENTITY_NONE) return;
    ent->type = ENTITY_NONE;
    ent->active = false;
    if (pool->count > 0) pool->count--;
}

entity_t *g_entity_find(entity_pool_t *pool, u8 id) {
    entity_t *ent = &pool->entities[id];
    if (ent->type != ENTITY_NONE && ent->active) return ent;
    return NULL;
}

entity_t *g_entity_first(entity_pool_t *pool, entity_type_t type) {
    for (u32 i = 0; i < pool->high_water; i++) {
        if (pool->entities[i].type == type && pool->entities[i].active) {
            return &pool->entities[i];
        }
    }
    return NULL;
}

entity_t *g_entity_next(entity_pool_t *pool, entity_t *after, entity_type_t type) {
    u32 start = (u32)(after - pool->entities) + 1;
    for (u32 i = start; i < pool->high_water; i++) {
        if (pool->entities[i].type == type && pool->entities[i].active) {
            return &pool->entities[i];
        }
    }
    return NULL;
}
