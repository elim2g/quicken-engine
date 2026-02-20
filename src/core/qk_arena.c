/*
 * QUICKEN Engine - Arena Allocator (REAL implementation)
 *
 * Simple bump allocator with 16-byte alignment.
 * This is NOT a stub -- all modules depend on it for memory allocation.
 */

#include "qk_arena.h"
#include <stdlib.h>
#include <string.h>

struct qk_arena {
    u8     *base;
    u64     size;
    u64     offset;
};

qk_arena_t *qk_arena_create(u64 size) {
    qk_arena_t *arena = (qk_arena_t *)malloc(sizeof(qk_arena_t));
    if (!arena) return NULL;

    arena->base = (u8 *)malloc((size_t)size);
    if (!arena->base) {
        free(arena);
        return NULL;
    }

    arena->size = size;
    arena->offset = 0;
    memset(arena->base, 0, (size_t)size);
    return arena;
}

void *qk_arena_alloc(qk_arena_t *arena, u64 size) {
    QK_ASSERT(arena != NULL);

    // Align to 16 bytes
    u64 aligned_offset = (arena->offset + 15) & ~(u64)15;
    if (aligned_offset + size > arena->size) {
        return NULL;
    }

    void *ptr = arena->base + aligned_offset;
    arena->offset = aligned_offset + size;
    return ptr;
}

void qk_arena_reset(qk_arena_t *arena) {
    QK_ASSERT(arena != NULL);
    arena->offset = 0;
}

void qk_arena_destroy(qk_arena_t *arena) {
    if (arena) {
        free(arena->base);
        free(arena);
    }
}
