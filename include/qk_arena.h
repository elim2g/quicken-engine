/*
 * QUICKEN Engine - Arena Allocator
 *
 * Simple bump allocator with 16-byte alignment.
 * All modules use arenas for memory allocation. No malloc/free in hot paths.
 */

#ifndef QK_ARENA_H
#define QK_ARENA_H

#include "quicken.h"

typedef struct qk_arena qk_arena_t;

qk_arena_t *qk_arena_create(u64 size);
void       *qk_arena_alloc(qk_arena_t *arena, u64 size);
void       qk_arena_reset(qk_arena_t *arena);
void       qk_arena_destroy(qk_arena_t *arena);

#endif /* QK_ARENA_H */
